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
#include <kernel/thread.h>
#include <kernel/event.h>

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
#define GPIO_B         82      /* held at launch = start fresh (skip resume); menu back */
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83      /* menu select */
#define RESET_HOLD_FRAMES 30    /* ~0.5 s at 59.7 Hz */

/* Run-ahead and suspend/resume are layered on basic emulation. The baseline (display,
 * relaunch, switch) is now stable on the dedicated gbc_emu thread, so both are enabled.
 * Kept as separate flags so either can be toggled if one misbehaves on hardware.
 *  - GBC_RUNAHEAD: present pf frames into the future with the current input, then rewind
 *    (per-frame gambatte state save/load; gambatte's state carries the APU).
 *  - GBC_SUSPEND:  save a state on exit and reload it on launch (resume where you left). */
#define GBC_RUNAHEAD 1
#define GBC_SUSPEND  1

/* Emulation CPU OPP by run-ahead tier (mirrors the GBA preempt tiers Off/Bal/Resp/Max):
 * run-ahead runs (pf+1) emulations per displayed frame, so escalate the clock with pf.
 * The menu runs at 2000 MHz; the game does not need that, and a lower clock saves power. */
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
static const unsigned s_gbc_opp[4] = { 600, 1000, 1200, 1400 };

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

/* ---- in-game overlay menu (GammaOS Pico), mirrors the GBA menu in gba_driver.c.
 * AYA toggles it; the game keeps running underneath with its input suppressed. ---- */
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch, int x, int y, int scale, unsigned int argb, const char *s);
extern int  ayaneo_brightness_pct(void);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_get_lcd_filter(void);
extern void ayaneo_set_lcd_filter(int v);
extern void ayaneo_set_preempt_frames(int v);
extern void ayaneo_menu_settings_persist(void);

volatile int g_gbc_menu_open;   /* read by ayaneo_gbc_pad_mask (gba_driver.c) to gate game input */

/* session context the menu acts on, set at session start */
static const struct gbc_core_exports *s_menu_c;
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static unsigned char       *s_menu_ahead;
static unsigned             s_menu_ahead_sz;
static int                  s_menu_is_dmg;
static int                 *s_menu_pal;      /* -> session pal_idx */
static int  s_msel;
static char s_mstat[48];

enum { GM_BRIGHT, GM_VOLUME, GM_FILTER, GM_PALETTE, GM_PREEMPT, GM_SAVE, GM_LOAD, GM_RESET, GM_CLOSE, GM_COUNT };

static char *mput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mputu(char *p, unsigned v) { char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n]; return p; }

