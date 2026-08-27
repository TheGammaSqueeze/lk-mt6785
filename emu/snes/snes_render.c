/*
 * Software renderer for the CLOVER scene graph, reproducing web/src/engine/
 * renderer.js: depth-first z-sorted painter, engine (+Y up) -> screen transform,
 * inverse-mapped affine sprite blit with RGB-multiply tint + straight alpha +
 * additive blend + flip + wallpaper tiling, and BMFont text.
 */
#include "snes_render.h"
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

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
void snes_target_view(snes_target *t, float sx, float sy, float dx, float dy)
{
	/* NB: this is called per view-group DURING a render; it must NOT touch the
	 * band clip (band_y0/band_y1), or a multicore core's band would be reset
	 * mid-frame. The band is zero-initialised at target creation instead. */
	t->vsx = sx; t->vsy = sy; t->vdx = dx; t->vdy = dy;
}

/* Absolute-framebuffer scanline band clip (see snes_render.h). */
void snes_target_band(snes_target *t, int y0, int y1)
{
	t->band_y0 = y0; t->band_y1 = y1;
}

static void blit(snes_target *t, const float M[6], const snes_draw *d)
{
	/* Apply the per-view-group aspect transform to the screen matrix: screen
	 * coords (X,Y) become (vsx*X + vdx, vsy*Y + vdy). At the native no-op
	 * (1,1,0,0) this leaves M unchanged bit-for-bit (x*1==x, x+0==x). */
	float a = M[0] * t->vsx, b = M[1] * t->vsy, c = M[2] * t->vsx, dd = M[3] * t->vsy;
	float e = M[4] * t->vsx + t->vdx, f = M[5] * t->vsy + t->vdy;
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
	/* Low clamp so buffer col/row = offx/offy + X/Y stays >= 0. The OVL card-cache
	 * (cache_layer) is a WIDE, band-height buffer with a margin (offx>0) and a shifted
	 * band (offy<0), so it needs the full extent [-offx .. t->W-offx); every other
	 * target keeps the original [0 .. min(t->W-offx, SNES_VW)) behaviour bit-for-bit. */
	{
		int lox = t->cache_layer ? -t->offx : 0, loy = t->cache_layer ? -t->offy : 0;
		if (x0 < lox) x0 = lox; if (y0 < loy) y0 = loy;
	}
	{
		int xlim = t->W - t->offx, ylim = t->H - t->offy;
		if (!t->cache_layer && xlim > SNES_VW) xlim = SNES_VW;   /* virtual design width */
		if (x1 > xlim) x1 = xlim;
		if (y1 > ylim) y1 = ylim;
	}
	/* Multicore band clip: intersect the destination row range with this core's
	 * absolute scanline band. Rows are written to fb[(offy+Y)*pitch]; convert the
	 * band's absolute panel rows [band_y0, band_y1) into virtual Y and tighten
	 * [y0, y1). band_y1==0 disables (single-core / default). The bands are chosen
	 * cache-line disjoint by the caller, so no two cores ever touch the same 64B
	 * framebuffer line and there is no write-write false sharing. */
	if (t->band_y1 > t->band_y0) {
		int by0 = t->band_y0 - t->offy;   /* band low  in virtual Y */
		int by1 = t->band_y1 - t->offy;   /* band high in virtual Y */
		if (y0 < by0) y0 = by0;
		if (y1 > by1) y1 = by1;
	}
	if (y0 >= y1 || x0 >= x1) return;
	{
	/* Integer tint (0..256) and source stride, hoisted out of the pixel loop. */
	int tr = (int)(d->tr * 256.0f), tg = (int)(d->tg * 256.0f);
	int tb = (int)(d->tb * 256.0f), ta = (int)(d->ta * 256.0f);
	int bpp = d->rgb565 ? 3 : 4;
	/* the common card frame / box art blit is opaque and untinted; skip the four
	 * per-pixel tint multiplies in that case (source colour passes straight). */
	int plain = (tr == 256 && tg == 256 && tb == 256 && ta == 256 && !d->additive);
	int axis = (b == 0.0f && c == 0.0f && a != 0.0f && dd != 0.0f);
	float inv_a = axis ? 1.0f / a : 0, inv_d = axis ? 1.0f / dd : 0;
	float du = axis ? inv_a / d->dw : 0;   /* u increment per X (no per-pixel divide) */
	if (axis && !d->is_quad && !d->tile && !t->cache_layer) {
		/* Fast axis-aligned path (every sprite/texture blit in the menu). v is
		 * constant per row, so iy + the source row pointer hoist out of the pixel
		 * loop; the source x advances by a fixed 16.16 step - no per-pixel float
		 * mul or float->int convert. Keeps the opaque write-through fast path. */
		int sxlo = d->sx << 16, sxhi = (d->sx + d->sw) << 16;
		for (Y = y0; Y < y1; Y++) {
			uint32_t *row = t->fb + (unsigned)(t->offy + Y) * t->pitch + t->offx;
			float fy = (float)Y + 0.5f - f;
			float vv = (fy * inv_d + d->py) / d->dh;
			float sv, u0, su0f, sustepf;
			const uint8_t *srow;
			int iy, su, sustep;
			if (d->vflip) vv = 1.0f - vv;
			if (vv < 0.0f || vv >= 1.0f) continue;
			sv = d->sy + vv * d->sh;
			iy = (int)sv; if (iy < 0) iy = 0; if (iy >= d->img_h) iy = d->img_h - 1;
			srow = d->pix + (unsigned)iy * d->img_w * bpp;
			u0 = (((float)x0 + 0.5f - e) * inv_a + d->px) / d->dw;
			if (!d->hflip) { su0f = d->sx + u0 * d->sw;          sustepf =  du * d->sw; }
			else           { su0f = d->sx + (1.0f - u0) * d->sw; sustepf = -du * d->sw; }
			su = (int)(su0f * 65536.0f); sustep = (int)(sustepf * 65536.0f);
#ifdef __ARM_NEON
			/* NEON: for the opaque untinted 565 upscale (card frame + box art),
			 * process 8 destination pixels at once - gather 8 source texels,
			 * unpack 565->8888 8-wide, store. Edge/transparent blocks fall back
			 * to scalar. Pixel-identical to the scalar path (qemu-validated). */
			if (plain && d->rgb565 && sustep > 0) {
				const uint16x8_t m3f = vdupq_n_u16(0x3f), m1f = vdupq_n_u16(0x1f);
				const uint32x4_t A = vdupq_n_u32(0xff000000u);
				X = x0;
				while (X < x1 && su < sxlo) { su += sustep; X++; }
				while (X + 8 <= x1) {
					uint16_t cv[8]; int allop = 1, k, s = su;
					for (k = 0; k < 8; k++) {
						int ix;
						const uint8_t *p;
						if (s >= sxhi) { allop = 0; break; }
						ix = s >> 16; p = srow + (unsigned)ix * 3;
						if (p[2] != 255) { allop = 0; break; }
						cv[k] = (uint16_t)(p[0] | (p[1] << 8)); s += sustep;
					}
					if (allop) {
						uint16x8_t v = vld1q_u16(cv);
						uint16x8_t r5 = vshrq_n_u16(v, 11);
						uint16x8_t g6 = vandq_u16(vshrq_n_u16(v, 5), m3f);
						uint16x8_t b5 = vandq_u16(v, m1f);
						uint16x8_t r8 = vorrq_u16(vshlq_n_u16(r5, 3), vshrq_n_u16(r5, 2));
						uint16x8_t g8 = vorrq_u16(vshlq_n_u16(g6, 2), vshrq_n_u16(g6, 4));
						uint16x8_t b8 = vorrq_u16(vshlq_n_u16(b5, 3), vshrq_n_u16(b5, 2));
						uint32x4_t lo = vorrq_u32(vorrq_u32(A, vshll_n_u16(vget_low_u16(r8), 16)),
							vorrq_u32(vshll_n_u16(vget_low_u16(g8), 8), vmovl_u16(vget_low_u16(b8))));
						uint32x4_t hi = vorrq_u32(vorrq_u32(A, vshll_n_u16(vget_high_u16(r8), 16)),
							vorrq_u32(vshll_n_u16(vget_high_u16(g8), 8), vmovl_u16(vget_high_u16(b8))));
						vst1q_u32(&row[X], lo); vst1q_u32(&row[X + 4], hi);
						X += 8; su += 8 * sustep;
					} else {
						for (k = 0; k < 8; k++, X++, su += sustep) {
							int ix, sr, sg, sb, sa;
							const uint8_t *sp;
							if (su < sxlo || su >= sxhi) continue;
							ix = su >> 16; if (ix >= d->img_w) ix = d->img_w - 1;
							sp = srow + (unsigned)ix * 3;
							src_rgba(sp, 1, &sr, &sg, &sb, &sa);
							if (sa == 0) continue;
							if (sa == 255) {
								row[X] = 0xff000000u | ((unsigned)sr << 16) | ((unsigned)sg << 8) | (unsigned)sb;
							} else {
								uint32_t dst = row[X];
								int dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff, ia = 255 - sa;
								dr = (sr * sa + dr * ia + 127) / 255; dg = (sg * sa + dg * ia + 127) / 255; db = (sb * sa + db * ia + 127) / 255;
								if (dr > 255) dr = 255; if (dg > 255) dg = 255; if (db > 255) db = 255;
								row[X] = 0xff000000u | ((unsigned)dr << 16) | ((unsigned)dg << 8) | (unsigned)db;
							}
						}
					}
				}
				for (; X < x1; X++, su += sustep) {
					int ix, sr, sg, sb, sa;
					const uint8_t *sp;
					if (su < sxlo || su >= sxhi) continue;
					ix = su >> 16; if (ix >= d->img_w) ix = d->img_w - 1;
					sp = srow + (unsigned)ix * 3;
					src_rgba(sp, 1, &sr, &sg, &sb, &sa);
					if (sa == 0) continue;
					if (sa == 255) {
						row[X] = 0xff000000u | ((unsigned)sr << 16) | ((unsigned)sg << 8) | (unsigned)sb;
					} else {
						uint32_t dst = row[X];
						int dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff, ia = 255 - sa;
						dr = (sr * sa + dr * ia + 127) / 255; dg = (sg * sa + dg * ia + 127) / 255; db = (sb * sa + db * ia + 127) / 255;
						if (dr > 255) dr = 255; if (dg > 255) dg = 255; if (db > 255) db = 255;
						row[X] = 0xff000000u | ((unsigned)dr << 16) | ((unsigned)dg << 8) | (unsigned)db;
					}
				}
				continue;
			}
#endif
			for (X = x0; X < x1; X++, su += sustep) {
				int sr, sg, sb, sa, af, ix;
				const uint8_t *sp;
				if (su < sxlo || su >= sxhi) continue;
				ix = su >> 16; if (ix >= d->img_w) ix = d->img_w - 1;
				sp = srow + (unsigned)ix * bpp;
				src_rgba(sp, d->rgb565, &sr, &sg, &sb, &sa);
				if (sa == 0) continue;
				if (plain) {
					if (sa == 255) {   /* opaque + untinted: straight store */
						row[X] = 0xff000000u | ((unsigned)sr << 16)
						       | ((unsigned)sg << 8) | (unsigned)sb;
						continue;
					}
					af = sa;
				} else {
					sr = (sr * tr) >> 8; sg = (sg * tg) >> 8; sb = (sb * tb) >> 8;
					af = (sa * ta) >> 8; if (af <= 0) continue; if (af > 255) af = 255;
					if (af == 255 && !d->additive) {
						row[X] = 0xff000000u | ((unsigned)sr << 16)
						       | ((unsigned)sg << 8) | (unsigned)sb;
						continue;
					}
				}
				{
					uint32_t dst = row[X];
					int dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
					if (d->additive) {
						dr += (sr * af) >> 8; dg += (sg * af) >> 8; db += (sb * af) >> 8;
					} else {
						int ia = 255 - af;
						dr = (sr * af + dr * ia + 127) / 255;
						dg = (sg * af + dg * ia + 127) / 255;
						db = (sb * af + db * ia + 127) / 255;
					}
					if (dr > 255) dr = 255; if (dg > 255) dg = 255; if (db > 255) db = 255;
					row[X] = 0xff000000u | ((unsigned)dr << 16) | ((unsigned)dg << 8) | (unsigned)db;
				}
			}
		}
	} else
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
			/* cache-layer mode: premultiplied source-over into an RGBA layer
			 * (preserves alpha so the card strip composites correctly over the
			 * live wallpaper/lower OVL layers later). Used to build the card cache;
			 * the result is un-premultiplied to straight alpha before it is handed
			 * to the OVL (which blends with coverage alpha). */
			if (t->cache_layer) {
				uint32_t c = row[X];
				int da = (c >> 24) & 0xff, dpr = (c >> 16) & 0xff, dpg = (c >> 8) & 0xff, dpb = c & 0xff;
				int ia = 255 - af;
				int opr = (sr * af + 127) / 255 + (dpr * ia + 127) / 255;
				int opg = (sg * af + 127) / 255 + (dpg * ia + 127) / 255;
				int opb = (sb * af + 127) / 255 + (dpb * ia + 127) / 255;
				int oa  = af + (da * ia + 127) / 255;
				if (opr > 255) opr = 255; if (opg > 255) opg = 255; if (opb > 255) opb = 255; if (oa > 255) oa = 255;
				row[X] = ((unsigned)oa << 24) | ((unsigned)opr << 16) | ((unsigned)opg << 8) | (unsigned)opb;
				continue;
			}
			/* opaque fast path: fully-covered, non-additive pixels (the bulk of
			 * the card frames + box art) need no destination read or blend -
			 * just write. Avoids a framebuffer read + the blend math per pixel. */
			if (af == 255 && !d->additive) {
				row[X] = 0xff000000u | ((unsigned)sr << 16)
				       | ((unsigned)sg << 8) | (unsigned)sb;
				continue;
			}
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
	float lcol[4];
	if (!fe || !text || !text[0]) return;
	/* fold the label component's own tint into the node colour chain (web
	 * mulColor(nodeCol, comp.color)); black button labels, gray captions, etc. */
	lcol[0] = col[0] * (lb->color[0] / 255.0f);
	lcol[1] = col[1] * (lb->color[1] / 255.0f);
	lcol[2] = col[2] * (lb->color[2] / 255.0f);
	lcol[3] = col[3] * (lb->color[3] / 255.0f);
	col = lcol;
	page = &pk->img[fe->page];
	pgpix = snes_img_pixels(pk, page); pgw = page->w; pgh = page->h;
	rgb565 = (page->flags & SNES_IMG_RGB565) ? 1 : 0;
	glyphs = (const snes_glyph *)(pk->base + fe->glyphs);
	screen_matrix(m, S);
	scale = S[0] < 0 ? -S[0] : S[0];         /* uniform-ish scale for text */
	if (scale < 0.01f) scale = 1.0f;
	/* Multi-line aware: split on '\n', centre each line horizontally and the whole
	 * block vertically. Single-line labels are byte-identical (nlines=1 -> the old
	 * th/2 centring). Ports the web LabelComponent's newline handling; without it a
	 * multi-line string (e.g. the reset dialog body) collapses onto one line. */
	{
		float line_h = fe->line_height * scale;
		int nlines = 1, li;
		float peny0 = S[5], block_h;
		const char *ls;
		(void)wpx;
		for (p = text; *p; p++) if (*p == '\n') nlines++;
		block_h = (float)nlines * line_h;
		if (lb->v_anchor == ANCHOR_MIDDLE || lb->v_anchor == ANCHOR_CENTER)
			peny0 -= block_h / 2.0f;
		else if (lb->v_anchor == ANCHOR_BOTTOM) peny0 -= block_h;
		ls = text;
		for (li = 0; li < nlines; li++) {
			const char *le = ls, *q;
			float lw = 0, peny = peny0 + (float)li * line_h;
			while (*le && *le != '\n') le++;      /* end of this line */
			for (q = ls; q < le; ) {              /* measure line width for the anchor */
				const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&q));
				lw += g ? g->xadv : 0;
			}
			penx = S[4];
			if (lb->h_anchor == ANCHOR_CENTER) penx -= lw * scale / 2.0f;
			else if (lb->h_anchor == ANCHOR_RIGHT) penx -= lw * scale;
			for (q = ls; q < le; ) {
				const snes_glyph *g = glyph_find(fe, glyphs, utf8_next(&q));
				if (!g) continue;   /* missing glyphs drop to zero width (web parity) */
				if (g->w > 0) {
					snes_draw d;
					float gm[6];
					d.pix = pgpix; d.rgb565 = rgb565; d.img_w = pgw; d.img_h = pgh;
					d.sx = g->x; d.sy = g->y; d.sw = g->w; d.sh = g->h;
					d.dw = g->w * scale; d.dh = g->h * scale; d.px = 0; d.py = 0;
					d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
					d.tr = col[0]; d.tg = col[1]; d.tb = col[2]; d.ta = col[3];
					gm[0] = 1; gm[1] = 0; gm[2] = 0; gm[3] = 1;
					gm[4] = penx + g->xo * scale; gm[5] = peny + g->yo * scale;
					blit(t, gm, &d);
				}
				penx += g->xadv * scale;
			}
			ls = (*le == '\n') ? le + 1 : le;
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

/* item for per-node z ordering. Kept in a bump scratch (not the C stack) so a
 * deep tree does not overflow the thread stack: each collect frame reserves its
 * items on entry and rewinds on exit; children allocate beyond. */
typedef struct { int z, layer, g, i; const snes_comp *comp; snes_rnode *child; } zitem;
#define SNES_MAXZ 4096

/* Per-core render scratch. Single-core (release + default debug) uses exactly
 * one instance, g_ctx0, so the g_dr/g_zs arrays and g_ndr/g_zt bookkeeping are
 * byte-identical to the old file-scope globals - no behaviour change. Under the
 * AYANEO_BIGCORE_EXPT multicore split each core owns its OWN context (its own
 * z-sort + draw list), so collect()/sort_and_paint() never share mutable state
 * across cores; the only shared object is the framebuffer, into which each core
 * writes a disjoint scanline band (see the band clip in blit()). */
struct snes_render_ctx {
	snes_dr dr[SNES_MAXDR];
	int     ndr;
	zitem   zs[SNES_MAXZ];
	int     zt;
};

/* Per-core render contexts. Release/host: exactly one (index 0), byte-identical
 * to the old single g_ctx0. The AYANEO_BIGCORE_EXPT multicore build adds a second
 * so the boot core and the worker core each collect+sort into private scratch;
 * bc_render_ctx() picks the caller's by MPIDR Aff1 (0 = boot core, else worker),
 * so snes_menu_render can be entered by both cores concurrently without sharing
 * mutable state. The band clip keeps their framebuffer writes disjoint. */
#ifdef AYANEO_BIGCORE_EXPT
#define BC_NCTX 2
#else
#define BC_NCTX 1
#endif
static snes_render_ctx g_ctx[BC_NCTX];

static snes_render_ctx *bc_render_ctx(void)
{
#if defined(__arm__) && defined(AYANEO_BIGCORE_EXPT)
	unsigned m;
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));   /* MPIDR */
	return &g_ctx[((m >> 8) & 0xffu) ? 1 : 0];                 /* Aff1: 0=boot */
#else
	return &g_ctx[0];   /* single-core release + host: one context */
#endif
}

