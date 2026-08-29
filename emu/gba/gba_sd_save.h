/*
 * Save/state persistence to the microSD, matched to the ROM file name:
 *   <rombase>.gba  ->  saves/gba/<rombase>.sav , states/gba/<rombase>.st<slot>
 * Thin glue over fat_ro (read) + fat_wr (write). The emu save/load path calls
 * these when running an SD ROM; the gpSP state/sav BUFFERS are unchanged.
 */
#ifndef GBA_SD_SAVE_H
#define GBA_SD_SAVE_H

#include "fat_ro.h"

/* Read <dir>/<rombase>.<ext> into dst (<= cap). Returns bytes read, 0 if absent. */
uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname,
			   const char *ext, unsigned char *dst, uint32_t cap);

/* Write <dir>/<rombase>.<ext> with len bytes (create or replace). 0 = ok. */
int gba_sd_write_named(fat_vol *v, const char *dir, const char *romname,
		       const char *ext, const void *buf, uint32_t len);

/* Convenience wrappers for the fixed layout. slot 0..9 -> ".st0".."st9". */
uint32_t gba_sd_load_sav(fat_vol *v, const char *rom, unsigned char *dst, uint32_t cap);
int      gba_sd_write_sav(fat_vol *v, const char *rom, const void *buf, uint32_t len);
uint32_t gba_sd_load_state(fat_vol *v, const char *rom, int slot, unsigned char *dst, uint32_t cap);
int      gba_sd_write_state(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len);

#endif
