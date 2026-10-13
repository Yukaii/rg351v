/*
 * ptterm - a tiny Big5-UAO BBS terminal for the RG351V (640x480, gamepad).
 *
 * Runs `ssh bbsu@ptt.cc` (or any host given as argv[1]) on a pty, decodes
 * Big5-UAO into an 80x24 cell grid in which every double-byte character is
 * exactly two cells wide (what BBS art expects, including "ambiguous"
 * box-drawing), and draws it with GNU Unifont bitmaps via SDL3.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <SDL3/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define COLS 80
#define ROWS 24
#define CW 8
#define CH 16 /* glyph bitmap height */
#define CELL_H 19 /* on-screen cell height: 3 glyph rows are repeated (ROW_MAP) */
#define SCR_W (COLS * CW) /* 640 */
#define SCR_H 480
#define TERM_H (ROWS * CELL_H) /* 456 */
#define HINT_H (SCR_H - TERM_H) /* 24: one hint line */

enum { F_BOLD = 1, F_BLINK = 2, F_REV = 4, F_UL = 8 };

typedef struct {
  uint16_t cp;   /* Unicode code point, 0 = blank */
  uint8_t fg, bg;
  uint8_t flags;
  uint8_t kind;  /* 0 single, 1 wide lead, 2 wide trail */
} Cell;

static Cell grid[ROWS][COLS];
static int cur_r, cur_c, save_r, save_c;
static uint8_t cur_fg = 7, cur_bg = 0, cur_flags;
static int dirty = 1;

/* ---- assets ---------------------------------------------------------- */
static uint32_t *uf_index; /* 65536 entries, see tools/gen-assets.py */
static const uint8_t *uf_blob;
static uint16_t *b5_table; /* 126 x 157 */

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
    fprintf(stderr, "ptterm: cannot load %s\n", path);
    return -1;
  }
  uf_index = (uint32_t *)(u + 8);
  uf_blob = u + 8 + 65536 * 4;
  snprintf(path, sizeof path, "%sbig5uao.bin", dir);
  b5_table = read_file(path, &n);
  if (!b5_table || n != 126 * 157 * 2) {
    fprintf(stderr, "ptterm: cannot load %s\n", path);
    return -1;
  }
  return 0;
}

static uint16_t big5_to_unicode(uint8_t lead, uint8_t trail) {
  if (lead < 0x81 || lead > 0xFE) return 0;
  int col;
  if (trail >= 0x40 && trail <= 0x7E)
    col = trail - 0x40;
  else if (trail >= 0xA1 && trail <= 0xFE)
    col = 63 + trail - 0xA1;
  else
    return 0;
  return b5_table[(lead - 0x81) * 157 + col];
}

/* ---- screen model ---------------------------------------------------- */
static void blank_cell(Cell *c) {
  c->cp = 0;
  c->fg = 7;
  c->bg = cur_bg;
  c->flags = 0;
  c->kind = 0;
}

static void clear_rows(int r0, int r1) {
  for (int r = r0; r <= r1; r++)
    for (int c = 0; c < COLS; c++) blank_cell(&grid[r][c]);
  dirty = 1;
}

static void scroll_up(void) {
  memmove(&grid[0], &grid[1], sizeof(Cell) * COLS * (ROWS - 1));
  clear_rows(ROWS - 1, ROWS - 1);
}

static void scroll_down(void) {
  memmove(&grid[1], &grid[0], sizeof(Cell) * COLS * (ROWS - 1));
  clear_rows(0, 0);
}

static void linefeed(void) {
  if (cur_r == ROWS - 1)
    scroll_up();
  else
    cur_r++;
}

/* Overwriting half of a wide character blanks the other half. */
static void break_wide(int r, int c) {
  if (grid[r][c].kind == 1 && c + 1 < COLS) blank_cell(&grid[r][c + 1]);
  if (grid[r][c].kind == 2 && c > 0) blank_cell(&grid[r][c - 1]);
}

static void put_cell(uint16_t cp, int wide) {
  int need = wide ? 2 : 1;
  if (cur_c + need > COLS) {
    cur_c = 0;
    linefeed();
  }
  for (int i = 0; i < need; i++) break_wide(cur_r, cur_c + i);
  Cell *c = &grid[cur_r][cur_c];
  c->cp = cp;
  c->fg = cur_fg;
  c->bg = cur_bg;
  c->flags = cur_flags;
  c->kind = wide ? 1 : 0;
  if (wide) {
    Cell *t = &grid[cur_r][cur_c + 1];
    *t = *c;
    t->cp = 0;
    t->kind = 2;
  }
  cur_c += need;
  dirty = 1;
}

/* ---- escape sequence parser (operates on raw Big5 bytes) -------------- */
enum { S_NORM, S_ESC, S_CSI, S_SKIP1 };
static int pstate = S_NORM;
static int params[8], nparams;
static int have_param;
/* Multi-byte decoder: UTF-8 first (PTT's "bbsu" service), Big5-UAO fallback
 * for bytes that are not valid UTF-8 (its welcome art is raw Big5). */
static int utf8_first = 1;
static uint8_t mb[4];
static int mb_len, mb_need, mb_big5;

static int param(int i, int def) {
  return (i < nparams && params[i] > 0) ? params[i] : def;
}

static void sgr(void) {
  if (nparams == 0) {
    params[0] = 0;
    nparams = 1;
  }
  for (int i = 0; i < nparams; i++) {
    int p = params[i];
    if (p == 0) {
      cur_fg = 7;
      cur_bg = 0;
      cur_flags = 0;
    } else if (p == 1) {
      cur_flags |= F_BOLD;
    } else if (p == 4) {
      cur_flags |= F_UL;
    } else if (p == 5 || p == 6) {
      cur_flags |= F_BLINK;
    } else if (p == 7) {
      cur_flags |= F_REV;
    } else if (p == 22) {
      cur_flags &= (uint8_t)~F_BOLD;
    } else if (p == 25) {
      cur_flags &= (uint8_t)~F_BLINK;
    } else if (p == 27) {
      cur_flags &= (uint8_t)~F_REV;
    } else if (p >= 30 && p <= 37) {
      cur_fg = (uint8_t)(p - 30);
    } else if (p == 39) {
      cur_fg = 7;
    } else if (p >= 40 && p <= 47) {
      cur_bg = (uint8_t)(p - 40);
    } else if (p == 49) {
      cur_bg = 0;
    } else if (p >= 90 && p <= 97) {
      cur_fg = (uint8_t)(p - 90 + 8);
    } else if (p >= 100 && p <= 107) {
      cur_bg = (uint8_t)(p - 100 + 8);
    }
  }
}

