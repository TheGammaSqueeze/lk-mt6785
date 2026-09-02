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
extern void     ayaneo_gb_show_frame(const unsigned short *pix);   /* dedicated 160x144x6 GB/GBC path */
extern void     ayaneo_gbc_audio_init(void);
extern void     ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern unsigned gpt4_get_current_tick(void);
extern void     mtk_wdt_restart(void);
extern void     mtk_wdt_disable(void);
extern int      mt_get_gpio_in(unsigned pin);
extern int      ayaneo_get_preempt_frames(void);   /* run-ahead depth 0..3 (shared setting) */

/* pad GPIOs (match gba_driver.c). Active-low; AYA/L/R are not GB buttons so they are
 * free for the session controls. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86      /* return to the selector */
#define GPIO_LB        92      /* DMG palette prev */
#define GPIO_RB        81      /* DMG palette next */
#define GPIO_SELECT    90      /* + START + L + R held = soft reset */
#define GPIO_START     91
#define GPIO_B         82      /* held at launch = start fresh (skip resume) */
#define RESET_HOLD_FRAMES 30    /* ~0.5 s at 59.7 Hz */

/* Run-ahead + suspend/resume save states are layered on top of basic emulation and are
 * untested for gambatte. While the basic GB/GBC display + relaunch path is being brought
 * up on hardware, keep them OFF so they are not confounding variables (run-ahead does a
 * state save/load every frame; the suspend load runs at launch). Flip to 1 to re-enable
 * once the baseline is confirmed. */
#define GBC_ADVANCED 0

/* GBA arena reuse (only one core runs at a time). Non-overlapping regions with margin:
 * ROM (8 MB) | gambatte heap (24 MB) | video | audio | savestate scratch, all inside
 * [0x50000000, 0x53E00000) below the GBA driver's 2 MB tail reserve. The heap end
 * (0x52000000) is well clear of the video buffer so a large ROM cannot spill into it. */
