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

static char lbuf[96];

/* oem menu-shot[:step] - dump the live front buffer downscaled (default /16) as RGB565
 * hex rows (bounded fastboot_info writes only, no bulk transfer), to eyeball a state
 * over USB. Reconstruct with tools/ayaneo/gba/fastboot_menu_shot.py. */
extern const unsigned int *ayaneo_canvas_front(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
static void cmd_shot(const char *arg, void *data, unsigned sz)
{
	static const char *hx = "0123456789abcdef";
	unsigned int pitch = 0, W = 0, H = 0, x, y;
	const unsigned int *fb = ayaneo_canvas_front(&pitch, &W, &H);
	int step = 16;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg >= '1' && *arg <= '9') step = *arg - '0';
	if (!fb || !W || !H) { fastboot_fail("no fb"); return; }
	for (y = 0; y < H; y += (unsigned)step) {
		const unsigned int *row = fb + (unsigned long)y * pitch;
		char *o = lbuf; int col = 0;
		o += snprintf(lbuf, sizeof lbuf, "%03u:", y / (unsigned)step);
		for (x = 0; x < W; x += (unsigned)step) {
			unsigned int px = row[x];
			unsigned int p = (((px>>16&0xff)>>3)<<11)|(((px>>8&0xff)>>2)<<5)|((px&0xff)>>3);
			if (col == 13) { *o = 0; fastboot_info(lbuf); o = lbuf;
					 o += snprintf(lbuf, sizeof lbuf, "%03u+", y/(unsigned)step); col = 0; }
			*o++ = hx[(p>>12)&0xf]; *o++ = hx[(p>>8)&0xf]; *o++ = hx[(p>>4)&0xf]; *o++ = hx[p&0xf]; col++;
		}
		*o = 0; fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem oy:<n|-9999> - freeze the slide at open_y=n (or -9999 to release) for capture. */
extern void gba_menu_dbg_oy(int v);
static void cmd_oy(const char *arg, void *data, unsigned sz)
{
	int v = 0, neg = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == '-') { neg = 1; arg++; }
	while (*arg >= '0' && *arg <= '9') { v = v * 10 + (*arg - '0'); arg++; }
	gba_menu_dbg_oy(neg ? -v : v);
	fastboot_okay("");
}

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "p=%u f=%u", g_dbg_peak_us, g_dbg_fps);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem launch - force-launch the focused ROM; oem nav:<L R U D A B S T [ ]> - inject one
 * clean nav press + report the peak render us for that movement (reset first) so the
 * flicker campaign can measure any state. g_dbg_force_launch when arg is empty. */
extern volatile int g_dbg_key, g_dbg_key_hold, g_dbg_peak_reset, g_dbg_force_launch;
static void cmd_nav(const char *arg, void *data, unsigned sz)
{
	static const char K[] = "LRUDABST[]";   /* codes 0..9 */
	int code = -1, i;
	char c;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	c = *arg;
	if (c == '!') { g_dbg_force_launch = 1; thread_sleep(300); fastboot_okay(""); return; }
	if (c >= 'a' && c <= 'z') c -= 32;
	for (i = 0; i < 10; i++) if (K[i] == c) { code = i; break; }
	if (code < 0) { fastboot_okay(""); return; }
	g_dbg_peak_reset = 1;
	g_dbg_key = code;
	g_dbg_key_hold = 7;
	thread_sleep(300);
	fastboot_okay("");   /* read the resulting peak via `oem diag` */
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem nav:", cmd_nav, 1, 0);
	fastboot_register("oem menu-shot", cmd_shot, 1, 0);
	fastboot_register("oem oy:", cmd_oy, 1, 0);
}
