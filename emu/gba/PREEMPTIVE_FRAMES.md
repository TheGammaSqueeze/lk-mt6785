# Preemptive Frames (input-latency reduction) for the LK gpSP GBA emulator

This document describes how "Preemptive Frames" is implemented in the bare-metal
LK gpSP GBA emulator (AYANEO pairmini, MT6785 / k85v1_64), why it exists, the
architecture-specific hazards we hit, and the on-device benchmarks.

Configurable via the GammaOS Pico in-game menu item **Preemptive Frames**, with
named tiers **Off / Balanced / Responsive / Max** (internally 0 / 1 / 2 / 3),
persisted in the settings blob. Default is **Off**.

> **Dynamic Preemptive Frames (the shippable feature).** The tier is a *max desired*
> look-ahead. Two loops keep it honest so it can never backfire: a closed loop
> that adapts the actual depth every frame to hold a locked 60 fps (section 9), and
> a per-tier CPU-clock escalation (Off 600 MHz / Balanced 800 / Responsive 900 /
> Max 1000) that gives the deeper look-ahead the headroom to actually run. Net: the
> lowest input latency the game and hardware can sustain, without ever dropping a
> frame or starving audio. The menu shows the live adaptive depth in parens
> (e.g. "Max (3)") so the player sees it working.

> **Shipped approach: run-ahead, at exact 1x speed.** We implemented and measured
> both run-ahead and true preemptive. Preemptive was tried on hardware but
> reverted: it modifies the committed timeline on every input edge (audio
> trajectory discontinuity = audible popping) and has a variable per-frame cost
> (heavy only on edge frames = frame pacing judder). Run-ahead keeps the committed
> timeline pristine (continuous, click-free audio) and has a constant per-frame
> cost (smooth pacing). Its original ~0.34% fast skew (the dynarec re-entry stub)
> is now fully eliminated by `gba_frame_boundary_finish` (section 5), so the
> committed frame advances an exact 280896 cycles and no audio-clamp workaround is
> needed. Sections 4-6 keep the full analysis; section 8 has the benchmarks.

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
  load S (light flush) ; s_cpu_clean_boundary = 1 ; s_cpu_restart_req = 1
  # no priming frame: the NEXT committed run_one_frame is the clean re-entry
