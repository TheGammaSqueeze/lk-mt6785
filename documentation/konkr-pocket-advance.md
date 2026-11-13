# Konkr Pocket Advance (KPA) port

MT6785 handheld, board family `k85v1_64` (same preloader name, partition map, eMMC, sub PMIC
and button GPIOs as the AYANEO Pocket Air Mini), with a different panel, a volume wheel instead
of volume keys, no analog sticks and I2S smart amps for the speaker. Stock firmware package:
`BW03_20260730` (Android 12, kernel 4.14.186, vendor `ARBOR/GT78-VN`).

## Build

    git submodule update --init --recursive   # once, or let the build fetch the cores
    ./build_konkr_kpa.sh                       # add "clean" for a fresh LK build

Output in `out/`:

| Output | Where it goes | What it is |
|---|---|---|
| `lk_a_kpa_signed.img` | `lk_a` | the LK (2 MB), signed with the Konkr cert partitions + stock `lk_main_dtb` |
| `sdcard/` | the microSD card | the card template: `gammaos/` (boot.cfg, menu.pack), `cores/`, `bios/`, `roms/` |

`lk_a` is the only partition GammaOS Pico writes. `logo`, `boot_a/b`, `vendor_boot`, `dtbo`, `misc`,
`vendor`, `system` stay stock, so Android boots as before whenever the LK does not start GammaOS Pico.
`lk_b` is left stock as a fallback.

The cores are the git submodules under `cores/` (`armGBA-pico`, `gpsp-pico`, `gambatte-pico`,
`snes9x-pico`, `genesis-plus-gx-pico`), each with the emulator's upstream as its own submodule plus
GammaOS Pico patches and glue. `tools/ayaneo/gba/make_sdcard.sh` fetches a missing one, rebuilds a core
whenever its commit changed or its tree has local changes (`out/.built-commit` in each core), and
notes a core checked out at a different commit than this tree pins. `CORE_REPOS=<dir>` builds from
other checkouts while working on a core. The core builds need `arm-none-eabi-gcc` and `g++`.

## Flashing

`lk_a` only. A first flash on a locked stock unit goes through SP Flash Tool: the GammaOS Pico package
(`gammaos/` with a V2.1.0 text scatter that ticks `lk_a` only, Download Only, SP Flash Tool v5)
or the stock BW03 package's own `.xml` scatter in SP Flash Tool V6 with only `lk_a` ticked (v5.1924
rejects the BW03 V2.2.0 scatter with S_DL_SCAT_VERSION_SUPPORTED 5079). Keep the stock `lk.img`
next to it: a bad `lk_a` is recovered the same way by pointing `lk_a` at the stock image.

Once our LK runs, fastboot works from the bootloader and alongside the running menu (USB VID
0x0E8D PID 0x201C). The serial is unique per unit (`oem serialinfo` shows its source):

    fastboot -s <serial> flash lk_a out/lk_a_kpa_signed.img

Flashing over USB, learned on the device:
- Always address the device: `fastboot -s <serial> ...`. Other devices share the host.
- Flash from plain bootloader mode (`fastboot reboot-bootloader` from the running menu, then
  `fastboot oem mode` until it reports boot=99), then ONE `fastboot flash`, then ONE `fastboot
  reboot`. The menu side channel refuses flashing ("EMU(no flash)" in `oem mode`).
- `oem sd-put:/path` (after `fastboot stage <file>`) writes a file to the card from the
  bootloader, paths with spaces included when the argument is quoted.
- Do not fire commands back to back right after a reboot, and never pipe fastboot output into
  `head` or wrap it in `timeout`: an unread reply desyncs the LK link. From WSL,
  `/usr/bin/fastboot` is the Windows fastboot.exe; NEVER kill it, a hang needs a cable replug.
- `oem` commands are registered in both the plain bootloader and the menu channel, so their
  presence does not tell the two apart; `oem mode` does.

## microSD card

FAT16, FAT32 or exFAT, MBR or GPT, first partition. Case insensitive names. Copy `out/sdcard/` to
the root and add games.

