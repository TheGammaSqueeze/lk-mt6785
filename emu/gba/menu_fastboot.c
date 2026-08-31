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
extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
extern volatile unsigned int g_dbg_present_cnt;
extern volatile int g_dbg_focus;
extern volatile int g_dbg_reverse_ran, g_dbg_arm_cnt, g_dbg_force_close, g_dbg_force_launch;
extern volatile int g_cap_want, g_cap_have;
extern volatile unsigned int g_cap_pitch, g_cap_w, g_cap_h;
#define GBA_CAP_PA  0x4E000000u
#define GBA_CAP_MAX 6

static char lbuf[96];

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "render=%u fps=%u arm=%d rev=%d",
		 g_dbg_render_us, g_dbg_fps, g_dbg_arm_cnt, g_dbg_reverse_ran);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Dump downscaled RGB565 hex frame i from the capture ring (13 px per <=64B line). */
static void cmd_getframe(const char *arg, void *data, unsigned sz)
{
	static const char *hx = "0123456789abcdef";
	const unsigned int *base;
	unsigned int W = g_cap_w, H = g_cap_h, pitch = g_cap_pitch, x, y;
	int i = 0, step = 20;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') { i = i * 10 + (*arg - '0'); arg++; }
	if (g_cap_have <= 0 || i < 0 || i >= g_cap_have) { fastboot_fail("bad idx"); return; }
	base = (const unsigned int *)(unsigned long)(GBA_CAP_PA +
		(unsigned long)i * pitch * H * 4u);
	for (y = 0; y < H; y += (unsigned)step) {
		const unsigned int *row = base + (unsigned long)y * pitch;
		char *o = lbuf; int col = 0, k;
		k = snprintf(lbuf, sizeof lbuf, "%03u:", y / (unsigned)step); o += k;
		for (x = 0; x < W; x += (unsigned)step) {
			unsigned int px = row[x], p = (((px>>16&0xff)>>3)<<11)|(((px>>8&0xff)>>2)<<5)|((px&0xff)>>3);
			if (col == 13) { *o = 0; fastboot_info(lbuf); o = lbuf; o += snprintf(lbuf, sizeof lbuf, "%03u+", y/(unsigned)step); col = 0; }
			*o++ = hx[(p>>12)&0xf]; *o++ = hx[(p>>8)&0xf]; *o++ = hx[(p>>4)&0xf]; *o++ = hx[p&0xf]; col++;
		}
		*o = 0; fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem close: arm the capture ring then trigger the in-game close path, so the reverse
 * frames it plays are grabbed for inspection. arm=/rev= in diag show trigger status. */
static void cmd_close(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	g_cap_have = 0; g_cap_want = GBA_CAP_MAX;
	g_dbg_force_close = 1;
	thread_sleep(1200);
	snprintf(lbuf, sizeof lbuf, "cap %d", g_cap_have);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_launch(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	g_dbg_force_launch = 1;
	thread_sleep(300);
	fastboot_okay("");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem getframe:", cmd_getframe, 1, 0);
	fastboot_register("oem launch", cmd_launch, 1, 0);
	fastboot_register("oem close", cmd_close, 1, 0);
}
