/*
 * pico_classic_ui.c - the parts the Classic themes' own screens are drawn with (pico_classic_ui.h),
 * and the kit the in-game sheet draws from.
 *
 * The kit: the menu's pack is gone once a game loads (its memory is the game's), so as a game
 * starts from a Classic theme a pack in the menu's own format is built in memory no game and no
 * opening writes: the fonts l, m and s (their pages as grey and alpha, two bytes a pixel, since a
 * page keeps a dark edge round each letter) and a small atlas of the sprites below, cut from the
 * theme's atlas. The renderer reads it as it reads the menu's pack.
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include "pico_classic_ui.h"
#include "pico_classic_igm.h"
#include "pico_i18n.h"
#include "pico_names.h"
#include "pico_libmem.h"

extern unsigned int gba_menu_theme(void);

#if defined(KIT_PA)
/* given from outside (the host preview) */
#elif defined(KONKR_KPA)
#define KIT_PA   0x54258000u      /* the Current theme's in-game pack and panel background otherwise */
#define KIT_MAX  0x001E8000u      /* up to the live layer's second buffer (0x54440000) */
#elif defined(KONKR_BLOCK)
#define KIT_PA   0x552B0000u
#define KIT_MAX  0x00310000u      /* ends 0x555C0000, below the frozen frame */
#endif

#define KIT_MAGIC   0x4B494743u   /* "CGIK" */
#define KIT_PACK    4096u         /* where its pack starts */
#define KIT_ATLAS_W 256
#define ATLAS_ID    "ff9fb46c-ad68-46f6-9e87-7108e2080773."   /* the menu's UI atlas: its sprites' ids */

static const char *const k_spr[KS_N] = {
	"dialog_corner.png", "dialog_line.png", "cursor_option.png", "switch_on.png", "switch_off.png",
	"arrow_Small_left.png", "arrow_Small_right.png", "Icon_direction_light.png", "icon_A_light.png",
	"icon_B_light.png", "btn_display.png", "btn_option.png", "icon_Resume.png",
	"scrollbar_allowup_on.png", "scrollbar_allowdown_on.png",
	/* the menubar's square cursor: corners 1 3 7 9, edges 2 4 6 8 */
	"cursor_1.png", "cursor_game_2.png", "cursor_3.png", "cursor_game_4.png", "cursor_game_6.png",
	"cursor_7.png", "cursor_game_8.png", "cursor_9.png",
	"radiobtn_off.png", "radiobtn_on.png", "check_on.png", "btn_language.png", "btn_copyright.png",
	"btn_link.png",
};
static const char *const k_font[3] = { "l.font", "m.font", "s.font" };
static const float k_font_lh[3] = { 47.0f, 31.0f, 24.0f };

uint32_t cui_font(int f) { return snes_hash(k_font[f < 0 || f > 2 ? 1 : f]); }

void cui_lang_fonts(void)
{
	static const char *const name[3] = { "c_l", "c_m", "c_s" };
	int i;
	for (i = 0; i < 3; i++) {
		uint32_t len = 0;
		const uint8_t *e = pico_lang_entry(name[i], 3, &len);
		const uint16_t *h = (const uint16_t *)e;
		if (e && len >= 16 && 16u + h[3] * (uint32_t)sizeof(snes_glyph) + (uint32_t)h[4] * h[5] <= len)
			snes_font_more(i, cui_font(i), (const snes_glyph *)(e + 16), h[3],
				       e + 16 + h[3] * sizeof(snes_glyph), h[4], h[5]);
		else
			snes_font_more(i, 0, 0, 0, 0, 0, 0);
	}
}

/* ---- game names in any script ----
 * The themes' fonts hold Latin letters only. The card's names pack (pico_names.c, which the Current
 * theme draws its names with) has every letter of the games' names: its glyphs, scaled to each font
 * of a pack and put in that font's own measures, are added to the font (snes_font_more_pk), so a
 * name in Japanese, Chinese or Korean shows as it is. The home menu's pack takes slots 4 to 11, the
 * in-game kit's 12 to 15 (0 to 2 are the language's). */
#define NM_SLOTS 16
static struct { snes_glyph *g; int cap; } s_nm_slot[NM_SLOTS];
static unsigned int s_nm_serial[NM_SLOTS];
static const snes_pack *s_nm_pk[NM_SLOTS];

