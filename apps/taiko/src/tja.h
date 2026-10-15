/*
 * TJA (TaikoJiro chart) parser and timeline compiler.
 *
 * tja_parse() turns the text into per-course note lists with absolute times in
 * seconds on the audio clock (0 = first sample of WAVE). No SDL, no I/O besides
 * tja_load(), so it builds on the host for tools/tja_dump.c.
 *
 * Supported: header and course fields, #START..#END (P1 of double charts),
 * #BPMCHANGE, #MEASURE, #DELAY, #SCROLL, #GOGOSTART/END, #BARLINEON/OFF.
 * Branches (#BRANCHSTART/#N/#E/#M/#BRANCHEND): only the normal (#N) path is
 * kept. Ignored: #SECTION, #LEVELHOLD, #NEXTSONG, #BRANCHSTART conditions.
 */
#ifndef TJA_H
#define TJA_H

#include <stddef.h>

enum {
  TJA_DON = 1,
  TJA_KA = 2,
  TJA_BIGDON = 3,
  TJA_BIGKA = 4,
  TJA_ROLL = 5,
  TJA_BIGROLL = 6,
  TJA_BALLOON = 7,
  /* 8 is the roll/balloon end marker; it is folded into end_t */
  TJA_KUSUDAMA = 9,
};

typedef struct {
  double t;      /* seconds on the audio clock */
  double end_t;  /* roll/balloon/kusudama end; equals t for single notes */
  double bpm;    /* BPM in effect at the note */
  double scroll; /* #SCROLL multiplier in effect */
  int type;      /* TJA_* */
  int hits;      /* balloon/kusudama: hits required (0 if BALLOON: ran out) */
  int gogo;      /* inside #GOGOSTART..#GOGOEND */
} TjaNote;

typedef struct {
  double t;
  double bpm;
  double scroll;
  int visible; /* #BARLINEON/OFF */
} TjaBar;

typedef struct {
  char name[16]; /* as written: Easy, Normal, Hard, Oni, Edit */
  int id;        /* 0 Easy, 1 Normal, 2 Hard, 3 Oni, 4 Edit/Ura */
  int level;
  int score_init, score_diff;
  int double_play; /* STYLE:Double (only P1 is parsed) */
  int *balloons;   /* BALLOON: list */
  size_t n_balloons;
  TjaNote *notes;
  size_t n_notes;
  TjaBar *bars;
  size_t n_bars;
  int branched; /* chart contained #BRANCHSTART; only #N was kept */
} TjaCourse;

typedef struct {
  char title[256];
  char subtitle[256];
  char wave[256];
  char genre[64];
  double bpm;
  double offset; /* OFFSET: first measure starts at -offset seconds */
  double demostart;
  int songvol, sevol;
  TjaCourse *courses;
  size_t n_courses;
  int bad_utf8; /* text was not UTF-8 and was converted from Shift-JIS (CP932) */
} Tja;

/* Both return 0 on success, -1 with a message in err. */
int tja_parse(Tja *t, const char *text, size_t len, char *err, size_t errsz);
int tja_load(Tja *t, const char *path, char *err, size_t errsz);
void tja_free(Tja *t);

/* Course by TJA id (3 = Oni), or NULL. */
const TjaCourse *tja_course(const Tja *t, int id);

#endif
