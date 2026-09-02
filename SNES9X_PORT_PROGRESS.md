# snes9x (SNES) core port to LK - progress

Porting the libretro **snes9x** core (https://github.com/libretro/snes9x) into the
GBA-from-SD flow as a THIRD loadable boot_b blob, alongside gpSP (GBA) and gambatte
(GB/GBC). Branch `lk-gba-emu-sd-card`. Read `emu/CORE_PORTING_NOTES.md` first - the
gambatte port established the whole pattern (blob at a fixed VMA, exports/imports ABI,
bundled libc + shim, boot_b packing, per-console display/dispatch/threading).

## Status: FEATURE PARITY PASS - save states + Pico menu + core options (2026-09-02)

Save states FIXED (was fully broken): root cause was the bundled `vsnprintf` computing
`e = buf + (cap-1)`; sprintf passes cap=(size_t)-1 so on 32-bit that WRAPS below buf and
`sprintf` returned an empty string - every snes9x block header was blank, so unfreeze
rejected the state (only 64-bit host round-trips passed). Fixed the overflow; also split
the auto-suspend slot ("sus") from the manual Save/Load slot ("st0"), and moved the state
buffer into the session's mapped arena (0x52C00000, heap 48->36 MB) off the menu chrome
cache. Verified on device: freeze -> SD -> unfreeze round-trip passes (oem diag
`snes-ss: core=1 sd=1`). SD `/states/snes` + `/saves/snes` auto-created by fat_wr_mkpath.

Pico in-game menu now near parity with GB/GBC/GBA: LCD Filter (Off/Scanlines/Grid/Dot
Matrix, applied in ayaneo_snes_show_frame), CPU Clock (600..2000 MHz OPP step, session
floors 1400), Benchmark (Uncap, skips vsync + audio, shows emulated FPS), Panel Refresh
(128-frame measured ~60.11 Hz). PLUS snes9x core-option settings routed through a new
blob env_cb option store + set_option export: Aspect Ratio (4:3/Pixel/NTSC/PAL, honoured
by a fractional-horizontal/integer-vertical display scaler on the 1280x960 panel),
Overscan (crop 8/12/16px/off), Audio Filter (Gaussian/Cubic/Sinc/Linear/None), Hi-Res
Blend (Off/Merge/Blur). Color Correction intentionally omitted (GB-specific).

Enter/exit punch transitions DONE (parity with GBA/GBC): launch grows a gameplay circle
over the frozen menu snapshot (ayaneo_snes_punch_prerender + shared compositor); AYA-hold
exit shrinks the frozen frame back into the carousel (snes_menu_arm_reverse mode 2). The
0x54xxxxxx/0x55xxxxxx transition buffers ARE mapped during a SNES session (GBC already uses
them) - the earlier "unmapped" scare was actually the sprintf bug writing identical garbage.

FIXED a nasty recurring bug: the GammaOS settings block was written to boot_b at
0x01E00000 = the snes9x blob offset, so any settings change corrupted the blob ("ASET"
magic, SNES fails to load). Moved settings to a guarded tail slot (0x020FF000). See
CORE_PORTING_NOTES 9d.

Run-ahead DONE: Off/Balanced/Responsive/Max in the Pico menu (shared preempt-frames
setting). Each frame commits one real frame, then saves state, runs pf muted look-ahead
frames with the current input, presents the future frame, and rewinds - the display leads
by pf frames to cut input latency. Clock escalates per tier (1400/1600/1800/2000 MHz) for
the pf+1 emulations/frame. Off under the menu / benchmark / headless test; state in the
0x53000000 scratch slot (disabled if the state exceeds 2 MB).

The four snes9x option picks (aspect/overscan/audio/hires) now PERSIST across sessions:
packed into one u32 at offset 52 of the GammaOS settings block (ver 5->6, backward
compatible); restored + pushed to the core at session start, repacked/saved on change.

R2 fast-forward added (parity with GBA): hold R2 (GPIO 57) to run flat out - present 1-in-8,
skip vsync, suppress audio + run-ahead. Off under menu/benchmark/headless.

RUN-AHEAD CRASH FIXED (2026-09-02): snes9x's serialize/unserialize new ~15 block buffers per
call and the bump arena never frees, so per-frame run-ahead leaked ~0.5 MB/frame and
exhausted the 36 MB arena -> new returns NULL -> LK crash mid-game. Added heap_mark/heap_reset
exports; run-ahead (and manual save/load) now mark before the save and reset after the load
to reclaim the temporaries. Verified headless (oem snes-ra:N forces run-ahead in the test):
depth 3 for 600 frames keeps peak arena usage FLAT at 20.2 MB = baseline. Note: depth 3 (Max)
is heavy (4 emulations + 2 serializes/frame) - stable but slow; use Balanced/Responsive on
demanding games.

STRETCH aspect option added (user request): a 5th Aspect Ratio choice that fills the whole
1280x960 panel, no bars. Display scaler is now 2D source-driven (Bresenham on both axes) so
it handles fractional vertical (224->960); normal modes keep integer vertical for clean
scanlines. Persisted with the other aspect picks.

Pico menu Up/Down AND Left/Right now auto-repeat (press, ~0.37 s delay -> ~0.08 s repeat) -
holding cycles menu rows or values (slots 0-9, CPU clock, aspect...). Benchmark toggles on A
only so repeating L/R never flips it. Settings persistence is DEBOUNCED (~0.5 s after the
last change, or on menu close / session exit) so rapid value changes no longer burst flash
writes; the value still applies in memory instantly.

Verified (2026-09-02) the exit reverse-punch buffer 0x55800000 is writable during a SNES
session (oem diag snes-ss revmap=1) - the AYA-hold exit-transition path is memory-safe (was
never exercised by the frame-limited headless test).

Multiple save-state slots added: a "Save Slot" (0-9) menu item; manual Save/Load use
/states/snes/<rom>.st<slot> (suspend stays on "sus", self-test on "sstst"). Beyond GB/GBC/GBA
(single slot). The selected slot PERSISTS across launches (settings block ver 7, offset 56),
and the Save Slot item shows used/empty (cheap 16-byte SD probe on slot change / save, not
per frame).

Turbo (auto-fire) added: a Pico-menu item cycling Off / A / B / A+B; holding the turbo'd
button rapid-fires it at ~15 Hz (pulse in ayaneo_snes_pad_mask). Persisted (settings ver 8,
offset 60).

Feature set now at parity with GB/GBC/GBA (incl. fast-forward) and beyond (multi-slot saves,
turbo). Optional leftovers: color-correction option (omitted as GB-specific); tuning the
run-ahead tiers if Max is too heavy on some games.

LCM refresh switch VALIDATED on device (2026-09-02) via oem diag: menu vfp 23 ->
g_dbg_hz1000 = 59723 (59.723 Hz measured); SNES session vfp 17 -> g_snes_dbg_hz1000 = 60088
(60.088 Hz measured, snes_vfp read-back = 17); reverts to 23 on exit. Added
ayaneo_dsi_get_vfp() + measured-Hz diag fields. Also rewrote the SNES display scaler
source-driven (decode each pixel once, Bresenham span fill) to cut per-frame render cost so
the Pico menu overlay stops dropping frames.

Timing readout bug fixed (2026-09-02): the Pico menu's Panel Refresh showed ~2.7 Hz and
Benchmark FPS was wrong because the timing math assumed an 812.5 kHz counter / 128-frame
window; gpt4 is 13 MHz and the 104000000000 constant is for an 8-frame window. Corrected to
match the GBA path (8-frame average + outlier reject; benchmark frames*13e6/ticks). If a
REAL framerate drop remains while the menu is open (SNES emulation + overlay near the 16.6
ms budget), the fix is to freeze emulation under the menu (standard pause behaviour) -
pending user confirmation that stutter persists after the readout fix.

## Status: ON-DEVICE PLAYABLE - renders + animates + audio + 1400 MHz (2026-09-02)

Confirmed on device (`oem snes-launch:600` + `oem diag`, SMW): `nz=835` non-black,
`chg=251` frames animating, `aud=319798` audio sample-pairs / 600 frames = 533/frame x
60 fps ~= 31980 Hz (matches the ~32040 Hz SPC rate) => audio path is live, `pitch=2048`.
User confirmed frames drawing on the panel. Perf: snes9x mainline is heavy on the A55, so
the SNES session now raises the ARM PLL to >=1400 MHz (restored on exit); if still slow,
next options are 1600 MHz or the faster snes9x2010 fork.

RECURRING GOTCHA (bit me again this session): a stale/mismatched **boot_b** shows up as
`oem snes-probe` reading `m=0x54455341` ("ASET", asset bytes) instead of `0x31534e53`
("SNS1") at 0x01E00000, and `snes-launch` returns `exit=2 stage=1` (snes_core_load NULL).
ALWAYS re-flash boot_b when the on-device blob magic is not "SNS1", even for a
lk_a-only change, and probe first. Also: TESTING needs a NORMAL `fastboot reboot` (the
menu thread that consumes `oem snes-launch` only runs then); `reboot-bootloader` is the
low-level flashing mode with no menu (every launch returns ran=0).

## Status: ON-DEVICE FIXED - SNES renders + animates (APU handshake resolved)

ROOT CAUSE (2026-09-02): **`.init_array` ran before `heap_init`**, so every global C++
constructor's `operator new` returned NULL from the still-unarmed bump arena. The worst
victim was `SNES::smp`'s ctor (`apuram = new uint8[64*1024]`): with `apuram == NULL`, all
SPC700 RAM/port writes went to physical address 0 (writes silently lost, reads back 0),
so the IPL ROM's `mov $f4,#$aa` never landed in `apuram[$f4]`. The main 65816 spun on the
`$2140` ready handshake forever -> frozen black + no audio. Invisible on host because the
native `operator new` is always live regardless of order.

Localization method (on-device, since host couldn't reproduce): instrumented
`SMP::op_write`, published counters through the `snes_dbg_pc` export, decoded via
`oem snes-launch:N` + `oem diag`'s `snes-px:` line. Sequence: store-vanishes-immediately
(readback 0 on the same line) -> captured `apuram` pointer -> pointer read `0x0001aa00`
(impossible for a `>=0x50800000` arena) => NULL/unarmed-arena allocation.

FIX: `snes_core_blob_init` no longer runs `.init_array`; it is deferred to `snes_init()`,
which LK calls immediately AFTER `heap_init()` arms the arena (snes_sd_run.c:213-214).
After the fix, `oem snes-launch:600` on the reloaded blob: `apuram=0x50800000` (valid),
`nz=835` (non-black), `chg=251` (frames animating) = real gameplay. Debug instrumentation
removed; `snes_dbg_pc` stubbed to 0. Next: user visual confirmation of SMW on the panel.

### (history) earlier bring-up steps

On-device debugging via `oem diag` counters + `oem snes-probe`:
1. exit=2 (blob load fail): STALE on-device boot_b. Re-flashing boot_b fixed it; the probe
   then confirmed snes_core_load()=OK. LESSON: always re-flash boot_b after a blob change.
2. black screen + no audio, but stage=5 / frames advancing / dim=256x224 / pitch=2048 / nz=0
   (frames genuinely black): the core build scripts were MISSING `-fno-strict-aliasing`.
   Upstream snes9x's Makefile sets it (lines 685-686); snes9x type-puns memory heavily, so
   with strict aliasing on (default at -Os) the compiler reordered/elided those accesses and
   the emulation ran but produced garbage/black. Added `-fno-strict-aliasing` to both
   build_core.sh and build_core_blob.sh. (The host validation only worked because that build
   command happened to include the flag - a false-positive that hid the bug.) Re-flashed.
   Awaiting confirmation that SMW now renders.

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

SUSPEND/RESUME + RESET: done (gbc parity). Host round-trip proved snes9x
serialize/unserialize is DETERMINISTIC (state ~0.8 MB for SMW; frame sequence bit-identical
after restore). Runner now writes a save STATE to /states/snes/<rom>.st0 on exit and reloads
it at launch (unless B held = start fresh), plus a SELECT+START+L+R soft-reset combo. State
buffer at 0x53800000 (4 MB, just above the 48 MB heap, below the 0x54000000 snapshot).

IN-GAME MENU: done (GammaOS Pico, gbc-style). AYA tap toggles a centred overlay
(Brightness, Volume, Save State, Load State, Reset Game, Close); AYA-hold ~1.5 s exits to
the selector. The game keeps running underneath with its input gated in
ayaneo_snes_pad_mask (returns 0 while open); ayaneo_snes_show_frame paints the overlay via
snes_menu_paint. Save/Load act on /states/snes/<rom>.st0 (same buffer/path as suspend).
Brightness/Volume reuse the shared ayaneo helpers + persist.

PERF: ayaneo_snes_show_frame no longer memsets the whole 1280x960x4 panel (~4.9 MB) every
frame; borders are static black so both buffers are cleared only on a resolution change
(256<->512 / 224<->239<->448) or first frame, with ayaneo_display_prepare's session-start
black-fill as the safety net. Removes ~295 MB/s of pointless memset from the 16.6 ms budget
(matters for hitting 60 fps under vsync-lock). Mirrors the GB path.

DISPLAY DIMS VALIDATED (host): host_test now reports the min/max video dims over a run.
SMW/SF2 Turbo/Secret of Mana/Jurassic Park all output 256x224 pitch 2048 (= 1024px stride),
confirming the display reads pitch/2=1024 stride and blits sx<sw correctly. The scale map
(256->4x, 512->2x; <=240->4x, else 2x) sends every SNES mode to 1024 wide / <=956 tall by
construction, so hi-res 512 + interlace 448/478 are correct too (they only trigger on
in-game menus that need input to reach, so a passive run stays at 256x224). No code change.

REMAINING: run-ahead + launch/close punch transitions (mirror gbc; run-ahead is heavy for
SNES - 0.8 MB state/frame), audio fine-trim. STILL UNVALIDATED on HW (headless) - need a
.sfc/.smc in /roms/snes to confirm the LK wiring (video/input/audio/vfp switch/badge/
suspend/menu). Feature-complete + host-proven; further polish wants real device feedback.

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
