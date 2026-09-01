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
/* Raw return code of the last microSD host init (mmc error code) - diagnostics. */
int gba_sd_hw_rc(void);
/* Raw sector read from the microSD (call gba_sd_hw_init first). */
unsigned gba_sd_bread(uint32_t lba, uint32_t count, void *buf);
int gba_sd_load_bios(fat_vol *v, unsigned char *dst);

/* Console type of a ROM entry - which core plays it and which badge the card
 * shows. Ordered GB, GBC, GBA (see GBA_CONSOLE_* -> logo asset keys). */
enum {
	GBA_CONSOLE_GB  = 0,   /* /roms/gb   *.gb   - gambatte, forced GB mode */
	GBA_CONSOLE_GBC = 1,   /* /roms/gbc  *.gbc  - gambatte, CGB mode */
	GBA_CONSOLE_GBA = 2    /* /roms/gba  *.gba  - gpSP */
};

/* One ROM in /roms/{gb,gbc,gba} (name kept for display + save/state matching). */
typedef struct {
	char name[128];           /* file name incl. extension (truncated to 127) */
	uint32_t first_clus;
	uint32_t size;
	unsigned char type;       /* GBA_CONSOLE_GB / _GBC / _GBA */
} gba_rom_entry;

/* Enumerate the ROMs across /roms/gb (*.gb), /roms/gbc (*.gbc) and /roms/gba
 * (*.gba) into out[0..max), each tagged with its console type, merged and sorted
 * case-insensitively by name. Returns the count STORED (<= max). If total is
 * non-NULL it receives the TOTAL number of matching ROMs found, so a caller can
 * tell the list was capped (total > returned) instead of silently dropping the
 * overflow. */
int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max, int *total);

/* Create the /roms/gb, /roms/gbc and /roms/gba folders on the card if missing
 * (idempotent). Returns 0 if all three exist or were created, negative if any
 * could not be created (read-only card / full FAT16 root). */
int gba_sd_make_rom_dirs(fat_vol *v);

/* Read a ROM (identified by its cached first_clus/size) into dst, up to cap bytes.
 * Returns the number of bytes read (== min(size,cap) on success). */
uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap);

#endif
