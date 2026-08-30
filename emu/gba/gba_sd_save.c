/* Save/state persistence to the microSD - see gba_sd_save.h. */
#include "gba_sd_save.h"
#include "fat_wr.h"

/* "<rombase>.<ext>" from a ROM name (strip its final .ext) into out[<=cap]. */
static void base_ext(const char *rom, const char *ext, char *out, int cap)
{
	int L = 0, dot = -1, i, k = 0;
	while (rom[L]) L++;
	for (i = L - 1; i >= 0; i--) if (rom[i] == '.') { dot = i; break; }
	if (dot < 0) dot = L;
	for (i = 0; i < dot && k < cap - 6; i++) out[k++] = rom[i];
	out[k++] = '.';
	for (i = 0; ext[i] && k < cap - 1; i++) out[k++] = ext[i];
	out[k] = 0;
}

uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname,
			   const char *ext, unsigned char *dst, uint32_t cap)
{
	char path[300], nm[288];
	int k = 0, i;
	fat_file f;
	base_ext(romname, ext, nm, (int)sizeof nm);
	for (i = 0; dir[i] && k < (int)sizeof path - 2; i++) path[k++] = dir[i];
	if (k == 0 || path[k - 1] != '/') path[k++] = '/';
	for (i = 0; nm[i] && k < (int)sizeof path - 1; i++) path[k++] = nm[i];
	path[k] = 0;
	if (fat_open(v, path, &f) != 0) return 0;
	return fat_read(&f, 0, dst, cap < f.size ? cap : f.size);
}

int gba_sd_write_named(fat_vol *v, const char *dir, const char *romname,
		       const char *ext, const void *buf, uint32_t len)
{
	char nm[288];
	base_ext(romname, ext, nm, (int)sizeof nm);
	fat_wr_mkpath(v, dir);   /* ensure saves/gba or states/gba exists (idempotent) */
	return fat_wr_put(v, dir, nm, buf, len);
}

uint32_t gba_sd_load_sav(fat_vol *v, const char *rom, unsigned char *dst, uint32_t cap)
{ return gba_sd_read_named(v, "/saves/gba", rom, "sav", dst, cap); }

int gba_sd_write_sav(fat_vol *v, const char *rom, const void *buf, uint32_t len)
{ return gba_sd_write_named(v, "/saves/gba", rom, "sav", buf, len); }

uint32_t gba_sd_load_state(fat_vol *v, const char *rom, int slot, unsigned char *dst, uint32_t cap)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return gba_sd_read_named(v, "/states/gba", rom, ext, dst, cap);
}

int gba_sd_write_state(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return gba_sd_write_named(v, "/states/gba", rom, ext, buf, len);
}

/* GammaOS Pico menu settings, persisted ON THE SD CARD so they travel with the
 * card and survive a reflash. Stored as /gba/settings.bin (the raw 64-byte
 * ayaneo settings block). Read at LK start to honour skip-boot-logo etc. */
extern int  ayaneo_settings_serialize(unsigned char *b, int cap);
extern void ayaneo_settings_deserialize(const unsigned char *b, int len);

int gba_sd_settings_load(fat_vol *v)
{
	unsigned char b[64];
	fat_file f;
	if (fat_open(v, "/gba/settings.bin", &f) != 0) return -1;
	if (fat_read(&f, 0, b, (uint32_t)sizeof b) != (uint32_t)sizeof b) return -1;
	ayaneo_settings_deserialize(b, (int)sizeof b);
	return 0;
}

int gba_sd_settings_save(fat_vol *v)
{
	unsigned char b[64];
	int n = ayaneo_settings_serialize(b, (int)sizeof b);
	if (n <= 0) return -1;
	fat_wr_mkpath(v, "/gba");
	return fat_wr_put(v, "/gba", "settings.bin", b, (uint32_t)n);
}
