/*
 * taiko-probe: audio/input latency probe for the RG351V (phase 0 of the TJA
 * player). No charts needed; it answers whether SDL3 audio through ALSA dmix
 * is steady and short enough for a rhythm game.
 *
 * Measures, per ALSA PCM variant (period/buffer, see VARIANTS):
 *   - callback interval: mean/min/max and late callbacks (> 1.5x nominal)
 *   - press->write: gamepad event timestamp to the moment the click samples
 *     are handed to SDL (software part of input latency)
 *   - the real ALSA period/buffer from /proc/asound (dmix slave), which is the
 *     hardware part of output latency
 * Total input->sound latency is roughly press->write + device buffer.
 *
 * Controls: A click, B metronome (120 bpm, screen flashes with it),
 * D-pad up/down switch PCM variant, X reset stats, SELECT+START leave.
 * Summary lines also go to stderr every 5 s (launcher logs to /tmp/logs).
 */
#include <SDL3/SDL.h>
#include <glob.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCR_W 640
#define SCR_H 480
#define RATE 48000
#define CH 2
#define CLICK_FRAMES 960 /* 20 ms, 1 kHz decaying burst */
#define METRO_FRAMES (RATE / 2) /* 120 bpm */
#define MAX_CHUNK 1024

/* PCM names defined by the launcher (period/buffer frames @ 48 kHz). SDL's own
 * sample-frames request does not change the dmix period, so the variants are
 * separate ALSA PCMs and the hint below picks one. */
static const struct {
  const char *pcm;
  const char *label;
} VARIANTS[] = {
    {"taiko256", "256/1024"},
    {"taiko256w", "256/1536"},
    {"taiko256x", "256/2048"},
    {"taiko512t", "512/1536"},
    {"taiko512", "512/2048"},
    {"taiko1024", "1024/4096"},
};
#define NVARIANTS ((int)(sizeof VARIANTS / sizeof VARIANTS[0]))

static volatile sig_atomic_t quit_flag;
static void on_term(int s) { (void)s; quit_flag = 1; }

/* ---- shared with the audio thread -------------------------------------- */
static SDL_Mutex *stats_mu;
static struct {
  uint64_t cb, under; /* under: interval > 1.5x nominal (an ALSA xrun is ~2x) */
  double dt_sum, dt_min, dt_max; /* ms */
  uint64_t lat_n;
  double lat_sum, lat_max; /* ms, press -> samples written */
  double nominal_ms;
} S;
static _Atomic uint64_t press_ns;   /* event timestamp of a pending click */
static _Atomic int metro_on;
static _Atomic uint64_t flash_until_ns;
static int16_t click_tab[CLICK_FRAMES];
/* Frames SDL asks for in the burst right after the device starts, before the
 * callbacks settle to one per period: the buffer depth the client can run
 * ahead of the hardware, i.e. the steady-state output latency. */
static _Atomic int prefill_reset;
static _Atomic int prefill_pub; /* frames, 0 until measured */

/* touched only by the audio thread */
static uint64_t last_cb_ns;
static uint64_t run_start_ns; /* main thread; set by stats_reset */
static int click_pos = CLICK_FRAMES; /* >= CLICK_FRAMES: idle */
static int metro_count;
static int pf_frames, pf_done;
static uint64_t pf_last_ns;

static void stats_reset(void) {
  SDL_LockMutex(stats_mu);
  memset(&S.cb, 0, sizeof S.cb);
  S.under = 0;
  S.dt_sum = S.dt_max = 0;
  S.dt_min = 1e9;
  S.lat_n = 0;
  S.lat_sum = S.lat_max = 0;
  SDL_UnlockMutex(stats_mu);
  last_cb_ns = 0; /* benign race: next callback just skips one interval */
  run_start_ns = SDL_GetTicksNS();
}

