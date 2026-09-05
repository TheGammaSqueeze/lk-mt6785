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
extern void     ayaneo_menu_overlay_mark_dirty(void);   /* repaint the hardware menu overlay (mt_disp_drv.c) */
extern void     ayaneo_display_prepare(void);
extern void     ayaneo_dsi_set_vfp(unsigned int vfp);   /* per-core panel refresh (ddp_dsi.c) */
extern unsigned int ayaneo_dsi_get_vfp(void);           /* read-back to validate the switch */
volatile unsigned g_snes_dbg_vfp;                       /* the live DSI vfp during the session */
extern int      priamry_display_wait_for_vsync(void);   /* primary_display.c (name has the typo) */
extern unsigned gpt4_get_current_tick(void);
extern int      ayaneo_get_lcd_filter(void);            /* 0 Off,1 Scanlines,2 Grid,3 Dot Matrix */
extern void     ayaneo_set_lcd_filter(int f);
extern int      ayaneo_get_preempt_frames(void);        /* run-ahead depth 0..3 (shared setting) */
extern void     ayaneo_set_preempt_frames(int v);
extern unsigned ayaneo_get_snes_opts(void);             /* packed aspect|overscan<<8|audio<<16|hires<<24 */
extern void     ayaneo_set_snes_opts(unsigned v);
extern int      ayaneo_get_snes_slot(void);             /* persisted manual save-state slot 0..9 */
extern void     ayaneo_set_snes_slot(int v);
extern int      ayaneo_get_snes_turbo(void);            /* persisted auto-fire mask (bit0 A, bit1 B) */
extern void     ayaneo_set_snes_turbo(int v);

/* Run-ahead: present pf frames into the future with the current input, then rewind to the
 * committed frame (mirrors GB/GBC/GBA "Preemptive Frames"). Runs pf+1 full emulations per
 * displayed frame, so escalate the ARM clock with the tier. Off while the menu is open. */
#define SNES_AHEAD_BUF 0x53000000u   /* run-ahead state (shares the mapped-arena scratch slot) */
#define SNES_AHEAD_CAP 0x00200000u   /* 2 MB room before the menu wallpaper cache (0x53200000) */
/* Clock per run-ahead tier, escalating with depth so the pf+1 emulations/frame still fit the
 * 16.7 ms budget: Off=1400, Balanced(1)=1600, Responsive(2)=1800, Max(3)=2000 (the ARM PLL
 * reads back ~1 MHz low, i.e. 1399/1599/1799/1999). The render/audio skip on look-ahead frames
 * cut the sustained duty enough that the higher tiers are stable now (previously any run-ahead
 * was clamped to 1400 to avoid a brownout data-abort at ~100% duty). */
static const unsigned s_snes_ra_opp[4] = { 1400, 1600, 1800, 2000 };
static const char *snes_ra_name(int pf)
{ return pf == 1 ? "Balanced" : pf == 2 ? "Responsive" : pf == 3 ? "Max" : "Off"; }

/* 1 if this game's save state fits the 2 MB look-ahead slot (SNES_AHEAD_CAP), so run-ahead
 * can actually engage. Set once per session after state_size() is known. When 0, a selected
 * tier is inert, so the Run-Ahead menu row shows it as unavailable rather than silently doing
 * nothing (large-state carts: some SA-1 / big-SRAM titles). */
static int s_snes_ra_avail = 1;

/* Benchmark (uncap): run the emulator with no vsync pacing and no audio, counting
 * emulated frames per second so CPU-clock changes are measurable (mirrors the GBC/GBA
 * "Benchmark (Uncap)" item). ayaneo_snes_show_frame skips its vsync wait when this is set. */
volatile int g_snes_benchmark;
static volatile int s_snes_fps;
int snes_benchmark_on(void) { return g_snes_benchmark; }

/* Headless benchmark harness (oem snes-bench:N): forces benchmark mode for a frame-limited
 * run so the cron can measure UNCAPPED emulation FPS - combined with oem snes-ra:N it also
 * measures run-ahead depth cost (run-ahead stays active under benchmark ONLY in the headless
 * test). g_snes_dbg_benchfps holds the last measured FPS, read back via oem diag. */
volatile int      g_snes_dbg_bench;
volatile unsigned g_snes_dbg_benchfps;

/* Measured SNES panel refresh (Hz*1000), an 8-frame average from the vsync-locked present;
 * shown read-only in the Pico menu and via oem diag. Same tick math as the menu's
 * g_dbg_hz1000 - comparing the two validates the per-core LCM vfp switch (menu vfp 23 ~=
 * 59.749 Hz vs SNES vfp 17 ~= 60.11 Hz). */
volatile unsigned g_snes_dbg_hz1000;
#define s_snes_hz1000 g_snes_dbg_hz1000

/* Manual CPU-clock OPP grid (matches gba_driver.c s_cpu_opp); the SNES session floors at
 * 1400 MHz but the player can raise it here. Not persisted. */
static const unsigned s_snes_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static void snes_cpu_step(int dir)
{
	int n = (int)(sizeof s_snes_cpu_opp / sizeof s_snes_cpu_opp[0]), i, best = 0;
	unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u;
	for (i = 0; i < n; i++) { unsigned d = s_snes_cpu_opp[i] > cur ? s_snes_cpu_opp[i] - cur : cur - s_snes_cpu_opp[i];
		if (d < bd) { bd = d; best = i; } }
	best += dir; if (best < 0) best = 0; if (best >= n) best = n - 1;
	ayaneo_set_cpu_mhz(s_snes_cpu_opp[best]);
}

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
#define GPIO_R2        57      /* second-stage right trigger = fast-forward (matches GBA) */

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
extern int  mtk_detect_key(unsigned short hwkey);   /* hardware volume rocker (0x11 up, 0x00 down) */
extern void ayaneo_gbc_osd_show(int kind, int pct); /* transient on-screen bar: 0 brightness,1 volume */

volatile int g_snes_menu_open;   /* gates ayaneo_snes_pad_mask so the game ignores menu input */
volatile int g_snes_menu_exit;   /* Pico-menu "Exit Game" -> run loop breaks back to the selector */
volatile int g_snes_turbo;       /* auto-fire mask: bit0 = A, bit1 = B (0=off,1=A,2=B,3=A+B) */

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
volatile unsigned g_snes_dbg_audframes; /* total audio sample-pairs submitted (0 = APU silent) */
/* headless save-state self-test (run at the end of an oem snes-launch frame-limited run):
 * ss_size = state_size bytes; ss_core = 1 if state_save then state_load both returned 0;
 * ss_sd = 1 if SD write -> read-back -> memcmp matched exactly (SD path integrity). */
volatile unsigned g_snes_dbg_ss_size, g_snes_dbg_ss_core, g_snes_dbg_ss_sd;
volatile int      g_snes_dbg_ra;        /* forced run-ahead depth in the headless test (oem snes-ra) */
volatile unsigned g_snes_dbg_revmap;    /* 1 = the exit reverse-punch buffer (0x55800000) is writable */
volatile unsigned g_snes_dbg_ss_fast;   /* 1 = fast-savestate (run-ahead) round-trip succeeded */
/* Run-ahead validation (headless test): ra_ok=1 means skipping the PPU render on look-ahead
 * frames leaves emulation state bit-identical (the presented look-ahead is accurate); ra_ahead=1
 * means the N-ahead frame genuinely differs from the committed 1-ahead frame (true look-ahead);
 * ra_depth = N tested. */
