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

/* ---- extern C bridge into the gambatte core (emu/gbc/gbc_wrap.cpp) ---- */
extern void gbc_heap_init(void *base, unsigned size);
extern unsigned gbc_heap_used(void);
extern int  gbc_create(void);
extern int  gbc_load(const void *rom, unsigned size);
extern void gbc_reset(void);
extern long gbc_run(unsigned short *video, int pitch,
		    unsigned int *sound, unsigned soundBufSize, unsigned *samples);

/* ---- LK primitives ---- */
extern void *memcpy(void *, const void *, unsigned int);
extern time_t current_time(void);
extern void ayaneo_gbc_show_frame(const unsigned short *pix);	/* mt_disp_drv.c */

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
	void          *cxx   = arena + GBC_ROM_MAX + GBC_SND_MAX * 4;
	unsigned       cxxsz = GBC_HEAP_SZ - GBC_ROM_MAX - GBC_SND_MAX * 4;
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
	gbc_ready = 1;

	for (;;) {
		unsigned samples = GBC_SND_MAX;
		long r = gbc_run(vbuf, GBC_W, snd, GBC_SND_MAX, &samples);

		if (r >= 0) {			/* a video frame completed */
			ayaneo_gbc_show_frame(vbuf);
			if ((frame % 60) == 0)
				dprintf(CRITICAL, "GBC: frame %u\n", frame);
			frame++;
			thread_sleep(12);	/* rough ~60fps pacing (refined later) */
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
