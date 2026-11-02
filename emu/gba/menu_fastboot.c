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
		/* AYANEO dynamic refresh: live panel rate + vfp, read register-direct so it reflects ANY
		 * running core (genesis/snes/gba/gbc/menu) and a real-time region switch. Sample this before
		 * and after a Genesis Region toggle to prove the panel follows the core fps. mHz/1000 = Hz. */
		extern unsigned int ayaneo_dsi_refresh_milli(void);
		extern unsigned int ayaneo_dsi_get_vfp(void);
		unsigned int rm = ayaneo_dsi_refresh_milli();
		{
			static unsigned int diag_seq;
			snprintf(lbuf, sizeof lbuf, "diag: seq=%u", ++diag_seq);   /* stale-reply detector */
			fastboot_info(lbuf);
		}
		snprintf(lbuf, sizeof lbuf, "panel: refresh_mHz=%u (%u.%02uHz) vfp=%u",
			 rm, rm / 1000u, (rm % 1000u) / 10u, ayaneo_dsi_get_vfp());
		fastboot_info(lbuf);
	}
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
			 "hz1000=%u em=%u epf=%d mhz=%u ticks=%u afr=%u",
			 g_dbg_hz1000, g_dbg_emu_us,
			 g_dbg_eff_pf, ayaneo_get_cpu_mhz(), g_dbg_frame_ticks, g_dbg_asub_frames);
		(void)g_dbg_btn_smt;
	}
	fastboot_info(lbuf);
	{
		/* menu render cost: last + peak snes_menu_render us and loop fps. Peak is the
		 * worst frame over ~2s (reset by `oem nav`), the number that breaks 60fps. */
		extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
		{
			extern volatile unsigned int g_dbg_present_us[2];
			extern volatile unsigned int g_dbg_pace_waits, g_dbg_pace_dt_us, g_dbg_pace_timeouts, g_dbg_pace_edge_us;
			snprintf(lbuf, sizeof lbuf, "present_us: fused=%u pace_waits=%u", g_dbg_present_us[0], g_dbg_pace_waits);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "pace: dt=%uus edge=%uus timeouts=%u", g_dbg_pace_dt_us, g_dbg_pace_edge_us, g_dbg_pace_timeouts);
			fastboot_info(lbuf);
			{
				extern volatile unsigned int g_dbg_present_fps, g_dbg_emu_fps, g_dbg_emu_frame_us, g_dbg_emu_total;
				extern unsigned int gpt4_get_current_tick(void);
				{ extern volatile unsigned int g_dbg_loop_us, g_dbg_apace_us, g_dbg_apace_fill; extern volatile unsigned g_rsz_show_us;
				  extern int ayaneo_get_lcd_filter_core(int c); extern unsigned ayaneo_core_clock(int core, unsigned t, unsigned t0); extern unsigned ayaneo_cpu_manual_get(int core);
				  snprintf(lbuf, sizeof lbuf, "loop: us=%u apace=%uus fill=%u show=%uus filt=%d clk=%u man=%u", g_dbg_loop_us, g_dbg_apace_us, g_dbg_apace_fill, g_rsz_show_us,
					   ayaneo_get_lcd_filter_core(1), ayaneo_core_clock(0, 1000u, 600u), ayaneo_cpu_manual_get(0));
				  fastboot_info(lbuf); }
				snprintf(lbuf, sizeof lbuf, "speed: presents/s=%u emu_frames/s=%u frame_us=%u",
					 g_dbg_present_fps, g_dbg_emu_fps, g_dbg_emu_frame_us);
				fastboot_info(lbuf);
				/* two reads a few seconds apart: d(total) / d(ms) * 1000 = true emulated fps */
				snprintf(lbuf, sizeof lbuf, "clock: total_frames=%u now_ms=%u",
					 g_dbg_emu_total, gpt4_get_current_tick() / 13000u);
				fastboot_info(lbuf);
				{
					extern volatile unsigned int g_dbg_bt_frame1, g_dbg_bt_intro_end, g_dbg_bt_intro_frames;
					snprintf(lbuf, sizeof lbuf, "intro: first=%ums end=%ums frames=%u (4520ms nominal)",
						 g_dbg_bt_frame1, g_dbg_bt_intro_end, g_dbg_bt_intro_frames);
					fastboot_info(lbuf);
				}
			}
		}
		snprintf(lbuf, sizeof lbuf, "render_us=%u peak_us=%u fps=%u",
			 g_dbg_render_us, g_dbg_peak_us, g_dbg_fps);
		fastboot_info(lbuf);
	}
	{
		/* per-phase render breakdown (us): where the frame goes. */
		extern unsigned g_perf[8];
		extern unsigned g_perf_xf[2];
		snprintf(lbuf, sizeof lbuf, "perf_us: wp=%u chrome=%u carousel=%u",
			 g_perf[0] / 13u, g_perf[1] / 13u, g_perf[2] / 13u);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "perf_us: filmstrip=%u rest=%u tiles dark=%u active=%u",
			 g_perf[3] / 13u, g_perf[4] / 13u, g_perf_xf[0], g_perf_xf[1]);
		fastboot_info(lbuf);
	}
	{
		/* frame drops: menu loop periods over 20 ms since the last peak reset (oem nav), and
		 * whether the overlay really showed the buffer presented one frame earlier. */
		extern volatile unsigned int g_dbg_menu_frames, g_dbg_menu_long, g_dbg_latch_hit, g_dbg_latch_miss;
		snprintf(lbuf, sizeof lbuf, "drops: frames=%u long=%u latch hit=%u miss=%u",
			 g_dbg_menu_frames, g_dbg_menu_long, g_dbg_latch_hit, g_dbg_latch_miss);
		fastboot_info(lbuf);
	}
	{
		/* cold-start stage timings (ms since the SD gate started) - where the
		 * black-backlight time-to-first-BIOS-frame goes. */
		extern volatile unsigned int g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list;
		extern volatile unsigned int g_dbg_bt_coreload, g_dbg_bt_coreinit, g_dbg_bt_start, g_dbg_bt_frame1;
		snprintf(lbuf, sizeof lbuf,
			 "bt: mount=%u bios=%u list=%u core=%u/%u",
			 g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list, g_dbg_bt_coreload, g_dbg_bt_coreinit);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "bt: start=%u frame1=%u ms", g_dbg_bt_start, g_dbg_bt_frame1);
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

/* oem gen-bench[:N] - headless Genesis core throughput benchmark: find the first ROM in /roms/genesis
 * (Sonic), load it, run N (default 300) frames UNCAPPED at a fixed-safe 1400 MHz on the genesis thread,
 * and report per-frame emu us / fps / state save+load us / an implied rewind speed. This is the stable
 * metric an autonomous optimization loop pushes (raw core speed, no display/vsync). Run it at the menu. */
