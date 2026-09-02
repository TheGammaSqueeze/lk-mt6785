/*
 * snes_sd_run.c - run a SNES ROM from the SD card through the loadable snes9x core.
 *
 * The GBA-from-SD selector hands a chosen ROM to gba_driver.c; when it is a SNES title
 * (roms/snes) the driver calls snes_sd_session() here. We load the snes9x core blob
 * (snes_core_load, boot_b), give it a DRAM heap, load the ROM (+ its .srm) from the card,
 * then run the frame loop: each frame the core produces an RGB565 image (256/512 wide) +
 * audio; we blit it (ayaneo_snes_show_frame) paced off the 13 MHz counter. Holding AYA
 * returns to the selector. Runs on its OWN thread (deep C++ would overflow emu_thread's
 * stack - see emu/CORE_PORTING_NOTES.md item 2). Only one core runs at a time, so we reuse
 * the 0x50000000 arena.
 *
 * First bring-up: video + input. Audio and the in-game menu/run-ahead come next.
 */
#include "snes_core_abi.h"
#include "../gba/sd_fat.h"
#include "../gba/gba_sd_save.h"
#include <kernel/thread.h>
#include <kernel/event.h>

extern const struct snes_core_exports *snes_core_load(void);   /* snes_core_loader.c */
extern void     ayaneo_snes_show_frame(const unsigned short *pix, unsigned sw, unsigned sh, unsigned spitch_px);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void         ayaneo_set_cpu_mhz(unsigned int mhz);   /* ARM-PLL OPP (see gba_driver s_cpu_opp) */
extern void     ayaneo_gbc_audio_init(void);       /* 48 kHz AFE ring (shared with GB/GBC) */
extern void     ayaneo_snes_audio_reset(void);
extern void     ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz);
extern void     ayaneo_menu_audio_silence(void);
extern void     ayaneo_display_prepare(void);
extern void     ayaneo_dsi_set_vfp(unsigned int vfp);   /* per-core panel refresh (ddp_dsi.c) */
extern int      priamry_display_wait_for_vsync(void);   /* primary_display.c (name has the typo) */
extern unsigned gpt4_get_current_tick(void);

/* Panel vertical-front-porch per refresh rate. Stock vfp 23 -> vtotal 999 -> 59.749 Hz
 * (GB/GBC/GBA/menu). SNES uses vfp 17 -> vtotal 993 -> ~60.11 Hz (0.02% off its native
 * 60.0988 Hz) so the vsync-locked present in ayaneo_snes_show_frame is smooth. */
#define SNES_VFP      17u
#define DEFAULT_VFP   23u
extern void     mtk_wdt_restart(void);
extern void     mtk_wdt_disable(void);
extern int      mt_get_gpio_in(unsigned pin);

/* pad GPIOs (match gba_driver.c). Active-low. */
#define GP(n)          ((n) | 0x80000000u)
#define PRESSED(g)     (mt_get_gpio_in(GP(g)) == 0)
#define GPIO_AYA       86
#define GPIO_LB        92
#define GPIO_RB        81
#define GPIO_SELECT    90
#define GPIO_START     91
#define GPIO_B         82
#define GPIO_UP        89
#define GPIO_DOWN      79
#define GPIO_LEFT      78
#define GPIO_RIGHT     80
#define GPIO_A         83
#define GPIO_X         85      /* physical X (84/85 read swapped vs the caps) */
#define GPIO_Y         84      /* physical Y */

/* RETRO_DEVICE_ID_JOYPAD_* bit positions (what the core's input_state_cb reads). */
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

/* ---- in-game overlay menu (GammaOS Pico), mirrors the GB/GBC one ---- */
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch, int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch, int x, int y, int scale, unsigned int argb, const char *s);
extern int  ayaneo_brightness_pct(void);
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern void ayaneo_menu_settings_persist(void);

volatile int g_snes_menu_open;   /* gates ayaneo_snes_pad_mask so the game ignores menu input */