static int is_drawable(int type)
{
	return type == COMP_SPRITE || type == COMP_TEXTURE ||
	       type == COMP_ANIMATED_SPRITE || type == COMP_LABEL;
}

static void collect(snes_render_ctx *ctx, snes_scene *s, snes_rnode *n,
		    const float pm[6], const float pcol[4], int base_layer)
{
	float m[6], col[4];
	int base = ctx->zt;
	zitem *items = &ctx->zs[base];
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
	ctx->zt = base + ni;   /* reserve; children collect() allocate beyond */
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
			if (ctx->ndr < SNES_MAXDR) {
				snes_dr *dr = &ctx->dr[ctx->ndr++];
				dr->comp = items[k].comp; dr->node = n;
				for (kk = 0; kk < 6; kk++) dr->m[kk] = m[kk];
				dr->col[0] = col[0]; dr->col[1] = col[1];
				dr->col[2] = col[2]; dr->col[3] = col[3];
				dr->layer = base_layer; dr->seq = ctx->ndr;
			}
		} else {
			collect(ctx, s, items[k].child, m, col, base_layer);
		}
	}
	ctx->zt = base;   /* rewind this frame's reservation */
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

/* Stable-sort a context's draw list by (layer, seq), then paint every drawable.
 * The z-sort is identical on every core (the collect result is deterministic and
 * core-independent); only the framebuffer WRITE differs, gated by t->band_y*. So
 * splitting is "same list, disjoint output rows": no core reorders differently. */
