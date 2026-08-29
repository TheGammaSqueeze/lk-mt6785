/* Host test for sd_fat.c list/load robustness AND the on-device mount wiring:
 *  - an empty (0-byte) .gba is skipped by the enumerator;
 *  - gba_sd_load_rom refuses a ROM larger than the arena (no silent truncation)
 *    and an empty one, while a normal ROM loads whole;
 *  - gba_sd_mount() attaches BOTH a reader and a WRITER (regression guard: the
 *    on-device save/state path silently no-ops if v->wr is left 0), verified by
 *    a real write+readback through the gba_sd_mount path.
 *
 * The mmc_wrap_bread/bwrite externs that sd_fat.c calls are backed here by the
 * image file, so gba_sd_mount() itself is exercised (not just fat_mount). */
#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "sd_fat.h"
#include "fat_wr.h"

static FILE *g_img;

/* sd_fat.c's device block ops, redirected to the loop image for the host test. */
unsigned long mmc_wrap_bread(int d, unsigned long b, unsigned long c, void *p, unsigned int pt)
{
	(void)d; (void)pt;
	if (fseek(g_img, (long)b * 512, SEEK_SET) != 0) return 0;
	return (unsigned long)fread(p, 512, c, g_img);
}
unsigned long mmc_wrap_bwrite(int d, unsigned long b, unsigned long c, const void *p, unsigned int pt)
{
	(void)d; (void)pt;
	if (fseek(g_img, (long)b * 512, SEEK_SET) != 0) return 0;
	return (unsigned long)fwrite(p, 512, c, g_img);
}
/* the SD host is "always up" in the host test (the image file is the card) */
int mmc_legacy_init(int verbose) { (void)verbose; return 0; }

int main(int argc, char **argv)
{
	const char *imgf = argc > 1 ? argv[1] : "/tmp/fat.img";
	fat_vol v;
	gba_rom_entry roms[16];
	int n, i, fails = 0, gi = -1;
	static unsigned char buf[8192];

	g_img = fopen(imgf, "r+b");
	if (!g_img) { fprintf(stderr, "open %s failed\n", imgf); return 1; }

	if (gba_sd_mount(&v) != 0) { fprintf(stderr, "MOUNT FAILED\n"); return 1; }
	/* regression: the on-device mount MUST attach a writer, else saves no-op */
	if (!v.wr) { printf("  FAIL: gba_sd_mount left v.wr=0 (saves would silently fail)\n"); fails++; }

	{
		int total = -1;
		n = gba_sd_list_roms(&v, roms, 16, &total);
		printf("list_roms n=%d total=%d\n", n, total);
		if (total != n) { printf("  FAIL: total(%d) != n(%d) when uncapped\n", total, n); fails++; }
	}
	for (i = 0; i < n; i++) {
		printf("  %-24s size=%u\n", roms[i].name, roms[i].size);
		if (!strcmp(roms[i].name, "empty.gba")) { printf("  FAIL: empty.gba listed\n"); fails++; }
		if (!strcmp(roms[i].name, "good.gba")) gi = i;
	}
	if (gi < 0) { printf("  FAIL: good.gba missing\n"); fails++; }

	/* truncation: cap the list below the real count and confirm total reports it */
	{
		int total = -1, cap;
		gba_rom_entry two[2];
		cap = gba_sd_list_roms(&v, two, 2, &total);
		printf("capped list_roms n=%d total=%d (fixture has good/good2/good3)\n", cap, total);
		if (cap != 2) { printf("  FAIL: stored count not capped to 2\n"); fails++; }
		if (total < 3) { printf("  FAIL: total(%d) did not count past the cap\n", total); fails++; }
	}

	if (gi >= 0) {
		uint32_t got = gba_sd_load_rom(&v, &roms[gi], buf, sizeof buf);
		printf("load good (cap=%u): got=%u\n", (unsigned)sizeof buf, got);
		if (got != roms[gi].size) { printf("  FAIL: short load\n"); fails++; }
		got = gba_sd_load_rom(&v, &roms[gi], buf, roms[gi].size - 1);
		printf("load oversize (cap=size-1): got=%u (want 0)\n", got);
		if (got != 0) { printf("  FAIL: oversize not refused\n"); fails++; }
	}

	/* write+readback through the gba_sd_mount path (validates the wired writer) */
	{
		static unsigned char wb[600], rb[600];
		fat_file f; uint32_t got; int bad = 0, rc;
		for (i = 0; i < 600; i++) wb[i] = (unsigned char)(i * 5 + 1);
		rc = fat_wr_put(&v, "/saves/gba", "wtest.bin", wb, sizeof wb);
		printf("write via mount path: rc=%d\n", rc);
		if (rc != 0) { printf("  FAIL: fat_wr_put rc=%d\n", rc); fails++; }
		else {
			if (gba_sd_mount(&v) != 0 || fat_open(&v, "/saves/gba/wtest.bin", &f) != 0) {
				printf("  FAIL: readback open\n"); fails++;
			} else {
				got = fat_read(&f, 0, rb, sizeof rb);
				for (i = 0; i < 600; i++) if (rb[i] != wb[i]) bad++;
				printf("readback size=%u mismatches=%d\n", got, bad);
				if (got != sizeof wb || bad) { printf("  FAIL: write verify\n"); fails++; }
			}
		}
	}

	printf(fails ? "SD_FAT TEST: %d FAIL\n" : "SD_FAT TEST: PASS\n", fails);
	return fails ? 1 : 0;
}
