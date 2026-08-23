/*
 * LK-side driver for the gambatte GBC core. Carves a DRAM arena, loads the ROM
 * from boot_b, and runs the emulation loop: each frame is handed to the display
 * path (mt_disp_drv) and, later, audio is streamed to the AFE.
 *
 * Runs entirely in LK after the boot animation; there is no kernel handoff.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <part_interface.h>

/* per-frame logging over UART is slow; keep it only in debug builds */
#ifdef AYANEO_DEBUG_LOGGING
#define GBC_LOG(...)	dprintf(CRITICAL, __VA_ARGS__)
#else
#define GBC_LOG(...)	do {} while (0)
#endif

/* diagnostic trace of the audio handoff, compiled into release when requested.
 * Uses _dprintf directly (not dprintf) because the dprintf macro is compiled
 * out entirely when DEBUGLEVEL==0 (release); _dprintf still emits, gated only
 * by uart_putc's log_enable (which the trace build force-enables). */
#if defined(AYANEO_AUDIO_TRACE) || defined(AYANEO_DEBUG_LOGGING)
extern int _dprintf(const char *fmt, ...);
#define GBC_ATRACE(...)	_dprintf(__VA_ARGS__)
#else
#define GBC_ATRACE(...)	do {} while (0)
#endif

/* ---- extern C bridge into the gambatte core (emu/gbc/gbc_wrap.cpp) ---- */
extern void gbc_heap_init(void *base, unsigned size);
extern unsigned gbc_heap_used(void);
extern int  gbc_create(void);
extern int  gbc_load(const void *rom, unsigned size);
extern void gbc_reset(void);
extern long gbc_run(unsigned short *video, int pitch,
		    unsigned int *sound, unsigned soundBufSize, unsigned *samples);
extern unsigned gbc_state_size(void);
extern void gbc_save_state(void *buf);
extern int  gbc_load_state(const void *buf, unsigned size);
extern void *gbc_savedata_ptr(void);	/* cartridge battery SRAM (.sav) */
extern unsigned gbc_savedata_size(void);
extern void *gbc_rtcdata_ptr(void);	/* cartridge RTC (Pokemon clock) */
extern unsigned gbc_rtcdata_size(void);
extern int  pmic_detect_powerkey(void);		/* 1 = power key pressed */
extern void mt_power_off(void);
/* partition_read/partition_write come from <part_interface.h> */

/* ---- LK primitives ---- */
extern void *memcpy(void *, const void *, unsigned int);
extern time_t current_time(void);
/* free-running 13 MHz counter (13 ticks/us); the scheduler tick (current_time)
 * is only 10 ms, far too coarse to pace a 16.74 ms GB frame without beating. */
extern unsigned gpt4_get_current_tick(void);
extern void arch_clean_cache_range(unsigned long start, unsigned int len);
extern void ayaneo_gbc_show_frame(const unsigned short *pix);	/* mt_disp_drv.c */
extern void mtk_wdt_restart(void);	/* kick the hardware watchdog */
extern void mtk_wdt_disable(void);
extern int  ayaneo_boot_audio_active(void);			/* boot chime playing? */
extern void ayaneo_gbc_audio_init(void);			/* ayaneo_audio.c */
extern void ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_pause(int on);
extern void ayaneo_gbc_audio_shutdown(void);	/* clean codec teardown before off */
/* persisted settings (volume + backlight) and the on-screen slider */
extern void ayaneo_settings_load(void);		/* ayaneo_audio.c */
extern void ayaneo_settings_save(void);
extern int  ayaneo_brightness_step(int dir);	/* dir +1/-1; returns new 0-100% */
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_gbc_osd_show(int kind, int pct);	/* kind: 1=volume 2=brightness (mt_disp_drv.c) */
/* persisted boot/video settings (ayaneo_audio.c) */
extern int  ayaneo_get_load_on_boot(void);
extern void ayaneo_set_load_on_boot(int v);
extern int  ayaneo_get_skip_boot(void);
extern void ayaneo_set_skip_boot(int v);
extern int  ayaneo_get_lcd_filter(void);
extern void ayaneo_set_lcd_filter(int v);
extern int  ayaneo_get_color_correct(void);
extern void ayaneo_set_color_correct(int v);
extern int  ayaneo_get_dark_filter(void);
extern void ayaneo_set_dark_filter(int v);
/* gambatte CGB colour bridge (gbc_wrap.cpp) */
extern void gbc_set_color_correction(int enable);
extern void gbc_set_dark_filter(unsigned level);
/* menu rendering canvas + text (mt_disp_drv.c) */
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern void ayaneo_fill_blend(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb, int alpha);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
/* battery (mt_battery.c / mt_pmic.c) */
extern int get_bat_sense_volt(int times);	/* mV */
extern int upmu_is_chr_det(void);		/* charger present */

