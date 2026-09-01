# Beating input lag on a bare-metal handheld: Preemptive Frames for GBA

This is the story of how I clawed back the input lag baked into Game Boy Advance games,
the 1-3 frames the game itself spends before it reacts, running on a phone-grade chip in a
bootloader, with no operating system, no GPU, and a single CPU core clocked as low as I
could get away with. The feature is called **Preemptive Frames** in the menu. Under the
hood it is run-ahead, and getting it to run at exactly the right speed, with clean audio,
on this hardware took a few fights worth writing down.

## The hardware I am working with

It helps to be honest about the machine first, because the constraints shape everything:

- **One small core.** This runs inside LK (Little Kernel, the bootloader) on a
  MediaTek MT6785. There is no SMP here: the emulator's CPU thread and the frontend that
  draws and presents frames share a **single ARM core**. A look-ahead frame is not
  offloaded anywhere. It is pure extra serial work inside the same 16.7 ms budget.
- **Minimal clocks by default.** That core sits at the **lowest operating point, 600
  MHz**, out of the box, for battery and heat. I only raise it when the player asks for
  a deeper setting.
- **No GPU.** There is no 3D block, no compositor, no hardware scaler in the path. The
  240x160 GBA frame is upscaled to the panel and pushed to the framebuffer entirely in
  software on that same CPU core. The blit is one of the most expensive things I do.

So every millisecond of latency I claw back is clawed back on a small, slow, lonely
core doing everything itself. That is the fun of it.

## Close to the metal: no OS in the way

Part of why this is even possible on a 600 MHz core is that there is almost nothing
between the emulator and the hardware. This runs in LK, the bootloader, not under an
operating system. It is worth spelling out what that removes, because it is the whole
reason a run-ahead win is visible here instead of drowned in stack latency. Follow a
button press and a frame through each environment:

- **Input.** Here, I read the keypad/GPIO registers directly, a single MMIO read, and
  hand the bits straight to the core on the same frame. On Linux that press travels
  through a keypad driver, the input subsystem (evdev), a wakeup and a context switch into
  userspace; on Android it goes further still, through the input HAL,
  InputReader/InputDispatcher, and the app's own event loop, each one a queue and a
  scheduler boundary.
- **Frames.** Here, the core renders into a buffer and I blit it straight to the MIPI
  framebuffer and kick the panel myself, one present per vblank, and nothing else ever
  touches those pixels. On Linux you hand a buffer to DRM/KMS and wait for an atomic
  commit; on Android SurfaceFlinger composites your surface together with everything else
  on screen and double- or triple-buffers the result, which by itself is one to two frames
  of latency before the panel even sees your image.
- **Scheduling.** Here, my two threads (the emulated CPU and the frontend) are the only
  things running; nothing preempts them. Under a general-purpose OS the scheduler
  time-slices you against dozens of other processes, and without a real-time kernel a
  frame can miss its deadline simply because something unrelated got the core.

None of this makes Linux or Android bad, they are doing enormously more than I am. But
every subsystem they add is a queue, a copy, or a context switch, and those cost time.
Even a lean, well-tuned Linux still passes every frame through a driver and a compositor,
and every input through the kernel and userspace. Bare-metal is how a slow single core
can keep the entire path down to just the emulation, one debounce, one blit, and one panel
refresh, with no invisible frames of buffering hiding in a stack I do not control.

## The problem: a game is late before the screen is

Press a button and the reaction does not appear instantly, even on a real console. The
game samples the pad at the top of a frame, runs its logic, then renders the reaction
one to three frames later. That "internal lag" is baked into the game, not the display.

How much is it, concretely? Tito (with Bob of RetroRGB's LED-and-1000fps-camera rig, six
samples per unit) measured a completely stock, unmodified Game Boy Advance at an average
of **2.4 frames** button-to-screen, and noted the GBA's own transflective LCD accounts
for about a frame of that ([Lag Testing Every GBA Consolizer][machonacho]). So on real
hardware you are looking at roughly 1.4 frames of the game and the console thinking, plus
about a frame of that famously slow original screen. A frame at the GBA's 59.7275 Hz is
16.74 ms, so a stock GBA is around 40 ms, and the internal part run-ahead can attack is
the ~1.4 frames underneath the LCD.

An emulator cannot make the game's code think faster. But it *can* do something the real
console cannot: run the game slightly into its own future and show you that future now.

