# snes9x (SNES) core port to LK - progress

Porting the libretro **snes9x** core (https://github.com/libretro/snes9x) into the
GBA-from-SD flow as a THIRD loadable boot_b blob, alongside gpSP (GBA) and gambatte
(GB/GBC). Branch `lk-gba-emu-sd-card`. Read `emu/CORE_PORTING_NOTES.md` first - the
gambatte port established the whole pattern (blob at a fixed VMA, exports/imports ABI,
bundled libc + shim, boot_b packing, per-console display/dispatch/threading).

## Local core benchmark + profile (2026-09-03)

Added tools/ayaneo/snes/core_bench.cpp + build_core_bench.sh: runs the REAL snes9x core natively
against a SYNTHETIC in-memory LoROM (a tight 65816 loop), timing retro_run and checking a
deterministic savestate round-trip. Enables local measure+correctness of core changes with no game
ROM and no device (host: 0.281 ms/frame, savestate PASS). Build --prof for a gprof profile.

gprof flat profile (synthetic ROM; video_cb is the harness hash, ignore). CORE hot spots:
- SPC_DSP voice processing ~29% (voice_V3c = Gaussian interpolation 14.4% ALONE, + V8_V5_V2,
  V4, V9_V6_V3, run). Runs all 8 voices every sample even when idle. THIS is why Audio Filter=
  Linear gives +7 fps - it swaps voice_V3c's Gaussian for the cheap linear tap.
- CPU ~31% (S9xMainLoop + Immediate8 7.2% + per-opcode; opcode mix is skewed by the synthetic
  DEX/BNE loop so not representative for opcode-specific tuning).
- SPC700 SMP ~13%. PPU ~3% (blank screen; real games higher).
Takeaway: the audio DSP is the biggest single core cost and its one safe knob (interpolation) is
already user-exposed. Further core gains need accuracy-affecting DSP/CPU edits (risky) - not doing
those autonomously. The harness is ready for any future core change: measure host ms/frame + assert
savestate PASS locally, then confirm on device via snes-bench (uncapped, chains safely).

## Measurement gotchas learned the hard way (2026-09-03)

- Back-to-back `oem snes-launch` (CAPPED) crashes the device on the 2nd launch (fastboot wedges
  to ????, needs a power-cycle). But chained `oem snes-bench` (UNCAPPED) is fine - the run-ahead
  sweep ran snes-bench x4 back-to-back cleanly. So: measure CORE fps with snes-bench (chains ok);
  measure DISPLAY show_us with a SINGLE snes-launch per boot (reboot between aspects). Root cause
  of the 2nd-capped-launch crash is unconfirmed (likely a session-teardown/re-arm issue in the
  capped present path) - worth fixing but only with one-launch-per-boot testing.
- NEVER poll `fastboot devices` (or any fastboot cmd) while a background task is holding the
  fastboot channel - concurrent access corrupts the USB protocol and wedges it to ????.
- Local core profiling needs a game ROM, which lives on the device SD card, not the dev box. The
  host harness (build_host_test.sh) is ready but has no ROM to run here, so core hot-path work must
  be measured on-device via snes-bench (safe, chains).

## Stretch-vs-Pixel display cost: root cause (2026-09-03)

Why Stretch (fill-panel 1280x960) is slower than Pixel-perfect (letterboxed 1024x896):
- Stretch writes 1.23M dest pixels vs Pixel 0.92M (+33%). Host scaler bench (tools/ayaneo/snes/
  scaler_bench.c, the exact row-replication fast path): Pixel 0.128 ms, Stretch 0.148 ms (+15%
  in the blit loop). The scaler is already optimal (decode-once + memcpy row replication).
- On device the display cost is dominated by the FIXED per-frame full-panel cache flush
  (arch_clean_cache_range ~4.9 MB). Measured via new g_snes_show_us (blit+flush, no vsync) during
  a capped oem snes-launch: PIXEL show_us=2079 us (~2.08 ms/frame). Of that, the flush's dirty-
  line writeback (~4 MB) is ~1.1-1.4 ms and the scale blit ~0.5-0.9 ms. Stretch pays ~15% more
  because both the blit and the dirty writeback scale with pixel count.
- CONCLUSION: Stretch being slower is INHERENT (33% more pixels), not a regression or a scaler
  inefficiency. Software cannot make filling more pixels as cheap as fewer. A partial cache flush
  (only the dirty game rows) would speed up Pixel/letterboxed modes ~7% but does NOT help Stretch
  (its game area is the whole panel), so it does not close the gap. The only way to fully close it
  is HARDWARE display-overlay scaling (upscale a small buffer via DISP-OVL) - a large change.
- FINAL on-device numbers (single snes-launch per boot, oem diag show_us): PIXEL 2133 us/frame,
  STRETCH 2279 us/frame = +146 us (+6.8%). Smaller than the host blit-only +15% because the fixed
  4.9MB cache flush (~1.2-1.5 ms) dominates both and only the pixel-count-dependent blit+writeback
  differs. In a 16.6 ms frame budget the +146 us matters only when the budget is already tight
  (heavy game + run-ahead). A partial cache flush (dirty rows only) would save ~20-50 us on Pixel
  and 0 on Stretch, so it does not close the gap - not worth the dirty-region-tracking complexity.
  CONCLUSION: Stretch slowdown is inherent (+6.8%, more pixels), no safe software fix; only a
  hardware display-overlay scaler would eliminate it. Instrumentation (oem diag show_us, oem
  snes-stretch:N) kept for future display work.

## Audio-interpolation speed knob is ALREADY user-controllable (2026-09-03)

The DSP interpolation (per-voice per-sample, the bapu SPC_DSP) honours Settings.InterpolationMethod
and is EXPOSED as the Pico "Audio Filter" menu item (snes9x_audio_interpolation: Gaussian default,
Cubic, Sinc, Linear, None). Measured cost at 1400 MHz on top of LTO: Gaussian 121 fps, Linear 128
fps (+7, +5.8%). Net vs -Os baseline with Linear: 95 -> 128 (+35%). Gaussian is the authentic SNES
timbre, so it stays the DEFAULT; the player opts into Linear/None for more fps via the menu - no
code change. (A hardcode of Settings.InterpolationMethod in snes_load was tried as the measurement
vehicle, then removed: it would override the menu selection.)

Also confirmed no-op: LTO link -O3 vs -O2 produces a byte-identical blob - under -flto the link-time
-O is ignored; the per-file compile opt (OPT_HOT -O2) drives LTO. Do not bother changing the link -O.

## EMULATION SPEEDUP pass 2 (2026-09-03): LTO whole-program -> 118 to 121 fps, smaller blob

Switched the blob link to the g++ driver with -flto (-nostdlib/-nostartfiles, linker script via
-Wl,-T, -O2 link-time). LTO inlines across TUs (the hot memory-access path) AND its whole-program
dead-code elimination removes unused chip support that --gc-sections alone could not - so the blob
is SMALLER (1.97 MB, 129 KB spare) than selective -O2 (1.99 MB, 105 KB) even though LTO -O2 now
optimizes EVERY file, not just the hot set. Measured @1400: 118 -> 121 fps, savestate core=1 sd=1
fast=1. Net vs the original -Os baseline: 95 -> 121 fps (+27%). LTO is standard for libretro cores
and -fno-strict-aliasing guards the usual LTO UB; -ffat-lto-objects keeps the ld fallback working.

## EMULATION SPEEDUP pass (2026-09-03): selective -O2 -> base 95 to 118 fps (+24%)

The whole snes9x core was built with -Os (optimize for SIZE) + generic -march=armv7-a. The blob
must fit a fixed 2 MB slot in boot_b (partition boot_b = 32 MB, blob at 0x01E00000..0x02000000),
so whole-core -O2 (2.27 MB) overflows by ~170 KB. SELECTIVE -O2 instead: the HOT per-frame files
(cpu, cpuexec, cpuops, dma, gfx, ppu, memmap, apu/apu, sdsp, smp, smp_state) get -O2; cold chip
code + tile (1.2 MB at -O2: inlined template variants, memory-bound) + sa1/sa1cpu (SA-1 carts
only) stay -Os. Plus -ffunction-sections/-fdata-sections with link --gc-sections. Blob = 1.99 MB
(105 KB spare). Measured uncapped @1400: base ra0 95 -> 118 fps (+24%), run-ahead ra1 37 -> 46 fps.
Savestate self-test still core=1 sd=1 fast=1 (no corruption).

NEGATIVE RESULTS (tried, reverted - do not re-try without a division-heavy game):
- -march=armv8-a (over armv7-a): 118 -> 119 fps = noise. AArch32 armv8 emits hardware sdiv
  (vs __aeabi_idiv), but SMW is not division-bound so no measurable gain. Size-neutral, stable,
  savestate clean - reverted per the "measurable win only" rule to keep the baseline disciplined.
- OPT_HOT -O3 (over -O2): 118 -> 120 fps = +1.7% (borderline noise). Cost: blob +66 KB (spare
  drops 105->38 KB), and -O3 bloats the cpuops opcode switch, risking icache thrashing on larger
  games not covered by the SMW bench. Marginal gain not worth the budget + untested-regression
  risk; reverted. -O2 is the interpreter sweet spot here.
- -mtune=cortex-a55 (over default armv7-a scheduling, ISA unchanged): 121 -> 119 fps on both of
  two runs = flat/slightly worse, savestate clean. gcc 10.3's A55 pipeline model does not help
  this code (same instructions, reordered). Size-identical. Reverted. Failed abort-crash note: whole-core -O2 boot_b flash returns
"size too large" - MUST keep blob < 2 MB; flash BOTH lk_a+boot_b when the blob changes.

## Perf/stability pass (2026-09-03): benchmark harness, clock, run-ahead RCA

User reports addressed: (1) Pico "Close" now EXITS to the ROM selector (relabeled "Exit Game",
arms the reverse-punch); B / AYA-tap still just dismiss the overlay. (2) Benchmark is now truly
UNCAPPED - it presented every frame before, so the reported FPS was the ~5 ms blit, not the
emulator; now it sparse-presents (1-in-8) like fast-forward. (3) Scaler row-replication: the
fast path renders ONE dest row then memcpy-replicates it to the rest, so Stretch/large scales
no longer take the per-row scalar-store hit. (4) Default SNES clock is 1800 MHz (reads 1799).

Headless benchmark harness added for the cron: `oem snes-bench:N` (uncapped, reports fps),
combined with `oem snes-ra:N`, plus an `oem diag` "snes-bench: fps=.. ra=.. mhz=.." line.

STABLE 1400 MHz benchmark sweep (uncapped, no crash - the safe harness clock):
  ra=0 (off) 95 fps | ra=1 37 fps | ra=2 27 fps | ra=3 21 fps (all exit=5, no brownout).
Base emulation is 95 fps at 1400 (10.5 ms/frame); run-ahead pf=1 = 2 emulations = ~47 fps
ideal, 37 measured. To hit 60 with pf=1 you need ~120 fps base = a much faster core. Capped
1800 no-run-ahead play verified STABLE (snes-launch:300 exit=5, hz=60088).

RUN-AHEAD FINDING (root cause of "poor at all clocks" + the GBA FAULT the user saw):
- Uncapped FPS at 1800 MHz: ra=1 -> 47, ra=2 -> 34, ra=3 -> 27. Each depth adds a FULL extra
  emulation/frame; snes9x is heavy, so run-ahead cannot reach 60 fps at any safe clock. Serialize
  overhead is small (~1.3 ms) - it's the extra emulations, i.e. compute-bound.
- Because run-ahead can't cap to 60, it runs at ~100% CPU duty. At 1800/2000 MHz (LK has no Vproc
  scaling; 1400 is the highest guaranteed-stable OPP) that sustained load BROWNS OUT the core:
  data abort pc=0x4f00e5ec dfar=0xffffffff (a legit instruction dereferencing a pointer whose
  VALUE glitched to ~0xffffffff). Reproduced by the uncapped bench at ra=0 AND ra=3 -> device
  halts. This is the same mechanism as the earlier "LK crash mid-game after enabling runahead".