/* diagnostics, read via `fastboot oem diag` after a launch (why did the session exit?) */
volatile unsigned g_snes_dbg_stage;   /* 1 blob,2 rom,3 heap,4 loading,5 running */
volatile unsigned g_snes_dbg_romsz;
volatile unsigned g_snes_dbg_loadrc;  /* c->load() return (0 = ok, 0xFF = not reached) */
volatile unsigned g_snes_dbg_frames;
volatile unsigned g_snes_dbg_w, g_snes_dbg_h;
volatile unsigned g_snes_dbg_exit;    /* 0 running,1 aya-hold,2 blob,3 rom,4 load */
volatile unsigned g_snes_dbg_pitch;   /* f.pitch of the last frame */
volatile unsigned g_snes_dbg_nz;      /* non-zero pixels in a full-frame sample (0 = black) */
volatile unsigned g_snes_dbg_changed; /* count of frames whose content hash changed (0 = frozen) */
static   unsigned s_snes_dbg_lasthash;
volatile unsigned g_snes_test_limit;  /* >0: run this many frames then exit (oem snes-launch) */
volatile unsigned g_snes_dbg_pcmin = 0xFFFFFFFF, g_snes_dbg_pcmax; /* emulated 65816 PC range */
volatile unsigned g_snes_dbg_audframes; /* total audio sample-pairs submitted (0 = APU silent) */

/* Physical pad -> SNES button bitmask (imports.read_buttons). Returns 0 while the in-game
 * menu is open so navigation keys do not leak into the game. */
unsigned ayaneo_snes_pad_mask(void)
{
	unsigned m = 0;
	if (g_snes_menu_open) return 0;
	if (PRESSED(GPIO_B))      m |= 1u << RJ_B;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	if (PRESSED(GPIO_A))      m |= 1u << RJ_A;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	return m;
}

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). */
#define SNES_ROM_BUF   0x50000000u
#define SNES_ROM_CAP   0x00800000u        /* 8 MB (largest SNES carts ~6 MB) */
#define SNES_HEAP_BASE 0x50800000u
#define SNES_HEAP_SZ   0x03000000u        /* 48 MB for snes9x's internal allocations */
#define SNES_STATE_BUF 0x53800000u        /* just above the heap, below the 0x54000000 snapshot */
#define SNES_STATE_CAP 0x00400000u        /* 4 MB (snes9x state ~0.8 MB, more with SA-1/SuperFX) */
#define RESET_HOLD_FRAMES 30              /* SELECT+START+L+R held ~0.5 s = soft reset */

/* ---- in-game menu: state, rendering, actions ---- */
static const struct snes_core_exports *s_menu_c;
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static int  s_msel;
static char s_mstat[48];
enum { SM_BRIGHT, SM_VOLUME, SM_SAVE, SM_LOAD, SM_RESET, SM_CLOSE, SM_COUNT };

static char *smput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *smputu(char *p, unsigned v) { char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n]; return p; }

static const char *sm_label(int i) { switch (i) {
	case SM_BRIGHT: return "Brightness"; case SM_VOLUME: return "Volume";
	case SM_SAVE:   return "Save State"; case SM_LOAD:   return "Load State";
	case SM_RESET:  return "Reset Game"; case SM_CLOSE:  return "Close"; } return ""; }
static const char *sm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case SM_BRIGHT: p = smputu(p, (unsigned)ayaneo_brightness_pct()); p = smput(p, "%"); break;
	case SM_VOLUME: p = smputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = smput(p, "%"); break;
	case SM_SAVE: case SM_LOAD: p = smput(p, "[A]"); break;
	default: break; } *p = 0; return buf; }

int snes_menu_open(void) { return g_snes_menu_open; }

void snes_menu_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H)
{
	int rowH = 38, panelW = 520, panelH = 84 + SM_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2, x = px + 28, y = py + 84, i;
	char val[48];
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");
	for (i = 0; i < SM_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_msel) ? 0xFF101018u : 0xFFC8D0E0u; int vw;
		if (i == s_msel) ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, sm_label(i));
		sm_value(i, val); for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_mstat[0]) ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_mstat);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* returns 1 to close the menu (Reset/Close), else 0 */
static int sm_change(int i, int dir, int act)
{
	s_mstat[0] = 0;
	switch (i) {
	case SM_BRIGHT: if (dir) { ayaneo_brightness_step(dir); ayaneo_menu_settings_persist(); } break;
	case SM_VOLUME: if (dir) { ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); ayaneo_menu_settings_persist(); } break;
	case SM_SAVE: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF; unsigned ssz = s_menu_c->state_size();
			if (ssz && ssz <= SNES_STATE_CAP && s_menu_c->state_save(st, ssz) == 0)
				smput(s_mstat, gba_sd_write_named(s_menu_vol, "/states/snes", s_menu_rom->name, "st0", st, ssz) == 0 ? "State saved" : "Save failed");
			else smput(s_mstat, "Save failed"); } break;
	case SM_LOAD: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF;
			unsigned n = gba_sd_read_named(s_menu_vol, "/states/snes", s_menu_rom->name, "st0", st, SNES_STATE_CAP);
			smput(s_mstat, (n && s_menu_c->state_load(st, n) == 0) ? "State loaded" : "No save state"); } break;
	case SM_RESET: if (act) { s_menu_c->reset(); return 1; } break;
	case SM_CLOSE: if (act) return 1; break;
	}
	return 0;
}

