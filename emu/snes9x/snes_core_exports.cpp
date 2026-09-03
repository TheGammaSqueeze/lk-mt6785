/*
 * snes_core_exports.cpp - blob side of the loadable snes9x core (see snes_core_abi.h).
 *
 * Compiled INTO the blob. Installs the libretro callbacks, drives the snes9x retro_* API,
 * and publishes the export table LK uses. The ROM is fed as a BUFFER via retro_load_game
 * (no file I/O). Video is RGB565; input comes from the LK pad via the imports table; audio
 * is captured per frame from audio_batch. libc/libm/libstdc++/shim are linked separately.
 */
#include <time.h>
#include "libretro/libretro.h"
#include "snes_core_abi.h"
#include "snes9x.h"
#include "65c816.h"   /* struct SRegisters Registers (the 65816 CPU registers) */
#include "bapu/snes/snes.hpp"   /* SNES::smp (the SPC700 APU CPU) */

/* ---- libretro public API (from libretro/libretro.cpp) ---- */
extern "C" {
void     retro_set_environment(retro_environment_t);
void     retro_set_video_refresh(retro_video_refresh_t);
void     retro_set_audio_sample(retro_audio_sample_t);
void     retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void     retro_set_input_poll(retro_input_poll_t);
void     retro_set_input_state(retro_input_state_t);
void     retro_init(void);
void     retro_deinit(void);
bool     retro_load_game(const struct retro_game_info *);
void     retro_unload_game(void);
void     retro_run(void);
void     retro_reset(void);
void     retro_get_system_av_info(struct retro_system_av_info *);
void     retro_set_controller_port_device(unsigned, unsigned);
void    *retro_get_memory_data(unsigned);
size_t   retro_get_memory_size(unsigned);
size_t   retro_serialize_size(void);
bool     retro_serialize(void *, size_t);
bool     retro_unserialize(const void *, size_t);
}

static const struct snes_core_imports *s_imp;

/* video frame stash (set by the video callback during retro_run) */
static const void *s_vid;
static unsigned    s_vw, s_vh, s_vpitch;

/* audio accumulation (interleaved s16 stereo) for the current frame */
#define SNES_AUD_MAX 2048u               /* >> one 60Hz frame at 32 kHz (~1067 pairs) */
static short    s_aud[SNES_AUD_MAX * 2];
static unsigned s_aud_frames;

/* ---- core-option store (LK sets these via snes_set_option; snes9x reads them through
 * env_cb GET_VARIABLE). Only the handful of keys the Pico menu exposes are kept; every
 * other key falls through to the core default. A tiny fixed table (no allocation). ---- */
#define SNES_OPT_MAX 8
static struct { char key[28]; char val[20]; } s_opt[SNES_OPT_MAX];
static unsigned s_opt_n;
static bool     s_opt_dirty;   /* set on any change; drives GET_VARIABLE_UPDATE */

