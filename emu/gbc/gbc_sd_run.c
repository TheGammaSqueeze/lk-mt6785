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
#define GPIO_X         85      /* PHYSICAL X button (GBA driver labels 84/85 X/Y for
                                * autofire, which reads swapped vs the printed caps; the
                                * menu uses the physically-labelled X to reset an option) */
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
extern unsigned int ayaneo_get_cpu_mhz(void);
static const unsigned s_gbc_opp[4] = { 600, 1000, 1200, 1400 };

/* Manual CPU-clock OPP grid for the in-game "CPU Clock" menu row (mirrors the GBA
 * driver's grid). ayaneo_set_cpu_mhz only reprograms the ARM-PLL PCW, NOT the core
 * voltage (LK has no DVFS table), so anything above the boot Vproc point is the user's
 * call. s_cpu_idx is lazily seeded from the live clock on first use. */
static const unsigned s_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int s_cpu_idx = -1;
static void gbc_cpu_step(int dir)
{
	int n = (int)(sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0])), i;
	if (s_cpu_idx < 0) {
		unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u; int best = 0;
		for (i = 0; i < n; i++) {
			unsigned d = s_cpu_opp[i] > cur ? s_cpu_opp[i] - cur : cur - s_cpu_opp[i];
			if (d < bd) { bd = d; best = i; }
		}
		s_cpu_idx = best;
	}
	s_cpu_idx += dir;
	if (s_cpu_idx < 0) s_cpu_idx = 0;
	if (s_cpu_idx >= n) s_cpu_idx = n - 1;
	ayaneo_set_cpu_mhz(s_cpu_opp[s_cpu_idx]);
}

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

/* GB colorization palettes now come from the gambatte core catalogue (gbcpalettes.h:
 * the real GB/GBC/SGB/Special + TWB64/community list, ~600 entries) rather than a few
 * hand-picked presets. The core owns the data; we browse it by index (count/name) and
 * install one (apply). Only DMG (.gb) games use these; GBC/SGB carts colour themselves. */
static int dmg_pal_count(const struct gbc_core_exports *c)
{
	int n = c->dmg_palette_count ? (int)c->dmg_palette_count() : 1;
	return n > 0 ? n : 1;
}

static void apply_dmg_palette(const struct gbc_core_exports *c, int idx)
{
	int n = dmg_pal_count(c);
	if (idx < 0) idx = n - 1;
	if (idx >= n) idx = 0;
	if (c->dmg_palette_apply) c->dmg_palette_apply((unsigned)idx);
}

/* CGB colour knobs (mainly for GBC/colour games; harmless on DMG). Remembered across
 * sessions in this LK boot. Color correction simulates the washed-out GBC LCD gamut;
 * the mode is Accurate (gamut-correct) vs Fast; the dark filter dims the whole image. */
static int s_cc_on   = 1;   /* color correction on by default (gambatte CGB default) */
static int s_cc_mode = 0;   /* 0 = Accurate, 1 = Fast */
static int s_dark    = 0;   /* dark filter level, 0..100 % */

