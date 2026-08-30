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
- SELECT toggles a name A-Z / Z-A sort (the SD scan is already A-Z, sorted
  case-insensitively in `sd_fat.c`); focus stays on the same game across the flip.
- rosters below `CAR_RING_MIN` (8) use a finite LINEAR carousel with clamped nav
  (no on-screen ring wrap = no card pop); 8+ keep the seamless SNES ring.

## Controls (GBA mode)

    D-pad L/R     move one game (auto-repeat when held)
    D-pad Up      open the menubar (SNES settings screens, decorative for GBA)
    D-pad Down    open the Suspend Point List (SNES firmware screen, not wired for GBA)
    A / Start     launch the focused ROM
    SELECT        toggle name sort A-Z <-> Z-A
    L / R shoulder  PAGE JUMP by 10 games, snap + auto-repeat (fast scroll for big
                    libraries); `car_jump()` clamps the step to [1, n-1] so it never
                    wraps onto the same card (a page of 10 in a 10-game ring)
    Start+Select  toggle the perf HUD (off by default; consumed so it does not
                  also launch/sort) - shows current + peak render ms for flicker
                  diagnosis on device

## Large-library affordances

A full SD card of ROMs (up to the 128 the driver hands over) is the common case, so:
- the bottom filmstrip is a fixed-spacing SCROLLING window (focus kept near centre,
  clamped at the ends) once the roster exceeds what fits, instead of cramming every
  cart into an unreadable line.
- a slim scroll-position bar (top-right, `VIEW_TOP` 1:1 screen space) shows a cyan
  thumb at `focus/(n-1)` when `n >= CAR_RING_MIN` - a numeric "N / total" is not used
  because the SNES firmware fonts have incomplete digit glyphs (no `4`, etc).
- the L/R page jump above; both it and a d-pad step nudge the parallax background
  (`bg_scroll_kick`) so the wallpaper stays alive during fast traversal.

## boot_b layout (SD flow: the ROM region is free, ROM comes from the card)

    0x00000000  video animation ("GBA1")
    0x01000000  boot chime ("ABA1")
    0x01100000  SNES pack: "SNSZ" u32 rawlen u32 complen  raw-deflate(pack)
    0x01C00000+ save / state / settings (runtime)

## DRAM map (menu phase; the whole WB window 0x50000000..0x56000000 is free)

    0x50000000  decompressed pack blob (capped at HOME_PA - 2MB = 22MB)
    0x51800000  home rnode pool (16MB)
    0x52800000  bg rnode pool (2MB)
    0x52A00000  deflate staging (8MB; free after load_pack) -> reused as the
                resume-panel overlay cache (fb-size ~4.9MB) during the menu
    0x53200000  wallpaper cache (1536x720, the neon bg)
    0x53700000  static chrome cache (1280x960)
    0x54000000  4:3 warped-wallpaper cache (WP43_PERIOD 2701 x 960 = 10.4MB)
    0x54C00000  card-tile cache (GBA cards all identical -> cap 1)
    0x54C80000  focused (blue) card-body tile (identical for all GBA carts)

The blob lives where the 64MB gpSP arena (0x50000000) normally sits; it is free
during ROM-select and the emulator reclaims it on launch.

## Performance (60fps target, 2100 MHz during the menu)

The panel is physically 1280x960 (4:3), so the menu runs the SNES 4:3 layout
(fills the panel; menubar pinned top, SUPER NINTENDO bar bottom) rather than
letterboxing the 720 design. That makes every full-frame pixel pass 33% larger,
and the 4:3 wallpaper originally needed a per-pixel inverse-map GATHER (scattered
reads, slow on ARM) - which put the render right at the 60fps budget and caused
frames to oscillate 60<->30 (visible flicker + audio-ring starvation). The perf
stack (host render times in parens):

- 4:3 warped-wallpaper cache (`wp43`): the ASP_WALL_S warp is baked once into a
  2701x960 buffer so `draw_wp_43` is a per-row MEMCPY-scroll, not a gather.
- static chrome cache: the homemenu chrome rendered once, run-length memcpy each
  frame (`draw_chrome`).
- card-tile cache: card body pre-rendered once, blitted per frame. Every GBA card
  is the identical cart placeholder, so ONE tile serves all (cap 1) - which frees
  the DRAM for the 10.4MB wallpaper cache.
- resume-panel cache: the suspend list is static once slid in, so it is rendered
  once into an overlay and `snes_composite`d each frame instead of re-walking its
  scene subtree (5.19 -> 3.02ms).
- focused-card body tile (`fct`): the ctile cache only serves the NON-focused cards;
  the blue (active) focused card was still rendered live every frame (~2.7ms on the
  A55). Its body is identical for every GBA cart, so it is baked ONCE (blue frame +
  cart art, cursor suppressed) and blitted each frame with only the pulsing cursor
  drawn live. Host carousel phase 1101 -> 555us, total frame 1891 -> 1370us. The
  cache is bypassed mid-nav crossfade (never happens in GBA mode - the crossfade is
  snapped - but kept for correctness) and rebuilt on an aspect flip.

Result (host): home / menubar / carousel ~1.9ms, resume 3.0ms, submenus 3.2-4.6ms
(static when viewed, so no flicker even if over budget on device). The blitter
`snes_render.o` is `-O2 + NEON`; the rest is `-Os` for the 2MB `lk_a` budget.

Frame pacing: match the flicker-free GBA game exactly - `skip_framedone=0`, ONE
blocking `ayaneo_canvas_present()` per frame (config_input blocks on the panel
FRAME_DONE = one buffer per vsync). Do NOT stack a wait/timer on top: that adds
delay after the vsync block, overruns a refresh, drops frames. `snes_menu_update`
gets the REAL measured dt (not fixed 1/60) so motion is correct at any frame rate.
An on-screen "R<render> P<peak> <fps>" readout (top-left) reports the device
render time; P (peak render ms) is the number to watch - a spike over 16.7ms is
what breaks vsync. It is OFF by default (clean menu) and toggles with Start+Select
so the user can still report the numbers if flicker is ever seen on device.

Audio handoff: the menu BGM feeds the shared AFE ring; on launch the whole ring is
zeroed (`ayaneo_menu_audio_silence()`) so the DMA loops silence during the ROM
load instead of replaying the BGM tail.

## Off-device validation

    cd emu/gba/menu && bash build_host.sh
    GBA_ROSTER=6 ./host_render <pack> <out.ppm> <settle> <nav>   # e.g. nav "URRRA"

`COVCHECK=1` (unwritten pixels), `ALPHACHECK=1` (alpha<0xFF), `TIMEIT=N`
(ms/render), `SNES_FORCE169=1` (16:9), `AUDIT_AUDIO=1` (BGM/SFX GUIDs resolve in the
pack), `GBA_ROSTER=N` (mock N ROMs, up to 128, cleaned by the real `gba_clean_name`).

`tools/ayaneo/gba/validate_menu.sh` is the full regression pass: all 11 menu states
plus a large-library set (n=10/40/128 exercising the page-jump `[`/`]`, sort-at-scale,
scrolling filmstrip and position bar) plus the audio-asset audit - 18 states + audio,
each non-blank-checked. `ctcheck` (host_render `- ctcheck`) diffs the card-tile /
focused-tile caches against a live render.

The device is often unreachable; validate host-side and stage `lk_a.img` +
`gba_menu_boot_b.img` to `/mnt/c/pairmini`.