/* input: SoC GPIOs (gpio-keys, active-low) + MTK keypad for volume */
extern int mt_set_gpio_mode(unsigned pin, unsigned mode);
extern int mt_set_gpio_dir(unsigned pin, unsigned dir);
extern int mt_set_gpio_pull_enable(unsigned pin, unsigned en);
extern int mt_set_gpio_pull_select(unsigned pin, unsigned sel);
extern int mt_get_gpio_in(unsigned pin);
extern int mtk_detect_key(unsigned short hwkey);	/* KP matrix key pressed? */

/* gambatte input bitmask (inputgetter.h) */
#define IG_A	0x01u
#define IG_B	0x02u
#define IG_SEL	0x04u
#define IG_START 0x08u
#define IG_RIGHT 0x10u
#define IG_LEFT	0x20u
#define IG_UP	0x40u
#define IG_DOWN	0x80u


/* ---- freestanding externals the core needs that LK/libgcc lack ----
 * (cartridge_set_rumble is a C++ symbol, defined in gbc_shim.cpp) */
int  atexit(void (*fn)(void)) { (void)fn; return 0; }
long time(long *t) { long v = (long)current_time() / 1000; if (t) *t = v; return v; }
float powf(float b, float e) { (void)e; return b; }	/* color-correction only; unused */

/* ---- config ---- */
#define GBC_HEAP_PA	0x50000000u	/* DRAM arena for the emulator */
#define GBC_HEAP_SZ	(32u * 1024 * 1024)
#define GBC_W		160
#define GBC_H		144
#define GBC_ROM_PART	"boot_b"
#define GBC_ROM_OFF	0x01400000u	/* 20 MB into boot_b: [magic u32][size u32][rom] */
#define GBC_ROM_MAGIC	0x52434247u	/* "GBCR" */
#define GBC_ROM_MAX	(8u * 1024 * 1024)

/* one frame of gambatte audio at 2097152 Hz is ~35112 stereo samples */
#define GBC_SND_MAX	35208u

static int gbc_ready;

/* Start/Select physical GPIOs. Swapped from the original mapping (Start was 90,
 * Select was 91) so the two buttons trade functions. Select doubles as the
 * brightness modifier (Select + Volume). */
#define GBC_GPIO_START	91
#define GBC_GPIO_SELECT	90

/* gpio-keys mapping (from the device tree, active-low): GB buttons -> SoC GPIO.
 * A/B are swapped vs the DTS labels to match the physical layout. */
static const struct { unsigned gpio; unsigned mask; } s_btn[] = {
	{ 89, IG_UP },   { 79, IG_DOWN }, { 78, IG_LEFT }, { 80, IG_RIGHT },
	{ 83, IG_A },    { 82, IG_B },
	{ GBC_GPIO_START, IG_START }, { GBC_GPIO_SELECT, IG_SEL },
};

/* extra buttons used for functions, not direct GB inputs */
#define GBC_GPIO_X	84	/* autofire A */
#define GBC_GPIO_Y	85	/* autofire B */
#define GBC_GPIO_R	81	/* hold = fast-forward */
#define GBC_AUTOFIRE_HZ	12

static volatile int s_fast_forward;

/* MTK gpio helpers expect the pin OR'd with this "magic" bit; without it they
 * spam a "decrypt warning" on every call (which floods the UART and stalls the
 * emulator, since the input getter runs many times per frame). */
#define GP(n)	((n) | 0x80000000u)

static void gbc_gpio_in_pullup(unsigned gpio)
{
	mt_set_gpio_mode(GP(gpio), 0);		/* GPIO function */
	mt_set_gpio_dir(GP(gpio), 0);		/* input */
	mt_set_gpio_pull_enable(GP(gpio), 1);
	mt_set_gpio_pull_select(GP(gpio), 1);	/* GPIO_PULL_UP */
}

/* configure all the button GPIOs once */
static void gbc_input_init(void)
{
	unsigned i;
	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		gbc_gpio_in_pullup(s_btn[i].gpio);
	gbc_gpio_in_pullup(GBC_GPIO_X);
	gbc_gpio_in_pullup(GBC_GPIO_Y);
	gbc_gpio_in_pullup(GBC_GPIO_R);
	gbc_gpio_in_pullup(86);		/* AYA button = menu trigger */
}

#define GBC_PRESSED(g)	(mt_get_gpio_in(GP(g)) == 0)	/* active-low */

/* Early boot: is Select held? (configures + reads the Select GPIO). Used to
 * skip the boot animation/chime and jump straight into the emulator. */
int ayaneo_gbc_select_held(void)
{
	gbc_gpio_in_pullup(GBC_GPIO_SELECT);
	return GBC_PRESSED(GBC_GPIO_SELECT);
}

