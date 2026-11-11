# LK (Little Kernel) for MT6785 - GammaOS handheld bootloader

A from-source MediaTek MT6785 LK bootloader, based on the
[svoboda18/lk](https://github.com/svoboda18/lk) fork, for two handhelds of the `k85v1_64` board
family:

- **Konkr Pocket Advance** (project `k85v1_64_kpa`, `./build_konkr_kpa.sh`): the main target of
  this branch (`lk-gba-emu-sd-card-kpa`). Build, flashing, the card layout, the boot rules and a
  porting checklist are in [KONKR_POCKET_ADVANCE.md](KONKR_POCKET_ADVANCE.md).
- **AYANEO Pocket Air Mini** (project `k85v1_64`): the original port, a plain LK with the ST7703
  panel (`./build_ayaneo_pocket_air_mini.sh`, sections below) and the SD build
  (`./build_ayaneo_gba_sd.sh`).

## GammaOS from the microSD card

The LK is the only thing GammaOS writes on the eMMC (`lk_a`). With a GammaOS card in the slot it
starts a console menu (a SNES Classic style carousel) and runs Game Boy, Game Boy Color, Game Boy
Advance, SNES, Mega Drive / Genesis, Master System, Game Gear and SG-1000 games from the card,
with saves, save states, run ahead, rewind, filters and per core options. Without the card, or
with the card's `gammaos/boot.cfg` set to `boot=android`, the stock Android boot runs untouched,
on the stock panel timing. Everything else lives on the card: the menu pack, the emulator cores,
the settings, the saves and the boot mode.

The emulator cores are separate programs loaded from the card (`/cores/*.blob`), each built in its
own repository, here as git submodules under `cores/`:

| Submodule | Core | Systems |
|---|---|---|
| `cores/armGBA-pico` | armGBA | Game Boy Advance (default; plays the boot intro) |
| `cores/gpsp-pico` | gpSP | Game Boy Advance (needs the card's BIOS) |
| `cores/gambatte-pico` | gambatte | Game Boy, Game Boy Color |
| `cores/snes9x-pico` | snes9x | SNES |
| `cores/genesis-plus-gx-pico` | Genesis Plus GX | Mega Drive / Genesis, Master System, Game Gear, SG-1000 |

Each one keeps the emulator's upstream as its own submodule, unmodified, with the GammaOS changes
as patches and the LK glue alongside. The LK holds only the interface headers (`*_core_abi.h`),
the loaders and the session code.

Clone with the submodules, or let the build fetch them:

```sh
git clone --recursive -b lk-gba-emu-sd-card-kpa git@github.com:TheGammaSqueeze/lk-mt6785.git
./build_konkr_kpa.sh       # out/lk_a_kpa_signed.img + out/sdcard/ (the card template)
```

`tools/ayaneo/gba/make_sdcard.sh` (run by both SD build scripts) fetches a core that is not checked
out yet, builds each core whenever its commit changed or its tree has local changes, packs the
menu and writes `out/sdcard/`. The core builds need `arm-none-eabi-gcc`/`g++` (the LK itself uses
the bundled toolchain in `gcc/`).

Known issue: the Pocket Air Mini SD build (`build_ayaneo_gba_sd.sh`) does not link at the moment;
Konkr only code (pacing, rewind, the backlight strobe counter) is referenced outside its
`KONKR_KPA` guards. The plain Pocket Air Mini LK below is unaffected.

## Pocket Air Mini: quick start

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
[BUILD_AYANEO_POCKET_AIR_MINI.md](BUILD_AYANEO_POCKET_AIR_MINI.md).

## Pocket Air Mini: flashing

Flash the `lk_a` partition (fastboot or SP Flash Tool):

```sh
fastboot flash lk_a out/lk_a_signed.img
```

WARNING: a bad LK bricks to a state that needs SP Flash Tool recovery. Keep a
stock `lk` image and SP Flash Tool ready. Recommended: flash `lk_a` only and
leave `lk_b` stock as a fallback for your first attempt.

## Pocket Air Mini: what is different on this device

The svoboda18/lk `k85v1_64` config is generic; several things had to change to
match this hardware (full detail in PORTING_NOTES.md):

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
  See [the panel voltage mods](#pocket-air-mini-panel-voltage-mods-applied-by-default) below.

## Signing

The build output is unsigned. `tools/ayaneo/sign_lk.py` grafts the device's
stock cert partitions, substitutes the stock `lk_main_dtb`, recomputes the
`cert2` hashes, and re-signs with the MTK default test image key (RSA-PSS,
salt 32). This device is fused with the MTK test keys, so the preloader accepts
the resigned image. See `tools/ayaneo/README.md`.

## Pocket Air Mini: panel voltage mods (applied by default)

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

## Layout

- `dev/lcm/st7703_hd720_dsi_vdo/` - the Pocket Air Mini panel driver + raw init-table dump
- `dev/lcm/st7123p_hd640_boe_dsi_vdo/` - the Konkr panel driver (both glass init tables)
- `emu/` - the GammaOS side of the LK: the SD gate, card FAT/exFAT, menu, input, display and
  audio drivers, and the loader + session for each core (`emu/gba`, `gbc`, `snes9x`, `genesis`)
- `cores/` - the emulator core repositories (git submodules)
- `tools/ayaneo/` - self-contained signer, keys, cert blobs, stock lk_main_dtb, the menu packer
  and the card template builder (`tools/ayaneo/gba/`); `tools/konkr/` - the Konkr cert parts
- `build_konkr_kpa.sh`, `build_ayaneo_gba_sd.sh` - LK + card template;
  `build_ayaneo_pocket_air_mini.sh` - the plain Pocket Air Mini LK
- `KONKR_POCKET_ADVANCE.md` - Konkr build, flashing, card, boot rules, code map
- `PORTING_NOTES.md` - full Pocket Air Mini bringup log (issues, fixes, untested areas)
- `BUILD_AYANEO_POCKET_AIR_MINI.md` - Pocket Air Mini build and device notes

## Licence

The Little Kernel code and the GammaOS additions are under the MIT licence ([LICENSE](LICENSE)).
The MediaTek BSP files, the stock firmware files, the SNES Classic menu assets and the emulator
cores keep their own terms; [LICENSES.md](LICENSES.md) lists which part is under what.

## Credit

Based on [svoboda18/lk](https://github.com/svoboda18/lk) (MediaTek LK, Android
10/11). The emulator cores are their authors' projects (armGBA, gpSP, gambatte, snes9x, Genesis
Plus GX); see each submodule's README and licence.
