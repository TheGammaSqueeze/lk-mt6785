/* Host test for gba_punch_composite (emu/gba/menu/gba_punch.h) - the punch-hole
 * launch-transition compositor. The transition runs only on device (live game
 * frames + framebuffer), so this pins the pure pixel MATH: game inside the circle,
 * black in the game letterbox inside the circle, frozen menu snapshot outside.
 * Build+run: gcc -O2 -Iemu/gba/menu -o /tmp/punch emu/gba/gba_punch_test.c && /tmp/punch */
#include <stdio.h>
#include <stdlib.h>
#include "gba_punch.h"

#define W     1280
#define H     960
#define S     5
#define SRCW  240
#define SRCH  160
#define XOFF  ((W - SRCW * S) / 2)   /* 40 */
#define YOFF  ((H - SRCH * S) / 2)   /* 80 */

#define SNAP_MARK  0xFF112233u       /* menu snapshot fill */
#define GAME_565   0x001Fu           /* pure blue -> b=0xF8 */
#define GAME_RGBA  0xFF0000F8u       /* what GAME_565 must expand to */
#define BLACK      0xFF000000u       /* game letterbox */

static int fails;
static void ck(int cond, const char *msg)
{
	if (!cond) { printf("  FAIL %s\n", msg); fails++; }
}

int main(void)
{
	uint32_t *dst  = malloc((size_t)W * H * 4);
	uint32_t *snap = malloc((size_t)W * H * 4);
	uint16_t *pix  = malloc((size_t)SRCW * SRCH * 2);
	int i, cx = W / 2, cy = H / 2;

	for (i = 0; i < W * H; i++) snap[i] = SNAP_MARK;
	for (i = 0; i < SRCW * SRCH; i++) pix[i] = GAME_565;

	/* radius 0: nothing revealed -> entire frame is the snapshot. */
	for (i = 0; i < W * H; i++) dst[i] = 0;
	gba_punch_composite(dst, snap, W, W, H, pix, cx, cy, 0, S, SRCW, SRCH, XOFF, YOFF);
	{ int all = 1; for (i = 0; i < W * H; i++) if (dst[i] != SNAP_MARK) { all = 0; break; }
	  ck(all, "radius 0 -> all snapshot"); }

	/* radius 100 at centre (inside the game area): centre = game, far corner = snapshot. */
	gba_punch_composite(dst, snap, W, W, H, pix, cx, cy, 100, S, SRCW, SRCH, XOFF, YOFF);
	ck(dst[cy * W + cx] == GAME_RGBA, "small hole: centre is game");
	ck(dst[(cy - 3) * W + (cx + 3)] == GAME_RGBA, "small hole: near-centre is game");
	ck(dst[0] == SNAP_MARK, "small hole: corner (0,0) is snapshot");
	ck(dst[(H - 1) * W + (W - 1)] == SNAP_MARK, "small hole: far corner is snapshot");
	/* just inside vs just outside the radius along the centre row */
	ck(dst[cy * W + (cx + 90)] == GAME_RGBA, "boundary: r-10 inside is game");
	ck(dst[cy * W + (cx + 110)] == SNAP_MARK, "boundary: r+10 outside is snapshot");

	/* huge radius: whole screen inside the hole -> no snapshot anywhere; game in the
	 * centred game rect, black letterbox outside it. */
	gba_punch_composite(dst, snap, W, W, H, pix, cx, cy, 4000, S, SRCW, SRCH, XOFF, YOFF);
	{ int any_snap = 0; for (i = 0; i < W * H; i++) if (dst[i] == SNAP_MARK) { any_snap = 1; break; }
	  ck(!any_snap, "full cover: no snapshot left"); }
	ck(dst[cy * W + cx] == GAME_RGBA, "full cover: centre is game");
	ck(dst[0] == BLACK, "full cover: corner is black letterbox");
	ck(dst[(YOFF - 5) * W + cx] == BLACK, "full cover: above game rect is black");
	ck(dst[(YOFF + 2) * W + (XOFF + 2)] == GAME_RGBA, "full cover: game rect top-left is game");
	ck(dst[cy * W + (XOFF - 5)] == BLACK, "full cover: left of game rect is black");

	/* default centre (cx<0) matches explicit W/2,H/2 */
	{
		uint32_t *d2 = malloc((size_t)W * H * 4);
		gba_punch_composite(d2, snap, W, W, H, pix, -1, -1, 300, S, SRCW, SRCH, XOFF, YOFF);
		gba_punch_composite(dst, snap, W, W, H, pix, cx, cy, 300, S, SRCW, SRCH, XOFF, YOFF);
		{ int same = 1; for (i = 0; i < W * H; i++) if (d2[i] != dst[i]) { same = 0; break; }
		  ck(same, "cx/cy < 0 defaults to screen centre"); }
		free(d2);
	}

	if (fails) { printf("GBA_PUNCH TEST: %d FAIL\n", fails); return 1; }
	printf("GBA_PUNCH TEST: PASS\n");
	return 0;
}
