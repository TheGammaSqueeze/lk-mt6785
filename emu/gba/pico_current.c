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
#include "sd_fat.h"                 /* gba_rom_entry, GBA_CONSOLE_* */
#include "fat_ro.h"
#include "fat_wr.h"

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
extern unsigned int ayaneo_get_menu_theme(void);
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
extern volatile int g_menu_live;
/* shared with the classic menu (gba_snes_menu.c) */
extern void gba_menu_read_input(snes_input *in);
extern void gba_menu_av_poll(void);
extern void gba_menu_av_draw(unsigned int *fb, unsigned int pitch, int W, int H);
extern int  gba_menu_boxart_load(const gba_rom_entry *roms, int nrom);
extern const unsigned char *gba_menu_boxart(int i, unsigned int *w, unsigned int *h);
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

#define NSYS          8               /* GBA_CONSOLE_GB .. GBA_CONSOLE_SG, the art in the same order */
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
#define NCOLOURS 6
static const struct { const char *name; uint32_t accent; int16_t m[9]; } k_colour[NCOLOURS] = {
	{ "Blue",     0x98E8FFu, { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 } },
	{ "Violet",   0xD4BBFFu, { 595, -302, 731, 181, 1034, -191, -527, 786, 764 } },
	{ "Crimson",  0xFFB4C0u, { -450, 606, 868, 468, 665, -109, -283, 1760, -453 } },
	{ "Amber",    0xFFD49Au, { -610, 1355, 279, 454, 463, 106, 322, 1559, -857 } },
	{ "Emerald",  0xA2F2C6u, { 637, 1250, -863, 20, 669, 335, 940, -165, 248 } },
	{ "Graphite", 0xE2E6ECu, { 299, 659, 66, 196, 761, 66, 196, 659, 169 } },
};
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
typedef struct { int px, asc, n, aw, nk; const cglyph *g; const int32_t *kern; const uint8_t *atlas; } cfont;

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
static cfont font(const char *name)
{
	cfont f = { 0, 0, 0, 0, 0, 0, 0, 0 };
	const cur_entry *e = find(name, T_FONT);
	if (e) {
		const uint16_t *h = (const uint16_t *)(s_raw + e->off);
		f.px = h[0]; f.asc = h[1]; f.n = h[3]; f.aw = h[4]; f.nk = h[6];
		f.g = (const cglyph *)(s_raw + e->off + 16);
		f.kern = (const int32_t *)(f.g + f.n);
		f.atlas = (const uint8_t *)(f.kern + 2 * f.nk);
	}
	return f;
}

