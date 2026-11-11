# Running Pokemon in the bootloader, or: how I taught a MediaTek preloader to emulate a Game Boy Color

> "Just because you can, doesn't mean you should" is a sentence I read after I already had.

This is the story of getting [gambatte](https://github.com/libretro/gambatte-libretro),
a real, cycle-accurate Game Boy Color emulator, to run inside LK, the MediaTek
Little Kernel bootloader, on an AYANEO Pocket Air Mini (Helio G95 / MT6785). No
Linux. No kernel. No userspace. The bootloader boots, plays a video, and then,
instead of doing its one job (booting an OS), it just runs Pokemon. Forever. With
sound.

If you do kernel work, you already have a headache forming. Good. Let's earn it.

## The setting

LK is the thing that runs after the preloader and before the kernel. Its entire
purpose in life is to find `boot.img`, wave a device tree around, and `smc` its
way into the Linux kernel at EL1. It has:

- One CPU core. SMP is somebody else's problem (the kernel's).
- A heap of about 638 KB. I know this number intimately because I blew past it
  and the secure-image verifier `panic()`'d in my face.
- No libc worth the name, no libstdc++, no dynamic linker, no filesystem, no
  scheduler you'd recognize, no MMU niceties, and a watchdog that will reboot the
  whole device if you stop paying attention to it for a few seconds.
- 32-bit ARM. On an ARMv8 SoC. In AArch32, because the preloader started it that
  way and nobody asked it to change. (Yes, the first thing I did was assume
  arm64. It is not arm64.)

Into this, we are going to link a 15,000-line C++ codebase that uses STL. On
purpose.

## Step 1: C++ in a place C++ should never be

gambatte is C++. It has `std::vector`, `std::string`, virtual functions,
templates, the works. LK is C, compiled by a decade-old Android
`arm-linux-androideabi` GCC 4.9 that ships no C++ standard library headers at
all. `#include <vector>` just laughs at you.

The move: compile the emulator with a different, newer toolchain
(`arm-none-eabi-g++` 10.3, which does ship libstdc++ headers), ABI-matched to LK
(armv7-a, soft-float, EABI, thumb-interwork), into a static archive, and hand
that archive to LK's ancient linker. Two compilers, one image, connected by a
thin `extern "C"` bridge so only the boring AAPCS ABI ever crosses the border.

But "freestanding C++" means you are now the C++ runtime. So:

- `operator new` / `delete`? That is a bump allocator I wrote over a fixed slab
  of DRAM. `delete` is a no-op. The emulator allocates once at boot and lives
  until the heat death of the device, so who cares.
- Exceptions and RTTI? `-fno-exceptions -fno-rtti`, gone.
- The handful of libstdc++ symbols that survive (`std::__throw_length_error`,
  `__cxa_pure_virtual`, and friends)? Stubbed to infinite loops. If a
  `std::vector` ever actually throws `bad_alloc` in a bootloader, spinning
  forever is honestly the correct emotional response.
- `std::string`? Only used by the cheat-code parser. I deleted the cheat-code
  parser. We do not Game Genie in this house.

The reward for all this was a beautiful, clean link, which then reused a stale
`lk.img` from a previous build and booted the kernel anyway, making me think none
of it worked. Which brings us to:

## Step 2: the build system, my nemesis

Here is a fun one for the incremental-build enjoyers. The emulator archive is not
a normal LK object, so nothing in the makefile depended on it. I would rebuild
the core, rebuild LK, flash it, and get the kernel, because `make` looked at the
final ELF, saw all of its prerequisites were unchanged, and served me a cached
image from before the emulator existed. My "build succeeded!" checks were reading
the timestamp of a lie.

The actual link was failing the whole time on one symbol:
`cartridge_set_rumble(unsigned)`. It is a C++ symbol (mangled
`_Z20cartridge_set_rumblej`), and I had defined the stub as a C function
(unmangled). The linker could not match them, errored, and `make` shrugged and
kept the old binary. Fix: define it on the C++ side so the mangling lines up, and
make the ELF actually depend on the archive so a stale image is impossible.

Lesson, re-learned for the hundredth time: the build system is not your friend,
it is a feral animal you have negotiated a temporary truce with.

## Step 3: alignment, the silent assassin

