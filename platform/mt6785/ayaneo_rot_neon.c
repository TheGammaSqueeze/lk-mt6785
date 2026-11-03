/*
 * NEON canvas passes for the Konkr Pocket Advance (portrait panel mounted landscape):
 * the 4:3 box downscale of the virtual 1280x853 menu canvas to the 960x640 logical screen,
 * and the quarter turn of a 32-bit image into the 640x960 frame buffer. Both run once per
 * presented menu/dialog frame; the overlay turn runs on repaint only. Compiled with
 * -mfpu=neon (see platform/mt6785/rules.mk), everything else in LK stays soft-float.
 */
#include <arm_neon.h>

/* Exact 4:3 box downscale: every 4 source columns -> 3 destination columns with weights
 * (3,1)/4, (2,2)/4, (1,3)/4, the same vertically. The partial last row group clamps to the
 * last source row. Processes 16 source pixels (four groups) per inner step: vertical mix in
 * 8-bit lanes widened to 16, then the horizontal mix from the four per-group pixels. */
void ayaneo_scale43_neon(const unsigned int *src, unsigned sw, unsigned sh, unsigned spitch,
			 unsigned int *dst, unsigned dw, unsigned dh, unsigned dpitch)
{
	unsigned y;
	for (y = 0; y < dh; y++) {
		unsigned grp = y / 3u, k = y % 3u, r0, r1;
		uint8x8_t wa8, wb8;
		if (k == 0)      { r0 = grp * 4u;      r1 = r0 + 1u; wa8 = vdup_n_u8(3); wb8 = vdup_n_u8(1); }
		else if (k == 1) { r0 = grp * 4u + 1u; r1 = r0 + 1u; wa8 = vdup_n_u8(2); wb8 = vdup_n_u8(2); }
		else             { r0 = grp * 4u + 2u; r1 = r0 + 1u; wa8 = vdup_n_u8(1); wb8 = vdup_n_u8(3); }
		if (r0 >= sh) r0 = sh - 1u;
		if (r1 >= sh) r1 = sh - 1u;
		{
			const uint8_t *a = (const uint8_t *)(src + r0 * spitch);
			const uint8_t *b = (const uint8_t *)(src + r1 * spitch);
			uint8_t *o = (uint8_t *)(dst + y * dpitch);
			unsigned x = 0, g = 0;
			/* horizontal weights per byte lane for one group of 4 pixels (16 bytes):
			 * out0 = 3*c0 + 1*c1, out1 = 2*c1 + 2*c2, out2 = 1*c2 + 3*c3 */
			const uint8x8_t h3 = vdup_n_u8(3), h1 = vdup_n_u8(1), h2 = vdup_n_u8(2);
			for (; x + 3u <= dw && g + 4u <= sw; x += 3u, g += 4u) {
				uint8x16_t va = vld1q_u8(a + g * 4u), vb = vld1q_u8(b + g * 4u);
				/* vertical mix -> c (16 bytes = pixels c0 c1 c2 c3) */
				uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(va), wa8), vget_low_u8(vb), wb8);
				uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(va), wa8), vget_high_u8(vb), wb8);
				uint8x8_t c01 = vshrn_n_u16(lo, 2);   /* c0 c1 */
				uint8x8_t c23 = vshrn_n_u16(hi, 2);   /* c2 c3 */
				/* horizontal: build (c0,c1) (c1,c2) (c2,c3) pairs as 8-byte vectors */
				uint8x8_t c0 = c01, c1 = vext_u8(c01, c23, 4), c2 = c23;   /* low halves hold c0 / c1 / c2 */
				uint8x8_t c3 = vext_u8(c23, c23, 4);                       /* low half holds c3 */
				uint16x8_t o0 = vmlal_u8(vmull_u8(c0, h3), c1, h1);        /* lanes 0..3 valid */
				uint16x8_t o1 = vmlal_u8(vmull_u8(c1, h2), c2, h2);
				uint16x8_t o2 = vmlal_u8(vmull_u8(c2, h1), c3, h3);
				uint8x8_t p0 = vshrn_n_u16(o0, 2), p1 = vshrn_n_u16(o1, 2), p2 = vshrn_n_u16(o2, 2);
				vst1_lane_u32((uint32_t *)(o + (x + 0u) * 4u), vreinterpret_u32_u8(p0), 0);
				vst1_lane_u32((uint32_t *)(o + (x + 1u) * 4u), vreinterpret_u32_u8(p1), 0);
				vst1_lane_u32((uint32_t *)(o + (x + 2u) * 4u), vreinterpret_u32_u8(p2), 0);
			}
			for (; x < dw; x++) ((unsigned int *)(dst + y * dpitch))[x] = 0xFF000000u;
		}
	}
}

/* Quarter turn of a w x h 32-bit image into dst (h x w). Works on 4x4 blocks: four source
 * rows loaded as 4-lane vectors, transposed, each result vector is one destination row
 * segment of four contiguous pixels. Clockwise: dst[x][h-1-y] = src[y][x], so rows are loaded
 * in reverse (y+3 .. y) to land contiguous in dst. Counter clockwise: dst[w-1-x][y] = src[y][x].
 * Edges not multiple of 4 are finished with scalar stores. Alpha is forced opaque on the way
 * (the frame buffer layer is scanned with alpha enabled). */
