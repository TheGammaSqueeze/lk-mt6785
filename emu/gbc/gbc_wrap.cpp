/*
 * extern "C" bridge between the LK C driver and the gambatte C++ core.
 * Only the AAPCS C ABI crosses this boundary.
 */
#include <gambatte.h>
#include <stddef.h>

using namespace gambatte;

static GB *g_gb;

extern "C" void gbc_heap_init(void *base, unsigned size);	/* shim.cpp */

/* Create the emulator (placement into the bump arena via operator new). */
extern "C" int gbc_create(void)
{
	g_gb = new GB();
	return g_gb ? 0 : -1;
}

/* Load a ROM image from a memory buffer. flags=0 auto-detects CGB. */
extern "C" int gbc_load(const void *rom, unsigned size)
{
	if (!g_gb)
		return -1;
	return g_gb->load(rom, size, 0);
}

extern "C" void gbc_reset(void)
{
	if (g_gb)
		g_gb->reset();
}

/*
 * Run emulation. videoBuf is 160x144 RGB565 (pitch in pixels); soundBuf holds
 * up to soundBufSize stereo samples at 2097152 Hz (each u32 = L|R<<16). On
 * input *samples is the number of samples of time to run; on output it is the
 * count actually produced. Returns >=0 when a video frame was completed this
 * call (videoBuf is then valid), <0 otherwise.
 */
extern "C" long gbc_run(unsigned short *videoBuf, int pitch,
			unsigned int *soundBuf, unsigned soundBufSize,
			unsigned *samples)
{
	if (!g_gb)
		return -1;
	return g_gb->runFor((video_pixel_t *)videoBuf, pitch,
			    (uint_least32_t *)soundBuf, soundBufSize, *samples);
}