static void apply_color_knobs(const struct gbc_core_exports *c)
{
	if (c->set_color_correction)      c->set_color_correction(s_cc_on);
	if (c->set_color_correction_mode) c->set_color_correction_mode((unsigned)s_cc_mode);
	if (c->set_dark_filter)           c->set_dark_filter((unsigned)s_dark);
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

/* Palette LIST picker (A on the Palette row opens it): hold Up/Down to browse with
 * accelerating auto-repeat, applied live each step; A applies, B restores the previous
 * selection. State survives across sessions on the reused gbc thread, so it is reset at
 * session start. */
static int  s_pal_pick;         /* picker overlay active */
static int  s_pal_pick_saved;   /* index to restore on cancel (B) */
static int  s_pick_dir;         /* current held direction (-1/0/+1) */
static int  s_pick_hold;        /* frames the direction has been held */
static int  s_pick_tick;        /* frames since the last auto-repeat step */

enum { GM_BRIGHT, GM_VOLUME, GM_FILTER, GM_PALETTE, GM_COLORCC, GM_CCMODE, GM_DARK,
       GM_PREEMPT, GM_CPU, GM_SAVE, GM_LOAD, GM_RESET, GM_CLOSE, GM_COUNT };

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
	case GM_COLORCC: return "Color Correction";
	case GM_CCMODE:  return "Correction Mode";
	case GM_DARK:    return "Dark Filter";
	case GM_PREEMPT: return "Preemptive Frames";
	case GM_CPU:     return "CPU Clock";
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
	case GM_PALETTE: if (s_menu_is_dmg && s_menu_c && s_menu_c->dmg_palette_name) {
				 const char *nm = s_menu_c->dmg_palette_name((unsigned)(s_menu_pal ? *s_menu_pal : 0));
				 int nl = 0; while (nm[nl] && nl < 30) nl++;   /* clamp long TWB64 names */
				 p = mput(p, "< "); { int j; for (j = 0; j < nl; j++) *p++ = nm[j]; } p = mput(p, " >");
			 } else p = mput(p, "CGB"); break;
	case GM_COLORCC: p = mput(p, s_cc_on ? "On" : "Off"); break;
	case GM_CCMODE:  p = mput(p, s_cc_mode ? "Fast" : "Accurate"); break;
	case GM_DARK:    if (s_dark <= 0) p = mput(p, "Off"); else { p = mputu(p, (unsigned)s_dark); p = mput(p, "%"); } break;
	case GM_PREEMPT: { int pf = ayaneo_get_preempt_frames();
			   p = mput(p, pf == 0 ? "Off" : pf == 1 ? "Balanced" : pf == 2 ? "Responsive" : "Max"); } break;
	case GM_CPU:     p = mputu(p, ayaneo_get_cpu_mhz()); p = mput(p, " MHz"); break;
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
	case GM_PALETTE: if (s_menu_is_dmg && s_menu_pal) {
				 if (act == 1) {   /* A = open the full palette list picker */
					 s_pal_pick_saved = *s_menu_pal;
					 s_pick_dir = 0; s_pick_hold = 0; s_pick_tick = 0;
					 s_pal_pick = 1;
				 } else if (act == 2) {   /* X = reset to the default GBC (automatic) palette */
					 *s_menu_pal = s_menu_c->dmg_palette_default ? (int)s_menu_c->dmg_palette_default() : 0;
					 apply_dmg_palette(s_menu_c, *s_menu_pal);
				 } else if (dir) { int n = dmg_pal_count(s_menu_c); *s_menu_pal = (*s_menu_pal + dir + n) % n; apply_dmg_palette(s_menu_c, *s_menu_pal); }
			 } changed = 0; break;
	case GM_COLORCC: if (dir) { s_cc_on = !s_cc_on; apply_color_knobs(s_menu_c); } changed = 0; break;
	case GM_CCMODE:  if (dir) { s_cc_mode = !s_cc_mode; apply_color_knobs(s_menu_c); } changed = 0; break;
	case GM_DARK:    if (dir) { s_dark += dir * 10; if (s_dark < 0) s_dark = 0; if (s_dark > 100) s_dark = 100; apply_color_knobs(s_menu_c); } changed = 0; break;
	case GM_PREEMPT: if (dir) ayaneo_set_preempt_frames((ayaneo_get_preempt_frames() + dir + 4) % 4); else changed = 0; break;
	case GM_CPU:     if (dir) gbc_cpu_step(dir); changed = 0; break;
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

/* Palette LIST picker overlay: a scrolling window of palette names centred on the live
 * selection (which is applied to the running game behind the panel, so the colours update
 * as you browse). Drawn instead of the Pico menu while s_pal_pick. */
static void gbc_pal_pick_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	const struct gbc_core_exports *c = s_menu_c;
	int n = dmg_pal_count(c);
	int cur = s_menu_pal ? *s_menu_pal : 0;
	const int rows = 11, half = rows / 2, rowH = 40;
	int panelW = 760, panelH = 80 + rows * rowH + 40;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2;
	int r, y = py + 80;
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "Select Palette");
	for (r = -half; r <= half; r++, y += rowH) {
		int idx = cur + r; char line[64]; char *pp = line; const char *nm; int k;
		unsigned int fg;
		if (idx < 0 || idx >= n) continue;
		fg = (r == 0) ? 0xFF101018u : 0xFFC8D0E0u;
		if (r == 0) ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		nm = c->dmg_palette_name ? c->dmg_palette_name((unsigned)idx) : "";
		pp = mputu(pp, (unsigned)(idx + 1)); pp = mput(pp, "  ");
		for (k = 0; nm[k] && k < 34; k++) *pp++ = nm[k];
		*pp = 0;
		ayaneo_text(buf, pitch, px + 28, y, 2, fg, line);
	}
	ayaneo_text(buf, pitch, px + 28, py + panelH - 28, 1, 0xFF8890A0u,
		    "Up/Down browse (hold to accelerate)   A apply   B cancel");
}

