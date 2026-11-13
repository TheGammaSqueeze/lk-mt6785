/* SD-backed box art loader (see gba_boxart_sd.h). LK-only. */
#include "gba_boxart_sd.h"
#include "gba_boxart.h"

/* GammaOS's own box art folder of each console type (sd_fat.h GBA_CONSOLE_*) */
static const char *const s_art_dir[] = {
	"/roms/gb/boxart", "/roms/gbc/boxart", "/roms/gba/boxart", "/roms/snes/boxart",
	"/roms/genesis/boxart", "/roms/sms/boxart", "/roms/gg/boxart", "/roms/sg/boxart",
};

const char *gba_boxart_home_dir(int console)
{
	if (console < 0 || console >= (int)(sizeof s_art_dir / sizeof s_art_dir[0])) return 0;
	return s_art_dir[console];
}

void gba_boxart_dir(const gba_rom_entry *rom, char *out, int cap)
{
	const char *d = gba_sd_rom_dir(rom), *t = "/boxart";
	int k = 0;
	while (*d && k < cap - 1) out[k++] = *d++;
	while (*t && k < cap - 1) out[k++] = *t++;
	out[k] = 0;
}

static int same_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return 0;
	}
	return *a == *b;
}

int gba_boxart_load_sd(fat_vol *v, const gba_rom_entry *rom,
		       unsigned char *scratch, unsigned scratch_cap,
		       unsigned char *dst, unsigned dst_cap, unsigned pixels_off,
		       snes_img_entry *out)
{
	char dir[112];
	const char *home;
	unsigned int n;
	unsigned short w = 0, h = 0;

	if (!v || !rom || !scratch || !dst || !out)
		return -1;
	home = gba_boxart_home_dir((int)rom->type);
	if (!home)
		return -1;

	/* The compact .ART for this ROM: <its folder>/boxart/<rombase>.ART, then GammaOS's own
	 * /roms/<system>/boxart (the art a release ships) for a game kept elsewhere.
	 * gba_sd_read_named strips the ROM's extension and appends "ART". */
	gba_boxart_dir(rom, dir, sizeof dir);
	n = gba_sd_read_named(v, dir, rom->name, "ART", scratch, scratch_cap);
	if (n < 12 && !same_ci(dir, home))
		n = gba_sd_read_named(v, home, rom->name, "ART", scratch, scratch_cap);
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
