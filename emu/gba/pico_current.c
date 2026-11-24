/*
 * The Current menu theme: a home with a rail (Games, Resume, Settings) over a blue current, the
 * selected system's console and mark on a stage, the systems on the card in a strip below, a
 * library per system with the game's box art, the recently played games, and the settings. It
 * replaces the classic carousel when it is the saved theme (gba_snes_menu_run hands over here).
 *
 * Everything is drawn in software into the menu canvas. The current of light behind it is drawn
 * in code every frame and drifts slowly; the art and type come from the theme's card pack,
 * /gammaos/current-<W>x<H>.pack (tools/ayaneo/pico_current/build_pack.py), already at this
 * screen's pixel size. The layout is worked out in design units: the screen is 720 units tall,
 * U() turns units into pixels.
 *
 * Memory (the classic menu's buffers are free while this theme runs), each buffer up to 6 MB:
 *   0x50000000  the unpacked pack
 *   0x51000000  the frame, composed each refresh and copied to the canvas
 *   0x51600000  the still part of the backdrop (its gradients), plain
 *   0x51C00000  the same with the reading shade of the library and the settings
 *   0x52200000  the menu itself (premultiplied ARGB, transparent around it), drawn on a change
 *   0x52A00000  the compressed pack while it loads (the classic menu's deflate staging, which the
 *               box art loader uses as its scratch afterwards)
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "menu/snespack.h"          /* snes_hash: the theme names are saved as this hash */
#include "menu/snes_menu.h"         /* snes_input */
#include "menu/gba_name.h"          /* gba_clean_name */
#include "pico_names.h"            /* names in any script */
#include "gba_sd_save.h"           /* gba_sd_legacy_own */
#include "pico_fold.h"             /* names' letters without their accents */
#include "sd_fat.h"                 /* gba_rom_entry, GBA_CONSOLE_* */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
#define PICO_LINK 1                 /* the consoles with Bluetooth: the Link tab (pico_link_ui.h) */
#include "pico_link_ui.h"
#include "pico_net_ui.h"            /* and Wi-Fi: the Network tab */
#else
#define PICO_LINK 0
#endif
#include "fat_ro.h"
#include "fat_wr.h"
#include "pico_i18n.h"          /* tr(): the menus' languages */
#include "pico_clock.h"         /* the date and time */
#include "pico_stats.h"         /* play time and last played */
#include "pico_libmem.h"        /* the library's tables: thousands of games */
#include "rom_archive.h"        /* why a game did not start */
#include "pico_version.h"       /* the About row */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
#include "pico_ra.h"            /* RetroAchievements: the cards, the Achievements page's detail */
#include "pico_cheats.h"        /* the Cheats page */
#endif
#include "pico_input.h"         /* the Controls page */
#include "pico_settings.h"      /* the settings every theme offers */
#include <kernel/thread.h>       /* thread_sleep: a pack read tried again */

extern unsigned int gpt4_get_current_tick(void);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern void ayaneo_display_prepare(void);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern fat_vol *gba_sd_menu_vol(void);
extern void mtk_wdt_restart(void);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void ayaneo_menu_audio_silence(void);
extern void ayaneo_menu_settings_persist(void);
extern void ayaneo_settings_persist_flush(void);
extern int  ayaneo_get_skip_gba_intro(void);
extern void ayaneo_set_skip_gba_intro(int v);
extern int  ayaneo_get_intro_black(void);
extern void ayaneo_set_intro_black(int v);
extern int  ayaneo_get_resume_on_boot(void);
extern void ayaneo_set_resume_on_boot(int v);
extern int  ayaneo_get_intro_on_resume(void);
extern void ayaneo_set_intro_on_resume(int v);
extern int  ayaneo_get_resume_on_open(void);
extern void ayaneo_set_resume_on_open(int v);
extern unsigned int gba_menu_theme(void);   /* the theme in use (gba_snes_menu.c): Current unless another was picked */
extern int  ayaneo_get_menu_colour(void);
extern void ayaneo_set_menu_colour(int c);
extern void ayaneo_set_menu_theme(unsigned int hash);
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_get_mute_bios(void);
extern void ayaneo_set_mute_bios(int v);
extern void ayaneo_boot_to_os(void);
extern int  gba_battery_pct(int *charging);
extern int  gba_battery_last(int *charging);
extern volatile int g_menu_live;
/* shared with the classic menu (gba_snes_menu.c) */
extern void gba_menu_read_input(snes_input *in);
extern void gba_menu_av_poll(void);
extern void gba_menu_av_draw(unsigned int *fb, unsigned int pitch, int W, int H);
extern int  gba_menu_boxart_load(const gba_rom_entry *roms, int nrom);
extern const unsigned char *gba_menu_boxart(int i, unsigned int *w, unsigned int *h);
extern void gba_menu_boxart_budget(int n);
extern int  gba_menu_boxart_pending(void);
extern void gba_menu_reverse_into(void (*render)(uint32_t *snap, unsigned int pitch));
extern void gba_menu_present(void);
extern int  ayaneo_get_strobe(void);
extern void ayaneo_set_strobe(int v);
extern const char *ayaneo_strobe_name(int v);
extern int  g_punch_fade;
extern const uint32_t *g_punch_ov;
extern const int16_t *g_punch_ov_x0, *g_punch_ov_x1;
extern void ayaneo_canvas_dims(unsigned int *cw, unsigned int *ch);

#define CUR_RAW_PA    0x50000000u
#define CUR_RAW_MAX   0x01000000u
#define CUR_FRAME_PA  0x51000000u
#define CUR_BG0_PA    0x51600000u
#define CUR_BG1_PA    0x51C00000u
#define CUR_UI_PA     0x52200000u
#define CUR_FRAME_MAX 0x00600000u
#define CUR_COMP_PA   0x52A00000u
#define CUR_COMP_MAX  0x00800000u
#define CUR_MAGIC_Z   0x5A534E53u     /* "SNSZ", the deflate wrapper of every menu pack */

#define NSYS          9               /* GBA_CONSOLE_GB .. GBA_CONSOLE_NES, the art in the same order */
#define NRECENT       10
#define LIB_ROWS      7
#define NTHEMES       8

/* ---- colours -------------------------------------------------------------------------------- */
#define C_INK     0xF4F8FFu
#define C_MUTED   0xBDCEE4u
#define C_ACCENT  g_accent
#define C_BTN_INK 0x112030u
#define C_SHADOW  0x060A23u
#define C_SHADE   0x060D19u

/* The colours of the theme: the current is recoloured by a 3x3 matrix (1024 = 1.0; a hue turn
 * that keeps the brightness, or a desaturation) and the accent follows. 0 is the design's blue. */
#define NCOLOURS 13
static const struct { const char *name; uint32_t accent; int16_t m[9]; } k_colour[NCOLOURS] = {
	{ "Blue",     0x98E8FFu, { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 } },
	{ "Violet",   0xD4BBFFu, { 595, -302, 731, 181, 1034, -191, -527, 786, 764 } },
	{ "Crimson",  0xFFB4C0u, { -450, 606, 868, 468, 665, -109, -283, 1760, -453 } },
	{ "Amber",    0xFFD49Au, { -610, 1355, 279, 454, 463, 106, 322, 1559, -857 } },
	{ "Emerald",  0xA2F2C6u, { 637, 1250, -863, 20, 669, 335, 940, -165, 248 } },
	{ "Graphite", 0xE2E6ECu, { 299, 659, 66, 196, 761, 66, 196, 659, 169 } },
	{ "Indigo",   0xADB8FFu, { 928, -179, 275, 51, 1053, -80, -221, 240, 1005 } },
	{ "Orchid",   0xF8ADFFu, { 293, -209, 939, 280, 968, -224, -613, 1165, 473 } },
	{ "Rose",     0xFFADD9u, { -99, 98, 1026, 391, 838, -205, -554, 1550, 28 } },
	{ "Coral",    0xFFBEADu, { -577, 951, 650, 481, 565, -21, -35, 1744, -685 } },
	{ "Lime",     0xD6F0A8u, { -251, 1765, -490, 288, 417, 319, 909, 810, -695 } },
	{ "Teal",     0xA7F1E7u, { 860, 910, -746, -27, 783, 268, 752, -288, 560 } },
	{ "Sepia",    0xF0D9B5u, { 389, 857, 86, 314, 692, 69, 224, 494, 50 } },
};
/* The order Settings steps through them: round the colour wheel from the design's blue, then the
 * two neutrals. The saved setting is the index above, so colours are only ever added at the end. */
static const uint8_t k_colour_order[NCOLOURS] = { 0, 6, 1, 7, 8, 2, 9, 3, 10, 4, 11, 12, 5 };
static int colour_step(int c, int dir)
{
	int i;
	for (i = 0; i < NCOLOURS && k_colour_order[i] != c; i++) ;
	if (i == NCOLOURS) i = 0;
	return k_colour_order[(i + dir + NCOLOURS) % NCOLOURS];
}
static uint32_t g_accent = 0x98E8FFu;
static int g_colour;
static inline uint32_t recolour(uint32_t c)
{
	const int16_t *m = k_colour[g_colour].m;
	int r = (int)(c >> 16) & 255, g = (int)(c >> 8) & 255, b = (int)c & 255, o[3], i;
	if (!g_colour) return c;
	for (i = 0; i < 3; i++) {
		int v = (m[3 * i] * r + m[3 * i + 1] * g + m[3 * i + 2] * b) >> 10;
		o[i] = v < 0 ? 0 : v > 255 ? 255 : v;
	}
	return (uint32_t)(o[0] << 16 | o[1] << 8 | o[2]);
}
static void colour_set(int c)
{
	g_colour = c >= 0 && c < NCOLOURS ? c : 0;
	g_accent = k_colour[g_colour].accent;
}

/* ---- the pack -------------------------------------------------------------------------------- */
enum { T_IMAGE = 1, T_MASK = 2, T_FONT = 3, T_TEXT = 4 };
typedef struct { char name[32]; uint16_t type, flags, w, h; uint32_t off, len; } cur_entry;
typedef struct { int w, h; const uint16_t *rgb; const uint8_t *a; const uint8_t *rgb24; } cimg;   /* no rgb: a mask */
typedef struct { uint32_t code; int16_t xo, yo; uint16_t x, y, w, h; uint32_t adv; } cglyph;
/* a font, the glyphs the language in use adds to it (its own atlas), and for a font that shows game
 * names the names' glyphs past the font's own (pico_names.c, an atlas of their own too) */
typedef struct {
	int px, asc, n, aw, nk; const cglyph *g; const int32_t *kern; const uint8_t *atlas;
	int sn, saw; const cglyph *sg; const uint8_t *satlas;
	int nn, naw, nmed; unsigned int nser; const cglyph *ng; const uint8_t *natlas;
} cfont;
typedef char cglyph_is_pico_nglyph[sizeof(cglyph) == sizeof(pico_nglyph) ? 1 : -1];

static const uint8_t *s_raw;
static const cur_entry *s_ent;
static int s_nent;

static const cur_entry *find(const char *name, int type)
{
	int i;
	for (i = 0; i < s_nent; i++)
		if (s_ent[i].type == type && !strncmp(s_ent[i].name, name, 32)) return &s_ent[i];
	return 0;
}
static cimg img(const char *name)
{
	cimg r = { 0, 0, 0, 0, 0 };
	const cur_entry *e = find(name, T_IMAGE);
	if (e && (e->flags & 2)) {	/* 8 bits a channel, opaque (the backdrops) */
		r.w = e->w; r.h = e->h; r.rgb24 = s_raw + e->off;
	} else if (e) {
		r.w = e->w; r.h = e->h; r.rgb = (const uint16_t *)(s_raw + e->off);
		r.a = (e->flags & 1) ? s_raw + e->off + (uint32_t)e->w * e->h * 2 : 0;
	} else if ((e = find(name, T_MASK))) {
		r.w = e->w; r.h = e->h; r.a = s_raw + e->off;
	}
	return r;
}
/* the language pack's glyphs for the font of this name (pico_i18n.c), or none */
static void font_extra(cfont *f, const char *name)
{
	uint32_t len = 0;
	const uint8_t *e = pico_lang_entry(name, T_FONT, &len);
	f->sn = 0; f->saw = 0; f->sg = 0; f->satlas = 0;
	if (e && len >= 16) {
		const uint16_t *h = (const uint16_t *)e;
		if (16u + h[3] * sizeof(cglyph) + (uint32_t)h[4] * h[5] <= len) {
			f->sn = h[3]; f->saw = h[4];
			f->sg = (const cglyph *)(e + 16);
			f->satlas = (const uint8_t *)(f->sg + f->sn);
		}
	}
}
/* the names' glyphs at this font's size (medium: its weight is the focused rows'); again whenever
 * more were read */
static void font_names(cfont *f, int medium)
{
	const pico_nglyph *g = 0;
	const uint8_t *at = 0;
	int aw = 0;
	f->nmed = medium;
	f->nser = pico_names_serial();
	f->nn = pico_names_set(f->px, medium, &g, &at, &aw);
	f->ng = (const cglyph *)g; f->natlas = at; f->naw = aw;
}
static void font_names_sync(cfont *f)
{
	if (f->g && f->nser && f->nser != pico_names_serial()) font_names(f, f->nmed);
}
static cfont font(const char *name)
{
	cfont f;
	const cur_entry *e = find(name, T_FONT);
	memset(&f, 0, sizeof f);
	if (e) {
		const uint16_t *h = (const uint16_t *)(s_raw + e->off);
		f.px = h[0]; f.asc = h[1]; f.n = h[3]; f.aw = h[4]; f.nk = h[6];
		f.g = (const cglyph *)(s_raw + e->off + 16);
		f.kern = (const int32_t *)(f.g + f.n);
		f.atlas = (const uint8_t *)(f.kern + 2 * f.nk);
	}
	font_extra(&f, name);
	return f;
}

/* read a card pack (/gammaos/<name>-<W>x<H>.pack) and unpack it at raw, the compressed bytes staged
 * at comp; the pack becomes the one looked up; 0 ok */
static int pack_read(const char *name, unsigned W, unsigned H, uint8_t *comp, uint32_t comp_max,
		     uint8_t *raw, uint32_t raw_max)
{
	char path[64];
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	unsigned char hdr[12];
	uint32_t rawn, compn;
	unsigned long zlen;
	const uint32_t *ph;
	snprintf(path, sizeof path, "/gammaos/%s-%ux%u.pack", name, W, H);
	if (!v || fat_open(v, path, &f) != 0) return -1;
	if (fat_read(&f, 0, hdr, 12) != 12) return -1;
	memcpy(&rawn, hdr + 4, 4); memcpy(&compn, hdr + 8, 4);
	if (*(const uint32_t *)hdr != CUR_MAGIC_Z || !rawn || rawn > raw_max || !compn || compn > comp_max ||
	    12u + compn > f.size) return -2;
	if (fat_read(&f, 12, comp, compn) != compn) return -3;
	zlen = compn;
	if (zunzip(comp, &zlen, raw, (int)rawn, 0) != 0) return -4;
	ph = (const uint32_t *)raw;
	if (ph[0] != 0x52554350u || ph[1] != 1u || (ph[3] & 0xFFFFu) != W || (ph[3] >> 16) != H) return -5;
	s_raw = raw;
	s_nent = (int)ph[2];
	s_ent = (const cur_entry *)(s_raw + 16);
	return 0;
}
/* the menu's pack, /gammaos/current-<W>x<H>.pack; 0 ok */
static int pack_load(unsigned W, unsigned H)
{
	return pack_read("current", W, H, (uint8_t *)CUR_COMP_PA, CUR_COMP_MAX, (uint8_t *)CUR_RAW_PA, CUR_RAW_MAX);
}

/* ---- the frame and drawing ------------------------------------------------------------------ */
static uint32_t *g_fb;
static int g_W, g_H;
static float g_s;                     /* px per design unit */
#define U(v) ((int)((v) * g_s + 0.5f))

/* rgb over d at a 0..256. d's colour is premultiplied by its alpha, so the same blend serves the
 * opaque frame and the transparent menu layer: the alpha goes towards 255 by the same amount. */
static inline uint32_t mix(uint32_t d, uint32_t rgb, int a)
{
	uint32_t rb = d & 0xFF00FFu, ag = (d >> 8) & 0xFF00FFu;
	rb += (((rgb & 0xFF00FFu) - rb) * (uint32_t)a) >> 8;
	ag += ((((0xFF0000u | ((rgb >> 8) & 0xFFu))) - ag) * (uint32_t)a) >> 8;
	return ((ag & 0xFF00FFu) << 8) | (rb & 0xFF00FFu);
}
static inline uint32_t from565(uint16_t v)
{
	uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
	return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
/* While the menu layer is drawn, the part of each row it touches (cleared and composited by it). */
static int g_track;
static int16_t s_ux0[1024], s_ux1[1024];
static inline void touch(int y, int x0, int x1)
{
	if (!g_track || (unsigned)y >= (unsigned)g_H) return;
	if (x0 < 0) x0 = 0;
	if (x1 > g_W) x1 = g_W;
	if (x0 >= x1) return;
	if (s_ux1[y] <= s_ux0[y]) { s_ux0[y] = (int16_t)x0; s_ux1[y] = (int16_t)x1; return; }
	if (x0 < s_ux0[y]) s_ux0[y] = (int16_t)x0;
	if (x1 > s_ux1[y]) s_ux1[y] = (int16_t)x1;
}
/* a horizontal clip for the ticker (put and everything drawn through it): x0..x1, the edges fading
 * over fl / fr px as text hides behind them (0, 0, 0, 0: none) */
static int g_cx0, g_cx1, g_cfl, g_cfr;
/* blend with a = 0..255 coverage times op 0..256 */
static inline void put(int x, int y, uint32_t rgb, int a)
{
	uint32_t *d;
	if (g_cx1) {
		if (x < g_cx0 || x >= g_cx1) return;
		if (g_cfl && x < g_cx0 + g_cfl) a = a * (x - g_cx0) / g_cfl;
		if (g_cfr && x >= g_cx1 - g_cfr) a = a * (g_cx1 - 1 - x) / g_cfr;
	}
	if ((unsigned)x >= (unsigned)g_W || (unsigned)y >= (unsigned)g_H || a <= 0) return;
	touch(y, x, x + 1);
	d = g_fb + y * g_W + x;
	*d = a >= 256 ? (0xFF000000u | rgb) : mix(*d, rgb, a);
}
static void fill(int x, int y, int w, int h, uint32_t rgb, int op)
{
	int i, j;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > g_W) w = g_W - x;
	if (y + h > g_H) h = g_H - y;
	for (j = 0; j < h; j++) {
		uint32_t *d = g_fb + (y + j) * g_W + x;
		touch(y + j, x, x + w);
		for (i = 0; i < w; i++) d[i] = op >= 256 ? (0xFF000000u | rgb) : mix(d[i], rgb, op);
	}
}
/* an image (opaque or with alpha) at x, y, faded by op 0..256 */
static void draw_img(const cimg *im, int x, int y, int op)
{
	int i, j;
	if (!im->rgb || op <= 0) return;
	for (j = 0; j < im->h; j++) {
		int yy = y + j;
		const uint16_t *s = im->rgb + j * im->w;
		const uint8_t *sa = im->a ? im->a + j * im->w : 0;
		if ((unsigned)yy >= (unsigned)g_H) continue;
		touch(yy, x, x + im->w);
		for (i = 0; i < im->w; i++) {
			int xx = x + i, a = sa ? (sa[i] * op) >> 8 : op;
			if ((unsigned)xx >= (unsigned)g_W || a <= 0) continue;
			if (a >= 255 && op >= 256) g_fb[yy * g_W + xx] = 0xFF000000u | from565(s[i]);
			else g_fb[yy * g_W + xx] = mix(g_fb[yy * g_W + xx], from565(s[i]), a + (a >> 7));
		}
	}
}
/* an alpha mask in one colour */
static void draw_mask(const cimg *m, int x, int y, uint32_t rgb, int op)
{
	int i, j;
	if (!m->a || op <= 0) return;
	for (j = 0; j < m->h; j++) {
		int yy = y + j;
		const uint8_t *sa = m->a + j * m->w;
		if ((unsigned)yy >= (unsigned)g_H) continue;
		for (i = 0; i < m->w; i++) {
			int a = (sa[i] * op) >> 8;
			if (a) put(x + i, yy, rgb, a + (a >> 7));
		}
	}
}
/* box art (RGB565 + A8 interleaved, gba_menu_boxart) scaled into a dw x dh box, bilinear; the
 * sample columns and their weights worked out once, opaque texels written straight */
static void draw_art(const unsigned char *px, int sw, int sh, int dx, int dy, int dw, int dh)
{
	static uint16_t cx0[1024], cx1[1024];
	static uint8_t cwx[1024];
	int i, j;
	if (dw > 1024) dw = 1024;
	for (i = 0; i < dw; i++) {
		int fx = ((i * 2 + 1) * sw * 128) / (2 * dw) - 64;
		if (fx < 0) fx = 0;
		cx0[i] = (uint16_t)(fx >> 7); cwx[i] = (uint8_t)(fx & 127);
		cx1[i] = (uint16_t)(cx0[i] + 1 < sw ? cx0[i] + 1 : cx0[i]);
	}
	for (j = 0; j < dh; j++) {
		int fy = ((j * 2 + 1) * sh * 128) / (2 * dh) - 64, y0, y1, wy, yy = dy + j;
		const unsigned char *r0, *r1;
		uint32_t *d;
		if ((unsigned)yy >= (unsigned)g_H) continue;
		if (fy < 0) fy = 0;
		y0 = fy >> 7; wy = fy & 127; y1 = y0 + 1 < sh ? y0 + 1 : y0;
		r0 = px + 3 * y0 * sw; r1 = px + 3 * y1 * sw;
		d = g_fb + yy * g_W;
		touch(yy, dx, dx + dw);
		for (i = 0; i < dw; i++) {
			int xx = dx + i, wx = cwx[i], k;
			const unsigned char *q[4] = { r0 + 3 * cx0[i], r0 + 3 * cx1[i], r1 + 3 * cx0[i], r1 + 3 * cx1[i] };
			int w4[4] = { (128 - wx) * (128 - wy), wx * (128 - wy), (128 - wx) * wy, wx * wy };
			uint32_t r = 0, g = 0, b = 0, a = 0;
			if ((unsigned)xx >= (unsigned)g_W) continue;
			for (k = 0; k < 4; k++) {
				uint32_t v = (uint32_t)(q[k][0] | q[k][1] << 8);
				r += (((v >> 11) & 31) << 3) * (uint32_t)w4[k];
				g += (((v >> 5) & 63) << 2) * (uint32_t)w4[k];
				b += ((v & 31) << 3) * (uint32_t)w4[k];
				a += q[k][2] * (uint32_t)w4[k];
			}
			r >>= 14; g >>= 14; b >>= 14; a >>= 14;
			if (a >= 255) d[xx] = 0xFF000000u | r << 16 | g << 8 | b;
			else if (a) d[xx] = mix(d[xx], r << 16 | g << 8 | b, (int)(a + (a >> 7)));
		}
	}
}
/* square root for the antialiased edges (no libm here): Newton from a bit-level first guess */
static float sq(float x)
{
	union { float f; uint32_t u; } v;
	float y;
	if (x <= 0) return 0;
	v.f = x;
	v.u = (v.u >> 1) + 0x1FC00000u;
	y = v.f;
	y = .5f * (y + x / y);
	y = .5f * (y + x / y);
	return y;
}
/* the area of a stadium (rounded ends of radius h/2) or a disc, antialiased */
static void pill_fill(int x, int y, int w, int h, uint32_t rgb, int op)
{
	int i, j;
	float r = h * .5f;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			float px = i + .5f, py = j + .5f - r, cx = px < r ? r : px > w - r ? w - r : px;
			float dx = px - cx, d = r - sq(dx * dx + py * py) + .5f;
			if (d <= 0) continue;
			put(x + i, y + j, rgb, d >= 1 ? op : (int)(d * op));
		}
}
static void disc(float cx, float cy, float rad, uint32_t rgb, int op)
{
	int x0 = (int)(cx - rad - 1), y0 = (int)(cy - rad - 1), i, j, n = (int)(rad * 2 + 3);
	for (j = 0; j < n; j++)
		for (i = 0; i < n; i++) {
			float dx = x0 + i + .5f - cx, dy = y0 + j + .5f - cy, d = rad - sq(dx * dx + dy * dy) + .5f;
			if (d > 0) put(x0 + i, y0 + j, rgb, d >= 1 ? op : (int)(d * op));
		}
}

/* ---- text ----------------------------------------------------------------------------------- */
/* next code point of UTF-8 text (bytes that are not UTF-8 read as Latin-1) */
static uint32_t utf8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = s[0];
	if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) { c = ((c & 31) << 6) | (s[1] & 63); *ps += 2; }
	else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		c = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); *ps += 3;
	} else if (c >= 0xF0 && c < 0xF8 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		c = ((c & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63); *ps += 4;
	} else *ps += 1;
	return c;
}
#define GLYPHS_FIXED 193           /* ASCII, Latin-1, the bullet and the ellipsis at fixed places */
static const cglyph *glyph_find(const cglyph *g, int lo, int hi, uint32_t c)
{
	while (lo <= hi) {
		int m = (lo + hi) >> 1;
		if (g[m].code == c) return &g[m];
		if (g[m].code < c) lo = m + 1; else hi = m - 1;
	}
	return 0;
}
/* a code point's glyph and the atlas it is in: the font's fixed ones, then the others it carries
 * (the languages' names, sorted), then the language pack's, then the game names'; '?' when none
 * has it */
static const cglyph *glyph(const cfont *f, uint32_t c, const uint8_t **atlas, int *aw)
{
	int i = c >= 0x20 && c < 0x7F ? (int)c - 0x20 : c >= 0xA0 && c <= 0xFF ? 95 + (int)c - 0xA0 :
		c == 0x2022 ? 191 : c == 0x2026 ? 192 : -1;
	const cglyph *g;
	*atlas = f->atlas; *aw = f->aw;
	if (i >= 0 && i < f->n) return &f->g[i];
	if (i < 0) {
		if ((g = glyph_find(f->g, GLYPHS_FIXED, f->n - 1, c))) return g;
		if (f->sg && (g = glyph_find(f->sg, 0, f->sn - 1, c))) { *atlas = f->satlas; *aw = f->saw; return g; }
		if (f->ng && (g = glyph_find(f->ng, 0, f->nn - 1, c))) { *atlas = f->natlas; *aw = f->naw; return g; }
	}
	return &f->g['?' - 0x20];
}
static int kern(const cfont *f, uint32_t a, uint32_t b)   /* 1/64 px */
{
	uint32_t key = (a << 16) | b;
	int lo = 0, hi = f->nk - 1;
	if (a > 0x7E || b > 0x7E) return 0;
	while (lo <= hi) {
		int m = (lo + hi) >> 1;
		uint32_t k = (uint32_t)f->kern[2 * m];
		if (k == key) return f->kern[2 * m + 1];
		if (k < key) lo = m + 1; else hi = m - 1;
	}
	return 0;
}
/* width in px of text (ls: extra px between letters, upper: draw in capitals) */
static int text_w(const cfont *f, const char *s, int ls, int upper)
{
	int w = 0, aw;
	uint32_t prev = 0;
	const uint8_t *at;
	char vis[512];
	if (!f->g) return 0;
	s = pico_bidi(s, vis, sizeof vis);   /* right-to-left letters in drawing order */
	while (*s) {
		uint32_t c = utf8_next(&s);
		if (upper) c = pico_upper(c);
		w += (int)glyph(f, c, &at, &aw)->adv + kern(f, prev, c) + ls * 64;
		prev = c;
	}
	return (w + 32) >> 6;
}
/* draw text with its baseline at y; anchor 0 start, 1 middle, 2 end; returns the width */
static int text(const cfont *f, int x, int y, const char *s, uint32_t rgb, int op, int anchor, int ls, int upper)
{
	int pen, w = text_w(f, s, ls, upper);
	uint32_t prev = 0;
	char vis[512];
	if (!f->g) return 0;
	s = pico_bidi(s, vis, sizeof vis);
	if (anchor == 1) x -= w / 2; else if (anchor == 2) x -= w;
	pen = x * 64;
	while (*s) {
		uint32_t c = utf8_next(&s);
		const cglyph *g;
		const uint8_t *at;
		int i, j, gx, aw;
		if (upper) c = pico_upper(c);
		pen += kern(f, prev, c);
		g = glyph(f, c, &at, &aw);
		gx = ((pen + 32) >> 6) + g->xo;
		for (j = 0; j < g->h; j++) {
			const uint8_t *a = at + (g->y + j) * aw + g->x;
			for (i = 0; i < g->w; i++)
				if (a[i]) { int al = (a[i] * op) >> 8; put(gx + i, y + g->yo + j, rgb, al + (al >> 7)); }
		}
		pen += (int)g->adv + ls * 64;
		prev = c;
	}
	return w;
}
/* s as it fits in max_w px: itself, or cut with an ellipsis into buf (cap bytes) */
static const char *fit_str(const cfont *f, const char *s, int max_w, char *buf, int cap)
{
	int n = (int)strlen(s);
	if (text_w(f, s, 0, 0) <= max_w || n >= cap - 4) return s;
	memcpy(buf, s, (size_t)n + 1);
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   /* whole characters */
		buf[n] = (char)0xE2; buf[n + 1] = (char)0x80; buf[n + 2] = (char)0xA6; buf[n + 3] = 0;   /* ... */
		if (text_w(f, buf, 0, 0) <= max_w) break;
	}
	return buf;
}
/* text cut to max_w px with an ellipsis */
static void text_fit(const cfont *f, int x, int y, const char *s, uint32_t rgb, int op, int max_w)
{
	char buf[192];
	text(f, x, y, fit_str(f, s, max_w, buf, sizeof buf), rgb, op, 0, 0, 0);
}
/* The bounce ticker of a text wider than its box (melonds-nano's, the home's marquee in time):
 * 1.5 s still at its start, then 30 units a second until its end shows, 1.5 s still there, back
 * the same way, over and over. How far the text moves left t seconds after the ticker started */
int pico_ui_ticker_shift(int excess, float speed, float t)
{
	float hold = 1.5f, travel, k;
	if (excess <= 0 || speed <= 0) return 0;
	travel = (float)excess / speed;
	k = 2 * (hold + travel);
	while (t >= k) t -= k;
	if (t < hold) return 0;
	if (t < hold + travel) return (int)((t - hold) * speed);
	if (t < 2 * hold + travel) return excess;
	t = (float)excess - (t - 2 * hold - travel) * speed;
	return t > 0 ? (int)t : 0;
}
static int ticker_shift(int excess, float t) { return pico_ui_ticker_shift(excess, 30.0f * g_s, t); }
/* text in a box box_w px wide: as it is when it fits, else on the ticker (t seconds since it
 * started), its hidden sides fading softly (16 units at most) */
static void text_ticker(const cfont *f, int x, int y, int box_w, const char *s, uint32_t rgb, int op, float t)
{
	int w = text_w(f, s, 0, 0), sh, fade = U(16);
	if (w <= box_w) { text(f, x, y, s, rgb, op, 0, 0, 0); return; }
	sh = ticker_shift(w - box_w, t);
	g_cx0 = x; g_cx1 = x + box_w;
	g_cfl = sh < fade ? sh : fade;
	g_cfr = w - box_w - sh < fade ? w - box_w - sh : fade;
	text(f, x - sh, y, s, rgb, op, 0, 0, 0);
	g_cx0 = g_cx1 = g_cfl = g_cfr = 0;
}
/* text over at most two lines of max_w px, broken at a space (between any two letters for a
 * script written without them), the second cut with an ellipsis; returns the lines drawn */
static int text_wrap2(const cfont *f, int x, int y, int line_h, const char *s, uint32_t rgb, int op, int max_w)
{
	char a[192];
	int n = (int)strlen(s), cut = 0, sp = 0, i;
	if (text_w(f, s, 0, 0) <= max_w || n >= (int)sizeof a) { text_fit(f, x, y, s, rgb, op, max_w); return 1; }
	for (i = 1; i <= n; i++) {	/* the longest first line that fits, preferring to end at a space */
		if (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) continue;
		memcpy(a, s, (size_t)i); a[i] = 0;
		if (text_w(f, a, 0, 0) > max_w) break;
		cut = i;
		if (s[i] == ' ') sp = i;
	}
	if (sp > cut / 2) cut = sp;
	if (!cut) { text_fit(f, x, y, s, rgb, op, max_w); return 1; }
	memcpy(a, s, (size_t)cut); a[cut] = 0;
	text(f, x, y, a, rgb, op, 0, 0, 0);
	while (s[cut] == ' ') cut++;
	text_fit(f, x, y + line_h, s + cut, rgb, op, max_w);
	return 2;
}

