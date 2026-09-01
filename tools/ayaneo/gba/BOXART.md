# GBA boxart pipeline

Per-ROM box art for the SNES-mini GBA menu. Each game card shows its own box
front instead of the shared placeholder cartridge, loaded at runtime from the SD
card (not baked into lk_a, which is at the 2MB wall).

## .ART tile format (little-endian)

    off 0  : magic  "GART" (0x47 0x41 0x52 0x54)
    off 4  : u16    version (1)
    off 6  : u16    format  (0 = RGB565 LE)
    off 8  : u16    width
    off 10 : u16    height
    off 12 : width*height * u16  RGB565 pixels, row-major top-to-bottom

Tiles are scaled aspect-preserving so the larger side is at most 224 px (the card
boxart window). A typical GBA front (which includes the left GAME BOY ADVANCE
banner) is near-square, so it fills the tile with little padding.

## Generating tiles (host)

    tools/ayaneo/gba/gen_boxart.py \
        --zip /work/gbaart/109869101_ABeezy1388NintendoGameBoyAdvanceBoxArt.zip \
        --outdir out/boxart

Output files are named by the source stem, which is the No-Intro title, e.g.
`007 - Everything or Nothing (USA, Europe).ART`. A ROM named identically on the
SD card matches its art by basename.

## Getting tiles onto the device

Push each tile to `/roms/gba/boxart/<romstem>.ART` on the SD card, either by
copying with the card in a reader, or over fastboot via the existing sd-put path
(see emu/gba/sd_fastboot.c):

    fastboot -s <serial> oem sd-put:/roms/gba/boxart/<romstem>.ART < out/boxart/<stem>.ART

## LK side (done)

At menu init the glue preloads each ROM's `/roms/gba/boxart/<romstem>.ART` into a
DRAM boxart region (gba_boxart_load_sd -> gba_boxart_decode), builds a per-game
snes_img table, and hands it to the menu (snes_menu_set_gba_boxart). draw_card
draws `gba_boxart[gi]` when present and falls back to the placeholder cartridge on
a missing, oversized or corrupt tile (the decoder validates and returns an error,
so a partial/bad file never breaks boot). The 60fps card-tile cache (commit
7d4d8b3) shared one tile across all GBA cards; with per-ROM art it goes per-game
(cap 12) only when boxart is loaded - the shared fast path is unchanged otherwise,
so no 60fps regression. Host-validated (validate_menu "per-ROM boxart render").

`fastboot -s <serial> oem diag` reports `bx=<loaded>/<total>` so the SD load can be
confirmed without a screen.

## Getting the ROM list and matching names

The menu keys art by each ROM's own basename, which usually differs from the
No-Intro title, so tiles must be renamed to match. Get the device ROM list from
the BOOTLOADER (the running menu holds the SD, so probe/put only work there):

    fastboot -s <serial> reboot bootloader
    fastboot -s <serial> oem sd-probe          # lists every ROM name

Then tools/ayaneo/gba/push_boxart.py fuzzy-matches that list to the box-art zip and
writes tiles renamed to the device basenames (optionally pushing them).

## Caveat: use a card reader for bulk copies

`oem sd-put` writes over the fastboot USB stack; a very long filename can wedge the
USB link mid-FAT-write (and may need a device power-cycle). It works one-file-at-a-
time in the bootloader, but a CARD READER is safer and faster for the whole library.