void ayaneo_rot8888_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			 unsigned int *dst, unsigned dpitch, int ccw)
{
	unsigned x, y;
	const uint32x4_t opaque = vdupq_n_u32(0xFF000000u);
	for (y = 0; y + 4u <= h; y += 4u) {
		for (x = 0; x + 4u <= w; x += 4u) {
			uint32x4_t r0, r1, r2, r3;
			if (!ccw) {   /* reversed row order so the transposed vectors are contiguous */
				r0 = vld1q_u32(src + (y + 3u) * spitch + x);
				r1 = vld1q_u32(src + (y + 2u) * spitch + x);
				r2 = vld1q_u32(src + (y + 1u) * spitch + x);
				r3 = vld1q_u32(src + (y + 0u) * spitch + x);
			} else {
				r0 = vld1q_u32(src + (y + 0u) * spitch + x);
				r1 = vld1q_u32(src + (y + 1u) * spitch + x);
				r2 = vld1q_u32(src + (y + 2u) * spitch + x);
				r3 = vld1q_u32(src + (y + 3u) * spitch + x);
			}
			{
				/* 4x4 transpose */
				uint32x4x2_t t01 = vtrnq_u32(r0, r1), t23 = vtrnq_u32(r2, r3);
				uint32x4_t c0 = vcombine_u32(vget_low_u32(t01.val[0]), vget_low_u32(t23.val[0]));
				uint32x4_t c1 = vcombine_u32(vget_low_u32(t01.val[1]), vget_low_u32(t23.val[1]));
				uint32x4_t c2 = vcombine_u32(vget_high_u32(t01.val[0]), vget_high_u32(t23.val[0]));
				uint32x4_t c3 = vcombine_u32(vget_high_u32(t01.val[1]), vget_high_u32(t23.val[1]));
				c0 = vorrq_u32(c0, opaque); c1 = vorrq_u32(c1, opaque);
				c2 = vorrq_u32(c2, opaque); c3 = vorrq_u32(c3, opaque);
				if (!ccw) {
					/* column x of the block -> dst row x, columns h-1-(y+3) .. h-1-y */
					unsigned dcol = h - 4u - y;
					vst1q_u32(dst + (x + 0u) * dpitch + dcol, c0);
					vst1q_u32(dst + (x + 1u) * dpitch + dcol, c1);
					vst1q_u32(dst + (x + 2u) * dpitch + dcol, c2);
					vst1q_u32(dst + (x + 3u) * dpitch + dcol, c3);
				} else {
					/* column x -> dst row w-1-x, columns y .. y+3 */
					vst1q_u32(dst + (w - 1u - (x + 0u)) * dpitch + y, c0);
					vst1q_u32(dst + (w - 1u - (x + 1u)) * dpitch + y, c1);
					vst1q_u32(dst + (w - 1u - (x + 2u)) * dpitch + y, c2);
					vst1q_u32(dst + (w - 1u - (x + 3u)) * dpitch + y, c3);
				}
			}
		}
		for (; x < w; x++) {   /* leftover columns of this row band */
			unsigned yy;
			for (yy = y; yy < y + 4u; yy++) {
				unsigned int p = src[yy * spitch + x] | 0xFF000000u;
				if (!ccw) dst[x * dpitch + (h - 1u - yy)] = p;
				else      dst[(w - 1u - x) * dpitch + yy] = p;
			}
		}
	}
	for (; y < h; y++) {   /* leftover rows */
		for (x = 0; x < w; x++) {
			unsigned int p = src[y * spitch + x] | 0xFF000000u;
			if (!ccw) dst[x * dpitch + (h - 1u - y)] = p;
			else      dst[(w - 1u - x) * dpitch + y] = p;
		}
	}
}

/* Same quarter turn but the pixels keep their alpha: for the in-game overlay layer, which the
 * hardware blends over the game (the plain turn forces every pixel opaque, which painted the
 * overlay buffer's transparent area black). */
