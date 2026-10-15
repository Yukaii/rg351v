/*
 * Judgement engine for the TJA player. No SDL, no I/O: feed it hit events
 * (kind + song time) and the current song time, read back the score state.
 *
 * Simplifications (v1): a big note is judged by one hit; a wrong-colour hit
 * near a note is ignored; scoring approximates the TJA SCOREINIT/SCOREDIFF
 * "combo step" rules.
 */
#ifndef GAME_H
#define GAME_H

#include <stddef.h>

#include "tja.h"

enum { HIT_DON = 0, HIT_KA = 1 };
enum { J_NONE = 0, J_GOOD, J_OK, J_BAD, J_MISS };

typedef struct {
  const TjaCourse *c;
  unsigned char *done; /* per note: 0 pending, 1 hit, 2 missed/expired */
  int *roll_hits;      /* per note: hits on a roll/balloon */
  size_t next;         /* first note index not yet resolved */
  double win_good, win_ok, win_bad; /* seconds, +/- around the note */
  double last_t;                    /* end of the last note */
  long score;
  double gauge;       /* soul gauge 0..100; all-good play ends at 100 */
  double gauge_clear; /* clear threshold in percent (per course) */
  int hit_notes;      /* number of don/ka notes (gauge unit) */
  int good, ok, bad, miss, combo, max_combo;
  int roll_total, balloons_popped, balloons_total;
  /* last feedback, for the UI (song seconds) */
  int fb_judge;
  double fb_t;
  int hit_kind;
  double hit_t;
} Game;

int game_init(Game *g, const TjaCourse *c);
void game_free(Game *g);
void game_hit(Game *g, int kind, double t);
void game_update(Game *g, double now);
int game_finished(const Game *g, double now);
int game_cleared(const Game *g); /* gauge reached the clear threshold */

#endif