static void csi_dispatch(uint8_t f) {
  int n = param(0, 1);
  switch (f) {
  case 'A': cur_r -= n; if (cur_r < 0) cur_r = 0; break;
  case 'B': cur_r += n; if (cur_r >= ROWS) cur_r = ROWS - 1; break;
  case 'C': cur_c += n; if (cur_c >= COLS) cur_c = COLS - 1; break;
  case 'D': cur_c -= n; if (cur_c < 0) cur_c = 0; break;
  case 'E': cur_r += n; if (cur_r >= ROWS) cur_r = ROWS - 1; cur_c = 0; break;
  case 'F': cur_r -= n; if (cur_r < 0) cur_r = 0; cur_c = 0; break;
  case 'G': cur_c = param(0, 1) - 1; if (cur_c >= COLS) cur_c = COLS - 1; break;
  case 'd': cur_r = param(0, 1) - 1; if (cur_r >= ROWS) cur_r = ROWS - 1; break;
  case 'H':
  case 'f':
    cur_r = param(0, 1) - 1;
    cur_c = param(1, 1) - 1;
    if (cur_r >= ROWS) cur_r = ROWS - 1;
    if (cur_c >= COLS) cur_c = COLS - 1;
    break;
  case 'J': {
    int m = nparams ? params[0] : 0;
    if (m == 2 || m == 3) {
      clear_rows(0, ROWS - 1);
    } else if (m == 1) {
      clear_rows(0, cur_r > 0 ? cur_r - 1 : 0);
      for (int c = 0; c <= cur_c; c++) blank_cell(&grid[cur_r][c]);
    } else {
      for (int c = cur_c; c < COLS; c++) blank_cell(&grid[cur_r][c]);
      if (cur_r + 1 < ROWS) clear_rows(cur_r + 1, ROWS - 1);
    }
    dirty = 1;
    break;
  }
  case 'K': {
    int m = nparams ? params[0] : 0;
    int c0 = (m == 0) ? cur_c : 0;
    int c1 = (m == 1) ? cur_c : COLS - 1;
    if (m == 2) c0 = 0, c1 = COLS - 1;
    for (int c = c0; c <= c1; c++) {
      break_wide(cur_r, c);
      blank_cell(&grid[cur_r][c]);
    }
    dirty = 1;
    break;
  }
  case 'L':
    for (int i = 0; i < n; i++) {
      memmove(&grid[cur_r + 1], &grid[cur_r], sizeof(Cell) * COLS * (ROWS - 1 - cur_r));
      clear_rows(cur_r, cur_r);
    }
    break;
  case 'M':
    for (int i = 0; i < n; i++) {
      memmove(&grid[cur_r], &grid[cur_r + 1], sizeof(Cell) * COLS * (ROWS - 1 - cur_r));
      clear_rows(ROWS - 1, ROWS - 1);
    }
    break;
  case 'P':
    for (int i = 0; i < n; i++) {
      memmove(&grid[cur_r][cur_c], &grid[cur_r][cur_c + 1], sizeof(Cell) * (COLS - 1 - cur_c));
      blank_cell(&grid[cur_r][COLS - 1]);
    }
    dirty = 1;
    break;
  case '@':
    for (int i = 0; i < n; i++) {
      memmove(&grid[cur_r][cur_c + 1], &grid[cur_r][cur_c], sizeof(Cell) * (COLS - 1 - cur_c));
      blank_cell(&grid[cur_r][cur_c]);
    }
    dirty = 1;
    break;
  case 'X':
    for (int i = 0; i < n && cur_c + i < COLS; i++) blank_cell(&grid[cur_r][cur_c + i]);
    dirty = 1;
    break;
  case 'm': sgr(); break;
  case 's': save_r = cur_r; save_c = cur_c; break;
  case 'u': cur_r = save_r; cur_c = save_c; break;
  default: break; /* scroll regions, modes, device reports: ignored */
  }
}

