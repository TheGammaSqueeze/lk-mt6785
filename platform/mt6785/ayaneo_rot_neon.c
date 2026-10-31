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
