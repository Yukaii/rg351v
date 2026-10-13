/*
 * deck - Spotify player UI for the RG351V (640x480, gamepad).
 *
 * A remote control for deckd (apps/deckd): it reads the now-playing state from
 * deckd's Unix socket and sends transport commands. Audio never passes
 * through this process, so leaving the app keeps the music playing.
 *
 * Drawn with GNU Unifont bitmaps (same assets and approach as ptterm) in a
 * phosphor-on-black "cyberdeck" look.
 *
 * Build with -DDECK_NO_SDL for a host-side preview: it renders one demo frame
 * to the PPM named by DECK_DUMP and exits.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DECK_NO_SDL
#include <SDL3/SDL.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SCR_W 640
#define SCR_H 480
#define GW 8  /* glyph cell */
#define GH 16

/* ---- assets ---------------------------------------------------------- */
static uint32_t *uf_index; /* 65536 entries, see apps/ptterm/tools/gen-assets.py */
static const uint8_t *uf_blob;

static void *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  void *p = malloc((size_t)n);
  if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
    free(p);
    p = NULL;
  }
  fclose(f);
  if (len) *len = (size_t)n;
  return p;
}

static int load_assets(const char *dir) {
  char path[1024];
  size_t n;
  snprintf(path, sizeof path, "%sunifont.bin", dir);
  uint8_t *u = read_file(path, &n);
  if (!u || n < 8 + 65536 * 4 || memcmp(u, "UF16", 4) != 0) {
    fprintf(stderr, "deck: cannot load %s\n", path);
    return -1;
  }
  uf_index = (uint32_t *)(u + 8);
  uf_blob = u + 8 + 65536 * 4;
  return 0;
}

/* ---- theme ----------------------------------------------------------- */
typedef struct {
  uint32_t bg, dim, mid, hi, accent, warn;
} Theme;

static const Theme THEMES[3] = {
    /* amber */ {0xFF0A0904, 0xFF5A3C00, 0xFFC27D00, 0xFFFFB000, 0xFF3CE8D8, 0xFFFF5A3C},
    /* green */ {0xFF040A05, 0xFF145A1E, 0xFF2EBE48, 0xFF6BFF8A, 0xFFFFC040, 0xFFFF5A3C},
    /* cyan  */ {0xFF03080C, 0xFF0F4A5E, 0xFF1EA8CC, 0xFF70F0FF, 0xFFFFB000, 0xFFFF5A3C},
};
static const Theme *T = &THEMES[0];

/* ---- framebuffer + text ---------------------------------------------- */
static uint32_t fb[SCR_W * SCR_H];

static void fill(int x, int y, int w, int h, uint32_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCR_W) w = SCR_W - x;
  if (y + h > SCR_H) h = SCR_H - y;
  for (int j = 0; j < h; j++) {
    uint32_t *p = &fb[(y + j) * SCR_W + x];
    for (int i = 0; i < w; i++) p[i] = c;
  }
}

/* Corner brackets instead of a full box: reads as a viewfinder / HUD. */
static void brackets(int x, int y, int w, int h, int len, uint32_t c) {
  fill(x, y, len, 2, c); fill(x, y, 2, len, c);
  fill(x + w - len, y, len, 2, c); fill(x + w - 2, y, 2, len, c);
  fill(x, y + h - 2, len, 2, c); fill(x, y + h - len, 2, len, c);
  fill(x + w - len, y + h - 2, len, 2, c); fill(x + w - 2, y + h - len, 2, len, c);
}

