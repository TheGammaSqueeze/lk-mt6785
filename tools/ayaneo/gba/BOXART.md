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

## LK side (in progress)

The menu resolves `<romstem>.ART` for each enumerated ROM, loads it into a DRAM
snes_img, and draws it in the card boxart window; a missing tile falls back to the
generated placeholder cartridge. Note: the 60fps card-tile cache (commit 7d4d8b3)
shares one tile across all GBA cards; per-ROM art makes every card distinct, so the
ctile cache needs per-gi slots again while staying within the 60fps budget.
