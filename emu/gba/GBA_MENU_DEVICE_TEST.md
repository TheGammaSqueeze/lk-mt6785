# GBA menu: on-device test checklist

The SNES-Classic-mini ROM selector is fully host-validated (see GBA_SNES_MENU.md);
the only things that need real hardware are frame timing (flicker) and the physical
controls. This is the quick pass to run after flashing.

## Flash

    fastboot -s 0123456789ABCDEF flash lk_a /mnt/c/pairmini/lk_a_gba_sd_signed.img

boot_b is UNCHANGED since the cart-art build, so lk_a alone is enough. Only reflash
boot_b if the SNES asset pack or the cart placeholder changed:

    fastboot -s 0123456789ABCDEF flash boot_b /mnt/c/pairmini/gba_menu_boot_b.img

NEVER target a28c0e0e. Put GBA ROMs in /roms/gba on the microSD (empty folder -> the
device cleanly boots the embedded game, never-brick).

## What to check

1. INTRO -> MENU: the BIOS logo plays, then the menu fades in from black (~0.3s).
2. CAROUSEL: the focused cart is blue with a pulsing cursor; a neon SNES wallpaper
   parallax-scrolls behind it; the SUPER NINTENDO bar is pinned bottom, menubar top
   (4:3, fills the 1280x960 panel).
3. AUDIO: looping background music; a cursor blip on every move; a confirm click on
   launch. No stutter or loop while browsing, no BGM tail after you press A.
4. NAV: D-pad L/R moves one game (hold = auto-repeat). Up opens the menubar, Down the
   Suspend Point List (SNES firmware screen, shows "No Data" - decorative for GBA).
5. SORT: SELECT toggles the title A-Z / Z-A (a "Sort A to Z" label flashes); the
   focused game stays selected.
6. FAST SCROLL (large library): L / R shoulder jumps 10 games; a cyan scroll-position
   bar (top-right) shows where you are in the list.
7. LAUNCH: A or Start boots the focused ROM; the menu fades to black over ~0.2s first.
   Confirm it boots the game whose title was on screen (verified in sim across sorts).
8. CLOCK: the menu runs at 2100 MHz for render headroom, then the game drops back to
   its 600 MHz default automatically - nothing to check, just noted.

## The flicker question (the one open item)

The menu render was measured ~7.5ms/frame in sim (A55 estimate, under the 16.7ms
60fps budget), but sim is not cycle-accurate. If you see any flicker/tearing on
movement, hold START+SELECT together to toggle the on-screen perf HUD (top-left):

    R<cur>ms  P<peak>ms  <fps>f

Watch P (peak render ms). If P stays < 16.7 the render is fine and any flicker is a
present/vsync issue; if P spikes over 16.7 in a state, that state's render is over
budget and is the target (next lever would be DISP-OVL hardware layering - see
ovl-layering, deliberately not done blind). Report the P value per state.

## Known-deferred (by design, not bugs)

- Cart art is a single GBA placeholder (real box art not available yet).
- The menubar settings screens + Suspend Point List are the real SNES firmware
  screens, kept for the 1:1 look; they are not wired to GBA functions.
