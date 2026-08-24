# In-bootloader Game Boy Advance emulation: implementation notes

This documents how a Game Boy Advance emulator ([gpSP](https://github.com/libretro/gpsp))
runs inside LK, the MediaTek Little Kernel bootloader, on the AYANEO Pocket Air
Mini (Helio G95 / MT6785). After the boot animation, LK runs the emulator instead
of loading a kernel; there is no OS, no userspace, and no return to Android.

It builds on the earlier Game Boy Color work (gambatte, branch `lk-gbc-emu`). The
two share a large harness: the display path, the audio stack, the input handling,
the settings/overlay menu, save-state and cartridge-save persistence, the battery
gauge, the charge LED and the offline-charging screen were all built for the GBC
port and are reused here. Those shared subsystems are documented in full below,
since the GBC tree never wrote them up. Everything specific to the GBA (the dynamic
recompiler, the ROM size, the frame timing) is called out as such.

Build gate: `AYANEO_GBA` (mutually exclusive with `AYANEO_GBC`). The two selections
choose which emulator driver compiles and which core archive links; the rest of the
harness compiles either way.

---

## 1. Build and integration

### Core archive

gpSP is vendored under `emu/gba/` and compiled into a static archive
`emu/gba/libgpsp.a` by `emu/gba/build_core_gba.sh`, using the bare-metal
`arm-none-eabi-gcc` toolchain (which ships newlib/libgcc). It is ABI-matched to
LK's 32-bit build:

```
-march=armv7-a -marm -mfloat-abi=soft -mno-unaligned-access
-ffreestanding -fno-builtin -fno-common -fno-short-enums -O2
-DARM_ARCH -DHAVE_DYNAREC -DHAVE_MMAP -DGBA_LK_SMALL_CACHE
```

`-DHAVE_DYNAREC` selects the ARM dynamic recompiler (`cpu_threaded.c`,
`arm/arm_stub.S`). `-DHAVE_MMAP` makes the translation caches plain pointer globals
rather than large static arrays (see memory layout). `-DGBA_LK_SMALL_CACHE` selects
the reduced cache sizes added in `cpu.h`. `-mno-unaligned-access` keeps the
compiler from emitting unaligned accesses; `-fno-short-enums` matches LK's 32-bit
enum ABI.

Three glue translation units are compiled into the same archive:

- `gba_shim.c`: freestanding runtime fills. `__clear_cache` (dynarec cache
  maintenance), `strtol`/`strcasecmp`, `time`/`localtime` (the GBA RTC, e.g.
  Pokemon), and stdio/time stubs. LK already provides `malloc`/`free`/`memcpy`/
  `printf`/most `str*`, so those are not redefined.
- `gba_wrap.c`: the bridge that drives the core directly (no libretro front-end,
  no libco). It owns the DRAM arena layout, defines the globals the removed
  `libretro.c` used to provide (`dynarec_enable`, `idle_loop_target_pc`, etc.),
  and supplies the input and audio callbacks.
- `gba_driver.c`: the LK-side driver (compiled with LK's toolchain, not into the
  archive). Bring-up, the run loop, input, the overlay menu, persistence and the
  charging screen.

`emu/gba/gba_memory.c` has small in-repo additions (`load_gamepak_mem`,
`load_bios_mem`, `init_gamepak_buffer_ext`) that load the ROM and BIOS from memory
instead of a filesystem while keeping gpSP's per-game configuration and backup
detection.

### LK linking

- `project/k85v1_64.mk`: sets `GBA_LIB` and defines `AYANEO_GBA` when
  `AYANEO_GBA=yes`.
- `make/build.mk`: adds `$(GBA_LIB)` to the final link (before `LIBGCC`, so the
  soft-float `__aeabi_*` helpers resolve).
- `platform/mt6785/rules.mk`: compiles `emu/gba/gba_driver.o` for the GBA build
  (else `emu/gbc/gbc_driver.o`).

The image is signed with `tools/ayaneo/sign_lk.py` and flashed to the `lk_a`
partition (2 MB). `build_ayaneo_gba.sh` builds the archive, the LK image and the
`boot_b` payload in one step.

### LK hook points

- `app/mt_boot/mt_boot.c`: after the boot animation, calls `ayaneo_gbc_start()`
  (the shared entry) instead of booting Linux.
- `platform/mt6785/platform.c`: skips `lk_vb_vfy_logo()` (its up-to-16 MB malloc
  panics against LK's small heap and is unnecessary here), and on a charger-insert
  power-on runs `ayaneo_gbc_charging_screen()` at platform init before falling
  through to a normal boot.

---

## 2. Memory layout

### LK RAM window

gpSP adds roughly 1.6 MB of static BSS (emulated EWRAM, the dynarec block-lookup
tables, sound buffers, the cartridge backup). This pushes LK's `_end` past the
linker's `_end_of_ram = MEMBASE + MEMSIZE`. With the stock `MEMSIZE` of 4 MB the
heap length (`_heap_end - _end`) goes negative and the BSS above the window is
outside the MMU's RAM mapping, so LK faults during early init. `MEMSIZE` is raised
to 9 MB in `target/k85v1_64/rules.mk`; the usable region runs up to TrustZone/BL31
at `0x4CE00000`, leaving headroom.

### DRAM arena

A 64 MB arena at physical `0x50000000` (inside the 128 MB download/scratch region,
identity-mapped, cacheable, executable). The wrap layer gets the low 62 MB; the
driver keeps the top 2 MB for save-state/`.sav` scratch. It holds:

- the 32 MB gamepak ROM buffer (`gamepak_ram_buffer_size`), so a full-size ROM is
  resident and gpSP never enters its file-paging path;
- the gamepak page map;
- the 240x160 RGB565 screen buffer;
- a scratch region above the ROM buffer used to stage the compressed ROM during
  decompression.

### Translation cache placement (GBA-specific)

gpSP's recompiler calls its C helper functions (`execute_load_u32`,
`arm_update_gba`, etc.) from emitted code with a direct ARM `bl`, whose signed
24-bit immediate reaches +-32 MB. Those helpers are in LK `.text` around `0x4C48xxxx`.
A translation cache placed in the far arena (`0x52xxxxxx`, about 96 MB away) puts every
such `bl` out of range; it overflows and branches to a wrong address.

The caches are therefore placed at `0x4E000000` (the base of the scratch region,
the nearest free executable DRAM to the helpers, about 27 MB away) and kept small so the
far end stays in range: ROM cache 2.5 MB, RAM cache 512 KB, BIOS cache 256 KB
(3.25 MB total, `GBA_LK_SMALL_CACHE` in `cpu.h`). The 32 MB ROM buffer stays in the
far arena; it is only read via `ldr`, so its distance is irrelevant.

The small ROM code cache means a large game occasionally flushes and re-translates
(a brief hitch). The general fix would be to emit indirect calls (`movw`/`movt` +
`blx`) so the cache can be large and anywhere; that is a change to the code
generator and is not done.

### boot_b partition layout

The `boot_b` partition (32 MB) holds runtime assets and persistent data:

| Offset       | Contents                                              |
|--------------|-------------------------------------------------------|
| `0x00000000` | Boot animation blob (shared with GBC)                 |
| `0x01000000` | Boot chime audio blob (shared)                        |
| `0x01100000` | Compressed ROM: `["GBAR"][u32 rawlen][u32 complen][raw deflate]` |
| `0x01C00000` | Save state                                            |
| `0x01D00000` | Cartridge save (`.sav`, the 128 KB `gamepak_backup`)  |
| `0x01E00000` | Settings block (shared)                               |

`tools/ayaneo/gba/build_boot_b_gba.py` assembles the image.

---

## 3. Driving the core

### Threading

gpSP has no "run one frame" entry point: the CPU runs continuously and yields to
the front-end at each vblank via `switch_to_main_thread()`. This is reproduced with
two LK auto-unsignal events and a dedicated CPU thread:

- The CPU thread runs `execute_arm_translate()` (the dynarec loop), which never
  returns.
- The front-end run loop signals the CPU event and waits on the main event to run
  one frame.
- `switch_to_main_thread()` (called by the core at vblank) is routed to
  `gba_yield_to_main()`, which signals the main event and waits on the CPU event.

Auto-unsignal events tolerate a signal arriving before the wait, so the strict
ping-pong is race-free even though the CPU thread runs at higher priority.

### Dynarec bring-up requirements

Running emitted code on bare-metal LK required, besides the cache placement above:

- **Alignment.** LK's `crt0.S` sets `SCTLR.A` (strict alignment faulting). The GBA
  permits unaligned word reads (they rotate) and gpSP performs the unaligned host
  load directly. `gba_driver.c` clears `SCTLR.A` on the emulator threads
  (`gba_disable_align_faults()`, three CP15 instructions; PL1/SVC so it is
  permitted).
- **Cache coherency.** Freshly emitted code is written through the data cache and
  fetched through the instruction cache. `__clear_cache` (in `gba_shim.c`) performs
  a range-scoped sequence over just the emitted bytes: clean D-line to PoU, `DSB`,
  invalidate I-line to PoU, invalidate the branch predictor, `DSB`, `ISB`. LK's
  `arch_sync_cache_range` is not used here because it invalidates the entire
  I-cache per call (pathological during the translate-heavy warmup) and omits the
  barriers.

The pure-C interpreter is still available with `AYANEO_GBA_INTERP=yes`
(`gba_driver.c` sets `dynarec_enable = 0`); it is a correctness reference.

### ROM and BIOS

The ROM is stored raw-deflate compressed in `boot_b` and inflated at boot with LK's
`zunzip` (`inflateInit2(-MAX_WBITS)`) into the arena ROM buffer. The compressed
stream is staged in the arena scratch above the ROM buffer, so the inflate output
never overruns the still-compressed input; ROMs up to the full 32 MB maximum load
(compressed size is capped at about 10 MB by the `boot_b` gap).

The BIOS is embedded as a C array (`gba_bios_data.h`) handed to `load_bios_mem`.
That header is copyrighted and not committed; `emu/gba/gen_bios_header.py`
regenerates it from a local `gba_bios.bin`.

---

## 4. Display

The panel is 1280x960, driven in DSI video mode (`st7703_hd720_dsi_vdo`).

### Scaling and format

`ayaneo_gbc_show_frame()` in `platform/mt6785/mt_disp_drv.c` takes the emulator's
240x160 RGB565 frame, scales it 5x with nearest-neighbour to 1200x800 (centred,
black borders), and converts RGB565 to the panel's BGRA8888 per pixel on the CPU
(there is no GPU). The GBC path uses the same function with 160x144 at 6x, selected
by build gate.

### Presentation and pacing

The framebuffer is double-buffered. `ayaneo_present()` submits the new scan-out
address via `primary_display_config_input()` and triggers. In video mode a bare
trigger does not push a new frame, so the two buffers are alternated to force the
overlay to re-latch. `primary_display_config_input()` blocks on the panel's
`FRAME_DONE` event, so presenting one frame per emulated frame paces the emulator to
the panel's vsync directly; no software frame timer is used. This is tear-free and
locked to the panel refresh.

### Panel retune (GBA-specific)

The emulator is vsync-locked, so the panel refresh determines the game speed. The
GBA runs at 59.7275 Hz; the stock panel runs at about 60.1 Hz. The vertical front
porch is stretched to slow the refresh: `vertical_frontporch` 16 -> 23
(`vtotal` 992 -> 999) under `#ifdef AYANEO_GBA` in the LCM driver, giving about 59.71 Hz.
Only the frame blanking changes; the DSI data rate and signal integrity are
untouched, and the change is gated so the GBC build is unaffected.

The naive DSI pixel-clock formula (`data_rate x lanes / bpp`) does not match this
panel's real pixel clock, so the timing is set from the measured refresh, not
calculated. `primary_display_get_vsync_interval()` returns the refresh as Hz x 100;
it is read once at startup, shown in the overlay menu ("Panel Refresh"), and used to
calibrate the audio (below). 59.71 Hz is 0.03% off the GBA rate, which is the
closest an integer scanline count reaches.

---

## 5. Audio

LK has no audio support. The entire path was brought up from bare registers for the
GBC boot chime and is reused here.

### Codec and AFE bring-up

`platform/mt6785/ayaneo_audio.c` brings up, from registers:

- the audio power domain (SPM MTCMOS);
- the AFE clocks and the DL1 downlink memif (a hardware ring DMA);
- the interconnect (DL1 -> ADDA downlink) and the ADDA sample-rate converter set
  to 48 kHz;
- the MT6359 codec analog path over the MTKAIF serial link: the 1.8 V audio LDO,
  DCXO, the DL LDO rails, the SDM/NCP, and the headphone output stage (the external
  class-D speaker amp is wired to the codec headphone outputs), with the documented
  power-up ramps to avoid pops. Two GPIOs enable the speaker amp.

The output format is fixed 48 kHz, stereo, 16-bit. Playback is a looping DMA ring
(`AFE_DL1`) the hardware plays continuously; software keeps it fed ahead of the read
pointer (`AFE_DL1_CUR`).

### Resampling and rate calibration (GBA-specific)

gpSP produces s16 stereo at 65536 Hz. `ayaneo_gba_audio_submit()` box-resamples that
to the 48 kHz ring. Because the emulator is vsync-locked to an about 59.71 Hz panel (not
exactly the GBA's 59.7275 Hz), the production rate is fixed but slightly off 48 kHz,
which would slowly drift the ring write cursor into the read cursor.

The correction is a fixed resample ratio calibrated once to the measured panel
refresh: `ayaneo_gba_audio_set_rate(panel_hz100)` sets the resampler increment to
`286692000 / panel_hz100` (that is `48000 * 59.7275 * 100 / panel_hz100`). Since the
DSI and audio clocks both derive from the same reference, a fixed calibrated ratio
holds the rate with negligible drift and, importantly, a constant pitch (a dynamic
feedback controller was tried first and produced an audible pitch wobble). A
wide-band safety check snaps the write cursor only if it approaches underrun or
overrun; it does not fire in normal playback.

The GBC path resamples its 2097152 Hz output to the same ring; the ring, the codec
bring-up, pause and shutdown are shared.

---

## 6. Input

The game buttons are plain active-low SoC GPIOs exposed through a device-tree
`gpio-keys` node, read with `mt_get_gpio_in` after configuring the pins as inputs
with pull-ups. State is sampled once per frame with a two-read debounce.

| GBA input     | GPIO | Notes                                  |
|---------------|------|----------------------------------------|
| Up/Down/Left/Right | 89/79/78/80 |                                 |
| A / B         | 83 / 82 | swapped vs the DTS labels to match layout |
| Start / Select| 91 / 90 | swapped; Select doubles as brightness modifier |
| L / R shoulders | 92 / 81 | KEY_LB / KEY_RB                     |
| R2 (`key_rc`) | 57   | hold to fast-forward                   |
| X / Y         | 84 / 85 | autofire A / B                       |
| AYA           | 86   | opens the overlay menu                 |

Volume keys are on the MTK keypad matrix (`mtk_detect_key`), edge-detected: plain
Volume changes audio level; Select + Volume changes screen brightness. Both show an
on-screen slider and persist.

The core is fed through gpSP's `input_state_cb`; `gba_driver.c` builds a GBA P1 mask
each frame and the wrap callback maps it to the core's per-button queries.

---

## 7. Overlay menu ("GammaOS Pico")

Pressing AYA opens a live settings overlay drawn on top of the running game. It is
rendered in `mt_disp_drv.c` with primitives (`ayaneo_fill`, `ayaneo_text`) using
LK's 8x16 console font (`<video_font.h>`); `gba_driver.c` (or `gbc_driver.c`)
supplies the item list, values and input handling.

Implementation notes:

- The panel is drawn **opaque** into the same double-buffered framebuffer as the
  game. In video mode a mid-scan buffer swap only tears when the two scan-out
  buffers differ, so a translucent overlay (whose pixels differ from the game
  underneath) tears; an opaque panel with identical pixels in both buffers does
  not. The game keeps running and stays visible around the panel.
- No slow work (PMIC reads, PLL read-back) runs during the render; those values are
  cached by the run loop, otherwise the overlay flickers.
- Navigation: Up/Down move, Left/Right change a value, A activates, B or AYA closes.

Items: Brightness, Volume, LCD Filter, Load State on Boot, Skip Boot Animation/Chime,
Load State, Save State, Battery, CPU Clock, Panel Refresh (read-only, the measured
value used to tune the display and audio), Benchmark, Close.

---

## 8. Persistence

All persistent data lives in dedicated `boot_b` regions (see the layout table):

- **Settings** (volume, brightness, boot behaviour, video options) in a 512-byte
  block, written block-aligned.
- **Save states**: the full gpSP machine state, written on the power-key path and
  from the menu, and resumed at boot unless Start is held or the setting is off.
- **Cartridge save** (`.sav`): the 128 KB `gamepak_backup` (SRAM / flash / EEPROM),
  persisted separately so the in-game save behaves like a real cartridge.

Buffers are flushed from the CPU cache with `arch_clean_cache_range` before the eMMC
DMA write.

---

## 9. Power, battery and charging

### Battery gauge

LK exposes no fuel-gauge state of charge, only voltage. The gauge averages a batch
of plain BATADC reads (`get_bat_sense_volt`), subtracts a small offset while
charging to approximate the resting voltage, maps it through a hand-tabulated
Li-ion open-circuit-voltage curve, and reads it **once and holds it** (the menu
seeds it on open). The PTIM current-compensated gauge is avoided because it hitches
the game and its current sign/scale were unreliable. The result is approximate
(about +-5-10%) but stable.

### Charge LED

An Awinic AW2033 RGB LED on i2c6 at address 0x45 (`mt_leds.c`): reset, enable, set
per-channel PWM. Red while charging, green when full, off on battery. It is only
re-written on a state change, since the I2C transaction is slow enough to hitch a
frame.

### Offline charging

A charger insert while "off" wakes the SoC (the PMIC does this unconditionally), and
MT6785 handles that boot mode entirely at platform init, before the emulator entry
is reached. The charging UI is therefore intercepted there
(`ayaneo_gbc_charging_screen()`): a dimmed battery-percentage screen that blanks
after idle, wakes on a power tap, boots the game on a power hold, and powers off on
unplug.

### CPU clock

The ARM PLL is re-clocked at runtime by rewriting `PCW` and pulsing the change bit
(`pll.c`), exposed as a menu stepper over a fixed OPP table (600-2000 MHz). It is
not persisted. The emulator defaults to the lowest OPP (600 MHz) because the dynarec
has ample headroom there.

Measured with every frame rendered (the uncapped benchmark presents each frame; see
below): about **110 fps at 600 MHz** and about **450 fps at 2000 MHz**, against the about 59.71 fps the vsync lock caps normal play to. So the lowest clock sustains full
speed with wide margin.

---

## 10. Benchmark and pacing modes

Normal play presents one frame per panel vsync (`config_input` blocks on
`FRAME_DONE`). Two modes bypass that cap:

- **Fast-forward** (hold R2): the emulator runs uncapped and presents sparsely
  (every 8th frame) so the vsync block does not throttle it.
- **Benchmark** (menu): renders **every** frame but presents non-blocking. A flag,
  `ayaneo_present_skip_framedone`, makes `primary_display_config_input()` skip its
  `FRAME_DONE` wait, so the front-end runs uncapped while still drawing every frame;
  the counter shows the true rendered frame rate (including the CPU blit cost),
  which is why the benchmark numbers above are lower than a compute-only figure.

---

## 11. Debugging

LK has a working UART console, but the dynarec's failure mode is a wrong-address
branch that either wedges the core mid-frame or faults, after which LK's handler
halts. That leaves no useful serial trail, so diagnostics were routed to the panel:

- `gba_dbg()` paints a numbered stage marker into the framebuffer at each bring-up
  step; the last marker left on screen identifies where it stopped.
- `exception_die()` (in `arch/arm/faults.c`, under `#ifdef AYANEO_GBA`) is hooked to
  paint the abort type and the faulting `pc`/`dfar` onto the panel before halting,
  which localises a JIT fault to an address.

A trace build (`AYANEO_AUDIO_TRACE=yes`) additionally forces LK's UART on and keeps
the on-screen markers.

---

## 12. Note on latency

Because there is no compositor and no input stack between the emulator and the
hardware, the input-to-display path is short: a button is a `mt_get_gpio_in()` read
in the frame loop microseconds before the frame that consumes it, and a finished
frame is one overlay-address flip picked up by the next panel scan. This is
structurally lower-latency than a composited OS path (an Android frame traverses
BufferQueue, SurfaceFlinger and HWComposer, and SurfaceFlinger is itself
vsync-scheduled), and there is no scheduler contention because nothing else runs.
The display-latency floor is the panel's own scan-out, which is the same for any
approach; the emulator is vsync-locked and does not attempt to beat it. This has not
been measured with instrumentation and is stated as a structural expectation, not a
number.
