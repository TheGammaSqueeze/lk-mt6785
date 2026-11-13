/*
 * sd_check.c - `fastboot oem sd-check`: is every cluster on the card used by at most one file?
 *
 * Every folder is walked from the root; each file's and folder's clusters (a contiguous exFAT run,
 * or its FAT chain) are marked in a bitmap, and a cluster met a second time is reported with both
 * paths. A write that went wrong can leave an entry pointing at clusters another file was given
 * since; this finds it without a PC. Reads only.
 */
#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
#include "sd_check.h"

extern void *ayaneo_import_arena(unsigned int *size);

#define CHECK_DEPTH 12
#define CHECK_SEEN  16                 /* doubly used clusters remembered for the owners' pass */

static uint8_t *s_map;                 /* a bit a cluster */
static uint32_t s_dups[CHECK_SEEN];
static int s_ndup;
static void (*s_out)(const char *);
static char s_line[200];

static uint32_t rd32le(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* the next cluster of a chain, 0 at its end */
static uint32_t next_of(fat_vol *v, uint32_t n)
{
	static uint8_t b[512];
	static uint32_t bs = 0xFFFFFFFFu;
	static fat_vol *bv;
	uint32_t sec, off, val;
	if (v->is_exfat) { sec = v->ex_fat_lba + (n >> 7); off = (n & 127u) * 4u; }
	else if (v->is_fat32) { sec = v->fat_start + n / 128u; off = (n % 128u) * 4u; }
	else { sec = v->fat_start + n / 256u; off = (n % 256u) * 2u; }
	if (sec != bs || bv != v) {
		if (v->rd(v->ctx, sec, 1, b) != 1) return 0;
		bs = sec; bv = v;
	}
	if (v->is_fat32 || v->is_exfat) {
		val = rd32le(b + off) & (v->is_exfat ? 0xFFFFFFFFu : 0x0FFFFFFFu);
		if (val >= (v->is_exfat ? 0xFFFFFFF8u : 0x0FFFFFF8u)) return 0;
	} else {
		val = b[off] | (b[off + 1] << 8);
		if (val >= 0xFFF8u) return 0;
	}
	return val >= 2u && val < v->total_clusters + 2u ? val : 0;
}

/* mark (pass 0) or look for the remembered clusters in (pass 1) the clusters of one entry;
 * n clusters, or the whole chain when n is 0 and it is chained */
static void walk(fat_vol *v, uint32_t first, int nofat, uint32_t n, const char *path, int pass, uint32_t *count)
{
	uint32_t c = first, k, hops = 0;
	for (k = 0; c >= 2u && c < v->total_clusters + 2u && (n == 0 || k < n); k++) {
		uint32_t i = c - 2u;
		if (pass == 0) {
			if (s_map[i >> 3] & (1u << (i & 7))) {
				if (s_ndup < CHECK_SEEN) s_dups[s_ndup] = c;
				s_ndup++;
			} else s_map[i >> 3] |= (uint8_t)(1u << (i & 7));
			(*count)++;
		} else {
			int d;
			for (d = 0; d < s_ndup && d < CHECK_SEEN; d++)
				if (s_dups[d] == c) {
					snprintf(s_line, sizeof s_line, "cluster %u: %s", c, path);
					s_out(s_line);
				}
		}
		if (nofat) { if (n == 0) break; c++; }
		else c = next_of(v, c);
		if (++hops > v->total_clusters) break;
	}
}

static void walk_tree(fat_vol *v, int pass, uint32_t *files, uint32_t *dirs, uint32_t *clusters)
{
	static fat_dir stack[CHECK_DEPTH];
	static char path[CHECK_DEPTH][256];
	static fat_dirent e;
	uint32_t cb = v->is_exfat ? v->ex_spc * 512u : (uint32_t)v->sec_per_clus * 512u;
	int sp = 0;
	if (fat_opendir(v, "/", &stack[0]) != 0) return;
	strcpy(path[0], "");
	/* the root's own clusters (a chain on FAT32 and exFAT, none on FAT16) */
	if (v->is_exfat || v->is_fat32) walk(v, v->root_cluster, 0, 0, "/", pass, clusters);
	while (sp >= 0) {
		char p[256];
		uint32_t n;
		if (!fat_readdir(&stack[sp], &e)) { sp--; continue; }
		if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
		snprintf(p, sizeof p, "%s/%s", path[sp], e.name);
		if (e.is_dir) {
			n = v->is_exfat ? (e.ex_dir_len + cb - 1u) / cb : 0u;   /* a FAT folder: its chain */
			walk(v, e.first_clus, e.ex_nofat, n, p, pass, clusters);
			(*dirs)++;
			if (sp + 1 < CHECK_DEPTH && fat_opendir_entry(v, &e, &stack[sp + 1]) == 0) {
				sp++;
				strcpy(path[sp], p);
			}
		} else {
			n = (e.size + cb - 1u) / cb;
			if (n) walk(v, e.first_clus, e.ex_nofat, n, p, pass, clusters);
			(*files)++;
		}
	}
}

int sd_check_run(fat_vol *v, void (*out)(const char *line))
{
	unsigned int size;
	uint32_t files = 0, dirs = 0, clusters = 0, f2 = 0, d2 = 0, c2 = 0;
	s_out = out;
	s_map = (uint8_t *)ayaneo_import_arena(&size);
	if (!s_map || (v->total_clusters + 7u) / 8u > size) { out("sd-check: no memory for the cluster map"); return -1; }
	memset(s_map, 0, (v->total_clusters + 7u) / 8u);
	s_ndup = 0;
	walk_tree(v, 0, &files, &dirs, &clusters);
	snprintf(s_line, sizeof s_line, "sd-check: %s, %u files, %u folders, %u clusters in use, %d used twice",
		 v->is_exfat ? "exFAT" : v->is_fat32 ? "FAT32" : "FAT16", files, dirs, clusters, s_ndup);
	out(s_line);
	if (s_ndup) walk_tree(v, 1, &f2, &d2, &c2);   /* who holds them */
	return s_ndup;
}