/* the size in pixels a font's letters were drawn at: from its capital H (about 0.7 of the size in
 * the themes' fonts), else from its line height */
static int font_size_px(const snes_pack *pk, const snes_font_entry *fe)
{
	const snes_glyph *g = (const snes_glyph *)(pk->base + fe->glyphs);
	int lo = 0, hi = (int)fe->glyph_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (g[mid].cp == 'H') return g[mid].h > 0 ? (g[mid].h * 10 + 3) / 7 : (int)fe->line_height * 4 / 5;
		if (g[mid].cp < 'H') lo = mid + 1; else hi = mid - 1;
	}
	return (int)fe->line_height * 4 / 5;
}

int cui_names_ready(void)
{
	const pico_nglyph *g;
	const uint8_t *a;
	int w;
	return pico_names_set(24, 0, &g, &a, &w) > 0;
}

void cui_names_fonts(const snes_pack *pk, int slot0, int nslots)
{
	int done[NM_SLOTS], k = 0, i, j;
	if (!pk || slot0 < 0 || nslots <= 0 || slot0 + nslots > NM_SLOTS) return;
	if (s_nm_pk[slot0] == pk && s_nm_serial[slot0] == pico_names_serial()) return;	/* (as they are) */
	s_nm_pk[slot0] = pk;
	s_nm_serial[slot0] = pico_names_serial();
	for (i = 0; i <= (int)pk->res_mask && k < nslots; i++) {
		const snes_res_entry *r = &pk->res[i];
		const snes_font_entry *fe;
		const pico_nglyph *ng = 0;
		const uint8_t *at = 0;
		int aw = 0, ah = 0, n, seen = 0, s = slot0 + k;
		if (!r->id_hash || r->type != RES_FONT) continue;
		for (j = 0; j < k; j++) if (done[j] == r->index) seen = 1;
		if (seen) continue;			/* (one font under several names) */
		done[k] = r->index;
		fe = &pk->font[r->index];
		n = pico_names_set(font_size_px(pk, fe), 0, &ng, &at, &aw);
		if (n > 0 && s_nm_slot[s].cap < n) {
			s_nm_slot[s].g = (snes_glyph *)pico_lib_alloc((unsigned)n * (unsigned)sizeof(snes_glyph));
			s_nm_slot[s].cap = s_nm_slot[s].g ? n : 0;
		}
		if (n <= 0 || !s_nm_slot[s].g) {
			snes_font_more_pk(s, pk, 0, 0, 0, 0, 0, 0);
			k++;
			continue;
		}
		for (j = 0; j < n; j++) {		/* from the pen on the baseline to the line's top */
			snes_glyph *o = &s_nm_slot[s].g[j];
			o->cp = ng[j].code;
			o->x = ng[j].x; o->y = ng[j].y; o->w = ng[j].w; o->h = ng[j].h;
			o->xo = ng[j].xo;
			o->yo = (int16_t)((int)fe->base + ng[j].yo);
			o->xadv = (int16_t)((ng[j].adv + 32u) / 64u);
			o->pad = 0;
			if (ng[j].y + ng[j].h > ah) ah = ng[j].y + ng[j].h;
		}
		snes_font_more_pk(s, pk, r->id_hash, s_nm_slot[s].g, n, at, aw, ah);
		k++;
	}
	for (; k < nslots; k++) snes_font_more_pk(slot0 + k, pk, 0, 0, 0, 0, 0, 0);
}

void cui_utf8_whole(char *s)
{
	int L = (int)strlen(s), k = L, need;
	unsigned char c;
	while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) k--;	/* the last letter's start */
	if (k == 0) return;
	c = (unsigned char)s[k - 1];
	need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
	if (L - (k - 1) < need) s[k - 1] = 0;
}

const char *cui_order(const char *s)
{
	static char buf[4][256];
	static int k;
	if (!s) return s;
	k = (k + 1) & 3;
	return pico_bidi(s, buf[k], sizeof buf[k]);
}

