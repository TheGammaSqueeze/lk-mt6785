# Beating input lag on a bare-metal handheld: Preemptive Frames for GBA

This is the story of how we made Game Boy Advance games feel *more* responsive than
they do on some real hardware, running on a phone-grade chip in a bootloader, with no
operating system, no GPU, and a single CPU core clocked as low as we could get away
with. The feature is called **Preemptive Frames** in the menu. Under the hood it is
run-ahead, and getting it to run at exactly the right speed, with clean audio, on this
hardware took a few fights worth writing down.

## The hardware we are working with

It helps to be honest about the machine first, because the constraints shape everything:

- **One small core.** This runs inside LK (Little Kernel, the bootloader) on an
  MediaTek MT6785. There is no SMP here: the emulator's CPU thread and the frontend that
  draws and presents frames share a **single ARM core**. A look-ahead frame is not
  offloaded anywhere. It is pure extra serial work inside the same 16.7 ms budget.
- **Minimal clocks by default.** That core sits at the **lowest operating point, 600
  MHz**, out of the box, for battery and heat. We only raise it when the player asks for
  a deeper setting.
- **No GPU.** There is no 3D block, no compositor, no hardware scaler in the path. The
  240x160 GBA frame is upscaled to the panel and pushed to the framebuffer entirely in
  software on that same CPU core. The blit is one of the most expensive things we do.

So every millisecond of latency we claw back is clawed back on a small, slow, lonely
core doing everything itself. That is the fun of it.

## The problem: a game is late before the screen is

Press a button and the reaction does not appear instantly, even on a real console. The
game samples the pad at the top of a frame, runs its logic, then renders the reaction
one to three frames later. That "internal lag" is baked into the game, not the display.

This is the dominant term on real GBA hardware: the console's own polling and display
path adds very little, so what you feel is mostly the game thinking. RetroRGB's
1000 fps-camera lag tests across every GBA handheld bear this out, an original GBA/GBA SP
sits at the floor and even the Analogue Pocket's FPGA only matches it, adding essentially
nothing ([RetroRGB][retrorgb-handheld]). A frame at the GBA's 59.7275 Hz is 16.74 ms, so
that one-to-three-frame internal lag is roughly 17 to 50 ms that no faithful reproduction
can touch.

An emulator cannot make the game's code think faster. But it *can* do something the real
console cannot: run the game slightly into its own future and show you that future now.

## Run-ahead in one paragraph

Every displayed frame, run the real frame with audio on, snapshot the machine, then run
N more frames with the same buttons held and audio muted, show that Nth "ahead" frame,
and roll the machine back to the snapshot. The picture on screen is always N frames
ahead of the committed timeline, so the game's internal lag is hidden by up to N frames,
every frame. This is exactly the technique RetroArch shipped in 2018, where it was shown
to reach latency *below original hardware* ([libretro][libretro-runahead],
[announcement][libretro-medium]).

The catch is cost: N+1 emulations per displayed frame, every frame, on our one small
core.

## Making it work on a threaded, OS-less core

gpSP (our GBA core, with an ARM dynarec) runs its CPU on one thread that yields at
vblank, while the driver thread kicks it once per frame. That split is where the
interesting bugs live.

- **Re-entry after a rewind.** The savestate is 512 KB of *machine* state (a
  deterministic memcpy), not the CPU thread's C stack or the dynarec's host registers.
  After a state load, the CPU thread must not resume its parked, now-stale translated
  block. It re-enters the recompiler from the top at the loaded PC via a
  `setjmp`/`longjmp` (`s_cpu_restart_req` + `s_cpu_jb`), the same path the intro-to-game
  reset already used.
- **Muting the ahead frames.** The look-ahead frames must not reach the speakers. A
  flag (`g_gba_audio_suppress`) drops their samples at the sink while still draining the
  core's audio ring so it does not desync. We do *not* latch the global audio pause,
  which would mute everything.
- **The sound ring the savestate forgot.** This one bit us. The 512 KB savestate stores
  the sound ring's read/write *indices* but not its 64K-sample *contents*. The muted
  ahead frames drain and mix into that buffer, so restoring only the indices left the
  committed audio garbled. The fix snapshots the whole ring alongside the state and
  restores it on rewind, so the audio is byte-identical to the committed timeline across
  every rewind.
- **A light flush.** A rewind restores a state from the same ROM this session, so the
  ROM and BIOS translation caches are still valid. We flush only the RAM translations,
  which roughly halves the committed-frame cost after a rewind.

### The 0.34% that would not go away

Run-ahead first ran the game **0.34% fast**. The threaded longjmp re-entry lands right
on the `vcount==228` boundary and yields again after a tiny ~960-cycle "priming stub,"
and those cycles were additive with each committed frame: 280896 (one exact GBA frame)
became 281856. That is past the audio clock-recovery trim, so it would drift pitch.

The fix (`gba_frame_boundary_finish`) runs the vblank tail that the mid-wrap re-entry
skips, so the re-entered frame starts a clean `vcount=0` boundary and advances a full
frame with no stub. Measured over a 60-frame window, the committed timeline now advances
exactly 59 x 280896 cycles. True 1x, no audio-clamp workaround.