/* load /gammaos/current-<W>x<H>.pack; 0 ok */
static int pack_load(unsigned W, unsigned H)
{
	char path[64];
	fat_vol *v = gba_sd_menu_vol();
	fat_file f;
	unsigned char hdr[12];
	uint32_t raw, comp;
	unsigned long zlen;
	const uint32_t *ph;
	snprintf(path, sizeof path, "/gammaos/current-%ux%u.pack", W, H);
	if (!v || fat_open(v, path, &f) != 0) return -1;
	if (fat_read(&f, 0, hdr, 12) != 12) return -1;
	memcpy(&raw, hdr + 4, 4); memcpy(&comp, hdr + 8, 4);
	if (*(const uint32_t *)hdr != CUR_MAGIC_Z || !raw || raw > CUR_RAW_MAX || !comp || comp > CUR_COMP_MAX ||
	    12u + comp > f.size) return -2;
	if (fat_read(&f, 12, (void *)CUR_COMP_PA, comp) != comp) return -3;
	zlen = comp;
	if (zunzip((unsigned char *)CUR_COMP_PA, &zlen, (void *)CUR_RAW_PA, (int)raw, 0) != 0) return -4;
	ph = (const uint32_t *)CUR_RAW_PA;
	if (ph[0] != 0x52554350u || ph[1] != 1u || (ph[3] & 0xFFFFu) != W || (ph[3] >> 16) != H) return -5;
	s_raw = (const uint8_t *)CUR_RAW_PA;
	s_nent = (int)ph[2];
	s_ent = (const cur_entry *)(s_raw + 16);
	return 0;
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
/* blend with a = 0..255 coverage times op 0..256 */
static inline void put(int x, int y, uint32_t rgb, int a)
{
	uint32_t *d;
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
	} else *ps += 1;
	return c;
}
static const cglyph *glyph(const cfont *f, uint32_t c)
{
	int i = c >= 0x20 && c < 0x7F ? (int)c - 0x20 : c >= 0xA0 && c <= 0xFF ? 95 + (int)c - 0xA0 :
		c == 0x2022 ? 191 : c == 0x2026 ? 192 : '?' - 0x20;
	return i < f->n ? &f->g[i] : &f->g['?' - 0x20];
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
	int w = 0;
	uint32_t prev = 0;
	if (!f->g) return 0;
	while (*s) {
		uint32_t c = utf8_next(&s);
		if (upper && c >= 'a' && c <= 'z') c -= 32;
		w += (int)glyph(f, c)->adv + kern(f, prev, c) + ls * 64;
		prev = c;
	}
	return (w + 32) >> 6;
}
/* draw text with its baseline at y; anchor 0 start, 1 middle, 2 end; returns the width */
static int text(const cfont *f, int x, int y, const char *s, uint32_t rgb, int op, int anchor, int ls, int upper)
{
	int pen, w = text_w(f, s, ls, upper);
	uint32_t prev = 0;
	if (!f->g) return 0;
	if (anchor == 1) x -= w / 2; else if (anchor == 2) x -= w;
	pen = x * 64;
	while (*s) {
		uint32_t c = utf8_next(&s);
		const cglyph *g;
		int i, j, gx;
		if (upper && c >= 'a' && c <= 'z') c -= 32;
		pen += kern(f, prev, c);
		g = glyph(f, c);
		gx = ((pen + 32) >> 6) + g->xo;
		for (j = 0; j < g->h; j++) {
			const uint8_t *a = f->atlas + (g->y + j) * f->aw + g->x;
			for (i = 0; i < g->w; i++)
				if (a[i]) { int al = (a[i] * op) >> 8; put(gx + i, y + g->yo + j, rgb, al + (al >> 7)); }
		}
		pen += (int)g->adv + ls * 64;
		prev = c;
	}
	return w;
}
/* text cut to max_w px with an ellipsis */
static void text_fit(const cfont *f, int x, int y, const char *s, uint32_t rgb, int op, int max_w)
{
	char buf[160];
	int n = (int)strlen(s);
	if (text_w(f, s, 0, 0) <= max_w || n >= (int)sizeof buf - 4) { text(f, x, y, s, rgb, op, 0, 0, 0); return; }
	memcpy(buf, s, (size_t)n + 1);
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   /* whole characters */
		buf[n] = (char)0xE2; buf[n + 1] = (char)0x80; buf[n + 2] = (char)0xA6; buf[n + 3] = 0;   /* ... */
		if (text_w(f, buf, 0, 0) <= max_w) break;
	}
	text(f, x, y, buf, rgb, op, 0, 0, 0);
}

/* ---- the theme's art and type, looked up once per pack load -------------------------------- */
enum { SET_BRIGHT, SET_VOLUME, SET_BFI, SET_THEME, SET_COLOUR, SET_MUTE_BIOS, SET_BOOT_OS, SET_N };   /* the settings rows */
static struct {
	cimg hw[NSYS], logo[NSYS], slogo[NSYS], llogo[NSYS], alogo[NSYS];
	cimg rail[3], railf[3], ic_library, ic_play, ic_set[SET_N], gamma, shadow, glow, ring, pillm;
	cfont brand, sub, realm, rail_t, railf_t, tag13, tag11, count, lib, button, row, rowf, info,
	      srow, srowf, sval, dname, dnum, dunit, hintc, hint, empty;
	int nthemes;
	char theme_id[NTHEMES][24], theme_label[NTHEMES][24];
} A;