void ayaneo_rot8888_alpha_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			 unsigned int *dst, unsigned dpitch, int ccw)
{
	unsigned x, y;
	for (y = 0; y + 4u <= h; y += 4u) {
		for (x = 0; x + 4u <= w; x += 4u) {
			uint32x4_t r0, r1, r2, r3;
			if (!ccw) {   /* reversed row order so the transposed vectors are contiguous */
				r0 = vld1q_u32(src + (y + 3u) * spitch + x);
				r1 = vld1q_u32(src + (y + 2u) * spitch + x);
				r2 = vld1q_u32(src + (y + 1u) * spitch + x);
				r3 = vld1q_u32(src + (y + 0u) * spitch + x);
			} else {
				r0 = vld1q_u32(src + (y + 0u) * spitch + x);
				r1 = vld1q_u32(src + (y + 1u) * spitch + x);
				r2 = vld1q_u32(src + (y + 2u) * spitch + x);
				r3 = vld1q_u32(src + (y + 3u) * spitch + x);
			}
			{
				/* 4x4 transpose */
				uint32x4x2_t t01 = vtrnq_u32(r0, r1), t23 = vtrnq_u32(r2, r3);
				uint32x4_t c0 = vcombine_u32(vget_low_u32(t01.val[0]), vget_low_u32(t23.val[0]));
				uint32x4_t c1 = vcombine_u32(vget_low_u32(t01.val[1]), vget_low_u32(t23.val[1]));
				uint32x4_t c2 = vcombine_u32(vget_high_u32(t01.val[0]), vget_high_u32(t23.val[0]));
				uint32x4_t c3 = vcombine_u32(vget_high_u32(t01.val[1]), vget_high_u32(t23.val[1]));
				if (!ccw) {
					/* column x of the block -> dst row x, columns h-1-(y+3) .. h-1-y */
					unsigned dcol = h - 4u - y;
					vst1q_u32(dst + (x + 0u) * dpitch + dcol, c0);
					vst1q_u32(dst + (x + 1u) * dpitch + dcol, c1);
					vst1q_u32(dst + (x + 2u) * dpitch + dcol, c2);
					vst1q_u32(dst + (x + 3u) * dpitch + dcol, c3);
				} else {
					/* column x -> dst row w-1-x, columns y .. y+3 */
					vst1q_u32(dst + (w - 1u - (x + 0u)) * dpitch + y, c0);
					vst1q_u32(dst + (w - 1u - (x + 1u)) * dpitch + y, c1);
					vst1q_u32(dst + (w - 1u - (x + 2u)) * dpitch + y, c2);
					vst1q_u32(dst + (w - 1u - (x + 3u)) * dpitch + y, c3);
				}
			}
		}
		for (; x < w; x++) {   /* leftover columns of this row band */
			unsigned yy;
			for (yy = y; yy < y + 4u; yy++) {
				unsigned int p = src[yy * spitch + x];
				if (!ccw) dst[x * dpitch + (h - 1u - yy)] = p;
				else      dst[(w - 1u - x) * dpitch + yy] = p;
			}
		}
	}
	for (; y < h; y++) {   /* leftover rows */
		for (x = 0; x < w; x++) {
			unsigned int p = src[y * spitch + x];
			if (!ccw) dst[x * dpitch + (h - 1u - y)] = p;
			else      dst[(w - 1u - x) * dpitch + y] = p;
		}
	}
}

/* Plain quarter turn in 16 row bands: a band of 16 source rows becomes 16 contiguous pixels (one
 * 64 byte line) of every destination row, so the frame buffer is written a whole cache line at a
 * time instead of the 16 byte scatters of ayaneo_rot8888_neon (which cost 6.5 ms for 960x640 on
 * the Konkr; the writes dominate). Same orientation rules as ayaneo_rot8888_neon. */
void ayaneo_rot_turn_band_neon(const unsigned int *src, unsigned w, unsigned h, unsigned spitch,
			       unsigned int *dst, unsigned dpitch, int ccw)
{
	static unsigned int band[16][1280] __attribute__((aligned(64)));
	const uint32x4_t opaque = vdupq_n_u32(0xFF000000u);
	unsigned y0, x;
	if (w > 1280u) return;
	for (y0 = 0; y0 + 16u <= h; y0 += 16u) {
		unsigned r;
		/* stream the band in with sequential row reads (prefetch friendly) so the transposed
		 * loads below hit the cache instead of walking 16 rows of DRAM at once */
		for (r = 0; r < 16u; r++) {
			const unsigned int *s = src + (y0 + r) * spitch;
			unsigned int *d = band[r];
			unsigned i;
			for (i = 0; i + 16u <= w; i += 16u) {
				vst1q_u32(d + i, vld1q_u32(s + i));           vst1q_u32(d + i + 4u, vld1q_u32(s + i + 4u));
				vst1q_u32(d + i + 8u, vld1q_u32(s + i + 8u)); vst1q_u32(d + i + 12u, vld1q_u32(s + i + 12u));
			}
			for (; i < w; i++) d[i] = s[i];
		}
		for (x = 0; x + 4u <= w; x += 4u) {
			unsigned q;
			for (q = 0; q < 16u; q += 4u) {
				uint32x4_t r0, r1, r2, r3;
				unsigned dcol;
				if (!ccw) {
					/* dst row x holds source rows in descending order: block q covers band rows
					 * 15-q-3 .. 15-q -> dst columns h-16-y0+q .. +3, loaded reversed */
					unsigned top = 15u - q;
					r0 = vld1q_u32(&band[top][x]);      r1 = vld1q_u32(&band[top - 1u][x]);
					r2 = vld1q_u32(&band[top - 2u][x]); r3 = vld1q_u32(&band[top - 3u][x]);
					dcol = h - 16u - y0 + q;
				} else {
					r0 = vld1q_u32(&band[q][x]);      r1 = vld1q_u32(&band[q + 1u][x]);
					r2 = vld1q_u32(&band[q + 2u][x]); r3 = vld1q_u32(&band[q + 3u][x]);
					dcol = y0 + q;
				}
				{
					uint32x4x2_t t01 = vtrnq_u32(r0, r1), t23 = vtrnq_u32(r2, r3);
					uint32x4_t c0 = vorrq_u32(vcombine_u32(vget_low_u32(t01.val[0]), vget_low_u32(t23.val[0])), opaque);
					uint32x4_t c1 = vorrq_u32(vcombine_u32(vget_low_u32(t01.val[1]), vget_low_u32(t23.val[1])), opaque);
					uint32x4_t c2 = vorrq_u32(vcombine_u32(vget_high_u32(t01.val[0]), vget_high_u32(t23.val[0])), opaque);
					uint32x4_t c3 = vorrq_u32(vcombine_u32(vget_high_u32(t01.val[1]), vget_high_u32(t23.val[1])), opaque);
					if (!ccw) {
						vst1q_u32(dst + (x + 0u) * dpitch + dcol, c0);
						vst1q_u32(dst + (x + 1u) * dpitch + dcol, c1);
						vst1q_u32(dst + (x + 2u) * dpitch + dcol, c2);
						vst1q_u32(dst + (x + 3u) * dpitch + dcol, c3);
					} else {
						vst1q_u32(dst + (w - 1u - (x + 0u)) * dpitch + dcol, c0);
						vst1q_u32(dst + (w - 1u - (x + 1u)) * dpitch + dcol, c1);
						vst1q_u32(dst + (w - 1u - (x + 2u)) * dpitch + dcol, c2);
						vst1q_u32(dst + (w - 1u - (x + 3u)) * dpitch + dcol, c3);
					}
				}
			}
		}
		for (; x < w; x++) {   /* leftover columns of this band */
			unsigned yy;
			for (yy = y0; yy < y0 + 16u; yy++) {
				unsigned int p = src[yy * spitch + x] | 0xFF000000u;
				if (!ccw) dst[x * dpitch + (h - 1u - yy)] = p;
				else      dst[(w - 1u - x) * dpitch + yy] = p;
			}
		}
	}
	for (; y0 < h; y0++) {   /* leftover rows (none for 640) */
		for (x = 0; x < w; x++) {
			unsigned int p = src[y0 * spitch + x] | 0xFF000000u;
			if (!ccw) dst[x * dpitch + (h - 1u - y0)] = p;
			else      dst[(w - 1u - x) * dpitch + y0] = p;
		}
	}
}