static bool str_eq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void str_cpy(char *d, const char *s, unsigned cap)
{ unsigned i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

/* exported to LK: set an option by libretro key (e.g. "snes9x_aspect" = "4:3"). Stored and
 * flagged so retro_run's GET_VARIABLE_UPDATE path re-reads it (geometry reflows for aspect/
 * overscan). Returns 0 on success. */
static int snes_set_option(const char *key, const char *value)
{
	unsigned i;
	if (!key || !value) return -1;
	for (i = 0; i < s_opt_n; i++) if (str_eq(s_opt[i].key, key)) {
		str_cpy(s_opt[i].val, value, sizeof s_opt[i].val); s_opt_dirty = true; return 0;
	}
	if (s_opt_n >= SNES_OPT_MAX) return -1;
	str_cpy(s_opt[s_opt_n].key, key, sizeof s_opt[s_opt_n].key);
	str_cpy(s_opt[s_opt_n].val, value, sizeof s_opt[s_opt_n].val);
	s_opt_n++; s_opt_dirty = true;
	return 0;
}

/* Run-ahead fast-savestate toggle: when set, GET_AUDIO_VIDEO_ENABLE advertises
 * RETRO_AV_ENABLE_FAST_SAVESTATES so snes9x's serialize/unserialize take the fast path
 * (direct memory, no ~15 per-block new/memcpy). LK sets it around the run-ahead save/load
 * only, so SD/manual saves stay in the portable normal format. Video/audio bits are always
 * on and hard-disable is always off, so this never changes rendering or audio. */
static volatile int s_ra_fast;
static void snes_set_ra_fast(int on) { s_ra_fast = on ? 1 : 0; }

/* ---- libretro callbacks ---- */
static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format *f = (const enum retro_pixel_format *)data;
		return *f == RETRO_PIXEL_FORMAT_RGB565;   /* we render RGB565 only */
	}
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		/* VIDEO|AUDIO always on, HARD_DISABLE_AUDIO never (matches the prior declined
		 * behaviour exactly); add FAST_SAVESTATES only during run-ahead. */
		if (data) *(int *)data = (1 | 2) | (s_ra_fast ? 4 : 0);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = (struct retro_variable *)data;
		unsigned i;
		if (!v) return false;
		v->value = 0;
		for (i = 0; i < s_opt_n; i++) if (v->key && str_eq(s_opt[i].key, v->key)) {
			v->value = s_opt[i].val; return true;   /* our override */
		}
		return false;   /* unknown key -> core default */
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool *)data = s_opt_dirty;
		s_opt_dirty = false;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		return true;   /* accept; LK re-reads av_info (base_h/aspect) after option changes */
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool *)data = true;
		return true;
	default:
		return false;   /* decline everything else; snes9x copes with core defaults */
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	s_vid = data; s_vw = w; s_vh = h; s_vpitch = (unsigned)pitch;
}
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
	unsigned b = (s_imp && s_imp->read_buttons) ? s_imp->read_buttons() : 0;
	return (int16_t)((b >> id) & 1u);   /* id = RETRO_DEVICE_ID_JOYPAD_* bit position */
}
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	unsigned room = (s_aud_frames < SNES_AUD_MAX) ? SNES_AUD_MAX - s_aud_frames : 0;
	unsigned n = (frames < room) ? (unsigned)frames : room;
	for (unsigned i = 0; i < n * 2; i++) s_aud[s_aud_frames * 2 + i] = data[i];
	s_aud_frames += n;
	return frames;
}
static void audio_sample_cb(int16_t l, int16_t r)   /* snes9x uses batch, but be safe */
{
	if (s_aud_frames < SNES_AUD_MAX) {
		s_aud[s_aud_frames * 2] = l; s_aud[s_aud_frames * 2 + 1] = r;
		s_aud_frames++;
	}
}

/* ---- time for SRTC / BS-X real-time-clock carts (uses the LK host clock) ---- */
extern "C" time_t time(time_t *t)
{
	long v = (s_imp && s_imp->host_time) ? s_imp->host_time() : 0;
	if (t) *t = (time_t)v;
	return (time_t)v;
}
extern "C" struct tm *localtime(const time_t *t)
{
	static struct tm tmv;
	long s = t ? (long)*t : 0;
	long days = s / 86400; long rem = s % 86400;
	if (rem < 0) { rem += 86400; days--; }
	tmv.tm_sec = rem % 60; tmv.tm_min = (rem / 60) % 60; tmv.tm_hour = rem / 3600;
	tmv.tm_wday = (int)((days % 7 + 4) % 7 + 7) % 7;   /* 1970-01-01 = Thursday */
	long y = 1970;
	for (;;) {
		int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
		long yd = leap ? 366 : 365;
		if (days < yd) break;
		days -= yd; y++;
	}
	int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
	static const int mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	tmv.tm_year = (int)(y - 1900); tmv.tm_yday = (int)days;
	int m = 0;
	for (; m < 12; m++) { int dm = mdays[m] + (m == 1 && leap ? 1 : 0); if (days < dm) break; days -= dm; }
	tmv.tm_mon = m; tmv.tm_mday = (int)days + 1; tmv.tm_isdst = 0;
	return &tmv;
}

/* ---- exports ---- */
extern "C" void snes_heap_init(void *base, unsigned size);   /* snes_shim.cpp */

extern "C" void snes_run_init_array(void);   /* snes_shim.cpp (walks __init_array_*) */

static void snes_init(void)
{
	/* Run C++ static constructors HERE, not in snes_core_blob_init: the global SNES::smp
	 * ctor does `apuram = new uint8[64K]`, and operator new is the bump arena which is only
	 * armed by heap_init(). LK calls heap_init() immediately before init(), so by now the
	 * arena is live; running .init_array at blob-load time (before heap_init) made every
	 * `new` return NULL - apuram=NULL sent all SPC RAM/port writes to physical 0 (lost,
	 * read back 0), deadlocking the CPU<->APU $2140 handshake => frozen black, no audio. */
	snes_run_init_array();
	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();
	retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}