static void feed_byte(uint8_t b) {
  switch (pstate) {
  case S_ESC:
    pstate = S_NORM;
    if (b == '[') {
      pstate = S_CSI;
      nparams = 0;
      have_param = 0;
      memset(params, 0, sizeof params);
    } else if (b == '(' || b == ')' || b == '#') {
      pstate = S_SKIP1;
    } else if (b == '7') {
      save_r = cur_r; save_c = cur_c;
    } else if (b == '8') {
      cur_r = save_r; cur_c = save_c;
    } else if (b == 'D') {
      linefeed();
    } else if (b == 'M') {
      if (cur_r == 0) scroll_down(); else cur_r--;
    } else if (b == 'E') {
      cur_c = 0; linefeed();
    } else if (b == 'c') {
      cur_fg = 7; cur_bg = 0; cur_flags = 0;
      clear_rows(0, ROWS - 1);
      cur_r = cur_c = 0;
    }
    return;
  case S_SKIP1:
    pstate = S_NORM;
    return;
  case S_CSI:
    if (b >= '0' && b <= '9') {
      if (nparams == 0) nparams = 1;
      if (nparams <= 8) {
        params[nparams - 1] = params[nparams - 1] * 10 + (b - '0');
        have_param = 1;
      }
    } else if (b == ';') {
      if (nparams == 0) nparams = 1;
      if (nparams < 8) nparams++;
    } else if (b >= 0x40 && b <= 0x7E) {
      pstate = S_NORM;
      csi_dispatch(b);
    } else if (b == 0x1B) {
      pstate = S_ESC;
    }
    /* '?', '=', '>' and intermediates: ignored */
    return;
  default:
    break;
  }

  /* S_NORM */
  if (mb_len > 0) {
    if (!mb_big5 && b >= 0x80 && b <= 0xBF) {
      mb[mb_len++] = b;
      if (mb_len - 1 == mb_need) {
        uint32_t cp;
        if (mb_len == 2)
          cp = ((mb[0] & 0x1Fu) << 6) | (mb[1] & 0x3Fu);
        else if (mb_len == 3)
          cp = ((mb[0] & 0x0Fu) << 12) | ((mb[1] & 0x3Fu) << 6) | (mb[2] & 0x3Fu);
        else
          cp = 0xFFFF; /* beyond the BMP: Unifont subset has no glyph */
        mb_len = 0;
        put_cell(cp > 0xFFFF ? '?' : (uint16_t)cp, 1);
      }
      return;
    }
    /* Not (or no longer) UTF-8: reinterpret what we have as Big5. */
    uint8_t held[4];
    int n = mb_len;
    memcpy(held, mb, sizeof held);
    mb_len = 0;
    uint8_t t = n >= 2 ? held[1] : b; /* candidate Big5 trail byte */
    if ((t >= 0x40 && t <= 0x7E) || (t >= 0xA1 && t <= 0xFE)) {
      put_cell(big5_to_unicode(held[0], t), 1);
      for (int i = 2; i < n; i++) feed_byte(held[i]);
      if (n >= 2) feed_byte(b);
      return;
    }
    put_cell('?', 0);
    for (int i = 1; i < n; i++) feed_byte(held[i]);
    /* fall through to handle b */
  }
  if (b >= 0x81 && b <= 0xFE) {
    mb[0] = b;
    mb_len = 1;
    mb_big5 = 0;
    if (utf8_first && b >= 0xC2 && b <= 0xF4)
      mb_need = b <= 0xDF ? 1 : (b <= 0xEF ? 2 : 3);
    else {
      mb_big5 = 1;
      mb_need = 1;
    }
  } else if (b == 0x1B) {
    pstate = S_ESC;
  } else if (b == '\r') {
    cur_c = 0;
  } else if (b == '\n') {
    linefeed();
  } else if (b == '\b') {
    if (cur_c > 0) cur_c--;
  } else if (b == '\t') {
    cur_c = (cur_c / 8 + 1) * 8;
    if (cur_c >= COLS) cur_c = COLS - 1;
  } else if (b >= 0x20 && b < 0x7F) {
    put_cell(b, 0);
  } /* BEL and other controls ignored */
}

/* ---- rendering -------------------------------------------------------- */
/* Colors are 0xAARRGGBB in ANSI order. Backgrounds stay dark so light text on
 * them (PTT's blue title bars) keeps its contrast; foregrounds are lifted so
 * dark red/blue/yellow text is readable on this small LCD (all >= 5:1 on black;
 * the VGA values were as low as 2.6:1). PTTERM_PALETTE=vga restores the old
 * foregrounds. */
static const uint32_t PAL_BG[16] = {
    0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAA5500, 0xFF3030DD, 0xFFAA00AA,
    0xFF00AAAA, 0xFFAAAAAA, 0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
    0xFF6C6CFF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF};
static const uint32_t PAL_FG_BRIGHT[16] = {
    0xFF000000, 0xFFE05050, 0xFF50D060, 0xFFE6D23C, 0xFF5A8CFF, 0xFFD264D2,
    0xFF3CC8DC, 0xFFE5E5E5, 0xFF828282, 0xFFFF6B6B, 0xFF6BFF6B, 0xFFFFFF70,
    0xFF6EA0FF, 0xFFFF80FF, 0xFF70F0FF, 0xFFFFFFFF};
static const uint32_t *pal_fg = PAL_FG_BRIGHT;

static uint32_t fb[SCR_W * SCR_H];

/* Draws one glyph into a (box_w x 16) box. Narrow glyphs in wide boxes are
 * doubled horizontally so BBS block art stays connected. */
static uint16_t dbl8[256]; /* 8 bits -> 16 bits, each bit doubled */

static void init_tables(void) {
  for (int v = 0; v < 256; v++) {
    uint16_t o = 0;
    for (int b = 0; b < 8; b++)
      if (v & (1 << b)) o |= (uint16_t)(3u << (2 * b));
    dbl8[v] = o;
  }
}

/* 16 glyph rows stretched to 19 by repeating rows 0, 7 and 12: those carry the
 * least isolated ink across Unifont (a repeated row thickens strokes that sit
 * alone on it), and repeating full rows keeps box/block art connected. */
static const uint8_t ROW_MAP[CELL_H] = {0, 0, 1, 2, 3, 4, 5, 6, 7, 7,
                                        8, 9, 10, 11, 12, 12, 13, 14, 15};

/* tall=1: CELL_H rows (terminal cells); tall=0: native CH rows (UI text). */
static void blit_glyph(int x, int y, uint16_t cp, int box_w, uint32_t fg, uint32_t bg, int ul,
                       int tall) {
  uint32_t ent = cp ? uf_index[cp] : 0;
  const uint8_t *g = ent ? uf_blob + ((ent & 0x7FFFFFFF) - 1) : NULL;
  int gw = (ent & 0x80000000u) ? 16 : 8;
  uint32_t full = (1u << box_w) - 1;
  int rows = tall ? CELL_H : CH;
  for (int orow = 0; orow < rows; orow++) {
    int row = tall ? ROW_MAP[orow] : orow;
    uint32_t bits = 0;
    if (g) {
      uint32_t gb = gw == 16 ? (uint32_t)(g[row * 2] << 8 | g[row * 2 + 1]) : g[row];
      if (box_w == 16 && gw == 8)
        bits = dbl8[gb];
      else if (box_w == 8 && gw == 16)
        bits = gb >> 8; /* clipped to the left half */
      else
        bits = gb;
    } else if (cp > 0x7F && box_w == 16) {
      /* missing glyph: hollow box */
      bits = (row == 1 || row == 14) ? 0x7FFE : ((row > 1 && row < 14) ? 0x4002 : 0);
    }
    if (ul && orow == rows - 1) bits = full;
    uint32_t *dst = &fb[(y + orow) * SCR_W + x];
    for (int px = 0; px < box_w; px++) dst[px] = ((bits >> (box_w - 1 - px)) & 1) ? fg : bg;
  }
}

static int row_has_blink(int r) {
  for (int c = 0; c < COLS; c++)
    if (grid[r][c].flags & F_BLINK) return 1;
  return 0;
}

static int dirty_y0, dirty_y1; /* pixel rows touched by the last render() */

