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

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
  char cover_file[160]; /* local JPEG fetched by deckd, empty until ready */
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
  else if (!strcmp(k, "cover_file")) snprintf(m->cover_file, sizeof m->cover_file, "%s", v);
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


/* ---- browsing -------------------------------------------------------- */
static void send_cmd(const char *line);

typedef struct {
  char kind; /* 'p' playlist, 'a' album, 't' track */
  char uri[96];
  char title[160];
  char sub[96];
} Item;

#define MAX_ITEMS 3200
static Item items[MAX_ITEMS];
static int n_items, total_items;
static char list_ctx[160]; /* "playlists", "liked", a playlist URI or "search:<q>" */
static char play_ctx[160]; /* context URI to play tracks inside ("-" = none) */
static char list_title[96];
static int list_busy, list_err;
static char err_msg[96];
static int sel, top, pending_sel = -1, pending_top;
static uint64_t sel_at;

typedef enum { V_NOW, V_MENU, V_LIST, V_OSK } View;
static View view = V_NOW;
static int menu_sel;

typedef struct {
  char ctx[160], title[96];
  int sel, top;
} Frame;
static Frame stack[4];
static int depth;

static char search_q[64];
static int osk_r, osk_c;
static const char *const OSK_ROWS[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.'"};
static const char *const OSK_SPECIAL[4] = {"SPACE", "DEL", "SEARCH", "CANCEL"};

static void list_request(int more) {
  char cmd[256];
  if (list_busy) return;
  int off = more ? n_items : 0;
  if (!strcmp(list_ctx, "playlists"))
    snprintf(cmd, sizeof cmd, "playlists");
  else if (!strcmp(list_ctx, "albums"))
    snprintf(cmd, sizeof cmd, "albums %d", off);
  else if (!strncmp(list_ctx, "search:", 7))
    snprintf(cmd, sizeof cmd, "search %d %s", off, list_ctx + 7);
  else
    snprintf(cmd, sizeof cmd, "tracks %s %d", list_ctx, off);
  list_busy = 1;
  list_err = 0;
  send_cmd(cmd);
}

static void open_list(const char *ctx, const char *title) {
  snprintf(list_ctx, sizeof list_ctx, "%s", ctx);
  snprintf(list_title, sizeof list_title, "%s", title);
  n_items = total_items = 0;
  sel = top = 0;
  play_ctx[0] = 0;
  list_busy = 0; /* a stale reply is dropped by its own page line */
  view = V_LIST;
  list_request(0);
}

static void push_list(const char *ctx, const char *title) {
  if (depth < 4) {
    snprintf(stack[depth].ctx, sizeof stack[depth].ctx, "%s", list_ctx);
    snprintf(stack[depth].title, sizeof stack[depth].title, "%s", list_title);
    stack[depth].sel = sel;
    stack[depth].top = top;
    depth++;
  }
  open_list(ctx, title);
}

static void pop_list(void) {
  if (depth == 0) {
    view = V_MENU;
    return;
  }
  depth--;
  open_list(stack[depth].ctx, stack[depth].title);
  pending_sel = stack[depth].sel;
  pending_top = stack[depth].top;
}

static void osk_move(int dr, int dc) {
  osk_r += dr;
  if (osk_r < 0) osk_r = 4;
  if (osk_r > 4) osk_r = 0;
  int len = osk_r < 4 ? 10 : 4;
  osk_c += dc;
  if (osk_c < 0) osk_c = len - 1;
  if (osk_c >= len) osk_c = osk_r < 4 && dc == 0 ? len - 1 : 0;
  if (osk_c >= len) osk_c = len - 1;
}

static void osk_submit(void) {
  char ctx[160], title[96];
  if (!search_q[0]) return;
  snprintf(ctx, sizeof ctx, "search:%s", search_q);
  snprintf(title, sizeof title, "SEARCH: %s", search_q);
  depth = 0;
  open_list(ctx, title);
}

/* A on the OSK; returns nothing, mutates search_q / view. */
static void osk_press(void) {
  size_t n = strlen(search_q);
  if (osk_r < 4) {
    if (n + 1 < sizeof search_q) {
      search_q[n] = OSK_ROWS[osk_r][osk_c];
      search_q[n + 1] = 0;
    }
    return;
  }
  switch (osk_c) {
  case 0:
    if (n + 1 < sizeof search_q && n > 0) {
      search_q[n] = ' ';
      search_q[n + 1] = 0;
    }
    break;
  case 1:
    if (n) search_q[n - 1] = 0;
    break;
  case 2: osk_submit(); break;
  default: view = V_MENU; break;
  }
}


/* ---- real cover art -------------------------------------------------- */
#define ART_N 192
static uint8_t art_lv[ART_N * ART_N]; /* 0..3 phosphor level per pixel */
static int art_ready;
static char art_file[160];

/* Decode a JPEG, scale to ART_N square (cover-fit), stretch contrast and
 * dither to four phosphor levels with a 4x4 Bayer matrix. */
static int load_art(const char *path) {
  size_t n;
  uint8_t *raw = read_file(path, &n);
  if (!raw) return -1;
  int w, h, ch;
  uint8_t *img = stbi_load_from_memory(raw, (int)n, &w, &h, &ch, 3);
  free(raw);
  if (!img || w < 8 || h < 8) {
    if (img) stbi_image_free(img);
    return -1;
  }
  static float lum[ART_N * ART_N];
  int side = w < h ? w : h; /* centre crop to a square */
  int ox = (w - side) / 2, oy = (h - side) / 2;
  for (int y = 0; y < ART_N; y++)
    for (int x = 0; x < ART_N; x++) {
      /* box filter over the source footprint (area average) */
      int x0 = ox + x * side / ART_N, x1 = ox + (x + 1) * side / ART_N;
      int y0 = oy + y * side / ART_N, y1 = oy + (y + 1) * side / ART_N;
      if (x1 <= x0) x1 = x0 + 1;
      if (y1 <= y0) y1 = y0 + 1;
      unsigned long sum = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const uint8_t *p = &img[((size_t)yy * w + xx) * 3];
          sum += 299u * p[0] + 587u * p[1] + 114u * p[2];
          cnt++;
        }
      lum[y * ART_N + x] = (float)sum / (float)(cnt * 1000);
    }
  stbi_image_free(img);
  /* 2nd and 98th percentile contrast stretch */
  int hist[256] = {0};
  for (int i = 0; i < ART_N * ART_N; i++) hist[(int)lum[i] & 255]++;
  int lo = 0, hi = 255, acc = 0, total = ART_N * ART_N;
  for (int v = 0; v < 256; v++) {
    acc += hist[v];
    if (acc * 100 >= total * 2) { lo = v; break; }
  }
  acc = 0;
  for (int v = 255; v >= 0; v--) {
    acc += hist[v];
    if (acc * 100 >= total * 2) { hi = v; break; }
  }
  if (hi - lo < 24) { lo = lo > 12 ? lo - 12 : 0; hi = lo + 24; }
  static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int y = 0; y < ART_N; y++)
    for (int x = 0; x < ART_N; x++) {
      float v = (lum[y * ART_N + x] - (float)lo) / (float)(hi - lo);
      v = v < 0 ? 0 : v > 1 ? 1 : v;
      v = sqrtf(v); /* lift mid-tones: only four levels to spend */
      float t = ((float)bayer[y & 3][x & 3] + 0.5f) / 16.f - 0.5f;
      int l = (int)floorf(v * 3.f + 0.5f + t);
      art_lv[y * ART_N + x] = (uint8_t)(l < 0 ? 0 : l > 3 ? 3 : l);
    }
  return 0;
}