/* one pixel of a source image as 0xAARRGGBB */
static uint32_t src_px(const snes_pack *pk, const snes_img_entry *im, int x, int y)
{
	const uint8_t *p = snes_img_pixels(pk, im);
	if (im->flags & SNES_IMG_RGB565) {
		unsigned v;
		p += ((unsigned)y * im->w + (unsigned)x) * 3u;
		v = p[0] | (p[1] << 8);
		return (uint32_t)p[2] << 24 | (((v >> 11) & 31u) * 255u / 31u) << 16 |
		       (((v >> 5) & 63u) * 255u / 63u) << 8 | ((v & 31u) * 255u / 31u);
	}
	p += ((unsigned)y * im->w + (unsigned)x) * 4u;
	return (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* the sprites of a pack, all from its one atlas image */
static const snes_img_entry *find_sprites(const snes_pack *pk, const snes_spr_entry **sp)
{
	const snes_img_entry *atlas = 0;
	char id[96];
	int i;
	for (i = 0; i < KS_N; i++) {
		snprintf(id, sizeof id, ATLAS_ID "%s", k_spr[i]);
		sp[i] = snes_res_spr(pk, snes_hash(id));
		if (sp[i] && !atlas) atlas = &pk->img[sp[i]->img];
		if (sp[i] && &pk->img[sp[i]->img] != atlas) sp[i] = 0;
	}
	if (!atlas || (atlas->flags & SNES_IMG_LA88) || !sp[KS_LINE] || !sp[KS_CORNER] || !sp[KS_BAR]) return 0;
	return atlas;
}
static void line_colours(struct cui_set *set, const snes_pack *pk, const snes_img_entry *atlas, const snes_spr_entry *l)
{
	set->line_edge = src_px(pk, atlas, l->sx + l->sw / 2, l->sy);
	set->line_core = src_px(pk, atlas, l->sx + l->sw / 2, l->sy + l->sh / 2);
}

int cui_set_from_pack(struct cui_set *set, const snes_pack *pk)
{
	const snes_spr_entry *sp[KS_N];
	const snes_img_entry *atlas = find_sprites(pk, sp);
	int i;
	memset(set, 0, sizeof *set);
	if (!atlas) return 0;
	set->img = (uint16_t)(atlas - pk->img);
	for (i = 0; i < KS_N; i++)
		if (sp[i]) { set->r[i][0] = sp[i]->sx; set->r[i][1] = sp[i]->sy; set->r[i][2] = sp[i]->sw; set->r[i][3] = sp[i]->sh; }
	line_colours(set, pk, atlas, sp[KS_LINE]);
	return 1;
}

/* ==== the kit ==================================================================================== */
struct ckit {
	uint32_t magic, size, sum, theme;
	struct cui_set set;           /* the sprites in the kit's own atlas (image 0) */
	uint32_t pack_len;
};

#ifdef KIT_PA
static uint32_t kit_sum(void)
{
	const struct ckit *k = (const struct ckit *)KIT_PA;
	const uint32_t *p = (const uint32_t *)KIT_PA;
	const snes_header *h = (const snes_header *)(KIT_PA + KIT_PACK);
	uint32_t s = 2166136261u, n, i;
	for (i = 3; i < sizeof(struct ckit) / 4; i++) s = (s ^ p[i]) * 16777619u;   /* past magic, size, sum */
	/* the pack's header and tables (the glyphs too), then a word every 4 KB of its pixels */
	p = (const uint32_t *)h;
	n = k->pack_len / 4;
	for (i = 0; i < n && i < 16384; i++) s = (s ^ p[i]) * 16777619u;
	for (; i < n; i += 1024) s = (s ^ p[i]) * 16777619u;
	return s;
}

#endif

void pico_classic_kit_take(const snes_pack *pk)
{
#ifdef KIT_PA
	struct ckit *k = (struct ckit *)KIT_PA;
	uint8_t *pb = (uint8_t *)(KIT_PA + KIT_PACK);
	snes_header *h = (snes_header *)pb;
	snes_res_entry *res;
	snes_img_entry *img;
	snes_font_entry *fnt;
	const snes_spr_entry *sp[KS_N];
	const snes_img_entry *atlas;
	uint32_t o, room = KIT_MAX - KIT_PACK;
	int i, x = 0, y = 0, rowh = 0, bpp, ah;
	k->magic = 0;   /* not a kit while it is built */
	if (!pk) return;
	memset(k, 0, sizeof *k);
	/* the sprites, all from the one atlas image (the theme's pixels are in it by now) */
	if (!(atlas = find_sprites(pk, sp))) return;
	bpp = (atlas->flags & SNES_IMG_RGB565) ? 3 : 4;
	for (i = 0; i < KS_N; i++) {	/* shelves 256 wide, a pixel apart */
		if (!sp[i] || sp[i]->sw > KIT_ATLAS_W) continue;
		if (x + sp[i]->sw > KIT_ATLAS_W) { x = 0; y += rowh + 1; rowh = 0; }
		k->set.r[i][0] = (uint16_t)x; k->set.r[i][1] = (uint16_t)y; k->set.r[i][2] = sp[i]->sw; k->set.r[i][3] = sp[i]->sh;
		x += sp[i]->sw + 1;
		if (sp[i]->sh > rowh) rowh = sp[i]->sh;
	}
	ah = y + rowh;
	line_colours(&k->set, pk, atlas, sp[KS_LINE]);   /* the line's colours: its edge row and its middle */
	/* the pack: header, an empty string pool and start block, 8 resource slots, 4 images, 3 fonts */
	memset(pb, 0, 512);
	o = (sizeof(snes_header) + 15u) & ~15u;
	h->strpool_off = o; h->strpool_len = 16; o += 16;
	h->init_off = o; o += 16;
	h->res_off = o; h->res_count = 8; res = (snes_res_entry *)(pb + o); o += 8 * sizeof(snes_res_entry);
	h->img_off = o; h->img_count = 4; img = (snes_img_entry *)(pb + o); o += 4 * sizeof(snes_img_entry);
	h->spr_off = o;
	h->font_off = o; h->font_count = 3; fnt = (snes_font_entry *)(pb + o); o += 3 * sizeof(snes_font_entry);
	h->scene_off = h->anim_off = h->sanim_off = h->snd_off = h->str_off = h->game_off = o;
	o = (o + 15u) & ~15u;
	for (i = 0; i < 3; i++) {	/* each font's glyphs, as they are */
		const snes_font_entry *fe = snes_res_font(pk, cui_font(i));
		uint32_t gl, slot;
		if (!fe) return;
		gl = fe->glyph_count * (uint32_t)sizeof(snes_glyph);
		if (o + gl > room) return;
		memcpy(pb + o, pk->base + fe->glyphs, gl);
		fnt[i] = *fe;
		fnt[i].page = (uint16_t)(1 + i);
		fnt[i].glyphs = o;
		o = (o + gl + 15u) & ~15u;
		slot = cui_font(i) & 7u;
		while (res[slot].id_hash) slot = (slot + 1u) & 7u;
		res[slot].id_hash = cui_font(i); res[slot].type = RES_FONT; res[slot].index = (uint16_t)i;
	}
	/* the atlas, in the theme's own pixel layout */
	if (o + (uint32_t)KIT_ATLAS_W * ah * bpp > room) return;
	img[0].w = KIT_ATLAS_W; img[0].h = (uint16_t)ah; img[0].flags = atlas->flags & SNES_IMG_RGB565; img[0].pixels = o;
	memset(pb + o, 0, (size_t)KIT_ATLAS_W * ah * bpp);
	for (i = 0; i < KS_N; i++) {
		int j;
		if (!k->set.r[i][2]) continue;
		for (j = 0; j < sp[i]->sh; j++)
			memcpy(pb + o + ((uint32_t)(k->set.r[i][1] + j) * KIT_ATLAS_W + k->set.r[i][0]) * bpp,
			       snes_img_pixels(pk, atlas) + ((uint32_t)(sp[i]->sy + j) * atlas->w + sp[i]->sx) * bpp,
			       (size_t)sp[i]->sw * bpp);
	}
	o = (o + (uint32_t)KIT_ATLAS_W * ah * bpp + 15u) & ~15u;
	for (i = 0; i < 3; i++) {	/* each font's page as grey and alpha */
		const snes_img_entry *pg = &pk->img[snes_res_font(pk, cui_font(i))->page];
		const uint8_t *s = snes_img_pixels(pk, pg);
		uint8_t *d = pb + o;
		uint32_t n = (uint32_t)pg->w * pg->h, q;
		if (o + n * 2u > room || (pg->flags & SNES_IMG_LA88)) return;
		if (pg->flags & SNES_IMG_RGB565)
			for (q = 0; q < n; q++, s += 3, d += 2) {
				unsigned g6 = ((s[0] | (s[1] << 8)) >> 5) & 63u;
				d[0] = (uint8_t)((g6 << 2) | (g6 >> 4)); d[1] = s[2];
			}
		else
			for (q = 0; q < n; q++, s += 4, d += 2) { d[0] = s[1]; d[1] = s[3]; }
		img[1 + i].w = pg->w; img[1 + i].h = pg->h; img[1 + i].flags = SNES_IMG_LA88; img[1 + i].pixels = o;
		o = (o + n * 2u + 15u) & ~15u;
	}
	h->magic = SNESPACK_MAGIC; h->version = SNESPACK_VERSION; h->total_size = o;
	k->pack_len = o;
	k->set.img = 0;
	k->theme = gba_menu_theme();
	k->size = KIT_PACK + o;
	k->sum = kit_sum();
	k->magic = KIT_MAGIC;
#else
	(void)pk;
#endif
}


int cui_kit_open(snes_pack *pk, const struct cui_set **set)
{
#ifdef KIT_PA
	const struct ckit *k = (const struct ckit *)KIT_PA;
	if (k->magic != KIT_MAGIC || k->size > KIT_MAX || k->pack_len > KIT_MAX - KIT_PACK ||
	    k->theme != gba_menu_theme() || kit_sum() != k->sum)
		return 0;
	if (snes_pack_open(pk, (const void *)(KIT_PA + KIT_PACK), k->pack_len) != 0) return 0;
	*set = &k->set;
	return 1;
#else
	(void)pk; (void)set;
	return 0;
#endif
}

/* ==== drawing ==================================================================================== */
static const snes_pack *u_pk;
static const struct cui_set *u_set;
static snes_target *u_t;

void cui_use(const snes_pack *pk, const struct cui_set *set, snes_target *t) { u_pk = pk; u_set = set; u_t = t; }

float cui_clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
/* 2^y for y <= 0: the whole part as a shift, the rest a short series */
static float pow2n(float y)
{
	int n = (int)y;
	float f;
	if ((float)n > y) n--;
	if (n < -30) return 0;
	f = y - (float)n;
	return (1.0f + f * (0.6931472f + f * (0.2402265f + f * 0.0555041f))) / (float)(1u << -n);
}
float cui_out_expo(float t) { t = cui_clamp01(t); return t >= 1 ? 1 : 1 - pow2n(-10 * t); }
float cui_in_expo(float t) { t = cui_clamp01(t); return t <= 0 ? 0 : pow2n(10 * (t - 1)); }
float cui_smooth(float t) { t = cui_clamp01(t); return t * t * (3 - 2 * t); }

int cui_has(int k) { return u_set && k >= 0 && k < KS_N && u_set->r[k][2]; }

static void entry(int k, snes_spr_entry *s)
{
	s->img = u_set->img; s->sx = u_set->r[k][0]; s->sy = u_set->r[k][1]; s->sw = u_set->r[k][2]; s->sh = u_set->r[k][3];
	s->px = (int16_t)(s->sw / 2); s->py = (int16_t)(s->sh / 2);
}
void cui_spr(int k, float cx, float cy, float w, float h, float a, float r, float g, float b)
{
	snes_spr_entry s;
	if (!cui_has(k) || a <= 0) return;
	entry(k, &s);
	snes_blit_spr_wh_tint(u_t, u_pk, &s, cx, cy, w, h, a, r, g, b);
}
void cui_spr_x(int k, float cx, float cy, float scale, float a)
{
	if (cui_has(k)) cui_spr(k, cx, cy, u_set->r[k][2] * scale, u_set->r[k][3] * scale, a, 1, 1, 1);
}
void cui_spr_flip(int k, float cx, float cy, float scale, int hf, int vf)
{
	snes_spr_entry s;
	if (!cui_has(k)) return;
	entry(k, &s);
	snes_blit_spr_tint_flip(u_t, u_pk, &s, cx, cy, scale, 1, 1, 1, 1, hf, vf);
}
void cui_quad(float x, float y, float w, float h, uint32_t argb, float a)
{
	snes_fill_quad(u_t, x + w / 2, y + h / 2, w, h, ((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f,
		       (argb & 255) / 255.0f, a);
}
void cui_fill(float x, float y, float w, float h, uint32_t argb)
{
	float b = snes_target_base(u_t);
	int x0 = (int)(x * b + .5f) + u_t->offx, x1 = (int)((x + w) * b + .5f) + u_t->offx;
	int y0 = (int)(y * b + .5f) + u_t->offy, y1 = (int)((y + h) * b + .5f) + u_t->offy, yy, xx;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > u_t->W) x1 = u_t->W;
	if (y1 > u_t->H) y1 = u_t->H;
	for (yy = y0; yy < y1; yy++) {
		uint32_t *r = u_t->fb + (unsigned)yy * u_t->pitch;
		for (xx = x0; xx < x1; xx++) r[xx] = argb;
	}
}
void cui_tex(const snes_img_entry *im, float cx, float cy, float w, float h, float a)
{
	if (im && a > 0) snes_blit_tex(u_t, u_pk, im, cx, cy, w, h, a);
}
const char *cui_say(int f, const char *en)
{
	const char *t = tr(en);
	if (t != en && !snes_font_covers(u_pk, snes_res_font(u_pk, cui_font(f)), t)) return en;
	return t;
}
float cui_text_w(int f, float sc, const char *s) { return snes_text_width(u_pk, cui_font(f), sc, s); }
float cui_text(int f, float x, float cy, float sc, uint32_t argb, int align, const char *s, float a)
{
	uint32_t al = (uint32_t)((float)((argb >> 24) & 255) * cui_clamp01(a));
	if (al)
		snes_draw_text(u_t, u_pk, cui_font(f), x, cy - k_font_lh[f] * sc * 0.5f, sc, (argb & 0xFFFFFFu) | al << 24, align,
			       cui_order(s));
	return cui_text_w(f, sc, s);
}
const char *cui_fit(int f, float sc, const char *s, float maxw, char *out, unsigned cap)
{
	unsigned n;
	if (cui_text_w(f, sc, s) <= maxw) return s;
	n = (unsigned)strlen(s);
	if (n > cap - 4) n = cap - 4;
	while (n > 0) {
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;   /* at a letter's start */
		memcpy(out, s, n);
		memcpy(out + n, "...", 4);
		if (cui_text_w(f, sc, out) <= maxw) return out;
		n--;
	}
	return "...";
}
void cui_cursor_box(float cx, float cy, float w, float h, float a, float clock)
{
	const float cw = 32.0f, hw = w / 2, hh = h / 2, ew = w - cw + 1, eh = h - cw + 1;
	float pt = clock - 2.25f * (float)(int)(clock / 2.25f);
	float v = pt < 0.5f ? 0.75f + pt / 0.5f * 0.25f : pt < 1.25f ? 1.0f : pt < 2.0f ? 1.0f - (pt - 1.25f) / 0.75f * 0.25f : 0.75f;
	cui_spr(KS_CUR1, cx - hw, cy - hh, cw, cw, a, v, v, 1); cui_spr(KS_CUR2, cx, cy - hh, ew, cw, a, v, v, 1);
	cui_spr(KS_CUR3, cx + hw, cy - hh, cw, cw, a, v, v, 1);
	cui_spr(KS_CUR4, cx - hw, cy, cw, eh, a, v, v, 1); cui_spr(KS_CUR6, cx + hw, cy, cw, eh, a, v, v, 1);
	cui_spr(KS_CUR7, cx - hw, cy + hh, cw, cw, a, v, v, 1); cui_spr(KS_CUR8, cx, cy + hh, ew, cw, a, v, v, 1);
	cui_spr(KS_CUR9, cx + hw, cy + hh, cw, cw, a, v, v, 1);
}
void cui_frame(float x0, float y0, float x1, float y1)
{
	float w = x1 - x0, h = y1 - y0;
	cui_fill(x0 + 12, y0 + 12, w - 24, h - 24, 0xFF000000u);
	cui_spr(KS_LINE, x0 + w / 2, y0 + 6, w - 72, 12, 1, 1, 1, 1);
	cui_spr(KS_LINE, x0 + w / 2, y1 - 6, w - 72, 12, 1, 1, 1, 1);
	{	/* the sides: the line's edge, middle and edge, upright (a unit into the corners: no gap) */
		float sy = y0 + 35, sh = h - 70;
		cui_quad(x0, sy, 3, sh, u_set->line_edge, 1); cui_quad(x0 + 3, sy, 6, sh, u_set->line_core, 1);
		cui_quad(x0 + 9, sy, 3, sh, u_set->line_edge, 1);
		cui_quad(x1 - 12, sy, 3, sh, u_set->line_edge, 1); cui_quad(x1 - 9, sy, 6, sh, u_set->line_core, 1);
		cui_quad(x1 - 3, sy, 3, sh, u_set->line_edge, 1);
	}
	cui_spr_flip(KS_CORNER, x0 + 18, y0 + 18, 3, 0, 0); cui_spr_flip(KS_CORNER, x1 - 18, y0 + 18, 3, 1, 0);
	cui_spr_flip(KS_CORNER, x0 + 18, y1 - 18, 3, 0, 1); cui_spr_flip(KS_CORNER, x1 - 18, y1 - 18, 3, 1, 1);
}
float cui_hints(float x_right, float y, int n, const int *icons, const char *const *labels)
{
	float iw[6], lw[6], total = 0, x, left;
	int i;
	if (n > 6) n = 6;
	for (i = 0; i < n; i++) {
		iw[i] = (cui_has(icons[i]) ? u_set->r[icons[i]][2] : 12) * 2.55f;
		lw[i] = cui_text_w(CF_S, .9f, labels[i]);
		total += iw[i] + 6 + lw[i];
	}
	x = left = x_right - total - 41 * (n - 1);
	for (i = 0; i < n; i++) {
		cui_spr_x(icons[i], x + iw[i] / 2, y, 2.55f, 1);
		snes_draw_text(u_t, u_pk, cui_font(CF_S), x + iw[i] + 6, y - 10, .9f, 0xFFF2F2F2u, 0, cui_order(labels[i]));
		x += iw[i] + 6 + lw[i] + 41;
	}
	return left;
}

/* ==== a sheet: the menubar pages' shape for screens of the LK's own (the Link screens) ============ */
#define SHEET_ROW  60.0f
#define SHEET_ROWS 6

/* words of s from reading order, as many lines as fit maxw across, each drawn in drawing order;
 * the y of the line after */
static float sheet_wrap(int f, float x, float y, float sc, float pitch, float maxw, uint32_t argb, const char *s)
{
	char line[200], trial[200];
	int ll = 0;
	while (s && *s) {
		const char *w = s;
		int wl, tl;
		while (*s && *s != ' ' && *s != '\n') s++;
		wl = (int)(s - w);
		if (wl > 120) wl = 120;
		tl = ll ? ll + 1 + wl : wl;
		if (tl >= (int)sizeof trial) tl = (int)sizeof trial - 1;
		if (ll) { memcpy(trial, line, (unsigned)ll); trial[ll] = ' '; memcpy(trial + ll + 1, w, (unsigned)(tl - ll - 1)); }
		else memcpy(trial, w, (unsigned)tl);
		trial[tl] = 0;
		if (ll && cui_text_w(f, sc, cui_order(trial)) > maxw) {	/* a full line: out, the word starts the next */
			line[ll] = 0;
			cui_text(f, x, y, sc, argb, 0, line, 1);
			y += pitch;
			memcpy(line, w, (unsigned)wl); ll = wl;
		} else {
			memcpy(line, trial, (unsigned)tl); ll = tl;
		}
		if (*s == '\n' && ll) {	/* a line ends here */
			line[ll] = 0;
			cui_text(f, x, y, sc, argb, 0, line, 1);
			y += pitch;
			ll = 0;
		}
		while (*s == ' ' || *s == '\n') s++;
	}
	if (ll) { line[ll] = 0; cui_text(f, x, y, sc, argb, 0, line, 1); y += pitch; }
	return y;
}

void cui_sheet(float top, const struct cui_sheet *s)
{
	static const int ic_of[3] = { KS_BTN_A, KS_BTN_B, KS_DPAD };
	const float bottom = top + 648.0f;
	int icons[5], i, k, n = 0, first;
	const char *labels[5];
	char fb[160];
	float room, y, tw;
	cui_frame(64, top, 1216, bottom);
	/* the header: the icon and the title at the left, the buttons at the right */
	/* in the pages' order: Select, then Back, then the rest (OK) */
	if (s->nrows > 1) { icons[n] = KS_DPAD; labels[n++] = cui_say(CF_S, "Select"); }
	for (k = 0; k < 2; k++)
		for (i = 0; i < s->nhints && n < 5; i++) {
			if ((s->hint_btn[i] == 'B') != !k) continue;
			icons[n] = ic_of[s->hint_btn[i] == 'B' ? 1 : s->hint_btn[i] == 'A' ? 0 : 2];
			labels[n++] = s->hint_label[i] ? s->hint_label[i] : "";
		}
	room = (n ? cui_hints(1180, top + 38, n, icons, labels) : 1180) - 32 - 176;
	cui_spr(s->icon, 128, top + 39, 72, 72, 1, 1, 1, 1);
	if (s->busy) room -= 72;
	tw = cui_text(CF_L, 176, top + 38, 1, 0xFFF4F4F4u, 0, cui_fit(CF_L, 1, s->title ? s->title : "", room, fb, sizeof fb), 1);
	if (s->busy) {	/* three dots after the title, one lit at a time in the cursor's cyan */
		int lit = (int)(s->clock * 3.0f) % 3;
		for (k = 0; k < 3; k++)
			cui_quad(176 + tw + 20 + 18 * (float)k, top + 46, 9, 9, k == lit ? 0xFF40C0FFu : 0xFF5A5A5Au, 1);
	}
	cui_spr(KS_LINE, 640, top + 72, 1134, 12, 1, 1, 1, 1);
	y = top + 118;
	if (s->subtitle && s->subtitle[0]) {	/* the game or the console it is about */
		cui_text(CF_M, 160, y, .95f, 0xFFB4B4B4u, 0, cui_fit(CF_M, .95f, s->subtitle, 960, fb, sizeof fb), 1);
		y += 58;
	} else y += 14;
	/* the list: the blue bar under the focused row, the value at the right */
	first = s->sel >= SHEET_ROWS ? s->sel - SHEET_ROWS + 1 : 0;
	for (i = first; i < s->nrows && i < first + SHEET_ROWS; i++, y += SHEET_ROW) {
		float vw = 0;
		if (i == s->sel) cui_spr(KS_BAR, 640, y, 1040, SHEET_ROW - 4, 1, 1, 1, 1);
		if (s->values[i] && s->values[i][0]) {
			const char *v = cui_fit(CF_M, 1, s->values[i], 420, fb, sizeof fb);
			vw = cui_text(CF_M, 1112, y, 1, 0xFFB4B4B4u, 2, v, 1);
		}
		if (s->rows[i])
			cui_text(CF_M, 160, y, 1, 0xFFF4F4F4u, 0, cui_fit(CF_M, 1, s->rows[i], 952 - (vw > 0 ? vw + 40 : 0), fb, sizeof fb), 1);
	}
	if (s->nrows > first + SHEET_ROWS) cui_spr_x(KS_DOWN, 1182, y - SHEET_ROW + 6, 1.5f, 1);
	if (first > 0) cui_spr_x(KS_UP, 1182, top + 104, 1.5f, 1);
	if (s->nrows) y += 10;
	/* what goes on, or why: wrapped across the page */
	if (s->status && s->status[0]) y = sheet_wrap(CF_M, 160, y, .9f, 44, 960, 0xFFD8D8D8u, s->status) + 8;
	/* how far: the cursor's cyan on a dark track, and the line under it */
	if (s->total) {
		unsigned int d = s->done < s->total ? s->done : s->total;
		float f = (float)d / (float)s->total;
		if (y > bottom - 110) y = bottom - 110;
		cui_quad(160, y, 960, 12, 0xFF2C3440u, 1);
		if (f > 0) cui_quad(160, y, 960 * f, 12, 0xFF40C0FFu, 1);
		if (s->progress && s->progress[0]) cui_text(CF_S, 160, y + 40, .9f, 0xFFB4B4B4u, 0, s->progress, 1);
	}
}