```
/gammaos/boot.cfg        boot=sd (start GammaOS Pico) or boot=android (one key per line, # comments)
/gammaos/menu.pack       the Classic and GBA Indigo themes' menu (compressed)
/gammaos/current-<W>x<H>.pack, current-igm-<W>x<H>.pack   the Current theme and its in-game menu
/gammaos/lang-<code>-<W>x<H>.pack   a language's texts and the glyphs it needs (one per language)
/gammaos/recent.txt      the games played most recently (written by the LK)
/gammaos/playtime.txt    each game's play time and when it was last played (written by the LK)
/gammaos/settings.bin    the settings (written by the LK; /gba/settings.bin is still read once)
/gammaos/cart_header.bin the last game's header, so the intro starts at once (written by the LK)
/gammaos/blackbox.bin    fault and event records moved here from DRAM (oem blackbox)
/cores/armgba-pico.blob  /cores/gpsp-pico.blob  /cores/gambatte-pico.blob
/cores/snes9x-pico.blob  /cores/genesis-plus-gx-pico.blob
/bios/gba_bios.bin       the GBA BIOS, exactly 16384 bytes: put your own here (/gba_bios.bin is read
                         too); without it armGBA runs on its built-in replacement, gpSP not at all
/roms/gb/*.gb            /roms/gbc/*.gbc        /roms/gba/*.gba
/roms/snes/*.sfc|.smc    /roms/genesis/*.md|.gen|.bin|.smd
/roms/sms/*.sms          /roms/gg/*.gg          /roms/sg/*.sg      (raw files, not zips)
/roms/<system>/boxart/<romstem>.ART   optional box art (tools/ayaneo/gba/gen_boxart.py)
/saves/<system>/         /states/<system>/      saves and save states (created by the LK)
/gba/armgba-pico.cfg     the GBA core choice and armGBA's options; /gba/gbcprefs.bin GB palettes
```

ROMs can also be pushed over USB with `tools/ayaneo/push_roms.py <system> <files> --serial
<serial>` (device in the bootloader; `--extract` unpacks zipped ROMs first).

## Boot

On a normal power on the panel comes up on the stock timing and the SD gate decides:

