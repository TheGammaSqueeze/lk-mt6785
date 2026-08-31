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

#endif /* GBA_PUNCH_H */