/* Decode one UTF-8 code point; returns bytes consumed (>=1). */
static int utf8_next(const char *s, uint32_t *cp) {
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

static int glyph_wide(uint32_t cp) {
  return cp < 0x10000 && cp && (uf_index[cp] & 0x80000000u) != 0;
}

/* Foreground-only glyph at `scale`; pixels outside [cx0,cx1) are clipped. */
static void glyph(int x, int y, uint32_t cp, uint32_t fg, int scale, int cx0, int cx1) {
  if (cp >= 0x10000) cp = 0xFFFD;
  uint32_t ent = cp ? uf_index[cp] : 0;
  const uint8_t *g = ent ? uf_blob + ((ent & 0x7FFFFFFF) - 1) : NULL;
  int wide = (ent & 0x80000000u) != 0;
  int w = wide ? 16 : 8;
  for (int row = 0; row < GH; row++) {
    uint32_t bits;
    if (g)
      bits = wide ? (uint32_t)(g[row * 2] << 8 | g[row * 2 + 1]) : g[row];
    else if (cp > 0x7F) /* missing: hollow box */
      bits = (row == 1 || row == 14) ? 0x7FFE : ((row > 1 && row < 14) ? 0x4002 : 0);
    else
      bits = 0;
    if (!bits) continue;
    for (int px = 0; px < w; px++) {
      if (!((bits >> (w - 1 - px)) & 1)) continue;
      int sx = x + px * scale;
      if (sx < cx0 || sx + scale > cx1) continue;
      fill(sx, y + row * scale, scale, scale, fg);
    }
  }
}

static int text_px(const char *s, int scale) {
  int w = 0;
  while (*s) {
    uint32_t cp;
    s += utf8_next(s, &cp);
    w += (glyph_wide(cp) ? 16 : 8) * scale;
  }
  return w;
}

static void text_clip(int x, int y, const char *s, uint32_t fg, int scale, int cx0, int cx1) {
  while (*s) {
    uint32_t cp;
    s += utf8_next(s, &cp);
    int adv = (glyph_wide(cp) ? 16 : 8) * scale;
    if (x + adv > cx0 && x < cx1) glyph(x, y, cp, fg, scale, cx0, cx1);
    x += adv;
  }
}

static void text(int x, int y, const char *s, uint32_t fg, int scale) {
  text_clip(x, y, s, fg, scale, 0, SCR_W);
}

static void text_right(int xr, int y, const char *s, uint32_t fg, int scale) {
  text(xr - text_px(s, scale), y, s, fg, scale);
}

/* Text in a box of width w: static if it fits, else a marquee. */
static void marquee(int x, int y, int w, const char *s, uint32_t fg, int scale, uint64_t t_ms) {
  int tw = text_px(s, scale);
  if (tw <= w) {
    text_clip(x, y, s, fg, scale, x, x + w);
    return;
  }
  int gap = 48 * scale;
  int span = tw + gap;
  /* Hold at the start for ~1.5 s, then scroll at 40 px/s. */
  int64_t off = (int64_t)t_ms - 1500;
  int shift = off > 0 ? (int)((off * 40 / 1000) % span) : 0;
  text_clip(x - shift, y, s, fg, scale, x, x + w);
  text_clip(x - shift + span, y, s, fg, scale, x, x + w);
}

/* ---- model ----------------------------------------------------------- */
typedef struct {
  int online; /* deckd socket reachable */
  int connected; /* deckd has a Spotify session */
  char state[16], user[64], uri[128], title[256], artist[256], album[256];
  uint32_t duration_ms, position_ms, volume; /* volume 0..65535 */
  int shuffle, repeat;
  uint64_t pos_at; /* mono_ms() when position_ms was received */
  uint64_t title_at; /* mono_ms() when the title last changed (marquee start) */
} Model;

static Model M;
static int bat_pct = -1, wifi_bars = -1;

static const char *model_state(void) { return M.state[0] ? M.state : "stopped"; }
static int is_playing(void) { return strcmp(model_state(), "playing") == 0; }

static uint32_t position_now(uint64_t now_ms) {
  if (!is_playing()) return M.position_ms;
  uint64_t p = M.position_ms + (now_ms - M.pos_at);
  return p > M.duration_ms ? M.duration_ms : (uint32_t)p;
}

static void apply_kv(Model *m, const char *k, const char *v, uint64_t now_ms) {
  if (!strcmp(k, "state")) snprintf(m->state, sizeof m->state, "%s", v);
  else if (!strcmp(k, "connected")) m->connected = atoi(v);
  else if (!strcmp(k, "user")) snprintf(m->user, sizeof m->user, "%s", v);
  else if (!strcmp(k, "uri")) snprintf(m->uri, sizeof m->uri, "%s", v);
  else if (!strcmp(k, "title")) {
    if (strcmp(m->title, v) != 0) m->title_at = now_ms;
    snprintf(m->title, sizeof m->title, "%s", v);
  }
  else if (!strcmp(k, "artist")) snprintf(m->artist, sizeof m->artist, "%s", v);
  else if (!strcmp(k, "album")) snprintf(m->album, sizeof m->album, "%s", v);
  else if (!strcmp(k, "duration_ms")) m->duration_ms = (uint32_t)atoll(v);
  else if (!strcmp(k, "position_ms")) {
    m->position_ms = (uint32_t)atoll(v);
    m->pos_at = now_ms;
  } else if (!strcmp(k, "volume")) m->volume = (uint32_t)atoll(v);
  else if (!strcmp(k, "shuffle")) m->shuffle = atoi(v);
  else if (!strcmp(k, "repeat")) m->repeat = atoi(v);
}

/* ---- screen ---------------------------------------------------------- */
static uint32_t fnv(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static uint32_t mix(uint32_t h) {
  h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
  return h;
}

/* Procedural "cover": a mirrored 24x24 pattern seeded by the track URI,
 * four phosphor levels. Real album art can replace it later. */
static void draw_art(int x, int y, const char *seed) {
  uint32_t h = fnv(seed[0] ? seed : "deck");
  const int N = 24, C = 8;
  const uint32_t lv[4] = {T->bg, T->dim, T->mid, T->hi};
  fill(x, y, N * C, N * C, T->bg);
  for (int gy = 0; gy < N; gy++)
    for (int gx = 0; gx < N / 2; gx++) {
      uint32_t v = mix(h ^ (uint32_t)(gy * 31 + gx * 7 + 1));
      /* bias towards the centre so it reads as a shape, not noise */
      int dx = gx - N / 4, dy = gy - N / 2;
      int keep = (dx * dx + dy * dy) < 100 + (int)(v & 31);
      int l = keep ? 1 + (int)((v >> 8) % 3) : (int)((v >> 8) % 8 == 0);
      fill(x + gx * C, y + gy * C, C, C, lv[l]);
      fill(x + (N - 1 - gx) * C, y + gy * C, C, C, lv[l]);
    }
  /* CRT-ish scanlines over the art only */
  for (int gy = 0; gy < N * C; gy += 2)
    for (int gx = 0; gx < N * C; gx++) {
      uint32_t *p = &fb[(y + gy) * SCR_W + x + gx];
      *p = (*p & 0xFF000000) | ((*p & 0x00FEFEFE) >> 1);
    }
}

static void fmt_time(char *out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  snprintf(out, n, "%02u:%02u", s / 60, s % 60);
}

static void draw_segbar(int x, int y, int w, int h, int pct, uint32_t on, uint32_t off) {
  const int seg = 6, gap = 2;
  int n = w / (seg + gap);
  int lit = pct * n / 100;
  for (int i = 0; i < n; i++) fill(x + i * (seg + gap), y, seg, h, i < lit ? on : off);
}

static void draw_wifi(int x, int y, int bars) {
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    fill(x + i * 5, y + 14 - h, 3, h, i < bars ? T->mid : T->dim);
  }
}

static void draw_spectrum(int x, int y, int w, int h, uint64_t t_ms, uint32_t seed, int playing) {
  static float amp;
  static int amp_init;
  if (!amp_init) {
    amp = playing ? 1.f : 0.f;
    amp_init = 1;
  }
  amp += ((playing ? 1.f : 0.f) - amp) * 0.12f;
  const int bw = 10, gap = 6, seg = 4, sgap = 2;
  int n = w / (bw + gap);
  float t = (float)t_ms / 1000.f;
  for (int i = 0; i < n; i++) {
    uint32_t r = mix(seed ^ (uint32_t)(i * 2654435761u));
    float f1 = 1.5f + (r & 7) * 0.45f, f2 = 0.7f + ((r >> 4) & 7) * 0.3f;
    float v = 0.5f + 0.5f * sinf(t * f1 + (float)(r >> 8 & 255));
    v = 0.25f + 0.75f * (0.6f * v + 0.4f * (0.5f + 0.5f * sinf(t * f2 + (float)i)));
    /* low bars (bass) swing harder, as a spectrum would */
    v *= 1.f - 0.35f * ((float)i / (float)n);
    int px = x + i * (bw + gap);
    int lit = (int)(v * amp * (float)(h / (seg + sgap)));
    if (amp < 0.03f) lit = 0; /* silent: no floor */
    else if (lit < 1) lit = 1;
    int rows = h / (seg + sgap);
    for (int k = 0; k < rows; k++) {
      uint32_t c = k < lit ? (k > rows * 3 / 4 ? T->hi : (k > rows / 3 ? T->mid : T->dim)) : 0xFF14110A;
      fill(px, y + h - (k + 1) * (seg + sgap), bw, seg, c);
    }
  }
}

static void draw_header(uint64_t t_ms) {
  fill(0, 0, SCR_W, SCR_H, T->bg);
  text(8, 2, "DECK//SPOT", T->hi, 1);
  /* blinking record dot while playing */
  if (is_playing() && ((t_ms / 500) & 1)) fill(96, 6, 6, 6, T->warn);
  char buf[64];
  int xr = SCR_W - 8;
  if (bat_pct >= 0) {
    snprintf(buf, sizeof buf, "BAT %d%%", bat_pct);
    text_right(xr, 2, buf, bat_pct < 20 ? T->warn : T->mid, 1);
    xr -= text_px(buf, 1) + 16;
  }
  if (wifi_bars >= 0) {
    draw_wifi(xr - 18, 2, wifi_bars);
    xr -= 34;
  }
  int vol = (int)((M.volume * 100 + 32767) / 65535);
  snprintf(buf, sizeof buf, "VOL %3d", vol);
  text_right(xr, 2, buf, T->mid, 1);
  fill(0, 22, SCR_W, 2, T->dim);
}

static void draw_hint(void) {
  fill(0, SCR_H - 24, SCR_W, 24, 0xFF14110A);
  fill(0, SCR_H - 24, SCR_W, 2, T->dim);
  const char *h = !M.online || !M.connected
                      ? "SELECT+START:Leave"
                      : (strcmp(model_state(), "stopped") == 0
                             ? "A:Resume last  ^/v:Vol  SELECT+START:Leave"
                             : "A:Play/Pause L1/R1:Prev/Next </>:Seek ^/v:Vol X:Shuf Y:Rep SEL+START:Leave");
  text(8, SCR_H - 20, h, T->mid, 1);
}

/* t_ms is absolute mono_ms(): the same clock as Model.pos_at and title_at. */
static void draw_screen(uint64_t t_ms) {
  uint64_t since_title = t_ms > M.title_at ? t_ms - M.title_at : 0;
  draw_header(t_ms);
  const int ax = 24, ay = 40;
  brackets(ax - 6, ay - 6, 192 + 12, 192 + 12, 14, T->mid);
  draw_art(ax, ay, M.uri);

  const int px = 256, pw = SCR_W - px - 24;
  if (!M.online) {
    text(px, ay, "DECKD OFFLINE", T->warn, 2);
    text(px, ay + 44, "The player service is not running.", T->mid, 1);
    text(px, ay + 64, "systemctl start rg351v-deckd", T->dim, 1);
  } else if (!M.connected) {
    text(px, ay, "NO SESSION", T->warn, 2);
    text(px, ay + 44, "Waiting for Spotify...", T->mid, 1);
    text(px, ay + 64, "Open Spotify on a phone, pick RG351V.", T->dim, 1);
  } else if (M.title[0] == 0) {
    text(px, ay, "READY", T->hi, 2);
    text(px, ay + 44, "Open Spotify on a phone or computer", T->mid, 1);
    text(px, ay + 64, "and choose the device RG351V.", T->mid, 1);
    if (M.user[0]) {
      char b[96];
      snprintf(b, sizeof b, "signed in as %s", M.user);
      text(px, ay + 100, b, T->dim, 1);
    }
  } else {
    text(px, ay, is_playing() ? "NOW PLAYING" : (strcmp(model_state(), "paused") == 0 ? "PAUSED" : "STOPPED"),
         is_playing() ? T->accent : T->dim, 1);
    marquee(px, ay + 24, pw, M.title, T->hi, 2, since_title);
    marquee(px, ay + 66, pw, M.artist, T->mid, 1, since_title);
    marquee(px, ay + 90, pw, M.album, T->dim, 1, since_title);

    uint32_t pos = position_now(t_ms);
    char a[16], b[16], tb[48];
    fmt_time(a, sizeof a, pos);
    fmt_time(b, sizeof b, M.duration_ms);
    snprintf(tb, sizeof tb, "%s", a);
    text(px, ay + 128, tb, T->hi, 2);
    snprintf(tb, sizeof tb, "/ %s", b);
    text_right(px + pw, ay + 128 + 8, tb, T->mid, 1);
    int pct = M.duration_ms ? (int)((uint64_t)pos * 100 / M.duration_ms) : 0;
    draw_segbar(px, ay + 168, pw, 14, pct, T->hi, 0xFF14110A);
    text(px, ay + 196, "SHUF", M.shuffle ? T->accent : T->dim, 1);
    text(px + 56, ay + 196, "REP", M.repeat ? T->accent : T->dim, 1);
    int vol = (int)((M.volume * 100 + 32767) / 65535);
    draw_segbar(px + pw - 120, ay + 198, 120, 8, vol, T->mid, 0xFF14110A);
  }
  draw_spectrum(16, 268, SCR_W - 32, 168, t_ms, fnv(M.uri), is_playing());
  fill(0, 448, SCR_W, 2, T->dim);
  draw_hint();
}

/* ---- deckd socket ---------------------------------------------------- */
static int sfd = -1;
static char inbuf[16384];
static size_t inlen;
static Model pending;
static int in_status;
static int want_status = 1;

static void conn_close(void) {
  if (sfd >= 0) close(sfd);
  sfd = -1;
  inlen = 0;
  in_status = 0;
  M.online = 0;
}

static int conn_open(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un a;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  sfd = fd;
  const char *hello = "subscribe\nstatus\n";
  if (write(fd, hello, strlen(hello)) < 0) {
    conn_close();
    return -1;
  }
  M.online = 1;
  return 0;
}

static void send_cmd(const char *line) {
  fprintf(stderr, "deck: send '%s' (%s)\n", line, sfd < 0 ? "no socket" : "ok");
  if (sfd < 0) return;
  char buf[96];
  int n = snprintf(buf, sizeof buf, "%s\n", line);
  if (write(sfd, buf, (size_t)n) < 0 && errno != EAGAIN) conn_close();
}

/* Returns 1 if the model changed. */
static int conn_read(uint64_t now_ms) {
  int changed = 0;
  ssize_t n = read(sfd, inbuf + inlen, sizeof inbuf - inlen - 1);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    conn_close();
    return 1;
  }
  if (n < 0) return 0;
  inlen += (size_t)n;
  inbuf[inlen] = 0;
  char *line = inbuf;
  for (;;) {
    char *nl = memchr(line, '\n', inbuf + inlen - line);
    if (!nl) break;
    *nl = 0;
    if (!strncmp(line, "event ", 6)) {
      want_status = 1;
    } else if (!strcmp(line, ".")) {
      pending.online = 1;
      M = pending;
      in_status = 0;
      changed = 1;
    } else if (!strncmp(line, "OK", 2) || !strncmp(line, "ERR", 3)) {
      /* command acknowledgements */
    } else {
      char *eq = strchr(line, '=');
      if (eq) {
        if (!in_status) {
          pending = M;
          in_status = 1;
        }
        *eq = 0;
        apply_kv(&pending, line, eq + 1, now_ms);
      }
    }
    line = nl + 1;
  }
  size_t rest = (size_t)(inbuf + inlen - line);
  memmove(inbuf, line, rest);
  inlen = rest;
  if (inlen >= sizeof inbuf - 1) inlen = 0; /* garbage guard */
  return changed;
}

