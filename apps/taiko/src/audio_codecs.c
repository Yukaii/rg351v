#include "audio_codecs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

extern int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);

static int ext_kind(const char *name) { /* 1 ogg, 2 wav, 3 mp3, 0 other */
  const char *dot = strrchr(name, '.');
  if (!dot) return 0;
  if (!strcasecmp(dot, ".ogg")) return 1;
  if (!strcasecmp(dot, ".wav")) return 2;
  if (!strcasecmp(dot, ".mp3")) return 3;
  return 0;
}

static int is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

int audio_resolve(const char *tja_path, const char *wave, char *out, size_t cap) {
  char dir[1024], want[512];
  snprintf(dir, sizeof dir, "%s", tja_path);
  char *sl = strrchr(dir, '/');
  if (sl) *sl = 0;
  else strcpy(dir, ".");
  snprintf(want, sizeof want, "%s", wave);
  for (char *q = want; *q; q++)
    if (*q == '\\') *q = '/'; /* charts written on Windows */
  const char *base = strrchr(want, '/') ? strrchr(want, '/') + 1 : want;
  if (!*base) return -1;

  char p[1700];
  snprintf(p, sizeof p, "%s/%s", dir, want);
  if (ext_kind(p) && is_file(p)) {
    snprintf(out, cap, "%s", p);
    return 0;
  }

  /* case-insensitive name match, then same stem with another extension */
  char stem[512];
  snprintf(stem, sizeof stem, "%s", base);
  char *dot = strrchr(stem, '.');
  if (dot) *dot = 0;
  DIR *d = opendir(dir);
  if (!d) return -1;
  char best[1700] = "";
  int best_rank = 99;
  struct dirent *de;
  while ((de = readdir(d))) {
    int k = ext_kind(de->d_name);
    if (!k) continue;
    char cand_stem[512];
    snprintf(cand_stem, sizeof cand_stem, "%s", de->d_name);
    char *cd = strrchr(cand_stem, '.');
    if (cd) *cd = 0;
    int rank;
    if (!strcasecmp(de->d_name, base)) rank = 0; /* same name, other case */
    else if (!strcasecmp(cand_stem, stem)) rank = k; /* same stem: ogg < wav < mp3 */
    else continue;
    if (rank < best_rank) {
      best_rank = rank;
      snprintf(best, sizeof best, "%s/%s", dir, de->d_name);
    }
  }
  closedir(d);
  if (!best[0]) return -1;
  snprintf(out, cap, "%s", best);
  return 0;
}

/* Normalise to interleaved stereo; takes ownership of src. */
static int to_stereo(int16_t *src, long frames, int ch, int16_t **out) {
  if (ch == 2) {
    *out = src;
    return 0;
  }
  int16_t *o = malloc((size_t)frames * 2 * sizeof(int16_t));
  if (!o) {
    free(src);
    return -1;
  }
  for (long i = 0; i < frames; i++) {
    o[2 * i] = src[(size_t)i * (size_t)ch];
    o[2 * i + 1] = ch >= 2 ? src[(size_t)i * (size_t)ch + 1] : src[(size_t)i * (size_t)ch];
  }
  free(src);
  *out = o;
  return 0;
}

int audio_decode(const char *path, int16_t **pcm, long *frames, int *rate) {
  switch (ext_kind(path)) {
    case 1: {
      int ch = 0, r = 0;
      short *raw = NULL;
      int n = stb_vorbis_decode_filename(path, &ch, &r, &raw);
      if (n <= 0 || ch < 1) return -1;
      *frames = n;
      *rate = r;
      return to_stereo(raw, n, ch, pcm); /* extra channels are dropped */
    }
    case 2: {
      unsigned ch = 0, r = 0;
      drwav_uint64 n = 0;
      drwav_int16 *raw = drwav_open_file_and_read_pcm_frames_s16(path, &ch, &r, &n, NULL);
      if (!raw || n == 0 || ch < 1) return -1;
      *frames = (long)n;
      *rate = (int)r;
      int16_t *copy = malloc((size_t)n * ch * sizeof(int16_t));
      if (!copy) {
        drwav_free(raw, NULL);
        return -1;
      }
      memcpy(copy, raw, (size_t)n * ch * sizeof(int16_t));
      drwav_free(raw, NULL);
      return to_stereo(copy, (long)n, (int)ch, pcm);
    }
    case 3: {
      drmp3_config cfg;
      drmp3_uint64 n = 0;
      drmp3_int16 *raw = drmp3_open_file_and_read_pcm_frames_s16(path, &cfg, &n, NULL);
      if (!raw || n == 0 || cfg.channels < 1) return -1;
      *frames = (long)n;
      *rate = (int)cfg.sampleRate;
      int16_t *copy = malloc((size_t)n * cfg.channels * sizeof(int16_t));
      if (!copy) {
        drmp3_free(raw, NULL);
        return -1;
      }
      memcpy(copy, raw, (size_t)n * cfg.channels * sizeof(int16_t));
      drmp3_free(raw, NULL);
      return to_stereo(copy, (long)n, (int)cfg.channels, pcm);
    }
    default: return -1;
  }
}
