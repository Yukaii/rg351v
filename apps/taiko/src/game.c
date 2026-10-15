#include "game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int is_roll(int type) { return type == TJA_ROLL || type == TJA_BIGROLL; }
static int is_balloon(int type) { return type == TJA_BALLOON || type == TJA_KUSUDAMA; }
static int is_hit(int type) { return type >= TJA_DON && type <= TJA_BIGKA; }
static int kind_of(int type) { return type == TJA_DON || type == TJA_BIGDON ? HIT_DON : HIT_KA; }

int game_init(Game *g, const TjaCourse *c) {
  memset(g, 0, sizeof *g);
  g->c = c;
  size_t n = c->n_notes ? c->n_notes : 1;
  g->done = calloc(n, 1);
  g->roll_hits = calloc(n, sizeof(int));
  if (!g->done || !g->roll_hits) return -1;
  g->win_good = 0.025025;
  g->win_ok = 0.075075;
  g->win_bad = 0.108442;
  for (size_t i = 0; i < c->n_notes; i++) {
    const TjaNote *nt = &c->notes[i];
    double e = nt->end_t > nt->t ? nt->end_t : nt->t;
    if (e > g->last_t) g->last_t = e;
    if (is_balloon(nt->type)) g->balloons_total++;
    if (is_hit(nt->type)) g->hit_notes++;
  }
  /* clear lines as in the arcade: Easy 60, Normal/Hard 70, Oni/Ura 80 */
  static const double CLEAR[5] = {60, 70, 70, 80, 80};
  g->gauge_clear = CLEAR[c->id >= 0 && c->id <= 4 ? c->id : 3];
  return 0;
}

void game_free(Game *g) {
  free(g->done);
  free(g->roll_hits);
  memset(g, 0, sizeof *g);
}

static void add_score(Game *g, const TjaNote *n, int judge) {
  long init = g->c->score_init > 0 ? g->c->score_init : 1000;
  long diff = g->c->score_diff > 0 ? g->c->score_diff : 100;
  int step = g->combo / 10;
  if (step > 10) step = 10;
  long v = init + diff * step;
  if (judge == J_OK) v /= 2;
  if (n->type == TJA_BIGDON || n->type == TJA_BIGKA) v *= 2;
  if (n->gogo) v = v * 12 / 10;
  g->score += v / 10 * 10;
}

/* good = +1 unit, ok = +0.5, bad/miss = -2 (a unit is 100 / hit notes) */
static void gauge_add(Game *g, double units) {
  if (g->hit_notes <= 0) return;
  g->gauge += units * 100.0 / g->hit_notes;
  if (g->gauge < 0) g->gauge = 0;
  if (g->gauge > 100) g->gauge = 100;
}

static void advance(Game *g) {
  while (g->next < g->c->n_notes && g->done[g->next]) g->next++;
}

void game_hit(Game *g, int kind, double t) {
  const TjaCourse *c = g->c;
  g->hit_kind = kind;
  g->hit_t = t;

  /* an open roll/balloon takes every hit (either colour) */
  for (size_t i = g->next; i < c->n_notes; i++) {
    const TjaNote *n = &c->notes[i];
    if (n->t > t) break;
    if (g->done[i] || !(is_roll(n->type) || is_balloon(n->type))) continue;
    if (t > n->end_t) continue;
    g->roll_hits[i]++;
    if (is_balloon(n->type)) {
      if (n->hits > 0 && g->roll_hits[i] >= n->hits) {
        g->done[i] = 1;
        g->balloons_popped++;
        g->score += 300;
        advance(g);
      }
    } else {
      g->roll_total++;
      g->score += n->type == TJA_BIGROLL ? 200 : 100;
    }
    return;
  }

  /* otherwise the closest unresolved note of the same colour in the window */
  long best = -1;
  double best_d = g->win_bad + 1;
  for (size_t i = g->next; i < c->n_notes; i++) {
    const TjaNote *n = &c->notes[i];
    if (n->t - g->win_bad > t) break;
    if (g->done[i] || !is_hit(n->type) || kind_of(n->type) != kind) continue;
    double d = fabs(n->t - t);
    if (d <= g->win_bad && d < best_d) {
      best = (long)i;
      best_d = d;
    }
  }
  if (best < 0) return;
  const TjaNote *n = &c->notes[best];
  g->done[best] = 1;
  g->fb_t = t;
  if (best_d <= g->win_good || best_d <= g->win_ok) {
    int j = best_d <= g->win_good ? J_GOOD : J_OK;
    g->combo++;
    if (g->combo > g->max_combo) g->max_combo = g->combo;
    add_score(g, n, j);
    if (j == J_GOOD) g->good++;
    else g->ok++;
    gauge_add(g, j == J_GOOD ? 1.0 : 0.5);
    g->fb_judge = j;
  } else {
    g->bad++;
    g->combo = 0;
    gauge_add(g, -2.0);
    g->fb_judge = J_BAD;
  }
  advance(g);
}

void game_update(Game *g, double now) {
  const TjaCourse *c = g->c;
  for (size_t i = g->next; i < c->n_notes; i++) {
    const TjaNote *n = &c->notes[i];
    if (n->t > now) break;
    if (g->done[i]) continue;
    if (is_hit(n->type)) {
      if (n->t + g->win_bad < now) {
        g->done[i] = 2;
        g->miss++;
        g->combo = 0;
        gauge_add(g, -2.0);
        g->fb_judge = J_MISS;
        g->fb_t = now;
      }
    } else if (n->end_t < now) {
      g->done[i] = 2; /* roll finished / balloon not popped: no combo break */
    }
  }
  advance(g);
}

int game_finished(const Game *g, double now) { return now > g->last_t + 1.0; }

int game_cleared(const Game *g) { return g->gauge + 1e-9 >= g->gauge_clear; }