```

The committed timeline is never modified by input injection, so its audio is
pristine and continuous. The look-ahead frames are muted. Cost is constant every
frame (N+1 emulations), giving steady pacing. The 512 KB `s_ahead_state` snapshot
lives in BSS (free versus the 2 MB lk_a image limit).

### Exact 1x (the priming stub, eliminated)

The rewind's dynarec flush forces a longjmp re-entry (the parked translated block
is stale). That longjmp jumps out of `switch_to_main_thread()` BEFORE `update_gba`
runs the tail of its `vcount==228` block (update_gbc_sound / process_cheats /
VCOUNT=0), so `io_registers[REG_VCOUNT]` stays at 227 and the re-entered frame is
a degenerate ~960-cycle stub. Left uncorrected that stub is additive with the
committed frame (280896+960 = ~0.34% fast).

`gba_frame_boundary_finish()` (main.c) runs that skipped tail on the run-ahead
re-entry only (gated by `s_cpu_clean_boundary`), including restoring the one
hblank (272 cyc) the mid-wrap snapshot omits, so the re-entry starts a clean
vcount=0 boundary and runs a FULL frame. The separate priming frame is therefore
removed - the next committed frame IS the clean re-entry. Measured: committed
cpu_ticks over a 60-frame window = exactly 59*280896 at pf=1/2/3 = true 1x, no
audio-clamp workaround needed. Normal play / reset / close / intro re-enter
unchanged (they are one-shot, so the harmless stub is left in place for them).

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

- Pico menu item **Preemptive Frames**, tiers Off / Balanced / Responsive / Max
  (0..3), persisted (settings VER 5, offset 48; `ayaneo_get/set_preempt_frames`).
  The value shows the live adaptive depth, e.g. "Max (3)".
- Each tier escalates the CPU clock (`preempt_apply_cpu`, applied in the game loop
  on any tier change): Off 600 / Balanced 800 / Responsive 900 / Max 1000 MHz.
  These stay in a voltage-stable band (ayaneo_set_cpu_mhz sets only the ARM PLL,
  not core voltage, so higher clocks can undervolt-glitch some silicon).
  The CPU Clock menu item can fine-tune afterward.
- `oem preempt:<0..3>` - set the tier live over fastboot (for measurement).
- `oem diag` - reports `hz1000=` (panel Hz*1000), `em=` (avg committed-frame us),
  `ft=` (committed GBA cycles = 280896), `epf=` (live adaptive depth), `mhz=` (CPU
  clock), `afr=` (cumulative audio frames submitted - climbs = audio flowing).

## 8. Benchmarks (on-device, AYANEO pairmini, Pokemon-class ROM)

Panel refresh is locked to the GBA rate (59.7275 Hz); `hz1000` is the measured
panel refresh, `ft` is the committed frame's GBA cycles (280896 = one exact
frame), `em` is average emulation wall time per frame. Frame budget at 60 fps is
~16.6 ms. `em` is scene-dependent (~2 to ~5.5 ms base).

### Dynamic Preemptive Frames (shipped) - heavy scene, per-tier CPU escalation

| Tier       | CPU MHz | em (us) | epf (live depth) | hz1000 | ft     |
|------------|---------|---------|------------------|--------|--------|
| Off        | 599     | 5057    | 0                | 59725  | 280896 |
| Balanced   | 999     | 2741    | 1                | 59730  | 280896 |
| Responsive | 1399    | 2187    | 2                | 59729  | 280896 |
| Max        | 1799    | 1795    | 3                | 59728  | 280896 |

Same demanding scene at every tier. The CPU escalation drops the per-frame
emulation cost (`em`), so the closed loop sustains the tier's full look-ahead
(`epf` = the configured depth) while holding a locked 60 fps - at 600 MHz that
scene only afforded epf=0. `ft` stays an exact 280896 (true 1x; the committed
cpu_ticks over a 60-frame window is exactly 59*280896 = 16572864). The committed
audio is pristine and click-free and `afr` (audio frames submitted) climbs
steadily at every tier.

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
| Run-ahead (ship)  | 1.000x (exact)      | pristine/continuous | constant cost, smooth |
| Preemptive        | 1.000x              | pops on edges    | variable cost, judder  |

## 9. Limitations / future

- **Per-frame rewind cost + adaptive depth.** Run-ahead runs N+1 emulations + a
  512 KB save/load every frame; the emulation cost is scene-dependent (~2.3 ms
  light, ~5 ms heavy) and there is ~7 ms of fixed per-frame overhead (present +
  battery/LED/volume polls). At a fixed depth a heavy scene overruns the ~16.7 ms
  vsync budget and the loop (which masters audio) starves the AFE ring = slow
  motion or silence. So the depth is a CLOSED LOOP on the measured full per-frame
  period (`preempt_adapt`): shed one level the instant the period runs long
  (>17.4 ms), ramp back up one level after ~4 s of clean frames. A lone probe
  frame is absorbed by the audio ring, so it settles at the deepest depth that
  holds 60 fps - heavy scenes fall to 0 (run-ahead off, full 60 fps + solid
  audio), light scenes get the configured depth. The menu setting is a "max
  desired depth".
- **Per-tier CPU-clock escalation raises that ceiling.** Each tier bumps the ARM
  PLL (Off 600 / Balanced 800 / Responsive 900 / Max 1000 MHz via
  `ayaneo_set_cpu_mhz`), which lowers the per-frame emulation cost so the closed
  loop sustains the requested depth instead of backing off. Device-measured in a
  heavy scene (committed em ~5 ms at 600 MHz -> epf 0): Balanced 999 MHz -> epf 1,
  Responsive 1399 MHz -> epf 2, Max 1799 MHz -> epf 3, all at hz=59.73 / ft=280896.
  Higher clock costs power/heat, which is why it scales with the tier the user
  chose rather than running maxed by default.
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
- `emu/gba/main.c` - `gba_frame_boundary_finish()` (the exact-1x re-entry tail).
- `emu/gba/menu_fastboot.c` - `oem preempt:` / `oem diag`.
- `platform/mt6785/ayaneo_audio.c` - `ayaneo_get/set_preempt_frames` + persistence.

Commits: `a65f445`, `7cff000` (run-ahead + light flush), `bb19959` + `3470ec0`
(preemptive, later reverted), `2983a57` (revert to run-ahead), `63afe03`
(exact-1x re-entry fix).