static void sort_and_paint_ctx(snes_render_ctx *ctx, snes_target *t, snes_scene *s)
{
	int i, k;
	for (i = 1; i < ctx->ndr; i++) {
		snes_dr key = ctx->dr[i];
		k = i - 1;
		while (k >= 0 && ((ctx->dr[k].layer != key.layer) ? ctx->dr[k].layer > key.layer
								  : ctx->dr[k].seq > key.seq)) {
			ctx->dr[k + 1] = ctx->dr[k]; k--;
		}
		ctx->dr[k + 1] = key;
	}
	for (i = 0; i < ctx->ndr; i++)
		paint(t, s, &ctx->dr[i]);
}

/* Collect (+ z-sort) a scene into an explicit context, then paint into the
 * target honouring the target's band clip. This is the multicore work unit: two
 * cores call it with the SAME scene and DIFFERENT (ctx, band) so each collects
 * its own list (private scratch) and paints only its own rows. */
void snes_render_scene_ctx(snes_render_ctx *ctx, snes_target *t, snes_scene *s)
{
	static const float I[6] = {1,0,0,0,1,0};
	static const float W[4] = {1,1,1,1};
	ctx->ndr = 0; ctx->zt = 0;
	if (!s->root) return;
	collect(ctx, s, s->root, I, W, 0);
	sort_and_paint_ctx(ctx, t, s);
}

