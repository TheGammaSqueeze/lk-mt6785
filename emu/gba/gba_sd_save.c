/* Save/state persistence to the microSD - see gba_sd_save.h. */
#include "gba_sd_save.h"
#include "fat_wr.h"
extern const char *g_gba_state_dir;   /* gba_driver.c: "/states/gba" for gpSP, the core's own folder for armGBA */

/* "<rombase>.<ext>" from a ROM name into out[<=cap]. The roster already drops the ROM's
 * extension (sd_fat.c scan_rom_folder), so only a real ROM extension is stripped here: cutting at
 * the last dot whatever it was turned "Mario vs. Donkey Kong" into "Mario vs", so its box art was
 * never found and its saves and states were keyed (and could collide) under the cut name. */
static int is_rom_ext(const char *e)
{
	static const char *const known[] = { "gba", "gb", "gbc", "sfc", "smc", "md", "gen", "bin", "smd", "sms", "gg", "sg", 0 };
	int i, j;
	for (i = 0; known[i]; i++) {
		for (j = 0; known[i][j] && e[j]; j++) {
			char c = e[j]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
			if (c != known[i][j]) break;
		}
		if (!known[i][j] && !e[j]) return 1;
	}
	return 0;
}
static void base_ext_mode(const char *rom, const char *ext, char *out, int cap, int legacy)
{
	int L = 0, dot = -1, i, k = 0;
	while (rom[L]) L++;
	for (i = L - 1; i >= 0; i--) if (rom[i] == '.') { dot = i; break; }
	if (dot < 0 || (!legacy && !is_rom_ext(rom + dot + 1))) dot = L;
	for (i = 0; i < dot && k < cap - 6; i++) out[k++] = rom[i];
	out[k++] = '.';
	for (i = 0; ext[i] && k < cap - 1; i++) out[k++] = ext[i];
	out[k] = 0;
}
static void base_ext(const char *rom, const char *ext, char *out, int cap) { base_ext_mode(rom, ext, out, cap, 0); }

static uint32_t read_one(fat_vol *v, const char *dir, const char *nm, unsigned char *dst, uint32_t cap)
{
	char path[300];
	int k = 0, i;
	fat_file f;
	for (i = 0; dir[i] && k < (int)sizeof path - 2; i++) path[k++] = dir[i];
	if (k == 0 || path[k - 1] != '/') path[k++] = '/';
	for (i = 0; nm[i] && k < (int)sizeof path - 1; i++) path[k++] = nm[i];
	path[k] = 0;
	if (fat_open(v, path, &f) != 0) return 0;
	return fat_read(&f, 0, dst, cap < f.size ? cap : f.size);
}

uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname,
			   const char *ext, unsigned char *dst, uint32_t cap)
{
	char nm[288], old[288];
	uint32_t n;
	base_ext(romname, ext, nm, (int)sizeof nm);
	n = read_one(v, dir, nm, dst, cap);
	if (n) return n;
	/* a file written before the fix sits under the cut name: read it there (the next
	 * write puts it under the full name) */
	base_ext_mode(romname, ext, old, (int)sizeof old, 1);
	{ int i = 0; while (nm[i] && nm[i] == old[i]) i++; if (nm[i] == old[i]) return 0; }
	return read_one(v, dir, old, dst, cap);
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
	return gba_sd_read_named(v, g_gba_state_dir, rom, ext, dst, cap);
}

int gba_sd_write_state(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len)
{
	char ext[4]; ext[0] = 's'; ext[1] = 't'; ext[2] = (char)('0' + (slot % 10)); ext[3] = 0;
	return gba_sd_write_named(v, g_gba_state_dir, rom, ext, buf, len);
}

/* GammaOS menu settings, kept ON THE SD CARD only (the LK writes nothing on the eMMC): stored as
 * /gammaos/settings.bin, the serialized settings block (ayaneo_settings_serialize). A card from an
 * older build has them in /gba/settings.bin, read when the new file is not there yet; the next
 * save writes the new place. Read by the SD gate before the intro (skip intro, brightness...). */
extern int  ayaneo_settings_serialize(unsigned char *b, int cap);
extern void ayaneo_settings_deserialize(const unsigned char *b, int len);

int gba_sd_settings_load(fat_vol *v)
{
	unsigned char b[192];
	fat_file f;
	if (fat_open(v, "/gammaos/settings.bin", &f) != 0 && fat_open(v, "/gba/settings.bin", &f) != 0)
		return -1;
	if (fat_read(&f, 0, b, (uint32_t)sizeof b) != (uint32_t)sizeof b) return -1;
	ayaneo_settings_deserialize(b, (int)sizeof b);
	return 0;
}

int gba_sd_settings_write(fat_vol *v, const unsigned char *b, int n)
{
	if (n <= 0) return -1;
	fat_wr_mkpath(v, "/gammaos");
	return fat_wr_put(v, "/gammaos", "settings.bin", b, (uint32_t)n);
}

int gba_sd_settings_save(fat_vol *v)
{
	unsigned char b[192];
	return gba_sd_settings_write(v, b, ayaneo_settings_serialize(b, (int)sizeof b));
}
