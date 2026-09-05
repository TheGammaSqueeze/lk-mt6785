/*
 * genesis_core_exports.c - blob side of the loadable Genesis-Plus-GX core (see genesis_core_abi.h).
 *
 * Compiled INTO the blob. Installs the libretro callbacks, drives GPGX's retro_* API, and
 * publishes the export table LK uses. The ROM is fed as a BUFFER without any source patch: GPGX's
 * load_archive() already has a "game already in memory" path (g_rom_data), populated from the
 * RETRO_ENVIRONMENT_GET_GAME_INFO_EXT reply, so env_cb answers that with a retro_game_info_ext
 * pointing at the buffer. The ext field ("md"/"sms"/"gg"/"sg") is what GPGX uses to pick the
 * emulated system. Video is RGB565, audio is captured per frame from audio_batch, input comes from
 * the LK pad via the imports table.
 */
#include <libretro.h>
#include "genesis_core_abi.h"

/* ---- libretro public API (from libretro/libretro.c) ---- */
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

/* heap services (genesis_shim.c) */
void     genesis_heap_init(void *base, unsigned size);
unsigned genesis_heap_used(void);
void    *genesis_heap_mark(void);
void     genesis_heap_reset(void *m);

static const struct genesis_core_imports *s_imp;

/* extended game info handed to GPGX via env GET_GAME_INFO_EXT so its load_archive() copies the ROM
 * from memory (g_rom_data) instead of opening a file. Filled by genesis_load() before retro_load_game. */
static struct retro_game_info_ext s_ext;
static char s_ext_name[8] = "rom";
static char s_ext_ext[4];

/* video frame stash (set by the video callback during retro_run) */
static const void *s_vid;
static unsigned    s_vw, s_vh, s_vpitch;

/* audio accumulation (interleaved s16 stereo). One 60 Hz frame at 44.1 kHz = ~735 pairs; SMS/GG
 * PAL 50 Hz ~882. 2048 is ample headroom. */
#define GEN_AUD_MAX 2048u
static short    s_aud[GEN_AUD_MAX * 2];
static unsigned s_aud_frames;

/* core-option store (LK sets these via genesis_set_option; GPGX reads them through env GET_VARIABLE) */
#define GEN_OPT_MAX 12
static struct { char key[40]; char val[24]; } s_opt[GEN_OPT_MAX];
static unsigned s_opt_n;
static int      s_opt_dirty;

static int str_eq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void str_cpy(char *d, const char *s, unsigned cap)
{ unsigned i = 0; for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

static int genesis_set_option(const char *key, const char *value)
{
	unsigned i;
	if (!key || !value) return -1;
	for (i = 0; i < s_opt_n; i++) if (str_eq(s_opt[i].key, key)) {
		str_cpy(s_opt[i].val, value, sizeof s_opt[i].val); s_opt_dirty = 1; return 0;
	}
	if (s_opt_n >= GEN_OPT_MAX) return -1;
	str_cpy(s_opt[s_opt_n].key, key, sizeof s_opt[s_opt_n].key);
	str_cpy(s_opt[s_opt_n].val, value, sizeof s_opt[s_opt_n].val);
	s_opt_n++; s_opt_dirty = 1;
	return 0;
}

/* per-frame render/audio skip for run-ahead look-ahead frames (GPGX honours GET_AUDIO_VIDEO_ENABLE) */
static volatile int s_ra_novideo, s_ra_noaudio;
static void genesis_set_av_skip(int nv, int na) { s_ra_novideo = nv ? 1 : 0; s_ra_noaudio = na ? 1 : 0; }

/* run-ahead FAST_SAVESTATES: OR'd into GET_AUDIO_VIDEO_ENABLE bit 2 (value 4). When on, GPGX's
 * retro_serialize/unserialize save/restore the blip-buffer + FM/CD audio-phase side-channel and
 * suppress the state_load blip_clear, so a save/load pair is audio-continuous (fixes run-ahead crackle).
 * Scoped by LK to the run-ahead save/load pair only (single global latch - see genesis_core_abi.h). */
static volatile int s_ra_fast;
static void genesis_set_ra_fast(int on) { s_ra_fast = on ? 1 : 0; }
/* re-baseline the audio synthesis after a (non-fast) rewind state_load. */
extern void sound_rebase(void);   /* core/sound/sound.c */
static void genesis_sound_rebase(void) { sound_rebase(); }

/* ---- libretro callbacks ---- */
static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format *f = (const enum retro_pixel_format *)data;
		return *f == RETRO_PIXEL_FORMAT_RGB565;   /* we render RGB565 only */
	}
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		/* VIDEO(bit0) on unless a look-ahead render is skipped; AUDIO(bit1) always on;
		 * FAST_SAVESTATES(bit2) during a run-ahead save/load pair so the audio phase is preserved;
		 * HARD_DISABLE_AUDIO(bit3) when a look-ahead frame's audio is skipped. */
		if (data) *(int *)data = (s_ra_novideo ? 0 : 1) | 2 | (s_ra_fast ? 4 : 0) | (s_ra_noaudio ? 8 : 0);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = (struct retro_variable *)data;
		unsigned i;
		if (!v) return false;
		v->value = 0;
		for (i = 0; i < s_opt_n; i++) if (v->key && str_eq(s_opt[i].key, v->key)) {
			v->value = s_opt[i].val; return true;
		}
		return false;   /* unknown key -> core default */
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool *)data = s_opt_dirty ? true : false;
		s_opt_dirty = 0;
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		return true;   /* accept; LK re-reads av_info after option changes */
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
		if (!data || !s_ext.data) return false;
		*(const struct retro_game_info_ext **)data = &s_ext;
		return true;
	default:
		return false;   /* decline everything else (incl. GET_SYSTEM/SAVE_DIRECTORY -> no bootrom) */
	}
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{ s_vid = data; s_vw = w; s_vh = h; s_vpitch = (unsigned)pitch; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
	unsigned b;
	(void)index;
	if (device != RETRO_DEVICE_JOYPAD) return 0;
	b = (s_imp && s_imp->read_buttons) ? s_imp->read_buttons(port) : 0;
	return (int16_t)((b >> id) & 1u);
}
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
	unsigned room = (s_aud_frames < GEN_AUD_MAX) ? GEN_AUD_MAX - s_aud_frames : 0;
	unsigned n = (frames < room) ? (unsigned)frames : room, i;
	for (i = 0; i < n * 2; i++) s_aud[s_aud_frames * 2 + i] = data[i];
	s_aud_frames += n;
	return frames;
}
static void audio_sample_cb(int16_t l, int16_t r)
{
	if (s_aud_frames < GEN_AUD_MAX) {
		s_aud[s_aud_frames * 2] = l; s_aud[s_aud_frames * 2 + 1] = r; s_aud_frames++;
	}
}

