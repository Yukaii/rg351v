#include "tja.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char kind; /* 'n' note char, 'c' command */
  char ch;
  int cmd;
  double val;
} Item;

enum { C_BPM, C_DELAY, C_SCROLL, C_GOGO_ON, C_GOGO_OFF, C_BAR_ON, C_BAR_OFF };

typedef struct {
  Tja *t;
  TjaCourse *c; /* course being filled */
  /* chart state */
  double cur_t, bpm, scroll;
  double meas_num, meas_den;
  double next_num, next_den; /* #MEASURE seen after note chars: next measure */
  int gogo, bar_on;
  size_t balloon_i;
  long open_roll; /* index into c->notes, or -1 */
  Item *items;
  size_t n_items, cap_items;
  int n_chars; /* note chars buffered for this measure */
  int bar_done;
  /* branch state: 0 none, 1 in branch + active (#N), 2 in branch + skipped */
  int branch;
} P;

static void *grow(void *p, size_t *cap, size_t elem, size_t need) {
  if (need <= *cap) return p;
  size_t nc = *cap ? *cap * 2 : 64;
  while (nc < need) nc *= 2;
  void *q = realloc(p, nc * elem);
  if (!q) {
    fprintf(stderr, "tja: out of memory\n");
    exit(1);
  }
  *cap = nc;
  return q;
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  return s;
}

static int utf8_valid(const unsigned char *s, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned char c = s[i];
    size_t k;
    if (c < 0x80) k = 0;
    else if ((c >> 5) == 6) k = 1;
    else if ((c >> 4) == 14) k = 2;
    else if ((c >> 3) == 30) k = 3;
    else return 0;
    if (i + k >= n && k) return 0;
    for (size_t j = 1; j <= k; j++)
      if ((s[i + j] & 0xC0) != 0x80) return 0;
    i += k + 1;
  }
  return 1;
}

static void set_str(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src);
}

static int course_id(const char *v) {
  if (isdigit((unsigned char)v[0])) return atoi(v);
  if (!strncasecmp(v, "easy", 4)) return 0;
  if (!strncasecmp(v, "normal", 6)) return 1;
  if (!strncasecmp(v, "hard", 4)) return 2;
  if (!strncasecmp(v, "oni", 3)) return 3;
  if (!strncasecmp(v, "edit", 4) || !strncasecmp(v, "ura", 3)) return 4;
  return 3;
}

static TjaCourse *new_course(Tja *t, const char *name) {
  t->courses = realloc(t->courses, (t->n_courses + 1) * sizeof *t->courses);
  if (!t->courses) exit(1);
  TjaCourse *c = &t->courses[t->n_courses++];
  memset(c, 0, sizeof *c);
  set_str(c->name, sizeof c->name, name);
  c->id = course_id(name);
  c->level = 0;
  return c;
}

static void parse_int_list(const char *v, int **out, size_t *n) {
  *out = NULL;
  *n = 0;
  while (*v) {
    while (*v == ',' || isspace((unsigned char)*v)) v++;
    if (!*v) break;
    char *e;
    long x = strtol(v, &e, 10);
    if (e == v) break;
    *out = realloc(*out, (*n + 1) * sizeof **out);
    (*out)[(*n)++] = (int)x;
    v = e;
  }
}

static void add_note(P *p, int type, double t) {
  TjaCourse *c = p->c;
  c->notes = realloc(c->notes, (c->n_notes + 1) * sizeof *c->notes);
  if (!c->notes) exit(1);
  TjaNote *n = &c->notes[c->n_notes++];
  n->t = n->end_t = t;
  n->bpm = p->bpm;
  n->scroll = p->scroll;
  n->type = type;
  n->hits = 0;
  n->gogo = p->gogo;
  if (type == TJA_BALLOON || type == TJA_KUSUDAMA) {
    n->hits = p->balloon_i < c->n_balloons ? c->balloons[p->balloon_i] : 0;
    p->balloon_i++;
  }
  if (type == TJA_ROLL || type == TJA_BIGROLL || type == TJA_BALLOON || type == TJA_KUSUDAMA)
    p->open_roll = (long)(c->n_notes - 1);
}

static void add_bar(P *p) {
  TjaCourse *c = p->c;
  c->bars = realloc(c->bars, (c->n_bars + 1) * sizeof *c->bars);
  if (!c->bars) exit(1);
  TjaBar *b = &c->bars[c->n_bars++];
  b->t = p->cur_t;
  b->bpm = p->bpm;
  b->scroll = p->scroll;
  b->visible = p->bar_on;
}

