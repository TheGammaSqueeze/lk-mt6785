/* .ART box art decode (see gba_boxart.h). Pure C, no LK/FAT dependencies. */
#include "gba_boxart.h"

int gba_boxart_decode(const unsigned char *art, unsigned art_len,
		      unsigned char *dst, unsigned dst_cap,
		      unsigned short *out_w, unsigned short *out_h)
{
	unsigned w, h, npx, i;
	unsigned long long need;

	if (!art || !dst || art_len < 12)
		return -1;
	if (((unsigned)art[0] | ((unsigned)art[1] << 8) |
	     ((unsigned)art[2] << 16) | ((unsigned)art[3] << 24)) != GBA_ART_MAGIC)
		return -2;
	if ((art[6] | (art[7] << 8)) != 0)		/* format 0 = RGB565 LE */
		return -3;
	w = art[8] | (art[9] << 8);
	h = art[10] | (art[11] << 8);
	npx = w * h;
	if (!npx || w > 4096 || h > 4096)		/* sanity */
		return -4;
	if (art_len < 12u + npx * 2u)			/* payload present */
		return -5;
	need = (unsigned long long)npx * 3u;
	if (need > dst_cap)				/* room in DRAM */
		return -6;

	for (i = 0; i < npx; i++) {
		dst[i * 3 + 0] = art[12 + i * 2 + 0];	/* 565 low  */
		dst[i * 3 + 1] = art[12 + i * 2 + 1];	/* 565 high */
		dst[i * 3 + 2] = 0xFF;			/* opaque a8 */
	}
	if (out_w) *out_w = (unsigned short)w;
	if (out_h) *out_h = (unsigned short)h;
	return 0;
}
