/*
 * host_test.c - headless Genesis-Plus-GX validator, built natively from the same core sources as
 * the ARM boot_b blob (emu/genesis/build_host_test.sh). Loads a ROM via the GET_GAME_INFO_EXT
 * buffer path (exactly like genesis_core_exports.c), runs frames, and round-trips a save state.
 * With no ROM arg it synthesises a minimal valid Mega Drive ROM so the core still boots + runs
 * (validates compile/init/load/run/serialize; real gameplay content needs a real ROM).
 *
 * Build + run:  emu/genesis/build_host_test.sh  &&  /tmp/gpgx_host_test [rom.md] [frames]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <libretro.h>

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
void     retro_get_system_av_info(struct retro_system_av_info *);
void     retro_set_controller_port_device(unsigned, unsigned);
size_t   retro_serialize_size(void);
bool     retro_serialize(void *, size_t);
bool     retro_unserialize(const void *, size_t);

static struct retro_game_info_ext s_ext;
static unsigned vcalls, vw, vh, vpitch;
static uint64_t vhash;
static unsigned aud_frames;

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		const enum retro_pixel_format *f = data;
		return *f == RETRO_PIXEL_FORMAT_RGB565;
	}
	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
		if (!data || !s_ext.data) return false;
		*(const struct retro_game_info_ext **)data = &s_ext;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool *)data = true;
		return true;
	default:
		return false;
	}
}
static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	vcalls++; vw = w; vh = h; vpitch = (unsigned)pitch;
	if (data) {
		const uint8_t *p = data; uint64_t hsh = 1469598103934665603ULL;
		unsigned y;
		for (y = 0; y < h; y++) {
			const uint8_t *row = p + (size_t)y * pitch; unsigned x;
			for (x = 0; x < w * 2; x++) { hsh ^= row[x]; hsh *= 1099511628211ULL; }
		}
		vhash = hsh;
	}
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned a, unsigned b, unsigned c, unsigned d)
{ (void)a;(void)b;(void)c;(void)d; return 0; }
static size_t audio_batch_cb(const int16_t *d, size_t frames) { (void)d; aud_frames += frames; return frames; }
static void audio_sample_cb(int16_t l, int16_t r) { (void)l;(void)r; aud_frames++; }

/* Minimal valid Mega Drive ROM: 68k vector table + "SEGA GENESIS" header + a BRA-self program so
 * the 68000/VDP/timing actually run (blank output, but the core emulates and serialises). */
static unsigned char *make_md_rom(long *out_size)
{
	long size = 512 * 1024;
	unsigned char *r = calloc(1, size);
	unsigned i;
	/* vector table (big-endian): [0]=initial SSP, [1]=reset PC; rest point at the reset stub */
	unsigned int ssp = 0x00FFFF00u, pc = 0x00000200u;
	r[0]=ssp>>24; r[1]=ssp>>16; r[2]=ssp>>8; r[3]=ssp;
	r[4]=pc>>24; r[5]=pc>>16; r[6]=pc>>8; r[7]=pc;
	for (i = 2; i < 64; i++) { r[i*4+0]=pc>>24; r[i*4+1]=pc>>16; r[i*4+2]=pc>>8; r[i*4+3]=pc; }
	memcpy(r + 0x100, "SEGA GENESIS    ", 16);
	memcpy(r + 0x110, "(C)TEST 2026.JUL", 16);
	memcpy(r + 0x120, "GENESIS HOST TEST                               ", 48);
	memcpy(r + 0x150, "GENESIS HOST TEST                               ", 48);
	memcpy(r + 0x180, "GM 00000000-00", 14);
	memcpy(r + 0x190, "J               ", 16);   /* I/O support */
	/* ROM/RAM address range */
	r[0x1A0]=0x00; r[0x1A1]=0x00; r[0x1A2]=0x00; r[0x1A3]=0x00;                    /* ROM start */
	r[0x1A4]=(size-1)>>24; r[0x1A5]=(size-1)>>16; r[0x1A6]=(size-1)>>8; r[0x1A7]=(size-1); /* ROM end */
	r[0x1A8]=0x00; r[0x1A9]=0xFF; r[0x1AA]=0x00; r[0x1AB]=0x00;                    /* RAM start 0xFF0000 */
	r[0x1AC]=0x00; r[0x1AD]=0xFF; r[0x1AE]=0xFF; r[0x1AF]=0xFF;                    /* RAM end   0xFFFFFF */
	memcpy(r + 0x1F0, "JUE             ", 16);   /* region */
	/* reset stub at 0x200: BRA.S *  (0x60 0xFE) - tight self-loop; CPU runs, VDP renders blank */
	r[0x200] = 0x60; r[0x201] = 0xFE;
	*out_size = size;
	return r;
}