- FIXES: run-ahead tiers clamped to the stable 1400 MHz (s_snes_ra_opp = {1800,1400,1400,1400};
  Off=1800 capped play, any run-ahead tier=1400) so enabling it can no longer crash. The headless
  bench also pins 1400. Normal capped play stays at 1800 (CPU idles ~40%/frame, stable).
- OPEN DECISION for the user: run-ahead at stable 1400 is safe but well under 60 fps (choppy).
  To get fast AND stable run-ahead needs either a lighter core (snes9x2010) or a Vproc bump
  (PMIC over-volt, risky). Recommend evaluating snes9x2010; otherwise keep run-ahead off.

## Validation note: `oem snes-launch` needs a WARM menu (2026-09-02)

`cmd_snes_launch` only sets `g_snes_test_limit` + `g_dbg_snes_launch` and then waits for the
**menu thread** to pick the flag up and run the session. After a direct `fastboot reboot
bootloader`, LK enters fastboot WITHOUT running the menu loop, so nothing services the flag and
the launch reports `ran=0 exit=0` with `oem diag` showing `stage=0 nz=0 ... core=0 sd=0`. That
is the cold-bootloader "menu not running" condition, NOT a regression. To validate the SNES path
headlessly, run `oem snes-launch` while the menu subsystem is warm (i.e. the device was already
sitting at the menu when it dropped to fastboot), where a healthy result is `exit=5` with
`snes-ss core=1 sd=1 fast=1 heap=~20.2M revmap=1`. A cold `reboot bootloader` needs the menu to
run once first. (No code change; verified `2d24f34` still cold-boots to the menu cleanly.)

