/*
 * pico_state_shot.c - a save state's picture and time (pico_state_shot.h).
 *
 * The file: a 20 byte head, then the picture's RGB565 pixels, row after row.
 *   0  'G' 'P' 'S' 'P'
 *   4  version (1), 0
 *   6  width, height (16 bits each)
 *   10 0 (16 bits)
 *   12 when it was made: seconds since 1970 UTC, 64 bits (0 not known)
 * Little-endian throughout.
 */
#include "pico_state_shot.h"
#include "gba_sd_save.h"
#include "pico_clock.h"
#include <string.h>

#define SHOT_HEAD 20
#define SHOT_MAX  (SHOT_HEAD + PSHOT_W * PSHOT_H * 2)

/* the game's last frame (the display's, pico_disp_head.inc); a build without one has no pictures */
extern int ayaneo_game_picture(const unsigned short **pix, unsigned int *sw, unsigned int *sh,
			       unsigned int *pitch, int *rect) __attribute__((weak));

static uint8_t s_file[SHOT_MAX];
/* the last game's picture as its frame was let go (pico_shot_keep): the exits that save their
 * suspend point after the display has given the frame back still have one */
static uint16_t s_kept[PSHOT_W * PSHOT_H];
static int s_kept_w, s_kept_h;

void pico_shot_ext(const char *state_ext, char *out)
{
	out[0] = 'p'; out[1] = 'v';
	out[2] = (state_ext && state_ext[0] == 's' && state_ext[1] == 't' && state_ext[2]) ? state_ext[2] : 's';
	out[3] = 0;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

/* the picture in the 160 x 120 box at the shape it has on the screen, each pixel the average of the
 * part of the game's picture it covers; its size in *w, *h. 0 when there is no picture. */
static int shrink(uint16_t *out, int *w, int *h)
{
	const unsigned short *pix = 0;
	unsigned int sw = 0, sh = 0, pitch = 0;
	int r[4], dw, dh, x, y;
	if (!ayaneo_game_picture || !ayaneo_game_picture(&pix, &sw, &sh, &pitch, r) || !pix || !sw || !sh)
		return 0;
	if (r[2] <= 0 || r[3] <= 0) { r[2] = (int)sw; r[3] = (int)sh; }
	/* the shape the player sees: the rectangle the picture fills on the screen */
	if ((long long)r[2] * PSHOT_H >= (long long)r[3] * PSHOT_W) {
		dw = PSHOT_W;
		dh = (int)(((long long)PSHOT_W * r[3] + r[2] / 2) / r[2]);
	} else {
		dh = PSHOT_H;
		dw = (int)(((long long)PSHOT_H * r[2] + r[3] / 2) / r[3]);
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;
	for (y = 0; y < dh; y++) {
		unsigned y0 = (unsigned)y * sh / (unsigned)dh, y1 = (unsigned)(y + 1) * sh / (unsigned)dh;
		if (y1 <= y0) y1 = y0 + 1;
		for (x = 0; x < dw; x++) {
			unsigned x0 = (unsigned)x * sw / (unsigned)dw, x1 = (unsigned)(x + 1) * sw / (unsigned)dw;
			unsigned sr = 0, sg = 0, sb = 0, n = 0, u, v;
			if (x1 <= x0) x1 = x0 + 1;
			for (v = y0; v < y1 && v < sh; v++)
				for (u = x0; u < x1 && u < sw; u++) {
					unsigned c = pix[v * pitch + u];
					sr += (c >> 11) & 31; sg += (c >> 5) & 63; sb += c & 31;
					n++;
				}
			if (!n) n = 1;
			out[y * dw + x] = (uint16_t)(((sr + n / 2) / n) << 11 | ((sg + n / 2) / n) << 5 | ((sb + n / 2) / n));
		}
	}
	*w = dw;
	*h = dh;
	return 1;
}

void pico_shot_keep(void)
{
	if (!shrink(s_kept, &s_kept_w, &s_kept_h)) s_kept_w = s_kept_h = 0;
}

int pico_shot_save(fat_vol *v, const char *dir, const char *rom, const char *state_ext, int bg)
{
	char ext[4];
	int w, h, i;
	long long t;
	uint16_t *px = (uint16_t *)(void *)(s_file + SHOT_HEAD);
	uint32_t len;
	{ extern volatile int g_gba_test_noio __attribute__((weak));   /* load testing: nothing written */
	  if (&g_gba_test_noio && g_gba_test_noio) return 0; }
	if (!v || !dir || !rom) return -1;
	if (!shrink(px, &w, &h)) {	/* the frame is gone: the one kept as it went, else the time alone */
		w = s_kept_w; h = s_kept_h;
		if (w && h) memcpy(px, s_kept, (unsigned)(w * h) * 2u);
	}
	t = pico_clock_is_set() ? pico_clock_utc() : 0;   /* a stand-in time is not recorded */
	s_file[0] = 'G'; s_file[1] = 'P'; s_file[2] = 'S'; s_file[3] = 'P';
	s_file[4] = 1; s_file[5] = 0;
	put16(s_file + 6, (unsigned)w);
	put16(s_file + 8, (unsigned)h);
	put16(s_file + 10, 0);
	for (i = 0; i < 8; i++) s_file[12 + i] = (uint8_t)((unsigned long long)(t > 0 ? t : 0) >> (8 * i));
	len = SHOT_HEAD + (uint32_t)w * (uint32_t)h * 2u;
	pico_shot_ext(state_ext, ext);
	if (bg) return gba_sd_write_named_bg(v, dir, rom, ext, s_file, len);
	return gba_sd_write_named(v, dir, rom, ext, s_file, len);
}

int pico_shot_load(fat_vol *v, const char *dir, const char *rom, const char *state_ext,
		   uint16_t *pix, int *w, int *h, long long *utc)
{
	char ext[4];
	uint32_t n;
	unsigned pw, ph;
	unsigned long long t = 0;
	int i;
	if (!v || !dir || !rom) return 0;
	pico_shot_ext(state_ext, ext);
	n = gba_sd_read_named(v, dir, rom, ext, s_file, SHOT_MAX);
	if (n < SHOT_HEAD || s_file[0] != 'G' || s_file[1] != 'P' || s_file[2] != 'S' || s_file[3] != 'P' || s_file[4] != 1)
		return 0;
	pw = get16(s_file + 6);
	ph = get16(s_file + 8);
	if (pw > PSHOT_W || ph > PSHOT_H || n < SHOT_HEAD + pw * ph * 2u) return 0;   /* 0 x 0: the time alone */
	for (i = 7; i >= 0; i--) t = t << 8 | s_file[12 + i];
	memcpy(pix, s_file + SHOT_HEAD, pw * ph * 2u);
	*w = (int)pw;
	*h = (int)ph;
	*utc = (long long)t;
	return 1;
}