static void art_load(void)
{
	static const char *sys[NSYS] = { "gb", "gbc", "gba", "snes", "genesis", "sms", "gg", "sg" };
	static const char *rail[3] = { "games", "resume", "settings" };
	static const char *setic[SET_N] = { "sun", "sound", "bfi", "display", "colour", "mute", "power" };
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
	A.ic_library = img("ic_library"); A.ic_play = img("ic_play"); A.gamma = img("gamma");
	A.shadow = img("shadow"); A.glow = img("glow"); A.ring = img("hint_ring"); A.pillm = img("hint_pill");
	A.brand = font("f_brand"); A.sub = font("f_sub"); A.realm = font("f_realm");
	A.rail_t = font("f_rail"); A.railf_t = font("f_railf"); A.tag13 = font("f_tag13"); A.tag11 = font("f_tag11");
	A.count = font("f_count"); A.lib = font("f_lib"); A.button = font("f_button");
	A.row = font("f_row"); A.rowf = font("f_rowf"); A.info = font("f_info");
	A.srow = font("f_srow"); A.srowf = font("f_srowf"); A.sval = font("f_sval");
	A.dname = font("f_dname"); A.dnum = font("f_dnum"); A.dunit = font("f_dunit");
	A.hintc = font("f_hintc"); A.hint = font("f_hint"); A.empty = font("f_empty");
	A.nthemes = 0;
	{	/* the card's themes, "id|Label" a line, in menu order */
		const cur_entry *e = find("themes", T_TEXT);
		const char *p = e ? (const char *)(s_raw + e->off) : 0, *end = p ? p + e->len : 0;
		while (p && p < end && A.nthemes < NTHEMES) {
			int k = 0, m = 0;
			while (p < end && *p != '|' && *p != '\n' && k < 23) A.theme_id[A.nthemes][k++] = *p++;
			A.theme_id[A.nthemes][k] = 0;
			if (p < end && *p == '|') p++;
			while (p < end && *p != '\n' && m < 23) A.theme_label[A.nthemes][m++] = *p++;
			A.theme_label[A.nthemes][m] = 0;
			while (p < end && *p != '\n') p++;
			if (p < end) p++;
			if (k) A.nthemes++;
		}
	}
}

/* ---- state ---------------------------------------------------------------------------------- */
enum { TAB_GAMES, TAB_RESUME, TAB_SETTINGS };
static const char *s_sysname[NSYS] = { "Game Boy", "Game Boy Color", "Game Boy Advance", "Super Nintendo",
					"Mega Drive / Genesis", "Master System", "Game Gear", "SG-1000" };

static struct {
	int ready;                        /* the state below survives from one menu run to the next */
	int tab, lib;                     /* the rail's tab; 1 while a system's library is open */
	int nsys, sys, sys_from;          /* the systems on the card (in console order) and the focus */
	int systype[NSYS], syscount[NSYS];
	float sys_t;                      /* 0..1, the console and the current moving to the new system */
	float dip;                        /* 1..0, the short dip of a screen change */
	int lib_sel[NSYS], lib_top[NSYS];
	int res_sel, set_sel, theme_pick;
	int nrec, rec[NRECENT];           /* recently played, rom indices, newest first */
	int batt, chg;
	unsigned int batt_tick;
	float phase;                      /* the current's phase (radians), drifting */
	float push;                       /* phase still to add from a system move */
	int bg_key[2];                    /* the colour each still backdrop was made in, -1 none */
	int pal_colour;                   /* the colour the shaded palette was made in, -1 none */
} S;

static const gba_rom_entry *s_roms;
static int s_nrom;
static char s_names[128][128];        /* the cleaned titles */
static short s_libidx[NSYS][128];     /* per system the rom indices, card order */

static int sys_of(int type) { int i; for (i = 0; i < S.nsys; i++) if (S.systype[i] == type) return i; return -1; }

static void roster_build(void)
{
	int i, t;
	S.nsys = 0;
	for (t = 0; t < NSYS; t++) {
		int n = 0;
		for (i = 0; i < s_nrom && i < 128; i++)
			if (s_roms[i].type == t) s_libidx[S.nsys][n++] = (short)i;
		if (n) { S.systype[S.nsys] = t; S.syscount[S.nsys] = n; S.nsys++; }
	}
	for (i = 0; i < s_nrom && i < 128; i++) {
		gba_clean_name(s_roms[i].name, s_names[i]);
		if (!s_names[i][0]) { strncpy(s_names[i], s_roms[i].name, 127); s_names[i][127] = 0; }
	}
}