/* set while Select is being used as the brightness modifier (Select + Volume),
 * so the Select press is not also delivered to the game as GB Select. */
static volatile int s_sel_modifier;

/* forward decl (defined with the menu below): the overlay menu is open, so the
 * d-pad/face buttons drive the menu, not the game. */
static volatile int s_menu_open;

/* current button state, refreshed once per frame by gbc_update_buttons() and
 * returned to the core's InputGetter (which is called many times per frame). */
static volatile unsigned s_btn_state;

/* extra raw bits for the function buttons (above the 8 GB button bits) */
#define RB_X	0x100u
#define RB_Y	0x200u
#define RB_R	0x400u

/* refresh the button state from the GPIOs; called once per frame (NOT per
 * input-getter call, which would read the GPIOs thousands of times per frame).
 * A bit only registers if pressed in two consecutive reads (~2 frames = ~33ms),
 * which debounces the line noise that otherwise causes phantom presses. */
static void gbc_update_buttons(void)
{
	static unsigned prev_raw;
	unsigned raw = 0, deb, m = 0, i;
	int af;

	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		if (GBC_PRESSED(s_btn[i].gpio))
			raw |= s_btn[i].mask;
	if (GBC_PRESSED(GBC_GPIO_X)) raw |= RB_X;
	if (GBC_PRESSED(GBC_GPIO_Y)) raw |= RB_Y;
	if (GBC_PRESSED(GBC_GPIO_R)) raw |= RB_R;

	deb = raw & prev_raw;		/* stable across two reads */
	prev_raw = raw;

	m = deb & 0xffu;		/* the 8 GB buttons */

	/* while Select is driving the brightness slider, keep it out of the game */
	if (s_sel_modifier)
		m &= ~IG_SEL;

	/* X/Y = autofire B/A: pulse the button at GBC_AUTOFIRE_HZ while held */
	af = ((unsigned)current_time() * GBC_AUTOFIRE_HZ / 500u) & 1;
	if (af) {
		if (deb & RB_X) m |= IG_B;	/* X = autofire B */
		if (deb & RB_Y) m |= IG_A;	/* Y = autofire A */
	}

	s_fast_forward = (deb & RB_R) ? 1 : 0;	/* R held = fast-forward */
	if (s_menu_open)			/* menu owns the buttons while open */
		m = 0;
	s_btn_state = m;
}

/* cheap getter for the core's InputGetter - returns the cached state */
unsigned gbc_read_buttons(void)
{
	return s_btn_state;
}

/* Poll the volume keys (MTK keypad), edge-detected. Plain Volume adjusts the
 * audio level; Select + Volume adjusts the screen brightness. Both show an
 * on-screen slider and persist the new value to boot_b so it survives reboots. */
static void gbc_poll_volume(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);		/* VolumeUp  (hw key 0x11) */
	int vd = mtk_detect_key(0x00);		/* VolumeDown (hw key 0x00) */
	int sel = GBC_PRESSED(GBC_GPIO_SELECT);	/* Select = brightness modifier */
	int dir = 0;

	if (vu && !vu_prev) dir = +1;
	else if (vd && !vd_prev) dir = -1;

	if (dir) {
		if (sel) {
			int pct = ayaneo_brightness_step(dir);
			ayaneo_gbc_osd_show(2, pct);	/* 2 = brightness */
			s_sel_modifier = 1;		/* suppress GB Select this hold */
		} else {
			int v = ayaneo_gbc_audio_get_volume() + dir * 5;
			ayaneo_gbc_audio_set_volume(v);
			ayaneo_gbc_osd_show(1, ayaneo_gbc_audio_get_volume());	/* 1 = volume */
		}
		ayaneo_settings_save();		/* persist across reboots */
	}
	if (!sel)
		s_sel_modifier = 0;		/* Select released: hand it back to the game */

	vu_prev = vu;
	vd_prev = vd;
}

/* Save state stored in boot_b, placed past the ROM's *max* reserved region
 * (GBC_ROM_OFF + GBC_ROM_MAX = 28 MB) so it can never overlap the ROM, and well
 * clear of the audio blob (16-17.2 MB) and video (0). boot_b is 32 MB. */
#define GBC_STATE_OFF	(GBC_ROM_OFF + GBC_ROM_MAX)	/* 0x1C00000 = 28 MB */
#define GBC_STATE_MAGIC	0x53534247u	/* "GBSS" */
#define GBC_STATE_MAX	(2u * 1024u * 1024u)	/* scratch cap for [hdr+state] */

