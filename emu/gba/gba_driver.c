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

/* ---- gpSP core bridge (emu/gba/gba_wrap.c) ---- */
extern int  gba_core_init(void *arena, unsigned size);
extern unsigned char *gba_core_rom_ptr(void);
extern unsigned gba_core_rom_capacity(void);
extern unsigned char *gba_core_scratch_ptr(void);
extern unsigned gba_core_scratch_size(void);
extern int  gba_core_start(unsigned romsz, const void *bios16k);
extern void gba_core_enter_bios(void);	/* run the BIOS boot logo (start PC at 0x0) */
extern void gba_core_cpu_loop(void);
extern void gba_core_pre_frame(void);
extern void gba_core_post_frame(void);
extern const unsigned short *gba_core_screen(void);
extern void gba_core_set_keys(unsigned gba_mask);
extern void *gba_core_backup_ptr(void);
extern unsigned gba_core_backup_size(void);
extern unsigned gba_core_state_size(void);
extern void gba_core_state_save(void *buf);
extern void gba_core_state_load(const void *buf);
extern int  dynarec_enable;	/* gpSP global (gba_wrap.c); 0 = pure interpreter */

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
extern int  mt_get_gpio_in(unsigned pin);
extern int  mtk_detect_key(unsigned short hwkey);

#include "gba_bios_data.h"

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
#define GPIO_X		84	/* autofire A */
#define GPIO_Y		85	/* autofire B */
#define GPIO_R2		57	/* key_rc / second-stage right trigger = fast-forward */
#define GPIO_AYA	86	/* menu */
#define AUTOFIRE_HZ	12

static const struct { unsigned gpio; unsigned mask; } s_btn[] = {
	{ GPIO_UP, GB_UP }, { GPIO_DOWN, GB_DOWN }, { GPIO_LEFT, GB_LEFT }, { GPIO_RIGHT, GB_RIGHT },
	{ GPIO_A, GB_A },   { GPIO_B, GB_B },
	{ GPIO_START, GB_START }, { GPIO_SELECT, GB_SEL },
	{ GPIO_LB, GB_L },  { GPIO_RB, GB_R },
};

