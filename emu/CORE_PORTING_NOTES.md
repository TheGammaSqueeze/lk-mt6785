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

## 2. Emulation THREAD STACK - a core that runs on emu_thread needs a big stack
gpSP runs its CPU on a SEPARATE `gba_cpu` thread (64 KB). gambatte instead runs its
frame loop DIRECTLY on `emu_thread`, on top of the menu + SD/FAT orchestration frames.
64 KB overflowed and corrupted the stack; the fault surfaced far away as a data abort
in `msdc_dma_transfer`'s epilogue (`pc` in lk_a, `sp = ~0xffffffxx`, `dfar=0xfffffff8`)
on the next SD DMA when switching games. Fix: `emu_thread` stack is now **256 KB**. If
you add another core that emulates on emu_thread, keep it there or give it its own
thread. Symptom to remember: a crash in unrelated code with a wild `sp` == stack
overflow, not that code's bug.

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

## 6. Menu CPU clock: use 2000, never 2100
The menu is a software NEON blitter and is clock-bound. Requesting **2100 MHz** is OFF
the OPP grid (`s_cpu_opp` maxes at 2000); with the emulation clock's POSDIV it drives
the ARM-PLL VCO out of range so the PLL never leaves ~600 MHz. The whole menu ran at
600 MHz (carousel ~28 ms, janky scroll). Set **2000** (on-grid) and re-assert it once
per menu frame if it has dropped below 1900 (a game-close restores the 600 MHz
emulation clock). `ayaneo_get/set_cpu_mhz` read/write ARMPLL_CON1 @ 0x1000C204.

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

## Still open (as of this doc)
- Port the Pico in-game overlay menu + its settings from `lk-gbc-emu` into the GBC
  session (AYA currently just exits). Then re-enable `GBC_ADVANCED`.
</content>