## Two ways to do it, and why I shipped run-ahead

There are two ways to hide the game's internal lag, and I built both before choosing.

**Run-ahead (what I shipped).** Every displayed frame: run the real frame with audio on,
snapshot the machine, run N more frames with the same buttons held and audio muted, show
that Nth "ahead" frame, then roll the machine back to the snapshot. The picture is always
N frames ahead of the committed/audio timeline, so the game's internal lag is hidden by up
to N frames, every frame. Cost: N+1 emulations per displayed frame, every frame. This is
the technique RetroArch shipped in 2018, where it reached latency *below original hardware*
([libretro][libretro-runahead], [announcement][libretro-medium]).

**True preemptive (built, then reverted).** Keep a ring of recent committed states and only
re-simulate when the input actually *changes*: on an input edge, rewind N frames and replay
them plus the current frame with the new button held. Static frames (the majority) cost a
single 1x emulation, and only edges pay the N+1. On paper it is cheaper and it measured at
exact 1x speed with no priming stub.

I shipped run-ahead anyway. On hardware, true preemptive **popped the audio on every input
edge**, because it re-simulates the committed timeline and the game's audio trajectory
jumps at each edge, and it **juddered the frame pacing**, because its per-frame cost is
variable (heavy on edge frames, light on static ones). Run-ahead has neither problem: it
never touches the committed timeline, so the audio stays continuous and click-free, and its
cost is the same every frame, so pacing is smooth. On a single core mastering its own audio
ring, a steady, slightly higher constant cost beat a spiky average every time. The menu
still carries the name "Preemptive Frames," but the mechanism underneath is run-ahead, and
the preemptive code is preserved in git history if the tradeoff is ever worth revisiting
(say, with a second core to hide the replay).

## Making it work on a threaded, OS-less core

gpSP (my GBA core, with an ARM dynarec) runs its CPU on one thread that yields at
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
  core's audio ring so it does not desync. I do *not* latch the global audio pause,
  which would mute everything.
- **The sound ring the savestate forgot.** This one bit me. The 512 KB savestate stores
  the sound ring's read/write *indices* but not its 64K-sample *contents*. The muted
  ahead frames drain and mix into that buffer, so restoring only the indices left the
  committed audio garbled. The fix snapshots the whole ring alongside the state and
  restores it on rewind, so the audio is byte-identical to the committed timeline across
  every rewind.
- **A light flush.** A rewind restores a state from the same ROM this session, so the
  ROM and BIOS translation caches are still valid. I flush only the RAM translations,
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

## Does the rewind actually restore everything?

I am blind to the panel from the build machine, so correctness cannot be eyeballed. It
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

Latency is cleanest counted in frames; the millisecond conversions are frame-model
estimates from my own measured pipeline, not lab captures (to nail absolutes you need a
high-speed camera or a photodiode on the panel, the methodology WydD documents at
[inputlag.science][inputlag] and the LED-and-1000fps-camera approach RetroRGB and Tito
used [here][machonacho]).

My fixed pipeline is about four frames: half a frame of polling, **two frames of input
debounce** (a three-read agreement filter, so a fresh press has to be seen on three
consecutive frames before it counts, which delays it by two frames; a deliberate
reliability choice on this unit, which let phantom inputs slip through a lighter filter),
about a frame of present and scanout on a software framebuffer, and half a frame of LCD.
On top of that sits the part run-ahead attacks: the game's own 1-3 frame internal lag.
Each tier removes one whole frame of that internal lag (16.74 ms), bounded by how much lag
the game actually has. **That per-tier delta is the rigorous, defensible claim.**

## GBA vs FPGA vs this build

Here is everything side by side. Measured rows are button-to-photon from high-speed-camera
or 240fps-app tests; my rows are the frame-model estimate above. One frame is 16.74 ms.