/* called by ayaneo_gb_show_frame (mt_disp_drv.c) after the game frame */
void gbc_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 780, panelH;
	int px, py, x, y, i; char val[48];
	if (s_pal_pick) { gbc_pal_pick_paint(buf, pitch, W, H); return; }
	panelH = 84 + GM_COUNT * rowH + 42;   /* wide enough for long palette names */
	px = ((int)W - panelW) / 2; py = ((int)H - panelH) / 2;
	x = px + 28; y = py + 84;
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
	int pal_idx = 0;                            /* set to the core's default GBC palette once loaded */
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
	if (is_dmg) {
		pal_idx = c->dmg_palette_default ? (int)c->dmg_palette_default() : 0;
		apply_dmg_palette(c, pal_idx);
	}
	apply_color_knobs(c);   /* CGB colour correction / dark filter (remembered settings) */

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
	s_msel = 0; s_mstat[0] = 0; g_gbc_menu_open = 0; s_pal_pick = 0;

	pace_base = gpt4_get_current_tick();
	{
	int up_prev = 0, dn_prev = 0, lt_prev = 0, rt_prev = 0, a_prev = 0, b_prev = 0, x_prev = 0;
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
			if (aya && !aya_prev) { g_gbc_menu_open = !g_gbc_menu_open; s_mstat[0] = 0; if (!g_gbc_menu_open) s_pal_pick = 0; }
			aya_prev = aya;
			if (aya) { if (++aya_hold >= 90) break; } else aya_hold = 0;

			if (g_gbc_menu_open && s_pal_pick) {
				/* Palette LIST picker: hold Up/Down to browse with accelerating
				 * auto-repeat (applied live each step); A applies, B restores the
				 * pre-open selection. Direction is polled every frame (not edge). */
				int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
				int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
				int dir = up ? -1 : dn ? +1 : 0;
				int move = 0, step = 1;
				if (dir == 0) { s_pick_dir = 0; s_pick_hold = 0; s_pick_tick = 0; }
				else if (dir != s_pick_dir) {        /* fresh press = one immediate step */
					s_pick_dir = dir; s_pick_hold = 0; s_pick_tick = 0; move = 1; step = 1;
				} else {                             /* held: accelerate the repeat */
					int interval, st;
					s_pick_hold++;
					if      (s_pick_hold < 16)  { interval = 0;  st = 0;  }   /* initial pause */
					else if (s_pick_hold < 48)  { interval = 6;  st = 1;  }
					else if (s_pick_hold < 96)  { interval = 3;  st = 2;  }
					else if (s_pick_hold < 160) { interval = 2;  st = 5;  }
					else                        { interval = 1;  st = 12; }
					if (interval > 0 && ++s_pick_tick >= interval) { s_pick_tick = 0; move = 1; step = st; }
				}
				if (move && s_menu_pal) {
					int n = dmg_pal_count(s_menu_c), v = *s_menu_pal + dir * step;
					while (v < 0) v += n; while (v >= n) v -= n;   /* wrap */
					*s_menu_pal = v; apply_dmg_palette(s_menu_c, v);
				}
				if (a && !a_prev) s_pal_pick = 0;                    /* apply (already live) */
				if (b && !b_prev) {                                  /* cancel: restore */
					if (s_menu_pal) { *s_menu_pal = s_pal_pick_saved; apply_dmg_palette(s_menu_c, s_pal_pick_saved); }
					s_pal_pick = 0;
				}
				a_prev = a; b_prev = b;
				up_prev = up; dn_prev = dn;   /* keep fresh so returning to nav is clean */
			} else if (g_gbc_menu_open) {
				/* menu nav: dpad move/change, A select, B close. The game's own
				 * input is gated off in ayaneo_gbc_pad_mask while the menu is open. */
				int res = 0;
				int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
				int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
				int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B), x = PRESSED(GPIO_X);
				if (up && !up_prev) s_msel = (s_msel + GM_COUNT - 1) % GM_COUNT;
				if (dn && !dn_prev) s_msel = (s_msel + 1) % GM_COUNT;
				if (lt && !lt_prev) res = gm_change(s_msel, -1, 0);
				if (rt && !rt_prev) res = gm_change(s_msel, +1, 0);
				if (a  && !a_prev)  res = gm_change(s_msel, 0, 1);
				if (x  && !x_prev)  res = gm_change(s_msel, 0, 2);  /* X = reset option to default */
				if (b  && !b_prev)  g_gbc_menu_open = 0;
				up_prev = up; dn_prev = dn; lt_prev = lt; rt_prev = rt; a_prev = a; b_prev = b; x_prev = x;
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
					int lb = PRESSED(GPIO_LB), rb = PRESSED(GPIO_RB), n = dmg_pal_count(c);
					if (lb && !lb_prev) { pal_idx = (pal_idx + n - 1) % n; apply_dmg_palette(c, pal_idx); }
					if (rb && !rb_prev) { pal_idx = (pal_idx + 1) % n; apply_dmg_palette(c, pal_idx); }
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
