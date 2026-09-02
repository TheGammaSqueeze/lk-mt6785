# snes9x (SNES) core port to LK - progress

Porting the libretro **snes9x** core (https://github.com/libretro/snes9x) into the
GBA-from-SD flow as a THIRD loadable boot_b blob, alongside gpSP (GBA) and gambatte
(GB/GBC). Branch `lk-gba-emu-sd-card`. Read `emu/CORE_PORTING_NOTES.md` first - the
gambatte port established the whole pattern (blob at a fixed VMA, exports/imports ABI,
bundled libc + shim, boot_b packing, per-console display/dispatch/threading).

## Status: FOUNDATION LAID (compiles + link surface characterized)

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
