# Preemptive Frames (input-latency reduction) for the LK gpSP GBA emulator

This document describes how "Preemptive Frames" is implemented in the bare-metal
LK gpSP GBA emulator (AYANEO pairmini, MT6785 / k85v1_64), why it exists, the
architecture-specific hazards we hit, and the on-device benchmarks.

Configurable via the GammaOS Pico in-game menu item **Preemptive Frames**
(Off / 1 / 2 / 3), persisted in the settings blob. Default is **Off**.

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

We implemented run-ahead first to validate the mechanism, then moved to preemptive
because it is both cheaper (lower average CPU / heat on a handheld) and, as
explained in section 5, the only one that keeps the game running at exactly 1x
speed on this architecture.

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

## 5. Why preemptive fixes it

In preemptive, a static frame is literally one `run_one_frame()` with no rewind,
so it advances exactly 280896 cycles = no priming stub, no speed skew. The 960
phantom only occurs on the rare input-edge frame, where it does not accumulate in
the audio (only the final replayed frame feeds audio) and is imperceptible.

## 6. The preemptive algorithm (`preempt_step`, gba_driver.c)

State:

- `s_pf_ring[PF_RING_MAX=4][512 KB]` (BSS; BSS is free versus the 2 MB lk_a image
  limit) - a ring of committed states, `s_pf_ring[head]` = state ENTERING the
  current frame.
- `s_pf_head`, `s_pf_fill` (count of valid consecutive past states),
  `s_pf_prev_in` (GBA button mask applied last frame).

Per displayed frame, after `update_buttons()` latches the mask:

```
pf   = ayaneo_get_preempt_frames()
cur  = s_keys
changed = (cur != s_pf_prev_in)

if pf == 0 or menu/fast-forward/benchmark:
    invalidate ring; run_one_frame(); return      # plain, exact 1x

save current state -> s_pf_ring[head]              # state entering this frame

if changed and fill >= pf:                         # INPUT EDGE
    back = head - pf
    load s_pf_ring[back]  (light flush) ; s_cpu_restart_req = 1
    suppress audio
    run_one_frame()                                # priming stub
    for i in 0..pf-1:                              # replay pf historical frames
        run_one_frame()
        save state -> ring slot (back+1+i)         # refresh ring on new trajectory
    unsuppress audio
    run_one_frame()                                # the current frame, audio ON
else:
    run_one_frame()                                # static frame, audio ON

head++ (mod RING) ; fill = min(fill+1, RING-1)
```

The new input is held (via `s_keys`) throughout the replay, so the game "sees" it
`pf` frames earlier. Only the final replayed frame feeds audio; the replayed
history was already heard, so it is muted. The ring is refreshed along the new
trajectory during the replay so back-to-back edges stay correct. Net committed
advance is exactly one frame per displayed frame (plus the ~960 phantom on an
edge frame only). The present at the bottom of the loop shows
`gba_core_screen()`, which after `preempt_step` holds the current frame.

### Ring invalidation

`preempt_invalidate()` (clears fill/head/prev-input) is called at every
discontinuous jump of the committed timeline so a later edge cannot rewind into a
stale or previous-game state:

- soft reset (Pico menu "Reset Game" item AND the Select+Start+L+R hotkey),
- game switch via in-game "Close" (after the new ROM loads),
- a fresh launch punch.

## 7. Configuration and diagnostics

- Pico menu item **Preemptive Frames** (Off/1/2/3), persisted (settings VER 5,
  offset 48; `ayaneo_get/set_preempt_frames`).
- `oem preempt:<0..3>` - set the depth live over fastboot (for measurement).
- `oem diag` - reports `hz1000=` (panel Hz*1000), `em=` (avg emulation us/frame),
  `ft=` (committed GBA cycles the last frame: 280896 static, 281856 on an edge).
- `oem pretest:<n>` - self-test: force the rewind+replay edge path for the next
  `n` frames without needing real input, to stress it device-blind.

