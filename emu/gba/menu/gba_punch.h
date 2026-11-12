/* Punch-hole launch-transition compositor, shared between the on-device display
 * driver (platform/mt6785/mt_disp_drv.c: ayaneo_gba_punch_frame) and the host test
 * (emu/gba/gba_punch_test.c). The whole transition is a device-only path (live game
 * frames + framebuffer), so keeping the PIXEL MATH in one pure, host-testable helper
 * is how its geometry/upscale correctness gets validated off-device.
 *
 * Composites the live game frame (`pix`, RGB565 SRC_W x SRC_H, drawn at scale S,
 * centred at xoff/yoff with a black letterbox) INSIDE a circle of `radius` centred at
 * (cx,cy), over the frozen menu `snap` (a full W x H, pitch-wide BGRA snapshot) OUTSIDE
 * it. Writes W x H BGRA into `dst`. Per row: memcpy the snapshot on the two outside
 * spans, per-pixel only inside the hole. No libc beyond memcpy; no LK/display deps. */
#ifndef GBA_PUNCH_H
#define GBA_PUNCH_H

#include <stdint.h>
#include <string.h>

/* integer sqrt (floor); no libm on the LK path. */
static inline int gba_punch_isqrt(long n)
{
	long x, y;
	if (n <= 0)
		return 0;
	x = n; y = (x + 1) / 2;
	while (y < x) { x = y; y = (x + n / x) / 2; }
	return (int)x;
}

static inline void gba_punch_composite(uint32_t *dst, const uint32_t *snap, int pitch,
				       int W, int H, const uint16_t *pix,
				       int cx, int cy, int radius,
				       int S, int SRC_W, int SRC_H, int xoff, int yoff)
{
	long r2 = (long)radius * radius;
	int y, x;
	if (cx < 0) cx = W / 2;                               /* default: screen centre */
	if (cy < 0) cy = H / 2;
	for (y = 0; y < H; y++) {
		uint32_t *drow = dst + (unsigned)y * pitch;
		const uint32_t *srow = snap + (unsigned)y * pitch;
		int dy = y - cy;
		long dd = r2 - (long)dy * dy;
		int hw = (dd <= 0) ? -1 : gba_punch_isqrt(dd);   /* hole half-width at row y */
		int x0, x1;
		if (hw < 0) {                                    /* whole row outside the hole */
			memcpy(drow, srow, (size_t)W * 4);
			continue;
		}
		x0 = cx - hw; x1 = cx + hw;
		if (x0 < 0) x0 = 0;
		if (x1 > W - 1) x1 = W - 1;
		if (x0 > 0)                                      /* left snapshot span */
			memcpy(drow, srow, (size_t)x0 * 4);
		for (x = x0; x <= x1; x++) {                     /* inside the hole: game/black */
			if (x >= xoff && x < xoff + SRC_W * S &&
			    y >= yoff && y < yoff + SRC_H * S) {
				unsigned int v = pix[(y - yoff) / S * SRC_W + (x - xoff) / S];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				drow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			} else {
				drow[x] = 0xFF000000u;                   /* game letterbox */
			}
		}
		if (x1 + 1 < W)                                  /* right snapshot span */
			memcpy(drow + x1 + 1, srow + x1 + 1, (size_t)(W - x1 - 1) * 4);
	}
}

/* FAST reverse path: when the game frame is FROZEN (the close transition), convert +
 * upscale it to a full W x H BGRA buffer ONCE (this helper), so each transition frame
 * is then just memcpy (gba_punch_composite_pre) instead of a per-pixel RGB565->888
 * convert. Turns a ~50ms/frame composite into ~5ms = a smooth 60fps shrink. `game_full`
 * is pitch-wide like dst; game at scale S centred at xoff/yoff, black letterbox. */
