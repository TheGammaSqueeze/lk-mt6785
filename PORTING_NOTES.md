# AYANEO Pocket Air Mini (MT6785 / k85v1_64) LK Porting Notes

This documents the full process of getting an LK (Little Kernel) bootloader
built from source (the svoboda18/lk fork) to boot the AYANEO Pocket Air Mini,
including every failure encountered, its root cause, and the fix. It also lists
the known-risk and untested areas.

Status: the device boots fully to the OS (GammaOS) with the resulting image.

## Device summary

- SoC: MediaTek MT6785 (Helio G95), project name `k85v1_64`.
- Storage: eMMC (64 GB). No UFS.
- DRAM: 3 GB (initialised by preloader).
- Layout: A/B, `lk_a`/`lk_b` = 0x200000 (2 MB) each. No dedicated recovery
  partition (recovery-as-boot). Virtual A/B via `super`.
- Secure OS: GenieZone (GZ), not Google Trusty.
- Secure boot: fused on (`sboot_state = 1`). Signed with MTK default test keys
  (preloader accepts our resigned LK).
- Panel: ST7703 HD720, 1280x960 DSI video mode, 4 lanes, RGB888.
- PMIC: MT6359 (main) + MT6360 (sub).

## Method / toolchain

- Base: `svoboda18/lk` fork. `platform/mt6785`, `project/k85v1_64.mk`,
  `target/k85v1_64`, and a bundled 32-bit `gcc/` toolchain all present.
- Build + sign in one step: `./build_ayaneo_pocket_air_mini.sh` ->
  `out/lk_a_signed.img`. See `tools/ayaneo/` for the self-contained signer.
- Signing: the build output is unsigned. The signer grafts the device's stock
  `cert1`/`cert2` partitions after the built code, substitutes the stock
  `lk_main_dtb` (see below), recomputes each `cert2` data+header SHA-256, and
  re-signs with the MTK test image key (RSA-PSS, salt 32). cert1 (root) is
  reused verbatim. Padded to the 2 MB `lk_a` size.

## Debugging technique

Stock LK and the fork both keep LK console output on UART0 (0x11002000, the
same port the preloader/ATF use) but with logging disabled, so LK is silent by
default. During bringup we:

1. Re-enabled LK logging (`include/debug.h` DEBUGLEVEL, `platform/pal/inc/pal_log.h`).
2. Added raw UART markers (`ayaneo_mark`) that write directly to UART0 before
   LK's own console is up, to bisect very early hangs.
3. Forced the kernel UART on (`mtk_printk_ctrl.disable_uart=0`) to see kernel
   panics.

All of these are reverted in the release build (LK quiet, kernel UART off).

## The failure chain (each was a distinct boot blocker)

Every issue below was the fork's generic config/code not matching this specific
device. The failure point marched forward with each fix.

### 1. Silent instant reboot before LK console

- Symptom: LK entered (`Entry point 0x4c400000`) then AP watchdog reset with no
  output, even with logging on. Markers showed it died between reaching
  `platform_early_init` and `uart_init_early`.
- Root cause: `lk_register_wdt_callback()` (AEE watchdog crash-dump helper) does
  an SMC (`MTK_SIP_LK_WDT`) then `arch_mmu_map`s the returned address; this hangs
  this early on this device.
- Fix: `#if 0` out the `lk_register_wdt_callback()` call in
  `platform/mt6785/platform.c`. Non-essential (only maps an ATF crash-dump
  region).

### 2. Panic in the MediaTek seclib (image verification)

- Symptom: `[SECLIB_IMG_VERIFY] img_read failed at 0x0`,
  `panic ... app/mt_boot/sec/img_utils.c:109`. Happened verifying the boot logo.
- Root cause: secure boot is fused on, so LK tries to verify the logo (and later
  boot/dtbo) through the precompiled seclib, which fails on this device.
- Fix: `get_vfy_policy()` in `platform/mt6785/sec_policy.c` returns 0 (never
  require verification). This is the from-source equivalent of the stock unlock
  + verification bypass; appropriate for an unlocked/custom-firmware device.

### 3. Panic: "LK driver's overlayed dtb is not initialized"

- Root cause: `lk_dtb_init()` failed because `load_lk_dtb()` returned -2
  (`-ENOENT`): `get_lk_part_name()` returned `lk` (no slot suffix) because
  `MTK_AB_OTA_UPDATER` was `no`, but this is an A/B device with `lk_a`/`lk_b`.
- Fix: `MTK_AB_OTA_UPDATER = yes` in the project. Pulls in the bootctrl module
  and enables slot-suffix handling; also enables `RECOVERY_AS_BOOT` (correct,
  no recovery partition).

### 4. A/B slot lookup returns NULL

- Symptom: `[LK] input suffix is NULL`, `p_AB_suffix: <null>`, dtb load -2 again.
- Root cause: bootctrl 2.0 `get_suffix()` reads the misc A/B metadata as AVB
  format (magic `\0AB0`), but this device stores it in the MediaTek `boot_ctrl_t`
  v1 format (magic `0x19191100`) - confirmed by cross-referencing stock LK,
  whose string is `BOOTCTRL_MAGIC` (numeric), not `AVB_AB_MAGIC`.
- Fix: `get_suffix()` in `platform/common/bootctrl/2.0/bootctrl_api.c` falls back
  to `_a` when the AVB parse fails. The device booted slot `_a` (preloader loaded
  `lk_a`). See "Untested areas" for the `_b` caveat.

### 5. Panic: dtbo overlay fails ("Couldn't find 'mt6360_pmu' symbol in main dtb")

