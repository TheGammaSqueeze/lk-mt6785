# GBA-from-SD on-device verification runbook

Everything in the GBA-SD feature that can be checked offline has been host-validated
(see gba-sd-card-project memory + the emu/gba/*_test.c suite run by build_host_tests.sh).
The ONE thing that cannot be verified without hardware is that the external microSD
enumerates under LK's MMC driver (mmc_wrap_bread, dev 0, user area) - RISK 1. This is
the ordered checklist to close that out the moment the target (0123456789ABCDEF) is back
on fastboot. Do NOT run any of this on a28c0e0e (a different box).

Staged image: /mnt/c/pairmini/lk_a_gba_sd_intro.img (signed, flashable to lk_a).
Flash helper: tools/ayaneo/fastboot_push.sh <image> [part=lk_a].

## 0. Prepare a FAT32 microSD (single partition)
- Format FAT32 (NOT exFAT - a >=64GB card usually ships exFAT; reformat it FAT32).
- Copy a real 16384-byte GBA BIOS to /gba_bios.bin (exact size 16384 or it is rejected).
- Create /roms/gba/ and drop one or more raw *.gba ROMs in it.
- saves/gba and states/gba do NOT need creating - the firmware makes them on first save.

## 1. Get the target into fastboot
- From a booted system: `adb reboot bootloader`; or from fastboot: `fastboot reboot bootloader`;
  or the power-on key combo. Confirm: `fastboot devices` shows 0123456789ABCDEF.

## 2. Verify SD enumeration BEFORE flashing anything (current lk already has the probe)
Run the fastboot debug commands (they work in fastboot mode; mmc is inited by platform.c):
- `fastboot oem sd-probe`
  Expect INFO lines:
    sd: mmc_wrap_bread(dev0,lba0)=1 sig=55aa b0=..        <- card read OK, MBR/VBR signature
    sd: fat_mount rc=0 fat32=1 spc=.. clusters=..         <- mounted FAT32
    sd: /gba_bios.bin OK size=16384
    sd: /roms/gba count=N (total=M)                       <- N shown, M total found
    sd: rom[0] <name> (<size>) ...
  Failure decode:
    "read=0 / card not enumerated at dev0" -> SD not on dev 0: try sd-read on other devs, or
        the MMC slot/dev number is wrong (retune SD_DEV_NUM in sd_fat.c). THIS is the RISK-1 case.
    "exFAT card not supported - reformat as FAT32" -> the card is exFAT (rc=-4). Reformat FAT32.
    "not a FAT16/32 volume" -> not FAT / corrupt. Reformat.
- `fastboot oem sd-read:0` dumps sector 0 (MBR) as hex, to eyeball the partition table if needed.
- `fastboot oem sd-wtest` writes /saves/gba/wtest.bin, remounts, reads it back, verifies:
  Expect "sd-wtest: write+verify OK" -> the WRITE path (saves/states) works on real hardware.

If sd-probe fails at the very first line (dev0 read=0), the whole feature is blocked on the
SD not enumerating; that is the only remaining unknown. Everything downstream is already proven.

## 3. Flash + boot the SD build
- `tools/ayaneo/fastboot_push.sh /mnt/c/pairmini/lk_a_gba_sd_intro.img lk_a`
- `fastboot reboot`
- With the prepared card in: the BIOS boot animation plays, then the ROM-select screen lists
  /roms/gba (D-pad up/down, A launches). Pick a ROM -> it runs.
- With NO card (or missing gba_bios.bin / empty roms/gba): the device must boot NORMALLY
  (logo partition + kernel). This is the never-brick fallback - confirm it still boots to Android.

## 4. Save/state persistence
- In a game, save a save-state (menu) and let the .sav flush (or power off via save_and_poweroff).
- Re-launch the same ROM: the save-state auto-resumes (unless B/Start held), and the .sav is intact.
- Pull the card, check on a PC: /saves/gba/<rom>.sav and /states/gba/<rom>.st0 exist and are sane.

## 5. If SD does NOT enumerate at dev 0 (RISK 1 realized)
- Try `fastboot oem sd-read:0` against different assumptions; the dev/part are compile-time
  (#define SD_DEV_NUM / SD_PART_USER in sd_fat.c) so retarget and re-flash.
- mmc_legacy_init(1) in platform.c brings up MSDC1 (the external slot; UFS is the boot device).
  If dev numbering differs, adjust SD_DEV_NUM and rebuild.

Once section 2 passes on hardware, the feature is effectively done; sections 3-4 are the
end-to-end confirmation.