/* ---- the theme's art and type, looked up once per pack load -------------------------------- */
/* the settings rows: the main list, then the two subsections it opens */
enum { SET_BRIGHT, SET_VOLUME, SET_BFI, SET_THEME, SET_COLOUR, SET_BIOS, SET_RESUME, SET_BOOT_OS,
       SET_SKIP_INTRO, SET_INTRO_BG, SET_MUTE_BIOS, SET_INTRO_RESUME, SET_RESUME_BOOT, SET_RESUME_OPEN,
       SET_LANGUAGE, SET_CLOCK, SET_YEAR, SET_MONTH, SET_DAY, SET_HOUR, SET_MINUTE, SET_ZONE, SET_CLOCK12,
       SET_ABOUT, SET_N };
enum { SPAGE_MAIN, SPAGE_BIOS, SPAGE_RESUME, SPAGE_CLOCK, SPAGE_N };
/* the rows are the shared settings' (pico_settings.h), in its order */
typedef char set_ids_are_shared[(SET_BRIGHT == PS_BRIGHT && SET_BOOT_OS == PS_BOOT_OS && SET_LANGUAGE == PS_LANGUAGE &&
				 SET_CLOCK12 == PS_CLOCK12 && SET_ABOUT == PS_ABOUT) ? 1 : -1];
#define SET_ROWS 12                   /* rows in a settings list, at most, with its end */
static const signed char k_set_page[SPAGE_N][SET_ROWS] = {
	{ SET_BRIGHT, SET_VOLUME, SET_BFI, SET_THEME, SET_COLOUR, SET_LANGUAGE, SET_CLOCK, SET_BIOS, SET_RESUME,
	  SET_BOOT_OS, SET_ABOUT, -1 },
	{ SET_SKIP_INTRO, SET_INTRO_BG, SET_MUTE_BIOS, SET_INTRO_RESUME, -1 },
	{ SET_RESUME_BOOT, SET_RESUME_OPEN, -1 },
	{ SET_YEAR, SET_MONTH, SET_DAY, SET_HOUR, SET_MINUTE, SET_ZONE, SET_CLOCK12, -1 },
};
static const char *const k_set_page_name[SPAGE_N] = { "", "BIOS boot", "Game resume", "Date & Time" };
/* the row of the main list that opens a page, and the page a row opens (or -1) */
static const signed char k_page_row[SPAGE_N] = { -1, SET_BIOS, SET_RESUME, SET_CLOCK };
static int page_of_row(int item)
{
	int p;
	for (p = 1; p < SPAGE_N; p++) if (k_page_row[p] == item) return p;
	return -1;
}
static struct {
	cimg hw[NSYS], logo[NSYS], slogo[NSYS], llogo[NSYS], alogo[NSYS];
	cimg rail[3], railf[3], ic_library, ic_play, ic_set[SET_N], ic_chev, gamma, shadow, glow, ring, pillm;
	cfont brand, sub, realm, rail_t, railf_t, tag13, tag11, count, lib, button, row, rowf, info,
	      srow, srowf, sval, dname, dnum, dunit, hintc, hint, empty;
	int nthemes;
	char theme_id[NTHEMES][24], theme_label[NTHEMES][24];
	unsigned char theme_beta[NTHEMES];   /* still being finished: shown as Beta */
} A;

static void art_load(void)
{
	static const char *sys[NSYS] = { "gb", "gbc", "gba", "snes", "genesis", "sms", "gg", "sg", "nes" };
	static const char *rail[3] = { "games", "resume", "settings" };
	static const char *setic[SET_N] = { "sun", "sound", "bfi", "display", "colour", "bios", "rsm", "power",
					    "bios", "display", "mute", "rsm", "rsm", "rsm", "language", "clock",
					    "clock", "clock", "clock", "clock", "clock", "clock", "clock", "about" };
	char n[32];
	int i;
	for (i = 0; i < NSYS; i++) {
		snprintf(n, sizeof n, "hw_%s", sys[i]); A.hw[i] = img(n);
		snprintf(n, sizeof n, "logo_%s", sys[i]); A.logo[i] = img(n);
		snprintf(n, sizeof n, "slogo_%s", sys[i]); A.slogo[i] = img(n);
		snprintf(n, sizeof n, "llogo_%s", sys[i]); A.llogo[i] = img(n);
		snprintf(n, sizeof n, "alogo_%s", sys[i]); A.alogo[i] = img(n);
	}
	for (i = 0; i < 3; i++) {
		snprintf(n, sizeof n, "ic_%s", rail[i]); A.rail[i] = img(n);
		snprintf(n, sizeof n, "ic_%s_f", rail[i]); A.railf[i] = img(n);
	}
	for (i = 0; i < SET_N; i++) { snprintf(n, sizeof n, "ic_%s", setic[i]); A.ic_set[i] = img(n); }
	A.ic_chev = img("ic_chev");
	A.ic_library = img("ic_library"); A.ic_play = img("ic_play"); A.gamma = img("gamma");
	A.shadow = img("shadow"); A.glow = img("glow"); A.ring = img("hint_ring"); A.pillm = img("hint_pill");
	A.brand = font("f_brand"); A.sub = font("f_sub"); A.realm = font("f_realm");
	A.rail_t = font("f_rail"); A.railf_t = font("f_railf"); A.tag13 = font("f_tag13"); A.tag11 = font("f_tag11");
	A.count = font("f_count"); A.lib = font("f_lib"); A.button = font("f_button");
	A.row = font("f_row"); A.rowf = font("f_rowf"); A.info = font("f_info");
	font_names(&A.row, 0); font_names(&A.rowf, 1);
	A.srow = font("f_srow"); A.srowf = font("f_srowf"); A.sval = font("f_sval");
	A.dname = font("f_dname"); A.dnum = font("f_dnum"); A.dunit = font("f_dunit");
	A.hintc = font("f_hintc"); A.hint = font("f_hint"); A.empty = font("f_empty");
	A.nthemes = 0;
	{	/* the card's themes, "id|Label" a line (then "|beta" for one still being finished), in menu order */
		const cur_entry *e = find("themes", T_TEXT);
		const char *p = e ? (const char *)(s_raw + e->off) : 0, *end = p ? p + e->len : 0;
		while (p && p < end && A.nthemes < NTHEMES) {
			int k = 0, m = 0;
			while (p < end && *p != '|' && *p != '\n' && k < 23) A.theme_id[A.nthemes][k++] = *p++;
			A.theme_id[A.nthemes][k] = 0;
			if (p < end && *p == '|') p++;
			while (p < end && *p != '\n' && *p != '|' && m < 23) A.theme_label[A.nthemes][m++] = *p++;
			A.theme_label[A.nthemes][m] = 0;
			A.theme_beta[A.nthemes] = p + 5 <= end && !strncmp(p, "|beta", 5);
			while (p < end && *p != '\n') p++;
			if (p < end) p++;
			if (k) A.nthemes++;
		}
	}
}

/* ---- state ---------------------------------------------------------------------------------- */
enum { TAB_GAMES, TAB_RESUME, TAB_SETTINGS, TAB_LINK, TAB_NET };
#define NTABS (PICO_LINK ? 5 : 3)        /* the Link and Network tabs only where there is Bluetooth and Wi-Fi */
static const char *s_sysname[NSYS] = { "Game Boy", "Game Boy Color", "Game Boy Advance", "Super Nintendo",
					"Mega Drive / Genesis", "Master System", "Game Gear", "SG-1000", "NES" };

static struct {
	int ready;                        /* the state below survives from one menu run to the next */
	int tab, lib;                     /* the rail's tab; 1 while a system's library is open */
	int pick;                         /* the library is open for the Link screen to pick a game */
	int nsys, sys, sys_from;          /* the systems on the card (in console order) and the focus */
	int systype[NSYS], syscount[NSYS];
	float sys_t;                      /* 0..1, the console and the current moving to the new system */
	float dip;                        /* 1..0, the short dip of a screen change */
	int lib_sel[NSYS], lib_top[NSYS];
	int res_sel, set_sel, theme_pick, lang_pick;
	int set_page;                     /* the settings list shown: the main one or a subsection */
	int nrec, rec[NRECENT];           /* recently played, rom indices, newest first */
	int batt, chg;
	unsigned int batt_tick;
	float phase;                      /* the current's phase (radians), drifting */
	float push;                       /* phase still to add from a system move */
	int bg_key[2];                    /* the colour each still backdrop was made in, -1 none */
	int pal_colour;                   /* the colour the shaded palette was made in, -1 none */
	float badge_t;                    /* seconds the letter badge of a long list still shows */
	char badge;                       /* its letter */
	int search;                       /* the search is open */
	int sr_sys;                       /* where it looks: a system (index into the systems), -1 all */
	int sr_from_lib;                  /* opened from a library: B goes back there */
	char sq[44];                      /* the words looked for */
	int sq_n;
	int kb_r, kb_c;                   /* the key in focus */
	int sr_list;                      /* 1 while the results have the focus */
	int sr_sel, sr_top, sr_n;         /* the results: focus, first shown, count */
	int note_err;                     /* why a game did not start (RA_ERR_*), shown for note_t s */
	float note_t;
	float reveal;                     /* 0..1: the screen cascading in, top to bottom (1 at rest) */
	float leave;                      /* 0..1 while the search cascades out, before the screen behind it */
} S;

static const gba_rom_entry *s_roms;
static int s_nrom;
/* the library's tables (pico_libmem.c: thousands of games), taken once */
static char (*s_names)[128];          /* the cleaned titles */
static unsigned short *s_lib;         /* the rom indexes, system after system, card order */
static int s_libofs[NSYS];            /* where each system's run starts in s_lib */
static unsigned short *s_res;         /* the search's results */
#define LIB(s) (s_lib + s_libofs[s])

static int sys_of(int type) { int i; for (i = 0; i < S.nsys; i++) if (S.systype[i] == type) return i; return -1; }

/* the names' glyphs: what the library's names use is read, then the fonts that show names take them */
static void names_take(void)
{
	pico_names_prepare(&s_names[0][0], 128, s_nrom);
	font_names_sync(&A.row); font_names_sync(&A.rowf);
}
static int roster_build(void)
{
	int i, t, o, k = 0;
	if (!s_names) s_names = (char (*)[128])pico_lib_alloc(PICO_GAMES_MAX * 128u);
	if (!s_lib) s_lib = (unsigned short *)pico_lib_alloc(PICO_GAMES_MAX * 2u);
	if (!s_res) s_res = (unsigned short *)pico_lib_alloc(PICO_GAMES_MAX * 2u);
	if (!s_names || !s_lib || !s_res) return -1;
	S.nsys = 0;
	for (o = 0; o < NSYS; o++) {
		/* the systems in the carousel's order: Nintendo's by age, then Sega's (the NES's type number
		 * came last, so the order is a table rather than the numbers) */
		static const unsigned char k_order[NSYS] = { GBA_CONSOLE_GB, GBA_CONSOLE_GBC, GBA_CONSOLE_GBA,
			GBA_CONSOLE_NES, GBA_CONSOLE_SNES, GBA_CONSOLE_GENESIS, GBA_CONSOLE_SMS, GBA_CONSOLE_GG, GBA_CONSOLE_SG };
		int n = 0;
		t = k_order[o];
		for (i = 0; i < s_nrom; i++)
			if (s_roms[i].type == t) s_lib[k + n++] = (unsigned short)i;
		if (n) { S.systype[S.nsys] = t; S.syscount[S.nsys] = n; s_libofs[S.nsys] = k; S.nsys++; k += n; }
	}
	for (i = 0; i < s_nrom; i++) {
		char clean[128], lab[128];
		const char *l;
		gba_clean_name(s_roms[i].uname[0] ? s_roms[i].uname : s_roms[i].name, clean);
		if (!clean[0]) { strncpy(clean, s_roms[i].uname[0] ? s_roms[i].uname : s_roms[i].name, 127); clean[127] = 0; }
		l = pico_names_label(clean, lab, sizeof lab);   /* Arabic joined, right to left in drawing order */
		strncpy(s_names[i], l, 127); s_names[i][127] = 0;
	}
	names_take();
	return 0;
}

/* ---- recently played: /gammaos/recent.txt, "<type> <name>" a line, newest first ----------- */
static int rom_find(int type, const char *name)
{
	char legacy[128];
	int i;
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type == type && !strcmp(s_roms[i].name, name)) return i;
	/* a line written before names were kept whole: the old form, '_' for each character outside
	 * ASCII, when no other game's name came out the same (the next save of the list writes the
	 * whole name) */
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type == type && gba_sd_legacy_own(s_roms[i].name, legacy, (int)sizeof legacy) &&
		    !strcmp(legacy, name)) return i;
	return -1;
}
static void recent_load(void)
{
	static char buf[NRECENT * 136];
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	uint32_t n;
	char *p, *end;
	S.nrec = 0;
	if (!v || fat_open(v, "/gammaos/recent.txt", &f) != 0) return;
	n = f.size < sizeof buf - 1 ? f.size : sizeof buf - 1;
	if (fat_read(&f, 0, buf, n) != n) return;
	buf[n] = 0;
	for (p = buf, end = buf + n; p < end && S.nrec < NRECENT; ) {
		char *e = p;
		int type, r;
		while (e < end && *e != '\n') e++;
		*e = 0;
		if (e > p && e[-1] == '\r') e[-1] = 0;
		if (p[0] >= '0' && p[0] <= '7' && p[1] == ' ') {
			type = p[0] - '0';
			r = rom_find(type, p + 2);
			if (r >= 0) S.rec[S.nrec++] = r;
		}
		p = e + 1;
	}
}
static void recent_save(void)
{
	static char buf[NRECENT * 136];
	fat_vol *v = gba_sd_menu_vol();
	int i, len = 0;
	for (i = 0; i < S.nrec; i++)
		len += snprintf(buf + len, sizeof buf - (size_t)len, "%d %s\n", s_roms[S.rec[i]].type, s_roms[S.rec[i]].name);
	if (v) fat_wr_put(v, "/gammaos", "recent.txt", buf, (uint32_t)len);
}
static void recent_drop_first(void)
{
	int i;
	for (i = 1; i < S.nrec; i++) S.rec[i - 1] = S.rec[i];
	S.nrec--;
	recent_save();
}
static void recent_add(int rom)
{
	int i, k;
	for (i = 0; i < S.nrec && S.rec[i] != rom; i++) ;
	if (i == S.nrec && S.nrec < NRECENT) S.nrec++;
	if (i >= S.nrec) i = S.nrec - 1;
	for (k = i; k > 0; k--) S.rec[k] = S.rec[k - 1];
	S.rec[0] = rom;
	recent_save();
}

/* ---- the backdrop: a current of light, drawn every frame ------------------------------------- */
/* sin for the curves: a 1024-step table with linear steps between (smooth while the phase moves) */
static float s_sintab[1025];
static void sin_init(void)
{
	int i;
	/* by recurrence from one rotation, no libm: e^(i*2pi/1024) */
	float c = 0.99998117528260111f, sn = 0.0061358846491544753f, x = 1, y = 0;
	for (i = 0; i <= 1024; i++) {
		float nx;
		s_sintab[i] = y;
		nx = x * c - y * sn; y = x * sn + y * c; x = nx;
		if ((i & 63) == 63) { float r = sq(x * x + y * y); x /= r; y /= r; }   /* keep it on the circle */
	}
}
static inline float fsin(float a)
{
	float f = a * (1024.0f / 6.28318530718f);
	int i = (int)f;
	if (f < 0) i--;
	f -= (float)i;
	i &= 1023;
	return s_sintab[i] + (s_sintab[i + 1] - s_sintab[i]) * f;
}

/* The still part, made once per colour: the deep blue field, the mist at the upper right, the dark
 * edge at the foot, then the left shade (and for the library and settings the reading shade).
 * Worked out in float and dithered to 8 bits, so the slow gradients have no steps. The shades are
 * a per-column  v * k + c  that the moving current uses too. */
static uint16_t s_colk[2][1280];      /* per column: the colour that stays (of 256) */
static uint16_t s_colc[2][1280][3];   /* per column: the shade colour added (0..255 * 256) */

typedef struct { float o; uint32_t c; float a; } gstop;
static void grad(const gstop *st, int n, float p, float *r, float *g, float *b, float *a)
{
	int i;
	if (p <= st[0].o) i = 0;
	else if (p >= st[n - 1].o) i = n - 2;
	else for (i = 0; i < n - 2 && p > st[i + 1].o; i++) ;
	{
		float t = (p - st[i].o) / (st[i + 1].o - st[i].o);
		uint32_t c0 = st[i].c, c1 = st[i + 1].c;
		if (t < 0) t = 0; else if (t > 1) t = 1;
		*r = ((c0 >> 16 & 255) * (1 - t) + (c1 >> 16 & 255) * t);
		*g = ((c0 >> 8 & 255) * (1 - t) + (c1 >> 8 & 255) * t);
		*b = ((c0 & 255) * (1 - t) + (c1 & 255) * t);
		*a = st[i].a * (1 - t) + st[i + 1].a * t;
	}
}
static void shpal_build(void);
/* The column shades (left shade, and the reading shade of the library and settings) */
static void shades_build(void)
{
	static const gstop left[3] = { { 0, 0x051020, .37f }, { .49f, 0x051020, .04f }, { 1, 0x051020, 0 } };
	static const gstop read[3] = { { 0, C_SHADE, .78f }, { .52f, C_SHADE, .72f }, { 1, C_SHADE, .37f } };
	int x, k;
	for (k = 0; k < 2; k++)
		for (x = 0; x < g_W; x++) {	/* as v * kk + cc per column */
			float u = (x + .5f) / g_W, r, g, b, a, r2, g2, b2, a2, kk;
			grad(left, 3, u, &r, &g, &b, &a);
			if (k) grad(read, 3, u, &r2, &g2, &b2, &a2); else { r2 = g2 = b2 = 0; a2 = 0; }
			kk = (1 - a) * (1 - a2);
			s_colk[k][x] = (uint16_t)(kk * 256 + .5f);
			s_colc[k][x][0] = (uint16_t)((r * a * (1 - a2) + r2 * a2) * 256);
			s_colc[k][x][1] = (uint16_t)((g * a * (1 - a2) + g2 * a2) * 256);
			s_colc[k][x][2] = (uint16_t)((b * a * (1 - a2) + b2 * a2) * 256);
		}
}
/* a gradient as a table: colour (8.8 fixed) and alpha (0..256) at n even steps of p0..p1 */
typedef struct { int16_t r, g, b, a; } glut;
static void lut_fill(glut *t, int n, const gstop *st, int ns, float p0, float p1, int squared)
{
	int i;
	for (i = 0; i < n; i++) {
		float p = p0 + (p1 - p0) * i / (n - 1), r, g, b, a;
		if (squared) p = sq(p) / .62f;   /* the mist's table is indexed by the squared distance */
		grad(st, ns, p, &r, &g, &b, &a);
		t[i].r = (int16_t)(r * 128); t[i].g = (int16_t)(g * 128); t[i].b = (int16_t)(b * 128);
		t[i].a = (int16_t)(a * 256 + .5f);
	}
}
/* The still part of the backdrop for one shade, made in the theme's colour: the deep blue field,
 * the mist at the upper right, the dark edge at the foot, then the shades. Fixed point from
 * tables, dithered to 8 bits so the slow gradients have no steps. */
static void bg_build(int shade)
{
	static const gstop air[4] = { { 0, 0x030914, 1 }, { .36f, 0x081D43, 1 }, { .68f, 0x0C3C7D, 1 }, { 1, 0x081B4C, 1 } };
	static const gstop mist[3] = { { 0, 0x2664CC, .88f }, { .6f, 0x1655B0, .34f }, { 1, 0x102443, 0 } };
	static const gstop edge[3] = { { 0, 0x08182E, 0 }, { .75f, 0x05132C, .17f }, { 1, 0x020914, .80f } };
	static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
	static glut lair[1024], lmist[2048], ledge[1024];
	static int16_t pu[1280], du2[1280];
	const int16_t *m = k_colour[g_colour].m;
	uint32_t *dst = (uint32_t *)(shade ? CUR_BG1_PA : CUR_BG0_PA);
	int x, y;
	/* the field runs across and a little down: p = (u + (v - .4) * .3) / 1.09, in -0.13 .. 1.04 */
	lut_fill(lair, 1024, air, 4, -.15f, 1.05f, 0);
	lut_fill(lmist, 2048, mist, 3, 0, 1.5625f, 1);   /* indexed by the squared distance, to 1.25 */
	lut_fill(ledge, 1024, edge, 3, 0, 1, 0);
	for (x = 0; x < g_W; x++) {
		float u = (x + .5f) / g_W;
		pu[x] = (int16_t)((u / 1.09f) * 852.5f);           /* 1023 / 1.2 per unit of p */
		du2[x] = (int16_t)((u - .81f) * (u - .81f) * 1310.4f); /* 2047 / 1.5625 per unit of d^2 */
	}
	for (y = 0; y < g_H; y++) {
		float v = (y + .5f) / g_H;
		int pv = (int)((((v - .4f) * .3f) / 1.09f + .15f) * 852.5f), dv2 = (int)((v - .42f) * (v - .42f) * 1310.4f);
		const glut *e = &ledge[(y * 1023) / (g_H - 1)];
		for (x = 0; x < g_W; x++) {
			int pi = pu[x] + pv, di = du2[x] + dv2, r, g, b, o[3], i, dth = (bayer[(y & 3) * 4 + (x & 3)] * 8) - 60;
			const glut *f, *mi;
			pi = pi < 0 ? 0 : pi > 1023 ? 1023 : pi;
			di = di > 2047 ? 2047 : di;
			f = &lair[pi]; mi = &lmist[di];
			r = f->r + (((mi->r - f->r) * mi->a) >> 8);
			g = f->g + (((mi->g - f->g) * mi->a) >> 8);
			b = f->b + (((mi->b - f->b) * mi->a) >> 8);
			r += ((e->r - r) * e->a) >> 8; g += ((e->g - g) * e->a) >> 8; b += ((e->b - b) * e->a) >> 8;
			/* the shades of this column (r, g, b are 7 bits of fraction here) */
			r = ((r * s_colk[shade][x]) >> 8) + s_colc[shade][x][0] / 2;
			g = ((g * s_colk[shade][x]) >> 8) + s_colc[shade][x][1] / 2;
			b = ((b * s_colk[shade][x]) >> 8) + s_colc[shade][x][2] / 2;
			if (g_colour) {
				o[0] = (m[0] * r + m[1] * g + m[2] * b) >> 10;
				o[1] = (m[3] * r + m[4] * g + m[5] * b) >> 10;
				o[2] = (m[6] * r + m[7] * g + m[8] * b) >> 10;
			} else { o[0] = r; o[1] = g; o[2] = b; }
			for (i = 0; i < 3; i++) { int t = (o[i] + 64 + dth) >> 7; o[i] = t < 0 ? 0 : t > 255 ? 255 : t; }
			dst[y * g_W + x] = 0xFF000000u | (uint32_t)(o[0] << 16 | o[1] << 8 | o[2]);
		}
	}
	S.bg_key[shade] = g_colour;
}

/* The moving current: the heights of its curves at each column, from one shared fold. edge runs
 * across the ribbon; the phase drifts. y in px. */
#define W_STRATA  38
#define W_THREADS 64
static float s_A[1281], s_M[1281], s_Ps[1281], s_Pc[1281];
static void wave_columns(float ph)
{
	int x;
	for (x = 0; x <= g_W; x++) {
		float t = (float)x / g_W;
		s_A[x] = (.71f - .30f * t + .086f * fsin(2.85f * t + ph * .13f - .70f) + .047f * fsin(5.7f * t - ph * .11f + 1.6f)) * g_H;
		s_M[x] = (.07f + .15f * fsin(3.8f * t - .42f + ph * .12f)) * g_H;
		s_Ps[x] = .026f * g_H * fsin(7.2f * t + ph * .10f);              /* the small ripple, split by */
		s_Pc[x] = .026f * g_H * fsin(7.2f * t + ph * .10f + 1.5707963f); /* sin(a + b) = ... */
	}
}
/* the height of the curve at edge e: ce, se the cos and sin of 1.8 e (the ripple's phase along it) */
static inline float wave_y(int x, float e, float ce, float se) { return s_A[x] + e * s_M[x] + s_Ps[x] * ce + s_Pc[x] * se; }

/* rgb (already in the theme's colour) into column x as it shows under the shades */
static inline uint32_t shaded(uint32_t rgb, int x, int k)
{
	uint32_t kk = s_colk[k][x];
	uint32_t r = ((rgb >> 16 & 255) * kk + s_colc[k][x][0]) >> 8, g = ((rgb >> 8 & 255) * kk + s_colc[k][x][1]) >> 8,
		 b = ((rgb & 255) * kk + s_colc[k][x][2]) >> 8;
	return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}
/* rgb over an opaque d at a 0..256 (the frame: no alpha to carry) */
static inline uint32_t mixo(uint32_t d, uint32_t rgb, int a)
{
	uint32_t rb = d & 0xFF00FFu, g = d & 0x00FF00u;
	rb += (((rgb & 0xFF00FFu) - rb) * (uint32_t)a) >> 8;
	g += (((rgb & 0x00FF00u) - g) * (uint32_t)a) >> 8;
	return 0xFF000000u | (rb & 0xFF00FFu) | (g & 0x00FF00u);
}
/* rows [y0, y1) of column x (y0 <= y1, in 1/256 px) at alpha a 0..256, the end rows by their part */
static void span(int x, int y0, int y1, uint32_t rgb, int a)
{
	uint32_t *d;
	int ya, yb, y;
	if (y0 < 0) y0 = 0;
	if (y1 > g_H << 8) y1 = g_H << 8;
	if (y1 <= y0 || a <= 0) return;
	ya = y0 >> 8; yb = (y1 - 1) >> 8;
	d = g_fb + ya * g_W + x;
	if (ya == yb) { *d = mixo(*d, rgb, (a * (y1 - y0)) >> 8); return; }
	*d = mixo(*d, rgb, (a * (256 - (y0 & 255))) >> 8);
	for (y = ya + 1, d += g_W; y < yb; y++, d += g_W) *d = mixo(*d, rgb, a);
	*d = mixo(*d, rgb, (a * (y1 - (yb << 8))) >> 8);
}
/* a thin line across column x from height ya to yb (1/256 px), th px thick, at op 0..256: a line
 * that moves less than a pixel across the column is two rows split by where it sits (its ink is th
 * of a pixel); a steeper one shares the same ink over the rows it crosses */
static void hair(int x, int ya, int yb, int th, uint32_t rgb, int op)
{
	int lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya, dy = hi - lo;
	if (dy < 256) {
		int c = ((lo + hi) >> 1) - 128, y = c >> 8, f = c & 255, ink = (op * th) >> 8;   /* the row above the centre */
		uint32_t *d;
		if (y < -1 || y >= g_H) return;
		d = g_fb + y * g_W + x;
		if (y >= 0) *d = mixo(*d, rgb, (ink * (256 - f)) >> 8);
		if (y + 1 < g_H) d[g_W] = mixo(d[g_W], rgb, (ink * f) >> 8);
	} else
		span(x, lo - th / 2, hi + th / 2, rgb, ((op * th) >> 8) * dy / (dy + th));
}
extern unsigned g_perf[8];   /* oem diag perf_us: here 0 backdrop copy, 1 bands, 2 threads, 3 menu, 4 canvas */
static unsigned s_pt;
static void perf_mark(int i) { unsigned n = gpt4_get_current_tick(); if (i != 4) g_perf[i] = n - s_pt; s_pt = n; }
static uint32_t s_shpal[2][1280][5];   /* the current's five colours under each column's shades */
static void shpal_build(void)
{
	static const uint32_t cols[5] = { 0x1141A5, 0x2A65BD, 0x72C5ED, 0x4E80E1, 0xBDDFFF };
	int k, x, n;
	for (n = 0; n < 5; n++) {
		uint32_t c = recolour(cols[n]);
		for (k = 0; k < 2; k++) for (x = 0; x < g_W; x++) s_shpal[k][x][n] = shaded(c, x, k);
	}
}
static void wave_draw(float ph, int shade)
{
	static const float crest[4][3] = { { -.62f, .39f, 1.2f }, { .32f, .26f, .7f }, { 1.015f, .60f, 1.5f }, { 1.03f, .15f, 3.1f } };
	static int yb[W_STRATA + 1][1281];
	static int yl[1281];
	int x, n;
	wave_columns(ph);
	/* the strata, the translucent sheet of the current */
	for (n = 0; n <= W_STRATA; n++) {
		float e = -.62f + n * 1.64f / W_STRATA, se = fsin(1.8f * e), ce = fsin(1.8f * e + 1.5707963f);
		for (x = 0; x < g_W; x++) yb[n][x] = (int)(wave_y(x, e, ce, se) * 256);
	}
	for (n = 0; n < W_STRATA; n++) {
		float t = (float)n / W_STRATA, sp = fsin(3.14159265f * t), op = (.15f + .49f * sp * sq(sp) * sq(sq(sq(sp)))) * .83f;
		int ci = t < .35f ? 0 : t < .57f ? 1 : t < .81f ? 2 : 3, a = (int)(op * 256);
		for (x = 0; x < g_W; x++) {
			int y0 = yb[n][x], y1 = yb[n + 1][x];
			if (y1 < y0) { int tt = y0; y0 = y1; y1 = tt; }
			span(x, y0, y1, s_shpal[shade][x][ci], a);
		}
	}
	perf_mark(1);
	/* the fine threads along it, then the bright crests */
	for (n = 0; n < W_THREADS + 4; n++) {
		float e, op, th;
		int c;
		if (n < W_THREADS) {
			float v = (float)n / (W_THREADS - 1), sv = fsin(3.14159265f * v);
			e = -.58f + v * 1.56f;
			op = (.05f + .25f * sv * sv * sv) * .88f;
			th = (n % 4 ? .48f : .85f) * g_s;
			c = n % 5 ? 2 : 4;
		} else {
			e = crest[n - W_THREADS][0]; op = crest[n - W_THREADS][1]; th = crest[n - W_THREADS][2] * g_s;
			c = 4;
		}
		if (op * th < .03f) continue;   /* the faintest threads at the ribbon's edges: no visible ink */
		{
			float se = fsin(1.8f * e), ce = fsin(1.8f * e + 1.5707963f);
			for (x = 0; x <= g_W; x++) yl[x] = (int)(wave_y(x, e, ce, se) * 256);
		}
		{
			int a = (int)(op * 256), t = (int)(th * 256);
			for (x = 0; x < g_W; x++) hair(x, yl[x], yl[x + 1], t, s_shpal[shade][x][c], a);
		}
	}
}
/* the backdrop into the frame */
static void backdrop(int shade)
{
	if (S.pal_colour != g_colour) { shades_build(); shpal_build(); S.pal_colour = g_colour; }
	if (S.bg_key[shade] != g_colour) bg_build(shade);
	s_pt = gpt4_get_current_tick();
	memcpy(g_fb, (const void *)(shade ? CUR_BG1_PA : CUR_BG0_PA), (size_t)g_W * g_H * 4);
	perf_mark(0);
	wave_draw(S.phase, shade);
	perf_mark(2);
}

/* ---- pieces of the layout --------------------------------------------------------------------- */
static float s_uw, s_p;               /* the layout's width and side margin in units */

static void hline(int x0, int x1, int y, uint32_t rgb, int op) { fill(x0, y, x1 - x0, 1, rgb, op); }

