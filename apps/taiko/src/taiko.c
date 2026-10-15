/*
 * taiko: native TJA player for the RG351V (phase 2: one song, one course).
 *
 *   taiko [--songs DIR]                       menu: songs, course, calibration
 *   taiko --tja <file.tja> [--course N] [--autoplay]
 *         [--dump out.bmp --at SECONDS]   (headless render of one frame)
 *
 * Two trims, deliberately separate: the judgement offset (g_user_off) is added
 * to hit times only, to cancel how early/late you habitually tap; the visual
 * offset (g_vis_off) only shifts where notes are drawn, to cancel display
 * delay. Notes on screen therefore always follow the sound.
 *
 * Timing model: the audio callback is the clock. Each callback publishes
 * (music position, CLOCK_MONOTONIC ns); song_time(ns) extrapolates from the
 * latest anchor minus the output latency. Hits come from a dedicated evdev
 * thread with kernel timestamps (CLOCK_MONOTONIC), so a frame never delays a
 * hit sound or its judgement. L2/R2 trims the offset by 5 ms and is saved to
 * calib.txt next to the binary.
 *
 * Controls: A/B/D-pad = don, X/Y/L1/R1 = ka, L2/R2 = offset (-/+5 ms),
 * SELECT+START = leave.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio_codecs.h"
#include "game.h"
#include "tja.h"

#define SCR_W 640
#define SCR_H 480
#define LEAD_S 2.0 /* silence before the audio so early notes can scroll in */
#define JX 140     /* judgement circle x */
#define LANE_Y 104
#define LANE_H 100
#define BEAT_PX 130.0

static volatile sig_atomic_t quit_flag;
static void on_term(int s) { (void)s; quit_flag = 1; }

static uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- audio ------------------------------------------------------------- */
static int16_t *pcm; /* stereo interleaved */
static long pcm_frames;
static int pcm_rate = 44100;
static int16_t *se_don, *se_ka;
static int se_len;

typedef struct {
  const int16_t *d;
  int pos;
} Voice;
static Voice voices[8]; /* audio thread only */
static unsigned char se_ring[64];
static _Atomic unsigned se_head;
static unsigned se_tail; /* audio thread only */
static long long mpos;   /* audio thread: next music frame, negative in lead-in */

static struct {
  _Atomic unsigned seq;
  double pos_s;
  uint64_t ns;
} anchor;

static double g_latency = 0.032; /* seconds from "written" to "heard" */
static double g_user_off;        /* judgement trim (your tap habit); NOT applied to visuals */
static double g_vis_off = 0.020; /* draw notes this far ahead: display pipeline delay */

/* Key mapping presets: which physical input is red (don) and which is blue (ka).
 * Chosen in the menu and saved in calib.txt. */
enum { IN_A, IN_B, IN_X, IN_Y, IN_L1, IN_R1, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_N };
#define NKEYMAPS 2
static const char *const KEYMAP_NAME[NKEYMAPS] = {"Classic", "Up/Y blue"};
static const char *const KEYMAP_HELP[NKEYMAPS] = {"red: A B D-pad  blue: X Y L1 R1", "blue: UP Y  red: all others"};
static const signed char KEYMAP_KIND[NKEYMAPS][IN_N] = {
    /*  A        B        X        Y        L1       R1       UP       DOWN     LEFT     RIGHT */
    {HIT_DON, HIT_DON, HIT_KA, HIT_KA, HIT_KA, HIT_KA, HIT_DON, HIT_DON, HIT_DON, HIT_DON},
    {HIT_DON, HIT_DON, HIT_DON, HIT_KA, HIT_DON, HIT_DON, HIT_KA, HIT_DON, HIT_DON, HIT_DON},
};
static _Atomic int g_keymap;
static int input_kind(int in) { return KEYMAP_KIND[atomic_load(&g_keymap)][in]; }

static unsigned stat_cb, stat_se; /* audio thread; logged when a session ends */

static void synth_se(void) {
  free(se_don);
  free(se_ka);
  se_len = pcm_rate / 8;
  se_don = calloc((size_t)se_len, sizeof(int16_t));
  se_ka = calloc((size_t)se_len, sizeof(int16_t));
  uint32_t seed = 12345;
  double ph = 0;
  for (int i = 0; i < se_len; i++) {
    double t = (double)i / pcm_rate;
    seed = seed * 1664525u + 1013904223u;
    double noise = ((double)(seed >> 16) / 32768.0) - 1.0;
    /* don: pitch drops 760 -> 200 Hz (audible on a small speaker) + a click */
    double f = 200.0 + 560.0 * exp(-t / 0.035);
    ph += 2 * M_PI * f / pcm_rate;
    double body = sin(ph) * exp(-t / 0.050);
    double click = noise * exp(-t / 0.003);
    se_don[i] = (int16_t)(20000.0 * body + 9000.0 * click);
    /* ka: bright rim click, 2.8 + 4.2 kHz with a noise burst */
    double ek = exp(-t / 0.020);
    double tone = 0.6 * sin(2 * M_PI * 2800.0 * t) + 0.4 * sin(2 * M_PI * 4200.0 * t);
    se_ka[i] = (int16_t)(15000.0 * ek * tone + 8000.0 * noise * exp(-t / 0.008));
  }
}

static void SDLCALL on_audio(void *ud, SDL_AudioStream *st, int additional, int total) {
  (void)ud;
  (void)total;
  uint64_t now = mono_ns();
  stat_cb++;
  unsigned head = atomic_load(&se_head);
  while (se_tail != head) {
    int kind = se_ring[se_tail++ & 63];
    for (int i = 0; i < 8; i++)
      if (!voices[i].d) {
        voices[i].d = kind == HIT_DON ? se_don : se_ka;
        voices[i].pos = 0;
        stat_se++;
        break;
      }
  }
  int frames = additional / 4;
  int16_t buf[1024 * 2];
  while (frames > 0) {
    int n = frames > 1024 ? 1024 : frames;
    for (int i = 0; i < n; i++, mpos++) {
      int l = 0, r = 0;
      if (mpos >= 0 && mpos < pcm_frames) {
        l = (pcm[2 * mpos] * 3) >> 2;
        r = (pcm[2 * mpos + 1] * 3) >> 2;
      }
      for (int v = 0; v < 8; v++) {
        if (!voices[v].d) continue;
        int s = voices[v].d[voices[v].pos++];
        l += s;
        r += s;
        if (voices[v].pos >= se_len) voices[v].d = NULL;
      }
      buf[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
      buf[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    SDL_PutAudioStreamData(st, buf, n * 4);
    frames -= n;
  }
  atomic_fetch_add(&anchor.seq, 1);
  atomic_thread_fence(memory_order_release);
  anchor.pos_s = (double)mpos / pcm_rate;
  anchor.ns = now;
  atomic_thread_fence(memory_order_release);
  atomic_fetch_add(&anchor.seq, 1);
}

/* Song seconds at CLOCK_MONOTONIC time ns (works from any thread). */
static double song_time(uint64_t ns) {
  unsigned s0, s1;
  double pos;
  uint64_t ans;
  do {
    s0 = atomic_load(&anchor.seq);
    atomic_thread_fence(memory_order_acquire);
    pos = anchor.pos_s;
    ans = anchor.ns;
    atomic_thread_fence(memory_order_acquire);
    s1 = atomic_load(&anchor.seq);
  } while ((s0 & 1) || s0 != s1);
  if (!ans) return -LEAD_S;
  return pos - g_latency + (double)(int64_t)(ns - ans) / 1e9;
}

/* ---- input ------------------------------------------------------------- */
typedef struct {
  double t;
  int kind;
} HitEv;
static HitEv hit_ring[256];
static _Atomic unsigned hit_head;
static unsigned hit_tail; /* main thread only */
static _Atomic int have_evdev;
static _Atomic int g_input_on; /* hits are accepted (inside a session) */
static _Atomic int g_sfx;      /* hits trigger the don/ka sound */
static _Atomic int offset_req_ms; /* L2/R2 presses not yet applied */
static int autoplay;

static void emit_hit(int kind, double t) {
  if (!atomic_load(&g_input_on)) return;
  if (atomic_load(&g_sfx)) {
    unsigned h = atomic_load(&se_head);
    se_ring[h & 63] = (unsigned char)kind;
    atomic_store(&se_head, h + 1);
  }
  unsigned q = atomic_load(&hit_head);
  hit_ring[q & 255] = (HitEv){t + g_user_off, kind}; /* t is audio-clock time */
  atomic_store(&hit_head, q + 1);
}

/* Measured evdev codes of this handheld's gamepad (docs/gotchas.md): the
 * kernel symbol names do not match the physical buttons. */
static int code_to_kind(int code) {
  switch (code) {
    case 304: return input_kind(IN_A);
    case 305: return input_kind(IN_B);
    case 306: return input_kind(IN_Y); /* the Nintendo-style layout: the LEFT face button is printed "Y" */
    case 307: return input_kind(IN_X); /* and the TOP one "X"; SDL's x:b2 / y:b3 name them by position */
    case 308: return input_kind(IN_L1);
    case 309: return input_kind(IN_R1);
    case 544: return input_kind(IN_UP); /* BTN_DPAD_UP..RIGHT, if a pad reports them */
    case 545: return input_kind(IN_DOWN);
    case 546: return input_kind(IN_LEFT);
    case 547: return input_kind(IN_RIGHT);
    default: return -1;
  }
}

static void *input_thread(void *arg) {
  (void)arg;
  int fds[16], nf = 0;
  for (int i = 0; i < 32 && nf < 16; i++) {
    char path[64];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
    memset(bits, 0, sizeof bits);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 ||
        !(bits[BTN_SOUTH / (8 * sizeof(long))] & (1ul << (BTN_SOUTH % (8 * sizeof(long)))))) {
      close(fd);
      continue;
    }
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    char name[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    fprintf(stderr, "taiko: input %s (%s)\n", path, name);
    fds[nf++] = fd;
  }
  if (nf == 0) {
    fprintf(stderr, "taiko: no evdev gamepad, falling back to SDL events\n");
    return NULL;
  }
  atomic_store(&have_evdev, 1);
  struct pollfd pf[16];
  int hat[2] = {0, 0}; /* ABS_HAT0X/Y, to fire only on 0 -> nonzero */
  for (int i = 0; i < nf; i++) pf[i] = (struct pollfd){fds[i], POLLIN, 0};
  while (!quit_flag) {
    if (poll(pf, (nfds_t)nf, 100) <= 0) continue;
    for (int i = 0; i < nf; i++) {
      if (!(pf[i].revents & POLLIN)) continue;
      struct input_event ev[32];
      ssize_t n = read(fds[i], ev, sizeof ev);
      for (ssize_t k = 0; k < n / (ssize_t)sizeof ev[0]; k++) {
        int kind = -1;
        if (ev[k].type == EV_KEY && ev[k].value == 1) {
          if (ev[k].code == 314 && atomic_load(&g_sfx)) atomic_fetch_sub(&offset_req_ms, 5); /* L2 */
          else if (ev[k].code == 315 && atomic_load(&g_sfx)) atomic_fetch_add(&offset_req_ms, 5); /* R2 */
          else kind = code_to_kind(ev[k].code);
        } else if (ev[k].type == EV_ABS && (ev[k].code == ABS_HAT0X || ev[k].code == ABS_HAT0Y)) {
          int ax = ev[k].code == ABS_HAT0Y;
          if (ev[k].value != 0 && hat[ax] == 0) {
            int in = ax ? (ev[k].value < 0 ? IN_UP : IN_DOWN) : (ev[k].value < 0 ? IN_LEFT : IN_RIGHT);
            kind = input_kind(in);
          }
          hat[ax] = ev[k].value;
        }
        if (kind < 0 || autoplay) continue;
        uint64_t ns = (uint64_t)ev[k].time.tv_sec * 1000000000ull + (uint64_t)ev[k].time.tv_usec * 1000ull;
        static int logged;
        if (logged < 80 && atomic_load(&g_input_on)) {
          /* kernel stamp vs now: ~0..few ms when CLOCK_MONOTONIC is honoured */
          fprintf(stderr, "taiko: tap code=%d evdev-age=%.2f ms audio-t=%.3f\n", ev[k].code,
                  (double)(int64_t)(mono_ns() - ns) / 1e6, song_time(ns));
          logged++;
        }
        emit_hit(kind, song_time(ns));
      }
    }
  }
  for (int i = 0; i < nf; i++) close(fds[i]);
  return NULL;
}

/* ---- primitives -------------------------------------------------------- */
static void fill_circle(SDL_Renderer *r, float cx, float cy, int rad) {
  for (int dy = -rad; dy <= rad; dy++) {
    float w = sqrtf((float)(rad * rad - dy * dy));
    SDL_FRect f = {cx - w, cy + (float)dy, 2 * w, 1};
    SDL_RenderFillRect(r, &f);
  }
}

static void text(SDL_Renderer *r, float x, float y, float s, const char *str) {
  SDL_SetRenderScale(r, s, s);
  SDL_RenderDebugText(r, x / s, y / s, str);
  SDL_SetRenderScale(r, 1, 1);
}

static void text_c(SDL_Renderer *r, float cx, float y, float s, const char *str) {
  text(r, cx - (float)strlen(str) * 4.0f * s, y, s, str);
}


/* ---- drawing ----------------------------------------------------------- */
/* Everything is drawn from primitives (no copied game art). Text uses GNU
 * Unifont (shared unifont.bin) so Japanese/Chinese titles and the judgement
 * kanji render; without the file it falls back to SDL's ASCII debug font. */
static uint32_t *uf_index; /* 65536 entries, see apps/ptterm/tools/gen-assets.py */
static const uint8_t *uf_blob;

static int load_font(const char *dir) {
  char path[1100];
  snprintf(path, sizeof path, "%sunifont.bin", dir);
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *u = malloc((size_t)n);
  if (!u || fread(u, 1, (size_t)n, f) != (size_t)n || n < 8 + 65536 * 4 || memcmp(u, "UF16", 4) != 0) {
    fclose(f);
    free(u);
    return -1;
  }
  fclose(f);
  uf_index = (uint32_t *)(u + 8);
  uf_blob = u + 8 + 65536 * 4;
  return 0;
}

static int u8_next(const char *s, uint32_t *cp) {
  const uint8_t *p = (const uint8_t *)s;
  if (p[0] < 0x80) { *cp = p[0]; return 1; }
  if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    *cp = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    return 2;
  }
  if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    *cp = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    return 3;
  }
  if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80) { /* astral: no glyph */
    *cp = 0xFFFD;
    return p[2] && p[3] ? 4 : 1;
  }
  *cp = '?';
  return 1;
}