/* ---- recently played: /gammaos/recent.txt, "<type> <name>" a line, newest first ----------- */
static int rom_find(int type, const char *name)
{
	int i;
	for (i = 0; i < s_nrom && i < 128; i++)
		if (s_roms[i].type == type && !strcmp(s_roms[i].name, name)) return i;
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
static void recent_add(int rom)
{
	static char buf[NRECENT * 136];
	fat_vol *v = gba_sd_menu_vol();
	int i, k, len = 0;
	for (i = 0; i < S.nrec && S.rec[i] != rom; i++) ;
	if (i == S.nrec && S.nrec < NRECENT) S.nrec++;
	if (i >= S.nrec) i = S.nrec - 1;
	for (k = i; k > 0; k--) S.rec[k] = S.rec[k - 1];
	S.rec[0] = rom;
	for (i = 0; i < S.nrec; i++)
		len += snprintf(buf + len, sizeof buf - (size_t)len, "%d %s\n", s_roms[S.rec[i]].type, s_roms[S.rec[i]].name);
	if (v) fat_wr_put(v, "/gammaos", "recent.txt", buf, (uint32_t)len);
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
	int x = U(s_p), w, r, chg = S.chg, pct = S.batt;
	draw_mask(&A.gamma, x, U(28), C_INK, 256);
	w = text(&A.brand, x + U(31), U(47), "GammaOS", C_INK, 256, 0, 0, 0);
	w = x + U(31) + w + U(8);
	w += text(&A.sub, w, U(47), "Pico", C_MUTED, 256, 0, 0, 0);
	fill(w + U(14), U(31), 1, U(17), C_MUTED, 115);
	text(&A.realm, w + U(32), U(46), realm, C_MUTED, 256, 0, 0, 0);
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
		xx += text(&A.hint, xx, U(720 - 22), labels[i], C_MUTED, 256, 0, 0, 0) + U(13);
	}
}
static void rail(void)
{
	static const char *label[3] = { "Games", "Resume", "Settings" };
	float pitch = s_uw * .116f < 148 ? s_uw * .116f : 148, yy = 119;
	int i;
	for (i = 0; i < 3; i++) {
		float x = s_p + i * pitch;
		int f = i == S.tab;
		if (f) draw_mask(&A.glow, U(x + 20) - A.glow.w / 2, U(yy + 15) - A.glow.h / 2, C_ACCENT, 256);
		draw_mask(f ? &A.railf[i] : &A.rail[i], U(x), U(yy) + (f ? 0 : U(2.5f)), f ? C_INK : C_MUTED, 256);
		text(f ? &A.railf_t : &A.rail_t, U(x + 42), U(yy + 23), label[i], f ? C_INK : C_MUTED, 256, 0, 0, 0);
		if (f) fill(U(x), U(yy + 41) - 1, U(77), U(1.8f) > 1 ? U(1.8f) : 2, C_ACCENT, 256);
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
		text_fit(sel ? ff : fn, xi + U(16), yy + (int)(ph * .61f), items[i], sel ? C_INK : C_MUTED, 256,
			 wi - U(32) - (values ? U(90) : 0));
		if (values && values[i])
			text(fv, xi + wi - U(12), yy + (int)(ph * .61f), values[i], sel ? C_INK : C_MUTED, 256, 2, 0, 0);
		hline(xi + U(14), xi + wi, yy + ph - 1, C_MUTED, 49);
	}
}
static void button(float x, float y, const char *label, const cimg *icon)
{
	int tw = text_w(&A.button, label, 0, 0), w = U(38) + tw + U(20);
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
	char t[32];
	if (S.sys_t >= 1) stage_draw();   /* at rest the consoles are part of the menu layer */
	/* the system, its count, the way in */
	text(&A.tag13, U(x), U(yy), "Game systems", C_MUTED, 256, 0, U(2), 1);
	draw_mask(&A.logo[cur], U(x), U(yy + 25 + 41.5f) - A.logo[cur].h / 2, C_INK, 256);
	snprintf(t, sizeof t, "%d %s", S.syscount[S.sys], S.syscount[S.sys] == 1 ? "game" : "games");
	w = text(&A.count, U(x), U(yy + 154), t, C_INK, 256, 0, 0, 0);
	cx = U(x) + w + U(14);
	disc((float)cx, U(yy + 148) + .0f, U(2) + .0f, C_MUTED, 256);
	text(&A.lib, cx + U(14), U(yy + 154), "Library", C_MUTED, 256, 0, 0, 0);
	button(x, yy + 190, "Open library", &A.ic_library);
	/* the systems strip */
	hline(U(s_p), U(s_uw - s_p), U(base - 27), C_MUTED, 72);
	text(&A.tag11, U(s_p), U(base + 4), "Systems", C_MUTED, 256, 0, U(2), 1);
	span = (s_uw - 2 * s_p - 121) / (S.nsys > 4 ? S.nsys : 4);
	if (span > 168) span = 168;
	for (i = 0; i < S.nsys; i++) {
		const cimg *l = &A.slogo[S.systype[i]];
		int xx = U(s_p + 118 + i * span);
		draw_mask(l, xx, U(base - 16 + 16.5f) - l->h / 2, C_INK, i == S.sys ? 256 : 133);
		if (i == S.sys) fill(xx, U(base + 26), l->w, U(1.8f) > 1 ? U(1.8f) : 2, C_ACCENT, 256);
	}
}
static const char *s_rowtxt[128];
static void game_list(const short *idx, int n, int sel, int top, int title_y, const char *title)
{
	float yy = 720 * .36f, lw = s_uw * .45f, rw = s_uw * .34f < 393 ? s_uw * .34f : 393, rh = 311;
	float rx = s_uw - s_p - rw, ry = 720 * .30f;
	int i, k = n - top < LIB_ROWS ? n - top : LIB_ROWS;
	if (title) text(&A.tag13, U(s_p), U(title_y), title, C_MUTED, 256, 0, U(2), 1);
	for (i = 0; i < k; i++) s_rowtxt[i] = s_names[idx[top + i]];
	rows(s_p, yy, lw, 51, s_rowtxt, 0, k, sel - top, &A.row, &A.rowf, 0);
	if (top > 0) disc(U(s_p + lw - 8) + .0f, U(yy - 10) + .0f, U(2.5f) + .0f, C_MUTED, 200);
	if (top + k < n) disc(U(s_p + lw - 8) + .0f, U(yy + LIB_ROWS * 51 + 6) + .0f, U(2.5f) + .0f, C_MUTED, 200);
	if (n) {
		int rom = idx[sel];
		art(rom, rx, ry, rw, rh);
		text(&A.info, U(rx + rw / 2), U(ry + rh + 29), s_sysname[s_roms[rom].type], C_MUTED, 256, 1, 0, 0);
	}
}
static void home_resume(void)
{
	short idx[NRECENT];
	int i;
	for (i = 0; i < S.nrec; i++) idx[i] = (short)S.rec[i];
	if (!S.nrec) {
		text(&A.tag13, U(s_p), U(720 * .36f - 22), "Recently played", C_MUTED, 256, 0, U(2), 1);
		text(&A.empty, U(s_p), U(720 * .36f + 32), "The games you play appear here.", C_MUTED, 256, 0, 0, 0);
		return;
	}
	game_list(idx, S.nrec, S.res_sel, S.res_sel >= LIB_ROWS ? S.res_sel - LIB_ROWS + 1 : 0,
		  (int)(720 * .36f - 22), "Recently played");
}
/* the saved form of theme i: 0 for the first, the hash of its name for the others */
static unsigned int theme_hash(int i) { return i <= 0 ? 0u : snes_hash(A.theme_id[i]); }
static void set_value(int i, char *buf, int n, int *num)
{
	*num = -1;
	switch (i) {
	case SET_BRIGHT: *num = ayaneo_brightness_pct(); snprintf(buf, n, "%d%%", *num); break;
	case SET_VOLUME: *num = ayaneo_gbc_audio_get_volume(); snprintf(buf, n, "%d%%", *num); break;
	case SET_BFI:    snprintf(buf, n, "%s", ayaneo_strobe_name(ayaneo_get_strobe())); break;
	case SET_THEME:  snprintf(buf, n, "%s", S.theme_pick < A.nthemes ? A.theme_label[S.theme_pick] : ""); break;
	case SET_COLOUR: snprintf(buf, n, "%s", k_colour[g_colour].name); break;
	case SET_MUTE_BIOS: snprintf(buf, n, "%s", ayaneo_get_mute_bios() ? "On" : "Off"); break;
	default: snprintf(buf, n, "Open"); break;
	}
}
static void home_settings(void)
{
	static const char *names[SET_N] = { "Brightness", "Volume", "Black Frame Insertion", "Theme", "Colour",
					    "Mute BIOS Audio", "Boot to Android" };
	static char vals[SET_N][24];
	const char *vp[SET_N];
	float top = 720 * .315f, xx = s_uw * .46f, lw = s_uw - s_p - xx, mid = top + 35, sw = s_uw * .255f;
	int i, num;
	for (i = 0; i < SET_N; i++) { set_value(i, vals[i], sizeof vals[i], &num); vp[i] = vals[i]; }
	rows(xx, top, lw, 720 * .08f < 58 ? 720 * .08f : 58, names, vp, SET_N, S.set_sel, &A.srow, &A.srowf, &A.sval);
	/* the focused setting, large on the left */
	if (sw > 270) sw = 270;
	draw_mask(&A.ic_set[S.set_sel], U(s_p), U(mid), C_ACCENT, 256);
	text(&A.dname, U(s_p), U(mid + 93), names[S.set_sel], C_INK, 256, 0, 0, 0);
	set_value(S.set_sel, vals[0], sizeof vals[0], &num);
	if (num >= 0) {
		char t[8];
		int w;
		snprintf(t, sizeof t, "%d", num);
		w = text(&A.dnum, U(s_p), U(mid + 141), t, C_INK, 256, 0, 0, 0);
		text(&A.dunit, U(s_p) + w + U(8), U(mid + 139), "%", C_MUTED, 256, 0, 0, 0);
		fill(U(s_p), U(mid + 176), U(sw), U(3), C_MUTED, 77);
		fill(U(s_p), U(mid + 176), U(sw) * num / 100, U(3), C_ACCENT, 256);
		disc(U(s_p) + U(sw) * num / 100 + .0f, U(mid + 177.5f) + .0f, U(5.3f) + .0f, C_INK, 256);
	} else if (S.set_sel == SET_BOOT_OS) {
		text(&A.lib, U(s_p), U(mid + 141), "Restart into Android", C_MUTED, 256, 0, 0, 0);
	} else {
		text(&A.dname, U(s_p), U(mid + 141), vals[0], C_INK, 256, 0, 0, 0);
		if (S.set_sel == SET_THEME && A.nthemes && theme_hash(S.theme_pick) != ayaneo_get_menu_theme())
			text(&A.lib, U(s_p), U(mid + 178), "Press A to apply", C_MUTED, 256, 0, 0, 0);
	}
}
static void library(void)
{
	int s = S.sys, t = S.systype[s];
	text(&A.tag13, U(s_p), U(124), "Game library", C_MUTED, 256, 0, U(2), 1);
	draw_mask(&A.llogo[t], U(s_p), U(150 + 29) - A.llogo[t].h / 2, C_INK, 256);
	game_list(s_libidx[s], S.syscount[s], S.lib_sel[s], S.lib_top[s], 0, 0);
}

