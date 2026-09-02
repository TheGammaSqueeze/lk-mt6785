/*
 * host_test.cpp - HEADLESS validation of the snes9x core (the SAME sources that go into
 * the ARM boot_b blob), built natively. Loads a ROM via retro_load_game and runs frames,
 * checking the video callback fires with sane SNES dimensions and real (non-uniform,
 * CHANGING) RGB565 content. This confirms the core itself works before trusting the LK
 * integration, and is a regression tool for future core-source updates.
 *
 * Build + run:  emu/snes9x/build_host_test.sh  &&  /tmp/s9x_host_test <rom.sfc> [frames]
 *
 * Verified 2026-09-02: Super Mario World + Street Fighter II Turbo both PASS
 * (av_info 256x224, fps 60.0988, sr 32040, pitch 2048=1024px stride, changing content).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "libretro/libretro.h"

extern "C" {
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_init(void);
bool retro_load_game(const struct retro_game_info *);
void retro_run(void);
void retro_get_system_av_info(struct retro_system_av_info *);
void retro_set_controller_port_device(unsigned, unsigned);
}

static unsigned vw, vh, vpitch, vcalls;
static uint64_t vhash;
static bool env_cb(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) { if (data) ((retro_variable *)data)->value = 0; return false; }
    if (cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE) { if (data) *(bool *)data = true; return true; }
    return false;
}
static void video_cb(const void *d, unsigned w, unsigned h, size_t pitch) {
    vw = w; vh = h; vpitch = pitch; vcalls++;
    if (d) { uint64_t hs = 1469598103934665603ULL;
        for (unsigned y = 0; y < h; y++) { const uint16_t *row = (const uint16_t *)((const uint8_t *)d + y * pitch);
            for (unsigned x = 0; x < w; x++) hs = (hs ^ row[x]) * 1099511628211ULL; }
        vhash = hs; }
}
static void ap_cb(void) {}
static int16_t is_cb(unsigned, unsigned, unsigned, unsigned) { return 0; }
static size_t ab_cb(const int16_t *, size_t f) { return f; }
static void as_cb(int16_t, int16_t) {}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s rom.sfc [frames]\n", argv[0]); return 2; }
    int frames = (argc >= 3) ? atoi(argv[2]) : 900;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; } fclose(f);
    printf("ROM: %s (%ld bytes)\n", argv[1], sz);

    retro_set_environment(env_cb); retro_set_video_refresh(video_cb);
    retro_set_audio_sample(as_cb); retro_set_audio_sample_batch(ab_cb);
    retro_set_input_poll(ap_cb); retro_set_input_state(is_cb);
    retro_init(); retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    struct retro_game_info gi; memset(&gi, 0, sizeof gi); gi.data = buf; gi.size = sz;
    if (!retro_load_game(&gi)) { fprintf(stderr, "LOAD FAILED\n"); return 1; }
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    printf("av_info: base %ux%u max %ux%u fps %.4f sr %.1f\n",
        av.geometry.base_width, av.geometry.base_height, av.geometry.max_width,
        av.geometry.max_height, av.timing.fps, av.timing.sample_rate);

    unsigned distinct = 0; uint64_t prev = 0;
    for (int i = 0; i < frames; i++) { retro_run(); if (vhash != prev) { distinct++; prev = vhash; } }
    printf("ran %d frames: video_cb calls=%u last dims=%ux%u pitch=%u\n", frames, vcalls, vw, vh, vpitch);
    printf("distinct frame hashes=%u (content is %s)\n", distinct, distinct > 2 ? "CHANGING (real gameplay)" : "static/blank");
    bool ok = vcalls > 0 && vw >= 200 && vw <= 600 && vh >= 200 && vh <= 512 && distinct > 2;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