Now it links, it is genuinely in the image, and on real hardware it loads the
ROM, initializes the emulator (`heap_used=2236608`, chef's kiss), and then
`data abort, halting`.

Emulators do unaligned memory access constantly. Reading a 16-bit value out of a
byte array at an odd offset is just Tuesday. On a desktop, or on Linux with the
alignment-fixup handler, nobody notices. LK runs with strict alignment checking
on and no fixup handler, so the first unaligned `ldrh` face-plants into a data
abort and the CPU just stops.

Fix: `-mno-unaligned-access`, which tells the compiler to stop assuming the
hardware will bail it out and emit byte-wise access sequences instead. One flag.
Hours of my life. The game booted.

## Step 4: the watchdog wants to go home

It booted. It ran. For about four seconds. Then the device rebooted.

Because, and this is the most bootloader bug imaginable, LK has a hardware
watchdog that it kicks during normal boot, on the assumption that boot is a
finite process that ends with jumping to a kernel. I had replaced "jump to
kernel" with "loop forever running Pokemon." The watchdog, reasonably, concluded
that boot had hung, and did its job: reset the SoC.

`mtk_wdt_disable()` plus a per-frame `mtk_wdt_restart()` and the watchdog was
persuaded that everything is fine, this is fine, we always emulate handhelds in
the bootloader, please go back to sleep.

## How it actually works

Once you accept the premise, the architecture is almost reasonable:

- Storage. The 2 MB ROM lives in the `boot_b` partition (the inactive A/B slot,
  33 MB of "nobody is using this right now") at a fixed offset, behind a tiny
  `[GBCR][size]` header, alongside the boot animation and boot-sound blobs.
  `gambatte::GB::load()` takes a memory buffer, so there is no filesystem to
  miss.
- Memory. I carve a 32 MB slab of raw DRAM at a fixed physical address
  (`0x50000000`, chosen by the ancient and honorable method of "seems free, let's
  find out") and hand-partition it: ROM here, audio scratch there, C++ bump-heap
  for the rest. The MMU identity-maps DRAM, so VA == PA and life is simple.
- The loop. On its own LK thread: `runFor()` emulates one frame, we get a 160x144
  RGB565 framebuffer and a pile of audio samples, we pace it to the Game Boy's
  real 59.7275 Hz, kick the watchdog, repeat until the sun expands.
- Video. That 160x144 frame gets an integer 6x nearest-neighbor scale (960x864,
  centered, black borders) blitted by hand into the panel framebuffer, converted
  RGB565 to the panel's eBGRA8888 per pixel, on the CPU, because there is no GPU
  here and there is no one coming to help. Then it is double-buffered onto the
  DSI overlay with the same address-flip trick the boot animation uses to force a
  re-latch in video mode.
- Audio. This is my favorite cursed bit. There is no audio support in LK at all.
  I had already brought up the entire MT6785 AFE and MT6359 codec from bare
  registers (power domain via SPM, the MTKAIF serial link to the codec chip, the
  analog LDO rails, the headphone output stage) just to play a boot chime. For
  the game, gambatte spits out stereo audio at the Game Boy's native 2,097,152 Hz
  (that is 2^21, the actual APU clock). I box-resample that to 48 kHz in
  fixed-point integer math and stream it into a looping DMA ring that the AFE
  plays continuously, refilling it ahead of the hardware read pointer every
  frame. The bootloader is now a real-time audio mixer. It was not designed to be
  a real-time audio mixer.

## The scoreboard of things that had opinions

- The Android GCC that has no STL, sidestepped with a second toolchain.
- The 638 KB heap, sidestepped with a private 32 MB DRAM slab.
- The linker's mangled-vs-unmangled symbol mismatch, an afternoon.
- `make` serving stale images, a genuine "am I losing my mind" arc.
- Strict alignment checking, one glorious compiler flag.
- The watchdog, a firm handshake every frame.
- Every single one of these failed silently or misleadingly. None of them said
  "hey, the emulator is not linked" or "hey, you did an unaligned load." They said
  `data abort, halting`, or rebooted, or just booted the wrong thing.

## Was it worth it?

A device whose bootloader, given the choice between loading a modern Android
system and emulating a 1998 handheld, chooses the handheld, is a device that has
been shown love. It plays video. It plays sound. It does not, yet, take input, so
it is less "playable" and more "the world's most over-engineered demo reel," but
that is the next commit.

It should not exist. It runs Pokemon. Both things are true.

Filed under: things the MTK_SIP secure world will have to answer for someday.
