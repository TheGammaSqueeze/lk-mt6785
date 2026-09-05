# Genesis performance campaign (autonomous, 15-min cron)

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
- (pending first flash) ROOT CAUSE FOUND: build_core.sh HOT_SET never matched (find paths are `core/...`,
  HOT_SET entries were un-prefixed) so the ENTIRE core built at -Os. Fixed: strip `core/` in opt_for,
  OPT_HOT=-O3 / OPT_COLD=-O2, widened HOT_SET. Expect a big fps jump. MEASURE with gen-bench after flash.

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