## Keeping it from ever backfiring

A fixed look-ahead depth is dangerous on one slow core: a heavy scene overruns the 16.7
ms budget, and because the loop masters audio, a sustained overrun starves the audio
ring into silence. So the menu tier is a *maximum desired* depth, and two controllers
keep it honest:

- **A predictive closed loop** (`preempt_adapt`) measures the real per-frame busy time
  and only steps the depth up when one more frame still fits the budget. Heavy scenes
  fall toward zero (plain 60 fps with solid audio), light scenes get the full setting.
  It never has to overshoot and eat an audio hitch to learn its limit.
- **Per-tier clock escalation.** Because the extra emulation is not free on one core, the
  deeper tiers spend a little more clock, chosen from the same OPP grid as the manual CPU
  menu: Off 600, Balanced 1000, Responsive 1200, Max 1400 MHz (the quantized PLL reads
  those back as 599 / 999 / 1199 / 1399). Higher clock costs power and heat, so it scales
  with the tier you pick instead of running maxed by default. In a heavy scene that only
  afforded depth 0 at 600 MHz, Balanced reached depth 1, Responsive depth 2, and Max
  depth 3, all holding a locked 60 fps.

We also built true preemptive frames (rewind only on an input change, so static frames
cost 1x) and reverted it: on hardware it popped the audio on every input edge (it
re-simulates the committed timeline) and juddered the pacing (variable per-frame cost).
Run-ahead's continuous audio and constant cost simply feel better. The preemptive code
is in git history if the tradeoff is ever worth revisiting.

## Does the rewind actually restore everything?

We are blind to the panel from the build machine, so correctness cannot be eyeballed. It
has to be proven. There is an on-device self-test (`oem selftest:N`) that runs two
N-frame trajectories from the same starting state: one clean, one where every single
frame does the full run-ahead dance (save, run muted ahead frames, rewind), and then
compares them. Both legs re-enter each frame through the same path, so the only variable
is whether the ahead frames left any residue.

It hashes three things separately: the 512 KB machine state, the 128 KB sound ring, and
the rendered framebuffer. Across N up to 600 frames of continuous rewinding, the
**machine state and the sound ring come back byte-identical every time**. The game
simulation and the audio are perfectly deterministic across any number of rewinds, which
is exactly the property that the sound-ring bug had been quietly violating.

The rendered screen can differ at large N by a one-frame transient in gpSP's per-frame
render scratch (rebuilt each frame, not part of the savestate). It does not accumulate
(the state is identical, so nothing carries forward), it is game-content dependent
rather than monotonic in N, and it self-corrects the next frame. In real run-ahead the
frame you see is the look-ahead frame and the screen refreshes continuously, so a
single-frame render transient is imperceptible. The test reports it honestly as
`PASS(scr-transient)` rather than pretending it did not happen, and only calls a run a
real FAIL if the state or the audio ring diverge.

## The payoff, in frames

Latency is cleanest counted in frames; the millisecond conversions below are frame-model
estimates from our own measured pipeline, not lab captures (to nail absolutes you need a
high-speed camera or a photodiode on the panel, the methodology WydD documents at
[inputlag.science][inputlag] and the LED-and-1000fps-camera approach RetroRGB used for
GBA handhelds [here][retrorgb-handheld]).

Our fixed pipeline is about four frames: half a frame of polling, two frames of input
debounce (a deliberate reliability choice on this unit, which had phantom inputs slip
through a lighter filter), about a frame of present and scanout, and half a frame of LCD.
On top of that sits the part run-ahead attacks: the game's own 1-3 frame internal lag.

| Preemptive Frames | Run-ahead depth | Frames to photon (fixed + game) |
|-------------------|-----------------|---------------------------------|
| Off               | 0               | ~4 + 2-3                        |
| Balanced          | 1               | ~4 + 1-2                        |
| Responsive        | 2               | ~4 + 0-1                        |
| Max               | 3               | ~4 + 0                          |

Each tier removes one whole frame of the game's internal lag (16.74 ms), bounded by how
much lag the game actually has. That per-tier delta is the rigorous, defensible claim.

## Where this lands against real hardware and FPGA

Two references matter to people who care about this: original GBA hardware, and the
Analogue Pocket, which reimplements the console on an FPGA
([Analogue Pocket][pocket-wiki]).

