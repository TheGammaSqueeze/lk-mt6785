/* Host test for the exFAT backend: mount a real mkfs.exfat image (the runner script
 * loop-mounts it to add the ROM files under /roms/gba and reference copies), then read every ROM back and
 * compare with the reference, create /saves/gba + /states/gba, write small and multi cluster
 * files, replace one, read them back, and leave the image for fsck.exfat + a kernel mount. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat_ro.h"
#include "fat_wr.h"

static FILE *g;
static unsigned rd(void *c, uint32_t l, uint32_t n, void *b) { (void)c; fseek(g, (long)l * 512, SEEK_SET); return (unsigned)fread(b, 512, n, g); }
static unsigned wr(void *c, uint32_t l, uint32_t n, const void *b) { (void)c; fseek(g, (long)l * 512, SEEK_SET); return (unsigned)fwrite(b, 512, n, g); }
static int fail(const char *m) { printf("FAIL: %s\n", m); return 1; }

int main(int argc, char **argv)
{
	fat_vol v; fat_dir d; fat_dirent e; fat_file f;
	static unsigned char buf[4 << 20], ref[4 << 20];
	int n = 0, i;
	if (argc < 3) { printf("usage: %s image refdir\n", argv[0]); return 2; }
	g = fopen(argv[1], "r+b"); if (!g) return fail("open image");
	if (fat_mount(&v, rd, 0) != 0) return fail("mount");
	if (!v.is_exfat) return fail("not detected as exFAT");
	fat_set_writer(&v, wr);
	printf("exfat: spc=%u clusters=%u root=%u bitmap=%u upcase=%u\n", v.ex_spc, v.total_clusters, v.root_cluster, v.ex_bitmap_clus, v.ex_upcase_clus);
	if (fat_opendir(&v, "/roms/gba", &d) != 0) return fail("opendir /roms/gba");
	while (fat_readdir(&d, &e)) {
		char path[512], rp[600]; FILE *r; size_t rl; uint32_t got;
		if (e.is_dir) continue;
		n++;
		snprintf(path, sizeof path, "/roms/gba/%s", e.name);
		if (fat_open(&v, path, &f) != 0) return fail("open rom");
		got = fat_read(&f, 0, buf, sizeof buf);
		snprintf(rp, sizeof rp, "%s/%s", argv[2], e.name);
		r = fopen(rp, "rb"); if (!r) { printf("no ref for %s\n", e.name); return 1; }
		rl = fread(ref, 1, sizeof ref, r); fclose(r);
		if (got != rl || memcmp(buf, ref, rl) != 0) { printf("content mismatch %s (%u vs %zu)\n", e.name, got, rl); return 1; }
		/* a partial read from an odd offset */
		if (rl > 1000) { got = fat_read(&f, 777, buf, 1000); if (got != 1000 || memcmp(buf, ref + 777, 1000) != 0) return fail("partial read"); }
		printf("rom ok: %s (%u bytes, %s)\n", e.name, (unsigned)rl, e.ex_nofat ? "contiguous" : "chained");
	}
	if (n == 0) return fail("no roms listed");
	/* writes */
	if (fat_wr_mkpath(&v, "/saves/gba") != 0) return fail("mkpath saves");
	if (fat_wr_mkpath(&v, "/states/gba") != 0) return fail("mkpath states");
	if (fat_wr_mkpath(&v, "/saves/gba") != 0) return fail("mkpath idempotent");
	for (i = 0; i < (1 << 20); i++) ref[i] = (unsigned char)(i * 7 + (i >> 9));
	if (fat_wr_put(&v, "/saves/gba", "Some Long Game Name (Europe).sav", ref, 32768) != 0) return fail("put 32K");
	if (fat_wr_put(&v, "/states/gba", "big state file.sus", ref, 1 << 20) != 0) return fail("put 1M");
	if (fat_wr_put(&v, "/saves/gba", "tiny.sav", ref, 100) != 0) return fail("put 100");
	for (i = 0; i < 32768; i++) ref[i] ^= 0x5A;
	if (fat_wr_put(&v, "/saves/gba", "Some Long Game Name (Europe).sav", ref, 32768) != 0) return fail("replace 32K");
	if (fat_open(&v, "/saves/gba/Some Long Game Name (Europe).sav", &f) != 0) return fail("open replaced");
	if (f.size != 32768 || fat_read(&f, 0, buf, 32768) != 32768 || memcmp(buf, ref, 32768) != 0) return fail("replaced content");
	for (i = 0; i < 32768; i++) ref[i] ^= 0x5A;
	if (fat_open(&v, "/states/gba/big state file.sus", &f) != 0) return fail("open big");
	if (f.size != (1 << 20) || fat_read(&f, 0, buf, 1 << 20) != (1u << 20) || memcmp(buf, ref, 1 << 20) != 0) return fail("big content");
	if (fat_open(&v, "/saves/gba/tiny.sav", &f) != 0 || f.size != 100) return fail("tiny");
	/* exactly one entry per name after the replace */
	{ int cnt = 0; fat_opendir(&v, "/saves/gba", &d); while (fat_readdir(&d, &e)) if (!strcmp(e.name, "Some Long Game Name (Europe).sav")) cnt++; if (cnt != 1) return fail("duplicate entries"); }
	/* many small files: forces the directory to grow */
	for (i = 0; i < 40; i++) { char nm[64]; snprintf(nm, sizeof nm, "grow test file number %02d.sav", i); if (fat_wr_put(&v, "/saves/gba", nm, ref, 64) != 0) { printf("grow put %d\n", i); return 1; } }
	fclose(g);
	printf("exfat rw test ok (%d roms)\n", n);
	return 0;
}