static void draw_real_art(int x, int y) {
  const uint32_t lv[4] = {T->bg, T->dim, T->mid, T->hi};
  for (int j = 0; j < ART_N; j++) {
    uint32_t *dst = &fb[(y + j) * SCR_W + x];
    const uint8_t *src = &art_lv[j * ART_N];
    for (int i = 0; i < ART_N; i++) dst[i] = lv[src[i]];
    if ((j & 1) == 0) /* faint scanlines like the procedural cover */
      for (int i = 0; i < ART_N; i++) dst[i] = (dst[i] & 0xFF000000) | ((dst[i] & 0x00FEFEFE) >> 1);
  }
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


static void draw_scrollbar(int x, int y, int h, int n, int top_i, int rows) {
  fill(x, y, 4, h, 0xFF14110A);
  if (n <= rows) return;
  int th = h * rows / n;
  if (th < 8) th = 8;
  int ty = y + (h - th) * top_i / (n - rows);
  fill(x, ty, 4, th, T->mid);
}

static void draw_menu(uint64_t t_ms) {
  draw_header(t_ms);
  static const char *const ENT[5] = {"LIKED SONGS", "ALBUMS", "PLAYLISTS", "SEARCH", "NOW PLAYING"};
  static const char *const DESC[5] = {"your saved tracks", "albums you saved, newest first",
                                      "your library lists", "find tracks by name",
                                      "back to the player"};
  text(24, 38, "LIBRARY", T->accent, 1);
  for (int i = 0; i < 5; i++) {
    int y = 64 + i * 76;
    if (i == menu_sel) {
      fill(16, y - 8, SCR_W - 32, 66, 0xFF14110A);
      brackets(16, y - 8, SCR_W - 32, 66, 12, T->hi);
    }
    text(40, y, ENT[i], i == menu_sel ? T->hi : T->mid, 2);
    text(40, y + 34, DESC[i], T->dim, 1);
    if (i == menu_sel) text_right(SCR_W - 40, y + 8, ">", T->hi, 2);
  }
}

#define LIST_ROWS 18
#define LIST_RH 20
#define LIST_Y 72

static void draw_list(uint64_t t_ms) {
  draw_header(t_ms);
  if (sel >= n_items) sel = n_items ? n_items - 1 : 0;
  if (sel < top) top = sel;
  if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
  if (top < 0) top = 0;
  text_clip(16, 32, list_title, T->hi, 2, 16, 470);
  char cnt[32];
  snprintf(cnt, sizeof cnt, "%d / %d", n_items ? sel + 1 : 0, total_items ? total_items : n_items);
  text_right(SCR_W - 16, 40, cnt, T->mid, 1);
  fill(0, 66, SCR_W, 2, T->dim);
  uint64_t since_sel = t_ms > sel_at ? t_ms - sel_at : 0;
  for (int i = 0; i < LIST_ROWS; i++) {
    int idx = top + i;
    if (idx >= n_items) break;
    int y = LIST_Y + i * LIST_RH;
    int cur = idx == sel;
    if (cur) fill(12, y - 2, SCR_W - 32, LIST_RH, T->dim);
    const Item *it = &items[idx];
    text(18, y, it->kind == 'p' ? "=" : it->kind == 'a' ? "#" : ">", cur ? T->hi : T->dim, 1);
    int tw = it->sub[0] ? 380 : 580;
    if (cur)
      marquee(34, y, tw, it->title, T->hi, 1, since_sel);
    else
      text_clip(34, y, it->title, T->mid, 1, 34, 34 + tw);
    if (it->sub[0]) text_clip(424, y, it->sub, cur ? T->mid : T->dim, 1, 424, 618);
  }
  if (n_items == 0) {
    if (list_err) {
      text(24, 120, "COULD NOT LOAD", T->warn, 2);
      text(24, 160, err_msg, T->mid, 1);
    } else if (list_busy) {
      text(24, 120, "LOADING...", T->accent, 2);
    } else {
      text(24, 120, "EMPTY", T->dim, 2);
    }
  } else if (list_busy) {
    text_right(SCR_W - 16, 52, "loading...", T->accent, 1);
  }
  draw_scrollbar(SCR_W - 14, LIST_Y - 2, LIST_ROWS * LIST_RH, n_items, top, LIST_ROWS);
}

static void draw_osk(uint64_t t_ms) {
  draw_header(t_ms);
  text(24, 40, "SEARCH", T->accent, 1);
  char q[80];
  snprintf(q, sizeof q, "%s%s", search_q, (t_ms / 400) & 1 ? "_" : " ");
  fill(16, 70, SCR_W - 32, 44, 0xFF14110A);
  fill(16, 70, SCR_W - 32, 2, T->dim);
  fill(16, 112, SCR_W - 32, 2, T->dim);
  text_clip(26, 78, q, T->hi, 2, 26, SCR_W - 26);
  const int kw = 56, kh = 38, gap = 4, x0 = 24, y0 = 140;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 10; c++) {
      int x = x0 + c * (kw + gap), y = y0 + r * (kh + gap);
      int cur = r == osk_r && c == osk_c;
      fill(x, y, kw, kh, cur ? T->mid : 0xFF14110A);
      char ch[2] = {OSK_ROWS[r][c], 0};
      text(x + (kw - 16) / 2, y + (kh - 32) / 2, ch, cur ? T->bg : T->hi, 2);
    }
  const int sw = 144;
  for (int c = 0; c < 4; c++) {
    int x = x0 + c * (sw + gap), y = y0 + 4 * (kh + gap);
    int cur = osk_r == 4 && c == osk_c;
    fill(x, y, sw, kh, cur ? T->mid : 0xFF14110A);
    int w = text_px(OSK_SPECIAL[c], 1);
    text(x + (sw - w) / 2, y + (kh - 16) / 2, OSK_SPECIAL[c], cur ? T->bg : T->hi, 1);
  }
}