volatile unsigned g_snes_dbg_ra_ok, g_snes_dbg_ra_ahead, g_snes_dbg_ra_depth;
/* raw-snapshot validation: 1 = the RAW run-ahead freeze/rewind produces the SAME N-ahead frame as
 * the trusted serialize + full-render path (raw snapshot is state-exact); 2 = unsupported cart
 * (fell back to serialize); 0 = MISMATCH (raw snapshot is losing state - would be a bug). */
volatile unsigned g_snes_dbg_ra_raw;
/* Clean display-independent cost breakdown (headless tight loops): us per headless frame,
 * per fully-rendered frame, per fast state_save, and per fast state_load. fps = 1e6/us. */
volatile unsigned g_snes_dbg_core_us, g_snes_dbg_render_us, g_snes_dbg_save_us, g_snes_dbg_load_us;
volatile unsigned g_snes_dbg_heapused;  /* arena bytes in use at test end (run-ahead leak check) */

/* Physical pad -> SNES button bitmask (imports.read_buttons). Returns 0 while the in-game
 * menu is open so navigation keys do not leak into the game. */
/* Release-latch for the buttons that dismissed the Pico menu (A select / B close): the menu
 * closes on the press edge, but the player is still holding the button that same frame, so
 * without this the held A/B bleeds straight into the game as an unintended input. Set on close,
 * cleared here once the button is physically released. */
int g_snes_ab_latch;   /* bit0 = A held-over, bit1 = B held-over */
/* FNV-1a hash of a returned frame's visible RGB565 pixels (0 if the frame had no video). Used
 * by the headless run-ahead determinism check to compare a rendered look-ahead frame across
 * the full-render and render-skip paths. */
static unsigned snes_hash_frame(const struct snes_frame *fr)
{
	if (!fr || !fr->video || !fr->width || !fr->height) return 0;
	const unsigned short *p = (const unsigned short *)fr->video;
	unsigned stride = fr->pitch / 2u, y, x, h = 2166136261u;
	for (y = 0; y < fr->height; y++)
		for (x = 0; x < fr->width; x++)
			h = (h ^ p[y * stride + x]) * 16777619u;
	return h ? h : 1u;   /* never collide with the "no video" sentinel 0 */
}

unsigned ayaneo_snes_pad_mask(void)
{
	/* Turbo (auto-fire): g_snes_turbo bit0 = A, bit1 = B. When set and the button is held,
	 * the bit is gated by a ~15 Hz pulse (2 emulated frames on, 2 off) so it rapid-fires. */
	unsigned m = 0;
	int a_on = 1, b_on = 1;
	if (g_snes_menu_open) return 0;
	/* Clear each latch once its button is up; while latched, gate that button out of the game. */
	if (!PRESSED(GPIO_A)) g_snes_ab_latch &= ~1;
	if (!PRESSED(GPIO_B)) g_snes_ab_latch &= ~2;
	if (g_snes_ab_latch & 1) a_on = 0;
	if (g_snes_ab_latch & 2) b_on = 0;
	if (g_snes_turbo) { int pulse = (g_snes_dbg_frames & 3u) < 2u;
		if (g_snes_turbo & 1) a_on = pulse;
		if (g_snes_turbo & 2) b_on = pulse; }
	if (PRESSED(GPIO_B) && b_on) m |= 1u << RJ_B;
	if (PRESSED(GPIO_Y))      m |= 1u << RJ_Y;
	if (PRESSED(GPIO_SELECT)) m |= 1u << RJ_SELECT;
	if (PRESSED(GPIO_START))  m |= 1u << RJ_START;
	if (PRESSED(GPIO_UP))     m |= 1u << RJ_UP;
	if (PRESSED(GPIO_DOWN))   m |= 1u << RJ_DOWN;
	if (PRESSED(GPIO_LEFT))   m |= 1u << RJ_LEFT;
	if (PRESSED(GPIO_RIGHT))  m |= 1u << RJ_RIGHT;
	{	/* left analog stick -> D-pad (JOY_UP=1 DOWN=2 LEFT=4 RIGHT=8) */
		extern unsigned int ayaneo_joypad_dpad(void);
		unsigned d = ayaneo_joypad_dpad();
		if (d & 0x01u) m |= 1u << RJ_UP;
		if (d & 0x02u) m |= 1u << RJ_DOWN;
		if (d & 0x04u) m |= 1u << RJ_LEFT;
		if (d & 0x08u) m |= 1u << RJ_RIGHT;
	}
	if (PRESSED(GPIO_A) && a_on) m |= 1u << RJ_A;
	if (PRESSED(GPIO_X))      m |= 1u << RJ_X;
	if (PRESSED(GPIO_LB))     m |= 1u << RJ_L;
	if (PRESSED(GPIO_RB))     m |= 1u << RJ_R;
	return m;
}

/* Arena (0x50000000, 64 MB, shared - only one core runs at a time). */
#define SNES_ROM_BUF   0x50000000u
#define SNES_ROM_CAP   0x00800000u        /* 8 MB (largest SNES carts ~6 MB) */
#define SNES_HEAP_BASE 0x50800000u
#define SNES_HEAP_SZ   0x02400000u        /* 36 MB for snes9x (SMW peaks ~19 MB); shrunk from
                                           * 48 MB to free mapped arena tail for the savestate
                                           * buffer - see SNES_STATE_BUF. */
/* Save-state buffer. The SNES session's MMU maps ONLY its own arena [0x50800000,0x53800000)
 * as WB during a game; addresses at/above 0x53800000 (the old buffer spot, AND the menu's
 * 0x54xxxxxx transition buffers) are UNMAPPED then - writes vanish and reads return a
 * constant, which silently corrupted every savestate ("save states broken"). So the buffer
 * MUST live inside the mapped arena. The heap is shrunk to 36 MB (ends 0x53000000), leaving
 * the mapped tail [0x52C00000,0x53800000) free; put the 4 MB state buffer at 0x52C00000
 * (ends 0x53000000) and the self-test scratch above it - both below the menu's wallpaper
 * cache (0x53200000) so nothing overlaps. */
#define SNES_STATE_BUF 0x52C00000u
#define SNES_STATE_CAP 0x00400000u        /* 4 MB (snes9x state ~0.8 MB, more with SA-1/SuperFX) */
#define SNES_STATE_SCRATCH 0x53000000u    /* self-test SD readback scratch (mapped arena tail) */
#define RESET_HOLD_FRAMES 30              /* SELECT+START+L+R held ~0.5 s = soft reset */

/* ---- in-game menu: state, rendering, actions ---- */
static const struct snes_core_exports *s_menu_c;
static fat_vol             *s_menu_vol;
static const gba_rom_entry *s_menu_rom;
static int  s_msel;
static char s_mstat[48];
#define SNES_SLOT_COUNT 3 /* manual save-state slots (0..2); the suspend point uses a separate "sus" */
static int  s_save_slot;   /* manual Save/Load state slot 0..SNES_SLOT_COUNT-1 */
static int  s_slot_used;   /* 1 if the current slot's file exists on the card (cached) */
static int  s_settings_dirty;   /* >0: a setting changed; counts down, persists to eMMC/SD at 0 */
#define SNES_SETTINGS_DEBOUNCE 30   /* ~0.5 s after the last change before the disk write */
/* Mark settings changed; the actual eMMC+SD write is debounced so holding a value key (or a
 * rapid series of taps) does not spam flash writes. Flushed by snes_settings_tick / on exit. */
static void snes_settings_touch(void) { s_settings_dirty = SNES_SETTINGS_DEBOUNCE; }
static void snes_settings_tick(void)
{ if (s_settings_dirty > 0 && --s_settings_dirty == 0) ayaneo_menu_settings_persist(); }
static void snes_settings_flush(void)
{ if (s_settings_dirty > 0) { s_settings_dirty = 0; ayaneo_menu_settings_persist(); } }

