/* Minimal FAT16/FAT32 write support - see fat_wr.h. */
#include "fat_wr.h"

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int rsec(fat_vol *v, uint32_t lba, uint8_t *b) { return v->rd(v->ctx, lba, 1, b) == 1 ? 0 : -1; }
static int wsec(fat_vol *v, uint32_t lba, const uint8_t *b) { return v->wr(v->ctx, lba, 1, b) == 1 ? 0 : -1; }
static uint32_t clus_lba(fat_vol *v, uint32_t n) { return v->data_start + (n - 2) * v->sec_per_clus; }
static uint32_t eoc(fat_vol *v) { return v->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu; }

/* raw FAT entry (masked); EOC returned as-is (>= eoc threshold) */
static uint32_t get_fat(fat_vol *v, uint32_t n)
{
	uint32_t bo = v->is_fat32 ? n * 4u : n * 2u;
	uint8_t b[512];
	if (rsec(v, v->fat_start + bo / 512u, b) != 0) return eoc(v);
	return v->is_fat32 ? (rd32(b + bo % 512u) & 0x0FFFFFFFu) : rd16(b + bo % 512u);
}

/* write a FAT entry to every FAT copy */
static int set_fat(fat_vol *v, uint32_t n, uint32_t val)
{
	uint32_t bo = v->is_fat32 ? n * 4u : n * 2u;
	uint32_t off = bo % 512u;
	uint8_t b[512]; unsigned f;
	for (f = 0; f < v->num_fats; f++) {
		uint32_t sec = v->fat_start + (uint32_t)f * v->fat_sectors + bo / 512u;
		if (rsec(v, sec, b) != 0) return -1;
		if (v->is_fat32) wr32(b + off, (rd32(b + off) & 0xF0000000u) | (val & 0x0FFFFFFFu));
		else             wr16(b + off, (uint16_t)val);
		if (wsec(v, sec, b) != 0) return -1;
	}
	return 0;
}

/* claim a free cluster (FAT==0), mark EOC, optionally link prev->new. 0 = full. */
static uint32_t alloc_clus(fat_vol *v, uint32_t prev)
{
	uint32_t n;
	for (n = 2; n < v->total_clusters + 2u; n++) {
		if (get_fat(v, n) == 0) {
			if (set_fat(v, n, eoc(v)) != 0) return 0;
			if (prev >= 2 && set_fat(v, prev, n) != 0) return 0;
			return n;
		}
	}
	return 0;
}

static void free_chain(fat_vol *v, uint32_t c)
{
	while (c >= 2 && c < v->total_clusters + 2u) {
		uint32_t nx = get_fat(v, c);
		set_fat(v, c, 0);
		if (nx < 2 || nx >= v->total_clusters + 2u) break;
		c = nx;
	}
}

/* "foo.sav" -> 11-byte padded 8.3 field (uppercased). 0 on ok, -1 if not 8.3. */
static int make_8_3(const char *name, uint8_t out[11])
{
	int i, dot = -1, L = 0, bl, el;
	for (i = 0; i < 11; i++) out[i] = ' ';
	while (name[L]) L++;
	for (i = L - 1; i >= 0; i--) if (name[i] == '.') { dot = i; break; }
	bl = (dot < 0) ? L : dot;
	el = (dot < 0) ? 0 : (L - dot - 1);
	if (bl < 1 || bl > 8 || el > 3) return -1;
	for (i = 0; i < bl; i++) out[i] = (uint8_t)up(name[i]);
	for (i = 0; i < el; i++) out[8 + i] = (uint8_t)up(name[dot + 1 + i]);
	return 0;
}

/* resolve dirpath to (cluster chain head) OR the FAT16 fixed root (cluster 0). */
static int dir_head(fat_vol *v, const char *dirpath, uint32_t *cluster,
		    uint32_t *root_sec, uint32_t *root_left)
{
	fat_dir d;
	if (fat_opendir(v, dirpath, &d) != 0) return -1;
	*cluster = d.cluster; *root_sec = d.root_sec; *root_left = d.root_left;
	return 0;
}

