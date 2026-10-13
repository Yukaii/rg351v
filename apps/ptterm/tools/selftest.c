/* Host-side parser test: feeds raw BBS bytes from stdin through ptterm's
 * escape/Big5 parser and prints the resulting 80x24 grid as UTF-8.
 * Build: see tools/selftest.sh. */
#define main ptterm_main
#include "../src/main.c"
#undef main

static void put_utf8(uint16_t cp) {
  if (cp < 0x80) putchar(cp);
  else if (cp < 0x800) { putchar(0xC0 | cp >> 6); putchar(0x80 | (cp & 0x3F)); }
  else { putchar(0xE0 | cp >> 12); putchar(0x80 | ((cp >> 6) & 0x3F)); putchar(0x80 | (cp & 0x3F)); }
}

int main(int argc, char **argv) {
  if (argc < 2 || load_assets(argv[1]) != 0) return 2;
  clear_rows(0, ROWS - 1);
  int ch;
  while ((ch = getchar()) != EOF) feed_byte((uint8_t)ch);
  int wide = 0, missing = 0;
  for (int r = 0; r < ROWS; r++) {
    int last = COLS - 1;
    while (last >= 0 && (grid[r][last].cp == 0 && grid[r][last].kind != 2)) last--;
    for (int c = 0; c <= last; c++) {
      Cell *x = &grid[r][c];
      if (x->kind == 2) continue;
      if (x->kind == 1) { wide++; if (!x->cp || !uf_index[x->cp]) missing++; }
      put_utf8(x->cp ? x->cp : ' ');
    }
    putchar('\n');
  }
  fprintf(stderr, "wide cells: %d, wide cells without glyph: %d, cursor at %d,%d\n",
          wide, missing, cur_r + 1, cur_c + 1);

  /* Snapshot round trip: what a reattaching client is sent must rebuild the
   * same grid in a fresh parser. */
  static Cell before[ROWS][COLS];
  memcpy(before, grid, sizeof grid);
  int r0 = cur_r, c0 = cur_c;
  Buf snap = {0};
  build_snapshot(&snap);
  clear_rows(0, ROWS - 1);
  cur_r = cur_c = 0;
  cur_fg = 7; cur_bg = 0; cur_flags = 0;
  pstate = S_NORM; mb_len = 0;
  for (size_t i = 0; i < snap.n; i++) feed_byte(snap.p[i]);
  int diffs = 0;
  for (int r = 0; r < ROWS; r++)
    for (int c = 0; c < COLS; c++) {
      Cell *a = &before[r][c], *b = &grid[r][c];
      /* a blank is cp 0 when erased and 0x20 when a space was typed; they
       * draw the same, and a blank's foreground is invisible */
      uint16_t ca = a->cp == 0x20 ? 0 : a->cp, cb = b->cp == 0x20 ? 0 : b->cp;
      int blank = ca == 0 && cb == 0;
      if (ca != cb || a->kind != b->kind || a->bg != b->bg ||
          (!blank && (a->fg != b->fg || a->flags != b->flags))) {
        if (diffs < 6)
          fprintf(stderr, "  diff r%d c%d: cp %04x/%04x kind %d/%d fg %d/%d bg %d/%d fl %d/%d\n", r, c,
                  a->cp, b->cp, a->kind, b->kind, a->fg, b->fg, a->bg, b->bg, a->flags, b->flags);
        diffs++;
      }
    }
  fprintf(stderr, "snapshot: %zu bytes, %d differing cells, cursor %s\n", snap.n, diffs,
          (cur_r == r0 && cur_c == c0) ? "restored" : "WRONG");
  return diffs ? 3 : 0;
}
