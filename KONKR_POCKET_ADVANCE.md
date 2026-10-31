# Konkr Pocket Advance (KPA) port

MT6785 handheld, board family `k85v1_64` (same preloader name, partition map, eMMC, sub PMIC
and button GPIOs as the AYANEO Pocket Air Mini), with a different panel, a volume wheel instead
of volume keys, no analog sticks and I2S smart amps for the speaker. Stock firmware package:
`BW03_20260730` (Android 12, kernel 4.14.186, vendor `ARBOR/GT78-VN`).

## Build

    ./build_konkr_kpa.sh            # add "clean" to rebuild the core blobs too

Output in `out/`:

| File | Flash to | What it is |
|---|---|---|
| `lk_a_kpa_signed.img` | `lk_a` | the LK (2 MB), signed with the Konkr cert partitions + stock `lk_main_dtb` |
| `kpa_menu_boot_b.img` | `boot_b` | menu asset pack, boot chime, the four core blobs, settings area (32 MB) |

Nothing else is flashed. `logo`, `boot_a`, `dtbo`, `vendor`, `system` stay stock, so the device
still boots Android normally when no microSD with emulator assets is present, or after
"Boot to OS" in the menu. `lk_b` is left stock as a fallback.

The same two images carry everything the pairmini port needed, with the same layout
(`tools/ayaneo/gba/build_snes_boot_b.py`): the animation at offset 0 is unused in the SD build
(the boot visual is the real GBA BIOS intro from the card), chime at 0x01000000, menu pack at
0x01100000, core blobs from 0x01900000, persisted settings at 0x020FF000.

## Flashing

First flash on a locked stock unit: SP Flash Tool V6 (the BW03 package is a V2.2.0 scatter;
SP Flash Tool v5.1924 rejects it with S_DL_SCAT_VERSION_SUPPORTED 5079 even with the version tag
edited, so use the V6 tool with the `.xml` scatter). Load `MT6785_Android_scatter.xml` with
`lk_a` set to `lk_a_kpa_signed.img` and `boot_b` to `kpa_menu_boot_b.img`, Download Only, untick
every other partition. Keep the stock `lk.img` next to it: a bad `lk_a` is recovered the same way
by pointing `lk_a` at the stock image.

Once our LK runs, fastboot works from the menu side too (USB VID 0x0E8D PID 0x201C, serial
`0123456789ABCDEF`):

    fastboot flash lk_a   out/lk_a_kpa_signed.img
    fastboot flash boot_b out/kpa_menu_boot_b.img

Flashing over USB, learned on the first device session (2026-10-31):
- Always address the device: `fastboot -s 0123456789ABCDEF ...`. Other devices share the host.
- `fastboot reboot bootloader` from the emulator channel answers "FAILED (remote: 'unknown
  command')" but the device DOES reboot into the bootloader; the USB link then usually needs a
  cable replug before it answers. Confirm with `fastboot oem mode` (boot_mode=99 = bootloader).
- Put the device in plain bootloader mode first (`fastboot reboot-bootloader` from the running LK,
  or SELECT + START at power-on), then ONE `fastboot flash`, then ONE `fastboot reboot`. Flashing
  from the emulator-side USB channel fails ("GetVar Variable Not found" / "no link").
- Do not fire commands back to back: the LK USB gadget wedges when probed within a few seconds of
  a reboot, and a USB cable left plugged across the reboot often needs a replug before the LK
  answers again. Wait 30 s or more after a reboot before the first command.
- From WSL, `/usr/bin/fastboot` is the Windows fastboot.exe. NEVER kill fastboot.exe (user rule);
  if a command times out, stop and let the user replug the cable.
- `oem` commands are registered in both the plain bootloader and the emulator menu channel, so
  their presence does not tell the two apart.

Entering fastboot on the Konkr: hold SELECT + START while powering on (the device has no volume
keys, so the stock "Volume Up" bootloader menu does not exist). From Android: `adb reboot
bootloader`. `tools/ayaneo/gba/flash_lk.sh` and `tools/ayaneo/fastboot_push.sh` work unchanged.

## microSD card

FAT32 (or FAT16), first partition. Case insensitive names.

```
/gba_bios.bin            real GBA BIOS, exactly 16384 bytes (the boot intro; required)
/roms/gb/*.gb            /roms/gbc/*.gbc        /roms/gba/*.gba
/roms/snes/*.sfc|.smc    /roms/genesis/*.md|.gen|.bin|.smd
/roms/sms/*.sms          /roms/gg/*.gg          /roms/sg/*.sg
/roms/<system>/boxart/<romstem>.ART   optional box art (tools/ayaneo/gba/gen_boxart.py)
/saves/<system>/         /states/<system>/      created by the LK, saves and save states
```

No card, no BIOS or no ROM folder: the LK falls through to the normal Android boot. ROMs can also
be pushed over USB with `tools/ayaneo/push_roms.py <system> <files> --serial 0123456789ABCDEF`
(device in fastboot, card not held by a running menu).

## What is different from the pairmini (code map)

- `project/k85v1_64_kpa.mk`: includes `k85v1_64.mk`, sets `KONKR_KPA`, the ST7123 LCM,
  `MTK_LCM_PHYSICAL_ROTATION = 90`, `BOOT_LOGO := kpa`, and forces the SD flow.
- Panel: `dev/lcm/st7123p_hd640_boe_dsi_vdo/`. 640x960 portrait scan, mounted landscape.
  Two glass vendors, chosen by the GPIO12/13 ID pins after reset; both init tables are kept.
  Name `st7123p_hd640_boe_lcm_drv` must match the kernel. Logo set `dev/logo/kpa` (960x640).
- Rotation layer in `platform/mt6785/mt_disp_drv.c` (`AYANEO_ROT`): native emulator frames are
  turned before the resizer and every GBA/GB/SNES/Genesis mode, the BIOS intro and the boot
  animation present through `ayaneo_rsz_present`. The menu, dialogs and transitions draw on a
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
  swap these). SELECT + START at power-on enters fastboot (`app/mt_boot/mt_boot.c`).
- Timer: the Konkr preloader hands GPT4 over with its divider at 2 (6.5 MHz, everything ran at
  half speed and the panel looked like 120 Hz); `platform/mt6785/timer.c` re-arms it at 13 MHz.
  `oem clk` compares GPT4 with the architected timer.
- Refresh: `platform/mt6785/ddp_dsi.c` Konkr constants measured on the device: line rate
  77507000 (with the data rate trimmed to 353935 kHz), 1004 fixed lines, resting vfp 294 =
  59.7275 Hz exactly (`oem diag` panel/hz1000), vsync divider 1, VFP_MAX 340 (the ST7123 fades
  past that, so PAL 50 Hz is time paced at the resting porch). SNES NTSC switches to vfp 286.
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
7. Build both projects, run the host harness, sign, and flash `lk_a` only first with the stock
   LK at hand. Drive the device from WSL with `fastboot -s <serial>`, one command at a time,
   40 s after a reboot to bootloader and 80 s after a reboot into the LK; `oem mode` tells
   which mode the LK is in (boot=99 = flash allowed).