int fat_wr_put(fat_vol *v, const char *dirpath, const char *name,
	       const void *buf, uint32_t len)
{
	const uint8_t *src = buf;
	uint8_t name11[11], sec[512];
	uint32_t cluster, root_sec, root_left, cbytes;
	uint32_t need, first = 0, prev = 0, i, off;
	uint32_t slot_lba = 0, slot_off = 0, old_first = 0;
	int have_slot = 0, replaced = 0;

	if (!v->wr) return -1;
	if (make_8_3(name, name11) != 0) return -2;
	if (dir_head(v, dirpath, &cluster, &root_sec, &root_left) != 0) return -3;
	cbytes = (uint32_t)v->sec_per_clus * 512u;

	/* allocate a fresh chain for the new contents */
	need = len ? (len + cbytes - 1u) / cbytes : 0u;
	for (i = 0; i < need; i++) {
		uint32_t c = alloc_clus(v, prev);
		if (!c) { if (first) free_chain(v, first); return -4; }   /* disk full */
		if (!first) first = c;
		prev = c;
	}

	/* write the data into the chain */
	off = 0;
	{
		uint32_t c = first;
		while (off < len && c >= 2) {
			uint32_t base = clus_lba(v, c), s;
			for (s = 0; s < v->sec_per_clus && off < len; s++) {
				uint32_t n = len - off; if (n > 512u) n = 512u;
				for (i = 0; i < n; i++) sec[i] = src[off + i];
				for (; i < 512u; i++) sec[i] = 0;
				if (wsec(v, base + s, sec) != 0) { free_chain(v, first); return -5; }
				off += n;
			}
			c = get_fat(v, c);
		}
	}

	/* find an existing entry with this 8.3 name (to replace) or a free slot */
	{
		uint32_t c = cluster, rsec_cur = root_sec, rleft = root_left;
		int done = 0;
		while (!done) {
			uint32_t lba, secs, si;
			if (c == 0) {                       /* FAT16 fixed root */
				if (rleft == 0) break;
				lba = rsec_cur; secs = 1;
			} else {
				if (c < 2 || c >= v->total_clusters + 2u) break;
				lba = clus_lba(v, c); secs = v->sec_per_clus;
			}
			for (si = 0; si < secs; si++) {
				uint8_t db[512]; int e;
				if (rsec(v, lba + si, db) != 0) { free_chain(v, first); return -6; }
				for (e = 0; e < 16; e++) {
					uint8_t *rec = db + e * 32;
					if (rec[0] == 0x00) {       /* end of dir -> free slot here */
						slot_lba = lba + si; slot_off = (uint32_t)e * 32; have_slot = 1; done = 1; break;
					}
					if (rec[0] == 0xE5) {       /* deleted -> reusable */
						if (!have_slot) { slot_lba = lba + si; slot_off = (uint32_t)e * 32; have_slot = 1; }
						continue;
					}
					if (rec[11] == 0x0F) continue;                 /* LFN part */
					{ int m = 1, k; for (k = 0; k < 11; k++) if (rec[k] != name11[k]) { m = 0; break; }
					  if (m) {                  /* existing entry: replace in place */
						slot_lba = lba + si; slot_off = (uint32_t)e * 32; have_slot = 1;
						old_first = ((uint32_t)rd16(rec + 20) << 16) | rd16(rec + 26);
						replaced = 1; done = 1; break;
					  } }
				}
				if (done) break;
			}
			if (done) break;
			if (c == 0) { rsec_cur++; rleft--; }
			else c = get_fat(v, c);
		}
	}
	if (!have_slot) { free_chain(v, first); return -7; }   /* dir full (no slot) */

	/* write the directory entry */
	{
		uint8_t db[512]; uint8_t *rec;
		if (rsec(v, slot_lba, db) != 0) { free_chain(v, first); return -8; }
		rec = db + slot_off;
		for (i = 0; i < 11; i++) rec[i] = name11[i];
		rec[11] = 0x20;                      /* archive */
		for (i = 12; i < 32; i++) rec[i] = 0;
		wr16(rec + 20, (uint16_t)(first >> 16));   /* first cluster hi */
		wr16(rec + 26, (uint16_t)(first & 0xFFFF));/* first cluster lo */
		wr32(rec + 28, len);
		if (wsec(v, slot_lba, db) != 0) { free_chain(v, first); return -9; }
	}

	if (replaced && old_first >= 2) free_chain(v, old_first);   /* release old data */

	/* FAT32: the FSINFO free-cluster count is now stale - mark it "unknown"
	 * (0xFFFFFFFF) so the OS recomputes it rather than trusting a wrong value. */
	if (v->is_fat32) {
		uint8_t bs[512];
		if (rsec(v, v->part_lba, bs) == 0) {
			uint16_t fsi = rd16(bs + 0x30);
			if (fsi != 0 && fsi != 0xFFFF && rsec(v, v->part_lba + fsi, bs) == 0) {
				wr32(bs + 488, 0xFFFFFFFFu);   /* free count unknown */
				wr32(bs + 492, 0xFFFFFFFFu);   /* next-free hint unknown */
				wsec(v, v->part_lba + fsi, bs);
			}
		}
	}
	return 0;
}
