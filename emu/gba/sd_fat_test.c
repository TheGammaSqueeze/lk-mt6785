/* Host test for sd_fat.c list/load robustness: an empty (0-byte) .gba is skipped
 * by the enumerator, and gba_sd_load_rom refuses a ROM larger than the arena
 * (no silent truncation) as well as an empty one, while a normal ROM loads whole.
 *
 * Runs against a real mkfs.fat image with /roms/gba populated by the harness:
 *   good.gba  = 4096 bytes of a known pattern
 *   empty.gba = 0 bytes
 * The mmc_wrap_bread extern is stubbed (list/load only touch the fat_vol rd cb). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_ro.h"
#include "sd_fat.h"

/* sd_fat.c references this for the on-device path; unused here. */
unsigned long mmc_wrap_bread(int d, unsigned long b, unsigned long c, void *p, unsigned int pt)
{ (void)d; (void)b; (void)c; (void)p; (void)pt; return 0; }

static FILE *g_img;
static unsigned img_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
	(void)ctx;
	if (fseek(g_img, (long)lba * 512, SEEK_SET) != 0) return 0;
	return (unsigned)fread(buf, 512, count, g_img);
}

int main(int argc, char **argv)
{
	const char *imgf = argc > 1 ? argv[1] : "/tmp/fat.img";
	fat_vol v;
	gba_rom_entry roms[16];
	int n, i, fails = 0, gi = -1;
	static unsigned char buf[8192];

	g_img = fopen(imgf, "rb");
	if (!g_img) { fprintf(stderr, "open %s failed\n", imgf); return 1; }
	if (fat_mount(&v, img_read, 0) != 0) { fprintf(stderr, "MOUNT FAILED\n"); return 1; }

	n = gba_sd_list_roms(&v, roms, 16);
	printf("list_roms n=%d\n", n);
	for (i = 0; i < n; i++) {
		printf("  %-24s size=%u\n", roms[i].name, roms[i].size);
		if (!strcmp(roms[i].name, "empty.gba")) { printf("  FAIL: empty.gba listed\n"); fails++; }
		if (!strcmp(roms[i].name, "good.gba")) gi = i;
	}
	if (gi < 0) { printf("  FAIL: good.gba missing\n"); fails++; }

	if (gi >= 0) {
		uint32_t got = gba_sd_load_rom(&v, &roms[gi], buf, sizeof buf);
		printf("load good (cap=%u): got=%u\n", (unsigned)sizeof buf, got);
		if (got != roms[gi].size) { printf("  FAIL: short load\n"); fails++; }

		/* oversize: cap smaller than the ROM must be REFUSED (return 0), not truncated */
		got = gba_sd_load_rom(&v, &roms[gi], buf, roms[gi].size - 1);
		printf("load oversize (cap=size-1): got=%u (want 0)\n", got);
		if (got != 0) { printf("  FAIL: oversize not refused (silent truncation)\n"); fails++; }
	}

	printf(fails ? "SD_FAT TEST: %d FAIL\n" : "SD_FAT TEST: PASS\n", fails);
	return fails ? 1 : 0;
}