static void header(const char *realm)
{
	int x = U(s_p), w, r, chg = S.chg, pct = S.batt, clock_x = U(s_uw - s_p);
	draw_mask(&A.gamma, x, U(28), C_INK, 256);
	w = text(&A.brand, x + U(31), U(47), "GammaOS", C_INK, 256, 0, 0, 0);
	w = x + U(31) + w + U(8);
	w += text(&A.sub, w, U(47), "Pico", C_MUTED, 256, 0, 0, 0);
	fill(w + U(14), U(31), 1, U(17), C_MUTED, 115);
	text(&A.realm, w + U(32), U(46), tr(realm), C_MUTED, 256, 0, 0, 0);
	if (pct >= 0) {	/* the battery: an outline, its level, the percent */
		char t[8];
		int tx = U(s_uw - s_p), bx;
		snprintf(t, sizeof t, "%d%%", pct);
		w = text(&A.realm, tx, U(46), t, C_INK, 256, 2, 0, 0);
		bx = tx - w - U(37);
		r = U(1.25f) > 0 ? U(1.25f) : 1;
		fill(bx, U(34), U(25), r, C_MUTED, 256); fill(bx, U(45) - r, U(25), r, C_MUTED, 256);
		fill(bx, U(34), r, U(11), C_MUTED, 256); fill(bx + U(25) - r, U(34), r, U(11), C_MUTED, 256);
		fill(bx + U(3), U(37), (U(19) * pct) / 100, U(5), chg ? C_ACCENT : C_MUTED, 256);
		fill(bx + U(26), U(37), U(2), U(5), C_MUTED, 256);
		clock_x = bx - U(22);
	}
	{	/* the day and the time, left of the battery */
		char d[48];
		long long now = pico_clock_local();
		pico_clock_time_text(now, d, sizeof d);
		w = text(&A.realm, clock_x, U(46), d, C_INK, 256, 2, 0, 0);
		fill(clock_x - w - U(14), U(31), 1, U(17), C_MUTED, 115);
		pico_clock_date_text(now, d, sizeof d);
		text(&A.realm, clock_x - w - U(28), U(46), d, C_MUTED, 256, 2, 0, 0);
	}
}
/* the button hints along the bottom: codes ("A", "B", "LR") and labels */
static void hints(const char *const *codes, const char *const *labels, int n)
{
	int i, xx = U(s_p), cy = U(720 - 27);
	for (i = 0; i < n; i++) {
		int one = codes[i][1] == 0, rad = one ? U(8.3f) : U(13.5f);
		if (one) draw_mask(&A.ring, xx, cy - A.ring.h / 2, C_MUTED, 256);
		else draw_mask(&A.pillm, xx, U(720 - 35), C_MUTED, 256);
		text(&A.hintc, xx + rad, U(720 - 22.9f), codes[i], C_INK, 256, 1, 0, 0);
		xx += 2 * rad + U(8);
		xx += text(&A.hint, xx, U(720 - 22), tr(labels[i]), C_MUTED, 256, 0, 0, 0) + U(13);
	}
}
static void rail(void)
{
	static const char *label[5] = { "Games", "Resume", "Settings", "Link", "Network" };
	float pitch = s_uw * .116f < 148 ? s_uw * .116f : 148, yy = 119, x = s_p;
	int i;
	for (i = 0; i < NTABS; i++) {
		int f = i == S.tab;
		float need = 42 + (float)text_w(&A.railf_t, tr(label[i]), 0, 0) / g_s + 22;   /* a long word pushes the next tab on */
		if (f) draw_mask(&A.glow, U(x + 20) - A.glow.w / 2, U(yy + 15) - A.glow.h / 2, C_ACCENT, 256);
		if (i == TAB_LINK) {	/* no picture of its own in the pack: two links of a chain, of the hint ring */
			int ry = U(yy + 15) - A.ring.h / 2 + (f ? 0 : U(2.5f));
			draw_mask(&A.ring, U(x + 2), ry, f ? C_INK : C_MUTED, 256);
			draw_mask(&A.ring, U(x + 2) + A.ring.w * 2 / 3, ry, f ? C_INK : C_MUTED, 256);
		} else if (i == TAB_NET) {	/* none either: four signal bars rising */
			int b, base = U(yy + 26) + (f ? 0 : U(2.5f));
			for (b = 0; b < 4; b++) {
				int h = U(6 + 5.5f * b);
				fill(U(x + 4 + 7.5f * b), base - h, U(4.5f), h, f ? C_INK : C_MUTED, 256);
			}
		} else
		draw_mask(f ? &A.railf[i] : &A.rail[i], U(x), U(yy) + (f ? 0 : U(2.5f)), f ? C_INK : C_MUTED, 256);
		text(f ? &A.railf_t : &A.rail_t, U(x + 42), U(yy + 23), tr(label[i]), f ? C_INK : C_MUTED, 256, 0, 0, 0);
		if (f) fill(U(x), U(yy + 41) - 1, U(77), U(1.8f) > 1 ? U(1.8f) : 2, C_ACCENT, 256);
		x += need > pitch ? need : pitch;
	}
}
/* list rows: the focused one gets the accent wash and bar */
static void rows(float x, float y, float w, float pitch, const char *const *items, const char *const *values,
		 int n, int focus, const cfont *fn, const cfont *ff, const cfont *fv)
{
	int i, xi = U(x), wi = U(w), ph = U(pitch);
	for (i = 0; i < n; i++) {
		int yy = U(y + i * pitch), sel = i == focus;
		if (sel) {
			int k;
			for (k = 0; k < wi; k++) {   /* accent 16 % fading to 2.5 % at 86 %, then out */
				int t = k * 1000 / wi, a = t < 860 ? 41 - (35 * t) / 860 : 6 - (6 * (t - 860)) / 140;
				fill(xi + k, yy, 1, ph - U(2), C_ACCENT, a);
			}
			fill(xi, yy + U(8), U(2.5f) > 2 ? U(2.5f) : 2, ph - U(18), C_ACCENT, 256);
		}
		{	/* the value at the right, at most 55 % of the row; the name in what is left */
			char vb[96];
			const char *v = values && values[i] && values[i][0] ? fit_str(fv, values[i], wi * 55 / 100, vb, sizeof vb) : 0;
			int vw = v ? text_w(fv, v, 0, 0) + U(16) : values ? U(40) : 0;
			text_fit(sel ? ff : fn, xi + U(16), yy + (int)(ph * .61f), items[i], sel ? C_INK : C_MUTED, 256,
				 wi - U(28) - vw);
			if (v) text(fv, xi + wi - U(12), yy + (int)(ph * .61f), v, sel ? C_INK : C_MUTED, 256, 2, 0, 0);
		}
		hline(xi + U(14), xi + wi, yy + ph - 1, C_MUTED, 49);
	}
}
static void button(float x, float y, const char *label, const cimg *icon)
{
	int tw, w;
	label = tr(label);
	tw = text_w(&A.button, label, 0, 0); w = U(38) + tw + U(20);
	pill_fill(U(x), U(y), w, U(41), C_INK, 256);
	if (icon) draw_mask(icon, U(x + 12), U(y + 11), C_BTN_INK, 256);
	text(&A.button, U(x + 38), U(y + 26), label, C_BTN_INK, 256, 0, 0, 0);
}
/* a game's box art fitted into the art box, or the console when it has none */
static void art(int rom, float rx, float ry, float rw, float rh)
{
	unsigned int sw, sh;
	const unsigned char *px = gba_menu_boxart(rom, &sw, &sh);
	int bw = U(rw), bh = U(rh), dw, dh;
	if (px && sw && sh) {
		if (sw * (unsigned)bh > sh * (unsigned)bw) { dw = bw; dh = (int)(sh * (unsigned)bw / sw); }
		else { dh = bh; dw = (int)(sw * (unsigned)bh / sh); }
		draw_art(px, (int)sw, (int)sh, U(rx) + (bw - dw) / 2, U(ry) + (bh - dh) / 2, dw, dh);
	} else {	/* no art: the system's mark in a soft frame */
		const cimg *l = &A.alogo[s_roms[rom].type];
		int fx = U(rx), fy = U(ry), t = 1;
		fill(fx, fy, bw, bh, C_SHADE, 110);
		fill(fx, fy, bw, t, C_MUTED, 60); fill(fx, fy + bh - t, bw, t, C_MUTED, 60);
		fill(fx, fy, t, bh, C_MUTED, 60); fill(fx + bw - t, fy, t, bh, C_MUTED, 60);
		draw_mask(l, fx + (bw - l->w) / 2, fy + (bh - l->h) / 2, C_MUTED, 200);
	}
}

/* ---- the screens ------------------------------------------------------------------------------- */
static float ease(float t) { return t >= 1 ? 1 : 1 - (1 - t) * (1 - t) * (1 - t); }

/* the stage: a soft shadow, then the console coming forward from the current (while a move runs
 * the old one leaves as the new one arrives); into the menu layer at rest, into the frame while
 * moving so the layer is not drawn again every frame */
static void stage_draw(void)
{
	float hx = s_uw * .525f, hy = 720 * .31f, hw = s_uw * .45f, hh = 720 * .40f, m = ease(S.sys_t);
	int cur = S.systype[S.sys], old = S.systype[S.sys_from];
	draw_mask(&A.shadow, U(hx + hw * .51f) - A.shadow.w / 2, U(hy + hh * .98f) - A.shadow.h / 2, C_SHADOW, 256);
	if (m < 1 && old != cur) {
		const cimg *h = &A.hw[old];
		draw_img(h, U(hx + 12 * m) + (U(hw) - h->w) / 2, U(hy) + (U(hh) - h->h) / 2, (int)((1 - m) * 256));
	}
	{
		const cimg *h = &A.hw[cur];
		draw_img(h, U(hx + 20 * (1 - m)) + (U(hw) - h->w) / 2, U(hy) + (U(hh) - h->h) / 2, old != cur && m < 1 ? (int)(m * 256) : 256);
	}
}
static void home_games(void)
{
	float x = s_p + 9, yy = 720 * .375f, base = 720 - 101, span;
	int cur = S.systype[S.sys], i, cx, w;
	char t[64];
	if (S.sys_t >= 1) stage_draw();   /* at rest the consoles are part of the menu layer */
	/* the system, its count, the way in */
	text(&A.tag13, U(x), U(yy), tr("Game systems"), C_MUTED, 256, 0, U(2), 1);
	draw_mask(&A.logo[cur], U(x), U(yy + 25 + 41.5f) - A.logo[cur].h / 2, C_INK, 256);
	snprintf(t, sizeof t, tr_n(S.syscount[S.sys], "%d game", "%d games"), S.syscount[S.sys]);
	w = text(&A.count, U(x), U(yy + 154), t, C_INK, 256, 0, 0, 0);
	cx = U(x) + w + U(14);
	disc((float)cx, U(yy + 148) + .0f, U(2) + .0f, C_MUTED, 256);
	text(&A.lib, cx + U(14), U(yy + 154), tr("Library"), C_MUTED, 256, 0, 0, 0);
	button(x, yy + 190, "Open library", &A.ic_library);
	/* the systems strip */
	hline(U(s_p), U(s_uw - s_p), U(base - 27), C_MUTED, 72);
	text(&A.tag11, U(s_p), U(base + 4), tr("Systems"), C_MUTED, 256, 0, U(2), 1);
	span = (s_uw - 2 * s_p - 121) / (S.nsys > 4 ? S.nsys : 4);
	if (span > 168) span = 168;
	for (i = 0; i < S.nsys; i++) {
		const cimg *l = &A.slogo[S.systype[i]];
		int xx = U(s_p + 118 + i * span);
		draw_mask(l, xx, U(base - 16 + 16.5f) - l->h / 2, C_INK, i == S.sys ? 256 : 133);
		if (i == S.sys) fill(xx, U(base + 26), l->w, U(1.8f) > 1 ? U(1.8f) : 2, C_ACCENT, 256);
	}
}
/* under a game's box art: how long it has been played and when it was last */
static void play_lines(int rom, float cx, float y)
{
	unsigned int secs;
	long long last;
	char d[64], t[96];
	if (!pico_stats_get(s_roms[rom].type, s_roms[rom].name, &secs, &last) || !last) return;
	if (secs) {	/* a launch that never got going has no play time to show */
		if (secs >= 3600) snprintf(d, sizeof d, tr("%d h %02d min"), (int)(secs / 3600), (int)(secs / 60 % 60));
		else snprintf(d, sizeof d, tr("%d min"), (int)((secs + 59) / 60));   /* a short go counts as a minute */
		snprintf(t, sizeof t, tr("Played %s"), d);
		text(&A.info, U(cx), U(y), t, C_MUTED, 256, 1, 0, 0);
		y += 24;
	}
	{	/* today, yesterday or the day, in the time zone set */
		long long now = pico_clock_local(), then = last + (long long)pico_clock_zone() * 900;
		long long days = now / 86400 - then / 86400;
		if (days == 0) snprintf(d, sizeof d, "%s", tr("today"));
		else if (days == 1) snprintf(d, sizeof d, "%s", tr("yesterday"));
		else pico_clock_date_text(then, d, sizeof d);
	}
	snprintf(t, sizeof t, tr("Last played %s"), d);
	text(&A.info, U(cx), U(y), t, C_MUTED, 256, 1, 0, 0);
}
static const char *s_rowtxt[LIB_ROWS];
static const char *s_rowval[LIB_ROWS];
/* A long list's place: a thin track down its right edge with the part in view, and under the
 * list the focused row's number of the whole ("1204 / 3012"). Nothing for a list that fits. */
static void list_marks(float x, float y, float w, int n, int top, int sel)
{
	float th = LIB_ROWS * 51.f - 8, tw = 2.4f, bar;
	char t[32];
	if (n <= LIB_ROWS) return;
	bar = th * LIB_ROWS / n;
	if (bar < 22) bar = 22;
	fill(U(x + w + 9), U(y + 4), U(tw) > 1 ? U(tw) : 2, U(th), C_MUTED, 46);
	fill(U(x + w + 9), U(y + 4 + (th - bar) * top / (n - LIB_ROWS)), U(tw) > 1 ? U(tw) : 2, U(bar), C_ACCENT, 256);
	snprintf(t, sizeof t, "%d / %d", sel + 1, n);
	text(&A.info, U(x + w), U(y + LIB_ROWS * 51 + 27), t, C_MUTED, 256, 2, 0, 0);
}
/* The letter a fast move or a jump lands on, large over the list for a moment (S.badge_t). */
/* the letter a name files under in a long list: its first letter without accents, capital, or '#'
 * for a name that starts with a digit or a letter of another script (sorted after Z) */
static char name_letter(const char *s)
{
	while (*s) {
		uint32_t c = pico_fold_cp(pico_utf8_next(&s));
		if (c >= 'a' && c <= 'z') return (char)(c - 32);
		if (c >= 'A' && c <= 'Z') return (char)c;
		if (c >= '0' && c <= '9') return '#';
		if ((c >= 0x370 && c <= 0x1FFF) || (c >= 0x3040 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
		    (c >= 0xFB1D && c <= 0xFEFF) || (c >= 0xFF21 && c <= 0xFF5A) || c >= 0x20000)
			return '#';   /* a letter of another script */
	}
	return '#';
}
static void letter_badge(float cx, float cy)
{
	char t[2];
	float r = 44;
	if (S.badge_t <= 0) return;
	t[0] = S.badge; t[1] = 0;
	disc(U(cx) + .0f, U(cy) + .0f, U(r + 2) + .0f, C_ACCENT, 220);
	disc(U(cx) + .0f, U(cy) + .0f, U(r) + .0f, C_SHADE, 236);
	text(&A.dname, U(cx), U(cy + 11), t, C_INK, 256, 1, 0, 0);
}
static void game_list(const unsigned short *idx, int n, int sel, int top, int title_y, const char *title)
{
	float yy = 720 * .36f, lw = s_uw * .45f, rw = s_uw * .34f < 393 ? s_uw * .34f : 393, rh = 311;
	float rx = s_uw - s_p - rw, ry = 720 * .30f;
	int i, k = n - top < LIB_ROWS ? n - top : LIB_ROWS;
	if (title) text(&A.tag13, U(s_p), U(title_y), tr(title), C_MUTED, 256, 0, U(2), 1);
	for (i = 0; i < k; i++) s_rowtxt[i] = s_names[idx[top + i]];
	rows(s_p, yy, lw, 51, s_rowtxt, 0, k, sel - top, &A.row, &A.rowf, 0);
	list_marks(s_p, yy, lw, n, top, sel);
	if (n) {
		int rom = idx[sel];
		art(rom, rx, ry, rw, rh);
		text(&A.info, U(rx + rw / 2), U(ry + rh + 29), tr(s_sysname[s_roms[rom].type]), C_MUTED, 256, 1, 0, 0);
		play_lines(rom, rx + rw / 2, ry + rh + 60);
	}
	letter_badge(s_p + lw / 2, yy + LIB_ROWS * 51 / 2);
}
static void home_resume(void)
{
	unsigned short idx[NRECENT];
	int i;
	for (i = 0; i < S.nrec; i++) idx[i] = (unsigned short)S.rec[i];
	if (!S.nrec) {
		text(&A.tag13, U(s_p), U(720 * .36f - 22), tr("Recently played"), C_MUTED, 256, 0, U(2), 1);
		text(&A.empty, U(s_p), U(720 * .36f + 32), tr("The games you play appear here."), C_MUTED, 256, 0, 0, 0);
		return;
	}
	game_list(idx, S.nrec, S.res_sel, S.res_sel >= LIB_ROWS ? S.res_sel - LIB_ROWS + 1 : 0,
		  (int)(720 * .36f - 22), "Recently played");
}
/* the saved form of theme i: 0 for the first, the hash of its name for the others */
static unsigned int theme_hash(int i) { return snes_hash(A.theme_id[i < 0 ? 0 : i]); }
/* a row's value: the theme's own picks here, the rest as every theme shows them (pico_settings.c) */
static void set_value(int i, char *buf, int n, int *num)
{
	*num = -1;
	switch (i) {
	case SET_THEME:
		if (S.theme_pick >= A.nthemes) buf[0] = 0;
		else if (A.theme_beta[S.theme_pick]) snprintf(buf, n, tr("%s (Beta)"), tr(A.theme_label[S.theme_pick]));
		else snprintf(buf, n, "%s", tr(A.theme_label[S.theme_pick]));
		break;
	case SET_COLOUR:   snprintf(buf, n, "%s", tr(k_colour[g_colour].name)); break;
	case SET_LANGUAGE: snprintf(buf, n, "%s", pico_lang_name(S.lang_pick)); break;
	case SET_BOOT_OS:  snprintf(buf, n, "%s", tr("Open")); break;
	default:           pico_set_value(i, buf, n, num); break;
	}
}
static int set_count(int page) { int n = 0; while (n < SET_ROWS && k_set_page[page][n] >= 0) n++; return n; }
/* the row of the main list that holds a setting */
static int set_row_of(int item) { int n = 0; while (n < SET_ROWS && k_set_page[SPAGE_MAIN][n] != item) n++; return n < SET_ROWS ? n : 0; }
static int set_item(void) { return k_set_page[S.set_page][S.set_sel]; }
static void home_settings(void)
{
	static char vals[SET_ROWS][64];
	const char *names[SET_ROWS], *vp[SET_ROWS];
	float top = 720 * .315f, xx = s_uw * .46f, lw = s_uw - s_p - xx, mid = top + 35, sw = s_uw * .255f, pitch;
	int i, num, n = set_count(S.set_page), it = set_item();
	pitch = (720 - 62 - top) / n;
	if (pitch > 57.6f) pitch = 57.6f;
	for (i = 0; i < n; i++) {
		int k = k_set_page[S.set_page][i];
		names[i] = tr(pico_set_name(k));
		set_value(k, vals[i], sizeof vals[i], &num); vp[i] = vals[i];
	}
	if (S.set_page) text(&A.tag13, U(xx + 16), U(top - 22), tr(k_set_page_name[S.set_page]), C_MUTED, 256, 0, U(2), 1);
	rows(xx, top, lw, pitch, names, vp, n, S.set_sel, &A.srow, &A.srowf, &A.sval);
	for (i = 0; i < n; i++) {	/* the rows that open a subsection: a chevron where the value goes */
		int k = k_set_page[S.set_page][i];
		if (page_of_row(k) > 0)
			draw_mask(&A.ic_chev, U(xx + lw - 12) - A.ic_chev.w, U(top + i * pitch) + (U(pitch) - A.ic_chev.h) / 2,
				  i == S.set_sel ? C_INK : C_MUTED, 256);
	}
	/* the focused setting, large on the left */
	if (sw > 270) sw = 270;
	draw_mask(&A.ic_set[it], U(s_p), U(mid), C_ACCENT, 256);
	text_fit(&A.dname, U(s_p), U(mid + 93), tr(pico_set_name(it)), C_INK, 256, U(xx - s_p - 20));
	set_value(it, vals[0], sizeof vals[0], &num);
	if (num >= 0) {
		char t[8];
		int w;
		snprintf(t, sizeof t, "%d", num);
		w = text(&A.dnum, U(s_p), U(mid + 141), t, C_INK, 256, 0, 0, 0);
		text(&A.dunit, U(s_p) + w + U(8), U(mid + 139), "%", C_MUTED, 256, 0, 0, 0);
		fill(U(s_p), U(mid + 176), U(sw), U(3), C_MUTED, 77);
		fill(U(s_p), U(mid + 176), U(sw) * num / 100, U(3), C_ACCENT, 256);
		disc(U(s_p) + U(sw) * num / 100 + .0f, U(mid + 177.5f) + .0f, U(5.3f) + .0f, C_INK, 256);
	} else if (it == SET_CLOCK) {	/* the time, large, and the day under it */
		char t[48];
		long long now = pico_clock_local();
		pico_clock_time_text(now, t, sizeof t);
		text_fit(&A.dname, U(s_p), U(mid + 141), t, C_INK, 256, U(xx - s_p - 20));
		pico_clock_date_text(now, t, sizeof t);
		text_fit(&A.lib, U(s_p), U(mid + 178), t, C_MUTED, 256, U(xx - s_p - 20));
	} else if (it == SET_ABOUT) {	/* the name, its version, the build and its warning */
		char t[96];
		text_fit(&A.dname, U(s_p), U(mid + 141), "GammaOS Pico", C_INK, 256, U(xx - s_p - 20));
		snprintf(t, sizeof t, tr("Version %s"), pico_version());
		text_fit(&A.lib, U(s_p), U(mid + 178), t, C_INK, 256, U(xx - s_p - 20));
		snprintf(t, sizeof t, tr("Build %s, %s"), pico_commit(), pico_build_day());
		text_fit(&A.info, U(s_p), U(mid + 206), t, C_MUTED, 256, U(xx - s_p - 20));
		text_wrap2(&A.info, U(s_p), U(mid + 240), U(21), tr("Experimental software, provided as it is, with no warranty"),
			   C_MUTED, 256, U(xx - s_p - 20));
	} else if (it == SET_BOOT_OS || it == SET_BIOS || it == SET_RESUME) {
		text_wrap2(&A.lib, U(s_p), U(mid + 141), U(27), tr(pico_set_about(it)), C_MUTED, 256, U(xx - s_p - 20));
	} else {
		text_fit(&A.dname, U(s_p), U(mid + 141), vals[0], C_INK, 256, U(xx - s_p - 20));
		if ((it == SET_THEME && A.nthemes && theme_hash(S.theme_pick) != gba_menu_theme()) ||
		    (it == SET_LANGUAGE && S.lang_pick != pico_lang()))
			text_wrap2(&A.lib, U(s_p), U(mid + 178), U(27), tr("Press A to apply"), C_MUTED, 256, U(xx - s_p - 20));
		else if (pico_set_about(it))
			text_wrap2(&A.lib, U(s_p), U(mid + 178), U(27), tr(pico_set_about(it)), C_MUTED, 256, U(xx - s_p - 20));
	}
}
#if PICO_LINK
static int link_paint(const struct lui_progress *p);   /* the start of a session (below) */
/* The Link tab: what the shared flow (pico_link_ui.c) shows, in this theme's look: the screen's
 * name large on the left with the game or console under it and what is going on, its list on the
 * right as the settings' rows. */
static void home_link(void)
{
	struct lui_view v;
	float top = 720 * .315f, xx = s_uw * .46f, lw = s_uw - s_p - xx, mid = top + 35, pitch = 57.6f, lx;
	int ly;
	lui_view(&v);
	if (v.screen == LUI_CLOSED) return;
	lx = xx - s_p - 20;
	ly = U(mid + 20) - A.ring.h / 2;
	draw_mask(&A.ring, U(s_p), ly, C_ACCENT, 256);
	draw_mask(&A.ring, U(s_p) + A.ring.w * 2 / 3, ly, C_ACCENT, 256);
	text_fit(&A.dname, U(s_p), U(mid + 93), v.title ? v.title : "", C_INK, 256, U(lx));
	if (v.subtitle) text_fit(&A.lib, U(s_p), U(mid + 135), v.subtitle, C_INK, 256, U(lx));
	if (v.status) text_wrap2(&A.lib, U(s_p), U(mid + (v.subtitle ? 178 : 141)), U(27), v.status, C_MUTED, 256, U(lx));
	if (v.total) {	/* how far a file is, as the start's bar */
		int by = U(mid + (v.subtitle ? 178 : 141) + 2 * 27 + 12), bw = U(lx);
		fill(U(s_p), by, bw, U(4), C_MUTED, 64);
		fill(U(s_p), by, (int)((unsigned long long)bw * (v.done < v.total ? v.done : v.total) / v.total), U(4), C_ACCENT, 256);
	}
	if (v.nrows) rows(xx, top, lw, pitch, v.rows, v.values, v.nrows, v.sel, &A.srow, &A.srowf, &A.sval);
}
#endif
#if PICO_LINK
/* The Network tab: what the shared flow (pico_net_ui.c) shows, as the Link tab does; its keyboard
 * as the search's, with the field on top. */
static void net_keyboard(const struct nui_view *v)
{
	const struct nui_kb *k = &v->kb;
	float kx = s_p, kw = s_uw - 2 * s_p, kp = kw / 10, fy = 206, ky = 300;	/* below the tabs */
	int r, c;
	text(&A.tag13, U(kx), U(186), k->field ? k->field : "", C_MUTED, 256, 0, U(2), 1);
	pill_fill(U(kx), U(fy), U(kw), U(50), C_INK, 30);
	{
		int w = text(&A.row, U(kx + 24), U(fy + 33), k->shown, C_INK, 256, 0, 0, 0);
		fill(U(kx + 24) + w + U(3), U(fy + 12), U(2) > 1 ? U(2) : 2, U(26), C_ACCENT, 256);
	}
	if (k->note) text(&A.hint, U(kx), U(fy + 78), k->note, C_MUTED, 256, 0, 0, 0);
	for (r = 0; r < 5; r++)
		for (c = 0; c < 10; c++) {
			const char *lab;
			char one[2];
			float x = kx + c * kp, y = ky + r * 52, w = kp - 6;
			int f, i;
			if (r == 4) {	/* the wide keys, drawn from their first column */
				for (i = 0; i < 5 && k->wide[i].col != c; i++) ;
				if (i == 5) continue;
				w = k->wide[i].span * kp - 6;
				lab = k->wide[i].label;
				f = k->r == 4 && k->c == c;
			} else {
				one[0] = k->keys[r][c]; one[1] = 0; lab = one;
				f = k->r == r && k->c == c;
			}
			fill(U(x), U(y), U(w), U(46), f ? C_ACCENT : C_SHADE, f ? 70 : 110);
			fill(U(x), U(y), U(w), 1, f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x), U(y + 46) - 1, U(w), 1, f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x), U(y), 1, U(46), f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x + w) - 1, U(y), 1, U(46), f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			if (r == 4) text_fit(&A.hint, U(x + w / 2) - text_w(&A.hint, lab, 0, 0) / 2, U(y + 28), lab, f ? C_INK : C_MUTED, 256, U(w - 8));
			else text(f ? &A.rowf : &A.row, U(x + w / 2), U(y + 31), lab, f ? C_INK : C_MUTED, 256, 1, 0, 0);
		}
}
static void home_net(void)
{
	struct nui_view v;
	float top = 720 * .315f, xx = s_uw * .40f, lw = s_uw - s_p - xx, mid = top + 35, pitch = 51.2f, lx;
	int b;
	nui_view(&v);
	if (v.screen == NUI_CLOSED) return;
	if (v.screen == NUI_KEYBOARD) { net_keyboard(&v); return; }
	lx = xx - s_p - 20;
	for (b = 0; b < 4; b++) {	/* the tab's signal bars, large */
		int h = U(14 + 13 * b);
		fill(U(s_p + 17 * b), U(mid + 40) - h, U(10), h, C_ACCENT, 256);
	}
	text_fit(&A.dname, U(s_p), U(mid + 93), v.title ? v.title : "", C_INK, 256, U(lx));
	if (v.subtitle) text_fit(&A.lib, U(s_p), U(mid + 135), v.subtitle, C_INK, 256, U(lx));
	if (v.status) text_wrap2(&A.lib, U(s_p), U(mid + (v.subtitle ? 178 : 141)), U(27), v.status, C_MUTED, 256, U(lx));
	if (v.total) {
		int by = U(mid + (v.subtitle ? 178 : 141) + 2 * 27 + 12), bw = U(lx);
		fill(U(s_p), by, bw, U(4), C_MUTED, 64);
		fill(U(s_p), by, (int)((unsigned long long)bw * (v.done < v.total ? v.done : v.total) / v.total), U(4), C_ACCENT, 256);
	}
	if (v.nrows) rows(xx, top, lw, pitch, v.rows, v.values, v.nrows, v.sel, &A.srow, &A.srowf, &A.sval);
	if (v.more_up) text(&A.hint, U(xx + lw / 2), U(top - 10), "^", C_MUTED, 256, 1, 0, 0);
	if (v.more_down) text(&A.hint, U(xx + lw / 2), U(top + v.nrows * pitch + 18), "v", C_MUTED, 256, 1, 0, 0);
}
#endif
static void library(void)
{
	int s = S.sys, t = S.systype[s];
	text(&A.tag13, U(s_p), U(124), tr("Game library"), C_MUTED, 256, 0, U(2), 1);
	draw_mask(&A.llogo[t], U(s_p), U(150 + 29) - A.llogo[t].h / 2, C_INK, 256);
	game_list(LIB(s), S.syscount[s], S.lib_sel[s], S.lib_top[s], 0, 0);
}

/* ---- search -------------------------------------------------------------------------------------
 * For libraries of thousands: Y in a library looks through its system, Y on the Games page through
 * them all. The words are typed on a keyboard of the menu's own look, and the results follow every
 * key: the games whose name holds each word, the ones that start with it first, then the ones with
 * a word that does, then the rest, each part in the list's own order. */
static const char *const k_kb[4] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ'&-.", "1234567890" };
#define KB_ROWS 5                      /* four of keys, and Space / Delete / Clear */
enum { KB_SPACE, KB_DEL, KB_CLEAR };
static int kb_special(int c) { return c < 4 ? KB_SPACE : c < 7 ? KB_DEL : KB_CLEAR; }
static const char *const k_short[NSYS] = { "GB", "GBC", "GBA", "SNES", "MD", "SMS", "GG", "SG", "NES" };

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
/* where word w (n letters, lower case) is in name s, without case: -1 none, else 0 at the start,
 * 1 at the start of one of its words, 2 inside one */
/* a name's character as a search compares it: its letter without accents, small; 0 for a mark
 * that only puts an accent on the letter before */