static inline void gba_punch_prerender(uint32_t *game_full, int pitch, int W, int H,
				       const uint16_t *pix, int S, int SRC_W, int SRC_H,
				       int xoff, int yoff)
{
	int y, x;
	for (y = 0; y < H; y++) {
		uint32_t *row = game_full + (unsigned)y * pitch;
		int in_gy = (y >= yoff && y < yoff + SRC_H * S);
		const uint16_t *srow = in_gy ? (pix + (unsigned)((y - yoff) / S) * SRC_W) : 0;
		for (x = 0; x < W; x++) {
			if (in_gy && x >= xoff && x < xoff + SRC_W * S) {
				unsigned int v = srow[(x - xoff) / S];
				unsigned int r = ((v >> 11) & 0x1f) << 3;
				unsigned int g = ((v >> 5) & 0x3f) << 2;
				unsigned int b = (v & 0x1f) << 3;
				row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			} else {
				row[x] = 0xFF000000u;
			}
		}
	}
}

/* memcpy-only circle composite: game_full inside the hole, snap outside. */
/* A menu theme can ask for a fade instead of the hole (the Current theme): g_punch_fade then holds
 * the transition's largest radius, and a radius is how far the fade has gone, the game blended
 * over the snapshot by radius / g_punch_fade. 0 = the hole. With g_punch_ov the menu's own layer
 * (premultiplied ARGB, W wide, g_punch_ov_x0/x1 the part of each row it covers) goes over the
 * fade, zooming out of view as it fades, in step with it (in, on the way back). Both are defined
 * by the display driver. */
extern int g_punch_fade;
extern int g_punch_cached;   /* the snapshot and the game are in cached memory: no row bursts */
extern const uint32_t *g_punch_ov;
extern const int16_t *g_punch_ov_x0, *g_punch_ov_x1;

static inline void gba_punch_menu_over(uint32_t *d, int y, int W, int H, int k)
{
	const uint32_t *ov = g_punch_ov;
	int x;
	if (!k) {	/* the menu as it is */
		const uint32_t *r = ov + (unsigned)y * W;
		for (x = g_punch_ov_x0[y]; x < g_punch_ov_x1[y]; x++) {
			uint32_t u = r[x], a = u >> 24, dd = d[x], kk;
			if (!a) continue;
			if (a == 255) { d[x] = u; continue; }
			kk = 256 - a - (a >> 7);
			d[x] = 0xFF000000u | ((((dd & 0xFF00FFu) * kk >> 8) & 0xFF00FFu) + (u & 0xFF00FFu)) |
			       ((((dd & 0x00FF00u) * kk >> 8) & 0x00FF00u) + (u & 0x00FF00u));
		}
	} else {	/* growing by up to a quarter about the centre, fading as it goes */
		static int16_t sxmap[2048];
		static int map_k = -1, map_w;
		int e = (k * k * (768 - 2 * k)) >> 16, op = 256 - e, z = 256 + e / 4, inv = (int)(65536L * 256 / z);
		int cx = W / 2, cy = H / 2, sy = cy + (int)(((long)(y - cy) * inv) >> 16), x0, x1, xa, xb;
		const uint32_t *r;
		if (W > 2048) W = 2048;
		if (map_k != k || map_w != W) {	/* the source column of each screen column, once a frame */
			for (x = 0; x < W; x++) sxmap[x] = (int16_t)(cx + (int)(((long)(x - cx) * inv) >> 16));
			map_k = k; map_w = W;
		}
		if (sy < 0 || sy >= H || g_punch_ov_x1[sy] <= g_punch_ov_x0[sy] || op <= 0) return;
		r = ov + (unsigned)sy * W; x0 = g_punch_ov_x0[sy]; x1 = g_punch_ov_x1[sy];
		/* only the screen span that lands on the drawn part of the row */
		xa = cx + (int)(((long)(x0 - cx) * z) >> 8) - 1; xb = cx + (int)(((long)(x1 - cx) * z) >> 8) + 1;
		if (xa < 0) xa = 0;
		if (xb > W) xb = W;
		for (x = xa; x < xb; x++) {
			int sx = sxmap[x];
			uint32_t u, a, dd, kk;
			if (sx < x0 || sx >= x1) continue;
			u = r[sx]; a = ((u >> 24) * (uint32_t)op) >> 8;
			if (!a) continue;
			dd = d[x]; kk = 256 - a - (a >> 7);
			d[x] = 0xFF000000u |
			       ((((dd & 0xFF00FFu) * kk >> 8) + ((u & 0xFF00FFu) * (uint32_t)op >> 8)) & 0xFF00FFu) |
			       ((((dd & 0x00FF00u) * kk >> 8) + ((u & 0x00FF00u) * (uint32_t)op >> 8)) & 0x00FF00u);
		}
	}
}

