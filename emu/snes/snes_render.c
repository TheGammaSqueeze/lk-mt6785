/*
 * Software renderer for the CLOVER scene graph, reproducing web/src/engine/
 * renderer.js: depth-first z-sorted painter, engine (+Y up) -> screen transform,
 * inverse-mapped affine sprite blit with RGB-multiply tint + straight alpha +
 * additive blend + flip + wallpaper tiling, and BMFont text.
 */
#include "snes_render.h"

/* ---- pixel helpers (source is RGB565+A8 or RGBA8888; fb is 0xAARRGGBB) ---- */
static inline void src_rgba(const uint8_t *px, int rgb565, int *r, int *g, int *b, int *a)
{
	if (rgb565) {
		unsigned v = px[0] | (px[1] << 8);
		*r = ((v >> 11) & 0x1f) << 3; *r |= *r >> 5;
		*g = ((v >> 5) & 0x3f) << 2;  *g |= *g >> 6;
		*b = (v & 0x1f) << 3;         *b |= *b >> 5;
		*a = px[2];
	} else {
		*r = px[0]; *g = px[1]; *b = px[2]; *a = px[3];
	}
}

typedef struct {
	const uint8_t *pix; int rgb565; int img_w, img_h;
	int sx, sy, sw, sh;          /* atlas frame */
	float dw, dh, px, py;        /* dest size + pivot (native px) */
	int hflip, vflip, tile, additive, is_quad;
	float uvx, uvy, uvrx, uvry;  /* wallpaper uv offset/repeat */
	float tr, tg, tb, ta;        /* tint 0..1 */
} snes_draw;

static inline int ifloor(float x) { int i = (int)x; return (x < 0 && (float)i != x) ? i - 1 : i; }

/* affine-blit one drawable using screen transform [a,b,c,d,e,f]:
 *   X = a*lx + c*ly + e ;  Y = b*lx + d*ly + f  (lx,ly local dest pixels) */
