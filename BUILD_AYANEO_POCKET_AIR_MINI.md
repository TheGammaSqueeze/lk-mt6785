# Building LK for the AYANEO Pocket Air Mini (MT6785 / k85v1_64)

## Build

    make k85v1_64 -j$(nproc)

Output: `build-k85v1_64/lk.img` (unsigned, two partitions: `lk` + `lk_main_dtb`).
The bundled `gcc/` toolchain (arm-linux-androideabi, 32-bit) is used
automatically; no external toolchain is required.

## Device configuration notes

Confirmed against the official `MT6785_Android_scatter.txt` and an SP Flash
Tool dump:

- Storage: eMMC (64 GB). UFS is absent but `MTK_UFS_SUPPORT` is left `yes`
  because `platform.c` references `ufs_lk_init` unconditionally; LK auto-detects
  storage at runtime, so eMMC is used regardless.
- DRAM: 3 GB (initialised by preloader, not LK).
- Layout: A/B, `lk_a`/`lk_b` = 0x200000 (2 MB) each.
- Panel: `CUSTOM_LK_LCM="st7703_hd720_dsi_vdo"`, `BOOT_LOGO := hd720`.

## Signing (required before flashing)

The build output is unsigned. The device verifies LK against the MTK cert
chain. To produce a flashable image:

1. Graft the stock `cert1`/`cert2` partitions after the freshly built `lk` and
   `lk_main_dtb` code blocks (the stock image is a six-partition container:
   lk, cert1, cert2, lk_main_dtb, cert1, cert2).
2. Re-sign each `cert2`: recompute the data and header SHA-256 hashes, then sign
   the TBS with `img_prvk.pem` using RSA-PSS, salt length 32. cert1 (root) is
   reused verbatim. Hashes are stored as BIT STRING, not OCTET STRING.
3. Pad to the 2 MB `lk_a` partition size. Do not resize beyond it.

Flash `lk_a` (and optionally `lk_b`) with fastboot or SP Flash Tool.

WARNING: a source-built LK is unverified on hardware until you flash it. A bad
LK bricks to a state that needs SP Flash Tool recovery. Validate at your own
risk.

## Behavioral changes vs stock

To match the patched stock lk this device ran, the source carries:

- Unlock by default: `platform/mt6785/load_image.c` forces
  `lock_state = DEVICE_STATE_UNLOCKED` after the seccfg query, so custom or
  unsigned images boot (orange state) without verification blocking.
- No orange-state message, timeout or wait: `orange_state_warning()` in
  `platform/common/boot/vboot_state.c` returns immediately.
- No dm-verity corruption check: `show_dm_verity_error()` in
  `platform/common/boot/avb20/dm_verity.c` returns immediately, so the device
  never shows the corruption screen or powers off.
- Tuned panel drive voltages: VGH/VGL 0x78, charge pump 0x48, AVDD 0xFF,
  AVEE 0x60 (see the panel driver defines).
- Kernel command line parity (required to keep the encrypted `/data`, see the
  lessons-learned section): `app/mt_boot/mt_boot.c` appends
  `androidboot.fstab_suffix=emmc` and the stock `ramoops.*` pstore carve-out,
  and `platform/common/partition/part_common.c` emits the full stock
  `androidboot.boot_devices` list (eMMC + UFS, SoC-path and raw forms).
- Root of Trust parity: `platform/common/RoT/avb_RoT.c` sends the RoT SMC
  (`MTK_SIP_LK_ROOT_OF_TRUST` 0x82000120) exactly like stock and forces
  `device_locked = 0`, matching the binary-patched lk (which fakes the seccfg
  lock_state to 3/unlocked at its source).

## Debug logging toggle

Release builds are silent, matching stock GammaOS (both the LK console and the
kernel UART are quiet). All bring-up logging is kept behind a single build-time
toggle, `AYANEO_DEBUG_LOGGING` (default `no` in `project/k85v1_64.mk`):

    ./build_ayaneo_pocket_air_mini.sh                      # release, silent
    make k85v1_64 AYANEO_DEBUG_LOGGING=yes -j$(nproc)      # verbose bring-up

When enabled it raises the LK `DEBUGLEVEL` (`include/debug.h`), turns on
`pal_log` err/warn/info (`platform/pal/inc/pal_log.h`), adds the kernel cmdline
`printk.disable_uart=0 mtk_printk_ctrl.disable_uart=0 ignore_loglevel`
(`ignore_loglevel` is required because the base bootargs carry `loglevel=0`),
and compiles in the `AYANEO_ROT:` Root-of-Trust trace. None of this is present
in a release build.

## Animated boot logo (experiment)

Gated behind `AYANEO_RAINBOW_BOOT` (default `yes` on the animated-boot-logo
branch): instead of blitting the static eMMC logo, paint a smooth scrolling
rainbow over the whole panel during LK.

How it works (`platform/mt6785/mt_disp_drv.c`):

- Runs on its own LK thread so boot proceeds in parallel; stopped in
  `boot_linux_fdt()` just before the kernel display handoff.
- The rainbow is rendered ONCE into a buffer one 256-row period taller than the
  screen; each frame just advances the OVL read address by a row offset
  (`primary_display_config_input`), so there is no per-frame pixel work - it
  tracks the panel refresh with almost no CPU. The pattern repeats every 256
  rows so the wrap is seamless.