static void cmd_gen_bench(const char *arg, void *data, unsigned sz)
{
	extern void *gba_sd_menu_vol(void);                 /* opaque fat_vol* (no sd_fat.h here) */
	extern const void *gba_sd_first_genesis_rom(void);  /* opaque gba_rom_entry* */
	extern void genesis_sd_bench(void *vol, const void *rom, int frames);
	extern int atoi(const char *);
	extern volatile unsigned g_gen_dbg_bench_fps, g_gen_dbg_bench_us, g_gen_dbg_bench_save_us,
				 g_gen_dbg_bench_load_us, g_gen_dbg_bench_mhz, g_gen_dbg_bench_rwx10,
				 g_gen_dbg_bench_loadfast_us, g_gen_dbg_bench_norender_us,
				 g_gen_dbg_bench_rw6_us, g_gen_dbg_bench_rwok, g_gen_dbg_bench_crc;
	void *vol = gba_sd_menu_vol();
	const void *rom = gba_sd_first_genesis_rom();
	int frames = 300;
	(void)data; (void)sz;
	if (arg && arg[0] == ':') { int v = atoi(arg + 1); if (v >= 30) frames = v; }
	if (!vol) { fastboot_info("gen-bench: no SD volume (be at the ROM menu)"); fastboot_okay(""); return; }
	if (!rom) { fastboot_info("gen-bench: no Genesis ROM found in /roms/genesis"); fastboot_okay(""); return; }
	snprintf(lbuf, sizeof lbuf, "gen-bench: %d frames uncapped...", frames);
	fastboot_info(lbuf);
	genesis_sd_bench(vol, rom, frames);
	snprintf(lbuf, sizeof lbuf, "gen-bench @%uMHz: fps=%u frame=%uus norender=%uus",
		 g_gen_dbg_bench_mhz, g_gen_dbg_bench_fps, g_gen_dbg_bench_us, g_gen_dbg_bench_norender_us);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "  frame-crc=%08x (render byte-identity check)", g_gen_dbg_bench_crc);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "  save=%uus load=%uus loadfast=%uus impliedRW=%u.%ux",
		 g_gen_dbg_bench_save_us, g_gen_dbg_bench_load_us, g_gen_dbg_bench_loadfast_us,
		 g_gen_dbg_bench_rwx10 / 10u, g_gen_dbg_bench_rwx10 % 10u);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "  rewind6x: cost=%uus/present ok=%u (fits 16667us=6x smooth)",
		 g_gen_dbg_bench_rw6_us, g_gen_dbg_bench_rwok);
	fastboot_info(lbuf);
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
	/* optional :name picks the first SNES ROM whose name contains it (case insensitive), e.g.
	 * snes-launch:600:mario for the PAL Super Mario World (E); default = the first SNES ROM */
	{
		extern char g_dbg_snes_name[32];
		unsigned k = 0;
		while (*arg == ' ' || *arg == ':') arg++;
		while (*arg && *arg != ' ' && k + 1 < sizeof g_dbg_snes_name) g_dbg_snes_name[k++] = *arg++;
		g_dbg_snes_name[k] = 0;
	}
	g_snes_test_limit = n;
	g_dbg_snes_launch = 1;
	/* wait for the menu to pick it up + the session to run n frames + return (~n/50 s) */
	{
		extern unsigned int gpt4_get_current_tick(void);
		extern volatile unsigned g_snes_dbg_stage;
		unsigned t0 = 0, t1 = 0;
		for (i = 0; i < 40 + n; i++) {
			thread_sleep(50);
			if (!t0 && g_snes_dbg_stage >= 4) t0 = gpt4_get_current_tick();   /* first frames running */
			if (g_snes_dbg_exit != 0 && g_snes_test_limit == 0) { t1 = gpt4_get_current_tick(); break; }
		}
		snprintf(lbuf, sizeof lbuf, "snes-launch: req=%u ran=%u exit=%u", n, g_snes_dbg_frames, g_snes_dbg_exit);
		fastboot_info(lbuf);
		{
			extern const char *g_dbg_snes_chosen;
			extern volatile unsigned g_snes_dbg_fps_milli;
			const char *nm = g_dbg_snes_chosen ? g_dbg_snes_chosen : "?";
			while (*nm == ' ') nm++;
			snprintf(lbuf, sizeof lbuf, "snes-launch: fps_milli=%u run_ms=%u", g_snes_dbg_fps_milli, (t0 && t1) ? (t1 - t0) / 13000u : 0u);
			fastboot_info(lbuf);
			snprintf(lbuf, sizeof lbuf, "snes-launch: rom=%.44s", nm);
		}
	}
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
static unsigned fb_dec(const char **pp);
/* oem igm - toggle the GBA in-game (Pico) menu, so it can be screenshotted with oem shot. */
static void cmd_igm(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_igm_toggle;
	(void)arg; (void)data; (void)sz;
	g_dbg_igm_toggle = 1;
	thread_sleep(200);
	fastboot_okay("");
}
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
	/* nav:R:600 holds the key for 600 frames (10 s) so a sustained scroll can be measured
	 * with oem diag while it runs; the default 7 is one tap. */
	arg++;
	while (*arg == ' ' || *arg == ':') arg++;
	{ unsigned hold = fb_dec(&arg); if (hold < 1u || hold > 6000u) hold = 7u; g_dbg_key_hold = (int)hold; }
	g_dbg_peak_reset = 1;
	g_dbg_key = code;
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

/* ---- live panel-register poke (st7703 tuning harness) ------------------------
 * Send a DCS write to the LIVE panel so drive registers (VCOM, gamma, charge
 * pump, ...) can be swept BY EYE with no edit/rebuild/reflash cycle. Uses the
 * exact path the init table uses (DSI_set_cmdq_V2 -> DSI0): select the register
 * PAGE (cmd 0xEE), then write REG=VAL. All values are HEX (no 0x prefix).
 *   fastboot oem lcmwr:1,28,20     page 0x01, reg 0x28 (VCOM low) = 0x20
 *   fastboot oem vcom:1F,29,63     page01 0x28/0x29/0x2A = lo,hi,off in one shot
 * DEBUG/tuning only: the poke shares the DSI with the present thread, so an
 * occasional glitch is possible; power-cycle to restore the init-table values.
 * Bake a winning value into ST7703_* / the init table in st7703_hd720_dsi_vdo.c. */
extern void DSI_set_cmdq_V2_Wrapper_DSI0(unsigned cmd, unsigned char count,
					 unsigned char *para_list, unsigned char force_update);

static unsigned fb_hex(const char **pp)
{
	const char *p = *pp;
	unsigned v = 0;
	while (*p == ' ' || *p == ',' || *p == ':') p++;
	for (;;) {
		char c = *p;
		unsigned d;
		if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
		else break;
		v = (v << 4) | d;
		p++;
	}
	*pp = p;
	return v;
}

static void lcm_page_write(unsigned page, unsigned reg, unsigned val)
{
	unsigned char b[1];
	b[0] = (unsigned char)page; DSI_set_cmdq_V2_Wrapper_DSI0(0xEE, 1, b, 1);   /* select page */
	b[0] = (unsigned char)val;  DSI_set_cmdq_V2_Wrapper_DSI0((unsigned char)reg, 1, b, 1);
}