/* Fused 4:3 box downscale + quarter turn: canvas (sw x sh, e.g. 1280x853) -> fb (dh x dw, e.g.
 * 640x960) in ONE pass. A band of BAND downscaled rows is produced into line buffers (same
 * arithmetic as ayaneo_scale43_neon), then written to the frame buffer transposed: for each
 * logical column x the band becomes BAND contiguous pixels of one fb row, i.e. whole cache lines
 * per fb row per band instead of 16 byte scatters. dw/dh are the LOGICAL (downscaled) dims; the
 * fb is dh wide and dw tall. */
#define BAND 16u
void ayaneo_canvas_present_neon(const unsigned int *src, unsigned sw, unsigned sh, unsigned spitch,
				unsigned int *fb, unsigned fbpitch, unsigned dw, unsigned dh, int ccw)
{
	static unsigned int line[BAND][1280] __attribute__((aligned(16)));
	const uint32x4_t opaque = vdupq_n_u32(0xFF000000u);
	const uint8x8_t h3 = vdup_n_u8(3), h1 = vdup_n_u8(1), h2 = vdup_n_u8(2);
	unsigned y0, x;
	if (dw > 1280u) return;
	for (y0 = 0; y0 < dh; y0 += BAND) {
		unsigned yy, nrow = (y0 + BAND <= dh) ? BAND : dh - y0;
		for (yy = 0; yy < nrow; yy++) {
			unsigned y = y0 + yy, grp = y / 3u, k = y % 3u, r0, r1;
			uint8x8_t wa8, wb8;
			if (k == 0)      { r0 = grp * 4u;      r1 = r0 + 1u; wa8 = vdup_n_u8(3); wb8 = vdup_n_u8(1); }
			else if (k == 1) { r0 = grp * 4u + 1u; r1 = r0 + 1u; wa8 = vdup_n_u8(2); wb8 = vdup_n_u8(2); }
			else             { r0 = grp * 4u + 2u; r1 = r0 + 1u; wa8 = vdup_n_u8(1); wb8 = vdup_n_u8(3); }
			if (r0 >= sh) r0 = sh - 1u;
			if (r1 >= sh) r1 = sh - 1u;
			{
				const uint8_t *a = (const uint8_t *)(src + r0 * spitch);
				const uint8_t *b = (const uint8_t *)(src + r1 * spitch);
				uint8_t *o = (uint8_t *)line[yy];
				unsigned g = 0;
				for (x = 0; x + 3u <= dw && g + 4u <= sw; x += 3u, g += 4u) {
					uint8x16_t va = vld1q_u8(a + g * 4u), vb = vld1q_u8(b + g * 4u);
					uint16x8_t lo = vmlal_u8(vmull_u8(vget_low_u8(va), wa8), vget_low_u8(vb), wb8);
					uint16x8_t hi = vmlal_u8(vmull_u8(vget_high_u8(va), wa8), vget_high_u8(vb), wb8);
					uint8x8_t c01 = vshrn_n_u16(lo, 2), c23 = vshrn_n_u16(hi, 2);
					uint8x8_t c0 = c01, c1 = vext_u8(c01, c23, 4), c2 = c23, c3 = vext_u8(c23, c23, 4);
					uint16x8_t o0 = vmlal_u8(vmull_u8(c0, h3), c1, h1);
					uint16x8_t o1 = vmlal_u8(vmull_u8(c1, h2), c2, h2);
					uint16x8_t o2 = vmlal_u8(vmull_u8(c2, h1), c3, h3);
					vst1_lane_u32((uint32_t *)(o + (x + 0u) * 4u), vreinterpret_u32_u8(vshrn_n_u16(o0, 2)), 0);
					vst1_lane_u32((uint32_t *)(o + (x + 1u) * 4u), vreinterpret_u32_u8(vshrn_n_u16(o1, 2)), 0);
					vst1_lane_u32((uint32_t *)(o + (x + 2u) * 4u), vreinterpret_u32_u8(vshrn_n_u16(o2, 2)), 0);
				}
				for (; x < dw; x++) line[yy][x] = 0xFF000000u;
			}
		}
		for (; yy < BAND; yy++)
			for (x = 0; x < dw; x++) line[yy][x] = 0xFF000000u;

		if (nrow == BAND) {
			/* full band: for each group of 4 logical columns, 4 fb rows x BAND contiguous pixels */
			for (x = 0; x + 4u <= dw; x += 4u) {
				unsigned q;
				for (q = 0; q < BAND; q += 4u) {
					uint32x4_t r0, r1, r2, r3;
					unsigned dcol;
					if (!ccw) {
						/* fb row x gets logical rows in DESCENDING order: column dh-1-(y0+BAND-1) .. dh-1-y0;
						 * block q covers rows y0+BAND-1-q-3 .. y0+BAND-1-q -> loaded reversed */
						unsigned top = BAND - 1u - q;   /* line index of the highest row in this block */
						r0 = vld1q_u32(&line[top][x]);      r1 = vld1q_u32(&line[top - 1u][x]);
						r2 = vld1q_u32(&line[top - 2u][x]); r3 = vld1q_u32(&line[top - 3u][x]);
						dcol = dh - BAND - y0 + q;
					} else {
						r0 = vld1q_u32(&line[q][x]);      r1 = vld1q_u32(&line[q + 1u][x]);
						r2 = vld1q_u32(&line[q + 2u][x]); r3 = vld1q_u32(&line[q + 3u][x]);
						dcol = y0 + q;
					}
					{
						uint32x4x2_t t01 = vtrnq_u32(r0, r1), t23 = vtrnq_u32(r2, r3);
						uint32x4_t c0 = vorrq_u32(vcombine_u32(vget_low_u32(t01.val[0]), vget_low_u32(t23.val[0])), opaque);
						uint32x4_t c1 = vorrq_u32(vcombine_u32(vget_low_u32(t01.val[1]), vget_low_u32(t23.val[1])), opaque);
						uint32x4_t c2 = vorrq_u32(vcombine_u32(vget_high_u32(t01.val[0]), vget_high_u32(t23.val[0])), opaque);
						uint32x4_t c3 = vorrq_u32(vcombine_u32(vget_high_u32(t01.val[1]), vget_high_u32(t23.val[1])), opaque);
						if (!ccw) {
							vst1q_u32(fb + (x + 0u) * fbpitch + dcol, c0);
							vst1q_u32(fb + (x + 1u) * fbpitch + dcol, c1);
							vst1q_u32(fb + (x + 2u) * fbpitch + dcol, c2);
							vst1q_u32(fb + (x + 3u) * fbpitch + dcol, c3);
						} else {
							vst1q_u32(fb + (dw - 1u - (x + 0u)) * fbpitch + dcol, c0);
							vst1q_u32(fb + (dw - 1u - (x + 1u)) * fbpitch + dcol, c1);
							vst1q_u32(fb + (dw - 1u - (x + 2u)) * fbpitch + dcol, c2);
							vst1q_u32(fb + (dw - 1u - (x + 3u)) * fbpitch + dcol, c3);
						}
					}
				}
			}
			for (; x < dw; x++) {
				unsigned i;
				for (i = 0; i < BAND; i++) {
					unsigned int p = line[i][x] | 0xFF000000u;
					if (!ccw) fb[x * fbpitch + (dh - 1u - (y0 + i))] = p;
					else      fb[(dw - 1u - x) * fbpitch + y0 + i] = p;
				}
			}
		} else {
			/* short last band: scalar */
			unsigned i;
			for (x = 0; x < dw; x++)
				for (i = 0; i < nrow; i++) {
					unsigned int p = line[i][x] | 0xFF000000u;
					if (!ccw) fb[x * fbpitch + (dh - 1u - (y0 + i))] = p;
					else      fb[(dw - 1u - x) * fbpitch + y0 + i] = p;
				}
		}
	}
}