static void blit(snes_target *t, const float M[6], const snes_draw *d)
{
	float a = M[0], b = M[1], c = M[2], dd = M[3], e = M[4], f = M[5];
	/* local dest rect corners: [-px, dw-px] x [-py, dh-py] */
	float lx0 = -d->px, lx1 = d->dw - d->px, ly0 = -d->py, ly1 = d->dh - d->py;
	float cx[4], cy[4];
	float minx, maxx, miny, maxy;
	int i, X, Y, x0, x1, y0, y1;
	float det = a * dd - c * b;
	if (det == 0.0f || d->dw <= 0 || d->dh <= 0) return;
	cx[0] = a*lx0 + c*ly0 + e; cy[0] = b*lx0 + dd*ly0 + f;
	cx[1] = a*lx1 + c*ly0 + e; cy[1] = b*lx1 + dd*ly0 + f;
	cx[2] = a*lx0 + c*ly1 + e; cy[2] = b*lx0 + dd*ly1 + f;
	cx[3] = a*lx1 + c*ly1 + e; cy[3] = b*lx1 + dd*ly1 + f;
	minx = maxx = cx[0]; miny = maxy = cy[0];
	for (i = 1; i < 4; i++) {
		if (cx[i] < minx) minx = cx[i]; if (cx[i] > maxx) maxx = cx[i];
		if (cy[i] < miny) miny = cy[i]; if (cy[i] > maxy) maxy = cy[i];
	}
	x0 = ifloor(minx); x1 = ifloor(maxx) + 1;
	y0 = ifloor(miny); y1 = ifloor(maxy) + 1;
	if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
	if (x1 > SNES_VW) x1 = SNES_VW; if (y1 > SNES_VH) y1 = SNES_VH;
	{
	/* Integer tint (0..256) and source stride, hoisted out of the pixel loop. */
	int tr = (int)(d->tr * 256.0f), tg = (int)(d->tg * 256.0f);
	int tb = (int)(d->tb * 256.0f), ta = (int)(d->ta * 256.0f);
	int bpp = d->rgb565 ? 3 : 4;
	int axis = (b == 0.0f && c == 0.0f && a != 0.0f && dd != 0.0f);
	float inv_a = axis ? 1.0f / a : 0, inv_d = axis ? 1.0f / dd : 0;
	float du = axis ? inv_a / d->dw : 0;   /* u increment per X (no per-pixel divide) */
	for (Y = y0; Y < y1; Y++) {
		uint32_t *row = t->fb + (unsigned)(t->offy + Y) * t->pitch + t->offx;
		float fy = (float)Y + 0.5f - f;
		float u, v;
		if (axis) {
			v = (fy * inv_d + d->py) / d->dh;              /* one divide per row */
			u = (((float)x0 + 0.5f - e) * inv_a + d->px) / d->dw;
		}
		for (X = x0; X < x1; X++, u += du) {
			float uu, vv;
			int sr, sg, sb, sa, ix, iy, af, dr, dg, db;
			uint32_t dst;
			if (axis) {
				uu = u; vv = v;
			} else {
				float fx = (float)X + 0.5f - e;
				float lx = (dd * fx - c * fy) / det;
				float ly = (-b * fx + a * fy) / det;
				uu = (lx + d->px) / d->dw;
				vv = (ly + d->py) / d->dh;
			}
			if (d->hflip) uu = 1.0f - uu;
			if (d->vflip) vv = 1.0f - vv;
			if (d->is_quad) {
				if (uu < 0 || uu >= 1 || vv < 0 || vv >= 1) continue;
				sr = 255; sg = 255; sb = 255; sa = 255;
			} else {
				float su, sv;
				if (d->tile) {
					uu = uu * d->uvrx + d->uvx; vv = vv * d->uvry + d->uvy;
					uu -= ifloor(uu); vv -= ifloor(vv);
				} else if (uu < 0 || uu >= 1 || vv < 0 || vv >= 1) {
					continue;
				}
				su = d->sx + uu * d->sw; sv = d->sy + vv * d->sh;
				ix = (int)su; iy = (int)sv;
				if (ix < 0) ix = 0; if (ix >= d->img_w) ix = d->img_w - 1;
				if (iy < 0) iy = 0; if (iy >= d->img_h) iy = d->img_h - 1;
				{
					const uint8_t *sp = d->pix + ((unsigned)iy * d->img_w + ix) * bpp;
					src_rgba(sp, d->rgb565, &sr, &sg, &sb, &sa);
				}
			}
			if (sa == 0) continue;
			sr = (sr * tr) >> 8; sg = (sg * tg) >> 8; sb = (sb * tb) >> 8;
			af = (sa * ta) >> 8;
			if (af <= 0) continue;
			if (af > 255) af = 255;
			dst = row[X];
			dr = (dst >> 16) & 0xff; dg = (dst >> 8) & 0xff; db = dst & 0xff;
			if (d->additive) {
				dr += (sr * af) >> 8; dg += (sg * af) >> 8; db += (sb * af) >> 8;
			} else {
				int ia = 255 - af;
				dr = (sr * af + dr * ia + 127) / 255;    /* constant /255 -> mul+shift */
				dg = (sg * af + dg * ia + 127) / 255;
				db = (sb * af + db * ia + 127) / 255;
			}
			if (dr > 255) dr = 255; if (dg > 255) dg = 255; if (db > 255) db = 255;
			row[X] = 0xff000000u | (dr << 16) | (dg << 8) | db;
		}
	}
	}
}

