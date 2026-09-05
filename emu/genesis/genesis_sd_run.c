/*
 * genesis_sd_run.c - run a Sega Genesis/MD, Master System, Game Gear or SG-1000 ROM from the SD
 * card through the loadable Genesis-Plus-GX core blob. Mirrors emu/gbc/gbc_sd_run.c: a dedicated
 * emulation thread (emu_thread's 64 KB stack overflows on a full core), the shared 0x50000000
 * arena, ROM/SRAM/state from the card. BASIC bring-up first (display + input + audio + SRAM +
 * suspend-resume); the parity extras (adaptive fast-forward + HUD, rewind, run-ahead, aspect
 * Fit/Stretch, Pico menu, GPGX core options) are layered on afterwards - see GENESIS_PORT_PROGRESS.md.
 *
 * Only one core runs at a time, so we reuse the gpSP/gambatte/snes9x arena at 0x50000000.
 */
#include "genesis_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"
#include "../ayaneo_rewind.h"
#include <kernel/thread.h>
#include <kernel/event.h>

/* ---- LK / driver primitives (externs; no LK headers here) ---- */
extern const struct genesis_core_exports *genesis_core_load(void);   /* genesis_core_loader.c */
extern void     ayaneo_genesis_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern unsigned gpt4_get_current_tick(void);
extern void     mtk_wdt_restart(void);
extern int      mt_get_gpio_in(unsigned pin);
extern void     ayaneo_joypad_poll(void);
extern unsigned int ayaneo_joypad_dpad(void);
extern void     ayaneo_hud_set(int mode, int speed_x10);   /* mt_disp_drv.c: FF/RW speed badge */
extern void     ayaneo_set_cpu_mhz(unsigned int mhz);      /* reprograms ARM-PLL PCW (not voltage) */
extern unsigned int ayaneo_get_cpu_mhz(void);

/* pad GPIOs (match gba_driver.c / gbc_sd_run.c). Active-low. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86      /* return to the selector */
#define GPIO_LB        92
#define GPIO_RB        81
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82      /* held at launch = start fresh (skip resume) */
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83
#define GPIO_X         85
#define GPIO_Y         84

/* retro joypad bit positions (RETRO_DEVICE_ID_JOYPAD_*) */
#define RJ_B 0
#define RJ_Y 1
#define RJ_SELECT 2
#define RJ_START 3
#define RJ_UP 4
#define RJ_DOWN 5
#define RJ_LEFT 6
#define RJ_RIGHT 7
#define RJ_A 8
#define RJ_X 9
#define RJ_L 10
#define RJ_R 11

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). GPGX with -DUSE_DYNAMIC_ALLOC
 * malloc()s cart.rom (up to 32 MB) + work RAM + VRAM + the render bitmap + sound state out of the
 * heap, so the heap must be large. Layout below stays inside the pre-mapped [0x50000000,0x54000000)
 * GBA arena (no custom MMU map, unlike snes9x - so no unmapped-tail savestate gotcha). */
#define GEN_HEAP_BASE  0x50000000u
#define GEN_HEAP_SZ    0x02C00000u        /* 44 MB */
#define GEN_ROM_BUF    0x52C00000u        /* SD ROM read here, handed to load() (GPGX copies it) */
#define GEN_ROM_CAP    0x00800000u        /* 8 MB (largest MD carts ~8 MB) */
#define GEN_STATE_BUF  0x53400000u        /* save/suspend state scratch */
#define GEN_STATE_CAP  0x00400000u        /* 4 MB (GPGX state ~0.5-1 MB) */
#define GEN_AHEAD_BUF  0x53800000u        /* run-ahead save/restore scratch (separate from state) */
#define GEN_REWIND_MAX_SPD 1536           /* max rewind speed in 256ths (1536 = 6x); floor 256 = 1x */

/* run-ahead CPU clock by depth: pf frames run pf+1 emulations/display + 1 save + 1 load, so
 * escalate the clock with depth (mirrors the snes s_snes_ra_opp). ayaneo_set_cpu_mhz is PLL-only. */
static const unsigned s_gen_ra_opp[4] = { 1400, 1600, 1800, 2000 };