static SDL_Texture *tex_from_text(SDL_Renderer *r, Uint32 *px, int w, int h) {
  SDL_Surface *sf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
  SDL_Texture *t = sf ? SDL_CreateTextureFromSurface(r, sf) : NULL;
  if (sf) SDL_DestroySurface(sf);
  if (t) {
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
  }
  return t;
}

static int glyph_adv(uint32_t cp, int scale) {
  if (cp < 0x10000 && uf_index && uf_index[cp] && (uf_index[cp] & 0x80000000u)) return 16 * scale;
  return 8 * scale;
}

static int utext_w(SDL_Renderer *r, const char *s, int scale) {
  (void)r;
  int w = 0;
  while (*s) {
    uint32_t cp;
    s += u8_next(s, &cp);
    w += glyph_adv(cp, scale);
  }
  return w;
}

static void ascii(const char *in, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < cap; i++) out[j++] = ((unsigned char)in[i] < 0x80) ? in[i] : '?';
  out[j] = 0;
}

/* Glyph-by-glyph drawing: only the fallback when the font is missing. */
static void utext_raw(SDL_Renderer *r, int x, int y, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
  char a[200];
  ascii(s, a, sizeof a);
  SDL_SetRenderDrawColor(r, R, G, B, A);
  text(r, (float)x, (float)y, (float)scale, a);
}

/* Text is rasterised once per (string, size, colours) into one texture. A
 * frame used to issue hundreds of per-glyph draws (and nine per glyph for an
 * outline), each switching texture, which the Mali GPU cannot keep at 60 fps. */
enum { TM_PLAIN = 0, TM_SHADOW = 1, TM_OUTLINE = 2 };
typedef struct {
  SDL_Texture *tex;
  char str[104];
  int scale, mode, pad;
  Uint8 R, G, B, oR, oG, oB;
  unsigned used;
} TextEntry;
#define TC_N 192
static TextEntry tcache[TC_N];
static unsigned tc_clock;

static SDL_Texture *text_make(SDL_Renderer *r, const char *s, int scale, int mode, Uint8 R, Uint8 G, Uint8 B, Uint8 oR,
                              Uint8 oG, Uint8 oB, int *pad_out) {
  int tw = utext_w(r, s, scale);
  int o = mode == TM_OUTLINE ? (scale > 2 ? 2 : 1) : 0;
  int pad = o + (mode == TM_SHADOW ? scale : 0) + 1;
  int W = tw + 2 * pad + scale, H = 16 * scale + 2 * pad + scale;
  uint8_t *mask = calloc((size_t)W * (size_t)H, 1);
  uint32_t *px = calloc((size_t)W * (size_t)H, sizeof *px);
  int x = pad;
  const char *p = s;
  while (*p) {
    uint32_t cp;
    p += u8_next(p, &cp);
    int wide = 0;
    int adv = 8 * scale;
    if (cp < 0x10000 && uf_index && uf_index[cp]) {
      uint32_t ent = uf_index[cp];
      wide = (ent & 0x80000000u) != 0;
      adv = (wide ? 16 : 8) * scale;
      const uint8_t *g = uf_blob + ((ent & 0x7FFFFFFF) - 1);
      int w = wide ? 16 : 8;
      for (int row = 0; row < 16; row++) {
        uint32_t bits = wide ? (uint32_t)(g[row * 2] << 8 | g[row * 2 + 1]) : g[row];
        for (int c = 0; c < w; c++) {
          if (!((bits >> (w - 1 - c)) & 1)) continue;
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale + 1; dx++) { /* +1 px: the slightly bold look */
              int xx = x + c * scale + dx, yy = pad + row * scale + dy;
              if (xx < W && yy < H) mask[yy * W + xx] = 1;
            }
        }
      }
    }
    x += adv;
  }
  for (int y = 0; y < H; y++) {
    for (int xx = 0; xx < W; xx++) {
      Uint32 c = 0;
      if (mask[y * W + xx]) {
        c = (Uint32)R | (Uint32)G << 8 | (Uint32)B << 16 | 0xFF000000u;
      } else {
        int hit = 0;
        if (mode == TM_OUTLINE) {
          for (int dy = -o; dy <= o && !hit; dy++)
            for (int dx = -o; dx <= o; dx++) {
              int yy = y + dy, x2 = xx + dx;
              if (yy >= 0 && yy < H && x2 >= 0 && x2 < W && mask[yy * W + x2]) {
                hit = 1;
                break;
              }
            }
          if (hit) c = (Uint32)oR | (Uint32)oG << 8 | (Uint32)oB << 16 | 0xFF000000u;
        } else if (mode == TM_SHADOW) {
          int yy = y - scale, x2 = xx - scale;
          if (yy >= 0 && x2 >= 0 && mask[yy * W + x2]) c = 0xC0000000u; /* black, 75% */
        }
      }
      px[y * W + xx] = c;
    }
  }
  SDL_Texture *t = tex_from_text(r, px, W, H);
  free(mask);
  free(px);
  *pad_out = pad;
  return t;
}

static void utext_cached(SDL_Renderer *r, int x, int y, const char *s, int scale, int mode, Uint8 R, Uint8 G, Uint8 B,
                         Uint8 oR, Uint8 oG, Uint8 oB, Uint8 A) {
  if (!uf_index || !s[0]) {
    if (!uf_index) utext_raw(r, x, y, s, scale, R, G, B, A);
    return;
  }
  TextEntry *hit = NULL, *lru = &tcache[0];
  for (int i = 0; i < TC_N; i++) {
    TextEntry *e = &tcache[i];
    if (e->tex && e->scale == scale && e->mode == mode && e->R == R && e->G == G && e->B == B && e->oR == oR &&
        e->oG == oG && e->oB == oB && !strcmp(e->str, s)) {
      hit = e;
      break;
    }
    if (!e->tex || e->used < lru->used) lru = e;
  }
  if (!hit) {
    if (strlen(s) >= sizeof hit->str) { /* too long to cache: never needed in practice */
      char cut[sizeof hit->str];
      memcpy(cut, s, sizeof cut - 1);
      cut[sizeof cut - 1] = 0;
      utext_cached(r, x, y, cut, scale, mode, R, G, B, oR, oG, oB, A);
      return;
    }
    if (lru->tex) SDL_DestroyTexture(lru->tex);
    int pad;
    lru->tex = text_make(r, s, scale, mode, R, G, B, oR, oG, oB, &pad);
    snprintf(lru->str, sizeof lru->str, "%s", s);
    lru->scale = scale;
    lru->mode = mode;
    lru->pad = pad;
    lru->R = R, lru->G = G, lru->B = B, lru->oR = oR, lru->oG = oG, lru->oB = oB;
    hit = lru;
  }
  hit->used = ++tc_clock;
  if (!hit->tex) return;
  float tw, th;
  SDL_GetTextureSize(hit->tex, &tw, &th);
  SDL_SetTextureAlphaMod(hit->tex, A);
  SDL_FRect d = {(float)(x - hit->pad), (float)(y - hit->pad), tw, th};
  SDL_RenderTexture(r, hit->tex, NULL, &d);
}

static void utext(SDL_Renderer *r, int x, int y, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
  utext_cached(r, x, y, s, scale, TM_PLAIN, R, G, B, 0, 0, 0, A);
}

/* shadowed, slightly emboldened text */
static void utext_s(SDL_Renderer *r, int x, int y, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
  utext_cached(r, x, y, s, scale, TM_SHADOW, R, G, B, 0, 0, 0, A);
}

/* thick outline around the text */
static void utext_o(SDL_Renderer *r, int x, int y, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 oR,
                    Uint8 oG, Uint8 oB, Uint8 A) {
  utext_cached(r, x, y, s, scale, TM_OUTLINE, R, G, B, oR, oG, oB, A);
}

/* Numbers (score, combo, counters) change constantly: draw them digit by digit
 * from ten cached glyph textures instead of rasterising a new string per value. */
static int num_w(long v, int scale) {
  char b[24];
  snprintf(b, sizeof b, "%ld", v);
  return (int)strlen(b) * 8 * scale;
}

static void draw_num(SDL_Renderer *r, int x, int y, long v, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 oR, Uint8 oG,
                     Uint8 oB) {
  char b[24];
  snprintf(b, sizeof b, "%ld", v);
  for (const char *p = b; *p; p++, x += 8 * scale) {
    char one[2] = {*p, 0};
    utext_cached(r, x, y, one, scale, TM_OUTLINE, R, G, B, oR, oG, oB, 255);
  }
}

/* Text that changes while playing (the status line) is drawn character by
 * character from prewarmed single-glyph textures: creating or destroying a
 * texture mid-song stalls the Mali driver for 100+ ms. */
static void utext_chars(SDL_Renderer *r, int x, int y, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
  for (; *s; s++, x += 8 * scale) {
    if (*s == ' ') continue;
    char one[2] = {*s, 0};
    utext_cached(r, x, y, one, scale, TM_PLAIN, R, G, B, 0, 0, 0, A);
  }
}

/* fit into maxw pixels: cut and end with "..." when too long */
static void utext_fit(SDL_Renderer *r, int x, int y, int maxw, const char *s, int scale, Uint8 R, Uint8 G, Uint8 B) {
  if (utext_w(r, s, scale) <= maxw) {
    utext_s(r, x, y, s, scale, R, G, B, 255);
    return;
  }
  char buf[256];
  size_t n = 0;
  int w = 0, dots = utext_w(r, "...", scale);
  while (*s && n + 8 < sizeof buf) {
    uint32_t cp;
    int len = u8_next(s, &cp);
    int adv = glyph_adv(cp, scale);
    if (w + adv + dots > maxw) break;
    memcpy(buf + n, s, (size_t)len);
    n += (size_t)len;
    w += adv;
    s += len;
  }
  buf[n] = 0;
  strcat(buf, "...");
  utext_s(r, x, y, buf, scale, R, G, B, 255);
}

static void col(SDL_Renderer *r, int R, int G, int B, int A) { SDL_SetRenderDrawColor(r, (Uint8)R, (Uint8)G, (Uint8)B, (Uint8)A); }

static void rect(SDL_Renderer *r, float x, float y, float w, float h) {
  SDL_FRect f = {x, y, w, h};
  SDL_RenderFillRect(r, &f);
}

