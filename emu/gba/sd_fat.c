/* On-device SD -> FAT glue (LK only). See sd_fat.h. */
#include "sd_fat.h"

/* LK MMC block-read: reads blkcnt 512B sectors at LBA blknr from mmc device
 * dev_num, partition part_id, into dst. Returns blkcnt on success. Declared here
 * to avoid pulling the whole MTK storage header set into the emu module. */
extern unsigned long mmc_wrap_bread(int dev_num, unsigned long blknr,
				    unsigned long blkcnt, void *dst, unsigned int part_id);

/* mmc_legacy_init(1) in platform.c -> id = verbose-1 = 0, so the external microSD
 * (MMC_SLOT=1 hardware; UFS is the boot device, so this MMC slot is the card) is
 * device 0. part_id 0 = the user data area (SD cards have no boot partitions).
 * These are compile-time so they are trivial to retarget once verified on HW. */
#ifndef SD_DEV_NUM
#define SD_DEV_NUM 0
#endif
#ifndef SD_PART_USER
#define SD_PART_USER 0
#endif

static unsigned sd_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	(void)ctx;
	return (unsigned)mmc_wrap_bread(SD_DEV_NUM, (unsigned long)lba,
					(unsigned long)count, buf, SD_PART_USER);
}

int gba_sd_mount(fat_vol *v)
{
	return fat_mount(v, sd_read, 0);
}

/* Load /gba_bios.bin (must be exactly 16384 bytes) into dst[16384]. Returns 0 on
 * success, negative if missing / wrong size / short read. */
int gba_sd_load_bios(fat_vol *v, unsigned char *dst)
{
	fat_file f;
	if (fat_open(v, "/gba_bios.bin", &f) != 0) return -1;
	if (f.size != 16384u) return -2;
	if (fat_read(&f, 0, dst, 16384u) != 16384u) return -3;
	return 0;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int name_ci_cmp(const char *a, const char *b)
{ while (*a && *b) { char x = lc(*a), y = lc(*b); if (x != y) return x - y; a++; b++; } return lc(*a) - lc(*b); }
static int ends_dot_gba(const char *n)
{
	int L = 0; while (n[L]) L++;
	if (L < 4) return 0;
	{ const char *e = n + L - 4;
	  return e[0] == '.' && lc(e[1]) == 'g' && lc(e[2]) == 'b' && lc(e[3]) == 'a'; }
}

int gba_sd_list_roms(fat_vol *v, gba_rom_entry *out, int max)
{
	fat_dir d; fat_dirent e; int n = 0, i, j;
	if (max <= 0 || fat_opendir(v, "/roms/gba", &d) != 0) return 0;
	while (fat_readdir(&d, &e) && n < max) {
		int k = 0;
		if (e.is_dir || !ends_dot_gba(e.name)) continue;
		if (e.size == 0) continue;          /* skip empty/placeholder files */
		while (e.name[k] && k < 127) { out[n].name[k] = e.name[k]; k++; }
		out[n].name[k] = 0;
		out[n].first_clus = e.first_clus;
		out[n].size = e.size;
		n++;
	}
	for (i = 1; i < n; i++) {           /* insertion sort, case-insensitive by name */
		gba_rom_entry t = out[i];
		for (j = i - 1; j >= 0 && name_ci_cmp(out[j].name, t.name) > 0; j--)
			out[j + 1] = out[j];
		out[j + 1] = t;
	}
	return n;
}

uint32_t gba_sd_load_rom(fat_vol *v, const gba_rom_entry *r, unsigned char *dst, uint32_t cap)
{
	fat_file f;
	uint32_t n = r->size;
	/* Refuse a ROM that does not fit the arena: a truncated read is a CORRUPT
	 * ROM (gpSP would execute garbage). Return 0 so the caller rejects it and
	 * falls back to the selector instead of booting a broken game. Also reject
	 * an empty file. */
	if (n == 0 || n > cap) return 0;
	f.v = v; f.first_clus = r->first_clus; f.size = r->size;   /* reuse the cached chain head */
	return fat_read(&f, 0, dst, n);
}
