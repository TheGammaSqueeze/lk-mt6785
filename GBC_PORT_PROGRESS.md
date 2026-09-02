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
- [x] Build gambatte (emu/gbc) as a flat blob at a fixed VMA (0x4E800000) + loader.
- [x] Port ROM + .sav I/O to SD (gbc_sd_run.c: gba_sd_load_rom/load_sav/write_sav).
      Save STATES to SD still TODO (tie to the in-game menu below).
- [x] Dispatch at launch by rom type: GBA -> gpSP; GB/GBC -> gbc_sd_session().
      Both ROM-select sites branch; s_gpsp_dirty rebuilds the gpSP arena after a
      GB/GBC session (the session reuses the 0x50000000 arena). GBA flow untouched.
- [x] Runahead (preempt frames) + reset for GBC. Runahead uses gambatte state
      save/load (its state carries the APU, so no separate sound-ring save): each
      frame, present pf frames ahead with current input then rewind, honouring the
      SHARED ayaneo_get_preempt_frames() setting. Reset = SELECT+START+L+R held
      ~0.5 s (same hotkey as GBA) -> c->reset(). A full GBC in-game overlay menu +
      save states is still open (optional refinement; the hotkeys deliver the two
      options the user asked for).
- [x] GB (.gb) forced to DMG via gambatte FORCE_DMG; L/R cycle 5 DMG palettes.
- [x] Shared display (ayaneo_gbc_show_frame 6x) + audio (ayaneo_gbc_audio_*) reused.
- [ ] ON-DEVICE TEST with real GB/GBC ROMs (couldn't test headless): verify a game
      runs, audio, L/R palette, AYA exit, and GB/GBC<->GBA switching (arena rebuild).

## COMPLETE — all requested features implemented (pending on-device validation)
Every item the user asked for is built, host-checked where possible, and confirmed
not to regress the GBA/menu/cold-boot flow on device. GB/GBC GAMEPLAY itself could
not be validated headless (no screen access; no GB/GBC ROM confirmed on the SD).
The autonomous cron is being stopped: nothing safe/testable remains to implement,
and the rest needs the user to test on hardware.

### For the user to test on device (drop a .gb into /roms/gb and a .gbc into /roms/gbc)
1. Both appear in the carousel with the correct GB / GBC badge (bottom-right).
2. Launch a .gbc: runs in colour, audio OK, AYA returns to the selector.
3. Launch a .gb: runs in GB (mono) mode; L / R cycle the 5 palettes.
4. Run-ahead: set Preemptive Frames (via a GBA game's menu) and confirm GB/GBC
   input feels tighter; audio stays clean.
5. Reset: SELECT+START+L+R held ~0.5 s restarts the game.
6. Suspend/resume: exit with AYA, relaunch -> resumes; hold B at launch -> fresh.
7. Switching GB/GBC <-> GBA repeatedly stays stable (gpSP arena rebuild).
If any of these misbehave, the fix point is emu/gbc/gbc_sd_run.c (session) or the
dispatch in emu/gba/gba_driver.c.

### Optional future polish (not built, hotkeys already cover the options)
- A GBC in-game overlay menu (brightness/volume/save-state rows) like the GBA one.
- Per-console no-boxart placeholder art (today every no-art card shows the GBA
  cart placeholder regardless of console).

## Lessons for future ports
See **emu/CORE_PORTING_NOTES.md** for the full list of non-obvious pitfalls hit
during this port (per-console display geometry, emu-thread stack overflow surfacing
as an msdc_dma_transfer crash, arena-does-not-survive-a-menu-visit, reload-blob-each-
session, GBA boot-logo cart must use a GBA ROM header, menu clock fixed 1200 (not
2000, not 2100, not idle-aware toggling),
card-tile cache keying by console type, feature-gating untested extras, C++ blob
build specifics, and device/workflow gotchas like oem sd-probe wedging USB).

## Status log (newest first)
- 2026-09-02: Menu clock reverted to a FIXED 1200 MHz. The idle-aware boost
  (600 idle / 2000 active) introduced in ccfc752 made the idle-time hang WORSE, not
  better: churning the ARM-PLL per-frame at the fixed boot Vproc point is itself
  destabilising. Per user, dropped the max menu OPP to a stable sustained 1200 MHz
  (set once at setup, re-asserted at loop top only if drifted < 1100), removed the
  active_frames idle logic. Built + flashed lk_a (code-only, boot_b unchanged).
  See CORE_PORTING_NOTES.md item 6.
- 2026-09-02: GB/GBC launch punch-hole transition (like GBA). The selector already
  captures the frozen menu snapshot (0x54000000) + sets gba_punch_ready for every
  launch; the GBC session now consumes it: run a few frames for real content,
  ayaneo_gb_punch_prerender (new, 160x144x6 - the GBA prerender bakes 240x160x5),
  then grow the circle via the geometry-agnostic ayaneo_gba_punch_frame_pre. All
  three requested items (menu, run-ahead, forward transition) are DONE. REMAINING:
  the REVERSE (close) punch - gba_menu_arm_reverse is hardcoded to GBA 240x160 and
  the reverse is rendered in the selector, so a GBC reverse needs a 160x144 variant
  + selector changes; deferred as a follow-up. NEEDS USER TEST: the launch circle
  opens onto the GB/GBC game at correct aspect.
- 2026-09-02: GBC in-game Pico menu implemented (self-contained in gbc_sd_run.c,
  mirrors the GBA MI_* menu). AYA toggles it; game runs underneath with its input
  gated off (ayaneo_gbc_pad_mask returns 0 while g_gbc_menu_open). Items: Brightness,
  Volume, LCD Filter, Palette (DMG), Preemptive Frames, Save/Load State, Reset Game,
  Close (Close exits to the selector; Reset resets + closes the menu). Overlay drawn
  by ayaneo_gb_show_frame -> gbc_menu_paint (reuses ayaneo_fill/ayaneo_text). Run-ahead
  suppressed while the menu is open. Build + GBA/boot healthy. NEEDS USER TEST
  (headless): menu opens/looks right, nav works, Save/Load/Reset/Close behave. NEXT
  (cron): punch-hole launch transition for GB/GBC (like the GBA growing-circle punch).
- 2026-09-02: gambatte core moved to its own dedicated gbc_emu thread (256K, lazy +
  reused via kick/done events; emu_thread back to 64K) - consistency with gpSP +
  isolates the stack (fixes the switch-games overflow properly). Then enabled GBC
  run-ahead + suspend/resume: split GBC_ADVANCED into GBC_RUNAHEAD + GBC_SUSPEND
  (both 1), and set the emulation CPU clock by run-ahead tier (s_gbc_opp
  600/1000/1200/1400 like GBA) so (pf+1) emulations/frame fit. Build green, GBA +
  boot healthy. NEEDS USER TEST (headless-blind): run-ahead feel, suspend/resume,
  and that GBC audio/video stay clean. NEXT (cron): Pico in-game menu (AYA overlay,
  like GBA MI_* in gba_driver.c) + punch-hole launch transition.
- 2026-09-02: On-device GB/GBC bring-up. FIXED: GBA boot logo (logo cart now uses
  first GBA ROM header, not s_roms[0] which the merged roster may make GB/GBC);
  GBC display distortion (dedicated ayaneo_gb_show_frame 160x144x6 - was reusing the
  GBA 240x160x5 path); relaunch crash (reload blob each session + clean arena);
  switch-games + volume-relaunch crash (emu_thread stack 64K->256K: gambatte runs on
  emu_thread, not a separate cpu_thread like gpSP, and overflowed -> data abort in
  msdc_dma_transfer with corrupt sp); menu scroll perf (menu was stuck at 600 MHz
  because it asked for off-grid 2100; now 2000, carousel 28->15 ms, scrolling 60fps);
  card badge scroll perf (cache 3 tiles keyed by console type). Half-res gba_cart
  placeholder. Gated run-ahead + suspend states behind GBC_ADVANCED (off) for
  bring-up. Documented all in emu/CORE_PORTING_NOTES.md. NEXT: Pico in-game menu.
- 2026-09-02: Phase 2.4 (suspend/resume) done + committed (f1f0b45). All requested
  features now implemented (Phases 1, 2.1-2.4). Attempted an on-device GB/GBC
  smoke test but it needs a screen + a GB/GBC ROM on the card (headless-blocked);
  sd-probe re-inits the SD host and disrupts the running emulator so it is not safe
  to poke live. Stopping the autonomous cron: implementation complete, remaining
  work is user on-device validation. See the checklist above.
- 2026-09-02: Phase 2.3 (GBC runahead + reset). gbc_sd_run.c frame loop now does
  run-ahead: state_save -> run pf muted look-ahead frames -> present the future
  frame -> state_load rewind, using the shared ayaneo_get_preempt_frames() (0..3).
  gambatte's savestate includes the APU so committed audio (already submitted)
  stays continuous; look-ahead audio is just not submitted. Soft reset via
  SELECT+START+L+R held ~0.5 s (matches GBA) -> c->reset(); palette L/R is gated
  off during the combo. Build green, lk_a 2 MB, GBA flow + fast cold boot intact on
  device. Still open: optional GBC in-game overlay menu + save states, and the
  on-device GB/GBC gameplay test (headless-blocked).
- 2026-09-02: Phase 2.2 (launch dispatch + gambatte session). New gbc_sd_run.c:
  gbc_sd_session(vol, rom) loads the blob (cached), loads ROM+.sav from SD into the
  reused gpSP arena (0x50000000: rom@0, heap@8MB, vbuf/snd@48MB), gbc_create/load
  (FORCE_DMG for .gb), runs the frame loop (gbc_run -> ayaneo_gbc_show_frame +
  audio_submit, 13 MHz cumulative pacing), AYA = save .sav + exit, L/R cycle 5 DMG
  palettes (mono games). gba_driver.c: both ROM-select sites dispatch by type; a
  s_gpsp_dirty flag re-runs gba_core_init after a GB/GBC session (arena reuse) so
  the next GBA game is clean. rules.mk +gbc_sd_run.o. Build green, lk_a 2 MB, GBA
  flow + fast cold boot verified intact on device (dispatch only fires for GB/GBC
  ROMs, which may be absent). Could NOT test GB/GBC gameplay headless. TODO next:
  GBC in-game menu with runahead + reset (+ save states), then on-device GB/GBC test.
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