static void SDLCALL on_audio(void *ud, SDL_AudioStream *st, int additional, int total) {
  (void)ud;
  (void)total;
  uint64_t now = SDL_GetTicksNS();
  int req_frames = additional / (int)(sizeof(int16_t) * CH);
  if (atomic_exchange(&prefill_reset, 0)) {
    pf_frames = pf_done = 0;
    pf_last_ns = 0;
  }
  if (!pf_done) {
    if (pf_last_ns && now - pf_last_ns > 2000000ull) {
      pf_done = 1;
      atomic_store(&prefill_pub, pf_frames);
    } else {
      pf_frames += req_frames;
    }
    pf_last_ns = now;
  }
  SDL_LockMutex(stats_mu);
  if (last_cb_ns) {
    double dt = (double)(now - last_cb_ns) / 1e6;
    S.cb++;
    S.dt_sum += dt;
    if (dt < S.dt_min) S.dt_min = dt;
    if (dt > S.dt_max) S.dt_max = dt;
    if (S.nominal_ms > 0 && dt > 1.5 * S.nominal_ms) S.under++;
  }
  SDL_UnlockMutex(stats_mu);
  last_cb_ns = now;

  uint64_t p = atomic_exchange(&press_ns, 0);
  if (p) {
    click_pos = 0;
    SDL_LockMutex(stats_mu);
    double l = (double)(now - p) / 1e6;
    S.lat_n++;
    S.lat_sum += l;
    if (l > S.lat_max) S.lat_max = l;
    SDL_UnlockMutex(stats_mu);
  }

  int frames = additional / (int)(sizeof(int16_t) * CH);
  int16_t buf[MAX_CHUNK * CH];
  while (frames > 0) {
    int n = frames > MAX_CHUNK ? MAX_CHUNK : frames;
    for (int i = 0; i < n; i++) {
      if (atomic_load(&metro_on) && ++metro_count >= METRO_FRAMES) {
        metro_count = 0;
        click_pos = 0;
        atomic_store(&flash_until_ns, SDL_GetTicksNS() + 60000000ull);
      }
      int16_t v = 0;
      if (click_pos < CLICK_FRAMES) v = click_tab[click_pos++];
      buf[i * CH] = v;
      buf[i * CH + 1] = v;
    }
    SDL_PutAudioStreamData(st, buf, n * CH * (int)sizeof(int16_t));
    frames -= n;
  }
}

/* ---- audio device ------------------------------------------------------ */
static SDL_AudioStream *astream;
static int size_idx = 3, dev_frames, dev_rate; /* 512/1536 */
static char dev_name[64] = "?";

static int audio_open(void) {
  SDL_SetHint(SDL_HINT_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE, VARIANTS[size_idx].pcm);
  SDL_AudioSpec spec = {SDL_AUDIO_S16, CH, RATE};
  astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_audio, NULL);
  if (!astream) {
    fprintf(stderr, "taiko-probe: audio open: %s\n", SDL_GetError());
    return -1;
  }
  SDL_AudioDeviceID dev = SDL_GetAudioStreamDevice(astream);
  SDL_AudioSpec got;
  dev_frames = 0;
  SDL_GetAudioDeviceFormat(dev, &got, &dev_frames);
  dev_rate = got.freq;
  const char *nm = SDL_GetAudioDeviceName(dev);
  snprintf(dev_name, sizeof dev_name, "%s", nm ? nm : "?");
  SDL_LockMutex(stats_mu);
  S.nominal_ms = dev_rate ? 1000.0 * dev_frames / dev_rate : 0;
  SDL_UnlockMutex(stats_mu);
  atomic_store(&prefill_pub, 0);
  atomic_store(&prefill_reset, 1);
  stats_reset();
  SDL_ResumeAudioStreamDevice(astream);
  fprintf(stderr, "taiko-probe: pcm %s (%s), SDL got %d frames @ %d Hz\n",
          VARIANTS[size_idx].pcm, VARIANTS[size_idx].label, dev_frames, dev_rate);
  return 0;
}

static void audio_close(void) {
  if (astream) SDL_DestroyAudioStream(astream);
  astream = NULL;
}

/* First open ALSA PCM in /proc/asound: the dmix slave that really talks to
 * the hardware. Fills "pcmXp period=P buffer=B rate=R" or "none". */
static void alsa_info(char *out, size_t cap) {
  snprintf(out, cap, "none");
  glob_t g;
  if (glob("/proc/asound/card*/pcm*p/sub*/hw_params", 0, NULL, &g) != 0) return;
  for (size_t i = 0; i < g.gl_pathc; i++) {
    FILE *f = fopen(g.gl_pathv[i], "r");
    if (!f) continue;
    char line[128];
    long period = 0, buffer = 0, rate = 0;
    int open = 0;
    while (fgets(line, sizeof line, f)) {
      if (!strncmp(line, "period_size:", 12)) period = atol(line + 12), open = 1;
      else if (!strncmp(line, "buffer_size:", 12)) buffer = atol(line + 12);
      else if (!strncmp(line, "rate:", 5)) rate = atol(line + 5);
    }
    fclose(f);
    if (open) {
      const char *c = strstr(g.gl_pathv[i], "card");
      snprintf(out, cap, "%.*s p=%ld b=%ld %ldHz", (int)strcspn(c, "/") , c, period, buffer, rate);
      break;
    }
  }
  globfree(&g);
}