static void flush_measure(P *p) {
  double beats = p->meas_den > 0 ? 4.0 * p->meas_num / p->meas_den : 4.0;
  int n = p->n_chars;
  int bar_pending = 1;
  for (size_t i = 0; i < p->n_items; i++) {
    Item *it = &p->items[i];
    if (it->kind == 'c') {
      switch (it->cmd) {
        case C_BPM: p->bpm = it->val; break;
        case C_DELAY: p->cur_t += it->val; break;
        case C_SCROLL: p->scroll = it->val; break;
        case C_GOGO_ON: p->gogo = 1; break;
        case C_GOGO_OFF: p->gogo = 0; break;
        case C_BAR_ON: p->bar_on = 1; break;
        case C_BAR_OFF: p->bar_on = 0; break;
      }
      continue;
    }
    if (bar_pending) {
      add_bar(p);
      bar_pending = 0;
    }
    int d = it->ch - '0';
    if (d >= 1 && d <= 9) {
      if (d == 8) {
        if (p->open_roll >= 0) {
          p->c->notes[p->open_roll].end_t = p->cur_t;
          p->open_roll = -1;
        }
      } else {
        add_note(p, d, p->cur_t);
      }
    }
    p->cur_t += (beats / n) * 60.0 / p->bpm;
  }
  if (n == 0) {
    add_bar(p);
    p->cur_t += beats * 60.0 / p->bpm;
  }
  p->n_items = 0;
  p->n_chars = 0;
  p->meas_num = p->next_num;
  p->meas_den = p->next_den;
}

static void push_item(P *p, Item it) {
  p->items = grow(p->items, &p->cap_items, sizeof *p->items, p->n_items + 1);
  p->items[p->n_items++] = it;
  if (it.kind == 'n') p->n_chars++;
}

static void cmd_item(P *p, int cmd, double v) {
  Item it = {'c', 0, cmd, v};
  push_item(p, it);
}

static void start_chart(P *p) {
  p->cur_t = -p->t->offset;
  p->bpm = p->t->bpm > 0 ? p->t->bpm : 120;
  p->scroll = 1.0;
  p->meas_num = p->meas_den = p->next_num = p->next_den = 4;
  p->gogo = 0;
  p->bar_on = 1;
  p->balloon_i = 0;
  p->open_roll = -1;
  p->n_items = 0;
  p->n_chars = 0;
  p->branch = 0;
}

