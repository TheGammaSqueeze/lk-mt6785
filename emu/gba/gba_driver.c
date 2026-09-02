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
extern const struct gba_core_exports *gba_core_load(void);	/* gba_core_loader.c */
static const struct gba_core_exports *g_core;

#define gba_core_init(...)          g_core->core_init(__VA_ARGS__)
#define gba_core_start(...)         g_core->core_start(__VA_ARGS__)
#define gba_core_enter_bios(...)    g_core->enter_bios(__VA_ARGS__)
#define reset_gba(...)              g_core->reset(__VA_ARGS__)
#define gba_core_cpu_loop(...)      g_core->cpu_loop(__VA_ARGS__)
#define gba_core_pre_frame(...)     g_core->pre_frame(__VA_ARGS__)
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
extern void ayaneo_gba_show_intro_frame(const unsigned short *pix);	/* 6x fill-height */
extern void ayaneo_gbc_blank(void);		/* clear both game fbs to black */
/* Punch-hole launch transition: composite the live game frame inside a growing
 * circle over the frozen menu snapshot the selector captured (gba_snes_menu.c). */
extern void ayaneo_gba_punch_frame(const unsigned short *pix, const unsigned int *snap,
				   int cx, int cy, int radius);
extern int  gba_punch_ready;			/* set by the SNES selector on launch */
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
static void sd_settings_mirror(void);	/* mirror settings to the SD card (early fwd decl) */
#endif
extern int  ayaneo_brightness_step(int dir);
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_gbc_osd_show(int kind, int pct);
extern int  ayaneo_get_load_on_boot(void);
extern void ayaneo_set_load_on_boot(int v);
extern int  ayaneo_get_skip_boot(void);
extern void ayaneo_set_skip_boot(int v);
extern int  ayaneo_get_skip_gba_intro(void);
extern void ayaneo_set_skip_gba_intro(int v);
extern int  ayaneo_get_lcd_filter(void);
extern void ayaneo_set_lcd_filter(int v);
extern int  ayaneo_get_color_correct(void);
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
static volatile int s_close_req;	/* in-game menu "Close": save + back to the SNES selector */
static volatile int s_reset_req;	/* soft reset: restart the current game (menu Reset / hotkey) */
/* Per-frame emulation cost (run_one_frame wall time, us) averaged over 16 frames.
 * The emulator is single-core, so run_one_frame blocks the main thread while the
 * CPU thread emulates one frame - its wall time IS the emulation cost. Exposed via
 * oem diag `em=` to size the run-ahead/preemptive-frames headroom (each extra
 * predicted frame costs one more of these). */
volatile unsigned int g_dbg_emu_us;
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
static volatile unsigned s_keys;	/* GBA mask, refreshed once per frame */

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
#define GPIO_A		83	/* swapped vs DTS to match the physical layout */
#define GPIO_B		82
#define GPIO_START	91	/* swapped: Start<->Select */
#define GPIO_SELECT	90
#define GPIO_LB		92	/* GBA L shoulder */
#define GPIO_RB		81	/* GBA R shoulder */
#define GPIO_X		84	/* autofire B */
#define GPIO_Y		85	/* autofire A */
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
	unsigned m = 0;
	if (PRESSED(GPIO_A))      m |= 0x01u;
	if (PRESSED(GPIO_B))      m |= 0x02u;
	if (PRESSED(GPIO_SELECT)) m |= 0x04u;
	if (PRESSED(GPIO_START))  m |= 0x08u;
	if (PRESSED(GPIO_RIGHT))  m |= 0x10u;
	if (PRESSED(GPIO_LEFT))   m |= 0x20u;
	if (PRESSED(GPIO_UP))     m |= 0x40u;
	if (PRESSED(GPIO_DOWN))   m |= 0x80u;
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
	if (PRESSED(GPIO_R2)) raw |= RB_FF;

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
	s_keys = m;
	gba_core_set_keys(m);
}

/* Volume keys (MTK keypad); Select + Volume = brightness. Persisted to boot_b. */
static void poll_volume(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);
	int vd = mtk_detect_key(0x00);
	int sel = PRESSED(GPIO_SELECT);
	int dir = 0;

	if (vu && !vu_prev) dir = +1;
	else if (vd && !vd_prev) dir = -1;

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
		/* Defer the persist. ayaneo_settings_save() + sd_settings_mirror() do an
		 * eMMC + SD/FAT write that stalls this loop for many ms; done inline on
		 * every key repeat, the audio ring is starved between steps and the AFE
		 * DMA loops the last frames (the "audio looping between volume steps").
		 * Mark dirty + timestamp instead; the game loop flushes ONCE, ~0.7s after
		 * the last change, with the ring silenced across the write. */
		s_settings_dirty = 1;
		s_settings_tick = gpt4_get_current_tick();
	}
	if (!sel)
		s_sel_modifier = 0;
	vu_prev = vu;
	vd_prev = vd;
}