/* full=0: only the cursor row and rows with blinking cells (blink-only frame) */
static void render(int blink_on, int show_cursor, int full) {
  dirty_y0 = SCR_H;
  dirty_y1 = 0;
  for (int r = 0; r < ROWS; r++) {
    if (!full && r != cur_r && !row_has_blink(r)) continue;
    if (r * CELL_H < dirty_y0) dirty_y0 = r * CELL_H;
    if ((r + 1) * CELL_H > dirty_y1) dirty_y1 = (r + 1) * CELL_H;
    for (int c = 0; c < COLS; c++) {
      Cell *cell = &grid[r][c];
      if (cell->kind == 2) continue; /* drawn with its lead */
      uint8_t fgi = cell->fg, bgi = cell->bg;
      if ((cell->flags & F_BOLD) && fgi < 8) fgi += 8;
      if (cell->flags & F_REV) {
        uint8_t t = fgi;
        fgi = bgi;
        bgi = t;
      }
      uint32_t fg = pal_fg[fgi & 15], bg = PAL_BG[bgi & 15];
      uint16_t cp = cell->cp;
      if ((cell->flags & F_BLINK) && !blink_on) cp = 0;
      if (show_cursor && blink_on && r == cur_r && c == cur_c) {
        uint32_t t = fg;
        fg = bg;
        bg = t == bg ? 0xFFAAAAAA : t;
      }
      blit_glyph(c * CW, r * CELL_H, cp, cell->kind == 1 ? 16 : 8, fg, bg,
                 (cell->flags & F_UL) != 0, 1);
    }
  }
}

/* The single hint line under the terminal: a connection dot, then text. */
static void draw_hint(const char *text, int connected) {
  for (int y = TERM_H; y < SCR_H; y++)
    for (int x = 0; x < SCR_W; x++) fb[y * SCR_W + x] = 0xFF000000;
  uint32_t dot = connected ? 0xFF55FF55 : 0xFFFF5555;
  for (int y = 0; y < 6; y++)
    for (int x = 0; x < 6; x++) fb[(TERM_H + (HINT_H - 6) / 2 + y) * SCR_W + 1 + x] = dot;
  int ty = TERM_H + (HINT_H - CH) / 2;
  int x = CW;
  for (; *text && x < SCR_W; text++, x += CW)
    blit_glyph(x, ty, (uint8_t)*text, 8, 0xFF55FFFF, 0xFF000000, 0, 0);
}

/* ---- on-screen keyboard (5 x 16 px overlay; top or bottom half) ---------- */
static const char *const OSK_ROWS[4] = {"1234567890-_./", "qwertyuiop@:!?",
                                        "asdfghjkl;#$%&", "zxcvbnm,'\"()=+"};
static const char *const OSK_SPECIAL[5] = {"SHIFT", "SPACE", "BKSP", "ENTER", "CLOSE"};
enum { OSK_COLS = 14, OSK_KEY_W = 40, OSK_SPECIAL_W = 112 };
static int osk_open, osk_r, osk_c, osk_shift;

static int osk_row_len(int r) { return r < 4 ? OSK_COLS : 5; }

static void osk_move(int dr, int dc) {
  osk_r = (osk_r + dr + 5) % 5;
  int len = osk_row_len(osk_r);
  if (osk_c >= len) osk_c = len - 1;
  osk_c = (osk_c + dc + len) % len;
}

static void box_text(int x, int y, int w, const char *label, uint32_t fg, uint32_t bg) {
  for (int row = 0; row < CH; row++)
    for (int px = 0; px < w; px++) fb[(y + row) * SCR_W + x + px] = bg;
  int tx = x + (w - (int)strlen(label) * CW) / 2;
  for (; *label; label++, tx += CW) blit_glyph(tx, y, (uint8_t)*label, 8, fg, bg, 0, 0);
}

/* Opposite half from the cursor, so the line being typed stays visible. */
static int osk_y0(void) { return cur_r >= ROWS / 2 ? 0 : SCR_H - 5 * CH; }

