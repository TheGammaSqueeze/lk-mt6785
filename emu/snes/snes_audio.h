/*
 * Portable software audio mixer for the SNES menu. One looping BGM voice plus a
 * handful of one-shot SFX voices, mixed to interleaved stereo s16 at 48 kHz for
 * the LK AFE ring. No LK dependencies (the host harness can link it too, though
 * it produces no sound there). Source PCM is mono s16 at an arbitrary rate; each
 * voice resamples with a 16.16 fixed-point phase accumulator.
 */
#ifndef SNES_AUDIO_H
#define SNES_AUDIO_H

#include <stdint.h>

#define SNES_AUD_HZ      48000u
#define SNES_AUD_VOICES  8          /* voice 0 is reserved for BGM */

typedef struct {
	const int16_t *pcm;     /* mono s16 */
	uint32_t len;           /* total frames */
	uint32_t loop_start, loop_end;   /* frames; loop when loop_end > loop_start */
	uint32_t step;          /* 16.16 phase increment (src_rate/48000) */
	uint32_t phase;         /* 16.16 position into pcm */
	int      loop;
	int      active;
	int      gain;          /* 0..256 */
} snes_voice;

typedef struct {
	snes_voice v[SNES_AUD_VOICES];
	int master;             /* 0..256 */
} snes_mixer;

void snes_audio_init(snes_mixer *mx);
void snes_audio_stop_all(snes_mixer *mx);

/* Start a voice. is_bgm routes to voice 0 (replacing any current BGM); SFX pick
 * the oldest free (or steal voice 1) slot. gain is 0..256. */
void snes_audio_play(snes_mixer *mx, const int16_t *pcm, uint32_t len,
		     uint32_t rate, uint32_t loop_start, uint32_t loop_end,
		     int loop, int gain, int is_bgm);

/* Mix `frames` interleaved stereo frames at 48 kHz into out (2*frames shorts). */
void snes_audio_mix(snes_mixer *mx, int16_t *out, unsigned frames);

#endif