static void ring(SDL_Renderer *r, float cx, float cy, int rad, int thick) {
  int rin = rad - thick;
  for (int dy = -rad; dy <= rad; dy++) {
    float wo = sqrtf((float)(rad * rad - dy * dy));
    if (abs(dy) < rin) {
      float wi = sqrtf((float)(rin * rin - dy * dy));
      rect(r, cx - wo, cy + (float)dy, wo - wi, 1);
      rect(r, cx + wi, cy + (float)dy, wo - wi, 1);
    } else {
      rect(r, cx - wo, cy + (float)dy, 2 * wo, 1);
    }
  }
}

static void gradient(SDL_Renderer *r, int y0, int y1, int R0, int G0, int B0, int R1, int G1, int B1) {
  const int band = 8;
  for (int y = y0; y < y1; y += band) {
    float k = (float)(y - y0) / (float)(y1 - y0 > 1 ? y1 - y0 : 1);
    col(r, (int)(R0 + (R1 - R0) * k), (int)(G0 + (G1 - G0) * k), (int)(B0 + (B1 - B0) * k), 255);
    rect(r, 0, (float)y, SCR_W, (float)(y + band > y1 ? y1 - y : band));
  }
}

/* course colours as in the arcade: Easy red-orange, Normal green, Hard
 * amber, Oni magenta, Ura purple */
static void course_rgb(int id, int *R, int *G, int *B) {
  static const int C[5][3] = {{232, 84, 56}, {104, 190, 60}, {236, 160, 28}, {214, 40, 140}, {142, 64, 204}};
  if (id < 0 || id > 4) id = 3;
  *R = C[id][0];
  *G = C[id][1];
  *B = C[id][2];
}

static const char *course_name(int id) {
  static const char *N[] = {"Easy", "Normal", "Hard", "Oni", "Ura"};
  return id >= 0 && id <= 4 ? N[id] : "?";
}

static void pill(SDL_Renderer *r, int x, int y, int w, int h, int R, int G, int B) {
  col(r, 0, 0, 0, 200);
  rect(r, (float)x - 2, (float)y - 2, (float)w + 4, (float)h + 4);
  col(r, R, G, B, 255);
  rect(r, (float)x, (float)y, (float)w, (float)h);
  col(r, R + (255 - R) / 3, G + (255 - G) / 3, B + (255 - B) / 3, 255);
  rect(r, (float)x, (float)y, (float)w, (float)h / 3);
}

/* ---- procedural sprites ------------------------------------------------- */
/* The note discs are generated once at start-up (outlined, vertical gradient,
 * three-lobed swirl, shine), which is both nicer and cheaper than drawing
 * them with scanline rectangles every frame. */
typedef struct {
  SDL_Texture *don, *ka, *bigdon, *bigka, *roll, *bigroll, *balloon, *cloud, *bubble;
  int ready;
} Sprites;
static Sprites spr;

static float clamp01f(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

typedef struct { float r, g, b; } RGBf;
static RGBf rgbf(int r, int g, int b) { return (RGBf){(float)r, (float)g, (float)b}; }
static RGBf mixc(RGBf a, RGBf b, float t) { return (RGBf){mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)}; }

static Uint32 pack(RGBf c, float a) {
  int R = (int)(c.r + 0.5f), G = (int)(c.g + 0.5f), B = (int)(c.b + 0.5f), A = (int)(clamp01f(a) * 255.0f + 0.5f);
  if (R > 255) R = 255;
  if (G > 255) G = 255;
  if (B > 255) B = 255;
  return (Uint32)R | (Uint32)G << 8 | (Uint32)B << 16 | (Uint32)A << 24;
}

static SDL_Texture *tex_from(SDL_Renderer *r, Uint32 *px, int w, int h) {
  SDL_Surface *sf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
  SDL_Texture *t = sf ? SDL_CreateTextureFromSurface(r, sf) : NULL;
  if (sf) SDL_DestroySurface(sf);
  if (t) {
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
  }
  return t;
}

/* A glossy note disc. tail > 0 adds a balloon tail to the right (in rad units). */
static SDL_Texture *make_disc(SDL_Renderer *r, int rad, RGBf outline, RGBf top, RGBf bottom, RGBf light, float tail,
                              int *out_w, int *out_h) {
  int tail_px = (int)(tail * (float)rad);
  int w = 2 * (rad + 4) + tail_px, h = 2 * (rad + 4);
  Uint32 *px = calloc((size_t)w * (size_t)h, sizeof *px);
  float cx = (float)(rad + 4), cy = (float)(rad + 4);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float fx = (float)x + 0.5f - cx, fy = (float)y + 0.5f - cy;
      float d = sqrtf(fx * fx + fy * fy);
      float sd = d - (float)rad; /* signed distance to the shape edge */
      if (tail_px > 0) { /* tapered neck + rounded end */
        float t = fx / (float)tail_px;
        float half = (float)rad * (0.62f - 0.30f * clamp01f(t));
        float sd_band = fmaxf(fabsf(fy) - half, fmaxf(-fx, fx - (float)tail_px));
        float ex = fx - (float)tail_px, sd_end = sqrtf(ex * ex + fy * fy) - (float)rad * 0.34f;
        sd = fminf(sd, fminf(sd_band, sd_end));
      }
      float a = clamp01f(0.5f - sd);
      if (a <= 0) continue;
      float t = clamp01f((fy + (float)rad) / (2.0f * (float)rad));
      RGBf c = mixc(top, bottom, t);
      if (d < (float)rad * 1.05f) {
        /* three-lobed swirl, like the arcade note face */
        for (int k = 0; k < 3; k++) {
          float ang = -1.5708f + (float)k * 2.0944f;
          float lx = fx - 0.40f * (float)rad * cosf(ang), ly = fy - 0.40f * (float)rad * sinf(ang);
          float m = clamp01f(0.30f * (float)rad - sqrtf(lx * lx + ly * ly) + 0.5f);
          c = mixc(c, light, m * 0.55f);
        }
        float hub = clamp01f(0.15f * (float)rad - d + 0.5f);
        c = mixc(c, bottom, hub * 0.7f);
        /* shine, upper left */
        float ex = (fx + 0.38f * (float)rad) / (0.34f * (float)rad), ey = (fy + 0.50f * (float)rad) / (0.16f * (float)rad);
        float sh = clamp01f(1.0f - (ex * ex + ey * ey));
        c = mixc(c, rgbf(255, 255, 255), sh * 0.55f);
      }
      float edge = clamp01f(sd + 2.4f + 0.5f); /* outline band just inside the edge */
      c = mixc(c, outline, edge);
      px[y * w + x] = pack(c, a);
    }
  }
  SDL_Texture *t = tex_from(r, px, w, h);
  free(px);
  *out_w = w;
  *out_h = h;
  return t;
}

static SDL_Texture *make_cloud(SDL_Renderer *r, int outlined, int *out_w, int *out_h) {
  const int w = 150, h = 92;
  static const float C[5][3] = {{36, 56, 24}, {64, 40, 28}, {96, 46, 28}, {118, 58, 20}, {78, 62, 26}};
  Uint32 *px = calloc((size_t)w * (size_t)h, sizeof *px);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float sd = 1e9f;
      for (int k = 0; k < 5; k++) {
        float dx = (float)x + 0.5f - C[k][0], dy = (float)y + 0.5f - C[k][1];
        sd = fminf(sd, sqrtf(dx * dx + dy * dy) - C[k][2]);
      }
      float a = clamp01f(0.5f - sd);
      if (a <= 0) continue;
      RGBf c = mixc(rgbf(255, 255, 255), rgbf(214, 232, 252), clamp01f((float)y / (float)h));
      if (outlined) c = mixc(c, rgbf(0, 0, 0), clamp01f(sd + 3.0f + 0.5f));
      px[y * w + x] = pack(c, outlined ? a : a * 0.92f);
    }
  }
  SDL_Texture *t = tex_from(r, px, w, h);
  free(px);
  *out_w = w;
  *out_h = h;
  return t;
}

static void sprites_init(SDL_Renderer *r) {
  if (spr.ready) return;
  int w, h;
  RGBf red_o = rgbf(96, 12, 20), red_t = rgbf(255, 118, 64), red_b = rgbf(232, 36, 36), red_l = rgbf(255, 170, 120);
  RGBf blu_o = rgbf(10, 36, 96), blu_t = rgbf(150, 236, 255), blu_b = rgbf(36, 150, 238), blu_l = rgbf(200, 248, 255);
  RGBf yel_o = rgbf(104, 64, 12), yel_t = rgbf(255, 232, 80), yel_b = rgbf(255, 178, 12), yel_l = rgbf(255, 246, 170);
  RGBf org_o = rgbf(110, 40, 8), org_t = rgbf(255, 170, 60), org_b = rgbf(240, 96, 14), org_l = rgbf(255, 206, 130);
  spr.don = make_disc(r, 21, red_o, red_t, red_b, red_l, 0, &w, &h);
  spr.ka = make_disc(r, 21, blu_o, blu_t, blu_b, blu_l, 0, &w, &h);
  spr.bigdon = make_disc(r, 31, red_o, red_t, red_b, red_l, 0, &w, &h);
  spr.bigka = make_disc(r, 31, blu_o, blu_t, blu_b, blu_l, 0, &w, &h);
  spr.roll = make_disc(r, 21, yel_o, yel_t, yel_b, yel_l, 0, &w, &h);
  spr.bigroll = make_disc(r, 31, yel_o, yel_t, yel_b, yel_l, 0, &w, &h);
  spr.balloon = make_disc(r, 27, org_o, org_t, org_b, org_l, 1.9f, &w, &h);
  spr.cloud = make_cloud(r, 0, &w, &h);
  spr.bubble = make_cloud(r, 1, &w, &h);
  spr.ready = 1;
}

static void blit(SDL_Renderer *r, SDL_Texture *t, float cx, float cy, float scale, Uint8 mod, Uint8 alpha) {
  if (!t) return;
  float tw, th;
  SDL_GetTextureSize(t, &tw, &th);
  SDL_SetTextureColorMod(t, mod, mod, mod);
  SDL_SetTextureAlphaMod(t, alpha);
  SDL_FRect d = {cx - tw * scale / 2, cy - th * scale / 2, tw * scale, th * scale};
  SDL_RenderTexture(r, t, NULL, &d);
}

/* disc sprites have the shape centre at (rad+4, rad+4); balloons have a tail
 * to the right, so anchor by the head: shift by the half-width difference */
static void blit_head(SDL_Renderer *r, SDL_Texture *t, float hx, float hy, int rad, Uint8 mod) {
  if (!t) return;
  float tw, th;
  SDL_GetTextureSize(t, &tw, &th);
  SDL_SetTextureColorMod(t, mod, mod, mod);
  SDL_SetTextureAlphaMod(t, 255);
  SDL_FRect d = {hx - (float)(rad + 4), hy - th / 2, tw, th};
  SDL_RenderTexture(r, t, NULL, &d);
}

static double px_per_s(double bpm, double scroll) { return bpm * scroll / 60.0 * BEAT_PX; }

static void draw_note(SDL_Renderer *r, const TjaNote *n, double T, int missed) {
  sprites_init(r);
  double sp = px_per_s(n->bpm, n->scroll);
  double x = JX + (n->t - T) * sp;
  float cy = LANE_Y + LANE_H / 2.0f;
  Uint8 mod = missed ? 120 : 255;
  switch (n->type) {
    case TJA_DON:
    case TJA_KA:
    case TJA_BIGDON:
    case TJA_BIGKA: {
      if (x < -40 || x > SCR_W + 40) return;
      SDL_Texture *t = n->type == TJA_DON ? spr.don : n->type == TJA_KA ? spr.ka : n->type == TJA_BIGDON ? spr.bigdon : spr.bigka;
      col(r, 0, 0, 0, missed ? 30 : 70); /* soft drop shadow */
      blit(r, t, (float)x + 1, cy + 3, 1.0f, 0, missed ? 40 : 90);
      blit(r, t, (float)x, cy, 1.0f, mod, 255);
      break;
    }
    case TJA_ROLL:
    case TJA_BIGROLL: {
      double x2 = JX + (n->end_t - T) * sp;
      if (x2 < -40 || x > SCR_W + 40) return;
      int big = n->type == TJA_BIGROLL;
      int h = big ? 31 : 21;
      double xa = x < JX && x2 > JX ? JX : x;
      float len = (float)(x2 - xa);
      col(r, 104, 64, 12, 255);
      rect(r, (float)xa, cy - h - 1, len, (float)(2 * h + 2));
      for (int i = 0; i < 2 * h; i++) { /* gold gradient, light on top */
        float t = (float)i / (float)(2 * h);
        col(r, 255, (int)(236 - 62 * t), (int)(90 - 70 * t), 255);
        rect(r, (float)xa, cy - h + (float)i, len, 1);
      }
      col(r, 255, 255, 255, 70);
      rect(r, (float)xa, cy - h + 3, len, (float)h / 3);
      blit_head(r, big ? spr.bigroll : spr.roll, (float)x2, cy, h, mod);
      if (x >= JX) blit_head(r, big ? spr.bigroll : spr.roll, (float)x, cy, h, mod);
      break;
    }
    default: { /* balloon, kusudama: a balloon with its tail trailing right */
      double xd = x < JX ? JX : x;
      if (xd > SCR_W + 40 || n->end_t < T) return;
      blit_head(r, spr.balloon, (float)xd, cy, 27, mod);
      break;
    }
  }
}