/* the menu into its own layer (transparent around it); redrawn only when something changed */
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
	static const char *hc_games[2] = { "A", "LR" }, *hl_games[2] = { "Open library", "Section" };
	static const char *hc_res[2] = { "A", "LR" }, *hl_res[2] = { "Resume", "Section" };
	static const char *hc_set[2] = { "A", "LR" }, *hl_set[2] = { "Select", "Section" };
	static const char *hc_lib[2] = { "A", "B" }, *hl_lib[2] = { "Launch", "Back" };
	g_fb = (uint32_t *)CUR_UI_PA;
	ui_clear();
	g_track = 1;
	if (S.lib) {
		library();
		header("Game library");
		hints(hc_lib, hl_lib, 2);
	} else {
		rail();
		if (S.tab == TAB_GAMES) home_games(); else if (S.tab == TAB_RESUME) home_resume(); else home_settings();
		header(S.tab == TAB_SETTINGS ? "Settings" : "Home");
		if (S.tab == TAB_GAMES) hints(hc_games, hl_games, 2);
		else if (S.tab == TAB_RESUME) { if (S.nrec) hints(hc_res, hl_res, 2); else hints(hc_res + 1, hl_res + 1, 1); }
		else hints(hc_set, hl_set, 2);
	}
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
	backdrop(S.lib || S.tab != TAB_GAMES);
	if (!S.lib && S.tab == TAB_GAMES && S.sys_t < 1) stage_draw();   /* the consoles crossfading */
	for (y = 0; y < g_H; y++)	/* premultiplied over, only where the menu layer was drawn */
		for (i = y * g_W + s_ux0[y]; i < y * g_W + s_ux1[y]; i++) {
			uint32_t u = ui[i], a = u >> 24, d, k;
			if (!a) continue;
			if (a == 255) { g_fb[i] = u; continue; }
			d = g_fb[i]; k = 256 - a - (a >> 7);
			g_fb[i] = 0xFF000000u | ((((d & 0xFF00FFu) * k >> 8) & 0xFF00FFu) + (u & 0xFF00FFu)) |
				  ((((d & 0x00FF00u) * k >> 8) & 0x00FF00u) + (u & 0x00FF00u));
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
	backdrop(S.lib || S.tab != TAB_GAMES);
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
	gba_punch_ready = 1;
}

