/* Host test for fat_ro.c: reads a real FAT32 image via a file-backed sector
 * callback and validates mount / readdir (LFN) / open / read (multi-cluster). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_ro.h"

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
	fat_vol v; fat_dir d; fat_dirent e; fat_file f;
	int rc, nlist = 0, fails = 0;
	g_img = fopen(imgf, "rb");
	if (!g_img) { fprintf(stderr, "open %s failed\n", imgf); return 1; }

	rc = fat_mount(&v, img_read, 0);
	printf("mount: rc=%d  fat32=%d spc=%u clusters=%u\n", rc, v.is_fat32, v.sec_per_clus, v.total_clusters);
	if (rc != 0) { fprintf(stderr, "MOUNT FAILED\n"); return 1; }

	printf("--- readdir /roms/gba ---\n");
	if (fat_opendir(&v, "/roms/gba", &d) != 0) { fprintf(stderr, "opendir FAILED\n"); return 1; }
	int saw_pokemon = 0, saw_zelda = 0, saw_big = 0;
	while (fat_readdir(&d, &e)) {
		printf("  %-32s %8u %s\n", e.name, e.size, e.is_dir ? "<DIR>" : "");
		nlist++;
		if (!strcmp(e.name, "Pokemon Emerald (USA).gba") && e.size == 41) saw_pokemon = 1;
		if (!strcmp(e.name, "Zelda.gba") && e.size == 9) saw_zelda = 1;
		if (!strcmp(e.name, "Big.gba") && e.size == 300000) saw_big = 1;
	}
	if (!saw_pokemon) { printf("  FAIL: missing LFN 'Pokemon Emerald (USA).gba'@41\n"); fails++; }
	if (!saw_zelda)   { printf("  FAIL: missing Zelda.gba@9\n"); fails++; }
	if (!saw_big)     { printf("  FAIL: missing Big.gba@300000\n"); fails++; }

	printf("--- open+read /gba_bios.bin ---\n");
	if (fat_open(&v, "/gba_bios.bin", &f) != 0) { printf("  FAIL open bios\n"); fails++; }
	else {
		char buf[64]; uint32_t n = fat_read(&f, 0, buf, sizeof buf); buf[n < 64 ? n : 63] = 0;
		printf("  size=%u read=%u content='%s'\n", f.size, n, buf);
		if (strncmp(buf, "GBA_BIOS_16KB_CONTENT_MARKER", 28) != 0) { printf("  FAIL bios content\n"); fails++; }
	}

	printf("--- multi-cluster read of Big.gba vs the real file ---\n");
	if (fat_open(&v, "/roms/gba/Big.gba", &f) != 0) { printf("  FAIL open Big\n"); fails++; }
	else {
		/* dump the real file content from the mounted copy is gone; instead read the
		 * whole thing twice at different offsets and confirm self-consistency + size,
		 * and CRC it so a rebuild with a known image can be compared. */
		unsigned char *whole = malloc(f.size);
		uint32_t n = fat_read(&f, 0, whole, f.size);
		unsigned long crc = 5381; uint32_t i;
		for (i = 0; i < n; i++) crc = ((crc << 5) + crc) ^ whole[i];
		printf("  size=%u read=%u djb2=%lu\n", f.size, n, crc & 0xffffffff);
		if (n != f.size) { printf("  FAIL short read\n"); fails++; }
		/* cross-cluster spot check: bytes at offset 130000 read via full buffer vs a
		 * targeted 100-byte read must match */
		{ unsigned char mid[100]; uint32_t m = fat_read(&f, 130000, mid, 100);
		  if (m != 100 || memcmp(mid, whole + 130000, 100) != 0) { printf("  FAIL mid-offset read\n"); fails++; }
		  else printf("  mid-offset read OK\n"); }
		free(whole);
	}

	/* mirrors the on-device asset probe (gba_driver.c): bios openable + roms/gba
	 * has >=1 file. This is the gate that decides emu-vs-normal-boot. */
	printf("--- SD asset probe (bios + >=1 rom) ---\n");
	{
		fat_file bios; fat_dir rd; fat_dirent re; int have_rom = 0, ok;
		int bios_ok = (fat_open(&v, "/gba_bios.bin", &bios) == 0);
		int roms_ok = (fat_opendir(&v, "/roms/gba", &rd) == 0);
		if (roms_ok) while (fat_readdir(&rd, &re)) if (!re.is_dir) { have_rom = 1; break; }
		ok = bios_ok && roms_ok && have_rom;
		printf("  bios=%d roms_dir=%d have_rom=%d -> assets_ok=%d\n", bios_ok, roms_ok, have_rom, ok);
		if (!ok) { printf("  FAIL: probe should pass on this image\n"); fails++; }
	}

	printf("\n%s (%d entries listed, %d failures)\n", fails ? "FAIL" : "ALL PASS", nlist, fails);
	return fails ? 1 : 0;
}