int tja_parse(Tja *t, const char *text, size_t len, char *err, size_t errsz) {
  memset(t, 0, sizeof *t);
  t->bpm = 120;
  (void)err;
  (void)errsz;
  const unsigned char *u = (const unsigned char *)text;
  if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
    text += 3;
    len -= 3;
  }
  t->bad_utf8 = !utf8_valid((const unsigned char *)text, len);

  P p;
  memset(&p, 0, sizeof p);
  p.t = t;
  p.open_roll = -1;
  TjaCourse *pending = NULL; /* course named by COURSE: awaiting #START */
  int in_chart = 0, skip_chart = 0;
  /* STYLE:Double after a course's #END: the following #START P1/P2 charts
   * belong to the course already parsed from its Single chart; skip them. */
  int double_after = 0;

  char *buf = malloc(len + 1);
  if (!buf) exit(1);
  memcpy(buf, text, len);
  buf[len] = 0;

  for (char *line = buf, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *cm = strstr(line, "//");
    if (cm) *cm = 0;
    char *l = trim(line);
    if (!*l) continue;

    if (l[0] == '#') {
      char *sp = l;
      while (*sp && !isspace((unsigned char)*sp)) sp++;
      char *arg = *sp ? trim(sp + 1) : sp;
      *sp = 0;
      char *k = l;
      for (char *q = k; *q; q++) *q = (char)toupper((unsigned char)*q);

      if (!strcmp(k, "#START")) {
        in_chart = 1;
        skip_chart = !strcasecmp(arg, "P2") || (!pending && double_after);
        if (skip_chart) continue; /* double play: P1 only */
        if (!pending) pending = new_course(t, "Oni");
        p.c = pending;
        start_chart(&p);
        continue;
      }
      if (!in_chart) continue;
      if (!strcmp(k, "#END")) {
        if (!skip_chart) flush_measure(&p);
        in_chart = 0;
        skip_chart = 0;
        pending = NULL;
        continue;
      }
      if (skip_chart) continue;
      if (!strcmp(k, "#BRANCHSTART")) {
        p.branch = 2; /* until #N */
        p.c->branched = 1;
      } else if (!strcmp(k, "#N")) {
        if (p.branch) p.branch = 1;
      } else if (!strcmp(k, "#E") || !strcmp(k, "#M")) {
        if (p.branch) p.branch = 2;
      } else if (!strcmp(k, "#BRANCHEND")) {
        p.branch = 0;
      } else if (p.branch == 2) {
        /* skipped path */
      } else if (!strcmp(k, "#BPMCHANGE")) {
        double v = atof(arg);
        if (v > 0) cmd_item(&p, C_BPM, v);
      } else if (!strcmp(k, "#DELAY")) {
        cmd_item(&p, C_DELAY, atof(arg));
      } else if (!strcmp(k, "#SCROLL")) {
        cmd_item(&p, C_SCROLL, atof(arg));
      } else if (!strcmp(k, "#GOGOSTART")) {
        cmd_item(&p, C_GOGO_ON, 0);
      } else if (!strcmp(k, "#GOGOEND")) {
        cmd_item(&p, C_GOGO_OFF, 0);
      } else if (!strcmp(k, "#BARLINEON")) {
        cmd_item(&p, C_BAR_ON, 0);
      } else if (!strcmp(k, "#BARLINEOFF")) {
        cmd_item(&p, C_BAR_OFF, 0);
      } else if (!strcmp(k, "#MEASURE")) {
        double a, b;
        if (sscanf(arg, "%lf/%lf", &a, &b) == 2 && a > 0 && b > 0) {
          /* before the first note char it applies to this measure; after
           * that (rare) from the next one */
          if (p.n_chars == 0) {
            p.meas_num = a;
            p.meas_den = b;
          }
          p.next_num = a;
          p.next_den = b;
        }
      }
      continue;
    }

    if (!in_chart) {
      char *colon = strchr(l, ':');
      if (!colon) continue;
      *colon = 0;
      char *key = trim(l), *val = trim(colon + 1);
      for (char *q = key; *q; q++) *q = (char)toupper((unsigned char)*q);
      if (!strcmp(key, "COURSE")) {
        pending = new_course(t, val);
        double_after = 0;
      } else if (pending) {
        if (!strcmp(key, "LEVEL")) pending->level = atoi(val);
        else if (!strcmp(key, "BALLOON")) parse_int_list(val, &pending->balloons, &pending->n_balloons);
        else if (!strcmp(key, "SCOREINIT")) pending->score_init = atoi(val);
        else if (!strcmp(key, "SCOREDIFF")) pending->score_diff = atoi(val);
        else if (!strcmp(key, "STYLE")) pending->double_play = !strcasecmp(val, "double");
      } else if (!strcmp(key, "STYLE") && !strcasecmp(val, "double")) {
        double_after = 1;
      }
      if (!strcmp(key, "TITLE")) set_str(t->title, sizeof t->title, val);
      else if (!strcmp(key, "SUBTITLE")) set_str(t->subtitle, sizeof t->subtitle, val);
      else if (!strcmp(key, "WAVE")) set_str(t->wave, sizeof t->wave, val);
      else if (!strcmp(key, "GENRE")) set_str(t->genre, sizeof t->genre, val);
      else if (!strcmp(key, "BPM")) t->bpm = atof(val);
      else if (!strcmp(key, "OFFSET")) t->offset = atof(val);
      else if (!strcmp(key, "DEMOSTART")) t->demostart = atof(val);
      else if (!strcmp(key, "SONGVOL")) t->songvol = atoi(val);
      else if (!strcmp(key, "SEVOL")) t->sevol = atoi(val);
      continue;
    }

    if (skip_chart || p.branch == 2) continue;
    /* note line: digits, optionally ending a measure with a comma */
    for (char *q = l; *q; q++) {
      if (*q == ',') {
        flush_measure(&p);
      } else if (isdigit((unsigned char)*q)) {
        Item it = {'n', *q, 0, 0};
        push_item(&p, it);
      }
    }
  }
  free(buf);
  free(p.items);
  return 0;
}

int tja_load(Tja *t, const char *path, char *err, size_t errsz) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(err, errsz, "%s: %s", path, strerror(errno));
    return -1;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(b);
    snprintf(err, errsz, "%s: read failed", path);
    return -1;
  }
  fclose(f);
  int r = tja_parse(t, b, (size_t)n, err, errsz);
  free(b);
  return r;
}

void tja_free(Tja *t) {
  for (size_t i = 0; i < t->n_courses; i++) {
    free(t->courses[i].balloons);
    free(t->courses[i].notes);
    free(t->courses[i].bars);
  }
  free(t->courses);
  memset(t, 0, sizeof *t);
}

const TjaCourse *tja_course(const Tja *t, int id) {
  for (size_t i = 0; i < t->n_courses; i++)
    if (t->courses[i].id == id) return &t->courses[i];
  return NULL;
}
