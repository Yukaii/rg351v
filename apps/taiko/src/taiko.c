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

#include "game.h"
#include "tja.h"

extern int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);

#define SCR_W 640
#define SCR_H 480
#define LEAD_S 2.0 /* silence before the audio so early notes can scroll in */
#define JX 110     /* judgement circle x */
#define LANE_Y 120
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
    case 304: /* A */
    case 305: /* B */
      return HIT_DON;
    case 544: /* BTN_DPAD_UP..RIGHT: the D-pad is also red */
    case 545:
    case 546:
    case 547:
      return HIT_DON;
    case 306: /* X */
    case 307: /* Y */
    case 308: /* L1 */
    case 309: /* R1 */
      return HIT_KA;
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
          if (ev[k].value != 0 && hat[ax] == 0) kind = HIT_DON;
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

/* ---- drawing ----------------------------------------------------------- */
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

static double px_per_s(double bpm, double scroll) { return bpm * scroll / 60.0 * BEAT_PX; }

static void note_colors(int type, Uint8 *rr, Uint8 *gg, Uint8 *bb) {
  if (type == TJA_DON || type == TJA_BIGDON) {
    *rr = 235, *gg = 64, *bb = 40;
  } else {
    *rr = 60, *gg = 160, *bb = 230;
  }
}

