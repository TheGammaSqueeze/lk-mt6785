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
		extern volatile unsigned int g_dbg_asub_done, g_dbg_asub_frames;
		extern volatile int g_dbg_eff_pf, g_dbg_btn_smt;
		extern unsigned int ayaneo_get_cpu_mhz(void);
		extern volatile unsigned int g_dbg_blit_us;
		snprintf(lbuf, sizeof lbuf,
			 "hz1000=%u em=%u epf=%d mhz=%u smt=%d afr=%u",
			 g_dbg_hz1000, g_dbg_emu_us,
			 g_dbg_eff_pf, ayaneo_get_cpu_mhz(), g_dbg_btn_smt, g_dbg_asub_frames);
	}
	fastboot_info(lbuf);
	{
		/* menu render cost: last + peak snes_menu_render us and loop fps. Peak is the
		 * worst frame over ~2s (reset by `oem nav`), the number that breaks 60fps. */
		extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
		snprintf(lbuf, sizeof lbuf, "render_us=%u peak_us=%u fps=%u",
			 g_dbg_render_us, g_dbg_peak_us, g_dbg_fps);
		fastboot_info(lbuf);
	}
	{
		/* per-phase render breakdown (us): where the frame goes. */
		extern unsigned g_perf[8];
		snprintf(lbuf, sizeof lbuf, "perf_us: wp=%u chrome=%u carousel=%u filmstrip=%u rest=%u",
			 g_perf[0] / 13u, g_perf[1] / 13u, g_perf[2] / 13u, g_perf[3] / 13u, g_perf[4] / 13u);
		fastboot_info(lbuf);
	}
	{
		/* cold-start stage timings (ms since the SD gate started) - where the
		 * black-backlight time-to-first-BIOS-frame goes. */
		extern volatile unsigned int g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list;
		extern volatile unsigned int g_dbg_bt_coreload, g_dbg_bt_coreinit, g_dbg_bt_start, g_dbg_bt_frame1;
		snprintf(lbuf, sizeof lbuf,
			 "bt: mount=%u bios=%u list=%u coreload=%u coreinit=%u start=%u frame1=%u ms",
			 g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list,
			 g_dbg_bt_coreload, g_dbg_bt_coreinit, g_dbg_bt_start, g_dbg_bt_frame1);
		fastboot_info(lbuf);
	}
	{
		/* last SNES session: how far it got + why it exited. */
		extern volatile unsigned g_snes_dbg_stage, g_snes_dbg_romsz, g_snes_dbg_loadrc;
		extern volatile unsigned g_snes_dbg_frames, g_snes_dbg_w, g_snes_dbg_h, g_snes_dbg_exit;
		snprintf(lbuf, sizeof lbuf,
			 "snes: stage=%u romsz=%u loadrc=%u frames=%u dim=%ux%u exit=%u",
			 g_snes_dbg_stage, g_snes_dbg_romsz, g_snes_dbg_loadrc,
			 g_snes_dbg_frames, g_snes_dbg_w, g_snes_dbg_h, g_snes_dbg_exit);
		fastboot_info(lbuf);
	}
	{
		extern volatile unsigned g_snes_dbg_loaderr, g_snes_dbg_hdr0;
		extern volatile int g_snes_dbg_prc;
		extern volatile unsigned g_snes_dbg_pitch, g_snes_dbg_nz, g_snes_dbg_changed;
		extern volatile unsigned g_snes_dbg_audframes, g_snes_dbg_hz1000, g_snes_dbg_vfp;
		extern unsigned int ayaneo_dsi_get_vfp(void);
		snprintf(lbuf, sizeof lbuf, "snes-px: nz=%u chg=%u hz1000=%u snes_vfp=%u menu_vfp=%u",
			 g_snes_dbg_nz, g_snes_dbg_changed, g_snes_dbg_hz1000, g_snes_dbg_vfp,
			 ayaneo_dsi_get_vfp());
		fastboot_info(lbuf);
	}
	{
		extern volatile unsigned g_snes_dbg_ss_size, g_snes_dbg_ss_core, g_snes_dbg_ss_sd, g_snes_dbg_heapused, g_snes_dbg_revmap, g_snes_dbg_ss_fast;
		snprintf(lbuf, sizeof lbuf, "snes-ss: core=%u sd=%u fast=%u heap=%u revmap=%u",
			 g_snes_dbg_ss_core, g_snes_dbg_ss_sd, g_snes_dbg_ss_fast, g_snes_dbg_heapused, g_snes_dbg_revmap);
		fastboot_info(lbuf);
	}
	{	/* run-ahead correctness: ra_ok=1 -> render-skip is state-transparent (look-ahead accurate);
		 * ahead=1 -> the N-ahead frame differs from committed (genuinely looking ahead). */
		extern volatile unsigned g_snes_dbg_ra_ok, g_snes_dbg_ra_ahead, g_snes_dbg_ra_depth, g_snes_dbg_ra_raw;
		snprintf(lbuf, sizeof lbuf, "snes-ra: ok=%u ahead=%u raw=%u depth=%u",
			 g_snes_dbg_ra_ok, g_snes_dbg_ra_ahead, g_snes_dbg_ra_raw, g_snes_dbg_ra_depth);
		fastboot_info(lbuf);
	}
	{	/* clean display-independent cost breakdown (headless tight loops); fps = 1e6/us. */
		extern volatile unsigned g_snes_dbg_core_us, g_snes_dbg_render_us, g_snes_dbg_save_us, g_snes_dbg_load_us;
		unsigned cu = g_snes_dbg_core_us, ru = g_snes_dbg_render_us;
		snprintf(lbuf, sizeof lbuf, "snes-perf: core=%uus(%ufps) render=%uus(%ufps)",
			 cu, cu ? 1000000u / cu : 0, ru, ru ? 1000000u / ru : 0);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "snes-perf: save=%uus load=%uus",
			 g_snes_dbg_save_us, g_snes_dbg_load_us);
		fastboot_info(lbuf);
	}
	{
		extern volatile unsigned g_snes_dbg_benchfps, g_snes_show_us;
		extern volatile int g_snes_dbg_ra, g_snes_stretch;
		extern unsigned int ayaneo_get_cpu_mhz(void);
		extern unsigned ayaneo_get_snes_opts(void);
		snprintf(lbuf, sizeof lbuf, "snes-bench: fps=%u ra=%d mhz=%u show_us=%u stretch=%d",
			 g_snes_dbg_benchfps, g_snes_dbg_ra, ayaneo_get_cpu_mhz(), g_snes_show_us, g_snes_stretch);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "snes-opts: 0x%08x (audio byte=%u)",
			 ayaneo_get_snes_opts(), (ayaneo_get_snes_opts() >> 16) & 0xFF);
		fastboot_info(lbuf);
		{
			extern volatile unsigned g_snes_flush_us;
			snprintf(lbuf, sizeof lbuf, "snes-disp: show_us=%u flush_us=%u", g_snes_show_us, g_snes_flush_us);
			fastboot_info(lbuf);
		}
		{
			extern volatile unsigned g_rszdbg[12];
			snprintf(lbuf, sizeof lbuf, "rszdbg: 2l_roi=%08x 2l_src=%08x", g_rszdbg[0], g_rszdbg[1]);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "rszdbg: 2l_pitch=%08x 2l_srccon=%08x", g_rszdbg[2], g_rszdbg[3]);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "rszdbg: rsz_en=%08x ctrl1=%08x", g_rszdbg[4], g_rszdbg[5]);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "rszdbg: rsz_in=%08x rsz_out=%08x", g_rszdbg[6], g_rszdbg[7]);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "rszdbg: dw_dh=%08x xoff_yoff=%08x", g_rszdbg[8], g_rszdbg[9]);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "rszdbg: iw_ih=%08x wp_hp=%08x", g_rszdbg[10], g_rszdbg[11]);
			fastboot_info(lbuf);
		}
	}
	fastboot_okay("");
}