/* Hardware volume rocker (mirrors gba_driver.c poll_volume). Volume-up = mtk_detect_key(0x11),
 * volume-down = 0x00. Holding SELECT turns the rocker into a brightness control (matches the GBA
 * path). Edge-detected so one press = one step; the on-screen bar shows the new level, and the
 * change is debounced to disk via snes_settings_touch. Runs whether or not the menu is open. */
static void snes_poll_volume(void)
{
	static int vu_p, vd_p;
	int vu = mtk_detect_key(0x11), vd = mtk_detect_key(0x00);
	int sel = PRESSED(GPIO_SELECT);
	int dir = 0;
	if (vu && !vu_p) dir = +1; else if (vd && !vd_p) dir = -1;
	vu_p = vu; vd_p = vd;
	if (!dir) return;
	if (sel) {
		ayaneo_brightness_step(dir);
		ayaneo_gbc_osd_show(0, ayaneo_brightness_pct());
	} else {
		int v = ayaneo_gbc_audio_get_volume() + dir * 5;
		if (v < 0) v = 0; if (v > 100) v = 100;
		ayaneo_gbc_audio_set_volume(v);
		ayaneo_gbc_osd_show(1, v);
	}
	snes_settings_touch();
}

/* build the "st<slot>" extension for a manual save-state slot. */
static void snes_slot_ext(char *e) { e[0] = 's'; e[1] = 't'; e[2] = (char)('0' + s_save_slot); e[3] = 0; }

/* refresh s_slot_used by probing the current slot's file (16-byte read; cheap, menu-time only). */
static void snes_slot_check(void)
{
	unsigned char tmp[16]; char ext[4];
	if (!s_menu_vol || !s_menu_rom) { s_slot_used = 0; return; }
	snes_slot_ext(ext);
	s_slot_used = gba_sd_read_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, tmp, sizeof tmp) > 0;
}
enum { SM_BRIGHT, SM_VOLUME, SM_FILTER, SM_ASPECT, SM_OVERSCAN, SM_AUDIO, SM_HIRES,
       SM_CPU, SM_RUNAHEAD, SM_TURBO, SM_BENCH, SM_PANEL,
       SM_SLOT, SM_SAVE, SM_LOAD, SM_RESET, SM_CLOSE, SM_COUNT };

static const char *snes_turbo_name(int t)
{ return t == 1 ? "A" : t == 2 ? "B" : t == 3 ? "A+B" : "Off"; }

static const char *snes_filter_name(int f)
{ return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : "Off"; }

/* snes9x core-option choices exposed in the Pico menu: {label, libretro value}. The
 * runner pushes the selected value with c->set_option(key, value); snes9x reflows on the
 * next frame (geometry for aspect/overscan). Index per item is tracked in s_opt_idx[]. */
struct snes_opt_choice { const char *label, *value; };
static const struct snes_opt_choice s_asp_ch[]  = { {"4:3","4:3"}, {"Pixel","uncorrected"}, {"NTSC","ntsc"}, {"PAL","pal"}, {"Stretch","4:3"} };
#define SNES_ASPECT_STRETCH 4   /* index of "Stretch": fill the whole 1280x960 panel, no bars */
static const struct snes_opt_choice s_ovs_ch[]  = { {"Crop 8px","enabled"}, {"Crop 12px","12_pixels"}, {"Crop 16px","16_pixels"}, {"Off","disabled"} };
/* Linear is index 0 = the DEFAULT (matches the core fallback set to linear in libretro.cpp): it
 * is ~7 fps faster than Gaussian and index 0 is not pushed, so it relies on the core default. */
static const struct snes_opt_choice s_aud_ch[]  = { {"Linear","linear"}, {"Gaussian","gaussian"}, {"Cubic","cubic"}, {"Sinc","sinc"}, {"None","none"} };
static const struct snes_opt_choice s_hib_ch[]  = { {"Off","disabled"}, {"Merge","merge"}, {"Blur","blur"} };
enum { OI_ASPECT, OI_OVERSCAN, OI_AUDIO, OI_HIRES, OI_N };
static struct { const char *key; const struct snes_opt_choice *ch; int n; } s_opt_def[OI_N] = {
	{ "snes9x_aspect",              s_asp_ch, 5 },
	{ "snes9x_overscan",           s_ovs_ch, 4 },
	{ "snes9x_audio_interpolation", s_aud_ch, 5 },
	{ "snes9x_hires_blend",        s_hib_ch, 3 },
};
static int s_opt_idx[OI_N];   /* current selection per option (defaults = index 0) */

/* aspect ratio (x1000) the display should stretch to; 0 = integer/pixel. Updated by the
 * runner from c->aspect_x1000() whenever aspect/overscan changes. Read by ayaneo_snes_show_frame. */
volatile unsigned g_snes_aspect_x1000;
/* 1 = "Stretch": the display fills the whole panel (no bars), overriding g_snes_aspect_x1000. */
volatile int g_snes_stretch;
volatile int g_snes_dbg_stretch = -1;   /* oem snes-stretch: -1 persisted, 0/1 force aspect for measurement */
volatile unsigned g_snes_show_us;   /* last scale+flush time in us (set by ayaneo_snes_show_frame) */
volatile unsigned g_snes_flush_us;  /* isolated cache-clean time in us (subset of show_us) */

static char *smput(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *smputu(char *p, unsigned v) { char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; } while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n]; return p; }

static const char *sm_label(int i) { switch (i) {
	case SM_BRIGHT: return "Brightness"; case SM_VOLUME: return "Volume";
	case SM_FILTER: return "LCD Filter";
	case SM_ASPECT: return "Aspect Ratio"; case SM_OVERSCAN: return "Overscan";
	case SM_AUDIO:  return "Audio Filter";  case SM_HIRES:    return "Hi-Res Blend";
	case SM_CPU:    return "CPU Clock"; case SM_RUNAHEAD: return "Run-Ahead";
	case SM_TURBO:  return "Turbo (auto-fire)";
	case SM_BENCH:  return "Benchmark (Uncap)"; case SM_PANEL: return "Panel Refresh";
	case SM_SLOT:   return "Save Slot";
	case SM_SAVE:   return "Save State"; case SM_LOAD:   return "Load State";
	case SM_RESET:  return "Reset Game"; case SM_CLOSE:  return "Exit Game"; } return ""; }