int main(int argc, char **argv)
{
	unsigned char *buf; long sz; int frames = (argc >= 3) ? atoi(argv[2]) : 600;
	struct retro_game_info gi; struct retro_system_av_info av;
	int distinct = 0; uint64_t prev = 0;
	int ss_ok = 0;

	if (argc >= 2) {
		FILE *f = fopen(argv[1], "rb");
		if (!f) { perror("open"); return 2; }
		fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
		buf = malloc(sz);
		if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
		fclose(f);
		printf("ROM: %s (%ld bytes)\n", argv[1], sz);
	} else {
		buf = make_md_rom(&sz);
		printf("ROM: <synthetic minimal Mega Drive> (%ld bytes)\n", sz);
	}

	retro_set_environment(env_cb);
	retro_set_video_refresh(video_cb);
	retro_set_audio_sample(audio_sample_cb);
	retro_set_audio_sample_batch(audio_batch_cb);
	retro_set_input_poll(input_poll_cb);
	retro_set_input_state(input_state_cb);
	retro_init();

	memset(&s_ext, 0, sizeof s_ext);
	s_ext.dir = "/"; s_ext.name = "rom"; s_ext.ext = "md";
	s_ext.data = buf; s_ext.size = sz; s_ext.file_in_archive = true;
	memset(&gi, 0, sizeof gi); gi.data = buf; gi.size = sz;
	if (!retro_load_game(&gi)) { fprintf(stderr, "LOAD FAILED\n"); return 1; }
	s_ext.data = 0;
	retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	retro_get_system_av_info(&av);
	printf("av_info: base %ux%u max %ux%u fps %.4f sr %.1f\n",
		av.geometry.base_width, av.geometry.base_height,
		av.geometry.max_width, av.geometry.max_height, av.timing.fps, av.timing.sample_rate);

	for (int i = 0; i < frames; i++) { retro_run(); if (vhash != prev) { distinct++; prev = vhash; } }
	printf("ran %d frames: video_cb=%u last dims=%ux%u pitch=%u audio_frames=%u\n",
		frames, vcalls, vw, vh, vpitch, aud_frames);
	printf("distinct frame hashes=%d\n", distinct);

	/* save-state round-trip (mirrors the LK genesis_sd_run.c sequence) */
	{
		size_t ssz = retro_serialize_size();
		printf("\nsave-state: serialize_size=%zu bytes\n", ssz);
		if (ssz) {
			void *sbuf = malloc(ssz);
			if (retro_serialize(sbuf, ssz)) {
				retro_run(); uint64_t h_after = vhash;
				for (int i = 0; i < 120; i++) retro_run();
				if (retro_unserialize(sbuf, ssz)) {
					retro_run(); uint64_t h_restored = vhash;
					printf("save-state: h_after=%016llx h_restored=%016llx\n",
						(unsigned long long)h_after, (unsigned long long)h_restored);
					ss_ok = (h_after == h_restored);
				} else printf("save-state: UNSERIALIZE FAILED\n");
			} else printf("save-state: SERIALIZE FAILED\n");
			free(sbuf);
		}
	}
	printf("save-state RESULT: %s\n", ss_ok ? "PASS (deterministic round-trip)" : "FAIL");

	/* serialize throughput: the LK rewind ring captures a full state every frame (GPGX has no raw
	 * fast path), so this must be cheap relative to a ~16.6ms frame. x86 timing is a rough proxy
	 * for the ARM A76 cost. */
	{
		size_t ssz = retro_serialize_size();
		void *sb = malloc(ssz);
		struct timespec t0, t1; int N = 300, i;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < N; i++) retro_serialize(sb, ssz);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1000.0 / N;
		printf("serialize: size=%zu bytes, %.1f us/call on host x86 (rewind captures 1/frame)\n", ssz, us);
		free(sb);
	}

	{
		int ran_ok = (vcalls > 0 && vw >= 160 && vh >= 100);
		int ok = ran_ok && ss_ok;
		printf("\nRESULT: %s (core built, loaded, ran %u video frames, save-state %s)\n",
			ok ? "PASS" : "PARTIAL", vcalls, ss_ok ? "ok" : "FAILED");
		retro_unload_game(); retro_deinit();
		return ok ? 0 : 1;
	}
}