/* oem snes-probe - read the 20-byte blob header directly from boot_b at each core's
 * offset (gbc 0x01900000, gpSP 0x01C00000, snes 0x01E00000) and report partition_read's
 * return + the magic word. Runs on the fastboot thread (no game loop), so it safely tests
 * whether the SNES blob is readable at its offset without launching anything. snes magic
 * should be 0x31534e53 ("SNS1"). */
static void cmd_snes_probe(const char *arg, void *data, unsigned sz)
{
	extern long partition_read(const char *part, long long off, unsigned char *buf, unsigned long len);
	unsigned char b[20]; long r; unsigned m;
	(void)arg; (void)data; (void)sz;
	r = partition_read("boot_b", 0x01900000LL, b, 20); m = *(unsigned *)b;
	snprintf(lbuf, sizeof lbuf, "probe gbc  @0x01900000 rc=%ld m=0x%08x", r, m); fastboot_info(lbuf);
	r = partition_read("boot_b", 0x01C00000LL, b, 20); m = *(unsigned *)b;
	snprintf(lbuf, sizeof lbuf, "probe gpsp @0x01C00000 rc=%ld m=0x%08x", r, m); fastboot_info(lbuf);
	r = partition_read("boot_b", 0x01E00000LL, b, 20); m = *(unsigned *)b;
	snprintf(lbuf, sizeof lbuf, "probe snes @0x01E00000 rc=%ld m=0x%08x (want 0x31534e53)", r, m); fastboot_info(lbuf);
	/* actually load it (safe here: no game loop) and report which check passed/failed */
	{
		extern void *snes_core_load(void);   /* returns the exports table ptr, NULL on fail */
		extern volatile unsigned g_snes_dbg_loaderr;
		void *ex = snes_core_load();
		snprintf(lbuf, sizeof lbuf, "probe load: ex=%s loaderr=%u", ex ? "OK" : "NULL", g_snes_dbg_loaderr);
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem gen-probe - read the Genesis-Plus-GX blob header from boot_b @0x01A00000 (magic should be
 * 0x31474553 "SEG1"), then genesis_core_load() it (safe on the fastboot thread: the loader only
 * copies the image + calls the blob entry, no emulation) and report the exports magic + loaderr.
 * Validates the blob-in-boot_b + loader path WITHOUT needing a Genesis ROM on the card. */
static void cmd_gen_probe(const char *arg, void *data, unsigned sz)
{
	extern long partition_read(const char *part, long long off, unsigned char *buf, unsigned long len);
	unsigned char b[20]; long r; unsigned m;
	(void)arg; (void)data; (void)sz;
	r = partition_read("boot_b", 0x01A00000LL, b, 20); m = *(unsigned *)b;
	snprintf(lbuf, sizeof lbuf, "probe genesis @0x01A00000 rc=%ld m=0x%08x (want 0x31474553)", r, m);
	fastboot_info(lbuf);
	{
		extern const void *genesis_core_load(void);   /* returns exports ptr, NULL on fail */
		extern volatile unsigned g_gen_dbg_loaderr;
		const void *ex = genesis_core_load();
		snprintf(lbuf, sizeof lbuf, "probe load: ex=%s loaderr=%u", ex ? "OK" : "NULL", g_gen_dbg_loaderr);
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem gen-rw - report the Genesis rewind cost breakdown captured during the LAST rewind: the average
 * per-step state_load (portable retro_unserialize + system_reset) vs the c->run re-emulate cost, the
 * smoothed per-step total, the achieved reverse speed, and the CPU clock it was measured at. Tells us
 * whether the state restore or the frame re-emulation is the wall for rewind at a given clock. Rewind on
 * the device first (hold left trigger for a couple seconds), then run this. */
static void cmd_gen_rw(const char *arg, void *data, unsigned sz)
{
	extern volatile unsigned g_gen_dbg_rw_load_us, g_gen_dbg_rw_run_us, g_gen_dbg_rw_step_us,
				 g_gen_dbg_rw_eff_x10, g_gen_dbg_rw_mhz;
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "rewind @%uMHz: state_load=%uus run(re-emulate)=%uus step=%uus eff=%u.%ux",
		 g_gen_dbg_rw_mhz, g_gen_dbg_rw_load_us, g_gen_dbg_rw_run_us, g_gen_dbg_rw_step_us,
		 g_gen_dbg_rw_eff_x10 / 10u, g_gen_dbg_rw_eff_x10 % 10u);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem snes-launch[:N] - launch the FIRST SNES ROM for N frames (default 240) then return,
 * via the menu thread (arena-safe), and report the result. Safe way to validate the SNES
 * path over USB without navigating or risking a wrong non-SNES force-launch. Read the
 * outcome on the next `oem diag` (frames/dim/nz/changed). */
static void cmd_snes_launch(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_snes_launch;
	extern volatile unsigned g_snes_test_limit, g_snes_dbg_frames, g_snes_dbg_exit;
	unsigned n = 0, i;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (unsigned)(*arg++ - '0');
	if (n < 1) n = 240;
	g_snes_test_limit = n;
	g_dbg_snes_launch = 1;
	/* wait for the menu to pick it up + the session to run n frames + return (~n/50 s) */
	for (i = 0; i < 40 + n; i++) {
		thread_sleep(50);
		if (g_snes_dbg_exit != 0 && g_snes_test_limit == 0) break;
	}
	snprintf(lbuf, sizeof lbuf, "snes-launch: requested %u frames, ran=%u exit=%u", n, g_snes_dbg_frames, g_snes_dbg_exit);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem snes-bench[:N] - like snes-launch but forces UNCAPPED benchmark mode for N frames
 * (default 600) so we measure the real max emulation FPS, not the 60 Hz vsync cap. Combine
 * with oem snes-ra:N first to measure run-ahead depth cost. Read fps via oem diag snes-bench. */
static void cmd_snes_bench(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_snes_launch, g_snes_dbg_bench;
	extern volatile unsigned g_snes_test_limit, g_snes_dbg_frames, g_snes_dbg_exit, g_snes_dbg_benchfps;
	unsigned n = 0, i;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (unsigned)(*arg++ - '0');
	if (n < 1) n = 600;
	g_snes_dbg_benchfps = 0;
	g_snes_dbg_bench = 1;      /* session forces g_snes_benchmark; cleared on session exit */
	g_snes_test_limit = n;
	g_dbg_snes_launch = 1;
	/* uncapped, so it finishes far faster than n/50 s; generous ceiling still bounds the wait. */
	for (i = 0; i < 40 + n; i++) {
		thread_sleep(50);
		if (g_snes_dbg_exit != 0 && g_snes_test_limit == 0) break;
	}
	snprintf(lbuf, sizeof lbuf, "snes-bench: %u frames, ran=%u fps=%u exit=%u", n, g_snes_dbg_frames, g_snes_dbg_benchfps, g_snes_dbg_exit);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem snes-stretch:N - force the display aspect for measurement: 1 = Stretch (fill panel),
 * 0 = Pixel/aspect. Sets g_snes_stretch live so oem diag show_us can be compared per aspect
 * during a capped oem snes-launch (which presents every frame). */
static void cmd_snes_stretch(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_snes_dbg_stretch;   /* -1 = use persisted opt; 0/1 = force for the test */
	int n = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
	g_snes_dbg_stretch = n ? 1 : 0;
	snprintf(lbuf, sizeof lbuf, "snes-stretch: forced g_snes_dbg_stretch=%d for next snes-launch", g_snes_dbg_stretch);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem snes-rsz:N - toggle the EXPERIMENTAL hardware-resizer SNES display path (1 on, 0 off).
 * Default off = the proven CPU-blit path. Live-effective from the next presented frame. */
static void cmd_snes_rsz(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_snes_rsz;
	extern volatile unsigned g_snes_rsz_dbg, g_snes_show_us;
	int n = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
	g_snes_rsz = n ? 1 : 0;
	if (!g_snes_rsz) { extern void ayaneo_snes_rsz_restore(void); ayaneo_snes_rsz_restore(); }
	snprintf(lbuf, sizeof lbuf, "snes-rsz=%d rsz_en=0x%x show_us=%u", g_snes_rsz, g_snes_rsz_dbg, g_snes_show_us);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem ovltest:N - de-risk the hardware-menu-layer feature: composite a semi-transparent test
 * square as OVL0 L0 over the RUNNING game (1) or disable it (0). Validates OVL overlay + alpha. */
static void cmd_ovltest(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_overlay_test(int on);
	int n = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
	ayaneo_overlay_test(n ? 1 : 0);
	snprintf(lbuf, sizeof lbuf, "ovltest=%d (OVL0 L0 overlay %s)", n, n ? "on" : "off");
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem snes-ra:N - force run-ahead depth N (0..3) for the NEXT headless snes-launch, to
 * verify run-ahead does not leak/crash the bump arena (watch snes-ss heap=... stay flat). */
static void cmd_snes_ra(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_snes_dbg_ra;
	int n = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
	g_snes_dbg_ra = (n < 0) ? 0 : (n > 3 ? 3 : n);
	snprintf(lbuf, sizeof lbuf, "snes-ra: forced run-ahead depth=%d for next snes-launch", g_snes_dbg_ra);
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
	extern void ayaneo_menu_settings_persist(void);	/* eMMC + SD (SD-mode boot reads SD) */
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg >= '0' && *arg <= '9') {
		ayaneo_set_preempt_frames(*arg - '0');
		ayaneo_menu_settings_persist();
	}
	snprintf(lbuf, sizeof lbuf, "preempt=%d", ayaneo_get_preempt_frames());
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem selftest[:N] - run the run-ahead determinism self-test (device-blind validation
 * of the rewind path) at a frame boundary + report PASS/FAIL and the two compared state
 * hashes. N = number of committed frames to compare (default 1; a larger N, e.g. 60,
 * stress-tests ACCUMULATING drift across many consecutive rewinds). Runs a game first
 * (oem nav:! or the menu) so the emu loop is live; a no-op unless the loop is running. */
static void cmd_selftest(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_selftest_req, g_dbg_selftest_pass, g_dbg_selftest_cmp;
	extern volatile unsigned int g_dbg_selftest_rref, g_dbg_selftest_rtest;
	int i, n = 0, c;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
	if (n < 1) n = 1;
	if (n > 600) n = 600;					/* cap the muted-hitch length */
	g_dbg_selftest_pass = -1;
	g_dbg_selftest_req = n;
	/* allow ~50ms/frame * N plus margin for the loop to run it */
	for (i = 0; i < 40 + n * 4 && g_dbg_selftest_pass < 0; i++)
		thread_sleep(50);
	c = g_dbg_selftest_cmp;
	snprintf(lbuf, sizeof lbuf, "selftest[%d]=%s st=%d rng=%d scr=%d rref=%08x rtest=%08x", n,
		 g_dbg_selftest_pass < 0 ? "TIMEOUT" :
		 g_dbg_selftest_pass == 1 ? "PASS" :
		 g_dbg_selftest_pass == 2 ? "PASS(scr-transient)" : "FAIL",
		 (c & 1) ? 1 : 0, (c & 2) ? 1 : 0, (c & 4) ? 1 : 0,
		 g_dbg_selftest_rref, g_dbg_selftest_rtest);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Dump all 16 SoC AUXADC channels (mediatek,mt6768-auxadc @0x11001000) as calibrated values.
 * The LK device tree only declares digital inputs (gpio-keys: dpad/face/L2=key_lc/R2=key_rc/
 * JOYSTICK_L_SW/R_SW clicks), so the analog stick axes and analog trigger pressure - if wired to
 * the SoC AUXADC - are on unlabeled channels 3..15 (0/1 = thermistors, 2 = md-channel). Run this,
 * then push each stick to its extremes and squeeze each trigger, and watch which channel swings:
 * that reveals the axis->channel map with no kernel DTS needed. If NOTHING swings, the analog
 * inputs are behind an external I2C ADC/MCU (i2c@1401e000) instead, which needs a client driver. */
static void cmd_adcscan(const char *arg, void *data, unsigned sz)
{
	extern int iio_read_channel_processed(int channel, int *val);
	int ch, val, base;
	(void)arg; (void)data; (void)sz;
	for (base = 0; base < 16; base += 4) {
		int v0 = -1, v1 = -1, v2 = -1, v3 = -1;
		ch = base + 0; if (iio_read_channel_processed(ch, &val) == 0) v0 = val;
		ch = base + 1; if (iio_read_channel_processed(ch, &val) == 0) v1 = val;
		ch = base + 2; if (iio_read_channel_processed(ch, &val) == 0) v2 = val;
		ch = base + 3; if (iio_read_channel_processed(ch, &val) == 0) v3 = val;
		snprintf(lbuf, sizeof lbuf, "adc ch%02d=%d ch%02d=%d ch%02d=%d ch%02d=%d",
			 base, v0, base + 1, v1, base + 2, v2, base + 3, v3);
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* Power the analog stick rail the way the Android mtk-gamepad driver does, then read the four
 * stick axes. The DT (gamepad@48) maps io-channels lx/ly/rx/ry -> SoC AUXADC ch1/2/3/4, gated by
 * vcc3v3-supply = mt6360 LDO1 ("VFP", 3.3V) + enable-gpios = GPIO15. Both are off in the bootloader,
 * which is why a bare adcscan reads floor. Enable LDO1 (mt6360 reg 0x1b VOUT=0xd0=3.30V, reg 0x17
 * bit0x40 = enable - same driver LK uses for the SD LDOs), drive GPIO15 high, settle, then read
 * ch1-4. Run it, push each stick to its extremes, and the matching channel should swing (centre
 * ~2500, raw range ~1000..4500 per the driver calibration). */
static void cmd_stickscan(const char *arg, void *data, unsigned sz)
{
	extern int  iio_read_channel_processed(int channel, int *val);
	extern int  mt6360_ldo_config_interface(unsigned char addr, unsigned char data,
						unsigned char mask, unsigned char shift);
	extern int  mt_set_gpio_mode(unsigned pin, unsigned mode);
	extern int  mt_set_gpio_dir(unsigned pin, unsigned dir);
	extern int  mt_set_gpio_out(unsigned pin, unsigned out);
	extern void udelay(unsigned long usec);
	int v1 = -1, v2 = -1, v3 = -1, v4 = -1, val;
	(void)arg; (void)data; (void)sz;

	mt6360_ldo_config_interface(0x1b, 0xd0, 0xff, 0);   /* LDO1 (VFP) VOUT = 3.30V */
	mt6360_ldo_config_interface(0x17, 0x40, 0x40, 0);   /* LDO1 (VFP) enable */
	mt_set_gpio_mode(15, 0);                            /* GPIO_MODE_00 (GPIO function) */
	mt_set_gpio_dir(15, 1);                             /* GPIO_DIR_OUT */
	mt_set_gpio_out(15, 1);                             /* GPIO_OUT_ONE (enable active-high) */
	udelay(30000);                                     /* let the 3.3V rail + pots settle */

	if (iio_read_channel_processed(1, &val) == 0) v1 = val;
	if (iio_read_channel_processed(2, &val) == 0) v2 = val;
	if (iio_read_channel_processed(3, &val) == 0) v3 = val;
	if (iio_read_channel_processed(4, &val) == 0) v4 = val;
	snprintf(lbuf, sizeof lbuf, "stick lx=%d ly=%d rx=%d ry=%d (ch1-4, centre~2500)", v1, v2, v3, v4);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Read both analog triggers from the SGM58031 ADC (I2C6 @ 0x48) - LEFT = AIN0, RIGHT = AIN1,
 * per the reverse-engineered stock driver. Powers the rail (mt6360 LDO1 + GPIO15) first. Raw
 * 16-bit, ~13500 released -> ~16500 fully squeezed. Proves the LK I2C trigger path before wiring
 * L2/R2 into the cores (the digital L2/R2 microswitches never worked on this device). */
static void cmd_trigscan(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_joypad_power(void);
	extern int  ayaneo_joypad_trigger(int lr);
	int lt, rt;
	(void)arg; (void)data; (void)sz;
	ayaneo_joypad_power();
	lt = ayaneo_joypad_trigger(0);
	rt = ayaneo_joypad_trigger(1);
	snprintf(lbuf, sizeof lbuf, "trig LT=%d RT=%d (raw ~13500 rest .. ~16500 pressed)", lt, rt);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Validate the input MAPPING (auto-calibrated): left stick -> D-pad bitmask, and the two triggers
 * mapped to a 0..255 press level (RT=fast-forward, LT=rewind), 0 below 25% actuation ramping to 255
 * at 85% of range. Hold sticks centred + triggers released for the first run (that samples the rest
 * baseline), then move the stick / feather each trigger and re-run to see the dpad + levels. */
static void cmd_joytest(const char *arg, void *data, unsigned sz)
{
	extern void         ayaneo_joypad_calibrate(int force);
	extern void         ayaneo_joypad_poll(void);
	extern unsigned int ayaneo_joypad_dpad(void);
	extern int          ayaneo_joypad_ff_level(void);
	extern int          ayaneo_joypad_rewind_level(void);
	extern int          ayaneo_joypad_stick(int ch);
	extern void         udelay(unsigned long usec);
	unsigned int m;
	int ff, rw, lx, ly, i;
	(void)arg; (void)data; (void)sz;
	/* Prime the pipelined trigger read: poll a few times (both channels alternate, ~1 frame
	 * apart) so the caches reflect the current press before we read them. */
	for (i = 0; i < 6; i++) { ayaneo_joypad_poll(); udelay(3000); }
	m  = ayaneo_joypad_dpad();
	ff = ayaneo_joypad_ff_level();
	rw = ayaneo_joypad_rewind_level();
	lx = ayaneo_joypad_stick(1);
	ly = ayaneo_joypad_stick(2);
	snprintf(lbuf, sizeof lbuf, "joy dpad=%c%c%c%c lx=%d ly=%d  FF(RT)=%d  REW(LT)=%d",
		 (m & 0x01) ? 'U' : '-', (m & 0x02) ? 'D' : '-',
		 (m & 0x04) ? 'L' : '-', (m & 0x08) ? 'R' : '-', lx, ly, ff, rw);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Dump the physical DRAM map (ranks + mblock usable + reserved regions with names) so we can pick
 * a safe high-DRAM region to claim for the rewind ring buffer. */
static void cmd_meminfo(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_meminfo_dump(void (*emit)(const char *));
	(void)arg; (void)data; (void)sz;
	ayaneo_meminfo_dump(fastboot_info);
	fastboot_okay("");
}

/* Validate the rewind buffer: dynamically find + MMU-map the high-DRAM region, write a
 * position-dependent pattern across the WHOLE region and read it back. Proves the mapping (and the
 * region choice) in isolation before any gameplay wiring - a bad map would fault or mismatch here. */
static void cmd_rewindtest(const char *arg, void *data, unsigned sz)
{
	extern unsigned int ayaneo_rewind_phys(void);
	extern unsigned int ayaneo_rewind_segs(void);
	extern int ayaneo_rewind_selftest(unsigned int *region_out, unsigned int *tested_out, unsigned int *bad_out);
	unsigned int region = 0, tested = 0, bad = 0;
	int rc;
	(void)arg; (void)data; (void)sz;
	rc = ayaneo_rewind_selftest(&region, &tested, &bad);
	snprintf(lbuf, sizeof lbuf, "rewind rc=%d bad=%u tested=%u %uMB/%useg @0x%08x",
		 rc, bad, tested, region >> 20, ayaneo_rewind_segs(), ayaneo_rewind_phys());
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Validate the rewind RING LOGIC (no core): push more slots than fit to exercise wraparound, then
 * rewind the whole history and verify the markers come back newest->oldest. Proves push/overwrite +
 * cursor-walk + resume-head bookkeeping in isolation before wiring per-core capture. */
static void cmd_rewindring(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_rewind_ring_selftest(unsigned int *slots_out, unsigned int *pushed_out, unsigned int *bad_out);
	unsigned int slots = 0, pushed = 0, bad = 0;
	int rc;
	(void)arg; (void)data; (void)sz;
	rc = ayaneo_rewind_ring_selftest(&slots, &pushed, &bad);
	snprintf(lbuf, sizeof lbuf, "rewindring rc=%d bad=%u slots=%u pushed=%u", rc, bad, slots, pushed);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Report the live delta-ring stats for the LAST game session (still resident until the next game
 * arms the ring): frames retained (=> seconds of rewind at 60fps), the average delta size (how well
 * that game's states compress), and the raw state size. Run from the carousel right after exiting a
 * game to see how long that core's rewind window actually is. */
static void cmd_rewindstat(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_rewind_stat(unsigned int *records, unsigned int *used, unsigned int *arena, unsigned int *state);
	unsigned int rec = 0, used = 0, arena = 0, state = 0, avg, secs;
	(void)arg; (void)data; (void)sz;
	ayaneo_rewind_stat(&rec, &used, &arena, &state);
	avg  = rec ? (used / rec) : 0u;
	secs = rec / 60u;
	snprintf(lbuf, sizeof lbuf, "rewindstat frames=%u win=%us avgdelta=%uB raw=%uKB used=%uMB/%uMB",
		 rec, secs, avg, state >> 10, used >> 20, arena >> 20);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Validate that persisting the GBA/GBC aspect (packed into the free bits of the b+24 filter word)
 * round-trips through serialize/deserialize without disturbing the per-core LCD filters. */
static void cmd_settingsrt(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_settings_aspect_selftest(void);
	int fail;
	(void)arg; (void)data; (void)sz;
	fail = ayaneo_settings_aspect_selftest();
	snprintf(lbuf, sizeof lbuf, "settingsrt %s failmask=0x%x", fail ? "FAIL" : "OK", (unsigned)fail);
	fastboot_info(lbuf);
	fastboot_okay("");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem meminfo", cmd_meminfo, 1, 0);
	fastboot_register("oem rewindtest", cmd_rewindtest, 1, 0);
	fastboot_register("oem rewindring", cmd_rewindring, 1, 0);
	fastboot_register("oem rewindstat", cmd_rewindstat, 1, 0);
	fastboot_register("oem settingsrt", cmd_settingsrt, 1, 0);
	fastboot_register("oem adcscan", cmd_adcscan, 1, 0);
	fastboot_register("oem stickscan", cmd_stickscan, 1, 0);
	fastboot_register("oem trigscan", cmd_trigscan, 1, 0);
	fastboot_register("oem joytest", cmd_joytest, 1, 0);
	fastboot_register("oem snes-probe", cmd_snes_probe, 1, 0);
	fastboot_register("oem gen-probe", cmd_gen_probe, 1, 0);
	fastboot_register("oem gen-rw", cmd_gen_rw, 1, 0);
	fastboot_register("oem snes-launch", cmd_snes_launch, 1, 0);
	fastboot_register("oem snes-bench", cmd_snes_bench, 1, 0);
	fastboot_register("oem snes-stretch", cmd_snes_stretch, 1, 0);
	fastboot_register("oem snes-rsz", cmd_snes_rsz, 1, 0);
	fastboot_register("oem ovltest", cmd_ovltest, 1, 0);
	fastboot_register("oem snes-ra", cmd_snes_ra, 1, 0);
	fastboot_register("oem nav:", cmd_nav, 1, 0);
	fastboot_register("oem preempt:", cmd_preempt, 1, 0);
	fastboot_register("oem selftest", cmd_selftest, 1, 0);
}
