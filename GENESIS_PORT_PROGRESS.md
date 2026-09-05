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
  - [x] Aspect Pixel/Fit/Stretch + LCD filter: ayaneo_genesis_show_frame now takes the hardware-RSZ
        path (ayaneo_rsz_present) for Fit (core aspect g_genesis_aspect_x1000, ~4:3) and Stretch
        (full panel); Pixel keeps the integer CPU blit. LCD filter (Off/Scanlines/Grid/Grid+) dims
        the last dest row/col per source pixel in the CPU blit and is passed to the RSZ. Session
        refreshes g_genesis_aspect_x1000 from c->aspect_x1000() (H32/H40 switch) and calls
        ayaneo_snes_rsz_restore() on exit (CORE_PORTING_NOTES: else Close hangs / carousel shears).
        New menu rows Aspect Ratio + LCD Filter (session-scoped globals; live preview). Builds clean.
## ON-DEVICE GAMEPLAY CONFIRMED (2026-09-05, user): Sonic 3 Complete (SONIC3C.GEN, pushed via
## fastboot stage + oem sd-put to /roms/genesis) RUNS on the Genesis core - display, input, audio,
## and ALL menu options work like snes. The emulation + menu + display path is validated end to end.

  - [x] Run-ahead: pf = ayaneo_get_preempt_frames() (Off/Balanced/Responsive/Max), gated off during
        FF/menu. After the committed frame: state_save to GEN_AHEAD_BUF, run pf look-ahead frames
        (set_av_skip render-except-last + audio-off), present the future frame, state_load back to
        committed. GPGX serialize is fast+deterministic (host-measured 2.8us) so no raw path needed;
        no heap_mark/reset (GPGX serialize is allocation-free, unlike snes9x). Session clock escalates
        by depth via s_gen_ra_opp {1400,1600,1800,2000}. New "Run-Ahead" menu row. Builds clean.
  - [x] FF performance: fast-forward now set_av_skip's the thrown-away frames (render only the last
        presented one), like snes - much cheaper FF frames.
  - [x] Menu enter/exit TRANSITIONS (commit 2429a9c): launch punch-hole (gameplay circle grows over the
        frozen carousel at 0x54000000) + exit reverse-punch (last frame shrinks back into the carousel),
        matching snes/gba/gbc. New ayaneo_genesis_punch_prerender (mt_disp_drv.c) renders the frozen frame
        with the LIVE aspect-mode geometry (Pixel/Fit/Stretch, honours g_genesis_aspect + _x1000);
        genesis_menu_arm_reverse (gba_snes_menu.c, g_reverse_is_gbc==3) packs the exit frame contiguous for
        the menu-thread reverse shrink. Launch punch armed generically by the menu (gba_punch_ready).
  - [x] PIXEL-ASPECT PERF ROOT CAUSE (commit 2429a9c): the Pixel path did priamry_display_wait_for_vsync()
        AFTER ayaneo_present() - but primary_display_config_input already blocks on DISP_PATH_EVENT_FRAME_DONE
        in DSI video mode (primary_display.c:1081), so that was a SECOND wait = two vsync periods = a hard
        30fps cap. Fit/Stretch (RSZ, wait_vsync=0) + GBC (no explicit wait) ran at 60. Removed the redundant
        wait; also narrowed the per-frame cache clean from the whole FB to just the game rows (mirror GBC).
        The earlier memcpy row-replicate blit change (commit 459a439) is a valid micro-opt but was NOT the
        cause. NOTE: the snes9x + snes paths (mt_disp_drv.c ~1935/1944) may carry the SAME double-wait - the
        perf audit workflow is checking; fix there too if confirmed.
  - [~] EXTREME PERF PASS + HOTKEY CONSISTENCY (user requested, IN PROGRESS). Audit workflow wf_3dae1c0c-e2f
        DONE; prioritized plan being implemented:
        PERF: [x] Genesis clock 1400->1800 @ pf=0 (s_gen_ra_opp={1800,1800,2000,2000}) - biggest lever.
              [ ] GBA/GBC Pixel blit -> memcpy row-replicate (mt_disp_drv.c ~1204, mirror Genesis:2017).
              [ ] SNES Pixel/Fit full-panel clean -> banded (mt_disp_drv.c:1935 -> yoff/dh like Genesis:2047).
              [x] Rewind s_prev refresh: scalar copy -> pointer swap (ayaneo_rewind.c commit + skip paths) -
                  removes ~2MB/frame (Genesis) copy, ALL cores, behavior-preserving. begin/end keep copies.
              [ ] (deferred) Rewind capture stride N=2-3: behavior-changing (rewind granularity); hold.
              [x] FAST_SAVESTATES bit4: NOT APPLICABLE - verified this vendored GPGX uses fast_savestates ONLY
                  to gate save/restore_sound_buffer in libretro.c; it does NOT skip a system_reset memset (the
                  runahead agent assumed upstream behavior). Enabling it would ADD sound-buffer copies. SKIPPED.
              [~] hq_fm/hq_psg: KEEP ON (audio quality); blip_buf/psg -O2 deferred (small win, needs boot_b reflash).
              [ ] (opt) run-ahead reuse rewind s_prev instead of 2nd state_save: medium/medium, deferred.
        HOTKEYS (canonical = GBA scheme; renderer kind 0=none/1=vol/2=bright):
              [x] SNES brightness OSD kind 0->2 (snes_sd_run.c:313) - bar never showed.
              [x] H1 Genesis genesis_poll_volume() (mtk_detect_key 0x11/0x00, SELECT=bright) @ loop top + deferred persist.
              [x] H2 GBC-SD gbc_sd_poll_volume() added (SD GB/GBC path had NO hardware rocker before).
              [x] H4 Genesis menu bright/vol deferred-persist (genesis_settings_touch/tick/flush).
              [x] H5 power key in-game -> save+suspend+flush+mt_power_off now in Genesis, GBC-SD, SNES (was GBA-only).
              [x] H6 Genesis soft-reset SELECT+START+L+R.
        GENESIS MENU (user requests): [x] Region (Auto/USA/Europe/Japan, live via region_detect);
              [x] Refresh Rate readout (ayaneo_dsi_refresh_milli from live vfp; groundwork for LCM refresh switch).
  - [x] Genesis per-core settings PERSIST across reboot: aspect/filter/region/slot packed into the shared
        settings blob b+24 free bits (10-17) via ayaneo_get/set_gen_aspect/filter/region/slot (ayaneo_audio.c,
        all 4 blob sites: load/serialize/deserialize/save); run-ahead persists via the shared preempt-frames
        (b+48). genesis_sd_run restores them at session start (region pushed via set_option) and touches the
        deferred persist on every menu change. No blob version bump needed (old blobs read 0 = correct defaults).
  - [ ] Also remaining: GPGX overclock core option in the menu (region done); boxart/console badges in
        the carousel (SMS/GG/SG/MD logos like the GB/GBC/GBA/SNES badges).

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