typedef struct {
  const Tja *tja;
  const TjaCourse *c;
  Game *g;
  size_t draw_start, bar_start;
  int offset_ms;
} View;

/* soul gauge: 50 cells, the clear line marked, cells past it turn gold */
static void soul_gauge(SDL_Renderer *r, int x, int y, int w, int h, const Game *g) {
  col(r, 0, 0, 0, 255);
  rect(r, (float)x - 3, (float)y - 3, (float)w + 6, (float)h + 6);
  col(r, 40, 40, 52, 255);
  rect(r, (float)x, (float)y, (float)w, (float)h);
  int cells = 50, clear = (int)lround(g->gauge_clear / 2.0), fill = (int)floor(g->gauge / 2.0);
  float cw = (float)w / (float)cells;
  for (int i = 0; i < fill && i < cells; i++) {
    if (i >= clear) col(r, 255, 214, 60, 255);
    else col(r, 232, 84 + i * 2, 40, 255);
    rect(r, (float)x + (float)i * cw + 1, (float)y + 2, cw - 1, (float)h - 4);
  }
  col(r, 255, 255, 255, 255);
  rect(r, (float)x + (float)clear * cw - 1, (float)y - 5, 2, (float)h + 10);
}

/* ---- baked scene -------------------------------------------------------- */
/* Static layers are drawn once at start-up into textures; a frame then needs a
 * handful of draws instead of ~1,500 small rectangles. */
typedef struct {
  SDL_Texture *bg[2];   /* sky gradient: normal, GOGO TIME */
  SDL_Texture *hills;
  SDL_Texture *glow;    /* GOGO lane glow, 640 x LANE_H */
  SDL_Texture *drum[3]; /* idle, ka (rim) lit, don (head) lit; 100 x 100 */
  SDL_Texture *target;  /* judgement ring, 100 x 100 */
  SDL_Texture *hitring; /* ring sprite, scaled for the hit effect, 80 x 80 */
  int ready;
} Scene;
static Scene scn;

static SDL_Texture *bake_begin(SDL_Renderer *r, int w, int h) {
  SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, w, h);
  if (!t) return NULL;
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
  SDL_SetRenderTarget(r, t);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
  SDL_RenderClear(r);
  return t;
}

static void bake_end(SDL_Renderer *r) { SDL_SetRenderTarget(r, NULL); }

static void draw_drum(SDL_Renderer *r, float cx, float cy, int hit_ka, int hit_don) {
  col(r, 0, 0, 0, 255);
  fill_circle(r, cx, cy, 47);
  col(r, 214, 218, 232, 255);
  fill_circle(r, cx, cy, 45);
  if (hit_ka) col(r, 150, 220, 255, 255);
  else col(r, 36, 108, 214, 255);
  fill_circle(r, cx, cy, 41);
  for (int k = 0; k < 12; k++) {
    float a = (float)k * 0.5236f;
    col(r, 250, 250, 255, 255);
    fill_circle(r, cx + 37.0f * cosf(a), cy + 37.0f * sinf(a), 2);
  }
  col(r, 244, 238, 222, 255);
  fill_circle(r, cx, cy, 30);
  col(r, 0, 0, 0, 90);
  fill_circle(r, cx, cy + 2, 21);
  if (hit_don) col(r, 255, 120, 96, 255);
  else col(r, 226, 48, 36, 255);
  fill_circle(r, cx, cy, 20);
  col(r, 255, 255, 255, 90);
  fill_circle(r, cx - 6, cy - 7, 6);
}

static void scene_init(SDL_Renderer *r) {
  if (scn.ready) return;
  for (int g = 0; g < 2; g++) {
    scn.bg[g] = bake_begin(r, SCR_W, SCR_H);
    gradient(r, 0, SCR_H, g ? 120 : 92, g ? 150 : 170, g ? 214 : 236, g ? 255 : 208, g ? 214 : 234, g ? 226 : 252);
    bake_end(r);
  }
  scn.hills = bake_begin(r, SCR_W, SCR_H);
  for (int x = 0; x < SCR_W; x += 8) {
    float h = 34.0f + 14.0f * sinf((float)x / 70.0f + 0.7f) + 8.0f * sinf((float)x / 23.0f);
    col(r, 112, 196, 140, 255);
    rect(r, (float)x, SCR_H - h, 8, h);
  }
  for (int x = 0; x < SCR_W; x += 8) {
    float h = 18.0f + 9.0f * sinf((float)x / 55.0f + 2.1f);
    col(r, 70, 160, 120, 255);
    rect(r, (float)x, SCR_H - h, 8, h);
  }
  bake_end(r);
  scn.glow = bake_begin(r, SCR_W, LANE_H);
  for (int x = 0; x < SCR_W; x += 4) {
    float k = clamp01f((float)(x - (JX - 20)) / 460.0f);
    RGBf cc = k < 0.4f ? mixc(rgbf(255, 70, 190), rgbf(255, 236, 140), k / 0.4f) : rgbf(255, 244, 190);
    float a = 0.98f - 0.62f * k;
    col(r, (int)cc.r, (int)cc.g, (int)cc.b, (int)(a * 255.0f));
    rect(r, (float)x, 0, 4, LANE_H);
  }
  bake_end(r);
  for (int v = 0; v < 3; v++) {
    scn.drum[v] = bake_begin(r, 100, 100);
    draw_drum(r, 50, 50, v == 1, v == 2);
    bake_end(r);
  }
  scn.target = bake_begin(r, 100, 100);
  col(r, 255, 255, 255, 70);
  ring(r, 50, 50, 38, 3);
  col(r, 0, 0, 0, 200);
  fill_circle(r, 50, 50, 29);
  col(r, 248, 248, 255, 255);
  ring(r, 50, 50, 29, 4);
  bake_end(r);
  scn.hitring = bake_begin(r, 80, 80);
  col(r, 255, 255, 255, 255);
  ring(r, 40, 40, 30, 5);
  bake_end(r);
  /* the hit ring is scaled up: smooth it */
  if (scn.hitring) SDL_SetTextureScaleMode(scn.hitring, SDL_SCALEMODE_LINEAR);
  scn.ready = 1;
}


/* sky background shared by the menu and the play screen */
static void sky(SDL_Renderer *r, double t, float pulse, int gogo) {
  scene_init(r);
  sprites_init(r);
  SDL_FRect full = {0, 0, SCR_W, SCR_H};
  SDL_SetTextureAlphaMod(scn.bg[gogo ? 1 : 0], 255);
  SDL_RenderTexture(r, scn.bg[gogo ? 1 : 0], NULL, &full);
  if (pulse > 0) {
    col(r, 255, 255, 255, (int)(pulse * 26));
    rect(r, 0, 0, SCR_W, SCR_H);
  }
  static const float CL[6][3] = {{0.0f, 300, 0.9f}, {150.0f, 372, 1.15f}, {300.0f, 330, 0.8f},
                                 {450.0f, 410, 1.3f}, {600.0f, 280, 0.7f}, {760.0f, 360, 1.0f}};
  for (int i = 0; i < 6; i++) {
    float x = fmodf(CL[i][0] + (float)(t * (6.0 + 4.0 * CL[i][2])), 820.0f) - 90.0f;
    blit(r, spr.cloud, x, CL[i][1], CL[i][2], 255, 235);
  }
  SDL_RenderTexture(r, scn.hills, NULL, &full);
}

