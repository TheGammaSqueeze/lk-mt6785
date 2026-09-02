# Emulator core porting notes (LK / MT6785 / k85v1_64)

Hard-won gotchas from porting the gambatte GB/GBC core into the GBA-from-SD flow
(branch `lk-gba-emu-sd-card`). Read this before adding a new emulator core or a new
console type. Each item cost real on-device debugging; they are easy to re-hit.

## Architecture at a glance
- Two loadable cores live as flat blobs in `boot_b`, loaded into fixed DRAM VMAs by
  small lk_a-side loaders, and driven through an exports/imports ABI struct:
  - gpSP (GBA): blob VMA `0x4E400000`, boot_b off `0x01C00000`, ABI `gba_core_abi.h`.
  - gambatte (GB/GBC): blob VMA `0x4E800000`, boot_b off `0x01900000`, ABI
    `gbc_core_abi.h`.
- Only ONE core runs at a time. Both reuse the same emulator DRAM arena at
  `0x50000000` (`GBA_ARENA_PA`, 64 MB, top 2 MB reserved via `GBA_DRV_RESERVE`).
- Launch dispatch is in `emu/gba/gba_driver.c` at BOTH ROM-select sites (initial +
  in-game "Close"): branch on `s_roms[sel].type` (`GBA_CONSOLE_GB/_GBC/_GBA`). GBA ->
  gpSP inline loop; GB/GBC -> `gbc_sd_session()` (emu/gbc/gbc_sd_run.c).

## 1. Per-console DISPLAY geometry - do NOT reuse ayaneo_gbc_show_frame
`ayaneo_gbc_show_frame` (mt_disp_drv.c) is compiled with `GBC_SRC_W/H/SCALE` that are
**240x160x5 when AYANEO_GBA is defined** (the GBA game's geometry) and only 160x144x6
otherwise. In the GBA-SD build AYANEO_GBA IS defined, so calling it for a 160x144 GB
frame stretches it to GBA aspect (the "distorted screen" bug). A new core needs its
OWN show function with its native dimensions. GB/GBC uses `ayaneo_gb_show_frame`
(160x144 integer 6x -> 960x864 centred; no GBA colour-correction LUT).

## 2. Every emulation core runs on its OWN thread (never on emu_thread)
`emu_thread` only orchestrates (menu + SD + display) and stays at 64 KB. Each core
emulates on a dedicated thread with its own large stack:
- gpSP: `gba_cpu` thread (producer/consumer with emu_thread via `ev_cpu`/`ev_main`).
- gambatte: `gbc_emu` thread (created lazily on first GB/GBC launch, 256 KB, REUSED via
  `s_gbc_kick`/`s_gbc_done` events; this LK does not reap exited threads, so a
  per-session thread would leak its stack). gambatte is synchronous (`gbc_run` = one
  whole frame), so it is a single session thread, NOT gpSP's two-thread split.
  emu_thread signals the kick and blocks on done, so exactly one core runs at a time.
HISTORY / symptom to remember: gambatte first ran INLINE on emu_thread; its deep C++
overflowed the 64 KB stack and the fault surfaced far away as a data abort in
`msdc_dma_transfer`'s epilogue (`pc` in lk_a, `sp = ~0xffffffxx`, `dfar=0xfffffff8`) on
the next SD DMA when switching games. A crash in unrelated code with a wild `sp` ==
stack overflow, not that code's bug. Bumping emu_thread to 256 KB "fixed" it but was a
workaround; the correct design is a dedicated per-core thread (above).

## 3. Reuse the arena, but assume it does NOT survive a menu visit
The GB/GBC session lays out the arena as ROM (8 MB) | gambatte heap (24 MB) | video |
audio | savestate, all inside `[0x50000000, 0x53E00000)` with margin. Rules:
- The heap must NOT abut the video buffer (the old 40 MB heap ended exactly at vbuf).
- The arena overlaps the menu's DRAM (decompressed pack `0x50000000`, home/bg pools,
  wallpaper/chrome caches up to `0x55E00000`). That is OK ONLY because the menu
  rebuilds everything on re-entry (`load_pack` + `snes_menu_prewarm`). A core must
  reload its own ROM/state each session and never assume its arena persisted.

## 4. Reload the core blob every session
`gbc_core_load()` is called at the start of EVERY `gbc_sd_session` (not cached). This
re-reads the blob from boot_b, re-validates the header, and re-runs `blob_init`
(resetting the imports table). Caching the exports pointer across sessions risked a
stale/clobbered core surviving into a relaunch. ~20 ms, negligible.

