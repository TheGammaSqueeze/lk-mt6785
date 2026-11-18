# ra-pico

RetroAchievements for GammaOS Pico (Konkr Pocket Block and Konkr Pocket Advance): RetroAchievements'
own library, rcheevos, built as a blob the LK loads from the card into its slot (0xBE600000, 4 MiB,
after the Wi-Fi memory) and drives through the export table in `pico/ra_core_abi.h`.

Softcore only. Unlocks never wait for the network: the blob answers them itself and hands each one
to the LK to keep on the card, and the LK sends them later with the time they happened.

- `./build.sh` builds `out/ra-pico.blob` (copy it to the card as `/cores/ra-pico.blob`).
- `tools/harness.sh` runs the glue and rcheevos on the PC against a small test server: sign in,
  a set, unlocks, progress, mastery, the offline queue and a start from the kept answers.
  `SAN=1 tools/harness.sh` adds the address and undefined behaviour checkers;
  `tools/harness.sh blob` runs the built blob itself under qemu-arm.

## Third party

- rcheevos 12.3.0 (RetroAchievements, MIT): their tagged release, unchanged, in `rcheevos/` (its
  `LICENSE` there; its own MD5 in `src/rhash/md5.c`).
- libretro-common's `libretro.h` (MIT), which rcheevos' `rc_libretro` reads the cores' memory maps
  with, from the Armulators app's `deps/` in `cores/armNES-pico` (see `sources.sh`).
- newlib (the C library of the arm-none-eabi toolchain).

Everything in `pico/` and `tools/` is GammaOS code (GPL-2.0-or-later).