#define GBC_ARENA      0x50000000u
#define GBC_ROM_BUF    (GBC_ARENA + 0x00000000u)   /* 0x50000000, 8 MB */
#define GBC_ROM_CAP    (8u * 1024 * 1024)
#define GBC_HEAP_BASE  (GBC_ARENA + 0x00800000u)   /* 0x50800000, 24 MB */
#define GBC_HEAP_SZ    (24u * 1024 * 1024)
#define GBC_VBUF       (GBC_ARENA + 0x02000000u)   /* 0x52000000 */
#define GBC_SND        (GBC_ARENA + 0x02100000u)   /* 0x52100000 */
#define GBC_AHEAD_STATE (GBC_ARENA + 0x02200000u)  /* 0x52200000, savestate scratch */
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
	const struct gbc_core_exports *c;   /* reloaded each session (cheap, ~20ms) so a
					     * clobbered blob or stale core state can never carry
					     * over between launches */
	unsigned char *rombuf = (unsigned char *)GBC_ROM_BUF;
	unsigned short *vbuf  = (unsigned short *)GBC_VBUF;
	unsigned int   *snd   = (unsigned int *)GBC_SND;
	unsigned romsz, flags;
	int is_dmg = (rom->type == GBA_CONSOLE_GB);
	int pal_idx = 1;                            /* default to DMG green for .gb */
	int aya_prev = 0, lb_prev = 0, rb_prev = 0;
	int reset_hold = 0;
	unsigned ahead_sz = 0;
	unsigned char *ahead = (unsigned char *)GBC_AHEAD_STATE;
	const unsigned long long TPF_NUM = 13000000ull * 35112ull;
	const unsigned long long TPF_DEN = 2097152ull;
	unsigned pace_base;
	unsigned long long pace_n = 0;

	c = gbc_core_load();
	if (!c) return;

	romsz = gba_sd_load_rom(vol, rom, rombuf, GBC_ROM_CAP);
	if (!romsz) return;

	c->heap_init((void *)GBC_HEAP_BASE, GBC_HEAP_SZ);
	if (c->create() != 0) return;
	flags = is_dmg ? GBC_FORCE_DMG : 0u;
	if (c->load(rombuf, romsz, flags) != 0) return;
	ahead_sz = c->state_size();
	if (ahead_sz > 0x00A00000u) ahead_sz = 0;   /* sanity: must fit the 10 MB scratch gap */

	/* cartridge battery save (.sav) from the card, if any. Clamp to the GB max SRAM
	 * (128 KB) so a bad size can never drive a runaway SD DMA. */
	{
		unsigned savsz = c->savedata_size();
		if (savsz > 128u * 1024u) savsz = 0;
		if (c->savedata_ptr() && savsz)
			gba_sd_load_sav(vol, rom->name, (unsigned char *)c->savedata_ptr(), savsz);
	}

	/* Suspend/resume: reload the save STATE written on the last exit so the game
	 * resumes where it was left, unless B is held (start fresh). Mirrors the GBA
	 * flow. The state file is keyed by the ROM name; gambatte validates it against
	 * the loaded ROM and no-ops on a mismatch. */
	if (GBC_ADVANCED && !PRESSED(GPIO_B)) {
		unsigned n = gba_sd_load_state(vol, rom->name, 0, ahead, 0x00A00000u);
		if (n)
			c->state_load(ahead, n);
	}
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
			int aya, lb, rb, combo, pf;
			mtk_wdt_restart();

			/* Soft reset: SELECT+START+L+R held ~0.5 s restarts the game (matches
			 * the GBA hotkey). While that combo is held, do NOT cycle the palette. */
			combo = PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) &&
				PRESSED(GPIO_LB) && PRESSED(GPIO_RB);
			if (combo) {
				if (++reset_hold >= RESET_HOLD_FRAMES) {
					c->reset();
					reset_hold = 0;
				}
			} else {
				reset_hold = 0;
			}

			/* AYA -> save .sav and return to the selector */
			aya = PRESSED(GPIO_AYA);
			if (aya && !aya_prev) break;
			aya_prev = aya;

			/* L/R cycle the DMG palette (mono games only, not during a reset combo) */
			if (is_dmg && !combo) {
				lb = PRESSED(GPIO_LB); rb = PRESSED(GPIO_RB);
				if (lb && !lb_prev) { pal_idx = (pal_idx + DMG_PAL_COUNT - 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
				if (rb && !rb_prev) { pal_idx = (pal_idx + 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
				lb_prev = lb; rb_prev = rb;
			}

			/* Run-ahead ("Preemptive Frames", shared setting): present a frame pf
			 * steps into the future with the current input, then rewind, so input
			 * latency drops by pf frames. gambatte's savestate carries the APU state,
			 * so (unlike gpSP) no separate sound-ring save is needed: the committed
			 * audio was already submitted above; the look-ahead runs are muted (their
			 * samples are simply not submitted). */
			pf = GBC_ADVANCED ? ayaneo_get_preempt_frames() : 0;
			if (pf > 0 && ahead_sz) {
				int i;
				c->state_save(ahead);
				for (i = 0; i < pf; i++) {
					unsigned s2 = GBC_SND_MAX;
					c->run(vbuf, GBC_W, snd, GBC_SND_MAX, &s2);   /* muted look-ahead */
				}
				ayaneo_gb_show_frame(vbuf);          /* present the future frame */
				c->state_load(ahead, ahead_sz);       /* rewind to the committed frame */
			} else {
				ayaneo_gb_show_frame(vbuf);
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

	/* persist the cartridge battery save + a suspend save STATE (so the next launch
	 * resumes), then hand the codec back to the menu */
	{
		unsigned savsz = c->savedata_size();
		if (savsz > 128u * 1024u) savsz = 0;
		if (c->savedata_ptr() && savsz)
			gba_sd_write_sav(vol, rom->name, (const unsigned char *)c->savedata_ptr(), savsz);
	}
	if (GBC_ADVANCED && ahead_sz) {
		c->state_save(ahead);
		gba_sd_write_state(vol, rom->name, 0, ahead, ahead_sz);
	}
	ayaneo_menu_audio_silence();
}