void snes_render_scene(snes_target *t, snes_scene *s)
{
	snes_render_scene_ctx(bc_render_ctx(), t, s);
}

void snes_render_node_ctx(snes_render_ctx *ctx, snes_target *t, snes_scene *s, snes_rnode *n)
{
	static const float W[4] = {1,1,1,1};
	float pm[6] = {1,0,0,0,1,0};
	ctx->ndr = 0; ctx->zt = 0;
	if (!n) return;
	if (n->parent) snes_node_world(n->parent, pm);
	collect(ctx, s, n, pm, W, 0);
	sort_and_paint_ctx(ctx, t, s);
}

/* Render the subtree rooted at n at its authored world position (n's own
 * transform is applied on top of its parent's world matrix). */
void snes_render_node(snes_target *t, snes_scene *s, snes_rnode *n)
{
	snes_render_node_ctx(bc_render_ctx(), t, s, n);
}

int snes_render_count(void) { return g_ctx[0].ndr; }

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
void snes_blit_tex_tint(snes_target *t, const snes_pack *pk, const snes_img_entry *im,
			float cx, float cy, float w, float h, float alpha,
			float tr, float tg, float tb)
{
	snes_draw d; float S[6];
	if (!im) return;
	d.pix = snes_img_pixels(pk, im); d.rgb565 = (im->flags & SNES_IMG_RGB565) ? 1 : 0;
	d.img_w = im->w; d.img_h = im->h; d.sx = 0; d.sy = 0; d.sw = im->w; d.sh = im->h;
	d.dw = w; d.dh = h; d.px = w / 2; d.py = h / 2;
	d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
	d.tr = tr; d.tg = tg; d.tb = tb; d.ta = alpha;
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
void snes_blit_spr_tint(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			float cx, float cy, float scale, float alpha,
			float tr, float tg, float tb)
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
	d.tr = tr; d.tg = tg; d.tb = tb; d.ta = alpha;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = cx; S[5] = cy;
	blit(t, S, &d);
}
void snes_blit_spr_wh_tint(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			   float cx, float cy, float dw, float dh, float alpha,
			   float tr, float tg, float tb)
{
	snes_draw d; float S[6];
	if (!sp) return;
	d.pix = snes_img_pixels(pk, &pk->img[sp->img]);
	d.rgb565 = (pk->img[sp->img].flags & SNES_IMG_RGB565) ? 1 : 0;
	d.img_w = pk->img[sp->img].w; d.img_h = pk->img[sp->img].h;
	d.sx = sp->sx; d.sy = sp->sy; d.sw = sp->sw; d.sh = sp->sh;
	d.dw = dw; d.dh = dh;
	d.px = dw / 2.0f; d.py = dh / 2.0f;
	d.hflip = d.vflip = d.tile = d.additive = d.is_quad = 0;
	d.tr = tr; d.tg = tg; d.tb = tb; d.ta = alpha;
	S[0] = 1; S[1] = 0; S[2] = 0; S[3] = 1; S[4] = cx; S[5] = cy;
	blit(t, S, &d);
}
void snes_blit_spr_tint_flip(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			     float cx, float cy, float scale, float alpha,
			     float tr, float tg, float tb, int hflip, int vflip)
{
	snes_draw d; float S[6];
	if (!sp) return;
	d.pix = snes_img_pixels(pk, &pk->img[sp->img]);
	d.rgb565 = (pk->img[sp->img].flags & SNES_IMG_RGB565) ? 1 : 0;
	d.img_w = pk->img[sp->img].w; d.img_h = pk->img[sp->img].h;
	d.sx = sp->sx; d.sy = sp->sy; d.sw = sp->sw; d.sh = sp->sh;
	d.dw = sp->sw * scale; d.dh = sp->sh * scale;
	d.px = sp->sw * scale / 2; d.py = sp->sh * scale / 2;
	d.hflip = hflip ? 1 : 0; d.vflip = vflip ? 1 : 0;
	d.tile = d.additive = d.is_quad = 0;
	d.tr = tr; d.tg = tg; d.tb = tb; d.ta = alpha;
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