## Status: UX POLISH 2 - Run-Ahead inert label fits the row (2026-09-02)

The previous pass appended " (N/A: state>2M)" to the tier name; for the longer tiers
("Balanced"/"Responsive") the right-aligned value string (24-26 chars * 16 px) overran into
the row label (label ends ~px+172, value would start ~px+76-108). The Run-Ahead row now shows
a short "N/A (>2MB)" (10 chars, right-aligns to ~px+332, clear of the label) instead of the
tier name when a tier is selected but inert - clearer (the tier value is meaningless when it
cannot engage) and no overlap on any tier. Verified: flashed lk_a, oem snes-launch ->
nz=806 chg=100821 hz1000=60088 vfp=17, snes-ss core=1 sd=1 fast=1 heap=20215840 revmap=1.
boot_b unchanged.

## Status: UX POLISH - surface inert Run-Ahead tiers (2026-09-02)

When a game's save state exceeds the 2 MB look-ahead slot (SNES_AHEAD_CAP), run-ahead is
silently forced off (`ra_ssz = 0`), yet the Pico menu still showed the selected tier
(Balanced/Responsive/Max) as if it were active - the player got no latency reduction and no
indication why. The session now records availability once (`s_snes_ra_avail = (ra_ssz != 0)`)
and the Run-Ahead row appends " (N/A: state>2M)" when a tier is selected but inert. SNES-only
(menu render + run loop); no shared display/menu-carousel code touched. Verified: flashed lk_a,
`oem snes-launch:400` -> frames=71973, snes-px nz=807 chg=83092 hz1000=60088 vfp=17,
snes-ss core=1 sd=1 fast=1 heap=20215840 revmap=1 (the 256 KB test ROM's state fits, so it
renders the normal tier - the N/A path engages only on large-state carts). boot_b unchanged.

