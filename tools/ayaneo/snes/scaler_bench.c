/*
 * scaler_bench.c - HOST benchmark of the SNES display scaler (ayaneo_snes_show_frame fast path).
 * Replicates the exact row-replication scale loop to measure Pixel-perfect vs Stretch blit cost
 * on the dev box, before touching the device. Build: cc -O2 scaler_bench.c -o /tmp/sbench
 *
 * The panel is 1280x960 ARGB (pitch 1280). SNES source is 256x224 RGB565.
 *   Pixel-perfect: integer 4x -> dw=1024, dh=896 (letterboxed).
 *   Stretch:       fill panel -> dw=1280, dh=960.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define W 1280u
#define H 960u
#define PITCH 1280u        /* ALIGN_TO(1280, fb_align); 1280 already aligned */

static uint32_t *g_dst;
static uint16_t *g_src;

/* the fast (filt==0) path, byte-for-byte the same structure as mt_disp_drv.c */
static void scale_fast(const uint16_t *pix, unsigned sw, unsigned sh, unsigned spitch_px,
                       unsigned dw, unsigned dh)
{
	uint32_t *dst = g_dst;
	int xoff = ((int)W - (int)dw) / 2, yoff = ((int)H - (int)dh) / 2;
	unsigned qx = dw / sw, rx = dw % sw;
	unsigned qy = dh / sh, ry = dh % sh;
	unsigned dy = (unsigned)yoff, ey = 0, sy, sx, iy, cx;
	if (xoff < 0) xoff = 0; if (yoff < 0) yoff = 0;
	for (sy = 0; sy < sh; sy++) {
		const uint16_t *srow = pix + sy * spitch_px;
		unsigned vspan = qy, dyend, dx = (unsigned)xoff, ex_e = 0;
		ey += ry; if (ey >= sh) { ey -= sh; vspan++; }
		dyend = dy + vspan;
		uint32_t *o0 = dst + dy * PITCH;
		for (sx = 0; sx < sw; sx++) {
			unsigned v = srow[sx];
			unsigned r = ((v >> 11) & 0x1f) << 3;
			unsigned g = ((v >> 5) & 0x3f) << 2;
			unsigned b = (v & 0x1f) << 3;
			unsigned px = 0xFF000000u | (r << 16) | (g << 8) | b;
			unsigned span = qx, ex;
			ex_e += rx; if (ex_e >= sw) { ex_e -= sw; span++; }
			ex = dx + span;
			for (cx = dx; cx < ex; cx++) o0[cx] = px;
			dx = ex;
		}
		for (iy = dy + 1; iy < dyend; iy++)
			memcpy(dst + iy * PITCH + (unsigned)xoff, o0 + (unsigned)xoff, (size_t)dw * 4u);
		dy = dyend;
	}
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec*1e3 + t.tv_nsec/1e6; }

static double bench(unsigned sw, unsigned sh, unsigned dw, unsigned dh, int iters, const char *name)
{
	/* warm */
	for (int i = 0; i < 8; i++) scale_fast(g_src, sw, sh, sw, dw, dh);
	double t0 = now_ms();
	for (int i = 0; i < iters; i++) scale_fast(g_src, sw, sh, sw, dw, dh);
	double t1 = now_ms();
	double per = (t1 - t0) / iters;
	unsigned long px = (unsigned long)dw * dh;
	printf("  %-16s dw=%u dh=%u  dst_px=%lu  %.4f ms/frame\n", name, dw, dh, px, per);
	return per;
}

int main(void)
{
	g_dst = malloc((size_t)PITCH * H * 4);
	g_src = malloc(256 * 224 * 2);
	/* a busy source (varying colours so decode is exercised, not a constant) */
	for (unsigned y = 0; y < 224; y++)
		for (unsigned x = 0; x < 256; x++)
			g_src[y*256 + x] = (uint16_t)((x*7 + y*13) ^ (x<<3) ^ (y<<8));
	memset(g_dst, 0, (size_t)PITCH * H * 4);

	int iters = 2000;
	printf("SNES scaler host bench (256x224 src, %ux%u panel), %d iters:\n", W, H, iters);
	double pixel   = bench(256, 224, 1024, 896, iters, "Pixel-perfect");
	double stretch = bench(256, 224, 1280, 960, iters, "Stretch");

	/* cache-flush proxy: full-panel vs game-band memset (device does arch_clean_cache_range;
	 * on host we just show the relative size of full-panel vs a partial flush) */
	double t0 = now_ms();
	for (int i = 0; i < iters; i++) memset(g_dst, 0, (size_t)PITCH * H * 4);
	double t1 = now_ms();
	printf("  full-panel flush proxy (memset %uKB): %.4f ms\n", (PITCH*H*4)/1024, (t1-t0)/iters);

	printf("=> Stretch is %.1f%% slower than Pixel in the blit loop (%.4f vs %.4f ms)\n",
	       (stretch/pixel - 1.0) * 100.0, stretch, pixel);
	return 0;
}
