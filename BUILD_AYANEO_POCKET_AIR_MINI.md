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

## One-step build and sign

    ./build_ayaneo_pocket_air_mini.sh

Produces `out/lk_a_signed.img` (2 MB, flashable). Everything needed is in the
repo: the bundled toolchain, the MTK default test keys and the device cert
blobs (`tools/ayaneo/`). Flash with `fastboot flash lk_a out/lk_a_signed.img`.

## GenieZone (secure OS) parity

The device's secure OS is GenieZone (GZ), confirmed by the preloader/ATF UART
log printing GZ params, not Google Trusty. So MTK_GOOGLE_TRUSTY_SUPPORT stays
`no`, and MTK_ENABLE_GENIEZONE is set to `yes`. That restores two stock lk
behaviours: parsing the modem-MTEE shared-memory boot tag, and passing lk's
mblock memory layout to GZ via SMC (MTK_SIP_LK_MEM_INFO_SET) just before the
kernel jump.