/* LCD1x / LCD3x prescale, NEON. For each source row and each of the pv sub rows, eight 565
 * source pixels become 8 x ph output pixels: the three channels are scaled by the sub row /
 * sub column factor table (m[k][c], 255 = x1) and the ph sub columns are written interleaved
 * with vst2/vst3/vst4 so the output row is the same layout as the C version
 * (drow[x * ph + k]). ph 1..4 only; the caller keeps the C loop for anything else. */
static inline void lcd_row8(const uint16_t *s, uint32_t *o, const unsigned char (*m)[3], unsigned ph)
{
	uint16x8_t v = vld1q_u16(s);
	uint16x8_t r = vshlq_n_u16(vshrq_n_u16(v, 11), 3);
	uint16x8_t g = vshlq_n_u16(vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x3f)), 2);
	uint16x8_t b = vshlq_n_u16(vandq_u16(v, vdupq_n_u16(0x1f)), 3);
	uint32x4_t lo[4], hi[4];
	unsigned k;
	for (k = 0; k < ph; k++) {
		uint16x8_t rk = vshrq_n_u16(vmulq_u16(r, vdupq_n_u16(m[k][0])), 8);
		uint16x8_t gk = vshrq_n_u16(vmulq_u16(g, vdupq_n_u16(m[k][1])), 8);
		uint16x8_t bk = vshrq_n_u16(vmulq_u16(b, vdupq_n_u16(m[k][2])), 8);
		uint32x4_t a = vdupq_n_u32(0xFF000000u);
		lo[k] = vorrq_u32(vorrq_u32(a, vshlq_n_u32(vmovl_u16(vget_low_u16(rk)), 16)),
				  vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(gk)), 8), vmovl_u16(vget_low_u16(bk))));
		hi[k] = vorrq_u32(vorrq_u32(a, vshlq_n_u32(vmovl_u16(vget_high_u16(rk)), 16)),
				  vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(gk)), 8), vmovl_u16(vget_high_u16(bk))));
	}
	switch (ph) {
	case 1: vst1q_u32(o, lo[0]); vst1q_u32(o + 4, hi[0]); break;
	case 2: { uint32x4x2_t t; t.val[0] = lo[0]; t.val[1] = lo[1]; vst2q_u32(o, t);
		  t.val[0] = hi[0]; t.val[1] = hi[1]; vst2q_u32(o + 8, t); break; }
	case 3: { uint32x4x3_t t; t.val[0] = lo[0]; t.val[1] = lo[1]; t.val[2] = lo[2]; vst3q_u32(o, t);
		  t.val[0] = hi[0]; t.val[1] = hi[1]; t.val[2] = hi[2]; vst3q_u32(o + 12, t); break; }
	default: { uint32x4x4_t t; t.val[0] = lo[0]; t.val[1] = lo[1]; t.val[2] = lo[2]; t.val[3] = lo[3]; vst4q_u32(o, t);
		  t.val[0] = hi[0]; t.val[1] = hi[1]; t.val[2] = hi[2]; t.val[3] = hi[3]; vst4q_u32(o + 16, t); break; }
	}
}
/* row: sw source pixels (already colour corrected), written as pv rows of sw*ph pixels at
 * dst (row pitch fbpitch pixels); mtab = the 8x8x3 factor table indexed [sub row][sub col]. */
