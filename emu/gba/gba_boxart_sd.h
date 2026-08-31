/*
 * SD-backed box art loader (LK-only; pulls in FAT + the menu pack image type).
 * Reads /roms/gba/boxart/<romstem>.ART for a ROM and decodes it into a caller
 * DRAM slot as a ready-to-draw RGB565 image. Kept separate from the pure,
 * host-tested gba_boxart.c decoder.
 */
#ifndef GBA_BOXART_SD_H
#define GBA_BOXART_SD_H

#include "gba_sd_save.h"    /* fat_vol, gba_sd_read_named */
#include "menu/snespack.h"  /* snes_img_entry, SNES_IMG_RGB565 */

/*
 * Load ROM `romname`'s box art. `scratch`/`scratch_cap` is a temporary buffer the
 * raw .ART is read into; the decoded 3-byte (565+a8) pixels are written to
 * `dst`/`dst_cap`, which the caller guarantees equals (pack_base + pixels_off) so
 * `out->pixels` can be resolved by snes_img_pixels. On success fills `out` (w, h,
 * flags, pixels_off) and returns 0; returns negative if the tile is absent or
 * invalid (the caller then draws the placeholder cart).
 */
int gba_boxart_load_sd(fat_vol *v, const char *romname,
		       unsigned char *scratch, unsigned scratch_cap,
		       unsigned char *dst, unsigned dst_cap, unsigned pixels_off,
		       snes_img_entry *out);

#endif /* GBA_BOXART_SD_H */