static const char *sm_value(int i, char *buf) { char *p = buf;
	switch (i) {
	case SM_BRIGHT: p = smputu(p, (unsigned)ayaneo_brightness_pct()); p = smput(p, "%"); break;
	case SM_VOLUME: p = smputu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = smput(p, "%"); break;
	case SM_FILTER: p = smput(p, snes_filter_name(ayaneo_get_lcd_filter())); break;
	case SM_ASPECT: case SM_OVERSCAN: case SM_AUDIO: case SM_HIRES: {
		int oi = i - SM_ASPECT; p = smput(p, s_opt_def[oi].ch[s_opt_idx[oi]].label); break; }
	case SM_CPU: { static unsigned mhz, tick;   /* cache: PLL read-back low bits jitter */
		if (!mhz || tick-- == 0) { mhz = ayaneo_get_cpu_mhz(); tick = 40; }
		p = smputu(p, mhz); p = smput(p, " MHz"); break; }
	case SM_RUNAHEAD: { int pf = ayaneo_get_preempt_frames();
		/* When the state does not fit the look-ahead slot the tier is inert, so show a short
		 * "N/A (>2MB)" (not the tier name, which would mislead AND, for the longer names, run
		 * into the row label). */
		if (!s_snes_ra_avail && pf > 0) p = smput(p, "N/A (>2MB)");
		else p = smput(p, snes_ra_name(pf)); break; }
	case SM_TURBO: p = smput(p, snes_turbo_name(g_snes_turbo)); break;
	case SM_BENCH: if (g_snes_benchmark) { p = smputu(p, (unsigned)s_snes_fps); p = smput(p, " fps"); }
		else p = smput(p, "Off"); break;
	case SM_PANEL: { unsigned hz = s_snes_hz1000;   /* Hz*1000 -> "60.11 Hz" */
		p = smputu(p, hz / 1000); p = smput(p, ".");
		{ unsigned f = (hz % 1000) / 10; if (f < 10) p = smput(p, "0"); p = smputu(p, f); }
		p = smput(p, " Hz"); break; }
	case SM_SLOT: p = smputu(p, (unsigned)s_save_slot); p = smput(p, s_slot_used ? " used" : " empty"); break;
	case SM_SAVE: case SM_LOAD: case SM_RESET: case SM_CLOSE: p = smput(p, "[A]"); break;
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
	case SM_BRIGHT: if (dir) { ayaneo_brightness_step(dir); snes_settings_touch(); } break;
	case SM_VOLUME: if (dir) { ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); snes_settings_touch(); } break;
	case SM_FILTER: if (dir) { ayaneo_set_lcd_filter((ayaneo_get_lcd_filter() + dir + 4) % 4); snes_settings_touch(); } break;
	case SM_ASPECT: case SM_OVERSCAN: case SM_AUDIO: case SM_HIRES:
		if (dir && s_menu_c->set_option) {
			int oi = i - SM_ASPECT, n = s_opt_def[oi].n, k;
			unsigned packed = 0;
			s_opt_idx[oi] = (s_opt_idx[oi] + dir + n) % n;
			s_menu_c->set_option(s_opt_def[oi].key, s_opt_def[oi].ch[s_opt_idx[oi]].value);
			if (oi == OI_ASPECT) {
				g_snes_stretch = (s_opt_idx[OI_ASPECT] == SNES_ASPECT_STRETCH);
				/* Live preview: the display reads g_snes_aspect_x1000 every frame, so refresh it
				 * now (the core's periodic refresh is skipped while the menu freezes emulation). */
				if (s_menu_c->aspect_x1000) g_snes_aspect_x1000 = s_menu_c->aspect_x1000();
			}
			/* Overscan/Hi-Res change the CORE's rendered frame (height/blend). The game now runs
			 * every frame (no menu freeze), so the change previews live with no extra step. */
			for (k = 0; k < OI_N; k++) packed |= (unsigned)(s_opt_idx[k] & 0xFF) << (k * 8);
			ayaneo_set_snes_opts(packed);   /* persist all picks */
			snes_settings_touch();
		}
		break;
	case SM_CPU:    if (dir) snes_cpu_step(dir); break;   /* not persisted (session floors at 1800) */
	case SM_RUNAHEAD: if (dir) {
		int pf = (ayaneo_get_preempt_frames() + dir + 4) % 4;
		ayaneo_set_preempt_frames(pf);
		ayaneo_set_cpu_mhz(s_snes_ra_opp[pf]);   /* escalate the clock for pf+1 emulations/frame */
		snes_settings_touch();
	} break;
	case SM_TURBO: if (dir) { g_snes_turbo = (g_snes_turbo + dir + 4) % 4;
			ayaneo_set_snes_turbo(g_snes_turbo); snes_settings_touch(); } break;
	case SM_BENCH:  if (act) g_snes_benchmark = !g_snes_benchmark; break;   /* A toggles (not L/R, which auto-repeat) */
	case SM_PANEL:  break;   /* read-only */
	case SM_SLOT: if (dir) { s_save_slot = (s_save_slot + dir + SNES_SLOT_COUNT) % SNES_SLOT_COUNT;
			ayaneo_set_snes_slot(s_save_slot); snes_settings_touch();
			snes_slot_check(); } break;   /* refresh used/empty for the new slot */
	case SM_SAVE: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF; unsigned ssz = s_menu_c->state_size();
			char ext[4]; int ok; void *m = s_menu_c->heap_mark ? s_menu_c->heap_mark() : 0;
			snes_slot_ext(ext);
			ok = (ssz && ssz <= SNES_STATE_CAP && s_menu_c->state_save(st, ssz) == 0 &&
			      gba_sd_write_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, st, ssz) == 0);
			smput(s_mstat, ok ? "State saved" : "Save failed");
			if (ok) s_slot_used = 1;
			if (m && s_menu_c->heap_reset) s_menu_c->heap_reset(m); } break;
	case SM_LOAD: if (act) { unsigned char *st = (unsigned char *)SNES_STATE_BUF;
			char ext[4]; unsigned n; void *m = s_menu_c->heap_mark ? s_menu_c->heap_mark() : 0;
			snes_slot_ext(ext);
			n = gba_sd_read_named(s_menu_vol, "/states/snes", s_menu_rom->name, ext, st, SNES_STATE_CAP);
			smput(s_mstat, (n && s_menu_c->state_load(st, n) == 0) ? "State loaded" : "No save state");
			if (m && s_menu_c->heap_reset) s_menu_c->heap_reset(m); } break;
	case SM_RESET: if (act) { s_menu_c->reset(); return 1; } break;
	case SM_CLOSE: if (act) { g_snes_menu_exit = 1; return 1; } break;   /* exit to the ROM selector */
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
	g_snes_dbg_heapused = 0;
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

	/* Suspend/resume: reload the AUTO save STATE from the last exit so the game resumes
	 * where it left off, unless B is held at launch (start fresh). Mirrors the GB/GBC flow.
	 * IMPORTANT: the auto slot is "sus", SEPARATE from the manual "st0" slot the Pico menu
	 * Save/Load use - otherwise every exit would clobber the player's manual save (that was
	 * the "save states are broken" bug). The host round-trip proved snes9x
	 * serialize/unserialize is deterministic. */
	if (!PRESSED(GPIO_B)) {
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned n = gba_sd_read_named(vol, "/states/snes", rom->name, "sus", st, SNES_STATE_CAP);
		if (n) c->state_load(st, n);
	}

	/* snes9x mainline is accuracy-first (full 65816 + SPC700 + PPU + DSP each frame), which
	 * is heavy on the A55; the menu/other cores idle around 1200 MHz. Raise the ARM PLL to
	 * at least 1400 MHz for the SNES session so plain-mapper games hit full speed, but never
	 * downclock a user who manually picked a higher OPP. Restored on session exit. NOTE:
	 * ayaneo_set_cpu_mhz only moves the PLL, not core voltage - 1400 is the OPP just above
	 * the boot Vproc point, the same one the "Max" preempt tier uses. */
	saved_mhz = ayaneo_get_cpu_mhz();
	if (g_snes_dbg_bench) {
		/* Headless uncapped benchmark: run at the guaranteed-stable 1400 MHz. Sustained 100%
		 * duty at 1800+ (no Vproc scaling in LK) BROWNS OUT the core - the uncapped bench faulted
		 * the device with dfar=0xffffffff. Normal play is vsync-capped (the CPU idles ~40% of each
		 * frame) so 1800 is fine there, but the bench must never crash the device. */
		g_snes_benchmark = 1;
		ayaneo_set_cpu_mhz(1400);
	} else if (saved_mhz < 1800) {
		ayaneo_set_cpu_mhz(1800);   /* SNES default: 1800 MHz PLL (reads back 1799), capped play */
	}

	/* If the menu armed a launch punch (snapshot at 0x54000000, gba_punch_ready), DON'T
	 * clear to black - keep the frozen menu on screen so the growing gameplay circle opens
	 * over it (matches GBA/GBC). Otherwise blank the panel. */
	{ extern int gba_punch_ready; if (!gba_punch_ready) ayaneo_display_prepare(); }
	ayaneo_gbc_audio_init();
	ayaneo_snes_audio_reset();
	mtk_wdt_disable();
	/* Switch the panel to ~60.11 Hz for SNES (vfp swap). The vsync-locked present in
	 * ayaneo_snes_show_frame then paces emulation to the panel scan - smooth, tear-free,
	 * no 13 MHz busy-wait needed. Restored to 59.749 Hz on exit below. */
	ayaneo_dsi_set_vfp(SNES_VFP);
	g_snes_dbg_vfp = ayaneo_dsi_get_vfp();   /* read-back: should equal SNES_VFP (17) */

	/* hand the in-game menu this session's context */
	s_menu_c = c; s_menu_vol = vol; s_menu_rom = rom;
	s_msel = 0; s_mstat[0] = 0; g_snes_menu_open = 0; g_snes_menu_exit = 0;
	s_save_slot = ayaneo_get_snes_slot();   /* restore the last-used manual save slot */
	if (s_save_slot < 0 || s_save_slot >= SNES_SLOT_COUNT) s_save_slot = 0;   /* clamp stale 0..9 values */
	g_snes_turbo = ayaneo_get_snes_turbo(); /* restore the persisted auto-fire setting */
	s_settings_dirty = 0;
	snes_slot_check();                      /* seed the used/empty indicator for it */
	/* Restore the persisted snes9x option picks (aspect/overscan/audio/hires) and push them
	 * to the core so a game launches with the player's last choices, not defaults. */
	{
		unsigned packed = ayaneo_get_snes_opts();
		int oi;
		for (oi = 0; oi < OI_N; oi++) {
			int idx = (int)((packed >> (oi * 8)) & 0xFF);
			if (idx < 0 || idx >= s_opt_def[oi].n) idx = 0;
			s_opt_idx[oi] = idx;
			/* Push non-default options; ALWAYS push audio interpolation (even idx 0 = "linear")
			 * so the core gets it explicitly instead of relying on its fallback default. */
			if ((idx != 0 || oi == OI_AUDIO) && c->set_option)
				c->set_option(s_opt_def[oi].key, s_opt_def[oi].ch[idx].value);
		}
		g_snes_stretch = (s_opt_idx[OI_ASPECT] == SNES_ASPECT_STRETCH);
		if (g_snes_dbg_stretch >= 0) g_snes_stretch = g_snes_dbg_stretch;   /* oem snes-stretch test override */
	}
	g_snes_aspect_x1000 = (c->aspect_x1000) ? c->aspect_x1000() : 0;

	/* Launch punch-hole (matches GBA/GBC): run a few frames so real gameplay is on screen
	 * (not a black/boot frame), pre-render it full-screen, then grow a circle from 0 to the
	 * panel diagonal over the frozen menu snapshot at 0x54000000. Skipped if the menu didn't
	 * arm it (e.g. oem snes-launch). */
	{
		extern int  gba_punch_ready;
		extern void ayaneo_snes_punch_prerender(const unsigned short *, unsigned, unsigned, unsigned);
		extern void ayaneo_gba_punch_frame_pre(const unsigned int *, int);
		if (gba_punch_ready) {
			struct snes_frame pf; int i, w; pf.video = 0;
			gba_punch_ready = 0;
			for (w = 0; w < 15; w++) { c->run(&pf); if (pf.video && pf.width && w >= 3) break; }
			if (c->aspect_x1000) g_snes_aspect_x1000 = c->aspect_x1000();
			if (pf.video && pf.width && pf.height) {
				ayaneo_snes_punch_prerender((const unsigned short *)pf.video, pf.width, pf.height, pf.pitch / 2u);
				for (i = 1; i <= 20; i++) {
					int r = 820 * i / 20; if (r < 1) r = 1;
					ayaneo_gba_punch_frame_pre((const unsigned int *)0x54000000u, r);
					mtk_wdt_restart();
				}
			}
		}
	}

	/* Run-ahead enable gate: the state must fit in the 2 MB SNES_AHEAD_BUF slot. ra_ssz != 0
	 * enables run-ahead; the save/load themselves pass the full buffer CAPACITY (not this
	 * one-shot size) so a game whose state grows later never truncates - unfreeze stops at
	 * the real end regardless. */
	unsigned ra_ssz = c->state_size();
	if (ra_ssz == 0 || ra_ssz > SNES_AHEAD_CAP) ra_ssz = 0;
	s_snes_ra_avail = (ra_ssz != 0);   /* surface in the Run-Ahead menu row when a tier is inert */
	if (!g_snes_dbg_bench) {   /* bench pins 1400 itself; do not disturb it */
		int pf0 = ayaneo_get_preempt_frames();   /* persisted run-ahead tier: pin its escalated clock */
		if (pf0 > 0 && pf0 <= 3) ayaneo_set_cpu_mhz(s_snes_ra_opp[pf0]);   /* 1600/1800/2000 by tier */
	}

	int reset_hold = 0, aya_prev = 0, ff_prev = 0;
	int up_p = 0, dn_p = 0, lt_p = 0, rt_p = 0, a_p = 0, b_p = 0;
	int up_h = 0, dn_h = 0, lt_h = 0, rt_h = 0;   /* hold-frame counters for nav auto-repeat */
	struct snes_frame f;
	f.video = 0; f.width = 0; f.height = 0; f.pitch = 0; f.audio = 0; f.frames = 0;
	for (;;) {
		int aya;
		mtk_wdt_restart();

		{ extern void ayaneo_joypad_poll(void); ayaneo_joypad_poll(); }  /* once/frame: cache stick+triggers */

		/* Hardware volume rocker works whether or not the menu is open. */
		snes_poll_volume();

		/* Render/refresh the Pico menu as a hardware overlay layer (OVL0 L0) over the running game,
		 * or disable it when closed - BEFORE the present, so g_overlay_active (which makes the RSZ
		 * path single-buffer to free 0x55900000 for the overlay) is consistent with the layer state
		 * on the frame it toggles. Reflects the previous frame's selection (1-frame lag, imperceptible). */
		{
			extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
			ayaneo_menu_overlay(snes_menu_paint, g_snes_menu_open);
		}

		/* Freeze emulation while the in-game menu is open: skip c->run so the overlay always
		 * fits the frame budget. The game clock is pinned to the run-ahead tier (Off=1400 MHz),
		 * so a menu opened with run-ahead disabled used to drop frames rendering game+overlay at
		 * the low clock. The last frame f persists and is re-presented below under the overlay;
		 * its input is gated off in ayaneo_snes_pad_mask regardless. */
		/* Compute fast-forward + run-ahead depth UP FRONT so the render/audio skip can be armed
		 * before the committed frame runs. When run-ahead presents a look-ahead frame, the
		 * committed frame's OWN video is thrown away, so skip rendering it (its audio is still the
		 * committed audio, so keep that). The headless test keeps video so frame validation works. */
		int ff_lvl;
		{ extern int ayaneo_joypad_ff_level(void); ff_lvl = ayaneo_joypad_ff_level(); }  /* RT, cached */
		int ff = ff_lvl > 0 && !g_snes_menu_open && !g_snes_benchmark && !g_snes_test_limit;
		ff_prev = ff;   /* FF audio stays continuous (speeds up); no ring clear on entry */
		int pf = 0;
		{
			extern volatile int g_snes_dbg_ra;   /* oem snes-ra:N forces run-ahead depth in the test */
			if (!ff && !g_snes_menu_open && ra_ssz && (!g_snes_benchmark || g_snes_test_limit))
				pf = g_snes_test_limit ? g_snes_dbg_ra : ayaneo_get_preempt_frames();
		}

		{	/* Run the game EVERY frame - the Pico menu no longer freezes it (it is now an
			 * independent hardware overlay). Run-ahead and fast-forward are already gated off
			 * while the menu is open (pf=0, ff=0 above), so this is a single committed frame; the
			 * game visibly keeps running under the menu and setting changes preview live. */
			if (c->set_av_skip) c->set_av_skip(((pf > 0 || ff) && !g_snes_test_limit) ? 1 : 0, 0);
			c->run(&f);
			g_snes_dbg_frames++;
			/* keep the display's target aspect current (cheap; refreshed periodically) - it
			 * changes when the player switches Aspect Ratio or Overscan in the menu. */
			if (c->aspect_x1000 && (g_snes_dbg_frames & 15u) == 0) g_snes_aspect_x1000 = c->aspect_x1000();
			if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) { g_snes_dbg_exit = 5; }
			/* Diagnostic full-frame sample (non-zero pixel count + content hash) feeds only the
			 * oem diag / headless snes-px counters - a bootloader-side debug path never read
			 * during real play. So sample EVERY frame in the frame-limited test (keeps chg/nz
			 * exact for validation) but only 1-in-16 in live play, where it is otherwise ~900
			 * wasted video reads + hash steps per frame. */
			if (f.video && f.width && f.height &&
			    (g_snes_test_limit || (g_snes_dbg_frames & 15u) == 0)) {
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
			}
		}
		/* Committed-frame audio submitted BEFORE any run-ahead look-ahead overwrites the
		 * blob's audio buffer; muted while benchmarking or fast-forwarding so the ring never
		 * throttles the uncapped loop. (ff / pf were computed up front, above.) */
		if (f.audio && f.frames && !g_snes_benchmark) {
			g_snes_dbg_audframes += f.frames;
			ayaneo_snes_audio_submit(f.audio, f.frames, sr ? sr : 32040u);
		}
		/* Variable fast-forward (right trigger): run extra committed frames per display frame,
		 * scaled by press depth, submitting each frame's audio so the sound speeds up too. The
		 * vsync-locked present below rate-limits it (2x .. CPU-bound = fastest at full press).
		 * Run-ahead (pf) is already gated off while ff is active. */
		if (ff) {
			int mult = 2 + (ff_lvl * (10 - 2)) / 255;   /* 2..10 */
			int k;
			for (k = 1; k < mult; k++) {
				/* skip the PPU render (the heavy part) on the thrown-away frames; render only
				 * the LAST one (the frame we actually present). Audio kept on all -> speeds up. */
				if (c->set_av_skip) c->set_av_skip(k < mult - 1 ? 1 : 0, 0);
				c->run(&f);
				g_snes_dbg_frames++;
				if (f.audio && f.frames)
					ayaneo_snes_audio_submit(f.audio, f.frames, sr ? sr : 32040u);
			}
			if (c->set_av_skip) c->set_av_skip(0, 0);
		}
		/* Run-ahead: advance the DISPLAY pf frames into the future with the current input,
		 * then rewind so the real emulation still advances exactly one frame per loop. */
		{
			int i, raw = 0;
			void *hmark = 0;
			if (pf > 0) {
				/* RAW run-ahead snapshot (state_save_ra) when the core offers it and the cart is
				 * plain: an in-memory freeze with no big-endian byte-swap, no per-struct alloc, and
				 * only the real SRAM extent - measured ~3-5x cheaper than the serialize. Returns 0
				 * for special-chip carts; then fall back to the fast serialize (which leaks temps
				 * into the bump arena, so bracket THAT path with heap_mark/reset + set_ra_fast). */
				if (c->state_save_ra && c->state_load_ra)
					raw = (int)c->state_save_ra((void *)SNES_AHEAD_BUF, SNES_AHEAD_CAP);
				if (!raw) {
					if (c->set_ra_fast) c->set_ra_fast(1);
					if (c->heap_mark) hmark = c->heap_mark();
					c->state_save((void *)SNES_AHEAD_BUF, SNES_AHEAD_CAP);
				}
				/* Only the LAST look-ahead frame is presented (needs video); NONE are heard
				 * (audio hard-disabled). Skipping the PPU render on the throwaway frames and the
				 * APU/DSP on all of them is the bulk of the run-ahead speedup - the emulated
				 * CPU/APU state still advances exactly, so the rewind is bit-identical. */
				for (i = 0; i < pf; i++) {
					if (c->set_av_skip) c->set_av_skip((i == pf - 1) ? 0 : 1, 1);
					c->run(&f);
				}
				if (c->set_av_skip) c->set_av_skip(0, 0);
			}
			/* Uncapped presentation for BOTH fast-forward and Benchmark: present ~every 8th
			 * frame and skip the blit entirely on the other 7. Benchmark previously blitted every
			 * frame, so the reported FPS was capped by the ~5 ms scaler+flush, not the emulator -
			 * you could not see the real max. With the sparse present the loop measures pure
			 * emulation throughput (ayaneo_snes_show_frame also skips the vsync wait when
			 * snes_benchmark_on(), so nothing paces it). */
			int uncapped = g_snes_benchmark;   /* FF uses the normal vsync present; the extra committed frames above rate-limit it */
			if (uncapped) {
				if ((g_snes_dbg_frames & 7u) == 0 && f.video && f.width && f.height)
					ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			} else if (f.video && f.width && f.height)
				ayaneo_snes_show_frame((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			else
				priamry_display_wait_for_vsync();   /* keep pacing if a frame was dropped */
			if (pf > 0) {
				if (raw) {
					c->state_load_ra((const void *)SNES_AHEAD_BUF);   /* raw rewind */
				} else {
					c->state_load((const void *)SNES_AHEAD_BUF, SNES_AHEAD_CAP);   /* rewind */
					if (hmark && c->heap_reset) c->heap_reset(hmark);      /* free the temporaries */
					if (c->set_ra_fast) c->set_ra_fast(0);   /* back to portable format for SD saves */
				}
			}
			/* leak watch (headless test): peak arena usage should stay FLAT with run-ahead
			 * on if the mark/reset works; without it, it climbs ~0.5 MB/frame. */
			if (g_snes_test_limit && c->dbg_get) {
				unsigned hu = c->dbg_get(0);
				if (hu > g_snes_dbg_heapused) g_snes_dbg_heapused = hu;
			}
		}

		/* Timing off the 13 MHz gpt4 counter (NOT 812.5 kHz - that was the bug that showed
		 * ~2.7 Hz). Panel refresh: 8-frame average of the vsync-locked loop period, Hz*1000 =
		 * 8*13e9/ticks; reject doubled/outlier frames (period outside 8..23 ms) exactly like
		 * the GBA path. Benchmark FPS: frames * 13e6 / ticks over ~0.5 s. */
		{
			static unsigned h_acc, h_n, h_last;
			static unsigned b_acc, b_n, b_last;
			unsigned now = gpt4_get_current_tick();
			if (h_last) { unsigned lt = now - h_last;
				if (lt > 104000u && lt < 300000u) { h_acc += lt; if (++h_n >= 8) {
					if (h_acc) s_snes_hz1000 = (unsigned)(104000000000ULL / (unsigned long long)h_acc);
					h_acc = 0; h_n = 0; } } }
			h_last = now;
			if (g_snes_benchmark) {
				if (b_last) { b_acc += now - b_last; b_n++;
					if (b_acc >= 6500000u) {   /* ~0.5 s at 13 MHz */
						s_snes_fps = (int)((unsigned long long)b_n * 13000000ULL / b_acc);
						g_snes_dbg_benchfps = (unsigned)s_snes_fps;   /* headless read-back */
						b_acc = 0; b_n = 0; } }
				b_last = now;
			} else { b_last = 0; b_acc = 0; b_n = 0; }
		}

		snes_settings_tick();   /* debounced settings persist (~0.5 s after the last change) */

		/* AYA taps toggle the menu; holding AYA ~1.5 s force-exits to the selector. */
		aya = PRESSED(GPIO_AYA);
		if (aya && !aya_prev) { g_snes_menu_open = !g_snes_menu_open; s_mstat[0] = 0;
			ayaneo_menu_overlay_mark_dirty();                    /* repaint the overlay on open/close */
			if (!g_snes_menu_open) snes_settings_flush(); }      /* flush pending on menu close; game keeps running + audio keeps playing */
		aya_prev = aya;
		if (aya) { if (++aya_hold >= 90) {
			/* Arm the reverse punch: the menu re-entry shrinks this frozen frame back into
			 * the carousel (matches GBA/GBC). Not for the headless test-limit exit. */
			extern void snes_menu_arm_reverse(const unsigned short *, unsigned, unsigned, unsigned);
			if (!g_snes_test_limit && f.video && f.width && f.height)
				snes_menu_arm_reverse((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			g_snes_dbg_exit = 1; break;
		} } else aya_hold = 0;

		if (g_snes_menu_open) {
			int up = PRESSED(GPIO_UP), dn = PRESSED(GPIO_DOWN);
			int lt = PRESSED(GPIO_LEFT), rt = PRESSED(GPIO_RIGHT);
			int a = PRESSED(GPIO_A), b = PRESSED(GPIO_B);
			/* Up/Down (navigate) and Left/Right (adjust value) auto-repeat: fire on press,
			 * then after ~0.37 s of holding repeat every ~0.08 s - useful with 16 rows and
			 * up-to-10-value items. The per-change eMMC/SD write is debounced (see
			 * snes_settings_touch), and Benchmark toggles only on A, so repeating L/R is safe.
			 * FIRE = press edge or repeat tick. */
			#define NAV_DELAY 22
			#define NAV_REP   5
			#define FIRE(h)   ((h) == 1 || ((h) > NAV_DELAY && (((h) - NAV_DELAY) % NAV_REP) == 0))
			up_h = up ? up_h + 1 : 0; dn_h = dn ? dn_h + 1 : 0;
			lt_h = lt ? lt_h + 1 : 0; rt_h = rt ? rt_h + 1 : 0;
			if ((up && FIRE(up_h)) || (dn && FIRE(dn_h)) || (lt && FIRE(lt_h)) || (rt && FIRE(rt_h)) ||
			    (a && !a_p) || (b && !b_p))
				ayaneo_menu_overlay_mark_dirty();   /* content changes -> repaint the static overlay */
			if (up && FIRE(up_h)) s_msel = (s_msel + SM_COUNT - 1) % SM_COUNT;
			if (dn && FIRE(dn_h)) s_msel = (s_msel + 1) % SM_COUNT;
			if (lt && FIRE(lt_h)) sm_change(s_msel, -1, 0);
			if (rt && FIRE(rt_h)) sm_change(s_msel, +1, 0);
			#undef NAV_DELAY
			#undef NAV_REP
			#undef FIRE
			if (a  && !a_p)  { if (sm_change(s_msel, 0, 1)) { g_snes_menu_open = 0; g_snes_ab_latch |= 1; snes_settings_flush(); } }
			if (b  && !b_p)  { g_snes_menu_open = 0; g_snes_ab_latch |= 2; snes_settings_flush(); }
			up_p = up; dn_p = dn; lt_p = lt; rt_p = rt; a_p = a; b_p = b;
			reset_hold = 0;
		} else {
			/* Soft reset: SELECT+START+L+R held ~0.5 s (mirrors GB/GBC/GBA). */
			if (PRESSED(GPIO_SELECT) && PRESSED(GPIO_START) && PRESSED(GPIO_LB) && PRESSED(GPIO_RB)) {
				if (++reset_hold >= RESET_HOLD_FRAMES) { c->reset(); reset_hold = 0; }
			} else reset_hold = 0;
		}
		/* Pico-menu "Exit Game" selected: leave the session like the AYA-hold exit, arming the
		 * reverse-punch so the frozen frame shrinks back into the carousel (matches GBA/GBC). */
		if (g_snes_menu_exit) {
			extern void snes_menu_arm_reverse(const unsigned short *, unsigned, unsigned, unsigned);
			if (!g_snes_test_limit && f.video && f.width && f.height)
				snes_menu_arm_reverse((const unsigned short *)f.video, f.width, f.height, f.pitch / 2u);
			g_snes_dbg_exit = 1; break;
		}
		if (g_snes_test_limit && g_snes_dbg_frames >= g_snes_test_limit) break;   /* oem snes-launch */
	}

	/* Headless save-state regression check - only in frame-limited (oem snes-launch) mode,
	 * so it never touches normal play. Exercises the full chain: core freeze -> SD write ->
	 * SD read-back byte-compare -> core unfreeze. ss_core=1 & ss_sd=1 means savestates work. */
	if (g_snes_test_limit) {
		unsigned char *st  = (unsigned char *)SNES_STATE_BUF;
		unsigned char *st2 = (unsigned char *)SNES_STATE_SCRATCH;
		unsigned ssz = c->state_size();
		g_snes_dbg_ss_size = ssz; g_snes_dbg_ss_core = 0; g_snes_dbg_ss_sd = 0;
		if (ssz && ssz <= SNES_STATE_CAP && c->state_save(st, ssz) == 0) {
			int wr = gba_sd_write_named(vol, "/states/snes", rom->name, "sstst", st, ssz);
			unsigned rd = gba_sd_read_named(vol, "/states/snes", rom->name, "sstst", st2, SNES_STATE_CAP);
			if (wr == 0 && rd == ssz) {
				unsigned i, diff = 0;
				for (i = 0; i < ssz; i++) if (st[i] != st2[i]) { diff = 1; break; }
				g_snes_dbg_ss_sd = diff ? 0 : 1;
			}
			g_snes_dbg_ss_core = (c->state_load(st, ssz) == 0) ? 1 : 0;
			/* fast-savestate (run-ahead path) round-trip: save+load in fast mode and confirm
			 * it succeeds, so enabling it for run-ahead is proven correct headlessly. */
			if (c->set_ra_fast) {
				void *fm = c->heap_mark ? c->heap_mark() : 0;
				c->set_ra_fast(1);
				g_snes_dbg_ss_fast = (c->state_save(st, SNES_AHEAD_CAP) == 0 &&
						      c->state_load(st, SNES_AHEAD_CAP) == 0) ? 1 : 0;
				c->set_ra_fast(0);
				if (fm && c->heap_reset) c->heap_reset(fm);
			}
			/* ---- run-ahead correctness proof (the point of run-ahead) ----
			 * From one saved committed state, produce the N-ahead frame TWICE: once rendering
			 * every look-ahead frame, once skipping the render on all but the last (exactly what
			 * live run-ahead does). If the two N-ahead frames are bit-identical, skipping the PPU
			 * render did NOT perturb emulation state, i.e. the look-ahead the player sees is the
			 * genuine future - run-ahead is working, not just burning CPU. Also confirm the N-ahead
			 * frame differs from the committed 1-ahead frame (it is really looking ahead). */
			if (c->set_ra_fast && c->set_av_skip && ssz && ssz <= SNES_STATE_CAP) {
				int N = (g_snes_dbg_ra > 0) ? g_snes_dbg_ra : 3, i;
				void *fm = c->heap_mark ? c->heap_mark() : 0;
				struct snes_frame tf;
				unsigned h_committed, h_full, h_skip;
				c->set_ra_fast(1);
				c->state_save(st, SNES_AHEAD_CAP);
				c->set_av_skip(0, 0); c->run(&tf); h_committed = snes_hash_frame(&tf);   /* 1-ahead */
				c->state_load(st, SNES_AHEAD_CAP);
				for (i = 0; i < N; i++) { c->set_av_skip(0, 0); c->run(&tf); }            /* N-ahead, full render */
				h_full = snes_hash_frame(&tf);
				c->state_load(st, SNES_AHEAD_CAP);
				for (i = 0; i < N; i++) { c->set_av_skip(i == N - 1 ? 0 : 1, 1); c->run(&tf); }  /* N-ahead, skip pattern */
				h_skip = snes_hash_frame(&tf);
				c->set_av_skip(0, 0);
				c->state_load(st, SNES_AHEAD_CAP);   /* restore committed state */
				c->set_ra_fast(0);
				if (fm && c->heap_reset) c->heap_reset(fm);
				g_snes_dbg_ra_depth = (unsigned)N;
				g_snes_dbg_ra_ok    = (h_full && h_full == h_skip) ? 1 : 0;
				g_snes_dbg_ra_ahead = (h_full && h_full != h_committed) ? 1 : 0;
				/* Validate the RAW snapshot against that trusted serialize truth: raw-save the
				 * committed state, run the same skip pattern, and confirm the N-ahead frame matches
				 * h_full. If it does, the raw freeze/rewind is state-exact (safe to use live). */
				if (c->state_save_ra && c->state_load_ra) {
					unsigned rn = c->state_save_ra((void *)SNES_STATE_SCRATCH, SNES_AHEAD_CAP);
					if (rn) {
						unsigned h_raw;
						for (i = 0; i < N; i++) { c->set_av_skip(i == N - 1 ? 0 : 1, 1); c->run(&tf); }
						h_raw = snes_hash_frame(&tf);
						c->set_av_skip(0, 0);
						c->state_load_ra((const void *)SNES_STATE_SCRATCH);   /* restore committed */
						g_snes_dbg_ra_raw = (h_full && h_full == h_raw) ? 1 : 0;
					} else {
						g_snes_dbg_ra_raw = 2;   /* special-chip cart: run-ahead uses serialize */
					}
				}
			}
			/* ---- CLEAN emulation cost breakdown (display-independent) ----
			 * The live snes-bench runs through the real loop, so it is polluted by the vsync
			 * pacing and the RSZ hardware present (the config_input frame-done wait) - it measures
			 * the display path, not the emulator. Here we time TIGHT loops of just c->run() and the
			 * state ops off the 13 MHz counter, no present, no vsync, so the numbers are pure core:
			 *   core_us   = one headless frame (video+audio skipped) = the run-ahead look-ahead cost
			 *   render_us = one fully-rendered frame                = normal committed-frame cost
			 *   save_us   = one fast state_save   (run-ahead per-frame overhead)
             *   load_us   = one fast state_load   (run-ahead per-frame overhead)
			 * fps = 1e6 / us. A Max-run-ahead frame costs ~ render_us + (N-1)*core_us + save+load. */
			if (c->set_av_skip && c->set_ra_fast && ssz && ssz <= SNES_STATE_CAP) {
				extern unsigned int gpt4_get_current_tick(void);
				int i; unsigned t0;
				void *fm = c->heap_mark ? c->heap_mark() : 0;
				struct snes_frame tf;
				const int NB = 240, NS = 60;
				c->set_ra_fast(1);
				c->state_save(st, SNES_AHEAD_CAP);
				c->set_av_skip(1, 1);                                  /* headless: skip video+audio */
				t0 = gpt4_get_current_tick();
				for (i = 0; i < NB; i++) c->run(&tf);
				g_snes_dbg_core_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NB);
				c->state_load(st, SNES_AHEAD_CAP);
				c->set_av_skip(0, 0);                                  /* full render */
				t0 = gpt4_get_current_tick();
				for (i = 0; i < NB; i++) c->run(&tf);
				g_snes_dbg_render_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NB);
				c->state_load(st, SNES_AHEAD_CAP);
				/* save/load cost: measure the RAW run-ahead path when available (what live run-ahead
				 * uses now), else the fast serialize it falls back to. */
				if (c->state_save_ra && c->state_load_ra && c->state_save_ra(st, SNES_AHEAD_CAP)) {
					t0 = gpt4_get_current_tick();
					for (i = 0; i < NS; i++) c->state_save_ra(st, SNES_AHEAD_CAP);
					g_snes_dbg_save_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NS);
					t0 = gpt4_get_current_tick();
					for (i = 0; i < NS; i++) c->state_load_ra(st);
					g_snes_dbg_load_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NS);
				} else {
					t0 = gpt4_get_current_tick();                          /* fast state_save cost */
					for (i = 0; i < NS; i++) { void *m = c->heap_mark ? c->heap_mark() : 0;
						c->state_save(st, SNES_AHEAD_CAP); if (m && c->heap_reset) c->heap_reset(m); }
					g_snes_dbg_save_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NS);
					t0 = gpt4_get_current_tick();                          /* fast state_load cost */
					for (i = 0; i < NS; i++) c->state_load(st, SNES_AHEAD_CAP);
					g_snes_dbg_load_us = (gpt4_get_current_tick() - t0) / (13u * (unsigned)NS);
				}
				c->state_load(st, SNES_AHEAD_CAP);   /* restore committed */
				c->set_ra_fast(0);
				if (fm && c->heap_reset) c->heap_reset(fm);
			}
		}
		/* Verify the exit reverse-punch freeze buffer (GBA_GAME_FREEZE_PA = 0x55800000) is
		 * mapped/writable DURING a SNES session - the AYA-hold exit writes the frozen frame
		 * there, a path the frame-limit test never takes. revmap=1 => the write sticks. */
		{
			volatile unsigned *rp = (volatile unsigned *)0x55800000u;
			*rp = 0xCAFEBABEu;   /* faults here if the region is unmapped during the session */
			g_snes_dbg_revmap = (*rp == 0xCAFEBABEu) ? 1 : 0;
		}
	}

	g_snes_menu_open = 0;
	g_snes_test_limit = 0;
	if (g_snes_dbg_bench) { g_snes_benchmark = 0; g_snes_dbg_bench = 0; }   /* headless bench: reset */
	snes_settings_flush();   /* write any pending settings change before leaving the session */

	ayaneo_dsi_set_vfp(DEFAULT_VFP);   /* restore 59.749 Hz for the menu / other cores */
	if (saved_mhz) ayaneo_set_cpu_mhz(saved_mhz);   /* restore the pre-session ARM clock */
	{	/* If the session presented through the hardware resizer, put the display path back to
		 * full-panel passthrough so the menu (which draws panel-sized frames) renders 1:1. */
		extern volatile int g_snes_rsz;
		extern void ayaneo_snes_rsz_restore(void);
		if (g_snes_rsz) ayaneo_snes_rsz_restore();
	}

	/* Suspend: write the AUTO save STATE ("sus" slot) so the next launch resumes here. Kept
	 * separate from the manual "st0" slot so exiting never overwrites a manual Save State. */
	{
		unsigned char *st = (unsigned char *)SNES_STATE_BUF;
		unsigned ssz = c->state_size();
		if (ssz && ssz <= SNES_STATE_CAP && c->state_save(st, ssz) == 0)
			gba_sd_write_named(vol, "/states/snes", rom->name, "sus", st, ssz);
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