/* ---- exports ---- */
static void genesis_init(void)
{
	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();
	/* NOTE: retro_set_controller_port_device is deferred to genesis_load(): GPGX's io_init/
	 * input_init touch per-system input state that only exists AFTER a game is loaded, so calling
	 * it here (before load) dereferences uninitialised state and crashes (caught by host_test). */
}

static const char *ext_for(int system)
{
	switch (system) {
	case GEN_SYS_SMS: return "sms";
	case GEN_SYS_GG:  return "gg";
	case GEN_SYS_SG:  return "sg";
	default:          return "md";   /* MD / auto */
	}
}

static int genesis_load(const void *rom, unsigned size, int system)
{
	struct retro_game_info info;
	const char *e = ext_for(system);
	int rc;
	str_cpy(s_ext_ext, e, sizeof s_ext_ext);
	s_ext.full_path       = 0;
	s_ext.archive_path    = 0;
	s_ext.archive_file    = 0;
	s_ext.dir             = "/";
	s_ext.name            = s_ext_name;
	s_ext.ext             = s_ext_ext;        /* drives GPGX system detection */
	s_ext.meta            = 0;
	s_ext.data            = rom;
	s_ext.size            = size;
	s_ext.file_in_archive = true;             /* no physical file; GPGX fakes content_path from dir/name/ext */
	s_ext.persistent_data = false;
	info.path = 0; info.data = rom; info.size = size; info.meta = 0;
	rc = retro_load_game(&info) ? 0 : -1;
	s_ext.data = 0;                           /* stop answering GET_GAME_INFO_EXT after load */
	if (rc == 0) {                            /* now that the system+input exist, select the pads */
		/* Mega Drive: 6-button pad (subclass 1) so the device X/Y/L/R/Select reach Genesis
		 * X/Y/Z/Mode; 8-bit systems (SMS/GG/SG) use the plain 2-button joypad. */
		unsigned dev = (system == GEN_SYS_MD || system == GEN_SYS_AUTO)
			? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)
			: RETRO_DEVICE_JOYPAD;
		retro_set_controller_port_device(0, dev);
		retro_set_controller_port_device(1, dev);
	}
	return rc;
}

static void genesis_reset_(void) { retro_reset(); }
static void genesis_unload(void) { retro_unload_game(); retro_deinit(); }

static void genesis_run(struct genesis_frame *out)
{
	s_vid = 0; s_aud_frames = 0;
	retro_run();
	if (out) {
		out->video = s_vid; out->width = s_vw; out->height = s_vh; out->pitch = s_vpitch;
		out->audio = s_aud; out->frames = s_aud_frames;
	}
}

static void genesis_av_info(unsigned *base_w, unsigned *base_h,
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

static unsigned genesis_aspect_x1000(void)
{
	struct retro_system_av_info av;
	float a;
	retro_get_system_av_info(&av);
	a = av.geometry.aspect_ratio;
	if (a <= 0.0f) a = (float)av.geometry.base_width / (float)av.geometry.base_height;
	return (unsigned)(a * 1000.0f + 0.5f);
}

static void    *genesis_sram_ptr(void)  { return retro_get_memory_data(RETRO_MEMORY_SAVE_RAM); }
static unsigned  genesis_sram_size(void) { return (unsigned)retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); }

static unsigned genesis_dbg_pc(void) { return 0; }
static unsigned genesis_dbg_get(unsigned idx) { return idx == 0 ? genesis_heap_used() : 0; }

static unsigned genesis_state_size(void) { return (unsigned)retro_serialize_size(); }
static int genesis_state_save(void *buf, unsigned size) { return retro_serialize(buf, size) ? 0 : -1; }
static int genesis_state_load(const void *buf, unsigned size) { return retro_unserialize(buf, size) ? 0 : -1; }

static const struct genesis_core_exports g_exports = {
	GENESIS_CORE_ABI_MAGIC,
	GENESIS_CORE_ABI_VERSION,
	genesis_heap_init,
	genesis_init,
	genesis_load,
	genesis_reset_,
	genesis_unload,
	genesis_run,
	genesis_av_info,
	genesis_sram_ptr,
	genesis_sram_size,
	genesis_state_size,
	genesis_state_save,
	genesis_state_load,
	genesis_dbg_pc,
	genesis_dbg_get,
	genesis_set_option,
	genesis_aspect_x1000,
	genesis_heap_mark,
	genesis_heap_reset,
	genesis_set_av_skip,
	genesis_set_ra_fast,
	genesis_sound_rebase,
};

const struct genesis_core_exports *genesis_core_blob_init(const struct genesis_core_imports *imp)
{
	s_imp = imp;
	return &g_exports;
}