/* ---- component resolution to a snes_draw ---- */
static int resolve_visual(snes_scene *s, const snes_comp *c, snes_rnode *n,
			  snes_draw *d)
{
	const snes_pack *pk = s->pk;
	const snes_comp_visual *cv = (const snes_comp_visual *)c;
	const snes_img_entry *im = 0;
	const snes_spr_entry *sp = 0;
	if (c->type == COMP_SPRITE || c->type == COMP_ANIMATED_SPRITE) {
		if (c->type == COMP_ANIMATED_SPRITE) {
			const snes_sanim_entry *sa = snes_res_sanim(pk, cv->res_hash);
			if (sa && sa->frame_count) {
				const snes_sanim_frame *fr = (const snes_sanim_frame *)(pk->base + sa->frames);
				sp = &pk->spr[fr[0].spr];
			}
		} else {
			sp = snes_res_spr(pk, cv->res_hash);
		}
		if (n->vis.ov_spr) sp = n->vis.ov_spr;
		if (!sp) return 0;
		im = &pk->img[sp->img];
		d->sx = sp->sx; d->sy = sp->sy; d->sw = sp->sw; d->sh = sp->sh;
		d->px = sp->px; d->py = sp->py;
		d->dw = sp->sw; d->dh = sp->sh;
	} else { /* COMP_TEXTURE */
		im = snes_res_img(pk, cv->res_hash);
		if (n->vis.ov_img) im = n->vis.ov_img;
		if (!im) return 0;
		d->sx = 0; d->sy = 0; d->sw = im->w; d->sh = im->h;
		d->px = im->w / 2.0f; d->py = im->h / 2.0f;
		d->dw = im->w; d->dh = im->h;
	}
	if (c->flags & SNES_COMP_HAS_SIZE) {
		/* pivot is native px; when stretched to size it scales with dw/sw */
		float sw = d->sw > 0 ? d->sw : 1, sh = d->sh > 0 ? d->sh : 1;
		d->dw = cv->size_w; d->dh = cv->size_h;
		d->px = (d->px) * (d->dw / sw);
		d->py = (d->py) * (d->dh / sh);
	}
	d->pix = snes_img_pixels(pk, im);
	d->rgb565 = (im->flags & SNES_IMG_RGB565) ? 1 : 0;
	d->img_w = im->w; d->img_h = im->h;
	d->hflip = (c->flags & SNES_COMP_HFLIP) ? 1 : 0;
	d->vflip = (c->flags & SNES_COMP_VFLIP) ? 1 : 0;
	d->additive = (c->blend == 1);
	d->is_quad = 0;
	d->tile = (im->flags & SNES_IMG_TILE) && (c->flags & SNES_COMP_HAS_SIZE) &&
		  (d->dw > d->sw + 1 || d->dh > d->sh + 1);
	if (d->tile) {
		d->uvrx = (cv->uv_rep_x != 0) ? cv->uv_rep_x : (d->dw / d->sw);
		d->uvry = (cv->uv_rep_y != 0) ? cv->uv_rep_y : (d->dh / d->sh);
		d->uvx = cv->uv_off_x + n->vis.uvx;
		d->uvy = cv->uv_off_y + n->vis.uvy;
	}
	return 1;
}

/* screen transform from an engine world matrix m[6] (renderer.js paint()). */
static void screen_matrix(const float m[6], float S[6])
{
	S[0] = m[0]; S[1] = -m[3]; S[2] = -m[1]; S[3] = m[4];
	S[4] = SNES_VW / 2.0f + m[2]; S[5] = SNES_VH / 2.0f - m[5];
}

/* ---- BMFont text (axis-aligned: translation + scale from the world matrix) ---- */
/* decode one UTF-8 codepoint from *p, advancing *p past it (labels are UTF-8:
 * accented Latin + Cyrillic glyphs are keyed by Unicode codepoint in the font). */
static uint32_t utf8_next(const char **pp)
{
	const uint8_t *p = (const uint8_t *)*pp;
	uint32_t c = *p++;
	if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
		c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
	} else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
		c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2;
	} else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) {
		c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1;
	}
	*pp = (const char *)p;
	return c;
}