static int snes_load(const void *rom, unsigned size)
{
	struct retro_game_info info;
	info.path = 0; info.data = rom; info.size = size; info.meta = 0;
	return retro_load_game(&info) ? 0 : -1;
	/* NOTE: DSP interpolation (Gaussian default) is user-selectable via the Pico "Audio Filter"
	 * menu (snes9x_audio_interpolation). Measured cost at 1400 MHz: Gaussian 121 fps, Linear
	 * 128 fps (+7). Left at the authentic Gaussian default; the player opts into Linear/None
	 * for more speed. Do NOT hardcode it here - that overrides the menu choice. */
}

static void snes_reset_(void) { retro_reset(); }
static void snes_unload(void) { retro_unload_game(); retro_deinit(); }

static void snes_run(struct snes_frame *out)
{
	s_vid = 0; s_aud_frames = 0;
	retro_run();
	if (out) {
		out->video = s_vid; out->width = s_vw; out->height = s_vh; out->pitch = s_vpitch;
		out->audio = s_aud; out->frames = s_aud_frames;
	}
}

static void snes_av_info(unsigned *base_w, unsigned *base_h,
			 unsigned *max_w, unsigned *max_h, unsigned *sr)
{
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	if (base_w) *base_w = av.geometry.base_width;
	if (base_h) *base_h = av.geometry.base_height;
	if (max_w)  *max_w  = av.geometry.max_width;
	if (max_h)  *max_h  = av.geometry.max_height;
	if (sr)     *sr     = (unsigned)av.timing.sample_rate;
}

static unsigned snes_aspect_x1000(void)
{
	struct retro_system_av_info av;
	retro_get_system_av_info(&av);
	float a = av.geometry.aspect_ratio;
	if (a <= 0.0f) a = (float)av.geometry.base_width / (float)av.geometry.base_height;
	return (unsigned)(a * 1000.0f + 0.5f);
}

static void    *snes_sram_ptr(void)  { return retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); }
static unsigned  snes_sram_size(void) { return (unsigned)retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); }

static unsigned snes_dbg_pc(void) { return 0; }   /* reserved debug hook (unused) */

/* idx 0 = bump-arena high-water (bytes); other indices reserved for future diagnostics. */
extern "C" unsigned snes_heap_used(void);   /* snes_shim.cpp */
extern "C" void *snes_heap_mark(void);      /* snes_shim.cpp: arena mark/reset for run-ahead */
extern "C" void  snes_heap_reset(void *m);
static unsigned snes_dbg_get(unsigned idx)
{
	switch (idx) {
	case 0: return snes_heap_used();
	default: return 0;
	}
}

static unsigned snes_state_size(void) { return (unsigned)retro_serialize_size(); }
static int snes_state_save(void *buf, unsigned size) { return retro_serialize(buf, size) ? 0 : -1; }
static int snes_state_load(const void *buf, unsigned size) { return retro_unserialize(buf, size) ? 0 : -1; }

static const struct snes_core_exports g_exports = {
	SNES_CORE_ABI_MAGIC,
	SNES_CORE_ABI_VERSION,
	snes_heap_init,
	snes_init,
	snes_load,
	snes_reset_,
	snes_unload,
	snes_run,
	snes_av_info,
	snes_sram_ptr,
	snes_sram_size,
	snes_state_size,
	snes_state_save,
	snes_state_load,
	snes_dbg_pc,
	snes_dbg_get,
	snes_set_option,
	snes_aspect_x1000,
	snes_heap_mark,
	snes_heap_reset,
	snes_set_ra_fast,
};

/* Blob entry: stash the imports and return the export table. NOTE: .init_array (libstdc++
 * ios_base::Init + the global SNES::smp/Memory ctors) is NOT run here - it is deferred to
 * snes_init(), which LK calls after heap_init() arms the bump allocator. Running it here
 * (before heap_init) made operator new return NULL for those global ctors. */
extern "C" const struct snes_core_exports *snes_core_blob_init(const struct snes_core_imports *imp)
{
	s_imp = imp;
	return &g_exports;
}