## 8. Benchmarks (on-device, AYANEO pairmini, Pokemon-class ROM)

Panel refresh is locked to the GBA rate (59.7275 Hz); `hz1000` is the measured
panel refresh, `ft` is committed GBA cycles per frame, `em` is average emulation
wall time per frame. Frame budget at 60 fps is ~16.6 ms.

### Static play (the real-world case)

| Preempt | hz1000  | ft (cycles/frame) | Result          |
|---------|---------|-------------------|-----------------|
| Off (0) | 59727   | 280896            | 60 fps, 1.000x  |
| 1       | 59728   | 280896            | 60 fps, 1.000x  |
| 2       | 59731   | 280896            | 60 fps, 1.000x  |
| 3       | 58286   | 280896            | ~58 fps (edge)  |

`ft` is a constant 280896 at every setting = exact 1x game speed, no skew.
Static frames hold a locked 60 fps at pf=1 and pf=2; pf=3 occasionally grazes the
budget and dips slightly.

`em` (emulation cost) is scene-dependent (~2.3-5.5 ms base). Enabling preemptive
adds ~1.3-1.8 ms per frame for the per-frame 512 KB ring save (kept warm so any
frame can become a rewind origin). Even in heavy scenes this stays well under the
16.6 ms budget.

### Input-edge frames (rare) and worst case

A real input edge costs `pf+1` emulations + one priming stub for that single
frame. To bound the worst case we forced EVERY frame to take the edge path
(`oem pretest:`), which never happens in real play:

| Test                         | Preempt | hz1000     | ft      | Notes                     |
|------------------------------|---------|------------|---------|---------------------------|
| Forced edge every frame      | 2       | 46000-53000| 281856  | pathological, no crash    |
| Recovery after test          | 2       | 59730      | 280896  | clean return to 1x/60 fps |

Even hammering the rewind+replay path on every single frame never crashed or hung
and recovered cleanly; real gameplay only takes edges on actual button presses
(a handful per second), so the average overhead is close to the static numbers.

### Run-ahead vs preemptive (why we switched)

| Approach          | committed cycles/frame | speed | avg cost                 |
|-------------------|------------------------|-------|--------------------------|
| Run-ahead (N=1/2) | 281856                 | 1.003x (0.34% fast) | N+1 emu every frame |
| Preemptive (ship) | 280896 static          | 1.000x | 1 emu static, N+1 on edge |

## 9. Limitations / future

- **Per-frame ring save cost** (~1.3-1.8 ms) is paid on every frame even when
  input never changes, because any frame may become a rewind origin. It fits the
  budget but is power/heat the idle case does not strictly need; a cheaper or
  lazier ring is possible future work.
- **Autofire** toggles the button mask every few frames, so it triggers frequent
  edges (higher CPU + a tiny non-accumulating drift) while autofiring. Acceptable.
- Preemptive reduces perceived latency at the moment of an input change (when it
  matters); a continuously held direction does not get run-ahead's constant lead.
  For button-tap responsiveness the two are equivalent.

## 10. Source map

- `emu/gba/gba_driver.c` - `preempt_step()`, `preempt_invalidate()`, the ring, the
  main-loop integration, `ft=` probe, `oem pretest` plumbing.
- `emu/gba/gba_wrap.c` - `g_gba_audio_suppress` gate in `gba_audio_cb`;
  `gba_core_cpu_ticks()`.
- `emu/gba/gba_memory.c` - `g_gba_load_light` (RAM-only flush path in
  `gba_load_state`).
- `emu/gba/menu_fastboot.c` - `oem preempt:` / `oem pretest:` / `oem diag`.
- `platform/mt6785/ayaneo_audio.c` - `ayaneo_get/set_preempt_frames`, persistence.

Commits: `a65f445`, `7cff000` (run-ahead + light flush), `bb19959` (preemptive
replacing run-ahead), `3470ec0` (ring invalidation).