static inline void gba_punch_fade_rows(uint32_t *dst, const uint32_t *snap, const uint32_t *game_full,
				       int pitch, int W, int H, int radius)
{
	/* the snapshot and the game sit in display memory that is not cached (write combined on the
	 * Konkr): a pixel at a time from there is ~100 ns, so each row comes over in one burst first */
	static uint32_t rs[2048], rg[2048];
	int y, x, a = (int)(((long)radius * 256) / g_punch_fade), k;
	if (a < 0) a = 0;
	if (a > 256) a = 256;
	if (W > 2048) W = 2048;
	k = a;   /* the menu leaves (or comes back) in step with the fade */
	for (y = 0; y < H; y++) {
		uint32_t *d = dst + (unsigned)y * pitch;
		const uint32_t *s = snap + (unsigned)y * pitch, *g = game_full + (unsigned)y * pitch;
		if (a >= 256) memcpy(d, g, (size_t)W * 4);
		else if (a == 0) memcpy(d, s, (size_t)W * 4);
		else {
			if (!g_punch_cached) {
				memcpy(rs, s, (size_t)W * 4); s = rs;
				memcpy(rg, g, (size_t)W * 4); g = rg;
			}
			for (x = 0; x < W; x++) {
				uint32_t rb = s[x] & 0xFF00FFu, gg = s[x] & 0x00FF00u;
				rb += (((g[x] & 0xFF00FFu) - rb) * (uint32_t)a) >> 8;
				gg += (((g[x] & 0x00FF00u) - gg) * (uint32_t)a) >> 8;
				d[x] = 0xFF000000u | (rb & 0xFF00FFu) | (gg & 0x00FF00u);
			}
		}
		if (g_punch_ov && k < 256) gba_punch_menu_over(d, y, W, H, k);
	}
}

/* oem diag trans_us: the last transition frame's composite, the worst, the frames (display driver) */
extern volatile unsigned int g_dbg_trans_us[3], g_dbg_trans_list[16];
static inline void gba_punch_composite_pre(uint32_t *dst, const uint32_t *snap,
					   const uint32_t *game_full, int pitch,
					   int W, int H, int cx, int cy, int radius)
{
	long r2 = (long)radius * radius;
	int y;
	if (g_punch_fade > 0) {
		extern unsigned int gpt4_get_current_tick(void);
		unsigned int t0 = gpt4_get_current_tick(), us;
		gba_punch_fade_rows(dst, snap, game_full, pitch, W, H, radius);
		us = (gpt4_get_current_tick() - t0) / 13u;
		g_dbg_trans_us[0] = us;
		if (us > g_dbg_trans_us[1]) g_dbg_trans_us[1] = us;
		g_dbg_trans_list[g_dbg_trans_us[2] & 15] = us;
		g_dbg_trans_us[2]++;
		return;
	}
	if (cx < 0) cx = W / 2;
	if (cy < 0) cy = H / 2;
	for (y = 0; y < H; y++) {
		uint32_t *drow = dst + (unsigned)y * pitch;
		const uint32_t *srow = snap + (unsigned)y * pitch;
		const uint32_t *grow = game_full + (unsigned)y * pitch;
		int dy = y - cy;
		long dd = r2 - (long)dy * dy;
		int hw = (dd <= 0) ? -1 : gba_punch_isqrt(dd);
		int x0, x1;
		if (hw < 0) { memcpy(drow, srow, (size_t)W * 4); continue; }
		x0 = cx - hw; x1 = cx + hw;
		if (x0 < 0) x0 = 0;
		if (x1 > W - 1) x1 = W - 1;
		if (x0 > 0)        memcpy(drow, srow, (size_t)x0 * 4);
		memcpy(drow + x0, grow + x0, (size_t)(x1 - x0 + 1) * 4);
		if (x1 + 1 < W)    memcpy(drow + x1 + 1, srow + x1 + 1, (size_t)(W - x1 - 1) * 4);
	}
}

#endif /* GBA_PUNCH_H */
