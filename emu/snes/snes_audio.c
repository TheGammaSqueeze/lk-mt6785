#include "snes_audio.h"

void snes_audio_init(snes_mixer *mx)
{
	int i;
	for (i = 0; i < SNES_AUD_VOICES; i++) mx->v[i].active = 0;
	mx->master = 256;
}

void snes_audio_stop_all(snes_mixer *mx)
{
	int i;
	for (i = 0; i < SNES_AUD_VOICES; i++) mx->v[i].active = 0;
}

void snes_audio_play(snes_mixer *mx, const int16_t *pcm, uint32_t len,
		     uint32_t rate, uint32_t loop_start, uint32_t loop_end,
		     int loop, int gain, int is_bgm)
{
	snes_voice *v;
	int i, slot;
	if (!pcm || !len) return;
	if (is_bgm) {
		slot = 0;
	} else {
		slot = -1;
		for (i = 1; i < SNES_AUD_VOICES; i++)
			if (!mx->v[i].active) { slot = i; break; }
		if (slot < 0) slot = 1;   /* steal the first SFX voice */
	}
	v = &mx->v[slot];
	v->pcm = pcm;
	v->len = len;
	v->loop_start = loop_start;
	v->loop_end = (loop_end > loop_start && loop_end <= len) ? loop_end : len;
	v->step = (uint32_t)(((uint64_t)rate << 16) / SNES_AUD_HZ);
	if (!v->step) v->step = 1;
	v->phase = 0;
	v->loop = loop;
	v->gain = gain < 0 ? 0 : (gain > 256 ? 256 : gain);
	v->active = 1;
}

void snes_audio_mix(snes_mixer *mx, int16_t *out, unsigned frames)
{
	unsigned f;
	int i;
	for (f = 0; f < frames; f++) {
		int acc = 0;
		for (i = 0; i < SNES_AUD_VOICES; i++) {
			snes_voice *v = &mx->v[i];
			uint32_t idx;
			int s;
			if (!v->active) continue;
			idx = v->phase >> 16;
			if (idx >= v->len) { v->active = 0; continue; }
			s = v->pcm[idx];
			acc += (s * v->gain) >> 8;
			v->phase += v->step;
			idx = v->phase >> 16;
			if (v->loop) {
				if (idx >= v->loop_end)
					v->phase -= (uint32_t)(v->loop_end - v->loop_start) << 16;
			} else if (idx >= v->len) {
				v->active = 0;
			}
		}
		acc = (acc * mx->master) >> 8;
		if (acc > 32767) acc = 32767; else if (acc < -32768) acc = -32768;
		out[f * 2 + 0] = (int16_t)acc;
		out[f * 2 + 1] = (int16_t)acc;
	}
}