static const char *gm_label(int i)
{
	switch (i) {
	case GM_BRIGHT:  return "Brightness";
	case GM_VOLUME:  return "Volume";
	case GM_FILTER:  return "LCD Filter";
	case GM_PALETTE: return "Palette";
	case GM_PREEMPT: return "Preemptive Frames";
	case GM_SAVE:    return "Save State";
	case GM_LOAD:    return "Load State";
	case GM_RESET:   return "Reset Game";
	case GM_CLOSE:   return "Close";
	}
	return "";
}
static const char *filt_name(int f) { return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : "Off"; }
static const char *gm_value(int i, char *buf)
{
	char *p = buf;
	switch (i) {
	case GM_BRIGHT:  p = mputu(p, (unsigned)ayaneo_brightness_pct()); p = mput(p, "%"); break;
	case GM_VOLUME:  p = mputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mput(p, "%"); break;
	case GM_FILTER:  p = mput(p, filt_name(ayaneo_get_lcd_filter())); break;
	case GM_PALETTE: if (s_menu_is_dmg) { p = mput(p, "< "); p = mputu(p, (unsigned)((s_menu_pal ? *s_menu_pal : 0) + 1)); p = mput(p, " >"); }
			 else p = mput(p, "CGB"); break;
	case GM_PREEMPT: { int pf = ayaneo_get_preempt_frames();
			   p = mput(p, pf == 0 ? "Off" : pf == 1 ? "Balanced" : pf == 2 ? "Responsive" : "Max"); } break;
	case GM_SAVE: case GM_LOAD: p = mput(p, "[A]"); break;
	default: break;
	}
	*p = 0; return buf;
}
/* returns 0 = stay in menu, 1 = close menu (resume game), 2 = exit to the selector */
static int gm_change(int i, int dir, int act)
{
	int changed = 1;
	s_mstat[0] = 0;
	switch (i) {
	case GM_BRIGHT:  if (dir) ayaneo_brightness_step(dir); else changed = 0; break;
	case GM_VOLUME:  if (dir) ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); else changed = 0; break;
	case GM_FILTER:  if (dir) ayaneo_set_lcd_filter((ayaneo_get_lcd_filter() + dir + 4) % 4); else changed = 0; break;
	case GM_PALETTE: if (dir && s_menu_is_dmg && s_menu_pal) { *s_menu_pal = (*s_menu_pal + dir + DMG_PAL_COUNT) % DMG_PAL_COUNT; apply_dmg_palette(s_menu_c, *s_menu_pal); } changed = 0; break;
	case GM_PREEMPT: if (dir) ayaneo_set_preempt_frames((ayaneo_get_preempt_frames() + dir + 4) % 4); else changed = 0; break;
	case GM_SAVE:    if (act && s_menu_ahead_sz) { s_menu_c->state_save(s_menu_ahead);
			     mput(s_mstat, gba_sd_write_state(s_menu_vol, s_menu_rom->name, 0, s_menu_ahead, s_menu_ahead_sz) == 0 ? "State saved" : "Save failed"); }
			 changed = 0; break;
	case GM_LOAD:    if (act && s_menu_ahead_sz) { unsigned n = gba_sd_load_state(s_menu_vol, s_menu_rom->name, 0, s_menu_ahead, 0x00A00000u);
			     mput(s_mstat, (n && s_menu_c->state_load(s_menu_ahead, n) == 0) ? "State loaded" : "No save state"); }
			 changed = 0; break;
	case GM_RESET:   if (act) { s_menu_c->reset(); return 1; } changed = 0; break;
	case GM_CLOSE:   if (act) return 2; changed = 0; break;
	default: changed = 0; break;
	}
	if (changed) ayaneo_menu_settings_persist();
	return 0;
}

int gbc_menu_open(void) { return g_gbc_menu_open; }
/* called by ayaneo_gb_show_frame (mt_disp_drv.c) after the game frame */
void gbc_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 660, panelH = 84 + GM_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2;
	int x = px + 28, y = py + 84, i; char val[48];
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");
	for (i = 0; i < GM_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_msel) ? 0xFF101018u : 0xFFC8D0E0u; int vw;
		if (i == s_msel) ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, gm_label(i));
		gm_value(i, val); for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_mstat[0]) ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_mstat);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* Run one GB/GBC game to completion (menu Close returns to the selector). Runs on the
 * dedicated gbc_emu thread (see gbc_sd_session below), not on emu_thread. */
