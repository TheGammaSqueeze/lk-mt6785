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

/* ---- libretro callbacks ---- */
static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format *f = (const enum retro_pixel_format *)data;
		return *f == RETRO_PIXEL_FORMAT_RGB565;   /* we render RGB565 only */
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		if (data) ((struct retro_variable *)data)->value = 0;   /* use core defaults */
		return false;
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

static void snes_init(void)
{
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

static void    *snes_sram_ptr(void)  { return retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); }
static unsigned  snes_sram_size(void) { return (unsigned)retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); }

/* the 4 SPC700 OUTPUT ports the main CPU reads at $2140-$2143 (S9xAPUReadPort returns
 * SNES::smp.port_read). At boot the IPL ROM writes $AA to port0 and $BB to port1 (ready
 * signal); if the CPU is stuck waiting on the APU handshake this should read 0xBBAA in the
 * low 16 bits - if it does, the SPC IS signalling and the stall is on the CPU-read path. */
static unsigned snes_dbg_pc(void)
{
	/* [7:0] apuram[$00f4] (port0, should be $aa) | [15:8] SPC P reg (dp bit 0x20 => wrong bank)
	 * | [23:16] apuram[$01f4] ($aa here => the write went to bank 1) | [31:24] SPC sp. */
	/* [15:0] SPC700 PC, [23:16] apuram[$f4]. Clear-loop PC ~$ffc5-c7 => never wrote;
	 * PC $ffcf with apuram[$f4]=0 => the store was lost. */
	return ((unsigned)SNES::smp.regs.pc & 0xFFFFu) | ((unsigned)SNES::smp.apuram[0x00f4] << 16);
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
};

/* Blob entry: run C++ static constructors (libstdc++ ios_base::Init etc. - unlike the
 * gambatte blob, snes9x uses std::string/stringstream/map so .init_array MUST run), stash
 * the imports, return the export table. */
extern "C" void snes_run_init_array(void);   /* snes_shim.cpp (walks __init_array_*) */

extern "C" const struct snes_core_exports *snes_core_blob_init(const struct snes_core_imports *imp)
{
	s_imp = imp;
	snes_run_init_array();
	return &g_exports;
}
