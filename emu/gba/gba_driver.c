/*
 * LK-side driver for the gpSP GBA core (ARM dynarec). After the boot animation,
 * LK decompresses the ROM from boot_b into a DRAM arena and runs the emulator
 * forever instead of booting the kernel.
 *
 * Structure mirrors emu/gbc/gbc_driver.c and reuses the same harness:
 *   - display   : ayaneo_gbc_show_frame() (mt_disp_drv.c; 240x160 -> 5x)
 *   - audio     : ayaneo_gbc_audio_init/pause/shutdown + ayaneo_gba_audio_submit
 *   - input     : gpio-keys (active-low)
 *   - menu      : GammaOS Pico overlay (AYA button)
 *   - charging  : ayaneo_gbc_charging_screen()
 *   - settings/battery/LED/CPU: shared with the GBC path
 *
 * gpSP has no "run one frame" call: its CPU runs on its own thread and yields at
 * vblank via switch_to_main_thread(). We keep that model with two LK events and a
 * dedicated CPU thread (see gba_yield_to_main / the run loop).
 */
#include <debug.h>
#include <platform/ayaneo_volume_input.h>
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <kernel/event.h>
#include <part_interface.h>

#ifdef AYANEO_DEBUG_LOGGING
#define GBA_LOG(...)	dprintf(CRITICAL, __VA_ARGS__)
#else
#define GBA_LOG(...)	do {} while (0)
#endif
#if defined(AYANEO_AUDIO_TRACE) || defined(AYANEO_DEBUG_LOGGING)
extern int _dprintf(const char *fmt, ...);
#define GBA_ATRACE(...)	_dprintf(__VA_ARGS__)
#else
#define GBA_ATRACE(...)	do {} while (0)
#endif

/* ---- gpSP core bridge (loadable blob in boot_b, see gba_core_abi.h) ----
 * The core is no longer linked into lk_a; gba_core_load() pulls the blob from boot_b into
 * DRAM at boot and hands back this export table. The macros below keep every existing
 * call site unchanged by routing it through the table, so gba_core_state_save(x) etc.
 * become g_core->state_save(x), and the three shared flags become pointer derefs. */
#include "gba_core_abi.h"
#include "../ayaneo_rewind.h"

static const struct gba_core_exports *g_core;

#define gba_core_init(...)          g_core->core_init(__VA_ARGS__)
#define gba_core_start(...)         g_core->core_start(__VA_ARGS__)
#define gba_core_enter_bios(...)    g_core->enter_bios(__VA_ARGS__)
#define reset_gba(...)              g_core->reset(__VA_ARGS__)
#define gba_core_cpu_loop(...)      g_core->cpu_loop(__VA_ARGS__)
#define gba_core_pre_frame(...)     g_core->pre_frame(__VA_ARGS__)
#define gba_core_skip_render(v)     (*g_core->skip_next_frame = (unsigned int)(v))
#define gba_core_post_frame(...)    g_core->post_frame(__VA_ARGS__)
#define gba_frame_boundary_finish(...) g_core->frame_boundary_finish(__VA_ARGS__)
#define gba_core_cpu_ticks(...)     g_core->cpu_ticks(__VA_ARGS__)
#define gba_core_set_keys(...)      g_core->set_keys(__VA_ARGS__)
#define gba_core_screen(...)        g_core->screen(__VA_ARGS__)
#define gba_core_screen_fill(...)   g_core->screen_fill(__VA_ARGS__)
#define gba_core_state_size(...)    g_core->state_size(__VA_ARGS__)
#define gba_core_state_save(...)    g_core->state_save(__VA_ARGS__)
#define gba_core_state_load(...)    g_core->state_load(__VA_ARGS__)
#define gba_sound_ring_save(...)    g_core->sound_ring_save(__VA_ARGS__)
#define gba_sound_ring_load(...)    g_core->sound_ring_load(__VA_ARGS__)
#define gba_core_rom_ptr(...)       g_core->rom_ptr(__VA_ARGS__)
#define gba_core_rom_capacity(...)  g_core->rom_capacity(__VA_ARGS__)
#define gba_core_scratch_ptr(...)   g_core->scratch_ptr(__VA_ARGS__)
#define gba_core_scratch_size(...)  g_core->scratch_size(__VA_ARGS__)
#define gba_core_backup_ptr(...)    g_core->backup_ptr(__VA_ARGS__)
#define gba_core_backup_size(...)   g_core->backup_size(__VA_ARGS__)
#define dynarec_enable              (*g_core->dynarec_enable)
#define g_gba_audio_suppress        (*g_core->audio_suppress)
#define g_gba_load_light            (*g_core->load_light)

/* ---- LK primitives ---- */
extern void *memcpy(void *, const void *, unsigned int);
extern time_t current_time(void);
extern unsigned gpt4_get_current_tick(void);
extern void arch_clean_cache_range(unsigned long start, unsigned int len);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern int  pmic_detect_powerkey(void);
extern void mt_power_off(void);
extern void ayaneo_gbc_show_frame(const unsigned short *pix);	/* mt_disp_drv.c */
extern void ayaneo_hud_set(int mode, int speed_x10);		/* mt_disp_drv.c: FF/RW speed badge */
extern void ayaneo_gba_show_intro_frame(const unsigned short *pix);	/* 6x fill-height */
extern void ayaneo_gbc_blank(void);		/* clear both game fbs to black */
/* Punch-hole launch transition: composite the live game frame inside a growing
 * circle over the frozen menu snapshot the selector captured (gba_snes_menu.c). */
extern void ayaneo_gba_punch_frame(const unsigned short *pix, const unsigned int *snap,
				   int cx, int cy, int radius);
extern int  gba_punch_ready;			/* set by the SNES selector on launch */
extern int  ayaneo_punch_steps(void);		/* its frames (mt_disp_drv.c) */
#define GBA_PUNCH_SNAP_PA  0x54000000u		/* menu snapshot (matches gba_snes_menu.c) */
#define GBA_PUNCH_MAX_R    820			/* > 1280x960 half-diagonal (800) = full cover */
/* Time-paced, NOT frame-count paced: the compositing cost per frame is unknown on the
 * A55, so drive the radius by elapsed wall-clock (13 MHz gpt4) to guarantee a snappy,
 * fixed-duration reveal (fewer frames if slow, more if fast). 180 ms = clearly < 0.2s. */
#define GBA_PUNCH_MS       180
#define GBA_PUNCH_TICKS    (GBA_PUNCH_MS * 13000u)	/* 13 MHz ticks for GBA_PUNCH_MS */
extern void mtk_wdt_restart(void);
extern void mtk_wdt_disable(void);
extern int  ayaneo_boot_audio_active(void);
extern void ayaneo_gbc_audio_init(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_pause(int on);
extern void ayaneo_gbc_audio_shutdown(void);
extern void ayaneo_menu_audio_silence(void);	/* zero the shared AFE ring (stop looping) */
extern void ayaneo_gba_audio_pace(void);	/* audio-clock frame pacing */
extern int  ayaneo_present_skip_framedone;	/* mt_disp_drv.c: 1 = non-blocking present */
extern void ayaneo_settings_load(void);
extern void ayaneo_settings_save(void);
#ifdef AYANEO_GBA_SD
#endif
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_gbc_osd_show(int kind, int pct);
extern int  ayaneo_get_load_on_boot(void);
extern void ayaneo_set_load_on_boot(int v);
extern int  ayaneo_get_skip_boot(void);
extern void ayaneo_set_skip_boot(int v);
extern int  ayaneo_get_skip_gba_intro(void);
extern int  ayaneo_get_intro_black(void);
extern void ayaneo_set_intro_black(int v);
extern void ayaneo_set_skip_gba_intro(int v);
extern int  ayaneo_get_lcd_filter_core(int c);
extern void ayaneo_set_lcd_filter_core(int c, int v);
extern int  ayaneo_get_color_correct(void);
extern int  ayaneo_get_strobe(void);
extern const char *ayaneo_strobe_name(int b);
extern int  ayaneo_get_overdrive(void); extern void ayaneo_set_overdrive_level(int k);   /* software motion-trail overdrive (shared) */
extern void ayaneo_set_color_correct(int v);
extern int  ayaneo_get_preempt_frames(void);
extern void ayaneo_set_preempt_frames(int v);
extern int  ayaneo_get_mute_bios(void);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb, int alpha);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern int  get_bat_sense_volt(int times);
extern int  upmu_is_chr_det(void);
extern unsigned int ayaneo_get_cpu_mhz(void);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void ayaneo_charge_led(int r, int g, int b);
extern int  mt_set_gpio_mode(unsigned pin, unsigned mode);
extern int  mt_set_gpio_dir(unsigned pin, unsigned dir);
extern int  mt_set_gpio_pull_enable(unsigned pin, unsigned en);
extern int  mt_set_gpio_pull_select(unsigned pin, unsigned sel);
extern int  mt_set_gpio_smt(unsigned pin, unsigned enable);	/* Schmitt trigger (hysteresis) */
extern int  mt_get_gpio_smt(unsigned pin);
extern int  mt_get_gpio_in(unsigned pin);
extern int  mtk_detect_key(unsigned short hwkey);

/* The embedded 16KB GBA BIOS is only needed by the NON-SD baked-game path. In the SD
 * build the emulator only ever runs with s_sd_mode=1 (ayaneo_gba_sd_boot returns <0 ->
 * normal Android boot otherwise), so that path is dead and the SD BIOS (s_sd_bios,
 * loaded from the card) is used instead - dropping the 16KB keeps lk_a under its 2MB
 * partition. A non-SD build still links + uses the real embedded BIOS. */
#ifndef AYANEO_GBA_SD
#include "gba_bios_data.h"
#endif

/* ---- GBA P1 button bits (BUTTON_* in gpSP input.h) ---- */
#define GB_A	 0x001u
#define GB_B	 0x002u
#define GB_SEL	 0x004u
#define GB_START 0x008u
#define GB_RIGHT 0x010u
#define GB_LEFT	 0x020u
#define GB_UP	 0x040u
#define GB_DOWN	 0x080u
#define GB_R	 0x100u
#define GB_L	 0x200u

/* ---- config ---- */
/* How many BIOS frames to play as the boot-logo intro before the ROM-select menu
 * (~59.73 fps, so 270 frames ~= 4.5 s: the full Nintendo logo slide + chime).
 * The user can cut it short by holding B, or disable it via the skip-boot setting. */
#define GBA_SD_INTRO_FRAMES	270
#define GBA_ARENA_PA	0x50000000u
#define GBA_ARENA_SZ	(64u * 1024 * 1024)
#define GBA_DRV_RESERVE	(2u * 1024 * 1024)	/* state/sav scratch at the arena tail */
#define GBA_W		240
#define GBA_H		160

/* boot_b layout (32 MB): animation @0, audio chime @16 MB (both shared with the
 * GBC build), compressed ROM @17 MB, save state @28 MB, .sav @29 MB, settings
 * @30 MB. The ROM sits in the 11 MB gap between the chime and the save state. */
#define GBA_PART	"boot_b"
#define GBA_ROM_OFF	0x01100000u	/* 17 MB: [magic][u32 rawlen][u32 complen][deflate] */
#define GBA_ROM_MAGIC	0x52414247u	/* "GBAR" */
#define GBA_ROM_COMPMAX	(10u * 1024 * 1024)	/* fits the 17-28 MB gap */
#define GBA_ROM_RAWMAX	(32u * 1024 * 1024)

/* the GBA renders ~280896 CPU cycles/frame at 16777216 Hz = 59.7275 Hz */
#define GBA_FRAME_CYC	280896ull
#define GBA_CPU_HZ	16777216ull

static int s_ready;
static volatile int s_fast_forward;
static volatile int s_ff_level;			/* right-trigger fast-forward level 0..255 (0 = off) */
static unsigned int s_ff_t0;			/* tick the committed frame started (fast forward budget) */
static int s_ff_active;				/* this loop frame ran a fast forward batch */
volatile unsigned int g_dbg_ff_skip_us, g_dbg_ff_rend_us;   /* oem ff: the batch's frame cost estimates */
#define FF_MAX_MULT   10			/* full press = up to 10x (CPU-bound = fastest, audio kept) */

/* Rewind (left trigger): a snapshot (core state + 128 KB sound ring) is pushed for EVERY committed
 * emulated frame - the committed frame plus each fast-forward frame - so FF'd frames can be rewound
 * too (NOT the speculative run-ahead look-ahead frames, which get rewound anyway). Holding the left
 * trigger walks it backward at a smooth 1x..6x speed set by press depth via a fractional accumulator
 * (256ths). One snapshot = one emulated frame, so speed = SPD/256 real-time. The per-frame save is a
 * ~640 KB memcpy (~0.3 ms) - the same save run-ahead already does every frame - so it fits inside the
 * run-ahead-Max vsync margin. */
#define GBA_REWIND_MAX_SPD   1536		/* max rewind speed in 256ths (1536 = 6x); floor is 256 = 1x */
#define GBA_RW_SND_SZ        (128u * 1024u)	/* sound ring size (matches the run-ahead s_ahead_snd) */
static unsigned s_gba_rw_snd_off;		/* byte offset of the sound ring within a ring slot */
static unsigned s_gba_rw_payload;		/* ring slot payload = snd_off + GBA_RW_SND_SZ (0 = disabled) */
static volatile int s_close_req;	/* in-game menu "Close": save + back to the SNES selector */
static volatile int s_reset_req;	/* soft reset: restart the current game (menu Reset / hotkey) */
/* Per-frame emulation cost (run_one_frame wall time, us) averaged over 16 frames.
 * The emulator is single-core, so run_one_frame blocks the main thread while the
 * CPU thread emulates one frame - its wall time IS the emulation cost. Exposed via
 * oem diag `em=` to size the run-ahead/preemptive-frames headroom (each extra
 * predicted frame costs one more of these). */
volatile unsigned int g_dbg_emu_us;
volatile unsigned int g_dbg_snap_us;
volatile unsigned int g_dbg_snap_parts[3];   /* core state save, sound ring, delta encode + store (us) */
/* oem diag lp: the game loop split (us), average and worst over 64 loops:
 * [0] before the frame (input, rewind, clock), [1] the committed frame, [2] rewind snapshot,
 * [3] the rest up to the present (FF, volume, power, menu), [4] present work, [5] present wait */
volatile unsigned int g_dbg_lp_avg[6], g_dbg_lp_max[6];
static unsigned int s_lp_t[6], s_lp_acc[6], s_lp_mx[6], s_lp_n;
static void lp_mark(int i) { s_lp_t[i] = gpt4_get_current_tick(); }
static void lp_commit(void)
{
	extern volatile unsigned int g_dbg_pace_last_ticks;
	unsigned int d[6], i, pw = g_dbg_pace_last_ticks;
	d[0] = s_lp_t[1] - s_lp_t[0]; d[1] = s_lp_t[2] - s_lp_t[1]; d[2] = s_lp_t[3] - s_lp_t[2];
	d[3] = s_lp_t[4] - s_lp_t[3]; d[4] = s_lp_t[5] - s_lp_t[4];
	if (pw > d[4]) pw = d[4];
	d[4] -= pw; d[5] = pw;
	for (i = 0; i < 6; i++) { s_lp_acc[i] += d[i]; if (d[i] > s_lp_mx[i]) s_lp_mx[i] = d[i]; }
	if (++s_lp_n >= 64) {
		for (i = 0; i < 6; i++) { g_dbg_lp_avg[i] = s_lp_acc[i] / (64u * 13u); g_dbg_lp_max[i] = s_lp_mx[i] / 13u; s_lp_acc[i] = 0; s_lp_mx[i] = 0; }
		s_lp_n = 0;
	}
}	/* rewind snapshot of the committed frame (us), oem diag */
volatile unsigned int g_dbg_loop_us;	/* whole game loop iteration (us), oem diag loop: */
volatile unsigned int g_dbg_frame_ticks;	/* GBA cycles advanced by the committed frame */
volatile unsigned int g_dbg_asub_calls, g_dbg_asub_done, g_dbg_asub_frames;	/* audio submit counters */
volatile int g_dbg_eff_pf;			/* last effective run-ahead depth */
volatile int g_dbg_btn_smt = -1;	/* button Schmitt-trigger state after init (oem diag, expect 1) */
volatile int g_dbg_force_close;		/* fastboot `oem close`: trigger the close path for testing */
static int s_settings_dirty;		/* volume/brightness changed: persist is deferred (see poll_volume) */
static unsigned s_settings_tick;	/* 13 MHz tick of the last volume/brightness change */
static volatile int s_benchmark;
static volatile int s_fps;
static volatile int s_menu_open;
/* GBA core choice (armGBA-pico from the SD card, gpSP from boot_b) and the Core Settings page */
static int s_core_is_sd;              /* g_core is the SD card's core */
static int s_core_pref;               /* 0 = the card's core when it is there, 1 = gpSP */
static int s_sdcore_present = -1;     /* checked at the first GBA launch, once the card is mounted */
/* oem core:gpsp|armgba|auto - a test override of the core choice for the next GBA game, in RAM only
 * (the saved preference is untouched): 0 follows the setting, 1 armGBA, 2 gpSP (needs a BIOS) */
volatile int g_dbg_core_force;
static int s_menu_page;               /* 0 the Pico menu, 1 Core Settings */
static int s_core_sel;                /* the selected row on the Core Settings page */
static volatile int s_corecfg_dirty;
const char *g_gba_state_dir = "/states/gba";   /* gba_sd_save.c: savestates are per core */
static void core_opt_store(const char *key, int val);
static volatile unsigned s_keys;	/* GBA mask, refreshed once per frame */
/* Release-latch for the A/B that dismissed the Pico menu: the menu closes on the press edge
 * while the button is still held, so without this the held A/B bleeds into the game as an
 * unintended input. Set on close (menu_tick), cleared once the button is released. */
static volatile unsigned s_ab_latch;

int gbc_benchmark_on(void) { return s_benchmark; }
int gbc_get_fps(void) { return s_fps; }
int gbc_menu_is_open(void) { return s_menu_open; }

/* wall-clock for the GBA RTC (Pokemon Emerald etc.); base ~2024-08-01 UTC. */
long gba_host_time(void)
{
	return 1722470400L + (long)(current_time() / 1000);
}

/* ---- gpio-keys (active-low) ---- */
#define GP(n)	((n) | 0x80000000u)
#define PRESSED(g)	(mt_get_gpio_in(GP(g)) == 0)

#define GPIO_UP		89
#define GPIO_DOWN	79
#define GPIO_LEFT	78
#define GPIO_RIGHT	80
#if defined(KONKR_KPA)
/* Konkr: the device tree's labels match the shell (KEY_A 82, KEY_B 83, KEY_X 84, KEY_Y 85,
 * KEY_START 90, KEY_SELECT 91); the pairmini swaps below are wrong on this board. */
#define GPIO_A		82
#define GPIO_B		83
#define GPIO_START	90
#define GPIO_SELECT	91
#define GPIO_LB		92	/* L shoulder */
#define GPIO_RB		81	/* R shoulder */
#define GPIO_X		85	/* autofire B: the Konkr X cap (the GBA's autofire pair was the one reading swapped) */
#define GPIO_Y		84	/* autofire A */
#else
#define GPIO_A		83	/* swapped vs DTS to match the physical layout */
#define GPIO_B		82
#define GPIO_START	91	/* swapped: Start<->Select */
#define GPIO_SELECT	90
#define GPIO_LB		92	/* GBA L shoulder */
#define GPIO_RB		81	/* GBA R shoulder */
#define GPIO_X		84	/* autofire B */
#define GPIO_Y		85	/* autofire A */
#endif
#define GPIO_R2		57	/* key_rc / second-stage right trigger = fast-forward */
#define GPIO_AYA	86	/* menu */
#define AUTOFIRE_HZ	12

static const struct { unsigned gpio; unsigned mask; } s_btn[] = {
	{ GPIO_UP, GB_UP }, { GPIO_DOWN, GB_DOWN }, { GPIO_LEFT, GB_LEFT }, { GPIO_RIGHT, GB_RIGHT },
	{ GPIO_A, GB_A },   { GPIO_B, GB_B },
	{ GPIO_START, GB_START }, { GPIO_SELECT, GB_SEL },
	{ GPIO_LB, GB_L },  { GPIO_RB, GB_R },
};

/* Configure a button pin as a glitch-hardened active-low input. Beyond the pull-up, this
 * enables the pin's hardware SCHMITT TRIGGER (input hysteresis): it widens the gap between
 * the high->low and low->high logic thresholds, so transient noise that only nudges the
 * line partway (coupling from the MIPI display, the audio amp, the DC-DC converters, or
 * the CPU switching harder under run-ahead) no longer registers as a phantom press. It is
 * a hardware deglitch with ZERO added latency, unlike a multi-frame software debounce. All
 * the button pins (57, 78-92) support SMT. */
static void gpio_in_pullup(unsigned gpio)
{
	mt_set_gpio_mode(GP(gpio), 0);
	mt_set_gpio_dir(GP(gpio), 0);
	mt_set_gpio_smt(GP(gpio), 1);		/* hardware hysteresis / deglitch */
	mt_set_gpio_pull_enable(GP(gpio), 1);
	mt_set_gpio_pull_select(GP(gpio), 1);	/* pull-up */
}

/* Map the physical pad to gambatte's button mask (inputgetter.h:
 * A=0x01 B=0x02 SELECT=0x04 START=0x08 RIGHT=0x10 LEFT=0x20 UP=0x40 DOWN=0x80).
 * Imported by the GB/GBC core blob via the imports table. Active-low GPIOs; the
 * pins are already configured (input_init) by the time a game runs. */
unsigned ayaneo_gbc_pad_mask(void)
{
	extern volatile int g_gbc_menu_open;   /* gbc_sd_run.c: gate game input while the menu is open */
	extern volatile int g_gbc_ab_latch;    /* held-over A/B from dismissing the menu */
	unsigned m = 0;
	if (g_gbc_menu_open) return 0;
	/* Release-latch: clear each latched bit once its button is up; gate held-over A/B meanwhile
	 * so dismissing the menu with A/B does not bleed a press into the game. */
	if (!PRESSED(GPIO_A)) g_gbc_ab_latch &= ~0x01;
	if (!PRESSED(GPIO_B)) g_gbc_ab_latch &= ~0x02;
	if (PRESSED(GPIO_A) && !(g_gbc_ab_latch & 0x01)) m |= 0x01u;
	if (PRESSED(GPIO_B) && !(g_gbc_ab_latch & 0x02)) m |= 0x02u;
	if (PRESSED(GPIO_SELECT)) m |= 0x04u;
	if (PRESSED(GPIO_START))  m |= 0x08u;
	if (PRESSED(GPIO_RIGHT))  m |= 0x10u;
	if (PRESSED(GPIO_LEFT))   m |= 0x20u;
	if (PRESSED(GPIO_UP))     m |= 0x40u;
	if (PRESSED(GPIO_DOWN))   m |= 0x80u;
	{	/* left analog stick -> D-pad (JOY_UP=1 DOWN=2 LEFT=4 RIGHT=8) */
		extern unsigned int ayaneo_joypad_dpad(void);
		unsigned d = ayaneo_joypad_dpad();
		if (d & 0x08u) m |= 0x10u;   /* RIGHT */
		if (d & 0x04u) m |= 0x20u;   /* LEFT */
		if (d & 0x01u) m |= 0x40u;   /* UP */
		if (d & 0x02u) m |= 0x80u;   /* DOWN */
	}
	return m;
}

static void input_init(void)
{
	unsigned i;
	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		gpio_in_pullup(s_btn[i].gpio);
	gpio_in_pullup(GPIO_X);
	gpio_in_pullup(GPIO_Y);
	gpio_in_pullup(GPIO_R2);
	gpio_in_pullup(GPIO_AYA);
	g_dbg_btn_smt = mt_get_gpio_smt(GP(GPIO_A));	/* verify SMT took (expect 1) */
}

int ayaneo_gbc_select_held(void)
{
	gpio_in_pullup(GPIO_SELECT);
	return PRESSED(GPIO_SELECT);
}

static volatile int s_sel_modifier;

#define RB_X	0x1000u
#define RB_Y	0x2000u
#define RB_FF	0x4000u

/* refresh the button state once per frame. GAMEPLAY_DEBOUNCE = N-read agreement: a bit
 * is accepted only after it read pressed on N CONSECUTIVE frames, adding N-1 frames of
 * press latency (release is immediate). N=1 is RAW (zero debounce, lowest latency); N=2
 * rejects single-frame glitches (+1 frame); N=3 rejects single- AND two-frame glitches
 * (+2 frames). Now that the button pins run with a hardware Schmitt trigger (gpio_in_pullup)
 * deglitching them at the pin, the software filter can be dialed down - testing N=1 (raw). */
