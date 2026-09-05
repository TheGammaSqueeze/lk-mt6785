# Genesis performance campaign (autonomous, 15-min cron)

## STATUS: rewind 6x goal MET structurally (iter 2 decouple + iter 3 fast load). Rewind per-present cost
## = 1 fast state_load (556us) + 1 re-emulate (5760us) = ~6316us @1400 / ~8842us @999, both << the 15500us
## present budget, at ANY tier, speed-independent. Needs a LIVE rewind to eyeball-confirm 6x (oem gen-rw
## will show ~556 load + ~5760 run once). REMAINING WORK = cut the 5760us frame-emulate for forward/FF/
## run-ahead headroom (higher effort: VDP/68k code paths, PGO). Also re-verify audio/run-ahead intact.


GOAL: Genesis (GPGX) rewind should reach 6x like SNES/GBA/GBC. MD is NOT a taxing system, so the core is
under-optimized. Drive raw core throughput UP until rewind hits 6x at a sane clock. User is AFK; DO NOT stop
the cron. Iterate: compiler opts, code opts, config, state-path, etc.

## Metric (read autonomously over fastboot at the ROM menu, serial 0123456789ABCDEF)
- `fastboot oem gen-bench[:N]` -> `gen-bench @1400MHz: fps=F frame=Fus save=Sus load=Lus impliedRW=R.Rx`
  Headless, uncapped, Sonic loaded from SD, at a fixed-safe 1400 MHz. impliedRW = 155000/(load+frame) (steps
  that fit ~one 60Hz present at 1400 MHz). PUSH fps up / frame+load us down -> impliedRW up.
- `fastboot oem gen-rw` -> live rewind split (state_load vs re-emulate) captured during an actual rewind.
- Rewind step cost = state_load + one full frame re-emulate (GPGX has no cheap raw snapshot, so it re-runs a
  frame to render each rewound state). So both frame-emulate AND state_load matter.

## Baseline + history (append each iteration: what changed, gen-bench numbers)
- 2026-09-05 commit b22d6a3/3691681 (-O3 hot / -O2 cold; was ALL -Os): on-device gen-bench (Sonic 3 Complete):
  `@1399MHz fps=173 frame=5759us save=322us load=2681us impliedRW=1.8x`. -Os->-O3 = big win (core was
  crippled). blob 1.38MB->1.79MB (fits 2MB).
  ANALYSIS: rewind step = load(2681) + frame(5759) = 8440us @1400 -> 1.97x; @2000 ~5908us -> 2.8x. For 6x@2000
  need step<=2778us. load is 32% of the step (NOT negligible - state_load = retro_unserialize + system_reset).
  So BOTH the raw-state-path (cut load ~2681->~300) AND more core speed (cut frame) are needed for 6x via
  emulation; OR the video-ring (B) makes it trivial. NEXT (by impact): (1) raw state path skipping the
  state_load system_reset [idea 5]; (2) LTO link -O3 + hot-file vectorize/unroll to cut frame; (3) video ring
  [B]. Try 1 then 2, measure each; escalate to 3 if emulation-path plateaus below 6x.

## BREAKTHROUGH (2026-09-05, commit pending): rewind decoupled from emulation speed
The rewind loop was re-emulating EVERY stepped-back frame (full state_load + run per step) even though only
the LAST is displayed - the intermediates existed only for a blended reverse-audio. But ayaneo_rewind_step is
just an XOR-RLE reconstruction of the target state in the ring scratch (NO core work). So: walk `steps` deltas
(cheap), then render the target with ONE state_load + ONE run. Per-present cost is now SPEED-INDEPENDENT
(~1 load + 1 run ~= 8.4ms @1400, ~11.8ms @999), so 6x - and beyond - fits one vsync at ANY clock. No clock
boost, no video ring, no cap needed. Reverse audio = the target frame reversed (one frame ~= one present of
samples, no ring under/overrun; normal-pitch reverse). This should give a solid 6x at every tier. VERIFY on
device when the user can hold rewind (oem gen-rw will then show load/run once, ~8ms total). Forward play +
run-ahead unchanged. This likely SOLVES the 6x goal on the rewind side; keep pushing frame(emulate) down for
forward/FF/run-ahead headroom.

