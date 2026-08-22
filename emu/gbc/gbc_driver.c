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
extern int  pmic_detect_powerkey(void);		/* 1 = power key pressed */
extern void mt_power_off(void);
/* partition_read/partition_write come from <part_interface.h> */

/* ---- LK primitives ---- */
extern void *memcpy(void *, const void *, unsigned int);
extern time_t current_time(void);
extern void ayaneo_gbc_show_frame(const unsigned short *pix);	/* mt_disp_drv.c */
extern void mtk_wdt_restart(void);	/* kick the hardware watchdog */
extern void mtk_wdt_disable(void);
extern int  ayaneo_boot_audio_active(void);			/* boot chime playing? */
extern void ayaneo_gbc_audio_init(void);			/* ayaneo_audio.c */
extern void ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_gbc_audio_get_volume(void);
extern void ayaneo_gbc_audio_pause(int on);

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

/* gpio-keys mapping (from the device tree, active-low): GB buttons -> SoC GPIO.
 * A/B are swapped vs the DTS labels to match the physical layout. */
static const struct { unsigned gpio; unsigned mask; } s_btn[] = {
	{ 89, IG_UP },   { 79, IG_DOWN }, { 78, IG_LEFT }, { 80, IG_RIGHT },
	{ 83, IG_A },    { 82, IG_B },    { 90, IG_START },{ 91, IG_SEL },
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
}

#define GBC_PRESSED(g)	(mt_get_gpio_in(GP(g)) == 0)	/* active-low */

/* Early boot: is Select held? (configures + reads the Select GPIO). Used to
 * skip the boot animation/chime and jump straight into the emulator. */
int ayaneo_gbc_select_held(void)
{
	gbc_gpio_in_pullup(91);		/* Select */
	return GBC_PRESSED(91);
}

/* current button state, refreshed once per frame by gbc_update_buttons() and
 * returned to the core's InputGetter (which is called many times per frame). */
static volatile unsigned s_btn_state;

/* refresh the button state from the GPIOs; called once per frame (NOT per
 * input-getter call, which would read the GPIOs thousands of times per frame). */
static void gbc_update_buttons(void)
{
	unsigned m = 0, i;
	int af;

	for (i = 0; i < sizeof(s_btn) / sizeof(s_btn[0]); i++)
		if (GBC_PRESSED(s_btn[i].gpio))
			m |= s_btn[i].mask;

	/* X/Y = autofire B/A: pulse the button at GBC_AUTOFIRE_HZ while held */
	af = ((unsigned)current_time() * GBC_AUTOFIRE_HZ / 500u) & 1;
	if (af) {
		if (GBC_PRESSED(GBC_GPIO_X)) m |= IG_B;	/* X = autofire B */
		if (GBC_PRESSED(GBC_GPIO_Y)) m |= IG_A;	/* Y = autofire A */
	}

	s_fast_forward = GBC_PRESSED(GBC_GPIO_R);	/* R held = fast-forward */
	s_btn_state = m;
}

/* cheap getter for the core's InputGetter - returns the cached state */
unsigned gbc_read_buttons(void)
{
	return s_btn_state;
}

/* poll the volume keys (MTK keypad) and adjust playback volume, edge-detected */
static void gbc_poll_volume(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);		/* VolumeUp  (hw key 0x11) */
	int vd = mtk_detect_key(0x00);		/* VolumeDown (hw key 0x00) */

	if (vu && !vu_prev)
		ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + 10);
	if (vd && !vd_prev)
		ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() - 10);
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

/* On boot: if a save state exists in boot_b, load it into the running emulator. */
static void gbc_try_load_state(unsigned char *scratch)
{
	unsigned char hdr[8];
	unsigned magic, sz;

	/* hold Start during boot to skip the save state and start fresh */
	if (GBC_PRESSED(90)) {
		dprintf(CRITICAL, "GBC: Start held - skipping save state\n");
		return;
	}
	if (partition_read(GBC_ROM_PART, GBC_STATE_OFF, hdr, 8) != 8)
		return;
	magic = rd32le(hdr);
	sz = rd32le(hdr + 4);
	if (magic != GBC_STATE_MAGIC || sz == 0 || sz > GBC_STATE_MAX - 8)
		return;
	if (partition_read(GBC_ROM_PART, GBC_STATE_OFF + 8, scratch, sz) != (ssize_t)sz)
		return;
	if (gbc_load_state(scratch, sz) == 0)
		dprintf(CRITICAL, "GBC: resumed save state (%u bytes)\n", sz);
}

/* Power key: save state to boot_b, then power off. Does not return. */
static void gbc_save_and_poweroff(unsigned char *scratch)
{
	unsigned sz = gbc_state_size();

	if (sz && (unsigned long long)sz + 8 <= GBC_STATE_MAX) {
		unsigned total = 8 + sz;

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
		partition_write(GBC_ROM_PART, GBC_STATE_OFF, scratch, total);
	}
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
	gbc_try_load_state(state);	/* resume unless Start is held */
	gbc_ready = 1;

	/* we run forever with no kernel handoff, so the boot watchdog would fire
	 * after a few seconds - disable it and also kick it every frame. */
	mtk_wdt_disable();

	/* let the boot chime finish before we take over the codec (bounded) */
	{
		int g = 0;
		while (ayaneo_boot_audio_active() && g++ < 300)
			thread_sleep(20);
	}
	ayaneo_gbc_audio_init();		/* bring up the streaming audio path */

	{
		unsigned pace_base = (unsigned)current_time();
		unsigned pace_n = 0;		/* frames since pace_base */
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
				unsigned target, now;

				mtk_wdt_restart();
				gbc_poll_volume();
				gbc_check_power(state);	/* power -> save + off */
				frame++;

				if (s_fast_forward) {
					/* run flat out; present every 4th frame so the
					 * vsync-locked blit doesn't cap the speed */
					if ((frame & 3) == 0)
						ayaneo_gbc_show_frame(vbuf);
					pace_base = (unsigned)current_time();
					pace_n = 0;	/* rebase for a clean exit */
					continue;	/* no pacing sleep */
				}

				ayaneo_gbc_show_frame(vbuf);
				if ((frame % 120) == 0)
					GBC_LOG("GBC: frame %u\n", frame);

				/* pace to the GB's ~59.7275 Hz */
				pace_n++;
				target = pace_base + (unsigned)(((unsigned long long)pace_n
								 * 100000ull) / 5973u);
				now = (unsigned)current_time();
				if ((int)(target - now) > 0)
					thread_sleep(target - now);
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