#define GAMEPLAY_DEBOUNCE 1
static void update_buttons(void)
{
#if GAMEPLAY_DEBOUNCE > 1
	static unsigned hist[GAMEPLAY_DEBOUNCE - 1];	/* the last N-1 raw reads */
#endif
	unsigned raw = 0, deb, m, i;
	int af;

	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		if (PRESSED(s_btn[i].gpio))
			raw |= s_btn[i].mask;
	if (PRESSED(GPIO_X))  raw |= RB_X;
	if (PRESSED(GPIO_Y))  raw |= RB_Y;
#if defined(KONKR_KPA)
	{ extern int ayaneo_joypad_ff_level(void); if (ayaneo_joypad_ff_level() > 0) raw |= RB_FF; }   /* R2 toggle */
#else
	if (PRESSED(GPIO_R2)) raw |= RB_FF;
#endif
	{	/* left analog stick -> D-pad (JOY_UP=1 DOWN=2 LEFT=4 RIGHT=8) */
		extern unsigned int ayaneo_joypad_dpad(void);
		unsigned d = ayaneo_joypad_dpad();
		if (d & 0x01u) raw |= GB_UP;
		if (d & 0x02u) raw |= GB_DOWN;
		if (d & 0x04u) raw |= GB_LEFT;
		if (d & 0x08u) raw |= GB_RIGHT;
	}

	/* N-frame agreement: accept a bit only when this read AND the previous N-1 reads
	 * all had it pressed. At N=1 this is a pure raw pass (no history). */
	deb = raw;
#if GAMEPLAY_DEBOUNCE > 1
	for (i = 0; i < GAMEPLAY_DEBOUNCE - 1; i++) deb &= hist[i];
	for (i = GAMEPLAY_DEBOUNCE - 2; i > 0; i--) hist[i] = hist[i - 1];
	hist[0] = raw;
#endif

	m = deb & 0x3ffu;			/* the 10 GBA buttons */

	if (s_sel_modifier)
		m &= ~GB_SEL;			/* Select drives brightness, not the game */

	af = ((unsigned)current_time() * AUTOFIRE_HZ / 500u) & 1;
	if (af) {
		if (deb & RB_X) m |= GB_B;	/* X = autofire B */
		if (deb & RB_Y) m |= GB_A;	/* Y = autofire A */
	}

	/* soft-reset hotkey: Select+Start+L+R held ~0.5s restarts the current game.
	 * Suppress those four from the game while the combo is engaged (so it does not
	 * also act on them) and latch so it fires exactly once per hold. */
	{
		static int rc_hold, rc_fired;
		unsigned combo = GB_SEL | GB_START | GB_L | GB_R;
		if ((deb & combo) == combo) {
			m &= ~combo;
			if (++rc_hold >= 30 && !rc_fired) { s_reset_req = 1; rc_fired = 1; }
		} else {
			rc_hold = 0; rc_fired = 0;
		}
	}

	s_fast_forward = (deb & RB_FF) ? 1 : 0;	/* R2 held = fast-forward */
	if (s_menu_open)
		m = 0;
	/* Release-latch: gate a held A/B that just dismissed the menu out of the game until it is
	 * physically released, so closing the menu never registers as an in-game button press. */
	if (!PRESSED(GPIO_A)) s_ab_latch &= ~GB_A;
	if (!PRESSED(GPIO_B)) s_ab_latch &= ~GB_B;
	m &= ~s_ab_latch;
	s_keys = m;
	gba_core_set_keys(m);
}

/* Volume keys (MTK keypad); Select + Volume = brightness. Persisted to boot_b. */
static void poll_volume(void)
{
	extern volatile int g_menu_live;
	int sel = PRESSED(GPIO_SELECT);
	g_menu_live = 0;   /* a game is running (oem mode) */
	int dir = ayaneo_volume_dir();

	if (dir) {
		if (sel) {
			int pct = ayaneo_brightness_step(dir);
			ayaneo_gbc_osd_show(2, pct);
			s_sel_modifier = 1;
		} else {
			int v = ayaneo_gbc_audio_get_volume() + dir * 5;
			ayaneo_gbc_audio_set_volume(v);
			ayaneo_gbc_osd_show(1, ayaneo_gbc_audio_get_volume());
		}
		/* Defer the persist. ayaneo_settings_save() does a card (FAT) write
		 * that stalls this loop for many ms; done inline on
		 * every key repeat, the audio ring is starved between steps and the AFE
		 * DMA loops the last frames (the "audio looping between volume steps").
		 * Mark dirty + timestamp instead; the game loop flushes ONCE, ~0.7s after
		 * the last change, with the ring silenced across the write. */
		s_settings_dirty = 1;
		s_settings_tick = gpt4_get_current_tick();
	}
	if (!sel)
		s_sel_modifier = 0;
}