static uint32_t fold_low(uint32_t c)
{
	c = pico_fold_cp(c);
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
static int word_at(const char *s, const char *w, int n)
{
	const char *p = s, *prev = 0;
	int best = -1;
	while (*p) {
		const char *q = p;
		int k = 0;
		while (k < n && *q) {
			uint32_t c = fold_low(pico_utf8_next(&q));
			if (!c) continue;
			if (c != (unsigned char)w[k]) break;
			k++;
		}
		if (k == n) {
			int at = p == s ? 0 : (*prev == ' ' || *prev == '-' || *prev == ':' || *prev == '.') ? 1 : 2;
			if (best < 0 || at < best) best = at;
			if (best == 0) break;
		}
		prev = p;
		pico_utf8_next(&p);
	}
	return best;
}
/* the results for the words in S.sq, into s_res */
static void search_run(void)
{
	char w[8][44];
	int wl[8], nw = 0, i, pass, from = 0, to = s_nrom;
	const unsigned short *list = 0;
	for (i = 0; i < S.sq_n && nw < 8; ) {	/* the words, lower case */
		int k = 0;
		while (i < S.sq_n && S.sq[i] == ' ') i++;
		while (i < S.sq_n && S.sq[i] != ' ' && k < 43) w[nw][k++] = lower(S.sq[i++]);
		if (k) { w[nw][k] = 0; wl[nw++] = k; }
	}
	if (S.sr_sys >= 0) { list = LIB(S.sr_sys); to = S.syscount[S.sr_sys]; }
	S.sr_n = 0;
	for (pass = 0; pass < 3; pass++) {	/* names starting with the first word, then at a word, then the rest */
		for (i = from; i < to; i++) {
			int rom = list ? list[i] : i, k, rank = 0;
			for (k = 0; k < nw; k++) {
				int at = word_at(s_names[rom], w[k], wl[k]);
				if (at < 0) { rank = -1; break; }
				if (k == 0) rank = at;
			}
			if (rank == pass || (!nw && pass == 0)) s_res[S.sr_n++] = (unsigned short)rom;
		}
		if (!nw) break;
	}
	S.sr_sel = 0; S.sr_top = 0;
}
static void search_open(int sys, int from_lib)
{
	S.search = 1; S.sr_sys = sys; S.sr_from_lib = from_lib;
	S.sr_list = 0; S.kb_r = 0; S.kb_c = 0;
	S.sq_n = 0; S.sq[0] = 0;      /* every search starts afresh */
	S.reveal = 0;                 /* it cascades in (frame_render) */
	S.dip = from_lib ? 0 : .3f;   /* from the Games page the backdrop darkens under it */
	search_run();
}
static void search_key(int r, int c)
{
	if (r < 4) {
		if (S.sq_n < (int)sizeof S.sq - 1) S.sq[S.sq_n++] = k_kb[r][c];
	} else if (kb_special(c) == KB_SPACE) {
		if (S.sq_n && S.sq[S.sq_n - 1] != ' ' && S.sq_n < (int)sizeof S.sq - 1) S.sq[S.sq_n++] = ' ';
	} else if (kb_special(c) == KB_DEL) {
		if (S.sq_n) S.sq_n--;
	} else S.sq_n = 0;
	S.sq[S.sq_n] = 0;
	search_run();
}
/* a magnifying glass: a ring and its handle */
static void magnifier(float cx, float cy, uint32_t rgb, int op)
{
	int x0 = U(cx - 12), y0 = U(cy - 12), n = U(24) + 2, i, j;
	float r = U(7.5f), th = U(1.9f) > 1.2f ? U(1.9f) : 1.2f, k;
	for (j = 0; j < n; j++)
		for (i = 0; i < n; i++) {
			float dx = x0 + i + .5f - U(cx), dy = y0 + j + .5f - U(cy), d = sq(dx * dx + dy * dy) - r;
			float c = th * .5f + .5f - (d < 0 ? -d : d);
			if (c > 0) put(x0 + i, y0 + j, rgb, c >= 1 ? op : (int)(c * op));
		}
	for (k = 0; k <= 1; k += .125f)
		disc(U(cx + 5.6f + 5.4f * k) + .0f, U(cy + 5.6f + 5.4f * k) + .0f, th * .62f, rgb, op);
}
static void search_view(void)
{
	float kx = s_p, kw = s_uw * .44f, kp = kw / 10, fy = 146, ky = 222, rx = kx + kw + s_uw * .055f;
	float rw = s_uw - s_p - rx - 16, ly = 146;
	int r, c, i, k;
	char t[64];
	/* where it looks */
	{
		int w = text(&A.tag13, U(kx), U(124), tr("Search"), C_MUTED, 256, 0, U(2), 1);
		disc(U(kx) + w + U(12) + .0f, U(119) + .0f, U(2) + .0f, C_MUTED, 256);
		text(&A.tag13, U(kx) + w + U(24), U(124), S.sr_sys >= 0 ? tr(s_sysname[S.systype[S.sr_sys]]) : tr("All systems"),
		     C_MUTED, 256, 0, U(2), 1);
	}
	/* the words typed, in a field */
	pill_fill(U(kx), U(fy), U(kw), U(50), C_INK, 30);
	magnifier(kx + 26, fy + 24, S.sq_n ? C_INK : C_MUTED, 256);
	if (S.sq_n) {	/* a long query shows its end, where the typing is */
		char b[sizeof S.sq + 4];
		const char *v = S.sq;
		int w;
		if (text_w(&A.row, v, 0, 0) > U(kw - 90)) {
			while (*v && text_w(&A.row, v, 0, 0) + text_w(&A.row, "...", 0, 0) > U(kw - 90)) v++;
			snprintf(b, sizeof b, "...%s", v);
			v = b;
		}
		w = text(&A.row, U(kx + 50), U(fy + 33), v, C_INK, 256, 0, 0, 0);
		if (!S.sr_list) fill(U(kx + 50) + w + U(3), U(fy + 12), U(2) > 1 ? U(2) : 2, U(26), C_ACCENT, 256);
	} else {
		text(&A.row, U(kx + 50), U(fy + 33), tr("Type a name"), C_MUTED, 200, 0, 0, 0);
		if (!S.sr_list) fill(U(kx + 48), U(fy + 12), U(2) > 1 ? U(2) : 2, U(26), C_ACCENT, 256);
	}
	/* the keyboard: keys in the soft frame of the menu, the focused one in the accent */
	for (r = 0; r < KB_ROWS; r++)
		for (c = 0; c < 10; c++) {
			int f = !S.sr_list && S.kb_r == r && (r < 4 ? S.kb_c == c : kb_special(S.kb_c) == kb_special(c));
			float x = kx + c * kp, y = ky + r * 50, w = kp - 6;
			const char *lab;
			char one[2];
			if (r == 4) {	/* the wide keys: drawn once, from their first column */
				int sp = kb_special(c);
				if (c != 0 && c != 4 && c != 7) continue;
				w = (sp == KB_SPACE ? 4 : 3) * kp - 6;
				lab = tr(sp == KB_SPACE ? "Space" : sp == KB_DEL ? "Delete" : "Clear");
			} else { one[0] = k_kb[r][c]; one[1] = 0; lab = one; }
			fill(U(x), U(y), U(w), U(44), f ? C_ACCENT : C_SHADE, f ? 70 : 110);
			fill(U(x), U(y), U(w), 1, f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x), U(y + 44) - 1, U(w), 1, f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x), U(y), 1, U(44), f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			fill(U(x + w) - 1, U(y), 1, U(44), f ? C_ACCENT : C_MUTED, f ? 256 : 60);
			if (r == 4) text_fit(&A.hint, U(x + w / 2) - text_w(&A.hint, lab, 0, 0) / 2, U(y + 27), lab, f ? C_INK : C_MUTED, 256, U(w - 8));
			else text(f ? &A.rowf : &A.row, U(x + w / 2), U(y + 30), lab, f ? C_INK : C_MUTED, 256, 1, 0, 0);
		}
	/* the results */
	if (S.sr_n) snprintf(t, sizeof t, tr_n(S.sr_n, "%d game", "%d games"), S.sr_n);
	text(&A.tag13, U(rx), U(124), S.sr_n ? t : tr("No games found"), C_MUTED, 256, 0, U(2), 1);
	k = S.sr_n - S.sr_top < LIB_ROWS ? S.sr_n - S.sr_top : LIB_ROWS;
	for (i = 0; i < k; i++) {
		int rom = s_res[S.sr_top + i];
		s_rowtxt[i] = s_names[rom];
		s_rowval[i] = S.sr_sys < 0 ? k_short[s_roms[rom].type] : 0;
	}
	rows(rx, ly, rw, 51, s_rowtxt, S.sr_sys < 0 ? s_rowval : 0, k, S.sr_list ? S.sr_sel - S.sr_top : -1,
	     &A.row, &A.rowf, &A.info);
	list_marks(rx, ly, rw, S.sr_n, S.sr_top, S.sr_sel);
	letter_badge(rx + rw / 2, ly + LIB_ROWS * 51 / 2);
}

/* the menu into its own layer (transparent around it); redrawn only when something changed */
extern volatile int g_dbg_colour;     /* oem showcolour:N - the menus in colour N for screenshots, the setting kept */
volatile int g_dbg_anim_slow = 1;   /* oem animslow:N - the screen cascades N times slower, to look at */
static void note_draw(void);   /* why a game did not start (with the launch, below) */
static int s_ui_clean;   /* the layer is known clear outside what it tracked */
/* clear what the last drawing of the menu layer touched, and start tracking again */
static void ui_clear(void)
{
	uint32_t *ui = (uint32_t *)CUR_UI_PA;
	int y;
	if (!s_ui_clean) {	/* first use in this menu: a game may have used the memory */
		memset(ui, 0, (size_t)g_W * g_H * 4);
		s_ui_clean = 1;
		for (y = 0; y < g_H; y++) s_ux0[y] = s_ux1[y] = 0;
	}
	for (y = 0; y < g_H; y++) {
		if (s_ux1[y] > s_ux0[y]) memset(ui + y * g_W + s_ux0[y], 0, (size_t)(s_ux1[y] - s_ux0[y]) * 4);
		s_ux0[y] = s_ux1[y] = 0;
	}
}
static void ui_compose(void)
{
	static const char *hc_games[3] = { "A", "Y", "LR" }, *hl_games[3] = { "Open library", "Search", "Section" };
	static const char *hc_kb[5] = { "A", "Y", "X", "R", "B" }, *hl_kb[5] = { "Type", "Delete", "Space", "Results", "Back" };
	static const char *hc_kb0[4] = { "A", "Y", "X", "B" }, *hl_kb0[4] = { "Type", "Delete", "Space", "Back" };
	static const char *hc_sr[2] = { "A", "B" }, *hl_sr[2] = { "Launch", "Keyboard" };
	static const char *hc_lib3[4] = { "A", "Y", "LR", "B" }, *hl_lib3[4] = { "Launch", "Search", "A-Z", "Back" };
	static const char *hc_res[2] = { "A", "LR" }, *hl_res[2] = { "Resume", "Section" };
	static const char *hc_resx[3] = { "A", "X", "LR" }, *hl_resx[3] = { "Resume", "Link", "Section" };
	static const char *hc_set[2] = { "A", "LR" }, *hl_set[2] = { "Select", "Section" };
	static const char *hc_lib[3] = { "A", "Y", "B" }, *hl_lib[3] = { "Launch", "Search", "Back" };
	static const char *hc_pick[2] = { "A", "B" }, *hl_pick[2] = { "Play this game", "Back" };
	static const char *hc_libx[4] = { "A", "X", "Y", "B" }, *hl_libx[4] = { "Launch", "Link", "Search", "Back" };
	static const char *hc_sub[2] = { "A", "B" }, *hl_sub[2] = { "Select", "Back" };
	g_fb = (uint32_t *)CUR_UI_PA;
	ui_clear();
	g_track = 1;
	if (S.search) {
		search_view();
		header("Search");
		if (S.sr_list) hints(hc_sr, hl_sr, 2);
		else if (S.sr_n) hints(hc_kb, hl_kb, 5);
		else hints(hc_kb0, hl_kb0, 4);   /* nothing found: no results to go to */
	} else if (S.lib) {
		library();
		header("Game library");
		if (S.pick) hints(hc_pick, hl_pick, 2);
		else if (PICO_LINK) hints(hc_libx, hl_libx, 4);
		else if (S.syscount[S.sys] > 3 * LIB_ROWS) hints(hc_lib3, hl_lib3, 4);
		else hints(hc_lib, hl_lib, 3);
	} else {
		rail();
		if (S.tab == TAB_GAMES) home_games(); else if (S.tab == TAB_RESUME) home_resume();
#if PICO_LINK
		else if (S.tab == TAB_LINK) home_link();
		else if (S.tab == TAB_NET) home_net();
#endif
		else home_settings();
		header(S.tab == TAB_SETTINGS ? "Settings" : S.tab == TAB_LINK ? "Link" : S.tab == TAB_NET ? "Network" : "Home");
#if PICO_LINK
		if (S.tab == TAB_LINK) {
			struct lui_view v;
			lui_view(&v);
			hints(v.hint_code, v.hint_label, v.nhints);
		} else if (S.tab == TAB_NET) {
			struct nui_view v;
			nui_view(&v);
			hints(v.hint_code, v.hint_label, v.nhints);
		} else
#endif
		if (S.tab == TAB_GAMES) hints(hc_games, hl_games, 3);
		else if (S.tab == TAB_RESUME) {
			if (S.nrec && PICO_LINK) hints(hc_resx, hl_resx, 3);   /* X: play it together, send it */
			else if (S.nrec) hints(hc_res, hl_res, 2);
			else hints(hc_res + 1, hl_res + 1, 1);
		}
		else if (S.set_page) hints(hc_sub, hl_sub, 2);
		else hints(hc_set, hl_set, 2);
	}
	note_draw();
	g_track = 0;
}
/* the frame: the backdrop and its moving current, the menu over it, the dip of a screen change */
static uint32_t *frame_target(void)
{
	unsigned int pitch, W, H;
	uint32_t *fb = ayaneo_canvas_back(&pitch, &W, &H);
	return pitch == (unsigned)g_W && W == (unsigned)g_W && H == (unsigned)g_H ? fb : (uint32_t *)CUR_FRAME_PA;
}
static void frame_render(void)
{
	const uint32_t *ui = (const uint32_t *)CUR_UI_PA;
	int i, y;
	g_fb = frame_target();   /* the canvas itself when it is laid out like the frame */
	backdrop(S.lib || S.search || S.tab != TAB_GAMES);
	if (!S.lib && !S.search && S.tab == TAB_GAMES && S.sys_t < 1) stage_draw();   /* the consoles crossfading */
	if (S.reveal >= 1 && S.leave <= 0) {
		for (y = 0; y < g_H; y++)	/* premultiplied over, only where the menu layer was drawn */
			for (i = y * g_W + s_ux0[y]; i < y * g_W + s_ux1[y]; i++) {
				uint32_t u = ui[i], a = u >> 24, d, k;
				if (!a) continue;
				if (a == 255) { g_fb[i] = u; continue; }
				d = g_fb[i]; k = 256 - a - (a >> 7);
				g_fb[i] = 0xFF000000u | ((((d & 0xFF00FFu) * k >> 8) & 0xFF00FFu) + (u & 0xFF00FFu)) |
					  ((((d & 0x00FF00u) * k >> 8) & 0x00FF00u) + (u & 0x00FF00u));
			}
	} else {
		/* A screen coming in (or the search going): the layer as it is, in bands from the top
		 * down, each fading in as it rises into place (going: out from the bottom up, sinking).
		 * Done here on the finished layer, so the menu is not drawn again for it. */
		float e = S.leave > 0 ? 1 - S.leave : ease(S.reveal), soft = g_H * .38f, head = e * (g_H + soft);
		int off = (int)((1 - e) * U(26));
		for (y = 0; y < g_H; y++) {
			int sy = y - off, j;
			float f = (head - (float)sy) / soft;
			uint32_t m;
			if (sy < 0 || f <= 0) continue;
			m = f >= 1 ? 256u : (uint32_t)(f * 256);
			for (j = s_ux0[sy]; j < s_ux1[sy]; j++) {
				uint32_t u = ui[sy * g_W + j], a, d, k;
				if (!(u >> 24)) continue;
				if (m < 256) u = ((((u & 0xFF00FFu) * m) >> 8) & 0xFF00FFu) | ((((u >> 8) & 0xFF00FFu) * m) & 0xFF00FF00u);
				a = u >> 24;
				i = y * g_W + j;
				d = g_fb[i]; k = 256 - a - (a >> 7);
				g_fb[i] = 0xFF000000u | ((((d & 0xFF00FFu) * k >> 8) & 0xFF00FFu) + (u & 0xFF00FFu)) |
					  ((((d & 0x00FF00u) * k >> 8) & 0x00FF00u) + (u & 0x00FF00u));
			}
		}
	}
	if (S.dip > 0) fill(0, 0, g_W, g_H, 0x071021u, (int)(S.dip * 180));
	perf_mark(3);
}
/* the frame into the canvas, with the volume and brightness bar over it */
static void show(void)
{
	unsigned int pitch, W, H, y;
	unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
	if ((uint32_t *)fb != g_fb)
		for (y = 0; y < (unsigned)g_H; y++)
			memcpy(fb + y * pitch, (const uint32_t *)CUR_FRAME_PA + y * (unsigned)g_W, (size_t)g_W * 4);
	gba_menu_av_draw(fb, pitch, (int)W, (int)H);
	perf_mark(4);
}
/* ---- into and out of a game ----------------------------------------------------------------
 * Both ways are one quick crossfade between the backdrop and the game, run by the shared opening
 * and closing transition (gba_punch.h, in its fade mode), with the menu layer over it: on the way
 * in the menu stays until the game is nearly all there, then zooms out of view; on the way out it
 * zooms back in over the fading game. The game's memory takes the menu's buffers once it loads,
 * so the layer and the backdrop it opens out of go where the transition buffers live. */
#define CUR_SNAP_PA   0x54000000u     /* the backdrop the game opens out of (the launch snapshot) */
#define CUR_OV_PA     0x54300000u     /* the menu layer during the opening (up to 1280 x 960) */
#define CUR_LFULL_PA  0x54560000u     /* the game the opening fades to, when it fits below the canvas */
#define CUR_LFULL_END 0x54800000u
/* (each staggered off the megabyte so the rows of the four buffers a frame reads and writes do not
 * all fall on the same cache sets: lined up they took half again as long) */
#define CUR_RSNAP_PA  0x52A45000u     /* the way back: the backdrop it lands on (the staging is free) */
#define CUR_RFULL_PA  0x5348A000u     /* the way back: the game it leaves (above the staging, which the
				       * pack load uses after the game is put up from here) */
static int16_t s_ov_x0[1024], s_ov_x1[1024];

/* the backdrop alone, drifting current included, into dst laid out pitch wide */
static void backdrop_into(uint32_t *dst, unsigned int pitch)
{
	unsigned int y;
	g_fb = (uint32_t *)CUR_FRAME_PA;
	backdrop(S.lib || S.search || S.tab != TAB_GAMES);
	for (y = 0; y < (unsigned)g_H; y++) memcpy(dst + y * pitch, g_fb + y * (unsigned)g_W, (size_t)g_W * 4);
}
/* the reveal of the way back: the backdrop into the transition's snapshot (the menu comes over it) */
static void reveal_backdrop(uint32_t *snap, unsigned int pitch)
{
	extern unsigned g_perf_xf[2];
	unsigned int t0 = gpt4_get_current_tick();
	backdrop_into(snap, pitch);
	g_perf_xf[1] = (gpt4_get_current_tick() - t0) / 13000u;   /* oem diag "active": its time, ms */
}
/* the largest radius of the shared closing transition (the canvas' half diagonal) */
static int reverse_rmax(void)
{
	unsigned int w, h, d2, r = 1;
	ayaneo_canvas_dims(&w, &h);
	d2 = (w * w + h * h) / 4u;
	while (r * r < d2) r++;
	return (int)r;
}
static void igm_forget(void);   /* the in-game menu (the end of this file) */
static void igm_preload(void);
/* a game was picked: arm the opening (the frame on screen stays until it starts) */
static void launch_arm(void)
{
	extern int gba_punch_ready;
	extern void ayaneo_gbc_audio_pause(int on);
	unsigned int pitch, W, H;
	{ extern volatile unsigned int g_dbg_launch_a, g_dbg_launch_pre_us[6];   /* oem diag launch_us */
	  unsigned int t = gpt4_get_current_tick(), k;
	  g_dbg_launch_a = t ? t : 1u;
	  for (k = 0; k < 6; k++) g_dbg_launch_pre_us[k] = 0; }
	(void)ayaneo_canvas_back(&pitch, &W, &H);
	backdrop_into((uint32_t *)CUR_SNAP_PA, pitch);
	memcpy((void *)CUR_OV_PA, (const void *)CUR_UI_PA, (size_t)g_W * g_H * 4);
	memcpy(s_ov_x0, s_ux0, sizeof s_ov_x0); memcpy(s_ov_x1, s_ux1, sizeof s_ov_x1);
	g_punch_ov = (const uint32_t *)CUR_OV_PA; g_punch_ov_x0 = s_ov_x0; g_punch_ov_x1 = s_ov_x1;
	g_punch_fade = 820;   /* the radius every core's opening grows to */
	{	/* the game frame in cached memory (the default buffer reads slowly on the Konkr) */
		extern unsigned int g_punch_game_full;
		extern int g_punch_cached;
		g_punch_cached = 0;
		if (CUR_LFULL_PA + (unsigned)g_W * g_H * 4u <= CUR_LFULL_END) { g_punch_game_full = CUR_LFULL_PA; g_punch_cached = 1; }
	}
	ayaneo_gbc_audio_pause(1);   /* silent until the game is all on screen */
	igm_forget();                /* the opening takes the in-game menu's memory: each game reads its pack */
	gba_punch_ready = 1;
}

/* ---- a game that cannot start ---------------------------------------------------------------- */
volatile int g_dbg_launch_refused;   /* oem diag: why the menu last refused a game (RA_ERR_*) */
static void note_show(int err) { S.note_err = err; S.note_t = 4.5f; g_dbg_launch_refused = err; }
/* the game, or -1 with a note saying why it cannot start (its archive is looked into first) */
static int launch_check(int rom)
{
	int why;
	if (rom < 0) return -1;
	why = gba_rom_check(gba_sd_menu_vol(), &s_roms[rom]);
	if (why == RA_OK) return rom;
	note_show(why);
	return -1;
}
/* the note: a dark pill along the bottom, a mark in the accent, the reason */
static void note_draw(void)
{
	const char *t;
	int tw, w, x, y = U(720 - 92), h = U(44);
	if (S.note_t <= 0) return;
	t = tr(rom_archive_error_text(S.note_err));
	tw = text_w(&A.info, t, 0, 0);
	if (tw > U(s_uw - 2 * s_p - 80)) tw = U(s_uw - 2 * s_p - 80);
	w = tw + U(70);
	x = (g_W - w) / 2;
	pill_fill(x, y, w, h, C_SHADE, 240);
	disc(x + U(26) + .0f, y + h / 2 + .0f, U(9) + .0f, C_ACCENT, 256);
	text(&A.hintc, x + U(26), y + h / 2 + U(4), "!", C_BTN_INK, 256, 1, 0, 0);
	text_fit(&A.info, x + U(46), y + h / 2 + U(5), t, C_INK, 256, tw);
}

/* ---- input ------------------------------------------------------------------------------------- */
static struct { int prev[6]; float rep[6], held[6]; } K;   /* up down left right lb rb: held, repeat timer, held for */
/* a press, and repeats while held (0.32 s, then every 0.085 s) */
static int fire(int i, int level, float dt)
{
	int f = 0;
	if (level && !K.prev[i]) { f = 1; K.rep[i] = .32f; K.held[i] = 0; }
	else if (level) { K.held[i] += dt; K.rep[i] -= dt; if (K.rep[i] <= 0) { f = 1; K.rep[i] = .085f; } }
	else K.held[i] = 0;
	K.prev[i] = level;
	return f;
}
/* rows a held Up or Down moves at each repeat: one, then more the longer it is held, so a list of
 * thousands is crossed in seconds */
static int step_of(int i) { return K.held[i] > 2.4f ? 24 : K.held[i] > 1.2f ? 5 : 1; }

/* the moves in a list of n (the focus, the first row shown); a long move shows its letter */
static void list_move(const unsigned short *idx, int n, int *sel, int *top, int d)
{
	int v = *sel + d;
	if (n <= 0) return;
	if (v < 0) v = 0;
	if (v > n - 1) v = n - 1;
	*sel = v;
	if (*sel < *top) *top = *sel;
	if (*sel >= *top + LIB_ROWS) *top = *sel - LIB_ROWS + 1;
	if (d > 1 || d < -1) { S.badge = name_letter(s_names[idx[v]]); S.badge_t = .7f; }
}
/* to the first game of the next letter, or back to the first of this one (of the one before when
 * already there): L and R in a list of more than a few pages */
static void list_letter(const unsigned short *idx, int n, int *sel, int *top, int dir)
{
	int i = *sel;
	char cur;
	if (n <= 0) return;
	cur = name_letter(s_names[idx[i]]);
	if (dir > 0) {
		while (i < n - 1 && name_letter(s_names[idx[i]]) == cur) i++;
	} else {
		if (i > 0 && name_letter(s_names[idx[i - 1]]) != cur) i--;
		cur = name_letter(s_names[idx[i]]);
		while (i > 0 && name_letter(s_names[idx[i - 1]]) == cur) i--;
	}
	*sel = i;
	if (*sel < *top) *top = *sel;
	if (*sel >= *top + LIB_ROWS) *top = *sel;   /* a jump: the letter's first game at the top */
	if (*top > n - LIB_ROWS) *top = n - LIB_ROWS > 0 ? n - LIB_ROWS : 0;
	S.badge = name_letter(s_names[idx[i]]);
	S.badge_t = .7f;
}

/* ---- the run ----------------------------------------------------------------------------------- */
static void focus_rom(int rom)
{
	int s = rom >= 0 && rom < s_nrom ? sys_of(s_roms[rom].type) : -1, k;
	if (s < 0) return;
	S.sys = S.sys_from = s;
	for (k = 0; k < S.syscount[s]; k++)
		if (LIB(s)[k] == rom) {
			S.lib_sel[s] = k;
			if (k < S.lib_top[s] || k >= S.lib_top[s] + LIB_ROWS) S.lib_top[s] = k >= LIB_ROWS ? k - LIB_ROWS + 1 : 0;
		}
}

/* Returns the picked game, -3 when a theme was picked (the caller runs the menu again), or -2
 * when the theme's pack is missing or does not fit this screen (the classic menu runs). */
/* oem diag "cur_ms": the last opening of this menu, ms from its start to each step done: the panel
 * handed over, the in-game menu's pack, the menu's pack, the language's, the setup, the box art and
 * the first frame shown; then within the setup the fonts, the roster, the recent games, the battery */
