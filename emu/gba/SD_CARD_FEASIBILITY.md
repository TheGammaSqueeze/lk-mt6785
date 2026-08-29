# GBA-from-SD-card: feasibility assessment (branch lk-gba-emu-sd-card)

Goal: replace the boot-animation + boot_b assets with an SD-card flow:
1. On boot, run `gba_bios.bin` in the emulator (nearest-neighbour zoom) as the "intro".
2. Then a ROM-select screen listing `roms/gba/` on the SD card.
3. Selecting a ROM launches it. Saves -> `saves/gba/<name>.sav`, states -> `states/gba/<name>.st*`,
   matched by ROM file name.
4. All assets load from the SD card; nothing from boot_b.
5. If NO SD card, or SD present but assets missing -> fall through to NORMAL boot (logo partition + kernel).

## VERDICT: FEASIBLE. One significant new component (a FAT filesystem layer). Everything else reuses
existing, working infrastructure. The normal-boot fallback makes it safe to develop incrementally.

## What already exists (no new work)
- **SD block access is ALREADY LIVE.** LK compiles + links the full MTK MMC/SD stack
  (platform/common/storage/mmc/: msdc.c, mmc_core.c, mmc_common_inter.c) gated by
  MTK_MMC_COMBO_DRV=yes, and mt6785/platform.c ALREADY calls `mmc_legacy_init(1)` at platform init.
  MMC_SLOT=1 = the external microSD (MSDC1); the device boots UFS so MSDC1 is free for the card.
  Block-read API present + linked: `mmc_wrap_bread(dev, blknr, blkcnt, dst, part_id)` (and mmc_bread).
  => reading raw 512B sectors off the microSD needs NO new driver, only device verification that the
     card enumerates (untestable right now - target is off adb).
- **gpSP GBA core (libgpsp.a) + gba_driver.c**: ROM run, save state, .sav backup, input, audio, and
  the 240x160 display are all working. Today they source the ROM/state/sav from boot_b via
  partition_read/partition_write - we just swap that SOURCE to SD files.
- **Display / nearest-neighbour zoom**: mt_disp_drv.c already scales the GBA 240x160 frame at integer
  5x -> 1200x800 centred on the 1280x960 panel (nearest-neighbour, table lookup). GBA is 3:2, the panel
  4:3, so 5x integer (letterboxed) is the max clean fit; "filling" the panel would distort the aspect.
  gba_bios.bin renders through this same path (the BIOS is a normal 240x160 GBA program).
- **Boot hook**: app/mt_boot/mt_boot.c ~734 (in boot_linux). Today: `if AYANEO_GBC/GBA -> ayaneo_gbc_start(); for(;;)`
  i.e. run the emulator forever INSTEAD of the kernel. The code AFTER that #if is the normal kernel boot,
  so the fallback path already exists - the emulator path just needs to become CONDITIONAL and `return`
  (fall through) when the SD/assets are absent.

## The one real new component: a FAT filesystem layer
- **LK has NO working filesystem.** lib/fs/fs.c is a skeleton: `#if WITH_LIB_FS_FAT32/EXT2` includes to
  fat32.c/ext2.c that are ABSENT from the tree, the flags are unset, and the API has open/read/stat by
  path but NO directory listing (no readdir). So we must add a filesystem reader.
- **Filesystem choice (single partition):**
  - FAT32 (RECOMMENDED): simplest to implement (~600-900 LOC for read+readdir), universally writable from
    the user's PC. 4 GB max file size and 2 TB volume - both irrelevant here (GBA ROMs <=32 MB, saves/states
    tiny). Note: Windows caps FAT32 *format* at 32 GB, but reads any size and third-party tools format larger.
  - exFAT: only needed for >4 GB single files (N/A) or if cards ship exFAT by default (common >=64 GB).
    ~1.5-2x the FAT32 effort. Optional follow-up if users' large cards come exFAT.
  - ext2/4: NOT natively writable on Windows/macOS -> bad UX for dropping ROMs. Avoid.
  => Target FAT32 (+ FAT16 for tiny cards, nearly free). Add exFAT later only if needed.