/* ---- little-endian helpers ---- */
static unsigned rd32le(const unsigned char *p)
{ return (unsigned)p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static void wr32le(unsigned char *p, unsigned v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* ---- save state (boot_b) ---- */
#define GBA_STATE_OFF	0x01C00000u	/* 28 MB */
volatile unsigned int g_dbg_legacy_refused;   /* oem mode: partition state / save writes refused in the SD build */
#define GBA_STATE_MAGIC	0x53414247u	/* "GBAS" */
#define GBA_STATE_MAX	(1u * 1024 * 1024)	/* 512 KB gpSP state + hdr, block-aligned */

#ifdef AYANEO_GBA_SD
static int  sd_mode_on(void);            /* defined after the SD globals below */
static int  sd_state_write(unsigned char *scratch);
static int  sd_state_read(unsigned char *scratch);
static int  sd_manual_write(unsigned char *scratch);
static int  sd_manual_read(unsigned char *scratch);
static void sd_sav_write(void);
#endif
static int  manual_state_write(unsigned char *scratch);  /* menu Save State -> selected slot */
static int  manual_state_read(unsigned char *scratch);   /* menu Load State -> selected slot */

static int state_write(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	unsigned total;

#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) return sd_state_write(scratch);
	/* The partition state slot is boot_b+28 MB, where the SD build keeps the gpSP core blob:
	 * a save with no game selected (the bare BIOS) overwrote it and the next boot could not
	 * load gpSP (2026-11-03). The SD build keeps states on the card only. */
	g_dbg_legacy_refused++;
	(void)sz; (void)total;
	return 0;
#else
	if (!sz || (unsigned long long)sz + 8 > GBA_STATE_MAX)
		return 0;
	total = 8 + sz;
	gba_core_state_save(scratch + 8);
	wr32le(scratch + 0, GBA_STATE_MAGIC);
	wr32le(scratch + 4, sz);
	if (total & 511u) {
		unsigned padded = (total + 511u) & ~511u;
		if (padded <= GBA_STATE_MAX) {
			unsigned k;
			for (k = total; k < padded; k++) scratch[k] = 0;
			total = padded;
		}
	}
	arch_clean_cache_range((unsigned long)scratch, total);
	partition_write(GBA_PART, GBA_STATE_OFF, scratch, total);
	return 1;
#endif
}

static int state_read(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned magic, sz;

#ifdef AYANEO_GBA_SD
	/* states live on the card only: the SD build reads and writes nothing on the eMMC */
	(void)hdr; (void)magic; (void)sz;
	return sd_mode_on() ? sd_state_read(scratch) : 0;
#else
	if (partition_read(GBA_PART, GBA_STATE_OFF, hdr, 8) != 8)
		return 0;
	magic = rd32le(hdr);
	sz = rd32le(hdr + 4);
	if (magic != GBA_STATE_MAGIC || sz == 0 || sz > GBA_STATE_MAX - 8 ||
	    sz != gba_core_state_size())
		return 0;
	if (partition_read(GBA_PART, GBA_STATE_OFF + 8, scratch, sz) != (ssize_t)sz)
		return 0;
	gba_core_state_load(scratch);
	return 1;
#endif
}

/* Manual Save/Load wrappers (menu): SD uses the selected slot st0..2; the legacy partition path
 * has a single fixed state region (no slots), so it falls back to the suspend state_write/read. */
static int manual_state_write(unsigned char *scratch)
{
#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) return sd_manual_write(scratch);
#endif
	return state_write(scratch);
}
static int manual_state_read(unsigned char *scratch)
{
#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) return sd_manual_read(scratch);
#endif
	return state_read(scratch);
}

/* ---- cartridge battery save (.sav = gpSP gamepak_backup) ---- */
#define GBA_SAV_OFF	0x01D00000u	/* 29 MB */
#define GBA_SAV_MAGIC	0x56415347u	/* "GSAV" */
#define GBA_SAV_MAX	(256u * 1024u)	/* 128 KB backup + hdr, block-aligned */

static void sav_save(unsigned char *scratch)
{
#ifdef AYANEO_GBA_SD
	/* the SD build keeps battery saves on the card only */
	(void)scratch;
	if (sd_mode_on()) sd_sav_write();
	else g_dbg_legacy_refused++;
#else
	unsigned sz = gba_core_backup_size();		/* fixed 128 KB */
	unsigned total = 8 + sz;

	if (!sz || total > GBA_SAV_MAX)
		return;
	wr32le(scratch + 0, GBA_SAV_MAGIC);
	wr32le(scratch + 4, sz);
	memcpy(scratch + 8, gba_core_backup_ptr(), sz);
	if (total & 511u) {
		unsigned padded = (total + 511u) & ~511u;
		if (padded <= GBA_SAV_MAX) {
			unsigned k;
			for (k = total; k < padded; k++) scratch[k] = 0;
			total = padded;
		}
	}
	arch_clean_cache_range((unsigned long)scratch, total);
	partition_write(GBA_PART, GBA_SAV_OFF, scratch, total);
#endif
}

static void sav_load(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned sz, live = gba_core_backup_size();

#ifdef AYANEO_GBA_SD
	(void)hdr; (void)sz; (void)live; (void)scratch;   /* battery saves live on the card (gba_sd_load_sav) */
#else
	if (partition_read(GBA_PART, GBA_SAV_OFF, hdr, 8) != 8)
		return;
	if (rd32le(hdr) != GBA_SAV_MAGIC)
		return;
	sz = rd32le(hdr + 4);
	if (!sz || sz != live || 8u + sz > GBA_SAV_MAX)
		return;
	if (partition_read(GBA_PART, GBA_SAV_OFF + 8, scratch, sz) != (ssize_t)sz)
		return;
	memcpy(gba_core_backup_ptr(), scratch, sz);
#endif
}

static void try_load_state(unsigned char *scratch)
{
	if (PRESSED(GPIO_START)) {
		GBA_ATRACE("GBA: Start held - skipping save state\n");
		return;
	}
	if (!ayaneo_get_load_on_boot()) {
		GBA_ATRACE("GBA: load-on-boot disabled - fresh start\n");
		return;
	}
	if (state_read(scratch))
		GBA_ATRACE("GBA: resumed save state\n");
}

#ifdef AYANEO_GBA_SD
static const char *gba_sd_current_name(void);   /* the ROM running from the card, or NULL */
#endif
/* The suspend save of the power key, timed per step (oem suspendtest: [0] state, [1] battery save,
 * [2] settings, ms; [4] state write result; [3] is left 0) */
volatile unsigned int g_dbg_suspend_ms[5];
volatile int g_dbg_suspend_test;
static int s_game_live;   /* a game (not the bare BIOS) runs: only then is there anything to save */
static void save_for_suspend(unsigned char *scratch)
{
	unsigned t0 = gpt4_get_current_tick(), t1, t2, t3;
	if (s_game_live) {
		g_dbg_suspend_ms[4] = (unsigned)state_write(scratch);
		t1 = gpt4_get_current_tick();
		sav_save(scratch);
		t2 = gpt4_get_current_tick();
#ifdef AYANEO_GBA_SD
		{ extern void ayaneo_resume_mark(const char *name); const char *nm = gba_sd_current_name(); if (nm) ayaneo_resume_mark(nm); }
#endif
	} else {
		/* the bare BIOS (no game on the card, or the list not read): saving here would write
		 * the BIOS's state and an empty battery save over the selected game's files */
		extern void ayaneo_resume_clear(void);
		ayaneo_resume_clear();
		g_dbg_suspend_ms[4] = 2u;
		t1 = t2 = gpt4_get_current_tick();
	}
	ayaneo_settings_save();   /* to the card in the SD build */
	t3 = gpt4_get_current_tick();
	g_dbg_suspend_ms[0] = (t1 - t0) / 13000u;
	g_dbg_suspend_ms[1] = (t2 - t1) / 13000u;
	g_dbg_suspend_ms[2] = (t3 - t2) / 13000u;
	g_dbg_suspend_ms[3] = 0;
}

static void save_and_poweroff(unsigned char *scratch)
{
	/* Mute the game audio FIRST so the ~1s card save does not replay the AFE ring
	 * (the loop the user heard on power-off). pause(1) both wipes the ring and stops
	 * the submit path refeeding it; the permanent mute is fine here - we are shutting
	 * down anyway (audio_shutdown follows). */
	ayaneo_gbc_audio_pause(1);
	save_for_suspend(scratch);
	ayaneo_gbc_audio_shutdown();
	ayaneo_leds_off_for_poweroff();
	mt_power_off();
}

static void check_power(unsigned char *scratch)
{
	extern int ayaneo_power_hold(int menu_open, int use_overlay);
	if (g_dbg_suspend_test) {   /* oem suspendtest: the power key's save without the power off */
		ayaneo_gbc_audio_pause(1);
		save_for_suspend(scratch);
		ayaneo_gbc_audio_pause(0);
		g_dbg_suspend_test = 0;
	}
	if (ayaneo_power_hold(s_menu_open, 1))
		save_and_poweroff(scratch);		/* no return */
}

/* ---- decompress the ROM from boot_b into the core's ROM buffer ---- */
static unsigned load_rom(void)
{
	unsigned char hdr[12];
	unsigned magic, rawlen, complen;
	unsigned char *rom = gba_core_rom_ptr();
	unsigned char *comp;
	unsigned long zlen;

#ifdef AYANEO_GBA_SD
	(void)hdr; (void)magic; (void)rawlen; (void)complen; (void)rom; (void)comp; (void)zlen;
	return 0;   /* games come from the card: no ROM on the eMMC */
#else
	if (partition_read(GBA_PART, GBA_ROM_OFF, hdr, 12) != 12)
		return 0;
	magic   = rd32le(hdr);
	rawlen  = rd32le(hdr + 4);
	complen = rd32le(hdr + 8);
	if (magic != GBA_ROM_MAGIC || rawlen == 0 || rawlen > gba_core_rom_capacity() ||
	    rawlen > GBA_ROM_RAWMAX || complen == 0 || complen > GBA_ROM_COMPMAX)
		return 0;

	/* Stage the compressed stream in the arena scratch ABOVE the ROM buffer, so
	 * inflate writes the full ROM into rom[0..rawlen] without ever overrunning the
	 * still-compressed input - supports ROMs up to the full 32 MB GBA maximum. */
	comp = gba_core_scratch_ptr();
	if (!comp || complen > gba_core_scratch_size())
		return 0;
	if (partition_read(GBA_PART, GBA_ROM_OFF + 12, comp, complen) != (ssize_t)complen)
		return 0;

	zlen = complen;
	if (zunzip(comp, &zlen, rom, (int)rawlen, 0) != 0)
		return 0;
	return rawlen;
#endif
}

/* ===================== CPU/frontend thread hand-off ===================== */
static event_t ev_cpu, ev_main;
static thread_t *s_cpu_thread;

/* Clean restart of the CPU thread's execute_arm after a mid-run core reset (the
 * BIOS-intro -> selected-game transition). We cannot resume execute_arm in place
 * after gba_core_start()/reset_gba() flushes the dynarec (it would run stale
 * translated code -> data abort in translate_block_arm). Instead, when a restart
 * is requested, the CPU thread longjmps back to the top of gba_core_cpu_loop so
 * execute_arm re-enters cleanly - exactly like the very first start. */
static void *s_cpu_jb[8];
static volatile int s_cpu_restart_req;
static volatile int s_cpu_clean_boundary;	/* run-ahead re-entry: finish the vblank tail first */

static void gba_dbg(const char *msg);	/* on-screen status (defined below) */

/* LK boots with SCTLR.A=1 (strict alignment faults, set in arch/arm/crt0.S).
 * gpSP - like on Linux (A=0) - does unaligned host word/halfword loads for the
 * GBA's unaligned-read semantics, which then trap. Clear SCTLR.A so the CPU
 * services those accesses in hardware. Must run on whichever core executes the
 * dynarec/helpers (LK is single-core here, but we clear it on both emulator
 * threads to be safe). PL1/SVC, so the CP15 write is permitted. */
static void gba_disable_align_faults(void)
{
	unsigned long v;
	__asm__ __volatile__("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
	v &= ~(1UL << 1);
	__asm__ __volatile__("mcr p15, 0, %0, c1, c0, 0" :: "r"(v) : "memory");
	__asm__ __volatile__("isb" ::: "memory");
}

/* called by the core (on the CPU thread) at each vblank */
void gba_yield_to_main(void)
{
	static int first = 1;
	if (first) { first = 0; gba_dbg("GBA 6b: dynarec yielded 1st frame"); }
	event_signal(&ev_main, false);
	event_wait(&ev_cpu);
	if (s_cpu_restart_req) {		/* a core reset happened while we were parked */
		s_cpu_restart_req = 0;
		if (s_cpu_clean_boundary) {	/* run-ahead rewind: run the skipped vblank tail so */
			s_cpu_clean_boundary = 0;	/* the re-entry starts a FULL frame, not a ~960cyc stub */
			gba_frame_boundary_finish();
		}
		__builtin_longjmp(s_cpu_jb, 1);	/* re-enter gba_core_cpu_loop from the top */
	}
}

static int cpu_thread_fn(void *arg)
{
	(void)arg;
	event_wait(&ev_cpu);		/* wait for the first frontend kick (once) */
	__builtin_setjmp(s_cpu_jb);	/* restart point for the intro -> game reset */
	gba_disable_align_faults();	/* (re)disable align faults on this core */
	gba_dbg("GBA 6a: cpu thread running core");
	gba_core_cpu_loop();		/* runs forever, yields via gba_yield_to_main */
	return 0;
}

/* Speed telemetry (oem diag): emulated frames per second from run_one_frame and the wall time
 * one frame takes inside the core, to tell "the core yields less than a frame" from "the loop
 * presents too slowly". */
volatile unsigned int g_dbg_emu_fps;        /* committed frames in the last second */
volatile unsigned int g_dbg_emu_total;      /* committed frames since boot (monotonic, oem diag speed check) */
volatile unsigned int g_dbg_bt_intro_end, g_dbg_bt_intro_frames;   /* BIOS intro end (ms since boot) + frames run */
volatile unsigned int g_dbg_emu_frame_us;   /* last run_one_frame duration (us) */

/* run exactly one frame: kick the CPU thread and block until it yields back */
static void run_one_frame(void)
{
	static unsigned int win0, cnt;
	unsigned int t0 = gpt4_get_current_tick(), t1;
	gba_core_pre_frame();		/* latch input */
	event_signal(&ev_cpu, false);
	event_wait(&ev_main);
	gba_core_post_frame();		/* drain audio */
	t1 = gpt4_get_current_tick();
	g_dbg_emu_frame_us = (t1 - t0) / 13u;
	g_dbg_emu_total++;
	cnt++;
	if (!win0) win0 = t1;
	if (t1 - win0 >= 13000000u) { g_dbg_emu_fps = cnt; cnt = 0; win0 = t1; }
}

/* (Re)arm the rewind ring for the current game: size a slot to hold the core state (aligned) plus
 * the 128 KB sound ring, then clear the history. Called at game start and at every timeline break
 * (soft reset, game switch, menu Load State). state_size is fixed for the gpSP build, so this just
 * recomputes cheaply and clears. Payload 0 disables rewind (state too large for a slot). */
static void gba_rewind_arm(void)
{
	unsigned ss = gba_core_state_size();
	if (!ss || ss > (1024u * 1024u)) { s_gba_rw_payload = 0; ayaneo_rewind_reset(0); return; }   /* gpSP 512 KiB, armGBA ~770 KiB */
	s_gba_rw_snd_off = (ss + 7u) & ~7u;
	/* gpSP keeps its sound ring outside the state; the SD card's core carries its audio in the
	 * state and its ring save/load do nothing, so the slot is the state alone (the 128 KB tail was
	 * scanned by the delta encoder every frame for nothing) */
	s_gba_rw_payload = s_gba_rw_snd_off + (s_core_is_sd ? 0u : GBA_RW_SND_SZ);
	ayaneo_rewind_reset(s_gba_rw_payload);
}

/* Push one rewind snapshot (core state + sound ring) of the CURRENT committed state. Called for
 * every committed emulated frame (the committed frame + each fast-forward frame); the caller gates
 * on the menu. No-op if rewind is unavailable this session. */
static void gba_rewind_snap(void)
{
	void *p;
	if (!s_gba_rw_payload || !ayaneo_rewind_ready()) return;
	p = ayaneo_rewind_capture_begin();
	if (p) {
		unsigned t0 = gpt4_get_current_tick(), t1, t2;
		gba_core_state_save(p);
		t1 = gpt4_get_current_tick();
		gba_sound_ring_save((unsigned char *)p + s_gba_rw_snd_off);
		t2 = gpt4_get_current_tick();
		ayaneo_rewind_capture_commit(s_gba_rw_payload);
		g_dbg_snap_parts[0] = (t1 - t0) / 13u;
		g_dbg_snap_parts[1] = (t2 - t1) / 13u;
		g_dbg_snap_parts[2] = (gpt4_get_current_tick() - t2) / 13u;
	}
}

/* ===================== run-ahead ("Preemptive Frames") =======================
 * "Preemptive Frames" (Pico menu, ayaneo_get_preempt_frames 0..3) hides the
 * game's internal 1-3 frame input lag. The committed real frame ran above (audio
 * ON, advancing state exactly 1/display frame). Here we snapshot it, run pf extra
 * frames forward with the SAME held input and audio MUTED, present that look-ahead
 * frame, then rewind to the committed snapshot so the committed timeline (and its
 * audio) stays pristine and continuous - no per-edge trajectory jump. This keeps
 * frame pacing steady (constant cost every frame) and audio click-free.
 *
 * gba_core_state_load flushes the dynarec, so the parked CPU thread cannot resume
 * a stale translated block - the NEXT committed frame re-enters via longjmp
 * (s_cpu_restart_req). We set s_cpu_clean_boundary so that re-entry first runs the
 * skipped vblank tail (gba_frame_boundary_finish), starting a FULL frame instead
 * of a degenerate ~960-cycle stub - so the committed timeline advances exactly one
 * frame per display, at true 1x speed (no priming frame, no 0.34% skew).
 *
 * ADAPTIVE depth: run-ahead costs (pf+1) emulations + a 512 KB save/load per
 * display frame. em (per-frame emulation us) is scene-dependent (~2.3 ms light,
 * ~5 ms heavy). In a heavy scene the full configured depth would overrun the
 * ~16.6 ms vsync budget and the game would drop into slow motion. So cap the
 * effective depth to what fits (budget ~13 ms emulation, ~3 ms save/load): heavy
 * scenes gracefully fall to pf=1 or 0, light scenes get the full setting. The
 * latency benefit matters least exactly when the scene is heavy. */
/* Closed-loop run-ahead depth. Run-ahead's whole per-frame cost (committed cold
 * frame + pf warm ahead frames + 512 KB save/load) must fit inside one vsync
 * (~16.6 ms) minus the present/overhead, or the loop overruns, drops below 60 fps
 * and STARVES the audio ring (the loop masters audio) = silence/slow motion. em
 * is scene-dependent (~2.3 ms light, ~5 ms+ heavy) and also depends on the CPU
 * clock, so a fixed estimate is fragile. Instead measure the ACTUAL total
 * emulation time each frame and step the cap +-1 with hysteresis so it always
 * settles at the deepest depth that fits. */
static int s_pf_cap = 3;				/* closed-loop depth ceiling */
static unsigned int s_committed_us;			/* committed frame emu us (this frame) */
static volatile unsigned int g_dbg_preempt_emu_us;	/* ahead+save+load us (excl present) */

/* Run-ahead determinism self-test (device-blind validation for the rewind path).
 * Set by `oem selftest[:N]` to N (>=1 = the number of committed frames to compare;
 * the emu loop runs it once at a frame boundary and reports the result here).
 * pass = -1 not-run / 0 FAIL / 1 PASS. rref/rtest are the compared state hashes
 * (must be equal for PASS). See gba_run_ahead_selftest below. */
volatile int g_dbg_selftest_req;
volatile int g_dbg_selftest_pass = -1;
volatile unsigned int g_dbg_selftest_rref, g_dbg_selftest_rtest;
/* per-component match flags after the last run: bit0 state, bit1 sound ring, bit2
 * screen (1 = REF==TEST for that component) - pinpoints WHERE a FAIL diverged. */
volatile int g_dbg_selftest_cmp;

/* Whether the committed frame is drawn under run ahead. With run ahead the picture shown is the
 * last look-ahead frame's, so not drawing the committed frame takes its drawing off the path from
 * the input read to the hand over. But the committed timeline then never draws, so armGBA's cache
 * of unchanged lines (the lines it skips stay dirty) is never cleaned there, and the look-ahead
 * frame redraws every line instead of only those that changed. A scrolling game gains (Sonic
 * Advance, run ahead 2: 6.72 -> 6.27 ms from the input read to the hand over), a still screen can
 * lose. So both ways are timed on the running game: the chosen one is kept for CSKIP_HOLD frames,
 * then the other is tried (CSKIP_SETTLE frames to settle, CSKIP_TIMED timed) and taken when it is
 * faster. The time is the committed frame plus the look-ahead frames up to the hand over. */
#define CSKIP_HOLD   1800u
#define CSKIP_SETTLE 8u
#define CSKIP_TIMED  64u
static int s_cskip = 1;			/* the chosen way: 1 = the committed frame is not drawn */
static unsigned int s_cskip_trial;	/* frames into a trial of the other way, 0 = none */
static unsigned int s_cskip_held;	/* frames of the chosen way since the last trial */
static unsigned int s_cskip_acc[2], s_cskip_n[2];
static int s_cskip_frame;		/* this display runs ahead and timed the committed frame */
static int s_ahead_shown;		/* preempt_present showed a look-ahead frame */
static unsigned int s_ahead_us;		/* its look-ahead frames, us, up to the hand over */
volatile unsigned int g_dbg_cskip_us[2];	/* the last timed averages: [0] drawn, [1] not drawn */
volatile int g_dbg_cskip = 1;		/* the chosen way */

static int cskip_now(void)
{
	return s_cskip_trial ? !s_cskip : s_cskip;
}

/* one display done the way cskip_now() gave, us from the committed frame to the hand over */
static void cskip_account(unsigned int us)
{
	int way = cskip_now();
	if (s_cskip_trial) {
		if (++s_cskip_trial > CSKIP_SETTLE) { s_cskip_acc[way] += us; s_cskip_n[way]++; }
		if (s_cskip_trial >= CSKIP_SETTLE + CSKIP_TIMED) {
			unsigned int tried = s_cskip_acc[way] / s_cskip_n[way];
			unsigned int kept = s_cskip_n[!way] ? s_cskip_acc[!way] / s_cskip_n[!way] : ~0u;
			g_dbg_cskip_us[way] = tried;
			if (s_cskip_n[!way]) g_dbg_cskip_us[!way] = kept;
			if (tried + 50u < kept) s_cskip = way;   /* a margin, so noise does not flip it */
			g_dbg_cskip = s_cskip;
			s_cskip_trial = 0; s_cskip_held = 0;
			s_cskip_acc[0] = s_cskip_acc[1] = 0; s_cskip_n[0] = s_cskip_n[1] = 0;
		}
		return;
	}
	/* the chosen way's last CSKIP_TIMED frames before a trial are its time */
	if (++s_cskip_held > CSKIP_HOLD - CSKIP_TIMED) { s_cskip_acc[way] += us; s_cskip_n[way]++; }
	if (s_cskip_held >= CSKIP_HOLD) s_cskip_trial = 1;
}

static int preempt_effective_depth(void)
{
	int cfg = ayaneo_get_preempt_frames();
	if (cfg <= 0)
		return 0;
	return s_pf_cap < cfg ? s_pf_cap : cfg;
}

/* PREDICTIVE closed loop on the measured per-frame BUSY time (committed frame +
 * the ahead frames/save/load + the present blit) - everything except the vsync
 * idle. Because it predicts the cost of one more level (busy + one committed-frame
 * worth) it never has to "probe" a too-deep setting and eat an overrun frame, so
 * there is no periodic audio hiccup. Paused while the Pico menu is open: the menu
 * overlay inflates the blit, which would otherwise crash the depth to 0 (what the
 * player was seeing as "(0)") - the depth should reflect gameplay, not the menu. */
static void preempt_adapt(int menu_open)
{
	extern volatile unsigned int g_dbg_blit_us;
	unsigned busy, step;
	if (menu_open)
		return;
	busy = s_committed_us + g_dbg_preempt_emu_us + g_dbg_blit_us;
	step = s_committed_us ? s_committed_us : 2500u;	/* cost of one more ahead frame (cold est) */
	if (busy > 15500u) {			/* over the ~16.7 ms budget (with margin) */
		if (s_pf_cap > 0) s_pf_cap--;
	} else if (busy + step < 15500u) {	/* room for one more whole frame: allow it */
		if (s_pf_cap < 3) s_pf_cap++;
	}
}

static void preempt_present(void)
{
	int pf;
	s_ahead_shown = 0;
	/* While the Pico menu is open the game is not being played (input is zeroed) and
	 * the overlay makes the present blit heavier. Running the look-ahead frames on top
	 * of that overran the 16.7 ms budget and starved the audio ring = the skipping heard
	 * with the menu open + Preemptive Frames on. Present the committed frame directly (no
	 * look-ahead, no rewind), which frees the budget for the overlay. Leave g_dbg_eff_pf
	 * at the last gameplay value so the menu still shows the chosen depth (e.g. "Max (3)")
	 * rather than flipping to "(0)". */
	/* (A menu opened by this frame's button press finds the committed frame undrawn: its picture
	 * holds the previous frame's pixels on the lines that changed for that one present, and the
	 * next committed frame, drawn because the menu is open, redraws them since they stayed dirty.) */
	if (s_menu_open) {
		g_dbg_preempt_emu_us = 0;
		ayaneo_gbc_show_frame(gba_core_screen());
		return;
	}
	pf = preempt_effective_depth();
	g_dbg_eff_pf = pf;
	if (pf <= 0) {
		g_dbg_preempt_emu_us = 0;
		ayaneo_gbc_show_frame(gba_core_screen());
		return;
	}
	{
		static unsigned char s_ahead_state[1024 * 1024] __attribute__((aligned(8)));   /* armGBA's state is ~770 KiB */
		static unsigned char s_ahead_snd[128 * 1024] __attribute__((aligned(8)));	/* sound ring */
		unsigned t0, t1, t2, t3;
		int i;
		t0 = gpt4_get_current_tick();
		gba_core_state_save(s_ahead_state);
		gba_sound_ring_save(s_ahead_snd);	/* ring contents (not in the savestate) */
		g_gba_audio_suppress = 1;
		for (i = 0; i < pf; i++) {
			gba_core_skip_render(i < pf - 1);   /* only the last look-ahead frame is shown */
			run_one_frame();		/* look-ahead frames (muted) */
		}
		gba_core_skip_render(0);
		g_gba_audio_suppress = 0;
		t1 = gpt4_get_current_tick();
		ayaneo_gbc_show_frame(gba_core_screen());	/* present (blit timed separately) */
		t2 = gpt4_get_current_tick();
		g_gba_load_light = 1;	/* ROM/BIOS caches stay valid across a same-ROM rewind */
		gba_core_state_load(s_ahead_state);
		gba_sound_ring_load(s_ahead_snd);	/* restore ring so committed audio is intact */
		g_gba_load_light = 0;
		s_cpu_clean_boundary = 1;	/* next committed frame re-enters as a full frame */
		s_cpu_restart_req = 1;
		t3 = gpt4_get_current_tick();
		g_dbg_preempt_emu_us = ((t1 - t0) + (t3 - t2)) / 13u;	/* excl the present/blit */
		s_ahead_us = (t1 - t0) / 13u;
		s_ahead_shown = 1;
	}
}

/* FNV-1a 32-bit over a buffer, chainable (seed the running hash in). */
static unsigned int fnv1a(unsigned int h, const unsigned char *p, unsigned long n)
{
	unsigned long i;
	for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}


/* Hash the full observable committed state - the 512 KB savestate + the 128 KB sound
 * ring (which the savestate omits) + the rendered 240x160 framebuffer (so VISUAL output
 * is covered - the sprite/video scratch is rebuilt from OAM outside the savestate, and a
 * rendering divergence would slip past a state-only hash). tmp_state/tmp_snd are caller
 * scratch. */
static unsigned int gba_selftest_hash(unsigned char *tmp_state, unsigned char *tmp_snd)
{
	unsigned int h;
	gba_core_state_save(tmp_state);
	gba_sound_ring_save(tmp_snd);
	h = fnv1a(2166136261u, tmp_state, 512u * 1024u);
	h = fnv1a(h, tmp_snd, 128u * 1024u);
	h = fnv1a(h, (const unsigned char *)gba_core_screen(), 240ul * 160ul * sizeof(unsigned short));
	return h;
}

/* Component hashes (state / sound ring / screen) into out[3], for pinpointing a FAIL. */
static void gba_selftest_hash3(unsigned char *tmp_state, unsigned char *tmp_snd, unsigned int out[3])
{
	gba_core_state_save(tmp_state);
	gba_sound_ring_save(tmp_snd);
	out[0] = fnv1a(2166136261u, tmp_state, 512u * 1024u);
	out[1] = fnv1a(2166136261u, tmp_snd, 128u * 1024u);
	out[2] = fnv1a(2166136261u, (const unsigned char *)gba_core_screen(),
		       240ul * 160ul * sizeof(unsigned short));
}

/* Re-enter the committed timeline from the state in buf (light flush, sound-ring
 * restore, clean-boundary longjmp) - the exact path a run-ahead committed frame uses. */
static void gba_selftest_reenter(const unsigned char *state, const unsigned char *snd)
{
	g_gba_load_light = 1;
	gba_core_state_load(state);
	gba_sound_ring_load(snd);
	g_gba_load_light = 0;
	s_cpu_clean_boundary = 1;
	s_cpu_restart_req = 1;
}

/* Determinism self-test for the run-ahead rewind path (device-blind validation).
 * Called at a frame boundary (CPU thread parked at vblank) with the machine at a
 * committed state S0. It compares two N-frame advances FROM S0:
 *   REF  = N committed frames, each re-entered via the load path with NO ahead frames.
 *   TEST = N committed frames, each preceded by the full run-ahead dance (save, run pf
 *          muted look-ahead frames, rewind) - i.e. exactly what preempt_present does.
 * REF and TEST are IDENTICAL except that TEST runs the muted look-ahead frames between
 * each save and rewind, and BOTH re-enter every committed frame through the SAME load
 * path (so the OAM-driven sprite scratch is rebuilt identically - REF is deliberately
 * NOT a normal-resume frame, which would render its scratch differently from a post-load
 * frame on games that skip their own oam_update, a false mismatch that is not a run-ahead
 * bug). Each leg's final hash covers state + sound ring + rendered screen. PASS (equal
 * hashes) proves the ahead frames leave NO residue on the committed trajectory over N
 * frames - so it catches not just a single-rewind gap (the sound-ring bug class) but
 * ACCUMULATING drift across many rewinds (how a per-frame speed skew like the old 0.34%
 * would manifest). Runs audio-MUTED; costs N*(pf+1) emulated frames of muted output,
 * then the game continues N frames past S0 (a valid advance). Zero cost when not run. */
static void gba_run_ahead_selftest(int nframes)
{
	static unsigned char s0_state[512 * 1024] __attribute__((aligned(8)));
	static unsigned char s0_snd[128 * 1024] __attribute__((aligned(8)));
	static unsigned char tmp_state[512 * 1024] __attribute__((aligned(8)));
	static unsigned char tmp_snd[128 * 1024] __attribute__((aligned(8)));
	unsigned int cref[3], ctest[3];
	if (gba_core_state_size() > sizeof s0_state) return;   /* the selftest buffers are gpSP sized */
	int pf = ayaneo_get_preempt_frames();
	int i, j, cmp;

	if (pf < 1) pf = 1;				/* always exercise at least depth 1 */
	if (nframes < 1) nframes = 1;

	/* snapshot S0 (the committed state we are standing on) */
	gba_core_state_save(s0_state);
	gba_sound_ring_save(s0_snd);

	g_gba_audio_suppress = 1;			/* mute the whole probe */

	/* REF: N committed frames, each via the load-path re-entry, NO ahead frames.
	 * Save the current state and re-enter from it (a no-op on state, but it forces
	 * the same dynarec-flush + oam_update + longjmp path TEST's committed frames use). */
	for (i = 0; i < nframes; i++) {
		gba_core_state_save(tmp_state);
		gba_sound_ring_save(tmp_snd);
		gba_selftest_reenter(tmp_state, tmp_snd);
		run_one_frame();
	}
	gba_selftest_hash3(tmp_state, tmp_snd, cref);

	/* restore S0 for the TEST leg */
	gba_selftest_reenter(s0_state, s0_snd);

	/* TEST: N committed frames, each preceded by the run-ahead dance (mirror of
	 * preempt_present: save, run pf muted ahead frames, rewind, committed frame). */
	for (i = 0; i < nframes; i++) {
		gba_core_state_save(tmp_state);
		gba_sound_ring_save(tmp_snd);
		for (j = 0; j < pf; j++)
			run_one_frame();		/* muted look-ahead frames */
		gba_selftest_reenter(tmp_state, tmp_snd);
		run_one_frame();			/* committed frame */
	}
	gba_selftest_hash3(tmp_state, tmp_snd, ctest);

	g_gba_audio_suppress = 0;

	cmp = (cref[0] == ctest[0] ? 1 : 0) |
	      (cref[1] == ctest[1] ? 2 : 0) |
	      (cref[2] == ctest[2] ? 4 : 0);
	g_dbg_selftest_cmp = cmp;
	g_dbg_selftest_rref = fnv1a(fnv1a(cref[0], (const unsigned char *)&cref[1], 4),
				    (const unsigned char *)&cref[2], 4);
	g_dbg_selftest_rtest = fnv1a(fnv1a(ctest[0], (const unsigned char *)&ctest[1], 4),
				     (const unsigned char *)&ctest[2], 4);
	/* Correctness invariant = machine state (bit0) AND sound ring (bit1) byte-identical:
	 * these are what MUST be perfect (the game plays and sounds identically across every
	 * rewind, incl. the sound-ring bug class), and device-testing confirms they hold at
	 * ANY N. The screen (bit2) is informational: at large N a final frame can differ by a
	 * transient from gpSP's per-frame internal render scratch (rebuilt each frame, not in
	 * the savestate) - it does NOT accumulate (state is proven identical, so nothing
	 * carries forward), is game-content dependent (non-monotonic in N), and self-corrects
	 * the next frame, so it is not a correctness failure. In real run-ahead the PRESENTED
	 * frame is the look-ahead frame and consecutive frames refresh continuously, so such a
	 * one-frame transient is imperceptible. pass: 0 FAIL (state/ring diverged = real bug),
	 * 1 PASS (all three match), 2 PASS with a benign screen transient (state+ring perfect,
	 * screen differs). */
	g_dbg_selftest_pass = ((cmp & 3) != 3) ? 0 : ((cmp == 7) ? 1 : 2);
	/* the game is now N frames past S0 on the TEST trajectory = a valid advance */
}

/* ===================== GammaOS Pico overlay menu ===================== */
static char *mi_puts(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mi_putu(char *p, unsigned v)
{
	char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n];
	return p;
}

static int ocv_pct(int mv)
{
	static const short lut[][2] = {
		{4350,100},{4250,95},{4150,85},{4060,75},{3980,65},{3900,55},
		{3840,45},{3790,35},{3740,25},{3690,17},{3630,10},{3550,5},
		{3450,1},{3300,0}
	};
	int n = (int)(sizeof(lut) / sizeof(lut[0])), i;
	if (mv >= lut[0][0]) return 100;
	if (mv <= lut[n - 1][0]) return 0;
	for (i = 0; i < n - 1; i++)
		if (mv <= lut[i][0] && mv > lut[i + 1][0]) {
			int v0 = lut[i][0], p0 = lut[i][1];
			int v1 = lut[i + 1][0], p1 = lut[i + 1][1];
			return p1 + (mv - v1) * (p0 - p1) / (v0 - v1);
		}
	return 0;
}

#define BAT_CHARGE_OFFSET_MV	80
/* Number of BATADC samples averaged. Each get_bat_sense_volt is one blocking pwrap
 * auxadc read (~1-2 ms). Kept small (4 ~= 5-8 ms) so that on the audio-mastered game
 * loop the whole read fits comfortably inside the AFE ring buffer (~341 ms) and is
 * absorbed with no audible artifact - so battery_read needs NO audio pause. */
#define BATTERY_SAMPLES		4
#include <platform/boot_mode.h>
/* boot facts from the preloader's arguments, for every board (oem boottime, the black box) */
unsigned ayaneo_pl_boot_ms(void) { extern BOOT_ARGUMENT *g_boot_arg; return g_boot_arg ? g_boot_arg->boot_time : 0u; }
int ayaneo_boot_reason(void) { extern BOOT_ARGUMENT *g_boot_arg; return g_boot_arg ? (int)g_boot_arg->boot_reason : -1; }
#ifdef KONKR_KPA
/* The MT6360 charger's own state (PMU block, I2C5 slave 0x34, CHG_STAT 0x4A bits 7:6):
 * 0 ready, 1 charging, 2 done, 3 fault; -1 when the read fails. The voltage table cannot
 * tell a full pack under charge apart from one still taking current (it rests at ~94%). */
#include <platform/mt_i2c.h>
#include <platform/boot_mode.h>
void ayaneo_ramcon_info(char *out, int n)
{
	extern BOOT_ARGUMENT *g_boot_arg;
	if (!g_boot_arg) { snprintf(out, n, "ramcon: no boot args"); return; }
	snprintf(out, n, "ramcon: sram %08x+%x def %u", g_boot_arg->ram_console_sram_addr,
		 g_boot_arg->ram_console_sram_size, g_boot_arg->ram_console_def_type);
}
int ayaneo_mt6360_chg_reg(unsigned char reg)
{
	mt_i2c i2c; unsigned char b[2];
	memset(&i2c, 0, sizeof i2c);
	i2c.id = I2C5; i2c.addr = 0x34; i2c.mode = FS_MODE; i2c.speed = 400; i2c.pushpull = true;
	b[0] = reg;
	if (i2c_write_read(&i2c, b, 1, 1) < 0) return -1;
	return b[0];
}
int ayaneo_mt6360_chg_wr(unsigned char reg, unsigned char val)
{
	mt_i2c i2c; unsigned char b[2];
	memset(&i2c, 0, sizeof i2c);
	i2c.id = I2C5; i2c.addr = 0x34; i2c.mode = FS_MODE; i2c.speed = 400; i2c.pushpull = true;
	b[0] = reg; b[1] = val;
	return i2c_write(&i2c, b, 2) < 0 ? -1 : 0;
}
/* The MT6360 also drives its own red charger indicator (CHRIND on ISINK1, PMU RGB_EN 0x80:
 * bit 3 = CHRIND under software control, bit 7 = ISINK1 on). Stock Android's rgbled driver
 * takes ISINK1 over the same way; here the AW2033 is the only power LED, so the hardware
 * indicator is switched to software mode and left off. Returns the register after the write. */
int ayaneo_mt6360_chrind_off(void)
{
	int v = ayaneo_mt6360_chg_reg(0x80);
	if (v < 0) return -1;
	v = (v | 0x08) & ~0x80;
	if (ayaneo_mt6360_chg_wr(0x80, (unsigned char)v) < 0) return -1;
	return ayaneo_mt6360_chg_reg(0x80);
}
static int charger_status(void)
{
	int v = ayaneo_mt6360_chg_reg(0x4A);
	return v < 0 ? -1 : ((v >> 6) & 3);
}
#else
static int charger_status(void) { return -1; }
#endif
static int battery_read(int *charging)
{
	int chr = upmu_is_chr_det();
	int pct;
#ifdef KONKR_KPA
	/* the fuel gauge path: pack profile, current compensation, filtered (ayaneo_gauge.c) */
	extern int ayaneo_gauge_sample(void);
	pct = ayaneo_gauge_sample();
	if (charging)
		*charging = chr;
#else
	long sum = 0;
	int i, vmv;
	for (i = 0; i < BATTERY_SAMPLES; i++)
		sum += get_bat_sense_volt(1);
	vmv = (int)(sum / BATTERY_SAMPLES);
	if (chr)
		vmv -= BAT_CHARGE_OFFSET_MV;
	if (charging)
		*charging = chr;
	pct = ocv_pct(vmv);
#endif
	if (chr) {
		int st = charger_status();
		if (st == 2) { pct = 100; { extern void ayaneo_gauge_set_full(void); ayaneo_gauge_set_full(); } }   /* the charger says done: full */
		else if (st == 1 && pct > 99) pct = 99;       /* still taking current: never show full */
	}
	return pct;
}
/* the battery for a menu's status line: percent, and whether it charges */
int gba_battery_pct(int *charging) { return battery_read(charging); }

/* charging screen: same colours as the running system (led_thread): charging = red + green,
 * full (97 %+ or the charger done) = green only, unplugged = off */
#define LED_LEVEL 12    /* the lowest clearly visible level (PWM of 255) */
/* charging = orange: red and green mixed at the low level, stepped by eye on the device
 * (12/12 read as lime green, the stock 24/4 too red) */
#define LED_CHG_R 17
#define LED_CHG_G 6
static void set_charge_led(int charging, int pct)
{
	static int prev = -1;
	int state = !charging ? 0 : (pct >= 97 ? 2 : 1);
	if (state == prev)
		return;
	prev = state;
	if (state == 2)      ayaneo_charge_led(0, LED_LEVEL, 0);
	else if (state == 1) ayaneo_charge_led(LED_CHG_R, LED_CHG_G, 0);
	else                 ayaneo_charge_led(0, 0, 0);
}

static volatile int s_batt_pct = 50;
static int s_panel_hz100;	/* measured panel refresh * 100 (e.g. 5973 = 59.73 Hz) */
/* Update the charge LED without hitching the frame. upmu_is_chr_det() is a cheap
 * PMIC read, done ~every 2 s.
 *
 * The near-full brightness cue used get_bat_sense_volt() (a BATADC auxadc read),
 * but that is a blocking pwrap spin (udelay 1300us/poll up to timeout, ~tens of ms
 * to ~1 s) plus a UART dprintf. On the audio-mastered emulator loop that stall
 * starves the AFE ring, which loops its 341 ms of samples = an audible replayed
 * blip. It fired ~every 30 s WHILE CHARGING - and the device is charging the whole
 * time it is on USB - so the game replayed a sample every 30-60 s. Dropping the
 * cosmetic full=bright cue (the LED still shows charging vs not) removes the stall
 * entirely; no periodic auxadc on the game thread. */
/* Power LED policy for every core and the menu, on its own low priority thread so neither the
 * 60 s battery read (an auxadc spin) nor the I2C writes touch a frame: charger connected =
 * red (green once full), on battery = green, at 5 % or less = red flashing at 1 Hz. The first
 * write waits for the speaker amps' bring-up, which shares i2c6. */
volatile int g_dbg_led_chr, g_dbg_led_pct, g_dbg_led_state, g_dbg_led_ticks;   /* oem led */
extern volatile int g_latency_led;   /* the latency light (below) owns the LED while it is on */
static int led_thread(void *arg)
{
	extern unsigned int pmic_upmu_get_rgs_chrdet(void);   /* the live CHRDET bit (upmu_is_chr_det prints to the UART) */
	int pct = -1, chr = 0, tick = 0, flash = 0, last = -1, state;
	(void)arg;
	thread_sleep(2500);
#ifdef KONKR_KPA
	{ extern int ayaneo_mt6360_chrind_off(void); ayaneo_mt6360_chrind_off(); }
	{ extern void ayaneo_bb_boot_check(void); ayaneo_bb_boot_check(); }   /* records the last runs left in DRAM -> the card */
#endif
	for (;;) {
		if ((tick & 1) == 0) chr = pmic_upmu_get_rgs_chrdet() ? 1 : 0;   /* every second, cheap */
		if (pct < 0 || (tick % 20) == 0) pct = battery_read(&chr);   /* every 10 s: the gauge filter and the time estimate feed on it */
		if (chr)              state = (pct >= 97) ? 1 : 3;             /* full (charger done or 97 %+): green only; charging: red + green */
		else if (pct <= 5)    { flash = !flash; state = flash ? 2 : 0; } /* low: red flash */
		else                  state = 1;                               /* battery: green */
		g_dbg_led_chr = chr; g_dbg_led_pct = pct; g_dbg_led_state = state; g_dbg_led_ticks = tick;
		{ extern unsigned int ayaneo_bb_uptime_ms(void); ayaneo_bb_uptime_ms(); }
		if (tick == 60) { extern void ayaneo_sd_guard_stable(void); ayaneo_sd_guard_stable(); }   /* 30 s up: a stable run */
		{ extern unsigned int ayaneo_rewind_verify_tables(int heal); ayaneo_rewind_verify_tables(1); }   /* page tables of the rewind islands, healed if hit */
		if (g_latency_led) {
			last = -1;                   /* the latency light owns the LED; restore once it is off */
		} else if (state != last) {
			last = state;
			if (state == 1)      ayaneo_charge_led(0, LED_LEVEL, 0);
			else if (state == 2) ayaneo_charge_led(LED_LEVEL, 0, 0);
			else if (state == 3) ayaneo_charge_led(LED_CHG_R, LED_CHG_G, 0);
			else                 ayaneo_charge_led(0, 0, 0);
		}
		thread_sleep(500);
		tick++;
	}
	return 0;
}
/* Latency light: while on, the power LED shows white for as long as any button is down, switched
 * within a millisecond of the press, so a camera filming the LED and the screen together measures
 * the whole path from the button to the picture (the game's reaction minus the LED's). Its own
 * thread at the highest priority samples the button pins every 1 ms tick and only sleeps in
 * between; a change costs one two byte I2C transfer (the enable bits, the levels set once). Not
 * saved: off at every boot. oem latled[:0|1] or the Pico menu's Latency LED. */
volatile int g_latency_led;
volatile unsigned int g_dbg_latled_presses;
static int latency_led_thread(void *arg)
{
	extern void ayaneo_led_enable(int mask);
	int on = 0, lit = 0;
	(void)arg;
	for (;;) {
		if (g_latency_led != on) {
			on = g_latency_led;
			lit = 0;
			if (on) { ayaneo_charge_led(255, 255, 255); ayaneo_led_enable(0); }
			/* off: led_thread puts the battery colour back at its next tick */
		}
		if (on) {
			unsigned i;
			int down = PRESSED(GPIO_X) || PRESSED(GPIO_Y);
			for (i = 0; !down && i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
				down = PRESSED(s_btn[i].gpio);
			if (down != lit) {
				lit = down;
				ayaneo_led_enable(down ? 7 : 0);
				if (down) g_dbg_latled_presses++;
			}
			thread_sleep(1);
		} else {
			thread_sleep(50);
		}
	}
	return 0;
}

/* every power off path: LED dark first (the AW2033 keeps its state across the SoC power off) */
void ayaneo_leds_off_for_poweroff(void)
{
	extern void ayaneo_charge_led_standby(void);
	ayaneo_charge_led_standby();
}
static void led_thread_start(void)
{
	static int up;
	thread_t *t;
	if (up) return;
	up = 1;
	t = thread_create("ayaneo_led", &led_thread, NULL, LOW_PRIORITY, 8192);
	if (t) thread_resume(t);
	t = thread_create("ayaneo_latled", &latency_led_thread, NULL, HIGHEST_PRIORITY, 4096);
	if (t) thread_resume(t);
}

/* Refresh s_batt_pct (shown in the Pico menu) at most once every interval_frames calls.
 * The read is a short BATADC average (BATTERY_SAMPLES) with NO audio pause: at ~5-8 ms it
 * fits inside the AFE ring buffer, so the loop just refills a touch late and the DMA plays
 * through - no cutout, no loop. It is still kept RARE because it does briefly stall this
 * single audio-mastered core: the Pico menu refreshes it at most once a minute while OPEN,
 * and gameplay only once every ten minutes in the background. Counted in frames, not the
 * 13 MHz tick, because that tick wraps every ~5.5 minutes (2^32 / 13e6) and cannot measure
 * a ten-minute interval. The shared counter resets on any read. */
#define BATTERY_MENU_FRAMES	3600u	/* ~1 min at 60 fps: refresh while the menu is open */
#define BATTERY_GAMEPLAY_FRAMES	36000u	/* ~10 min at 60 fps: rare background refresh in play */
static void battery_poll(unsigned interval_frames)
{
	static unsigned since;
	static int primed;
	int chr;
	if (primed && since++ < interval_frames)
		return;
	primed = 1;
	since = 0;
	s_batt_pct = battery_read(&chr);	/* no audio pause: short read, absorbed by the ring */
}

/* Manual CPU-clock OPPs, selectable in the CPU Clock menu all the way up to 2 GHz.
 * NOTE: these are ARM-PLL frequencies (ayaneo_set_cpu_mhz reprograms the PCW); LK
 * does NOT scale core voltage with them (see the escalation note below). */
static const unsigned s_cpu_opp[] = { 600, 800, 1000, 1200, 1400, 1600, 1800, 2000 };
static int s_cpu_idx = -1;
static int s_cpu_dirty = 1;
static void cpu_step(int dir)
{
	int n = (int)(sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0])), i;
	if (s_cpu_idx < 0) {
		unsigned cur = ayaneo_get_cpu_mhz(), bd = ~0u;
		int best = 0;
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
	{ extern void ayaneo_cpu_manual_set(int core, unsigned mhz); ayaneo_cpu_manual_set(0, s_cpu_opp[s_cpu_idx]); }
	s_settings_dirty = 1; s_settings_tick = gpt4_get_current_tick();   /* persisted like volume/brightness */
	s_cpu_dirty = 1;
}

/* Per-tier CPU escalation, driven from the SAME s_cpu_opp grid the manual CPU-Clock
 * menu uses, so the two paths set identical values and read back identically
 * (600->599, 1000->999, 1200->1199, 1400->1399): Off = idx0 600 (shows 599),
 * Balanced = idx2 1000 (999), Responsive = idx3 1200 (1199), Max = idx4 1400
 * (1399, the next OPP up from Responsive). NOTE: ayaneo_set_cpu_mhz only reprograms
 * the ARM-PLL PCW - it does NOT change core voltage (LK has no DVFS voltage table),
 * so the clock the preloader left the cores at (g_dbg_boot_mhz, the boot
 * Vproc-backed point) is the highest guaranteed-stable frequency; higher OPPs run
 * at that same fixed voltage and are the user's call. */
static void preempt_apply_cpu(int pf)
{
	static const int tier_idx[] = { 0, 2, 3, 4 };	/* Off, Balanced, Responsive, Max */
	int idx = (pf >= 1 && pf <= 3) ? tier_idx[pf] : tier_idx[0];
	extern unsigned ayaneo_core_clock(int core, unsigned tier_mhz, unsigned tier0_mhz);
	unsigned mhz = ayaneo_core_clock(0, s_cpu_opp[idx], s_cpu_opp[0]);   /* manual choice, filter bump */
	ayaneo_set_cpu_mhz(mhz);
	s_cpu_idx = -1;		/* re-seed the CPU-menu index from the live clock */
	s_cpu_dirty = 1;
}

enum { MK_UP=1, MK_DOWN=2, MK_LEFT=4, MK_RIGHT=8, MK_A=16, MK_B=32, MK_AYA=64 };
/* Debounce for MENU navigation (Pico menu + SNES carousel): a bit only counts as
 * pressed after it read pressed on MENU_DEBOUNCE consecutive frames, rejecting
 * contact bounce and single-frame line glitches. Release is immediate. 2-frame
 * matches gameplay (proven glitch-free and responsive); a heavier filter (was 4)
 * added ~50 ms of felt lag to menu presses, which is too much. */
#define MENU_DEBOUNCE 2
unsigned ayaneo_menu_debounce(unsigned raw, unsigned *hist)	/* hist: MENU_DEBOUNCE-1 words */
{
	unsigned deb = raw;
	int i;
	for (i = 0; i < MENU_DEBOUNCE - 1; i++) deb &= hist[i];
	for (i = MENU_DEBOUNCE - 2; i > 0; i--) hist[i] = hist[i - 1];
	hist[0] = raw;
	return deb;
}

unsigned menu_keys(void)	/* exported for gba_menu.c (carousel) */
{
	static unsigned prev, hist[MENU_DEBOUNCE - 1];
	unsigned raw = 0, deb, edge;
	if (PRESSED(GPIO_UP))    raw |= MK_UP;
	if (PRESSED(GPIO_DOWN))  raw |= MK_DOWN;
	if (PRESSED(GPIO_LEFT))  raw |= MK_LEFT;
	if (PRESSED(GPIO_RIGHT)) raw |= MK_RIGHT;
	if (PRESSED(GPIO_A))     raw |= MK_A;
	if (PRESSED(GPIO_B))     raw |= MK_B;
	if (PRESSED(GPIO_AYA))   raw |= MK_AYA;
	/* Left analog stick drives the menu D-pad too (JOY_UP/DOWN/LEFT/RIGHT = MK_UP/DOWN/LEFT/RIGHT =
	 * 1/2/4/8). Cache is refreshed by ayaneo_joypad_poll() at the game-loop top; deadzoned, so a
	 * centred stick contributes nothing (menu behaves exactly as before at rest). */
	{ extern unsigned int ayaneo_joypad_dpad(void); raw |= ayaneo_joypad_dpad() & (MK_UP|MK_DOWN|MK_LEFT|MK_RIGHT); }
	{	/* oem nav: a key injected over USB drives the Pico menu too (L R U D A B; screenshots) */
		extern volatile int g_dbg_key, g_dbg_key_hold;
		static const unsigned char map[6] = { MK_LEFT, MK_RIGHT, MK_UP, MK_DOWN, MK_A, MK_B };
		if (s_menu_open && g_dbg_key_hold > 0 && g_dbg_key >= 0 && g_dbg_key < 6) {
			raw |= map[g_dbg_key];
			g_dbg_key_hold--;
		}
	}
	deb = ayaneo_menu_debounce(raw, hist);	/* max debounce for the Pico menu */
	edge = deb & ~prev;
	prev = deb;
	return edge;
}

/* the SD build has no boot animation or chime: its row is the BIOS intro background instead */
#if defined(AYANEO_GBA_SD)
#define MI_BOOTROW MI_INTROBG
#else
#define MI_BOOTROW MI_SKIPBOOT
#endif
enum {
	MI_BRIGHT, MI_VOLUME, MI_FILTER, MI_ASPECT, MI_COLORCORRECT, MI_LOADBOOT, MI_RESUME, MI_BOOTROW, MI_SKIPINTRO,
	MI_SLOT, MI_LOADSTATE, MI_SAVESTATE, MI_BATTERY, MI_CPU, MI_PANEL, MI_STROBE, MI_PREEMPT, MI_REWIND, MI_BENCH, MI_SHOWFPS, MI_LATLED, MI_CORE, MI_CORESET, MI_RESET, MI_CLOSE, MI_COUNT
};
#define GBA_OD_MENU_MAX 1   /* overdrive is On/Off (On = tuned halo-free profile; live-poke via `oem od:`) */

/* Display aspect (0=Pixel integer, 1=Fit native-aspect fill, 2=Stretch full panel). Read by
 * ayaneo_gbc_show_frame (mt_disp_drv.c); session-scoped, defaults to Pixel = current behaviour. */
extern volatile int g_gba_aspect;

/* Manual save-state slot (0..GBA_SLOT_COUNT-1). Session-scoped; the .st<slot> files persist. */
#define GBA_SLOT_COUNT 3
static int s_gba_slot;

static const char *filter_name(int f)
{ return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : f == 4 ? "LCD1x" : f == 5 ? "LCD3x" : "Off"; }

static const char *menu_value(int item, char *buf)
{
	char *p = buf;
	switch (item) {
	case MI_BRIGHT:   p = mi_putu(p, (unsigned)ayaneo_brightness_pct()); p = mi_puts(p, "%"); break;
	case MI_VOLUME:   p = mi_putu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mi_puts(p, "%"); break;
	case MI_FILTER:   p = mi_puts(p, filter_name(ayaneo_get_lcd_filter_core(1))); break;
	case MI_COLORCORRECT: p = mi_puts(p, ayaneo_get_color_correct() ? "On" : "Off"); break;
	case MI_STROBE:   { int b = ayaneo_get_strobe(); p = mi_puts(p, ayaneo_strobe_name(b)); } break;
	case MI_REWIND:   { extern int ayaneo_get_rewind(void); p = mi_puts(p, ayaneo_get_rewind() ? "On" : "Off"); } break;
	case MI_PREEMPT: { int pf = ayaneo_get_preempt_frames();
		/* Named tiers (max desired depth) + the depth the closed loop is running
		 * right now (it backs off to hold 60 fps), so the user sees it adapt -
		 * e.g. "Max (1)". */
		switch (pf) {
		case 1:  p = mi_puts(p, "Balanced"); break;
		case 2:  p = mi_puts(p, "Responsive"); break;
		case 3:  p = mi_puts(p, "Max"); break;
		default: p = mi_puts(p, "Off"); break;
		}
		if (pf > 0) {
			p = mi_puts(p, " (");
			p = mi_putu(p, (unsigned)(g_dbg_eff_pf < 0 ? 0 : g_dbg_eff_pf));
			p = mi_puts(p, ")");
		}
		break; }
	case MI_LOADBOOT: p = mi_puts(p, ayaneo_get_load_on_boot() ? "On" : "Off"); break;
	case MI_RESUME:   { extern int ayaneo_get_resume_on_boot(void); p = mi_puts(p, ayaneo_get_resume_on_boot() ? "On" : "Off"); } break;
#if defined(AYANEO_GBA_SD)
	case MI_INTROBG: p = mi_puts(p, ayaneo_get_intro_black() ? "Black" : "White"); break;
#else
	case MI_SKIPBOOT: p = mi_puts(p, ayaneo_get_skip_boot() ? "On" : "Off"); break;
#endif
	case MI_SKIPINTRO: p = mi_puts(p, ayaneo_get_skip_gba_intro() ? "On" : "Off"); break;
	case MI_ASPECT:    p = mi_puts(p, g_gba_aspect == 2 ? "Stretch" : g_gba_aspect == 1 ? "Fit" : "Pixel Perfect"); break;
	case MI_SLOT:      p = mi_putu(p, (unsigned)s_gba_slot); break;
	case MI_LOADSTATE:
	case MI_SAVESTATE:
	case MI_RESET:     p = mi_puts(p, "[A]"); break;
	case MI_BATTERY: {
		extern int ayaneo_gauge_time_str(char *out, int n);
		char ts[24];
		p = mi_putu(p, (unsigned)s_batt_pct); p = mi_puts(p, "% ");
		p = mi_puts(p, upmu_is_chr_det() ? "Charging" : "Battery");
		if (ayaneo_gauge_time_str(ts, sizeof ts)) { p = mi_puts(p, "  "); p = mi_puts(p, ts); }
		break; }
	case MI_CPU: {
		static unsigned mhz, tick;
		if (s_cpu_dirty || !mhz || tick-- <= 0) { mhz = ayaneo_get_cpu_mhz(); tick = 40; s_cpu_dirty = 0; }
		p = mi_putu(p, mhz); p = mi_puts(p, " MHz");
		break;
	}
	case MI_PANEL: {
		/* measured refresh (3 decimals) vs the GBA's 59.7275 Hz. Prefer the precise
		 * 128-frame average (g_dbg_hz1000, Hz*1000); fall back to the coarse boot
		 * measurement (s_panel_hz100, Hz*100) until the average is valid. */
		extern volatile unsigned int g_dbg_hz1000;
		unsigned hz = g_dbg_hz1000 ? g_dbg_hz1000 : (unsigned)s_panel_hz100 * 10u;
		unsigned frac = hz % 1000u;
		p = mi_putu(p, hz / 1000u); *p++ = '.';
		if (frac < 100u) *p++ = '0';
		if (frac < 10u)  *p++ = '0';
		p = mi_putu(p, frac);
		p = mi_puts(p, " Hz");
		break;
	}
	case MI_BENCH:
		if (s_benchmark) { p = mi_putu(p, (unsigned)s_fps); p = mi_puts(p, " fps"); }
		else p = mi_puts(p, "Off");
		break;
	case MI_SHOWFPS: { extern int ayaneo_get_show_fps(void); p = mi_puts(p, ayaneo_get_show_fps() ? "On" : "Off"); } break;
	case MI_LATLED:  p = mi_puts(p, g_latency_led ? "On" : "Off"); break;
	case MI_CORE:
		if (s_sdcore_present == 0) p = mi_puts(p, "gpSP");
		else p = mi_puts(p, s_core_pref ? "gpSP" : "armGBA");
		if ((s_core_pref == 0 && s_sdcore_present > 0) != s_core_is_sd) p = mi_puts(p, " (next game)");
		break;
	case MI_CORESET:
		p = mi_puts(p, (g_core && g_core->version >= GBA_CORE_ABI_V3 && g_core->opt_count && g_core->opt_count() > 0) ? ">" : "None");
		break;
	case MI_CLOSE: default: break;
	}
	*p = 0;
	return buf;
}

static const char *menu_label(int item)
{
	switch (item) {
	case MI_BRIGHT:    return "Brightness";
	case MI_VOLUME:    return "Volume";
	case MI_FILTER:    return "LCD Filter";
	case MI_COLORCORRECT: return "Color Correction";
	case MI_LOADBOOT:  return "Load State on Boot";
	case MI_RESUME:    return "Resume Game on Boot";
#if defined(AYANEO_GBA_SD)
	case MI_INTROBG:   return "BIOS Intro Background";
#else
	case MI_SKIPBOOT:  return "Skip Boot Anim/Chime";
#endif
	case MI_SKIPINTRO: return "Skip BIOS Intro";
	case MI_ASPECT:    return "Aspect Ratio";
	case MI_SLOT:      return "Save Slot";
	case MI_LOADSTATE: return "Load State";
	case MI_SAVESTATE: return "Save State";
	case MI_BATTERY:   return "Battery";
	case MI_CPU:       return "CPU Clock";
	case MI_PANEL:     return "Panel Refresh";
	case MI_STROBE:    return "BFI";
	case MI_PREEMPT:   return "Run-Ahead";
	case MI_REWIND:    return "Rewind";
	case MI_BENCH:     return "Benchmark (Uncap)";
	case MI_SHOWFPS:   return "Show FPS";
	case MI_LATLED:    return "Latency LED";
	case MI_CORE:      return "GBA Core";
	case MI_CORESET:   return "Core Settings";
	case MI_RESET:     return "Reset Game";
	case MI_CLOSE:     return "Close";
	}
	return "";
}

static int s_menu_sel;
static char s_menu_status[48];

static int menu_change(int item, int dir, int act, unsigned char *state, char *status)
{
	int changed = 1;
	status[0] = 0;
	switch (item) {
	case MI_BRIGHT:   if (dir) ayaneo_brightness_step(dir); else changed = 0; break;
	case MI_VOLUME:   if (dir) ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); else changed = 0; break;
	case MI_FILTER:   if (dir) { ayaneo_set_lcd_filter_core(1, (ayaneo_get_lcd_filter_core(1) + dir + 6) % 6); preempt_apply_cpu(ayaneo_get_preempt_frames()); } else changed = 0; break;
	case MI_COLORCORRECT: if (dir || act) ayaneo_set_color_correct(!ayaneo_get_color_correct()); else changed = 0; break;
	case MI_STROBE:   if (dir) ayaneo_set_strobe((ayaneo_get_strobe() + dir + 4) % 4); else if (act) ayaneo_set_strobe((ayaneo_get_strobe() + 1) % 4); else changed = 0; break;
	case MI_PREEMPT:  if (dir || act) { int d = dir ? dir : 1;
		ayaneo_set_preempt_frames((ayaneo_get_preempt_frames() + d + 4) % 4);
		/* CPU clock escalation is applied in the game loop on any level change,
		 * so it also covers the oem preempt: path. */
	} else changed = 0; break;
	case MI_REWIND:   { extern int ayaneo_get_rewind(void); extern void ayaneo_set_rewind(int v);
			    if (dir || act) ayaneo_set_rewind(!ayaneo_get_rewind()); else changed = 0; } break;
	case MI_LOADBOOT: if (dir || act) ayaneo_set_load_on_boot(!ayaneo_get_load_on_boot()); else changed = 0; break;
	case MI_RESUME:   { extern int ayaneo_get_resume_on_boot(void); extern void ayaneo_set_resume_on_boot(int v);
			    if (dir || act) ayaneo_set_resume_on_boot(!ayaneo_get_resume_on_boot()); else changed = 0; } break;
#if defined(AYANEO_GBA_SD)
	case MI_INTROBG:  if (dir || act) ayaneo_set_intro_black(!ayaneo_get_intro_black()); else changed = 0; break;
#else
	case MI_SKIPBOOT: if (dir || act) ayaneo_set_skip_boot(!ayaneo_get_skip_boot()); else changed = 0; break;
#endif
	case MI_SKIPINTRO: if (dir || act) ayaneo_set_skip_gba_intro(!ayaneo_get_skip_gba_intro()); else changed = 0; break;
	case MI_CPU:      if (dir) cpu_step(dir); changed = 0; break;
	case MI_BENCH:    if (dir || act) s_benchmark = !s_benchmark; changed = 0; break;
	case MI_SHOWFPS:  { extern int ayaneo_get_show_fps(void); extern void ayaneo_set_show_fps(int v);
			    if (dir || act) ayaneo_set_show_fps(!ayaneo_get_show_fps()); else changed = 0; } break;
	case MI_LATLED:   if (dir || act) g_latency_led = !g_latency_led; else changed = 0; break;   /* not saved */
	case MI_CORE:
		if (!(dir || act)) { changed = 0; break; }
		if (s_sdcore_present == 0) { mi_puts(status, "No armGBA core on the card"); changed = 0; break; }
		s_core_pref = !s_core_pref;
		core_opt_store("core", s_core_pref);
		mi_puts(status, "Takes effect at the next game start");
		break;
	case MI_CORESET:
		changed = 0;
		if ((act || dir > 0) && g_core && g_core->version >= GBA_CORE_ABI_V3 && g_core->opt_count && g_core->opt_count() > 0) {
			s_menu_page = 1; s_core_sel = 0;
		} else if (act || dir) {
			mi_puts(status, "gpSP has no core settings");
		}
		break;
	case MI_ASPECT:   if (dir) g_gba_aspect = (g_gba_aspect + dir + 3) % 3; else changed = 0; break;   /* persist */
	case MI_SLOT:     if (dir) s_gba_slot = (s_gba_slot + dir + GBA_SLOT_COUNT) % GBA_SLOT_COUNT; changed = 0; break;
	case MI_LOADSTATE: if (act) { int ok = manual_state_read(state); if (ok) gba_rewind_arm(); mi_puts(status, ok ? "State loaded" : "No save state"); } changed = 0; break;
	case MI_SAVESTATE: if (act) { int ok = manual_state_write(state); sav_save(state); mi_puts(status, ok ? "State saved" : "Save failed"); } changed = 0; break;
	case MI_RESET:    if (act) { s_reset_req = 1; return 1; } changed = 0; break;
	case MI_CLOSE:    if (act) { s_close_req = 1; return 1; } changed = 0; break;
	default: changed = 0; break;
	}
	if (changed) {
		/* DEFER the persist. ayaneo_settings_save() does a card (FAT) write that
		 * stalls this loop for ~1 s; done inline on every menu change
		 * (brightness, LCD filter, ...) that stall stutters the game and skips audio
		 * WHILE the menu is open. Mark dirty + timestamp instead and let the game loop
		 * flush ONCE, ~0.7 s after the last change, with the audio ring silenced across
		 * the write (same deferral the in-game volume/brightness already use). Rapid
		 * changes (holding the key) coalesce into a single write. */
		s_settings_dirty = 1;
		s_settings_tick = gpt4_get_current_tick();
	}
	return 0;
}

volatile int g_dbg_igm_toggle;   /* fastboot `oem igm`: toggle the in-game menu as if the menu key was pressed */
static int aya_edge(void)
{
	static int prev;
	int now = PRESSED(GPIO_AYA), edge = now && !prev;
	prev = now;
	if (g_dbg_igm_toggle) { g_dbg_igm_toggle = 0; return 1; }
	return edge;
}
static void menu_toggle(void)
{
	extern void ayaneo_menu_overlay_mark_dirty(void);
	s_menu_open = !s_menu_open;
	s_menu_status[0] = 0;
	s_menu_page = 0;
	ayaneo_menu_overlay_mark_dirty();	/* repaint the static overlay on open/close */
	if (s_menu_open)
		battery_poll(BATTERY_MENU_FRAMES);	/* refresh the shown % on open (<= once/min) */
	menu_keys();		/* drop the AYA edge */
}

static void menu_tick(unsigned char *state)
{
	extern void ayaneo_menu_overlay_mark_dirty(void);
	unsigned k = menu_keys();
	battery_poll(BATTERY_MENU_FRAMES);	/* menu open: refresh the shown % at most once/min */
	if (k) ayaneo_menu_overlay_mark_dirty();	/* any nav/value key changes the panel -> repaint */
	if (s_menu_page == 1) {
		/* Core Settings: the core's own options, then Back */
		int n = g_core->opt_count(), rows = n + 1;
		if (k & MK_UP)   { s_core_sel = (s_core_sel + rows - 1) % rows; s_menu_status[0] = 0; }
		if (k & MK_DOWN) { s_core_sel = (s_core_sel + 1) % rows; s_menu_status[0] = 0; }
		if ((k & (MK_LEFT | MK_RIGHT | MK_A)) && s_core_sel < n) {
			const struct gba_core_option *o = g_core->opt_info(s_core_sel);
			int d = (k & MK_LEFT) ? -1 : 1;
			if (o && o->nvalues > 0) {
				int v = (g_core->opt_get(s_core_sel) + d + o->nvalues) % o->nvalues;
				g_core->opt_set(s_core_sel, v);
				core_opt_store(o->key, v);
				s_menu_status[0] = 0;
				if (o->flags & GBA_CORE_OPT_RESTART) mi_puts(s_menu_status, "Takes effect at the next game start");
				s_settings_dirty = 1;
				s_settings_tick = gpt4_get_current_tick();
			}
		}
		if ((k & MK_A) && s_core_sel == n) { s_menu_page = 0; s_menu_status[0] = 0; }
		if (k & MK_B) { s_menu_page = 0; s_menu_status[0] = 0; }
		return;
	}
	if (k & MK_UP)    { s_menu_sel = (s_menu_sel + MI_COUNT - 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_DOWN)  { s_menu_sel = (s_menu_sel + 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_LEFT)  { if (menu_change(s_menu_sel, -1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_RIGHT) { if (menu_change(s_menu_sel, +1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_A)     { if (menu_change(s_menu_sel, 0, 1, state, s_menu_status)) { s_menu_open = 0; s_ab_latch |= GB_A; } }
	if (k & MK_B)     { s_menu_open = 0; s_ab_latch |= GB_B; }
}

extern int ayaneo_pico_layout(int count, int H, int sel, int *top, int *rowH, int *panelH);
extern void ayaneo_pico_bar(unsigned int *buf, unsigned int pitch, int x, int w, int y, int rowH);
extern void ayaneo_pico_more(unsigned int *buf, unsigned int pitch, int px, int py, int panelW, int panelH, int up, int down);
/* called by ayaneo_gbc_show_frame() (mt_disp_drv.c) to paint the overlay */
void gbc_menu_draw_overlay(unsigned int *buf, unsigned int pitch,
			   unsigned int W, unsigned int H)
{
	static int s_top, s_core_top;
	int rowH, panelW = 660, panelH, vis;
	int px, py;
	/* the row pitch, the panel height and the rows shown (the list scrolls when it does not fit) */
	if (s_menu_page == 1 && g_core && g_core->version >= GBA_CORE_ABI_V3)
		vis = ayaneo_pico_layout(g_core->opt_count() + 1, (int)H, s_core_sel, &s_core_top, &rowH, &panelH);
	else
		vis = ayaneo_pico_layout(MI_COUNT, (int)H, s_menu_sel, &s_top, &rowH, &panelH);
	px = ((int)W - panelW) / 2; py = ((int)H - panelH) / 2;
	int x = px + 28, y = py + 60, i;
	char val[48];

	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xD010141Cu);   /* translucent: the game shows through (hardware blend) */
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	if (s_menu_page == 1 && g_core && g_core->version >= GBA_CORE_ABI_V3) {
		/* Core Settings: title, the core's name, its options and Back, in the same panel */
		int n = g_core->opt_count(), r;
		/* The title sits higher than the Pico menu's: its "g" reaches glyph row 14 (44 px at 3x)
		 * and the first row's selection bar starts 57 px down the panel. The core's name follows
		 * it on the same baseline (row 11 of both sizes); under the title it ran into the glyphs. */
		int tx = ayaneo_text(buf, pitch, px + 28, py + 11, 3, 0xFFFFFFFFu, "Core Settings");
		ayaneo_text(buf, pitch, tx + 16, py + 33, 1, 0xFF8890A0u, g_core->name ? g_core->name : "");
		for (r = s_core_top; r <= n && r < s_core_top + vis; r++, y += rowH) {
			unsigned int fg = (r == s_core_sel) ? 0xFF101018u : 0xFFC8D0E0u;
			const char *lab = "Back", *v = "";
			int vw;
			if (r == s_core_sel)
				ayaneo_pico_bar(buf, pitch, px + 10, panelW - 20, y, rowH);
			if (r < n) {
				const struct gba_core_option *o = g_core->opt_info(r);
				int cur = g_core->opt_get(r);
				lab = o ? o->label : "";
				v = (o && cur >= 0 && cur < o->nvalues) ? o->values[cur] : "";
			}
			ayaneo_text(buf, pitch, x, y, 2, fg, lab);
			for (vw = 0; v[vw]; vw++) ;
			ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, v);
		}
		ayaneo_pico_more(buf, pitch, px, py, panelW, panelH, s_core_top > 0, s_core_top + vis < n + 1);
		if (s_menu_status[0])
			ayaneo_text(buf, pitch, x, py + panelH - 32, 2, 0xFF80E080u, s_menu_status);
		ayaneo_text(buf, pitch, x, py + panelH - 14, 1, 0xFF8890A0u,
			    "Up/Down move  Left/Right change  B back");
		return;
	}
	ayaneo_text(buf, pitch, px + 28, py + 20, 3, 0xFFFFFFFFu, "GammaOS Pico");

	for (i = s_top; i < MI_COUNT && i < s_top + vis; i++, y += rowH) {
		unsigned int fg = (i == s_menu_sel) ? 0xFF101018u : 0xFFC8D0E0u;
		int vw;
		if (i == s_menu_sel)
			ayaneo_pico_bar(buf, pitch, px + 10, panelW - 20, y, rowH);
		ayaneo_text(buf, pitch, x, y, 2, fg, menu_label(i));
		menu_value(i, val);
		for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	ayaneo_pico_more(buf, pitch, px, py, panelW, panelH, s_top > 0, s_top + vis < MI_COUNT);
	if (s_menu_status[0])
		ayaneo_text(buf, pitch, x, py + panelH - 32, 2, 0xFF80E080u, s_menu_status);
	ayaneo_text(buf, pitch, x, py + panelH - 14, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

/* ===================== on-screen debug (UART is not wired on this device) =====
 * Paint a status line to the panel so the LAST message left on screen shows how
 * far bring-up got (or which step hung). Drawn into BOTH scan-out buffers so a
 * hang leaves a stable, readable line. */
extern void ayaneo_display_prepare(void);	/* mt_disp_drv.c (also declared below) */
static void gba_dbg(const char *msg)
{
	/* Trace-only now that GBA is stable: the on-screen green status text is gated
	 * out (it also cost two vsync-blocked presents per call, slowing the load).
	 * Define AYANEO_GBA_DEBUG_OSD to bring the on-screen status back. */
#ifdef AYANEO_GBA_DEBUG_OSD
	int i;
	for (i = 0; i < 2; i++) {		/* both buffers -> stable on hang */
		unsigned int pitch, W, H;
		unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
		ayaneo_fill(buf, pitch, 0, 0, (int)W, 44, 0xFF000000u);
		ayaneo_text(buf, pitch, 20, 8, 3, 0xFF30FF60u, msg);
		ayaneo_canvas_present();
	}
#endif
	GBA_ATRACE("%s\n", msg);
}

/* 0-padded 8-hex into p (returns end) */
static char *gba_hex(char *p, unsigned v)
{
	static const char h[] = "0123456789abcdef";
	int i;
	*p++ = '0'; *p++ = 'x';
	for (i = 28; i >= 0; i -= 4)
		*p++ = h[(v >> i) & 0xf];
	return p;
}

/* Called from LK's exception handler (arch/arm/faults.c) instead of a silent
 * halt(), so a JIT fault paints its type + faulting PC + data-fault address to
 * the panel. If pc lands in the translation-cache arena (0x50000000+), the
 * generated code itself faulted; dfar is the bad data address it touched. */
unsigned g_fault_regs[5];   /* set by arch/arm/faults.c before the banner: r0 r1 r2 r3 svc_lr */
void ayaneo_gba_fault_screen(const char *msg, unsigned pc, unsigned addr, unsigned spsr)
{
	char l1[64], l2[128], *p;
	int i, n;
	/* A SNES RSZ session may have left the display path resized/tiled, which would render this
	 * screen as unreadable garbage. Restore the normal full-panel OVL path first. */
	extern void ayaneo_snes_rsz_restore(void);
	ayaneo_snes_rsz_restore();

	/* l1 = the message (already short, e.g. "data abort, halting") */
	for (n = 0; msg[n] && n < 40; n++) l1[n] = msg[n];
	l1[n] = 0;
	/* strip trailing newline */
	if (n && l1[n-1] == '\n') l1[n-1] = 0;

	/* One combined diagnostic line: "FAULT <msg> pc=.. dfar=.. spsr=.." */
	p = l2;
	p = mi_puts(p, "FAULT ");
	for (n = 0; l1[n]; n++) *p++ = l1[n];
	p = mi_puts(p, " pc="); p = gba_hex(p, pc);
	p = mi_puts(p, " dfar="); p = gba_hex(p, addr);
	p = mi_puts(p, " spsr="); p = gba_hex(p, spsr);
	*p = 0;

	/* Also dump the captured RSZ register readbacks (persist across the fault) so a photo of the
	 * crash screen gives the full display-path state. */
	{
		extern volatile unsigned g_rszdbg[12];
		char l3[128], l4[128], *q;
		q = l3; q = mi_puts(q, "roi="); q = gba_hex(q, g_rszdbg[0]);
		q = mi_puts(q, " pitch="); q = gba_hex(q, g_rszdbg[2]);
		q = mi_puts(q, " in="); q = gba_hex(q, g_rszdbg[6]);
		q = mi_puts(q, " out="); q = gba_hex(q, g_rszdbg[7]); *q = 0;
		q = l4; q = mi_puts(q, "2lm="); q = gba_hex(q, g_rszdbg[8]);
		q = mi_puts(q, " rszm="); q = gba_hex(q, g_rszdbg[9]);
		q = mi_puts(q, " wcg="); q = gba_hex(q, g_rszdbg[10]);
		q = mi_puts(q, " rszsel="); q = gba_hex(q, g_rszdbg[11]); *q = 0;
		{	/* rewind ring: wr/arena records recon prev lastoff+len bad rewinding */
			extern void ayaneo_rewind_dbg_line(char *out, unsigned int cap);
			static char l5[160], l6[128];
			char *r = l5; r = mi_puts(r, "rw "); ayaneo_rewind_dbg_line(r, (unsigned)(sizeof l5 - 4));
			r = l6; r = mi_puts(r, "r0="); r = gba_hex(r, g_fault_regs[0]); r = mi_puts(r, " r1="); r = gba_hex(r, g_fault_regs[1]);
			r = mi_puts(r, " r2="); r = gba_hex(r, g_fault_regs[2]); r = mi_puts(r, " r3="); r = gba_hex(r, g_fault_regs[3]);
			r = mi_puts(r, " lr="); r = gba_hex(r, g_fault_regs[4]); *r = 0;
			for (i = 0; i < 2; i++) {
				unsigned int pitch, W, H;
				unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
				ayaneo_fill(buf, pitch, 0, 0, (int)W, 192, 0xFF200000u);
				ayaneo_text(buf, pitch, 20, 20, 2, 0xFFFFFFFFu, l2);
				ayaneo_text(buf, pitch, 20, 60, 2, 0xFFFFC0C0u, l3);
				ayaneo_text(buf, pitch, 20, 92, 2, 0xFFFFC0C0u, l4);
				ayaneo_text(buf, pitch, 20, 124, 2, 0xFFFFE0A0u, l5);
				ayaneo_text(buf, pitch, 20, 156, 2, 0xFFA0E0FFu, l6);
				ayaneo_canvas_present();
			}
		}
	}
}

#ifdef AYANEO_GBA_SD
#include "sd_fat.h"
#include "fat_wr.h"
#include "gba_sd_save.h"
#include "core_file.h"
extern const struct gba_core_exports *gba_core_load_file(fat_vol *v, const char *path);	/* gba_core_loader.c: from the card */
extern volatile int g_gba_core_err;	/* gba_core_loader.c: why the last load failed (core_file.h) */
/* SD flow state, shared between the boot gate and emu_thread. s_sd_mode=1 makes
 * emu_thread run the SD flow (ROM-select then game) instead of the boot_b ROM. */
static int s_sd_mode;
static void sd_bg_wait(void);   /* fast boot path: wait for the card listing (defined below) */
static unsigned char s_sd_bios[16384];
static int s_have_bios;   /* the card has a GBA BIOS: the intro plays, and gpSP can run */
static fat_vol s_sd_vol;
static gba_rom_entry s_roms[128];   /* enumerated /roms/gba, sorted (task d/e) */
static int s_nrom;

/* GB/GBC games run through the gambatte core blob (gbc_sd_run.c); it reuses the gpSP
 * arena at 0x50000000, so after a GB/GBC session the gpSP arena must be rebuilt
 * (gba_core_init) before the next GBA game. s_gpsp_dirty tracks that. */
extern void gbc_sd_session(fat_vol *vol, const gba_rom_entry *rom);
extern void snes_sd_session(fat_vol *vol, const gba_rom_entry *rom);   /* snes9x via boot_b blob */
extern void genesis_sd_session(fat_vol *vol, const gba_rom_entry *rom); /* Genesis-Plus-GX (MD/SMS/GG/SG) blob */
static int s_gpsp_dirty;

/* ---- GBA core choice: armGBA-pico from the card, gpSP from boot_b ----
 * Boot and the BIOS intro always run on gpSP from boot_b (the card is still being mounted then).
 * At each GBA game launch gba_core_for_launch loads /cores/armgba-pico.blob into the core slot
 * when it is on the card and GBA Core is not set to gpSP; if the file is missing or rejected,
 * gpSP stays (or is loaded back). The CPU thread is parked in gba_yield_to_main while this
 * happens, and the launch sets s_cpu_restart_req, so it re-enters the new core's cpu_loop.
 * The menu reuses the arena while it runs, so the core is initialised again at every launch.
 * /gba/armgba-pico.cfg holds the choice ("core") and the core's options (by key). */
#define SDCORE_PATH "/cores/armgba-pico.blob"
#define CORECFG_MAX 24
static struct { char key[32]; int val; } s_corecfg[CORECFG_MAX];
static int s_corecfg_n, s_corecfg_loaded;

static void core_opt_store(const char *key, int val)
{
	int i, k;
	for (i = 0; i < s_corecfg_n; i++)
		if (!strcmp(s_corecfg[i].key, key)) { s_corecfg[i].val = val; s_corecfg_dirty = 1; return; }
	if (s_corecfg_n >= CORECFG_MAX) return;
	for (k = 0; key[k] && k < (int)sizeof s_corecfg[0].key - 1; k++) s_corecfg[s_corecfg_n].key[k] = key[k];
	s_corecfg[s_corecfg_n].key[k] = 0;
	s_corecfg[s_corecfg_n].val = val;
	s_corecfg_n++;
	s_corecfg_dirty = 1;
}
static int core_opt_lookup(const char *key, int *val)
{
	int i;
	for (i = 0; i < s_corecfg_n; i++) if (!strcmp(s_corecfg[i].key, key)) { *val = s_corecfg[i].val; return 1; }
	return 0;
}
static void corecfg_load(void)
{
	static char buf[2048];
	uint32_t n = gba_sd_read_named(&s_sd_vol, "/gba", "armgba-pico", "cfg", (unsigned char *)buf, sizeof buf - 1);
	char *p = buf, *e;
	int v;
	s_corecfg_loaded = 1;
	s_corecfg_n = 0;
	buf[n] = 0;
	while (*p) {
		char *eq;
		e = p; while (*e && *e != '\n') e++;
		eq = p; while (eq < e && *eq != '=') eq++;
		if (eq < e) {
			char key[32]; int k = 0; char *q = eq + 1;
			while (p < eq && k < (int)sizeof key - 1) key[k++] = *p++;
			key[k] = 0;
			v = 0; while (q < e && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
			core_opt_store(key, v);
		}
		p = *e ? e + 1 : e;
	}
	s_corecfg_dirty = 0;
	if (core_opt_lookup("core", &v)) s_core_pref = v ? 1 : 0;
}
static void corecfg_save(void)
{
	static char buf[2048];
	int i, n = 0;
	for (i = 0; i < s_corecfg_n && n < (int)sizeof buf - 48; i++)
		n += snprintf(buf + n, sizeof buf - n, "%s=%d\n", s_corecfg[i].key, s_corecfg[i].val);
	gba_sd_write_named(&s_sd_vol, "/gba", "armgba-pico", "cfg", buf, (uint32_t)n);
}
/* the saved value of every option the core offers (its default where nothing is saved) */
static void core_apply_opts(void)
{
	int i, n, v;
	if (!g_core || g_core->version < GBA_CORE_ABI_V3 || !g_core->opt_count) return;
	n = g_core->opt_count();
	for (i = 0; i < n; i++) {
		const struct gba_core_option *o = g_core->opt_info(i);
		if (!o) continue;
		if (!core_opt_lookup(o->key, &v) || v < 0 || v >= o->nvalues) v = o->def;
		g_core->opt_set(i, v);
	}
}
volatile unsigned int g_dbg_core_switch_ms, g_dbg_core_is_sd;   /* oem core */
const struct gba_core_exports *gba_core_current(void) { return g_core; }
/* Load a GBA core from the card: the preferred one, else the other. Both use the same slot, so a
 * failed load may have overwritten what was there; the caller drops g_core then. gpSP needs the
 * card's BIOS (it has none of its own), armGBA uses its built-in BIOS when there is none. */
static const struct gba_core_exports *gba_core_load_pref(int want_armgba, int *got_armgba)
{
	const struct gba_core_exports *ex = 0;
	int i;
	for (i = 0; i < 2 && !ex; i++) {
		int armgba = (i == 0) ? want_armgba : !want_armgba;
		if (!armgba && !s_have_bios) continue;
		ex = gba_core_load_file(&s_sd_vol, armgba ? CORE_PATH_ARMGBA : CORE_PATH_GPSP);
		if (ex) *got_armgba = armgba;
		else GBA_LOG("gba: %s not loaded (%s)\n", armgba ? CORE_PATH_ARMGBA : CORE_PATH_GPSP,
			     core_file_error(g_gba_core_err));
	}
	return ex;
}

/* armGBA for the next GBA game: always without a BIOS (gpSP has none of its own) */
static int gba_want_armgba(void)
{
	if (!s_have_bios) return 1;
	if (g_dbg_core_force) return g_dbg_core_force == 1;
	return s_sdcore_present > 0 && s_core_pref == 0;
}
/* The core for the next GBA game, initialised for it; 0 when no GBA core loads. */
static unsigned char *gba_core_for_launch(void)
{
	unsigned t0 = gpt4_get_current_tick();
	int want_armgba;
	if (!s_corecfg_loaded) corecfg_load();
	if (s_sdcore_present < 0) { fat_file f; s_sdcore_present = (fat_open(&s_sd_vol, SDCORE_PATH, &f) == 0) ? 1 : 0; }
	want_armgba = gba_want_armgba();
	if (!g_core || want_armgba != s_core_is_sd) {
		int got = 0;
		g_core = gba_core_load_pref(want_armgba, &got);
		if (!g_core) return 0;
		s_core_is_sd = got;
		s_gpsp_dirty = 1;
	}
	if (s_core_is_sd || s_gpsp_dirty) {
		gba_core_init((void *)GBA_ARENA_PA, GBA_ARENA_SZ - GBA_DRV_RESERVE);
		s_gpsp_dirty = 0;
	}
	if (s_core_is_sd) core_apply_opts();
	g_gba_state_dir = (g_core->version >= GBA_CORE_ABI_V3 && g_core->state_dir) ? g_core->state_dir : "/states/gba";
	g_dbg_core_is_sd = (unsigned)s_core_is_sd;
	g_dbg_core_switch_ms = (gpt4_get_current_tick() - t0) / 13000u;
	return gba_core_rom_ptr();
}

/* Cold-start stage timings (ms since the SD gate started), read via `fastboot oem
 * diag` as the bt=... line. Pinpoints where the black-backlight time-to-first-BIOS-
 * frame goes (SD mount / bios read / rom list / core blob load / core init / core
 * start / first drawn frame). */
static unsigned int s_bt_t0;
volatile unsigned g_dbg_emu_abs_ms; volatile int g_dbg_emu_chime;   /* oem led: emu_thread start on the gpt4 clock, chime still playing */
volatile unsigned int g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list;
volatile int g_dbg_bt_retime = -2;   /* GammaOS panel timing: -2 not applied (stock), else the panel write timeouts */
volatile unsigned int g_dbg_bt_coreload, g_dbg_bt_coreinit, g_dbg_bt_start, g_dbg_bt_frame1;
#define BT_MS() ((gpt4_get_current_tick() - s_bt_t0) / 13000u)

/* The mounted SD volume, for the menu's per-ROM boxart loader; 0 if not in SD mode
 * (the plain-list/never-brick fallback then just shows placeholders). */
fat_vol *gba_sd_menu_vol(void) { return s_sd_mode ? &s_sd_vol : 0; }
/* First enumerated Genesis-family ROM (type >= GENESIS), for the headless bench (oem gen-bench).
 * Returns an opaque pointer (menu_fastboot.c has no sd_fat.h); NULL if none / not in SD mode. */
const void *gba_sd_first_genesis_rom(void)
{
	int i;
	if (!s_sd_mode) return 0;
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type >= GBA_CONSOLE_GENESIS) return &s_roms[i];
	return 0;
}
static int s_sel_rom = -1;          /* chosen ROM index (for save/state paths) */

#include "rom_select_nav.h"   /* rs_move / rs_scroll (host-tested nav math) */

/* Draw the ROM list to the panel and let the user pick one. D-pad moves, A plays,
 * B/AYA has no effect (there is nothing to go back to). Returns the chosen index. */
extern int gba_snes_menu_run(const gba_rom_entry *roms, int nrom, int start_sel);

static int gba_sd_rom_select(void)
{
	unsigned pitch, W, H;
	int sel = 0, top = 0, rows, i;
	int x, y0, rowh = 30;
	int fade = 15;   /* quick fade in from a full white canvas (~0.25s), GBA-style */
	if (s_nrom <= 0) return -1;

	/* Preferred: the real SNES-Classic-mini menu (assets from boot_b). Returns -2 if
	 * the SNES asset pack is not present -> fall through to the plain list so a
	 * boot_b without the pack still lets the user pick a game (never-brick). */
	{
		int r;
		sd_bg_wait();   /* fast path: the card listing may still be running */
		r = gba_snes_menu_run(s_roms, s_nrom, s_sel_rom >= 0 ? s_sel_rom : 0);
		while (r == -3)   /* a theme was picked on the Display page: the menu again, in that theme */
			r = gba_snes_menu_run(s_roms, s_nrom, s_sel_rom >= 0 ? s_sel_rom : 0);
		if (r >= 0) return r;
	}
	for (;;) {
		mtk_wdt_restart();   /* kick the 10s watchdog: idling on the menu must not reset the device */
		{ extern void ayaneo_joypad_poll(void); ayaneo_joypad_poll(); }   /* so the stick works in this fallback list too */
		unsigned k = menu_keys();
		unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
		if (k & MK_UP)   sel = rs_move(sel, s_nrom, 1, 0);
		if (k & MK_DOWN) sel = rs_move(sel, s_nrom, 0, 1);
		if (k & MK_A) return sel;
		rows = ((int)H - 120) / rowh; if (rows < 1) rows = 1;
		top = rs_scroll(top, sel, rows, s_nrom);
		x = 40; y0 = 80;
		ayaneo_fill(buf, pitch, 0, 0, (int)W, (int)H, 0xFF10141Cu);
		ayaneo_fill(buf, pitch, 0, 0, (int)W, 6, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, 28, 3, 0xFFFFFFFFu, "Select a GBA game");
		for (i = 0; i < rows && top + i < s_nrom; i++) {
			int idx = top + i, y = y0 + i * rowh, L = 0;
			unsigned int fg = 0xFFC8D0E0u;
			char dn[128];
			const char *nm = s_roms[idx].name;
			while (nm[L] && L < 127) { dn[L] = nm[L]; L++; }   /* strip a trailing .gba */
			dn[L] = 0;
			if (L >= 4 && dn[L-4] == '.' && (dn[L-3]|32) == 'g' && (dn[L-2]|32) == 'b' && (dn[L-1]|32) == 'a')
				dn[L-4] = 0;
			if (idx == sel) { ayaneo_fill(buf, pitch, x - 12, y - 4, (int)W - 2 * (x - 12), rowh, 0xFF5090F0u); fg = 0xFF102030u; }
			ayaneo_text(buf, pitch, x, y, 2, fg, dn);
		}
		ayaneo_text(buf, pitch, x, (int)H - 28, 2, 0xFF80E080u, "Up/Down: move    A: play");
		if (fade > 0) {			/* white -> menu fade over the first frames */
			ayaneo_fill_blend(buf, pitch, 0, 0, (int)W, (int)H, 0xFFFFFFFFu, (255 * fade) / 15);
			fade--;
		}
		ayaneo_canvas_present();
		thread_sleep(16);
	}
}

/* ---- save-state / .sav persistence to the SD, matched to the selected ROM ---- */
static int sd_mode_on(void) { return s_sd_mode && s_sel_rom >= 0 && s_sel_rom < s_nrom; }

/* SUSPEND state (load-on-boot / power-off / Close auto-save): a fixed ".sus" file, kept
 * separate from the user-selectable manual slots so auto-resume never clobbers a manual save
 * and vice-versa (mirrors the SNES "sus" vs st0..2 split). */
static const char *gba_sd_current_name(void)
{
	return (s_sd_mode && s_sel_rom >= 0 && s_sel_rom < s_nrom) ? s_roms[s_sel_rom].name : (const char *)0;
}

static int sd_state_write(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	if (!sz) return 0;
	gba_core_state_save(scratch);   /* raw gpSP state (no boot_b header needed) */
	return gba_sd_write_named(&s_sd_vol, g_gba_state_dir, s_roms[s_sel_rom].name, "sus", scratch, sz) == 0;
}

static int sd_state_read(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	uint32_t n = gba_sd_read_named(&s_sd_vol, g_gba_state_dir, s_roms[s_sel_rom].name, "sus", scratch, GBA_STATE_MAX);
	if (!sz || n != sz) return 0;   /* absent or size mismatch (version guard) */
	gba_core_state_load(scratch);
	return 1;
}

/* MANUAL state (Pico menu Save/Load State): the user-selected slot st0..st2. */
static int sd_manual_write(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	if (!sz) return 0;
	gba_core_state_save(scratch);
	return gba_sd_write_state(&s_sd_vol, s_roms[s_sel_rom].name, s_gba_slot, scratch, sz) == 0;
}

static int sd_manual_read(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	uint32_t n = gba_sd_load_state(&s_sd_vol, s_roms[s_sel_rom].name, s_gba_slot, scratch, GBA_STATE_MAX);
	if (!sz || n != sz) return 0;
	gba_core_state_load(scratch);
	return 1;
}

/* oem gbabench[:N]: the same N frames every time, to compare core builds and settings on the
 * device. Loads the game's save state slot 0 (the control scene, or the slot given; with no state
 * there the game is started afresh instead), runs N frames (audio muted,
 * pictures not shown) and puts the game back where it was. Results: the first 60 frames
 * (recompiling after the load), the average and worst of the rest, and a hash of the last picture
 * (equal across builds = the same emulation). The buffers sit in the menu canvas region, unused
 * while a game runs. */
volatile int g_dbg_corebench_slot;   /* the control scene: save state slot 0 unless oem gbabench names one */
volatile int g_dbg_corebench_skip;   /* K > 1: draw one frame in K, the others render skipped (as fast forward) */
const char *gba_bench_rom_name(void) { return s_roms[s_sel_rom].name; }
volatile int g_dbg_corebench_req;
volatile unsigned int g_dbg_corebench[6];   /* warm avg, steady avg, steady max, slot loaded, hash, frames */
static void gba_corebench(int n)
{
	unsigned sz = gba_core_state_size(), i, warm = 0, steady = 0, mx = 0;
	unsigned char *bench = (unsigned char *)0x54900000u, *cur = (unsigned char *)0x54A00000u;
	g_dbg_corebench[3] = 0;
	if (!sz || sz > (1024u * 1024u) || n < 61) { g_dbg_corebench[5] = 0; return; }
	int from_reset = gba_sd_load_state(&s_sd_vol, s_roms[s_sel_rom].name, g_dbg_corebench_slot, bench,
					   1024u * 1024u) != sz;
	gba_core_state_save(cur);
	if (from_reset) {
		/* no state in that slot for this game and core: start the game afresh instead, the same
		 * frames every time as well (no input, the same start) */
		reset_gba();
		s_cpu_restart_req = 1;
	} else {
		g_gba_load_light = 1; gba_core_state_load(bench); g_gba_load_light = 0;
		s_cpu_clean_boundary = 1; s_cpu_restart_req = 1;
	}
	g_gba_audio_suppress = 1;
	for (i = 0; i < (unsigned)n; i++) {
		unsigned t = gpt4_get_current_tick(), us, k = (unsigned)g_dbg_corebench_skip;
		gba_core_skip_render(k > 1u && (i % k) != k - 1u);
		run_one_frame();
		us = (gpt4_get_current_tick() - t) / 13u;
		if (i < 60u) warm += us;
		else { steady += us; if (us > mx) mx = us; }
		if ((i % 60u) == 59u) mtk_wdt_restart();
	}
	gba_core_skip_render(0);
	g_gba_audio_suppress = 0;
	g_dbg_corebench[4] = fnv1a(2166136261u, (const unsigned char *)gba_core_screen(), 240ul * 160ul * 2ul);
	g_gba_load_light = 1; gba_core_state_load(cur); g_gba_load_light = 0;
	s_cpu_clean_boundary = 1; s_cpu_restart_req = 1;
	g_dbg_corebench[0] = warm / 60u;
	g_dbg_corebench[1] = steady / ((unsigned)n - 60u);
	g_dbg_corebench[2] = mx;
	g_dbg_corebench[3] = from_reset ? 0u : (unsigned)g_dbg_corebench_slot + 1u;
	g_dbg_corebench[5] = (unsigned)n;
}

/* oem gamefile: one of the running game's files from the card, for a look on the PC:
 * st0..st9 (save state slots) and sus (the suspend state) from the core's state folder, sav
 * (the battery save). Returns the bytes read, 0 when absent; name gets "<folder>/<rom>". */
unsigned gba_dbg_game_file(const char *ext, unsigned char *dst, unsigned cap, char *name, int ncap)
{
	const char *dir;
	int i = 0, k;
	if (!s_sd_mode || s_sel_rom < 0 || s_sel_rom >= s_nrom || !ext) return 0;
	if (ext[0] == 'r' && ext[1] == 'o' && ext[2] == 'm') {
		/* romN: 1 MB piece N of the ROM as loaded in the core's buffer */
		unsigned piece = 0, size = s_roms[s_sel_rom].size, off, len;
		for (k = 3; ext[k] >= '0' && ext[k] <= '9'; k++) piece = piece * 10u + (unsigned)(ext[k] - '0');
		if (size > gba_core_rom_capacity()) size = gba_core_rom_capacity();
		off = piece << 20;
		if (off >= size) return 0;
		len = size - off; if (len > (1u << 20)) len = 1u << 20; if (len > cap) len = cap;
		memcpy(dst, gba_core_rom_ptr() + off, len);
		if (name && ncap > 12) { const char *t = "ROM in RAM"; for (k = 0; t[k]; k++) name[k] = t[k]; name[k] = 0; }
		return len;
	}
	dir = (ext[0] == 's' && ext[1] == 'a' && ext[2] == 'v') ? "/saves/gba" : g_gba_state_dir;
	if (name && ncap > 1) {
		for (k = 0; dir[k] && i + 1 < ncap; k++) name[i++] = dir[k];
		if (i + 1 < ncap) name[i++] = '/';
		for (k = 0; s_roms[s_sel_rom].name[k] && i + 1 < ncap; k++) name[i++] = s_roms[s_sel_rom].name[k];
		name[i] = 0;
	}
	return gba_sd_read_named(&s_sd_vol, dir, s_roms[s_sel_rom].name, ext, dst, cap);
}

static void sd_sav_write(void)
{
	gba_sd_write_sav(&s_sd_vol, s_roms[s_sel_rom].name,
			 gba_core_backup_ptr(), gba_core_backup_size());
}

/* Mirror the GammaOS Pico settings to the SD card whenever they change, so they
 * travel with the card and are read back at the next LK start. No-op if not in
 * the SD flow. */
/* Small whole-file reads and writes on the mounted card for the platform code (the black box,
 * ayaneo_blackbox.c); < 0 when no card is mounted or the file is not there. */
int ayaneo_card_read(const char *dir, const char *name, void *buf, unsigned int len)
{
	char path[128];
	unsigned int n = 0, k;
	fat_file f;
	if (!s_sd_mode)
		return -1;
	for (k = 0; dir[k] && n < sizeof path - 2; k++) path[n++] = dir[k];
	path[n++] = '/';
	for (k = 0; name[k] && n < sizeof path - 1; k++) path[n++] = name[k];
	path[n] = 0;
	if (fat_open(&s_sd_vol, path, &f) != 0)
		return -1;
	return (int)fat_read(&f, 0, buf, len < f.size ? len : f.size);
}
int ayaneo_card_write(const char *dir, const char *name, const void *buf, unsigned int len)
{
	if (!s_sd_mode)
		return -1;
	fat_wr_mkpath(&s_sd_vol, dir);
	return fat_wr_put(&s_sd_vol, dir, name, buf, len) == 0 ? 0 : -1;
}

/* ayaneo_settings_save (ayaneo_audio.c) hands the serialized settings here: the card is their only
 * store (/gammaos/settings.bin) */
int ayaneo_settings_store(const unsigned char *b, int n)
{
	if (!s_sd_mode)
		return -1;
	return gba_sd_settings_write(&s_sd_vol, b, n);
}

/* Settings persist off the game thread (to /gammaos/settings.bin, and the GBA core choice to its
 * own file). A volume tick or a Pico change used to write the settings synchronously on the
 * emulation thread (tens to hundreds of ms on a slow card) and the audio ring ran dry: the stutter
 * the user heard on every adjustment. Now
 * the change marks the settings dirty and a low priority writer thread does the storage work,
 * coalescing bursts (wheel spins) into one write; the block layer is serialised by the storage
 * mutex (sd_fat.c) and whole file writes by the fat_wr mutex, so the game may touch the card
 * at the same time. ayaneo_settings_persist_flush() waits for the writer before a power off. */
static volatile int s_persist_dirty, s_persist_busy;
static int s_persist_thread_up;
static event_t s_persist_ev;
volatile unsigned g_dbg_persist_us, g_dbg_persist_core_us, g_dbg_persist_n;   /* oem persist: the last settings write */
static int persist_thread(void *arg)
{
	(void)arg;
	for (;;) {
		event_wait(&s_persist_ev);
		thread_sleep(80);                        /* coalesce a burst of changes */
		s_persist_busy = 1;
		while (s_persist_dirty) {
			unsigned t0 = gpt4_get_current_tick(), t1;
			s_persist_dirty = 0;
			ayaneo_settings_save();   /* to the card (ayaneo_settings_store) */
			t1 = gpt4_get_current_tick();
			if (s_corecfg_dirty) { s_corecfg_dirty = 0; corecfg_save(); }
			g_dbg_persist_us = (t1 - t0) / 13u;   /* wall time: the low priority writer waits for idle CPU */
			g_dbg_persist_core_us = (gpt4_get_current_tick() - t1) / 13u;
			g_dbg_persist_n++;
		}
		s_persist_busy = 0;
	}
	return 0;
}
void ayaneo_menu_settings_persist(void)
{
	s_persist_dirty = 1;
	if (!s_persist_thread_up) {
		thread_t *t;
		event_init(&s_persist_ev, false, EVENT_FLAG_AUTOUNSIGNAL);
		t = thread_create("ayaneo_settings", &persist_thread, NULL, LOW_PRIORITY, 16384);
		if (!t) { s_persist_dirty = 0; ayaneo_settings_save(); return; }
		thread_resume(t);
		s_persist_thread_up = 1;
	}
	event_signal(&s_persist_ev, false);
}
void ayaneo_settings_persist_flush(void)
{
	int i;
	for (i = 0; i < 300 && (s_persist_dirty || s_persist_busy); i++)
		thread_sleep(10);
}

/* Build a minimal "logo cart" in the gpSP ROM buffer for the BIOS boot-logo
 * intro. The GBA BIOS renders its logo from the CART HEADER's Nintendo-logo
 * bytes and then boots the cart, so an empty cart makes the BIOS jump into
 * unmapped ROM and the dynarec faults (data abort in translate_block_arm).
 * Instead: fill the whole buffer with ARM "b ." (0xEAFFFFFE) so any execution in
 * cart space loops harmlessly, then overlay a REAL header (Nintendo logo + valid
 * checksum + entry, which branches to 0xC0 = a loop) copied from the first ROM on
 * the card so the authentic logo shows. No prebaked data. Returns the cart size. */
/* The first GBA ROM's cart header (its first 0xC0 bytes: the Nintendo logo the BIOS draws), kept
 * on the card in /gammaos/cart_header.bin so the intro can start right after the card mount
 * instead of after the ROM listing; the listing, on its own thread during the intro, writes it
 * again when the games on the card change. */
#define CART_HDR_DIR  "/gammaos"
#define CART_HDR_NAME "cart_header.bin"
static unsigned char s_cart_hdr_cache[512] __attribute__((aligned(64)));
static int s_cart_hdr_valid;
static volatile int s_sd_bg_state;   /* the listing thread: 0 running, 1 done */
static int s_sd_fast;                /* the listing runs on its own thread during the intro */
static volatile int s_sd_bg_pending; /* start the listing thread once the core is loaded */
static int sd_bg_thread(void *arg);
static void sd_bg_launch(void)
{
	thread_t *t;
	if (!s_sd_bg_pending) return;
	s_sd_bg_pending = 0;
	t = thread_create("gba_sdbg", &sd_bg_thread, NULL, LOW_PRIORITY, 32768);
	if (t) thread_resume(t); else sd_bg_thread(NULL);
}
static int cart_header_read(fat_vol *v)
{
	fat_file f;
	s_cart_hdr_valid = fat_open(v, CART_HDR_DIR "/" CART_HDR_NAME, &f) == 0 && f.size == 0xC0u &&
			   fat_read(&f, 0, s_cart_hdr_cache, 0xC0u) == 0xC0u;
	return s_cart_hdr_valid;
}
/* the first GBA ROM's cart header from the card (0xC0 bytes), 0 if there is none */
static int logo_header_from_card(unsigned char *dst)
{
	int i, hdr_rom = -1;
	for (i = 0; i < s_nrom; i++)
		if (s_roms[i].type == GBA_CONSOLE_GBA) { hdr_rom = i; break; }
	if (hdr_rom < 0 && s_nrom > 0) hdr_rom = 0;
	if (hdr_rom < 0) return 0;
	{
		fat_file f;
		f.v = &s_sd_vol;
		f.first_clus = s_roms[hdr_rom].first_clus;
		f.size = s_roms[hdr_rom].size;
		f.ex_nofat = s_roms[hdr_rom].ex_nofat;
		return fat_read(&f, 0, dst, 0xC0u) == 0xC0u;
	}
}
/* after a listing: write the header file when it is missing or the first GBA game changed */
static void cart_header_refresh(void)
{
	static unsigned char hdr[0xC0] __attribute__((aligned(64)));
	if (!logo_header_from_card(hdr)) return;
	if (s_cart_hdr_valid && memcmp(hdr, s_cart_hdr_cache, 0xC0u) == 0) return;
	fat_wr_mkpath(&s_sd_vol, CART_HDR_DIR);
	if (fat_wr_put(&s_sd_vol, CART_HDR_DIR, CART_HDR_NAME, hdr, 0xC0u) == 0) {
		memcpy(s_cart_hdr_cache, hdr, 0xC0u);
		s_cart_hdr_valid = 1;
	}
}
/* the ROM listing, on its own thread while the intro plays (the gate has mounted the card) */
static int sd_bg_thread(void *arg)
{
	int total = 0;
	(void)arg;
	gba_sd_make_rom_dirs(&s_sd_vol);
	s_nrom = gba_sd_list_roms(&s_sd_vol, s_roms, 128, &total);
	g_dbg_bt_list = BT_MS();
	cart_header_refresh();
	s_sd_bg_state = 1;
	return 0;
}
static void sd_bg_wait(void)
{
	if (!s_sd_fast) return;
	while (s_sd_bg_state == 0) thread_sleep(5);
}

static unsigned gba_sd_build_logo_cart(unsigned char *rp)
{
	unsigned *w = (unsigned *)rp;
	unsigned n = 0x8000u / 4u, j;
	int hdr_rom = -1;
	for (j = 0; j < n; j++)
		w[j] = 0xEAFFFFFEu;			/* b . everywhere = safe self-loop */
	/* The header must come from a GBA cart: only .gba ROMs carry the GBA BIOS
	 * Nintendo-logo bytes at 0x04-0x9F. Since the roster now merges /roms/gb,
	 * /roms/gbc and /roms/gba (sorted), s_roms[0] may be a GB/GBC ROM whose header
	 * has no GBA logo - copying it makes the BIOS show no cartridge logo. Pick the
	 * first GBA ROM instead (fall back to s_roms[0] only if there is no GBA ROM). */
	{
		int i;
		for (i = 0; i < s_nrom; i++)
			if (s_roms[i].type == GBA_CONSOLE_GBA) { hdr_rom = i; break; }
		if (hdr_rom < 0 && s_nrom > 0) hdr_rom = 0;
	}
	if (s_cart_hdr_valid) {
		memcpy(rp, s_cart_hdr_cache, 0xC0u);   /* the card's header file (the listing may still be running) */
		return 0x8000u;
	}
	if (hdr_rom >= 0) {
		fat_file f;
		f.v = &s_sd_vol;
		f.first_clus = s_roms[hdr_rom].first_clus;
		f.size = s_roms[hdr_rom].size;
		f.ex_nofat = s_roms[hdr_rom].ex_nofat;
		/* 0..0xC0 = real header: logo(0x04-0x9F) + checksum(0xBD) + entry(0x00,
		 * which branches to 0x080000C0 -> our loop). Restores the loops beyond. */
		fat_read(&f, 0, rp, 0xC0u);
	}
	return 0x8000u;
}
#endif

/* The BIOS intro on black ("BIOS Intro Background: Black"). The BIOS draws its logo over a white
 * screen and fades by blending toward white, so each pixel is a colour mixed with white, plus dark
 * ink (armGBA's "Open BIOS" lettering). Per pixel, on a 0..63 scale: the smallest channel is the
 * white in it and comes off, the room above the largest channel is the dark ink and is added back
 * as light, and the colour left over is raised by 99/64 (1.55) so the logo's blue and magenta,
 * themselves 64% colour over white, come out as bright as on the white screen. White becomes black,
 * black ink white, a fade from white a fade from black. Written to its own buffer: the core's
 * screen stays the core's. */
static unsigned short s_intro_black_px[240 * 160];
static const unsigned short *intro_on_black(const unsigned short *src)
{
	unsigned i;
	for (i = 0; i < 240u * 160u; i++) {
		unsigned p = src[i];
		int r = (int)(((p >> 11) & 31u) << 1 | ((p >> 15) & 1u));   /* 5 bit channels widened to 6 */
		int g = (int)((p >> 5) & 63u);
		int b = (int)((p & 31u) << 1 | ((p >> 4) & 1u));
		int m = r < g ? (r < b ? r : b) : (g < b ? g : b);
		int M = r > g ? (r > b ? r : b) : (g > b ? g : b);
		int ink = 63 - M;
		r = ((r - m) * 99 >> 6) + ink; if (r > 63) r = 63;
		g = ((g - m) * 99 >> 6) + ink; if (g > 63) g = 63;
		b = ((b - m) * 99 >> 6) + ink; if (b > 63) b = 63;
		s_intro_black_px[i] = (unsigned short)((r >> 1) << 11 | g << 5 | (b >> 1));
	}
	return s_intro_black_px;
}

/* ===================== emulator thread ===================== */
volatile int g_dbg_boot_step;   /* oem mode: how far the emulator thread got */
volatile unsigned int g_dbg_intro_aw_wait_ms;   /* oem diag intro: how long the intro waited for the speaker amps */
static int emu_thread(void *arg)
{
	unsigned char *scratch = (unsigned char *)(GBA_ARENA_PA + GBA_ARENA_SZ - GBA_DRV_RESERVE);
	unsigned romsz, frame = 0;

	(void)arg;

	g_dbg_boot_step = 1;
	{ extern void ayaneo_boot_stamp(int i); ayaneo_boot_stamp(5); }   /* oem boottime: emulator thread */
	gba_disable_align_faults();		/* gpSP relies on unaligned host accesses */
	ayaneo_display_prepare();		/* own the panel + backlight for on-screen debug */
	g_dbg_boot_step = 2;
	g_dbg_emu_abs_ms = gpt4_get_current_tick() / 13000u;
	g_dbg_emu_chime = ayaneo_boot_audio_active();
#ifdef KONKR_KPA
	{ extern unsigned int ayaneo_rewind_map(void); ayaneo_rewind_map(); }   /* islands mapped now, so oem rewindmap works from the menu */
	{	/* the panel runs at twice the GBA rate from here (the LCM rests it at 59.7275 Hz) */
		extern void ayaneo_dsi_set_vfp(unsigned int vfp); extern unsigned int ayaneo_dsi_default_vfp(void);
		ayaneo_dsi_set_vfp(ayaneo_dsi_default_vfp());
	}
	/* the speaker amps lock to the I2S clocks and load their DSP image: start that now, under
	 * the core load and the card mount, so sound is there from the first BIOS frame (the ring
	 * is silence until a core feeds it); a boot chime still playing keeps the usual order */
	g_dbg_boot_step = 3;
	if (!ayaneo_boot_audio_active())
		ayaneo_gbc_audio_init();
#endif
	g_dbg_boot_step = 4;

	/* The boot intro runs on armGBA whenever the card has it, whatever core the GBA games are set
	 * to: the card's BIOS when there is a valid one, else armGBA's own Open BIOS splash. Without
	 * armGBA it runs on gpSP when the card has gpSP and a BIOS (gpSP has none of its own). With
	 * neither there is no intro and no GBA core is loaded here: the menu runs on its own. */
	if (!s_corecfg_loaded) corecfg_load();
	g_core = gba_core_load_file(&s_sd_vol, SDCORE_PATH);
	s_sdcore_present = g_core ? 1 : 0;
	if (!g_core) {
		GBA_LOG("gba: %s not loaded (%s)\n", SDCORE_PATH, core_file_error(g_gba_core_err));
		if (s_have_bios) {
			g_core = gba_core_load_file(&s_sd_vol, CORE_PATH_GPSP);
			if (!g_core)
				GBA_LOG("gba: %s not loaded (%s): no intro\n", CORE_PATH_GPSP, core_file_error(g_gba_core_err));
		}
	}
	s_core_is_sd = s_sdcore_present;
	g_dbg_core_is_sd = (unsigned)s_core_is_sd;
	g_dbg_bt_coreload = BT_MS();
	g_dbg_boot_step = 5;
	{ extern void ayaneo_boot_stamp(int i); ayaneo_boot_stamp(6); }   /* oem boottime: core loaded */

#ifdef AYANEO_GBA_INTERP
	dynarec_enable = 0;			/* diagnostic: pure interpreter, no JIT */
	gba_dbg("GBA 1: emu start (INTERP, no dynarec)");
#else
	gba_dbg("GBA 1: emu thread start");
#endif

	if (g_core && gba_core_init((void *)GBA_ARENA_PA, GBA_ARENA_SZ - GBA_DRV_RESERVE) != 0) {
		gba_dbg("GBA ERR: arena too small");
		g_core = 0;   /* no intro; the first GBA game loads its core again */
	}
	g_dbg_bt_coreinit = BT_MS();
	g_dbg_boot_step = 6;
	sd_bg_launch();   /* fast boot path: the card work starts now, during the intro */
#ifdef AYANEO_GBA_SD
	if (s_sd_mode) {
		input_init();                        /* need the D-pad/A for the ROM-select */
		/* (the settings came from the card in the gate, before the intro) */
		/* Boot the emulator with NO cartridge, using the SD card's own
		 * gba_bios.bin (s_sd_bios), so the real GBA BIOS plays its boot logo as
		 * the intro. The ROM-select menu and the chosen game are driven after the
		 * display + CPU thread are up (see the s_sd_mode intro block just before
		 * the main frame loop). No prebaked BIOS: if gba_bios.bin was missing,
		 * ayaneo_gba_sd_boot already fell through to a normal boot. */
		romsz = 0;
		if (g_core) {
			romsz = gba_sd_build_logo_cart(gba_core_rom_ptr());	/* real logo header + safe self-loops */
			if (gba_core_start(romsz, s_have_bios ? s_sd_bios : 0) != 0) {
				gba_dbg("GBA ERR: SD core_start failed");
				g_core = 0;   /* no intro; the first GBA game loads its core again */
			} else {
				gba_core_enter_bios();	/* start at the BIOS so its logo plays */
			}
		}
		g_dbg_bt_start = BT_MS();
	} else
#endif
	{
		gba_dbg("GBA 2: core_init ok, loading ROM");
		romsz = load_rom();
		if (!romsz) {
			gba_dbg("GBA ERR: ROM load/decompress failed");
			return 0;
		}
		gba_dbg("GBA 3: ROM decompressed, core_start");
#ifdef AYANEO_GBA_SD
		if (gba_core_start(romsz, s_sd_bios) != 0) {   /* dead path (s_sd_mode always 1) */
#else
		if (gba_core_start(romsz, gba_bios_data) != 0) {
#endif
			gba_dbg("GBA ERR: core_start failed");
			return 0;
		}
	}
	gba_dbg("GBA 4: reset_gba ok");

#ifdef AYANEO_GBA_SD
	if (!s_sd_mode)   /* SD mode already inited input before the ROM-select */
#endif
	input_init();
	ayaneo_settings_load();
#ifdef AYANEO_GBA_SD
	if (!s_sd_mode)
#endif
	{
		sav_load(scratch);		/* inject cartridge battery save (boot_b) */
		try_load_state(scratch);	/* resume unless Start held */
	}
	s_ready = 1;

	/* Non-SD: disable the watchdog as before. SD flow: KEEP it armed through the
	 * BIOS intro + ROM-select + the mid-run reset into the game (the main frame
	 * loop kicks it every frame, and sd_read/sd_write kick during loads/saves), so
	 * if the game-start reset ever hangs the watchdog recovers to a normal boot
	 * instead of a dead screen. */
#ifdef AYANEO_GBA_SD
	if (!s_sd_mode)
#endif
	mtk_wdt_disable();

	/* Clock to match the persisted Preemptive Frames tier (Off = low-power 600 MHz,
	 * Balanced/Responsive/Max escalate for the deeper look-ahead). */
	preempt_apply_cpu(ayaneo_get_preempt_frames());

	/* start the CPU thread (blocks on ev_cpu until the first frame kick) */
	event_init(&ev_cpu, false, EVENT_FLAG_AUTOUNSIGNAL);
	event_init(&ev_main, false, EVENT_FLAG_AUTOUNSIGNAL);
	s_cpu_thread = thread_create("gba_cpu", &cpu_thread_fn, NULL, HIGH_PRIORITY, 65536);
	if (s_cpu_thread)
		thread_resume(s_cpu_thread);
	gba_dbg("GBA 5: cpu thread created");

	/* let the boot chime finish before we take the codec */
	{
		int g = 0;
		while (ayaneo_boot_audio_active() && g++ < 300) {
			thread_sleep(20);
			mtk_wdt_restart();	/* SD flow keeps the wdt armed here; feed it */
		}
	}
	ayaneo_gbc_audio_init();
	{
		/* show the measured panel refresh (returned as Hz*100) so we can confirm
		 * the retune to ~5973 = 59.73 Hz for the vsync-locked pacing */
		extern int primary_display_get_vsync_interval(void);
		char m[48], *p;
#if defined(KONKR_KPA)
		/* the dpmgr vsync wait returns at once on this board (the DSI frame events never
		 * fire in video mode), so the timed interval is garbage and seeded the resampler
		 * with a wrong pitch for the BIOS intro: take the rate the porch produces instead */
		extern unsigned int ayaneo_dsi_core_refresh_milli(void);
		int hz100 = (int)(ayaneo_dsi_core_refresh_milli() / 10u);
#else
		int hz100 = primary_display_get_vsync_interval();
#endif
		s_panel_hz100 = hz100;
		p = mi_puts(m, "GBA 6: panel Hz*100 = ");
		p = mi_putu(p, (unsigned)(hz100 < 0 ? 0 : hz100));
		*p = 0;
		gba_dbg(m);
		/* calibrate the audio resampler to the real panel rate: fixed rate =
		 * constant pitch, no dynamic wobble */
		if (hz100 > 0) {
			extern void ayaneo_gba_audio_set_rate(int panel_hz100);
			ayaneo_gba_audio_set_rate(hz100);
		}
	}

	{
		/*
		 * Frame pacing = the panel's own vsync. ayaneo_gbc_show_frame() ->
		 * ayaneo_present() -> primary_display_config_input() BLOCKS on FRAME_DONE
		 * (the panel scan-out boundary) and, because show_frame alternates the
		 * scan-out buffer each call, always forces a re-latch - so presenting one
		 * frame per call locks the emulator exactly to the panel refresh. That is
		 * inherently smooth (no tearing, no beat). We do NOT add a second software
		 * pacer (a 13 MHz busy-wait on top of the vsync block just beat against it
		 * -> the ~1 s micro-judder). The tiny GBA(59.73)-vs-panel(~60) rate
		 * difference is absorbed by the audio write-cursor resync in
		 * ayaneo_gba_audio_submit(); the emulator is ~300 fps capable so a frame is
		 * always ready before each vsync.
		 */
		int ff_prev = 0;

#ifdef AYANEO_GBA_SD
		/* SD flow: the core is currently running the SD card's BIOS (no cart), so
		 * present the BIOS boot logo as the intro, then drop into the ROM-select
		 * menu, then reset the core into the chosen game. Honour skip-boot-logo
		 * (SD setting); the user can also cut the logo short by holding B. */
		if (s_sd_mode) {
			extern int ayaneo_resume_pending(void);
			extern volatile int g_dbg_resume[4];
			int pend = ayaneo_resume_pending();
			/* the intro plays on the core loaded above (armGBA with the card's BIOS or its own
			 * splash, or gpSP with the card's BIOS); none: straight to the menu or the resumed game */
			int fi, intro_frames = (ayaneo_get_skip_gba_intro() || pend || !g_core) ? 0 : GBA_SD_INTRO_FRAMES;
			g_dbg_resume[0] = pend;
			g_dbg_boot_step = 7;
			/* Run the BIOS-logo intro on the pure INTERPRETER: the dynarec faults
			 * in translate_block_arm here, and the interpreter also naturally picks
			 * up the reset PC after the intro->game gba_core_start (it re-reads PC
			 * each step, no translated blocks to go stale). The dynarec is switched
			 * back on for the actual game below. */
			if (g_core) dynarec_enable = 0;
			ayaneo_gbc_blank();			/* black edges outside the cropped logo */
			/* "Mute BIOS Audio" also silences the GBA BIOS boot-logo jingle (the
			 * emulated BIOS runs through the game audio path here). Pause across the
			 * intro, then unpause so the menu/game audio is restored. */
			if (ayaneo_get_mute_bios()) ayaneo_gbc_audio_pause(1);
#ifdef KONKR_KPA
			/* The speaker amps (AW88166, their own thread from the top of emu_thread) take
			 * ~0.5 s to load and unmute; the intro now starts ~1.1 s after power on, before
			 * they are up, and the start of the BIOS jingle was lost. Wait for them when the
			 * jingle is going to be heard, sleeping so their thread gets the CPU; capped so a
			 * failed amp never holds the boot. (A boot stall at an early card read used to
			 * hide this until 2e2a62c removed that read.) */
			if (intro_frames > 0 && !ayaneo_get_mute_bios()) {
				extern int ayaneo_aw88166_started(void);
				unsigned w0 = gpt4_get_current_tick();
				while (!ayaneo_aw88166_started() && (gpt4_get_current_tick() - w0) < 800u * 13000u)
					thread_sleep(5);
				g_dbg_intro_aw_wait_ms = (gpt4_get_current_tick() - w0) / 13000u;
			}
#endif
			{ extern void ayaneo_hud_hide(int on); ayaneo_hud_hide(1); }   /* no FPS badge over the BIOS logo */
			int intro_vol = 0;
			int intro_black = ayaneo_get_intro_black();
			for (fi = 0; fi < intro_frames; fi++) {
				update_buttons();
				{	/* the volume wheel works during the intro too, in the game's steps, with no
					 * OSD over the logo; saved once the intro ends */
					int dir = ayaneo_volume_dir();
					if (dir) {
						ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5);
						intro_vol = 1;
					}
				}
				run_one_frame();
				ayaneo_gba_show_intro_frame(intro_black ? intro_on_black(gba_core_screen())
								      : gba_core_screen());	/* 6x fill-height */
				if (!g_dbg_bt_frame1) g_dbg_bt_frame1 = BT_MS();  /* first BIOS frame on screen */
				{ extern void ayaneo_boot_stamp(int i); ayaneo_boot_stamp(7); }
				mtk_wdt_restart();		/* keep the armed watchdog fed during the intro */
				if (PRESSED(GPIO_B)) break;	/* hold B to skip the logo */
			}
			if (ayaneo_get_mute_bios()) ayaneo_gbc_audio_pause(0);
			if (intro_vol)
				ayaneo_menu_settings_persist();   /* the volume, written off this thread */
			{ extern void ayaneo_hud_hide(int on); extern void ayaneo_fps_reset(void); ayaneo_hud_hide(0); ayaneo_fps_reset(); }
			{	/* intro wall time (oem diag): 270 frames at 59.73 Hz = 4520 ms when paced right */
				extern volatile unsigned int g_dbg_bt_intro_end, g_dbg_bt_intro_frames;
				g_dbg_bt_intro_end = BT_MS(); g_dbg_bt_intro_frames = (unsigned int)fi;
			}
			/* The card thread may still be listing the games: the intro used to cover it, but
			 * a resumed game skips the intro, and with the list still empty this went to the
			 * no ROMs branch and ran the bare BIOS for ever (the resume bug). */
			g_dbg_boot_step = 8;
			sd_bg_wait();
			g_dbg_boot_step = 9;
			if (s_nrom > 0) {
				unsigned char *rp;
				for (;;) {
					int sel = gba_sd_rom_select();
					s_sel_rom = sel;
					if (s_roms[sel].type == GBA_CONSOLE_SNES) {
						snes_sd_session(&s_sd_vol, &s_roms[sel]);  /* SNES via snes9x */
						s_gpsp_dirty = 1;
						continue;
					}
					if (s_roms[sel].type >= GBA_CONSOLE_GENESIS) {
						genesis_sd_session(&s_sd_vol, &s_roms[sel]);  /* Sega MD/SMS/GG/SG via GPGX */
						s_gpsp_dirty = 1;
						continue;
					}
					if (s_roms[sel].type != GBA_CONSOLE_GBA) {
						gbc_sd_session(&s_sd_vol, &s_roms[sel]);   /* GB/GBC via gambatte */
						s_gpsp_dirty = 1;                          /* arena clobbered */
						continue;                                  /* back to the selector */
					}
					rp = gba_core_for_launch();   /* armGBA or gpSP from the card; rebuilds the arena */
					if (!rp) { gba_dbg("GBA ERR: no GBA core loads from the card"); continue; }
					romsz = gba_sd_load_rom(&s_sd_vol, &s_roms[sel], rp, gba_core_rom_capacity());
					if (romsz) break;
					gba_dbg("GBA ERR: SD ROM load failed, back to selector");
				}
				/* reset the running core into the selected game (SD BIOS + cart).
				 * reset_gba flushes the dynarec, so we must NOT let execute_arm
				 * resume in place; request a clean restart (see gba_yield_to_main)
				 * which the first main-loop frame below triggers. */
				if (gba_core_start(romsz, s_have_bios ? s_sd_bios : 0) != 0) {
					gba_dbg("GBA ERR: SD game core_start failed");
					return 0;
				}
				s_game_live = 1;
				gba_sd_load_sav(&s_sd_vol, s_roms[s_sel_rom].name,
						(unsigned char *)gba_core_backup_ptr(), gba_core_backup_size());
				if (!PRESSED(GPIO_B))
					state_read(scratch);
				/* Do NOT blank here: it blacks the live frozen-menu snapshot = a black
				 * flash before the opening punch. The punch composites the full frame
				 * (menu snapshot + game) then clears the letterbox, so the frozen menu
				 * stays on screen right up to the seamless growing-circle opening. */
				dynarec_enable = 1;	/* full-speed dynarec for the actual game */
				s_cpu_restart_req = 1;	/* CPU thread re-enters gba_core_cpu_loop cleanly */
			}
			/* no ROMs on the card: keep showing the BIOS (romsz stays 0); with no GBA core either
			 * there is nothing to run, so wait here with the watchdog fed */
			if (s_nrom <= 0 && !g_core) {
				GBA_LOG("gba-sd: no games and no GBA core on the card\n");
				for (;;) { mtk_wdt_restart(); thread_sleep(500); }
			}
		}
#endif

		unsigned punch_start = 0;	/* 13 MHz tick the punch-hole began (0 = not yet) */
		int rw_acc = 0;			/* rewind fractional-speed accumulator (256ths), reset on entry */
		gba_rewind_arm();		/* size + clear the rewind ring for this game */
		for (;;) {
			int uncapped;
			{ static unsigned last; unsigned now = gpt4_get_current_tick(); if (last) g_dbg_loop_us = (now - last) / 13u; last = now; }
			lp_mark(0);

			/* precise IN-GAME panel refresh (vsync-locked show_frame): average the loop
			 * period over 128 frames -> Hz*1000. The boot-time primary_display_get_
			 * vsync_interval() only samples 2 vsync intervals (IRQ jitter ~+-0.1 Hz), so
			 * s_panel_hz100 (shown in the GammaOS menu AND used to calibrate the audio
			 * resampler) is imprecise. Recalibrate BOTH from this 128-frame average the
			 * first time it is valid, so audio locks to the TRUE panel rate and the menu
			 * reports it correctly. */
			{
				extern volatile unsigned int g_dbg_hz1000;
				extern void ayaneo_gba_audio_set_rate_milli(int hz1000);
				static unsigned int g_acc, g_last, g_prevhz; static int g_cnt, g_recal;
				unsigned int now = gpt4_get_current_tick();
				unsigned int lt = g_last ? (now - g_last) : 0u;   /* period in 13 MHz ticks */
				g_last = now;
				/* only frames paced by the panel count: with the Pico menu open, fast forward or
				 * the benchmark the loop is not vsync-locked and its period says nothing about
				 * the panel (the menu's own row read 59.4 to 60.1 Hz while it was open). Drop the
				 * partial window too, so it restarts clean once pacing resumes. */
				if (s_menu_open || s_fast_forward || s_benchmark) {
					g_last = 0; g_acc = 0; g_cnt = 0; lt = 0;
				}
				if (lt > 104000u && lt < 300000u) {   /* 8..23 ms; reject doubled frames */
					unsigned int hz = 0;
#if defined(KONKR_KPA)
					/* The Konkr's pacer counts the refreshes between the frame ends it measures
					 * and stamps the last one: refreshes over their span is the exact rate. The
					 * loop period is not: a late frame splits into a long period (rejected) and a
					 * short one (kept), which read up to 60.1 Hz on a 59.727 Hz panel. A window
					 * with a timed out edge wait is dropped. */
					extern volatile unsigned int g_dbg_pace_refreshes, g_dbg_pace_refresh_tick, g_dbg_pace_timeouts;
					static unsigned int w_ref, w_tick, w_to;
					if (g_cnt == 0) { w_ref = g_dbg_pace_refreshes; w_tick = g_dbg_pace_refresh_tick; w_to = g_dbg_pace_timeouts; }
					if (++g_cnt >= 64) {   /* ~1.07 s */
						unsigned int n = g_dbg_pace_refreshes - w_ref, dt = g_dbg_pace_refresh_tick - w_tick;
						if (n && dt && g_dbg_pace_timeouts == w_to)
							hz = (unsigned int)(13000000000ULL * n / dt);   /* 13 MHz ticks -> milli-Hz */
						g_cnt = 0;
					}
#else
					g_acc += lt;
					if (++g_cnt >= 64) {   /* 64-frame window (~1.07s): averages out per-frame
					                         emulation CPU jitter so the readout tracks the TRUE
					                         fixed panel rate as steadily as the menu loop. */
						hz = (unsigned int)(832000000000LL / (long long)g_acc);
						g_acc = 0; g_cnt = 0;
					}
#endif
					if (hz) {
						g_dbg_hz1000 = hz;
						/* Recalibrate the audio resampler to the TRUE panel rate, but ONLY
						 * from a STABLE reading: two consecutive 64-frame windows within
						 * 0.05 Hz and in the realistic panel band. This rejects the boot /
						 * dynarec-warmup windows (irregular frame timing) that would
						 * otherwise mis-seed the resampler and leave the recovery loop
						 * hunting = the audible stretching. Precise (milli-Hz), one-shot. */
						if (!g_recal && hz >= 58000u && hz <= 61000u && g_prevhz &&
						    (hz > g_prevhz ? hz - g_prevhz : g_prevhz - hz) <= 50u) {
							g_recal = 1;
							s_panel_hz100 = (int)(hz / 10u);
							ayaneo_gba_audio_set_rate_milli((int)hz);
						}
						g_prevhz = hz;
					}
				}
			}

			{ extern void ayaneo_frame_delay(void); ayaneo_frame_delay(); }   /* read the buttons as late as the frame can still make its refresh */
			{ extern void ayaneo_joypad_poll(void); ayaneo_joypad_poll(); }  /* once/frame: cache stick+triggers (non-blocking) */
			update_buttons();

			/* Rewind (left trigger): instead of advancing the game, walk the ring of periodic
			 * snapshots backward and re-render. Press depth sets a smooth 1x..6x speed (see
			 * GBA_REWIND_MAX_SPD). Each stepped-back frame is rendered (newest-first) and its audio is
			 * captured (g_gba_audio_capture routes gba_audio_cb into g_gba_cap instead of the sink),
			 * then REVERSED + decimated by `steps` and submitted so you hear the game backwards, with
			 * `steps` frames compressed into one present (correct 1x..6x pitch, no ring overrun). Only
			 * the last (oldest) frame's video is presented. State-load flushes the dynarec, so each
			 * render re-enters the CPU thread cleanly (clean boundary + restart) like the run-ahead
			 * rewind. Menu-gated. On release the ring commits the rewound point as the new head. */
			{
				extern int ayaneo_joypad_rewind_level(void);
				extern void ayaneo_audio_reverse_flip(void);
				extern void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames);
				extern void ayaneo_gba_rw_capture_begin(void);
				extern unsigned ayaneo_gba_rw_capture_end(const short **buf);
				static short s_gba_audrev[4096 * 2];   /* reversed+decimated audio scratch */
				int rw = (!s_menu_open) ? ayaneo_joypad_rewind_level() : 0;
				if (rw > 0 && ayaneo_rewind_ready() && ayaneo_rewind_count() > 0) {
					int spd = 256 + (rw * (GBA_REWIND_MAX_SPD - 256)) / 255;   /* 256(1x)..MAX_SPD(6x) */
					unsigned sz; const void *st; int k, steps, rendered = 0;
					ayaneo_hud_set(2, spd * 10 / 256);   /* show reverse speed, e.g. "6.0x" */
					if (!ayaneo_rewind_active()) { ayaneo_rewind_begin(); rw_acc = 0; ayaneo_audio_reverse_flip(); }
					rw_acc += spd;
					steps = rw_acc >> 8; rw_acc &= 255;         /* whole snapshots to step this present (>=1) */
					for (k = 0; k < steps; k++) {
						const short *cap; unsigned fr, i, j = 0;
						int atold = (ayaneo_rewind_step() != 0);
						if (k > 0 && atold) break;          /* already rendered >=1 and now at oldest */
						st = ayaneo_rewind_cur(&sz);
						if (!st || !sz) break;
						g_gba_load_light = 1;
						gba_core_state_load((void *)st);
						gba_sound_ring_load((unsigned char *)st + s_gba_rw_snd_off);
						g_gba_load_light = 0;
						s_cpu_clean_boundary = 1;   /* re-enter as a FULL frame */
						s_cpu_restart_req = 1;      /* the render run_one_frame re-enters cleanly */
						ayaneo_gba_rw_capture_begin();
						run_one_frame();            /* render video + capture this frame's audio */
						fr = ayaneo_gba_rw_capture_end(&cap);
						if (fr > 4096u) fr = 4096u;
						for (i = 0; i < fr; i += (unsigned)steps) {   /* reverse (+ decimate) */
							s_gba_audrev[j * 2]     = cap[(fr - 1u - i) * 2];
							s_gba_audrev[j * 2 + 1] = cap[(fr - 1u - i) * 2 + 1];
							j++;
						}
						ayaneo_gba_audio_submit(s_gba_audrev, j);   /* play this frame backwards */
						rendered = 1;
						if (atold) break;                   /* rendered the oldest: hold */
					}
					if (rendered) {
						ayaneo_present_skip_framedone = 0;
						ayaneo_gbc_show_frame(gba_core_screen());   /* present the oldest video */
						mtk_wdt_restart();
						continue;
					}
					/* nothing valid to render (should not happen with count>0): abandon rewind. */
					ayaneo_rewind_end(); ayaneo_audio_reverse_flip();
				} else if (ayaneo_rewind_active()) {
					ayaneo_rewind_end(); ayaneo_audio_reverse_flip();   /* released: resume forward */
				}
			}

			{	/* apply the per-tier CPU clock when the Preemptive Frames tier
				 * changes (covers the Pico menu, oem preempt:, and boot). */
				static int s_pf_applied = -1;
				int cur_pf = ayaneo_get_preempt_frames();
				/* the clock follows the CONFIGURED tier, not the shed depth: lowering the clock
				 * when the adaptive depth drops kept the depth from ever coming back (the loop
				 * sat at tier 0 speed with run-ahead off while the setting said Balanced) */
				if (cur_pf != s_pf_applied) { s_pf_applied = cur_pf; preempt_apply_cpu(ayaneo_get_preempt_frames()); }
			}
			{
				extern volatile unsigned int g_dbg_frame_ticks;
				unsigned int ct0 = gba_core_cpu_ticks();
				unsigned int em0 = gpt4_get_current_tick();
				s_ff_t0 = em0;   /* the frame budget for fast forward starts here */
				/* With run ahead on, the committed frame's picture is never shown, so it may go
				 * undrawn (cskip_now, see CSKIP_HOLD). The lines it skips stay dirty, so the
				 * look-ahead frame drawn after it redraws them, and the savestate keeps the line
				 * cache with the picture, so a rewind to this frame is consistent. Decided before
				 * the frame from the menu and from last frame's fast forward: a fast forward
				 * starting now ends in a drawn frame of its own. */
				s_cskip_frame = !s_menu_open && !s_benchmark && !s_ff_active &&
				                preempt_effective_depth() > 0;
				gba_core_skip_render(s_cskip_frame && cskip_now());
				lp_mark(1);
				run_one_frame();	/* runs one GBA frame + submits its audio */
				gba_core_skip_render(0);
				/* committed GBA cycles this display frame = one full frame (280896);
				 * run-ahead re-enters as a full frame (no priming stub) so no skew. */
				lp_mark(2);
				g_dbg_frame_ticks = gba_core_cpu_ticks() - ct0;
				s_committed_us = (gpt4_get_current_tick() - em0) / 13u;	/* this frame us */
				{	/* average the emulation wall time over 16 frames -> us */
					static unsigned int acc; static int cnt;
					acc += s_committed_us * 13u;	/* 13 MHz ticks */
					if (++cnt >= 16) { g_dbg_emu_us = acc / (16u * 13u); acc = 0; cnt = 0; }
				}
			}
			/* Rewind capture: snapshot this committed frame (and each FF frame below), so FF'd frames
			 * can be rewound. Not the run-ahead look-ahead frames (rewound anyway). Skipped in-menu. */
			{ unsigned ts0 = gpt4_get_current_tick();
			if (!s_menu_open && !s_benchmark) gba_rewind_snap();
			  g_dbg_snap_us = (gpt4_get_current_tick() - ts0) / 13u; }
			lp_mark(3);   /* benchmark measures emulation: no 640 KB snapshot per frame */
			/* Variable fast-forward (right analog trigger): after the committed frame, run extra
			 * MUTED frames so the game advances s_ff_level -> 2..FF_MAX_MULT x per displayed frame.
			 * The display stays vsync-locked at 60 Hz (normal present below), so the speed is
			 * rate-limited (not uncapped) and scales with how hard the trigger is squeezed. The
			 * ~2 ms trigger read is throttled to every 4th frame. */
			{
				extern int ayaneo_joypad_ff_level(void);
				s_ff_level = ayaneo_joypad_ff_level();   /* cached (poll'd above), no I2C stall */
				/* Variable fast-forward across the WHOLE range via extra emulated frames: run
				 * (2..FF_MAX_MULT-1) extra frames scaled by press depth. At full press FF_MAX_MULT
				 * frames/present is CPU-bound = the fastest the emulator can run (effectively
				 * uncapped). Audio is NOT muted (each extra frame submits its samples, so it speeds
				 * up). We deliberately do NOT use the old s_fast_forward present-every-8th path - it
				 * frame-skips, PAUSES audio, and was slower than this. Keep s_fast_forward off. */
				s_fast_forward = 0;
				s_ff_active = (s_ff_level > 0 && !s_menu_open);
				if (s_ff_active) {
					/* Adaptive cap: run only as many extra frames as fit ~one vsync so the panel
					 * stays a smooth 60fps instead of overrunning at a fixed 10x. Every GBA FF frame
					 * is a full render, so the committed-frame cost (s_committed_us) is the per-frame
					 * cost; reserve the present blit (g_dbg_blit_us). Keep a 2x floor so a squeeze
					 * always does something even on a heavy scene / low CPU clock. */
					extern volatile unsigned int g_dbg_blit_us;
					/* The frames between two presents are never seen: their render is skipped
					 * (ABI 2 skip_next_frame), only the last one draws. The budget is one
					 * refresh less the present blit: the rendered committed frame plus the
					 * rendered last frame, the rest at the cheaper skipped cost. Costs are
					 * averaged so the multiplier holds steady from frame to frame (a cap that
					 * jumps between 6 and 8 reads as uneven motion). */
					/* No multiplier and no cap: run render-skipped frames while another one
					 * still fits before this refresh's present (ayaneo_ff_fits), then one rendered
					 * frame, which is the picture shown. The panel gets a new frame every refresh,
					 * the game advances as far as the CPU allows in between. */
					extern int ayaneo_ff_fits(unsigned int t0, unsigned int blit_us, unsigned int next_us, unsigned int rend_us);
					static unsigned int s_ff_skip_us, s_ff_rend_us;
					unsigned int rend = s_ff_rend_us ? s_ff_rend_us : (s_committed_us ? s_committed_us : 2500u);
					unsigned int skipc = s_ff_skip_us ? s_ff_skip_us : rend;
					unsigned int blit = g_dbg_blit_us < 15500u ? g_dbg_blit_us : 0u;
					int n = 1;
					while (n < 120 && ayaneo_ff_fits(s_ff_t0, blit, skipc, rend)) {
						unsigned int t = gpt4_get_current_tick(), us;
						gba_core_skip_render(1);
						run_one_frame();
						us = (gpt4_get_current_tick() - t) / 13u;
						s_ff_skip_us += ((int)us - (int)s_ff_skip_us) / 4; skipc = s_ff_skip_us;
						{ extern void ayaneo_fps_tick(void); ayaneo_fps_tick(); }   /* every emulated frame counts */
						n++;   /* no rewind snapshot for frames between presents: one per shown frame */
					}
					{	/* the last frame of the batch is rendered: it is the one presented */
						unsigned int t = gpt4_get_current_tick(), us;
						gba_core_skip_render(0);
						run_one_frame();
						us = (gpt4_get_current_tick() - t) / 13u;
						s_ff_rend_us += ((int)us - (int)s_ff_rend_us) / 4;
						/* The skipped frame cost only moves when a skipped frame runs: one slow one (a
						 * translation burst) left it above the budget for good, no skipped frame ran
						 * again and fast forward sat at 2x until the game was closed. A frame that
						 * skips its render costs no more than one that draws, and that one is measured
						 * every batch: cap the estimate there. */
						if (s_ff_skip_us > s_ff_rend_us) s_ff_skip_us = s_ff_rend_us;
						g_dbg_ff_skip_us = s_ff_skip_us; g_dbg_ff_rend_us = s_ff_rend_us;
						{ extern void ayaneo_fps_tick(void); ayaneo_fps_tick(); }
						gba_rewind_snap();
						n++;
					}
					ayaneo_hud_set(1, n * 10);
				} else {
					ayaneo_hud_set(0, 0);
				}
			}
			/* (Rewind capture is done above per committed + FF frame, so the run-ahead look-ahead
			 * frames below are correctly NOT captured.) */
#ifdef AYANEO_GBA_SD
			if (g_dbg_corebench_req && !s_menu_open) { int nb = g_dbg_corebench_req; g_dbg_corebench_req = 0; gba_corebench(nb); }
#endif
			if (g_dbg_selftest_req) {	/* `oem selftest[:N]`: validate the rewind path */
				int nf = g_dbg_selftest_req;	/* N frames (1 = single-rewind) */
				g_dbg_selftest_req = 0;
				gba_run_ahead_selftest(nf);
			}

			if (frame == 0)		/* first frame done => dynarec executed OK */
				gba_dbg("GBA 7: first frame rendered (dynarec ok)");

			uncapped = s_fast_forward || s_benchmark;   /* FF keeps audio playing (sped up) */
			if (uncapped != ff_prev) {
				ayaneo_gbc_audio_pause(uncapped);
				ff_prev = uncapped;
			}

			mtk_wdt_restart();
			poll_volume();
			/* Hand the deferred volume/brightness persist to the writer thread once the user
			 * has stopped adjusting (~0.7s): the SD mirror takes 200-500 ms and used to run
			 * here with the ring silenced, which is the stutter heard on every change. */
			if (s_settings_dirty &&
			    (gpt4_get_current_tick() - s_settings_tick) >= 9100000u) {
				s_settings_dirty = 0;
				ayaneo_menu_settings_persist();
			}
			/* the power LED is handled by led_thread (all cores) */
			check_power(scratch);
			if (!s_menu_open)
				battery_poll(BATTERY_GAMEPLAY_FRAMES);	/* ~once/10 min, no pause */
			frame++;
			{ extern void ayaneo_fps_tick(void); extern void ayaneo_hud_bench(int on); ayaneo_fps_tick(); ayaneo_hud_bench(s_benchmark); }

			if (aya_edge())
				menu_toggle();
			/* Hold AYA ~1.5 s to exit straight back to the ROM selector (matches SNES/GBC).
			 * A tap still just toggles the menu above; only a sustained hold requests Close. */
			{
				static int aya_hold;
				if (PRESSED(GPIO_AYA)) { if (++aya_hold >= 90) { s_close_req = 1; aya_hold = 0; } }
				else aya_hold = 0;
			}
			if (s_menu_open)
				menu_tick(scratch);

			/* soft reset (menu "Reset Game" or the Select+Start+L+R hotkey): restart
			 * the current game. The ROM + BIOS are still resident, so reset_gba() alone
			 * re-inits the core from the cart entry; it flushes the dynarec, so we must
			 * NOT resume execute_arm in place - request a clean CPU-thread restart, which
			 * the next run_one_frame triggers (same mechanism as the intro->game reset). */
			if (s_reset_req) {
				s_reset_req = 0;
				s_menu_open = 0;
				ayaneo_menu_audio_silence();	/* drop the stale ring so it does not loop */
				reset_gba();
				gba_rewind_arm();		/* reset breaks the rewind timeline */
				s_cpu_restart_req = 1;
			}

			/* in-game menu "Close": save the current game (state + battery sav) and
			 * go back to the SNES ROM selector with a reverse punch-hole transition.
			 * The CPU thread is parked on the frame-sync event while gba_sd_rom_select
			 * runs; core_start + s_cpu_restart_req re-enter it cleanly into the pick. */
			if (g_dbg_force_close) { g_dbg_force_close = 0; s_close_req = 1; }
			if (s_close_req) {
				s_close_req = 0;
				s_menu_open = 0;
				/* Tear down the hardware menu overlay (OVL0 L0) before the reverse punch +
				 * carousel: an AYA-hold or "Close" exit can leave it enabled, which would
				 * composite the stale Pico panel over the transition and the carousel. */
				{
					extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
					ayaneo_menu_overlay(0, 0);
				}
#ifdef AYANEO_GBA_SD
				if (s_sd_mode && s_nrom > 0) {
					extern void gba_menu_arm_reverse(const unsigned short *game_frame);
					unsigned char *rp = gba_core_rom_ptr();
					long rsz;
					/* Wipe the shared AFE ring FIRST, BEFORE the slow card saves:
					 * the CPU thread is parked here so nothing refeeds the ring, and
					 * state_write/sav_save stall the loop for ~1s during which the DMA
					 * would otherwise replay the last game audio (the loop the user
					 * heard on close). Silencing up front makes the save play silence.
					 * Do NOT use ayaneo_gbc_audio_pause(1) (latches s_gbc_paused=1 and
					 * permanently mutes the menu BGM + future games); the wipe alone
					 * stops the loop and the menu re-inits the codec like first boot. */
					ayaneo_menu_audio_silence();
					state_write(scratch);		/* persist the current game */
					sav_save(scratch);
					gba_menu_arm_reverse(gba_core_screen());  /* freeze frame for reverse */
					for (;;) {
						int sel = gba_sd_rom_select();
						s_sel_rom = sel;
						if (s_roms[sel].type == GBA_CONSOLE_SNES) {
							snes_sd_session(&s_sd_vol, &s_roms[sel]);
							s_gpsp_dirty = 1;
							continue;
						}
						if (s_roms[sel].type >= GBA_CONSOLE_GENESIS) {
							genesis_sd_session(&s_sd_vol, &s_roms[sel]);
							s_gpsp_dirty = 1;
							continue;
						}
						if (s_roms[sel].type != GBA_CONSOLE_GBA) {
							gbc_sd_session(&s_sd_vol, &s_roms[sel]);
							s_gpsp_dirty = 1;
							continue;
						}
						rp = gba_core_for_launch();
						if (!rp) continue;   /* no GBA core loads from the card */
						rsz = gba_sd_load_rom(&s_sd_vol, &s_roms[sel], rp,
								      gba_core_rom_capacity());
						if (rsz) break;
					}
					if (gba_core_start(rsz, s_have_bios ? s_sd_bios : 0) != 0)
						return 0;
					s_game_live = 1;
					gba_sd_load_sav(&s_sd_vol, s_roms[s_sel_rom].name,
							(unsigned char *)gba_core_backup_ptr(),
							gba_core_backup_size());
					if (!PRESSED(GPIO_B))
						state_read(scratch);
					gba_rewind_arm();	/* fresh game -> fresh rewind timeline */
					/* no blank: keep the frozen menu on screen for the seamless
					 * growing-circle opening (the punch composites the full frame). */
					dynarec_enable = 1;
					s_cpu_restart_req = 1;
					punch_start = 0;	/* re-arm the forward punch */
				}
#endif
				continue;
			}

			/* Refresh the Pico menu as a hardware overlay (OVL0 L0) over the running game,
			 * or disable it when closed - so aspect/filter/etc. preview live with the game
			 * visible underneath and no pause. Done each frame before the present dispatch. */
			{
				extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
				ayaneo_menu_overlay(gbc_menu_draw_overlay, s_menu_open);
			}

			/* fast-forward: present sparsely so the vsync block does not cap the
			 * rate - runs flat out. */
			if (s_fast_forward) {
				ayaneo_present_skip_framedone = 0;
				if ((frame & 7) == 0)
					ayaneo_gbc_show_frame(gba_core_screen());
				continue;
			}

			/* benchmark: run UNCAPPED (non-blocking present, no vsync wait) and
			 * render EVERY frame, so the fps counter shows the true emulation rate
			 * (the panel just samples the latest frame each scan). */
			if (s_benchmark) {
				static unsigned bench_base, bench_cnt;
				unsigned now = gpt4_get_current_tick(), el;
				if (!bench_base) bench_base = now;
				bench_cnt++;
				el = now - bench_base;
				if (el >= 6500000u) {
					s_fps = (int)((unsigned long long)bench_cnt * 13000000ull / el);
					bench_base = now; bench_cnt = 0;
				}
				/* present every 8th frame like fast forward: on the Konkr a present is
				 * paced to the panel edge, so one per emulated frame capped the loop */
				ayaneo_present_skip_framedone = 1;
				if ((frame & 7) == 0)
					ayaneo_gbc_show_frame(gba_core_screen());
				continue;
			}

			/* normal play: one present per frame = paced to the panel vsync */
			ayaneo_present_skip_framedone = 0;
			/* Launch punch-hole: for GBA_PUNCH_MS after a menu launch, composite the
			 * live game inside a growing circle over the frozen menu snapshot so the
			 * menu is visibly eaten by the expanding hole while real gameplay runs
			 * underneath. Radius is paced by elapsed wall-clock (snappy, fixed
			 * duration regardless of the per-frame composite cost). Fast-forward/
			 * benchmark above skip this; once done, the normal game present resumes. */
			if (gba_punch_ready) {
				/* FRAME-paced fast opening (mirror of the smooth close): freeze +
				 * pre-render the launch game frame once, then step a growing circle
				 * over a fixed count with a memcpy-only composite = smooth 60fps. The
				 * game is paused for the ~0.3s punch (imperceptible at launch). Then
				 * clear only the letterbox (no black flash) and resume live gameplay. */
				extern void ayaneo_gba_punch_prerender(const unsigned short *pix);
				extern void ayaneo_gba_punch_frame_pre(const unsigned int *snap, int radius);
				extern void ayaneo_gbc_clear_letterbox(void);
				int i, N = ayaneo_punch_steps(), w;
				gba_punch_ready = 0;
				(void)punch_start;
				/* On a FRESH launch the game has only rendered the GBA BIOS white
				 * screen, so pre-rendering it here would open into a jarring white.
				 * Run the game forward until it has produced real (non-blank-white)
				 * content, capped, so the opening shows the actual game.
				 *
				 * When SWITCHING games, s_screen still holds the PREVIOUS game's last
				 * frame (non-white), so the check below would break at w=0 and open onto
				 * the old game's screenshot. Wipe the screen to white first: now BOTH the
				 * fresh-boot BIOS and the stale-previous-game cases start white, and the
				 * loop runs the NEW game until it renders its own real content. Resumed
				 * games redraw on their first emulated frame => ~1 frame of warmup. */
				gba_core_screen_fill(0xFFFFu);
				for (w = 0; w < 150; w++) {
					const unsigned short *s = gba_core_screen();
					int k, white = 0;
					run_one_frame();                        /* render at least one NEW frame */
					s = gba_core_screen();
					for (k = 0; k < 240 * 160; k += 373)
						if (s[k] >= 0xF7DEu) white++;   /* near-white RGB565 */
					if (white < 90)                         /* <90% of ~103 samples */
						break;
				}
				ayaneo_gba_punch_prerender(gba_core_screen());
				for (i = 1; i <= N; i++) {
					int r = (int)((long long)GBA_PUNCH_MAX_R * i / N);   /* 0 -> MAX */
					if (r < 1) r = 1;
					ayaneo_gba_punch_frame_pre((const unsigned int *)GBA_PUNCH_SNAP_PA, r);
					mtk_wdt_restart();
				}
				ayaneo_gbc_clear_letterbox();
				ayaneo_gbc_show_frame(gba_core_screen());
				continue;
			}
			/* Fast forward presents the batch's last frame as it is: no run-ahead on top
			 * (the look-ahead frames ate the time the batch had measured for itself, the
			 * budget then missed the panel edge and widened, which is the capped feel), and
			 * no run-ahead depth adaptation from a loop that is busy by design. */
			if (s_ff_active) {
				g_dbg_preempt_emu_us = 0;
				ayaneo_gbc_show_frame(gba_core_screen());
				continue;
			}
			/* run-ahead present: look-ahead pf frames then rewind (or a plain
			 * present when Off), paced to the panel vsync. */
			lp_mark(4);
			preempt_present();
			if (s_cskip_frame && s_ahead_shown)
				cskip_account(s_committed_us + s_ahead_us);
			lp_mark(5);
			lp_commit();
			/* closed-loop: shed run-ahead depth if the full frame period shows
			 * the loop overrunning vsync (which would starve audio). */
			preempt_adapt(s_menu_open);
		}
	}
	return 0;
}

/* ===================== offline charging screen ===================== */
extern void ayaneo_display_prepare(void);
extern void ayaneo_apply_backlight(int level);
extern void ayaneo_apply_persisted_brightness(void);

static void text_center(unsigned int *buf, unsigned int pitch, int cx, int y,
			int scale, unsigned int argb, const char *s)
{
	int n = 0;
	while (s[n]) n++;
	ayaneo_text(buf, pitch, cx - n * 8 * scale / 2, y, scale, argb, s);
}

/* Offline charging screen. Everything is timed on the 13 MHz counter rather than on loop
 * turns, because a present may wait for display edges that are slow or absent in this early
 * context: hold POWER 1.2 s to boot, the panel goes dark after 10 s idle, unplugging for
 * 300 ms powers off. The frame is redrawn when something changed or every 500 ms, with a
 * small heartbeat line (loop count, last charger state) so a stall shows where it stopped. */
void ayaneo_gbc_charging_screen(void)
{
	int disp_on = 1, pct = 0, dummy, chr_prev = -1, pct_prev = -1, holding_prev = -1;
	unsigned pk_reads = 0, pk_runs = 0;   /* power key: reads in the current hold, holds started */
	unsigned t_hold0 = 0, t_unplug0 = 0, t_idle0, t_bat0, t_draw0 = 0, loops = 0;
	int chg_st = -1;

	ayaneo_settings_load();
#ifdef KONKR_KPA
	{ extern int ayaneo_mt6360_chrind_off(void); ayaneo_mt6360_chrind_off(); }
#endif
	ayaneo_display_prepare();
	ayaneo_apply_backlight(40);
	mtk_wdt_disable();
	pct = battery_read(&dummy);
	t_idle0 = t_bat0 = gpt4_get_current_tick();

	for (;;) {
		unsigned now = gpt4_get_current_tick();
		int chr = upmu_is_chr_det();
		int pk = pmic_detect_powerkey();
		int holding;
		loops++;
		{ extern unsigned int ayaneo_bb_uptime_ms(void); ayaneo_bb_uptime_ms(); }   /* keep the wrap safe uptime running */

		if (now - t_bat0 >= 13000000u) { pct = battery_read(&dummy); chg_st = charger_status(); t_bat0 = now; }
		set_charge_led(chr, pct);

		/* unplugged for 300 ms: off (KPOC exists only to charge) */
		if (!chr) { if (!t_unplug0) t_unplug0 = now ? now : 1u; else if (now - t_unplug0 >= 3900000u) mt_power_off(); }
		else t_unplug0 = 0;

		/* POWER: a tap wakes the screen, 1.2 s held boots */
		if (pk) {
			if (!t_hold0) { t_hold0 = now ? now : 1u; pk_runs++; pk_reads = 0; }
			pk_reads++;
			if (now - t_hold0 >= 15600000u) {
				/* record why the charging screen handed over: a night-time start nobody asked for
				 * (2026-11-03) has to be told apart from a real press */
				extern void ayaneo_bb_note(unsigned type, const char *msg, const unsigned *a, unsigned n);
				extern unsigned int ayaneo_bb_uptime_ms(void);
				extern volatile unsigned int g_dbg_pwrap_waits, g_dbg_pwrap_fails;
				unsigned a[8];
				a[0] = (now - t_hold0) / 13000u; a[1] = pk_reads; a[2] = pk_runs; a[3] = loops;
				a[4] = g_dbg_pwrap_waits; a[5] = g_dbg_pwrap_fails; a[6] = (unsigned)pct; a[7] = (unsigned)chr;
				ayaneo_bb_note(2, "charging screen: power held, starting", a, 8);
				ayaneo_apply_persisted_brightness(); return;
			}
			holding = 1 + (int)((now - t_hold0) / 1300000u);   /* tenths of the way */
		} else {
			if (t_hold0 && !disp_on) { disp_on = 1; ayaneo_apply_backlight(40); }
			if (t_hold0) t_idle0 = now;
			t_hold0 = 0;
			holding = 0;
		}

		if (disp_on && now - t_idle0 >= 130000000u) { disp_on = 0; ayaneo_apply_backlight(0); }

		if (disp_on && (chr != chr_prev || pct != pct_prev || holding != holding_prev || now - t_draw0 >= 6500000u)) {
			unsigned int pitch, W, H;
			unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
			int cx = (int)W / 2, cy = (int)H / 2;
			char line[48], *p;

			chr_prev = chr; pct_prev = pct; holding_prev = holding; t_draw0 = now;
			p = mi_putu(line, (unsigned)pct); p = mi_puts(p, "%"); *p = 0;
			ayaneo_fill(buf, pitch, 0, 0, (int)W, (int)H, 0xFF000000u);
			text_center(buf, pitch, cx, cy - 96, 6, 0xFFFFFFFFu, line);
			text_center(buf, pitch, cx, cy + 24, 3,
				    chr ? 0xFF60D080u : 0xFFD08060u,
				    chr ? (chg_st == 2 ? "Charged" : "Charging") : "On battery");
			text_center(buf, pitch, cx, cy + 96, 1, 0xFF808890u,
				    "Tap POWER for status  -  hold to start  -  unplug for off");
			if (holding) {   /* hold progress bar */
				int w = (int)W / 3, x0 = cx - w / 2, y0 = cy + 130;
				ayaneo_fill(buf, pitch, x0, y0, w, 8, 0xFF303840u);
				ayaneo_fill(buf, pitch, x0, y0, w * (holding > 12 ? 12 : holding) / 12, 8, 0xFFFFFFFFu);
			}
			p = mi_puts(line, "loop "); p = mi_putu(p, loops); p = mi_puts(p, "  chg ");
			p = mi_putu(p, (unsigned)(chg_st < 0 ? 9 : chg_st)); *p = 0;
			text_center(buf, pitch, cx, (int)H - 24, 1, 0xFF404850u, line);
			ayaneo_canvas_present();
		}

		mtk_wdt_restart();
		thread_sleep(50);
	}
}

#ifdef AYANEO_GBA_SD
void ayaneo_gbc_start(void);   /* defined just below; spawns emu_thread */

/*
 * The boot mode lives on the card, in /gammaos/boot.cfg: text, one key=value per line, '#'
 * comments, "boot=sd" to start the GammaOS Pico menu from the card, "boot=android" to boot Android.
 * A card without the file, or without a valid boot line, is not a GammaOS card and the console
 * boots Android. The LK writes nothing on the eMMC for this (the sticky marker that used to live
 * in misc is gone): "Boot to Android" in the menu writes boot=android, holding SELECT at power
 * on writes boot=sd.
 */
#define GAMMAOS_DIR    "/gammaos"
#define BOOTCFG_NAME   "boot.cfg"
#define MENU_PACK_PATH "/gammaos/menu.pack"
enum { BOOTCFG_NONE = 0, BOOTCFG_SD, BOOTCFG_ANDROID };

static int cfg_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int cfg_word(const char *p, const char *e, const char *w)
{
	while (*w) {
		char c = (p < e) ? *p : 0;
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c != *w) return 0;
		p++; w++;
	}
	return p == e;
}
static int bootcfg_read(fat_vol *v)
{
	static char b[1024];
	fat_file f;
	uint32_t n, i = 0;
	int mode = BOOTCFG_NONE;
	if (fat_open(v, GAMMAOS_DIR "/" BOOTCFG_NAME, &f) != 0)
		return BOOTCFG_NONE;
	n = fat_read(&f, 0, b, sizeof b);
	while (i < n) {
		uint32_t s0 = i, e;
		const char *k, *ke, *val, *ve;
		while (i < n && b[i] != '\n') i++;
		e = i++;
		while (s0 < e && cfg_space(b[s0])) s0++;
		while (e > s0 && cfg_space(b[e - 1])) e--;
		if (s0 == e || b[s0] == '#') continue;
		k = b + s0; ke = k;
		while (ke < b + e && *ke != '=' && !cfg_space(*ke)) ke++;
		val = ke;
		while (val < b + e && cfg_space(*val)) val++;
		if (val >= b + e || *val != '=') continue;
		val++;
		while (val < b + e && cfg_space(*val)) val++;
		ve = b + e;
		if (!cfg_word(k, ke, "boot")) continue;
		if (cfg_word(val, ve, "sd")) mode = BOOTCFG_SD;
		else if (cfg_word(val, ve, "android")) mode = BOOTCFG_ANDROID;
	}
	return mode;
}
static int bootcfg_write(fat_vol *v, int mode)
{
	static const char head[] =
		"# GammaOS Pico boot mode for this card.\n"
		"#   boot=sd       start the GammaOS Pico menu from this card\n"
		"#   boot=android  boot Android (hold SELECT at power on to come back)\n";
	static char txt[sizeof head + 32];
	unsigned n = 0, k;
	const char *line = (mode == BOOTCFG_ANDROID) ? "boot=android\n" : "boot=sd\n";
	for (k = 0; head[k]; k++) txt[n++] = head[k];
	for (k = 0; line[k]; k++) txt[n++] = line[k];
	fat_wr_mkpath(v, GAMMAOS_DIR);
	return fat_wr_put(v, GAMMAOS_DIR, BOOTCFG_NAME, txt, n);
}
static int card_file_present(fat_vol *v, const char *path)
{
	fat_file f;
	return fat_open(v, path, &f) == 0 && f.size > 0;
}

/* The menu's "Boot to Android": boot=android on the card, then a reset, which the gate turns into
 * a normal Android boot. The emulator owns the clocks, DMA and caches by now, so jumping into the
 * kernel from here is not an option. */
void ayaneo_boot_to_os(void)
{
	extern void mtk_arch_reset(char mode);
	if (s_sd_mode)
		bootcfg_write(&s_sd_vol, BOOTCFG_ANDROID);
	mtk_wdt_restart();			/* keep alive across the write + reset */
	mtk_arch_reset(1);
	for (;;) ;				/* not reached */
}

/*
 * SD boot gate, at the top of the normal boot. Returns < 0 for the normal Android boot (the
 * caller shows the logo held back for it), 0 when the GammaOS menu has been started from the
 * card. In order:
 *  - SELECT held at power on: go to the menu if the card can (writes boot=sd).
 *  - two watchdog resets in a row inside this path: Android (the never-brick guard).
 *  - no card, a card that does not mount, no /gammaos/boot.cfg with a valid boot line, or
 *    boot=android: Android.
 *  - the menu needs /gammaos/menu.pack; missing: Android. The GBA cores are optional: armGBA
 *    (/cores/armgba-pico.blob) plays the intro and is the default for GBA games, gpSP
 *    (/cores/gpsp-pico.blob) plays the BIOS intro and runs them when armGBA is missing and the
 *    card has a BIOS; with neither the menu runs without GBA games.
 * Then the settings, the BIOS (optional: /bios/gba_bios.bin or /gba_bios.bin; without it the
 * intro is armGBA's own splash) and the cart header file are read, and the emulator thread starts.
 */
int ayaneo_gba_sd_boot(void)
{
	int rc, mode;
	{ extern void ayaneo_boot_stamp(int i); ayaneo_boot_stamp(4); }   /* oem boottime: SD boot begins */
	/* Hold SELECT at power on for the menu: the way back from boot=android. It also overrides the
	 * watchdog guard, since the user is asking for the menu. */
	int force_emu = ayaneo_gbc_select_held();

	/* NEVER-BRICK GUARD: a crash in the SD path ends in a watchdog reset. Two SD attempts in a
	 * row that died before 30 s of stable running (ayaneo_sd_guard_enter, black box DRAM) send
	 * the boot to Android; one crash does not. */
	{
		extern unsigned int mtk_wdt_check_status(void);
		extern int ayaneo_sd_guard_enter(unsigned int wdt_status);
		unsigned int wsta = mtk_wdt_check_status();
		if (ayaneo_sd_guard_enter(wsta) && !force_emu) {
			GBA_LOG("gba-sd: two watchdog resets in the SD path in a row (0x%x) - Android\n", wsta);
			return -9;
		}
	}

	s_bt_t0 = gpt4_get_current_tick();		/* cold-start clock base (~backlight time) */
	{
		extern int gba_sd_card_present(void);
		if (!gba_sd_card_present()) {
			GBA_LOG("gba-sd: no card in the slot (GPIO8) - Android\n");
			return -1;
		}
	}
	rc = gba_sd_mount(&s_sd_vol);
	if (rc != 0) {
		GBA_LOG("gba-sd: the card does not mount (rc=%d) - Android\n", rc);
		return -1;
	}
	g_dbg_bt_mount = BT_MS();

	mode = bootcfg_read(&s_sd_vol);
	if (force_emu) {
		if (mode != BOOTCFG_SD && card_file_present(&s_sd_vol, MENU_PACK_PATH)) {
			bootcfg_write(&s_sd_vol, BOOTCFG_SD);
			GBA_LOG("gba-sd: SELECT held - boot=sd written\n");
		}
	} else if (mode == BOOTCFG_NONE) {
		GBA_LOG("gba-sd: no valid " GAMMAOS_DIR "/" BOOTCFG_NAME " - Android\n");
		return -2;
	} else if (mode == BOOTCFG_ANDROID) {
		GBA_LOG("gba-sd: boot=android - Android (hold SELECT at power on for the menu)\n");
		return -10;
	}

	if (!card_file_present(&s_sd_vol, MENU_PACK_PATH)) {
		GBA_LOG("gba-sd: " MENU_PACK_PATH " missing - Android\n");
		return -3;
	}
	s_have_bios = (gba_sd_load_bios(&s_sd_vol, s_sd_bios) == 0);
	g_dbg_bt_bios = BT_MS();
	if (!card_file_present(&s_sd_vol, CORE_PATH_ARMGBA) &&
	    !(s_have_bios && card_file_present(&s_sd_vol, CORE_PATH_GPSP)))
		GBA_LOG("gba-sd: no usable GBA core (%s, or %s with a BIOS): the menu runs without GBA games\n",
			CORE_PATH_ARMGBA, CORE_PATH_GPSP);
#if defined(KONKR_KPA)
	/* GammaOS starts: the panel came up on the stock timing (what Android gets), now it moves to
	 * GammaOS's (st7123 LCM driver). Done here, before the settings and the cart header are read,
	 * so the panel has settled on the new timing well before the first frame. */
	{
		extern int st7123_gammaos_timing(void);
		g_dbg_bt_retime = st7123_gammaos_timing();
	}
#endif

	gba_sd_settings_load(&s_sd_vol);		/* /gammaos/settings.bin; defaults when missing */
	if (cart_header_read(&s_sd_vol)) {
		/* the intro starts now; the ROM listing runs on its own thread meanwhile */
		s_sd_fast = 1;
		s_sd_bg_state = 0;
		s_sd_bg_pending = 1;	/* emu_thread starts the listing once the core is loaded */
	} else {
		/* first boot of this card (no header file yet): list first, so the intro has the logo */
		int total = 0;
		gba_sd_make_rom_dirs(&s_sd_vol);
		s_nrom = gba_sd_list_roms(&s_sd_vol, s_roms, 128, &total);
		g_dbg_bt_list = BT_MS();
		if (total > s_nrom)
			GBA_LOG("gba-sd: WARNING %d roms present, showing first %d (cap)\n", total, s_nrom);
		cart_header_refresh();
	}
	GBA_LOG("gba-sd: GammaOS card (bios=%d, header=%d) - starting the menu\n", s_have_bios, s_cart_hdr_valid);
	s_sd_mode = 1;
	ayaneo_gbc_start();   /* spawns emu_thread */
	/* the USB fastboot debug channel alongside the running emulator (oem diag, screenshots) */
	{
		extern void ayaneo_fastboot_usb_start(void);
		ayaneo_fastboot_usb_start();
	}
	return 0;
}
#endif /* AYANEO_GBA_SD */

void ayaneo_gbc_start(void)
{
#if defined(AYANEO_AUDIO_TRACE) || defined(AYANEO_DEBUG_LOGGING)
	/* Force LK's UART console on for the emulator bring-up traces, independent of
	 * the boot-chime path (uart_putc gates on g_boot_arg->log_enable). */
	extern void ayaneo_gba_force_uart(void);
	ayaneo_gba_force_uart();
	GBA_ATRACE("GBA: ayaneo_gbc_start (GBA build) - spawning emu thread\n");
#endif
	/* emu_thread only orchestrates (menu + SD + display); both cores emulate on their
	 * OWN threads (gpSP on gba_cpu, gambatte on gbc_emu, each with its own large stack),
	 * so 64 KB is enough here. See emu/gbc/gbc_sd_run.c and documentation/development/core-porting-notes.md. */
	thread_t *t = thread_create("ayaneo_gba", &emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t)
		thread_resume(t);
	led_thread_start();
}