static void draw_note(SDL_Renderer *r, const TjaNote *n, double T, int missed) {
  double sp = px_per_s(n->bpm, n->scroll);
  double x = JX + (n->t - T) * sp;
  Uint8 cr, cg, cb;
  float cy = LANE_Y + LANE_H / 2.0f;
  switch (n->type) {
    case TJA_DON:
    case TJA_KA:
    case TJA_BIGDON:
    case TJA_BIGKA: {
      if (x < -40 || x > SCR_W + 40) return;
      int big = n->type == TJA_BIGDON || n->type == TJA_BIGKA;
      int rad = big ? 28 : 20;
      note_colors(n->type, &cr, &cg, &cb);
      SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
      fill_circle(r, (float)x, cy, rad + 3);
      if (missed) SDL_SetRenderDrawColor(r, (Uint8)(cr / 2), (Uint8)(cg / 2), (Uint8)(cb / 2), 255);
      else SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
      fill_circle(r, (float)x, cy, rad);
      break;
    }
    case TJA_ROLL:
    case TJA_BIGROLL: {
      double x2 = JX + (n->end_t - T) * sp;
      if (x2 < -40 || x > SCR_W + 40) return;
      int h = n->type == TJA_BIGROLL ? 28 : 20;
      double xa = x < JX && x2 > JX ? JX : x;
      SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
      SDL_FRect o = {(float)xa, cy - h - 3, (float)(x2 - xa), (float)(2 * h + 6)};
      SDL_RenderFillRect(r, &o);
      SDL_SetRenderDrawColor(r, 250, 200, 40, 255);
      SDL_FRect f = {(float)xa, cy - h, (float)(x2 - xa), (float)(2 * h)};
      SDL_RenderFillRect(r, &f);
      fill_circle(r, (float)x2, cy, h);
      if (x >= JX) fill_circle(r, (float)x, cy, h);
      break;
    }
    default: { /* balloon, kusudama */
      double xd = x < JX ? JX : x;
      if (xd > SCR_W + 40 || n->end_t < T) return;
      SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
      fill_circle(r, (float)xd, cy, 28);
      SDL_SetRenderDrawColor(r, 240, 120, 30, 255);
      fill_circle(r, (float)xd, cy, 24);
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

static void ascii(const char *in, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < cap; i++) out[j++] = ((unsigned char)in[i] < 0x80) ? in[i] : '?';
  out[j] = 0;
}

static void render(SDL_Renderer *r, View *v, double Tj, double T, int result) {
  const TjaCourse *c = v->c;
  Game *g = v->g;
  int gogo = 0;
  for (size_t i = v->draw_start; i < c->n_notes && c->notes[i].t < T + 0.01; i++) gogo = c->notes[i].gogo;
  SDL_SetRenderDrawColor(r, 22, 22, 30, 255);
  SDL_RenderClear(r);

  SDL_SetRenderDrawColor(r, gogo ? 110 : 52, gogo ? 60 : 52, gogo ? 40 : 62, 255);
  SDL_FRect lane = {0, LANE_Y, SCR_W, LANE_H};
  SDL_RenderFillRect(r, &lane);
  SDL_SetRenderDrawColor(r, 30, 30, 36, 255);
  SDL_FRect cap = {0, LANE_Y, JX + 40, LANE_H};
  SDL_RenderFillRect(r, &cap);

  /* bar lines */
  while (v->bar_start < c->n_bars && c->bars[v->bar_start].t < T - 0.5) v->bar_start++;
  SDL_SetRenderDrawColor(r, 150, 150, 160, 255);
  for (size_t i = v->bar_start; i < c->n_bars && c->bars[i].t < T + 8.0; i++) {
    const TjaBar *b = &c->bars[i];
    if (!b->visible) continue;
    double x = JX + (b->t - T) * px_per_s(b->bpm, b->scroll);
    if (x < JX - 30 || x > SCR_W) continue;
    SDL_FRect f = {(float)x, LANE_Y, 2, LANE_H};
    SDL_RenderFillRect(r, &f);
  }

  /* judgement target, flashes with the last hit */
  float cy = LANE_Y + LANE_H / 2.0f;
  int flash = Tj - g->hit_t < 0.10 && g->hit_t > 0;
  SDL_SetRenderDrawColor(r, 200, 200, 210, 255);
  fill_circle(r, JX, cy, 34);
  if (flash) {
    if (g->hit_kind == HIT_DON) SDL_SetRenderDrawColor(r, 235, 64, 40, 255);
    else SDL_SetRenderDrawColor(r, 60, 160, 230, 255);
  } else {
    SDL_SetRenderDrawColor(r, 40, 40, 48, 255);
  }
  fill_circle(r, JX, cy, 30);

  /* notes, far to near so near ones sit on top */
  while (v->draw_start < c->n_notes) {
    const TjaNote *n = &c->notes[v->draw_start];
    double e = n->end_t > n->t ? n->end_t : n->t;
    if (e < T - 0.4) v->draw_start++;
    else break;
  }
  size_t last = v->draw_start;
  while (last < c->n_notes && c->notes[last].t < T + 8.0) last++;
  for (size_t i = last; i-- > v->draw_start;) {
    if (g->done[i] == 1) continue;
    draw_note(r, &c->notes[i], T, g->done[i] == 2);
  }
  /* balloon counter */
  for (size_t i = v->draw_start; i < last; i++) {
    const TjaNote *n = &c->notes[i];
    if ((n->type == TJA_BALLOON || n->type == TJA_KUSUDAMA) && !g->done[i] && n->t <= T && T <= n->end_t) {
      char b[16];
      snprintf(b, sizeof b, "%d", n->hits - g->roll_hits[i]);
      text_c(r, JX + 70, LANE_Y + 8, 3, b);
    }
  }

  /* feedback text */
  if (g->fb_judge && Tj - g->fb_t < 0.45) {
    static const char *J[] = {"", "GOOD", "OK", "BAD", "MISS"};
    static const Uint8 col[5][3] = {{0, 0, 0}, {255, 220, 80}, {255, 255, 255}, {80, 140, 255}, {160, 160, 170}};
    SDL_SetRenderDrawColor(r, col[g->fb_judge][0], col[g->fb_judge][1], col[g->fb_judge][2], 255);
    text_c(r, JX, LANE_Y - 28, 2, J[g->fb_judge]);
  }

  /* HUD */
  SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
  char t1[160], t2[96];
  ascii(v->tja->title, t1, sizeof t1);
  text(r, 8, 8, 2, t1);
  snprintf(t2, sizeof t2, "%s  lv%d", c->name, c->level);
  text(r, 8, 30, 1, t2);
  snprintf(t2, sizeof t2, "SCORE %ld", g->score);
  text(r, SCR_W - 8 - (float)strlen(t2) * 16, 8, 2, t2);
  if (g->combo >= 2) {
    char cb[24];
    snprintf(cb, sizeof cb, "%d", g->combo);
    SDL_SetRenderDrawColor(r, 250, 220, 90, 255);
    text_c(r, SCR_W / 2.0f, 250, 6, cb);
    SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
    text_c(r, SCR_W / 2.0f, 304, 2, "COMBO");
  }
  /* progress + status line */
  double prog = g->last_t > 0 ? Tj / g->last_t : 0;
  if (prog < 0) prog = 0;
  if (prog > 1) prog = 1;
  SDL_SetRenderDrawColor(r, 70, 70, 84, 255);
  SDL_FRect pb = {8, SCR_H - 22, SCR_W - 16, 6};
  SDL_RenderFillRect(r, &pb);
  SDL_SetRenderDrawColor(r, 250, 200, 40, 255);
  SDL_FRect pf = {8, SCR_H - 22, (float)((SCR_W - 16) * prog), 6};
  SDL_RenderFillRect(r, &pf);
  SDL_SetRenderDrawColor(r, 150, 150, 165, 255);
  snprintf(t2, sizeof t2, "judge %+d ms (L2/R2)   good %d ok %d bad %d miss %d", v->offset_ms, g->good, g->ok,
           g->bad, g->miss);
  text(r, 8, SCR_H - 12, 1, t2);

  if (result) {
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_FRect box = {70, 70, SCR_W - 140, SCR_H - 140};
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 250, 220, 90, 255);
    text_c(r, SCR_W / 2.0f, 90, 3, "RESULT");
    SDL_SetRenderDrawColor(r, 235, 235, 240, 255);
    char l[64];
    snprintf(l, sizeof l, "SCORE     %ld", g->score);
    text(r, 120, 150, 2, l);
    snprintf(l, sizeof l, "GOOD      %d", g->good);
    text(r, 120, 180, 2, l);
    snprintf(l, sizeof l, "OK        %d", g->ok);
    text(r, 120, 205, 2, l);
    snprintf(l, sizeof l, "BAD       %d", g->bad);
    text(r, 120, 230, 2, l);
    snprintf(l, sizeof l, "MISS      %d", g->miss);
    text(r, 120, 255, 2, l);
    snprintf(l, sizeof l, "MAX COMBO %d", g->max_combo);
    text(r, 120, 290, 2, l);
    snprintf(l, sizeof l, "ROLL HITS %d", g->roll_total);
    text(r, 120, 315, 2, l);
    snprintf(l, sizeof l, "BALLOONS  %d/%d", g->balloons_popped, g->balloons_total);
    text(r, 120, 340, 2, l);
    text_c(r, SCR_W / 2.0f, 395, 2, "press A to leave");
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
    fprintf(f, "%d %d\n", g_offset_ms, (int)lround(g_vis_off * 1000.0));
    fclose(f);
  }
}

static int load_audio(const char *tja_path, const Tja *t) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", tja_path);
  char *sl = strrchr(dir, '/');
  if (sl) *sl = 0;
  else strcpy(dir, ".");
  char path[1400];
  snprintf(path, sizeof path, "%s/%s", dir, t->wave);
  int ch = 0, rate = 0;
  short *raw = NULL;
  int n = stb_vorbis_decode_filename(path, &ch, &rate, &raw);
  if (n <= 0 || ch < 1 || ch > 2) {
    fprintf(stderr, "taiko: cannot decode %s (n=%d ch=%d)\n", path, n, ch);
    return -1;
  }
  pcm_rate = rate;
  pcm_frames = n;
  if (ch == 2) {
    pcm = (int16_t *)raw;
  } else {
    pcm = malloc((size_t)n * 2 * sizeof(int16_t));
    for (int i = 0; i < n; i++) pcm[2 * i] = pcm[2 * i + 1] = raw[i];
    free(raw);
  }
  fprintf(stderr, "taiko: audio %s: %d Hz, %d frames (%.1f s)\n", path, rate, n, (double)n / rate);
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
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return HIT_DON;
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return HIT_KA;
    default: return -1;
  }
}