static const snes_glyph *glyph_find(const snes_font_entry *fe, const snes_glyph *g, uint32_t cp)
{
	int lo = 0, hi = fe->glyph_count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (g[mid].cp == cp) return &g[mid];
		if (g[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}

static void draw_label(snes_target *t, snes_scene *s, const snes_comp *c,
		       const float m[6], const float col[4])
{
	const snes_pack *pk = s->pk;
	const snes_comp_label *lb = (const snes_comp_label *)c;
	const snes_font_entry *fe = snes_res_font(pk, lb->font_hash);
	const snes_glyph *glyphs;
	const snes_img_entry *page;
	const uint8_t *pgpix;
	const char *text = snes_str(pk, lb->text);
	float S[6], scale, penx, wpx = 0;
	/* localise: if the label carries an @key, resolve it in the active locale */
	if (lb->text_key) {
		const char *key = snes_str(pk, lb->text_key);
		if (key && key[0] == '@') key++;
		text = snes_text_locale(pk, pk->locale, key);
	}
	const char *p;
	int pgw, pgh, rgb565;
	if (!fe || !text || !text[0]) return;
	page = &pk->img[fe->page];
	pgpix = snes_img_pixels(pk, page); pgw = page->w; pgh = page->h;
	rgb565 = (page->flags & SNES_IMG_RGB565) ? 1 : 0;
	glyphs = (const snes_glyph *)(pk->base + fe->glyphs);
	screen_matrix(m, S);
	scale = S[0] < 0 ? -S[0] : S[0];         /* uniform-ish scale for text */
	if (scale < 0.01f) scale = 1.0f;
	/* measure for anchor */
	for (p = text; *p; ) {
		const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&p));
		wpx += g ? g->xadv : 0;   /* missing glyphs drop to zero width (web parity) */
	}
	penx = S[4];
	if (lb->h_anchor == ANCHOR_CENTER) penx -= wpx * scale / 2.0f;
	else if (lb->h_anchor == ANCHOR_RIGHT) penx -= wpx * scale;
	{
		float peny = S[5];
		float th = fe->line_height * scale;
		/* the CLOVER format encodes vertical Center as ANCHOR_CENTER(1); treat
		 * both that and MIDDLE(4) as vertical-centred (default Top = no offset) */
		if (lb->v_anchor == ANCHOR_MIDDLE || lb->v_anchor == ANCHOR_CENTER)
			peny -= th / 2.0f;
		else if (lb->v_anchor == ANCHOR_BOTTOM) peny -= th;
		for (p = text; *p; ) {
			const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&p));
			if (!g) continue;   /* missing glyphs drop to zero width (web parity) */
			if (g->w > 0) {
				snes_draw d;
				float gm[6];
				d.pix = pgpix; d.rgb565 = rgb565; d.img_w = pgw; d.img_h = pgh;
				d.sx = g->x; d.sy = g->y; d.sw = g->w; d.sh = g->h;
				d.dw = g->w * scale; d.dh = g->h * scale; d.px = 0; d.py = 0;
				d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
				d.tr = col[0]; d.tg = col[1]; d.tb = col[2]; d.ta = col[3];
				/* place glyph as an axis-aligned quad at pen (screen space) */
				gm[0] = 1; gm[1] = 0; gm[2] = 0; gm[3] = 1;
				gm[4] = penx + g->xo * scale; gm[5] = peny + g->yo * scale;
				blit(t, gm, &d);
			}
			penx += g->xadv * scale;
		}
	}
}

/* Public: draw a string with a pack font at a screen position (x,y = pen origin;
 * align 0 left / 1 centre / 2 right about x). argb = 0xAARRGGBB. */
float snes_text_width(const snes_pack *pk, uint32_t font_hash, float scale, const char *text)
{
	const snes_font_entry *fe = snes_res_font(pk, font_hash);
	const snes_glyph *glyphs;
	const char *p;
	float w = 0;
	if (!fe || !text) return 0;
	glyphs = (const snes_glyph *)(pk->base + fe->glyphs);
	for (p = text; *p; ) {
		const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&p));
		w += g ? g->xadv : 0;   /* missing glyphs drop to zero width (web parity) */
	}
	return w * scale;
}

