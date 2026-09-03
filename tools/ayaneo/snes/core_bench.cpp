/*
 * core_bench.cpp - HOST benchmark of the snes9x core with a SYNTHETIC in-memory ROM, so core
 * hot-path changes can be measured + correctness-checked on the dev box (no game ROM, no device).
 * The ROM is a minimal valid LoROM whose reset code runs a tight 65816 loop (loads/stores/ADC/
 * DEX-BNE/INC/JMP) - it exercises the CPU interpreter (cpuops/cpuexec) plus the fixed per-frame
 * PPU/APU overhead. NOT a substitute for on-device snes-bench (which runs a real game), but a fast
 * local signal for relative speedups + a gprof target.
 *
 * Build:  emu/snes9x/build_host_test.sh is the flag reference; this file is built by
 *         tools/ayaneo/snes/build_core_bench.sh  ->  /tmp/s9x_core_bench [frames]
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
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
size_t retro_serialize_size(void);
bool retro_serialize(void *, size_t);
bool retro_unserialize(const void *, size_t);
}

static uint64_t vhash; static unsigned vcalls;
static bool env_cb(unsigned cmd, void *data) {
    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE) { if (data) ((retro_variable *)data)->value = 0; return false; }
    if (cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE) { if (data) *(bool *)data = true; return true; }
    return false;
}
static void video_cb(const void *d, unsigned w, unsigned h, size_t pitch) {
    vcalls++;
    if (d) { uint64_t hs = 1469598103934665603ULL;
        for (unsigned y = 0; y < h; y++) { const uint16_t *row = (const uint16_t *)((const uint8_t *)d + y * pitch);
            for (unsigned x = 0; x < w; x++) hs = (hs ^ row[x]) * 1099511628211ULL; }
        vhash = hs; }
}
static void ap_cb(void) {}
static int16_t is_cb(unsigned, unsigned, unsigned, unsigned) { return 0; }
static size_t ab_cb(const int16_t *, size_t f) { return f; }
static void as_cb(int16_t, int16_t) {}

/* Build a 32 KB LoROM in-memory. Code at file 0x0000 == CPU addr $00:8000. */
static void build_rom(uint8_t *rom) {
    memset(rom, 0, 0x8000);
    /* CPU-exercising loop at $8000 (emulation mode, 8-bit A/X) */
    static const uint8_t code[] = {
        0x78,                   /* SEI                */
        0x18,                   /* CLC                */
        /* loop: ($8002) */
        0xA9, 0x34,             /* LDA #$34           */
        0x85, 0x10,             /* STA $10   (DP w)   */
        0xA5, 0x10,             /* LDA $10   (DP r)   */
        0x18,                   /* CLC                */
        0x69, 0x12,             /* ADC #$12           */
        0x8D, 0x00, 0x02,       /* STA $0200 (abs w)  */
        0xAD, 0x00, 0x02,       /* LDA $0200 (abs r)  */
        0xA2, 0x20,             /* LDX #$20           */
        /* inner: ($8013) */
        0xCA,                   /* DEX                */
        0xD0, 0xFD,             /* BNE inner          */
        0xEE, 0x00, 0x02,       /* INC $0200          */
        0x4C, 0x02, 0x80,       /* JMP $8002 (loop)   */
    };
    memcpy(rom + 0x0000, code, sizeof code);
    /* LoROM header at file 0x7FC0 */
    uint8_t *h = rom + 0x7FC0;
    memcpy(h, "SNES CORE BENCH ROM  ", 21); /* title */
    h[0x15] = 0x20;   /* map mode: LoROM, slow */
    h[0x16] = 0x00;   /* cart type: ROM only */
    h[0x17] = 0x05;   /* ROM size: 256 Kbit region code (log2(KB)-ish); snes9x scores loosely */
    h[0x18] = 0x00;   /* RAM size */
    h[0x19] = 0x01;   /* country (NTSC) */
    h[0x1A] = 0x33;   /* dev id */
    h[0x1B] = 0x00;   /* version */
    /* checksum: complement + sum (snes9x recomputes; set consistent-ish) */
    uint16_t sum = 0; for (int i = 0; i < 0x8000; i++) sum += rom[i];
    h[0x1C] = (uint8_t)(~sum); h[0x1D] = (uint8_t)((~sum) >> 8);
    h[0x1E] = (uint8_t)sum;    h[0x1F] = (uint8_t)(sum >> 8);
    /* emulation vectors (0x7FF0..0x7FFF): RESET at 0x7FFC = $8000 */
    uint8_t *v = rom + 0x7FE0;
    for (int i = 0; i < 32; i++) v[i] = 0x00;
    rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;   /* RESET -> $8000 */
    rom[0x7FFE] = 0x00; rom[0x7FFF] = 0x80;   /* IRQ/BRK -> $8000 */
}

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3 + t.tv_nsec/1e6; }

int main(int argc, char **argv) {
    int frames = (argc >= 2) ? atoi(argv[1]) : 4000;
    uint8_t *rom = (uint8_t *)malloc(0x8000);
    build_rom(rom);

    retro_set_environment(env_cb); retro_set_video_refresh(video_cb);
    retro_set_audio_sample(as_cb); retro_set_audio_sample_batch(ab_cb);
    retro_set_input_poll(ap_cb); retro_set_input_state(is_cb);
    retro_init(); retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    struct retro_game_info gi; memset(&gi, 0, sizeof gi); gi.data = rom; gi.size = 0x8000;
    if (!retro_load_game(&gi)) { fprintf(stderr, "LOAD FAILED (synthetic ROM rejected)\n"); return 1; }
    struct retro_system_av_info av; retro_get_system_av_info(&av);
    printf("loaded synthetic ROM: base %ux%u fps %.4f sr %.1f\n",
        av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);

    for (int i = 0; i < 60; i++) retro_run();   /* warm */
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) retro_run();
    double t1 = now_ms();
    double per = (t1 - t0) / frames;
    printf("ran %d frames, video_cb=%u, vhash=%016llx\n", frames, vcalls, (unsigned long long)vhash);
    printf("=> %.4f ms/frame  (%.1f emulated fps on host)\n", per, 1000.0 / per);

    /* correctness: savestate round-trip must be deterministic */
    size_t ssz = retro_serialize_size();
    bool ss_ok = false;
    if (ssz > 0 && ssz < (16u<<20)) {
        uint8_t *sb = (uint8_t*)malloc(ssz);
        if (retro_serialize(sb, ssz)) {
            retro_run(); uint64_t a = vhash;
            for (int i=0;i<120;i++) retro_run();
            retro_unserialize(sb, ssz);
            retro_run(); uint64_t r = vhash;
            ss_ok = (r == a);
        }
        free(sb);
    }
    printf("savestate round-trip: %s\n", ss_ok ? "PASS" : "FAIL");
    return 0;
}