static void gbc_session_body(fat_vol *vol, const gba_rom_entry *rom)
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
	if (GBC_SUSPEND && !PRESSED(GPIO_B)) {
		unsigned n = gba_sd_load_state(vol, rom->name, 0, ahead, 0x00A00000u);
		if (n)
			c->state_load(ahead, n);
	}
	if (is_dmg)
		apply_dmg_palette(c, pal_idx);

	/* Drop from the menu's 2000 MHz to the run-ahead-tier emulation clock (Off = 600,
	 * escalating with pf so the (pf+1) emulations per frame still fit the 16.7 ms budget). */
	{
		int pf = ayaneo_get_preempt_frames();
		ayaneo_set_cpu_mhz(s_gbc_opp[(pf >= 0 && pf <= 3) ? pf : 0]);
	}

	ayaneo_gbc_audio_init();
	mtk_wdt_disable();                          /* no kernel handoff; kick each frame */

	/* Launch punch-hole transition (like GBA): the selector captured a frozen menu
	 * snapshot (0x54000000) and set gba_punch_ready. Grow a circle over it revealing
	 * the running game. Run a few frames first so the opening shows real content, not
	 * the boot screen. Reuses the geometry-agnostic composite (frame_pre) with a GB
	 * prerender (160x144x6).
	 *
	 * ayaneo_display_prepare() is DELIBERATELY skipped when a punch is pending: it
	 * memsets BOTH fb buffers to black (including the one the OVL is scanning out),
	 * which blanks the live frozen-menu snapshot to black for the ~20 silent emulation
	 * frames before the punch starts = the menu->game flicker (GBA never calls it here,
	 * so it did not flicker). The punch composite takes over the display seamlessly and
	 * clear_letterbox blacks only the borders. Only when there is NO snapshot to punch
	 * over do we fall back to the black-fill prepare. */
	{
		extern int  gba_punch_ready;
		extern void ayaneo_gb_punch_prerender(const unsigned short *pix);
		extern void ayaneo_gba_punch_frame_pre(const unsigned int *snap, int radius);
		extern void ayaneo_gbc_clear_letterbox(void);
		if (!gba_punch_ready)
			ayaneo_display_prepare();   /* fallback: no snapshot, clear to black */
		if (gba_punch_ready) {
			int i, w;
			gba_punch_ready = 0;
			for (w = 0; w < 20; w++) {
				unsigned s2 = GBC_SND_MAX;
				if (c->run(vbuf, GBC_W, snd, GBC_SND_MAX, &s2) >= 0 && w >= 3) break;
			}
			ayaneo_gb_punch_prerender(vbuf);
			for (i = 1; i <= 20; i++) {
				int r = (int)((long long)820 * i / 20);   /* 820 = GBA_PUNCH_MAX_R */
				if (r < 1) r = 1;
				ayaneo_gba_punch_frame_pre((const unsigned int *)0x54000000u, r);
				mtk_wdt_restart();
			}
			ayaneo_gbc_clear_letterbox();
		}
	}

	/* hand the in-game menu this session's context */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom;
	s_menu_ahead = ahead; s_menu_ahead_sz = ahead_sz;
	s_menu_is_dmg = is_dmg; s_menu_pal = &pal_idx;
	s_msel = 0; s_mstat[0] = 0; g_gbc_menu_open = 0;

	pace_base = gpt4_get_current_tick();
	{
	int up_prev = 0, dn_prev = 0, lt_prev = 0, rt_prev = 0, a_prev = 0, b_prev = 0;
	int aya_hold = 0;
	for (;;) {
		unsigned samples = GBC_SND_MAX;
		long r = c->run(vbuf, GBC_W, snd, GBC_SND_MAX, &samples);

		ayaneo_gbc_audio_submit(snd, samples);

		if (r >= 0) {                       /* a video frame completed */
			int combo, pf, aya;
			mtk_wdt_restart();

			/* AYA taps toggle the in-game menu (game keeps running underneath).
			 * Holding AYA ~1.5 s force-exits to the selector - a safety so a broken
			 * menu can never soft-lock the game (the watchdog is disabled here). */
			aya = PRESSED(GPIO_AYA);
			if (aya && !aya_prev) { g_gbc_menu_open = !g_gbc_menu_open; s_mstat[0] = 0; }
			aya_prev = aya;
			if (aya) { if (++aya_hold >= 90) break; } else aya_hold = 0;

			if (g_gbc_menu_open) {
				/* menu nav: dpad move/change, A select, B close. The game's own
				 * input is gated off in ayaneo_gbc_pad_mask while the menu is open. */
				int res = 0;
				int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
				int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
				int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
				if (up && !up_prev) s_msel = (s_msel + GM_COUNT - 1) % GM_COUNT;
				if (dn && !dn_prev) s_msel = (s_msel + 1) % GM_COUNT;
				if (lt && !lt_prev) res = gm_change(s_msel, -1, 0);
				if (rt && !rt_prev) res = gm_change(s_msel, +1, 0);
				if (a  && !a_prev)  res = gm_change(s_msel, 0, 1);
				if (b  && !b_prev)  g_gbc_menu_open = 0;
				up_prev = up; dn_prev = dn; lt_prev = lt; rt_prev = rt; a_prev = a; b_prev = b;
				if (res == 2) {                    /* Close -> exit to the selector */
					extern void gbc_menu_arm_reverse(const unsigned short *frame);
					gbc_menu_arm_reverse(vbuf);   /* closing punch: shrink into the menu */
					break;
				}
				if (res == 1) g_gbc_menu_open = 0; /* Reset -> close menu, game runs */
			} else {
				/* Soft reset hotkey: SELECT+START+L+R held ~0.5 s. */
				combo = PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) &&
					PRESSED(GPIO_LB) && PRESSED(GPIO_RB);
				if (combo) { if (++reset_hold >= RESET_HOLD_FRAMES) { c->reset(); reset_hold = 0; } }
				else reset_hold = 0;
				/* L/R cycle the DMG palette (mono games only, not during the combo) */
				if (is_dmg && !combo) {
					int lb = PRESSED(GPIO_LB), rb = PRESSED(GPIO_RB);
					if (lb && !lb_prev) { pal_idx = (pal_idx + DMG_PAL_COUNT - 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
					if (rb && !rb_prev) { pal_idx = (pal_idx + 1) % DMG_PAL_COUNT; apply_dmg_palette(c, pal_idx); }
					lb_prev = lb; rb_prev = rb;
				}
			}

			/* Run-ahead: present pf frames into the future then rewind. Off while the
			 * menu is open (do not race the game ahead under the overlay, and a menu
			 * Load State just replaced the state). ayaneo_gb_show_frame paints the
			 * menu overlay on top when g_gbc_menu_open. */
			pf = (!g_gbc_menu_open && GBC_RUNAHEAD) ? ayaneo_get_preempt_frames() : 0;
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
	}
	g_gbc_menu_open = 0;

	/* persist the cartridge battery save + a suspend save STATE (so the next launch
	 * resumes), then hand the codec back to the menu */
	{
		unsigned savsz = c->savedata_size();
		if (savsz > 128u * 1024u) savsz = 0;
		if (c->savedata_ptr() && savsz)
			gba_sd_write_sav(vol, rom->name, (const unsigned char *)c->savedata_ptr(), savsz);
	}
	if (GBC_SUSPEND && ahead_sz) {
		c->state_save(ahead);
		gba_sd_write_state(vol, rom->name, 0, ahead, ahead_sz);
	}
	ayaneo_menu_audio_silence();
}

/* ---- dedicated emulation thread ----
 * For consistency with the gpSP core (which emulates on its own gba_cpu thread), the
 * gambatte core runs its session on a DEDICATED thread with its own large stack,
 * instead of on emu_thread (deep C++ on emu_thread's 64 KB overflowed and crashed the
 * next SD DMA). Created LAZILY on the first GB/GBC launch and reused (this LK does not
 * reap exited threads, so a per-session thread would leak its stack); emu_thread kicks
 * it and blocks on the done event, so exactly one core emulates at a time. gambatte is
 * a synchronous core (gbc_run does a whole frame), so a single session thread is the
 * right shape - it does NOT need gpSP's producer/consumer CPU/main split. */
static thread_t *s_gbc_thread;
static event_t   s_gbc_kick, s_gbc_done;
static fat_vol  *s_gbc_vol;
static const gba_rom_entry *s_gbc_rom;

static int gbc_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_gbc_kick);
		gbc_session_body(s_gbc_vol, s_gbc_rom);
		event_signal(&s_gbc_done, false);
	}
	return 0;
}

/* Called from emu_thread's ROM-select dispatch. Runs the game on the gbc_emu thread
 * and blocks here until it returns to the selector (AYA). */
void gbc_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	if (!s_gbc_thread) {
		event_init(&s_gbc_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_gbc_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_gbc_thread = thread_create("gbc_emu", &gbc_thread_fn, NULL,
					     DEFAULT_PRIORITY, 262144);
		if (!s_gbc_thread) { gbc_session_body(vol, rom); return; }  /* fallback: inline */
		thread_resume(s_gbc_thread);
	}
	s_gbc_vol = vol;
	s_gbc_rom = rom;
	event_signal(&s_gbc_kick, false);   /* start the session on the emu thread */
	event_wait(&s_gbc_done);            /* block until it returns to the selector */
}