- SELECT + START held: Android. SELECT + START + R held: fastboot.
- SELECT held: GammaOS Pico, rewriting `boot=sd` if the card was set to Android (the way back after
  the menu's Boot to Android, which writes `boot=android` and restarts).
- Two watchdog resets in a row inside the GammaOS Pico path: Android (the never brick guard).
- No card, a card that does not mount, no valid `boot.cfg`, `boot=android` or no `menu.pack`:
  Android, with the stock logo and the stock panel timing.
- Otherwise GammaOS Pico starts. The GBA cores are optional:
  - armGBA on the card: the intro is the card's GBA BIOS boot logo on armGBA, or armGBA's own Open
    BIOS splash when there is no valid BIOS. GBA games default to armGBA (the in-game menu can pick
    gpSP when the card has a BIOS).
  - no armGBA, gpSP and a BIOS: the BIOS intro on gpSP, GBA games on gpSP.
  - no usable GBA core: no intro, the menu runs without GBA games.
  The volume wheel works during the intro (no OSD); hold B to cut the intro short. The intro
  plays on black by default: each frame's white is turned to black and its dark ink to light
  (`intro_on_black` in `emu/gba/gba_driver.c`), and the menu then fades in from black. The
  in-game menu's "BIOS Intro Background" row (`oem introbg:white|black`) brings back white.

## Panel timing

Every boot initialises the ST7123 exactly as the stock LK does: sync 4, back porch 40, front porch
280, PLL_CLOCK 177 (354 Mbps, integer PCW), start line 0x2C in register 0xD7. Android, recovery,
fastboot and the charging screen keep it. When the SD gate starts GammaOS Pico it retimes the running
panel (`st7123_gammaos_timing`, `ayaneo_dsi_retime`): start line 4 written to 0xD7 in video mode,
the PHY timing words for PLL 239, sync 2 / back porch 2 / front porch 769, 477.6 Mbps. That is
1739 lines at 59.7275 Hz with 9.24 ms of scan and 7.5 ms of blanking. The PLL spread spectrum
depth is a fixed PCW step worked out from the boot rate, so the retime rewrites it for the new
clock with the spread spectrum off for 1 ms (written while it runs, the refresh lands anywhere
from 59.73 to 59.79 Hz). `oem dsitime` shows the live timing, `oem palate` measures the refresh.

## What is different from the pairmini (code map)

- `project/k85v1_64_kpa.mk`: includes `k85v1_64.mk`, sets `KONKR_KPA`, the ST7123 LCM,
  `MTK_LCM_PHYSICAL_ROTATION = 90`, `BOOT_LOGO := kpa`, and forces the SD flow.
- Panel: `dev/lcm/st7123p_hd640_boe_dsi_vdo/`. 640x960 portrait scan, mounted landscape.
  Two glass vendors, chosen by the GPIO12/13 ID pins after reset; both init tables are kept.
  Name `st7123p_hd640_boe_lcm_drv` must match the kernel. Logo set `dev/logo/kpa` (960x640).
- Rotation layer in `platform/mt6785/mt_disp_drv.c` (`AYANEO_ROT`): native emulator frames are
  turned before the resizer and every GBA/GB/SNES/Genesis mode and the BIOS intro present
  through `ayaneo_rsz_present`. The menu, dialogs and transitions draw on a
  canvas the size of the logical screen (960x640, `ayaneo_canvas_back`) that
  `ayaneo_canvas_present` turns into the frame buffer in 16 row bands (3.8 ms). The in-game
  overlay, the volume OSD and the FF/RW badge are turned too. `oem rot:0|1` flips the turn
  direction live (0 = clockwise, the default).
- Menu: the engine renders at a native scale (`snes_target.base`, 0.75 on a 960 wide canvas,
  `snes_target_fit`); `emu/gba/menu/snes_menu.c` aspect 2 = 3:2 (bar shift 66.5, wallpaper
  1.56227, content 1.18519), picked by `snes_aspect_for` from the canvas ratio. Card tiles are
  cached per console type (badges) or per game with LRU slots (box art) and prefetched during a
  held scroll; the held scroll runs at 60 fps with no long frames. Host check:
  `SNES_NATIVE32=1 emu/gba/menu/host_render out/snes_pack.bin out.ppm 60 "<nav>"`
  (`SNES_CTCAP=8 GBA_BOXART=1` for the device sized cache).
- Input: `platform/mt6785/ayaneo_volume_input.c` (encoder GPIO15/GPIO6 on the Konkr, keypad on
  the pairmini) feeds the six volume sites. `ayaneo_joypad_kpa.c` replaces the pairmini analog
  ADC code (which drives GPIO15 and i2c6 0x48, both repurposed here). The encoder is decoded
  as a quadrature accumulator and sampled every 20 us inside the frame end wait. Button GPIOs
  are the same as the pairmini but the Konkr DTS labels match its shell, so under `KONKR_KPA`
  every core and the menu use A 82, B 83, X 84, Y 85, START 90, SELECT 91 (the pairmini tables
  swap these). The power on combinations are in [Boot](#boot) (`app/mt_boot/mt_boot.c`,
  `emu/gba/gba_driver.c` `ayaneo_gba_sd_boot`). `oem volstep:+N|-N` turns the wheel from USB.
- Timer: the Konkr preloader hands GPT4 over with its divider at 2 (6.5 MHz, everything ran at
  half speed and the panel looked like 120 Hz); `platform/mt6785/timer.c` re-arms it at 13 MHz.
  `oem clk` compares GPT4 with the architected timer.
- Refresh: `platform/mt6785/ddp_dsi.c` Konkr constants measured on the device: the line
  constant 76972117 is given for 353935 kHz and scaled to the live rate, 970 fixed lines, the
  shorter scan of 1739 lines at 477.6 Mbps for the GBA's 59.7275 Hz (see [Panel timing](#panel-timing));
  other refresh rates (SNES NTSC 60.1 Hz, PAL 50 Hz, the 2x modes) move the MIPI rate live
  (`ayaneo_dsi_set_dr_khz`) at the same line count.
- Pico menu: the panels fit the 640 row screen (row pitch shrinks from 38), the backdrop is
  translucent (hardware ARGB layer, the game stays live underneath). Strobe = backlight black
  frame insertion: one DISP_PWM period per frame (div 424) restarted at every RDMA frame end
  by the pacer, the brightness scales the lit pulse.
- Captures: `oem shotstage[:o]` then `fastboot -s <serial> get_staged x.bin` and
  `tools/konkr/shot2png.py x.bin x.png [--turn]` (frame buffer, game plane or the in-game
  overlay). The fastboot `upload` path sends 64512 byte requests (the QMU GPD limit). Never
  dump more than `oem shot:4` as text (a long INFO dump wedges the USB link).
- Audio: two AW88166 smart amps on i2c6 (0x34 left, reset GPIO24; 0x35 right, reset GPIO21)
  on AFE I2S3 (GPIO67/68/71 mode 1, the kernel's "I2S3" DAI is the I2S4 register block,
  48 kHz, 32 bit slots). `platform/mt6785/ayaneo_aw88166.c` replays the kernel driver's
  bring-up with the stock Music profile, DSP firmware and config that
  `tools/konkr/aw883xx_acf.py` extracts from `vendor/firmware/aw883xx_acf.bin` into
  `ayaneo_aw88166_acf.h`. `oem aw[:reinit]` reports both amps (step 12, SYSST 0x0311 = up).
- Signing: `tools/konkr/` (cert partitions carved from the stock BW03 `lk.img`, stock
  `lk_main_dtb`), `tools/ayaneo/sign_lk.py --device konkr`. Same MTK test key as the pairmini.

## Porting checklist for the next MT6785 device

1. Carve the stock `lk.img`: MTK header (magic 0x58881688) per partition `lk`, `cert1`, `cert2`,
   `lk_main_dtb`, `cert1`, `cert2`. Check the cert2 signature verifies with
   `tools/ayaneo/keys/img_pubk.pem`; if it does, drop the two cert regions and the dtb partition
   into `tools/<device>/` and sign with `--device <device>`. If it does not, the unit is not on
   the MTK test key and cannot be signed by us.
2. Decompile the DTBs (`boot.img` base DTB, `dtbo.img`) and diff against `k85v1_64`: gpio-keys,
   LCM node and reset/power GPIOs, msdc1, sound card, extra input devices.
3. Panel: find the LCM driver name in the stock LK strings, decompile `get_params` and the init
   table (Ghidra, `[DISPCHECK][LCM]` dump function confirms struct offsets), transcribe into
   `dev/lcm/<name>/`. Check `MTK_LCM_PHYSICAL_ROTATION` in the stock LK: a portrait scan needs
   the rotation layer; the logo partition images show the effective orientation.
4. Geometry: everything is derived from the logical screen (`ayaneo_screen_w/h`); a panel whose
   integer multiples of 240x160 / 160x144 differ only needs the pixel scale check in
   `rot_int_scale`. A new aspect needs a `snes_menu.c` aspect entry built from the web app's
   `setAspect` maths.
5. Refresh: FIRST check the timer (`oem clk`: GPT4 must read 13 MHz against the architected
   timer; a divided GPT4 halves every measurement), then compute the line rate from PLL, lanes
   and htotal, set the fixed line count and the default vfp, calibrate with `oem diag` and trim
   the data rate (`AYANEO_GBA_DR_KHZ`) to land on 59.7275 Hz exactly.
6. Input and audio: map the volume control and the speaker path from the DTB (gpio-keys,
   rotary-encoder, keypad matrix, smart PA nodes) and gate the pairmini specific code. Check
   the DTS button labels against the shell before trusting the pairmini swaps. For a smart PA,
   find its kernel driver source and the vendor firmware container: the ACF holds the register
   profile and the DSP image, and the bring-up is a fixed register sequence.
7. Build both projects, run the host harness, sign, and flash `lk_a` with the stock LK at hand. Drive the device from WSL with `fastboot -s <serial>`, one command at a time,
   40 s after a reboot to bootloader and 80 s after a reboot into the LK; `oem mode` tells
   which mode the LK is in (boot=99 = flash allowed).
