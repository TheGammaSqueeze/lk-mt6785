# GB/GBC port into the GBA-from-SD flow — autonomous work log

Branch: `lk-gba-emu-sd-card`. Build: `./build_ayaneo_gba_sd.sh`. Stage lk_a to
`/mnt/c/pairmini`. Flash device `0123456789ABCDEF` (ALWAYS `-s 0123456789ABCDEF`).
Push remote `ayaneo`. Never regress the working game/menu/boxart or the
never-brick / boot-to-OS / SELECT-return flow.

## Goal (from the user)
1. Scan `roms/gb`, `roms/gbc`, `roms/gba` on the SD; merge into one games list.
2. Create the folder structure on the SD if missing.
3. On each carousel card, show a console-type badge (GB / GBC / GBA logo) in the
   bottom-right corner, mirroring the SNES web menu's controller icon. Logos in
   `/work/logos`.
4. Port the gambatte GB/GBC core into the SD flow as a SECOND boot_b blob (like
   gpSP). Dispatch by ROM type at launch (GBA -> gpSP blob, GB/GBC -> gambatte
   blob).
5. GBC core gets the SAME runahead (preempt frames) + reset options as GBA.
6. GB (non-color) games run in GB mode. GB palettes cycle via L / R buttons.

## Decisions
- Core integration: SECOND boot_b blob (user chose this over linking into lk_a).
- Phasing: Phase 1 (menu/scan/icons/folders) first, then Phase 2 (core).

## Phase 1 — menu / SD (no core risk)
- [x] sd_fat.h: add console-type enum + `unsigned char type` to gba_rom_entry.
- [x] sd_fat.c: scan /roms/gb (.gb), /roms/gbc (.gbc), /roms/gba (.gba); tag type;
      merge + case-insensitive sort. Generic ends_ext + scan_folder helper.
- [x] Folder creation: fat_wr_mkpath /roms/gb, /roms/gbc, /roms/gba after
      assets_ok in ayaneo_gba_sd_boot (idempotent).
- [x] Icons: rasterize /work/logos to small PNGs (gb, gbc, gba). Add to pack via
      pack_snes.py (new --logo-gb/gbc/gba args -> res_map keys logo_gb/gbc/gba).
- [x] Thread per-game console type: gba_rom_entry.type -> gba_snes_menu.c ->
      snes_menu_set_gba_roster -> snes_menu.c draw_card corner badge.
- [x] draw_card: blit the console logo bottom-right (mirror the player-count icon
      at m->card_pi_wx/wy). Host-validate, build, flash.
- [x] Launch: GB/GBC stubbed (Phase 2) until Phase 2 (GBA path unchanged).

## Phase 2 — gambatte GB/GBC core as a boot_b blob
- [ ] Build gambatte (emu/gbc) as a flat blob at a fixed VMA (reuse the gpSP blob
      ABI pattern: exports/imports struct, blob_libc, .ld, build script). C++ so
      keep gbc_shim.cpp runtime. Load from boot_b via a gbc_core_loader.
- [ ] Port gbc_driver ROM/save/state I/O from boot_b to the SD card (roms/gb[c],
      saves, states) — reuse gba_sd_load_rom / gba_sd_write_sav / _state.
- [ ] Dispatch at launch by rom type: GBA -> existing gpSP; GB/GBC -> gambatte.
- [ ] Runahead (preempt frames) + reset options wired for the GBC core, matching
      the GBA core's Pico menu options.
- [ ] GB (non-color) ROMs forced to GB mode (not CGB); L/R cycle GB palettes.
- [ ] Shared display (ayaneo_gbc_show_frame 6x), audio, input already exist.

## Status log (newest first)
- 2026-09-02: Phase 2 milestone 1 (GBC blob builds + packs + loader in lk_a).
  Created emu/gbc/: gbc_core_abi.h (imports read_buttons+host_time; exports the
  gbc_* API incl load(flags) for FORCE_DMG), gbc_core_exports.cpp (blob entry +
  forwarders: gbc_read_buttons/time/atexit), gbc_core_blob.ld (VMA 0x4E800000,
  magic "GBC1"), gbc_blob_libc.c (mem*/strcmp/__errno; powf from libm, __aeabi from
  libgcc), build_core_blob.sh -> core_gbc.blob (116 KiB, span 0x1d500).
  gbc_core_loader.c (lk_a side, boot_b off 0x01900000). gbc_wrap.cpp: gbc_load now
  takes flags. gba_driver.c: ayaneo_gbc_pad_mask() maps the pad to gambatte bits.
  Wired: rules.mk (+gbc_core_loader.o under AYANEO_GBA_SD), build_snes_boot_b.py
  (gbc blob @25MB, between pack and gpSP blob), build_ayaneo_gba_sd.sh (builds gbc
  core+blob). Full build green, lk_a still 2MB, boot_b has the gbc blob. Loader is
  dead code until dispatch (next). GBA flow unaffected. NEXT: wire launch dispatch
  in emu_thread (GBA rom -> gpSP; GB/GBC rom -> gbc_core_load + gambatte frame
  loop), then saves/states to SD, runahead+reset, GB FORCE_DMG + L/R palettes.
- 2026-09-02: Badge rendering VALIDATED on host (GBA_BADGES=1 in host_render.c,
  cycles GB/GBC/GBA; GBA_BADGE_FORCE to test one). Fixed a real bug: the ctile
  shared-single-tile fast path (gba_mode && !gba_boxart) and fct_get assumed all
  cards identical, so badges leaked across cards. Now gated to also require
  !gba_types (per-card badges force per-gi tiles). Badges tuned smaller (72x22
  card units) with a dark backing plate, bottom-right corner. Host render confirms
  GB->GBC->GBA correct + distinct. NOTE for user review: the no-boxart placeholder
  cart art still says "GBA GAME BOY ADVANCE" for every console (cosmetic; consider
  per-console placeholders later). Phase 1 complete pending on-device visual check.
- 2026-09-02: Phase 1 code complete. sd_fat: type enum + field, 3-folder scan
  (scan_rom_folder/ends_ext), gba_sd_make_rom_dirs (fat_wr_mkpath) wired into the
  boot gate. Logos rasterized+vendored to tools/ayaneo/snes/logos/{gb,gbc,gba}.png;
  pack_snes.py --logo-gb/gbc/gba -> res keys logo_gb/gbc/gba (packed idx 75/76/77);
  build script passes them. snes_menu: gba_types + console_logo[3] + setter +
  bottom-right badge draw in draw_card; gba_snes_menu builds s_types[] and calls
  the setter. Build OK, signed 2MB, logos packed. Host validator segfaults are
  PRE-EXISTING flakiness (crash even with my changes stashed), non-fatal. Next:
  flash lk_a+boot_b, screenshot to confirm badge renders on device.
- 2026-09-02: plan written; starting Phase 1 (sd_fat data model + scan + folders).
</content>