static void render(SDL_Renderer *r, View *v, double Tj, double T, int result) {
  const TjaCourse *c = v->c;
  Game *g = v->g;
  const Tja *tja = v->tja;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  sprites_init(r);

  while (v->draw_start < c->n_notes) {
    const TjaNote *n = &c->notes[v->draw_start];
    double e = n->end_t > n->t ? n->end_t : n->t;
    if (e < T - 0.4) v->draw_start++;
    else break;
  }
  int gogo = 0;
  double bpm = tja->bpm > 0 ? tja->bpm : 120;
  for (size_t i = v->draw_start; i < c->n_notes && c->notes[i].t < T + 0.01; i++) gogo = c->notes[i].gogo;
  if (v->draw_start < c->n_notes) bpm = c->notes[v->draw_start].bpm;
  double beat = fmod(T * bpm / 60.0, 1.0);
  if (beat < 0) beat += 1.0;
  float pulse = beat < 0.30 ? (float)(1.0 - beat / 0.30) : 0.0f;

  sky(r, T, pulse, gogo);

  /* header: ribbon with title, course badge, score and soul gauge */
  col(r, 14, 22, 58, 215);
  rect(r, 0, 0, SCR_W, 74);
  col(r, 255, 255, 255, 70);
  rect(r, 0, 72, SCR_W, 2);
  int cr, cg, cb;
  course_rgb(c->id, &cr, &cg, &cb);
  {
    const char *title = tja->title[0] ? tja->title : "?";
    int maxw = 300;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", title);
    if (utext_w(r, buf, 2) > maxw) { /* cut with "..." */
      size_t n = 0;
      int w = 0, dots = utext_w(r, "...", 2);
      const char *p = title;
      while (*p && n + 8 < sizeof buf) {
        uint32_t cp;
        int len = u8_next(p, &cp);
        int adv = glyph_adv(cp, 2);
        if (w + adv + dots > maxw) break;
        memcpy(buf + n, p, (size_t)len);
        n += (size_t)len;
        w += adv;
        p += len;
      }
      buf[n] = 0;
      strcat(buf, "...");
    }
    utext_o(r, 12, 6, buf, 2, 255, 255, 255, 14, 22, 58, 255);
  }
  char badge[48];
  snprintf(badge, sizeof badge, "%s  \xE2\x98\x85%d", course_name(c->id), c->level);
  int bw = utext_w(r, badge, 1) + 16;
  pill(r, 12, 42, bw, 22, cr, cg, cb);
  utext_o(r, 20, 45, badge, 1, 255, 255, 255, 60, 20, 30, 255);

  draw_num(r, SCR_W - 12 - num_w(g->score, 3), 0, g->score, 3, 255, 255, 255, 214, 120, 10);
  utext_o(r, 336, 52, "\xE9\xAD\x82", 1, 255, 236, 120, 120, 30, 10, 255); /* soul */
  soul_gauge(r, 360, 53, 268, 16, g);

  /* lane: dark bar, bright edges; GOGO adds the pink-to-yellow glow */
  float cy = LANE_Y + LANE_H / 2.0f;
  col(r, 0, 0, 0, 110);
  rect(r, 0, LANE_Y - 5, SCR_W, LANE_H + 12);
  col(r, 30, 24, 40, 255);
  rect(r, 0, LANE_Y, SCR_W, LANE_H);
  if (gogo) {
    SDL_FRect gr = {0, LANE_Y, SCR_W, LANE_H};
    SDL_SetTextureAlphaMod(scn.glow, (Uint8)(255.0f * (0.88f + 0.12f * pulse)));
    SDL_RenderTexture(r, scn.glow, NULL, &gr);
  }
  col(r, 246, 246, 252, 255);
  rect(r, 0, LANE_Y, SCR_W, 3);
  rect(r, 0, LANE_Y + LANE_H - 3, SCR_W, 3);
  col(r, 60, 50, 80, 255);
  rect(r, 0, LANE_Y + 3, JX + 46, LANE_H - 6);
  col(r, 255, 255, 255, 30);
  rect(r, 0, LANE_Y + 3, JX + 46, 4);

  /* bar lines */
  while (v->bar_start < c->n_bars && c->bars[v->bar_start].t < T - 0.5) v->bar_start++;
  col(r, 240, 240, 250, 210);
  for (size_t i = v->bar_start; i < c->n_bars && c->bars[i].t < T + 8.0; i++) {
    const TjaBar *b = &c->bars[i];
    if (!b->visible) continue;
    double x = JX + (b->t - T) * px_per_s(b->bpm, b->scroll);
    if (x < JX + 40 || x > SCR_W) continue;
    rect(r, (float)x, LANE_Y + 3, 2, LANE_H - 6);
  }

  /* taiko: rim (ka) and head (don) light up on a hit */
  int since = (Tj - g->hit_t < 0.09 && g->hit_t > 0);
  int hit_don = since && g->hit_kind == HIT_DON, hit_ka = since && g->hit_kind == HIT_KA;
  {
    SDL_FRect dr = {62 - 50, cy - 50, 100, 100};
    SDL_RenderTexture(r, scn.drum[hit_don ? 2 : hit_ka ? 1 : 0], NULL, &dr);
    SDL_FRect tr = {JX - 50, cy - 50, 100, 100};
    SDL_RenderTexture(r, scn.target, NULL, &tr);
  }

  /* notes, far to near so near ones sit on top */
  size_t last = v->draw_start;
  while (last < c->n_notes && c->notes[last].t < T + 8.0) last++;
  for (size_t i = last; i-- > v->draw_start;) {
    if (g->done[i] == 1) continue;
    draw_note(r, &c->notes[i], T, g->done[i] == 2);
  }
  /* roll / balloon counter in a speech cloud */
  for (size_t i = v->draw_start; i < last; i++) {
    const TjaNote *n = &c->notes[i];
    int is_b = n->type == TJA_BALLOON || n->type == TJA_KUSUDAMA;
    int is_r = n->type == TJA_ROLL || n->type == TJA_BIGROLL;
    if (!(is_b || is_r) || g->done[i] || n->t > T || T > n->end_t) continue;
    int shown = is_b ? n->hits - g->roll_hits[i] : g->roll_hits[i];
    if (is_r && shown == 0) continue;
    blit(r, spr.bubble, JX + 96, LANE_Y - 22, 0.85f, 255, 255);
    draw_num(r, JX + 96 - num_w(shown, 3) / 2, LANE_Y - 22 - 24, shown, 3, is_b ? 255 : 80, is_b ? 110 : 150,
             is_b ? 10 : 255, 255, 255, 255);
  }

  /* judgement: outlined kanji rising off the drum, ring on good/ok */
  double age = Tj - g->fb_t;
  if (g->fb_judge && age >= 0 && age < 0.45) {
    static const char *J[] = {"", "\xE8\x89\xAF", "\xE5\x8F\xAF", "\xE4\xB8\x8D\xE5\x8F\xAF", "\xE4\xB8\x8D\xE5\x8F\xAF"};
    static const int F[5][3] = {{0, 0, 0}, {255, 230, 80}, {255, 255, 255}, {120, 170, 255}, {170, 170, 190}};
    static const int O[5][3] = {{0, 0, 0}, {168, 70, 0}, {80, 80, 100}, {16, 28, 100}, {50, 50, 70}};
    int a = (int)(255 * (1.0 - age / 0.45));
    int yy = LANE_Y - 54 - (int)(age * 30);
    int jw = utext_w(r, J[g->fb_judge], 3);
    utext_o(r, JX - jw / 2, yy, J[g->fb_judge], 3, (Uint8)F[g->fb_judge][0], (Uint8)F[g->fb_judge][1],
            (Uint8)F[g->fb_judge][2], (Uint8)O[g->fb_judge][0], (Uint8)O[g->fb_judge][1], (Uint8)O[g->fb_judge][2],
            (Uint8)a);
    if (g->fb_judge <= J_OK && age < 0.2) {
      float sc = (30.0f + (float)age * 150.0f) / 30.0f;
      SDL_SetTextureColorMod(scn.hitring, (Uint8)F[g->fb_judge][0], (Uint8)F[g->fb_judge][1], (Uint8)F[g->fb_judge][2]);
      SDL_SetTextureAlphaMod(scn.hitring, (Uint8)(230 * (1.0 - age / 0.2)));
      SDL_FRect hr = {JX - 40 * sc, cy - 40 * sc, 80 * sc, 80 * sc};
      SDL_RenderTexture(r, scn.hitring, NULL, &hr);
    }
  }

  /* combo under the drum */
  if (g->combo >= 2) {
    int w = num_w(g->combo, 5);
    draw_num(r, 62 - w / 2, LANE_Y + LANE_H + 12, g->combo, 5, 255, 236, 100, 150, 40, 0);
    utext_o(r, 62 - utext_w(r, "COMBO", 1) / 2, LANE_Y + LANE_H + 12 + 84, "COMBO", 1, 255, 255, 255, 40, 50, 100, 255);
  }

  /* progress and trims */
  double prog = g->last_t > 0 ? Tj / g->last_t : 0;
  if (prog < 0) prog = 0;
  if (prog > 1) prog = 1;
  col(r, 14, 22, 58, 190);
  rect(r, 0, SCR_H - 26, SCR_W, 26);
  col(r, 80, 90, 130, 255);
  rect(r, 8, SCR_H - 8, SCR_W - 16, 4);
  col(r, 255, 210, 50, 255);
  rect(r, 8, SCR_H - 8, (float)((SCR_W - 16) * prog), 4);
  char st[120];
  snprintf(st, sizeof st, "judge %+d ms (L2/R2)   %d good  %d ok  %d bad  %d miss", v->offset_ms, g->good, g->ok, g->bad,
           g->miss);
  utext_chars(r, 8, SCR_H - 24, st, 1, 214, 220, 240, 255);

  if (result) {
    col(r, 8, 16, 48, 150);
    rect(r, 0, 0, SCR_W, SCR_H);
    /* light-blue dotted panel, as on the arcade result board */
    col(r, 255, 255, 255, 255);
    rect(r, 50, 50, SCR_W - 100, SCR_H - 100);
    col(r, 150, 214, 255, 255);
    rect(r, 56, 56, SCR_W - 112, SCR_H - 112);
    for (int dy = 0; dy < 14; dy++)
      for (int dx = 0; dx < 24; dx++) {
        col(r, 184, 230, 255, 255);
        fill_circle(r, 66.0f + (float)dx * 22.0f + (dy % 2) * 11.0f, 66.0f + (float)dy * 22.0f, 6);
      }
    col(r, 255, 255, 255, 215);
    rect(r, 70, 140, 270, 240);
    rect(r, 360, 140, SCR_W - 70 - 360, 240);
    pill(r, 70, 70, 120, 24, cr, cg, cb);
    utext_o(r, 82, 74, badge, 1, 255, 255, 255, 60, 20, 30, 255);
    {
      char tb[256];
      snprintf(tb, sizeof tb, "%s", tja->title);
      utext_fit(r, 70, 100, SCR_W - 330, tb, 2, 255, 255, 255);
    }
    int cleared = game_cleared(g);
    const char *verdict = cleared ? "CLEAR" : "FAILED";
    utext_o(r, SCR_W - 70 - utext_w(r, verdict, 4), 66, verdict, 4, cleared ? 255 : 190, cleared ? 230 : 200,
            cleared ? 80 : 230, cleared ? 190 : 40, cleared ? 70 : 60, cleared ? 0 : 110, 255);
    char l[64];
    const char *lab[] = {"\xE8\x89\xAF GOOD", "\xE5\x8F\xAF OK", "\xE4\xB8\x8D\xE5\x8F\xAF BAD", "MISS", "MAX COMBO", "ROLL",
                         "BALLOON"};
    static const int LC[7][3] = {{20, 140, 230}, {40, 170, 60}, {220, 50, 50}, {110, 110, 130},
                                 {230, 130, 20}, {230, 130, 20}, {230, 130, 20}};
    int val[] = {g->good, g->ok, g->bad, g->miss, g->max_combo, g->roll_total, g->balloons_popped};
    for (int i = 0; i < 7; i++) {
      int yy = 146 + i * 33;
      utext_o(r, 82, yy, lab[i], 2, (Uint8)LC[i][0], (Uint8)LC[i][1], (Uint8)LC[i][2], 255, 255, 255, 255);
      if (i == 6) snprintf(l, sizeof l, "%d / %d", val[i], g->balloons_total);
      else snprintf(l, sizeof l, "%d", val[i]);
      utext_o(r, 328 - utext_w(r, l, 2), yy, l, 2, 30, 40, 90, 255, 255, 255, 255);
    }
    snprintf(l, sizeof l, "%ld", g->score);
    utext_o(r, 372, 148, "SCORE", 1, 30, 40, 90, 255, 255, 255, 255);
    utext_o(r, SCR_W - 82 - utext_w(r, l, 4), 172, l, 4, 255, 236, 100, 150, 70, 0, 255);
    soul_gauge(r, 376, 270, SCR_W - 82 - 376, 22, g);
    snprintf(l, sizeof l, "%d%%", (int)lround(g->gauge));
    utext_o(r, SCR_W - 82 - utext_w(r, l, 2), 306, l, 2, 30, 40, 90, 255, 255, 255, 255);
    utext_o(r, SCR_W / 2 - utext_w(r, "PRESS A", 2) / 2, SCR_H - 82, "PRESS A", 2, 255, 255, 255, 30, 60, 130, 255);
  }
}

/* ---- debug snapshots ---------------------------------------------------- */
static const char *snap_path; /* --snap: save one frame after snap_after_s, quit */
static double snap_after_s = 3.0;
static uint64_t snap_start_ns;
static double fake_tap_s = -1; /* TAIKO_FAKE_TAP_MS: calibration taps at click+X */

/* RenderPresent that can also grab the finished frame for --snap. */
static void present(SDL_Renderer *ren) {
  if (snap_path) {
    if (!snap_start_ns) snap_start_ns = mono_ns();
    if ((double)(mono_ns() - snap_start_ns) / 1e9 >= snap_after_s) {
      SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
      int ok = sf && SDL_SaveBMP(sf, snap_path);
      fprintf(stderr, "taiko: snap %s: %s\n", snap_path, ok ? "ok" : SDL_GetError());
      quit_flag = 1;
    }
  }
  SDL_RenderPresent(ren);
}

/* ---- sessions ---------------------------------------------------------- */
static int g_offset_ms;
static char calib_path[1100] = "calib.txt";

static void set_offset(int ms) {
  g_offset_ms = ms;
  g_user_off = ms / 1000.0;
}

static void save_offset(void) {
  FILE *f = fopen(calib_path, "w");
  if (f) {
    fprintf(f, "%d %d %d\n", g_offset_ms, (int)lround(g_vis_off * 1000.0), atomic_load(&g_keymap));
    fclose(f);
  }
}

static int load_audio(const char *tja_path, const Tja *t) {
  char path[1800];
  if (audio_resolve(tja_path, t->wave, path, sizeof path) != 0) {
    fprintf(stderr, "taiko: no playable audio for WAVE:%s next to %s\n", t->wave, tja_path);
    return -1;
  }
  int16_t *data = NULL;
  long frames = 0;
  int rate = 0;
  if (audio_decode(path, &data, &frames, &rate) != 0 || rate < 8000 || rate > 192000) {
    fprintf(stderr, "taiko: cannot decode %s\n", path);
    free(data);
    return -1;
  }
  pcm = data;
  pcm_frames = frames;
  pcm_rate = rate;
  fprintf(stderr, "taiko: audio %s: %d Hz, %ld frames (%.1f s)\n", path, rate, frames, (double)frames / rate);
  return 0;
}

static SDL_AudioStream *audio_start(void) {
  mpos = -(long long)(LEAD_S * pcm_rate);
  memset(voices, 0, sizeof voices);
  se_tail = atomic_load(&se_head);
  hit_tail = atomic_load(&hit_head);
  atomic_store(&anchor.seq, 0);
  anchor.ns = 0;
  anchor.pos_s = 0;
  SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, pcm_rate};
  SDL_AudioStream *as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_audio, NULL);
  if (!as) {
    fprintf(stderr, "taiko: audio: %s\n", SDL_GetError());
    return NULL;
  }
  SDL_ResumeAudioStreamDevice(as);
  return as;
}

static void audio_stop(SDL_AudioStream *as) {
  fprintf(stderr, "taiko: audio session: callbacks=%u hit-sounds-started=%u\n", stat_cb, stat_se);
  stat_cb = stat_se = 0;
  atomic_store(&g_input_on, 0);
  atomic_store(&g_sfx, 0);
  if (as) SDL_DestroyAudioStream(as);
  free(pcm);
  pcm = NULL;
  pcm_frames = 0;
}

