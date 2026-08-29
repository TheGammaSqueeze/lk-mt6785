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

/* One ROM in /roms/gba (name kept for display + save/state matching). */
typedef struct {
	char name[128];           /* file name incl. .gba (truncated to 127) */
	uint32_t first_clus;
	uint32_t size;
} gba_rom_entry;

/* Enumerate *.gba files in /roms/gba into out[0..max), sorted case-insensitively
 * by name. Returns the count (<= max). */
int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max);

#endif
