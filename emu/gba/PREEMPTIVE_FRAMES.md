# Preemptive Frames (input-latency reduction) for the LK gpSP GBA emulator

This document describes how "Preemptive Frames" is implemented in the bare-metal
LK gpSP GBA emulator (AYANEO pairmini, MT6785 / k85v1_64), why it exists, the
architecture-specific hazards we hit, and the on-device benchmarks.

Configurable via the GammaOS Pico in-game menu item **Preemptive Frames**
(Off / 1 / 2 / 3), persisted in the settings blob. Default is **Off**.

> **Shipped approach: run-ahead.** We implemented and measured both run-ahead and
> true preemptive. Preemptive was tried on hardware but reverted: it modifies the
> committed timeline on every input edge (audio trajectory discontinuity = audible
> popping) and has a variable per-frame cost (heavy only on edge frames = frame
> pacing judder). Run-ahead keeps the committed timeline pristine (continuous,
> click-free audio) and has a constant per-frame cost (smooth pacing); its only
> drawback, a ~0.34% fast game speed, is absorbed by the audio clock-recovery.
> Sections 4-6 keep the full analysis of both; section 8 has the benchmarks.

## 1. Motivation

A game has internal input lag: a button press is sampled at the top of a frame,
but the on-screen reaction typically appears 1-3 frames later because the game
polls input, updates logic, then renders. On top of that the display pipeline
adds its own latency. Running in LK (no Android compositor / input stack) already
removes ~40-60 ms versus Android, but the game's own internal lag remains.

"Run-ahead" and "Preemptive Frames" (the two names RetroArch uses) both hide that
internal lag by running the emulator further ahead of what it is about to show,
so the reaction to an input surfaces one or more frames sooner.

## 2. Two approaches

**Run-ahead (classic).** Every displayed frame:
1. run the real committed frame (audio ON),
2. snapshot it,
3. run N extra frames with the same held input (audio OFF),
4. present the Nth "ahead" frame,
5. roll back to the committed snapshot.

The display is always N frames ahead of the committed/audio timeline, so latency
drops every frame. Cost: N+1 emulations per displayed frame, every frame.

**Preemptive frames (what we ship).** Keep a ring of the last few committed
states. On a normal (static-input) frame just run one frame. Only when the input
**changes** do we rewind N committed frames and replay them plus the current frame
with the new input held, so the game has "seen" the new input N frames earlier and
its reaction surfaces that much sooner. Cost: 1 emulation on the (vast majority of)
static frames, N+1 only on the rare input-edge frame.

We implemented run-ahead first, then tried preemptive (cheaper, and exact 1x
speed - see section 5), but on-hardware testing showed preemptive's committed-
timeline modification causes audio popping and its variable cost causes pacing
judder (section 8). We shipped run-ahead: continuous pristine audio and steady
pacing, with the 0.34% speed offset absorbed by the audio clock-recovery.

## 3. Architecture-specific mechanisms

The emulator core is gpSP with an ARM dynarec. In LK we drive its two halves from
separate threads:

- **CPU thread** runs `gba_core_cpu_loop()` -> `execute_arm_translate()` and
  yields once per frame at vblank (`switch_to_main_thread()` -> `gba_yield_to_main`).
- **Main/driver thread** kicks the CPU thread once per frame via events
  (`run_one_frame()`: signal `ev_cpu`, wait `ev_main`).

Key primitives, all pre-existing:

- **Savestate**: `gba_core_state_save/load(void*)`, `gba_core_state_size()` =
  512 KB, deterministic memcpy of machine state (regs, RAM, video/timer counters,
  `cpu_ticks`, `execute_cycles`).
- **Dynarec flush on load**: `gba_load_state()` flushes the RAM/ROM/BIOS
  translation caches so stale translated blocks are not executed after the loaded
  RAM changes.
