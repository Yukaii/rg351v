/*
 * Host test for the judgement engine using real charts:
 *   game_test <file.tja> <course-id>
 * Build: cc -O2 -Wall -Wextra -I../src -o /tmp/game_test tools/game_test.c src/game.c src/tja.c -lm
 * Exit status is non-zero when an expectation fails.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "game.h"
#include "tja.h"

static int fails;
#define EXPECT(cond, ...)            \
  do {                               \
    if (!(cond)) {                   \
      printf("  FAIL: " __VA_ARGS__); \
      printf("\n");                  \
      fails++;                       \
    }                                \
  } while (0)

/* Play the whole chart. jitter_s: max abs offset of each hit (deterministic
 * pattern); skip_every: skip every Nth note (0 = none). */
static void play(const TjaCourse *c, double jitter_s, int skip_every, Game *g) {
  game_init(g, c);
  size_t idx = 0;
  for (size_t i = 0; i < c->n_notes; i++) {
    const TjaNote *n = &c->notes[i];
    if (n->type == TJA_ROLL || n->type == TJA_BIGROLL || n->type == TJA_BALLOON || n->type == TJA_KUSUDAMA) {
      double step = 1.0 / 16;
      for (double t = n->t + 0.01; t < n->end_t; t += step) {
        game_update(g, t);
        game_hit(g, HIT_DON, t);
      }
      continue;
    }
    if (skip_every && ++idx % (size_t)skip_every == 0) continue;
    double j = jitter_s * (((i * 2654435761u) >> 8) % 2001 / 1000.0 - 1.0);
    double t = n->t + j;
    game_update(g, t);
    game_hit(g, (n->type == TJA_DON || n->type == TJA_BIGDON) ? HIT_DON : HIT_KA, t);
  }
  game_update(g, g->last_t + 2.0);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s file.tja course-id\n", argv[0]);
    return 2;
  }
  Tja t;
  char err[256];
  if (tja_load(&t, argv[1], err, sizeof err)) {
    fprintf(stderr, "%s\n", err);
    return 1;
  }
  const TjaCourse *c = tja_course(&t, atoi(argv[2]));
  if (!c) {
    fprintf(stderr, "no such course\n");
    return 1;
  }
  size_t hit_notes = 0;
  for (size_t i = 0; i < c->n_notes; i++)
    if (c->notes[i].type <= TJA_BIGKA) hit_notes++;
  printf("%s (%s): %zu hit notes\n", t.title, c->name, hit_notes);

  Game g;
  printf("perfect:\n");
  play(c, 0.0, 0, &g);
  printf("  good=%d ok=%d bad=%d miss=%d maxcombo=%d score=%ld rolls=%d balloons=%d/%d\n", g.good, g.ok, g.bad,
         g.miss, g.max_combo, g.score, g.roll_total, g.balloons_popped, g.balloons_total);
  EXPECT((size_t)g.good == hit_notes, "perfect play should be all good (%d/%zu)", g.good, hit_notes);
  EXPECT(g.miss == 0 && g.bad == 0 && g.ok == 0, "no ok/bad/miss expected");
  EXPECT((size_t)g.max_combo == hit_notes, "full combo expected");
  EXPECT(g.balloons_popped == g.balloons_total, "all balloons should pop");
  long perfect_score = g.score;
  game_free(&g);

  printf("jitter +-60ms:\n");
  play(c, 0.060, 0, &g);
  printf("  good=%d ok=%d bad=%d miss=%d maxcombo=%d score=%ld\n", g.good, g.ok, g.bad, g.miss, g.max_combo, g.score);
  /* dense charts: a jittered hit can land on a neighbouring same-colour
   * note, so a few notes legitimately end up bad/miss */
  EXPECT((size_t)(g.good + g.ok) * 100 >= hit_notes * 95, "95%% of hits within 60ms must be good/ok");
  EXPECT((size_t)(g.good + g.ok + g.bad + g.miss) == hit_notes, "every note resolved exactly once");
  EXPECT(g.ok > 0 && g.good > 0, "mix of good and ok expected");
  EXPECT(g.score < perfect_score, "jitter should score less than perfect");
  game_free(&g);

  printf("jitter +-130ms (outside bad window sometimes):\n");
  play(c, 0.130, 0, &g);
  printf("  good=%d ok=%d bad=%d miss=%d maxcombo=%d\n", g.good, g.ok, g.bad, g.miss, g.max_combo);
  EXPECT((size_t)(g.good + g.ok + g.bad + g.miss) == hit_notes, "every note must be resolved exactly once");
  EXPECT(g.miss > 0, "some misses expected");
  game_free(&g);

  printf("skip every 4th:\n");
  play(c, 0.0, 4, &g);
  printf("  good=%d miss=%d maxcombo=%d\n", g.good, g.miss, g.max_combo);
  EXPECT((size_t)(g.good + g.miss) == hit_notes, "good+miss must equal note count");
  /* a skipped note only turns into a miss 108 ms later, so in dense streams
   * a few more hits can still extend the combo before it breaks */
  EXPECT(g.max_combo < 20, "combo must break on skips (max %d)", g.max_combo);
  game_free(&g);

  printf("no input:\n");
  game_init(&g, c);
  game_update(&g, g.last_t + 2.0);
  printf("  miss=%d\n", g.miss);
  EXPECT((size_t)g.miss == hit_notes, "all notes should miss without input");
  EXPECT(game_finished(&g, g.last_t + 2.0), "should be finished");
  game_free(&g);

  tja_free(&t);
  printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
  return fails ? 1 : 0;
}