void ayaneo_lcd_prescale_row_neon(const uint16_t *src, unsigned sw, uint32_t *dst, unsigned fbpitch,
				  unsigned ph, unsigned pv, const unsigned char (*mtab)[8][3])
{
	unsigned j, x;
	for (j = 0; j < pv; j++) {
		uint32_t *drow = dst + j * fbpitch;
		const unsigned char (*m)[3] = mtab[j < 8u ? j : 7u];
		for (x = 0; x + 8u <= sw; x += 8u)
			lcd_row8(src + x, drow + x * ph, m, ph);
		for (; x < sw; x++) {   /* tail */
			unsigned v = src[x], r = ((v >> 11) & 0x1fu) << 3, g = ((v >> 5) & 0x3fu) << 2, b = (v & 0x1fu) << 3, k;
			for (k = 0; k < ph; k++)
				drow[x * ph + k] = 0xFF000000u | (((r * m[k][0]) >> 8) << 16) | (((g * m[k][1]) >> 8) << 8) | ((b * m[k][2]) >> 8);
		}
	}
}

/* Plain integer prescale (no LCD filter), NEON: eight 565 source pixels become 8 x ph ARGB
 * pixels, the same value repeated across the ph sub columns, and that row is stored to all pv
 * sub rows from registers (the C path rebuilt one row then copied it pv - 1 times). Same
 * pixels as the C loop: r, g, b shifted up without bit replication, alpha opaque. ph 1..4. */