void snes_draw_text(snes_target *t, const snes_pack *pk, uint32_t font_hash,
		    float x, float y, float scale, uint32_t argb, int align,
		    const char *text)
{
	const snes_font_entry *fe = snes_res_font(pk, font_hash);
	const snes_glyph *glyphs;
	const snes_img_entry *page;
	const uint8_t *pgpix;
	const char *p;
	float col[4], penx, wpx = 0;
	int pgw, pgh, rgb565;
	if (!fe || !text) return;
	page = &pk->img[fe->page];
	pgpix = snes_img_pixels(pk, page); pgw = page->w; pgh = page->h;
	rgb565 = (page->flags & SNES_IMG_RGB565) ? 1 : 0;
	glyphs = (const snes_glyph *)(pk->base + fe->glyphs);
	col[0] = ((argb >> 16) & 0xff) / 255.0f; col[1] = ((argb >> 8) & 0xff) / 255.0f;
	col[2] = (argb & 0xff) / 255.0f; col[3] = ((argb >> 24) & 0xff) / 255.0f;
	for (p = text; *p; ) {
		const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&p));
		wpx += g ? g->xadv : 0;   /* missing glyphs drop to zero width (web parity) */
	}
	penx = x;
	if (align == 1) penx -= wpx * scale / 2.0f;
	else if (align == 2) penx -= wpx * scale;
	for (p = text; *p; ) {
		const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&p));
		if (!g) continue;   /* missing glyphs drop to zero width (web parity) */
		if (g->w > 0) {
			snes_draw d; float gm[6];
			d.pix = pgpix; d.rgb565 = rgb565; d.img_w = pgw; d.img_h = pgh;
			d.sx = g->x; d.sy = g->y; d.sw = g->w; d.sh = g->h;
			d.dw = g->w * scale; d.dh = g->h * scale; d.px = 0; d.py = 0;
			d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
			d.tr = col[0]; d.tg = col[1]; d.tb = col[2]; d.ta = col[3];
			gm[0] = 1; gm[1] = 0; gm[2] = 0; gm[3] = 1;
			gm[4] = penx + g->xo * scale; gm[5] = y + g->yo * scale;
			blit(t, gm, &d);
		}
		penx += g->xadv * scale;
	}
}

/* ---- collect + z-sort (mirrors renderer.js collect/drawScene) ---- */
typedef struct { const snes_comp *comp; snes_rnode *node; float m[6]; float col[4];
		 int layer; int seq; } snes_dr;

#define SNES_MAXDR 2048
static snes_dr g_dr[SNES_MAXDR];
static int g_ndr;

/* item for per-node z ordering. Kept in a global bump scratch (not the C stack)
 * so a deep tree does not overflow the thread stack: each collect frame reserves
 * its items on entry and rewinds on exit; children allocate beyond. */
typedef struct { int z, layer, g, i; const snes_comp *comp; snes_rnode *child; } zitem;
#define SNES_MAXZ 4096
static zitem g_zs[SNES_MAXZ];
static int g_zt;

static int is_drawable(int type)
{
	return type == COMP_SPRITE || type == COMP_TEXTURE ||
	       type == COMP_ANIMATED_SPRITE || type == COMP_LABEL;
}