volatile int g_genesis_menu_open;         /* gates game input + FF/rewind while the Pico menu is up */
volatile int g_genesis_aspect;            /* display aspect: 0=Pixel 1=Fit 2=Stretch (ayaneo_genesis_show_frame) */
volatile unsigned g_genesis_aspect_x1000; /* core display aspect * 1000 (Fit target); 0 -> 4:3 */
volatile int g_genesis_filter;            /* LCD filter: 0=off 1=scanlines 2/3=grid */

/* physical pad -> RETRO_DEVICE_ID_JOYPAD_* bits. GPGX maps retro -> Genesis internally:
 * Genesis B=retro B, C=retro A, A=retro Y, Start=retro Start; 6-button X=retro L, Y=retro X,
 * Z=retro R, Mode=retro Select. So the device face buttons land on sensible Genesis buttons. */
unsigned ayaneo_genesis_pad_mask(unsigned port)
{
	unsigned m = 0, d;
	if (port != 0 || g_genesis_menu_open) return 0;
	if (PRESSED(GPIO_B))      m |= 1u << RJ_B;
	if (PRESSED(GPIO_A))      m |= 1u << RJ_A;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	d = ayaneo_joypad_dpad();   /* left analog stick -> D-pad */
	if (d & 0x01u) m |= 1u << RJ_UP;
	if (d & 0x02u) m |= 1u << RJ_DOWN;
	if (d & 0x04u) m |= 1u << RJ_LEFT;
	if (d & 0x08u) m |= 1u << RJ_RIGHT;
	return m;
}

static int rom_type_to_system(unsigned char type)
{
	switch (type) {
	case GBA_CONSOLE_SMS: return GEN_SYS_SMS;
	case GBA_CONSOLE_GG:  return GEN_SYS_GG;
	case GBA_CONSOLE_SG:  return GEN_SYS_SG;
	default:              return GEN_SYS_MD;
	}
}

/* debug counters for `oem gen-*` */
volatile unsigned g_gen_dbg_frames;
volatile unsigned g_gen_dbg_w, g_gen_dbg_h, g_gen_dbg_sr;
volatile int      g_gen_dbg_loadrc;

/* ---- in-game Pico menu (hardware OVL0 L0 overlay over the running game; mirrors the snes menu).
 * g_genesis_menu_open (declared above) gates game input + FF/rewind; the game keeps running so
 * settings preview live. Scoped to the settings that need no display-path change yet (Brightness,
 * Volume, CPU Clock, Save-state slots, Reset, Exit); aspect/filter/core-options land with the RSZ
 * display path. ---- */
volatile int g_genesis_menu_exit;   /* "Exit Game" -> the run loop breaks back to the selector */
static int   s_msel;
static char  s_mstat[48];
static int   s_save_slot;            /* manual save-state slot 0..2 */
static const struct genesis_core_exports *s_menu_c;   /* session context for the menu actions */
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static unsigned             s_menu_rw_payload;

extern void ayaneo_fill(unsigned int *buf, unsigned int pitch, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch, int x, int y, int scale, unsigned int argb, const char *s);
extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
extern void ayaneo_menu_overlay_mark_dirty(void);
extern int  ayaneo_brightness_pct(void);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);

/* manual CPU-clock grid (ayaneo_set_cpu_mhz reprograms the PLL only, so >boot-Vproc is the user's call) */
static const unsigned s_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int s_cpu_idx = -1;
static void genesis_cpu_step(int dir)
{
	int n = (int)(sizeof s_cpu_opp / sizeof s_cpu_opp[0]), i;
	if (s_cpu_idx < 0) {
		unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u; int best = 0;
		for (i = 0; i < n; i++) { unsigned d = s_cpu_opp[i] > cur ? s_cpu_opp[i] - cur : cur - s_cpu_opp[i]; if (d < bd) { bd = d; best = i; } }
		s_cpu_idx = best;
	}
	s_cpu_idx += dir; if (s_cpu_idx < 0) s_cpu_idx = 0; if (s_cpu_idx >= n) s_cpu_idx = n - 1;
	ayaneo_set_cpu_mhz(s_cpu_opp[s_cpu_idx]);
}

extern int  ayaneo_get_preempt_frames(void);
extern void ayaneo_set_preempt_frames(int v);

