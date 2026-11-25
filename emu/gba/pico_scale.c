/*
 * pico_scale.c - the picture scaling rows every system's in-game menu shows (pico_scale.h).
 * The settings themselves and the ratio table are the display's (ayaneo_settings.inc).
 */
#include "pico_scale.h"

extern int  ayaneo_scale_ratio(int sys);
extern int  ayaneo_scale_whole(int sys);
extern int  ayaneo_scale_onscreen(int sys);
extern void ayaneo_scale_custom(int sys, unsigned *w, unsigned *h);
extern void ayaneo_scale_set_ratio(int sys, int v);
extern void ayaneo_scale_set_whole(int sys, int v);
extern void ayaneo_scale_set_onscreen(int sys, int v);
extern void ayaneo_scale_set_custom(int sys, unsigned w, unsigned h);
extern const char *ayaneo_scale_ratio_name(int ratio);

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putu(char *p, unsigned v)
{
	char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = (char)('0' + v % 10u); v /= 10u; }
	while (n) *p++ = t[--n];
	return p;
}

const char *pico_scale_aspect_name(int sys)
{
	int r = ayaneo_scale_ratio(sys);
	return r > PICO_RATIO_OWN && r < PICO_RATIO_COUNT ? ayaneo_scale_ratio_name(r) : 0;
}

int pico_scale_aspect_step(int sys, int dir, int pos, int n, unsigned own)
{
	unsigned char ring[PICO_RATIO_COUNT];
	int m = 0, r, at, cur = ayaneo_scale_ratio(sys);
	if (n < 1) n = 1;
	if (pos < 0 || pos >= n) pos = 0;
	for (r = PICO_RATIO_OWN + 1; r < PICO_RATIO_COUNT; r++)
		if (!((own >> r) & 1u)) ring[m++] = (unsigned char)r;
	at = pos;
	for (r = 0; r < m; r++) if (ring[r] == cur) at = n + r;
	at = (at + (dir > 0 ? 1 : -1) + n + m) % (n + m);
	if (at < n) { ayaneo_scale_set_ratio(sys, PICO_RATIO_OWN); return at; }
	ayaneo_scale_set_ratio(sys, ring[at - n]);
	return -1;
}

const char *pico_scale_row_label(int row)
{
	switch (row) {
	case PICO_SCALE_ROW_WHOLE:    return "Integer Scale";
	case PICO_SCALE_ROW_ONSCREEN: return "Filter Alignment";
	case PICO_SCALE_ROW_CUSTOM_W: return "Custom Width";
	default:                      return "Custom Height";
	}
}

char *pico_scale_row_value(int sys, int row, char *p)
{
	unsigned w, h;
	switch (row) {
	case PICO_SCALE_ROW_WHOLE: {
		int v = ayaneo_scale_whole(sys);
		return put(p, v == 2 ? "Overscale" : v == 1 ? "On" : "Off");
	}
	case PICO_SCALE_ROW_ONSCREEN:
		return put(p, ayaneo_scale_onscreen(sys) ? "Screen Pixels" : "Game Pixels");
	default:
		ayaneo_scale_custom(sys, &w, &h);
		return putu(p, row == PICO_SCALE_ROW_CUSTOM_W ? w : h);
	}
}

int pico_scale_row_change(int sys, int row, int dir)
{
	unsigned w, h;
	if (!dir) return 0;
	dir = dir > 0 ? 1 : -1;
	switch (row) {
	case PICO_SCALE_ROW_WHOLE:
		ayaneo_scale_set_whole(sys, (ayaneo_scale_whole(sys) + dir + 3) % 3);
		return 1;
	case PICO_SCALE_ROW_ONSCREEN:
		ayaneo_scale_set_onscreen(sys, !ayaneo_scale_onscreen(sys));
		return 1;
	default:
		ayaneo_scale_custom(sys, &w, &h);
		if (row == PICO_SCALE_ROW_CUSTOM_W) w = (unsigned)((int)w + dir < 1 ? 99 : (int)w + dir > 99 ? 1 : (int)w + dir);
		else                                h = (unsigned)((int)h + dir < 1 ? 99 : (int)h + dir > 99 ? 1 : (int)h + dir);
		ayaneo_scale_set_custom(sys, w, h);
		return 1;
	}
}
