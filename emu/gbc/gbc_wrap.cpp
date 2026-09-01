/*
 * extern "C" bridge between the LK C driver and the gambatte C++ core.
 * Only the AAPCS C ABI crosses this boundary.
 */
#include <gambatte.h>
#include <stddef.h>

#include <inputgetter.h>

using namespace gambatte;

static GB *g_gb;

extern "C" void gbc_heap_init(void *base, unsigned size);	/* shim.cpp */
extern "C" unsigned gbc_read_buttons(void);			/* gbc_driver.c */

/* Feeds the physical button state (SoC GPIOs) into the core. */
namespace {
struct LkInput : InputGetter {
	unsigned operator()() { return gbc_read_buttons(); }
};
}
static InputGetter *g_input;

/* Create the emulator (placement into the bump arena via operator new). */
extern "C" int gbc_create(void)
{
	g_gb = new GB();
	if (!g_gb)
		return -1;
	g_input = new LkInput();
	g_gb->setInputGetter(g_input);
	return 0;
}

/* Load a ROM image from a memory buffer. flags=0 auto-detects CGB; pass
 * gambatte::GB::FORCE_DMG (1) to force original Game Boy (DMG) mode for .gb ROMs. */
extern "C" int gbc_load(const void *rom, unsigned size, unsigned flags)
{
	if (!g_gb)
		return -1;
	return g_gb->load(rom, size, flags);
}

extern "C" void gbc_reset(void)
{
	if (g_gb)
		g_gb->reset();
}

/* ---- cartridge battery save (.sav) + RTC access ---- */
extern "C" void *gbc_savedata_ptr(void) { return g_gb ? g_gb->savedata_ptr() : 0; }
extern "C" unsigned gbc_savedata_size(void) { return g_gb ? g_gb->savedata_size() : 0; }
extern "C" void *gbc_rtcdata_ptr(void) { return g_gb ? g_gb->rtcdata_ptr() : 0; }
extern "C" unsigned gbc_rtcdata_size(void) { return g_gb ? g_gb->rtcdata_size() : 0; }

/* ---- CGB colour / palette knobs (menu-driven) ---- */
extern "C" void gbc_set_color_correction(int enable)
{
	if (g_gb)
		g_gb->setColorCorrection(enable != 0);
}
extern "C" void gbc_set_color_correction_mode(unsigned mode)
{
	if (g_gb)
		g_gb->setColorCorrectionMode(mode);
}
extern "C" void gbc_set_dark_filter(unsigned level)
{
	if (g_gb)
		g_gb->setDarkFilterLevel(level);
}
/* DMG (mono) games only: set one of the 4 shades of a palette. No effect on a
 * CGB game, which supplies its own palettes. */
extern "C" void gbc_set_dmg_palette_color(unsigned palNum, unsigned colorNum,
					  unsigned rgb32)
{
	if (g_gb)
		g_gb->setDmgPaletteColor(palNum, colorNum, rgb32);
}

/* ---- save states (buffer based) ---- */
extern "C" unsigned gbc_state_size(void)
{
	return g_gb ? (unsigned)g_gb->stateSize() : 0;
}
extern "C" void gbc_save_state(void *buf)
{
	if (g_gb)
		g_gb->saveState(buf);
}
extern "C" int gbc_load_state(const void *buf, unsigned size)
{
	return (g_gb && g_gb->loadState(buf, size)) ? 0 : -1;
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