static void collect(snes_scene *s, snes_rnode *n, const float pm[6],
		    const float pcol[4], int base_layer)
{
	float m[6], col[4];
	int base = g_zt;
	zitem *items = &g_zs[base];
	int ni = 0, k, kk;
	unsigned ci;
	if (!n->enabled || !n->visible) return;
	snes_mat_mul(pm, n->tf, m);
	col[0] = pcol[0]*n->col[0]; col[1] = pcol[1]*n->col[1];
	col[2] = pcol[2]*n->col[2]; col[3] = pcol[3]*n->col[3];
	if (n->base_layer_bg) base_layer = -1000;
	for (ci = 0; ci < n->def->comp_count && base + ni < SNES_MAXZ; ci++) {
		const snes_comp *c = snes_node_comp(s, n->def, ci);
		if (!(c->flags & SNES_COMP_ENABLED) || !(c->flags & SNES_COMP_VISIBLE)) continue;
		if (!is_drawable(c->type)) continue;
		items[ni].z = c->zindex; items[ni].layer = c->layer; items[ni].g = 0;
		items[ni].i = (int)ci; items[ni].comp = c; items[ni].child = 0; ni++;
	}
	{
		snes_rnode *ch; int idx = 0;
		for (ch = n->child; ch && base + ni < SNES_MAXZ; ch = ch->sib, idx++) {
			items[ni].z = ch->def->zindex; items[ni].layer = ch->min_layer;
			items[ni].g = 1; items[ni].i = idx; items[ni].comp = 0;
			items[ni].child = ch; ni++;
		}
	}
	g_zt = base + ni;   /* reserve; children collect() allocate beyond */
	/* stable insertion sort by (z, g, layer, i) */
	for (k = 1; k < ni; k++) {
		zitem key = items[k];
		kk = k - 1;
		while (kk >= 0) {
			zitem *a = &items[kk];
			int cmp = (a->z != key.z) ? a->z - key.z :
				  (a->g != key.g) ? a->g - key.g :
				  (a->layer != key.layer) ? a->layer - key.layer : a->i - key.i;
			if (cmp <= 0) break;
			items[kk + 1] = items[kk]; kk--;
		}
		items[kk + 1] = key;
	}
	for (k = 0; k < ni; k++) {
		if (items[k].comp) {
			if (g_ndr < SNES_MAXDR) {
				snes_dr *dr = &g_dr[g_ndr++];
				dr->comp = items[k].comp; dr->node = n;
				for (kk = 0; kk < 6; kk++) dr->m[kk] = m[kk];
				dr->col[0] = col[0]; dr->col[1] = col[1];
				dr->col[2] = col[2]; dr->col[3] = col[3];
				dr->layer = base_layer; dr->seq = g_ndr;
			}
		} else {
			collect(s, items[k].child, m, col, base_layer);
		}
	}
	g_zt = base;   /* rewind this frame's reservation */
}

static void paint(snes_target *t, snes_scene *s, snes_dr *dr)
{
	const snes_comp *c = dr->comp;
	float S[6];
	if (c->type == COMP_LABEL) { draw_label(t, s, c, dr->m, dr->col); return; }
	{
		snes_draw d;
		if (!resolve_visual(s, c, dr->node, &d)) return;
		d.tr = dr->col[0]; d.tg = dr->col[1]; d.tb = dr->col[2]; d.ta = dr->col[3];
		screen_matrix(dr->m, S);
		blit(t, S, &d);
	}
}

static void sort_and_paint(snes_target *t, snes_scene *s)
{
	int i, k;
	for (i = 1; i < g_ndr; i++) {
		snes_dr key = g_dr[i];
		k = i - 1;
		while (k >= 0 && ((g_dr[k].layer != key.layer) ? g_dr[k].layer > key.layer
							       : g_dr[k].seq > key.seq)) {
			g_dr[k + 1] = g_dr[k]; k--;
		}
		g_dr[k + 1] = key;
	}
	for (i = 0; i < g_ndr; i++)
		paint(t, s, &g_dr[i]);
}

void snes_render_scene(snes_target *t, snes_scene *s)
{
	static const float I[6] = {1,0,0,0,1,0};
	static const float W[4] = {1,1,1,1};
	g_ndr = 0; g_zt = 0;
	if (!s->root) return;
	collect(s, s->root, I, W, 0);
	sort_and_paint(t, s);
}

/* Render the subtree rooted at n at its authored world position (n's own
 * transform is applied on top of its parent's world matrix). */