static void cmd_lcmwr(const char *arg, void *data, unsigned sz)
{
	unsigned page, reg, val;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	page = fb_hex(&arg); reg = fb_hex(&arg); val = fb_hex(&arg);
	lcm_page_write(page, reg, val);
	snprintf(lbuf, sizeof lbuf, "lcmwr: page 0x%02x reg 0x%02x = 0x%02x",
		 page & 0xff, reg & 0xff, val & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* VCOM convenience: page01 0x28/0x29/0x2A = lo,hi,off (stock 0x1F,0x29,0x63). */
/* oem dsiw:REG[,b0,b1,...|,stock] - Konkr ST7123 live register write (hex). The vendor pages
 * are unlocked with the stock 0x60 71 23 A2/A3/A4 sequence and locked again with 0x60 71 23 00
 * around the write, as the init table does. With no bytes the stock table bytes are shown;
 * "stock" writes them back. The panel keeps the value until the next reset, nothing persists. */
static int s_dsiw_fail;
static void dsi_write(unsigned char reg, unsigned char *b, unsigned n)
{
	extern int ayaneo_dsi_vm_cmd(unsigned char cmd, unsigned int n, const unsigned char *p);
	unsigned char u[3] = { 0x71, 0x23, 0xA2 };
	s_dsiw_fail = 0;
	if (reg < 0xB0 && reg != 0x60) {   /* a standard DCS command needs no vendor unlock */
		s_dsiw_fail += ayaneo_dsi_vm_cmd(reg, n, b) != 0;
		return;
	}
	s_dsiw_fail += ayaneo_dsi_vm_cmd(0x60, 3, u) != 0; u[2] = 0xA3;
	s_dsiw_fail += ayaneo_dsi_vm_cmd(0x60, 3, u) != 0; u[2] = 0xA4;
	s_dsiw_fail += ayaneo_dsi_vm_cmd(0x60, 3, u) != 0;
	s_dsiw_fail += ayaneo_dsi_vm_cmd(reg, n, b) != 0;
	u[2] = 0x00;
	s_dsiw_fail += ayaneo_dsi_vm_cmd(0x60, 3, u) != 0;
}
static void cmd_dsiw(const char *arg, void *data, unsigned sz)
{
	extern unsigned int st7123_stock_reg(unsigned char reg, unsigned char *out, unsigned int max);
	unsigned char b[64], stock[64]; unsigned n = 0, ns, reg, i; int p;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	reg = fb_hex(&arg);
	ns = st7123_stock_reg((unsigned char)reg, stock, sizeof stock);
	while (*arg == ' ' || *arg == ',') arg++;
	if (arg[0] == 's') { memcpy(b, stock, ns); n = ns; }
	else while (*arg && n < sizeof b) { b[n++] = (unsigned char)fb_hex(&arg); while (*arg == ' ' || *arg == ',') arg++; }
	if (n) dsi_write((unsigned char)reg, b, n);
	if (n > 15) { fastboot_fail("at most 15 bytes in video mode"); return; }
	p = snprintf(lbuf, sizeof lbuf, "dsiw %02x %s%s:", reg & 0xff, n ? "wrote" : "stock", (n && s_dsiw_fail) ? " (TIMEOUT)" : "");
	for (i = 0; i < (n ? n : ns) && p < (int)sizeof lbuf - 4; i++) p += snprintf(lbuf + p, sizeof lbuf - p, " %02x", n ? b[i] : stock[i]);
	fastboot_info(lbuf);
	if (n && ns) {
		p = snprintf(lbuf, sizeof lbuf, "stock:");
		for (i = 0; i < ns && p < (int)sizeof lbuf - 4; i++) p += snprintf(lbuf + p, sizeof lbuf - p, " %02x", stock[i]);
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem gray:LEVEL - flat gray field (0..255) over the game, 0 clears; oem gray:rgb,RRGGBB for a colour */
static void cmd_gray(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_test_fill(unsigned int argb, int motion);
	unsigned v, argb; int motion = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (arg[0] == 'r') { while (*arg && *arg != ',') arg++; v = fb_hex(&arg); argb = 0xFF000000u | (v & 0xFFFFFFu); }
	else { v = fb_dec(&arg); if (v > 255) v = 255; argb = v ? (0xFF000000u | (v << 16) | (v << 8) | v) : 0u; }
	while (*arg && *arg != ',') arg++;
	if (*arg == ',' && arg[1] == 'm') motion = 1;   /* oem gray:LEVEL,m - the moving square */
	ayaneo_test_fill(argb, motion);
	snprintf(lbuf, sizeof lbuf, "gray: %08x %s%s", argb, argb ? "shown (in game only)" : "cleared", motion ? " + moving square" : "");
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_vcom(const char *arg, void *data, unsigned sz)
{
	unsigned lo, hi, off;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	lo = fb_hex(&arg); hi = fb_hex(&arg); off = fb_hex(&arg);
	lcm_page_write(0x01, 0x28, lo);
	lcm_page_write(0x01, 0x29, hi);
	lcm_page_write(0x01, 0x2A, off);
	snprintf(lbuf, sizeof lbuf, "vcom: 0x28=0x%02x 0x29=0x%02x 0x2A=0x%02x",
		 lo & 0xff, hi & 0xff, off & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Backlight-strobe experiment (motion-blur test): oem strobe:DIV,DUTY (DECIMAL, 0..1023).
 * DIV cranks the DISP_PWM carrier down toward the frame rate (bigger = slower; sweep
 * ~150..900 to find where a strobe/flicker appears). DUTY is the ON level out of 1024
 * (brightness of the lit phase; lower = shorter hold = less blur but dimmer). DIV=0 =
 * normal dimming at DUTY, e.g. strobe:0,800 restores a normal backlight. Reboot also
 * restores. Free-running (not vsync-locked yet), so expect a slow rolling brightness band. */
extern void disp_pwm_strobe(unsigned int div, unsigned int duty);

static unsigned fb_dec(const char **pp)
{
	const char *p = *pp;
	unsigned v = 0;
	while (*p == ' ' || *p == ',' || *p == ':') p++;
	while (*p >= '0' && *p <= '9') { v = v * 10u + (unsigned)(*p - '0'); p++; }
	*pp = p;
	return v;
}

static void cmd_strobe(const char *arg, void *data, unsigned sz)
{
	unsigned div, duty;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	div = fb_dec(&arg);
	duty = fb_dec(&arg);
	disp_pwm_strobe(div, duty);
	snprintf(lbuf, sizeof lbuf, "strobe: div=%u duty=%u/1024", div, duty);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Vsync-locked strobe: oem strobelk:DIV,DUTY (DECIMAL). One pulse per frame, phase-locked
 * to FRAME_DONE (kills the free-running double-image). DIV sets the period ~ 1 frame (start
 * near ~420 and tune), DUTY the ON fraction of 1024. strobelk:0,<bright> disables + restores. */
extern void disp_pwm_strobe_locked(unsigned int div, unsigned int duty);

static void cmd_strobelk(const char *arg, void *data, unsigned sz)
{
	unsigned div, duty;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	div = fb_dec(&arg);
	duty = fb_dec(&arg);
	disp_pwm_strobe_locked(div, duty);
	snprintf(lbuf, sizeof lbuf, "strobelk: div=%u duty=%u/1024 (vsync-locked)", div, duty);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Software overdrive (motion-trail RTC): oem od:ON,KUP,KDN,THR,CAP (DECIMAL). ON=1 enable / 0 off.
 * KUP/KDN = overshoot for rising / falling transitions in 1/16ths of the delta (KDN omitted -> = KUP,
 * i.e. symmetric). THR = skip overshoot for |delta| <= THR (kills small-change halos). CAP = max
 * |overshoot| per 5-bit channel, 0 = uncapped (bounds the high-contrast halo). Drives each changed
 * pixel past its target so the slow LC settles within the frame. Live-tunable, zero DC.
 * e.g. od:1,3 (symmetric 3), od:1,4,2,1 (rise 4, fall 2, threshold 1), od:1,5,4,2,6 (+cap 6). */
extern void ayaneo_set_overdrive(int on, int kup, int kdn, int thr, int cap);

static void cmd_od(const char *arg, void *data, unsigned sz)
{
	unsigned on, kup, kdn, thr, cap;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	on = fb_dec(&arg);
	kup = fb_dec(&arg);
	kdn = fb_dec(&arg); if (kdn == 0) kdn = kup;   /* omitted -> symmetric */
	thr = fb_dec(&arg);
	cap = fb_dec(&arg);                            /* omitted -> 0 = uncapped */
	ayaneo_set_overdrive((int)on, (int)kup, (int)kdn, (int)thr, (int)cap);
	snprintf(lbuf, sizeof lbuf, "od: %s kup=%u kdn=%u thr=%u cap=%u", on ? "on" : "off", kup, kdn, thr, cap);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Prev-indexed rising-gain curve: oem odup:KUPHI,KUPLO,GAM (DECIMAL). KUPHI = rising gain (/16) at
 * prev=BLACK (dark-origin smear killer), KUPLO = rising gain at prev=WHITE (halo-free floor), GAM =
 * falloff shape (16 linear, higher collapses the boost toward the darkest origins sooner). Leaves
 * ON/KDN/THR/CAP as-is; a preceding `oem od:` flattens the curve, this restores it. */
extern void ayaneo_set_overdrive_curve(int kuphi, int kuplo, int gam);
/* oem odcap:UP,DN,HEAD (decimal) - rising cap, falling cap, rail headroom (0..3 levels) */
static void cmd_odcap(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_set_overdrive_caps(int up, int dn, int head);
	extern void ayaneo_get_overdrive_caps(int *up, int *dn, int *head);
	int up, dn, head;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg) { up = (int)fb_dec(&arg); dn = (int)fb_dec(&arg); head = (int)fb_dec(&arg); ayaneo_set_overdrive_caps(up, dn, head); }
	ayaneo_get_overdrive_caps(&up, &dn, &head);
	snprintf(lbuf, sizeof lbuf, "odcap: up=%d dn=%d head=%d", up, dn, head);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_odup(const char *arg, void *data, unsigned sz)
{
	unsigned kuphi, kuplo, gam;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	kuphi = fb_dec(&arg);
	kuplo = fb_dec(&arg);
	gam = fb_dec(&arg); if (gam == 0) gam = 16;   /* omitted -> linear */
	ayaneo_set_overdrive_curve((int)kuphi, (int)kuplo, (int)gam);
	snprintf(lbuf, sizeof lbuf, "odup: kuphi=%u kuplo=%u gam=%u", kuphi, kuplo, gam);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* Konkr: flip the quarter-turn direction of the rotation layer live (oem rot:0 clockwise, oem rot:1
 * counter clockwise) so the right one can be picked on the device without a reflash. Takes effect
 * on the next presented frame. On the pairmini the layer is the identity and this is inert. */
extern volatile int g_rot_ccw;
static void cmd_rot(const char *arg, void *data, unsigned sz)
{
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	g_rot_ccw = fb_dec(&arg) ? 1 : 0;
	snprintf(lbuf, sizeof lbuf, "rot: %s", g_rot_ccw ? "ccw" : "cw");
	fastboot_info(lbuf);
	fastboot_okay("");
}

#ifdef KONKR_KPA
/* Konkr panel bring-up: `oem lcm` reports the glass ID pins (raw and as read at init), the live
 * DSI porch and the frame buffer geometry; `oem lcminit:N` re-runs the panel reset + init with
 * table N (0 WEIHUI, 1 HONGZHAN) regardless of the ID pins; `oem gpio:N` reads one GPIO. */
extern unsigned int st7123_read_id_raw(void);
extern void st7123_force_init(int which);
extern volatile unsigned int g_st7123_init_id;
extern unsigned int ayaneo_dsi_get_vfp(void);
extern unsigned int ayaneo_screen_w(void), ayaneo_screen_h(void);

static void cmd_lcm(const char *arg, void *data, unsigned sz)
{
	unsigned raw = st7123_read_id_raw();
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "lcm: id1=%u id2=%u init_pick=%d (0 weihui 1 hongzhan -1 none)",
		 (raw >> 1) & 1u, raw & 1u, (int)g_st7123_init_id);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "lcm: dsi vfp=%u logical=%ux%u rot_ccw=%d",
		 ayaneo_dsi_get_vfp(), ayaneo_screen_w(), ayaneo_screen_h(), g_rot_ccw);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_lcminit(const char *arg, void *data, unsigned sz)
{
	unsigned which;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	which = fb_dec(&arg);
	st7123_force_init((int)which);
	snprintf(lbuf, sizeof lbuf, "lcminit: reset + table %u sent", which);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_gpio(const char *arg, void *data, unsigned sz)
{
	extern int mt_get_gpio_in(unsigned pin);
	extern int mt_set_gpio_dir(unsigned pin, unsigned dir);
	extern int mt_set_gpio_mode(unsigned pin, unsigned mode);
	unsigned n;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	n = fb_dec(&arg);
	mt_set_gpio_mode(n | 0x80000000u, 0);
	mt_set_gpio_dir(n | 0x80000000u, 0);
	snprintf(lbuf, sizeof lbuf, "gpio%u=%d", n, mt_get_gpio_in(n | 0x80000000u));
	fastboot_info(lbuf);
	fastboot_okay("");
}
#endif

/* `oem mode`: which fastboot we are talking to. The same oem commands are registered in the
 * plain bootloader and in the emulator's USB channel, so a tool cannot tell them apart by
 * probing commands; this answers directly: boot_mode (FASTBOOT = plain bootloader, flashing
 * works), whether the ROM menu loop is live, and whether a game is presenting (resizer up). */
static void cmd_mode(const char *arg, void *data, unsigned sz)
{
	extern int g_boot_mode;
	enum { AYANEO_BM_FASTBOOT = 99 };   /* platform/boot_mode.h: FASTBOOT = 99 */
	extern volatile int g_menu_live;
	extern int ayaneo_shot_source(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	unsigned pa, w, h, pitch;
	int game = ayaneo_shot_source(&pa, &w, &h, &pitch);
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "mode: boot=%d %s menu=%d game=%d",
		 g_boot_mode, g_boot_mode == AYANEO_BM_FASTBOOT ? "BOOTLOADER(flash ok)" : "EMU(no flash)",
		 g_menu_live, game);
	fastboot_info(lbuf);
	{	/* why the preloader started us: 0 power key, 1 usb, 2 rtc, 3/4/9/10 watchdog, 8 kernel panic,
		 * 30 power exception, 31 long power key, 32 power loss */
		extern int ayaneo_boot_reason(void);   /* gba_driver.c: g_boot_arg->boot_reason */
		snprintf(lbuf, sizeof lbuf, "boot reason: %d", ayaneo_boot_reason());
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* `oem shot[:step]`: dump the currently displayed picture downscaled by step (default 16) as
 * RGB565 hex, 13 pixels per INFO line, rows as "NNN:" + continuations "NNN+" (decode with
 * tools/ayaneo/gba/fastboot_menu_shot.py). Source = the game's resizer buffer when a game runs,
 * else the front frame buffer; both in PHYSICAL (panel) orientation. Bounded INFO writes only. */
static void cmd_shot(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_shot_source(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	extern int ayaneo_shot_overlay(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	unsigned pa, w, h, pitch, step, x, y, n, k;
	const unsigned int *src;
	int game, ovl = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	step = fb_dec(&arg); if (step < 2u) step = 16u;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == 'o') ovl = 1;   /* shot:N:o = the in-game overlay paint buffer (logical orientation) */
	game = ovl ? (ayaneo_shot_overlay(&pa, &w, &h, &pitch) ? 2 : ayaneo_shot_source(&pa, &w, &h, &pitch))
		   : ayaneo_shot_source(&pa, &w, &h, &pitch);
	src = (const unsigned int *)pa;
	snprintf(lbuf, sizeof lbuf, "shot: %s %ux%u pitch=%u step=%u -> %ux%u", game == 2 ? "overlay" : game ? "game" : "fb",
		 w, h, pitch, step, w / step, h / step);
	fastboot_info(lbuf);
	for (y = 0, n = 0; y + step <= h; y += step, n++) {
		unsigned row = y / step, col = 0;
		for (x = 0; x + step <= w; ) {
			int len = snprintf(lbuf, sizeof lbuf, "%u%c", row, col ? '+' : ':');
			for (k = 0; k < 13u && x + step <= w; k++, x += step, col++) {
				unsigned int p = src[y * pitch + x];
				unsigned int v = (((p >> 19) & 0x1fu) << 11) | (((p >> 10) & 0x3fu) << 5) | ((p >> 3) & 0x1fu);
				len += snprintf(lbuf + len, sizeof lbuf - (unsigned)len, "%04x", v);
			}
			fastboot_info(lbuf);
		}
	}
	fastboot_okay("");
}

/* Full resolution capture without thousands of INFO lines (a 12k line text dump wedged the
 * USB link): `oem shotstage[:o]` packs the displayed frame (or the in-game overlay with :o)
 * as RGB565 behind a 16 byte header into free DRAM, and the standard fastboot `upload`
 * command (host: fastboot get_staged <file>) streams it back in one bulk transfer. */
#define SHOT_STAGE_PA  0x55E00000u   /* 2 MB free on the Konkr (the active tiles moved to 0x54470000) */
#define SHOT_STAGE_MAX 0x1F0000u
static unsigned s_stage_len;
static void cmd_shotstage(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_shot_source(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	extern int ayaneo_shot_overlay(unsigned int *pa, unsigned int *w, unsigned int *h, unsigned int *pitch_px);
	unsigned pa, w, h, pitch, x, y;
	const unsigned int *src;
	unsigned int *hdr = (unsigned int *)SHOT_STAGE_PA;
	unsigned short *dst = (unsigned short *)(SHOT_STAGE_PA + 16u);
	int kind;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == 'o' && ayaneo_shot_overlay(&pa, &w, &h, &pitch)) kind = 2;
	else kind = ayaneo_shot_source(&pa, &w, &h, &pitch);
	if (16u + w * h * 2u > SHOT_STAGE_MAX) { fastboot_fail("frame too large"); return; }
	src = (const unsigned int *)pa;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			unsigned int p = src[y * pitch + x];
			dst[y * w + x] = (unsigned short)((((p >> 19) & 0x1fu) << 11) | (((p >> 10) & 0x3fu) << 5) | ((p >> 3) & 0x1fu));
		}
	hdr[0] = 0x544F4853u;   /* "SHOT" */
	hdr[1] = w; hdr[2] = h; hdr[3] = (unsigned)kind;   /* 0 fb, 1 game plane, 2 overlay; all RGB565 */
	s_stage_len = 16u + w * h * 2u;
	snprintf(lbuf, sizeof lbuf, "stage: %s %ux%u rgb565 %u bytes (fastboot get_staged <file>)",
		 kind == 2 ? "overlay" : kind ? "game" : "fb", w, h, s_stage_len);
	fastboot_info(lbuf);
	fastboot_okay("");
}
/* transcript of the last upload for `oem upstat` (the first attempt failed on the host side and
 * the unbounded bulk writes then hung the fastboot thread: every write here is bounded) */
static unsigned s_up_step, s_up_sent; static int s_up_rc;
extern void fastboot_session_recover(void);   /* app/mt_boot/fastboot.c */
static void cmd_upload(const char *arg, void *data, unsigned sz)
{
	extern int usb_write_with_timeout(void *buf, unsigned len, unsigned long timeout);   /* time_t */
	static char rsp[64];
	unsigned off = 0;
	(void)arg; (void)data; (void)sz;
	s_up_step = 1; s_up_sent = 0; s_up_rc = 0;
	if (!s_stage_len) { s_up_step = 9; fastboot_fail("nothing staged (oem shotstage first)"); return; }
	snprintf(rsp, sizeof rsp, "DATA%08x", s_stage_len);
	s_up_rc = usb_write_with_timeout(rsp, 12, 2000);
	if (s_up_rc < 0) { s_up_step = 2; fastboot_session_recover(); return; }
	s_up_step = 3;
	/* The QMU takes at most GPD_BUF_SIZE_ALIGN (64512) bytes per IN request and the write path
	 * does not check (65536 byte chunks and a single 1.2 MB request both failed). IN requests
	 * carry no ZLP, so 64512 = 126 x 512 byte chunks produce no short packet until the real
	 * tail, and fastboot.exe's 1 MB reads (which end on any short packet) complete. */
	while (off < s_stage_len) {
		unsigned n = s_stage_len - off; if (n > 64512u) n = 64512u;
		s_up_rc = usb_write_with_timeout((void *)(SHOT_STAGE_PA + off), n, 3000);
		if (s_up_rc < 0) { s_up_step = 4; fastboot_session_recover(); return; }
		off += n; s_up_sent = off;
	}
	s_up_step = 5;
	fastboot_okay("");
	s_up_step = 6;
}
static void cmd_upstat(const char *arg, void *data, unsigned sz)
{
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "upstat: step=%u sent=%u rc=%d staged=%u", s_up_step, s_up_sent, s_up_rc, s_stage_len);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem led - the power LED thread's view: charger, battery percent, state (0 off 1 green 2 red) */
static void cmd_led(const char *arg, void *data, unsigned sz)
{
	extern volatile int g_dbg_led_chr, g_dbg_led_pct, g_dbg_led_state, g_dbg_led_ticks;
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "led: chr=%d pct=%d state=%d ticks=%d", g_dbg_led_chr, g_dbg_led_pct, g_dbg_led_state, g_dbg_led_ticks);
	fastboot_info(lbuf);
	{
		extern volatile int g_dbg_alead_min, g_dbg_asnap_n, g_dbg_asnap_lead; extern volatile unsigned g_dbg_asnap_ms;
		extern volatile unsigned g_dbg_aw_up_ms[2], g_dbg_aw_unmute_ms;
		extern volatile unsigned g_dbg_idle_sleep_max_us, g_dbg_idle_sleeps;
		snprintf(lbuf, sizeof lbuf, "aud: lead_min=%d snaps=%d last=%d at=%ums aw_up=%u/%u unmute=%u",
			 g_dbg_alead_min, g_dbg_asnap_n, g_dbg_asnap_lead, g_dbg_asnap_ms, g_dbg_aw_up_ms[0], g_dbg_aw_up_ms[1], g_dbg_aw_unmute_ms);
		fastboot_info(lbuf);
		{ extern volatile int g_dbg_aw_dma;
		  extern volatile unsigned g_dbg_emu_abs_ms; extern volatile int g_dbg_emu_chime;
		  snprintf(lbuf, sizeof lbuf, "idle: sleeps=%u max=%uus aw_dma=%d emu@%ums chime=%d", g_dbg_idle_sleeps, g_dbg_idle_sleep_max_us, g_dbg_aw_dma, g_dbg_emu_abs_ms, g_dbg_emu_chime); }
		fastboot_info(lbuf);
		{ extern volatile unsigned int g_dbg_usb_suspends, g_dbg_usb_recovers, g_dbg_usb_backoff;
		  snprintf(lbuf, sizeof lbuf, "usb: suspends=%u recovers=%u backoff=%u", g_dbg_usb_suspends, g_dbg_usb_recovers, g_dbg_usb_backoff); }
	fastboot_info(lbuf);
	{ extern volatile unsigned int g_dbg_pwrap_waits, g_dbg_pwrap_fails;
	  snprintf(lbuf, sizeof lbuf, "pwrap: waits=%u fails=%u", g_dbg_pwrap_waits, g_dbg_pwrap_fails); }
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* oem hfp[:wc] - Konkr: DSI horizontal front porch word count (0 = read only); prints the line words */
static void cmd_hfp(const char *arg, void *data, unsigned sz)
{
	extern unsigned int ayaneo_dsi_set_hfp_wc(unsigned int wc);
	extern void ayaneo_dsi_line_words(unsigned int *hsa, unsigned int *hbp, unsigned int *hfp, unsigned int *ps);
	unsigned v, old, hsa, hbp, hfp, ps;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	v = (*arg >= '0' && *arg <= '9') ? fb_dec(&arg) : 0u;
	old = ayaneo_dsi_set_hfp_wc(v);
	ayaneo_dsi_line_words(&hsa, &hbp, &hfp, &ps);
	snprintf(lbuf, sizeof lbuf, "hfp: was %u now %u; hsa=%u hbp=%u ps=0x%x (measure: oem edges)", old, hfp, hsa, hbp, ps);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem dr:KHZ - Konkr: MIPI data rate live (250001..999999 kHz; stock 353935) */
static void cmd_dr(const char *arg, void *data, unsigned sz)
{
	extern unsigned int ayaneo_dsi_set_dr_khz(unsigned int khz);
	unsigned v, pcw;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	v = fb_dec(&arg);
	pcw = ayaneo_dsi_set_dr_khz(v);
	snprintf(lbuf, sizeof lbuf, "dr: %u kHz -> pcw 0x%08x %s (measure: oem edges / diag hz1000)", v, pcw, pcw ? "set" : "REJECTED");
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem chg - Konkr: MT6360 charger block raw registers (0x00 id, 0x4A status, 0x4B..0x4E) */
static void cmd_chg(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_mt6360_chg_reg(unsigned char reg);
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "chg: 00=%02x 01=%02x 4a=%02x 4b=%02x 4c=%02x 4d=%02x 4e=%02x (4a>>6: 0 ready 1 charging 2 done 3 fault)",
		 ayaneo_mt6360_chg_reg(0x00) & 0xff, ayaneo_mt6360_chg_reg(0x01) & 0xff, ayaneo_mt6360_chg_reg(0x4A) & 0xff,
		 ayaneo_mt6360_chg_reg(0x4B) & 0xff, ayaneo_mt6360_chg_reg(0x4C) & 0xff, ayaneo_mt6360_chg_reg(0x4D) & 0xff, ayaneo_mt6360_chg_reg(0x4E) & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem chrind - MT6360 PMU RGB_EN (0x80) before and after taking the red charger indicator into software mode, off */
static void cmd_chrind(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_mt6360_chg_reg(unsigned char reg);
	extern int ayaneo_mt6360_chrind_off(void);
	int before = ayaneo_mt6360_chg_reg(0x80), after;
	(void)arg; (void)data; (void)sz;
	after = ayaneo_mt6360_chrind_off();
	snprintf(lbuf, sizeof lbuf, "chrind: rgb_en before=%02x after=%02x (bit3 sw mode, bit7 isink1 on)", before & 0xff, after & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem chgw:REG,VAL - write one MT6360 PMU register (hex), report 0x80..0x8f afterwards */
static void cmd_chgw(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_mt6360_chg_reg(unsigned char reg);
	extern int ayaneo_mt6360_chg_wr(unsigned char reg, unsigned char val);
	unsigned reg = 0, val = 0; int i, n = 0, rc = 0;
	(void)data; (void)sz;
	if (arg && *arg == ':') arg++;
	if (arg && *arg) {
		char *e;
		reg = strtoul(arg, &e, 16);
		if (*e == ',') { val = strtoul(e + 1, 0, 16); rc = ayaneo_mt6360_chg_wr((unsigned char)reg, (unsigned char)val); }
	}
	n = snprintf(lbuf, sizeof lbuf, "chgw %02x<-%02x rc=%d rd=%02x |", reg, val, rc, ayaneo_mt6360_chg_reg((unsigned char)reg) & 0xff);
	for (i = 0x80; i < 0x90; i++) n += snprintf(lbuf + n, sizeof lbuf - n, " %02x", ayaneo_mt6360_chg_reg((unsigned char)i) & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem pmicw:ADDR[,VAL] - read (or write then read) one MT6359 PMIC register over the wrapper (hex),
 * then the ISINK block 0x210a..0x2110 */
static void cmd_pmicw(const char *arg, void *data, unsigned sz)
{
	extern unsigned int pmic_read_interface(unsigned int RegNum, unsigned int *val, unsigned int MASK, unsigned int SHIFT);
	extern unsigned int pmic_config_interface(unsigned int RegNum, unsigned int val, unsigned int MASK, unsigned int SHIFT);
	unsigned reg = 0, val = 0, rd = 0, a; int n, wr = 0;
	(void)data; (void)sz;
	if (arg && *arg == ':') arg++;
	if (arg && *arg) {
		char *e;
		reg = strtoul(arg, &e, 16);
		if (*e == ',') { val = strtoul(e + 1, 0, 16); pmic_config_interface(reg, val & 0xffff, 0xffff, 0); wr = 1; }
		pmic_read_interface(reg, &rd, 0xffff, 0);
	}
	n = snprintf(lbuf, sizeof lbuf, "pmicw %04x%s%04x rd=%04x | isink", reg, wr ? "<-" : " ", wr ? val : 0, rd);
	for (a = 0x210a; a <= 0x2110; a += 2) { unsigned v = 0; pmic_read_interface(a, &v, 0xffff, 0); n += snprintf(lbuf + n, sizeof lbuf - n, " %04x", v); }
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem awdump[:REG,VAL] - AW2033 registers 0x00..0x0f and 0x30..0x3f, after an optional write (hex) */
static void cmd_awdump(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_aw2033_read(unsigned char reg);
	extern int ayaneo_aw2033_write(unsigned char reg, unsigned char val);
	int i, n;
	(void)data; (void)sz;
	if (arg && *arg == ':') arg++;
	if (arg && *arg) {
		char *e; unsigned reg = strtoul(arg, &e, 16);
		if (*e == ',') ayaneo_aw2033_write((unsigned char)reg, (unsigned char)strtoul(e + 1, 0, 16));
	}
	n = snprintf(lbuf, sizeof lbuf, "aw 00:");
	for (i = 0; i < 0x10; i++) n += snprintf(lbuf + n, sizeof lbuf - n, " %02x", ayaneo_aw2033_read((unsigned char)i) & 0xff);
	fastboot_info(lbuf);
	n = snprintf(lbuf, sizeof lbuf, "aw 30:");
	for (i = 0x30; i < 0x40; i++) n += snprintf(lbuf + n, sizeof lbuf - n, " %02x", ayaneo_aw2033_read((unsigned char)i) & 0xff);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem gauge - battery gauge internals (ayaneo_gauge.c), after a fresh sample */
static void cmd_gauge(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_gauge_sample(void);
	extern int ayaneo_gauge_report(char *out, int n);
	extern void ayaneo_gauge_hist(void (*emit)(const char *line), char *buf, int n);
	(void)data; (void)sz;
	if (arg && *arg == ':') arg++;
	if (arg && arg[0] == 'h') { ayaneo_gauge_hist(fastboot_info, lbuf, sizeof lbuf); fastboot_okay(""); return; }
	ayaneo_gauge_sample();
	ayaneo_gauge_report(lbuf, sizeof lbuf);
	fastboot_info(lbuf);
	ayaneo_gauge_report(lbuf, -(int)sizeof lbuf);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static int g_hud_fps_read(void) { extern volatile int g_hud_fps; return g_hud_fps; }

/* oem serialinfo - the candidate unique serial sources: the SoC efuse key (devinfo 12..15,
 * MediaTek's CONFIG_MTK_USB_UNIQUE_SERIAL source) and the boot eMMC CID; and the serial now */
static void cmd_serialinfo(const char *arg, void *data, unsigned sz)
{
	extern unsigned int get_devinfo_with_index(unsigned int index);
	extern int ayaneo_emmc_cid(unsigned int out[4]);
	extern char sn_buf[];
	unsigned int cid[4] = {0, 0, 0, 0}; int rc;
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "efuse: %08x %08x %08x %08x", get_devinfo_with_index(12), get_devinfo_with_index(13),
		 get_devinfo_with_index(14), get_devinfo_with_index(15));
	fastboot_info(lbuf);
	rc = ayaneo_emmc_cid(cid);
	snprintf(lbuf, sizeof lbuf, "emmc cid: rc=%d %08x %08x %08x %08x", rc, cid[0], cid[1], cid[2], cid[3]);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "serial now: %s", sn_buf);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem rewindmap - the rewind arena islands (phys base + MB) and the ring state line */
static void cmd_rewindmap(const char *arg, void *data, unsigned sz)
{
	extern unsigned int ayaneo_rewind_island(unsigned int i, unsigned int *size);
	extern void ayaneo_rewind_dbg_line(char *out, unsigned int cap);
	unsigned i, s, b;
	(void)arg; (void)data; (void)sz;
	for (i = 0; (b = ayaneo_rewind_island(i, &s)) != 0; i++) {
		snprintf(lbuf, sizeof lbuf, "island %u: 0x%08x +%u MB (end 0x%08x)", i, b, s >> 20, b + s);
		fastboot_info(lbuf);
	}
	{ char l[160]; ayaneo_rewind_dbg_line(l, sizeof l); snprintf(lbuf, sizeof lbuf, "ring: %s", l); fastboot_info(lbuf); }
	{
		extern volatile unsigned int g_dbg_pt_bad, g_dbg_pt_first_va, g_dbg_pt_first_ms, g_dbg_pt_checks, g_dbg_pt_l1[4];
		extern volatile unsigned int g_dbg_l2_diffs, g_dbg_l2_first_ms, g_dbg_l2_first_idx, g_dbg_l2_first_gb, g_dbg_l2_old_lo, g_dbg_l2_new_lo, g_dbg_l2_ticks;
		extern unsigned int ayaneo_rewind_tables_addr(unsigned int g);
		snprintf(lbuf, sizeof lbuf, "pt: checks=%u bad=%u first=0x%08x at %ums", g_dbg_pt_checks, g_dbg_pt_bad, g_dbg_pt_first_va, g_dbg_pt_first_ms);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "l2: ticks=%u diffs=%u first gb%u idx %u at %ums", g_dbg_l2_ticks, g_dbg_l2_diffs, g_dbg_l2_first_gb, g_dbg_l2_first_idx, g_dbg_l2_first_ms);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "l2 first: old %08x new %08x", g_dbg_l2_old_lo, g_dbg_l2_new_lo);
		fastboot_info(lbuf);
		{
			extern volatile unsigned int g_dbg_l2_last[4][4]; extern unsigned int ayaneo_rewind_l2_owned(void);
			unsigned r;
			for (r = 0; r < 4u; r++) {
				snprintf(lbuf, sizeof lbuf, "l2 hit%u: gb%u idx %u new %08x at %ums", r, g_dbg_l2_last[r][0], g_dbg_l2_last[r][1], g_dbg_l2_last[r][2], g_dbg_l2_last[r][3]);
				fastboot_info(lbuf);
			}
			snprintf(lbuf, sizeof lbuf, "l2 owned tables: 0x%x (bit g = GB g table is ours)", ayaneo_rewind_l2_owned());
			fastboot_info(lbuf);
		}
		snprintf(lbuf, sizeof lbuf, "l2 tables: gb1 %08x gb2 %08x gb3 %08x", ayaneo_rewind_tables_addr(1), ayaneo_rewind_tables_addr(2), ayaneo_rewind_tables_addr(3));
		fastboot_info(lbuf);
		{
			extern volatile unsigned int g_dbg_mmu_log[8][3], g_dbg_mmu_log_n;
			unsigned k, n = g_dbg_mmu_log_n;
			for (k = (n > 8u ? n - 8u : 0u); k < n; k++) {
				snprintf(lbuf, sizeof lbuf, "map%u: va %08x +%08x at %ums", k, g_dbg_mmu_log[k % 8u][0], g_dbg_mmu_log[k % 8u][1], g_dbg_mmu_log[k % 8u][2]);
				fastboot_info(lbuf);
			}
		}
	}
	fastboot_okay("");
}

/* oem mmuq:HEXADDR - Konkr: is this VA mapped (page table walk), and the raw descriptors */
static void cmd_mmuq(const char *arg, void *data, unsigned sz)
{
	extern int arch_mmu_probe(unsigned int vaddr, unsigned long long *l1, unsigned long long *l2);
	unsigned long long l1 = 0, l2 = 0; unsigned a; int k;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	a = fb_hex(&arg);
	k = arch_mmu_probe(a, &l1, &l2);
	snprintf(lbuf, sizeof lbuf, "mmuq 0x%08x: %s l1=%08x%08x l2=%08x%08x", a,
		 k == 1 ? "1GB block" : k == 2 ? "2MB section" : k == 3 ? "page table" : "UNMAPPED",
		 (unsigned)(l1 >> 32), (unsigned)l1, (unsigned)(l2 >> 32), (unsigned)l2);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem env:NAME - the LK environment (para partition) value, as the serial number code reads it */
static void cmd_env(const char *arg, void *data, unsigned sz)
{
	extern char *get_env(char *name);
	char name[64]; int n = 0; char *val;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	while (*arg && n < 63) name[n++] = *arg++;
	name[n] = 0;
	val = get_env(name);
	snprintf(lbuf, sizeof lbuf, "env %s = %s", name, val ? val : "(unset)");
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem ledset:R,G,B - drive the AW2033 channels directly (0..255 each) to check the wiring by eye */
static void cmd_ledset(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_charge_led(int r, int g, int b);
	unsigned r, g, b;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	r = fb_dec(&arg); g = fb_dec(&arg); b = fb_dec(&arg);
	ayaneo_charge_led((int)r, (int)g, (int)b);
	snprintf(lbuf, sizeof lbuf, "ledset: r=%u g=%u b=%u (channel order as the driver assumes)", r, g, b);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem persist - queue a settings write (eMMC + SD mirror) and report the last write's durations */
static void cmd_persist(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_menu_settings_persist(void);
	extern volatile unsigned g_dbg_persist_emmc_us, g_dbg_persist_sd_us, g_dbg_persist_n;
	extern volatile int g_dbg_mlead_min;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (arg[0] == 'r') g_dbg_mlead_min = 99999;
	else ayaneo_menu_settings_persist();
	snprintf(lbuf, sizeof lbuf, "persist: n=%u emmc=%uus sd=%uus menu_lead_min=%d", g_dbg_persist_n, g_dbg_persist_emmc_us, g_dbg_persist_sd_us, g_dbg_mlead_min);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* oem aw[:reinit] - Konkr speaker amps: state per amp, optionally redo the bring-up */
static void cmd_aw(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_aw88166_diag(int k, char *buf, unsigned cap);
	extern void ayaneo_aw88166_reinit(void);
	extern void ayaneo_aw88166_start(void);
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (arg[0] == 'r') ayaneo_aw88166_reinit();
	else if (arg[0] == 's') ayaneo_aw88166_start();
	ayaneo_aw88166_diag(0, lbuf, sizeof lbuf); fastboot_info(lbuf);
	ayaneo_aw88166_diag(1, lbuf, sizeof lbuf); fastboot_info(lbuf);
	{ extern void ayaneo_aw88166_times(int k, char *buf, unsigned cap);
	  ayaneo_aw88166_times(0, lbuf, sizeof lbuf); fastboot_info(lbuf);
	  ayaneo_aw88166_times(1, lbuf, sizeof lbuf); fastboot_info(lbuf); }
	fastboot_okay("");
}

/* oem bfi[:window] - Konkr: lit fraction of the frame (1024ths) for the Strobe (BFI) mode */
static void cmd_bfi(const char *arg, void *data, unsigned sz)
{
	extern void disp_pwm_bfi_window(unsigned int w);
	extern unsigned int disp_pwm_bfi_window_get(void);
	extern int ayaneo_get_strobe(void);
	unsigned w;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	w = fb_dec(&arg);
	if (w) disp_pwm_bfi_window(w);
	snprintf(lbuf, sizeof lbuf, "bfi: window=%u/1024 strobe=%s", disp_pwm_bfi_window_get(), ayaneo_get_strobe() ? "on" : "off");
	fastboot_info(lbuf);
	{
		extern int disp_pwm_bfi_state(char *out, int n);
		extern unsigned int ayaneo_dsi_refresh_milli(void);
		extern volatile unsigned int g_vsync_div, g_pace_fps_milli;
		extern volatile unsigned int g_dbg_pace_timeouts;
		disp_pwm_bfi_state(lbuf, sizeof lbuf);
		fastboot_info(lbuf);
		disp_pwm_bfi_state(lbuf, -(int)sizeof lbuf);
		fastboot_info(lbuf);
		snprintf(lbuf, sizeof lbuf, "panel: hz=%u vdiv=%u fps=%u tmo=%u", ayaneo_dsi_refresh_milli(), g_vsync_div, g_pace_fps_milli, g_dbg_pace_timeouts);
		fastboot_info(lbuf);
		{ extern volatile unsigned int g_dbg_ff_margin_us, g_dbg_ff_misses, g_dbg_ff_frames;
		  snprintf(lbuf, sizeof lbuf, "ff: margin=%u us misses=%u frames=%u hud_fps=%d", g_dbg_ff_margin_us, g_dbg_ff_misses, g_dbg_ff_frames, g_hud_fps_read());
		  fastboot_info(lbuf); }
	}
	fastboot_okay("");
}

static void cmd_vdiv(const char *arg, void *data, unsigned sz)
{
	extern volatile unsigned int g_vsync_div;
	extern unsigned int ayaneo_dsi_default_vfp(void);
	extern void ayaneo_dsi_set_vfp(unsigned int vfp);
	unsigned v;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	v = fb_dec(&arg); if (v < 1u) v = 1u; if (v > 4u) v = 4u;
	g_vsync_div = v;
	ayaneo_dsi_set_vfp(ayaneo_dsi_default_vfp());   /* re-rest the panel at div x GBA rate */
	snprintf(lbuf, sizeof lbuf, "vdiv: %u, default vfp now %u", v, ayaneo_dsi_default_vfp());
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* `oem edges`: how often does the display pipe actually finish a frame? Counts RDMA0 FRAME_END
 * rises over 500 ms by polling (clear, wait rise, repeat) and reports edges per second next to
 * the DSI timing's computed refresh. Also samples the raw status register a few times. */
static void cmd_edges(const char *arg, void *data, unsigned sz)
{
	extern int ayaneo_rdma_wait_frame_end(unsigned int timeout_us);
	extern unsigned int gpt4_get_current_tick(void), ayaneo_dsi_refresh_milli(void);
	unsigned int t0 = gpt4_get_current_tick(), n = 0, to = 0, first = 0, last = 0;
	(void)arg; (void)data; (void)sz;
	while ((gpt4_get_current_tick() - t0) < 6500000u) {   /* 500 ms */
		unsigned int t = gpt4_get_current_tick();
		if (ayaneo_rdma_wait_frame_end(40000u)) { n++; if (!first) first = t; last = gpt4_get_current_tick(); }
		else to++;
	}
	snprintf(lbuf, sizeof lbuf, "edges: rdma0 frame_end %u in 500ms (%u/s) timeouts=%u dsi=%umHz",
		 n, n * 2u, to, ayaneo_dsi_refresh_milli());
	fastboot_info(lbuf);
	if (n > 1) {
		snprintf(lbuf, sizeof lbuf, "edges: mean period %u us", ((last - first) / 13u) / (n - 1u));
		fastboot_info(lbuf);
	}
	fastboot_okay("");
}

/* `oem latch`: how often does the overlay actually switch to a new buffer? Samples the LIVE
 * OVL0_2L L0 address register for one second and counts changes; the frame buffer layer (menu
 * canvas, FB_LAYER) and the resizer game layer both sit on OVL0_2L L0. 60 changes/s = every
 * presented frame reaches the panel; 30 = every other present is lost. */
static void cmd_latch(const char *arg, void *data, unsigned sz)
{
	extern unsigned int gpt4_get_current_tick(void);
	extern void udelay(unsigned long usec);
	volatile unsigned int *l0 = (volatile unsigned int *)(0x14009000u + 0xF40u);   /* OVL0_2L L0_ADDR */
	volatile unsigned int *l0b = (volatile unsigned int *)(0x14008000u + 0xF40u);  /* OVL0 L0_ADDR */
	unsigned int t0 = gpt4_get_current_tick(), a = *l0, b = *l0b, na = 0, nb = 0, samples = 0;
	(void)arg; (void)data; (void)sz;
	while ((gpt4_get_current_tick() - t0) < 13000000u) {   /* 1 s */
		unsigned int x = *l0, y = *l0b;
		if (x != a) { na++; a = x; }
		if (y != b) { nb++; b = y; }
		samples++;
		udelay(100);
	}
	snprintf(lbuf, sizeof lbuf, "latch: ovl0_2l l0 addr changes/s=%u ovl0 l0=%u samples=%u", na, nb, samples);
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "latch: ovl0_2l l0 addr now=0x%08x ovl0=0x%08x", a, b);
	fastboot_info(lbuf);
	fastboot_okay("");
}

/* `oem clk`: is GPT4 really 13 MHz on this board? Compare it with the ARM architected timer
 * (CNTFRQ is the CPU's own statement of that timer's rate) over ~100 ms, and dump the GPT4
 * clock / control registers. Every emulator pacing constant assumes 13 MHz. */
static void cmd_clk(const char *arg, void *data, unsigned sz)
{
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int cntfrq, g0, g1; unsigned long long c0, c1;
	volatile unsigned int *gpt4_con = (volatile unsigned int *)(0x10008000u + 0x40u);
	volatile unsigned int *gpt4_clk = (volatile unsigned int *)(0x10008000u + 0x44u);
	(void)arg; (void)data; (void)sz;
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(cntfrq));
	__asm__ volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r"(c0));
	g0 = gpt4_get_current_tick();
	while ((gpt4_get_current_tick() - g0) < 1300000u) ;   /* "100 ms" of gpt4 at 13 MHz */
	__asm__ volatile("mrrc p15, 0, %Q0, %R0, c14" : "=r"(c1));
	g1 = gpt4_get_current_tick();
	{
		unsigned int dg = g1 - g0, dc = (unsigned int)(c1 - c0);
		/* gpt4 rate = dg / (dc / cntfrq) */
		unsigned long long gpt_hz = dc ? ((unsigned long long)dg * cntfrq) / dc : 0;
		snprintf(lbuf, sizeof lbuf, "clk: cntfrq=%u archticks=%u gpt4ticks=%u -> gpt4=%u Hz",
			 cntfrq, dc, dg, (unsigned int)gpt_hz);
		fastboot_info(lbuf);
	}
	snprintf(lbuf, sizeof lbuf, "clk: GPT4_CON=0x%08x GPT4_CLK=0x%08x", *gpt4_con, *gpt4_clk);
	fastboot_info(lbuf);
	fastboot_okay("");
}

static void cmd_vfp(const char *arg, void *data, unsigned sz)
{
	extern void ayaneo_dsi_set_vfp(unsigned int vfp);
	extern unsigned int ayaneo_dsi_get_vfp(void), ayaneo_dsi_refresh_milli(void);
	unsigned v;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	v = fb_dec(&arg);
	ayaneo_dsi_set_vfp(v);
	snprintf(lbuf, sizeof lbuf, "vfp: asked %u now %u -> computed %u mHz (measure with oem diag hz1000)",
		 v, ayaneo_dsi_get_vfp(), ayaneo_dsi_refresh_milli());
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
	fastboot_register("oem gen-bench", cmd_gen_bench, 1, 0);
	fastboot_register("oem snes-launch", cmd_snes_launch, 1, 0);
	fastboot_register("oem snes-bench", cmd_snes_bench, 1, 0);
	fastboot_register("oem snes-stretch", cmd_snes_stretch, 1, 0);
	fastboot_register("oem snes-rsz", cmd_snes_rsz, 1, 0);
	fastboot_register("oem ovltest", cmd_ovltest, 1, 0);
	fastboot_register("oem snes-ra", cmd_snes_ra, 1, 0);
	fastboot_register("oem nav:", cmd_nav, 1, 0);
	fastboot_register("oem preempt:", cmd_preempt, 1, 0);
	fastboot_register("oem lcmwr:", cmd_lcmwr, 1, 0);   /* live panel-register poke (VCOM/gamma sweep) */
	fastboot_register("oem vcom:", cmd_vcom, 1, 0);     /* VCOM convenience: lo,hi,off */
	fastboot_register("oem strobe:", cmd_strobe, 1, 0); /* free-running strobe: div,duty */
	fastboot_register("oem strobelk:", cmd_strobelk, 1, 0); /* vsync-locked strobe: div,duty */
	fastboot_register("oem od:", cmd_od, 1, 0);             /* software overdrive: on,kup,kdn,thr,cap */
	fastboot_register("oem odup:", cmd_odup, 1, 0);
	fastboot_register("oem odcap", cmd_odcap, 1, 0);         /* prev-indexed rising curve: kuphi,kuplo,gam */
	fastboot_register("oem rot:", cmd_rot, 1, 0);
	fastboot_register("oem igm", cmd_igm, 1, 0);           /* Konkr: 0 clockwise / 1 counter clockwise turn */
	fastboot_register("oem shot", cmd_shot, 1, 0);
#if defined(KONKR_KPA)
	fastboot_register("oem aw", cmd_aw, 1, 0);
	fastboot_register("oem bfi", cmd_bfi, 1, 0);
	fastboot_register("oem led", cmd_led, 1, 0);
	fastboot_register("oem persist", cmd_persist, 1, 0);
	fastboot_register("oem env:", cmd_env, 1, 0);
	fastboot_register("oem ledset:", cmd_ledset, 1, 0);
	fastboot_register("oem hfp", cmd_hfp, 1, 0);
	fastboot_register("oem chg", cmd_chg, 1, 0);
	fastboot_register("oem chrind", cmd_chrind, 1, 0);
	fastboot_register("oem chgw", cmd_chgw, 1, 0);
	fastboot_register("oem pmicw", cmd_pmicw, 1, 0);
	fastboot_register("oem gauge", cmd_gauge, 1, 0);
	fastboot_register("oem serialinfo", cmd_serialinfo, 1, 0);
	fastboot_register("oem dsiw", cmd_dsiw, 1, 0);
	fastboot_register("oem gray", cmd_gray, 1, 0);
	fastboot_register("oem awdump", cmd_awdump, 1, 0);
	fastboot_register("oem rewindmap", cmd_rewindmap, 1, 0);
	fastboot_register("oem mmuq:", cmd_mmuq, 1, 0);
	fastboot_register("oem dr:", cmd_dr, 1, 0);
#endif
	fastboot_register("oem shotstage", cmd_shotstage, 1, 0);   /* pack the frame for `upload` */
	fastboot_register("upload", cmd_upload, 1, 0);              /* fastboot get_staged <file> */
	fastboot_register("oem upstat", cmd_upstat, 1, 0);          /* dump the displayed frame (game buffer or fb), /step */
	fastboot_register("oem mode", cmd_mode, 1, 0);          /* bootloader vs emulator channel, menu/game state */
	fastboot_register("oem vfp:", cmd_vfp, 1, 0);           /* set the DSI vertical front porch live */
	fastboot_register("oem edges", cmd_edges, 1, 0);        /* measure display frame-end rate */
	fastboot_register("oem latch", cmd_latch, 1, 0);        /* overlay buffer switches per second */
	fastboot_register("oem clk", cmd_clk, 1, 0);            /* gpt4 rate vs the ARM architected timer */
	fastboot_register("oem vdiv:", cmd_vdiv, 1, 0);         /* vsync divider (panel = N x core rate) */
#ifdef KONKR_KPA
	fastboot_register("oem lcm", cmd_lcm, 1, 0);            /* Konkr: glass id pins + dsi state */
	fastboot_register("oem lcminit:", cmd_lcminit, 1, 0);   /* Konkr: force panel init with table N */
	fastboot_register("oem gpio:", cmd_gpio, 1, 0);         /* Konkr: read one gpio */
#endif
	fastboot_register("oem selftest", cmd_selftest, 1, 0);
}