static void gpio_in_pullup(unsigned gpio)
{
	mt_set_gpio_mode(GP(gpio), 0);
	mt_set_gpio_dir(GP(gpio), 0);
	mt_set_gpio_pull_enable(GP(gpio), 1);
	mt_set_gpio_pull_select(GP(gpio), 1);	/* pull-up */
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

/* refresh the button state once per frame (debounced across two reads) */
static void update_buttons(void)
{
	static unsigned prev_raw;
	unsigned raw = 0, deb, m, i;
	int af;

	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		if (PRESSED(s_btn[i].gpio))
			raw |= s_btn[i].mask;
	if (PRESSED(GPIO_X))  raw |= RB_X;
	if (PRESSED(GPIO_Y))  raw |= RB_Y;
	if (PRESSED(GPIO_R2)) raw |= RB_FF;

	deb = raw & prev_raw;
	prev_raw = raw;

	m = deb & 0x3ffu;			/* the 10 GBA buttons */

	if (s_sel_modifier)
		m &= ~GB_SEL;			/* Select drives brightness, not the game */

	af = ((unsigned)current_time() * AUTOFIRE_HZ / 500u) & 1;
	if (af) {
		if (deb & RB_X) m |= GB_A;	/* X = autofire A */
		if (deb & RB_Y) m |= GB_B;	/* Y = autofire B */
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
static int battery_read(int *charging)
{
	int chr = upmu_is_chr_det();
	long sum = 0;
	int i, vmv;
	for (i = 0; i < 16; i++)
		sum += get_bat_sense_volt(1);
	vmv = (int)(sum / 16);
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
 * PMIC read, done ~every 2 s; get_bat_sense_volt() is a slow auxadc conversion
 * (it caused a ~1 s frame hitch while charging), so the near-full check is done
 * rarely (~every 30 s) and its result cached. */
static void poll_led(void)
{
	static int tick, vtick, full_cached;
	int chr;
	if (tick-- > 0)
		return;
	tick = 120;			/* ~2 s: charger-detect only (cheap) */
	chr = upmu_is_chr_det();
	if (chr && --vtick <= 0) {	/* ~every 30 s while charging: one auxadc read */
		vtick = 15;		/* 15 * 2 s */
		full_cached = get_bat_sense_volt(1) >= 4300;
	}
	if (!chr)
		vtick = 0;		/* re-read promptly on next charge */
	set_charge_led(chr, (chr && full_cached) ? 100 : 50);
}

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

enum { MK_UP=1, MK_DOWN=2, MK_LEFT=4, MK_RIGHT=8, MK_A=16, MK_B=32, MK_AYA=64 };
unsigned menu_keys(void)	/* exported for gba_menu.c (carousel) */
{
	static unsigned prev;
	unsigned raw = 0, edge;
	if (PRESSED(GPIO_UP))    raw |= MK_UP;
	if (PRESSED(GPIO_DOWN))  raw |= MK_DOWN;
	if (PRESSED(GPIO_LEFT))  raw |= MK_LEFT;
	if (PRESSED(GPIO_RIGHT)) raw |= MK_RIGHT;
	if (PRESSED(GPIO_A))     raw |= MK_A;
	if (PRESSED(GPIO_B))     raw |= MK_B;
	if (PRESSED(GPIO_AYA))   raw |= MK_AYA;
	edge = raw & ~prev;
	prev = raw;
	return edge;
}

enum {
	MI_BRIGHT, MI_VOLUME, MI_FILTER, MI_LOADBOOT, MI_SKIPBOOT, MI_SKIPINTRO,
	MI_LOADSTATE, MI_SAVESTATE, MI_BATTERY, MI_CPU, MI_PANEL, MI_BENCH, MI_CLOSE, MI_COUNT
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
	case MI_LOADBOOT: p = mi_puts(p, ayaneo_get_load_on_boot() ? "On" : "Off"); break;
	case MI_SKIPBOOT: p = mi_puts(p, ayaneo_get_skip_boot() ? "On" : "Off"); break;
	case MI_SKIPINTRO: p = mi_puts(p, ayaneo_get_skip_gba_intro() ? "On" : "Off"); break;
	case MI_LOADSTATE:
	case MI_SAVESTATE: p = mi_puts(p, "[A]"); break;
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
	case MI_PANEL:
		/* measured refresh vs the GBA's 59.7275 Hz (target 5973) */
		p = mi_putu(p, (unsigned)(s_panel_hz100 / 100)); *p++ = '.';
		{ unsigned f = (unsigned)(s_panel_hz100 % 100); if (f < 10) *p++ = '0'; p = mi_putu(p, f); }
		p = mi_puts(p, " Hz");
		break;
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
	case MI_LOADBOOT:  return "Load State on Boot";
	case MI_SKIPBOOT:  return "Skip Boot Anim/Chime";
	case MI_SKIPINTRO: return "Skip BIOS Intro";
	case MI_LOADSTATE: return "Load State";
	case MI_SAVESTATE: return "Save State";
	case MI_BATTERY:   return "Battery";
	case MI_CPU:       return "CPU Clock";
	case MI_PANEL:     return "Panel Refresh";
	case MI_BENCH:     return "Benchmark (Uncap)";
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
	case MI_LOADBOOT: if (dir || act) ayaneo_set_load_on_boot(!ayaneo_get_load_on_boot()); else changed = 0; break;
	case MI_SKIPBOOT: if (dir || act) ayaneo_set_skip_boot(!ayaneo_get_skip_boot()); else changed = 0; break;
	case MI_SKIPINTRO: if (dir || act) ayaneo_set_skip_gba_intro(!ayaneo_get_skip_gba_intro()); else changed = 0; break;
	case MI_CPU:      if (dir) cpu_step(dir); changed = 0; break;
	case MI_BENCH:    if (dir || act) s_benchmark = !s_benchmark; changed = 0; break;
	case MI_LOADSTATE: if (act) mi_puts(status, state_read(state) ? "State loaded" : "No save state"); changed = 0; break;
	case MI_SAVESTATE: if (act) { int ok = state_write(state); sav_save(state); mi_puts(status, ok ? "State saved" : "Save failed"); } changed = 0; break;
	case MI_CLOSE:    if (act) { s_close_req = 1; return 1; } changed = 0; break;
	default: changed = 0; break;
	}
	if (changed) {
		ayaneo_settings_save();
#ifdef AYANEO_GBA_SD
		sd_settings_mirror();
#endif
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
	if (s_menu_open) {
		int c;
		s_batt_pct = battery_read(&c);
	}
	menu_keys();		/* drop the AYA edge */
}

static void menu_tick(unsigned char *state)
{
	unsigned k = menu_keys();
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
	int panelW = 660, panelH = 620;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2;
	int rowH = 38, x = px + 28, y = py + 84, i;
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
	for (j = 0; j < n; j++)
		w[j] = 0xEAFFFFFEu;			/* b . everywhere = safe self-loop */
	if (s_nrom > 0) {
		fat_file f;
		f.v = &s_sd_vol;
		f.first_clus = s_roms[0].first_clus;
		f.size = s_roms[0].size;
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
		if (gba_core_start(romsz, gba_bios_data) != 0) {
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

	/* Default to the LOWEST OPP: the dynarec is ~300 fps-capable even at 600 MHz,
	 * and the emulator is vsync-locked to ~59.73 fps, so the lowest clock sustains
	 * full speed while minimising power/heat. User can raise it via the AYA menu. */
	ayaneo_set_cpu_mhz(600);

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
			for (fi = 0; fi < intro_frames; fi++) {
				update_buttons();
				run_one_frame();
				ayaneo_gba_show_intro_frame(gba_core_screen());	/* 6x fill-height */
				mtk_wdt_restart();		/* keep the armed watchdog fed during the intro */
				if (PRESSED(GPIO_B)) break;	/* hold B to skip the logo */
			}
			if (s_nrom > 0) {
				unsigned char *rp = gba_core_rom_ptr();
				for (;;) {
					int sel = gba_sd_rom_select();
					s_sel_rom = sel;
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
				ayaneo_gbc_blank();	/* wipe menu/BIOS pixels before the game draws */
				dynarec_enable = 1;	/* full-speed dynarec for the actual game */
				s_cpu_restart_req = 1;	/* CPU thread re-enters gba_core_cpu_loop cleanly */
			}
			/* no ROMs on the card: keep showing the BIOS (romsz stays 0). */
		}
#endif

		unsigned punch_start = 0;	/* 13 MHz tick the punch-hole began (0 = not yet) */
		for (;;) {
			int uncapped;

			update_buttons();
			run_one_frame();		/* runs one GBA frame + submits its audio */
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
			frame++;

			if (aya_edge())
				menu_toggle();
			if (s_menu_open)
				menu_tick(scratch);

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
					state_write(scratch);		/* persist the current game */
					sav_save(scratch);
					/* Wipe the shared AFE ring so it does not loop the last game
					 * frames while the menu re-inits (mirror of the launch handoff).
					 * Do NOT call ayaneo_gbc_audio_pause(1) here: it latches
					 * s_gbc_paused=1, and the submit path early-returns while paused,
					 * which would permanently mute the menu BGM and every game launched
					 * afterwards. The silence() wipe alone stops the loop; the menu then
					 * re-inits the codec and plays its BGM (exactly like first boot). */
					ayaneo_menu_audio_silence();
					gba_menu_arm_reverse(gba_core_screen());  /* freeze frame for reverse */
					for (;;) {
						int sel = gba_sd_rom_select();
						s_sel_rom = sel;
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
					ayaneo_gbc_blank();
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
				unsigned now = gpt4_get_current_tick();
				unsigned el;
				int r;
				if (!punch_start) punch_start = now ? now : 1u;
				el = now - punch_start;
				if (el >= GBA_PUNCH_TICKS) {		/* time up -> full gameplay */
					gba_punch_ready = 0;
					/* The punch left the menu snapshot in the 40/80px letterbox that
					 * normal show_frame never redraws. Clear ONLY the letterbox (both
					 * buffers) - a full ayaneo_gbc_blank() here memsets the LIVE buffer
					 * black = a one-frame black flash = the menu->game transition
					 * flicker the user saw. The game area is already full gameplay. */
					{
						extern void ayaneo_gbc_clear_letterbox(void);
						ayaneo_gbc_clear_letterbox();
					}
					ayaneo_gbc_show_frame(gba_core_screen());
					continue;
				}
				r = (int)((long long)GBA_PUNCH_MAX_R * el / GBA_PUNCH_TICKS);
				if (r < 1) r = 1;
				ayaneo_gba_punch_frame(gba_core_screen(),
						       (const unsigned int *)GBA_PUNCH_SNAP_PA,
						       -1, -1, r);
				continue;
			}
			ayaneo_gbc_show_frame(gba_core_screen());
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
 * SD boot gate. Returns < 0 to tell the boot hook to FALL THROUGH to the normal
 * kernel boot (no card / not FAT / assets missing / BIOS unreadable) - the always
 * safe default. On success (card + assets) it loads the SD BIOS, spawns the emu in
 * SD mode (BIOS intro; ROM select + game are tasks d-e), and returns 0 so the boot
 * hook loops forever. The intro runs the BIOS boot logo from /gba_bios.bin.
 */
int ayaneo_gba_sd_boot(void)
{
	int rc;

	/* NEVER-BRICK GUARD: if the previous boot was reset by the watchdog, the last
	 * SD attempt may have crashed (data abort -> "halting" -> HW watchdog reset) or
	 * hung. Skip the SD emulator path this boot and fall through to normal Android
	 * boot, so the device always self-recovers instead of boot looping. A later
	 * clean (non-watchdog) boot will retry the SD path automatically. */
	{
		extern unsigned int mtk_wdt_check_status(void);
		unsigned int wsta = mtk_wdt_check_status();
		/* HWWDT_RST | SWWDT_RST | IRQWDT_RST */
		if (wsta & (0x80000000u | 0x40000000u | 0x20000000u)) {
			GBA_LOG("gba-sd: prior boot was a watchdog reset (0x%x) - skipping SD to avoid a loop -> normal boot\n", wsta);
			return -9;
		}
	}

	rc = gba_sd_mount(&s_sd_vol);
	if (rc != 0) {
		if (rc == -4)
			GBA_LOG("gba-sd: microSD is exFAT (unsupported) - reformat FAT32 -> normal boot\n");
		else
			GBA_LOG("gba-sd: no FAT microSD (rc=%d) -> normal boot\n", rc);
		return -1;
	}
	if (!gba_sd_assets_ok(&s_sd_vol)) {
		GBA_LOG("gba-sd: microSD present but /gba_bios.bin + /roms/gba missing -> normal boot\n");
		return -2;
	}
	if (gba_sd_load_bios(&s_sd_vol, s_sd_bios) != 0) {
		GBA_LOG("gba-sd: /gba_bios.bin present but not a 16KB readable BIOS -> normal boot\n");
		return -3;
	}
	{
		int total = 0;
		s_nrom = gba_sd_list_roms(&s_sd_vol, s_roms, 128, &total);
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
	thread_t *t = thread_create("ayaneo_gba", &emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t)
		thread_resume(t);
}
