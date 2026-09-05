# Genesis-Plus-GX core port — progress

Porting libretro **Genesis-Plus-GX** (Sega Genesis/MD, Master System, Game Gear, SG-1000)
into LK as a 4th boot_b core blob, feature parity with the snes9x core. Branch
`lk-gba-emu-sd-card`. Started 2026-09-05. See emu/CORE_PORTING_NOTES.md and the snes9x
port for the recipe. Mirrors the emu/snes9x/ file set (both are libretro cores).

## Decisions (user, 2026-09-05)
- Scope: ALL 4 Sega systems the core supports (MD, SMS, GG, SG-1000). One blob does all.
- Blob in **boot_b (repacked)**, not SD. Plan: place `core_genesis.blob` at boot_b
  `0x01A00000` in the SLACK of the GBC slot (GBC blob is only 0.14MB in a 3MB slot at
  0x01900000). GBC stays at 0x01900000 (now a 1MB effective slot), Genesis gets
  0x01A00000..0x01C00000 = 2MB, and gpSP (0x01C00000) / snes9x (0x01E00000) / settings
  (0x020FF000) are UNCHANGED. If the blob exceeds 2MB, reclaim from the 16MB anim slot.

## Status
- [x] Source vendored to emu/genesis/ (core/ incl cd_hw HARDWARE but no CHD/mp3/vorbis
      image deps; libretro/libretro.c + osd.h + libretro-common). cd_hw compiles but is
      never activated (no CD BIOS/game loaded). Excluded: core/sound/minimp3, cart_hw/yx5200
      (DFPlayer MP3), cd_hw/libchdr.
- [x] **build_core.sh** works: freestanding arm-none-eabi-gcc, LTO, -O2 hot / -Os cold,
      defines -DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -D__LIBRETRO__ -DALIGN_LONG -DALIGN_WORD
      -DUSE_16BPP_RENDERING -DFRONTEND_SUPPORTS_RGB565 -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE.
      Output: emu/genesis/libgenesis.a (70 objects). CLEAN compile.
- [x] genesis_core_abi.h (mirror snes_core_abi.h; adds `system` hint MD/SMS/GG/SG; NO
      state_save_ra/load_ra - GPGX has no raw fast-snapshot, run-ahead uses retro_serialize)
- [x] genesis_core_exports.c (wraps retro_* -> ABI; RGB565 video, s16 audio, input via
      imports read_buttons(port); retro_serialize state; set_option/aspect; av_skip via
      GET_AUDIO_VIDEO_ENABLE which GPGX honours). ROM fed as a BUFFER with NO source patch:
      env_cb answers RETRO_ENVIRONMENT_GET_GAME_INFO_EXT with a retro_game_info_ext pointing at
      the buffer -> GPGX's load_archive() "game already in memory" path (g_rom_data) copies it.
      A synthetic ext ("md"/"sms"/"gg"/"sg") drives GPGX system detection.
- [x] genesis_shim.c (C bump allocator; heap_init zeroes the arena; mark/reset) +
      genesis_blob_libc.c (copied from snes) + genesis_blob_stubs.c (strstr/strdup/setjmp/longjmp
      [ARM asm]/_ctype_ table/crc32 + filestream+rf file-I/O stubs + fill_pathname_join/strl +
      yx5200 no-op stubs).
- [x] genesis_core_blob.ld (VMA 0x4F800000, above snes9x, below arena; magic "SEG1") +
      build_core_blob.sh -> **core_genesis.blob BUILDS: 1,376,333 bytes (1.34 MB, fits the 2MB
      boot_b slot), span 0x43e8f0 (~4.24 MB incl ~3MB BSS)**. libc+stubs compiled NON-LTO.
- [x] build_host_test.sh + host_test.c: **HOST TEST PASSES** (native x86-64, synthetic minimal
      MD ROM). Core builds+inits+loads (via GET_GAME_INFO_EXT buffer path)+runs 600 frames
      (video_cb 600, audio ~735/frame at 44100/60) + **save-state round-trip deterministic**
      (serialize_size ~1.01MB). GOTCHA CAUGHT + FIXED: retro_set_controller_port_device must be
      called AFTER retro_load_game (GPGX io_init/input_init touch per-system state that only
      exists post-load; calling before crashed input_init). Fixed in genesis_core_exports.c
      (deferred to genesis_load) and host_test.c. Run a REAL ROM later on device for gameplay.
- [ ] genesis_core_loader.c (lk_a side, boot_b 0x01A00000 -> VMA 0x4F800000) <-- NEXT STEP
- [ ] genesis_sd_run.c (own thread; native geom show_frame; input; ~44.1kHz audio)
- [ ] Console types GENESIS/SMS/GG/SG in sd_fat.h + ROM folders + launch dispatch
      (gba_driver.c both ROM-select sites) + boxart/badge
- [ ] Parity: save states, SRAM battery, suspend/resume, adaptive FF + HUD, rewind
      delta-ring + reverse audio (6x), run-ahead, aspect Pixel/Fit/Stretch (RSZ), LCD
      filter, CPU clock, per-core settings, live Pico overlay menu w/ GPGX core options
- [ ] boot_b packer: add --genesis-blob at 0x01A00000; guard vs gpSP 0x01C00000

## Notes / geometry
- MD: 320x224 (also 256x224, 320x240, H32/H40, interlace 320x448). SMS: 256x192/256x224.
  GG: 160x144 (LCD crop of 256x224). SG-1000: 256x192. All RGB565 from GPGX with
  USE_16BPP_RENDERING. Sample rate ~44100 (libretro av_info; set via config).
- GPGX core options (libretro_core_options.h): region, aspect, overscan, filter, blargg NTSC,
  ym2612/psg volume, overclock, etc. -> exposed via set_option like snes9x.
