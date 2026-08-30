# GBA-from-SD ROM selector: the SNES-Classic-mini menu

The GBA-from-SD flow (branch `lk-gba-emu-sd-card`) shows a ROM picker before
launching a game. That picker is the REAL Super Nintendo Classic Edition home
menu engine, driven 1:1, with only the per-card boxart swapped for a GBA
cartridge placeholder and the roster taken from the microSD ROM scan.

Build with `./build_ayaneo_gba_sd.sh` (NOT `build_ayaneo_gba.sh`, which omits the
menu - the whole SD flow is gated behind `AYANEO_GBA_SD=yes`).

## Pieces

- `emu/gba/menu/` - the SNES menu engine imported verbatim from the `lk-snes-menu`
  branch: `snes_pack` (asset blob reader), `snes_scene` (node tree), `snes_render`
  (NEON software blitter), `snes_audio` (48 kHz mixer), and `snes_menu.[ch]` (the
  full home-menu logic: carousel, menubar, 5 settings screens, resume list, HUD,
  fades, sort). `host_render.c` renders any state to a PPM for off-device
  validation (COVCHECK / ALPHACHECK / TIMEIT env modes).
- `emu/gba/gba_snes_menu.c` - the LK driver. `gba_snes_menu_run(roms, nrom, sel)`
  loads the pack from boot_b, builds the roster, runs the menu loop, and returns
  the picked ROM index (or -2 if the pack is missing -> caller falls back to the
  plain list, never-brick). Called from `gba_sd_rom_select()` in `gba_driver.c`.
- `tools/ayaneo/snes/pack_snes.py` - packs the (copyright, user-supplied) SNES
  firmware asset tree into the blob; `--gba-cart <png>` injects the cartridge
  placeholder as resource `gba_cart`.
- `tools/ayaneo/gba/gen_gba_cart.py` - draws the 228x160 cartridge placeholder.
- `tools/ayaneo/gba/build_snes_boot_b.py` - packs anim + chime + the SNES pack
  (`SNSZ` deflate) into the boot_b image.

## GBA adaptation of `snes_menu.c`

`snes_menu_set_gba_roster(m, names, n, cart)` puts the menu in GBA mode:
- `game()` returns no pack record (the roster is the SD ROM list), so per-game
  SNES fields (boxart, player icon, suspend dots, the 4-rule sort) are bypassed.
- each card's boxart is the single `gba_cart` placeholder image.
- the title uses the ROM name; `clean_name()` (in the driver) strips `.gba` and
  trailing No-Intro tags `(USA)`/`[!]`; the title auto-fits the caption plate.
- the bottom filmstrip draws a small GBA cart per slot (centred), so the chevron
  points at a real thumbnail.
- A / Start set `m->launch`; `snes_menu_take_launch()` hands it to the driver.
- SELECT toggles a name A-Z / Z-A sort (the SD scan is already A-Z).
- rosters below `CAR_RING_MIN` (8) use a finite LINEAR carousel with clamped nav
  (no on-screen ring wrap = no card pop); 8+ keep the seamless SNES ring.

## boot_b layout (SD flow: the ROM region is free, ROM comes from the card)

    0x00000000  video animation ("GBA1")
    0x01000000  boot chime ("ABA1")
    0x01100000  SNES pack: "SNSZ" u32 rawlen u32 complen  raw-deflate(pack)
    0x01C00000+ save / state / settings (runtime)

## DRAM map (menu phase; the whole WB window 0x50000000..0x56000000 is free)

    0x50000000  decompressed pack blob (capped at HOME_PA - 2MB = 22MB)
    0x51800000  home rnode pool (16MB)
    0x52800000  bg rnode pool (2MB)
    0x52A00000  deflate staging (8MB; free after load_pack)
    0x53200000  wallpaper cache (1536x720)
    0x53700000  static chrome cache (1280x960)
    0x54000000  card-tile cache (<=64 tiles)

The blob lives where the 64MB gpSP arena (0x50000000) normally sits; it is free
during ROM-select and the emulator reclaims it on launch.

## Performance (60fps target, 2000 MHz during the menu)

Reuses the SNES perf stack: cached wallpaper (memcpy scroll in 16:9, per-pixel
warp in 4:3), a static chrome cache composited each frame, and a card-tile cache
(each card body pre-rendered once, blitted per frame). The blitter
(`snes_render.o`) is built `-O2 + NEON`; the rest of the menu is `-Os` to keep
`lk_a` within its 2MB budget alongside the gpSP emulator (raw ~1.98MB).

The panel is physically 1280x960 (4:3), so the menu runs the SNES 4:3 layout
(fills the panel; menubar pinned top, SUPER NINTENDO bar bottom) rather than
letterboxing the 720 design.

Frame pacing: the render is only ~2ms, so an uncapped present would flip the
double buffer many times per refresh (torn/mixed buffers = flicker on movement).
The loop makes the present non-blocking (`skip_framedone=1`) and paces with
`ayaneo_wait_frame_done()` (one buffer per vsync), with a 13MHz-timer floor and an
adaptive fallback to timer-only if the FRAME_DONE event never fires (command mode).

Audio handoff: the menu BGM feeds the shared AFE ring; on launch the whole ring is
zeroed (`ayaneo_menu_audio_silence()`) so the DMA loops silence during the ROM
load instead of replaying the BGM tail.

## Off-device validation

    cd emu/gba/menu && bash build_host.sh
    GBA_ROSTER=6 ./host_render <pack> <out.ppm> <settle> <nav>   # e.g. nav "URRRA"

`COVCHECK=1` (unwritten pixels), `ALPHACHECK=1` (alpha<0xFF), `TIMEIT=N`
(ms/render), `SNES_FORCE169=1` (16:9). The device is often unreachable; validate
host-side and stage `lk_a.img` + `gba_menu_boot_b.img` to `/mnt/c/pairmini`.