static void draw_hint(void) {
  fill(0, SCR_H - 24, SCR_W, 24, 0xFF14110A);
  fill(0, SCR_H - 24, SCR_W, 2, T->dim);
  const char *h = view == V_MENU   ? "UP/DN:Choose A:Open B:Back START:Now playing SEL+START:Leave"
                  : view == V_LIST ? "A:Open/Play X:Play all L1/R1:Page B:Back START:Now playing"
                  : view == V_OSK  ? "Arrows:Move A:Type B:Delete X:Space START:Search Y:Cancel"
                  : !M.online || !M.connected
                      ? "START:Library  SELECT+START:Leave"
                      : (strcmp(model_state(), "stopped") == 0
                             ? "A:Resume last  START:Library  ^/v:Vol  SEL+START:Leave"
                             : "A:Play/Pause L1/R1:Prev/Next </>:Seek ^/v:Vol X:Shuf Y:Rep START:Library");
  text(8, SCR_H - 20, h, T->mid, 1);
}

/* t_ms is absolute mono_ms(): the same clock as Model.pos_at and title_at. */
static void draw_now(uint64_t t_ms);

static void draw_screen(uint64_t t_ms) {
  switch (view) {
  case V_MENU: draw_menu(t_ms); break;
  case V_LIST: draw_list(t_ms); break;
  case V_OSK: draw_osk(t_ms); break;
  default: draw_now(t_ms); break;
  }
  draw_hint();
}