/* Hits from the SDL gamepad when no evdev device is available. */
static int sdl_hit_kind(int b) {
  switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return input_kind(IN_A);
    case SDL_GAMEPAD_BUTTON_EAST: return input_kind(IN_B);
    case SDL_GAMEPAD_BUTTON_WEST: return input_kind(IN_Y); /* printed labels, see code_to_kind */
    case SDL_GAMEPAD_BUTTON_NORTH: return input_kind(IN_X);
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return input_kind(IN_L1);
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return input_kind(IN_R1);
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return input_kind(IN_UP);
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return input_kind(IN_DOWN);
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return input_kind(IN_LEFT);
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return input_kind(IN_RIGHT);
    default: return -1;
  }
}

/* ---- stall forensics ---------------------------------------------------- */
/* A 100 ms system-wide stall (display and audio together) was seen once in a
 * real session and could not be reproduced on demand. Every 250 ms the CPU
 * time of all processes is sampled (about 1 ms); when a frame takes > 34 ms the
 * busiest ones over that window are logged, so the next occurrence names the
 * culprit. */
typedef struct {
  int pid;
  char comm[20];
  unsigned long ticks;
} ProcTicks;
#define PW_MAX 600
static ProcTicks pw_a[PW_MAX], pw_b[PW_MAX];
static ProcTicks *pw_prev = pw_a, *pw_cur = pw_b;
static int pw_nprev, pw_ncur;
static uint64_t pw_next_ns;

static void pw_sample(void) {
  ProcTicks *t = pw_prev;
  pw_prev = pw_cur;
  pw_nprev = pw_ncur;
  pw_cur = t;
  pw_ncur = 0;
  DIR *d = opendir("/proc");
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d)) && pw_ncur < PW_MAX) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    char path[300], buf[512];
    snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp) continue;
    ProcTicks *e = &pw_cur[pw_ncur];
    e->pid = atoi(de->d_name);
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof e->comm) cl = sizeof e->comm - 1;
    memcpy(e->comm, lp + 1, cl);
    e->comm[cl] = 0;
    unsigned long ut = 0, st = 0;
    /* fields after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
    e->ticks = ut + st;
    pw_ncur++;
  }
  closedir(d);
}

static void pw_report(double gap_ms, double song_t) {
  struct {
    int idx;
    unsigned long delta;
  } top[3] = {{-1, 0}, {-1, 0}, {-1, 0}};
  for (int i = 0; i < pw_ncur; i++) {
    unsigned long prev = 0;
    for (int j = 0; j < pw_nprev; j++)
      if (pw_prev[j].pid == pw_cur[i].pid) {
        prev = pw_prev[j].ticks;
        break;
      }
    unsigned long delta = pw_cur[i].ticks >= prev ? pw_cur[i].ticks - prev : 0;
    for (int k = 0; k < 3; k++)
      if (delta > top[k].delta) {
        for (int m = 2; m > k; m--) top[m] = top[m - 1];
        top[k].idx = i;
        top[k].delta = delta;
        break;
      }
  }
  char line[300];
  size_t o = (size_t)snprintf(line, sizeof line, "taiko: STALL %.0f ms at song %.2fs; cpu ticks (10 ms) by process:", gap_ms, song_t);
  for (int k = 0; k < 3 && top[k].idx >= 0 && o < sizeof line - 40; k++)
    o += (size_t)snprintf(line + o, sizeof line - o, " %s(%d)+%lu", pw_cur[top[k].idx].comm, pw_cur[top[k].idx].pid,
                          top[k].delta);
  fprintf(stderr, "%s\n", line);
}

/* ---- frame timing ------------------------------------------------------- */
/* Logged per session: with vsync a healthy frame is ~16.7 ms; "late" counts
 * frames whose interval exceeded 20 ms (a missed vsync). cpu = time spent
 * issuing draw calls, present = SDL_RenderPresent (flush + vsync wait). */
#define FT_MAX 6000
typedef struct {
  float interval[FT_MAX], cpu[FT_MAX], present[FT_MAX], song_t[FT_MAX], ev[FT_MAX], logic[FT_MAX];
  int n;
  uint64_t last_start;
} FrameTimes;

static int cmp_f(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

static void ft_report(FrameTimes *f, const char *tag) {
  if (f->n < 10) return;
  float tmp[FT_MAX];
  struct {
    const char *name;
    float *v;
  } sets[3] = {{"interval", f->interval}, {"cpu", f->cpu}, {"present", f->present}};
  fprintf(stderr, "taiko: frames[%s] n=%d", tag, f->n);
  for (int k = 0; k < 3; k++) {
    memcpy(tmp, sets[k].v, (size_t)f->n * sizeof(float));
    qsort(tmp, (size_t)f->n, sizeof(float), cmp_f);
    float sum = 0;
    for (int i = 0; i < f->n; i++) sum += tmp[i];
    fprintf(stderr, "  %s avg/p50/p95/p99/max=%.1f/%.1f/%.1f/%.1f/%.1f", sets[k].name, sum / (float)f->n,
            tmp[f->n / 2], tmp[(int)((float)f->n * 0.95f)], tmp[(int)((float)f->n * 0.99f)], tmp[f->n - 1]);
  }
  int late = 0, very = 0;
  for (int i = 0; i < f->n; i++) {
    if (f->interval[i] > 20.0f) late++;
    if (f->interval[i] > 34.0f) very++;
  }
  fprintf(stderr, "  late(>20ms)=%d very(>34ms)=%d\n", late, very);
  int shown = 0;
  for (int i = 0; i < f->n && shown < 10; i++)
    if (f->interval[i] > 34.0f) {
      /* interval[i] is how long frame i-1 took: print that frame's stages */
      int j = i > 0 ? i - 1 : 0;
      fprintf(stderr,
              "taiko:   slow frame #%d at song %.2fs took %.1f ms (events %.1f, logic %.1f, draw %.1f, present %.1f)\n", j,
              f->song_t[j], f->interval[i], f->ev[j], f->logic[j], f->cpu[j], f->present[j]);
      shown++;
    }
}

/* One frame shown while a (possibly long, mp3) song decodes. */
static void draw_loading(SDL_Renderer *ren, const char *title) {
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  sky(ren, 0, 0, 0);
  col(ren, 14, 22, 58, 200);
  rect(ren, 0, 170, SCR_W, 140);
  utext_o(ren, SCR_W / 2 - utext_w(ren, "LOADING...", 3) / 2, 190, "LOADING...", 3, 255, 224, 70, 150, 60, 0, 255);
  utext_fit(ren, 40, 250, SCR_W - 80, title[0] ? title : "", 2, 255, 255, 255);
  SDL_RenderPresent(ren);
}

/* Plays one course. Returns 1 when the whole app should quit. */
static int play_run(SDL_Renderer *ren, const Tja *tja, const char *tja_path, const TjaCourse *c) {
  Game g;
  if (game_init(&g, c) != 0) return 0;
  View v = {tja, c, &g, 0, 0, g_offset_ms};
  draw_loading(ren, tja->title);
  if (load_audio(tja_path, tja) != 0) {
    game_free(&g);
    return 0;
  }
  synth_se();
  {
    /* warm the per-song textures (title, badge) while the loading screen is up */
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    render(ren, &v, -LEAD_S, -LEAD_S, 0);
    SDL_RenderPresent(ren);
    v.draw_start = v.bar_start = 0;
  }
  SDL_AudioStream *as = audio_start();
  if (!as) {
    audio_stop(NULL);
    game_free(&g);
    return 0;
  }
  atomic_store(&g_sfx, 1);
  atomic_store(&g_input_on, 1);
  atomic_store(&offset_req_ms, 0);
  fprintf(stderr, "taiko: playing %s (%s) latency=%.0f ms offset=%+d ms\n", tja->title, c->name, g_latency * 1000,
          g_offset_ms);

  int sel = 0, start = 0, result = 0, quit_app = 0;
  uint64_t result_at = 0;
  size_t auto_i = 0;
  FrameTimes *ft = calloc(1, sizeof *ft);
  atomic_store(&offset_req_ms, 0);
  while (!quit_flag) {
    uint64_t frame_t0 = mono_ns();
    if (ft && ft->last_start) {
      double gap = (double)(frame_t0 - ft->last_start) / 1e6;
      if (gap > 34.0 && !result) {
        pw_sample();
        pw_report(gap, song_time(frame_t0));
      } else if (frame_t0 >= pw_next_ns) {
        pw_sample();
        pw_next_ns = frame_t0 + 250000000ull;
      }
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) quit_flag = 1;
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      if (e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.type != SDL_EVENT_GAMEPAD_BUTTON_UP) continue;
      int down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      int b = e.gbutton.button;
      if (b == SDL_GAMEPAD_BUTTON_BACK) sel = down;
      else if (b == SDL_GAMEPAD_BUTTON_START) start = down;
      if (!down) continue;
      if (b == SDL_GAMEPAD_BUTTON_SOUTH && result && mono_ns() - result_at > 800000000ull) sel = start = 1;
      int kind = sdl_hit_kind(b);
      if (kind >= 0 && !atomic_load(&have_evdev) && !autoplay && !result) {
        uint64_t ns = mono_ns() - (SDL_GetTicksNS() - e.gbutton.timestamp);
        emit_hit(kind, song_time(ns));
      }
    }
    if (sel && start) break;
    uint64_t t_ev = mono_ns();

    int req = atomic_exchange(&offset_req_ms, 0);
    if (req) {
      set_offset(g_offset_ms + req);
      v.offset_ms = g_offset_ms;
    }
    double Ta = song_time(mono_ns()); /* audio clock: what you hear now */
    double T = Ta + g_user_off;       /* judgement time */
    if (autoplay && !result) {
      while (auto_i < c->n_notes && c->notes[auto_i].t <= T) {
        const TjaNote *n = &c->notes[auto_i++];
        if (n->type > TJA_BIGKA) continue;
        emit_hit((n->type == TJA_DON || n->type == TJA_BIGDON) ? HIT_DON : HIT_KA, n->t - g_user_off);
      }
    }
    unsigned hh = atomic_load(&hit_head);
    while (hit_tail != hh) {
      HitEv h = hit_ring[hit_tail++ & 255];
      game_update(&g, h.t);
      game_hit(&g, h.kind, h.t);
    }
    game_update(&g, T);
    if (!result && game_finished(&g, T)) {
      result = 1;
      result_at = mono_ns();
      fprintf(stderr, "taiko: result score=%ld good=%d ok=%d bad=%d miss=%d maxcombo=%d\n", g.score, g.good, g.ok,
              g.bad, g.miss, g.max_combo);
    }
    uint64_t t_r0 = mono_ns();
    render(ren, &v, T, Ta + g_vis_off, result);
    uint64_t t_r1 = mono_ns();
    present(ren);
    uint64_t t_r2 = mono_ns();
    if (ft && ft->n < FT_MAX && !result) {
      if (ft->last_start) ft->interval[ft->n] = (float)(frame_t0 - ft->last_start) / 1e6f;
      else ft->interval[ft->n] = 16.7f;
      ft->cpu[ft->n] = (float)(t_r1 - t_r0) / 1e6f;
      ft->present[ft->n] = (float)(t_r2 - t_r1) / 1e6f;
      ft->song_t[ft->n] = (float)T;
      ft->ev[ft->n] = (float)(t_ev - frame_t0) / 1e6f;
      ft->logic[ft->n] = (float)(t_r0 - t_ev) / 1e6f;
      ft->n++;
    }
    if (ft) ft->last_start = frame_t0;
  }
  quit_app = quit_flag != 0;
  if (ft) {
    ft_report(ft, c->name);
    free(ft);
  }
  audio_stop(as);
  save_offset();
  game_free(&g);
  return quit_app;
}