- The scroll offset is derived from `current_time()` so the speed is constant
  regardless of loop rate. The backlight is enabled at the first frame (the boot
  flow otherwise only enables it after the logo call).

Tunables (defines, overridable at build time):

- `AYANEO_RAINBOW_MS_PER_ROW` (6) - ms per scrolled row; lower is faster.
- `AYANEO_RAINBOW_LOOP_MS` (14) - loop pacing (~vsync); boot runs during the sleep.
- `AYANEO_RAINBOW_MIN_MS` (4000) - guaranteed on-screen time; LK reaches the
  kernel handoff in ~1s, so this holds at the handoff (still scrolling) until it
  elapses. Bounded, tunable added boot latency; `0` = run for boot's natural length.

Notes: LK is single-core, so the animation shares CPU0 with boot; it runs at
`HIGH_PRIORITY` with a guaranteed per-frame `thread_sleep()` so it cannot
spin-starve the boot thread (which would trip the watchdog). It cannot animate
past the kernel jump - once LK hands off, its final frame simply holds on the
video-mode panel until the kernel blits.

## One-step build and sign

    ./build_ayaneo_pocket_air_mini.sh

Produces `out/lk_a_signed.img` (2 MB, flashable). Everything needed is in the
repo: the bundled toolchain, the MTK default test keys and the device cert
blobs (`tools/ayaneo/`). Flash with `fastboot flash lk_a out/lk_a_signed.img`.

## GenieZone (secure OS) note

The device's secure OS is GenieZone (GZ), confirmed by the preloader/ATF UART
log printing GZ params, not Google Trusty, so `MTK_GOOGLE_TRUSTY_SUPPORT` stays
`no`. `MTK_ENABLE_GENIEZONE` is left `no`: the kernel line
`GZ SMC version(0) is not supported for MTEE 0/1 ... Failed to init smc number
table` prints on the **stock** boot too (see `stocklk_capture/recovery_dmesg.txt`)
and is harmless; GZ is loaded by ATF, not lk, so this flag was not the cause of
any boot problem.

## Lessons learned: the userdata-wipe investigation

The from-source lk booted the kernel to the boot animation, then vold rejected
the existing encrypted `/data` and rebooted to format it. The real cause was a
single missing kernel-cmdline token. The path to finding it is worth recording,
because most of it was chasing the wrong thing.

The failure signature was identical every time: init tried to apply FBE key
`d0a46c5f...` to `/data` subdirectories and got *"the directory already has a
different encryption policy"*, then `vold` (with `reboot_on_failure`) rebooted.

What it was **not** (each ruled out with evidence):

- **Keymaster / Root of Trust.** The RoT SMC content is invariant to the derived
  FBE key: the same `d0a46c5f` appeared whether the SMC was sent or not, and with
  `device_locked` 0 or 1. A known-good boot uses the *same* key `d0a46c5f`.
- **vbmeta digest.** The RoT carried `eb503bd2` while stock is `9fd8df67`, but the
  key is digest-independent (the working boot has `9fd8df67`, ours `eb503bd2`,
  same key). `eb503bd2` was simply the digest of the *debug* boot.img flashed for
  logging (computed offline: `SHA256(vbmeta || vbmeta_system || vbmeta_vendor ||
  boot)` in chain order); the real GammaOS boot yields `9fd8df67`.
- **GZ SMC version(0)**, **SSMR "failed to allocate"**, **tee_reserved_mem**:
  all print on the stock boot as well (`stocklk_capture/`); red herrings.

What it actually was: the fscrypt policy **flags** differed, not the key. The
working boot logs `... encryption policy d0a46c5f... flags 0x2`; ours logged
`flags 0xa`. `0xa = 0x2 | 0x8`, where `0x8` is `IV_INO_LBLK_64`, the
`inlinecrypt_optimized` flag. The vendor ships two fstab files:

- `fstab.emmc` -> `fileencryption=aes-256-xts:aes-256-cts:v2` -> flags `0x2`
- `fstab.mt6785` -> `...v2+inlinecrypt_optimized` -> flags `0xa`

vold picks the file by `androidboot.fstab_suffix`. Stock lk sets it to `emmc`
(-> `fstab.emmc` -> `0x2`); our lk never set it, so vold fell back to
`fstab.${ro.hardware}` = `fstab.mt6785` (-> `0xa`), which does not match the
existing `/data` (encrypted with `0x2`). Adding `androidboot.fstab_suffix=emmc`
to the cmdline fixed it. Confirmed on the live device: `ro.boot.fstab_suffix=emmc`,
`ro.hardware=mt6785`, and after the fix `init: Verified that /data/... policy
d0a46c5f... flags 0x2` with `/data` intact.

Method that finally worked: build a logging copy of the *working* binary-patched
lk (patch its cmdline `disable_uart` bytes and DTB `bootargs`, re-sign in place),
boot it, and diff its kernel log against ours line by line. The one-token
difference in the policy-flags line was the whole bug.

General takeaways for this platform:

- The FBE key-encryption key is derived from a hardware secret, independent of
  the lk-supplied RoT and vbmeta digest; do not assume the RoT feeds it.
- fscrypt "different encryption policy" with a matching key is a *flags*
  mismatch, driven by the fstab entry, i.e. by `androidboot.fstab_suffix`.
- A modified boot.img changes only the vbmeta digest, not the FBE key, so it is
  safe to use a logging boot.img for bring-up; it will not by itself trigger a
  wipe.
