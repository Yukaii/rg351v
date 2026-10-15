/*
 * Host tool: parse a .tja and print what the player will see.
 *   tja_dump <file.tja> [--notes <course-id>]
 * Build: cc -O2 -Wall -Wextra -I../src -o /tmp/tja_dump tja_dump.c ../src/tja.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tja.h"

static const char *TN[] = {"?", "don", "ka", "DON", "KA", "roll", "ROLL", "balloon", "end", "kusudama"};

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s file.tja [--notes <course-id>]\n", argv[0]);
    return 2;
  }
  Tja t;
  char err[256];
  if (tja_load(&t, argv[1], err, sizeof err) != 0) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  int notes_id = -1;
  if (argc >= 4 && !strcmp(argv[2], "--notes")) notes_id = atoi(argv[3]);

  printf("title=%s | %s\nwave=%s bpm=%.2f offset=%.3f%s\n", t.title, t.subtitle, t.wave, t.bpm, t.offset,
         t.bad_utf8 ? " [NOT UTF-8]" : "");
  for (size_t ci = 0; ci < t.n_courses; ci++) {
    const TjaCourse *c = &t.courses[ci];
    size_t cnt[10] = {0};
    double bmin = 1e9, bmax = 0, smin = 1e9, smax = -1e9;
    size_t gogo = 0, miss_balloon = 0;
    for (size_t i = 0; i < c->n_notes; i++) {
      const TjaNote *n = &c->notes[i];
      cnt[n->type]++;
      if (n->bpm < bmin) bmin = n->bpm;
      if (n->bpm > bmax) bmax = n->bpm;
      if (n->scroll < smin) smin = n->scroll;
      if (n->scroll > smax) smax = n->scroll;
      if (n->gogo) gogo++;
      if ((n->type == TJA_BALLOON || n->type == TJA_KUSUDAMA) && n->hits == 0) miss_balloon++;
    }
    double first = c->n_notes ? c->notes[0].t : 0, last = 0;
    for (size_t i = 0; i < c->n_notes; i++) {
      double e = c->notes[i].end_t > c->notes[i].t ? c->notes[i].end_t : c->notes[i].t;
      if (e > last) last = e;
    }
    printf("course %-6s id=%d level=%d%s%s: notes=%zu (don %zu ka %zu DON %zu KA %zu roll %zu ROLL %zu "
           "balloon %zu kusu %zu) combo=%zu bars=%zu first=%.3fs last=%.3fs bpm %.1f..%.1f scroll %.2f..%.2f "
           "gogo-notes=%zu balloons=%zu/%zu%s\n",
           c->name, c->id, c->level, c->double_play ? " double(P1)" : "", c->branched ? " branched(N only)" : "",
           c->n_notes, cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[9],
           cnt[1] + cnt[2] + cnt[3] + cnt[4], c->n_bars, first, last, bmin, bmax, smin, smax, gogo,
           c->n_balloons, cnt[7] + cnt[9], miss_balloon ? " [balloon list short]" : "");
    if (c->id == notes_id) {
      for (size_t i = 0; i < c->n_notes; i++) {
        const TjaNote *n = &c->notes[i];
        printf("  %9.3f %-8s", n->t, TN[n->type]);
        if (n->end_t > n->t) printf(" -> %9.3f", n->end_t);
        if (n->hits) printf(" hits=%d", n->hits);
        printf(" bpm=%.2f scroll=%.2f%s\n", n->bpm, n->scroll, n->gogo ? " gogo" : "");
      }
    }
  }
  tja_free(&t);
  return 0;
}