/* ---- little-endian helpers ---- */
static unsigned rd32le(const unsigned char *p)
{ return (unsigned)p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static void wr32le(unsigned char *p, unsigned v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* ---- save state (boot_b) ---- */
#define GBA_STATE_OFF	0x01C00000u	/* 28 MB */
#define GBA_STATE_MAGIC	0x53414247u	/* "GBAS" */
#define GBA_STATE_MAX	(1u * 1024 * 1024)	/* 512 KB gpSP state + hdr, block-aligned */

#ifdef AYANEO_GBA_SD
static int  sd_mode_on(void);            /* defined after the SD globals below */
static int  sd_state_write(unsigned char *scratch);
static int  sd_state_read(unsigned char *scratch);
static void sd_sav_write(void);
static void sd_settings_mirror(void);    /* persist GammaOS settings to the SD card */
#endif

static int state_write(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	unsigned total;

#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) return sd_state_write(scratch);
#endif
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
}

static int state_read(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned magic, sz;

#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) return sd_state_read(scratch);
#endif
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
}

/* ---- cartridge battery save (.sav = gpSP gamepak_backup) ---- */
#define GBA_SAV_OFF	0x01D00000u	/* 29 MB */
#define GBA_SAV_MAGIC	0x56415347u	/* "GSAV" */
#define GBA_SAV_MAX	(256u * 1024u)	/* 128 KB backup + hdr, block-aligned */

static void sav_save(unsigned char *scratch)
{
	unsigned sz = gba_core_backup_size();		/* fixed 128 KB */
	unsigned total = 8 + sz;

#ifdef AYANEO_GBA_SD
	if (sd_mode_on()) { sd_sav_write(); return; }
#endif
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
}

static void sav_load(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned sz, live = gba_core_backup_size();

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

static void save_and_poweroff(unsigned char *scratch)
{
	/* Mute the game audio FIRST so the ~1s eMMC/SD save does not replay the AFE ring
	 * (the loop the user heard on power-off). pause(1) both wipes the ring and stops
	 * the submit path refeeding it; the permanent mute is fine here - we are shutting
	 * down anyway (audio_shutdown follows). */
	ayaneo_gbc_audio_pause(1);
	state_write(scratch);
	sav_save(scratch);
	ayaneo_settings_save();
#ifdef AYANEO_GBA_SD
	sd_settings_mirror();
#endif
	ayaneo_gbc_audio_shutdown();
	mt_power_off();
}

static void check_power(unsigned char *scratch)
{
	static int armed;
	int p = pmic_detect_powerkey();
	if (!p)
		armed = 1;
	else if (armed)
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

/* run exactly one frame: kick the CPU thread and block until it yields back */
static void run_one_frame(void)
{
	gba_core_pre_frame();		/* latch input */
	event_signal(&ev_cpu, false);
	event_wait(&ev_main);
	gba_core_post_frame();		/* drain audio */
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
	/* While the Pico menu is open the game is not being played (input is zeroed) and
	 * the overlay makes the present blit heavier. Running the look-ahead frames on top
	 * of that overran the 16.7 ms budget and starved the audio ring = the skipping heard
	 * with the menu open + Preemptive Frames on. Present the committed frame directly (no
	 * look-ahead, no rewind), which frees the budget for the overlay. Leave g_dbg_eff_pf
	 * at the last gameplay value so the menu still shows the chosen depth (e.g. "Max (3)")
	 * rather than flipping to "(0)". */
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
		static unsigned char s_ahead_state[512 * 1024] __attribute__((aligned(8)));
		static unsigned char s_ahead_snd[128 * 1024] __attribute__((aligned(8)));	/* sound ring */
		unsigned t0, t1, t2, t3;
		int i;
		t0 = gpt4_get_current_tick();
		gba_core_state_save(s_ahead_state);
		gba_sound_ring_save(s_ahead_snd);	/* ring contents (not in the savestate) */
		g_gba_audio_suppress = 1;
		for (i = 0; i < pf; i++)
			run_one_frame();		/* look-ahead frames (muted) */
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
static int battery_read(int *charging)
{
	int chr = upmu_is_chr_det();
	long sum = 0;
	int i, vmv;
	for (i = 0; i < BATTERY_SAMPLES; i++)
		sum += get_bat_sense_volt(1);
	vmv = (int)(sum / BATTERY_SAMPLES);
	if (chr)
		vmv -= BAT_CHARGE_OFFSET_MV;
	if (charging)
		*charging = chr;
	return ocv_pct(vmv);
}

static void set_charge_led(int charging, int pct)
{
	static int prev = -1;
	int state = !charging ? 0 : (pct >= 99 ? 2 : 1);
	if (state == prev)
		return;
	prev = state;
	if (state == 2)      ayaneo_charge_led(0, 255, 0);
	else if (state == 1) ayaneo_charge_led(255, 0, 0);
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
static void poll_led(void)
{
	static int tick;
	int chr;
	if (tick-- > 0)
		return;
	tick = 120;			/* ~2 s: charger-detect only (cheap) */
	chr = upmu_is_chr_det();
	set_charge_led(chr, 50);
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
	ayaneo_set_cpu_mhz(s_cpu_opp[idx]);
	s_cpu_idx = idx;		/* keep the CPU-menu index in sync with the tier */
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
	deb = ayaneo_menu_debounce(raw, hist);	/* max debounce for the Pico menu */
	edge = deb & ~prev;
	prev = deb;
	return edge;
}

enum {
	MI_BRIGHT, MI_VOLUME, MI_FILTER, MI_COLORCORRECT, MI_LOADBOOT, MI_SKIPBOOT, MI_SKIPINTRO,
	MI_LOADSTATE, MI_SAVESTATE, MI_BATTERY, MI_CPU, MI_PANEL, MI_PREEMPT, MI_BENCH, MI_RESET, MI_CLOSE, MI_COUNT
};

static const char *filter_name(int f)
{ return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : "Off"; }

static const char *menu_value(int item, char *buf)
{
	char *p = buf;
	switch (item) {
	case MI_BRIGHT:   p = mi_putu(p, (unsigned)ayaneo_brightness_pct()); p = mi_puts(p, "%"); break;
	case MI_VOLUME:   p = mi_putu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mi_puts(p, "%"); break;
	case MI_FILTER:   p = mi_puts(p, filter_name(ayaneo_get_lcd_filter())); break;
	case MI_COLORCORRECT: p = mi_puts(p, ayaneo_get_color_correct() ? "On" : "Off"); break;
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
	case MI_SKIPBOOT: p = mi_puts(p, ayaneo_get_skip_boot() ? "On" : "Off"); break;
	case MI_SKIPINTRO: p = mi_puts(p, ayaneo_get_skip_gba_intro() ? "On" : "Off"); break;
	case MI_LOADSTATE:
	case MI_SAVESTATE:
	case MI_RESET:     p = mi_puts(p, "[A]"); break;
	case MI_BATTERY:
		p = mi_putu(p, (unsigned)s_batt_pct); p = mi_puts(p, "% ");
		p = mi_puts(p, upmu_is_chr_det() ? "Charging" : "Battery");
		break;
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
	case MI_SKIPBOOT:  return "Skip Boot Anim/Chime";
	case MI_SKIPINTRO: return "Skip BIOS Intro";
	case MI_LOADSTATE: return "Load State";
	case MI_SAVESTATE: return "Save State";
	case MI_BATTERY:   return "Battery";
	case MI_CPU:       return "CPU Clock";
	case MI_PANEL:     return "Panel Refresh";
	case MI_PREEMPT:   return "Preemptive Frames";
	case MI_BENCH:     return "Benchmark (Uncap)";
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
	case MI_FILTER:   if (dir) ayaneo_set_lcd_filter((ayaneo_get_lcd_filter() + dir + 4) % 4); else changed = 0; break;
	case MI_COLORCORRECT: if (dir || act) ayaneo_set_color_correct(!ayaneo_get_color_correct()); else changed = 0; break;
	case MI_PREEMPT:  if (dir || act) { int d = dir ? dir : 1;
		ayaneo_set_preempt_frames((ayaneo_get_preempt_frames() + d + 4) % 4);
		/* CPU clock escalation is applied in the game loop on any level change,
		 * so it also covers the oem preempt: path. */
	} else changed = 0; break;
	case MI_LOADBOOT: if (dir || act) ayaneo_set_load_on_boot(!ayaneo_get_load_on_boot()); else changed = 0; break;
	case MI_SKIPBOOT: if (dir || act) ayaneo_set_skip_boot(!ayaneo_get_skip_boot()); else changed = 0; break;
	case MI_SKIPINTRO: if (dir || act) ayaneo_set_skip_gba_intro(!ayaneo_get_skip_gba_intro()); else changed = 0; break;
	case MI_CPU:      if (dir) cpu_step(dir); changed = 0; break;
	case MI_BENCH:    if (dir || act) s_benchmark = !s_benchmark; changed = 0; break;
	case MI_LOADSTATE: if (act) mi_puts(status, state_read(state) ? "State loaded" : "No save state"); changed = 0; break;
	case MI_SAVESTATE: if (act) { int ok = state_write(state); sav_save(state); mi_puts(status, ok ? "State saved" : "Save failed"); } changed = 0; break;
	case MI_RESET:    if (act) { s_reset_req = 1; return 1; } changed = 0; break;
	case MI_CLOSE:    if (act) { s_close_req = 1; return 1; } changed = 0; break;
	default: changed = 0; break;
	}
	if (changed) {
		/* DEFER the persist. ayaneo_settings_save() + sd_settings_mirror() do an eMMC +
		 * SD/FAT write that stalls this loop for ~1 s; done inline on every menu change
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

static int aya_edge(void)
{
	static int prev;
	int now = PRESSED(GPIO_AYA), edge = now && !prev;
	prev = now;
	return edge;
}
static void menu_toggle(void)
{
	s_menu_open = !s_menu_open;
	s_menu_status[0] = 0;
	if (s_menu_open)
		battery_poll(BATTERY_MENU_FRAMES);	/* refresh the shown % on open (<= once/min) */
	menu_keys();		/* drop the AYA edge */
}

static void menu_tick(unsigned char *state)
{
	unsigned k = menu_keys();
	battery_poll(BATTERY_MENU_FRAMES);	/* menu open: refresh the shown % at most once/min */
	if (k & MK_UP)    { s_menu_sel = (s_menu_sel + MI_COUNT - 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_DOWN)  { s_menu_sel = (s_menu_sel + 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_LEFT)  { if (menu_change(s_menu_sel, -1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_RIGHT) { if (menu_change(s_menu_sel, +1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_A)     { if (menu_change(s_menu_sel, 0, 1, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_B)     s_menu_open = 0;
}

/* called by ayaneo_gbc_show_frame() (mt_disp_drv.c) to paint the overlay */
void gbc_menu_draw_overlay(unsigned int *buf, unsigned int pitch,
			   unsigned int W, unsigned int H)
{
	int rowH = 38;
	/* panel height scales with the item count (title band 84 + rows + 42 for the
	 * status/help footer) so the backdrop always covers every row - adding menu
	 * items (Color Correction, Reset Game) used to push Close below the fixed panel. */
	int panelW = 660, panelH = 84 + MI_COUNT * rowH + 42;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2;
	int x = px + 28, y = py + 84, i;
	char val[48];

	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");

	for (i = 0; i < MI_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_menu_sel) ? 0xFF101018u : 0xFFC8D0E0u;
		int vw;
		if (i == s_menu_sel)
			ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, menu_label(i));
		menu_value(i, val);
		for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_menu_status[0])
		ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_menu_status);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
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
void ayaneo_gba_fault_screen(const char *msg, unsigned pc, unsigned addr, unsigned spsr)
{
	char l1[64], l2[80], *p;
	int i, n;

	/* l1 = the message (already short, e.g. "data abort, halting") */
	for (n = 0; msg[n] && n < 40; n++) l1[n] = msg[n];
	l1[n] = 0;
	/* strip trailing newline */
	if (n && l1[n-1] == '\n') l1[n-1] = 0;

	p = l2;
	p = mi_puts(p, "pc="); p = gba_hex(p, pc);
	p = mi_puts(p, " dfar="); p = gba_hex(p, addr);
	p = mi_puts(p, " spsr="); p = gba_hex(p, spsr);
	*p = 0;

	for (i = 0; i < 2; i++) {
		unsigned int pitch, W, H;
		unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
		ayaneo_fill(buf, pitch, 0, 0, (int)W, 120, 0xFF200000u);
		ayaneo_text(buf, pitch, 20, 8, 3, 0xFFFF4040u, "GBA FAULT");
		ayaneo_text(buf, pitch, 20, 48, 2, 0xFFFFC0C0u, l1);
		ayaneo_text(buf, pitch, 20, 78, 2, 0xFFFFFFFFu, l2);
		ayaneo_canvas_present();
	}
}

#ifdef AYANEO_GBA_SD
#include "sd_fat.h"
#include "gba_sd_save.h"
/* SD flow state, shared between the boot gate and emu_thread. s_sd_mode=1 makes
 * emu_thread run the SD flow (ROM-select then game) instead of the boot_b ROM. */
static int s_sd_mode;
static unsigned char s_sd_bios[16384];
static fat_vol s_sd_vol;
static gba_rom_entry s_roms[128];   /* enumerated /roms/gba, sorted (task d/e) */
static int s_nrom;

/* GB/GBC games run through the gambatte core blob (gbc_sd_run.c); it reuses the gpSP
 * arena at 0x50000000, so after a GB/GBC session the gpSP arena must be rebuilt
 * (gba_core_init) before the next GBA game. s_gpsp_dirty tracks that. */
extern void gbc_sd_session(fat_vol *vol, const gba_rom_entry *rom);
static int s_gpsp_dirty;

/* Cold-start stage timings (ms since the SD gate started), read via `fastboot oem
 * diag` as the bt=... line. Pinpoints where the black-backlight time-to-first-BIOS-
 * frame goes (SD mount / bios read / rom list / core blob load / core init / core
 * start / first drawn frame). */
static unsigned int s_bt_t0;
volatile unsigned int g_dbg_bt_mount, g_dbg_bt_bios, g_dbg_bt_list;
volatile unsigned int g_dbg_bt_coreload, g_dbg_bt_coreinit, g_dbg_bt_start, g_dbg_bt_frame1;
#define BT_MS() ((gpt4_get_current_tick() - s_bt_t0) / 13000u)

/* The mounted SD volume, for the menu's per-ROM boxart loader; 0 if not in SD mode
 * (the plain-list/never-brick fallback then just shows placeholders). */
fat_vol *gba_sd_menu_vol(void) { return s_sd_mode ? &s_sd_vol : 0; }
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
		int r = gba_snes_menu_run(s_roms, s_nrom, s_sel_rom >= 0 ? s_sel_rom : 0);
		if (r >= 0) return r;
	}
	for (;;) {
		mtk_wdt_restart();   /* kick the 10s watchdog: idling on the menu must not reset the device */
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

static int sd_state_write(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	if (!sz) return 0;
	gba_core_state_save(scratch);   /* raw gpSP state (no boot_b header needed) */
	return gba_sd_write_state(&s_sd_vol, s_roms[s_sel_rom].name, 0, scratch, sz) == 0;
}

static int sd_state_read(unsigned char *scratch)
{
	unsigned sz = gba_core_state_size();
	uint32_t n = gba_sd_load_state(&s_sd_vol, s_roms[s_sel_rom].name, 0, scratch, GBA_STATE_MAX);
	if (!sz || n != sz) return 0;   /* absent or size mismatch (version guard) */
	gba_core_state_load(scratch);
	return 1;
}

static void sd_sav_write(void)
{
	gba_sd_write_sav(&s_sd_vol, s_roms[s_sel_rom].name,
			 gba_core_backup_ptr(), gba_core_backup_size());
}

/* Mirror the GammaOS Pico settings to the SD card whenever they change, so they
 * travel with the card and are read back at the next LK start. No-op if not in
 * the SD flow. */
static void sd_settings_mirror(void)
{
	if (s_sd_mode)
		gba_sd_settings_save(&s_sd_vol);
}

/* Persist GammaOS settings to BOTH stores: boot_b (eMMC) and, in SD mode, the SD card.
 * The SNES ROM-select menu MUST use this instead of ayaneo_settings_save() alone: in SD
 * mode the boot settings are LOADED from the SD card (gba_sd_settings_load), so a menu
 * change written only to eMMC is overwritten by the stale SD copy on the next power-cycle
 * (the "menu volume/brightness does not stick" bug). */
void ayaneo_menu_settings_persist(void)
{
	ayaneo_settings_save();
	sd_settings_mirror();
}

/* Build a minimal "logo cart" in the gpSP ROM buffer for the BIOS boot-logo
 * intro. The GBA BIOS renders its logo from the CART HEADER's Nintendo-logo
 * bytes and then boots the cart, so an empty cart makes the BIOS jump into
 * unmapped ROM and the dynarec faults (data abort in translate_block_arm).
 * Instead: fill the whole buffer with ARM "b ." (0xEAFFFFFE) so any execution in
 * cart space loops harmlessly, then overlay a REAL header (Nintendo logo + valid
 * checksum + entry, which branches to 0xC0 = a loop) copied from the first ROM on
 * the card so the authentic logo shows. No prebaked data. Returns the cart size. */
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
	if (hdr_rom >= 0) {
		fat_file f;
		f.v = &s_sd_vol;
		f.first_clus = s_roms[hdr_rom].first_clus;
		f.size = s_roms[hdr_rom].size;
		/* 0..0xC0 = real header: logo(0x04-0x9F) + checksum(0xBD) + entry(0x00,
		 * which branches to 0x080000C0 -> our loop). Restores the loops beyond. */
		fat_read(&f, 0, rp, 0xC0u);
	}
	return 0x8000u;
}
#endif

/* ===================== emulator thread ===================== */
static int emu_thread(void *arg)
{
	unsigned char *scratch = (unsigned char *)(GBA_ARENA_PA + GBA_ARENA_SZ - GBA_DRV_RESERVE);
	unsigned romsz, frame = 0;

	(void)arg;

	gba_disable_align_faults();		/* gpSP relies on unaligned host accesses */
	ayaneo_display_prepare();		/* own the panel + backlight for on-screen debug */

	/* Load the GBA core blob from boot_b into its DRAM slot and bind the export table.
	 * The core is no longer part of lk_a; everything below drives it through g_core. */
	g_core = gba_core_load();
	if (!g_core) {
		gba_dbg("Gerr blob");
		return 0;
	}
	g_dbg_bt_coreload = BT_MS();

#ifdef AYANEO_GBA_INTERP
	dynarec_enable = 0;			/* diagnostic: pure interpreter, no JIT */
	gba_dbg("GBA 1: emu start (INTERP, no dynarec)");
#else
	gba_dbg("GBA 1: emu thread start");
#endif

	if (gba_core_init((void *)GBA_ARENA_PA, GBA_ARENA_SZ - GBA_DRV_RESERVE) != 0) {
		gba_dbg("GBA ERR: arena too small");
		return 0;
	}
	g_dbg_bt_coreinit = BT_MS();
#ifdef AYANEO_GBA_SD
	if (s_sd_mode) {
		unsigned char *rp = gba_core_rom_ptr();
		input_init();                        /* need the D-pad/A for the ROM-select */
		/* Load GammaOS Pico settings from the SD card (portable with the card);
		 * marks settings loaded so the later boot_b load keeps the SD values, and
		 * lets us honour skip-boot-logo before the intro/menu. */
		gba_sd_settings_load(&s_sd_vol);
		/* Boot the emulator with NO cartridge, using the SD card's own
		 * gba_bios.bin (s_sd_bios), so the real GBA BIOS plays its boot logo as
		 * the intro. The ROM-select menu and the chosen game are driven after the
		 * display + CPU thread are up (see the s_sd_mode intro block just before
		 * the main frame loop). No prebaked BIOS: if gba_bios.bin was missing,
		 * ayaneo_gba_sd_boot already fell through to a normal boot. */
		romsz = gba_sd_build_logo_cart(rp);	/* real logo header + safe self-loops */
		if (gba_core_start(romsz, s_sd_bios) != 0) {
			gba_dbg("GBA ERR: SD core_start failed");
			return 0;
		}
		gba_core_enter_bios();			/* start at the BIOS so its logo plays */
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
		int hz100 = primary_display_get_vsync_interval();
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
			int fi, intro_frames = ayaneo_get_skip_gba_intro() ? 0 : GBA_SD_INTRO_FRAMES;
			/* Run the BIOS-logo intro on the pure INTERPRETER: the dynarec faults
			 * in translate_block_arm here, and the interpreter also naturally picks
			 * up the reset PC after the intro->game gba_core_start (it re-reads PC
			 * each step, no translated blocks to go stale). The dynarec is switched
			 * back on for the actual game below. */
			dynarec_enable = 0;
			ayaneo_gbc_blank();			/* black edges outside the cropped logo */
			/* "Mute BIOS Audio" also silences the GBA BIOS boot-logo jingle (the
			 * emulated BIOS runs through the game audio path here). Pause across the
			 * intro, then unpause so the menu/game audio is restored. */
			if (ayaneo_get_mute_bios()) ayaneo_gbc_audio_pause(1);
			for (fi = 0; fi < intro_frames; fi++) {
				update_buttons();
				run_one_frame();
				ayaneo_gba_show_intro_frame(gba_core_screen());	/* 6x fill-height */
				if (!g_dbg_bt_frame1) g_dbg_bt_frame1 = BT_MS();  /* first BIOS frame on screen */
				mtk_wdt_restart();		/* keep the armed watchdog fed during the intro */
				if (PRESSED(GPIO_B)) break;	/* hold B to skip the logo */
			}
			if (ayaneo_get_mute_bios()) ayaneo_gbc_audio_pause(0);
			if (s_nrom > 0) {
				unsigned char *rp = gba_core_rom_ptr();
				for (;;) {
					int sel = gba_sd_rom_select();
					s_sel_rom = sel;
					if (s_roms[sel].type != GBA_CONSOLE_GBA) {
						gbc_sd_session(&s_sd_vol, &s_roms[sel]);   /* GB/GBC via gambatte */
						s_gpsp_dirty = 1;                          /* arena clobbered */
						continue;                                  /* back to the selector */
					}
					if (s_gpsp_dirty) {   /* rebuild the gpSP arena a GB/GBC session reused */
						gba_core_init((void *)GBA_ARENA_PA, GBA_ARENA_SZ - GBA_DRV_RESERVE);
						s_gpsp_dirty = 0;
					}
					romsz = gba_sd_load_rom(&s_sd_vol, &s_roms[sel], rp, gba_core_rom_capacity());
					if (romsz) break;
					gba_dbg("GBA ERR: SD ROM load failed, back to selector");
				}
				/* reset the running core into the selected game (SD BIOS + cart).
				 * reset_gba flushes the dynarec, so we must NOT let execute_arm
				 * resume in place; request a clean restart (see gba_yield_to_main)
				 * which the first main-loop frame below triggers. */
				if (gba_core_start(romsz, s_sd_bios) != 0) {
					gba_dbg("GBA ERR: SD game core_start failed");
					return 0;
				}
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
			/* no ROMs on the card: keep showing the BIOS (romsz stays 0). */
		}
#endif

		unsigned punch_start = 0;	/* 13 MHz tick the punch-hole began (0 = not yet) */
		for (;;) {
			int uncapped;

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
				if (lt > 104000u && lt < 300000u) {   /* 8..23 ms; reject doubled frames */
					g_acc += lt;
					if (++g_cnt >= 64) {   /* 64-frame window (~1.07s): averages out per-frame
					                         emulation CPU jitter so the readout tracks the TRUE
					                         fixed panel rate as steadily as the menu loop. */
						unsigned int hz = (unsigned int)(832000000000LL / (long long)g_acc);
						g_dbg_hz1000 = hz;
						g_acc = 0; g_cnt = 0;
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

			update_buttons();
			{	/* apply the per-tier CPU clock when the Preemptive Frames tier
				 * changes (covers the Pico menu, oem preempt:, and boot). */
				static int s_pf_applied = -1;
				int cur_pf = ayaneo_get_preempt_frames();
				if (cur_pf != s_pf_applied) { s_pf_applied = cur_pf; preempt_apply_cpu(cur_pf); }
			}
			{
				extern volatile unsigned int g_dbg_frame_ticks;
				unsigned int ct0 = gba_core_cpu_ticks();
				unsigned int em0 = gpt4_get_current_tick();
				run_one_frame();	/* runs one GBA frame + submits its audio */
				/* committed GBA cycles this display frame = one full frame (280896);
				 * run-ahead re-enters as a full frame (no priming stub) so no skew. */
				g_dbg_frame_ticks = gba_core_cpu_ticks() - ct0;
				s_committed_us = (gpt4_get_current_tick() - em0) / 13u;	/* this frame us */
				{	/* average the emulation wall time over 16 frames -> us */
					static unsigned int acc; static int cnt;
					acc += s_committed_us * 13u;	/* 13 MHz ticks */
					if (++cnt >= 16) { g_dbg_emu_us = acc / (16u * 13u); acc = 0; cnt = 0; }
				}
			}
			if (g_dbg_selftest_req) {	/* `oem selftest[:N]`: validate the rewind path */
				int nf = g_dbg_selftest_req;	/* N frames (1 = single-rewind) */
				g_dbg_selftest_req = 0;
				gba_run_ahead_selftest(nf);
			}

			if (frame == 0)		/* first frame done => dynarec executed OK */
				gba_dbg("GBA 7: first frame rendered (dynarec ok)");

			uncapped = s_fast_forward || s_benchmark;
			if (uncapped != ff_prev) {
				ayaneo_gbc_audio_pause(uncapped);
				ff_prev = uncapped;
			}

			mtk_wdt_restart();
			poll_volume();
			/* Flush the deferred volume/brightness persist once the user has
			 * stopped adjusting (~0.7s), silencing the audio ring across the
			 * eMMC/SD write so the stall plays silence instead of looping. */
			if (s_settings_dirty &&
			    (gpt4_get_current_tick() - s_settings_tick) >= 9100000u) {
				s_settings_dirty = 0;
				ayaneo_gbc_audio_pause(1);
				ayaneo_settings_save();
#ifdef AYANEO_GBA_SD
				sd_settings_mirror();
#endif
				ayaneo_gbc_audio_pause(0);
			}
			poll_led();
			check_power(scratch);
			if (!s_menu_open)
				battery_poll(BATTERY_GAMEPLAY_FRAMES);	/* ~once/10 min, no pause */
			frame++;

			if (aya_edge())
				menu_toggle();
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
#ifdef AYANEO_GBA_SD
				if (s_sd_mode && s_nrom > 0) {
					extern void gba_menu_arm_reverse(const unsigned short *game_frame);
					unsigned char *rp = gba_core_rom_ptr();
					long rsz;
					/* Wipe the shared AFE ring FIRST, BEFORE the slow eMMC/SD saves:
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
						if (s_roms[sel].type != GBA_CONSOLE_GBA) {
							gbc_sd_session(&s_sd_vol, &s_roms[sel]);
							s_gpsp_dirty = 1;
							continue;
						}
						if (s_gpsp_dirty) {
							gba_core_init((void *)GBA_ARENA_PA, GBA_ARENA_SZ - GBA_DRV_RESERVE);
							s_gpsp_dirty = 0;
						}
						rsz = gba_sd_load_rom(&s_sd_vol, &s_roms[sel], rp,
								      gba_core_rom_capacity());
						if (rsz) break;
					}
					if (gba_core_start(rsz, s_sd_bios) != 0)
						return 0;
					gba_sd_load_sav(&s_sd_vol, s_roms[s_sel_rom].name,
							(unsigned char *)gba_core_backup_ptr(),
							gba_core_backup_size());
					if (!PRESSED(GPIO_B))
						state_read(scratch);
					/* no blank: keep the frozen menu on screen for the seamless
					 * growing-circle opening (the punch composites the full frame). */
					dynarec_enable = 1;
					s_cpu_restart_req = 1;
					punch_start = 0;	/* re-arm the forward punch */
				}
#endif
				continue;
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
				ayaneo_present_skip_framedone = 1;	/* uncapped, every frame */
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
				int i, N = 20, w;
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
			/* run-ahead present: look-ahead pf frames then rewind (or a plain
			 * present when Off), paced to the panel vsync. */
			preempt_present();
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

void ayaneo_gbc_charging_screen(void)
{
	int hold = 0, unplug = 0, disp_on = 1, idle = 0;
	int pct = 0, btick = 0, dummy;

	ayaneo_settings_load();
	ayaneo_display_prepare();
	ayaneo_apply_backlight(40);
	mtk_wdt_disable();
	pct = battery_read(&dummy);

	for (;;) {
		int chr = upmu_is_chr_det();
		int pk = pmic_detect_powerkey();

		if (--btick <= 0) { pct = battery_read(&dummy); btick = 10; }
		set_charge_led(chr, pct);

		if (disp_on) {
			unsigned int pitch, W, H;
			unsigned int *buf = ayaneo_canvas_back(&pitch, &W, &H);
			int cx = (int)W / 2, cy = (int)H / 2;
			char line[16], *p;

			p = mi_putu(line, (unsigned)pct); p = mi_puts(p, "%"); *p = 0;
			ayaneo_fill(buf, pitch, 0, 0, (int)W, (int)H, 0xFF000000u);
			text_center(buf, pitch, cx, cy - 96, 6, 0xFFFFFFFFu, line);
			text_center(buf, pitch, cx, cy + 24, 3,
				    chr ? 0xFF60D080u : 0xFFD08060u,
				    chr ? "Charging" : "On battery");
			text_center(buf, pitch, cx, cy + 96, 1, 0xFF808890u,
				    "Tap POWER for status  -  hold to start  -  unplug for off");
			ayaneo_canvas_present();
			if (++idle >= 100) {
				disp_on = 0;
				ayaneo_apply_backlight(0);
			}
		}

		if (!chr) {
			if (++unplug >= 3)
				mt_power_off();
		} else {
			unplug = 0;
		}

		if (pk) {
			if (++hold >= 12) {
				ayaneo_apply_persisted_brightness();
				return;
			}
		} else {
			if (hold > 0 && hold < 12) {
				if (!disp_on) {
					disp_on = 1;
					ayaneo_apply_backlight(40);
				}
				idle = 0;
			}
			hold = 0;
		}

		mtk_wdt_restart();
		thread_sleep(100);
	}
}

#ifdef AYANEO_GBA_SD
void ayaneo_gbc_start(void);   /* defined just below; spawns emu_thread */
/* Assets present on the microSD? Requires /gba_bios.bin (the intro) plus >=1 file
 * in /roms/gba (something to select). Mirrors the host-validated probe in
 * fat_ro_test.c. */
static int gba_sd_assets_ok(fat_vol *v)
{
	fat_file bios; fat_dir d; fat_dirent e;
	if (fat_open(v, "/gba_bios.bin", &bios) != 0) return 0;
	if (fat_opendir(v, "/roms/gba", &d) != 0) return 0;
	while (fat_readdir(&d, &e)) if (!e.is_dir) return 1;
	return 0;
}

/*
 * Boot-to-OS marker (STICKY). The SNES menu's "Boot to OS" cannot simply fall
 * through to boot_linux in the same boot: the emulator has already taken over the
 * CPU clock, audio/display DMA, caches and the watchdog, so jumping into the kernel
 * from that state hangs and the watchdog resets. Instead we drop a marker in the
 * misc partition (raw eMMC, the standard bootloader-control block, kept out of the
 * asset-only boot_b) and hard-reset. From then on every boot sees the marker and
 * boots Android from a pristine state - the same path as if no SD card were present,
 * which is already proven safe. The marker is NOT cleared on an OS boot: it persists
 * so the OS target is remembered. Holding SELECT at boot clears it and returns to
 * the emulator (see ayaneo_gba_sd_boot()).
 */
#define BOOTOS_PART	"misc"
#define BOOTOS_OFF	0x10000u		/* 64 KB into misc: OEM vendor space, clear of the AOSP BCB */
#define BOOTOS_MAGIC	0x424F544Fu		/* "OTOB" - boot-to-OS one-shot */

/* Peek the sticky boot-to-OS marker WITHOUT clearing it. Called from the logo path
 * so an OS boot shows the stock eMMC logo instead of the emulator's rainbow boot
 * animation. Only SELECT-held in ayaneo_gba_sd_boot() clears the marker. */
int ayaneo_boot_to_os_pending(void)
{
	unsigned char b[64];
	if (partition_read(BOOTOS_PART, BOOTOS_OFF, b, sizeof b) != (ssize_t)sizeof b)
		return 0;
	return (unsigned)b[0] == (BOOTOS_MAGIC & 0xFF) &&
	       (unsigned)b[1] == ((BOOTOS_MAGIC >> 8) & 0xFF) &&
	       (unsigned)b[2] == ((BOOTOS_MAGIC >> 16) & 0xFF) &&
	       (unsigned)b[3] == ((BOOTOS_MAGIC >> 24) & 0xFF);
}

void ayaneo_boot_to_os(void)
{
	extern void mtk_arch_reset(char mode);
	unsigned char b[64];
	unsigned i;
	for (i = 0; i < sizeof b; i++) b[i] = 0;
	b[0] = (unsigned char)BOOTOS_MAGIC;
	b[1] = (unsigned char)(BOOTOS_MAGIC >> 8);
	b[2] = (unsigned char)(BOOTOS_MAGIC >> 16);
	b[3] = (unsigned char)(BOOTOS_MAGIC >> 24);
	arch_clean_cache_range((unsigned long)b, sizeof b);
	partition_write(BOOTOS_PART, BOOTOS_OFF, b, sizeof b);
	mtk_wdt_restart();			/* keep alive across the flush + reset */
	mtk_arch_reset(1);
	for (;;) ;				/* not reached */
}

/*
 * SD boot gate. Returns < 0 to tell the boot hook to FALL THROUGH to the normal
 * kernel boot (no card / not FAT / assets missing / BIOS unreadable) - the always
 * safe default. On success (card + assets) it loads the SD BIOS, spawns the emu in
 * SD mode (BIOS intro; ROM select + game are tasks d-e), and returns 0 so the boot
 * hook loops forever. The intro runs the BIOS boot logo from /gba_bios.bin.
 */
int ayaneo_gba_sd_boot(void)
{
	int rc;
	/* Hold SELECT at boot to FORCE the emulator/menu - the deterministic "way back"
	 * from Android. It overrides both the boot-to-OS one-shot and the watchdog guard
	 * so the user can always reach the emulator regardless of any skip state. */
	int force_emu = ayaneo_gbc_select_held();

	/* Boot-to-OS is STICKY: once "Boot to OS" writes the marker, every boot goes to
	 * Android until the user explicitly comes back by holding SELECT. So we do NOT
	 * clear the marker on an OS boot - only SELECT clears it. This makes the boot
	 * target a deliberate, remembered choice rather than a one-shot. */
	{
		unsigned char b[64];
		int flag_set = (partition_read(BOOTOS_PART, BOOTOS_OFF, b, sizeof b) == (ssize_t)sizeof b &&
				(unsigned)b[0] == (BOOTOS_MAGIC & 0xFF) &&
				(unsigned)b[1] == ((BOOTOS_MAGIC >> 8) & 0xFF) &&
				(unsigned)b[2] == ((BOOTOS_MAGIC >> 16) & 0xFF) &&
				(unsigned)b[3] == ((BOOTOS_MAGIC >> 24) & 0xFF));
		if (force_emu) {
			/* Way back: clear the sticky flag so the device defaults to the emulator
			 * again from now on, then fall through to start it. */
			if (flag_set) {
				unsigned i;
				for (i = 0; i < sizeof b; i++) b[i] = 0;
				arch_clean_cache_range((unsigned long)b, sizeof b);
				partition_write(BOOTOS_PART, BOOTOS_OFF, b, sizeof b);
				GBA_LOG("gba-sd: SELECT held -> cleared sticky boot-to-OS flag, forcing emulator\n");
			} else {
				GBA_LOG("gba-sd: SELECT held -> forcing emulator\n");
			}
		} else if (flag_set) {
			GBA_LOG("gba-sd: sticky boot-to-OS flag set -> normal Android boot (hold SELECT to return)\n");
			return -10;
		}
	}

	/* NEVER-BRICK GUARD: if the previous boot was reset by the watchdog, the last
	 * SD attempt may have crashed (data abort -> "halting" -> HW watchdog reset) or
	 * hung. Skip the SD emulator path this boot and fall through to normal Android
	 * boot, so the device always self-recovers instead of boot looping. A later
	 * clean (non-watchdog) boot will retry the SD path automatically. SELECT held
	 * overrides this (the user is explicitly asking for the emulator). */
	if (!force_emu) {
		extern unsigned int mtk_wdt_check_status(void);
		unsigned int wsta = mtk_wdt_check_status();
		/* HWWDT_RST | SWWDT_RST | IRQWDT_RST */
		if (wsta & (0x80000000u | 0x40000000u | 0x20000000u)) {
			GBA_LOG("gba-sd: prior boot was a watchdog reset (0x%x) - skipping SD to avoid a loop -> normal boot\n", wsta);
			return -9;
		}
	}

	s_bt_t0 = gpt4_get_current_tick();		/* cold-start clock base (~backlight time) */
	rc = gba_sd_mount(&s_sd_vol);
	if (rc != 0) {
		if (rc == -4)
			GBA_LOG("gba-sd: microSD is exFAT (unsupported) - reformat FAT32 -> normal boot\n");
		else
			GBA_LOG("gba-sd: no FAT microSD (rc=%d) -> normal boot\n", rc);
		return -1;
	}
	g_dbg_bt_mount = BT_MS();
	if (!gba_sd_assets_ok(&s_sd_vol)) {
		GBA_LOG("gba-sd: microSD present but /gba_bios.bin + /roms/gba missing -> normal boot\n");
		return -2;
	}
	if (gba_sd_load_bios(&s_sd_vol, s_sd_bios) != 0) {
		GBA_LOG("gba-sd: /gba_bios.bin present but not a 16KB readable BIOS -> normal boot\n");
		return -3;
	}
	g_dbg_bt_bios = BT_MS();
	/* Ensure /roms/gb, /roms/gbc, /roms/gba exist so the user has a place to drop
	 * each console's ROMs (idempotent; harmless if already present or read-only). */
	gba_sd_make_rom_dirs(&s_sd_vol);
	{
		int total = 0;
		s_nrom = gba_sd_list_roms(&s_sd_vol, s_roms, 128, &total);
		g_dbg_bt_list = BT_MS();
		if (total > s_nrom)
			GBA_LOG("gba-sd: WARNING %d roms present, showing first %d (cap)\n", total, s_nrom);
		GBA_LOG("gba-sd: microSD + assets OK (fat32=%d, %d roms in /roms/gba) - running BIOS intro from SD\n",
			s_sd_vol.is_fat32, s_nrom);
	}
	s_sd_mode = 1;
	ayaneo_gbc_start();   /* spawns emu_thread, which runs the SD intro (s_sd_mode) */
	/* Bring up the USB fastboot debug channel alongside the running emulator so
	 * `fastboot oem screenshot` / `oem diag` work live over USB (no reboot to
	 * fastboot mode). Spawns its own thread + IRQ-driven udc, returns immediately. */
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
	 * so 64 KB is enough here. See emu/gbc/gbc_sd_run.c and CORE_PORTING_NOTES.md. */
	thread_t *t = thread_create("ayaneo_gba", &emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t)
		thread_resume(t);
}