volatile unsigned int g_dbg_cur_ms[12];
#define CUR_MARK(i) (g_dbg_cur_ms[i] = (gpt4_get_current_tick() - t_run) / 13000u)
int pico_current_run(const gba_rom_entry *roms, int nrom, int start_sel, int do_reverse)
{
	unsigned int pitch, W, H, saved_mhz, prev = 0, t_run = gpt4_get_current_tick();
	int launch_gate = 1, i, dirty = 1, shown = 0, failed;
	snes_input in;
	(void)ayaneo_canvas_back(&pitch, &W, &H);
	if (W * H * 4u > CUR_FRAME_MAX || nrom <= 0) return -2;
	saved_mhz = ayaneo_get_cpu_mhz();
	ayaneo_set_cpu_mhz(1200);
	if (do_reverse) {	/* the frozen game onto our plane first: the pack lands on its memory */
		extern unsigned int g_punch_game_full;
		extern int g_punch_cached;
		extern void gba_menu_reverse_hold(void);
		g_punch_game_full = CUR_RFULL_PA; g_punch_cached = 1;
		gba_menu_reverse_hold();
	}
	if (!do_reverse) {	/* own the panel, black, through the pack load */
		unsigned int *fb;
		ayaneo_display_prepare();
		fb = ayaneo_canvas_back(&pitch, &W, &H);
		for (i = 0; i < (int)H; i++) memset(fb + i * pitch, 0, (size_t)W * 4);
		ayaneo_canvas_present();
	}
	CUR_MARK(0);
	igm_preload();   /* the in-game menu's pack, where it is kept */
	CUR_MARK(1);
	{	/* a read that fails once (the card busy elsewhere) is tried again: the fallback is another
		 * theme, which must only come when the pack really is not there */
		int tries = 0;
		while (pack_load(W, H) != 0) {
			if (++tries == 3) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
			thread_sleep(30);
		}
	}
	CUR_MARK(2);
	pico_lang_load(W, H);   /* the words and glyphs of the language in use (none for English) */
	CUR_MARK(3);
	g_W = (int)W; g_H = (int)H; g_s = (float)H / 720.0f;
	s_uw = (float)W / g_s;
	s_p = s_uw * .048f > 38 ? s_uw * .048f : 38;
	art_load();
	CUR_MARK(7);
	colour_set(g_dbg_colour >= 0 ? g_dbg_colour : ayaneo_get_menu_colour());   /* oem showcolour */
	s_roms = roms; s_nrom = nrom > PICO_GAMES_MAX ? PICO_GAMES_MAX : nrom;
	if (roster_build() != 0) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
	CUR_MARK(8);
	if (!S.nsys) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
	S.bg_key[0] = S.bg_key[1] = -1; S.pal_colour = -1;   /* the game took their memory */
	s_ui_clean = 0;
	sin_init();
	if (!S.ready) {	/* the first menu of this power on: Game Boy Advance, when the card has its games */
		memset(S.lib_sel, 0, sizeof S.lib_sel); memset(S.lib_top, 0, sizeof S.lib_top);
		S.tab = TAB_GAMES; S.lib = 0; S.set_sel = 0; S.set_page = 0; S.res_sel = 0;
		S.sys = sys_of(GBA_CONSOLE_GBA) >= 0 ? sys_of(GBA_CONSOLE_GBA) : 0;
		S.ready = 1;
	}
	if (S.sys >= S.nsys) S.sys = 0;
	S.sys_from = S.sys; S.sys_t = 1;
	S.reveal = 1; S.leave = 0;   /* no cascade left over from before a game */
	failed = gba_rom_take_error();   /* the last game did not start: why, below */
	if (do_reverse) focus_rom(start_sel);   /* back from a game: on it */
	for (i = 0; i < S.nsys; i++)
		if (S.lib_sel[i] >= S.syscount[i]) { S.lib_sel[i] = 0; S.lib_top[i] = 0; }
	S.lang_pick = pico_lang();
	S.theme_pick = 0;
	for (i = 0; i < A.nthemes; i++)
		if (theme_hash(i) == gba_menu_theme()) S.theme_pick = i;
	CUR_MARK(9);
	recent_load();
	if (failed) {	/* it did not start: not among the games played, and the note says why */
		note_show(failed);
		if (S.nrec && S.rec[0] == start_sel) recent_drop_first();
	}
	CUR_MARK(10);
	if (S.res_sel >= S.nrec) S.res_sel = 0;
	S.batt = gba_battery_pct(&S.chg);
	CUR_MARK(11);
	S.batt_tick = gpt4_get_current_tick();
	ayaneo_menu_audio_silence();
	CUR_MARK(4);
	gba_menu_boxart_load(roms, nrom);
	CUR_MARK(5);
	g_menu_live = 1;
#if PICO_LINK
	lui_set_roms(s_roms, s_nrom);   /* a Link screen carries on over this listing */
	lui_set_english(0);             /* this theme's fonts have every language's letters */
	lui_menu_enter();   /* Bluetooth off unless a Link screen still needs it (zero cost) */
	lui_set_painter(link_paint);   /* the start of a session, drawn in this theme's look */
	nui_set_roms(gba_sd_menu_vol(), s_roms, s_nrom);   /* the Network tab: the card, its games */
#endif

	if (do_reverse) {	/* back from a game: it crossfades into the backdrop, the menu zooming in */
		S.dip = 0;
		ui_compose();
		g_punch_ov = (const uint32_t *)CUR_UI_PA; g_punch_ov_x0 = s_ux0; g_punch_ov_x1 = s_ux1;
		g_punch_fade = reverse_rmax();
		{	/* everything in cached buffers of this menu's own, shown from the frame buffer: the
			 * canvas is where the frozen game is still shown from on the Konkr */
			extern unsigned int g_punch_game_full;
			extern void gba_menu_reverse_buffers(uint32_t *snap, unsigned int *dst);
			extern unsigned g_perf_xf[2];
			unsigned int t0 = gpt4_get_current_tick();
			extern int g_punch_cached;
			g_punch_game_full = CUR_RFULL_PA; g_punch_cached = 1;   /* prerendered at the start */
			gba_menu_reverse_buffers((uint32_t *)CUR_RSNAP_PA, (unsigned int *)CUR_FRAME_PA);
			gba_menu_reverse_into(reveal_backdrop);
			gba_menu_reverse_buffers(0, 0);
			g_punch_game_full = 0x55900000u; g_punch_cached = 0;
			g_perf_xf[0] = (gpt4_get_current_tick() - t0) / 13000u;   /* oem diag "tiles dark", ms */
		}
		g_punch_fade = 0; g_punch_ov = 0;
	} else S.dip = 1;          /* fade in from the black handover */

	for (;;) {
		float dt;
		int rom = -1;
		unsigned int now = gpt4_get_current_tick();
		if (ayaneo_get_cpu_mhz() < 1100) ayaneo_set_cpu_mhz(1200);
		dt = prev ? (float)(now - prev) / 13000000.0f : 1.0f / 60;
		prev = now;
		if (dt < .004f) dt = .004f; else if (dt > .1f) dt = .1f;
		if (now - S.batt_tick > 13000000u * 20) { S.batt = gba_battery_pct(&S.chg); S.batt_tick = now; dirty = 1; }
#if PICO_LINK
		{	/* the network: its news every frame; the Network tab drawn again as it moves on */
			static unsigned int net_frame;
			if (nui_tick() == NUI_ACT_ART) {	/* box art arrived: the card and its art listed again */
				ayaneo_set_cpu_mhz(saved_mhz);
				return -4;
			}
			if (S.tab == TAB_NET && !S.lib && ++net_frame % 6 == 0) dirty = 1;
		}
#endif
		{	/* the clock in the header turns over: the menu is drawn again */
			static long long shown_min = -1;
			long long m = pico_clock_local() / 60;
			if (m != shown_min) { shown_min = m; dirty = 1; }
		}

		memset(&in, 0, sizeof in);
		gba_menu_read_input(&in);
		if (launch_gate) {	/* A or Start still held from the close or the launch: not a press */
			if (in.a || in.start) { in.a = 0; in.start = 0; } else launch_gate = 0;
		}
		gba_menu_av_poll();
		{
			static int pa, pb, pst, px, py;
			int ea = in.a && !pa, eb = in.b && !pb, est = in.start && !pst;
			int up = fire(0, in.up, dt), dn = fire(1, in.down, dt), lf = fire(2, in.left, dt), rt = fire(3, in.right, dt);
			int lb = fire(4, in.lb, dt), rb = fire(5, in.rb, dt);
			int ey = in.y && !py, ex = in.x && !px;
			pa = in.a; pb = in.b; pst = in.start; px = in.x; py = in.y;
			if (ea || eb || est || up || dn || lf || rt || lb || rb || ey || ex) dirty = 1;
			if (S.search && S.leave > 0) {
				/* on its way out: the keys wait */
			} else if (S.search) {
				if (!S.sr_list) {	/* the keyboard */
					if (up && S.kb_r > 0) S.kb_r--;
					if (dn && S.kb_r < KB_ROWS - 1) S.kb_r++;
					if (lf) {
						if (S.kb_r == 4) S.kb_c = kb_special(S.kb_c) == KB_CLEAR ? 4 : 0;
						else if (S.kb_c > 0) S.kb_c--;
					}
					if (rt) {
						int last = S.kb_r == 4 ? kb_special(S.kb_c) == KB_CLEAR : S.kb_c == 9;
						if (last) { if (S.sr_n) S.sr_list = 1; }
						else if (S.kb_r == 4) S.kb_c = kb_special(S.kb_c) == KB_SPACE ? 4 : 7;
						else S.kb_c++;
					}
					if (ea) search_key(S.kb_r, S.kb_c);
					if (ey) search_key(4, 4);          /* Y: delete */
					if (ex) search_key(4, 0);          /* X: a space */
					if ((rb || est) && S.sr_n) S.sr_list = 1;
					if (eb) S.leave = .001f;   /* it cascades out, then back where it was opened */
				} else {	/* the results */
					if (up) list_move(s_res, S.sr_n, &S.sr_sel, &S.sr_top, -step_of(0));
					if (dn) list_move(s_res, S.sr_n, &S.sr_sel, &S.sr_top, step_of(1));
					if (lb) list_move(s_res, S.sr_n, &S.sr_sel, &S.sr_top, -LIB_ROWS);
					if (rb) list_move(s_res, S.sr_n, &S.sr_sel, &S.sr_top, LIB_ROWS);
					if (lf || eb) S.sr_list = 0;
					if (ey) { S.sr_list = 0; search_key(4, 4); }
					if ((ea || est) && S.sr_n && (rom = launch_check(s_res[S.sr_sel])) >= 0) {
						S.search = 0; S.lib = 1;   /* back from the game: its library, on it */
					}
				}
			} else if (S.lib) {
				int s = S.sys, n = S.syscount[s];
				const unsigned short *ix = LIB(s);
				if (up) list_move(ix, n, &S.lib_sel[s], &S.lib_top[s], -step_of(0));
				if (dn) list_move(ix, n, &S.lib_sel[s], &S.lib_top[s], step_of(1));
				if (lf) list_move(ix, n, &S.lib_sel[s], &S.lib_top[s], -LIB_ROWS);
				if (rt) list_move(ix, n, &S.lib_sel[s], &S.lib_top[s], LIB_ROWS);
				if (lb || rb) {	/* a long list: letter by letter; a short one: a page */
					if (n > 3 * LIB_ROWS) list_letter(ix, n, &S.lib_sel[s], &S.lib_top[s], rb ? 1 : -1);
					else list_move(ix, n, &S.lib_sel[s], &S.lib_top[s], rb ? LIB_ROWS : -LIB_ROWS);
				}
#if PICO_LINK
				if (S.pick) {	/* the guest's game for the Link screen: it launches it, armed */
					if (eb) { S.pick = 0; S.lib = 0; S.tab = TAB_LINK; S.dip = .6f; lui_picked(-1); }
					else if ((ea || est) && n) {
						S.pick = 0; S.lib = 0; S.tab = TAB_LINK;
						lui_picked(LIB(s)[S.lib_sel[s]]);
						if (lui_action_rom() >= 0) rom = launch_check(lui_action_rom());
					}
				} else if (ex && n) {	/* Link for this game: play it together, send it */
					lui_open_home_game(s_roms, s_nrom, LIB(s)[S.lib_sel[s]]);
					S.lib = 0; S.tab = TAB_LINK; S.dip = .6f;
				} else
#endif
				if (ey) { search_open(s, 1); S.lib = 0; }
				else if (eb) { S.lib = 0; S.dip = .6f; }
				else if (ea || est) rom = launch_check(LIB(s)[S.lib_sel[s]]);
			} else {
				/* a Link screen in the middle of something (waiting, joining) keeps the tab */
#if PICO_LINK
				struct nui_view nv;
				int link_busy = S.tab == TAB_LINK && lui_busy_screen();
				nv.screen = NUI_CLOSED;
				if (S.tab == TAB_NET) nui_view(&nv);
				if (nv.screen == NUI_KEYBOARD) link_busy = 1;	/* the keyboard keeps the tab */
#else
				int link_busy = 0;
#endif
				if (lb && S.tab > 0 && !link_busy) { S.tab--; S.dip = .45f; }
				if (rb && S.tab < NTABS - 1 && !link_busy) { S.tab++; S.dip = .45f; }
				if (S.tab == TAB_GAMES) {
					int to = S.sys;
					if (lf && S.sys > 0) to = S.sys - 1;
					if (rt && S.sys < S.nsys - 1) to = S.sys + 1;
					if (to != S.sys) {
						/* the console leaving is the one on show now, also when a held key
						 * moves again before the last move finished */
						S.sys_from = S.sys;
						S.sys = to; S.sys_t = 0;
						S.push += 1.2f * (float)(to > S.sys_from ? 1 : -1);   /* the current moves on with it */
					}
					if (ea || est) { S.lib = 1; S.dip = .6f; S.sys_from = S.sys; S.sys_t = 1; }
					else if (ey) { S.sys_from = S.sys; S.sys_t = 1; search_open(-1, 0); }
				}
#if PICO_LINK
				else if (S.tab == TAB_LINK) {
					unsigned int e = (up ? LUI_UP : 0) | (dn ? LUI_DOWN : 0) | (lf ? LUI_LEFT : 0) |
							 (rt ? LUI_RIGHT : 0) | (ea || est ? LUI_A : 0) | (eb ? LUI_B : 0);
					int act;
					if (!lui_open()) lui_open_home(s_roms, s_nrom);
					act = lui_input(e);
					if (act == LUI_ACT_CLOSE && !lui_open()) lui_open_home(s_roms, s_nrom);
					if (act == LUI_ACT_LAUNCH) rom = launch_check(lui_action_rom());
					if (act == LUI_ACT_RELIST) {	/* games arrived: the card listed again */
						ayaneo_set_cpu_mhz(saved_mhz);
						return -4;
					}
					if (act == LUI_ACT_PICK && sys_of(lui_action_system()) >= 0) {
						S.sys = S.sys_from = sys_of(lui_action_system()); S.sys_t = 1;
						S.lib = 1; S.pick = 1; S.dip = .6f;
					} else if (act == LUI_ACT_PICK)
						lui_picked(-1);	/* no game of that system here */
					dirty = 1;	/* what it shows moves on by itself (waiting, the consoles heard) */
				}
				else if (S.tab == TAB_NET) {
					unsigned int e = (up ? NUI_UP : 0) | (dn ? NUI_DOWN : 0) | (lf ? NUI_LEFT : 0) |
							 (rt ? NUI_RIGHT : 0) | (ea || est ? NUI_A : 0) | (eb ? NUI_B : 0) |
							 (ex ? NUI_X : 0) | (ey ? NUI_Y : 0);
					if (!nui_is_open()) nui_open();
					if (nui_input(e) == NUI_ACT_CLOSE) nui_open();	/* the tab stays on its home */
					dirty = 1;
				}
#endif
				else if (S.tab == TAB_RESUME) {
					if (up && S.res_sel > 0) S.res_sel--;
					if (dn && S.res_sel < S.nrec - 1) S.res_sel++;
					if ((ea || est) && S.nrec) rom = launch_check(S.rec[S.res_sel]);
#if PICO_LINK
					else if (ex && S.nrec) {	/* Link for this game, as from the library */
						lui_open_home_game(s_roms, s_nrom, S.rec[S.res_sel]);
						S.tab = TAB_LINK; S.dip = .6f;
					}
#endif
				} else {
					int dir = rt ? 1 : lf ? -1 : 0, it, flip = dir || ea;
					if (up && S.set_sel > 0) S.set_sel--;
					if (dn && S.set_sel < set_count(S.set_page) - 1) S.set_sel++;
					it = set_item();
					if (eb && S.set_page) {	/* back to the main list, on the row that opened it */
						S.set_sel = set_row_of(k_page_row[S.set_page]);
						S.set_page = SPAGE_MAIN; S.dip = .45f;
					} else if (page_of_row(it) > 0 && (ea || rt)) {
						S.set_page = page_of_row(it); S.set_sel = 0; S.dip = .45f;
					}
					if (pico_set_kind(it) == PSK_STEP || pico_set_kind(it) == PSK_SWITCH)
						pico_set_change(it, dir, ea);   /* as every theme does it */
					if (it == SET_THEME && dir && A.nthemes) S.theme_pick = (S.theme_pick + dir + A.nthemes) % A.nthemes;
					if (it == SET_COLOUR && flip) {	/* live: the backdrop is made again */
						colour_set(colour_step(g_colour, dir ? dir : 1));
						ayaneo_set_menu_colour(g_colour);
						ayaneo_menu_settings_persist();
					}
					if (it == SET_THEME && ea && A.nthemes) {
						unsigned int h = theme_hash(S.theme_pick);
						if (h != gba_menu_theme()) {
							ayaneo_set_menu_theme(h);
							ayaneo_menu_settings_persist();
							ayaneo_settings_persist_flush();   /* written before the card is read again */
							ayaneo_set_cpu_mhz(saved_mhz);
							return -3;
						}
					}
					if (it == SET_LANGUAGE && dir) S.lang_pick = (S.lang_pick + dir + LANG_N) % LANG_N;
					if (it == SET_LANGUAGE && ea && S.lang_pick != pico_lang()) {
						/* the words and glyphs of the new language, then the fonts take them up */
						pico_lang_set(S.lang_pick);
						ayaneo_menu_settings_persist();
						pico_lang_load(W, H);
						art_load();
						S.dip = .45f;
					}
					if (it == SET_BOOT_OS && ea) pico_set_change(it, 0, 1);   /* never returns */
				}
			}
		}
		if (S.sys_t < 1) { S.sys_t += dt / .46f; if (S.sys_t >= 1) { S.sys_t = 1; S.sys_from = S.sys; dirty = 1; } }
		if (S.dip > 0) { S.dip -= dt / .28f; if (S.dip <= 0) { S.dip = 0; dirty = 1; } }
		if (S.note_t > 0) { S.note_t -= dt; if (S.note_t <= 0) { S.note_t = 0; dirty = 1; } }
		if (S.reveal < 1) { S.reveal += dt / (.36f * g_dbg_anim_slow); if (S.reveal > 1) S.reveal = 1; }
		if (S.leave > 0) {	/* the search going: when it has, the screen behind it comes in */
			S.leave += dt / (.2f * g_dbg_anim_slow);
			if (S.leave >= 1) {
				S.leave = 0; S.search = 0; S.lib = S.sr_from_lib; S.reveal = 0;
				if (!S.lib) S.dip = .3f;
				dirty = 1;
			}
		}
		/* the badge stays whole, then goes: no fade, which would draw the menu again every frame */
		if (S.badge_t > 0) { S.badge_t -= dt; if (S.badge_t <= 0) { S.badge_t = 0; dirty = 1; } }
		/* a box art read a frame, none while a held key runs fast through a list (the art of the
		 * games it passes would hold every frame up): it comes as the list slows */
		gba_menu_boxart_budget(K.held[0] > 1.2f || K.held[1] > 1.2f ? 0 : 1);

		/* the current drifts (0.9 rad/s, the design's pace) and takes up a system move's push */
		{
			float take = S.push * (dt * 7 > 1 ? 1 : dt * 7);
			S.phase += dt * .9f + take;
			S.push -= take;
			if (S.phase > 6283.0f) S.phase -= 6283.18530718f;   /* a whole number of turns of every term */
		}
		/* the menu is drawn again only when something changed or moves; the frame every refresh */
		{	/* oem diag: render_us (this frame's drawing), peak_us (the worst of the last 2 s) */
			extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
			static unsigned int peak, t_peak, frames, t_fps;
			unsigned int r0 = gpt4_get_current_tick(), us;
			if (dirty || rom >= 0) {
				unsigned int c0 = gpt4_get_current_tick();
				ui_compose(); dirty = gba_menu_boxart_pending();   /* art still to come: drawn again */
				g_perf[4] = gpt4_get_current_tick() - c0;   /* oem diag rest: the last menu redraw */
			}
			frame_render();
			show();
			if (!shown) { shown = 1; CUR_MARK(6); }
			us = (gpt4_get_current_tick() - r0) / 13u;
			g_dbg_render_us = us;
			if (us > peak) peak = us;
			if (r0 - t_peak > 26000000u) { g_dbg_peak_us = peak; peak = 0; t_peak = r0; }
			frames++;
			if (r0 - t_fps >= 13000000u) { g_dbg_fps = frames; frames = 0; t_fps = r0; }
		}
		/* fastboot oem launch / snes-launch: a game picked by console and name, as if chosen here */
		{
			extern volatile int g_dbg_snes_launch, g_dbg_launch_type;
			extern char g_dbg_snes_name[32];
			if (g_dbg_snes_launch && rom < 0) {
				int want = g_dbg_launch_type ? g_dbg_launch_type - 1 : GBA_CONSOLE_SNES, k, a, b;
				g_dbg_snes_launch = 0;
				g_dbg_launch_type = 0;
				for (k = 0; k < s_nrom && rom < 0; k++) {
					const char *nm = s_roms[k].name, *sub = g_dbg_snes_name;
					if ((int)s_roms[k].type != want) continue;
					for (a = 0; nm[a] && rom < 0; a++) {
						for (b = 0; sub[b]; b++) {
							char x = nm[a + b], y = sub[b];
							if (x >= 'A' && x <= 'Z') x += 32;
							if (y >= 'A' && y <= 'Z') y += 32;
							if (x != y) break;
						}
						if (!sub[b]) rom = launch_check(k);
					}
				}
			}
		}
		if (rom >= 0) {	/* launch: remember it, then the game crossfades in over the backdrop */
			recent_add(rom);
			focus_rom(rom);
			gba_menu_present();   /* the menu stays on screen while the game loads */
			launch_arm();
			ayaneo_menu_audio_silence();
			ayaneo_set_cpu_mhz(saved_mhz);
			return rom;
		}
		gba_menu_present();
	}
}

/* ==== the in-game menu (pico_igm.h) ===============================================================
 * Over the running game, on the live overlay layer (ayaneo_overlay_live): a panel down the left
 * edge in the menu's own look, with the game, the actions, and the settings grouped in pages. It
 * slides in and out, the focus glides from row to row, a page slides in from the side and back, a
 * changed value nudges in from the way it went, and a saved or loaded state shows a note that
 * fades. It has a small pack of its own (current-igm-<W>x<H>.pack): the menu's pack is gone once a
 * game loads. On the Konkr the pack lies where neither a game nor the opening transition writes,
 * so the menu reads it once and an opening in a game costs no card read (which held the game long
 * enough for its sound to stutter); on the pairmini that memory is the opening's, so each game
 * reads it at its first opening. */
#include "pico_igm.h"
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
/* the Classic themes have an in-game menu of their own on these consoles: the calls below hand
 * over to it while one is in use (pico_classic_igm.c) */
#include "pico_classic_igm.h"
static int s_igm_classic;
#define CLASSIC_IGM s_igm_classic
#else
#define CLASSIC_IGM 0
#define pico_classic_igm_on() 0
#define pico_classic_igm_open(c) ((void)(c))
#define pico_classic_igm_dismiss() ((void)0)
#define pico_classic_igm_frame(k) ((void)(k), 0)
#define pico_classic_igm_overlay(o) ((void)(o))
#endif
#if defined(KONKR_KPA)
#define IGM_PACK_PA  0x54258000u      /* between the opening's snapshot and its menu layer copy */
#define IGM_PACK_MAX 0x000A8000u      /* 0.6 MB at 960 x 640 */
#define IGM_BG_PA    0x54300000u      /* the panel's background, drawn at each opening */
#define IGM_BG_MAX   0x00140000u      /* up to the live layer's second buffer (0x54440000) */
#define IGM_KEEP     1
#elif defined(KONKR_BLOCK)
/* Konkr Pocket Block (960 x 720): the opening's snapshot and the live layer's first buffer take
 * 0x54000000 with a frame 0x2A3000 long, so the pack and the background go to the memory between
 * the game's first buffer (0x55000000, ending 0x552A3000) and the frozen frame (0x55800000), which
 * neither a game nor an opening writes: kept from one game to the next as on the KPA. */
#define IGM_PACK_PA  0x552B0000u
#define IGM_PACK_MAX 0x00100000u      /* the 960 x 720 pack unpacks to 0.75 MB; ends 0x553B0000 */
#define IGM_BG_PA    0x553C0000u
#define IGM_BG_MAX   0x00200000u      /* ends 0x555C0000 */
#define IGM_KEEP     1
#else
#define IGM_PACK_PA  0x54000000u
#define IGM_PACK_MAX 0x00180000u      /* 1 MB at 1280 x 960 */
#define IGM_BG_PA    0x54180000u
#define IGM_BG_MAX   0x00280000u      /* up to the live layer's second buffer (0x54400000) */
#define IGM_KEEP     0
#endif
#define IGM_ROWS 96                   /* rows in a page, at most (a core's own options: armNES has 91) */
#define IGM_FIG  32                   /* rows whose figures are cached (battery, refresh): the usual
                                       * pages only; 96 bytes each in the static data, where the KPA's
                                       * boot heap has little to spare */
#define IGM_Y0   180.f                /* the first row, units */
#define IGM_RP   44.f                 /* the row pitch, units */

extern void ayaneo_overlay_live(int (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int, int *), int open);
extern int ayaneo_overlay_live_up(void);
extern volatile int g_dbg_key, g_dbg_key_hold;
typedef struct { int kind, item, icon; const char *label; } igm_line;
enum { IC_PLAY, IC_SAVE, IC_LOAD, IC_DISPLAY, IC_SOUND, IC_SPEED, IC_SYSTEM, IC_RESET, IC_EXIT, IC_N, IC_NONE = -1 };
static const char *const k_igm_page[IGM_NPAGES] = { "", "Display", "Sound", "Gameplay", "System", "Core options",
						   "Achievements", "Cheats", "Controls", "Netplay" };
static const signed char k_igm_page_ic[IGM_NPAGES] = { IC_NONE, IC_DISPLAY, IC_SOUND, IC_SPEED, IC_SYSTEM, IC_SYSTEM,
							IC_PLAY, IC_SPEED, IC_SYSTEM, IC_PLAY };

/* RetroAchievements (pico_ra.c, on the consoles with Wi-Fi; absent elsewhere) */
extern int pico_ra_game_loaded(void) __attribute__((weak));
extern int pico_ra_list(void) __attribute__((weak));
extern const char *pico_ra_row_title(int i) __attribute__((weak));
extern const char *pico_ra_row_value(int i) __attribute__((weak));
extern const char *pico_ra_short(void) __attribute__((weak));
extern void pico_ra_paused(void) __attribute__((weak));
extern int pico_ra_row_change(int i) __attribute__((weak));
extern int pico_ra_row_switch(int i) __attribute__((weak));
extern int pico_ra_row_head(int i) __attribute__((weak));
extern int pico_ra_row_tone(int i) __attribute__((weak));
/* the Achievements page's row i is a group's heading (never focused) */
static int igm_row_head(int page, const igm_line *l)
{
	if (page == IGM_CHT) return pico_cheats_row_head(l->item);
	if (page == IGM_CTL) return pico_input_row_head(l->item);
	if (page == IGM_NET) return lui_game_row_head(l->item);
	return page == IGM_ACH && pico_ra_row_head && pico_ra_row_head(l->item);
}
extern const char *pico_ra_row_text(int i) __attribute__((weak));
extern int pico_ra_igm_page(void) __attribute__((weak));
/* the Achievements page: any game of a system RetroAchievements knows (the account and settings,
 * then the game's set when it loaded) */
static int igm_ra_on(void) { return pico_ra_igm_page && pico_ra_igm_page(); }

static struct {
	int loaded, failed;          /* the pack is in memory / could not be read */
	int bg_ok;                   /* the panel's background is drawn for this game */
	uint32_t sum;                /* a hash of its header and table, to know it is still there */
	const struct igm_core *c;
	cfont brand, sub, realm, tag11, tag13, srow, srowf, sval, dname, info, hint, hintc;
	cimg ic[IC_N], chev, back, gamma, ring, pillm;
	char title[128];
	int pw, sh;                  /* the panel's width and its shadow, px */
	float pwu;                   /* the panel's width, units */
	/* the state */
	int up;                      /* the core has it open */
	int closing, latch;          /* playing the close; the buttons to hold back after it */
	float show, show_time;       /* 0..1 in; the seconds since it began opening */
	int page, from, dir;         /* the page, the one before it, +1 deeper -1 back */
	float page_t;                /* 0..1 the page change */
	int sel[IGM_NPAGES];
	float top[IGM_NPAGES];       /* the first row shown, eased */
	float hy;                    /* the focus wash, units from the first row */
	int nudge_row; float nudge_t; int nudge_dir;
	char toast[96]; float toast_t;
	unsigned int tick, last_paint;
	int dirty, prev_keys;
	float rep[4];
	int ext_prev;                /* the width the last picture covered, px */
	int batt, chg;               /* read at the opening and every 20 s (the gauge is slow to ask) */
	unsigned int batt_tick;
	char fig[IGM_FIG][96];      /* the figures rows (battery, refresh): read once a second */
	unsigned int lang_serial;    /* the language pack the fonts' extra glyphs come from */
	unsigned int fig_tick;
	int fig_page;
} G;

/* a hash of each row of the last picture: only the rows that came out different are handed on */
static uint32_t s_igm_rowh[1024];
static int s_igm_rowh_ok;
static int s_tk_want;			/* the focused row's ticker moved: that row again next frame (it alone) */
static int s_igm_only = -1;		/* igm_rows(): draw this row only (the ticker's frames) */
static int s_band_y0 = -1, s_band_y1;	/* igm_paint(): only these lines were drawn again */
static float igm_ease(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : 1 - (1 - t) * (1 - t) * (1 - t); }
static float igm_smooth(float t) { return t <= 0 ? 0 : t >= 1 ? 1 : t * t * (3 - 2 * t); }

static int igm_load(void)
{
	static const char *const ic[IC_N] = { "ig_play", "ig_save", "ig_load", "ig_display", "ig_sound", "ig_speed",
					      "ig_system", "ig_reset", "ig_exit" };
	unsigned int W, H;
	int i;
	ayaneo_canvas_dims(&W, &H);
	if (pack_read("current-igm", W, H, (uint8_t *)IGM_BG_PA, IGM_BG_MAX, (uint8_t *)IGM_PACK_PA, IGM_PACK_MAX) != 0)
		return -1;
	G.brand = font("f_brand"); G.sub = font("f_sub"); G.realm = font("f_realm");
	G.tag11 = font("f_tag11"); G.tag13 = font("f_tag13"); G.srow = font("f_srow"); G.srowf = font("f_srowf");
	G.sval = font("f_sval"); G.dname = font("f_dname"); G.info = font("f_info");
	G.hint = font("f_hint"); G.hintc = font("f_hintc");
	font_names(&G.dname, 0);
	G.lang_serial = pico_lang_serial();
	for (i = 0; i < IC_N; i++) G.ic[i] = img(ic[i]);
	G.chev = img("ig_chev"); G.back = img("ig_back"); G.gamma = img("gamma");
	G.ring = img("hint_ring"); G.pillm = img("hint_pill");
	if (!G.srow.g || !G.dname.g) return -1;
	return 0;
}
/* the fonts take up the glyphs of the language pack in memory now */
static void igm_fonts_extra(void)
{
	font_extra(&G.brand, "f_brand"); font_extra(&G.sub, "f_sub"); font_extra(&G.realm, "f_realm");
	font_extra(&G.tag11, "f_tag11"); font_extra(&G.tag13, "f_tag13"); font_extra(&G.srow, "f_srow");
	font_extra(&G.srowf, "f_srowf"); font_extra(&G.sval, "f_sval"); font_extra(&G.dname, "f_dname");
	font_extra(&G.info, "f_info"); font_extra(&G.hint, "f_hint"); font_extra(&G.hintc, "f_hintc");
	G.lang_serial = pico_lang_serial();
}
static uint32_t igm_sum(void)
{
	const uint32_t *p = (const uint32_t *)IGM_PACK_PA;
	uint32_t h = 2166136261u, n = 4 + (p[2] < 256 ? p[2] : 256) * 12, i;
	for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
	return h;
}
/* the pack is in memory as it was read */
static int igm_valid(void) { return G.loaded && igm_sum() == G.sum; }
/* read the pack (the lookups then point at it: the menu reads its own pack after this) */
static int igm_read(void)
{
	G.loaded = 0;
	if (igm_load() != 0) { G.failed = 1; return -1; }
	G.sum = igm_sum(); G.loaded = 1; G.failed = 0;
	return 0;
}
/* the menu runs: where the pack is kept, read it now unless it is still there */
static void igm_preload(void) { if (IGM_KEEP && !igm_valid()) igm_read(); }
/* a game starts: where the opening takes the pack's memory, the next opening reads it again */
static void igm_forget(void)
{
	G.failed = 0; G.bg_ok = 0;
	if (!IGM_KEEP) G.loaded = 0;
}

int pico_igm_on(void)
{
	unsigned int W, H;
	if (gba_menu_theme() != snes_hash("current")) {
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
		s_igm_classic = pico_classic_igm_on();
		return s_igm_classic;
#else
		return 0;
#endif
	}
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
	s_igm_classic = 0;
#endif
	if (!igm_valid() && (G.failed || igm_read() != 0)) return 0;
	ayaneo_canvas_dims(&W, &H);
	pico_lang_load(W, H);   /* a game started straight from power on: the menu never read it */
	if (G.lang_serial != pico_lang_serial()) igm_fonts_extra();
	return 1;
}

/* the rows of a page: on the top one Resume first */
static int igm_lines(int page, igm_line *out)
{
	const struct igm_core *c = G.c;
	int i, n = 0;
	if (page == IGM_CORE) {
		int k = c->opt_n ? c->opt_n() : 0;
		for (i = 0; i < k && n < IGM_ROWS; i++) {
			out[n].kind = IGM_VALUE; out[n].item = i; out[n].icon = IC_NONE; out[n].label = tr(c->opt_label(i));
			n++;
		}
		return n;
	}
	if (page == IGM_ACH) {	/* the set: each achievement, its state on the right (looked at, not changed) */
		static unsigned int built;	/* the list is made again at most once a second */
		static int k;
		unsigned int now = gpt4_get_current_tick();
		if (!built || now - built > 13000000u) { built = now | 1u; k = igm_ra_on() ? pico_ra_list() : 0; }
		for (i = 0; i < k && n < IGM_ROWS; i++, n++) {
			/* a switch shows Left / Right like a setting (the page's own keys flip it) */
			out[n].kind = pico_ra_row_switch && pico_ra_row_switch(i) >= 0 ? IGM_VALUE : IGM_INFO;
			out[n].item = i; out[n].icon = IC_NONE; out[n].label = pico_ra_row_title(i);
		}
		return n;
	}
	if (page == IGM_CHT) {	/* the cheats: each a switch, under its format's heading */
		int k = pico_cheats_rows();
		for (i = 0; i < k && n < IGM_ROWS; i++, n++) {
			out[n].kind = pico_cheats_row_head(i) ? IGM_INFO : IGM_VALUE;
			out[n].item = i; out[n].icon = IC_NONE; out[n].label = tr(pico_cheats_row_title(i));
		}
		return n;
	}
	if (page == IGM_CTL) {	/* the buttons: what plays each of the game's, the hotkeys */
		int k = pico_input_rows();
		for (i = 0; i < k && n < IGM_ROWS; i++, n++) {
			out[n].kind = pico_input_row_head(i) ? IGM_INFO : pico_input_row_value(i) ? IGM_VALUE : IGM_ACTION;
			out[n].item = i; out[n].icon = IC_NONE; out[n].label = tr(pico_input_row_title(i));
		}
		return n;
	}
	if (page == IGM_NET) {	/* a link cable game's session: host, join, leave */
		int k = lui_game_rows();
		for (i = 0; i < k && n < IGM_ROWS; i++, n++) {
			out[n].kind = lui_game_row_head(i) ? IGM_INFO : lui_game_row_value(i) ? IGM_VALUE : IGM_ACTION;
			out[n].item = i; out[n].icon = IC_NONE; out[n].label = tr(lui_game_row_title(i));
		}
		return n;
	}
	if (page == IGM_TOP) { out[0].kind = IGM_ACTION; out[0].item = -1; out[0].icon = IC_PLAY; out[0].label = tr("Resume"); n = 1; }
	if (page == IGM_TOP && igm_ra_on()) {	/* RetroAchievements: the game's set, under Resume */
		out[n].kind = IGM_LINK; out[n].item = IGM_ACH; out[n].icon = k_igm_page_ic[IGM_ACH];
		out[n].label = k_igm_page[IGM_ACH];
		n++;
	}
	if (page == IGM_TOP && pico_cheats_page()) {	/* the game's cheats, under it */
		out[n].kind = IGM_LINK; out[n].item = IGM_CHT; out[n].icon = k_igm_page_ic[IGM_CHT];
		out[n].label = tr(k_igm_page[IGM_CHT]);
		n++;
	}
	if (page == IGM_TOP && pico_input_page()) {	/* the buttons, under them */
		out[n].kind = IGM_LINK; out[n].item = IGM_CTL; out[n].icon = k_igm_page_ic[IGM_CTL];
		out[n].label = tr(k_igm_page[IGM_CTL]);
		n++;
	}
	if (page == IGM_TOP && lui_game_page()) {	/* a link cable game: playing together from here */
		out[n].kind = IGM_LINK; out[n].item = IGM_NET; out[n].icon = k_igm_page_ic[IGM_NET];
		out[n].label = tr(k_igm_page[IGM_NET]);
		n++;
	}
	for (i = 0; i < c->nrows && n < IGM_ROWS; i++) {
		const struct igm_row *r = &c->rows[i];
		igm_line *l = &out[n];
		if (r->page != page) continue;
		l->kind = r->kind; l->item = r->item; l->icon = IC_NONE; l->label = 0;
		if (r->kind == IGM_LINK) {
			if (r->item == IGM_CORE && (!c->opt_n || !c->opt_n())) continue;
			l->label = tr(k_igm_page[r->item]); l->icon = k_igm_page_ic[r->item];
		} else if (r->item == c->save_item) { l->label = tr("Save state"); l->icon = IC_SAVE; }
		else if (r->item == c->load_item) { l->label = tr("Load state"); l->icon = IC_LOAD; }
		else if (r->item == c->reset_item) { l->label = tr("Reset game"); l->icon = IC_RESET; }
		else if (r->item == c->exit_item) { l->label = tr("Exit game"); l->icon = IC_EXIT; }
		else l->label = tr(c->label(r->item));
		if (page != IGM_TOP) l->icon = IC_NONE;
		n++;
	}
	return n;
}
/* the Achievements page's detail (below, with the cards' look) */
static float s_dt;
static int s_dt_on, s_dt_row = -1;
static unsigned s_dt_tick;
static int igm_detail_w(int ext, int W);
static void igm_detail_paint(void);
static int igm_detail_keys(int up, int dn, int a, int b);
#if !defined(KONKR_KPA) && !defined(KONKR_BLOCK)
static int igm_detail_w(int ext, int W) { (void)W; return ext; }
static void igm_detail_paint(void) {}
static int igm_detail_keys(int up, int dn, int a, int b) { (void)up; (void)dn; (void)a; (void)b; return 0; }
#endif
static igm_line s_igm_ln[IGM_ROWS];   /* the page being drawn (igm_rows), for the figure cache */
/* what a row shows on its right, or 0 */
static const char *igm_value(int page, const igm_line *l, char *buf)
{
	const struct igm_core *c = G.c;
	buf[0] = 0;
	if (page == IGM_CORE) return tr(c->opt_value(l->item, buf));
	if (page == IGM_ACH) return pico_ra_row_value ? pico_ra_row_value(l->item) : 0;
	if (l->kind == IGM_LINK && l->item == IGM_ACH) return pico_ra_short ? pico_ra_short() : 0;
	if (page == IGM_CHT) { const char *v = pico_cheats_row_value(l->item); return v ? tr(v) : 0; }
	if (page == IGM_CTL) return pico_input_row_value(l->item);	/* (in the language already) */
	if (page == IGM_NET) return lui_game_row_value(l->item);	/* (in the language already) */
	if (l->kind == IGM_LINK && l->item == IGM_CHT) {	/* how many are on: "On: 2" */
		const char *v = pico_cheats_short();
		if (!v) return 0;
		snprintf(buf, 96, tr("On: %s"), v);
		return buf;
	}
	if (l->kind == IGM_LINK || l->item < 0) return 0;
	if ((l->item == c->save_item || l->item == c->load_item) && c->slot_item >= 0) {
		char v[96], w[32], *sp;
		c->value(c->slot_item, v);
		sp = strchr(v, ' ');
		if (sp) {	/* "3 used": the slot, then its word with a capital ("Used") */
			*sp = 0;
			snprintf(w, sizeof w, "%c%s", sp[1] >= 'a' && sp[1] <= 'z' ? sp[1] - 32 : sp[1], sp[1] ? sp + 2 : "");
			snprintf(buf, 96, tr("Slot %s \xC2\xB7 %s"), v, tr(w));
		} else snprintf(buf, 96, tr("Slot %s"), v);
		return buf;
	}
	if (l->kind == IGM_ACTION) return 0;
	if (l->kind == IGM_INFO && l->label) {	/* a figure: at most once a second (it may ask the gauge) */
		int r = (int)(l - s_igm_ln);
		if (r >= 0 && r < IGM_FIG) {
			if (!G.fig[r][0] || G.fig_page != page) c->value(l->item, G.fig[r]);
			return tr(G.fig[r]);
		}
	}
	return tr(c->value(l->item, buf));
}