static void draw_now(uint64_t t_ms) {
  uint64_t since_title = t_ms > M.title_at ? t_ms - M.title_at : 0;
  draw_header(t_ms);
  const int ax = 24, ay = 40;
  brackets(ax - 6, ay - 6, 192 + 12, 192 + 12, 14, T->mid);
  if (art_ready)
    draw_real_art(ax, ay);
  else
    draw_art(ax, ay, M.uri);

  const int px = 256, pw = SCR_W - px - 24;
  if (!M.online) {
    text(px, ay, "DECKD OFFLINE", T->warn, 2);
    text(px, ay + 44, "The player service is not running.", T->mid, 1);
    text(px, ay + 64, "systemctl start rg351v-deckd", T->dim, 1);
  } else if (!M.connected && M.user[0]) {
    /* had a session: the link dropped (suspend, Wi-Fi) and deckd is
     * re-establishing it; button presses are held and run once it is back */
    text(px, ay, "RECONNECTING", T->accent, 2);
    text(px, ay + 44, "Spotify session is coming back...", T->mid, 1);
    text(px, ay + 64, "Play/skip pressed now runs when connected.", T->dim, 1);
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
}

/* ---- deckd socket ---------------------------------------------------- */
static int sfd = -1;
static char inbuf[16384];
static size_t inlen;
static Model pending;
static int in_status;
static int in_list;
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
  char buf[320];
  int n = snprintf(buf, sizeof buf, "%s\n", line);
  if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
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
    } else if (!strncmp(line, "page\t", 5)) {
      int off = atoi(line + 5);
      char *t = strchr(line + 5, '\t');
      in_list = 1;
      if (off == 0) n_items = 0;
      if (t) total_items = atoi(t + 1);
    } else if (!strncmp(line, "context\t", 8)) {
      snprintf(play_ctx, sizeof play_ctx, "%s", line + 8);
      in_list = 1;
    } else if (!strncmp(line, "item\t", 5)) {
      char *f[4] = {0};
      char *c = line + 5;
      for (int i = 0; i < 4 && c; i++) {
        f[i] = c;
        c = strchr(c, '\t');
        if (c) *c++ = 0;
      }
      if (n_items < MAX_ITEMS && f[0] && f[1] && f[2]) {
        Item *it = &items[n_items++];
        it->kind = f[0][0] == 'p' ? 'p' : f[0][0] == 'a' ? 'a' : 't';
        snprintf(it->uri, sizeof it->uri, "%s", f[1]);
        snprintf(it->title, sizeof it->title, "%s", f[2]);
        snprintf(it->sub, sizeof it->sub, "%s", f[3] ? f[3] : "");
      }
      in_list = 1;
      changed = 1;
    } else if (!strcmp(line, ".")) {
      if (in_list) {
        in_list = 0;
        list_busy = 0;
        if (pending_sel >= 0) {
          sel = pending_sel < n_items ? pending_sel : (n_items ? n_items - 1 : 0);
          top = pending_top;
          pending_sel = -1;
        }
      } else {
        pending.online = 1;
        M = pending;
        in_status = 0;
      }
      changed = 1;
    } else if (!strncmp(line, "ERR", 3)) {
      if (list_busy) {
        list_busy = 0;
        list_err = 1;
        in_list = 0;
        snprintf(err_msg, sizeof err_msg, "%s", line[3] ? line + 4 : "error");
        changed = 1;
      }
    } else if (!strncmp(line, "OK", 2)) {
      /* command acknowledgement */
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


#ifndef DECK_NO_SDL
/* One logical button press (a button or a trigger edge, or a key repeat).
 * Button numbers are SDL_GamepadButton values; triggers use shoulder+100. */
static void handle_press(int b, uint64_t now) {
  char cmd[320];
  int vol_pct = (int)((M.volume * 100 + 32767) / 65535);
  const int L1 = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, R1 = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
  const int UP = SDL_GAMEPAD_BUTTON_DPAD_UP, DN = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
  const int LT = SDL_GAMEPAD_BUTTON_DPAD_LEFT, RT = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
  const int A = SDL_GAMEPAD_BUTTON_SOUTH, B = SDL_GAMEPAD_BUTTON_EAST;
  const int X = SDL_GAMEPAD_BUTTON_WEST, Y = SDL_GAMEPAD_BUTTON_NORTH;
  const int START = SDL_GAMEPAD_BUTTON_START;

  if (view == V_MENU) {
    if (b == UP) menu_sel = (menu_sel + 4) % 5;
    else if (b == DN) menu_sel = (menu_sel + 1) % 5;
    else if (b == B || b == START) view = V_NOW;
    else if (b == A) {
      depth = 0;
      if (menu_sel == 0) open_list("liked", "LIKED SONGS");
      else if (menu_sel == 1) open_list("albums", "ALBUMS");
      else if (menu_sel == 2) open_list("playlists", "PLAYLISTS");
      else if (menu_sel == 3) { search_q[0] = 0; osk_r = osk_c = 0; view = V_OSK; }
      else view = V_NOW;
    }
    return;
  }
  if (view == V_OSK) {
    if (b == UP) osk_move(-1, 0);
    else if (b == DN) osk_move(1, 0);
    else if (b == LT) osk_move(0, -1);
    else if (b == RT) osk_move(0, 1);
    else if (b == A) osk_press();
    else if (b == B) { size_t n = strlen(search_q); if (n) search_q[n - 1] = 0; }
    else if (b == X) osk_r = 4, osk_c = 0, osk_press(), osk_r = 0, osk_c = 0;
    else if (b == START) osk_submit();
    else if (b == Y) view = V_MENU;
    return;
  }
  if (view == V_LIST) {
    int page = LIST_ROWS - 1;
    if (b == UP && sel > 0) sel--;
    else if (b == DN && sel + 1 < n_items) sel++;
    else if (b == L1 || b == LT) sel = sel - page < 0 ? 0 : sel - page;
    else if (b == R1 || b == RT) sel = sel + page >= n_items ? (n_items ? n_items - 1 : 0) : sel + page;
    else if (b == B) pop_list();
    else if (b == START) view = V_NOW;
    else if (b == A && sel < n_items) {
      const Item *it = &items[sel];
      if (it->kind != 't') {
        push_list(it->uri, it->title);
      } else {
        snprintf(cmd, sizeof cmd, "play %s %s", play_ctx[0] ? play_ctx : "-", it->uri);
        send_cmd(cmd);
        view = V_NOW;
      }
    } else if (b == X) {
      const char *c = NULL;
      if (n_items && sel < n_items && items[sel].kind != 't') c = items[sel].uri;
      else if (play_ctx[0] && strcmp(play_ctx, "-") != 0) c = play_ctx;
      if (c) {
        snprintf(cmd, sizeof cmd, "load %s", c);
        send_cmd(cmd);
        view = V_NOW;
      }
    }
    sel_at = now;
    /* fetch the next page before the cursor reaches the end */
    if (n_items && n_items < total_items && n_items - sel < 8) list_request(1);
    return;
  }

  /* now playing */
  if (b == A) send_cmd("toggle");
  else if (b == L1) send_cmd("prev");
  else if (b == R1) send_cmd("next");
  else if (b == START) { view = V_MENU; menu_sel = 0; }
  else if (b == LT || b == RT) {
    int64_t p = (int64_t)position_now(now) + (b == RT ? 10000 : -10000);
    if (p < 0) p = 0;
    snprintf(cmd, sizeof cmd, "seek %lld", (long long)p);
    send_cmd(cmd);
  } else if (b == UP || b == DN || b == L1 + 100 || b == R1 + 100) {
    int up = b == UP || b == R1 + 100;
    int v = vol_pct + (up ? 5 : -5);
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    snprintf(cmd, sizeof cmd, "volume %d", v);
    send_cmd(cmd);
    M.volume = (uint32_t)(v * 65535 / 100); /* show it before the event */
  } else if (b == X) {
    snprintf(cmd, sizeof cmd, "shuffle %s", M.shuffle ? "off" : "on");
    send_cmd(cmd);
  } else if (b == Y) {
    snprintf(cmd, sizeof cmd, "repeat %s", M.repeat ? "off" : "on");
    send_cmd(cmd);
  }
}
#endif

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
  if (getenv("DECK_COVER")) art_ready = load_art(getenv("DECK_COVER")) == 0;
  const char *vw = getenv("DECK_VIEW");
  if (vw) {
    static const char *const names[] = {"Anomalie bleue", "Hématome", "New notes",
                                        "\xe6\x9c\x88\xe3\x81\xae\xe5\x85\x89 - Moonlight Sonata (Extended Live Version) [Remastered 2011]",
                                        "STORM I", "Last Train to London", "Veridis Quo"};
    for (int i = 0; i < 30; i++) {
      Item *it = &items[n_items++];
      it->kind = !strcmp(vw, "playlists") ? 'p' : !strcmp(vw, "albums") ? 'a' : 't';
      snprintf(it->uri, sizeof it->uri, "spotify:track:%d", i);
      snprintf(it->title, sizeof it->title, "%s", names[i % 7]);
      snprintf(it->sub, sizeof it->sub, it->kind == 'p' ? "%d tracks" : "Artist %d", 10 + i);
    }
    total_items = 2928;
    sel = 4;
    snprintf(list_title, sizeof list_title, "%s", !strcmp(vw, "playlists") ? "PLAYLISTS" : !strcmp(vw, "albums") ? "ALBUMS" : "LIKED SONGS");
    if (!strcmp(vw, "menu")) { view = V_MENU; menu_sel = 1; }
    else if (!strcmp(vw, "osk")) { view = V_OSK; snprintf(search_q, sizeof search_q, "daft pun"); osk_r = 1; osk_c = 3; }
    else view = V_LIST;
  }
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
  int rep_btn = -1;
  uint64_t rep_next = 0;

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
    if (strcmp(M.cover_file, art_file) != 0) {
      snprintf(art_file, sizeof art_file, "%s", M.cover_file);
      art_ready = M.cover_file[0] && load_art(M.cover_file) == 0;
      dirty = 1;
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
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
            b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
          rep_btn = b;
          rep_next = now + 350;
        }
      } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        b = (SDL_GamepadButton)e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 0;
        if (b == SDL_GAMEPAD_BUTTON_START) start_held = 0;
        if ((int)b == rep_btn) rep_btn = -1;
      } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int hit = e.gaxis.value > 16000;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
          if (hit && !trig_l) { pressed = 1; b = (SDL_GamepadButton)(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER + 100); }
          trig_l = hit;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          if (hit && !trig_r) { pressed = 1; b = (SDL_GamepadButton)(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 100); }
          trig_r = hit;
        }
      }
      if (!pressed) continue;
      fprintf(stderr, "deck: press %d view=%d (blanked=%d online=%d state=%s)\n", (int)b, (int)view,
              blanked, M.online, model_state());
      last_input = now;
      if (blanked) { /* the press that wakes the screen is not a command */
        blanked = 0;
        backlight(1);
        dirty = 1;
        rep_btn = -1;
        continue;
      }
      if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 1;
      if (b == SDL_GAMEPAD_BUTTON_START) start_held = 1;
      if (select_held && start_held) {
        quit_flag = 1;
        break;
      }
      handle_press((int)b, now);
      dirty = 1;
    }
    if (rep_btn >= 0 && now >= rep_next && !blanked) {
      rep_next = now + 80;
      last_input = now;
      handle_press(rep_btn, now);
      dirty = 1;
    }

    if (!blanked && blank_secs > 0 && now - last_input > (uint64_t)blank_secs * 1000) {
      blanked = 1;
      backlight(0);
    }
    if (blanked) continue; /* nothing to draw, nothing to upload */

    uint64_t tick_ms = (view == V_NOW && M.online && is_playing()) || view == V_LIST || view == V_OSK ? 80 : 500;
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