/* Plays one course. Returns 1 when the whole app should quit. */
static int play_run(SDL_Renderer *ren, const Tja *tja, const char *tja_path, const TjaCourse *c) {
  Game g;
  if (game_init(&g, c) != 0) return 0;
  View v = {tja, c, &g, 0, 0, g_offset_ms};
  if (load_audio(tja_path, tja) != 0) {
    game_free(&g);
    return 0;
  }
  synth_se();
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
  atomic_store(&offset_req_ms, 0);
  while (!quit_flag) {
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
    render(ren, &v, T, Ta + g_vis_off, result);
    present(ren);
  }
  quit_app = quit_flag != 0;
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
  char label[96];
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
  if (s.n_ids == 0 || !s.tja.wave[0]) {
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
  ascii(base, s.label, sizeof s.label);
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

static const char *course_name(int id) {
  static const char *N[] = {"Easy", "Normal", "Hard", "Oni", "Ura"};
  return id >= 0 && id <= 4 ? N[id] : "?";
}

typedef struct {
  int quit;
  int calibrate;
  int song;
  int course_id;
} Choice;

static Choice menu_run(SDL_Renderer *ren, int *sel_io, int *cidx) {
  Choice ch = {0, 0, -1, 0};
  int sel = *sel_io, top = 0, bsel = 0, bstart = 0;
  int items = n_songs + 1; /* item 0 = calibration */
  const int rows = 12;
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
      else if ((b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) && sel > 0) {
        Song *s = &songs[sel - 1];
        int d = b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 1 : -1;
        cidx[sel - 1] = (cidx[sel - 1] + d + s->n_ids) % s->n_ids;
      } else if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
        if (sel == 0) ch.calibrate = 1;
        else {
          ch.song = sel - 1;
          ch.course_id = songs[sel - 1].ids[cidx[sel - 1]];
        }
        *sel_io = sel;
        return ch;
      }
    }
    if (bsel && bstart) break;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;

    SDL_SetRenderDrawColor(ren, 22, 22, 30, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 250, 220, 90, 255);
    text(ren, 16, 14, 3, "TAIKO");
    SDL_SetRenderDrawColor(ren, 150, 150, 165, 255);
    char l[96];
    snprintf(l, sizeof l, "judge %+d ms   visual %+d ms", g_offset_ms, (int)lround(g_vis_off * 1000.0));
    text(ren, SCR_W - 16 - (float)strlen(l) * 8, 22, 1, l);
    for (int r = 0; r < rows && top + r < items; r++) {
      int it = top + r;
      float y = 64 + (float)r * 30;
      if (it == sel) {
        SDL_SetRenderDrawColor(ren, 60, 60, 84, 255);
        SDL_FRect hl = {8, y - 6, SCR_W - 16, 28};
        SDL_RenderFillRect(ren, &hl);
      }
      if (it == 0) {
        SDL_SetRenderDrawColor(ren, 120, 230, 140, 255);
        text(ren, 20, y, 2, "[ Calibration ]");
        continue;
      }
      Song *s = &songs[it - 1];
      SDL_SetRenderDrawColor(ren, 235, 235, 240, 255);
      char lab[40];
      snprintf(lab, sizeof lab, "%.38s", s->label);
      text(ren, 20, y, 2, lab);
      const TjaCourse *c = tja_course(&s->tja, s->ids[cidx[it - 1]]);
      snprintf(l, sizeof l, "%s%s%s lv%d", it == sel ? "< " : "", course_name(s->ids[cidx[it - 1]]),
               it == sel ? " >" : "", c ? c->level : 0);
      SDL_SetRenderDrawColor(ren, it == sel ? 250 : 170, it == sel ? 200 : 170, it == sel ? 40 : 180, 255);
      text(ren, SCR_W - 20 - (float)strlen(l) * 16, y, 2, l);
    }
    SDL_SetRenderDrawColor(ren, 150, 150, 165, 255);
    text(ren, 16, SCR_H - 22, 1, "UP/DOWN select   LEFT/RIGHT course   A play   SELECT+START quit");
    if (n_songs == 0) text(ren, 20, 110, 2, "no songs found");
    present(ren);
  }
  ch.quit = 1;
  *sel_io = sel;
  return ch;
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
    int ms = 0, vis = 0;
    int n = fscanf(cf, "%d %d", &ms, &vis);
    if (n >= 1) set_offset(ms);
    if (n >= 2) g_vis_off = vis / 1000.0;
    fclose(cf);
  }

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
    int sel = n_songs ? 1 : 0;
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