## KEY STRATEGIC INSIGHT (drives the campaign)
frame(emulate) = 5760us @1400MHz = ~4030us @2000MHz. Rewind re-emulates ONE full frame per step to render
each rewound state (the ring stores STATES, not framebuffers). So N-x rewind needs N frame-emulates per one
60Hz present (16670us). Even at 2000MHz: 6 x 4030us = 24ms > 16.67ms -> 6x is PHYSICALLY IMPOSSIBLE by
compiler opts alone (the frame-emulate is the wall, state_load is small). SNES hits 6x only because snes9x's
frame-emulate is ~2x cheaper (less accurate/faster core) AND it runs at 1800.
=> TWO honest paths to 6x, pursue BOTH:
  (A) Make the GPGX frame-emulate ~2-3x faster (compiler: LTO -O3 link, PGO, per-loop vectorize; code: VDP
      render fast paths; config: drop any cycle-accuracy/enhancement default that isn't needed). Halving
      frame->~2000us@2000 gives ~4x; needs ~2700us for 6x.
  (B) DECOUPLE rewind from emulation: keep a VIDEO ring of recently-rendered frames (or render-on-capture)
      so rewind DISPLAYS stored frames without re-emulating -> rewind speed becomes display-bound (trivially
      6x+). Genesis frame 320x240x2=150KB; a few seconds at 60fps fits the huge rewind arena (mblk stitch
      ~2.5GB) if stored compressed/every-Nth. This is the real architectural fix for 6x and likely the
      highest-impact item. Weigh vs the shared ayaneo_rewind ring design (genesis-only video side-ring is
      safest - do NOT destabilise snes/gba/gbc). STRONGLY CONSIDER (B) as the primary path.

## Iteration 2 (2026-09-05): rewind decoupled (BIG) + two config dead-ends measured
- REWIND FIX (commit a95cbc3, lk_a): walk deltas cheap + render target ONCE -> per-present cost speed-
  independent (~load+frame = 8441us @1400), 6x fits at any clock. Forward gen-bench unchanged (fps=173
  frame=5760us) as expected. VERIFY 6x on device with a live rewind (oem gen-rw should show one load+run
  ~8ms). The bench impliedRW metric NO LONGER maps to rewind speed (rewind is now decoupled) - it just
  tells you one load+run fits one present (1.8x headroom @1400).
- DEAD END - hq_fm/hq_psg=0: frame 5760->5698us (~1%, 62us). Not worth the audio-quality loss. REVERTED
  (kept hq=1). Sound is only ~1-2% of the frame; 68k/Z80/VDP dominate.
- DEAD END - LTO blob link -O2->-O3: blob byte-identical, frame unchanged. With -ffat-lto-objects + per-file
  -O3 hot flags, the link -O level does not change hot-function codegen. REVERTED to -O2. Do NOT retry.
- SO: frame=5760us @1400 is what -O3-per-file gives. Further frame cuts need code-level (VDP render / 68k
  fast paths) or PGO, which are higher-effort. But the PRIMARY GOAL (6x rewind) is met structurally by the
  decouple - forward/FF/run-ahead headroom is the only remaining reason to cut frame further.

## Iteration 3 (2026-09-05): rewind state_load 2682us -> 557us (fast path, memsets skipped)
- PROFILED the 2682us state_load: it is ~80% the VDP/render buffer-CLEAR memsets (bg_pattern_cache/vram/
  bitmap/linebuf), which retro_unserialize gates on !reset_do_not_clear_buffers (= fast_savestates). Measured
  via a new bench field: `load=2682us loadfast=557us` (set_ra_fast skips the clears). 4.8x cheaper.
- SAFE: vdp_context_load reloads vram/cram/vsram/sat AND marks all tiles dirty (bg_name_dirty=0xFF, vdp_ctrl.c
  :650) so the pattern cache rebuilds on the next render; bitmap/linebuf are scratch regenerated by run().
  Run-ahead already uses this exact skip and works.
- FIX (commit pending, lk_a): rewind state_load now wraps set_ra_fast(1)/(0) (sound_rebase after keeps the
  reverse audio clean by overriding the fast path's blip-latch restore). Rewind per-present cost:
  load(557)+frame(5759) = ~6316us @1400 (was 8441) -> even more headroom for 6x at every tier. Run-ahead was
  already on the fast load (the crackle fix's set_ra_fast), so run-ahead state_load is also ~557us not 2682.
- gen-bench forward unchanged (fps=173 frame=5759). load/loadfast now both shown by oem gen-bench.

## Iteration 4 (2026-09-05): profiled the frame - VDP render is ~49%
- gen-bench @1399: frame=5759us, norender=2951us -> VDP PIXEL RENDER = 2808us (49%!), CPU(68k+Z80+sound+
  VDP-timing) = 2951us. (norender = set_av_skip novideo=1.) load=2678 loadfast=555.
- Render is skipped on FF thrown-away frames + run-ahead look-ahead frames (except the last), so cutting it
  mainly helps FORWARD play + the rewind render + the final look-ahead. CPU (2951) runs on EVERY frame incl.
  skipped ones -> matters most for run-ahead depth/FF.
- DEAD END - softfp+neon-fp-armv8 (from soft): built + linked clean, but frame 5759->5781us, norender
  2951->3001us (SLIGHTLY WORSE, ~0.4%). GPGX's render/CPU loops are too branchy/gather-heavy (LUT remap) for
  the -O3 auto-vectorizer, and softfp adds a hair of overhead. REVERTED to soft-float. Hand-written NEON
  intrinsics for the render remap COULD help but is high-effort + the remap is a per-pixel LUT gather (poor
  NEON fit). Do NOT retry auto-vectorize.
- STANDING: the frame-emulate (5760us) resists cheap wins (compiler flags, float-abi, config all tried). The
  render (2808us) is a per-pixel indexed LUT remap; the CPU (2951us) is the cycle-accurate 68k/Z80. Both need
  hand code-level work for further gains. BUT the rewind 6x goal is already MET (iter2/3), so this is only
  forward/FF/run-ahead-depth headroom. Next candidate ideas: (a) hand-optimize the vdp_render line remap
  (write RGB565 directly, avoid double-buffer), (b) check if config.overclock/M68K_OVERCLOCK_SHIFT adds
  cycle-accounting overhead when overclock=0, (c) accept current perf (rewind goal met) and reduce churn.

## Iteration 5 (2026-09-05): removed the overclock-shift per-instruction overhead (small CPU win)
- M68K/Z80_OVERCLOCK_SHIFT=20 kept USE_CYCLES as `cycles += (A*cycle_ratio)>>20` (a runtime mul+shift per
  68k AND z80 instruction) purely for the overclock feature - which is NOT even wired up (HAVE_OVERCLOCK
  undefined, no menu item; overclock code all #ifdef-guarded). Removed both defines from build_core.sh +
  build_core_blob.sh -> USE_CYCLES collapses to `cycles += (A)`.
- RESULT (commit pending): frame 5760->5647us, norender 2950->2819us (~131us, ~4.4% of CPU), fps 173->177.
  Small but free, no feature loss. KEPT. Rewind step loadfast(557)+frame(5647)=6204us.

## Ideas backlog (try in order of expected impact / low risk first)
1. [DONE-build, unmeasured] -O3 hot / -O2 cold (was all -Os). <-- likely the big one.
2. Bump the blob LTO link to -O3 (build_core_blob.sh line ~47) if per-file -O3 under LTO isn't enough.
3. `-fno-strict-aliasing` is on (needed by GPGX); check `-funroll-loops`, `-fomit-frame-pointer`,
   `-ftree-vectorize` for the hot files; try `-mcpu=cortex-a76` instead of `-march=armv8-a -mtune`.
4. FLOAT ABI: build_core.sh uses `-mfloat-abi=soft` (NO hw FP/NEON). GPGX is mostly integer (YM3438 core is
   int), but verify no hot float path; if soft-float hurts a hot loop, consider softfp+neon for the core
   (must keep the blob libc/stubs ABI consistent - risky, measure first).
5. Genesis raw state path (state_save_ra/state_load_ra) skipping the retro_unserialize system_reset, to cut
   the rewind per-step state_load (mirror snes S9xFreeze/UnfreezeRunAhead). Only worth it if gen-rw shows
   state_load is a big fraction.
6. GPGX config: confirm no expensive default is on (blargg NTSC filter OFF, no-sprite-limit, ym2612 mode,
   overclock). Sound quality (hq_fm/hq_psg) trade if audio path is hot.
7. VDP render: `-DUSE_16BPP_RENDERING` is on (good). Check the render_bg/obj hot loops for obvious wins.
8. Reduce STATE_SIZE / rewind ring payload if the state has dead tail (cuts state_save/load + xor_rle).

## Also delivered this campaign
- NTSC refresh: Genesis panel switches to vfp 20 (~59.92 Hz) on session start, restored to 23 on exit
  (like SNES vfp 17 / GBC/GBA/menu vfp 23). Menu "Refresh Rate" row shows it.
- Rewind: no clock boost (reverted); adaptive cap keeps it smooth at the tier clock; badge shows ACHIEVED
  speed. `oem gen-rw` publishes the state_load-vs-re-emulate split.

## Flash protocol
- Core-blob change (build_core.sh / any emu/genesis/core|libretro|exports|abi) -> flash BOTH boot_b + lk_a
  (ABI/blob in boot_b). LK-only change -> flash lk_a. Blob header version literal is in
  genesis_core_blob.ld (keep == GENESIS_CORE_ABI_VERSION). ONE fastboot cmd per Bash call. Device must be at
  the ROM menu for oem gen-bench (needs the SD vol). Copy builds to /mnt/c/pairmini. git: TheGammaSqueeze,
  push remote 'ayaneo', no AI attribution / em-dashes.