static void snes_session_body(fat_vol *vol, const gba_rom_entry *rom)
{
	const struct snes_core_exports *c;
	unsigned char *rombuf = (unsigned char *)SNES_ROM_BUF;
	unsigned romsz;
	unsigned bw = 0, bh = 0, mw = 0, mh = 0, sr = 0;
	unsigned saved_mhz = 0;
	int aya_hold = 0;

	g_snes_dbg_stage = 1; g_snes_dbg_frames = 0; g_snes_dbg_exit = 0; g_snes_dbg_audframes = 0;
	g_snes_dbg_w = g_snes_dbg_h = 0; g_snes_dbg_loadrc = 0xFF;

	c = snes_core_load();
	if (!c) { g_snes_dbg_exit = 2; return; }   /* blob load failed */
	g_snes_dbg_stage = 2;

	romsz = gba_sd_load_rom(vol, rom, rombuf, SNES_ROM_CAP);
	g_snes_dbg_romsz = romsz;
	if (!romsz) { g_snes_dbg_exit = 3; return; }   /* ROM read failed */
	g_snes_dbg_stage = 3;

	c->heap_init((void *)SNES_HEAP_BASE, SNES_HEAP_SZ);
	c->init();
	g_snes_dbg_stage = 4;
	g_snes_dbg_loadrc = (unsigned)c->load(rombuf, romsz);
	if (g_snes_dbg_loadrc != 0) { g_snes_dbg_exit = 4; return; }   /* core load failed */
	g_snes_dbg_stage = 5;
	c->av_info(&bw, &bh, &mw, &mh, &sr);
	g_snes_dbg_w = bw; g_snes_dbg_h = bh;

	/* cartridge battery save (.srm) from the card, if any. */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)   /* SNES SRAM max 128 KB */
			gba_sd_read_named(vol, "/saves/snes", rom->name, "srm", (unsigned char *)p, sz);
	}

	/* Suspend/resume: reload the save STATE from the last exit so the game resumes where
	 * it left off, unless B is held at launch (start fresh). Mirrors the GB/GBC flow; the
	 * host round-trip proved snes9x serialize/unserialize is deterministic. */
	if (!PRESSED(GPIO_B)) {
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned n = gba_sd_read_named(vol, "/states/snes", rom->name, "st0", st, SNES_STATE_CAP);
		if (n) c->state_load(st, n);
	}

	/* snes9x mainline is accuracy-first (full 65816 + SPC700 + PPU + DSP each frame), which
	 * is heavy on the A55; the menu/other cores idle around 1200 MHz. Raise the ARM PLL to
	 * at least 1400 MHz for the SNES session so plain-mapper games hit full speed, but never
	 * downclock a user who manually picked a higher OPP. Restored on session exit. NOTE:
	 * ayaneo_set_cpu_mhz only moves the PLL, not core voltage - 1400 is the OPP just above
	 * the boot Vproc point, the same one the "Max" preempt tier uses. */
	saved_mhz = ayaneo_get_cpu_mhz();
	if (saved_mhz < 1400) ayaneo_set_cpu_mhz(1400);

	ayaneo_display_prepare();
	ayaneo_gbc_audio_init();
	ayaneo_snes_audio_reset();
	mtk_wdt_disable();
	/* Switch the panel to ~60.11 Hz for SNES (vfp swap). The vsync-locked present in
	 * ayaneo_snes_show_frame then paces emulation to the panel scan - smooth, tear-free,
	 * no 13 MHz busy-wait needed. Restored to 59.749 Hz on exit below. */
	ayaneo_dsi_set_vfp(SNES_VFP);

	/* hand the in-game menu this session's context */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom;
	s_msel = 0; s_mstat[0] = 0; g_snes_menu_open = 0;

	int reset_hold = 0, aya_prev = 0;
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	for (;;) {
		struct snes_frame f;
		int aya;
		mtk_wdt_restart();

		/* The game keeps running under the menu (frame + audio keep flowing); its input
		 * is gated off in ayaneo_snes_pad_mask while the menu is open. */
		c->run(&f);
		g_snes_dbg_frames++;
		if (c->dbg_pc) { unsigned pc = c->dbg_pc();
			if (pc < g_snes_dbg_pcmin) g_snes_dbg_pcmin = pc;
			if (pc > g_snes_dbg_pcmax) g_snes_dbg_pcmax = pc; }
		if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) { g_snes_dbg_exit = 5; }
		if (f.video && f.width && f.height) {
			g_snes_dbg_w = f.width; g_snes_dbg_h = f.height; g_snes_dbg_pitch = f.pitch;
			{	/* full-frame sample: non-zero pixel count + a content hash (to tell a
				 * static black frame from live-but-mis-displayed content). */
				const unsigned short *p = (const unsigned short *)f.video;
				unsigned stride = f.pitch / 2u, y, x, nz = 0, hash = 2166136261u;
				for (y = 0; y < f.height; y += 8)
					for (x = 0; x < f.width; x += 8) {
						unsigned v = p[y * stride + x];
						if (v) nz++;
						hash = (hash ^ v) * 16777619u;
					}
				g_snes_dbg_nz = nz;
				if (hash != s_snes_dbg_lasthash) { g_snes_dbg_changed++; s_snes_dbg_lasthash = hash; }
			}
			ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
		} else
			priamry_display_wait_for_vsync();   /* keep pacing if a frame was dropped */
		if (f.audio && f.frames) {
			g_snes_dbg_audframes += f.frames;
			ayaneo_snes_audio_submit(f.audio, f.frames, sr ? sr : 32040u);
		}

		/* AYA taps toggle the menu; holding AYA ~1.5 s force-exits to the selector. */
		aya = PRESSED(GPIO_AYA);
		if (aya && !aya_prev) { g_snes_menu_open = !g_snes_menu_open; s_mstat[0] = 0; }
		aya_prev = aya;
		if (aya) { if (++aya_hold >= 90) { g_snes_dbg_exit = 1; break; } } else aya_hold = 0;

		if (g_snes_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
			if (up && !up_p) s_msel = (s_msel + SM_COUNT - 1) % SM_COUNT;
			if (dn && !dn_p) s_msel = (s_msel + 1) % SM_COUNT;
			if (lt && !lt_p) sm_change(s_msel, -1, 0);
			if (rt && !rt_p) sm_change(s_msel, +1, 0);
			if (a  && !a_p)  { if (sm_change(s_msel, 0, 1)) g_snes_menu_open = 0; }
			if (b  && !b_p)  g_snes_menu_open = 0;
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
			reset_hold = 0;
		} else {
			/* Soft reset: SELECT+START+L+R held ~0.5 s (mirrors GB/GBC/GBA). */
			if (PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) && PRESSED(GPIO_LB) && PRESSED(GPIO_RB)) {
				if (++reset_hold >= RESET_HOLD_FRAMES) { c->reset(); reset_hold = 0; }
			} else reset_hold = 0;
		}
		if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) break;   /* oem snes-launch */
	}
	g_snes_menu_open = 0;
	g_snes_test_limit = 0;

	ayaneo_dsi_set_vfp(DEFAULT_VFP);   /* restore 59.749 Hz for the menu / other cores */
	if (saved_mhz) ayaneo_set_cpu_mhz(saved_mhz);   /* restore the pre-session ARM clock */

	/* Suspend: write a save STATE so the next launch resumes here (mirrors GB/GBC). */
	{
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned ssz = c->state_size();
		if (ssz && ssz <= SNES_STATE_CAP && c->state_save(st, ssz) == 0)
			gba_sd_write_named(vol, "/states/snes", rom->name, "st0", st, ssz);
	}

	/* persist SRAM on exit */
	{
		unsigned sz = c->sram_size();
		void *p = c->sram_ptr();
		if (p && sz && sz <= 0x20000u)
			gba_sd_write_named(vol, "/saves/snes", rom->name, "srm", p, sz);
	}
	c->unload();
	ayaneo_menu_audio_silence();
}

/* ---- dedicated emulation thread (see CORE_PORTING_NOTES item 2) ---- */
static thread_t *s_snes_thread;
static event_t   s_snes_kick, s_snes_done;
static fat_vol            *s_snes_vol;
static const gba_rom_entry *s_snes_rom;

static int snes_thread_fn(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_snes_kick);
		snes_session_body(s_snes_vol, s_snes_rom);
		event_signal(&s_snes_done, false);
	}
	return 0;
}

void snes_sd_session(fat_vol *vol, const gba_rom_entry *rom)
{
	s_snes_vol = vol; s_snes_rom = rom;
	if (!s_snes_thread) {
		event_init(&s_snes_kick, false, EVENT_FLAG_AUTOUNSIGNAL);
		event_init(&s_snes_done, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_snes_thread = thread_create("snes_emu", snes_thread_fn, 0, DEFAULT_PRIORITY, 262144);
		thread_resume(s_snes_thread);
	}
	event_signal(&s_snes_kick, true);
	event_wait(&s_snes_done);
}