/* ---- system bits ----------------------------------------------------- */
static int read_int_file(const char *path, int def) {
  FILE *f = fopen(path, "r");
  if (!f) return def;
  int v = def;
  if (fscanf(f, "%d", &v) != 1) v = def;
  fclose(f);
  return v;
}

static int read_wifi_bars(void) {
  FILE *f = fopen("/proc/net/wireless", "r");
  if (!f) return -1;
  char line[256];
  int bars = -1;
  while (fgets(line, sizeof line, f)) {
    char *c = strstr(line, "wlan0:");
    if (!c) continue;
    float link, level;
    if (sscanf(c + 6, " %*x %f %f", &link, &level) == 2) {
      bars = level > -55 ? 4 : level > -65 ? 3 : level > -75 ? 2 : level > -85 ? 1 : 0;
    }
  }
  fclose(f);
  return bars;
}

static void backlight(int on) {
  FILE *f = fopen("/sys/class/backlight/backlight/bl_power", "w");
  if (!f) return;
  fputs(on ? "0" : "4", f);
  fclose(f);
}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static volatile sig_atomic_t quit_flag;
static void on_signal(int sig) {
  (void)sig;
  quit_flag = 1;
}

static void demo_model(void) {
  M.online = 1;
  M.connected = 1;
  snprintf(M.state, sizeof M.state, "playing");
  snprintf(M.user, sizeof M.user, "demo");
  snprintf(M.uri, sizeof M.uri, "spotify:track:6z4n862KhNJNWDYSn4aLL5");
  snprintf(M.title, sizeof M.title, "Anomalie bleue");
  snprintf(M.artist, sizeof M.artist, "L'Imp\xc3\xa9ratrice");
  snprintf(M.album, sizeof M.album, "Tako Tsubo");
  M.duration_ms = 232178;
  M.position_ms = 102000;
  M.volume = 41534;
  M.shuffle = 1;
  bat_pct = 78;
  wifi_bars = 3;
}