enum { GM_BRIGHT, GM_VOLUME, GM_ASPECT, GM_FILTER, GM_RUNAHEAD, GM_CPU, GM_SLOT, GM_SAVE, GM_LOAD, GM_RESET, GM_EXIT, GM_COUNT };
static const char *gen_aspect_name(int a) { return a == 2 ? "Stretch" : a == 1 ? "Fit" : "Pixel"; }
static const char *gen_filter_name(int f) { return f == 3 ? "Grid+" : f == 2 ? "Grid" : f == 1 ? "Scanlines" : "Off"; }
static const char *gen_ra_name(int pf) { return pf == 3 ? "Max" : pf == 2 ? "Responsive" : pf == 1 ? "Balanced" : "Off"; }

static char *mput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mputu(char *p, unsigned v) { char t[12]; int n = 0; if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; } while (n) *p++ = t[--n]; return p; }

static const char *gm_label(int i) { switch (i) {
	case GM_BRIGHT: return "Brightness"; case GM_VOLUME: return "Volume";
	case GM_ASPECT: return "Aspect Ratio"; case GM_FILTER: return "LCD Filter";
	case GM_RUNAHEAD: return "Run-Ahead"; case GM_CPU: return "CPU Clock";
	case GM_SLOT: return "Save Slot"; case GM_SAVE: return "Save State"; case GM_LOAD: return "Load State";
	case GM_RESET: return "Reset Game"; case GM_EXIT: return "Exit Game"; } return ""; }

static const char *gm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case GM_BRIGHT: p = mputu(p, (unsigned)ayaneo_brightness_pct()); p = mput(p, "%"); break;
	case GM_VOLUME: p = mputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mput(p, "%"); break;
	case GM_ASPECT: p = mput(p, gen_aspect_name(g_genesis_aspect)); break;
	case GM_FILTER: p = mput(p, gen_filter_name(g_genesis_filter)); break;
	case GM_RUNAHEAD: p = mput(p, gen_ra_name(ayaneo_get_preempt_frames())); break;
	case GM_CPU:    p = mputu(p, ayaneo_get_cpu_mhz()); p = mput(p, " MHz"); break;
	case GM_SLOT:   p = mputu(p, (unsigned)s_save_slot); break;
	case GM_SAVE: case GM_LOAD: case GM_RESET: case GM_EXIT: p = mput(p, "[A]"); break;
	} *p = 0; return buf; }

static void genesis_slot_ext(char *e) { e[0] = 's'; e[1] = 't'; e[2] = (char)('0' + s_save_slot); e[3] = 0; }

/* returns 1 to close the menu (Reset/Exit) */
static int gm_change(int i, int dir, int act)
{
	s_mstat[0] = 0;
	switch (i) {
	case GM_BRIGHT: if (dir) ayaneo_brightness_step(dir); break;
	case GM_VOLUME: if (dir) ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); break;
	case GM_ASPECT: if (dir) g_genesis_aspect = (g_genesis_aspect + dir + 3) % 3; break;   /* live preview */
	case GM_FILTER: if (dir) g_genesis_filter = (g_genesis_filter + dir + 4) % 4; break;
	case GM_RUNAHEAD: if (dir) { int pf = (ayaneo_get_preempt_frames() + dir + 4) % 4;
		ayaneo_set_preempt_frames(pf); ayaneo_set_cpu_mhz(s_gen_ra_opp[pf]); s_cpu_idx = -1; } break;
	case GM_CPU:    if (dir) genesis_cpu_step(dir); break;
	case GM_SLOT:   if (dir) s_save_slot = (s_save_slot + dir + 3) % 3; break;
	case GM_SAVE: if (act) {
		unsigned char *st = (unsigned char *)GEN_STATE_BUF; unsigned ssz = s_menu_c->state_size();
		char ext[4]; int ok; genesis_slot_ext(ext);
		ok = (ssz && ssz <= GEN_STATE_CAP && s_menu_c->state_save(st, ssz) == 0 &&
		      gba_sd_write_named(s_menu_vol, "/states/genesis", s_menu_rom->name, ext, st, ssz) == 0);
		mput(s_mstat, ok ? "State saved" : "Save failed"); } break;
	case GM_LOAD: if (act) {
		unsigned char *st = (unsigned char *)GEN_STATE_BUF; char ext[4]; unsigned n; int ok;
		genesis_slot_ext(ext);
		n = gba_sd_read_named(s_menu_vol, "/states/genesis", s_menu_rom->name, ext, st, GEN_STATE_CAP);
		ok = (n && s_menu_c->state_load(st, n) == 0);
		if (ok && s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload);   /* loaded state breaks the rewind timeline */
		mput(s_mstat, ok ? "State loaded" : "No save state"); } break;
	case GM_RESET: if (act) { s_menu_c->reset(); if (s_menu_rw_payload) ayaneo_rewind_reset(s_menu_rw_payload); return 1; } break;
	case GM_EXIT:  if (act) { g_genesis_menu_exit = 1; return 1; } break;
	}
	return 0;
}