static unsigned rd32le(const unsigned char *p)
{
	return (unsigned)p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr32le(unsigned char *p, unsigned v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* Write the current emulator state to boot_b. Returns 1 on success. */
static int gbc_write_state(unsigned char *scratch)
{
	unsigned sz = gbc_state_size();
	unsigned total;

	if (!sz || (unsigned long long)sz + 8 > GBC_STATE_MAX)
		return 0;
	total = 8 + sz;
	gbc_save_state(scratch + 8);
	wr32le(scratch + 0, GBC_STATE_MAGIC);
	wr32le(scratch + 4, sz);
	/* pad to a 512-byte block and zero the tail so the eMMC write is
	 * block-aligned (no partial-block read-modify-write) and touches
	 * nothing but the state region (28 MB+, past ROM/audio/video). */
	if (total & 511u) {
		unsigned padded = (total + 511u) & ~511u;
		if (padded <= GBC_STATE_MAX) {
			unsigned k;
			for (k = total; k < padded; k++)
				scratch[k] = 0;
			total = padded;
		}
	}
	/* flush the CPU-written buffer to DRAM so the eMMC DMA writes the real
	 * state, not stale cache (else the reloaded state is garbage/hangs). */
	arch_clean_cache_range((unsigned long)scratch, total);
	partition_write(GBC_ROM_PART, GBC_STATE_OFF, scratch, total);
	return 1;
}

/* Load a save state from boot_b into the running emulator. Returns 1 on success. */
static int gbc_read_state(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned magic, sz;

	if (partition_read(GBC_ROM_PART, GBC_STATE_OFF, hdr, 8) != 8)
		return 0;
	magic = rd32le(hdr);
	sz = rd32le(hdr + 4);
	if (magic != GBC_STATE_MAGIC || sz == 0 || sz > GBC_STATE_MAX - 8)
		return 0;
	if (partition_read(GBC_ROM_PART, GBC_STATE_OFF + 8, scratch, sz) != (ssize_t)sz)
		return 0;
	return gbc_load_state(scratch, sz) == 0;
}

/* Cartridge battery save (.sav) + RTC, persisted independently of save states so
 * the game's own in-cart save behaves like a real cartridge. Own region in
 * boot_b past the settings block. Layout: [magic][savsz][rtcsz][sav][rtc]. */
#define GBC_SAV_OFF	0x01E08000u	/* 30 MB + 32 KB (past settings at 30 MB) */
#define GBC_SAV_MAGIC	0x56415347u	/* "GSAV" */
#define GBC_SAV_MAX	(320u * 1024u)	/* SRAM<=128KB + RTC + header, block-aligned */

static void gbc_sav_save(unsigned char *scratch)
{
	unsigned savsz = gbc_savedata_size();
	unsigned rtcsz = gbc_rtcdata_size();
	unsigned total = 12 + savsz + rtcsz;

	if (!savsz || total > GBC_SAV_MAX)
		return;
	wr32le(scratch + 0, GBC_SAV_MAGIC);
	wr32le(scratch + 4, savsz);
	wr32le(scratch + 8, rtcsz);
	memcpy(scratch + 12, gbc_savedata_ptr(), savsz);
	if (rtcsz)
		memcpy(scratch + 12 + savsz, gbc_rtcdata_ptr(), rtcsz);
	if (total & 511u) {
		unsigned padded = (total + 511u) & ~511u;
		if (padded <= GBC_SAV_MAX) {
			unsigned k;
			for (k = total; k < padded; k++)
				scratch[k] = 0;
			total = padded;
		}
	}
	arch_clean_cache_range((unsigned long)scratch, total);
	partition_write(GBC_ROM_PART, GBC_SAV_OFF, scratch, total);
}

static void gbc_sav_load(unsigned char *scratch)
{
	unsigned char hdr[12];
	unsigned savsz, rtcsz, live_sav = gbc_savedata_size(), live_rtc = gbc_rtcdata_size();

	if (partition_read(GBC_ROM_PART, GBC_SAV_OFF, hdr, 12) != 12)
		return;
	if (rd32le(hdr) != GBC_SAV_MAGIC)
		return;
	savsz = rd32le(hdr + 4);
	rtcsz = rd32le(hdr + 8);
	if (!savsz || 12u + savsz + rtcsz > GBC_SAV_MAX)
		return;
	if (partition_read(GBC_ROM_PART, GBC_SAV_OFF + 12, scratch, savsz + rtcsz)
	    != (ssize_t)(savsz + rtcsz))
		return;
	if (savsz == live_sav)			/* only inject a matching-size SRAM */
		memcpy(gbc_savedata_ptr(), scratch, savsz);
	if (rtcsz && rtcsz == live_rtc)
		memcpy(gbc_rtcdata_ptr(), scratch + savsz, rtcsz);
}

/* On boot: resume the save state unless the user opted out (Start held, or the
 * "load state on boot" setting is off). */
static void gbc_try_load_state(unsigned char *scratch)
{
	if (GBC_PRESSED(GBC_GPIO_START)) {
		dprintf(CRITICAL, "GBC: Start held - skipping save state\n");
		return;
	}
	if (!ayaneo_get_load_on_boot()) {
		dprintf(CRITICAL, "GBC: load-on-boot disabled - fresh start\n");
		return;
	}
	if (gbc_read_state(scratch))
		dprintf(CRITICAL, "GBC: resumed save state\n");
}

/* Power key: save state to boot_b, then power off. Does not return. */
static void gbc_save_and_poweroff(unsigned char *scratch)
{
	gbc_write_state(scratch);
	gbc_sav_save(scratch);		/* persist the cartridge battery save (.sav) */
	ayaneo_settings_save();		/* persist volume/brightness */
	/* leave the codec/amp cold so the next boot's chime plays (mt_power_off is
	 * a soft off that keeps the audio registers latched). */
	ayaneo_gbc_audio_shutdown();
	mt_power_off();
}

/* Poll power key (armed after first release so a boot-time hold is ignored). */
static void gbc_check_power(unsigned char *scratch)
{
	static int armed;
	int p = pmic_detect_powerkey();

	if (!p)
		armed = 1;
	else if (armed)
		gbc_save_and_poweroff(scratch);		/* no return */
}

/* Load the ROM from boot_b into the arena. Returns rom size or 0 on failure. */
static unsigned gbc_load_rom(unsigned char *dst)
{
	unsigned char hdr[8];
	unsigned magic, size;

	if (partition_read(GBC_ROM_PART, GBC_ROM_OFF, hdr, 8) != 8)
		return 0;
	magic = (unsigned)hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((unsigned)hdr[3] << 24);
	size  = (unsigned)hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((unsigned)hdr[7] << 24);
	if (magic != GBC_ROM_MAGIC || size == 0 || size > GBC_ROM_MAX)
		return 0;
	if (partition_read(GBC_ROM_PART, GBC_ROM_OFF + 8, dst, size) != (ssize_t)size)
		return 0;
	return size;
}

/* ===================== GammaOS Pico overlay menu ===================== */
/* Triggered by the AYA button (GPIO 86). Pauses the emulator, dims the panel,
 * and lets the user change settings (all persisted to boot_b) and load/save
 * state. Navigation: Up/Down move, Left/Right change value, A activate,
 * B or AYA close. */
#define GBC_GPIO_AYA	86

/* apply the emulator-side colour settings to the running core */
static void gbc_apply_video_settings(void)
{
	gbc_set_color_correction(ayaneo_get_color_correct());
	gbc_set_dark_filter((unsigned)ayaneo_get_dark_filter());
}

/* minimal unsigned/int -> decimal, appended to *p (no libc dependency) */
static char *mi_puts(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *mi_putu(char *p, unsigned v)
{
	char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = '0' + v % 10; v /= 10; }
	while (n) *p++ = t[--n];
	return p;
}

/* rough Li-ion percentage from the pack voltage (3.50 V = 0%, 4.30 V = 100%) */
static int gbc_battery_pct(int mv)
{
	int pct = (mv - 3500) * 100 / (4300 - 3500);
	return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

/* edge-detected menu button read: returns bits for newly-pressed keys */
enum { MK_UP=1, MK_DOWN=2, MK_LEFT=4, MK_RIGHT=8, MK_A=16, MK_B=32, MK_AYA=64 };
static unsigned gbc_menu_keys(void)
{
	static unsigned prev;
	unsigned raw = 0, edge;
	if (GBC_PRESSED(89)) raw |= MK_UP;
	if (GBC_PRESSED(79)) raw |= MK_DOWN;
	if (GBC_PRESSED(78)) raw |= MK_LEFT;
	if (GBC_PRESSED(80)) raw |= MK_RIGHT;
	if (GBC_PRESSED(83)) raw |= MK_A;	/* physical A (see button table) */
	if (GBC_PRESSED(82)) raw |= MK_B;	/* physical B */
	if (GBC_PRESSED(GBC_GPIO_AYA)) raw |= MK_AYA;
	edge = raw & ~prev;
	prev = raw;
	return edge;
}

enum {
	MI_BRIGHT, MI_VOLUME, MI_FILTER, MI_COLORCC, MI_DARKF,
	MI_LOADBOOT, MI_SKIPBOOT, MI_LOADSTATE, MI_SAVESTATE, MI_BATTERY,
	MI_CLOSE, MI_COUNT
};

static const char *filter_name(int f)
{
	return f == 1 ? "Scanlines" : f == 2 ? "LCD Grid" : f == 3 ? "Dot Matrix" : "Off";
}

/* build the value string for one row into buf; returns buf */
static const char *gbc_menu_value(int item, char *buf, unsigned char *state)
{
	char *p = buf;
	(void)state;
	switch (item) {
	case MI_BRIGHT:   p = mi_putu(p, (unsigned)ayaneo_brightness_pct()); p = mi_puts(p, "%"); break;
	case MI_VOLUME:   p = mi_putu(p, (unsigned)ayaneo_gbc_audio_get_volume()); p = mi_puts(p, "%"); break;
	case MI_FILTER:   p = mi_puts(p, filter_name(ayaneo_get_lcd_filter())); break;
	case MI_COLORCC:  p = mi_puts(p, ayaneo_get_color_correct() ? "On" : "Off"); break;
	case MI_DARKF:    p = mi_putu(p, (unsigned)ayaneo_get_dark_filter()); break;
	case MI_LOADBOOT: p = mi_puts(p, ayaneo_get_load_on_boot() ? "On" : "Off"); break;
	case MI_SKIPBOOT: p = mi_puts(p, ayaneo_get_skip_boot() ? "On" : "Off"); break;
	case MI_LOADSTATE:
	case MI_SAVESTATE: p = mi_puts(p, "[A]"); break;
	case MI_BATTERY: {
		/* cache the reading and refresh it at ~1 Hz so the line doesn't jitter
		 * between the two scan-out buffers (which would flicker on that row). */
		static int mv, chr, tick;
		if (tick-- <= 0) { mv = get_bat_sense_volt(1); chr = upmu_is_chr_det(); tick = 60; }
		p = mi_putu(p, (unsigned)gbc_battery_pct(mv)); p = mi_puts(p, "% ");
		p = mi_puts(p, chr ? "Charging" : "Battery");
		break;
	}
	case MI_CLOSE: default: break;
	}
	*p = 0;
	return buf;
}

static const char *gbc_menu_label(int item)
{
	switch (item) {
	case MI_BRIGHT:    return "Brightness";
	case MI_VOLUME:    return "Volume";
	case MI_FILTER:    return "LCD Filter";
	case MI_COLORCC:   return "Color Correction";
	case MI_DARKF:     return "Dark Filter";
	case MI_LOADBOOT:  return "Load State on Boot";
	case MI_SKIPBOOT:  return "Skip Boot Anim/Chime";
	case MI_LOADSTATE: return "Load State";
	case MI_SAVESTATE: return "Save State";
	case MI_BATTERY:   return "Battery";
	case MI_CLOSE:     return "Close";
	}
	return "";
}

/* change the selected item by dir (-1/+1) or activate it (act). Returns 1 if
 * the menu should close. status[] gets a short transient message. */
static int gbc_menu_change(int item, int dir, int act, unsigned char *state, char *status)
{
	int changed = 1;
	status[0] = 0;
	switch (item) {
	case MI_BRIGHT:   if (dir) ayaneo_brightness_step(dir); else changed = 0; break;
	case MI_VOLUME:   if (dir) ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5); else changed = 0; break;
	case MI_FILTER:   if (dir) ayaneo_set_lcd_filter((ayaneo_get_lcd_filter() + dir + 4) % 4); else changed = 0; break;
	case MI_COLORCC:  if (dir || act) { ayaneo_set_color_correct(!ayaneo_get_color_correct()); gbc_apply_video_settings(); } else changed = 0; break;
	case MI_DARKF:    if (dir) { ayaneo_set_dark_filter(ayaneo_get_dark_filter() + dir); gbc_apply_video_settings(); } else changed = 0; break;
	case MI_LOADBOOT: if (dir || act) ayaneo_set_load_on_boot(!ayaneo_get_load_on_boot()); else changed = 0; break;
	case MI_SKIPBOOT: if (dir || act) ayaneo_set_skip_boot(!ayaneo_get_skip_boot()); else changed = 0; break;
	case MI_LOADSTATE: if (act) { mi_puts(status, gbc_read_state(state) ? "State loaded" : "No save state"); } changed = 0; break;
	case MI_SAVESTATE: if (act) { int ok = gbc_write_state(state); gbc_sav_save(state); mi_puts(status, ok ? "State saved" : "Save failed"); } changed = 0; break;
	case MI_CLOSE:    if (act) return 1; changed = 0; break;
	default: changed = 0; break;
	}
	if (changed)
		ayaneo_settings_save();		/* every setting is persisted */
	return 0;
}

/* The menu is a live overlay: the emulator keeps running underneath so filter,
 * colour and save-state changes are visible in real time. State is file-scope so
 * the run loop can drive input while the display path draws the panel on top of
 * each game frame. (s_menu_open is forward-declared above gbc_update_buttons.) */
static int s_menu_sel;
static char s_menu_status[48];

int gbc_menu_is_open(void) { return s_menu_open; }

/* AYA button edge -> toggle the menu */
static int gbc_aya_edge(void)
{
	static int prev;
	int now = GBC_PRESSED(GBC_GPIO_AYA), edge = now && !prev;
	prev = now;
	return edge;
}
static void gbc_menu_toggle(void)
{
	s_menu_open = !s_menu_open;
	s_menu_status[0] = 0;
	gbc_menu_keys();		/* drop the AYA edge so it isn't re-read */
}

/* Per-frame menu input while open (does NOT pause the game). B closes; AYA is
 * handled by the toggle. Game input is suppressed elsewhere while open. */
static void gbc_menu_tick(unsigned char *state)
{
	unsigned k = gbc_menu_keys();

	if (k & MK_UP)    { s_menu_sel = (s_menu_sel + MI_COUNT - 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_DOWN)  { s_menu_sel = (s_menu_sel + 1) % MI_COUNT; s_menu_status[0] = 0; }
	if (k & MK_LEFT)  { if (gbc_menu_change(s_menu_sel, -1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_RIGHT) { if (gbc_menu_change(s_menu_sel, +1, 0, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_A)     { if (gbc_menu_change(s_menu_sel, 0, 1, state, s_menu_status)) s_menu_open = 0; }
	if (k & MK_B)     s_menu_open = 0;
}

/* Draw the menu panel on top of the already-blitted game frame. A translucent
 * background keeps the game visible so changes show through in real time. */
void gbc_menu_draw_overlay(unsigned int *buf, unsigned int pitch,
			   unsigned int W, unsigned int H)
{
	int panelW = 640, panelH = 560;
	int px = ((int)W - panelW) / 2, py = ((int)H - panelH) / 2;
	int rowH = 40, x = px + 28, y = py + 92, i;
	char val[48];

	/* Opaque panel so its pixels are identical in both scan-out buffers: a
	 * mid-scan buffer swap then shows the same menu either way, with no drifting
	 * tear on the static text. The game stays visible and live AROUND the panel. */
	ayaneo_fill(buf, pitch, px, py, panelW, panelH, 0xFF10141Cu);		/* panel */
	ayaneo_fill(buf, pitch, px, py, panelW, 6, 0xFF5090F0u);			/* top bar */
	ayaneo_text(buf, pitch, px + 28, py + 32, 3, 0xFFFFFFFFu, "GammaOS Pico");

	for (i = 0; i < MI_COUNT; i++, y += rowH) {
		unsigned int fg = (i == s_menu_sel) ? 0xFF101018u : 0xFFC8D0E0u;
		int vw;
		if (i == s_menu_sel)
			ayaneo_fill(buf, pitch, px + 10, y - 4, panelW - 20, rowH, 0xFF5090F0u);
		ayaneo_text(buf, pitch, x, y, 2, fg, gbc_menu_label(i));
		gbc_menu_value(i, val, (unsigned char *)0);
		for (vw = 0; val[vw]; vw++) ;
		ayaneo_text(buf, pitch, px + panelW - 28 - vw * 16, y, 2, fg, val);
	}
	if (s_menu_status[0])
		ayaneo_text(buf, pitch, x, py + panelH - 40, 2, 0xFF80E080u, s_menu_status);
	ayaneo_text(buf, pitch, x, py + panelH - 16, 1, 0xFF8890A0u,
		    "Up/Down move  Left/Right change  A select  B/AYA close");
}

static int gbc_emu_thread(void *arg)
{
	unsigned char *arena = (unsigned char *)GBC_HEAP_PA;
	unsigned char *rom   = arena;			/* 0 .. 8MB : ROM */
	unsigned int  *snd   = (unsigned int *)(arena + GBC_ROM_MAX);
	unsigned char *state = arena + GBC_HEAP_SZ - GBC_STATE_MAX;	/* end slab */
	void          *cxx   = arena + GBC_ROM_MAX + GBC_SND_MAX * 4;
	unsigned       cxxsz = GBC_HEAP_SZ - GBC_ROM_MAX - GBC_SND_MAX * 4
			       - GBC_STATE_MAX;
	static unsigned short vbuf[GBC_W * GBC_H];
	unsigned romsz, frame = 0;

	romsz = gbc_load_rom(rom);
	if (!romsz) {
		dprintf(CRITICAL, "GBC: no ROM at %s+0x%x\n", GBC_ROM_PART, GBC_ROM_OFF);
		return 0;
	}

	gbc_heap_init(cxx, cxxsz);
	if (gbc_create() != 0) {
		dprintf(CRITICAL, "GBC: create failed\n");
		return 0;
	}
	if (gbc_load(rom, romsz) != 0) {
		dprintf(CRITICAL, "GBC: load failed (romsz=%u)\n", romsz);
		return 0;
	}
	dprintf(CRITICAL, "GBC: loaded romsz=%u heap_used=%u\n", romsz, gbc_heap_used());

	gbc_input_init();		/* configure the button GPIOs (before state) */
	ayaneo_settings_load();		/* persisted volume/brightness for the game */
	gbc_apply_video_settings();	/* colour correction / dark filter from settings */
	gbc_sav_load(state);		/* inject the cartridge battery save (.sav) */
	gbc_try_load_state(state);	/* resume unless Start is held (overrides SRAM) */
	gbc_ready = 1;

	/* we run forever with no kernel handoff, so the boot watchdog would fire
	 * after a few seconds - disable it and also kick it every frame. */
	mtk_wdt_disable();

	/* let the boot chime finish before we take over the codec (bounded) */
	{
		int g = 0;
		GBC_ATRACE("GBC: audio wait enter, chime active=%d t=%u\n",
			   ayaneo_boot_audio_active(), (unsigned)current_time());
		while (ayaneo_boot_audio_active() && g++ < 300)
			thread_sleep(20);
		GBC_ATRACE("GBC: audio wait exit, active=%d g=%d t=%u\n",
			   ayaneo_boot_audio_active(), g, (unsigned)current_time());
	}
	ayaneo_gbc_audio_init();		/* bring up the streaming audio path */
	GBC_ATRACE("GBC: gbc audio init done t=%u\n", (unsigned)current_time());

	{
		/* Pace off the 13 MHz free-running counter, not the 10 ms scheduler
		 * tick. Ticks per GB frame = 13e6 * (samples/frame) / soundrate =
		 * 13000000 * 35112 / 2097152. Kept as an exact rational and applied
		 * cumulatively from a fixed base so there is no per-frame rounding
		 * drift (and the u32 truncation wraps in step with the hardware). */
		const unsigned long long TPF_NUM = 13000000ull * 35112ull;
		const unsigned long long TPF_DEN = 2097152ull;
		unsigned pace_base = gpt4_get_current_tick();
		unsigned long long pace_n = 0;	/* frames since pace_base */
		int ff_prev = 0;

		for (;;) {
			unsigned samples = GBC_SND_MAX;
			long r;

			gbc_update_buttons();	/* refresh input once per frame */
			r = gbc_run(vbuf, GBC_W, snd, GBC_SND_MAX, &samples);

			/* fast-forward edge: mute (silence the ring) on enter, resync
			 * the audio on exit */
			if (s_fast_forward != ff_prev) {
				ayaneo_gbc_audio_pause(s_fast_forward);
				ff_prev = s_fast_forward;
			}
			/* skip audio while fast-forwarding (the 48k ring can't keep
			 * up with sped-up generation without artifacts) */
			if (!s_fast_forward)
				ayaneo_gbc_audio_submit(snd, samples);

			if (r >= 0) {		/* a video frame completed */
				unsigned target;

				mtk_wdt_restart();
				gbc_poll_volume();
				gbc_check_power(state);	/* power -> save + off */
				frame++;

				/* AYA toggles the live overlay menu; while open, drive the
				 * menu each frame (game keeps running underneath). */
				if (gbc_aya_edge())
					gbc_menu_toggle();
				if (s_menu_open)
					gbc_menu_tick(state);

				if (s_fast_forward) {
					/* run flat out; present every 4th frame so the
					 * vsync-locked blit doesn't cap the speed */
					if ((frame & 3) == 0)
						ayaneo_gbc_show_frame(vbuf);
					pace_base = gpt4_get_current_tick();
					pace_n = 0;	/* rebase for a clean exit */
					continue;	/* no pacing sleep */
				}

				ayaneo_gbc_show_frame(vbuf);
				if ((frame % 120) == 0)
					GBC_LOG("GBC: frame %u\n", frame);

				/* pace to the GB's ~59.7275 Hz: cumulative target tick */
				pace_n++;
				target = pace_base + (unsigned)((pace_n * TPF_NUM) / TPF_DEN);

				/* if we have fallen more than a couple of frames behind
				 * (long hitch), rebase so we don't then sprint to catch
				 * up; otherwise spin on the fine counter until target. */
				if ((int)(gpt4_get_current_tick() - target) > 2 * 217645) {
					pace_base = gpt4_get_current_tick();
					pace_n = 0;
				} else {
					while ((int)(gpt4_get_current_tick() - target) < 0)
						; /* busy-wait ~<16 ms; we own the CPU */
				}
			}
		}
	}
	return 0;
}

void ayaneo_gbc_start(void)
{
	thread_t *t = thread_create("ayaneo_gbc", &gbc_emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t)
		thread_resume(t);
}