static void osk_draw(void) {
  for (int r = 0; r < 5; r++) {
    int y = osk_y0() + r * CH;
    int n = osk_row_len(r);
    int kw = r < 4 ? OSK_KEY_W : OSK_SPECIAL_W;
    int x = (SCR_W - n * kw) / 2;
    for (int c = 0; c < n; c++) {
      char label[8];
      if (r < 4) {
        char ch = OSK_ROWS[r][c];
        if (osk_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        label[0] = ch;
        label[1] = 0;
      } else {
        snprintf(label, sizeof label, "%s", OSK_SPECIAL[c]);
        if (c == 0 && osk_shift) snprintf(label, sizeof label, "shift*");
      }
      int sel = (r == osk_r && c == osk_c);
      box_text(x + c * kw, y, kw, label, sel ? 0xFF000000 : 0xFFFFFFFF,
               sel ? 0xFF55FFFF : 0xFF202020);
    }
  }
}

/* ---- saved login ------------------------------------------------------ */
/* /storage/.config/ptterm/account: line 1 = ID, line 2 = password. Lives on
 * the ext4 /storage (root-only, not shared over SMB), never under roms. */
static char acct_user[64], acct_pass[96];
static int acct_loaded;
static char macro_buf[128];
static uint64_t macro_at;
static int macro_pending;

static void load_account(void) {
  const char *path = getenv("PTTERM_ACCOUNT");
  if (!path) path = "/storage/.config/ptterm/account";
  FILE *f = fopen(path, "r");
  if (!f) return;
  if (fgets(acct_user, sizeof acct_user, f) && fgets(acct_pass, sizeof acct_pass, f)) {
    acct_user[strcspn(acct_user, "\r\n")] = 0;
    acct_pass[strcspn(acct_pass, "\r\n")] = 0;
    acct_loaded = acct_user[0] != 0;
  }
  fclose(f);
}

/* ---- pty / ssh -------------------------------------------------------- */
static int pty_fd = -1;
static pid_t child = -1;

static void start_pty_session(const char *target, const char *known_hosts) {
  struct winsize ws = {ROWS, COLS, 0, 0};
  child = forkpty(&pty_fd, NULL, NULL, &ws);
  if (child == 0) {
    setenv("TERM", "xterm", 1);
    execlp("ssh", "ssh", "-tt", "-o", "StrictHostKeyChecking=accept-new", "-o",
           known_hosts, "-o", "ServerAliveInterval=30", "-o", "LogLevel=ERROR",
           target, (char *)NULL);
    _exit(127);
  }
  if (pty_fd >= 0) fcntl(pty_fd, F_SETFL, fcntl(pty_fd, F_GETFL) | O_NONBLOCK);
}

static int session_via_socket; /* 1: pty_fd is a socket to the session holder */

static int session_alive(void) {
  if (session_via_socket) return pty_fd >= 0;
  if (child <= 0) return 0;
  int st;
  pid_t r = waitpid(child, &st, WNOHANG);
  if (r == child || r < 0) {
    child = -1;
    if (pty_fd >= 0) close(pty_fd);
    pty_fd = -1;
    return 0;
  }
  return 1;
}

/* ---- persistent session: detach / attach -------------------------------- */
/* The first ptterm forks a background "holder" that owns the ssh pty and
 * keeps its own copy of the screen model. The UI is a client on a Unix
 * socket; leaving the app only closes the socket, and the next launch
 * reattaches and gets the screen repainted from the holder's grid. */
static int persist = 1;
static int detach_secs = 30 * 60; /* holder exits after this long unattended */
static char sock_path[160];
static char *g_argv0;
static uint16_t *b5_rev; /* Unicode -> Big5-UAO code (for big5 hosts) */

typedef struct {
  uint8_t *p;
  size_t n, cap;
} Buf;

static void buf_put(Buf *b, const void *d, size_t n) {
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 256;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}

static void buf_fmt(Buf *b, const char *fmt, ...) {
  char tmp[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) buf_put(b, tmp, (size_t)n);
}

static void build_b5_rev(void) {
  if (b5_rev) return;
  b5_rev = calloc(65536, sizeof(uint16_t));
  for (int l = 0; l < 126; l++)
    for (int c = 0; c < 157; c++) {
      uint16_t u = b5_table[l * 157 + c];
      if (u && !b5_rev[u]) {
        int trail = c < 63 ? 0x40 + c : 0xA1 + (c - 63);
        b5_rev[u] = (uint16_t)(((0x81 + l) << 8) | trail);
      }
    }
}

static void snap_pen(Buf *b, uint8_t fg, uint8_t bg, uint8_t flags) {
  buf_fmt(b, "\x1b[0%s%s%s%s;%d;%dm", (flags & F_BOLD) ? ";1" : "",
          (flags & F_BLINK) ? ";5" : "", (flags & F_REV) ? ";7" : "",
          (flags & F_UL) ? ";4" : "", fg < 8 ? 30 + fg : 90 + (fg - 8),
          bg < 8 ? 40 + bg : 100 + (bg - 8));
}

/* An escape/text stream that, fed to a blank terminal, reproduces `grid`. */
static void build_snapshot(Buf *b) {
  if (!utf8_first) build_b5_rev();
  buf_put(b, "\x1b[0m\x1b[2J", 8);
  int lfg = -1, lbg = -1, lfl = -1;
  for (int r = 0; r < ROWS; r++) {
    buf_fmt(b, "\x1b[%d;1H", r + 1);
    for (int c = 0; c < COLS; c++) {
      Cell *x = &grid[r][c];
      if (x->kind == 2) continue; /* trail half: the lead emits the char */
      if (x->fg != lfg || x->bg != lbg || x->flags != lfl) {
        snap_pen(b, x->fg, x->bg, x->flags);
        lfg = x->fg;
        lbg = x->bg;
        lfl = x->flags;
      }
      uint16_t cp = x->cp;
      if (cp == 0) {
        buf_put(b, " ", 1);
      } else if (cp < 0x80 && x->kind == 0) {
        uint8_t ch = (uint8_t)cp;
        buf_put(b, &ch, 1);
      } else if (utf8_first) {
        uint8_t u[3];
        size_t n;
        if (cp < 0x80) { u[0] = (uint8_t)cp; n = 1; }
        else if (cp < 0x800) { u[0] = (uint8_t)(0xC0 | cp >> 6); u[1] = (uint8_t)(0x80 | (cp & 0x3F)); n = 2; }
        else { u[0] = (uint8_t)(0xE0 | cp >> 12); u[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (uint8_t)(0x80 | (cp & 0x3F)); n = 3; }
        buf_put(b, u, n);
      } else {
        uint16_t code = b5_rev[cp];
        uint8_t two[2] = {(uint8_t)(code ? code >> 8 : '?'), (uint8_t)(code ? code & 0xFF : '?')};
        buf_put(b, two, 2);
      }
    }
  }
  buf_fmt(b, "\x1b[%d;%dH", cur_r + 1, cur_c + 1);
  snap_pen(b, cur_fg, cur_bg, cur_flags);
}

static void write_all(int fd, const uint8_t *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w > 0) {
      p += w;
      n -= (size_t)w;
    } else if (w < 0 && errno == EAGAIN) {
      struct pollfd pf = {fd, POLLOUT, 0};
      if (poll(&pf, 1, 1000) <= 0) return;
    } else {
      return;
    }
  }
}