extern volatile unsigned int g_dbg_igm_open_us, g_dbg_igm_open_part[2], g_dbg_igm_glass_us;
void pico_igm_open(const struct igm_core *c)
{
	unsigned int W, H;
	unsigned int t_open = gpt4_get_current_tick();
	if (CLASSIC_IGM) { pico_classic_igm_open(c); return; }
	if (!G.loaded) return;
	ayaneo_canvas_dims(&W, &H);
	g_W = (int)W; g_H = (int)H; g_s = (float)H / 720.0f;
	s_uw = (float)W / g_s;
	s_p = s_uw * .048f > 38 ? s_uw * .048f : 38;
	G.c = c;
	G.up = 1; G.closing = 0; G.latch = 0;
	G.show = 0; G.show_time = 0;
	G.dir = 1; G.page_t = 1;
	{	/* come back to the page and row we left, but start fresh for a different game */
		static uint32_t s_igm_game;
		const char *g = c->game ? c->game : "";
		uint32_t h = 2166136261u;
		int i;
		for (i = 0; g[i] && i < 256; i++) h = (h ^ (uint8_t)g[i]) * 16777619u;
		if (h != s_igm_game) {
			s_igm_game = h;
			G.page = G.from = IGM_TOP;
			memset(G.sel, 0, sizeof G.sel);
			memset(G.top, 0, sizeof G.top);
		} else {
			G.from = G.page;   /* no page slide: we are already there */
		}
	}
	G.hy = (G.sel[G.page] - G.top[G.page]) * IGM_RP; G.nudge_t = 1; G.toast_t = 99; G.toast[0] = 0;
	G.tick = gpt4_get_current_tick(); G.last_paint = 0;
	s_dt = 0; s_dt_on = 0;	/* no detail open from the last time */
	G.dirty = 1; G.prev_keys = 0x3F;   /* what is held now (the menu key, a face button) is not a press */
	g_dbg_igm_open_part[0] = (gpt4_get_current_tick() - t_open) / 13u;
	G.ext_prev = 0; s_igm_rowh_ok = 0;
	G.batt = gba_battery_last(&G.chg); G.batt_tick = G.tick;
	G.fig_tick = 0; G.fig_page = -1;
	{	/* the game's name as the menus draw it, and its glyphs */
		char clean[128], lab[128];
		gba_clean_name(c->game ? c->game : "", clean);
		strncpy(G.title, pico_names_label(clean, lab, sizeof lab), sizeof G.title - 1);
		G.title[sizeof G.title - 1] = 0;
		pico_names_prepare_one(G.title);
		font_names_sync(&G.dname);
	}
	colour_set(g_dbg_colour >= 0 ? g_dbg_colour : ayaneo_get_menu_colour());   /* oem showcolour */
	G.pwu = s_uw * .42f < 470 ? s_uw * .42f : 470;
	G.pw = U(G.pwu); G.sh = U(34);
	if ((uint32_t)(G.pw + G.sh) * H * 4u > IGM_BG_MAX) G.sh = 0;
	g_dbg_igm_open_us = (gpt4_get_current_tick() - t_open) / 13u;
}
volatile unsigned int g_dbg_igm_open_us, g_dbg_igm_open_part[2], g_dbg_igm_glass_us;   /* oem diag: the last opening's setup, the glass */
void pico_igm_dismiss(void)
{
	if (CLASSIC_IGM) { pico_classic_igm_dismiss(); return; }
	if (G.up && !G.closing) { G.closing = 1; G.latch = 0; G.dirty = 1; }
}
static void igm_toast(const char *t)
{
	snprintf(G.toast, sizeof G.toast, "%s", tr(t));
	G.toast_t = 0; G.dirty = 1;
}
/* a page in, deeper (+1) or back (-1) */
static void igm_go(int page, int dir)
{
	if (page == IGM_CHT && page != G.page) pico_cheats_page_open();
	if (page == IGM_CTL && page != G.page) pico_input_page_open();
	if (page == IGM_NET && page != G.page) lui_game_page_open();
	G.from = G.page; G.page = page; G.dir = dir; G.page_t = 0;
	if (dir > 0) { G.sel[page] = 0; G.top[page] = 0; }
	G.hy = (G.sel[page] - G.top[page]) * IGM_RP;
	G.nudge_t = 1; G.dirty = 1;
}
static int igm_fire(int i, int level, float dt)
{
	int f = 0;
	if (level && !(G.prev_keys & (1 << i))) { f = 1; G.rep[i] = .32f; }
	else if (level) { G.rep[i] -= dt; if (G.rep[i] <= 0) { f = 1; G.rep[i] = .07f; } }
	return f;
}
int pico_igm_frame(unsigned int keys)
{
	igm_line ln[IGM_ROWS];
	unsigned int now = gpt4_get_current_tick();
	float dt = (float)(now - G.tick) / 13000000.0f;
	int n, sel, up, dn, lt, rt, a, b, nv;
	if (CLASSIC_IGM) return pico_classic_igm_frame(keys);
	G.tick = now;
	if (dt > .05f) dt = .05f;
	if (!G.up) return 0;
	if (pico_ra_paused) pico_ra_paused();	/* RetroAchievements while the game waits (about once a second) */
	if (g_dbg_key_hold > 0 && g_dbg_key >= 0 && g_dbg_key < 10 && g_dbg_key != 6 && g_dbg_key != 7) {   /* oem nav: an injected key */
		/* LRUDAB, then [ and ] as the shoulders (Select and Start are not the menu's) */
		static const unsigned int map[10] = { IGM_K_LEFT, IGM_K_RIGHT, IGM_K_UP, IGM_K_DOWN, IGM_K_A, IGM_K_B, 0, 0,
						      IGM_K_L, IGM_K_R };
		keys |= map[g_dbg_key];
		g_dbg_key_hold--;
	}
	/* the animations */
	G.show_time += dt;
	if (G.closing) {
		G.show -= dt / .16f;
		if (G.show <= 0) { G.show = 0; G.up = 0; G.prev_keys = (int)keys; return G.latch; }
	} else if (G.show < 1) { G.show += dt / .22f; if (G.show > 1) G.show = 1; }
	if (G.page_t < 1) { G.page_t += dt / .24f; if (G.page_t > 1) G.page_t = 1; }
	if (G.nudge_t < 1) { G.nudge_t += dt / .16f; if (G.nudge_t > 1) G.nudge_t = 1; }
	if (G.toast_t < 9) G.toast_t += dt;
	if (G.c->status && G.c->status[0]) { igm_toast(G.c->status); G.c->status[0] = 0; }
	{	/* the clock in the header turns over */
		static long long shown_min = -1;
		long long m = pico_clock_local() / 60;
		if (m != shown_min) { shown_min = m; G.dirty = 1; }
	}
	if (G.page == IGM_NET) {	/* the Netplay page moves on by itself: a host found, a friend joined */
		static unsigned shown_sig;
		unsigned sig = lui_game_sig();
		if (sig != shown_sig) { shown_sig = sig; G.dirty = 1; s_igm_rowh_ok = 0; }
	}
	n = igm_lines(G.page, ln);
	nv = (int)((720 - 64 - IGM_Y0) / IGM_RP);
	if (G.sel[G.page] >= n) G.sel[G.page] = n ? n - 1 : 0;
	{	/* never on a group's heading (a page opens on its first row, which may be one) */
		int tries;
		for (tries = 0; tries < n && igm_row_head(G.page, &ln[G.sel[G.page]]); tries++)
			G.sel[G.page] = (G.sel[G.page] + 1) % n;
	}
	sel = G.sel[G.page];
	{	/* the list follows the focus; the wash glides to it (both settle in about a tenth of a second) */
		/* the list's goal is a whole row: taken from where it is now rounded, so a press while it
		 * still glides cannot leave it stopped between rows (with the focus out of sight) */
		float k = dt * 18, f = k / (1 + k + k * k * .5f) * (1 + k * .5f), want;
		int w = (int)(G.top[G.page] + .5f);
		if (sel < w) w = sel;
		if (sel > w + nv - 1) w = sel - nv + 1;
		want = (float)(w < 0 ? 0 : w);
		G.top[G.page] += (want - G.top[G.page]) * f;
		if (G.top[G.page] - want < .01f && want - G.top[G.page] < .01f) G.top[G.page] = want;
		{
			float hy = (sel - G.top[G.page]) * IGM_RP;
			G.hy += (hy - G.hy) * f;
			if (G.hy - hy < .05f && hy - G.hy < .05f) G.hy = hy;
		}
	}
	if (G.closing || G.show < .6f) { G.prev_keys = (int)keys; return -1; }
	if (pico_input_busy()) {	/* the Controls page listens for a button: the keys are its */
		pico_input_listen();
		G.prev_keys = (int)keys;
		G.dirty = 1; s_igm_rowh_ok = 0;
		return -1;
	}
	/* the keys */
	up = igm_fire(0, !!(keys & IGM_K_UP), dt);
	dn = igm_fire(1, !!(keys & IGM_K_DOWN), dt);
	lt = igm_fire(2, !!(keys & IGM_K_LEFT), dt);
	rt = igm_fire(3, !!(keys & IGM_K_RIGHT), dt);
	a = (keys & IGM_K_A) && !(G.prev_keys & IGM_K_A);
	b = (keys & IGM_K_B) && !(G.prev_keys & IGM_K_B);
	if (G.page == IGM_CHT) {	/* L / R: a long list's part before or after */
		int sl = (keys & IGM_K_L) && !(G.prev_keys & IGM_K_L), sr = (keys & IGM_K_R) && !(G.prev_keys & IGM_K_R);
		if ((sl || sr) && pico_cheats_step_part(sl ? -1 : 1)) {
			G.sel[G.page] = 0; G.top[G.page] = 0; G.hy = 0;
			G.dirty = 1; s_igm_rowh_ok = 0;
			G.prev_keys = (int)keys;
			return -1;
		}
	}
	G.prev_keys = (int)keys;
	if (G.page == IGM_ACH && igm_detail_keys(up, dn, a, b)) return -1;
	if (!n) { if (b) igm_go(IGM_TOP, -1); return -1; }
	if (up) { G.sel[G.page] = (sel + n - 1) % n; G.dirty = 1; }
	if (dn) { G.sel[G.page] = (sel + 1) % n; G.dirty = 1; }
	if (up || dn) {	/* past the group headings */
		int tries;
		for (tries = 0; tries < n && igm_row_head(G.page, &ln[G.sel[G.page]]); tries++)
			G.sel[G.page] = (G.sel[G.page] + (up ? n - 1 : 1)) % n;
		return -1;
	}
	{
		igm_line *l = &ln[sel];
		const struct igm_core *c = G.c;
		int dir = lt ? -1 : rt ? 1 : 0;
		if (G.page == IGM_ACH) {	/* the settings' switches flip; an achievement opens its detail */
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
			struct pico_ra_detail dd;
			if (a && pico_ra_detail(l->item, &dd) == 0) { s_dt_on = 1; s_dt_row = l->item; s_dt_tick = gpt4_get_current_tick(); G.dirty = 1; return -1; }
#endif
			if ((dir || a) && pico_ra_row_change && pico_ra_row_change(l->item)) {
				G.nudge_row = sel; G.nudge_dir = dir ? dir : 1; G.nudge_t = 0; G.dirty = 1;
			}
			dir = 0; a = 0;
		}
		if (G.page == IGM_CHT) {	/* a cheat flips with Left, Right or A; the parts row steps */
			if ((dir || a) && pico_cheats_row_change(l->item, dir)) {
				G.nudge_row = sel; G.nudge_dir = dir ? dir : 1; G.nudge_t = 0; G.dirty = 1;
				s_igm_rowh_ok = 0;
			}
			dir = 0; a = 0;
		}
		if (G.page == IGM_CTL) {	/* Left / Right step a job's button, A listens for one */
			if ((dir || a) && pico_input_row_change(l->item, dir)) {
				G.nudge_row = sel; G.nudge_dir = dir ? dir : 1; G.nudge_t = 0; G.dirty = 1;
				s_igm_rowh_ok = 0;
			}
			dir = 0; a = 0;
		}
		if (G.page == IGM_NET) {	/* host, join (Left / Right the host), leave */
			if ((dir || a) && lui_game_row_change(l->item, dir)) {
				G.nudge_row = sel; G.nudge_dir = dir ? dir : 1; G.nudge_t = 0; G.dirty = 1;
				s_igm_rowh_ok = 0;
			}
			dir = 0; a = 0;
		}
		if (dir) {
			if (G.page == IGM_CORE) c->opt_change(l->item, dir);
			else if (l->kind == IGM_VALUE) { if (c->change(l->item, dir, 0)) { G.closing = 1; G.latch = 0; } }
			else if ((l->item == c->save_item || l->item == c->load_item) && c->slot_item >= 0 && l->item >= 0)
				c->change(c->slot_item, dir, 0);
			else if (l->kind == IGM_LINK && dir > 0) { igm_go(l->item, 1); return -1; }
			else return -1;
			G.nudge_row = sel; G.nudge_dir = dir; G.nudge_t = 0; G.dirty = 1;
		}
		if (a) {
			if (l->kind == IGM_LINK) igm_go(l->item, 1);
			else if (l->item < 0 && G.page == IGM_TOP) { G.closing = 1; G.latch = 1; }   /* Resume */
			else if (G.page != IGM_CORE && (l->kind == IGM_ACTION || l->kind == IGM_VALUE)) {
				if (c->change(l->item, 0, 1)) {
					if (l->item == c->exit_item) { G.up = 0; G.show = 0; return 1; }   /* the game is ending */
					G.closing = 1; G.latch = 1;
				}
				G.nudge_row = sel; G.nudge_dir = 1; G.nudge_t = l->kind == IGM_VALUE ? 0 : 1;
			}
			G.dirty = 1;
		}
		if (b) {
			if (G.page != IGM_TOP) igm_go(G.page == IGM_CORE ? IGM_PLAY : IGM_TOP, -1);
			else { G.closing = 1; G.latch = 2; }
			G.dirty = 1;
		}
	}
	return -1;
}

/* ---- the picture ---- */
static int s_igm_ox;   /* the panel's slide, px */
#define IX(u) (U(u) + s_igm_ox)
/* the rows of a page, moved dx units and faded to op; the wash on the focus if wash */
static void igm_rows(int page, float dx, int op, int wash)
{
	igm_line own[IGM_ROWS], *ln = page == G.page ? s_igm_ln : own;
	char vb[96], vfit[128];
	int n = igm_lines(page, ln), i, nv = (int)((720 - 64 - IGM_Y0) / IGM_RP), sel = G.sel[page];
	float rx = 18 + dx, rw = G.pwu - 36, top = G.top[page];
	int first = (int)top, xi = IX(rx), wi = U(rw), ph = U(IGM_RP);
	if (op <= 0) return;
	if (wash) {	/* the focus: the accent wash fading out to the right, and its bar */
		int yy = U(IGM_Y0 + G.hy), k;
		for (k = 0; k < wi; k++) {
			int t = k * 1000 / wi, al = t < 860 ? 41 - (35 * t) / 860 : 6 - (6 * (t - 860)) / 140;
			fill(xi + k, yy, 1, ph - U(2), C_ACCENT, (al * op) >> 8);
		}
		fill(xi, yy + U(9), U(2.5f) > 2 ? U(2.5f) : 2, ph - U(20), C_ACCENT, op);
	}
	for (i = first; i < n && i <= first + nv; i++) {
		float ry = IGM_Y0 + (i - top) * IGM_RP, rdx = 0;
		int yy, f = i == sel, rop = op, lx, vx;
		const char *v;
		if (s_igm_only >= 0 && i != s_igm_only) continue;
		if (ry < IGM_Y0 - IGM_RP * .5f || ry > IGM_Y0 + (nv - .5f) * IGM_RP) continue;
		if (!G.closing && G.show_time < .6f && page == G.page) {	/* the opening: the rows arrive one after another */
			float e = igm_ease((G.show_time - .035f * (i - first)) / .2f);
			rdx = (1 - e) * 26; rop = (int)(op * e);
			if (rop <= 0) continue;
		}
		yy = U(ry);
		lx = IX(rx + rdx) + U(14);
		if (igm_row_head(page, &ln[i])) {	/* a group's heading: its name in small capitals, its count */
			const char *cnt = page == IGM_CHT ? pico_cheats_row_value(ln[i].item) : page == IGM_CTL ? "" :
					  page == IGM_NET ? lui_game_row_value(ln[i].item) :
					  pico_ra_row_value ? pico_ra_row_value(ln[i].item) : "";
			text(&G.tag13, lx, yy + (int)(ph * .66f), tr(ln[i].label), C_MUTED, rop, 0, U(2), 1);
			if (cnt && cnt[0]) text(&G.tag13, IX(rx + rw + rdx) - U(14), yy + (int)(ph * .66f), cnt, C_MUTED, rop, 2, 0, 0);
			continue;
		}
		if (ln[i].icon >= 0) {
			const cimg *m = &G.ic[ln[i].icon];
			draw_mask(m, lx, yy + (ph - m->h) / 2, f ? C_ACCENT : C_MUTED, rop);
			lx += m->w + U(14);
		}
		vx = IX(rx + rw + rdx) - U(14);
		/* a link shows no value, except Achievements' count ("12/40", Sign in, Off) */
		v = ln[i].kind == IGM_LINK && ln[i].item != IGM_ACH && ln[i].item != IGM_CHT ? 0 : igm_value(page, &ln[i], vb);
		{	/* the value takes at most 55 % of the row (cut with an ellipsis), the name what is left */
			int arrows = f && v && v[0] && (ln[i].kind == IGM_VALUE || (page != IGM_ACH && page != IGM_CTL && page != IGM_NET && (ln[i].item == G.c->save_item ||
				     ln[i].item == G.c->load_item) && G.c->slot_item >= 0));
			int room = ln[i].kind == IGM_LINK ? G.chev.w + U(16) : U(40);
			if (v && v[0]) {
				v = fit_str(&G.sval, v, U(rw) * 55 / 100 - (arrows ? 2 * G.chev.w + U(12) : 0), vfit, sizeof vfit);
				room = text_w(&G.sval, v, 0, 0) + U(28) + (arrows ? 2 * G.chev.w + U(12) : 0) +
				       (ln[i].kind == IGM_LINK ? G.chev.w + U(10) : 0);
			}
			if (f && wash) {	/* the focused row: its name on the bounce ticker when too long (melonds-nano's) */
				static int tk_sel = -1, tk_page = -1;
				static unsigned tk_t0;
				unsigned tnow = gpt4_get_current_tick();
				const cfont *ff = &G.srowf;
				int box = IX(rx + rw) - lx - room;
				if (tk_sel != sel || tk_page != page) { tk_sel = sel; tk_page = page; tk_t0 = tnow; }
				if (text_w(ff, ln[i].label, 0, 0) > box) {
					text_ticker(ff, lx, yy + (int)(ph * .63f), box, ln[i].label, C_INK, rop,
						    (float)((tnow - tk_t0) / 13000u) / 1000.0f);
					if (!(s_dt_on && s_dt >= 1)) s_tk_want = 1;	/* (it moves: its row again next frame; not under the detail) */
				} else {
					text(ff, lx, yy + (int)(ph * .63f), ln[i].label, C_INK, rop, 0, 0, 0);
				}
			} else
				text_fit(f ? &G.srowf : &G.srow, lx, yy + (int)(ph * .63f), ln[i].label, f ? C_INK : C_MUTED, rop,
					 IX(rx + rw) - lx - room);
		}
		if (ln[i].kind == IGM_LINK) {
			draw_mask(&G.chev, vx - G.chev.w, yy + (ph - G.chev.h) / 2, f ? C_INK : C_MUTED, rop);
			if (v && v[0])
				text(&G.sval, vx - G.chev.w - U(10), yy + (int)(ph * .61f), v, f ? C_INK : C_MUTED, rop, 2, 0, 0);
		} else if (v && v[0]) {
			int nudge = 0, vop = rop, w, pct = -1, len = (int)strlen(v);
			if (i == G.nudge_row && page == G.page && G.nudge_t < 1) {
				float e = igm_ease(G.nudge_t);
				nudge = (int)((1 - e) * U(12)) * G.nudge_dir; vop = (int)(rop * (.35f + .65f * e));
			}
			if (f && (ln[i].kind == IGM_VALUE || (page != IGM_ACH && page != IGM_CTL && page != IGM_NET && (ln[i].item == G.c->save_item || ln[i].item == G.c->load_item)
								&& G.c->slot_item >= 0))) {	/* Left / Right change it */
				draw_mask(&G.chev, vx - G.chev.w, yy + (ph - G.chev.h) / 2, C_INK, rop);
				vx -= G.chev.w + U(6);
			}
			w = text_w(&G.sval, v, 0, 0);
			{	/* an achievement waiting to sync in amber, an active challenge in gold (melonds-nano's) */
				int tone = page == IGM_ACH && pico_ra_row_tone ? pico_ra_row_tone(ln[i].item) : 0;
				uint32_t vc = tone == 1 ? 0xFFC870u : tone == 2 ? 0xFFD36Bu : f ? C_INK : C_MUTED;
				text(&G.sval, vx + nudge, yy + (int)(ph * .61f), v, vc, vop, 2, 0, 0);
			}
			if (f && (ln[i].kind == IGM_VALUE || (page != IGM_ACH && page != IGM_CTL && page != IGM_NET && (ln[i].item == G.c->save_item || ln[i].item == G.c->load_item)
								&& G.c->slot_item >= 0)))
				draw_mask(&G.back, vx - w - U(6) - G.back.w, yy + (ph - G.back.h) / 2, C_INK, rop);
			if (len >= 2 && v[len - 1] == '%' && v[0] >= '0' && v[0] <= '9') {	/* a level: its bar */
				int k = 0, bw = U(64), bx;
				const char *q = v;
				while (*q >= '0' && *q <= '9') k = k * 10 + (*q++ - '0');
				pct = k > 100 ? 100 : k;
				bx = vx - w - U(f ? 32 : 14) - bw;
				fill(bx, yy + ph / 2, bw, U(2.5f) > 2 ? U(2.5f) : 2, C_MUTED, (rop * 70) >> 8);
				fill(bx, yy + ph / 2, bw * pct / 100, U(2.5f) > 2 ? U(2.5f) : 2, f ? C_ACCENT : C_MUTED, rop);
			}
		}
		if (i + 1 < n) fill(IX(rx + rdx) + U(14), yy + ph - 1, wi - U(14), 1, C_MUTED, (rop * 36) >> 8);
	}
	if (s_igm_only >= 0) return;
	if (first > 0) disc(IX(rx + rw) - U(6) + .0f, U(IGM_Y0 - 9) + .0f, U(2.3f) + .0f, C_MUTED, op * 3 / 4);
	if (first + nv < n) disc(IX(rx + rw) - U(6) + .0f, U(IGM_Y0 + nv * IGM_RP + 6) + .0f, U(2.3f) + .0f, C_MUTED, op * 3 / 4);
}
/* ---- the glass ----
 * The drawer is frosted glass: the game behind it, blurred, under the theme's deep tint, its corner
 * lit by the mist and its edge by the accent. Still opaque, so the layer copy stays a plain turn.
 * The game's last frame (ayaneo_game_picture: the core's own small picture and where it goes on
 * the screen) is sampled into a coarse grid over the drawer, the grid is smoothed twice, and it is
 * spread back out at a quarter of the resolution: a wide blur for a fraction of a millisecond. It
 * follows the game up to 30 times a second, and only when the picture behind has changed. The
 * glass stays where it is on the screen while the drawer slides: the game behind is taken at each
 * point's place on the screen, not in the drawer, so the blur fills in under the edge as it comes
 * across (the tint, the mist and the lit edge are the drawer's own and travel with it). */
#define GLASS_C   16                  /* a cell of the grid, px */
#define GLASS_MAX 72                  /* cells across or down, at most */
#define GLASS_QW  160                 /* the quarter resolution picture, at most */
#define GLASS_QH  256
#define GLASS_TINT 180                /* how much of the tint over the game, of 256 */
static struct {
	int gw, gh, ok;
	int ox;                               /* the drawer's slide the glass was painted for, px */
	uint8_t c[GLASS_MAX][GLASS_MAX][3];   /* the blurred game, a cell each */
	unsigned int tick;
} s_gl;
static uint32_t s_tint[GLASS_QH][GLASS_QW];   /* the tint and the mist, at a quarter of the resolution */
extern int ayaneo_game_picture(const unsigned short **pix, unsigned int *sw, unsigned int *sh, unsigned int *pitch, int *rect);

/* sample the game behind the drawer into the grid and smooth it; 1 when it came out different */
static int igm_glass_sample(void)
{
	static int16_t a[GLASS_MAX][GLASS_MAX][3], t[GLASS_MAX][GLASS_MAX][3];
	const unsigned short *pix = 0;
	unsigned int sw = 0, sh = 0, pitch = 0;
	int r[4], gw = (G.pw + GLASS_C - 1) / GLASS_C, gh = (g_H + GLASS_C - 1) / GLASS_C, i, j, k, p, changed = 0;
	int have = ayaneo_game_picture(&pix, &sw, &sh, &pitch, r) && r[2] > 0 && r[3] > 0;
	if (gw > GLASS_MAX) gw = GLASS_MAX;
	if (gh > GLASS_MAX) gh = GLASS_MAX;
	for (j = 0; j < gh; j++)
		for (i = 0; i < gw; i++) {	/* nine points of the cell, averaged; outside the picture black */
			int sr = 0, sg = 0, sb = 0, dx, dy;
			for (dy = 0; dy < 3; dy++)
				for (dx = 0; dx < 3; dx++) {
					int x = i * GLASS_C + (2 * dx + 1) * GLASS_C / 6, y = j * GLASS_C + (2 * dy + 1) * GLASS_C / 6;
					if (have && x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
						unsigned int u = (unsigned)(x - r[0]) * sw / (unsigned)r[2], v = (unsigned)(y - r[1]) * sh / (unsigned)r[3];
						unsigned int c = pix[v * pitch + u];
						sr += (int)((c >> 11) & 31) << 3; sg += (int)((c >> 5) & 63) << 2; sb += (int)(c & 31) << 3;
					}
				}
			a[j][i][0] = (int16_t)(sr / 9); a[j][i][1] = (int16_t)(sg / 9); a[j][i][2] = (int16_t)(sb / 9);
		}
	for (p = 0; p < 2; p++) {	/* 1 2 1 across, then down, twice */
		for (j = 0; j < gh; j++)
			for (i = 0; i < gw; i++)
				for (k = 0; k < 3; k++)
					t[j][i][k] = (int16_t)((a[j][i > 0 ? i - 1 : 0][k] + 2 * a[j][i][k] + a[j][i + 1 < gw ? i + 1 : i][k]) >> 2);
		for (j = 0; j < gh; j++)
			for (i = 0; i < gw; i++)
				for (k = 0; k < 3; k++)
					a[j][i][k] = (int16_t)((t[j > 0 ? j - 1 : 0][i][k] + 2 * t[j][i][k] + t[j + 1 < gh ? j + 1 : j][i][k]) >> 2);
	}
	for (j = 0; j < gh; j++)
		for (i = 0; i < gw; i++)
			for (k = 0; k < 3; k++) {
				int d = a[j][i][k] - s_gl.c[j][i][k];
				if (d > 2 || d < -2) changed = 1;
				s_gl.c[j][i][k] = (uint8_t)a[j][i][k];
			}
	if (!s_gl.ok || gw != s_gl.gw || gh != s_gl.gh) changed = 1;
	s_gl.gw = gw; s_gl.gh = gh; s_gl.ok = 1;
	return changed;
}
/* the drawer's background from the grid and the tint, with the drawer slid ox px (0 or less): each
 * quarter resolution point fills 4 x 4; the points still off the screen are left */
static void igm_glass_paint(int ox)
{
	uint32_t *bg = (uint32_t *)IGM_BG_PA;
	int ext = G.pw + G.sh, qw = (G.pw + 3) / 4, qh = (g_H + 3) / 4, qx, qy, y;
	uint32_t edge = C_ACCENT;
	if (qw > GLASS_QW) qw = GLASS_QW;
	if (qh > GLASS_QH) qh = GLASS_QH;
	s_gl.ox = ox;
	for (qy = 0; qy < qh; qy++) {
		int py = qy * 4 + 2, gy = ((py - GLASS_C / 2) << 8) / GLASS_C, j0, j1, fy;
		if (gy < 0) gy = 0;
		j0 = gy >> 8; fy = gy & 255; j1 = j0 + 1;
		if (j0 >= s_gl.gh - 1) { j0 = j1 = s_gl.gh - 1; fy = 0; }
		for (qx = (-ox) / 4; qx < qw; qx++) {	/* the points before -ox are off the screen */
			int px = qx * 4 + 2, gx = (px + ox - GLASS_C / 2) * 256 / GLASS_C, i0, i1, fx, k, c[3], yy, xx;
			uint32_t tint = s_tint[qy][qx], out;
			if (gx < 0) gx = 0;
			i0 = gx >> 8; fx = gx & 255; i1 = i0 + 1;
			if (i0 >= s_gl.gw - 1) { i0 = i1 = s_gl.gw - 1; fx = 0; }
			for (k = 0; k < 3; k++) {
				int top = s_gl.c[j0][i0][k] * (256 - fx) + s_gl.c[j0][i1][k] * fx;
				int bot = s_gl.c[j1][i0][k] * (256 - fx) + s_gl.c[j1][i1][k] * fx;
				int g = (top * (256 - fy) + bot * fy) >> 16;
				int tc = (int)(tint >> (16 - 8 * k)) & 255;
				c[k] = (g * (256 - GLASS_TINT) + tc * GLASS_TINT) >> 8;
			}
			out = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
			for (yy = qy * 4; yy < qy * 4 + 4 && yy < g_H; yy++) {
				uint32_t *row = bg + yy * ext + qx * 4;
				for (xx = 0; xx < 4 && qx * 4 + xx < G.pw; xx++) row[xx] = out;
			}
		}
	}
	for (y = 0; y < g_H; y++) bg[y * ext + G.pw - 1] = mix(bg[y * ext + G.pw - 1], edge, 70);   /* the lit edge */
}
/* once a game (its memory is the opening's): the tint (the backdrop's deep air, the glow of its
 * mist in the top corner) and the soft shadow off the drawer, then the glass. Drawn at the first
 * picture of an opening, the frame after the menu key, when the game's frame delay has stepped aside. */