- Root cause: the fork builds a generic `lk_main_dtb` that lacks this device's
  nodes/symbols. The device's kernel `dtbo_a` references `mt6360_pmu`, which the
  generic dtb doesn't have, so `ufdt_apply_overlay()` fails.
- Fix: substitute the stock `lk_main_dtb` (extracted from the stock lk image;
  139444 bytes, has `mt6360_pmu` + `__symbols__`) into the built image at sign
  time. It is device data, not code; the from-source LK reads it fine. This
  first brought up the display and the boot logo.

### 6. Reboot in the audio DSP (ADSP) init

- Symptom: `[ADSP] load_adsp_image fail hifi3_a_iram`, then `disable_adsp_hw()`
  crashed/reset.
- Root cause: the fork enabled all coprocessor loaders (SCP, SSPM, VPU, ADSP);
  this device has no such firmware partitions, and the kernel loads them later.
  Cross-referencing stock LK: it has zero coprocessor loader code.
- Fix: disable `MTK_TINYSYS_SCP_SUPPORT`, `MTK_TINYSYS_SSPM_SUPPORT`,
  `MTK_VPU_SUPPORT`, `MTK_AUDIODSP_SUPPORT` in the project, matching stock. Also
  fixed a latent include bug: `aee_platform_debug.c` had `#include <arch/arm/mmu.h>`
  nested under the SSPM `#ifdef`, but the PICACHU code needs it unconditionally.

### 7. Silent kernel reboot: verified-boot state

- Symptom: LK completed and jumped to the kernel; the kernel reset silently.
- Root cause: the AVB20 boot path generates its own cmdline independent of the
  `load_image.c` unlock patch. It read `lock_state = 0x1` and emitted
  `device_state=locked` + `verifiedbootstate=green`; the kernel then enforced
  verified boot on the modified images and reset. The working stock boot uses
  `device_state=unlock` + `verifiedbootstate=orange`.
- Fix: force unlocked in the two AVB consumers:
  `avb_hal_read_is_device_unlocked()` (-> `device_state=unlocked`) and
  `load_vfy_boot_ab.c` (-> `g_boot_state = BOOT_STATE_ORANGE` ->
  `verifiedbootstate=orange`). `veritymode=enforcing` is unchanged (same as the
  working stock boot).

### 8. Kernel Oops in mtkfb: LCM name mismatch (final blocker)

- Symptom (with kernel UART on): `FATAL ERROR! LCM Driver defined in
  kernel(st7703_hd720_lcm_drv) is different with LK(st7703_hd720_dsi_vdo)`,
  `disp_lcm_probe returns null`, NULL deref in `layering_rule_init`.
- Root cause: the kernel has the panel driver compiled in as
  `st7703_hd720_lcm_drv` and requires LK to report the exact same LCM name (LK
  passes it to the kernel via the `videolfb` handoff). Our driver's `.name` was
  `st7703_hd720_dsi_vdo`.
- Fix: set the `.name` field of the LCM driver to `st7703_hd720_lcm_drv`
  (the stock name). The directory/struct name can stay; only `.name` matters.

## Config changes vs the fork's stock `k85v1_64.mk`

- `MTK_AB_OTA_UPDATER`: no -> yes
- `MTK_TINYSYS_SCP_SUPPORT`: yes -> no
- `MTK_TINYSYS_SSPM_SUPPORT`: yes -> no
- `MTK_VPU_SUPPORT`: yes -> no
- `MTK_AUDIODSP_SUPPORT`: yes -> no
- `MTK_ENABLE_GENIEZONE`: (unset) -> yes  (device uses GZ; passes mblock info to
  GZ via SMC before kernel jump, matches stock)
- `CUSTOM_LK_LCM`: FHD+ panels -> `st7703_hd720_dsi_vdo`
- `BOOT_LOGO`: fhdplus -> hd720
- `MTK_UFS_SUPPORT`: left `yes` (platform.c references `ufs_lk_init`
  unconditionally; LK auto-detects storage, uses eMMC)

## Panel

- Driver: `dev/lcm/st7703_hd720_dsi_vdo/`. Geometry recovered from the stock LK
  `get_params` by disassembly; init table transcribed from the stock 171-entry
  DSI table (`st7703_hd720_init_dump.txt`).
- `.name` MUST be `st7703_hd720_lcm_drv` (kernel handshake, see issue 8).
- Drive voltages exposed as `ST7703_*` defines, set to the device's tuned values
  (VGH/VGL 0x78, charge pump 0x48, AVDD 0xFF, AVEE 0x60; stock 0x58/0x58/0x32/
  0xE0/0x20). Tune ONE at a time and verify; over-driving does not fail safe.

## Untested / known-risk areas

- Slot B: `get_suffix()` hard-falls-back to `_a`. If the device ever boots slot
  `_b`, LK would load `_a` images. Proper fix: read the MediaTek `boot_ctrl_t`
  v1 metadata (magic `0x19191100`) like stock, or use the fork's bootctrl 1.0.
- Image verification is fully disabled (`get_vfy_policy` returns 0). Intended for
  unlocked/custom firmware; do not use as-is where verified boot is required.
- Coprocessors (SCP/SSPM/VPU/ADSP) are not loaded by LK. The kernel loads them;
  this matches stock, but any LK-stage feature depending on them is off.
- The other coprocessor/tinsys/PMIC options in `k85v1_64.mk` are the fork's
  generic template and were not each individually validated beyond "the device
  boots and runs".
- OTA / slot switching, fastboot flows, and charging/off-mode charging were not
  exercised.
- Signing uses MTK default test keys; only valid because this unit is fused with
  those test keys.
