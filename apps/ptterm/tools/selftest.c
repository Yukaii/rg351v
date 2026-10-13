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
  return 0;
}