- **Frame timing.** We drive the panel at the GBA's own 59.7275 Hz (measured 59.727 Hz),
  not a rounded 60.0 Hz, so there is no pull-down cadence, no duplicated or dropped
  frames, and audio pitch is exactly right. The Pocket reaches the same goal from the
  other direction: it ships a variable-refresh display and a per-core "GBA VRR" mode that
  slews the panel to the core's native rate, exactly to kill the 60.0 Hz-vs-59.7 Hz
  micro-stutter (write-up of the Pocket's GBA VRR firmware feature [here][gbatemp-vrr]).
  Most 60.0 Hz software emulators do neither.
- **Accuracy.** Real hardware and the Pocket's FPGA core are cycle-accurate. gpSP is a
  dynamic recompiler: extremely compatible, but not cycle-perfect on a handful of games
  with unusual timing tricks. This is the one axis where the two references have the edge,
  and we do not claim otherwise.
- **Input latency.** This is the interesting one. Real hardware and a cycle-accurate FPGA
  faithfully *reproduce* the game's own 1-3 frames of internal lag; they cannot remove it
  without breaking accuracy. RetroRGB tested lag and ghosting across every GBA handheld
  with an LED and a 1000 fps camera, and found the Analogue Pocket's FPGA has **virtually
  zero added lag** over an original GBA, with the original GBA/GBA SP at the low end and
  emulation-based handhelds like the DS/3DS adding roughly a frame on top
  ([RetroRGB, "Comparing Lag and Ghosting for Every GBA handheld"][retrorgb-handheld]; the
  crowd-sourced hardware-lag-tester database is [here][retrorgb-lagdb]). So the Pocket sits
  right at original-hardware latency: it reproduces the game's internal lag exactly. That
  is the bar. Run-ahead is the one trick that *removes* that internal lag instead of
  reproducing it, which is how RetroArch demonstrated latency below original hardware
  ([libretro][libretro-medium]).

So the honest positioning: with Preemptive Frames **Off** we are in the same class as
real hardware and the Pocket, minus cycle-perfect accuracy, plus our two-frame debounce.
Turn it **on**, and for a game with real internal lag we hand back frames that neither
the original console nor a faithful FPGA can, on a single small core at minimal clocks
with no GPU. That last part is the point we are proudest of.

## Configuration and diagnostics

- Menu item **Preemptive Frames**: Off / Balanced / Responsive / Max, persisted. The
  value shows the live adaptive depth, e.g. "Max (3)".
- `oem preempt:<0..3>` sets the tier live over fastboot for measurement.
- `oem selftest[:N]` runs the determinism self-test (reports `st=/rng=/scr=`).
- `oem diag` reports `hz1000=` (panel Hz x1000), `em=` (avg committed-frame us), `ft=`
  (committed GBA cycles, 280896 = exact 1x), `epf=` (live adaptive depth), `mhz=` (CPU
  clock), `afr=` (cumulative audio frames submitted; climbing means audio is flowing).

## For the next person in this code

- `emu/gba/gba_driver.c` - `preempt_present()` (the run-ahead loop),
  `preempt_effective_depth`/`preempt_adapt` (adaptive depth), `preempt_apply_cpu` (per-tier
  clock off the shared `s_cpu_opp` grid, indices `{0,2,3,4}`), `gba_run_ahead_selftest`.
- `emu/gba/gba_wrap.c` - `g_gba_audio_suppress` gate; `gba_core_cpu_ticks()`.
- `emu/gba/sound.c` - `gba_sound_ring_save/load` (the ring the savestate omits).
- `emu/gba/gba_memory.c` - `g_gba_load_light` (RAM-only flush on rewind).
- `emu/gba/main.c` - `gba_frame_boundary_finish()` (the exact-1x re-entry tail).
- `platform/mt6785/mt_disp_drv.c` - the no-filter CPU blit fast path.
- `platform/mt6785/pll.c` - `ayaneo_set/get_cpu_mhz` (the OPP grid).
- `emu/gba/menu_fastboot.c` - `oem preempt:` / `oem selftest` / `oem diag`.

## Sources

- RetroRGB, *Comparing Lag and Ghosting for Every GBA handheld* (GBA-specific: LED +
  1000 fps camera; Analogue Pocket FPGA = virtually zero lag over original GBA, DS/3DS
  add ~1 frame): [retrorgb-handheld]
- RetroRGB, crowd-sourced hardware-lag-tester database (Time Sleuth / Leo Bodnar):
  [retrorgb-lagdb]
- Analogue Pocket GBA variable-refresh (VRR) display timing feature: [gbatemp-vrr]
- Analogue Pocket, FPGA background: [pocket-wiki]
- libretro, *RetroArch 1.7.2: better latency than original hardware through run-ahead*:
  [libretro-medium]
- libretro Run-Ahead guide [libretro-runahead] and Latency guide [libretro-latency]
- WydD / inputlag.science, latency measurement methodology: [inputlag]

[retrorgb-handheld]: https://retrorgb.com/comparing-lag-and-ghosting-for-every-gba-handheld.html
[retrorgb-lagdb]: https://retrorgb.com/lagtest.html
[gbatemp-vrr]: https://gbatemp.net/threads/what-is-variable-refresh-rate-for-gba-feature-of-analogue-pocket.673346/
[pocket-wiki]: https://en.wikipedia.org/wiki/Analogue_Pocket
[libretro-runahead]: https://docs.libretro.com/guides/runahead/
[libretro-latency]: https://docs.libretro.com/guides/latency/
[libretro-medium]: https://medium.com/@libretro/retroarch-1-7-2-achieving-better-latency-than-original-hardware-through-new-runahead-method-1b80d26bb5d1
[inputlag]: https://inputlag.science/controller/methodology