/* ---- input ------------------------------------------------------------------------------------- */
static struct { int prev[6]; float rep[6]; } K;   /* up down left right lb rb: held, repeat timer */
/* a press, and repeats while held (0.32 s, then every 0.085 s) */
static int fire(int i, int level, float dt)
{
	int f = 0;
	if (level && !K.prev[i]) { f = 1; K.rep[i] = .32f; }
	else if (level) { K.rep[i] -= dt; if (K.rep[i] <= 0) { f = 1; K.rep[i] = .085f; } }
	K.prev[i] = level;
	return f;
}

/* ---- the run ----------------------------------------------------------------------------------- */
static void focus_rom(int rom)
{
	int s = rom >= 0 && rom < s_nrom ? sys_of(s_roms[rom].type) : -1, k;
	if (s < 0) return;
	S.sys = S.sys_from = s;
	for (k = 0; k < S.syscount[s]; k++)
		if (s_libidx[s][k] == rom) {
			S.lib_sel[s] = k;
			if (k < S.lib_top[s] || k >= S.lib_top[s] + LIB_ROWS) S.lib_top[s] = k >= LIB_ROWS ? k - LIB_ROWS + 1 : 0;
		}
}

/* Returns the picked game, -3 when a theme was picked (the caller runs the menu again), or -2
 * when the theme's pack is missing or does not fit this screen (the classic menu runs). */
