/*
 * render_status_test.c - checks render_line_status (core/vdp_render.c) against the full render on
 * the host: the same ROM and input script run with the full render, with
 * render_line_status, and with the plain skip, and the save state is hashed every frame. The
 * sprites-only pass must match the full render on every frame; the plain skip is expected to drift
 * on games that use sprite collision. NOHASH=1 times the three without the hashing.
 *
 *   emu/genesis/build_render_status_test.sh && /tmp/gpgx_render_status_test rom.bin [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <libretro.h>
void retro_set_environment(retro_environment_t); void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t); void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t); void retro_set_input_state(retro_input_state_t);
void retro_init(void); bool retro_load_game(const struct retro_game_info *); void retro_run(void);
void retro_set_controller_port_device(unsigned, unsigned); size_t retro_serialize_size(void);
bool retro_serialize(void *, size_t); bool retro_unserialize(const void *, size_t);
extern int render_status_only;

static struct retro_game_info_ext s_ext;
static int s_video = 1;
static unsigned s_frame;
static uint16_t s_pad;
static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT: if (!data || !s_ext.data) return false; *(const struct retro_game_info_ext **)data = &s_ext; return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: if (data) *(int *)data = (s_video ? 1 : 0) | 2; return true;
	default: return false;
	}
}
static void video_cb(const void *d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{ (void)dev;(void)idx; if (port) return 0; if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)s_pad; return (s_pad >> id) & 1; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void audio_sample_cb(int16_t l, int16_t r) { (void)l;(void)r; }
/* scripted input: start presses to get through menus, then held directions and button mashing */
static uint16_t script(unsigned f)
{
	uint32_t x = f * 2654435761u;
	uint16_t p = 0;
	if ((f % 180) < 6) p |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
	if (f > 600) {
		unsigned seg = (f / 45) * 2246822519u >> 28;
		if (seg & 1) p |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT; else if (seg & 2) p |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
		if (seg & 4) p |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
		if ((x >> 29) & 1) p |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
		if ((x >> 27) & 1) p |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
		if (((x >> 25) & 3) == 0) p |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	}
	return p;
}
static uint64_t fnv(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ULL; while (n--) { h ^= *p++; h *= 1099511628211ULL; } return h; }
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb"); long sz; unsigned char *buf; int frames = argc > 2 ? atoi(argv[2]) : 6000;
	struct retro_game_info gi; size_t ssz; void *s0, *st; uint64_t *h[3]; double ms[3]; int mode, i;
	fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); buf = malloc(sz); fread(buf, 1, sz, f); fclose(f);
	retro_set_environment(env_cb); retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb); retro_set_input_poll(input_poll_cb); retro_set_input_state(input_state_cb);
	retro_init();
	memset(&s_ext, 0, sizeof s_ext); s_ext.dir = "/"; s_ext.name = "rom"; s_ext.ext = "md"; s_ext.data = buf; s_ext.size = sz; s_ext.file_in_archive = true;
	memset(&gi, 0, sizeof gi); gi.data = buf; gi.size = sz;
	if (!retro_load_game(&gi)) { printf("load failed\n"); return 1; }
	s_ext.data = 0; retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	ssz = retro_serialize_size(); s0 = malloc(ssz); st = malloc(ssz); retro_serialize(s0, ssz);
	for (mode = 0; mode < 3; mode++) {
		struct timespec t0, t1;
		h[mode] = malloc(sizeof(uint64_t) * frames);
		retro_unserialize(s0, ssz);
		s_video = (mode == 0); render_status_only = (mode == 1);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < frames; i++) {
			s_pad = script(i); retro_run();
			if (!getenv("NOHASH")) { retro_serialize(st, ssz); h[mode][i] = fnv(st, ssz); } else h[mode][i] = 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ms[mode] = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	}
	{
		int d1 = -1, d2 = -1;
		for (i = 0; i < frames; i++) { if (d1 < 0 && h[1][i] != h[0][i]) d1 = i; if (d2 < 0 && h[2][i] != h[0][i]) d2 = i; }
		printf("%-40s %d frames | status-only: %s | plain skip: %s | ms full %.0f status %.0f skip %.0f (incl serialize)\n",
		       argv[1], frames, d1 < 0 ? "IDENTICAL" : "DIVERGES", d2 < 0 ? "identical" : "diverges",
		       ms[0], ms[1], ms[2]);
		if (d1 >= 0) printf("   status-only first differs at frame %d\n", d1);
		if (d2 >= 0) printf("   plain skip first differs at frame %d\n", d2);
		return d1 >= 0;
	}
}
