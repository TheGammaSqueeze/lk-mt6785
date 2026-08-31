/*
 * Fastboot debug channel for the SNES-Classic-mini menu (AYANEO_GBA_SD).
 *
 * Lets me drive the menu and pull data from a REAL device over USB, without
 * UART, to diagnose the display-sync / transition issues that host_render can
 * never reproduce (the banding is a scanout artifact, timings are A55-specific).
 * It runs in FASTBOOT mode, where USB is up and the whole DRAM menu window
 * [0x50000000,0x56000000) + the panel are already usable. The harness reuses the
 * exact snes_menu_update/snes_menu_render of the live menu (see gba_snes_menu.c
 * gba_menu_dbg_*), so both the per-state RENDER TIMING and the pixels are real.
 *
 *   fastboot oem menu-init           - bring the menu up headlessly (loads the
 *                                      pack + the real SD ROM roster); reports WxH
 *   fastboot oem menu-nav:<keys>     - drive it. keys = L R U D A B S T [ ]
 *                                      (same language as validate_menu.sh). Each
 *                                      key is one pressed frame + a settle; the
 *                                      per-key render us + static/changed gate
 *                                      decision come back, plus the worst frame.
 *                                      Leaves the final frame ON the panel.
 *   fastboot oem menu-shot           - dump the current frame downscaled (default
 *                                      /16 = 80x60) as RGB565 hex rows so I can
 *                                      SEE on-device content (reverse-punch frame,
 *                                      a "static" screen, etc). oem menu-shot:<n>
 *                                      overrides the downscale step.
 *
 * Results come back as fastboot INFO lines ("(bootloader) ...").
 */
#include "sd_fat.h"                 /* gba_rom_entry, gba_sd_mount, gba_sd_list_roms */

extern void fastboot_register(const char *prefix,
			      void (*handle)(const char *arg, void *data, unsigned sz),
			      int allow_locked, int need_download);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern int  snprintf(char *str, unsigned long size, const char *fmt, ...);

/* headless harness in gba_snes_menu.c */
extern int          gba_menu_dbg_init(const gba_rom_entry *roms, int nrom);
extern int          gba_menu_dbg_step(char c, int settle);
extern unsigned int gba_menu_dbg_render_us(void);
extern int          gba_menu_dbg_is_static(void);
extern int          gba_menu_dbg_take_launch(void);
extern void         gba_menu_dbg_present(void);
extern const unsigned int *gba_menu_dbg_fb(unsigned int *pitch, unsigned int *W, unsigned int *H);

static char lbuf[224];
static gba_rom_entry s_roms[64];
static int s_nrom;

static unsigned long parse_u(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == ':') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned long)(*s - '0'); s++; }
	return v;
}

static void cmd_menu_init(const char *arg, void *data, unsigned sz)
{
	fat_vol v;
	int rc, tot = 0;
	unsigned int pitch, W, H;
	(void)arg; (void)data; (void)sz;

	rc = gba_sd_mount(&v);
	if (rc != 0) { fastboot_fail("menu-init: SD mount failed (need a FAT32 card)"); return; }
	s_nrom = gba_sd_list_roms(&v, s_roms, (int)(sizeof s_roms / sizeof s_roms[0]), &tot);
	if (s_nrom <= 0) { fastboot_fail("menu-init: no ROMs under /roms/gba"); return; }

	rc = gba_menu_dbg_init(s_roms, s_nrom);
	if (rc != 0) {
		snprintf(lbuf, sizeof lbuf, "menu-init: harness init failed rc=%d (pack/DRAM)", rc);
		fastboot_fail(lbuf);
		return;
	}
	gba_menu_dbg_present();               /* show the home frame on the panel */
	(void)gba_menu_dbg_fb(&pitch, &W, &H);
	snprintf(lbuf, sizeof lbuf, "menu-init: OK  %ux%u  nrom=%d (total=%d)  render=%uus static=%d",
		 W, H, s_nrom, tot, gba_menu_dbg_render_us(), gba_menu_dbg_is_static());
	fastboot_info(lbuf);
	fastboot_okay("menu-init done");
}

static void cmd_menu_nav(const char *arg, void *data, unsigned sz)
{
	const char *p = arg;
	unsigned int worst = 0;
	int worst_i = -1, i = 0, launch;
	(void)data; (void)sz;

	while (*p == ' ' || *p == ':') p++;
	if (s_nrom <= 0) { fastboot_fail("menu-nav: run 'oem menu-init' first"); return; }

	for (; *p; p++, i++) {
		unsigned int us;
		int st;
		if (gba_menu_dbg_step(*p, 24) != 0) { fastboot_fail("menu-nav: harness not ready"); return; }
		us = gba_menu_dbg_render_us();
		st = gba_menu_dbg_is_static();
		if (us > worst) { worst = us; worst_i = i; }
		snprintf(lbuf, sizeof lbuf, "nav[%d] '%c'  render=%uus  %s",
			 i, *p, us, st ? "STATIC(gate-skips)" : "changed(presents)");
		fastboot_info(lbuf);
	}
	gba_menu_dbg_present();               /* leave the final frame on the panel */
	launch = gba_menu_dbg_take_launch();
	snprintf(lbuf, sizeof lbuf, "menu-nav: worst render=%uus at step %d  launch=%d",
		 worst, worst_i, launch);
	fastboot_info(lbuf);
	fastboot_okay("menu-nav done");
}

static void cmd_menu_shot(const char *arg, void *data, unsigned sz)
{
	static const char *hx = "0123456789abcdef";
	const unsigned int *fb;
	unsigned int pitch, W, H, x, y;
	int step = (int)parse_u(arg);
	(void)data; (void)sz;

	if (s_nrom <= 0) { fastboot_fail("menu-shot: run 'oem menu-init' first"); return; }
	if (step <= 0) step = 16;             /* default /16 -> 80x60 */
	fb = gba_menu_dbg_fb(&pitch, &W, &H);
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
			 * spill the rest of the row onto continuation lines with the same y. */
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
	fastboot_okay("menu-shot done");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem menu-init", cmd_menu_init, 1, 0);
	fastboot_register("oem menu-nav:", cmd_menu_nav, 1, 0);
	fastboot_register("oem menu-shot", cmd_menu_shot, 1, 0);
}