int pico_current_run(const gba_rom_entry *roms, int nrom, int start_sel, int do_reverse)
{
	unsigned int pitch, W, H, saved_mhz, prev = 0;
	int launch_gate = 1, i, dirty = 1;
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
	if (pack_load(W, H) != 0) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
	g_W = (int)W; g_H = (int)H; g_s = (float)H / 720.0f;
	s_uw = (float)W / g_s;
	s_p = s_uw * .048f > 38 ? s_uw * .048f : 38;
	art_load();
	colour_set(ayaneo_get_menu_colour());
	s_roms = roms; s_nrom = nrom > 128 ? 128 : nrom;
	roster_build();
	if (!S.nsys) { ayaneo_set_cpu_mhz(saved_mhz); return -2; }
	S.bg_key[0] = S.bg_key[1] = -1; S.pal_colour = -1;   /* the game took their memory */
	s_ui_clean = 0;
	sin_init();
	if (!S.ready) {	/* the first menu of this power on: Game Boy Advance, when the card has its games */
		memset(S.lib_sel, 0, sizeof S.lib_sel); memset(S.lib_top, 0, sizeof S.lib_top);
		S.tab = TAB_GAMES; S.lib = 0; S.set_sel = 0; S.res_sel = 0;
		S.sys = sys_of(GBA_CONSOLE_GBA) >= 0 ? sys_of(GBA_CONSOLE_GBA) : 0;
		S.ready = 1;
	}
	if (S.sys >= S.nsys) S.sys = 0;
	S.sys_from = S.sys; S.sys_t = 1;
	if (do_reverse) focus_rom(start_sel);   /* back from a game: on it */
	for (i = 0; i < S.nsys; i++)
		if (S.lib_sel[i] >= S.syscount[i]) { S.lib_sel[i] = 0; S.lib_top[i] = 0; }
	S.theme_pick = 0;
	for (i = 0; i < A.nthemes; i++)
		if (theme_hash(i) == ayaneo_get_menu_theme()) S.theme_pick = i;
	recent_load();
	if (S.res_sel >= S.nrec) S.res_sel = 0;
	S.batt = gba_battery_pct(&S.chg);
	S.batt_tick = gpt4_get_current_tick();
	ayaneo_menu_audio_silence();
	gba_menu_boxart_load(roms, nrom);
	g_menu_live = 1;

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

		memset(&in, 0, sizeof in);
		gba_menu_read_input(&in);
		if (launch_gate) {	/* A or Start still held from the close or the launch: not a press */
			if (in.a || in.start) { in.a = 0; in.start = 0; } else launch_gate = 0;
		}
		gba_menu_av_poll();
		{
			static int pa, pb, pst;
			int ea = in.a && !pa, eb = in.b && !pb, est = in.start && !pst;
			int up = fire(0, in.up, dt), dn = fire(1, in.down, dt), lf = fire(2, in.left, dt), rt = fire(3, in.right, dt);
			int lb = fire(4, in.lb, dt), rb = fire(5, in.rb, dt);
			pa = in.a; pb = in.b; pst = in.start;
			if (ea || eb || est || up || dn || lf || rt || lb || rb) dirty = 1;
			if (S.lib) {
				int s = S.sys, n = S.syscount[s];
				if (up && S.lib_sel[s] > 0) S.lib_sel[s]--;
				if (dn && S.lib_sel[s] < n - 1) S.lib_sel[s]++;
				if (lb || lf) S.lib_sel[s] = S.lib_sel[s] > LIB_ROWS ? S.lib_sel[s] - LIB_ROWS : 0;
				if (rb || rt) S.lib_sel[s] = S.lib_sel[s] + LIB_ROWS < n ? S.lib_sel[s] + LIB_ROWS : n - 1;
				if (S.lib_sel[s] < S.lib_top[s]) S.lib_top[s] = S.lib_sel[s];
				if (S.lib_sel[s] >= S.lib_top[s] + LIB_ROWS) S.lib_top[s] = S.lib_sel[s] - LIB_ROWS + 1;
				if (eb) { S.lib = 0; S.dip = .6f; }
				if (ea || est) rom = s_libidx[s][S.lib_sel[s]];
			} else {
				if (lb && S.tab > 0) { S.tab--; S.dip = .45f; }
				if (rb && S.tab < 2) { S.tab++; S.dip = .45f; }
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
				} else if (S.tab == TAB_RESUME) {
					if (up && S.res_sel > 0) S.res_sel--;
					if (dn && S.res_sel < S.nrec - 1) S.res_sel++;
					if ((ea || est) && S.nrec) rom = S.rec[S.res_sel];
				} else {
					int dir = rt ? 1 : lf ? -1 : 0;
					if (up && S.set_sel > 0) S.set_sel--;
					if (dn && S.set_sel < SET_N - 1) S.set_sel++;
					if (S.set_sel == SET_BRIGHT && dir) { ayaneo_brightness_step(dir); ayaneo_menu_settings_persist(); }
					if (S.set_sel == SET_VOLUME && dir) {
						int v = ayaneo_gbc_audio_get_volume() + dir * 5;
						ayaneo_gbc_audio_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
						ayaneo_menu_settings_persist();
					}
					if (S.set_sel == SET_MUTE_BIOS && (dir || ea)) { ayaneo_set_mute_bios(!ayaneo_get_mute_bios()); ayaneo_menu_settings_persist(); }
					if (S.set_sel == SET_BFI && (dir || ea)) {
						ayaneo_set_strobe((ayaneo_get_strobe() + (dir ? dir : 1) + 4) % 4);
						ayaneo_menu_settings_persist();
					}
					if (S.set_sel == SET_THEME && dir && A.nthemes) S.theme_pick = (S.theme_pick + dir + A.nthemes) % A.nthemes;
					if (S.set_sel == SET_COLOUR && (dir || ea)) {	/* live: the backdrop is made again */
						colour_set((g_colour + (dir ? dir : 1) + NCOLOURS) % NCOLOURS);
						ayaneo_set_menu_colour(g_colour);
						ayaneo_menu_settings_persist();
					}
					if (S.set_sel == SET_THEME && ea && A.nthemes) {
						unsigned int h = theme_hash(S.theme_pick);
						if (h != ayaneo_get_menu_theme()) {
							ayaneo_set_menu_theme(h);
							ayaneo_menu_settings_persist();
							ayaneo_settings_persist_flush();   /* written before the card is read again */
							ayaneo_set_cpu_mhz(saved_mhz);
							return -3;
						}
					}
					if (S.set_sel == SET_BOOT_OS && ea) ayaneo_boot_to_os();   /* never returns */
				}
			}
		}
		if (S.sys_t < 1) { S.sys_t += dt / .46f; if (S.sys_t >= 1) { S.sys_t = 1; S.sys_from = S.sys; dirty = 1; } }
		if (S.dip > 0) { S.dip -= dt / .28f; if (S.dip <= 0) { S.dip = 0; dirty = 1; } }

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
				ui_compose(); dirty = 0;
				g_perf[4] = gpt4_get_current_tick() - c0;   /* oem diag rest: the last menu redraw */
			}
			frame_render();
			show();
			us = (gpt4_get_current_tick() - r0) / 13u;
			g_dbg_render_us = us;
			if (us > peak) peak = us;
			if (r0 - t_peak > 26000000u) { g_dbg_peak_us = peak; peak = 0; t_peak = r0; }
			frames++;
			if (r0 - t_fps >= 13000000u) { g_dbg_fps = frames; frames = 0; t_fps = r0; }
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