void snes_render_node(snes_target *t, snes_scene *s, snes_rnode *n)
{
	static const float W[4] = {1,1,1,1};
	float pm[6] = {1,0,0,0,1,0};
	g_ndr = 0; g_zt = 0;
	if (!n) return;
	if (n->parent) snes_node_world(n->parent, pm);
	collect(s, n, pm, W, 0);
	sort_and_paint(t, s);
}

int snes_render_count(void) { return g_ndr; }

/* ---- direct-draw helpers (screen space; cx,cy = centre in the 1280x720 space) --- */
void snes_blit_tex(snes_target *t, const snes_pack *pk, const snes_img_entry *im,
		   float cx, float cy, float w, float h, float alpha)
{
	snes_draw d; float S[6];
	if (!im) return;
	d.pix = snes_img_pixels(pk, im); d.rgb565 = (im->flags & SNES_IMG_RGB565) ? 1 : 0;
	d.img_w = im->w; d.img_h = im->h; d.sx = 0; d.sy = 0; d.sw = im->w; d.sh = im->h;
	d.dw = w; d.dh = h; d.px = w / 2; d.py = h / 2;
	d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
	d.tr = d.tg = d.tb = 1; d.ta = alpha;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = cx; S[5] = cy;
	blit(t, S, &d);
}
void snes_blit_spr(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
		   float cx, float cy, float scale, float alpha)
{
	snes_draw d; float S[6];
	if (!sp) return;
	d.pix = snes_img_pixels(pk, &pk->img[sp->img]);
	d.rgb565 = (pk->img[sp->img].flags & SNES_IMG_RGB565) ? 1 : 0;
	d.img_w = pk->img[sp->img].w; d.img_h = pk->img[sp->img].h;
	d.sx = sp->sx; d.sy = sp->sy; d.sw = sp->sw; d.sh = sp->sh;
	d.dw = sp->sw * scale; d.dh = sp->sh * scale;
	d.px = sp->sw * scale / 2; d.py = sp->sh * scale / 2;
	d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
	d.tr = d.tg = d.tb = 1; d.ta = alpha;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = cx; S[5] = cy;
	blit(t, S, &d);
}
void snes_fill_quad(snes_target *t, float cx, float cy, float w, float h,
		    float r, float g, float b, float a)
{
	snes_draw d; float S[6];
	d.is_quad = 1; d.dw = w; d.dh = h; d.px = w / 2; d.py = h / 2;
	d.hflip = d.vflip = d.tile = d.additive = 0;
	d.tr = r; d.tg = g; d.tb = b; d.ta = a;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = cx; S[5] = cy;
	blit(t, S, &d);
}

/* Diagnostic: draw solid test quads THROUGH the blitter so their placement can be
 * compared with known-good ayaneo_fill markers - separates a coordinate-mapping
 * bug from an asset-resolution bug. Draws a magenta full-virtual-area border and a
 * green quad at virtual (100,100)-(400,300). */
static void quad(snes_target *t, float x, float y, float w, float h, float r, float g, float b)
{
	float S[6]; snes_draw d;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = x; S[5] = y;
	d.is_quad = 1; d.dw = w; d.dh = h; d.px = 0; d.py = 0;
	d.hflip = d.vflip = d.tile = d.additive = 0;
	d.tr = r; d.tg = g; d.tb = b; d.ta = 1;
	blit(t, S, &d);
}
void snes_render_debug_markers(snes_target *t)
{
	quad(t, 0, 0, SNES_VW, 3, 1, 0, 1);              /* top border */
	quad(t, 0, SNES_VH - 3, SNES_VW, 3, 1, 0, 1);    /* bottom */
	quad(t, 0, 0, 3, SNES_VH, 1, 0, 1);              /* left */
	quad(t, SNES_VW - 3, 0, 3, SNES_VH, 1, 0, 1);    /* right */
	quad(t, 100, 100, 300, 200, 0, 1, 0);            /* green box upper-left */
	quad(t, SNES_VW / 2 - 40, SNES_VH / 2 - 40, 80, 80, 1, 0, 0); /* red centre */
}
