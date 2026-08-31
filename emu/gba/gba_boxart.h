/*
 * Per-ROM box art for the GBA menu. Tiles live on the SD card as compact .ART
 * files (/roms/gba/boxart/<romstem>.ART, see tools/ayaneo/gba/BOXART.md): a 12-byte
 * "GART" header + 2-byte-per-pixel RGB565. The menu engine draws RGB565 images as
 * 3 bytes per pixel (565 low, 565 high, a8), so loading expands 2 -> 3 bytes.
 */
#ifndef GBA_BOXART_H
#define GBA_BOXART_H

#define GBA_ART_MAGIC 0x54524147u   /* "GART" little-endian */

/*
 * Validate a .ART blob and expand its RGB565 payload into dst as the engine's
 * 3-byte (565 + a8=255 opaque) image data. Pure and host-testable (no FAT/LK
 * deps). Returns 0 on success and writes the dimensions to out_w and out_h; returns
 * a negative code on a bad magic/format, a truncated blob, or dst too small.
 */
int gba_boxart_decode(const unsigned char *art, unsigned art_len,
		      unsigned char *dst, unsigned dst_cap,
		      unsigned short *out_w, unsigned short *out_h);

#endif /* GBA_BOXART_H */
