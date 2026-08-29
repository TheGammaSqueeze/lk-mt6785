/* Host test for fat_wr_mkpath: on an image that has ONLY /roms/gba (no save dirs),
 * creating /saves/gba then writing+reading a file must succeed, and mkpath must be
 * idempotent (a second call is a no-op that still lets writes through). fsck (run by
 * the harness afterwards) confirms the created dirs + their "." / ".." are valid. */
#include <stdio.h>
#include "fat_ro.h"
#include "fat_wr.h"

static FILE *g;
static unsigned rd(void *c, uint32_t l, uint32_t n, void *b)
{ (void)c; if (fseek(g, (long)l * 512, SEEK_SET)) return 0; return (unsigned)fread(b, 512, n, g); }
static unsigned wr(void *c, uint32_t l, uint32_t n, const void *b)
{ (void)c; if (fseek(g, (long)l * 512, SEEK_SET)) return 0; return (unsigned)fwrite(b, 512, n, g); }

int main(int argc, char **argv)
{
	fat_vol v; fat_dir d; fat_file f;
	int fails = 0, i;
	static unsigned char wb[777], rb[777];

	g = fopen(argc > 1 ? argv[1] : "/tmp/fat.img", "r+b");
	if (!g) { fprintf(stderr, "open failed\n"); return 1; }
	if (fat_mount(&v, rd, 0)) { fprintf(stderr, "mount FAIL\n"); return 1; }
	fat_set_writer(&v, wr);

	/* precondition: /saves/gba absent (harness image only has /roms/gba) */
	if (fat_opendir(&v, "/saves/gba", &d) == 0) { printf("  NOTE: /saves/gba already existed\n"); }

	if (fat_wr_mkpath(&v, "/saves/gba") != 0) { printf("  FAIL: mkpath rc!=0\n"); fails++; }
	if (fat_opendir(&v, "/saves/gba", &d) != 0) { printf("  FAIL: /saves/gba missing after mkpath\n"); fails++; }
	/* idempotent second call */
	if (fat_wr_mkpath(&v, "/saves/gba") != 0) { printf("  FAIL: mkpath not idempotent\n"); fails++; }

	/* deeper fresh path too */
	if (fat_wr_mkpath(&v, "/states/gba") != 0) { printf("  FAIL: mkpath states rc!=0\n"); fails++; }
	if (fat_opendir(&v, "/states/gba", &d) != 0) { printf("  FAIL: /states/gba missing\n"); fails++; }

	/* write into the just-created dir and read back */
	for (i = 0; i < (int)sizeof wb; i++) wb[i] = (unsigned char)(i * 11 + 5);
	if (fat_wr_put(&v, "/saves/gba", "Long Game Name (USA).sav", wb, sizeof wb) != 0) {
		printf("  FAIL: write into created dir\n"); fails++;
	} else {
		uint32_t got;
		if (fat_open(&v, "/saves/gba/Long Game Name (USA).sav", &f) != 0) { printf("  FAIL: readback open\n"); fails++; }
		else {
			int bad = 0;
			got = fat_read(&f, 0, rb, sizeof rb);
			for (i = 0; i < (int)sizeof wb; i++) if (rb[i] != wb[i]) bad++;
			printf("readback size=%u mismatches=%d\n", got, bad);
			if (got != sizeof wb || bad) { printf("  FAIL: verify\n"); fails++; }
		}
	}

	printf(fails ? "FAT_MKDIR TEST: %d FAIL\n" : "FAT_MKDIR TEST: PASS\n", fails);
	return fails ? 1 : 0;
}