/* ---- calibration ------------------------------------------------------- */
#define CAL_FIRST 1.0
#define CAL_STEP 0.6
#define CAL_CLICKS 40
#define CAL_SKIP 3

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/* Returns 1 when the app should quit. */
static int calibrate_run(SDL_Renderer *ren) {
  pcm_rate = 48000;
  pcm_frames = (long)((CAL_FIRST + CAL_STEP * CAL_CLICKS + 1.0) * pcm_rate);
  pcm = calloc((size_t)pcm_frames * 2, sizeof(int16_t));
  synth_se();
  /* metronome: a woodblock-like tick, distinct from the don/ka hit sounds */
  int tick_len = pcm_rate / 20;
  for (int k = 0; k < CAL_CLICKS; k++) {
    long at = (long)((CAL_FIRST + CAL_STEP * k) * pcm_rate);
    for (int i = 0; i < tick_len && at + i < pcm_frames; i++) {
      double t = (double)i / pcm_rate, e = exp(-t / 0.012);
      int16_t v = (int16_t)(13000.0 * e * (sin(2 * M_PI * 880.0 * t) + 0.5 * sin(2 * M_PI * 1320.0 * t)));
      pcm[2 * (at + i)] = pcm[2 * (at + i) + 1] = v;
    }
  }
  SDL_AudioStream *as = audio_start();
  if (!as) {
    audio_stop(NULL);
    return 0;
  }
  atomic_store(&g_sfx, 1); /* taps play the same don/ka sound as in a song */
  atomic_store(&g_input_on, 1);
  atomic_store(&offset_req_ms, 0);

  double deltas[128];
  int n = 0, quit_app = 0, apply = 0, cancel = 0, fake_i = 0;
  int vis_ms = (int)lround(g_vis_off * 1000.0);
  while (!quit_flag && !apply && !cancel) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) quit_flag = 1;
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      if (e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) continue;
      int b = e.gbutton.button;
      if (b == SDL_GAMEPAD_BUTTON_START) apply = 1;
      else if (b == SDL_GAMEPAD_BUTTON_BACK) cancel = 1;
      int kind = sdl_hit_kind(b);
      if (kind >= 0 && !atomic_load(&have_evdev)) {
        uint64_t ns = mono_ns() - (SDL_GetTicksNS() - e.gbutton.timestamp);
        emit_hit(kind, song_time(ns));
      }
    }
    vis_ms += atomic_exchange(&offset_req_ms, 0); /* L2/R2: visual sync */
    if (fake_tap_s >= 0) { /* debug: a steady tapper that is fake_tap_s late */
      double Tn = song_time(mono_ns());
      while (fake_i < CAL_CLICKS && CAL_FIRST + CAL_STEP * fake_i + fake_tap_s <= Tn) {
        emit_hit(HIT_DON, CAL_FIRST + CAL_STEP * fake_i + fake_tap_s - g_user_off);
        fake_i++;
      }
    }
    unsigned hh = atomic_load(&hit_head);
    while (hit_tail != hh) {
      HitEv h = hit_ring[hit_tail++ & 255];
      long k = lround((h.t - CAL_FIRST) / CAL_STEP);
      if (k < 0 || k >= CAL_CLICKS) continue;
      double d = h.t - (CAL_FIRST + CAL_STEP * (double)k);
      fprintf(stderr, "taiko: cal tap click=%ld delta=%+.1f ms%s\n", k, d * 1000.0, fabs(d) > 0.25 ? " (ignored)" : "");
      if (fabs(d) > 0.25) continue; /* sloppy tap: ignore */
      if (n < 128) deltas[n++] = d;
    }

    /* median of the taps after the first few, last 16 at most */
    double med = 0;
    int used = 0;
    if (n > CAL_SKIP) {
      double tmp[128];
      int from = n - 16 > CAL_SKIP ? n - 16 : CAL_SKIP;
      used = n - from;
      memcpy(tmp, deltas + from, (size_t)used * sizeof(double));
      qsort(tmp, (size_t)used, sizeof(double), cmp_d);
      med = used % 2 ? tmp[used / 2] : (tmp[used / 2 - 1] + tmp[used / 2]) / 2;
    }
    double T = song_time(mono_ns());
    double Tf = T + vis_ms / 1000.0; /* the flash is "drawn" on the visual clock */
    int flash = 0;
    if (Tf >= CAL_FIRST - 0.02) {
      double ph = fmod(Tf - CAL_FIRST, CAL_STEP);
      flash = ph >= 0 && ph < 0.06;
    }

    SDL_SetRenderDrawColor(ren, 22, 22, 30, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 250, 220, 90, 255);
    text_c(ren, SCR_W / 2.0f, 20, 3, "CALIBRATION");
    SDL_SetRenderDrawColor(ren, 235, 235, 240, 255);
    text_c(ren, SCR_W / 2.0f, 66, 1, "1) Tap A (or any hit button) in time with the CLICKS. ~15 taps.");
    text_c(ren, SCR_W / 2.0f, 82, 1, "2) L2/R2: make the white square flash exactly with the click.");
    /* timing strip: -150..+150 ms, taps as ticks */
    SDL_SetRenderDrawColor(ren, 60, 60, 74, 255);
    SDL_FRect strip = {70, 150, 500, 30};
    SDL_RenderFillRect(ren, &strip);
    SDL_SetRenderDrawColor(ren, 200, 200, 210, 255);
    SDL_FRect mid = {319, 140, 2, 50};
    SDL_RenderFillRect(ren, &mid);
    for (int i = n > 24 ? n - 24 : 0; i < n; i++) {
      double d = deltas[i] * 1000.0;
      if (d < -150) d = -150;
      if (d > 150) d = 150;
      SDL_SetRenderDrawColor(ren, i < CAL_SKIP ? 120 : 90, i < CAL_SKIP ? 120 : 200, i < CAL_SKIP ? 130 : 120, 255);
      SDL_FRect t = {(float)(320 + d * 500.0 / 300.0) - 1, 154, 3, 22};
      SDL_RenderFillRect(ren, &t);
    }
    SDL_SetRenderDrawColor(ren, 250, 200, 40, 255);
    SDL_FRect m = {(float)(320 + med * 1000.0 * 500.0 / 300.0) - 2, 146, 5, 38};
    if (used > 0) SDL_RenderFillRect(ren, &m);
    SDL_SetRenderDrawColor(ren, 150, 150, 165, 255);
    text(ren, 70, 190, 1, "-150 ms");
    text(ren, 500, 190, 1, "+150 ms");
    text_c(ren, SCR_W / 2.0f, 190, 1, "on the beat");

    char l[96];
    SDL_SetRenderDrawColor(ren, 235, 235, 240, 255);
    snprintf(l, sizeof l, "taps used: %d   median error: %+.0f ms", used, med * 1000.0);
    text_c(ren, SCR_W / 2.0f, 230, 2, l);
    snprintf(l, sizeof l, "offset now %+d ms  ->  new %+d ms", g_offset_ms, g_offset_ms - (int)lround(med * 1000.0));
    SDL_SetRenderDrawColor(ren, used >= 8 ? 120 : 150, used >= 8 ? 230 : 150, used >= 8 ? 140 : 165, 255);
    text_c(ren, SCR_W / 2.0f, 270, 2, l);
    snprintf(l, sizeof l, "visual offset %+d ms (L2/R2)", vis_ms);
    SDL_SetRenderDrawColor(ren, 235, 235, 240, 255);
    text_c(ren, SCR_W / 2.0f, 296, 2, l);
    if (flash) {
      SDL_SetRenderDrawColor(ren, 250, 250, 250, 255);
      SDL_FRect f = {SCR_W / 2.0f - 30, 340, 60, 60};
      SDL_RenderFillRect(ren, &f);
    } else {
      SDL_SetRenderDrawColor(ren, 70, 70, 84, 255);
      SDL_FRect f = {SCR_W / 2.0f - 30, 340, 60, 60};
      SDL_RenderFillRect(ren, &f);
    }
    SDL_SetRenderDrawColor(ren, 150, 150, 165, 255);
    text_c(ren, SCR_W / 2.0f, 430, 1, "START: apply and save     SELECT: cancel");
    present(ren);
    if (T > CAL_FIRST + CAL_STEP * CAL_CLICKS + 0.5) break; /* ran out of clicks */
  }
  quit_app = quit_flag != 0;
  if (apply) {
    g_vis_off = vis_ms / 1000.0;
    save_offset();
  }
  if (apply && n - CAL_SKIP >= 6) {
    /* recompute the median over the same window */
    int from = n - 16 > CAL_SKIP ? n - 16 : CAL_SKIP;
    int used = n - from;
    double tmp[128];
    memcpy(tmp, deltas + from, (size_t)used * sizeof(double));
    qsort(tmp, (size_t)used, sizeof(double), cmp_d);
    double med = used % 2 ? tmp[used / 2] : (tmp[used / 2 - 1] + tmp[used / 2]) / 2;
    set_offset(g_offset_ms - (int)lround(med * 1000.0));
    save_offset();
    fprintf(stderr, "taiko: calibration applied: median %+.1f ms -> offset %+d ms (%d taps)\n", med * 1000.0,
            g_offset_ms, used);
  }
  audio_stop(as);
  return quit_app;
}

/* ---- songs and menu ---------------------------------------------------- */
typedef struct {
  char path[1024];
  char label[256];
  Tja tja;
  int ids[8];
  int n_ids;
} Song;
static Song *songs;
static int n_songs;

static int cmp_song(const void *a, const void *b) {
  return strcasecmp(((const Song *)a)->label, ((const Song *)b)->label);
}

static void add_song(const char *path, const char *fname) {
  Song s;
  memset(&s, 0, sizeof s);
  char err[128];
  if (tja_load(&s.tja, path, err, sizeof err) != 0) return;
  for (size_t i = 0; i < s.tja.n_courses && s.n_ids < 8; i++) {
    const TjaCourse *c = &s.tja.courses[i];
    int dup = 0;
    for (int k = 0; k < s.n_ids; k++) dup |= s.ids[k] == c->id;
    if (c->n_notes && !dup) s.ids[s.n_ids++] = c->id;
  }
  char audio[1800];
  if (s.n_ids == 0 || !s.tja.wave[0] || audio_resolve(path, s.tja.wave, audio, sizeof audio) != 0) {
    if (s.n_ids) fprintf(stderr, "taiko: skip %s: audio \"%s\" not found (ogg/wav/mp3)\n", path, s.tja.wave);
    tja_free(&s.tja);
    return;
  }
  for (int i = 1; i < s.n_ids; i++) { /* ascending course id */
    int x = s.ids[i], j = i - 1;
    while (j >= 0 && s.ids[j] > x) s.ids[j + 1] = s.ids[j], j--;
    s.ids[j + 1] = x;
  }
  snprintf(s.path, sizeof s.path, "%s", path);
  char base[96];
  snprintf(base, sizeof base, "%s", fname);
  char *dot = strrchr(base, '.');
  if (dot) *dot = 0;
  snprintf(s.label, sizeof s.label, "%s", s.tja.title[0] ? s.tja.title : base);
  songs = realloc(songs, (size_t)(n_songs + 1) * sizeof *songs);
  songs[n_songs++] = s;
}

static void scan_dir(const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] == '.') continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    size_t n = strlen(de->d_name);
    if (S_ISDIR(st.st_mode) && depth < 2) scan_dir(p, depth + 1);
    else if (S_ISREG(st.st_mode) && n > 4 && !strcasecmp(de->d_name + n - 4, ".tja")) add_song(p, de->d_name);
  }
  closedir(d);
}

typedef struct {
  int quit;
  int calibrate;
  int song;
  int course_id;
} Choice;

