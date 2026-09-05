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
- [x] genesis_core_loader.c (lk_a side): reads blob from boot_b 0x01A00000 -> VMA 0x4F800000,
      validates "SEG1"/version, zeroes BSS, DCCMVAU/ICIMVAU, calls genesis_core_blob_init(imports).
      imports = {ayaneo_genesis_pad_mask(port), gba_host_time}. Debug: g_gen_dbg_loaderr/hdr0/prc.
      NOT yet in rules.mk (references ayaneo_genesis_pad_mask from genesis_sd_run.c which is next;
      adding to lk_a before that file exists would break the link).
- [x] genesis_sd_run.c (own genesis_emu thread, mirrors gbc): ayaneo_genesis_pad_mask(port)
      GPIO->retro bits, genesis_session_body (blob load, heap_init 44MB @0x50000000, ROM from SD
      @0x52C00000, load by system hint, SRAM + suspend-resume, frame loop run/show/audio/input,
      AYA exit). + ayaneo_genesis_show_frame in mt_disp_drv.c (integer-scale CPU blit, RGB565,
      native geom, vsync present, HUD). + rules.mk entries. **lk_a BUILDS CLEAN** with all of it
      (not yet reachable: no dispatch / ROM folders / blob-in-boot_b). Parity extras (FF/rewind/
      run-ahead/menu/aspect-RSZ) deferred.
- [x] Menu integration: genesis blob built + packed into boot_b @0x01A00000 (build_ayaneo_gba_sd.sh
      + build_snes_boot_b.py, guards GBC-end <= 0x01A00000 and genesis-end <= gpSP 0x01C00000);
      ROM folder scan added (/roms/genesis .md/.gen/.bin/.smd, /roms/sms .sms, /roms/gg .gg,
      /roms/sg .sg) in sd_fat.c + mkdirs; launch dispatch at BOTH ROM-select sites in gba_driver.c
      (type >= GBA_CONSOLE_GENESIS -> genesis_sd_session, before the != GBA catch-all). **FULL
      BUILD CLEAN + FLASHED to device** (lk_a + boot_b). Boxart/badges = later (cosmetic).
- [x] On-device validation: **oem gen-probe PASSES on the device**: probe genesis @0x01A00000
      rc=20 m=0x31474553 ("SEG1"); probe load: ex=OK loaderr=0. The whole blob-in-boot_b + loader
      path (read from boot_b -> copy to VMA 0x4F800000 -> zero BSS -> genesis_core_blob_init ->
      valid exports) works on real hardware. lk_a+boot_b flashed.
- [ ] Gameplay validation: needs a REAL Genesis ROM in /roms/genesis on the SD card (user
      provides; cannot place autonomously). Core loads + host-test proves the emulation path.
      An oem gen-launch (headless N-frame run on a synthetic ROM) could be added like snes-launch
      for further no-ROM validation. <-- NEXT