static double mono_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* Background process: owns ssh, mirrors the screen, serves one client. */
static void holder_run(int lfd, const char *target, const char *known_hosts) {
  for (int fd = 3; fd < 256; fd++)
    if (fd != lfd) close(fd); /* drop inherited DRM/SDL descriptors */
  int nul = open("/dev/null", O_RDWR);
  if (nul >= 0) {
    dup2(nul, 0);
    dup2(nul, 1);
    dup2(nul, 2);
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_DFL);
  /* Readable in `ps`. (BusyBox killall also matches /proc/PID/exe, so the
   * rename does not hide us from it; the launcher's exit combo signals the
   * UI via /tmp/ptterm-ui.pid instead of killing by name.) */
  prctl(PR_SET_NAME, "ptsess", 0, 0, 0);
  if (g_argv0 && strlen(g_argv0) >= 6) {
    size_t n = strlen(g_argv0);
    memset(g_argv0, 0, n);
    memcpy(g_argv0, "ptsess", 6);
  }

  clear_rows(0, ROWS - 1);
  cur_r = cur_c = 0;
  cur_fg = 7;
  cur_bg = 0;
  cur_flags = 0;
  pstate = S_NORM;
  mb_len = 0;
  start_pty_session(target, known_hosts);
  if (pty_fd < 0) _exit(1);

  int client = -1, attach_pending = 0;
  double idle_since = mono_s();
  for (;;) {
    struct pollfd p[3];
    int np = 0;
    p[np++] = (struct pollfd){pty_fd, POLLIN, 0};
    p[np++] = (struct pollfd){lfd, POLLIN, 0};
    if (client >= 0) p[np++] = (struct pollfd){client, POLLIN, 0};
    poll(p, (nfds_t)np, 5000);

    uint8_t buf[4096];
    int pty_dead = 0;
    if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      ssize_t n;
      while ((n = read(pty_fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) feed_byte(buf[i]);
        if (client >= 0 && !attach_pending) write_all(client, buf, (size_t)n);
      }
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) pty_dead = 1;
    }
    if (p[1].revents & POLLIN) {
      int c = accept(lfd, NULL, NULL);
      if (c >= 0) {
        if (client >= 0) close(client); /* newest client wins */
        client = c;
        set_nonblock(client);
        attach_pending = 1;
      }
    }
    if (client >= 0 && np == 3 && (p[2].revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(client, buf, sizeof buf);
      if (n > 0) {
        write_all(pty_fd, buf, (size_t)n);
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        close(client);
        client = -1;
        attach_pending = 0;
        idle_since = mono_s();
      }
    }
    /* Repaint once the parser is between sequences, so the client's
     * fresh parser does not start in the middle of an escape. */
    if (client >= 0 && attach_pending && pstate == S_NORM && mb_len == 0) {
      Buf b = {0};
      build_snapshot(&b);
      write_all(client, b.p, b.n);
      free(b.p);
      attach_pending = 0;
    }
    int st;
    if (pty_dead || waitpid(child, &st, WNOHANG) == child) break;
    if (client < 0 && mono_s() - idle_since > detach_secs) {
      kill(child, SIGHUP);
      break;
    }
  }
  if (client >= 0) close(client);
  unlink(sock_path);
  _exit(0);
}

static void make_sock_path(const char *target) {
  char clean[96];
  size_t i = 0;
  for (; target[i] && i < sizeof clean - 1; i++) {
    char c = target[i];
    clean[i] = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? c : '_';
  }
  clean[i] = 0;
  snprintf(sock_path, sizeof sock_path, "/tmp/ptterm-%s.sock", clean);
}

static int unix_addr(struct sockaddr_un *a) {
  memset(a, 0, sizeof *a);
  a->sun_family = AF_UNIX;
  if (strlen(sock_path) >= sizeof a->sun_path) return -1;
  strcpy(a->sun_path, sock_path);
  return 0;
}

static int unix_connect(void) {
  struct sockaddr_un a;
  if (unix_addr(&a) != 0) return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int spawn_holder(const char *target, const char *known_hosts) {
  struct sockaddr_un a;
  if (unix_addr(&a) != 0) return -1;
  unlink(sock_path); /* stale socket from a dead holder */
  int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (lfd < 0) return -1;
  if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(lfd, 2) != 0) {
    close(lfd);
    return -1;
  }
  chmod(sock_path, 0600);
  pid_t p = fork();
  if (p < 0) {
    close(lfd);
    return -1;
  }
  if (p == 0) {
    setsid();
    if (fork() != 0) _exit(0); /* double fork: reparent the holder to init */
    holder_run(lfd, target, known_hosts); /* never returns */
  }
  close(lfd);
  waitpid(p, NULL, 0);
  return 0;
}

static void start_session(const char *target, const char *known_hosts) {
  session_via_socket = 0;
  if (persist) {
    int fd = unix_connect();
    if (fd < 0 && spawn_holder(target, known_hosts) == 0) {
      for (int i = 0; i < 50 && fd < 0; i++) { /* holder listens already */
        fd = unix_connect();
        if (fd < 0) usleep(20000);
      }
    }
    if (fd >= 0) {
      set_nonblock(fd);
      pty_fd = fd;
      child = -1;
      session_via_socket = 1;
      return;
    }
    fprintf(stderr, "ptterm: persistent session unavailable, running directly\n");
  }
  start_pty_session(target, known_hosts);
}

static void send_str(const char *s) {
  if (pty_fd >= 0) {
    ssize_t n = write(pty_fd, s, strlen(s));
    (void)n;
  }
}

