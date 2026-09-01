/*
 * gbc_core_exports.cpp - blob side of the loadable gambatte core (see gbc_core_abi.h).
 *
 * Compiled INTO the blob (not lk_a). Publishes the export table LK drives the core
 * through, stores the imports table LK passes at init, and provides the handful of
 * outward-call forwarders the core needs (input, time) plus an atexit stub. libc mem*,
 * libgcc __aeabi_*, libm powf and the C++ runtime (gbc_shim.cpp) are bundled separately.
 */
#include "gbc_core_abi.h"

/* extern "C" core wrappers (gbc_wrap.cpp) */
extern "C" {
void     gbc_heap_init(void *base, unsigned size);
int      gbc_create(void);
int      gbc_load(const void *rom, unsigned size, unsigned flags);
void     gbc_reset(void);
long     gbc_run(unsigned short *video, int pitch, unsigned int *sound, unsigned sound_sz, unsigned *samples);
void    *gbc_savedata_ptr(void);
unsigned gbc_savedata_size(void);
void    *gbc_rtcdata_ptr(void);
unsigned gbc_rtcdata_size(void);
unsigned gbc_state_size(void);
void     gbc_save_state(void *buf);
int      gbc_load_state(const void *buf, unsigned size);
void     gbc_set_dmg_palette_color(unsigned pal, unsigned col, unsigned rgb32);
void     gbc_set_color_correction(int enable);
void     gbc_set_color_correction_mode(unsigned mode);
void     gbc_set_dark_filter(unsigned level);
}

static const struct gbc_core_imports *s_imp;

/* ---- outward-call forwarders the bundled core references ---- */
extern "C" unsigned gbc_read_buttons(void)      /* gambatte LkInput -> LK pad reader */
{
	return s_imp && s_imp->read_buttons ? s_imp->read_buttons() : 0;
}
extern "C" long time(long *t)                   /* cart RTC clock */
{
	long v = s_imp && s_imp->host_time ? s_imp->host_time() : 0;
	if (t) *t = v;
	return v;
}
extern "C" int atexit(void (*)(void)) { return 0; }   /* no teardown in the blob */

static const struct gbc_core_exports g_exports = {
	GBC_CORE_ABI_MAGIC,
	GBC_CORE_ABI_VERSION,
	gbc_heap_init,
	gbc_create,
	gbc_load,
	gbc_reset,
	gbc_run,
	gbc_savedata_ptr,
	gbc_savedata_size,
	gbc_rtcdata_ptr,
	gbc_rtcdata_size,
	gbc_state_size,
	gbc_save_state,
	gbc_load_state,
	gbc_set_dmg_palette_color,
	gbc_set_color_correction,
	gbc_set_color_correction_mode,
	gbc_set_dark_filter,
};

/* Blob entry: LK calls this with the imports table and gets the export table back. */
extern "C" const struct gbc_core_exports *gbc_core_blob_init(const struct gbc_core_imports *imp)
{
	s_imp = imp;
	return &g_exports;
}