- Parity extras (add incrementally, mirror snes_sd_run.c):
  - [x] Adaptive fast-forward + on-screen HUD: RT trigger -> run 2..10 extra frames/present,
        capped to what fits one vsync (committed-frame cost + g_dbg_blit_us reserve, 2x floor),
        audio of every frame submitted (speeds up), only the last presented; ayaneo_hud_set green
        badge. Builds clean. (Render-every-frame; set_av_skip optimization deferred.)
  - [x] Rewind delta-ring + reverse audio (6x): arms ayaneo_rewind_reset(state_size()) after load;
        captures c->state_save into the ring each committed + FF frame; LT-hold walks back
        (state_load + re-render + reverse+decimate-by-steps audio, GEN_REWIND_MAX_SPD 1536 = 6x),
        cyan HUD; on release ayaneo_rewind_end commits the head. Mirrors the snes rewind block but
        uses full state_save/load (GPGX has no raw fast path). Builds clean. Serialize measured
        2.8us/call on x86 (GPGX state is a compact fixed buffer, not a real 1MB copy), so per-frame
        capture is cheap; the ring delta-encode (~256K-word scan) is the dominant cost, well within
        the 16.6ms budget -> 60fps rewind is viable. host_test.c gained a serialize-throughput probe.
  - [x] CPU clock: session raises to 1400 MHz for gameplay (Genesis + per-frame rewind capture +
        60fps), restores the menu idle clock on exit (ayaneo_get/set_cpu_mhz).
  - [x] 6-button pad: MD sets RETRO_DEVICE_SUBCLASS(JOYPAD,1) = MDPAD_6B so device X/Y/L/R/Select
        reach Genesis X/Y/Z/Mode; 8-bit systems use the plain joypad. (blob change -> boot_b reflash.)
  - [x] Live Pico in-game menu (hardware OVL0 L0 overlay over the running game, mirrors the snes
        menu): AYA-tap toggles it (AYA-hold ~1.5s still force-exits), game runs underneath so
        settings preview live, pad mask + FF + rewind gated while open. Options: Brightness, Volume,
        CPU Clock (manual OPP grid), Save Slot (0..2), Save State, Load State, Reset Game, Exit Game.
        Nav = Up/Down move, Left/Right change, A select, B/AYA close, with press-edge + auto-repeat;
        left stick also navigates. Save/Load use /states/genesis/<rom>.st<slot>; Load re-arms the
        rewind ring. Overlay disabled on exit (CORE_PORTING_NOTES gotcha). Builds clean.
  - [ ] Remaining parity: run-ahead (state_save/load per frame + set_av_skip look-ahead), aspect
        Pixel/Fit/Stretch via ayaneo_rsz_present + LCD filter in ayaneo_genesis_show_frame (+ wire
        into the menu), GPGX core options in the menu (region/overclock/etc. via c->set_option),
        per-core settings PERSIST across reboot (mirror ayaneo_set_snes_opts), boxart/console badges
        in the carousel. <-- NEXT: aspect Fit/Stretch + LCD filter (then wire menu rows for them).

## Milestone reached (2026-09-05): Genesis core LOADS on the device
Blob builds (1.34MB) + host-test PASS + on-device gen-probe PASS. A Genesis/MD/SMS/GG/SG ROM
placed in the matching /roms/* folder will now launch through the carousel. Basic emulation
(display/input/audio/SRAM/suspend) is code-complete; parity extras are the remaining work.
      PLAN (mirror emu/snes9x/snes_sd_run.c, but BASIC first per CORE_PORTING_NOTES #8; gate
      parity extras behind a flag and add them in later firings):
      * ayaneo_genesis_pad_mask(unsigned port): LK pad GPIOs -> RETRO_DEVICE_ID_JOYPAD_* bits.
        MD 3-button: B=RETRO_B(0), C=RETRO_A(8), A=RETRO_Y(1), Start=RETRO_START(3), D-pad; add
        6-button (X/Y/Z=RETRO_L/R/..., Mode=RETRO_SELECT) later. Reuse gba_driver GPIO defines.
      * genesis_session_body(vol, rom): genesis_core_load() every session; heap_init a ~48MB arena
        in 0x50000000 (cart.rom mallocs up to 32MB via USE_DYNAMIC_ALLOC, so size >= 40MB); load
        ROM from SD by console type (system hint MD/SMS/GG/SG from rom->type); run frames; present
        via ayaneo_genesis_show_frame; submit audio via ayaneo_snes_audio_submit(buf,frames,44100)
        (reuse SNES 48kHz resampler path - both s16 stereo); poll input; save/resume via state
        exports + SRAM via sram_ptr/size. Own thread like gbc/snes (64KB emu_thread overflows).
      * ayaneo_genesis_show_frame(pix,w,h,pitch) in mt_disp_drv.c: native geom (MD 320x224/256x224,
        SMS/SG 256x192, GG 160x144-cropped), RGB565, integer/RSZ scale (mirror ayaneo_snes_show_frame;
        default Pixel CPU blit, Fit/Stretch via ayaneo_rsz_present). NOTE GPGX pitch is 720*2 bytes
        (fixed 720px stride) - use the passed pitch, blit only w x h.
      * rules.mk: add emu/genesis/genesis_core_loader.o + genesis_sd_run.o (-Os) to OBJS.
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
