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
/* Bring up the external microSD host (msdc1) once; 0 = card present + identified. */
int gba_sd_hw_init(void);
/* Raw sector read from the microSD (call gba_sd_hw_init first). */
unsigned gba_sd_bread(uint32_t lba, uint32_t count, void *buf);
int gba_sd_load_bios(fat_vol *v, unsigned char *dst);

/* One ROM in /roms/gba (name kept for display + save/state matching). */
typedef struct {
	char name[128];           /* file name incl. .gba (truncated to 127) */
	uint32_t first_clus;
	uint32_t size;
} gba_rom_entry;

/* Enumerate *.gba files in /roms/gba into out[0..max), sorted case-insensitively
 * by name. Returns the count STORED (<= max). If total is non-NULL it receives the
 * TOTAL number of matching ROMs found, so a caller can tell the list was capped
 * (total > returned) instead of silently dropping the overflow. */
int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max, int *total);

/* Read a ROM (identified by its cached first_clus/size) into dst, up to cap bytes.
 * Returns the number of bytes read (== min(size,cap) on success). */
uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap);

#endif
