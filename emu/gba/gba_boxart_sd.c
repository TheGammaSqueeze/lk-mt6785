/* SD-backed box art loader (see gba_boxart_sd.h). LK-only. */
#include "gba_boxart_sd.h"
#include "gba_boxart.h"

/* the box art folder of each console type (sd_fat.h GBA_CONSOLE_*), beside its ROM folder */
static const char *const s_art_dir[] = {
	"/roms/gb/boxart", "/roms/gbc/boxart", "/roms/gba/boxart", "/roms/snes/boxart",
	"/roms/genesis/boxart", "/roms/sms/boxart", "/roms/gg/boxart", "/roms/sg/boxart",
};

int gba_boxart_load_sd(fat_vol *v, int console, const char *romname,
		       unsigned char *scratch, unsigned scratch_cap,
		       unsigned char *dst, unsigned dst_cap, unsigned pixels_off,
		       snes_img_entry *out)
{
	unsigned int n;
	unsigned short w = 0, h = 0;

	if (!v || !romname || !scratch || !dst || !out)
		return -1;
	if (console < 0 || console >= (int)(sizeof s_art_dir / sizeof s_art_dir[0]))
		return -1;

	/* Read the compact .ART for this ROM: /roms/<system>/boxart/<rombase>.ART.
	 * gba_sd_read_named strips the ROM's extension and appends "ART". */
	n = gba_sd_read_named(v, s_art_dir[console], romname, "ART", scratch, scratch_cap);
	if (n < 12)			/* missing / empty / too small for a header */
		return -2;

	if (gba_boxart_decode(scratch, n, dst, dst_cap, &w, &h) != 0)
		return -3;

	out->w = w;
	out->h = h;
	out->flags = SNES_IMG_RGB565;
	out->pad = 0;
	out->pixels = pixels_off;	/* caller: dst == pack_base + pixels_off */
	return 0;
}
