/*
 * GammaOS Pico display, part 1 of 4 (platform/common/pico): the transitions' punch hole and fade,
 * and the last game picture (the in-game menu's glass). Included as text by each board's
 * mt_disp_drv.c after its own includes, as the code always sat there.
 */
#include "../../../emu/gba/menu/gba_punch.h"	/* gba_punch_composite (shared w/ host test) */
int g_punch_fade;   /* 0 the punch-hole, else a fade (gba_punch.h) */
int g_punch_cached;   /* gba_punch.h: its buffers are cached (a theme's own) */
const uint32_t *g_punch_ov;                     /* the menu over the fade, or none */
const int16_t *g_punch_ov_x0, *g_punch_ov_x1;
/* frames of the opening and closing transitions: the hole takes 20, a theme's fade 8 (quick) */
/* the steps of the next opening or closing; a caller may set the next one's once
 * (ayaneo_punch_steps_once: the slow fade of a game resumed after the BIOS intro) */
static int s_punch_steps_once;
void ayaneo_punch_steps_once(int n) { s_punch_steps_once = n; }
int ayaneo_punch_steps(void)
{
	int n = s_punch_steps_once;
	if (n > 0) { s_punch_steps_once = 0; return n; }
	return g_punch_fade > 0 ? 8 : 20;
}
/* one transition frame (gba_punch_composite_pre), built here at this file's optimisation: the
 * menu's own file is built for size and composed the same frame at half the speed */
__attribute__((optimize("O3")))
void ayaneo_punch_compose(uint32_t *dst, const uint32_t *snap, const uint32_t *game_full, int pitch,
			  int W, int H, int radius)
{
	gba_punch_composite_pre(dst, snap, game_full, pitch, W, H, -1, -1, radius);
}
volatile unsigned int g_dbg_trans_us[3], g_dbg_trans_list[16];   /* gba_punch.h: the fade's frame cost */
volatile unsigned int g_dbg_game_presents, g_dbg_game_long;   /* resizer presents, and gaps over 25 ms */
/* The last game frame the resizer showed (RGB565, as the core made it) and its rectangle on the
 * logical screen: what lies behind the in-game menu, which draws it blurred as its glass. */
static struct { const unsigned short *pix; unsigned int sw, sh, pitch; int r[4]; } s_pic;
int ayaneo_game_picture(const unsigned short **pix, unsigned int *sw, unsigned int *sh, unsigned int *pitch, int *rect)
{
	if (!s_pic.pix || !s_pic.sw || !s_pic.sh) return 0;
	*pix = s_pic.pix; *sw = s_pic.sw; *sh = s_pic.sh; *pitch = s_pic.pitch;
	rect[0] = s_pic.r[0]; rect[1] = s_pic.r[1]; rect[2] = s_pic.r[2]; rect[3] = s_pic.r[3];
	return 1;
}
/* a game ends: its frame memory is not its own any more */
void ayaneo_game_picture_forget(void) { s_pic.pix = 0; }
volatile unsigned int g_dbg_launch_t0, g_dbg_launch_us[4];   /* the opening's end, then the game's own marks */
volatile unsigned int g_dbg_launch_a, g_dbg_launch_pre_us[6];   /* from the launch press: the core's loads, then the opening */
