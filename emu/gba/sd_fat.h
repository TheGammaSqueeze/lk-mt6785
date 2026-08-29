/*
 * On-device glue: mount the external microSD as a FAT volume using the LK MMC
 * block driver (mmc_wrap_bread). Host builds use their own file-backed callback
 * (see fat_ro_test.c); this file is LK-only.
 */
#ifndef SD_FAT_H
#define SD_FAT_H

#include "fat_ro.h"

/* Mount the external microSD (mmc device 0, user area). Returns 0 on success,
 * negative if the card is absent / unreadable / not FAT16-32. On failure the
 * caller falls through to the normal kernel boot. */
int gba_sd_mount(fat_vol *v);
int gba_sd_load_bios(fat_vol *v, unsigned char *dst);

#endif