| System                          | Button-to-photon         | How it gets there                                   |
|---------------------------------|--------------------------|-----------------------------------------------------|
| Real GBA (AGB-001), measured    | ~2.4 frames / ~40 ms     | game's 1-3 fr internal lag + ~1 fr slow AGB LCD      |
| Analogue Pocket, GBA, measured  | ~1 frame *more* than a real GBA (e.g. ~50 ms vs ~30 ms, same game) | faithful FPGA core, but its rotated screen must buffer a frame to scale/draw GBA |
| Best HDMI consolizer, measured  | ~1.6 frames              | original GBA silicon, fast modern panel, no scaler buffer |
| **This build, Off**             | ~6-7 frames (est)        | ~4 fixed (0.5 poll + 2 debounce + 1 present + 0.5 LCD) + full 2-3 fr game lag |
| **This build, Balanced (1)**    | ~5-6 frames (est)        | run-ahead removes 1 game frame                       |
| **This build, Responsive (2)**  | ~4-5 frames (est)        | run-ahead removes 2 game frames                      |
| **This build, Max (3)**         | ~4 frames (est)          | game's internal lag fully removed; only the fixed pipeline remains |
| **This build, Max, 1-frame debounce** | ~3 frames (est)   | *hypothetical*: relax the glitch filter to a 2-read agreement |
| **This build, Max, no debounce** | ~2 frames (est)         | *hypothetical*: raw input, would match/beat a stock GBA and the Pocket |
| Android emulator, RetroArch     | ~92 ms in one favorable reading, typically higher | always-triple-buffered SurfaceFlinger + input stack + emulator buffering; only nears hardware with run-ahead tuning |

A word on that Android row, because the number flatters Android. The ~92 ms is one
person's favorable reading (a Retroid, a Game Boy game, default RetroArch). It is a floor,
not a typical figure: Android apps are *always* triple-buffered and the graphics stack is
built "for throughput rather than fast reaction," drawing frames as fast as possible and
processing them in queue order ([Android systrace docs][android-systrace]). SurfaceFlinger
compositing plus that triple buffering alone is often two-plus frames before the input
stack and the emulator's own buffering are even counted, so untuned Android emulation
usually lands well above 92 ms, and RetroArch only approaches hardware once you turn on
run-ahead and hand-tune the latency settings. I left the favorable number in the table on
purpose, not to flatter myself: at ~92 ms it is in the same ballpark as my *worst* tier
(Off, ~100-117 ms), Android's typical untuned case is worse still, and my run-ahead tiers
pull below it. If I had cited Android's typical figure instead, the gap would look larger
than it honestly is.

**The single thing standing between me and beating real hardware is the debounce.** At
Max the game's internal lag is already gone, so those ~4 fixed frames are 2 debounce +
0.5 poll + 1 present + 0.5 LCD. The debounce is the biggest slice, and it is the only slice
I could still cut: drop the three-read agreement to two reads and Max lands at ~3 frames,
drop it entirely and Max lands at ~2 frames, which would actually undercut a stock GBA's
2.4 and the Pocket. **But the filter is there for a reason, and it stays.** This unit's
GPIO input lines glitch, and with a lighter filter phantom presses slipped through during
real gameplay, inputs the player never made. A game that occasionally jumps on its own is
worse than one that reacts a frame later, so the three-read agreement is a deliberate
reliability-over-latency trade. Those two hypothetical rows are not shipping numbers; they
are there to show exactly where the remaining latency lives and why I am choosing to keep
it.

Read the rest honestly too. In absolute button-to-photon I do **not** beat a stock GBA or
the Pocket: my fixed pipeline is heavier, dominated by that two-frame debounce and the
software framebuffer flip. What the table shows is the *shape* of the win. Run-ahead is the
one lever that removes the game's own internal lag, which is the single biggest chunk
software
can touch, so each tier walks me down a frame at a time from "worse than an Android
emulator" toward the consolizer range, on a single 600 MHz core with no GPU.

## Where this lands against real hardware and FPGA

Two references matter to people who care about this: original GBA hardware, and the
Analogue Pocket, which reimplements the console on an FPGA ([Analogue Pocket][pocket-wiki]).

