# AYANEO Pocket Air Mini

The original port in this tree: a plain MT6785 LK for the AYANEO Pocket Air Mini (project
`k85v1_64`, ST7703 HD720 panel) that boots Android, built with `./build_ayaneo_pocket_air_mini.sh`.
GammaOS Pico for it is `./build_ayaneo_gba_sd.sh` (the LK plus the SD card template, like the
Konkr build).

## Quick start

```sh
./build_ayaneo_pocket_air_mini.sh
```

This builds `k85v1_64` with the bundled toolchain and signs the result to
`out/lk_a_signed.img` (2 MB, ready to flash). Everything needed is in the repo:
the toolchain (`gcc/`), the signing keys and cert blobs, and the stock
`lk_main_dtb` (all under `tools/ayaneo/`).

Requirements: a Linux host, Python 3, and the `openssl` CLI. No external
toolchain or SDK.

Release builds are silent (matching stock GammaOS). All bring-up logging is kept
behind one build-time toggle, off by default:

```sh
make k85v1_64 AYANEO_DEBUG_LOGGING=yes -j$(nproc)   # verbose LK + kernel UART
```

See the debug-logging section of
[build.md](build.md).

## Flashing

Flash the `lk_a` partition (fastboot or SP Flash Tool):

```sh
fastboot flash lk_a out/lk_a_signed.img
```

WARNING: a bad LK bricks to a state that needs SP Flash Tool recovery. Keep a
stock `lk` image and SP Flash Tool ready. Recommended: flash `lk_a` only and
leave `lk_b` stock as a fallback for your first attempt.

## What is different on this device

The svoboda18/lk `k85v1_64` config is generic; several things had to change to
match this hardware (full detail in [porting-notes.md](porting-notes.md)):

- ST7703 HD720 panel driver (`dev/lcm/st7703_hd720_dsi_vdo/`), with the LCM
  `.name` set to `st7703_hd720_lcm_drv` to match the kernel's compiled-in driver.
- A/B enabled (`MTK_AB_OTA_UPDATER`), with a fallback to slot `_a` because this
  device uses MediaTek `boot_ctrl_t` metadata, not AVB.
- Coprocessor loaders (SCP/SSPM/VPU/ADSP) disabled - this device has no such
  firmware partitions; the kernel loads them.
- GenieZone enabled (the device's secure OS).
- Image verification and verified-boot state forced to unlocked/orange for
  custom firmware.
- AEE watchdog debug registration skipped (hangs this early on this device).
- The stock `lk_main_dtb` is substituted at sign time (the device's kernel dtbo
  requires its nodes/symbols, e.g. `mt6360_pmu`).
- The panel is **over-driven by default** (tuned drive voltages above stock).
  See [the panel voltage mods](#panel-voltage-mods-applied-by-default) below.

## Signing

The build output is unsigned. `tools/ayaneo/sign_lk.py` grafts the device's
stock cert partitions, substitutes the stock `lk_main_dtb`, recomputes the
`cert2` hashes, and re-signs with the MTK default test image key (RSA-PSS,
salt 32). This device is fused with the MTK test keys, so the preloader accepts
the resigned image. See `tools/ayaneo/README.md`.

## Panel voltage mods (applied by default)

IMPORTANT: this build ships with the ST7703 panel **over-driven** - the drive
voltages are set above stock, ACTIVE by default in every image produced here.
This is intentional (it matches the tuned values this device was running), but
you are driving the panel harder than the manufacturer's settings.

| Register | This build | Stock | Ceiling |
|----------|-----------|-------|---------|
| VGH         | `0x78` | `0x58` | `0x78` (higher inverts the image) |
| VGL         | `0x78` | `0x58` | `0x78` |
| Charge pump | `0x48` | `0x32` | - |
| AVDD        | `0xFF` | `0xE0` | `0xFF` |
| AVEE        | `0x60` | `0x20` | `0x60` (`0x70` flickers) |

The values are the `ST7703_*` defines at the top of
`dev/lcm/st7703_hd720_dsi_vdo/st7703_hd720_dsi_vdo.c`. To run stock voltages,
set them back to the stock column and rebuild. To retune, change ONE register
at a time and verify on the panel - over-driving does NOT fail safe (it can
cause flicker, image inversion, or temporary retention/burn-in). AVEE has the
largest visible effect.