- **Clean CPU-thread re-entry after a load**: because the savestate is *machine*
  state only (not the CPU thread's C stack / dynarec host-register context), after
  a state load the CPU thread must NOT resume its parked translated block. It
  re-enters `execute_arm_translate` from the top at the loaded PC via
  `s_cpu_restart_req` + a `__builtin_setjmp`/`longjmp` (`s_cpu_jb`), the same path
  used by the intro->game reset.
- **Audio suppression for replayed frames**: `g_gba_audio_suppress` gates
  `gba_audio_cb` (gba_wrap.c). When set, the gpSP audio ring is still drained (so
  the core does not desync) but the samples are NOT forwarded to the AFE sink.
  We do NOT latch `ayaneo_gbc_audio_pause` (that would mute everything).
- **Light flush**: `g_gba_load_light` makes `gba_load_state()` flush only the RAM
  translation cache. The rewind restores a state captured earlier this session
  from the SAME ROM, so the ROM/BIOS caches are still valid; keeping that hot code
  warm roughly halves the committed-frame cost after a rewind.

## 4. The 0.34% speed bug in run-ahead (and how we found it)

The threaded-CPU + longjmp re-entry has a subtle cost. When we rewind to a state
saved at a frame boundary and re-enter via longjmp, the first re-entered frame
yields again almost immediately after a **~960-cycle "priming stub"** (the restored
state sits right at the vcount==228 boundary, so the first `update_gba` tick
re-hits the yield). Those 960 cycles are real, extra machine-state advancement.

In run-ahead this priming stub runs once per displayed frame and its cycles are
ADDITIVE with the committed frame. We measured it on-device by probing the GBA
`cpu_ticks` delta:

- exact one GBA frame = **280896** cycles (16.78 MHz / 59.7275 Hz),
- run-ahead committed advance = **281856** cycles/frame = 280896 + 960,
- => the game runs **0.34% fast**.

0.34% exceeds the audio clock-recovery trim range (+-0.25%), so it would surface
as a slow pitch/ring drift. The single-frame `ft=` (committed cycles this frame)
in `oem diag` reads a clean 280896 or 281856 and is the reliable probe;
cross-frame `cpu_ticks` window probes proved unreliable (the counter
wraps/rebases over long windows).

## 5. The shipped run-ahead loop (`preempt_present`, gba_driver.c)

The main loop runs the committed frame normally, then calls `preempt_present()`
at the vsync-paced present point:

```
run_one_frame()                 # committed frame, audio ON, advances 1 frame

preempt_present():
  pf = ayaneo_get_preempt_frames()
  if pf == 0: show(committed); return
  save committed state S
  suppress audio
  for i in 0..pf-1: run_one_frame()      # look-ahead frames (muted)
  unsuppress audio
  show(current screen)                   # present the look-ahead frame
  load S (light flush) ; s_cpu_restart_req = 1
  run_one_frame()                        # priming re-entry (audio ON)
```

The committed timeline is never modified by input injection, so its audio is
pristine and continuous. The look-ahead frames are muted; the priming re-entry
(the ~960-cycle stub) runs with audio ON so the committed audio stream stays
continuous. Cost is constant every frame (N+1 emulations), giving steady pacing.
The 512 KB `s_ahead_state` snapshot lives in BSS (free versus the 2 MB lk_a image
limit).

### The 0.34% offset

The committed timeline advances 280896 + 960 = 281856 cycles/frame. The audio
clock-recovery clamp in `ayaneo_audio.c` is widened from ~0.25% to ~0.67% so it
absorbs that (plus panel/boot-measure error) instead of saturating and firing the
emergency sample-repeat snap (the periodic pop). Audio and video both run ~0.34%
fast, in sync, at an inaudible pitch offset.

## 6. The preemptive alternative (implemented, then reverted)

We also implemented true preemptive: a ring of the last few committed states, and
on an input EDGE rewind pf frames and replay them + the current frame with the new
input held (muted except the last), so static frames run exactly 1x (no stub) and
only edges pay the rewind. It measured clean (exact speed, held 60 fps, no crash
under a forced-edge stress) but on hardware produced audio popping (the committed
timeline is re-simulated per edge, so the game audio trajectory jumps) and pacing
judder (variable per-frame cost). We reverted it; run-ahead's continuous audio and
constant cost feel better. The code is in git history (commits `bb19959`,
`3470ec0`) if the tradeoff is ever revisited (e.g. with a CPU-clock bump).

## 7. Configuration and diagnostics

- Pico menu item **Preemptive Frames** (Off/1/2/3), persisted (settings VER 5,
  offset 48; `ayaneo_get/set_preempt_frames`).
- `oem preempt:<0..3>` - set the depth live over fastboot (for measurement).
- `oem diag` - reports `hz1000=` (panel Hz*1000), `em=` (avg emulation us/frame),
  `ft=` (committed frame's GBA cycles = 280896).

## 8. Benchmarks (on-device, AYANEO pairmini, Pokemon-class ROM)

Panel refresh is locked to the GBA rate (59.7275 Hz); `hz1000` is the measured
panel refresh, `ft` is the committed frame's GBA cycles (280896 = one exact
frame), `em` is average emulation wall time per frame. Frame budget at 60 fps is
~16.6 ms. `em` is scene-dependent (~2 to ~5.5 ms base).

### Run-ahead (shipped)

| Preempt | hz1000 | ft     | Result                          |
|---------|--------|--------|---------------------------------|
| Off (0) | 59727  | 280896 | 60 fps baseline                 |
| 1       | 59729  | 280896 | 60 fps, constant cost (smooth)  |
| 2       | 59721  | 280896 | 60 fps, constant cost (smooth)  |
| 3       | 56150  | 280896 | ~56 fps in heavy scenes (extreme) |

`ft` is the committed frame (280896); the per-frame rewind adds a ~960-cycle
priming stub, so the committed timeline advances 281856 cycles/frame = ~0.34%
fast. That offset is absorbed by the widened audio clock-recovery clamp (~0.67%),
so audio and video both run ~0.34% fast, in sync, at an inaudible pitch offset.
Cost is constant every frame (N+1 emulations), so pacing is steady and the
committed audio is pristine and click-free. pf=1/2 hold a locked 60 fps; pf=3
(4 emulations/frame) grazes or exceeds the budget in heavy scenes.

### Why not preemptive (measured, then reverted)

Preemptive was implemented and measured: static frames ran exactly 280896 (1.000x,
no skew) and held 60 fps, and a forced-every-frame edge stress (`oem pretest:`)
never crashed (hz dipped to ~46-53 under that pathological load, recovering
cleanly). But in real play it produced **audio popping** (the committed timeline
is re-simulated on each input edge, so the game audio trajectory jumps) and
**pacing judder** (edge frames cost `pf+1` emulations while static frames cost 1,
so per-frame time is variable). Run-ahead has neither because it never modifies
the committed timeline and its cost is constant.

| Approach          | speed               | audio            | pacing                 |
|-------------------|---------------------|------------------|------------------------|
| Run-ahead (ship)  | 1.003x (absorbed)   | pristine/continuous | constant cost, smooth |
| Preemptive        | 1.000x              | pops on edges    | variable cost, judder  |

## 9. Limitations / future

- **Per-frame rewind cost.** Run-ahead runs N+1 emulations + a 512 KB save/load
  every frame. pf=1/2 fit the budget; pf=3 can exceed it in heavy scenes on this
  downclocked part. Bumping the CPU clock while a game runs would give pf=3 more
  headroom (future work).
- The 0.34% speed offset is inherent to the dynarec longjmp re-entry (the priming
  stub); it is absorbed in audio and imperceptible in video, but a from-scratch
  fix would require the re-entry to run gpSP's post-yield cleanup so the stub runs
  a full frame instead of a degenerate one.
- Run-ahead reduces latency every frame (display is always N ahead), which is the
  smooth, click-free behavior we want; preemptive's theoretical win (exact speed,
  lower average CPU) did not survive contact with real audio/pacing on hardware.

## 10. Source map

- `emu/gba/gba_driver.c` - `preempt_present()` (the shipped run-ahead), main-loop
  integration, `ft=` probe.
- `emu/gba/gba_wrap.c` - `g_gba_audio_suppress` gate in `gba_audio_cb`;
  `gba_core_cpu_ticks()`.
- `emu/gba/gba_memory.c` - `g_gba_load_light` (RAM-only flush path in
  `gba_load_state`).
- `emu/gba/menu_fastboot.c` - `oem preempt:` / `oem diag`.
- `platform/mt6785/ayaneo_audio.c` - `ayaneo_get/set_preempt_frames` + persistence;
  widened clock-recovery clamp.

Commits: `a65f445`, `7cff000` (run-ahead + light flush), `bb19959` + `3470ec0`
(preemptive, later reverted), `2983a57` (revert to run-ahead + widen audio clamp).