## Status: PERF PASS 3 - throttle live-play diagnostic sampling (2026-09-02)

The run loop's full-frame content-hash sample (~900 video reads + FNV steps per frame) only
feeds the `oem diag` / headless `snes-px` counters (`nz`/`changed`), a bootloader-side debug
path that is never read during real play. It now samples EVERY frame only in the frame-limited
headless test (so `chg`/`nz` stay exact for validation) and 1-in-16 in live play, removing the
per-frame waste with zero visual/behavioural change. Verified: `oem snes-launch:400` ->
frames=45383 exit=5, snes-px nz=806 chg=46731 (test-mode per-frame sampling intact),
hz1000=60088 vfp=17, snes-ss core=1 sd=1 fast=1 heap=20215840 revmap=1. No GBA/GBC/menu
regression (shared display code untouched; change is SNES-run-loop only). boot_b unchanged.

## Status: PERF PASS 2 - menu freezes emu, HW volume rocker, filter fast path (2026-09-02)

Follow-up to the on-device perf/bug reports:

- **HW volume rocker now works in-game.** The SNES run loop never polled the physical
  volume keys (only the Pico menu's Volume row applied `s_gbc_vol`), so the rocker did
  nothing during play. Added `snes_poll_volume()` (mirrors `gba_driver.c poll_volume`):
  edge-detected `mtk_detect_key(0x11/0x00)`, SELECT modifier -> brightness else volume,
  debounced persist via `snes_settings_touch`, transient on-screen bar via
  `ayaneo_gbc_osd_show`. Called every loop iteration (works menu-open or closed).
  `ayaneo_snes_show_frame` now also draws the OSD slider (`ayaneo_draw_osd`), which it
  previously skipped (GBC/GBA-only), so the bar is visible during SNES.

- **Menu freezes emulation (fixes the inverse-FPS report).** The game clock is pinned to
  the run-ahead tier (Off=1400 MHz); opening the menu with run-ahead OFF rendered
  game+overlay at only 1400 MHz and dropped frames. The loop now skips `c->run` (and audio
  submit) while `g_snes_menu_open`; the last `snes_frame` persists and is re-presented under
  the overlay, so the menu always fits the frame budget. Audio ring is silenced on the
  menu-open edge (`ayaneo_menu_audio_silence`) so it does not drone the frozen buffer.

- **LCD filter scaler fast path.** The scanline/grid branch tested `lastrow||lastcol` per
  destination pixel. Hoisted out of the inner column loop: non-last rows are a tight `px`
  fill with at most one `dk` patch (grid column); the last row is a straight `dk` fill. No
  per-pixel branch, so Scanlines/Grid/Dot-Matrix cost far less.

Verified (lk_a flashed, `oem snes-launch:400` + `oem diag`): frames=45264 exit=5,
snes-px nz=807 chg=23803 hz1000=60088 vfp=17 (live, correct 60.088 Hz per-core refresh),
snes-ss core=1 sd=1 fast=1 heap=20215840 (run-ahead leak-free) revmap=1. Menu path
hz=59727 unchanged (no GBA/GBC/menu/cold-boot regression). boot_b unchanged (code-only).

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

Run-ahead now uses FAST savestates (direct-memory unfreeze, no per-block new/memcpy) via a
set_ra_fast export + RETRO_AV_ENABLE_FAST_SAVESTATES in env_cb, toggled only around the
run-ahead save/load (SD saves stay portable). Verified: video renders (env change is a no-op
for rendering), core=1 sd=1, fast round-trip fast=1, and depth 3 now finishes 600 frames in
the test window (was ~591) while staying leak-free. Higher run-ahead tiers are now usable.

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