static void draw_menu(SDL_Renderer *ren, int sel, int top, int rows, int items, const int *cidx) {
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  double t = (double)mono_ns() / 1e9;
  sky(ren, t, 0, 0);
  col(ren, 14, 22, 58, 215);
  rect(ren, 0, 0, SCR_W, 62);
  col(ren, 255, 255, 255, 70);
  rect(ren, 0, 60, SCR_W, 2);
  utext_o(ren, 16, 6, "TAIKO", 3, 255, 224, 70, 150, 60, 0, 255); /* 48 px: inside the 62 px band */
  char l[96];
  snprintf(l, sizeof l, "judge %+d ms   visual %+d ms", g_offset_ms, (int)lround(g_vis_off * 1000.0));
  utext(ren, SCR_W - 16 - utext_w(ren, l, 1), 22, l, 1, 214, 220, 240, 255);

  /* a row must hold the 32 px tall text (scale 2) with a margin, and the
   * badge and the text share the row's centre line */
  const int x_list = 20, x_badge = 440, row_h = 36, y0 = 68, band = row_h - 2, ty = 1, by = 6;
  for (int r = 0; r < rows && top + r < items; r++) {
    int it = top + r, y = y0 + r * row_h;
    if (it == sel) {
      col(ren, 14, 22, 58, 190);
      rect(ren, 8, (float)y, SCR_W - 16, (float)band);
      col(ren, 255, 214, 60, 255);
      rect(ren, 8, (float)y, 5, (float)band);
    } else {
      col(ren, 14, 22, 58, 90);
      rect(ren, 8, (float)y, SCR_W - 16, (float)band);
    }
    if (it == 0) {
      utext_o(ren, x_list, y + ty, "[ Calibration ]", 2, 150, 255, 170, 10, 70, 30, 255);
      continue;
    }
    if (it == 1) {
      int km = atomic_load(&g_keymap);
      char kl[64];
      snprintf(kl, sizeof kl, "[ Keys: %s ]", KEYMAP_NAME[km]);
      utext_o(ren, x_list, y + ty, kl, 2, 255, 224, 120, 90, 50, 0, 255);
      utext_o(ren, SCR_W - 16 - utext_w(ren, KEYMAP_HELP[km], 1), y + by + 3, KEYMAP_HELP[km], 1, 255, 255, 255, 14,
              22, 58, 255);
      continue;
    }
    const Song *s = &songs[it - 2];
    int maxw = x_badge - x_list - 12;
    if (it == sel && utext_w(ren, s->label, 2) > maxw) {
      /* the selected title scrolls instead of being cut off */
      int w = utext_w(ren, s->label, 2), gap = 80;
      double off = fmod(t * 45.0, (double)(w + gap));
      SDL_Rect clip = {x_list, y, maxw, band};
      SDL_SetRenderClipRect(ren, &clip);
      utext_o(ren, x_list - (int)off, y + ty, s->label, 2, 255, 255, 255, 14, 22, 58, 255);
      utext_o(ren, x_list - (int)off + w + gap, y + ty, s->label, 2, 255, 255, 255, 14, 22, 58, 255);
      SDL_SetRenderClipRect(ren, NULL);
    } else {
      utext_fit(ren, x_list, y + ty, maxw, s->label, 2, it == sel ? 255 : 225, it == sel ? 255 : 225,
                it == sel ? 255 : 235);
    }
    /* fixed-width course column: it can never be covered by a title */
    int id = s->ids[cidx[it - 2]];
    const TjaCourse *c = tja_course(&s->tja, id);
    int R, G, B;
    course_rgb(id, &R, &G, &B);
    snprintf(l, sizeof l, "%s \xE2\x98\x85%d", course_name(id), c ? c->level : 0);
    pill(ren, x_badge + 22, y + by, 150, 22, R, G, B);
    utext_o(ren, x_badge + 22 + 75 - utext_w(ren, l, 1) / 2, y + by + 3, l, 1, 255, 255, 255, 60, 20, 30, 255);
    if (it == sel) {
      utext_o(ren, x_badge, y + ty, "<", 2, 255, 224, 70, 14, 22, 58, 255);
      utext_o(ren, x_badge + 22 + 150 + 4, y + ty, ">", 2, 255, 224, 70, 14, 22, 58, 255);
    }
  }
  if (n_songs == 0) utext_o(ren, 20, 110, "no songs found", 2, 255, 255, 255, 14, 22, 58, 255);
  col(ren, 14, 22, 58, 215);
  rect(ren, 0, SCR_H - 26, SCR_W, 26);
  utext(ren, 12, SCR_H - 22, "UP/DOWN select   LEFT/RIGHT course or keys   A play   SELECT+START quit", 1, 214, 220, 240, 255);
}

static Choice menu_run(SDL_Renderer *ren, int *sel_io, int *cidx) {
  Choice ch = {0, 0, -1, 0};
  int sel = *sel_io, top = 0, bsel = 0, bstart = 0;
  int items = n_songs + 2; /* item 0 = calibration, 1 = key mapping, then songs */
  const int rows = 10;
  while (!quit_flag) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) quit_flag = 1;
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      if (e.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.type != SDL_EVENT_GAMEPAD_BUTTON_UP) continue;
      int down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      int b = e.gbutton.button;
      if (b == SDL_GAMEPAD_BUTTON_BACK) bsel = down;
      if (b == SDL_GAMEPAD_BUTTON_START) bstart = down;
      if (!down) continue;
      if (b == SDL_GAMEPAD_BUTTON_DPAD_UP && sel > 0) sel--;
      else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN && sel < items - 1) sel++;
      else if ((b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) && sel == 1) {
        int d = b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1;
        atomic_store(&g_keymap, (atomic_load(&g_keymap) + d + NKEYMAPS) % NKEYMAPS);
        save_offset();
      } else if ((b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) && sel > 1) {
        Song *s = &songs[sel - 2];
        int d = b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1;
        cidx[sel - 2] = (cidx[sel - 2] + d + s->n_ids) % s->n_ids;
      } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
        if (sel == 1) { /* A also cycles the key mapping */
          atomic_store(&g_keymap, (atomic_load(&g_keymap) + 1) % NKEYMAPS);
          save_offset();
          continue;
        }
        if (sel == 0) ch.calibrate = 1;
        else {
          ch.song = sel - 2;
          ch.course_id = songs[sel - 2].ids[cidx[sel - 2]];
        }
        *sel_io = sel;
        return ch;
      }
    }
    if (bsel && bstart) break;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;

    draw_menu(ren, sel, top, rows, items, cidx);
    present(ren);
  }
  ch.quit = 1;
  *sel_io = sel;
  return ch;
}

/* Build every static texture and the digit glyphs before the first frame, so
 * nothing is rasterised or uploaded in the middle of a song. */
static void warm_up(SDL_Renderer *r) {
  scene_init(r);
  sprites_init(r);
  for (char d = '0'; d <= '9'; d++) {
    char one[2] = {d, 0};
    utext_cached(r, 0, 0, one, 3, TM_OUTLINE, 255, 255, 255, 214, 120, 10, 0);   /* score */
    utext_cached(r, 0, 0, one, 5, TM_OUTLINE, 255, 236, 100, 150, 40, 0, 0);    /* combo */
    utext_cached(r, 0, 0, one, 3, TM_OUTLINE, 255, 110, 10, 255, 255, 255, 0);  /* balloon count */
    utext_cached(r, 0, 0, one, 3, TM_OUTLINE, 80, 150, 255, 255, 255, 255, 0);  /* roll count */
  }
  for (int c = 33; c < 127; c++) { /* status line */
    char one[2] = {(char)c, 0};
    utext_cached(r, 0, 0, one, 1, TM_PLAIN, 214, 220, 240, 0, 0, 0, 0);
  }
  static const char *J[] = {"\xE8\x89\xAF", "\xE5\x8F\xAF", "\xE4\xB8\x8D\xE5\x8F\xAF"};
  static const int F[3][3] = {{255, 230, 80}, {255, 255, 255}, {120, 170, 255}};
  static const int O[3][3] = {{168, 70, 0}, {80, 80, 100}, {16, 28, 100}};
  for (int k = 0; k < 3; k++)
    utext_cached(r, 0, 0, J[k], 3, TM_OUTLINE, (Uint8)F[k][0], (Uint8)F[k][1], (Uint8)F[k][2], (Uint8)O[k][0],
                 (Uint8)O[k][1], (Uint8)O[k][2], 0);
  /* MISS shows the same "bad" kanji in grey */
  utext_cached(r, 0, 0, J[2], 3, TM_OUTLINE, 170, 170, 190, 50, 50, 70, 0);
}

/* ---- main -------------------------------------------------------------- */
int main(int argc, char **argv) {
  const char *tja_path = NULL, *dump = NULL, *songs_dir = "songs", *screen = NULL;
  int course_id = 0;
  double dump_at = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--tja") && i + 1 < argc) tja_path = argv[++i];
    else if (!strcmp(argv[i], "--songs") && i + 1 < argc) songs_dir = argv[++i];
    else if (!strcmp(argv[i], "--course") && i + 1 < argc) course_id = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--autoplay")) autoplay = 1;
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
    else if (!strcmp(argv[i], "--at") && i + 1 < argc) dump_at = atof(argv[++i]);
    else if (!strcmp(argv[i], "--snap") && i + 1 < argc) snap_path = argv[++i];
    else if (!strcmp(argv[i], "--snap-after") && i + 1 < argc) snap_after_s = atof(argv[++i]);
    else if (!strcmp(argv[i], "--screen") && i + 1 < argc) screen = argv[++i];
  }
  const char *ft = getenv("TAIKO_FAKE_TAP_MS");
  if (ft) fake_tap_s = atof(ft) / 1000.0;
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);

  const char *lat = getenv("TAIKO_LATENCY_MS");
  if (lat) g_latency = atof(lat) / 1000.0;
  const char *vis_env = getenv("TAIKO_VISUAL_MS");
  if (vis_env) g_vis_off = atof(vis_env) / 1000.0;
  const char *bp = SDL_GetBasePath();
  if (bp) snprintf(calib_path, sizeof calib_path, "%scalib.txt", bp);
  FILE *cf = fopen(calib_path, "r");
  if (cf) {
    int ms = 0, vis = 0, km = 0;
    int n = fscanf(cf, "%d %d %d", &ms, &vis, &km);
    if (n >= 1) set_offset(ms);
    if (n >= 2) g_vis_off = vis / 1000.0;
    if (n >= 3 && km >= 0 && km < NKEYMAPS) atomic_store(&g_keymap, km);
    fclose(cf);
  }

  if (load_font(bp ? bp : "./") != 0) fprintf(stderr, "taiko: no unifont.bin, ASCII-only text\n");

  if (!SDL_Init(SDL_INIT_VIDEO | (dump ? 0 : SDL_INIT_AUDIO) | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "taiko: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow("taiko", SCR_W, SCR_H, (dump || snap_path) ? 0 : SDL_WINDOW_FULLSCREEN);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
  if (!ren) {
    fprintf(stderr, "taiko: window/renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderLogicalPresentation(ren, SCR_W, SCR_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);

  if (dump) { /* headless: play perfectly up to dump_at, render one frame */
    Tja tja;
    char err[256];
    if (!tja_path || tja_load(&tja, tja_path, err, sizeof err) != 0) {
      fprintf(stderr, "taiko: --dump needs --tja\n");
      return 1;
    }
    const TjaCourse *c = tja_course(&tja, course_id);
    Game g;
    if (!c || c->n_notes == 0 || game_init(&g, c) != 0) return 1;
    View v = {&tja, c, &g, 0, 0, g_offset_ms};
    for (size_t i = 0; i < c->n_notes && c->notes[i].t <= dump_at; i++) {
      const TjaNote *n = &c->notes[i];
      if (n->type > TJA_BIGKA) {
        for (double t = n->t + 0.02; t < n->end_t && t <= dump_at; t += 0.06) {
          game_update(&g, t);
          game_hit(&g, HIT_DON, t);
        }
        continue;
      }
      game_update(&g, n->t);
      game_hit(&g, (n->type == TJA_DON || n->type == TJA_BIGDON) ? HIT_DON : HIT_KA, n->t);
    }
    game_update(&g, dump_at);
    render(ren, &v, dump_at, dump_at, game_finished(&g, dump_at));
    SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
    int ok = s && SDL_SaveBMP(s, dump);
    fprintf(stderr, "taiko: dump %s at %.2fs: %s\n", dump, dump_at, ok ? "ok" : SDL_GetError());
    return ok ? 0 : 1;
  }

  SDL_SetRenderVSync(ren, 1);
  SDL_HideCursor();
  warm_up(ren);
  pthread_t th;
  int th_ok = pthread_create(&th, NULL, input_thread, NULL) == 0;

  if (tja_path) { /* direct single-song mode (development) */
    Tja tja;
    char err[256];
    if (tja_load(&tja, tja_path, err, sizeof err) != 0) {
      fprintf(stderr, "taiko: %s\n", err);
      return 1;
    }
    const TjaCourse *c = tja_course(&tja, course_id);
    if (!c || c->n_notes == 0) {
      fprintf(stderr, "taiko: no course %d in %s\n", course_id, tja_path);
      return 1;
    }
    play_run(ren, &tja, tja_path, c);
    tja_free(&tja);
  } else {
    scan_dir(songs_dir, 0);
    qsort(songs, (size_t)n_songs, sizeof *songs, cmp_song);
    fprintf(stderr, "taiko: %d songs in %s\n", n_songs, songs_dir);
    int *cidx = calloc((size_t)(n_songs ? n_songs : 1), sizeof(int));
    int sel = n_songs ? 2 : 0;
    if (getenv("TAIKO_MENU_SEL")) sel = atoi(getenv("TAIKO_MENU_SEL"));
    if (screen && !strcmp(screen, "calibrate")) calibrate_run(ren);
    else
    while (!quit_flag) {
      Choice ch = menu_run(ren, &sel, cidx);
      if (ch.quit) break;
      if (ch.calibrate) {
        if (calibrate_run(ren)) break;
      } else {
        Song *s = &songs[ch.song];
        const TjaCourse *c = tja_course(&s->tja, ch.course_id);
        if (c && play_run(ren, &s->tja, s->path, c)) break;
      }
    }
    free(cidx);
  }

  quit_flag = 1;
  if (th_ok) pthread_join(th, NULL);
  SDL_Quit();
  return 0;
}