int main(void) {
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  for (int i = 0; i < CLICK_FRAMES; i++) {
    double env = 1.0 - (double)i / CLICK_FRAMES;
    click_tab[i] = (int16_t)(12000.0 * env * env * SDL_sin(2.0 * 3.14159265 * 1000.0 * i / RATE));
  }
  stats_mu = SDL_CreateMutex();
  stats_reset();

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "taiko-probe: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("taiko-probe", SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
  if (!ren) {
    fprintf(stderr, "taiko-probe: window/renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderLogicalPresentation(ren, SCR_W / 2, SCR_H / 2, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_HideCursor();
  if (audio_open() != 0) return 1;

  int sel = 0, start = 0, presses = 0;
  uint64_t last_log = SDL_GetTicksNS(), alsa_at = last_log + 1000000000ull;
  char alsa[96] = "pending";

  while (!quit_flag) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) quit_flag = 1;
      /* SDL3 delivers button events only for gamepads that were opened. */
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      if (e.type !=SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.type != SDL_EVENT_GAMEPAD_BUTTON_UP) continue;
      int down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      switch (e.gbutton.button) {
        case SDL_GAMEPAD_BUTTON_BACK: sel = down; break;
        case SDL_GAMEPAD_BUTTON_START: start = down; break;
        case SDL_GAMEPAD_BUTTON_SOUTH:
          if (down) {
            presses++;
            atomic_store(&press_ns, e.gbutton.timestamp);
          }
          break;
        case SDL_GAMEPAD_BUTTON_EAST:
          if (down) atomic_store(&metro_on, !atomic_load(&metro_on));
          break;
        case SDL_GAMEPAD_BUTTON_WEST:
          if (down) stats_reset();
          break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
          if (down) {
            int d = e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP ? 1 : -1;
            int ni = size_idx + d;
            if (ni >= 0 && ni < NVARIANTS) {
              audio_close();
              size_idx = ni;
              if (audio_open() != 0) return 1;
              alsa_at = SDL_GetTicksNS() + 1000000000ull;
              snprintf(alsa, sizeof alsa, "pending");
            }
          }
          break;
        default: break;
      }
    }
    if (sel && start) break;

    uint64_t now = SDL_GetTicksNS();
    if (alsa_at && now >= alsa_at) {
      alsa_info(alsa, sizeof alsa);
      fprintf(stderr, "taiko-probe: alsa %s\n", alsa);
      alsa_at = 0;
    }

    int pf = atomic_load(&prefill_pub);
    double pf_ms = pf ? 1000.0 * pf / (dev_rate ? dev_rate : RATE) : 0;

    SDL_LockMutex(stats_mu);
    uint64_t cb = S.cb, under = S.under, ln = S.lat_n;
    double dmean = cb ? S.dt_sum / cb : 0, dmin = S.dt_min > 1e8 ? 0 : S.dt_min, dmax = S.dt_max;
    double lmean = ln ? S.lat_sum / ln : 0, lmax = S.lat_max, nom = S.nominal_ms;
    SDL_UnlockMutex(stats_mu);

    if (now - last_log > 5000000000ull) {
      last_log = now;
      fprintf(stderr,
              "taiko-probe: %s cb=%llu dt mean/min/max=%.2f/%.2f/%.2f ms nominal=%.2f "
              "late=%llu press n=%llu mean/max=%.2f/%.2f ms prefill=%d fr (%.1f ms)\n",
              VARIANTS[size_idx].label, (unsigned long long)cb, dmean, dmin, dmax, nom,
              (unsigned long long)under, (unsigned long long)ln, lmean, lmax, pf, pf_ms);
    }

    int flash = now < atomic_load(&flash_until_ns);
    SDL_SetRenderDrawColor(ren, flash ? 255 : 16, flash ? 255 : 16, flash ? 255 : 24, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, flash ? 0 : 255, flash ? 0 : 255, flash ? 0 : 255, 255);
    int y = 8;
#define LINE(...) (SDL_RenderDebugTextFormat(ren, 8, (float)y, __VA_ARGS__), y += 14)
    LINE("TAIKO PROBE  (phase 0)");
    y += 6;
    LINE("pcm %s  %s", VARIANTS[size_idx].pcm, VARIANTS[size_idx].label);
    LINE("SDL %d fr @ %d Hz = %.1f ms", dev_frames, dev_rate, nom);
    LINE("alsa %s", alsa);
    y += 6;
    double mins = (double)(now - run_start_ns) / 6e10;
    LINE("cb %llu  late %llu (%.1f/min)", (unsigned long long)cb, (unsigned long long)under,
         mins > 0.05 ? (double)under / mins : 0.0);
    LINE("dt mean %.2f ms", dmean);
    LINE("dt min %.2f  max %.2f ms", dmin, dmax);
    y += 6;
    LINE("presses %d", presses);
    LINE("press->write mean %.2f ms", lmean);
    LINE("press->write max  %.2f ms", lmax);
    LINE("prefill %d fr = %.1f ms", pf, pf_ms);
    LINE("~out latency %.1f ms", lmean + pf_ms);
    y += 6;
    LINE("A click  B metronome %s", atomic_load(&metro_on) ? "ON" : "off");
    LINE("UP/DOWN pcm  X reset");
    LINE("SELECT+START leave");
#undef LINE
    SDL_RenderPresent(ren);
  }

  audio_close();
  SDL_Quit();
  return 0;
}
