/*
 * gbc_sd_run.c - run a GB/GBC ROM from the SD card through the loadable gambatte core.
 *
 * The GBA-from-SD selector (gba_snes_menu) hands a chosen ROM to gba_driver.c; when it is
 * a GB or GBC title (roms/gb, roms/gbc) the driver calls gbc_sd_session() here instead of
 * the gpSP path. We load the gambatte core blob (gbc_core_load, boot_b), point it at a DRAM
 * arena, load the ROM + its .sav from the card, then run the emulation frame loop: each
 * frame gbc_run() produces a 160x144 RGB565 image + audio which we blit (ayaneo_gbc_show_
 * frame, 6x) and stream (ayaneo_gbc_audio_submit), paced off the 13 MHz counter. The AYA
 * button saves and returns to the selector; on a DMG (.gb) game the L/R shoulders cycle the
 * monochrome palette.
 *
 * Only one core runs at a time, so we reuse the gpSP arena at 0x50000000.
 */
#include "gbc_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"

/* ---- LK / driver primitives (externs; no LK headers here) ---- */
extern const struct gbc_core_exports *gbc_core_load(void);   /* gbc_core_loader.c */
extern void     ayaneo_gbc_show_frame(const unsigned short *pix);
extern void     ayaneo_gbc_audio_init(void);
extern void     ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern unsigned gpt4_get_current_tick(void);
extern void     mtk_wdt_restart(void);
extern void     mtk_wdt_disable(void);
extern int      mt_get_gpio_in(unsigned pin);

/* pad GPIOs (match gba_driver.c). Active-low; AYA/L/R are not GB buttons so they are
 * free for the session controls. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86      /* return to the selector */
#define GPIO_LB        92      /* DMG palette prev */
#define GPIO_RB        81      /* DMG palette next */

/* GBA arena reuse (only one core runs at a time). */
#define GBC_ARENA      0x50000000u
#define GBC_ROM_BUF    (GBC_ARENA + 0x00000000u)
#define GBC_ROM_CAP    (8u * 1024 * 1024)
#define GBC_HEAP_BASE  (GBC_ARENA + 0x00800000u)
#define GBC_HEAP_SZ    (40u * 1024 * 1024)
#define GBC_VBUF       (GBC_ARENA + 0x03000000u)
#define GBC_SND        (GBC_ARENA + 0x03040000u)
#define GBC_W          160
#define GBC_H          144
#define GBC_SND_MAX    35208u   /* one 2097152 Hz frame of stereo samples */
#define GBC_FORCE_DMG  1u       /* gambatte::GB::FORCE_DMG */

/* DMG (monochrome) palette presets, lightest..darkest (0x00RRGGBB). Cycled with L/R on a
 * .gb game and applied to all three DMG palettes (BG, OBJ0, OBJ1). */
static const unsigned s_dmg_pal[][4] = {
	{ 0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000 },   /* grayscale */
	{ 0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F },   /* DMG green */
	{ 0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F },   /* pocket */
	{ 0xE0F8D0, 0x88C070, 0x346856, 0x081820 },   /* light green */
	{ 0xD8E8F8, 0x8098C0, 0x506890, 0x182848 },   /* blue */
};
#define DMG_PAL_COUNT (int)(sizeof(s_dmg_pal) / sizeof(s_dmg_pal[0]))

static void apply_dmg_palette(const struct gbc_core_exports *c, int idx)
{
	int p, col;
	if (idx < 0) idx = DMG_PAL_COUNT - 1;
	if (idx >= DMG_PAL_COUNT) idx = 0;
	for (p = 0; p < 3; p++)
		for (col = 0; col < 4; col++)
			c->set_dmg_palette_color((unsigned)p, (unsigned)col, s_dmg_pal[idx][col]);
}

/* Run one GB/GBC game to completion (AYA returns to the selector). Blocking. */
void gbc_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	static const struct gbc_core_exports *c;   /* blob loaded once, reused */
	unsigned char *rombuf = (unsigned char *)GBC_ROM_BUF;
	unsigned short *vbuf  = (unsigned short *)GBC_VBUF;
	unsigned int   *snd   = (unsigned int *)GBC_SND;
	unsigned romsz, flags;
	int is_dmg = (rom->type == GBA_CONSOLE_GB);
	int pal_idx = 1;                            /* default to DMG green for .gb */
	int aya_prev = 0, lb_prev = 0, rb_prev = 0;
	const unsigned long long TPF_NUM = 13000000ull * 35112ull;
	const unsigned long long TPF_DEN = 2097152ull;
	unsigned pace_base;
	unsigned long long pace_n = 0;

	if (!c) { c = gbc_core_load(); if (!c) return; }

	romsz = gba_sd_load_rom(vol, rom, rombuf, GBC_ROM_CAP);
	if (!romsz) return;

	c->heap_init((void *)GBC_HEAP_BASE, GBC_HEAP_SZ);
	if (c->create() != 0) return;
	flags = is_dmg ? GBC_FORCE_DMG : 0u;
	if (c->load(rombuf, romsz, flags) != 0) return;

	/* cartridge battery save (.sav) from the card, if any */
	if (c->savedata_ptr() && c->savedata_size())
		gba_sd_load_sav(vol, rom->name, (unsigned char *)c->savedata_ptr(), c->savedata_size());
	if (is_dmg)
		apply_dmg_palette(c, pal_idx);

	ayaneo_display_prepare();
	ayaneo_gbc_audio_init();
	mtk_wdt_disable();                          /* no kernel handoff; kick each frame */

	pace_base = gpt4_get_current_tick();
	for (;;) {
		unsigned samples = GBC_SND_MAX;
		long r = c->run(vbuf, GBC_W, snd, GBC_SND_MAX, &samples);

		ayaneo_gbc_audio_submit(snd, samples);

		if (r >= 0) {                       /* a video frame completed */
			int aya, lb, rb;
			mtk_wdt_restart();
			ayaneo_gbc_show_frame(vbuf);

			/* AYA -> save .sav and return to the selector */
			aya = PRESSED(GPIO_AYA);
			if (aya && !aya_prev) break;
			aya_prev = aya;

			/* L/R cycle the DMG palette (mono games only) */
			if (is_dmg) {
				lb = PRESSED(GPIO_LB); rb = PRESSED(GPIO_RB);
				if (lb && !lb_prev) { pal_idx = (pal_idx + DMG_PAL_COUNT - 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
				if (rb && !rb_prev) { pal_idx = (pal_idx + 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
				lb_prev = lb; rb_prev = rb;
			}

			/* pace to 59.7 Hz off the 13 MHz counter (cumulative, no drift) */
			pace_n++;
			{
				unsigned target = pace_base + (unsigned)(pace_n * TPF_NUM / TPF_DEN);
				while ((int)(gpt4_get_current_tick() - target) < 0)
					;
			}
		}
	}

	/* persist the cartridge battery save, then hand the codec back to the menu */
	if (c->savedata_ptr() && c->savedata_size())
		gba_sd_write_sav(vol, rom->name, (const unsigned char *)c->savedata_ptr(), c->savedata_size());
	ayaneo_menu_audio_silence();
}