#ifdef DECK_NO_SDL
int main(void) {
  const char *dir = getenv("DECK_ASSETS");
  char d[512];
  snprintf(d, sizeof d, "%s/", dir ? dir : ".");
  if (load_assets(d) != 0) return 1;
  const char *th = getenv("DECK_THEME");
  if (th && !strcmp(th, "green")) T = &THEMES[1];
  if (th && !strcmp(th, "cyan")) T = &THEMES[2];
  demo_model();
  if (getenv("DECK_DEMO_CJK")) {
    snprintf(M.title, sizeof M.title, "\xe6\x9c\x88\xe3\x81\xae\xe5\x85\x89 - Moonlight Sonata (Extended Live Version)");
    snprintf(M.artist, sizeof M.artist, "\xe3\x81\x82\xe3\x81\xa8\xe3\x81\xa7 Artist, Another Artist");
  }
  /* A large absolute clock, like mono_ms() on a device that has been up
   * for a while: 1.5 s after the position was stamped. */
  const uint64_t base = 987654321ULL;
  const uint64_t extra = getenv("DECK_T") ? (uint64_t)atoi(getenv("DECK_T")) : 0;
  M.pos_at = base;
  M.title_at = base;
  draw_screen(base + 1500 + extra);
  const char *out = getenv("DECK_DUMP");
  FILE *f = fopen(out ? out : "deck.ppm", "wb");
  if (!f) return 1;
  fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
  for (int i = 0; i < SCR_W * SCR_H; i++) {
    uint8_t px[3] = {(uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i]};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
  return 0;
}
#else
int main(int argc, char **argv) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

  const char *sock = getenv("DECK_SOCK");
  if (!sock) sock = "/run/rg351v/deckd.sock";
  int blank_secs = getenv("DECK_BLANK_SECS") ? atoi(getenv("DECK_BLANK_SECS")) : 60;
  const char *th = getenv("DECK_THEME");
  if (th && !strcmp(th, "green")) T = &THEMES[1];
  if (th && !strcmp(th, "cyan")) T = &THEMES[2];
  int demo = argc > 1 && !strcmp(argv[1], "--demo");

  FILE *pf = fopen("/tmp/deck-ui.pid", "w");
  if (pf) {
    fprintf(pf, "%d\n", (int)getpid());
    fclose(pf);
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "deck: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  const char *base = SDL_GetBasePath();
  if (!base) base = "./";
  if (load_assets(base) != 0) return 1;

  SDL_Window *win = SDL_CreateWindow("deck", SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
  if (!ren) {
    fprintf(stderr, "deck: window/renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderLogicalPresentation(ren, SCR_W, SCR_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, SCR_W, SCR_H);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  SDL_HideCursor();
  backlight(1);

  if (demo) demo_model();
  uint64_t last_conn_try = 0, last_status = 0, last_sys = 0, last_draw = 0, last_input = mono_ms();
  int select_held = 0, start_held = 0, trig_l = 0, trig_r = 0, blanked = 0, dirty = 1;

  while (!quit_flag) {
    uint64_t now = mono_ms();
    /* socket */
    if (!demo) {
      if (sfd < 0) {
        if (now - last_conn_try > 1000) {
          last_conn_try = now;
          if (conn_open(sock) == 0) {
            want_status = 1;
            dirty = 1;
          } else if (M.online) {
            M.online = 0;
            dirty = 1;
          }
        }
        SDL_Delay(30);
      } else {
        struct pollfd p = {sfd, POLLIN, 0};
        if (poll(&p, 1, 30) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))
          if (conn_read(now)) dirty = 1;
        if (want_status || now - last_status > 3000) {
          want_status = 0;
          last_status = now;
          send_cmd("status");
        }
      }
    } else {
      SDL_Delay(30);
    }
    if (now - last_sys > 5000) {
      last_sys = now;
      if (!demo) {
        bat_pct = read_int_file("/sys/class/power_supply/battery/capacity", -1);
        wifi_bars = read_wifi_bars();
      }
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_EVENT_QUIT) quit_flag = 1;
      if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
      int pressed = 0;
      SDL_GamepadButton b = SDL_GAMEPAD_BUTTON_INVALID;
      if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b = (SDL_GamepadButton)e.gbutton.button;
        pressed = 1;
      } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        b = (SDL_GamepadButton)e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 0;
        if (b == SDL_GAMEPAD_BUTTON_START) start_held = 0;
      } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int hit = e.gaxis.value > 16000;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
          if (hit && !trig_l) { pressed = 1; b = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER + 100; }
          trig_l = hit;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          if (hit && !trig_r) { pressed = 1; b = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 100; }
          trig_r = hit;
        }
      }
      if (!pressed) continue;
      fprintf(stderr, "deck: press %d (blanked=%d online=%d state=%s)\n", (int)b, blanked, M.online,
              model_state());
      last_input = now;
      if (blanked) { /* the press that wakes the screen is not a command */
        blanked = 0;
        backlight(1);
        dirty = 1;
        continue;
      }
      if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 1;
      if (b == SDL_GAMEPAD_BUTTON_START) start_held = 1;
      if (select_held && start_held) {
        quit_flag = 1;
        break;
      }
      int vol_pct = (int)((M.volume * 100 + 32767) / 65535);
      char cmd[48];
      switch ((int)b) {
      case SDL_GAMEPAD_BUTTON_SOUTH: send_cmd("toggle"); break;
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: send_cmd("prev"); break;
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: send_cmd("next"); break;
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: {
        int64_t p = (int64_t)position_now(now) + (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? 10000 : -10000);
        if (p < 0) p = 0;
        snprintf(cmd, sizeof cmd, "seek %lld", (long long)p);
        send_cmd(cmd);
        break;
      }
      case SDL_GAMEPAD_BUTTON_DPAD_UP:
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER + 100:
      case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 100: {
        int up = b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 100;
        int v = vol_pct + (up ? 5 : -5);
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        snprintf(cmd, sizeof cmd, "volume %d", v);
        send_cmd(cmd);
        M.volume = (uint32_t)(v * 65535 / 100); /* show it before the event */
        break;
      }
      case SDL_GAMEPAD_BUTTON_WEST: /* X */
        snprintf(cmd, sizeof cmd, "shuffle %s", M.shuffle ? "off" : "on");
        send_cmd(cmd);
        break;
      case SDL_GAMEPAD_BUTTON_NORTH: /* Y */
        snprintf(cmd, sizeof cmd, "repeat %s", M.repeat ? "off" : "on");
        send_cmd(cmd);
        break;
      default: break;
      }
      dirty = 1;
    }

    if (!blanked && blank_secs > 0 && now - last_input > (uint64_t)blank_secs * 1000) {
      blanked = 1;
      backlight(0);
    }
    if (blanked) continue; /* nothing to draw, nothing to upload */

    uint64_t tick_ms = M.online && is_playing() ? 80 : 500;
    if ((dirty || now - last_draw >= tick_ms) && now - last_draw >= 33) {
      last_draw = now;
      dirty = 0;
      draw_screen(now);
      SDL_UpdateTexture(tex, NULL, fb, SCR_W * 4);
      SDL_RenderClear(ren);
      SDL_RenderTexture(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
    }
  }

  backlight(1);
  conn_close();
  unlink("/tmp/deck-ui.pid");
  SDL_Quit();
  return 0;
}
#endif