## 5. GBA BIOS boot-logo cart must use a GBA ROM's header
`gba_sd_build_logo_cart` copies the Nintendo-logo bytes (cart header 0x04-0x9F) into a
stub cart so the GBA BIOS intro shows the authentic logo. The roster now MERGES
/roms/gb, /roms/gbc, /roms/gba sorted, so `s_roms[0]` can be a GB/GBC ROM whose header
has no GBA logo -> "BIOS thinks no cartridge" (only the Game Boy text, no Nintendo
logo). Always pick the first ROM of type `GBA_CONSOLE_GBA` for the header. gambatte
itself needs NO GBA BIOS; the intro is just the device's boot animation.

## 6. Menu CPU clock: fixed 1200 MHz, NEVER 2000 MHz and NEVER toggle per-frame
The menu is a software NEON blitter and is clock-bound. Three traps, learned in order:
- Requesting **2100 MHz** is OFF the OPP grid (`s_cpu_opp` maxes at 2000); with the
  emulation clock's POSDIV it drives the ARM-PLL VCO out of range so the PLL never
  leaves ~600 MHz. The whole menu ran at 600 MHz (carousel ~28 ms, janky scroll).
- HOLDING 2000 MHz continuously is worse: `ayaneo_set_cpu_mhz` only reprograms the
  PLL, NOT the core voltage (LK has no DVFS table), so 2000 MHz runs at the fixed boot
  Vproc point. Sustained 2000 MHz on an IDLE menu destabilised the core after a few
  minutes -> silent hang, NO fault message (a marginal CPU cannot run the fault
  handler).
- The idle-aware fix (boost to 2000 while navigating, drop to 600 when idle) was ALSO
  unstable: toggling the PLL per-frame at a fixed voltage point stressed the core more,
  not less. Reverted.
