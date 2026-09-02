# snes9x (SNES) core port to LK - progress

Porting the libretro **snes9x** core (https://github.com/libretro/snes9x) into the
GBA-from-SD flow as a THIRD loadable boot_b blob, alongside gpSP (GBA) and gambatte
(GB/GBC). Branch `lk-gba-emu-sd-card`. Read `emu/CORE_PORTING_NOTES.md` first - the
gambatte port established the whole pattern (blob at a fixed VMA, exports/imports ABI,
bundled libc + shim, boot_b packing, per-console display/dispatch/threading).

## Status: CORE VALIDATED (host) + PHASE 2 WIRED/FLASHED (video/input/audio/badge)

CORE PROVEN (2026-09-02, headless on host): `emu/snes9x/build_host_test.sh` builds the
SAME core sources natively and `emu/snes9x/host_test.cpp` loads a ROM + runs frames.
Super Mario World AND Street Fighter II Turbo both PASS: correct mapper (LoROM), av_info
256x224 / max 1024x478 / fps 60.0988 / sr 32040, video_cb fires every frame with pitch
2048 (= 1024px stride -> the display already reads pitch/2), and content CHANGES across
frames (real gameplay, not a stuck APU handshake). This confirms my LK integration
constants (60.0988 pacing, 32040 audio, 1024 stride) and de-risks the port: the only
unproven part left is the LK wiring (blob load / present / GPIO / AFE ring), which mirrors
the proven gambatte path. Reusable as a regression tool for future core updates.

Full lk_a integration builds end-to-end and is flashed (lk_a + boot_b). Awaiting on-device
validation (headless here - need a .sfc/.smc in /roms/snes to confirm a game renders).
- `snes_core_loader.c`: loads core_snes.blob from boot_b @0x01E00000 -> VMA 0x4F000000.
- `snes_sd_run.c`: session runner on its OWN `snes_emu` thread (262 KB); loads ROM into the
  0x50000000 arena (8 MB ROM buf + 48 MB heap), inits core, per-frame run + display, ~60 Hz
  pacing, AYA-hold exits, SRAM (.srm) load/save under /saves/snes. `ayaneo_snes_pad_mask`
  maps the pad to RETRO joypad bit order.
- `ayaneo_snes_show_frame` (mt_disp_drv.c): RGB565 256/512-wide -> 1024-wide integer scale
  (256->4x, 512->2x), centred on the 1280x960 panel, double-buffered.
- Dispatch: `GBA_CONSOLE_SNES` -> `snes_sd_session` at both ROM-select sites in gba_driver.c.
- `/roms/snes` scan (.sfc + .smc) and auto-create in sd_fat.c.
- boot_b packer: 3rd blob @0x01E00000 (33 MB partition, overrun-checked). Build script builds
  the snes blob. boot_b now 31.8 MB, lk_a still fits 2 MB.

PACING / PER-CORE LCM REFRESH: the panel (st7703_hd720) runs at a FIXED rate set at boot
(vfp 23 -> vtotal 999 -> 59.749 Hz, tuned to ~= the 59.7275 GB/GBA rate). LK now switches
the panel refresh PER CORE via the DSI vertical-front-porch (MTK dynamic-fps): new
`ayaneo_dsi_set_vfp` (ddp_dsi.c) writes DSI_VFP_NL live. SNES sets vfp 17 -> vtotal 993 ->
~60.11 Hz (0.02% off its native 60.0988), then a VSYNC-LOCKED present in
ayaneo_snes_show_frame (priamry_display_wait_for_vsync after present) paces emulation to the
real scan-out - smooth + tear-free, replacing the 13 MHz busy-wait. On exit vfp is restored
to 23 (59.749 Hz) for the menu / other cores. GB/GBC/GBA/menu/cold-boot paths are untouched
(they never call the vfp switch and don't use the SNES show_frame). Audio resampler keys off
the core's av_info rate; clock recovery absorbs the ~0.02% residual. NEEDS on-device check
(headless): panel switches cleanly, no blank/tear, smooth scroll in-game.

AUDIO: done (first cut). snes9x outputs stereo s16 at its native ~32040 Hz; new
`ayaneo_snes_audio_submit` (ayaneo_audio.c) linear-interp UPSAMPLES that to the 48 kHz AFE
ring (the GBA path box-DECIMATES 65536->48000; SNES needs the other direction), src rate
from av_info. Video stays 60.0988 Hz-mastered (per user), so audio uses an emergency
write-cursor snap near under/overrun rather than audio-mastered pacing; a fine-trim clock
recovery like the GBA path can be added later if the periodic snap is audible. Wired in
snes_sd_run.c (ayaneo_gbc_audio_init + reset at session start, submit each frame). lk_a-only.

MENU BADGE: done. SNES cards now show a console badge bottom-right like GB/GBC/GBA.
console_logo[3]->[4], set_console_badges takes a 4th (snes) logo, draw gate ty<3 -> ty<4;
the ctile cache is type-keyed with cap 12 so a 4th type has ample room (no scroll regression
- resolves item 7). pack_snes.py --logo-snes -> resource "logo_snes"; build passes it;
host_render cycles 4 types. Asset tools/ayaneo/snes/logos/snes.png is a CLEAN PLACEHOLDER
("SNES" wordmark, slate-purple + white stroke, 198x70 to match the others) - swap for an
official logo when available. Changed the SNES pack -> boot_b re-flashed.

REMAINING: in-game menu/run-ahead/transition (mirror gbc), perf pass, hi-res/interlace +
overscan polish, audio fine-trim if needed. STILL UNVALIDATED on HW (headless) - need a
.sfc/.smc in /roms/snes to confirm video+input+audio (and eyeball the badge).

### (earlier) BLOB LINKS milestone

`emu/snes9x/build_core.sh` -> `libsnes9x.a`; `emu/snes9x/build_core_blob.sh` ->
`core_snes.blob` (1.83 MB flat, ~3.3 MB DRAM span incl BSS, header magic "SNS1",
VMA 0x4F000000). All blob-side files written: `snes_core_abi.h`, `snes_core_exports.cpp`
(drives the libretro retro_* API + time/localtime via the LK host clock), `snes_shim.cpp`
(bump alloc + malloc/free/calloc + init_array runner + LoadZip stub), `snes_blob_libc.c`
(mem/str/ctype/number + compact vsnprintf), `snes_blob_stubs.c` (all file/zip/VFS I/O +
libstdc++ locale/wide-char stubs), `snes_core_blob.ld` (keeps .init_array, empty exidx).
libstdc++.a/libm.a/libgcc.a linked; std::__throw_* come from libstdc++ (not redefined).

NEXT: lk_a integration (Phase 2 below) - none of this runs on device yet.

### Done
- Vendored upstream `emu/snes9x/` (shallow clone, nested .git + non-libretro frontends
  removed; ~4.3 MB). Self-contained per the tree rule.
- **Feasibility PROVEN**: the ENTIRE core (all 36 core sources + `libretro/libretro.cpp`)
  compiles clean with the exact gambatte freestanding flags
  (`-march=armv7-a -mfloat-abi=soft -ffreestanding -fno-exceptions -fno-rtti -Os
  -D__LIBRETRO__ ...`). Zero source changes needed to compile.
- `emu/snes9x/build_core.sh` builds `libsnes9x.a` (~2.2 MB archive, 36 objects) and
  reports the external link surface.

### External symbol surface (what the blob must provide) - fully characterized
- **libc** (bundle in `snes_blob_libc.c`, extend the gambatte one): mem*/str*, snprintf/
  sprintf/sscanf/printf/fprintf, atoi/strtol/strtoul, abs/isalnum/toupper, rand/srand,
  calloc, exit/perror/__assert_func, time/localtime, strcasecmp/strncasecmp.
- **libgcc.a**: all `__aeabi_*` (32-bit int/float/double div + conversions).
- **libm.a**: sin/cos/pow/exp/ceil.
- **shim** (`snes_shim.cpp`): operator new/delete (bump allocator over the arena),
  malloc/free/calloc, `__throw_*` -> abort stubs, `_impure_ptr` stub.
- **file I/O stubs**: rf*/filestream_*/zip_*/LoadZip/vfs_hybrid_init -> return failure.
  The ROM is fed as a BUFFER via `retro_load_game` (no file I/O for the ROM), exactly
  like gambatte. Saves/cheats/MSU1/zip are stubbed off.
- **libstdc++.a**: the one non-trivial dependency. `memmap.cpp`, `controls.cpp`, `bml.cpp`
  use `std::string`/`std::stringstream`/`std::map` (iostream + locale + Rb_tree). These
  files are essential (ROM load / input), so the usage can't be excised. SOLUTION: link
  `libstdc++.a` AND, unlike the gambatte blob, KEEP `.init_array` and run it at blob
  entry so `ios_base::Init`/locale are constructed before use (standard bare-metal C++).

## Plan (phased, mirrors the gambatte port)
1. **Blob builds + links** (next): `snes_core_abi.h`, `snes_core_exports.cpp` (drive the
   libretro `retro_*` API: minimal environment cb, set RGB565 pixel format, feed ROM,
   pump `retro_run` with video/audio/input callbacks), `snes_shim.cpp`, `snes_blob_libc.c`,
   `snes_core_blob.ld` (fixed VMA, keeps .init_array; run it in the entry), `build_core_blob.sh`.
   Pick a VMA in the WB window [0x4E000000,0x56000000): gpSP@0x4E400000, gambatte@0x4E800000,
   arena@0x50000000. snes9x blob is bigger (~2-4 MB) -> propose 0x4F000000 (leaves gambatte
   room to 0x4F000000 and the blob 16 MB to the arena). Its big buffers (ROM/RAM/work) go in
   the shared 0x50000000 arena, reused since only one core runs at a time.
2. **lk_a side**: `snes_core_loader.c` (load blob from boot_b), `snes_sd_run.c` (session
   runner on its OWN thread - see NOTE item 2), display path (SNES 256x224 / 512x448 hires,
   RGB565 -> integer/aspect scale like `ayaneo_gb_show_frame`), input map, audio (32 kHz SPC
   -> AFE), dispatch in `gba_driver.c` on `GBA_CONSOLE_SNES`, `/roms/snes` (+.sfc) scan in
   `sd_fat.c`, boot_b packing for the third blob in `build_snes_boot_b.py`, menu badge/logo.
3. **Perf pass**: snes9x mainline is accuracy-first; on the A55 in LK expect full speed on
   plain games but heavy on SuperFX/SA-1/DSP. If too slow, evaluate snes9x2010 (faster fork)
   as a drop-in alternative. Software blitter + 32 kHz audio add load.

## Open decisions (confirm before phase 2)
- Perf target / acceptable game set (all games vs plain-mapper only)? Fallback to snes9x2010?
- Blob VMA (proposed 0x4F000000).
- Audio priority (bring up video/input first, audio second - as gambatte did).
</content>
