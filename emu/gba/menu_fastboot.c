/*
 * Fastboot debug channel for the SNES-Classic-mini menu (AYANEO_GBA_SD).
 *
 * The USB fastboot stack is brought up ALONGSIDE the running emulator (see
 * ayaneo_fastboot_usb_start in mt_boot.c), so these commands work over USB while
 * the menu/game is live - no reboot into fastboot mode. They let me diagnose the
 * display issues host_render can never reproduce (banding is a scanout artifact,
 * timings are A55-specific) by reading the LIVE panel and the live render metrics:
 *
 *   fastboot oem screenshot         - dump the CURRENTLY DISPLAYED frame downscaled
 *                                     (default /16 = 80x60) as RGB565 hex rows, so I
 *                                     can see exactly what is on the panel right now
 *                                     (a reverse-punch frame, a "static" screen, the
 *                                     BIOS->menu handover). oem screenshot:<n> sets
 *                                     the downscale step.
 *   fastboot oem diag               - live render metrics: last/peak render us,
 *                                     loop fps, present vs gate-skipped frame counts,
 *                                     focused game, and whether the last frame was
 *                                     treated as static by the present gate.
 *
 * Reconstruct a screenshot PNG with tools/ayaneo/gba/fastboot_menu_shot.py.
 * Results come back as fastboot INFO lines ("(bootloader) ...").
 */
extern void fastboot_register(const char *prefix,
			      void (*handle)(const char *arg, void *data, unsigned sz),
			      int allow_locked, int need_download);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern int  snprintf(char *str, unsigned long size, const char *fmt, ...);

/* live panel + metrics (platform display driver + gba_snes_menu.c) */
extern const unsigned int *ayaneo_canvas_front(unsigned int *pitch, unsigned int *W, unsigned int *H);
extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
extern volatile unsigned int g_dbg_present_cnt, g_dbg_skip_cnt;
extern volatile int g_dbg_focus, g_dbg_last_static;

static char lbuf[224];

static unsigned long parse_u(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == ':') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned long)(*s - '0'); s++; }
	return v;
}

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	unsigned pc = g_dbg_present_cnt, sk = g_dbg_skip_cnt;
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf,
		 "diag: render=%uus peak=%uus fps=%u focus=%d last=%s",
		 g_dbg_render_us, g_dbg_peak_us, g_dbg_fps, g_dbg_focus,
		 g_dbg_last_static ? "STATIC(skipped)" : "changed(presented)");
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf,
		 "diag: present=%u skip=%u  (skip%% = %u of %u)",
		 pc, sk, (pc + sk) ? (sk * 100u / (pc + sk)) : 0u, pc + sk);
	fastboot_info(lbuf);
	fastboot_okay("diag done");
}

static void cmd_screenshot(const char *arg, void *data, unsigned sz)
{
	static const char *hx = "0123456789abcdef";
	const unsigned int *fb;
	unsigned int pitch, W, H, x, y;
	int step = (int)parse_u(arg);
	(void)data; (void)sz;

	if (step <= 0) step = 16;              /* default /16 -> 80x60 */
	fb = ayaneo_canvas_front(&pitch, &W, &H);
	snprintf(lbuf, sizeof lbuf, "shot: %ux%u step=%d (RGB565 hex, %u cols/row)",
		 W, H, step, (W + (unsigned)step - 1) / (unsigned)step);
	fastboot_info(lbuf);

	for (y = 0; y < H; y += (unsigned)step) {
		const unsigned int *row = fb + (unsigned long)y * pitch;
		char *o = lbuf;
		int col = 0, k;
		k = snprintf(lbuf, sizeof lbuf, "%03u:", y / (unsigned)step);
		o += k;
		for (x = 0; x < W; x += (unsigned)step) {
			unsigned int px = row[x];
			unsigned int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
			unsigned int p565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			/* 40 pixels (160 hex) per INFO line keeps it under the response cap;
			 * the rest of a wide row continues on a "<row>+" line. */
			if (col == 40) {
				*o = 0; fastboot_info(lbuf);
				o = lbuf; o += snprintf(lbuf, sizeof lbuf, "%03u+", y / (unsigned)step);
				col = 0;
			}
			*o++ = hx[(p565 >> 12) & 0xf]; *o++ = hx[(p565 >> 8) & 0xf];
			*o++ = hx[(p565 >> 4) & 0xf];  *o++ = hx[p565 & 0xf];
			col++;
		}
		*o = 0;
		fastboot_info(lbuf);
	}
	fastboot_okay("screenshot done");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem screenshot", cmd_screenshot, 1, 0);
	fastboot_register("oem diag", cmd_diag, 1, 0);
}