static inline void plain_px8(const uint16_t *s, uint32x4_t *lo, uint32x4_t *hi)
{
	uint16x8_t v = vld1q_u16(s);
	uint16x8_t r = vshlq_n_u16(vshrq_n_u16(v, 11), 3);
	uint16x8_t g = vshlq_n_u16(vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(0x3f)), 2);
	uint16x8_t b = vshlq_n_u16(vandq_u16(v, vdupq_n_u16(0x1f)), 3);
	uint32x4_t a = vdupq_n_u32(0xFF000000u);
	*lo = vorrq_u32(vorrq_u32(a, vshlq_n_u32(vmovl_u16(vget_low_u16(r)), 16)),
			vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(g)), 8), vmovl_u16(vget_low_u16(b))));
	*hi = vorrq_u32(vorrq_u32(a, vshlq_n_u32(vmovl_u16(vget_high_u16(r)), 16)),
			vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(g)), 8), vmovl_u16(vget_high_u16(b))));
}
void ayaneo_plain_prescale_row_neon(const uint16_t *src, unsigned sw, uint32_t *dst, unsigned fbpitch,
				    unsigned ph, unsigned pv)
{
	unsigned x, j;
	for (x = 0; x + 8u <= sw; x += 8u) {
		uint32x4_t lo, hi;
		uint32_t *o = dst + x * ph;
		plain_px8(src + x, &lo, &hi);
		switch (ph) {
		case 1:
			for (j = 0; j < pv; j++, o += fbpitch) { vst1q_u32(o, lo); vst1q_u32(o + 4, hi); }
			break;
		case 2: {
			uint32x4x2_t l, h; l.val[0] = l.val[1] = lo; h.val[0] = h.val[1] = hi;
			for (j = 0; j < pv; j++, o += fbpitch) { vst2q_u32(o, l); vst2q_u32(o + 8, h); }
			break; }
		case 3: {
			uint32x4x3_t l, h; l.val[0] = l.val[1] = l.val[2] = lo; h.val[0] = h.val[1] = h.val[2] = hi;
			for (j = 0; j < pv; j++, o += fbpitch) { vst3q_u32(o, l); vst3q_u32(o + 12, h); }
			break; }
		default: {
			uint32x4x4_t l, h; l.val[0] = l.val[1] = l.val[2] = l.val[3] = lo; h.val[0] = h.val[1] = h.val[2] = h.val[3] = hi;
			for (j = 0; j < pv; j++, o += fbpitch) { vst4q_u32(o, l); vst4q_u32(o + 16, h); }
			break; }
		}
	}
	for (; x < sw; x++) {   /* tail */
		unsigned v = src[x], k;
		uint32_t p = 0xFF000000u | ((((v >> 11) & 0x1fu) << 3) << 16) | ((((v >> 5) & 0x3fu) << 2) << 8) | ((v & 0x1fu) << 3);
		for (j = 0; j < pv; j++)
			for (k = 0; k < ph; k++) dst[j * fbpitch + x * ph + k] = p;
	}
}

/* Rewind delta scan: the first word index >= i (below n) where a ^ b is not zero, n when none.
 * b == 0 scans a against zero. Sixteen words per step while they are all equal, which is most
 * of a state from one frame to the next. */
unsigned ayaneo_xor_scan_neon(const unsigned *a, const unsigned *b, unsigned i, unsigned n)
{
	if (b) {
		while (i + 16u <= n) {
			uint32x4_t d = vorrq_u32(vorrq_u32(veorq_u32(vld1q_u32(a + i), vld1q_u32(b + i)),
							   veorq_u32(vld1q_u32(a + i + 4u), vld1q_u32(b + i + 4u))),
						 vorrq_u32(veorq_u32(vld1q_u32(a + i + 8u), vld1q_u32(b + i + 8u)),
							   veorq_u32(vld1q_u32(a + i + 12u), vld1q_u32(b + i + 12u))));
			uint32x2_t m = vorr_u32(vget_low_u32(d), vget_high_u32(d));
			if (vget_lane_u32(m, 0) | vget_lane_u32(m, 1)) break;
			i += 16u;
		}
		while (i < n && a[i] == b[i]) i++;
	} else {
		while (i + 16u <= n) {
			uint32x4_t d = vorrq_u32(vorrq_u32(vld1q_u32(a + i), vld1q_u32(a + i + 4u)),
						 vorrq_u32(vld1q_u32(a + i + 8u), vld1q_u32(a + i + 12u)));
			uint32x2_t m = vorr_u32(vget_low_u32(d), vget_high_u32(d));
			if (vget_lane_u32(m, 0) | vget_lane_u32(m, 1)) break;
			i += 16u;
		}
		while (i < n && a[i] == 0u) i++;
	}
	return i;
}

/* oem perf:bench memory probe on a free scratch range: 0 = stores, 1 = loads (returns a sum so
 * the reads are not dropped), 2 = copy of the first half onto the second. */
unsigned ayaneo_membench_neon(int which, void *buf, unsigned bytes)
{
	uint32_t *p = (uint32_t *)buf;
	unsigned i, n = bytes / 4u;
	if (which == 0) {
		uint32x4_t v = vdupq_n_u32(0xFF102030u);
		for (i = 0; i + 16u <= n; i += 16u) { vst1q_u32(p + i, v); vst1q_u32(p + i + 4, v); vst1q_u32(p + i + 8, v); vst1q_u32(p + i + 12, v); }
		return 0;
	} else if (which == 1) {
		uint32x4_t s = vdupq_n_u32(0);
		for (i = 0; i + 16u <= n; i += 16u)
			s = vaddq_u32(s, vaddq_u32(vaddq_u32(vld1q_u32(p + i), vld1q_u32(p + i + 4)), vaddq_u32(vld1q_u32(p + i + 8), vld1q_u32(p + i + 12))));
		return vgetq_lane_u32(s, 0) + vgetq_lane_u32(s, 1) + vgetq_lane_u32(s, 2) + vgetq_lane_u32(s, 3);
	} else {
		uint32_t *q = p + n / 2u;
		for (i = 0; i + 16u <= n / 2u; i += 16u) {
			vst1q_u32(q + i, vld1q_u32(p + i)); vst1q_u32(q + i + 4, vld1q_u32(p + i + 4));
			vst1q_u32(q + i + 8, vld1q_u32(p + i + 8)); vst1q_u32(q + i + 12, vld1q_u32(p + i + 12));
		}
		return 0;
	}
}

/* LCD1x / LCD3x prescale in 8 bit lanes: sixteen source pixels a step, each channel times the
 * sub position factor with an 8 x 8 -> 16 bit multiply and the top byte kept ((c * m) >> 8, the
 * same as the 16 bit version), the ph sub columns interleaved with zips and the ARGB pixels
 * written with vst4 (bytes B, G, R, A). About half the work of ayaneo_lcd_prescale_row_neon.
 * ph 1, 2 or 4; the caller uses the 16 bit version for 3. */
