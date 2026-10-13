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
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define COLS 80
#define ROWS 24
#define CW 8
#define CH 16
#define SCR_W (COLS * CW) /* 640 */
#define SCR_H 480
#define TERM_H (ROWS * CH) /* 384 */

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
static const uint32_t PAL[16] = {
    0xFF000000, 0xFFAA0000, 0xFF00AA00, 0xFFAA5500, 0xFF3030DD, 0xFFAA00AA,
    0xFF00AAAA, 0xFFAAAAAA, 0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
    0xFF6C6CFF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF};
/* PAL entries are 0xAARRGGBB; index 1 is red, 4 is blue (ANSI order). */

static uint32_t fb[SCR_W * SCR_H];

/* Draws one glyph into a (cell_w x 16) box. Narrow glyphs in wide boxes are
 * doubled horizontally so BBS block art stays connected. */
static void blit_glyph(int x, int y, uint16_t cp, int box_w, uint32_t fg, uint32_t bg, int ul) {
  uint32_t ent = cp ? uf_index[cp] : 0;
  const uint8_t *g = ent ? uf_blob + ((ent & 0x7FFFFFFF) - 1) : NULL;
  int gw = (ent & 0x80000000u) ? 16 : 8;
  for (int row = 0; row < CH; row++) {
    uint32_t *dst = &fb[(y + row) * SCR_W + x];
    for (int px = 0; px < box_w; px++) {
      int on = 0;
      if (g) {
        int sx = (box_w == 16 && gw == 8) ? px / 2 : px;
        if (sx < gw) {
          uint8_t byte = g[row * (gw / 8) + sx / 8];
          on = (byte >> (7 - (sx % 8))) & 1;
        }
      } else if (cp > 0x7F && box_w == 16) {
        /* missing glyph: hollow box */
        on = (row == 1 || row == 14 || px == 1 || px == 14) && row > 0 && row < 15;
      }
      if (ul && row == CH - 1) on = 1;
      dst[px] = on ? fg : bg;
    }
  }
}

static void render(int blink_on, int show_cursor) {
  for (int r = 0; r < ROWS; r++) {
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
      uint32_t fg = PAL[fgi & 15], bg = PAL[bgi & 15];
      uint16_t cp = cell->cp;
      if ((cell->flags & F_BLINK) && !blink_on) cp = 0;
      if (show_cursor && blink_on && r == cur_r && c == cur_c) {
        uint32_t t = fg;
        fg = bg;
        bg = t == bg ? 0xFFAAAAAA : t;
      }
      blit_glyph(c * CW, r * CH, cp, cell->kind == 1 ? 16 : 8, fg, bg,
                 (cell->flags & F_UL) != 0);
    }
  }
}

static void draw_text(int row, const char *s, uint32_t fg, uint32_t bg) {
  int y = TERM_H + row * CH, x = 0;
  for (; *s && x < SCR_W; s++, x += CW)
    blit_glyph(x, y, (uint8_t)*s, 8, fg, bg, 0);
  for (; x < SCR_W; x += CW) blit_glyph(x, y, 0, 8, fg, bg, 0);
}

/* ---- on-screen keyboard (bottom 96 px) --------------------------------- */
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
  for (; *label; label++, tx += CW) blit_glyph(tx, y, (uint8_t)*label, 8, fg, bg, 0);
}

static void osk_draw(void) {
  for (int r = 0; r < 5; r++) {
    int y = TERM_H + (1 + r) * CH;
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

/* ---- pty / ssh -------------------------------------------------------- */
static int pty_fd = -1;
static pid_t child = -1;

static void start_session(const char *target, const char *known_hosts) {
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

static int session_alive(void) {
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

int main(int argc, char **argv) {
  const char *target = argc > 1 ? argv[1] : "bbsu@ptt.cc";
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
  start_session(target, kh);

  uint64_t last_blink = 0, last_draw = 0;
  int blink_on = 1, running = 1, select_held = 0, start_held = 0;
  char status[96];

  while (running) {
    /* pty -> screen */
    if (pty_fd >= 0) {
      struct pollfd p = {pty_fd, POLLIN, 0};
      if (poll(&p, 1, 25) > 0) {
        uint8_t buf[4096];
        ssize_t n;
        while ((n = read(pty_fd, buf, sizeof buf)) > 0)
          for (ssize_t i = 0; i < n; i++) feed_byte(buf[i]);
      }
    } else {
      SDL_Delay(25);
    }
    int alive = session_alive();

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
        if (b == SDL_GAMEPAD_BUTTON_NORTH) { /* Y: guest login macro */
          if (screen_contains("guest")) send_str("guest\r");
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

    uint64_t now = SDL_GetTicks();
    if (rep.active && now >= rep.next && alive) {
      send_str(rep.seq);
      rep.next = now + 70;
    }
    if (now - last_blink >= 500) {
      last_blink = now;
      blink_on = !blink_on;
      dirty = 1;
    }
    if (dirty && now - last_draw >= 33) {
      last_draw = now;
      dirty = 0;
      render(blink_on, alive);
      uint32_t hint = 0xFF55FFFF, dim = 0xFFAAAAAA, black = 0xFF000000;
      snprintf(status, sizeof status, "%s %s", alive ? "[connected]" : "[disconnected - A: reconnect]", target);
      draw_text(0, status, alive ? 0xFF55FF55 : 0xFFFF5555, black);
      if (osk_open && alive) {
        osk_draw();
      } else {
        draw_text(1, "D-pad:move  A:Enter  B:Back  X:Space  Y:guest login", hint, black);
        draw_text(2, "L1/R1:PageUp/PageDown  L2/R2:Top/End", dim, black);
        draw_text(3, "SELECT:keyboard (type account/password)  SELECT+START:quit", dim, black);
        for (int r = 4; r < 6; r++) draw_text(r, "", dim, black);
      }
      SDL_UpdateTexture(tex, NULL, fb, SCR_W * 4);
      SDL_RenderClear(ren);
      SDL_RenderTexture(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
    }
  }

  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
  }
  SDL_Quit();
  return 0;
}