So: the menu now holds a **fixed 1200 MHz** (`s_cpu_opp`'s "Responsive gameplay" tier),
set once at setup and re-asserted at the loop top only if the clock has drifted below
1100 (`if (ayaneo_get_cpu_mhz() < 1100) ayaneo_set_cpu_mhz(1200)`), then restored to the
saved emulation clock on launch. 1200 MHz is a stable sustained point at the boot Vproc
and still gives a smooth carousel. `ayaneo_get/set_cpu_mhz` read/write ARMPLL_CON1 @
0x1000C204. Symptom to remember: an idle-time crash with NO debug message == a
clock/voltage stability problem (too high a sustained clock, or churning the PLL), not a
software bug.

## 6b. Do NOT ayaneo_display_prepare() before a launch punch (black-flash)
The SNES menu presents its frozen launch snapshot on `FB_LAYER` via
`ayaneo_canvas_present` - the SAME layer + double-buffer the game uses (there is no
separate always-on menu overlay for the SNES menu; `BOOT_MENU_LAYER` is the boot
animation). So at launch the live on-screen image IS one of the two game fb buffers.
`ayaneo_display_prepare()` memsets BOTH fb buffers to black, including the one the OVL
is currently scanning out -> the frozen menu instantly goes black and stays black for
the ~20 silent emulation frames run before the punch loop paints = a menu->game flicker.
The GBA path never calls display_prepare at launch (only once at init, before the menu),
so it never flickered. Fix: when `gba_punch_ready`, SKIP display_prepare; let the punch
composite take over the display and `ayaneo_gbc_clear_letterbox` black only the borders.
Call the black-fill prepare only as the no-snapshot fallback.

## 7. Card-tile cache must key by whatever makes cards differ
The carousel caches pre-rendered card tiles. Without per-ROM box art it used ONE shared
tile ("all GBA carts identical"). Adding per-card CONSOLE BADGES broke that assumption;
the naive fix (per-game tiles) tanked scroll. Correct: cards without box art only
differ by CONSOLE TYPE, so cache 3 tiles keyed by type (ctile slots 0..2; `fct` rebuilds
only on a type boundary). Lesson: any per-card content added to `draw_card` must be
reflected in the tile-cache key, or you either get stale tiles or a per-frame re-render.

## 8. Feature-gate untested extras; bring up BASIC emulation first
Run-ahead (per-frame state save/load) and suspend/resume save states were untested for
gambatte and confounded the display/crash bring-up. They are gated behind
`GBC_ADVANCED` (0) in gbc_sd_run.c. Get plain display + relaunch + switch solid, THEN
flip it on. Also clamp SD transfer sizes from the core (GB .sav clamped to 128 KB) so a
bad length can never drive a runaway DMA.

## 9. Blob build specifics (C++ core)
- Link `libm.a` for `powf` (gambatte colour correction); newlib `powf` pulls `__errno`
  -> provide a stub in the bundled libc.
- `__aeabi_*` from libgcc; `mem*/strcmp` bundled; C++ runtime (operator new, `__cxa_*`)
  in `gbc_shim.cpp` (bump allocator over the arena, delete is a no-op).
- Flat link at a fixed VMA with a 20-byte header (magic/version/entry/load/span); the
  loader zeroes BSS and does DCCMVAU/ICIMVAU cache maintenance before entry.

## 10. Device / workflow gotchas
- NEVER run `fastboot oem sd-probe` on the live device: it re-inits the microSD host
  and can wedge the fastboot USB endpoint (device still lists but every command says
  "no link"); recovery needs a PHYSICAL power cycle.
- Flash dance: `reboot-bootloader`, `sleep 4`, then `flash`; retry on "flash write
  failure". Always `-s 0123456789ABCDEF`. Stage images to `/mnt/c/pairmini`.
- Measure menu perf via `fastboot oem diag`: `render_us/peak_us/fps` + the per-phase
  `perf_us: wp/chrome/carousel/filmstrip/rest` breakdown (profiler enabled via
  `g_perf_tick`). `bt:` line = cold-start stage timings.
- Cold-boot microSD mount skips a 250 ms VDD-collapse delay when the rail is already
  off (msdc_ext_sd_power_on) - do not "fix" that back to an unconditional delay.

## 11. Palette catalogue + colour knobs live in the CORE, exposed by index
The GB-colorization palettes (the real gambatte GB/GBC/SGB/Special + TWB64 list, ~600)
come from `libgambatte/libretro/gbcpalettes.h`, compiled INTO the blob (gbc_core_exports.cpp)
and exposed over the ABI as `dmg_palette_count/name/apply/default` (browse + install by
index). The frontend keeps NO palette data; it just cycles the index (L/R shoulders or the
Pico "Palette" row) and shows `dmg_palette_name`. Default is "GBC - Dark Green" (the CGB
BIOS default). Palettes are PACK15 (15-bit 0bBGR) x 12 = 3 DMG palettes x 4 shades; the
core converts to 0x00RRGGBB and drives the existing `set_dmg_palette_color`. Only DMG (.gb)
games use these; GBC/SGB carts colour themselves. GOTCHA: gbcpalettes.h `#include`s
libretro-common `array/rhmap.h` (a title->palette hash we do NOT use), which drags in
`retro_common_api.h` and `#error`s under `-ffreestanding` ("inttypes.h is being screwy").
Short-circuit it: before the include, `#define __LIBRETRO_SDK_ARRAY_RHMAP_H__`, `NULL`, and
stub `RHMAP_SET_STR/GET_STR/FREE` so the (unused, compiler-dropped) map helpers still parse.
Index 0 is a frontend-synthesised "Auto (Detect)" slot (the DEFAULT): it calls the core's
`dmg_palette_apply_auto(title)`, which matches the ROM header title (0x134, up to 16 bytes,
uppercase ASCII, ctrl-terminated - the frontend extracts it from its ROM buffer) against
gambatte's per-game `gbcTitlePalettes` and installs that palette, falling back to
"GBC - Dark Green" when the game is not listed. This is gambatte's "internal" colorization.
The frontend maps its indices 1..N onto the core dir palettes 0..N-1, so `dmg_pal_count`
returns core count + 1 and `dmg_pal_name(0)` is "Auto (Detect)". X on the Palette row resets
to Auto.

The CGB colour knobs (`set_color_correction`, `_mode`, `set_dark_filter`) were already in the
ABI but unexposed; they are now Pico rows (Color Correction on/off, Correction Mode
Accurate/Fast, Dark Filter 0..100%), applied at session start via `apply_color_knobs` and
remembered in module statics for the LK boot (not yet in the SD-persisted settings block).

The Pico menu also carries a "CPU Clock" row (manual OPP grid 600..2000, mirrors the GBA
driver's `s_cpu_opp`/`cpu_step`; ayaneo_set_cpu_mhz reprograms the PLL only, so >boot-Vproc
is the user's call) and, on the Palette row: X resets to the default GBC palette, and A
opens a full-screen LIST PICKER. The picker polls Up/Down every frame (not edge) with
accelerating auto-repeat (pause < 16 frames held, then interval 6/3/2/1 and step 1/2/5/12
as the hold grows) so all ~600 palettes are reachable quickly while keeping fine control;
each step applies live on the running game behind the panel; A confirms, B restores the
pre-open selection. Picker state is a module static, so it is reset at session start and
cleared when the menu closes (the gbc thread is reused across sessions).

NOTE: the gambatte core blob lives in boot_b (off 0x01900000), so ANY core change
(new exports, palette data) requires re-flashing BOTH lk_a AND boot_b. The build's "boot_b
assets UNCHANGED" note only tracks the SNES menu pack, not the core blobs - ignore it when
you touched a core.

## Still open (as of this doc)
- Port the Pico in-game overlay menu + its settings from `lk-gbc-emu` into the GBC
  session (AYA currently just exits). Then re-enable `GBC_ADVANCED`.
</content>