- **Frame timing.** I drive the panel at the GBA's own 59.7275 Hz (measured 59.727 Hz),
  not a rounded 60.0 Hz, so there is no pull-down cadence, no duplicated or dropped
  frames, and audio pitch is exactly right. The Pocket reaches the same goal from the
  other direction: it ships a variable-refresh display and a per-core "GBA VRR" mode that
  slews the panel to the core's native rate, to kill the 60.0 Hz-vs-59.7 Hz micro-stutter
  ([write-up of the Pocket's GBA VRR feature][gbatemp-vrr]). Most 60.0 Hz software
  emulators do neither.
- **Accuracy.** Real hardware and the Pocket's FPGA core are cycle-accurate. gpSP is a
  dynamic recompiler: extremely compatible, but not cycle-perfect on a handful of games
  with unusual timing tricks. This is the one axis where the two references have the edge,
  and I do not claim otherwise.
- **Input latency.** Here is the wrinkle I did not expect. A cycle-accurate FPGA faithfully
  *reproduces* the game's own internal lag, and cannot remove it without breaking accuracy.
  But the Analogue Pocket, the gold-standard FPGA, actually *adds* about a frame for GBA
  specifically: its display is effectively rotated and scans right-to-left, so a GBA frame
  has to be drawn into a framebuffer before it can go to the panel (the visible right-to-
  left "jelly scroll"). Measured with a 240 fps app, a GBA game that is ~30 ms on a real
  AGB-001 comes out ~50 ms on the Pocket, a full 60 Hz frame slower, consistently
  ([r/AnaloguePocket][pocket-lag-reddit]). Palmer Luckey's own high-speed comparison put
  the Pocket "more than a frame behind" a Game Boy Color and the ModRetro Chromatic, and
  RetroRGB's Bob traced it to exactly that rotated-screen framebuffer
  ([RetroRGB][retrorgb-chromatic]). For GB/GBC, where the Pocket scales natively, it is
  much closer to original.

The useful takeaway for me: even the best FPGA in the world pays about a framebuffer frame
to put GBA on a modern scaled panel, which is the same cost my software present pays. What
I can do that neither the original console nor a faithful FPGA will is *remove the game's
own internal lag* with run-ahead. In absolute terms my two-frame debounce still keeps me
behind them, but the framebuffer frame is a cost everyone in this space pays, and run-ahead
is the equalizer that claws back the rest. That is the honest version.

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

- Tito, *Lag Testing Every GBA Consolizer* (Bob of RetroRGB's LED + 1000 fps camera method,
  6 samples each: stock GBA 2.4 frames, best consolizers 1.6, worst 6.2; the GBA LCD alone
  adds ~1 frame): [machonacho]
- r/AnaloguePocket, *Analogue Pocket adds input lag* (240 fps-app measurements: a GBA game
  ~30 ms on a real AGB-001 vs ~50 ms on the Pocket, ~1 frame more, from the rotated-screen
  framebuffer; also a Retroid Android RetroArch reading of ~92 ms): [pocket-lag-reddit]
- RetroRGB (Bob), *Modretro Chromatic - In Stock & Lag Tested* (Palmer Luckey's high-speed
  comparison: Chromatic matches original GBA and is faster than the Pocket, which is "more
  than a frame behind"; Bob attributes the Pocket's extra frame to its rotated screen
  needing a framebuffer): [retrorgb-chromatic]
- RetroRGB, crowd-sourced hardware-lag-tester database (Time Sleuth / Leo Bodnar):
  [retrorgb-lagdb]
- Analogue Pocket GBA variable-refresh (VRR) display timing feature: [gbatemp-vrr]
- Analogue Pocket, FPGA background: [pocket-wiki]
- libretro, *RetroArch 1.7.2: better latency than original hardware through run-ahead*:
  [libretro-medium]
- libretro Run-Ahead guide [libretro-runahead] and Latency guide [libretro-latency]
- WydD / inputlag.science, latency measurement methodology: [inputlag]

[machonacho]: https://www.youtube.com/watch?v=TDxjd5d2Q8E
[pocket-lag-reddit]: https://www.reddit.com/r/AnaloguePocket/comments/tqtn91/analogue_pocket_adds_input_lag/
[retrorgb-chromatic]: https://retrorgb.com/modretro-chromatic-in-stock-lag-tested.html
[retrorgb-lagdb]: https://retrorgb.com/lagtest.html
[gbatemp-vrr]: https://gbatemp.net/threads/what-is-variable-refresh-rate-for-gba-feature-of-analogue-pocket.673346/
[pocket-wiki]: https://en.wikipedia.org/wiki/Analogue_Pocket
[android-systrace]: https://source.android.com/docs/core/tests/debug/systrace
[libretro-runahead]: https://docs.libretro.com/guides/runahead/
[libretro-latency]: https://docs.libretro.com/guides/latency/
[libretro-medium]: https://medium.com/@libretro/retroarch-1-7-2-achieving-better-latency-than-original-hardware-through-new-runahead-method-1b80d26bb5d1
[inputlag]: https://inputlag.science/controller/methodology