int genesis_menu_open(void) { return g_genesis_menu_open; }

void genesis_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 520, panelH = 84 + GM_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2, x = px + 28, y = py + 84, i;
	char val[48];
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

static void genesis_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct genesis_core_exports *c = genesis_core_load();
	unsigned romsz, sr = 44100, ssz, rw_payload, saved_mhz;
	int aya_prev = 0, aya_hold = 0, rw_acc = 0;
	int up_h = 0, dn_h = 0, lt_h = 0, rt_h = 0;             /* menu nav auto-repeat hold counters */
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	struct genesis_frame fr;

	if (!c) return;   /* blob load failed (g_gen_dbg_loaderr set by the loader) */

	c->heap_init((void *)GEN_HEAP_BASE, GEN_HEAP_SZ);
	c->init();

	romsz = gba_sd_load_rom(vol, rom, (unsigned char *)GEN_ROM_BUF, GEN_ROM_CAP);
	g_gen_dbg_loadrc = -2;
	if (!romsz) { ayaneo_menu_audio_silence(); return; }
	if (c->load((const void *)GEN_ROM_BUF, romsz, rom_type_to_system(rom->type)) != 0) {
		g_gen_dbg_loadrc = -1; ayaneo_menu_audio_silence(); return;
	}
	g_gen_dbg_loadrc = 0;

	/* cartridge battery (SRAM/EEPROM) from /saves */
	if (c->sram_ptr() && c->sram_size())
		gba_sd_load_sav(vol, rom->name, (unsigned char *)c->sram_ptr(), c->sram_size());

	/* suspend-resume: reload the auto-saved state unless B is held at launch (fresh start) */
	if (!PRESSED(GPIO_B)) {
		ssz = c->state_size();
		if (ssz && ssz <= GEN_STATE_CAP) {
			unsigned n = gba_sd_read_named(vol, "/states/genesis", rom->name, "sus",
						       (unsigned char *)GEN_STATE_BUF, GEN_STATE_CAP);
			if (n) c->state_load((const void *)GEN_STATE_BUF, n);
		}
	}

	c->av_info(0, 0, 0, 0, &sr);
	if (!sr) sr = 44100;
	g_gen_dbg_sr = sr;
	if (c->aspect_x1000) g_genesis_aspect_x1000 = c->aspect_x1000();   /* Fit target (~4:3) */

	/* Arm the rewind ring: capture the FULL serialized state each frame (GPGX has no raw fast
	 * snapshot, so unlike snes we use state_save/state_load = retro_serialize). The high-DRAM delta
	 * ring XOR+RLEs consecutive same-size states, so a frame of change compresses well. Disabled if
	 * the state is absurdly large or the region is unavailable. */
	rw_payload = c->state_size();
	if (rw_payload && rw_payload <= 0x00400000u) ayaneo_rewind_reset(rw_payload);
	else rw_payload = 0;

	/* Raise the CPU clock for gameplay, escalated by the run-ahead depth (pf frames run pf+1
	 * emulations + a save + a load per display). The menu holds a lower idle clock; restored on
	 * exit. ayaneo_set_cpu_mhz reprograms only the PLL (boot Vproc), like the SNES gameplay tiers. */
	saved_mhz = ayaneo_get_cpu_mhz();
	{ int pf0 = ayaneo_get_preempt_frames(); ayaneo_set_cpu_mhz(s_gen_ra_opp[(pf0 >= 0 && pf0 < 4) ? pf0 : 0]); }

	/* menu context (actions reference the session core + save target) */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom; s_menu_rw_payload = rw_payload;
	g_genesis_menu_open = 0; g_genesis_menu_exit = 0; s_msel = 0; s_mstat[0] = 0; s_cpu_idx = -1;

	for (;;) {
		extern int ayaneo_joypad_ff_level(void);
		extern int ayaneo_joypad_rewind_level(void);
		extern void ayaneo_audio_reverse_flip(void);
		extern volatile unsigned int g_dbg_blit_us;
		static short s_gen_audrev[2048 * 2];   /* reversed+decimated rewind audio scratch */
		unsigned int em0, fe;
		int ff, rw;

		ayaneo_joypad_poll();

		/* Refresh the Pico menu as a hardware overlay (OVL0 L0) over the running game, or disable it
		 * when closed - the game keeps running underneath so settings preview live (mirrors snes). */
		ayaneo_menu_overlay(genesis_menu_paint, g_genesis_menu_open);

		/* Rewind (left trigger): walk the delta ring backward and re-render instead of advancing.
		 * Press depth sets a smooth 1x..6x speed via a fractional accumulator; each stepped-back
		 * frame's audio is reversed + decimated by `steps` and submitted (game plays backwards). On
		 * release the ring commits the rewound point as the new head. Mirrors the snes rewind block.
		 * Gated off while the menu is open. */
		rw = g_genesis_menu_open ? 0 : ayaneo_joypad_rewind_level();
		if (rw > 0 && ayaneo_rewind_ready() && ayaneo_rewind_count() > 0) {
			int spd = 256 + (rw * (GEN_REWIND_MAX_SPD - 256)) / 255;   /* 256(1x)..MAX_SPD(6x) */
			unsigned int sz; const void *st; int k, steps;
			ayaneo_hud_set(2, spd * 10 / 256);   /* cyan reverse-speed badge */
			if (!ayaneo_rewind_active()) { ayaneo_rewind_begin(); rw_acc = 0; ayaneo_audio_reverse_flip(); }
			rw_acc += spd; steps = rw_acc >> 8; rw_acc &= 255;
			for (k = 0; k < steps; k++) {
				int atold = (ayaneo_rewind_step() != 0);
				if (k > 0 && atold) break;
				st = ayaneo_rewind_cur(&sz);
				if (!st || !sz) break;
				c->state_load(st, sz);
				c->run(&fr);                 /* render the rewound state */
				g_gen_dbg_frames++;
				if (fr.audio && fr.frames) {
					const short *a = fr.audio; unsigned int f = fr.frames, i, j = 0;
					if (f > 2048u) f = 2048u;
					for (i = 0; i < f; i += (unsigned)steps) {
						s_gen_audrev[j * 2]     = a[(f - 1u - i) * 2];
						s_gen_audrev[j * 2 + 1] = a[(f - 1u - i) * 2 + 1];
						j++;
					}
					ayaneo_snes_audio_submit(s_gen_audrev, j, sr);
				}
				if (atold) break;
			}
			if (fr.video && fr.width && fr.height)
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height, fr.pitch / 2u);
			mtk_wdt_restart();
			continue;   /* rewind present done; skip the forward path */
		}
		if (ayaneo_rewind_active()) { ayaneo_rewind_end(); ayaneo_audio_reverse_flip(); }

		em0 = gpt4_get_current_tick();
		c->run(&fr);                                 /* committed frame */
		fe = (gpt4_get_current_tick() - em0) / 13u;  /* per-frame emu cost (us) for the FF cap */
		g_gen_dbg_frames++;
		if (c->aspect_x1000 && (g_gen_dbg_frames & 15u) == 0)   /* refresh Fit target (H32/H40 switch) */
			g_genesis_aspect_x1000 = c->aspect_x1000();
		if (rw_payload && ayaneo_rewind_ready()) {   /* capture committed frame into the rewind ring */
			void *p = ayaneo_rewind_capture_begin();
			if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
		}
		if (fr.audio && fr.frames)
			ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);

		/* Variable fast-forward (right trigger): after the committed frame, run extra frames so the
		 * game advances 2..10x per displayed frame, capped adaptively to what fits one vsync so the
		 * panel stays a smooth 60fps (mirrors the GBA/GBC/SNES adaptive cap). GPGX renders every FF
		 * frame, so the committed-frame cost is the per-frame cost; reserve the present blit; 2x floor.
		 * Audio of every frame is submitted (so the sound speeds up). Only the LAST frame is presented. */
		ff = g_genesis_menu_open ? 0 : ayaneo_joypad_ff_level();
		if (ff > 0) {
			int raw = 2 + (ff * (10 - 2)) / 255;
			unsigned int blit = g_dbg_blit_us < 15500u ? g_dbg_blit_us : 0u;
			unsigned int fef = fe ? fe : 2500u;
			int cap = (int)((15500u - blit) / fef);
			int mult = raw < cap ? raw : cap, k;
			if (mult < 2) mult = 2;
			if (mult > 10) mult = 10;
			ayaneo_hud_set(1, mult * 10);
			for (k = 1; k < mult; k++) {
				/* skip the VDP render (the heavy part) on the thrown-away frames; render only
				 * the LAST one (the frame we present). Audio kept on all so the sound speeds up. */
				if (c->set_av_skip) c->set_av_skip(k < mult - 1 ? 1 : 0, 0);
				c->run(&fr);
				g_gen_dbg_frames++;
				if (rw_payload && ayaneo_rewind_ready()) {   /* capture each FF frame too */
					void *p = ayaneo_rewind_capture_begin();
					if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
				}
				if (fr.audio && fr.frames)
					ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);
			}
			if (c->set_av_skip) c->set_av_skip(0, 0);
		} else {
			ayaneo_hud_set(0, 0);
		}

		/* Run-ahead: advance the DISPLAY pf frames into the future with the current input, present
		 * that future frame, then restore the committed state so real emulation still advances one
		 * frame per loop - hiding pf frames of the game's internal input lag. Gated off during FF /
		 * menu. GPGX has no raw snapshot, so state_save/load (fast + deterministic per the host
		 * test); the look-ahead frames skip render (all but the last) and audio (all) via set_av_skip. */
		{
			int pf = (ff || g_genesis_menu_open || !rw_payload) ? 0 : ayaneo_get_preempt_frames();
			int did_ra = 0, i;
			if (pf > 3) pf = 3;
			/* Only run the look-ahead if the committed state was actually saved. Pass the EXACT
			 * state size: GPGX retro_serialize is `if (size != STATE_SIZE) return FALSE`, so a wrong
			 * size (e.g. the buffer capacity) silently fails the save; then the restore has nothing
			 * to rewind to and the look-ahead frames STICK, running the game pf+1x - the
			 * fast-forward-with-frameskip bug. Guarding on the save return makes that impossible. */
			if (pf > 0 && c->state_save((void *)GEN_AHEAD_BUF, rw_payload) == 0) {
				did_ra = 1;
				for (i = 0; i < pf; i++) {
					if (c->set_av_skip) c->set_av_skip(i == pf - 1 ? 0 : 1, 1);
					c->run(&fr);
				}
				if (c->set_av_skip) c->set_av_skip(0, 0);
			}
			if (fr.video && fr.width && fr.height) {   /* present the future (or committed) frame */
				g_gen_dbg_w = fr.width; g_gen_dbg_h = fr.height;
				ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height,
							  fr.pitch / 2u);
			}
			if (did_ra) c->state_load((const void *)GEN_AHEAD_BUF, rw_payload);   /* rewind to committed */
		}
		mtk_wdt_restart();

		/* AYA taps toggle the Pico menu; holding AYA ~1.5 s force-exits to the selector. */
		{
			int aya = PRESSED(GPIO_AYA);
			if (aya && !aya_prev) { g_genesis_menu_open = !g_genesis_menu_open; s_mstat[0] = 0;
				ayaneo_menu_overlay_mark_dirty(); }
			aya_prev = aya;
			if (aya) { if (++aya_hold >= 90) break; } else aya_hold = 0;
		}

		/* menu navigation (Up/Down move, Left/Right change, A select, B/AYA close), with press-edge
		 * + auto-repeat. The game keeps running underneath (pad mask returns 0 while open). */
		if (g_genesis_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
			unsigned int jd = ayaneo_joypad_dpad();   /* left analog stick also drives nav */
			up |= !!(jd & 0x01u); dn |= !!(jd & 0x02u); lt |= !!(jd & 0x04u); rt |= !!(jd & 0x08u);
			#define NAV_DELAY 22
			#define NAV_REP   5
			#define FIRE(h)   ((h) == 1 || ((h) > NAV_DELAY && (((h) - NAV_DELAY) % NAV_REP) == 0))
			up_h = up ? up_h + 1 : 0; dn_h = dn ? dn_h + 1 : 0;
			lt_h = lt ? lt_h + 1 : 0; rt_h = rt ? rt_h + 1 : 0;
			if ((up && FIRE(up_h)) || (dn && FIRE(dn_h)) || (lt && FIRE(lt_h)) || (rt && FIRE(rt_h)) ||
			    (a && !a_p) || (b && !b_p))
				ayaneo_menu_overlay_mark_dirty();
			if (up && FIRE(up_h)) s_msel = (s_msel + GM_COUNT - 1) % GM_COUNT;
			if (dn && FIRE(dn_h)) s_msel = (s_msel + 1) % GM_COUNT;
			if (lt && FIRE(lt_h)) gm_change(s_msel, -1, 0);
			if (rt && FIRE(rt_h)) gm_change(s_msel, +1, 0);
			#undef NAV_DELAY
			#undef NAV_REP
			#undef FIRE
			if (a && !a_p) { if (gm_change(s_msel, 0, 1)) g_genesis_menu_open = 0; }
			if (b && !b_p) g_genesis_menu_open = 0;
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
		}
		if (g_genesis_menu_exit) break;   /* Pico "Exit Game" selected */
	}
	{ extern void ayaneo_snes_rsz_restore(void); ayaneo_snes_rsz_restore(); }   /* RSZ back to 1:1 (else Close hangs / carousel shears) */
	ayaneo_menu_overlay(0, 0);   /* disable the overlay BEFORE returning to the carousel (else the
				      * stale Pico panel composites over the selector - see CORE_PORTING_NOTES) */
	ayaneo_hud_set(0, 0);   /* clear the FF/RW badge on exit */
	ayaneo_set_cpu_mhz(saved_mhz);   /* restore the menu's idle clock */

	/* persist SRAM + a suspend state so the next launch resumes */
	if (c->sram_ptr() && c->sram_size())
		gba_sd_write_sav(vol, rom->name, c->sram_ptr(), c->sram_size());
	ssz = c->state_size();
	if (ssz && ssz <= GEN_STATE_CAP && c->state_save((void *)GEN_STATE_BUF, ssz) == 0)
		gba_sd_write_named(vol, "/states/genesis", rom->name, "sus",
				   (const unsigned char *)GEN_STATE_BUF, ssz);
	c->unload();
	ayaneo_menu_audio_silence();
}

/* ---- dedicated emulation thread (see gbc_sd_run.c rationale: emu_thread's 64 KB overflows) ---- */
static thread_t *s_gen_thread;
static event_t   s_gen_kick, s_gen_done;
static fat_vol  *s_gen_vol;
static const gba_rom_entry *s_gen_rom;

static int genesis_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_gen_kick);
		genesis_session_body(s_gen_vol, s_gen_rom);
		event_signal(&s_gen_done, false);
	}
	return 0;
}

/* Called from emu_thread's ROM-select dispatch. Runs the game on the genesis_emu thread and blocks
 * here until it returns to the selector (AYA). */
void genesis_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	if (!s_gen_thread) {
		event_init(&s_gen_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_gen_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_gen_thread = thread_create("genesis_emu", &genesis_thread_fn, NULL,
					     DEFAULT_PRIORITY, 262144);
		if (!s_gen_thread) { genesis_session_body(vol, rom); return; }   /* fallback: inline */
		thread_resume(s_gen_thread);
	}
	s_gen_vol = vol;
	s_gen_rom = rom;
	event_signal(&s_gen_kick, false);
	event_wait(&s_gen_done);
}