static void igm_bg_build(void)
{
	static const gstop air[3] = { { 0, 0x0A1C40, 1 }, { .45f, 0x0A2552, 1 }, { 1, 0x050F24, 1 } };
	uint32_t *bg = (uint32_t *)IGM_BG_PA, mist = recolour(0x2664CCu);
	int ext = G.pw + G.sh, qw = (G.pw + 3) / 4, qh = (g_H + 3) / 4, qx, qy, y, x;
	int R = (int)(G.pw * 1.1f), R2 = R * R;
	unsigned int t0 = gpt4_get_current_tick();
	if (qw > GLASS_QW) qw = GLASS_QW;
	if (qh > GLASS_QH) qh = GLASS_QH;
	for (qy = 0; qy < qh; qy++) {
		float r, g, b, al;
		int py = qy * 4 + 2;
		uint32_t base;
		grad(air, 3, (py + .5f) / g_H, &r, &g, &b, &al);
		base = recolour((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
		for (qx = 0; qx < qw; qx++) {	/* the mist, falling off with the square of the distance */
			int px = qx * 4 + 2, d2 = px * px + py * py;
			uint32_t c = base;
			if (d2 < R2) {
				int f = (int)(((unsigned)(R2 - d2) << 8) / (unsigned)R2), w = (f * f * 70) >> 16;
				if (w > 0) c = mix(0xFF000000u | base, mist, w) & 0xFFFFFFu;
			}
			s_tint[qy][qx] = c;
		}
	}
	for (y = 0; y < g_H; y++)
		for (x = 0; x < G.sh; x++) {	/* the shadow: dark, falling off with the square of the distance */
			int f = 256 - x * 256 / G.sh, sa = (90 * f * f) >> 16;
			bg[y * ext + G.pw + x] = (uint32_t)sa << 24;
		}
	s_gl.ok = 0;
	igm_glass_sample();
	igm_glass_paint(-ext);   /* the first picture has the drawer just off the edge */
	s_gl.tick = gpt4_get_current_tick();
	G.bg_ok = 1;
	g_dbg_igm_open_part[1] = (gpt4_get_current_tick() - t0) / 13u;
}
static int igm_paint_all(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int *box);

volatile unsigned int g_dbg_igm_paint_us[3];	/* oem diag: the in-game menu's last paint, its worst, the hash pass */
static int igm_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int *box)
{
	int y, y0 = -1, y1 = -1, x, w, drawn;
	unsigned int t0 = gpt4_get_current_tick(), t1;
	pico_tr_igm_scope(1);   /* the cores' rows come out in the language in use */
	drawn = igm_paint_all(buf, pitch, W, H, box);
	pico_tr_igm_scope(0);
	t1 = gpt4_get_current_tick();
	if (drawn) {
		g_dbg_igm_paint_us[0] = (t1 - t0) / 13u;
		if (g_dbg_igm_paint_us[0] > g_dbg_igm_paint_us[1]) g_dbg_igm_paint_us[1] = g_dbg_igm_paint_us[0];
	}
	if (!drawn) return 0;
	w = box[2];
	{
		int ya = 0, yb = (int)H;
		if (s_band_y0 >= 0) { ya = s_band_y0; yb = s_band_y1; s_band_y0 = -1; }
	for (y = ya; y < yb && y < 1024; y++) {
		const uint32_t *r = buf + y * W;
		uint32_t h = 2166136261u;
		for (x = 0; x < w; x++) h = (h ^ r[x]) * 16777619u;
		if (!s_igm_rowh_ok || h != s_igm_rowh[y]) { s_igm_rowh[y] = h; if (y0 < 0) y0 = y; y1 = y + 1; }
	}
	}
	s_igm_rowh_ok = 1;
	g_dbg_igm_paint_us[2] = (gpt4_get_current_tick() - t1) / 13u;
	if (y0 < 0) return 0;
	box[1] = y0; box[3] = y1;
	return 1;
}
static int igm_paint_all(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int *box)
{
	unsigned int now = gpt4_get_current_tick();
	float e = igm_ease(G.show);
	int op = e > 0 ? 256 : 0, ext = G.pw + G.sh, y, still;
	still = G.show >= 1 && !G.closing && G.page_t >= 1 && G.nudge_t >= 1 && G.toast_t > 2.2f &&
		G.show_time > .7f;
	{	/* the focus and the list settled */
		float hy = (G.sel[G.page] - G.top[G.page]) * IGM_RP;
		if (G.hy != hy) still = 0;
		if (G.top[G.page] != (float)(int)G.top[G.page]) still = 0;
	}
	if (G.bg_ok && !(s_dt_on && s_dt >= 1) && now - s_gl.tick >= 13000000u / 30u) {	/* the glass follows the game (not under the detail, which hides it) */
		unsigned int tg = gpt4_get_current_tick();
		s_gl.tick = now;
		if (igm_glass_sample()) { igm_glass_paint(s_gl.ox); G.dirty = 1; }
		g_dbg_igm_glass_us = (gpt4_get_current_tick() - tg) / 13u;
	}
	if (still && !G.dirty && s_tk_want && s_dt <= 0 && !s_dt_on && G.bg_ok && now - G.last_paint < 6500000u) {
		/* only the ticker moved: its row alone, over the glass as it is (the whole drawer a frame
		 * was the game's time under it) */
		const uint32_t *bg = (const uint32_t *)IGM_BG_PA;
		int sel = G.sel[G.page], b0, b1;
		g_fb = buf; g_W = (int)W; g_H = (int)H; g_track = 0;
		s_igm_ox = 0;
		b0 = U(IGM_Y0 + (sel - G.top[G.page]) * IGM_RP); b1 = b0 + U(IGM_RP);
		if (b0 < 0) b0 = 0;
		if (b1 > (int)H) b1 = (int)H;
		for (y = b0; y < b1; y++) memcpy(buf + y * W, bg + y * ext, (size_t)ext * 4);
		s_tk_want = 0;
		s_igm_only = sel;
		igm_rows(G.page, 0, op, 1);
		s_igm_only = -1;
		box[0] = 0; box[1] = b0; box[2] = ext; box[3] = b1;
		s_band_y0 = b0; s_band_y1 = b1;
		return 1;
	}
	if (still && !G.dirty && now - G.last_paint < 6500000u) return 0;   /* live figures: twice a second */
	if (!G.bg_ok) { igm_bg_build(); return 0; }   /* its own frame: the first picture comes next */
	G.dirty = 0; G.last_paint = now; s_tk_want = 0;	/* (the rows ask again if the ticker runs) */
	if (now - G.fig_tick > 13000000u || G.fig_page != G.page) {	/* the figures: once a second */
		int i;
		for (i = 0; i < IGM_FIG; i++) G.fig[i][0] = 0;
		G.fig_tick = now; G.fig_page = G.page;
	}
	if (now - G.batt_tick > 13000000u * 20) { G.batt = gba_battery_last(&G.chg); G.batt_tick = now; }
	g_fb = buf; g_W = (int)W; g_H = (int)H; g_track = 0;
	(void)pitch;
	{
		int want = igm_detail_w(ext, (int)W), clr = want > G.ext_prev ? want : G.ext_prev;
		for (y = 0; y < (int)H; y++) memset(buf + y * W, 0, (size_t)clr * 4);
		box[0] = 0; box[1] = 0; box[2] = clr; box[3] = (int)H;
		G.ext_prev = want;
	}
	if (op <= 0) return 1;
	s_igm_ox = -(int)((1 - e) * ext + .5f);   /* the drawer slides in from beyond the edge, its shadow with it */
	if (s_igm_ox <= -ext) return 1;
	if (s_igm_ox != s_gl.ox) {	/* sliding: the glass again for where the drawer is now */
		unsigned int tg = gpt4_get_current_tick();
		igm_glass_paint(s_igm_ox);
		g_dbg_igm_glass_us = (gpt4_get_current_tick() - tg) / 13u;
	}
	{	/* the background */
		const uint32_t *bg = (const uint32_t *)IGM_BG_PA;
		int x0 = -s_igm_ox, n = ext - x0;
		for (y = 0; y < (int)H; y++) memcpy(buf + y * W, bg + y * ext + x0, (size_t)n * 4);
	}
	{	/* the header: the brand, the battery */
		float px = 30;
		int x = IX(px), w, pct, chg = 0;
		draw_mask(&G.gamma, x, U(28), C_INK, op);
		w = text(&G.brand, x + U(31), U(47), "GammaOS", C_INK, op, 0, 0, 0);
		text(&G.sub, x + U(31) + w + U(7), U(47), "Pico", C_MUTED, op, 0, 0, 0);
		pct = G.batt; chg = G.chg;
		if (pct >= 0) {
			char t[8];
			int tx = IX(G.pwu - 30), bx, r = U(1.25f) > 0 ? U(1.25f) : 1;
			snprintf(t, sizeof t, "%d%%", pct);
			w = text(&G.realm, tx, U(46), t, C_INK, op, 2, 0, 0);
			bx = tx - w - U(37);
			fill(bx, U(34), U(25), r, C_MUTED, op); fill(bx, U(45) - r, U(25), r, C_MUTED, op);
			fill(bx, U(34), r, U(11), C_MUTED, op); fill(bx + U(25) - r, U(34), r, U(11), C_MUTED, op);
			fill(bx + U(3), U(37), (U(19) * pct) / 100, U(5), chg ? C_ACCENT : C_MUTED, op);
			fill(bx + U(26), U(37), U(2), U(5), C_MUTED, op);
			{	/* the time, left of the battery */
				char tm[32];
				pico_clock_time_text(pico_clock_local(), tm, sizeof tm);
				text(&G.realm, bx - U(14), U(46), tm, C_MUTED, op, 2, 0, 0);
			}
		}
	}
	{	/* the system, and the game or the page */
		const struct igm_core *c = G.c;
		float k = igm_smooth(G.page_t);
		int sys = c->system >= 0 && c->system < NSYS ? c->system : 0, w;
		const char *now_t = G.page == IGM_TOP ? G.title : tr(k_igm_page[G.page]);
		const char *was_t = G.from == IGM_TOP ? G.title : tr(k_igm_page[G.from]);
		text(&G.tag13, IX(30), U(106), tr(s_sysname[sys]), C_MUTED, op, 0, U(2), 1);
		w = U(G.pwu - 60);
		if (k < 1 && G.from != G.page) {
			int o = (int)(op * (1 - k));
			if (o > 0) {
				int bx = IX(30 - G.dir * 40 * k) + (G.from != IGM_TOP ? G.back.w + U(8) : 0);
				if (G.from != IGM_TOP) draw_mask(&G.back, IX(30 - G.dir * 40 * k) - U(3), U(143 - 10.5f) - G.back.h / 2, C_MUTED, o);
				text_fit(&G.dname, bx, U(143), was_t, C_INK, o, w);
			}
		}
		{
			int o = (int)(op * (G.from != G.page ? k : 1)), bx = IX(30 + G.dir * 40 * (1 - k));
			if (G.from == G.page) bx = IX(30);
			if (G.page != IGM_TOP) {
				draw_mask(&G.back, bx - U(3), U(143 - 10.5f) - G.back.h / 2, C_MUTED, o);
				bx += G.back.w + U(8);
			}
			text_fit(&G.dname, bx, U(143), now_t, C_INK, o, w);
		}
		fill(IX(30), U(162), U(G.pwu - 60), 1, C_MUTED, (op * 50) >> 8);
	}
	{	/* the rows: the page, and while it changes the one it came from */
		float k = igm_smooth(G.page_t);
		if (k < 1 && G.from != G.page) {
			igm_rows(G.from, -G.dir * 56 * k, (int)(op * (1 - k)), 0);
			igm_rows(G.page, G.dir * 56 * (1 - k), (int)(op * k), 1);
		} else igm_rows(G.page, 0, op, 1);
	}
	{	/* the hints */
		const char *codes[2] = { "A", "B" };
		const char *labels[2] = { "Select", G.page == IGM_TOP ? "Resume" : "Back" };
		int i, xx = IX(30), cy = U(720 - 27);
		for (i = 0; i < 2; i++) {
			int rad = U(8.3f);
			draw_mask(&G.ring, xx, cy - G.ring.h / 2, C_MUTED, op);
			text(&G.hintc, xx + rad, U(720 - 22.9f), codes[i], C_INK, op, 1, 0, 0);
			xx += 2 * rad + U(8);
			xx += text(&G.hint, xx, U(720 - 22), tr(labels[i]), C_MUTED, op, 0, 0, 0) + U(16);
		}
		if (G.page == IGM_CHT) {	/* the focused cheat's code */
			int s = G.sel[IGM_CHT];
			const char *d = s >= 0 && s < IGM_ROWS ? pico_cheats_row_text(s_igm_ln[s].item) : "";
			if (d && d[0])
				text_fit(&G.info, xx + U(8), U(720 - 22), d, C_MUTED, op, IX(30) + U(G.pwu - 60) - xx - U(8));
		}
		if (G.page == IGM_ACH && pico_ra_row_text) {	/* the focused row's words: what to do for it */
			int s = G.sel[IGM_ACH];
			const char *d = s >= 0 && s < IGM_ROWS ? pico_ra_row_text(s_igm_ln[s].item) : "";
			if (d && d[0])
				text_fit(&G.info, xx + U(8), U(720 - 22), d, C_MUTED, op, IX(30) + U(G.pwu - 60) - xx - U(8));
		}
	}
	if (G.toast[0] && G.toast_t < 2.2f) {	/* a note: up and in, then out */
		float t = G.toast_t, in = igm_ease(t / .18f), out = t > 1.9f ? 1 - (t - 1.9f) / .3f : 1;
		int o = (int)(op * in * (out < 0 ? 0 : out)), tw = text_w(&G.info, G.toast, 0, 0), ty = U(720 - 82) + (int)((1 - in) * U(10));
		if (o > 0) {
			pill_fill(IX(30), ty, tw + U(36), U(34), C_ACCENT, (o * 46) >> 8);
			disc(IX(30) + U(17) + .0f, ty + U(17) + .0f, U(3) + .0f, C_ACCENT, o);
			text(&G.info, IX(30) + U(28), ty + U(22), G.toast, C_INK, o, 0, 0, 0);
		}
	}
	igm_detail_paint();
	return 1;
}
#if PICO_LINK
/* ---- the start of a session, in this theme's look (pico_link_ui.h lui_set_painter) ----
 * While the states (and maybe the cartridges) cross, the game is loading: the menu's pack is
 * gone (its memory is the game's), so the screen is drawn with the in-game menu's own pack, kept
 * where no game writes, straight onto the canvas: the header, "Playing together" large, the
 * games and the other console, the step, a bar with how far it is, and B to cancel. */
static void prog_bytes(char *d, unsigned int cap, unsigned int b)
{
	if (b >= 1024u * 1024u) snprintf(d, cap, "%u.%u MB", b / (1024u * 1024u), (b % (1024u * 1024u)) * 10u / (1024u * 1024u));
	else snprintf(d, cap, "%u KB", (b + 1023u) / 1024u);
}
static int link_paint(const struct lui_progress *p)
{
	unsigned int pitch, W, H, y;
	unsigned int *fb;
	snes_input in;
	static int prev_b = 1;
	int cancel = 0;
	char t[160], a[24], b[24];
	float x = s_p, top = 720 * .34f;
	memset(&in, 0, sizeof in);
	gba_menu_read_input(&in);
	if (in.b && !prev_b) cancel = 1;	/* B pressed now (not still held from before) */
	prev_b = in.b;
	if (!igm_valid() && igm_read() != 0)
		return cancel;			/* no pack to draw with: the start goes on unseen */
	fb = ayaneo_canvas_back(&pitch, &W, &H);
	if (!fb) return cancel;
	g_fb = fb; g_W = (int)pitch; g_H = (int)H; g_track = 0;
	/* a deep wash of the theme's colour, darker at the bottom. So dark a slope has only a few
	 * dozen levels per channel over the whole height, each channel stepping on its own rows: as
	 * plain rows that showed as bands. Each channel is worked out finer (1/65536 of a level) and
	 * an ordered 4x4 pattern decides, pixel by pixel, between the two levels around it. */
	for (y = 0; y < H; y++) {
		static const unsigned char bayer[4][4] = {
			{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
		unsigned int *r = fb + y * pitch, k, c[4];
		int wt = 34 * 256 - (int)(y * 26u * 256u / H), ch, j;	/* the accent's share, in 1/65536 */
		for (j = 0; j < 4; j++) {
			c[j] = 0xFF000000u;
			for (ch = 0; ch < 24; ch += 8) {
				int lo = (int)((0x050507u >> ch) & 0xFFu), hi = (int)((C_ACCENT >> ch) & 0xFFu);
				int v = (lo * 65536 + (hi - lo) * wt + bayer[y & 3][j] * 4096) >> 16;
				c[j] |= (unsigned int)(v < 0 ? 0 : v > 255 ? 255 : v) << ch;
			}
		}
		for (k = 0; k < W; k++) r[k] = c[k & 3];
	}
	{	/* the header, as the menu's */
		int hx = U(x), w;
		draw_mask(&G.gamma, hx, U(28), C_INK, 256);
		w = text(&G.brand, hx + U(31), U(47), "GammaOS", C_INK, 256, 0, 0, 0);
		w = hx + U(31) + w + U(8);
		w += text(&G.sub, w, U(47), "Pico", C_MUTED, 256, 0, 0, 0);
		fill(w + U(14), U(31), 1, U(17), C_MUTED, 115);
		text(&G.realm, w + U(32), U(46), p->realm ? p->realm : tr("Link"), C_MUTED, 256, 0, 0, 0);
	}
	draw_mask(&G.ring, U(x), U(top - 70), C_ACCENT, 256);
	draw_mask(&G.ring, U(x) + G.ring.w * 2 / 3, U(top - 70), C_ACCENT, 256);
	text_fit(&G.dname, U(x), U(top), p->title, C_INK, 256, U(s_uw - 2 * x));
	if (p->theirs) snprintf(t, sizeof t, "%s  +  %s", p->mine, p->theirs);
	else snprintf(t, sizeof t, "%s", p->mine);
	text_fit(&G.srowf, U(x), U(top + 46), t, C_INK, 256, U(s_uw - 2 * x));
	if (p->other) {
		snprintf(t, sizeof t, tr("With %s"), p->other);
		text_fit(&G.info, U(x), U(top + 78), t, C_MUTED, 256, U(s_uw - 2 * x));
	}
	text_fit(&G.srow, U(x), U(top + 150), p->step, C_INK, 256, U(s_uw - 2 * x));
	{	/* the bar, and how far, how fast, how long */
		int bx = U(x), bw = U(s_uw - 2 * x), by = U(top + 172), fillw = 0;
		fill(bx, by, bw, U(4), C_MUTED, 64);
		if (p->total) {
			fillw = (int)((unsigned long long)bw * (p->done < p->total ? p->done : p->total) / p->total);
			fill(bx, by, fillw, U(4), C_ACCENT, 256);
			if (p->items) { snprintf(a, sizeof a, "%u", p->done); snprintf(b, sizeof b, "%u", p->total); }
			else { prog_bytes(a, sizeof a, p->done); prog_bytes(b, sizeof b, p->total); }
			if (p->kbs && p->secs_left)
				snprintf(t, sizeof t, tr("%s of %s, %u KB/s, %u s left"), a, b, p->kbs, p->secs_left);
			else
				snprintf(t, sizeof t, tr("%s of %s"), a, b);
			text_fit(&G.info, bx, U(top + 208), t, C_MUTED, 256, bw);
		} else {	/* waiting: a short stretch that moves along */
			int span = bw / 5, ph = (int)((gpt4_get_current_tick() / 130000u) % 100u);
			fill(bx + (bw - span) * ph / 100, by, span, U(4), C_ACCENT, 256);
		}
	}
	{	/* B to cancel, as the menu's hints */
		int cy = U(720 - 27), rad = U(8.3f), hx = U(x);
		draw_mask(&G.ring, hx, cy - G.ring.h / 2, C_MUTED, 256);
		text(&G.hintc, hx + rad, U(720 - 22.9f), "B", C_INK, 256, 1, 0, 0);
		text(&G.hint, hx + 2 * rad + U(8), U(720 - 22), p->hint ? p->hint : tr("Cancel"), C_MUTED, 256, 0, 0, 0);
	}
	ayaneo_canvas_present();
	return cancel;
}
#endif

#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
/* ---- RetroAchievements' cards over the game, in the theme's own look -------------------------------
 * While the in-game menu is shut, a card (an unlock, mastery, the game's start, a note) goes on the
 * menu's live layer, drawn by the theme in use: here the Current theme's dark rounded panel, its
 * spaced capitals for the heading (coloured by the kind of card), its row and info fonts, the badge
 * at the left; the Classic theme draws its own (pico_classic_igm.c). The card fades in and out
 * with its alpha. Drawn on the layer, it never touches the game's picture. */
static struct pico_ra_card s_cd;		/* the card shown now */
static int s_cd_up;			/* the layer carries a card */
static int s_layer_menu;		/* the layer last carried the menu (a card takes it only once it is down) */
static int s_cd_fresh;			/* its first frame: its columns cleared top to bottom */
static int s_cd_was[4];			/* where the last card frame drew (x0 y0 x1 y1) */
volatile int g_ra_card_on_layer;		/* (pico_disp.inc leaves the game's picture alone) */
volatile int g_ra_theme_cards;			/* a theme carries the cards (the old ones never come back then) */

/* an antialiased rounded rectangle, corners of radius r */
static void rrect_fill(int x, int y, int w, int h, float r, uint32_t rgb, int op)
{
	int i, j;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			float px = i + .5f, py = j + .5f, cx = px < r ? r : px > w - r ? w - r : px;
			float cy = py < r ? r : py > h - r ? h - r : py, dx = px - cx, dy = py - cy, d;
			if (dx == 0 && dy == 0) { put(x + i, y + j, rgb, op); continue; }	/* inside: no root to take */
			d = r - sq(dx * dx + dy * dy) + .5f;
			if (d <= 0) continue;
			put(x + i, y + j, rgb, d >= 1 ? op : (int)(d * op));
		}
}

/* the card's pixels faded by a 0..255 (colour and alpha: the layer's pixels are premultiplied) */
static void fade_box(uint32_t *buf, int pitch, const int *b, unsigned a)
{
	int x, y;
	if (a >= 255) return;
	for (y = b[1]; y < b[3]; y++) {
		uint32_t *r = buf + y * pitch;
		for (x = b[0]; x < b[2]; x++) {
			uint32_t v = r[x], rb = ((v & 0x00FF00FFu) * a >> 8) & 0x00FF00FFu, ag = (((v >> 8) & 0x00FF00FFu) * a) & 0xFF00FF00u;
			r[x] = ag | rb;
		}
	}
}

/* melonds-nano's cards (RaCards), in this theme's fonts: the drawer's dark blue air, solid, a light
 * rim, the accent's wash from the left and its stripe down the left edge, the badge in a dark box,
 * the header in spaced capitals in the accent with the points in a chip at its right, the title
 * large, the description under it; gold for unlocks and mastery (mastery larger), the mode's colour
 * for a game's start, amber for a note, the leaderboards' own; the progress as a pill (the badge,
 * "23/50", a bar); each leaderboard's running value in a tracker at the bottom right, its digits in
 * cells of one width so a count does not jitter. All at the top right, over the game. */
#define RA_AIR    0x0B1D42u
#define RA_GOLD   0xFFD36Bu
#define RA_GREEN  0x8FE3B0u
#define RA_RED    0xFF9C9Cu
#define RA_AMBER  0xFFC870u

/* a rounded rectangle of radius r whose opacity falls from op at its left to 0 at fade_w px (0: flat) */
static void rrect_fade(int x, int y, int w, int h, float r, uint32_t rgb, int op, int fade_w)
{
	int i, j;
	if (r < .5f && !g_cfl && !g_cfr) {
		/* square corners (the detail panel's whole screen ground): the same pixels without the
		 * corner sums and a put per pixel, each row one run (it took 18 ms of a 20 ms paint) */
		int n = fade_w && fade_w < w ? fade_w : w, i0 = 0, i1 = n;
		if (x + i0 < 0) i0 = -x;
		if (x + i1 > g_W) i1 = g_W - x;
		if (g_cx1) {
			if (x + i0 < g_cx0) i0 = g_cx0 - x;
			if (x + i1 > g_cx1) i1 = g_cx1 - x;
		}
		if (i1 <= i0 || op <= 0) return;
		for (j = 0; j < h; j++) {
			int yy = y + j;
			uint32_t *d;
			if ((unsigned)yy >= (unsigned)g_H) continue;
			touch(yy, x + i0, x + i1);
			d = g_fb + yy * g_W + x;
			for (i = i0; i < i1; i++) {
				int a = fade_w ? op * (fade_w - i) / fade_w : op;
				if (a <= 0) break;
				d[i] = a >= 256 ? (0xFF000000u | rgb) : mix(d[i], rgb, a);
			}
		}
		return;
	}
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			float px = i + .5f, py = j + .5f, cx = px < r ? r : px > w - r ? w - r : px;
			float cy = py < r ? r : py > h - r ? h - r : py, dx = px - cx, dy = py - cy, d;
			int o = op;
			if (fade_w) { if (i >= fade_w) break; o = op * (fade_w - i) / fade_w; }
			d = dx == 0 && dy == 0 ? 1 : r - sq(dx * dx + dy * dy) + .5f;	/* (inside: no root to take) */
			if (d <= 0) continue;
			put(x + i, y + j, rgb, d >= 1 ? o : (int)(d * o));
		}
}
/* the cards' ground (melonds-nano's Ground): rim, air, the accent's wash and stripe */
static void ra_ground(int x, int y, int w, int h, int r, uint32_t accent, int stripe)
{
	int px = 1;
	rrect_fill(x, y, w, h, (float)r, C_MUTED, 61);
	rrect_fill(x + px, y + px, w - 2 * px, h - 2 * px, (float)(r - px), RA_AIR, 256);
	if (!accent) return;
	rrect_fade(x + px, y + px, w - 2 * px, h - 2 * px, (float)(r - px), accent, 41, w * 62 / 100);
	if (stripe) {	/* the lit left edge, along the corners' curve */
		g_cx0 = x; g_cx1 = x + U(3) + 1;
		rrect_fill(x, y, 2 * r + U(6), h, (float)r, accent, 256);
		g_cx0 = g_cx1 = 0;
	}
}
/* the badge (48 x 48 RGB565) at size px, bilinear, on its dark backing; a trophy in its place */
static void ra_pic(int x, int y, int size, const unsigned short *px, uint32_t accent)
{
	int i, j;
	rrect_fill(x - U(2), y - U(2), size + 2 * U(2), size + 2 * U(2), (float)U(7), 0, 90);
	if (px) {
		for (j = 0; j < size; j++) {
			float sy = (j + .5f) * PICO_RA_BADGE / size - .5f;
			int y0 = (int)sy, y1;
			float fy = sy - y0;
			if (sy < 0) { y0 = 0; fy = 0; }
			y1 = y0 + 1 < PICO_RA_BADGE ? y0 + 1 : y0;
			for (i = 0; i < size; i++) {
				float sx = (i + .5f) * PICO_RA_BADGE / size - .5f;
				int x0 = (int)sx, x1, k;
				float fx = sx - x0;
				uint32_t a, b2, c2, d2, o = 0;
				if (sx < 0) { x0 = 0; fx = 0; }
				x1 = x0 + 1 < PICO_RA_BADGE ? x0 + 1 : x0;
				a = from565(px[y0 * PICO_RA_BADGE + x0]); b2 = from565(px[y0 * PICO_RA_BADGE + x1]);
				c2 = from565(px[y1 * PICO_RA_BADGE + x0]); d2 = from565(px[y1 * PICO_RA_BADGE + x1]);
				for (k = 0; k < 24; k += 8) {
					float t = ((a >> k) & 255) * (1 - fx) + ((b2 >> k) & 255) * fx;
					float u = ((c2 >> k) & 255) * (1 - fx) + ((d2 >> k) & 255) * fx;
					o |= (uint32_t)(t * (1 - fy) + u * fy + .5f) << k;
				}
				put(x + i, y + j, o, 256);
			}
		}
		return;
	}
	rrect_fill(x, y, size, size, (float)U(6), accent, 36);
	{	/* a trophy: the cup, its stem and foot */
		int cw = size * 46 / 100, cx = x + (size - cw) / 2, cy = y + size * 24 / 100;
		rrect_fill(cx, cy, cw, size * 30 / 100, (float)(cw / 3), accent, 256);
		fill(x + size / 2 - size / 32, cy + size * 28 / 100, size / 16 + 1, size * 14 / 100, accent, 256);
		rrect_fill(x + size * 34 / 100, cy + size * 42 / 100, size * 32 / 100, size / 12 + 1, 2.0f, accent, 256);
	}
}
/* the badge as the cards draw it, into another picture (the Classic in-game menu's detail): fb W x H,
 * size px at x, y; badge "" or not in yet: the trophy in accent */
/* a badge drawn at x, y (size px) turned grey and darker: a locked achievement's look (the server's
 * locked badges are the same picture so), a failed challenge's */
static void ra_grey(int x, int y, int size)
{
	int i, j;
	for (j = 0; j < size; j++) {
		uint32_t *d;
		if (y + j < 0 || y + j >= g_H) continue;
		d = g_fb + (y + j) * g_W + x;
		for (i = 0; i < size; i++) {
			uint32_t v, l;
			if (x + i < 0 || x + i >= g_W) continue;
			v = d[i]; l = (((v >> 16) & 255) * 77 + ((v >> 8) & 255) * 150 + (v & 255) * 29) >> 8;
			l = l * 3 / 4;
			d[i] = (v & 0xFF000000u) | l << 16 | l << 8 | l;
		}
	}
}
/* (grey: a locked achievement's, when its picture is in) */
void pico_ra_badge_paint(uint32_t *fb, int W, int H, int x, int y, int size, const char *badge, uint32_t accent, int grey)
{
	uint32_t *ofb = g_fb;
	int oW = g_W, oH = g_H, otr = g_track, ocx0 = g_cx0, ocx1 = g_cx1;
	float os = g_s;
	g_fb = fb; g_W = W; g_H = H; g_s = (float)H / 720.0f; g_track = 0; g_cx0 = g_cx1 = 0;
	{
		const unsigned short *px = badge && badge[0] ? pico_ra_badge(badge) : 0;
		ra_pic(x, y, size, px, accent);
		if (grey && px) ra_grey(x, y, size);
	}
	g_fb = ofb; g_W = oW; g_H = oH; g_s = os; g_track = otr; g_cx0 = ocx0; g_cx1 = ocx1;
}
/* the cards' times: when the card in hand came (its title's ticker starts there) */
static unsigned s_cd_tick;
static char s_cd_key[96];
static int s_cd_tick_long;			/* its title is on the ticker (drawn each frame while so) */

/* the cards' words: in the Current theme its pack's fonts (Inter, as melonds-nano's), in a Classic
 * theme the kit's (pico_classic_igm.c), in panel pixels either way */
enum { RF_HEAD, RF_TITLE, RF_BODY, RF_FIG, RF_CHIP, RF_N };
static int s_rf_classic;
static const signed char k_rf_cf[RF_N] = { 2, 0, 1, 1, 2 };		/* CF_S, CF_L, CF_M, CF_M, CF_S */
static const float k_rf_sc[RF_N] = { .78f, .74f, .8f, .86f, .74f };
static const cfont *rf_cur(int r)
{
	return r == RF_HEAD ? &G.tag13 : r == RF_TITLE ? &G.dname : r == RF_BODY ? &G.srow : r == RF_FIG ? &G.srowf : &G.sval;
}
static const char *rf_up(const char *s, int upper, char *b, int cap)
{
	int i;
	if (!upper) return s;
	for (i = 0; s[i] && i < cap - 1; i++) b[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
	b[i] = 0;
	return b;
}
static int rf_w(int r, const char *s, int ls, int upper)
{
	char b[160];
	if (!s_rf_classic) return text_w(rf_cur(r), s, ls, upper);
	return pico_classic_ra_text_w(k_rf_cf[r], k_rf_sc[r], rf_up(s, upper, b, sizeof b));
}
static void rf_text(int r, int x, int y, const char *s, uint32_t rgb, int anchor, int ls, int upper)
{
	char b[160];
	if (!s_rf_classic) { text(rf_cur(r), x, y, s, rgb, 256, anchor, ls, upper); return; }
	pico_classic_ra_text(k_rf_cf[r], k_rf_sc[r], x, y, rf_up(s, upper, b, sizeof b), rgb, anchor);
}
static int rf_asc(int r) { return s_rf_classic ? pico_classic_ra_asc(k_rf_cf[r], k_rf_sc[r]) : rf_cur(r)->asc; }
static int rf_px(int r) { return s_rf_classic ? pico_classic_ra_px(k_rf_cf[r], k_rf_sc[r]) : rf_cur(r)->px; }
/* s cut with an ellipsis to max_w px (into b) */
static const char *rf_fit(int r, const char *s, int max_w, char *b, int cap)
{
	int n = (int)strlen(s);
	if (rf_w(r, s, 0, 0) <= max_w || n >= cap - 4) return s;
	memcpy(b, s, (size_t)n + 1);
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80) n--;
		b[n] = (char)0xE2; b[n + 1] = (char)0x80; b[n + 2] = (char)0xA6; b[n + 3] = 0;
		if (rf_w(r, b, 0, 0) <= max_w) break;
	}
	return b;
}
/* the title: on the ticker when wider than its box (Current), cut short in the Classic fonts */
static void rf_ticker(int r, int x, int y, int box, const char *s, uint32_t rgb, float t)
{
	char b[192];
	if (!s_rf_classic) { text_ticker(rf_cur(r), x, y, box, s, rgb, 256, t); return; }
	rf_text(r, x, y, rf_fit(r, s, box, b, sizeof b), rgb, 0, 0, 0);
}
/* two lines at most (Current), one cut short (Classic) */
static void rf_wrap2(int r, int x, int y, int lh, const char *s, uint32_t rgb, int max_w)
{
	char b[192];
	if (!s_rf_classic) { text_wrap2(rf_cur(r), x, y, lh, s, rgb, 256, max_w); return; }
	rf_text(r, x, y, rf_fit(r, s, max_w, b, sizeof b), rgb, 0, 0, 0);
}

