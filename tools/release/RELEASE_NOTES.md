GammaOS for the **Konkr Pocket Advance**, release @VERSION@ (lk-mt6785 `@COMMIT@`).

GammaOS replaces only the console's bootloader (LK). With the GammaOS microSD card in the slot the
console starts a console menu and plays Game Boy, Game Boy Color, Game Boy Advance, SNES,
Mega Drive / Genesis, Master System, Game Gear and SG-1000 games from the card. Without the card,
or after Boot to Android in the menu, Android starts exactly as before.

## Downloads

- **gammaos-kpa-@VERSION@-flash.zip**: the LK for `lk_a` and `lk_b`, to install GammaOS and to go
  back to stock: SP Flash Tool v5 and V6 scatters (Windows) and mtkclient scripts (Linux), both of
  which work on a stock console with a locked bootloader, plus fastboot scripts for updates. Read
  its README.txt.
- **gammaos-kpa-@VERSION@-sdcard.zip**: the card. Copy its contents to the root of a FAT32 or
  exFAT microSD card. It has the menu, the five emulator cores and the demo games below.

## Install

1. Flash the GammaOS LK from the flash package: on Windows `spflash/gammaos/` with SP Flash Tool
   (V6 `.xml` or v5 `.txt` scatter, mode Download Only, only `lk_a` and `lk_b` ticked); on Linux
   `mtkclient/flash_gammaos.sh` with [mtkclient](https://github.com/bkerler/mtkclient).
2. Copy the sdcard package's contents to a microSD card, put it in and power on.
3. Optional: put your own Game Boy Advance BIOS on the card as `bios/gba_bios.bin` for the original
   boot logo; without it armGBA's own Open BIOS splash plays.

To go back, flash `spflash/stock/` or run `mtkclient/flash_stock.sh` the same way.

### What has been tested on a console

| Method | Status |
|---|---|
| SP Flash Tool V6 with the `.xml` scatter | tested: the route the first consoles were installed with |
| fastboot scripts (updates) | tested: both slots written, the console starts GammaOS |
| SP Flash Tool v5 with the `.txt` scatter | **untested** on a console (same partitions and files as V6) |
| mtkclient on Linux | **untested** on a console (writes the same two partitions) |

If you use one of the untested methods, please report how it went.

## Demo games

@GAMES@

Each is redistributed under its licence; the texts are in `licences/games/` on the card.

## Source

- LK: https://github.com/TheGammaSqueeze/lk-mt6785 (branch `lk-gba-emu-sd-card-kpa`); build this
  release with `tools/release/make_release_kpa.sh`.
- Cores: [armGBA-pico](https://github.com/TheGammaSqueeze/armGBA-pico),
  [gpsp-pico](https://github.com/TheGammaSqueeze/gpsp-pico),
  [gambatte-pico](https://github.com/TheGammaSqueeze/gambatte-pico),
  [snes9x-pico](https://github.com/TheGammaSqueeze/snes9x-pico),
  [genesis-plus-gx-pico](https://github.com/TheGammaSqueeze/genesis-plus-gx-pico).

Checksums are in `SHA256SUMS`.
