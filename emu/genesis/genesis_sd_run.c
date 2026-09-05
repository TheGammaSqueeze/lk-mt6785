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
#define GEN_REWIND_MAX_SPD 1536           /* max rewind speed in 256ths (1536 = 6x); floor 256 = 1x */

volatile int g_genesis_menu_open;         /* reserved for the Pico menu (parity phase) */

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

static void genesis_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct genesis_core_exports *c = genesis_core_load();
	unsigned romsz, sr = 44100, ssz, rw_payload, saved_mhz;
	int aya_prev = 0, aya_hold = 0, rw_acc = 0;
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

	/* Arm the rewind ring: capture the FULL serialized state each frame (GPGX has no raw fast
	 * snapshot, so unlike snes we use state_save/state_load = retro_serialize). The high-DRAM delta
	 * ring XOR+RLEs consecutive same-size states, so a frame of change compresses well. Disabled if
	 * the state is absurdly large or the region is unavailable. */
	rw_payload = c->state_size();
	if (rw_payload && rw_payload <= 0x00400000u) ayaneo_rewind_reset(rw_payload);
	else rw_payload = 0;

	/* Raise the CPU clock for gameplay (Genesis + per-frame rewind capture + 60fps present); the
	 * menu holds a lower idle clock. Restored on exit. ayaneo_set_cpu_mhz reprograms only the PLL,
	 * so 1400 MHz runs at the boot Vproc - the same point the SNES gameplay tiers use. */
	saved_mhz = ayaneo_get_cpu_mhz();
	ayaneo_set_cpu_mhz(1400);

	for (;;) {
		extern int ayaneo_joypad_ff_level(void);
		extern int ayaneo_joypad_rewind_level(void);
		extern void ayaneo_audio_reverse_flip(void);
		extern volatile unsigned int g_dbg_blit_us;
		static short s_gen_audrev[2048 * 2];   /* reversed+decimated rewind audio scratch */
		unsigned int em0, fe;
		int ff, rw;

		ayaneo_joypad_poll();

		/* Rewind (left trigger): walk the delta ring backward and re-render instead of advancing.
		 * Press depth sets a smooth 1x..6x speed via a fractional accumulator; each stepped-back
		 * frame's audio is reversed + decimated by `steps` and submitted (game plays backwards). On
		 * release the ring commits the rewound point as the new head. Mirrors the snes rewind block. */
		rw = ayaneo_joypad_rewind_level();
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
		ff = ayaneo_joypad_ff_level();
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
				c->run(&fr);
				g_gen_dbg_frames++;
				if (rw_payload && ayaneo_rewind_ready()) {   /* capture each FF frame too */
					void *p = ayaneo_rewind_capture_begin();
					if (p && c->state_save(p, rw_payload) == 0) ayaneo_rewind_capture_commit(rw_payload);
				}
				if (fr.audio && fr.frames)
					ayaneo_snes_audio_submit(fr.audio, fr.frames, sr);
			}
		} else {
			ayaneo_hud_set(0, 0);
		}

		if (fr.video && fr.width && fr.height) {     /* present the (last) frame, vsync-paced */
			g_gen_dbg_w = fr.width; g_gen_dbg_h = fr.height;
			ayaneo_genesis_show_frame((const unsigned short *)fr.video, fr.width, fr.height,
						  fr.pitch / 2u);
		}
		mtk_wdt_restart();

		/* AYA: tap or hold ~1.5 s exits back to the ROM selector (matches the other cores). */
		{
			int aya = PRESSED(GPIO_AYA);
			if (aya) { if (++aya_hold >= 90) break; }
			else { if (aya_prev && aya_hold < 90) break; aya_hold = 0; }
			aya_prev = aya;
		}
	}
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
