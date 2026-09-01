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

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	{
		extern volatile unsigned int g_dbg_hz1000;
		extern volatile unsigned int g_dbg_boxart_ok, g_dbg_boxart_tot;
		extern volatile unsigned int g_dbg_emu_us;
		extern volatile unsigned int g_dbg_frame_ticks;
		extern volatile unsigned int g_dbg_preempt_fires;
		snprintf(lbuf, sizeof lbuf,
			 "p=%u f=%u hz1000=%u bx=%u/%u em=%u ft=%u pfire=%u",
			 g_dbg_peak_us, g_dbg_fps, g_dbg_hz1000,
			 g_dbg_boxart_ok, g_dbg_boxart_tot, g_dbg_emu_us,
			 g_dbg_frame_ticks, g_dbg_preempt_fires);
	}
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

/* oem preempt:<0..3> - set the run-ahead ("Preemptive Frames") depth live so the
 * latency mechanism can be exercised + measured (via oem diag) without the Pico
 * menu. Persists like the menu toggle. */
static void cmd_preempt(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_set_preempt_frames(int v);
	extern int ayaneo_get_preempt_frames(void);
	extern void ayaneo_settings_save(void);
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg >= '0' && *arg <= '9') {
		ayaneo_set_preempt_frames(*arg - '0');
		ayaneo_settings_save();
	}
	snprintf(lbuf, sizeof lbuf, "preempt=%d", ayaneo_get_preempt_frames());
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem pretest:<n> - force the preemptive edge (rewind+replay) path for the next
 * n frames to exercise it without in-game input; read pfire via oem diag. */
static void cmd_pretest(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_force_edge;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	{
		int n = 0;
		while (*arg >= '0' && *arg <= '9') { n = n * 10 + (*arg - '0'); arg++; }
		if (n <= 0) n = 120;
		g_dbg_force_edge = n;
	}
	fastboot_info("pretest armed");
	fastboot_okay("");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem nav:", cmd_nav, 1, 0);
	fastboot_register("oem preempt:", cmd_preempt, 1, 0);
	fastboot_register("oem pretest:", cmd_pretest, 1, 0);
}
