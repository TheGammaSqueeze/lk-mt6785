/*
 * Fastboot debug channel for the SNES-Classic-mini menu (AYANEO_GBA_SD).
 *
 * The USB fastboot stack is brought up ALONGSIDE the running emulator (see
 * ayaneo_fastboot_usb_start in mt_boot.c), so these work over USB while the
 * menu/game is live - no reboot into fastboot mode. They let me diagnose the
 * display issues host_render can never reproduce (banding is a scanout artifact,
 * timings are A55-specific) by reading the LIVE panel + metrics and driving nav:
 *
 *   fastboot oem diag               - live render metrics: last/peak render us, loop
 *                                     fps, present vs gate-skipped counts, focus.
 *   fastboot oem key:<L R U D A B S T [ ]>
 *                                   - inject one button into the live menu (held a
 *                                     few frames = a clean press), so I can navigate
 *                                     over USB and screenshot each state.
 *   fastboot oem shot[:step]        - dump the currently displayed frame downscaled
 *                                     (default /16 = 80x60) as RGB565 hex rows, 13
 *                                     px per INFO line. Reconstruct a PNG with
 *                                     tools/ayaneo/gba/fastboot_menu_shot.py.
 *
 * IMPORTANT: everything here uses ONLY bounded fastboot_info()/okay() writes (each a
 * single <=64-byte transfer that always completes). An earlier DATA-phase bulk
 * upload (get_staged) could leave the IN endpoint stalled if the host aborted
 * mid-transfer, wedging the USB link ("no link") until a device reboot - do NOT
 * reintroduce a bulk/DATA response here.
 */
extern void fastboot_register(const char *prefix,
			      void (*handle)(const char *arg, void *data, unsigned sz),
			      int allow_locked, int need_download);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern int  snprintf(char *str, unsigned long size, const char *fmt, ...);
extern void thread_sleep(unsigned ms);

/* live panel + metrics + input injection (display driver + gba_snes_menu.c) */
extern const unsigned int *ayaneo_canvas_front(unsigned int *pitch, unsigned int *W, unsigned int *H);
extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
extern volatile unsigned int g_dbg_present_cnt;
extern volatile int g_dbg_focus;
extern volatile int g_inject_btn, g_inject_frames;
extern volatile int g_cap_want, g_cap_have;
extern volatile unsigned int g_cap_pitch, g_cap_w, g_cap_h;
extern volatile int g_dbg_reverse_ran, g_dbg_arm_cnt, g_dbg_force_close;
#define GBA_CAP_PA  0x4E000000u
#define GBA_CAP_MAX 6

static char lbuf[96];

static unsigned long parse_u(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == ':') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned long)(*s - '0'); s++; }
	return v;
}

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "render=%u fps=%u focus=%d arm=%d rev=%d",
		 g_dbg_render_us, g_dbg_fps, g_dbg_focus, g_dbg_arm_cnt, g_dbg_reverse_ran);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static int keycode(const char *s)
{
	while (*s == ' ' || *s == ':') s++;
	switch (*s) {
	case 'L': return 0; case 'R': return 1; case 'U': return 2; case 'D': return 3;
	case 'A': return 4; case 'B': return 5; case 'S': return 6; case 'T': return 7;
	case '[': return 8; case ']': return 9;
	default:  return -1;
	}
}

static void cmd_key(const char *arg, void *data, unsigned sz)
{
	int k = keycode(arg), w = 0;
	(void)data; (void)sz;
	if (k < 0) { fastboot_fail("bad key"); return; }
	g_inject_btn = k;
	g_inject_frames = 3;                       /* held ~3 frames = a clean press edge */
	while (g_inject_frames > 0 && w < 400) { thread_sleep(2); w++; }  /* wait consumed */
	thread_sleep(220);                         /* let the nav animation settle */
	fastboot_okay("");
}

/* Downscaled RGB565 hex screenshot over bounded INFO lines (13 px = 52 hex + a 4
 * char row tag <= 63 bytes, safely under MAX_RSP_SIZE). Wide rows continue on
 * "<row>+" lines. Reconstruct with tools/ayaneo/gba/fastboot_menu_shot.py. */
/* Dump one downscaled RGB565 hex frame from `base` (front buffer or a capture-ring
 * slot). 13 px per <=64-byte INFO line; wide rows continue on "<row>+" lines. */
static void dump_frame_hex(const unsigned int *base, unsigned int pitch,
			   unsigned int W, unsigned int H, int step)
{
	static const char *hx = "0123456789abcdef";
	unsigned int x, y;
	if (step <= 0) step = 16;
	for (y = 0; y < H; y += (unsigned)step) {
		const unsigned int *row = base + (unsigned long)y * pitch;
		char *o = lbuf;
		int col = 0, k;
		k = snprintf(lbuf, sizeof lbuf, "%03u:", y / (unsigned)step);
		o += k;
		for (x = 0; x < W; x += (unsigned)step) {
			unsigned int px = row[x];
			unsigned int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
			unsigned int p = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			if (col == 13) {
				*o = 0; fastboot_info(lbuf);
				o = lbuf; o += snprintf(lbuf, sizeof lbuf, "%03u+", y / (unsigned)step);
				col = 0;
			}
			*o++ = hx[(p >> 12) & 0xf]; *o++ = hx[(p >> 8) & 0xf];
			*o++ = hx[(p >> 4) & 0xf];  *o++ = hx[p & 0xf];
			col++;
		}
		*o = 0;
		fastboot_info(lbuf);
	}
}

static void cmd_getframe(const char *arg, void *data, unsigned sz)
{
	int i = (int)parse_u(arg);
	unsigned long bytes;
	(void)data; (void)sz;
	if (g_cap_have <= 0 || i < 0 || i >= g_cap_have) { fastboot_fail("bad idx"); return; }
	bytes = (unsigned long)g_cap_pitch * g_cap_h * 4u;
	dump_frame_hex((const unsigned int *)(unsigned long)(GBA_CAP_PA + (unsigned long)i * bytes),
		       g_cap_pitch, g_cap_w, g_cap_h, 20);
	fastboot_okay("");
}

/* oem close: trigger the in-game close path (game -> menu reverse) so it can be
 * tested over USB. Read `oem diag` after: arm=<n> rev=<n> shows whether the driver
 * armed the reverse and whether the reverse block actually ran. */
static void cmd_close(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	g_dbg_force_close = 1;
	thread_sleep(400);
	fastboot_okay("");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem key:", cmd_key, 1, 0);
	fastboot_register("oem getframe:", cmd_getframe, 1, 0);
	fastboot_register("oem close", cmd_close, 1, 0);
}