- **Read is the easy half** (mount BPB, FAT-chain walk, root+subdir readdir, file read). ~1-2 days.
- **Write is the harder half** (saves/states): FAT cluster allocation + FAT-table update + dirent
  create/resize + flush. Correctness-critical (save corruption risk). ~2-3 days. Mitigations: write to a
  temp file then rename, flush FAT + dirent + backup-FAT, keep the access pattern simple (one open file).

## Proposed boot flow (mt_boot.c hook)
1. `mmc_wrap_bread` a few sectors -> probe for a FAT partition on the microSD (MBR/VBR).
   Fail (no card / not FAT / read error) -> `return` to normal kernel boot.
2. Mount, check `gba_bios.bin` + `roms/gba/` exist. Missing -> `return` to normal boot.
3. Run `gba_bios.bin` in the emulator as the intro (fixed duration or until settled).
4. ROM-select screen: readdir `roms/gba/`, list .gba/.gz, D-pad + A to choose (reuse the menu render
   primitives / a simple list drawer).
5. Load the chosen ROM from SD into the gpSP arena; on load, read `saves/gba/<name>.sav` and the newest
   `states/gba/<name>.st*` if present. On save/exit, write back to SD (needs FAT write).
   Runs forever (never boots the kernel).
Any failure at any step falls back to normal boot -> the device is never bricked into a non-booting state.

## Effort / risk
- FAT read + readdir: ~1-2 days. FAT write: ~2-3 days. ROM-select UI: ~1-2 days. Boot integration +
  BIOS-intro + fallback: ~1 day. SD-enumeration verification + tuning: needs the device. Total ~1.5-2 weeks.
- RISK 1 (primary): the microSD enumerating under mmc_wrap_bread is UNVERIFIED on this unit (target off adb).
  The driver is present + proven on MTK, and platform.c already inits it, so risk is low-moderate, but it
  is the one thing that must be confirmed on hardware before the rest is worth wiring.
- RISK 2: FAT write correctness (save integrity) - the main implementation care-point.
- RISK 3: exFAT-formatted large cards won't mount a FAT32-only reader -> falls through to normal boot
  (safe, but the feature is inert until the card is FAT32 or exFAT support is added).

## Recommended order
1. Verify the microSD reads via mmc_wrap_bread on-device (dump sector 0, confirm MBR/FAT signature).
2. FAT32 read + readdir. 3. Boot-flow gate + gba_bios.bin intro + ROM-select + ROM load (read-only saves).
4. FAT32 write for save/state persistence. 5. (optional) exFAT.
Steps 2-3 are fully host/offline-implementable; step 1 and final on-device polish need the target on adb.

## Cache-coherency of the SD read/write path (verified 2026-08-29, source analysis)
The boot_b saver (sav_save) calls arch_clean_cache_range before partition_write, which raised the question of
whether the SD path (mmc_wrap_bread/bwrite via fat_ro/fat_wr) also needs caller-side cache maintenance on its
transfer buffers. It does NOT. The MMC driver self-maintains: platform/common/storage/mmc/msdc_dma.c wraps every
DMA transfer with msdc_flush_membuf() -> arch_clean_invalidate_cache_range() on the data buffer (msdc_dma.c:251,
281) plus the descriptor buffers. mt6785 enables MSDC_ENABLE_DMA_MODE (msdc_cfg.h:93). If MSDC ever runs PIO
(msdc_pio_bread/bwrite), the CPU does the copy through cached accesses and no maintenance is needed either. So
fat_wr's unflushed stack sector buffers are safe on device by construction; adding arch_clean_cache_range around
mmc_wrap_bwrite would be redundant (the boot_b clean is for the separate UFS/partition path). No code change
needed - this is a negative result recorded so the on-device bring-up does not chase a non-bug.
