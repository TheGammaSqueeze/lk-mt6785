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
	GBA_CONSOLE_GB      = 0,   /* /roms/gb      *.gb            - gambatte, forced GB mode (other folder names: sd_fat.c) */
	GBA_CONSOLE_GBC     = 1,   /* /roms/gbc     *.gbc           - gambatte, CGB mode */
	GBA_CONSOLE_GBA     = 2,   /* /roms/gba     *.gba           - gpSP */
	GBA_CONSOLE_SNES    = 3,   /* /roms/snes    *.sfc/.smc      - snes9x */
	GBA_CONSOLE_GENESIS = 4,   /* /roms/genesis *.md/.gen/.bin/.smd - Genesis-Plus-GX (Mega Drive) */
	GBA_CONSOLE_SMS     = 5,   /* /roms/sms     *.sms           - Genesis-Plus-GX (Master System) */
	GBA_CONSOLE_GG      = 6,   /* /roms/gg      *.gg            - Genesis-Plus-GX (Game Gear) */
	GBA_CONSOLE_SG      = 7    /* /roms/sg      *.sg            - Genesis-Plus-GX (SG-1000) */
};

/* One ROM found on the card (name kept for display + save/state matching). */
typedef struct {
	char name[128];           /* file name without its extension (truncated to 127) */
	uint32_t first_clus;
	uint32_t size;
	unsigned char type;       /* GBA_CONSOLE_* */
	unsigned char ex_nofat;   /* exFAT: one contiguous run, no FAT chain (fat_file.ex_nofat) */
	unsigned char dir;        /* the folder it is in (gba_sd_rom_dir) */
} gba_rom_entry;

/* Enumerate the ROMs into out[0..max), each tagged with its console type, merged
 * and sorted case-insensitively by name. The system folders (gba, snes, megadrive,
 * ... see k_systems in sd_fat.c) are looked for in /roms, at the card's root, and
 * in every other folder at the root and its roms folder (ES-DE's /ROMs, EmuDeck's
 * /Emulation/roms); a game in two places is listed once. Returns the count STORED
 * (<= max). If total is non-NULL it receives the TOTAL number of ROMs found, so a
 * caller can tell the list was capped (total > returned) instead of silently
 * dropping the overflow. */
int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max, int *total);

/* The folder a listed ROM is in ("/roms/gba", "/ROMs/snes", ...), from the last listing. */
const char *gba_sd_rom_dir(const gba_rom_entry *r);

/* Create the /roms/gb, /roms/gbc and /roms/gba folders on the card if missing
 * (idempotent). Returns 0 if all three exist or were created, negative if any
 * could not be created (read-only card / full FAT16 root). */
int gba_sd_make_rom_dirs(fat_vol *v);

/* Read a ROM (identified by its cached first_clus/size) into dst, up to cap bytes.
 * Returns the number of bytes read (== min(size,cap) on success). */
uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap);

#endif