static inline uint8x16_t lcd_mul8(uint8x16_t c, uint8_t m)
{
	uint8x8_t mm = vdup_n_u8(m);
	return vcombine_u8(vshrn_n_u16(vmull_u8(vget_low_u8(c), mm), 8), vshrn_n_u16(vmull_u8(vget_high_u8(c), mm), 8));
}
void ayaneo_lcd_prescale_row_neon8(const uint16_t *src, unsigned sw, uint32_t *dst, unsigned fbpitch,
				   unsigned ph, unsigned pv, const unsigned char (*mtab)[8][3])
{
	const uint8x16_t A = vdupq_n_u8(0xFF);
	unsigned x, j;
	for (x = 0; x + 16u <= sw; x += 16u) {
		uint16x8_t v0 = vld1q_u16(src + x), v1 = vld1q_u16(src + x + 8u);
		uint8x16_t r = vandq_u8(vcombine_u8(vshrn_n_u16(v0, 8), vshrn_n_u16(v1, 8)), vdupq_n_u8(0xF8));   /* (v >> 11) << 3 */
		uint8x16_t g = vshlq_n_u8(vcombine_u8(vshrn_n_u16(v0, 5), vshrn_n_u16(v1, 5)), 2);                 /* ((v >> 5) & 63) << 2 */
		uint8x16_t b = vshlq_n_u8(vcombine_u8(vmovn_u16(v0), vmovn_u16(v1)), 3);   /* low 5 bits, shifted out the rest */
		for (j = 0; j < pv; j++) {
			const unsigned char (*m)[3] = mtab[j < 8u ? j : 7u];
			uint8_t *o = (uint8_t *)(dst + j * fbpitch + x * ph);
			uint8x16x4_t q;
			q.val[3] = A;
			if (ph == 1) {
				q.val[2] = lcd_mul8(r, m[0][0]); q.val[1] = lcd_mul8(g, m[0][1]); q.val[0] = lcd_mul8(b, m[0][2]);
				vst4q_u8(o, q);
			} else if (ph == 2) {
				uint8x16x2_t R = vzipq_u8(lcd_mul8(r, m[0][0]), lcd_mul8(r, m[1][0]));
				uint8x16x2_t G = vzipq_u8(lcd_mul8(g, m[0][1]), lcd_mul8(g, m[1][1]));
				uint8x16x2_t B = vzipq_u8(lcd_mul8(b, m[0][2]), lcd_mul8(b, m[1][2]));
				q.val[2] = R.val[0]; q.val[1] = G.val[0]; q.val[0] = B.val[0]; vst4q_u8(o, q);
				q.val[2] = R.val[1]; q.val[1] = G.val[1]; q.val[0] = B.val[1]; vst4q_u8(o + 64, q);
			} else {
				/* zip(k0, k2) and zip(k1, k3), then zip those: k0 k1 k2 k3 per source pixel */
				uint8x16x2_t r02 = vzipq_u8(lcd_mul8(r, m[0][0]), lcd_mul8(r, m[2][0]));
				uint8x16x2_t r13 = vzipq_u8(lcd_mul8(r, m[1][0]), lcd_mul8(r, m[3][0]));
				uint8x16x2_t g02 = vzipq_u8(lcd_mul8(g, m[0][1]), lcd_mul8(g, m[2][1]));
				uint8x16x2_t g13 = vzipq_u8(lcd_mul8(g, m[1][1]), lcd_mul8(g, m[3][1]));
				uint8x16x2_t b02 = vzipq_u8(lcd_mul8(b, m[0][2]), lcd_mul8(b, m[2][2]));
				uint8x16x2_t b13 = vzipq_u8(lcd_mul8(b, m[1][2]), lcd_mul8(b, m[3][2]));
				uint8x16x2_t R0 = vzipq_u8(r02.val[0], r13.val[0]), R1 = vzipq_u8(r02.val[1], r13.val[1]);
				uint8x16x2_t G0 = vzipq_u8(g02.val[0], g13.val[0]), G1 = vzipq_u8(g02.val[1], g13.val[1]);
				uint8x16x2_t B0 = vzipq_u8(b02.val[0], b13.val[0]), B1 = vzipq_u8(b02.val[1], b13.val[1]);
				q.val[2] = R0.val[0]; q.val[1] = G0.val[0]; q.val[0] = B0.val[0]; vst4q_u8(o, q);
				q.val[2] = R0.val[1]; q.val[1] = G0.val[1]; q.val[0] = B0.val[1]; vst4q_u8(o + 64, q);
				q.val[2] = R1.val[0]; q.val[1] = G1.val[0]; q.val[0] = B1.val[0]; vst4q_u8(o + 128, q);
				q.val[2] = R1.val[1]; q.val[1] = G1.val[1]; q.val[0] = B1.val[1]; vst4q_u8(o + 192, q);
			}
		}
	}
	if (x < sw) {   /* the last pixels through the 16 bit version (eight a step, scalar tail) */
		ayaneo_lcd_prescale_row_neon(src + x, sw - x, dst + x * ph, fbpitch, ph, pv, mtab);
	}
}

