/*
 * Audio file loading for charts: .ogg (stb_vorbis), .wav (dr_wav), .mp3
 * (dr_mp3), all decoded to 16-bit stereo. Old charts often name a file that is
 * not on disk exactly as written (case, extension), so audio_resolve() finds
 * the closest playable match.
 */
#ifndef AUDIO_CODECS_H
#define AUDIO_CODECS_H

#include <stddef.h>
#include <stdint.h>

/* Resolve the WAVE: value of the chart at tja_path to an existing, supported
 * file. Returns 0 and fills out, or -1. */
int audio_resolve(const char *tja_path, const char *wave, char *out, size_t cap);

/* Decode to interleaved stereo int16 (malloc'd; caller frees). Returns 0/-1. */
int audio_decode(const char *path, int16_t **pcm, long *frames, int *rate);

#endif