/* one card at the top right: its box into r (x0 y0 x1 y1) */
static void ra_card(const struct pico_ra_card *c, int *r, float t)
{
	int big = c->kind == PICO_RA_CARD_MASTERED, P = U(big ? 92 : 72), x0, right = U(16), pad = U(big ? 16 : 13);
	int W, H, colW, x, y, chipw = 0, hb, tb, lb = 0, nb = 0, rad = U(big ? 16 : 13), lines = 0, sp = s_rf_classic ? 0 : U(1.2f);
	uint32_t acc = c->kind == PICO_RA_CARD_UNLOCK || big ? RA_GOLD : c->kind == PICO_RA_CARD_GAME ?
		       (c->hardcore ? RA_GOLD : C_ACCENT) :
		       c->kind == PICO_RA_CARD_NOTE ? (c->pct < 0 ? RA_RED : c->pct == 1 ? RA_GREEN : RA_AMBER) :
		       c->kind == PICO_RA_CARD_BOARD ? (c->pct < 0 ? RA_RED : C_ACCENT) : C_ACCENT;
	char chip[32], hb_s[64];
	const unsigned short *pic = c->badge[0] ? pico_ra_badge(c->badge) : 0;
	x0 = c->plain ? U(19) : U(19) + P + U(16);	/* a plain note: no picture, its words from the edge */
	chip[0] = 0;
	if (c->chip[0]) snprintf(chip, sizeof chip, "%s", c->chip);	/* (a leaderboard's score) */
	else if (c->points >= 0) snprintf(chip, sizeof chip, "+%d", c->points);
	if (chip[0]) chipw = rf_w(RF_CHIP, chip, 0, 0) + U(16);
	snprintf(hb_s, sizeof hb_s, "%s", c->head);
	{
		int need = rf_w(RF_HEAD, hb_s, sp, 1) + (chipw ? chipw + U(12) : 0), tw = rf_w(RF_TITLE, c->title, 0, 0);
		int lw = c->line[0] ? rf_w(RF_BODY, c->line, 0, 0) : 0, maxW = U(500), minW = c->plain ? U(200) : U(big ? 400 : 320);
		int nw = c->note[0] ? rf_w(RF_BODY, c->note, 0, 0) : 0;
		if (tw > need) need = tw;
		if (lw > need) need = lw;
		if (nw > need) need = nw;
		need += x0 + right;
		if (maxW > g_W * 62 / 100) maxW = g_W * 62 / 100;
		W = need < maxW ? need : maxW;
		if (W < (minW < maxW ? minW : maxW)) W = minW < maxW ? minW : maxW;
	}
	colW = W - x0 - right;
	hb = pad + rf_asc(RF_HEAD);
	tb = hb + U(6) + rf_asc(RF_TITLE);
	y = tb + (rf_px(RF_TITLE) - rf_asc(RF_TITLE));
	if (c->line[0]) {
		lb = y + U(4) + rf_asc(RF_BODY);
		lines = rf_w(RF_BODY, c->line, 0, 0) > colW && !s_rf_classic ? 2 : 1;
		y = lb + (lines - 1) * (rf_px(RF_BODY) + U(2)) + (rf_px(RF_BODY) - rf_asc(RF_BODY));
	}
	if (c->note[0]) {	/* the player and the time played (the mastery card) */
		nb = y + U(4) + rf_asc(RF_BODY);
		y = nb + (rf_px(RF_BODY) - rf_asc(RF_BODY));
	}
	H = y + pad;
	if (!c->plain && H < P + U(34)) H = P + U(34);
	x = g_W - W - U(20);
	y = U(20);
	r[0] = x; r[1] = y; r[2] = x + W; r[3] = y + H;
	ra_ground(x, y, W, H, rad, acc, 1);
	if (big) rrect_fade(x + 1, y + 1, W - 2, H * 55 / 100, (float)(rad - 1), acc, 26, 0);
	if (!c->plain) ra_pic(x + U(19), y + (H - P) / 2 < U(17) ? y + (H - P) / 2 : y + U(17), P, pic, acc);
	if (chipw) {
		int ch = rf_px(RF_CHIP) + U(6), cy = y + hb - rf_asc(RF_CHIP) - U(3);
		rrect_fill(x + W - right - chipw, cy, chipw, ch, (float)(ch / 2), acc, 51);
		rf_text(RF_CHIP, x + W - right - chipw + U(8), cy + U(3) + rf_asc(RF_CHIP), chip, acc, 0, 0, 0);
	}
	{
		char cut[64];
		rf_text(RF_HEAD, x + x0, y + hb, rf_fit(RF_HEAD, hb_s, colW - (chipw ? chipw + U(12) : 0), cut, sizeof cut), acc, 0, sp, 1);
	}
	s_cd_tick_long = !s_rf_classic && rf_w(RF_TITLE, c->title, 0, 0) > colW;
	rf_ticker(RF_TITLE, x + x0, y + tb, colW, c->title, C_INK, t);
	if (c->line[0]) {
		if (lines > 1) rf_wrap2(RF_BODY, x + x0, y + lb, rf_px(RF_BODY) + U(2), c->line, C_INK, colW);
		else { char cut[192]; rf_text(RF_BODY, x + x0, y + lb, rf_fit(RF_BODY, c->line, colW, cut, sizeof cut), C_INK, 0, 0, 0); }
	}
	if (c->note[0]) { char cut[96]; rf_text(RF_BODY, x + x0, y + nb, rf_fit(RF_BODY, c->note, colW, cut, sizeof cut), C_MUTED, 0, 0, 0); }
}

/* the progress pill at the top right: the badge, "23/50", a bar */
static void ra_pill(const struct pico_ra_card *c, int *r)
{
	int h = U(42), P = U(30), tx = U(7) + P + U(11), tw = rf_w(RF_FIG, c->line, 0, 0), barW = U(86), bx, w, x, y;
	int pct = c->pct < 0 ? 0 : c->pct > 100 ? 100 : c->pct;
	bx = tx + tw + U(14);
	w = bx + barW + U(18);
	x = g_W - w - U(20); y = U(20);
	r[0] = x; r[1] = y; r[2] = x + w; r[3] = y + h;
	ra_ground(x, y, w, h, h / 2, 0, 0);
	ra_pic(x + U(7), y + (h - P) / 2, P, c->badge[0] ? pico_ra_badge(c->badge) : 0, C_ACCENT);
	rf_text(RF_FIG, x + tx, y + h / 2 + rf_asc(RF_FIG) / 2 - U(1), c->line, C_INK, 0, 0, 0);
	rrect_fill(x + bx, y + h / 2 - U(3), barW, U(6), (float)U(3), C_MUTED, 56);
	if (pct) rrect_fill(x + bx, y + h / 2 - U(3), barW * pct / 100 > U(6) ? barW * pct / 100 : U(6), U(6), (float)U(3), C_ACCENT, 256);
}

/* a leaderboard's running value at the bottom right (k from the bottom) */
static void ra_tracker(const char *v, int k, int *r)
{
	int cell = 0, tw = 0, h = U(42), w, x, y, i;
	char d[2] = { 0, 0 };
	for (d[0] = '0'; d[0] <= '9'; d[0]++) { int a = rf_w(RF_FIG, d, 0, 0); if (a > cell) cell = a; }
	for (i = 0; v[i]; i++) { d[0] = v[i]; tw += v[i] >= '0' && v[i] <= '9' ? cell : rf_w(RF_FIG, d, 0, 0); }
	w = U(14) + U(14) + U(12) + tw + U(18);
	x = g_W - w - U(20);
	y = g_H - U(20) - (k + 1) * h - k * U(8);
	r[0] = x; r[1] = y; r[2] = x + w; r[3] = y + h;
	ra_ground(x, y, w, h, U(11), 0, 0);
	disc((float)(x + U(21)), (float)(y + h / 2), (float)U(6), C_ACCENT, 256);	/* (the timer's place: the accent dot) */
	{
		int px = x + U(40);
		for (i = 0; v[i]; i++) {
			int a;
			d[0] = v[i];
			a = v[i] >= '0' && v[i] <= '9' ? cell : rf_w(RF_FIG, d, 0, 0);
			rf_text(RF_FIG, px + a / 2, y + h / 2 + rf_asc(RF_FIG) / 2 - U(1), d, C_INK, 1, 0, 0);
			px += a;
		}
	}
}

/* a round-ended line x0,y0 to x1,y1, rad px thick on each side (the tick and the cross) */
static void ra_stroke(float x0, float y0, float x1, float y1, float rad, uint32_t rgb)
{
	float dx = x1 - x0, dy = y1 - y0, len = dx * dx + dy * dy;
	int i, n;
	for (n = 1; (float)(n * n) < len * 4; n++) ;	/* (a step every half pixel) */
	for (i = 0; i <= n; i++) disc(x0 + dx * i / n, y0 + dy * i / n, rad, rgb, 256);
}
/* a challenge's badge, k-th down the left of the game (melonds-nano's): a gold ring while it is on,
 * green with a tick when it unlocked, red with a cross (the badge grey) when it failed */
static void ra_challenge(const struct pico_ra_chal *c, int k, int *r)
{
	int S = U(58), x = U(12), y = g_H * 16 / 100 + k * (S + U(8)), B = U(40), i, j;
	uint32_t tone = c->outcome > 0 ? RA_GREEN : c->outcome < 0 ? RA_RED : RA_GOLD;
	r[0] = x; r[1] = y; r[2] = x + S; r[3] = y + S;
	ra_ground(x, y, S, S, U(11), 0, 0);
	rrect_fill(x + U(3), y + U(3), S - U(6), S - U(6), (float)U(9), tone, 77);
	ra_pic(x + U(9), y + U(9), B, c->badge[0] ? pico_ra_badge(c->badge) : 0, RA_GOLD);
	if (c->outcome < 0) ra_grey(x + U(9), y + U(9), B);	/* failed: the badge in grey */
	if (c->outcome) {
		float cx = (float)(x + S - U(13)), cy = (float)(y + S - U(13)), q = (float)U(5), w = U(1.6f) > 1 ? (float)U(1.6f) : 1.0f;
		disc(cx, cy, (float)U(10.5f), tone, 256);
		if (c->outcome > 0) {
			ra_stroke(cx - q, cy, cx - q * .3f, cy + q * .7f, w * .5f, RA_AIR);
			ra_stroke(cx - q * .3f, cy + q * .7f, cx + q, cy - q * .7f, w * .5f, RA_AIR);
		} else {
			ra_stroke(cx - q * .7f, cy - q * .7f, cx + q * .7f, cy + q * .7f, w * .5f, RA_AIR);
			ra_stroke(cx - q * .7f, cy + q * .7f, cx + q * .7f, cy - q * .7f, w * .5f, RA_AIR);
		}
	}
}

/* ---- the Achievements page's detail: a panel out of the drawer to the whole screen ----------------
 * A on an achievement opens it, growing from the drawer's edge over the game (melonds-nano's detail
 * card): the badge large, its state in a chip, the title, the points, the description, the progress
 * with its bar while it counts, when it was unlocked. Up and Down go to the one before or after, A
 * or B back to the list. */
static int igm_detail_w(int ext, int W) { return s_dt > 0 || s_dt_on ? W : ext; }
/* the text in lines of max_w px, words whole where they fit, at most max lines (the last cut short) */
static int wrap_lines(const cfont *f, const char *s, int max_w, char (*out)[160], int max)
{
	int n = 0;
	while (*s && n < max) {
		int len = (int)strlen(s), cut = 0, sp = 0, i;
		char a[160];
		while (*s == ' ') { s++; len--; }
		if (!len) break;
		for (i = 1; i <= len && i < (int)sizeof a; i++) {
			if (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) continue;
			memcpy(a, s, (size_t)i); a[i] = 0;
			if (text_w(f, a, 0, 0) > max_w) break;
			cut = i;
			if (s[i] == ' ' || !s[i]) sp = i;
		}
		if (!cut) break;
		if (cut < len && sp > 0) cut = sp;
		if (n == max - 1 && cut < len) {	/* the last line: what is left, cut with an ellipsis */
			const char *t = fit_str(f, s, max_w, out[n], 160);
			if (t != out[n]) snprintf(out[n], 160, "%s", t);
			return n + 1;
		}
		memcpy(out[n], s, (size_t)cut); out[n][cut] = 0;
		n++;
		s += cut;
	}
	return n;
}
volatile int g_dbg_dt[5];	/* oem diag: the detail panel's last paint (stage, width, title, open x1000, frames) */
volatile unsigned int g_dbg_dt_us[4];	/* oem diag: its parts, last paint: the ground, the badge, the words, all */
static void igm_detail_paint(void)
{
	struct pico_ra_detail d;
	unsigned now = gpt4_get_current_tick();
	float dt = (float)((now - s_dt_tick) / 13000u) / 1000.0f, k;
	int pw = G.pw + G.sh, w, bx, by, bwid, bh, pad, B, tx, tw, y, i, n;
	uint32_t tone;
	const char *tag;
	char lines[8][160];
	s_dt_tick = now;
	if (dt > .05f) dt = .05f;
	if (s_dt_on && s_dt < 1) { s_dt += dt * 4; if (s_dt > 1) s_dt = 1; }
	if (!s_dt_on && s_dt > 0) { s_dt -= dt * 5; if (s_dt < 0) s_dt = 0; }
	if (s_dt <= 0) return;
	if ((s_dt_on && s_dt < 1) || !s_dt_on) G.dirty = 1;	/* (drawn again while it grows or goes; still, only on a key) */
	g_dbg_dt[0] = 1; g_dbg_dt[4]++;
	if (pico_ra_detail(s_dt_row, &d) != 0) { s_dt_on = 0; return; }
	g_dbg_dt[0] = 2; g_dbg_dt[2] = (int)strlen(d.title); g_dbg_dt[3] = (int)(s_dt * 1000);
	k = igm_ease(s_dt);
	w = pw + (int)((g_W - pw) * k);	/* from the drawer's edge to the screen's */
	g_cx0 = 0; g_cx1 = w;
	{ static unsigned t0d; t0d = gpt4_get_current_tick(); g_dbg_dt_us[3] = t0d; }
	if (k >= .3f && w > 0 && w <= 2048) {
		/* opaque by now: every row of the ground is the same (the air, the accent's wash from the
		 * left, the edge), so one row is made and copied down the screen */
		static uint32_t row[2048];
		int fw = (int)(w * 36 / 100), yy;
		for (i = 0; i < w; i++) {
			uint32_t c = 0xFF000000u | RA_AIR;
			if (i < fw) { int a = 30 * (fw - i) / fw; if (a > 0) c = mix(c, C_ACCENT, a); }
			if (i == w - 1) c = mix(c, C_MUTED, 60);
			row[i] = c;
		}
		for (yy = 0; yy < g_H; yy++) {
			touch(yy, 0, w);
			memcpy(g_fb + yy * g_W, row, (size_t)w * 4);
		}
	} else {	/* still see-through as it opens */
		fill(0, 0, w, g_H, RA_AIR, (int)(256 * (k < .3f ? k / .3f : 1)));
		rrect_fade(0, 0, w, g_H, 0, C_ACCENT, 30, (int)(w * 36 / 100));
		fill(w - 1, 0, 1, g_H, C_MUTED, 60);	/* its edge */
	}
	g_dbg_dt[0] = 3; g_dbg_dt[1] = w;
	bx = U(30); by = U(26); bwid = g_W - U(60); bh = g_H - by - U(70);
	pad = U(26);
	B = U(128); if (B > bh * 30 / 100) B = bh * 30 / 100; if (B > bwid * 24 / 100) B = bwid * 24 / 100;
	tx = bx + pad + B + U(24); tw = bx + bwid - pad - tx;
	switch (d.state) {
	case PICO_RA_UNLOCKED: tag = d.waiting ? "Waiting to sync" : "Unlocked"; tone = d.waiting ? RA_AMBER : RA_GREEN; break;
	case PICO_RA_UNSUPPORTED: tag = "Unsupported"; tone = RA_RED; break;
	case PICO_RA_ALMOST: tag = "Almost there"; tone = C_ACCENT; break;
	default: tag = "Locked"; tone = C_MUTED; break;
	}
	{	/* the badge; locked (not unlocked) in grey, as the server's locked badge is */
		const unsigned short *px = d.badge[0] ? pico_ra_badge(d.badge) : 0;
		unsigned tb = gpt4_get_current_tick();
		g_dbg_dt_us[0] = (tb - g_dbg_dt_us[3]) / 13u;
		ra_pic(bx + pad, by + pad, B, px, tone == C_MUTED ? C_ACCENT : tone);
		if (px && d.state != PICO_RA_UNLOCKED) ra_grey(bx + pad, by + pad, B);
		g_dbg_dt_us[1] = (gpt4_get_current_tick() - tb) / 13u;
		g_dbg_dt_us[2] = gpt4_get_current_tick();
	}
	y = by + pad;
	{	/* the state's chip */
		int cw = text_w(&G.tag11, tr(tag), U(1), 1) + U(16), ch = U(19);
		rrect_fill(tx, y, cw, ch, (float)(ch / 2), tone, 51);
		text(&G.tag11, tx + U(8), y + U(14), tr(tag), tone, 256, 0, U(1), 1);
		y += ch + U(10);
	}
	n = wrap_lines(&G.dname, d.title, tw, lines, 2);
	for (i = 0; i < n; i++) text(&G.dname, tx, y + G.dname.asc + i * U(34), lines[i], C_INK, 256, 0, 0, 0);
	y += G.dname.asc + (n > 1 ? U(34) : 0) + U(30);
	{
		char pt[32];
		snprintf(pt, sizeof pt, d.points == 1 ? tr("%u point") : tr("%u points"), d.points);
		text(&G.sval, tx, y, pt, C_MUTED, 256, 0, 0, 0);
	}
	{	/* the footer: the progress while it counts, then when it was unlocked */
		int foot = by + bh - pad, bottom = foot - U(34), dy, lh = U(30);
		if (d.percent >= 0 && d.state != PICO_RA_UNLOCKED && d.measured[0]) {
			int barY = foot - U(42);
			text(&G.tag13, bx + pad, barY - U(12), tr("Progress"), C_MUTED, 256, 0, U(2), 1);
			text(&G.sval, bx + bwid - pad, barY - U(12), d.measured, C_INK, 256, 2, 0, 0);
			rrect_fill(bx + pad, barY, bwid - 2 * pad, U(7), (float)U(3), C_MUTED, 56);
			if (d.percent > 0)
				rrect_fill(bx + pad, barY, (bwid - 2 * pad) * (d.percent > 100 ? 100 : d.percent) / 100, U(7), (float)U(3),
					   C_ACCENT, 256);
			bottom = barY - U(40);
		}
		if (d.when > 0 && d.state == PICO_RA_UNLOCKED) {
			char dt_s[48], f2[80];
			long long off = pico_clock_is_set() ? pico_clock_local() - pico_clock_utc() : 0;
			pico_clock_date_text(d.when + off, dt_s, sizeof dt_s);
			snprintf(f2, sizeof f2, tr("Unlocked %s"), dt_s);
			text(&G.info, bx + pad, foot, f2, C_MUTED, 256, 0, 0, 0);
		} else if (d.waiting) {
			text(&G.info, bx + pad, foot, tr("Unlocked here, waiting to reach RetroAchievements"), C_MUTED, 256, 0, 0, 0);
		}
		/* the description under the badge, as many lines as there is room for */
		dy = by + pad + B + U(18);
		if (dy < y + U(18)) dy = y + U(18);
		n = (bottom - dy) / lh + 1;
		if (n > 8) n = 8;
		if (n > 0) {
			n = wrap_lines(&G.srow, d.text, bwid - 2 * pad, lines, n);
			for (i = 0; i < n; i++) text(&G.srow, bx + pad, dy + G.srow.asc + i * lh, lines[i], C_INK, 236, 0, 0, 0);
		}
	}
	{	/* the keys */
		int xx = U(30), cy = g_H - U(27), rad = U(8.3f);
		const char *codes[1] = { "B" };
		const char *labels[1] = { "Back" };
		for (i = 0; i < 1; i++) {
			draw_mask(&G.ring, xx, cy - G.ring.h / 2, C_MUTED, 256);
			text(&G.hintc, xx + rad, g_H - U(22.9f), codes[i], C_INK, 256, 1, 0, 0);
			xx += 2 * rad + U(8);
			xx += text(&G.hint, xx, g_H - U(22), tr(labels[i]), C_MUTED, 256, 0, 0, 0) + U(16);
		}
		text(&G.hint, xx, g_H - U(22), tr("Up / Down: the one before or after"), C_MUTED, 256, 0, 0, 0);
	}
	g_dbg_dt[0] = 4;
	{	/* (the words since the badge, then the whole paint, in place of its start) */
		unsigned te = gpt4_get_current_tick();
		g_dbg_dt_us[2] = (te - g_dbg_dt_us[2]) / 13u;
		g_dbg_dt_us[3] = (te - g_dbg_dt_us[3]) / 13u;
	}
	g_cx0 = g_cx1 = 0;
}
/* the keys while it is open: 1 taken */
static int igm_detail_keys(int up, int dn, int a, int b)
{
	struct pico_ra_detail d;
	if (!s_dt_on) return 0;
	if (a || b) { s_dt_on = 0; G.dirty = 1; return 1; }
	if (up || dn) {
		int r = s_dt_row, step = up ? -1 : 1, tries;
		for (tries = 0; tries < 256; tries++) {
			r += step;
			if (pico_ra_detail(r, &d) == 0) { s_dt_row = r; G.sel[IGM_ACH] = r; G.dirty = 1; break; }
			if (r < 0 || r > 255) break;
		}
		return 1;
	}
	return 1;		/* (the panel holds the keys while open) */
}
static int s_cd_have;				/* a card is in hand (else only the trackers) */
/* What the card's picture is made of (not its fade): drawn again when that changes */
static unsigned card_sig(int *ticks)
{
	unsigned h = 2166136261u;
	const char *p;
	char tv[PICO_RA_TRACKERS][24];
	int i, nt;
	*ticks = 0;
	h = (h ^ (unsigned)s_cd_have) * 16777619u;
	if (s_cd_have) {
		for (p = s_cd.title; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
		for (p = s_cd.line; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
		for (p = s_cd.head; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
		for (p = s_cd.chip; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
		for (p = s_cd.note; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
		h = (h ^ (unsigned)(s_cd.badge[0] && pico_ra_badge(s_cd.badge) != 0)) * 16777619u;
		h = (h ^ (unsigned)s_cd.pct) * 16777619u;
		*ticks = !CLASSIC_IGM && s_cd.kind != PICO_RA_CARD_PROGRESS && s_cd_tick_long;
	}
	nt = pico_ra_trackers(tv, PICO_RA_TRACKERS);
	for (i = 0; i < nt; i++) for (p = tv[i]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
	h = (h ^ (unsigned)nt) * 16777619u;
	{	/* the challenge badges: which, how each ended, their fade in eight steps, their pictures */
		struct pico_ra_chal ch[PICO_RA_CHALLENGES];
		int nc = pico_ra_challenges(ch, PICO_RA_CHALLENGES);
		for (i = 0; i < nc; i++) {
			h = (h ^ ch[i].id) * 16777619u;
			h = (h ^ (unsigned)(ch[i].outcome + 2)) * 16777619u;
			h = (h ^ ((ch[i].alpha + 16u) >> 5)) * 16777619u;
			h = (h ^ (unsigned)(ch[i].badge[0] && pico_ra_badge(ch[i].badge) != 0)) * 16777619u;
		}
		h = (h ^ (unsigned)nc) * 16777619u;
	}
	return h;
}

/* Drawn in full only when the card's picture changes (a new card, a new value, a tracker that
 * moved, a title on the ticker): then its pixels are kept (pico_ra_card_pic), and each step of its
 * fade (eight in, eight out) only scales those into the layer. A card standing in full is not
 * touched at all (the layer then copies nothing). Every frame drawn in full had cost the game. */
static int card_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H, int *box)
{
	static int was[1 + PICO_RA_TRACKERS + PICO_RA_CHALLENGES][4], nwas, card_r[4], have_r;
	static unsigned s_sig, s_a = 999;
	char tv[PICO_RA_TRACKERS][24];
	int now[1 + PICO_RA_TRACKERS + PICO_RA_CHALLENGES][4], n = 0, k, i, nt, ticks;
	unsigned tick = gpt4_get_current_tick(), sig, a, pic_cap = 0;
	uint32_t *pic = (uint32_t *)pico_ra_card_pic(&pic_cap);
	float t;
	g_fb = buf; g_W = (int)W; g_H = (int)H; g_s = (float)H / 720.0f; g_track = 0;
	s_rf_classic = CLASSIC_IGM;
	a = s_cd_have ? (s_cd.alpha >= 240 ? 255u : (s_cd.alpha + 16u) & ~31u) : 0u;
	sig = card_sig(&ticks);
	if (!s_cd_fresh && sig == s_sig && !ticks) {
		/* the same picture: only its fade, from the kept pixels */
		if (a == s_a || !have_r || !pic) return 0;
		{
			int cw = card_r[2] - card_r[0], y;
			for (y = card_r[1]; y < card_r[3]; y++) {
				const uint32_t *src = pic + (y - card_r[1]) * cw;
				uint32_t *d = buf + y * pitch + card_r[0];
				int x;
				for (x = 0; x < cw; x++) {
					uint32_t v = src[x];
					d[x] = ((((v >> 8) & 0x00FF00FFu) * a) & 0xFF00FF00u) | (((v & 0x00FF00FFu) * a >> 8) & 0x00FF00FFu);
				}
			}
			memcpy(box, card_r, sizeof card_r);
			s_a = a;
			return 1;
		}
	}
	s_sig = sig; s_a = a;
	if (s_rf_classic) pico_classic_ra_target(buf, pitch, W, H);
	if (s_cd_fresh) {	/* the whole picture: the layer shows every column (the challenge badges at the left,
				 * the cards at the right), and the menu's drawer may have been drawn there */
		for (k = 0; k < (int)H; k++) memset(buf + k * pitch, 0, (size_t)W * 4);
		box[0] = 0; box[1] = 0; box[2] = (int)W; box[3] = (int)H;
		s_cd_fresh = 0; nwas = 0;
	} else {
		box[0] = (int)W; box[1] = (int)H; box[2] = 0; box[3] = 0;
	}
	for (i = 0; i < nwas; i++) {	/* what was there goes */
		for (k = was[i][1]; k < was[i][3]; k++) memset(buf + k * pitch + was[i][0], 0, (size_t)(was[i][2] - was[i][0]) * 4);
		if (was[i][0] < box[0]) box[0] = was[i][0]; if (was[i][1] < box[1]) box[1] = was[i][1];
		if (was[i][2] > box[2]) box[2] = was[i][2]; if (was[i][3] > box[3]) box[3] = was[i][3];
	}
	have_r = 0;
	if (s_cd_have) {
		if (strncmp(s_cd_key, s_cd.title, sizeof s_cd_key)) {
			snprintf(s_cd_key, sizeof s_cd_key, "%s", s_cd.title);
			s_cd_tick = tick;
		}
		t = (float)((tick - s_cd_tick) / 13000u) / 1000.0f;
		if (s_cd.kind == PICO_RA_CARD_PROGRESS) ra_pill(&s_cd, now[n]); else ra_card(&s_cd, now[n], t);
		{	/* kept in full, for the fade's steps */
			int cw = now[n][2] - now[n][0], ch = now[n][3] - now[n][1], y;
			if (pic && cw > 0 && ch > 0 && (unsigned)(cw * ch * 4) <= pic_cap) {
				for (y = 0; y < ch; y++) memcpy(pic + y * cw, buf + (now[n][1] + y) * pitch + now[n][0], (size_t)cw * 4);
				memcpy(card_r, now[n], sizeof card_r);
				have_r = 1;
			}
		}
		fade_box(buf, (int)pitch, now[n], a);
		n++;
	}
	nt = pico_ra_trackers(tv, PICO_RA_TRACKERS);
	for (i = 0; i < nt; i++) ra_tracker(tv[i], i, now[n++]);
	{
		struct pico_ra_chal ch[PICO_RA_CHALLENGES];
		int nc = pico_ra_challenges(ch, PICO_RA_CHALLENGES);
		for (i = 0; i < nc; i++) {
			unsigned ca = ch[i].alpha >= 240 ? 255u : (ch[i].alpha + 16u) & ~31u;
			ra_challenge(&ch[i], i, now[n]);
			if (ca < 255u) fade_box(buf, (int)pitch, now[n], ca);
			n++;
		}
	}
	for (i = 0; i < n; i++) {
		if (now[i][0] < box[0]) box[0] = now[i][0]; if (now[i][1] < box[1]) box[1] = now[i][1];
		if (now[i][2] > box[2]) box[2] = now[i][2]; if (now[i][3] > box[3]) box[3] = now[i][3];
		memcpy(was[i], now[i], sizeof was[i]);
	}
	nwas = n;
	if (box[2] <= box[0]) { box[0] = box[1] = box[2] = box[3] = 0; return 0; }
	return 1;
}

/* a card to show now, the menu shut (1: it is on the layer this frame) */
static int card_frame(int open)
{
	struct pico_ra_card c;
	char tv[PICO_RA_TRACKERS][24];
	struct pico_ra_chal ch[PICO_RA_CHALLENGES];
	int card = !open && pico_ra_card_now(&c), have = card || (!open && (pico_ra_trackers(tv, PICO_RA_TRACKERS) > 0 ||
									    pico_ra_challenges(ch, PICO_RA_CHALLENGES) > 0));
	g_ra_card_on_layer = 0;
	if (!have) {
		if (s_cd_up) { s_cd_up = 0; if (!open) { ayaneo_overlay_live(card_paint, 0); return 1; } }
		return 0;
	}
	if (CLASSIC_IGM ? !pico_classic_igm_card_ok() : !G.loaded) return 0;	/* the theme's look is not here */
	g_ra_theme_cards = 1;
	if (s_layer_menu) {	/* the menu just shut over a card: its layer down first (it spans the drawer's
				 * columns, which the card would leave showing), the card's own next frame */
		s_layer_menu = 0;
		ayaneo_overlay_live(card_paint, 0);
		return 1;
	}
	s_cd_have = card;
	if (card) s_cd = c;
	if (!s_cd_up) { s_cd_up = 1; s_cd_fresh = 1; }
	g_ra_card_on_layer = 1;
	ayaneo_overlay_live(card_paint, 1);	/* (both themes: melonds-nano's cards, the theme's fonts) */
	return 1;
}

/* a card wants the layer (the runners drive it then, the menu shut too) */
int pico_igm_card_wanted(void)
{
	struct pico_ra_card c;
	char tv[PICO_RA_TRACKERS][24];
	if (s_cd_up) return 1;			/* (checked when it came up: the theme's packs are costly to check) */
	struct pico_ra_chal ch[PICO_RA_CHALLENGES];
	if (!pico_ra_card_now(&c) && !pico_ra_trackers(tv, PICO_RA_TRACKERS) && !pico_ra_challenges(ch, PICO_RA_CHALLENGES))
		return 0;
	return pico_igm_on();
}
#endif

void pico_igm_overlay(int open)
{
#if defined(KONKR_KPA) || defined(KONKR_BLOCK)
	if (open && s_cd_up) {		/* the menu comes over a card: the layer down first, then the menu's */
		s_cd_up = 0;
		g_ra_card_on_layer = 0;
		ayaneo_overlay_live(card_paint, 0);
		return;
	}
	if (card_frame(open)) return;
#endif
	if (open) s_layer_menu = 1;
	else if (s_layer_menu && !ayaneo_overlay_live_up()) s_layer_menu = 0;	/* (down on its own: nothing to hand over) */
	if (CLASSIC_IGM) { pico_classic_igm_overlay(open); return; }
	ayaneo_overlay_live(igm_paint, open && G.loaded);
}