/* Press the selected on-screen key. Returns 1 if the keyboard should close. */
static int osk_press(void) {
  if (osk_r < 4) {
    char ch = OSK_ROWS[osk_r][osk_c];
    if (osk_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    char one[2] = {ch, 0};
    send_str(one);
    return 0;
  }
  switch (osk_c) {
  case 0: osk_shift = !osk_shift; break;
  case 1: send_str(" "); break;
  case 2: send_str("\x08"); break;
  case 3: send_str("\r"); break;
  default: return 1;
  }
  return 0;
}

static int screen_contains(const char *needle) {
  size_t nl = strlen(needle);
  for (int r = 0; r < ROWS; r++) {
    char line[COLS + 1];
    for (int c = 0; c < COLS; c++) {
      uint16_t cp = grid[r][c].cp;
      line[c] = (cp >= 0x20 && cp < 0x7F) ? (char)cp : ' ';
    }
    line[COLS] = 0;
    for (int c = 0; c + nl <= COLS; c++)
      if (strncmp(line + c, needle, nl) == 0) return 1;
  }
  return 0;
}

/* Types the saved ID now and the password shortly after (PTT must have shown
 * its password prompt first). Only acts on the login screen. */
static int account_login(uint64_t now) {
  if (!acct_loaded || macro_pending || !screen_contains("guest")) return 0;
  char line[96];
  snprintf(line, sizeof line, "%s\r", acct_user);
  send_str(line);
  snprintf(macro_buf, sizeof macro_buf, "%s\r", acct_pass);
  macro_at = now + 800;
  macro_pending = 1;
  return 1;
}

/* ---- input ------------------------------------------------------------ */
typedef struct {
  const char *seq;
  uint64_t next;
  int active;
} Repeat;
static Repeat rep;

static void key_send(const char *seq, int repeating) {
  send_str(seq);
  if (repeating) {
    rep.seq = seq;
    rep.active = 1;
    rep.next = SDL_GetTicks() + 350;
  }
}

static const char *button_seq(SDL_GamepadButton b, int *repeating) {
  *repeating = 0;
  switch (b) {
  case SDL_GAMEPAD_BUTTON_DPAD_UP: *repeating = 1; return "\x1b[A";
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN: *repeating = 1; return "\x1b[B";
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "\x1b[C";
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "\x1b[D";
  case SDL_GAMEPAD_BUTTON_SOUTH: return "\r";
  case SDL_GAMEPAD_BUTTON_EAST: return "\x1b[D"; /* back */
  case SDL_GAMEPAD_BUTTON_WEST: return " ";
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: *repeating = 1; return "\x1b[5~";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: *repeating = 1; return "\x1b[6~";
  default: return NULL;
  }
}

/* PTTERM_PROF=1: accumulate wall time per loop phase, print on exit. */
static double prof_ms[4];
static const char *const PROF_NAMES[4] = {"poll(pty)", "SDL_PollEvent", "render+present", "other"};
static int prof_on;
static double mono_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define PROF_BEGIN(v) double v = prof_on ? mono_ms() : 0
#define PROF_END(v, i) do { if (prof_on) prof_ms[i] += mono_ms() - v; } while (0)

static volatile sig_atomic_t quit_flag;
static void on_signal(int sig) {
  (void)sig;
  quit_flag = 1;
}

int main(int argc, char **argv) {
  /* Handle our own termination (killall from the exit combo) so ssh is
   * reaped and the terminal state is torn down cleanly. */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

  g_argv0 = argv[0];
  {
    /* UI pid for the launcher's exit combo (the session holder never writes it). */
    FILE *pf = fopen("/tmp/ptterm-ui.pid", "w");
    if (pf) {
      fprintf(pf, "%d\n", (int)getpid());
      fclose(pf);
    }
  }
  const char *target = argc > 1 ? argv[1] : "bbsu@ptt.cc";
  {
    const char *pe = getenv("PTTERM_PERSIST");
    if (pe && strcmp(pe, "0") == 0) persist = 0;
    const char *dm = getenv("PTTERM_DETACH_MINUTES");
    if (dm && atoi(dm) > 0) detach_secs = atoi(dm) * 60;
    make_sock_path(target);
  }
  /* bbsu@ serves UTF-8 text (with raw-Big5 art); plain bbs@ is pure Big5.
   * PTTERM_ENCODING=utf8|big5 overrides. */
  const char *enc = getenv("PTTERM_ENCODING");
  utf8_first = enc ? strcmp(enc, "big5") != 0 : strstr(target, "bbsu") != NULL;

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "ptterm: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  const char *base = SDL_GetBasePath();
  if (!base) base = "./";
  if (load_assets(base) != 0) return 1;
  init_tables();
  {
    const char *pal = getenv("PTTERM_PALETTE");
    if (pal && strcmp(pal, "vga") == 0) pal_fg = PAL_BG;
  }

  char kh[1100];
  snprintf(kh, sizeof kh, "UserKnownHostsFile=%sknown_hosts", base);

  SDL_Window *win = SDL_CreateWindow("ptterm", SCR_W, SCR_H, SDL_WINDOW_FULLSCREEN);
  SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
  if (!ren) {
    fprintf(stderr, "ptterm: window/renderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderLogicalPresentation(ren, SCR_W, SCR_H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, SCR_W, SCR_H);
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  SDL_HideCursor();
  SDL_StartTextInput(win);

  clear_rows(0, ROWS - 1);
  for (int i = 0; i < SCR_W * SCR_H; i++) fb[i] = 0xFF000000;
  load_account();
  prof_on = getenv("PTTERM_PROF") != NULL;
  const double prof_t0 = mono_ms();
  start_session(target, kh);
  const uint64_t t_start = SDL_GetTicks();
  int autologin_done = !getenv("PTTERM_AUTOLOGIN");

  uint64_t last_blink = 0, last_draw = 0;
  int blink_on = 1, running = 1, select_held = 0, start_held = 0;
  int blink_dirty = 0, prev_alive = -1;

  while (running && !quit_flag) {
    /* pty -> screen */
    PROF_BEGIN(t_poll);
    if (pty_fd >= 0) {
      struct pollfd p = {pty_fd, POLLIN, 0};
      if (poll(&p, 1, 25) > 0) {
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(pty_fd, buf, sizeof buf)) > 0)
          for (ssize_t i = 0; i < n; i++) feed_byte(buf[i]);
        if (n == 0 && session_via_socket) { /* holder closed: ssh ended */
          close(pty_fd);
          pty_fd = -1;
        }
      }
    } else {
      SDL_Delay(25);
    }
    PROF_END(t_poll, 0);
    int alive = session_alive();

    PROF_BEGIN(t_ev);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
      case SDL_EVENT_QUIT: running = 0; break;
      case SDL_EVENT_GAMEPAD_ADDED: SDL_OpenGamepad(e.gdevice.which); break;
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        SDL_GamepadButton b = (SDL_GamepadButton)e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 1;
        if (b == SDL_GAMEPAD_BUTTON_START) start_held = 1;
        if (select_held && start_held) {
          running = 0;
          break;
        }
        if (b == SDL_GAMEPAD_BUTTON_BACK && alive) {
          osk_open = !osk_open;
          dirty = 1;
          break;
        }
        if (osk_open && alive) {
          dirty = 1;
          switch (b) {
          case SDL_GAMEPAD_BUTTON_DPAD_UP: osk_move(-1, 0); break;
          case SDL_GAMEPAD_BUTTON_DPAD_DOWN: osk_move(1, 0); break;
          case SDL_GAMEPAD_BUTTON_DPAD_LEFT: osk_move(0, -1); break;
          case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: osk_move(0, 1); break;
          case SDL_GAMEPAD_BUTTON_SOUTH:
            if (osk_press()) osk_open = 0;
            break;
          case SDL_GAMEPAD_BUTTON_EAST: send_str("\x08"); break; /* backspace */
          case SDL_GAMEPAD_BUTTON_WEST: send_str(" "); break;
          case SDL_GAMEPAD_BUTTON_NORTH: send_str("\r"); break;
          case SDL_GAMEPAD_BUTTON_START: osk_open = 0; break;
          default: break;
          }
          break;
        }
        if (!alive) {
          if (b == SDL_GAMEPAD_BUTTON_SOUTH) {
            clear_rows(0, ROWS - 1);
            cur_r = cur_c = 0;
            start_session(target, kh);
          }
          break;
        }
        if (b == SDL_GAMEPAD_BUTTON_NORTH) { /* Y: saved login, else guest */
          if (!account_login(SDL_GetTicks()) && !acct_loaded && screen_contains("guest"))
            send_str("guest\r");
          break;
        }
        int repeating;
        const char *seq = button_seq(b, &repeating);
        if (seq) key_send(seq, repeating);
        break;
      }
      case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        SDL_GamepadButton b = (SDL_GamepadButton)e.gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_BACK) select_held = 0;
        if (b == SDL_GAMEPAD_BUTTON_START) start_held = 0;
        int repeating;
        if (button_seq(b, &repeating) && repeating) rep.active = 0;
        break;
      }
      case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        /* L2/R2 triggers: jump to top / bottom of a list */
        static int trig_l, trig_r;
        int hit = e.gaxis.value > 16000;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
          if (hit && !trig_l && alive) send_str("\x1b[1~");
          trig_l = hit;
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          if (hit && !trig_r && alive) send_str("\x1b[4~");
          trig_r = hit;
        }
        break;
      }
      case SDL_EVENT_KEY_DOWN: {
        const char *s = NULL;
        switch (e.key.key) {
        case SDLK_UP: s = "\x1b[A"; break;
        case SDLK_DOWN: s = "\x1b[B"; break;
        case SDLK_RIGHT: s = "\x1b[C"; break;
        case SDLK_LEFT: s = "\x1b[D"; break;
        case SDLK_RETURN: s = "\r"; break;
        case SDLK_BACKSPACE: s = "\x08"; break;
        case SDLK_PAGEUP: s = "\x1b[5~"; break;
        case SDLK_PAGEDOWN: s = "\x1b[6~"; break;
        case SDLK_HOME: s = "\x1b[1~"; break;
        case SDLK_END: s = "\x1b[4~"; break;
        case SDLK_ESCAPE: s = "\x1b"; break;
        default: break;
        }
        if (s && alive) send_str(s);
        break;
      }
      case SDL_EVENT_TEXT_INPUT:
        if (alive)
          for (const char *p = e.text.text; *p; p++)
            if ((uint8_t)*p < 0x80) {
              char one[2] = {*p, 0};
              send_str(one);
            }
        break;
      default: break;
      }
    }

    PROF_END(t_ev, 1);
    uint64_t now = SDL_GetTicks();
    if (macro_pending && now >= macro_at && alive) {
      send_str(macro_buf);
      memset(macro_buf, 0, sizeof macro_buf);
      macro_pending = 0;
    }
    if (!autologin_done && alive && now - t_start > 2000 && account_login(now))
      autologin_done = 1;
    if (rep.active && now >= rep.next && alive) {
      send_str(rep.seq);
      rep.next = now + 70;
    }
    if (alive != prev_alive) {
      prev_alive = alive;
      dirty = 1;
    }
    if (now - last_blink >= 500) {
      last_blink = now;
      blink_on = !blink_on;
      blink_dirty = 1;
    }
    if ((dirty || blink_dirty) && now - last_draw >= 33) {
      PROF_BEGIN(t_r);
      last_draw = now;
      int full = dirty;
      dirty = 0;
      blink_dirty = 0;
      render(blink_on, alive, full);
      if (full) {
        draw_hint(alive ? (acct_loaded ? "A:Enter B:Back X:Space Y:Login L1/R1:Pg L2/R2:Top/End SEL:Keys SEL+START:Leave"
                                       : "A:Enter B:Back X:Space Y:Guest L1/R1:Pg L2/R2:Top/End SEL:Keys SEL+START:Leave")
                        : "Disconnected - A:Reconnect  SEL+START:Quit",
                  alive);
        dirty_y0 = 0;
        dirty_y1 = SCR_H;
      }
      if (osk_open && alive) {
        osk_draw();
        if (osk_y0() < dirty_y0) dirty_y0 = osk_y0();
        if (osk_y0() + 5 * CH > dirty_y1) dirty_y1 = osk_y0() + 5 * CH;
      }
      if (dirty_y1 > dirty_y0) {
        SDL_Rect rr = {0, dirty_y0, SCR_W, dirty_y1 - dirty_y0};
        SDL_UpdateTexture(tex, &rr, fb + (size_t)dirty_y0 * SCR_W, SCR_W * 4);
      }
      SDL_RenderClear(ren);
      SDL_RenderTexture(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
      PROF_END(t_r, 2);
    }
  }

  if (prof_on) {
    double total = mono_ms() - prof_t0;
    fprintf(stderr, "ptterm profile over %.1f s:\n", total / 1000);
    for (int i = 0; i < 3; i++)
      fprintf(stderr, "  %-16s %8.1f ms  (%.1f%%)\n", PROF_NAMES[i], prof_ms[i], 100 * prof_ms[i] / total);
  }
  const char *dump = getenv("PTTERM_DUMP");
  if (dump) {
    FILE *f = fopen(dump, "w");
    if (f) {
      for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
          Cell *x = &grid[r][c];
          if (x->kind == 2) continue;
          uint16_t cp = x->cp ? x->cp : ' ';
          if (cp < 0x80) fputc(cp, f);
          else if (cp < 0x800) { fputc(0xC0 | cp >> 6, f); fputc(0x80 | (cp & 0x3F), f); }
          else { fputc(0xE0 | cp >> 12, f); fputc(0x80 | ((cp >> 6) & 0x3F), f); fputc(0x80 | (cp & 0x3F), f); }
        }
        fputc('\n', f);
      }
      fclose(f);
    }
  }
  if (child > 0) { /* direct (non-persistent) session: end ssh with us */
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
  }
  if (pty_fd >= 0) close(pty_fd); /* persistent session: just detach */
  unlink("/tmp/ptterm-ui.pid");
  SDL_Quit();
  return 0;
}
