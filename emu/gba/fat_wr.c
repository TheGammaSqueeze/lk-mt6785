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

/* ---- directory slot iterator (walks every physical 32B slot in the dir) ---- */
typedef struct {
	fat_vol *v;
	uint32_t cluster;         /* 0 = FAT16 fixed root */
	uint32_t root_sec, root_left;
	uint32_t sec_in_clus;
	int e;                    /* 0..15 within the current sector */
	int have;                 /* sbuf holds cur_lba */
	uint32_t cur_lba;
	uint32_t hops;            /* cluster hops taken (loop guard vs a corrupt chain) */
	uint8_t sbuf[512];
} dir_iter;

static void di_init(dir_iter *it, fat_vol *v, uint32_t cluster, uint32_t root_sec, uint32_t root_left)
{
	it->v = v; it->cluster = cluster; it->root_sec = root_sec; it->root_left = root_left;
	it->sec_in_clus = 0; it->e = 16; it->have = 0; it->cur_lba = 0; it->hops = 0;
}

/* fetch the next physical slot; returns 1 with (lba,off,rec) or 0 at end of dir */
static int di_next(dir_iter *it, uint32_t *lba, uint32_t *off, uint8_t **rec)
{
	fat_vol *v = it->v;
	if (it->e >= 16) {                        /* advance to the next sector */
		uint32_t nl;
		if (it->cluster == 0) {
			if (it->root_left == 0) return 0;
			nl = it->root_sec; it->root_sec++; it->root_left--;
		} else {
			if (it->cluster < 2 || it->cluster >= v->total_clusters + 2u) return 0;
			nl = clus_lba(v, it->cluster) + it->sec_in_clus;
			it->sec_in_clus++;
			if (it->sec_in_clus >= v->sec_per_clus) {
				it->sec_in_clus = 0;
				it->cluster = get_fat(v, it->cluster);
				if (++it->hops > v->total_clusters) return 0;
			}
		}
		if (rsec(v, nl, it->sbuf) != 0) return 0;
		it->cur_lba = nl; it->e = 0; it->have = 1;
	}
	*lba = it->cur_lba; *off = (uint32_t)it->e * 32u; *rec = it->sbuf + it->e * 32;
	it->e++;
	return 1;
}

static uint8_t lfn_checksum(const uint8_t n11[11])
{
	uint8_t s = 0; int i;
	for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + n11[i]);
	return s;
}

/* mark a single slot's first byte = 0xE5 (deleted) */
static int slot_del(fat_vol *v, uint32_t lba, uint32_t off)
{
	uint8_t b[512];
	if (rsec(v, lba, b) != 0) return -1;
	b[off] = 0xE5;
	return wsec(v, lba, b);
}

/* case-insensitive compare of an ascii name vs a reconstructed name */
static int ci_eq(const char *a, const char *b)
{ while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; } return *a == *b; }

/* 8.3 field -> dotted ascii name (for comparing short-only entries) */
static void n11_to_name(const uint8_t n11[11], char *out)
{
	int i, k = 0;
	for (i = 0; i < 8 && n11[i] != ' '; i++) out[k++] = (char)n11[i];
	if (n11[8] != ' ') { out[k++] = '.'; for (i = 8; i < 11 && n11[i] != ' '; i++) out[k++] = (char)n11[i]; }
	out[k] = 0;
}

/* Build a unique short 8.3 (BASE~N.EXT) for a long name, avoiding existing shorts. */
static void mangle_short(fat_vol *v, uint32_t cluster, uint32_t root_sec, uint32_t root_left,
			 const char *name, uint8_t out[11])
{
	int i, dot = -1, L = 0, bl = 0;
	uint8_t base[8], ext[3]; int el = 0;
	int n;
	while (name[L]) L++;
	for (i = L - 1; i >= 0; i--) if (name[i] == '.') { dot = i; break; }
	for (i = 0; (dot < 0 ? i < L : i < dot) && bl < 6; i++) {           /* up to 6 base chars */
		char c = up(name[i]);
		if (c == ' ' || c == '.') continue;
		base[bl++] = (uint8_t)c;
	}
	if (bl == 0) base[bl++] = 'S';
	if (dot >= 0) for (i = dot + 1; name[i] && el < 3; i++) { char c = up(name[i]); if (c != ' ') ext[el++] = (uint8_t)c; }
	for (n = 1; n <= 999; n++) {                                        /* try BASE~n */
		dir_iter it; uint32_t lba, off; uint8_t *rec; int taken = 0, p;
		char tail[6]; int tl = 0, k;
		/* build the 11-byte field */
		for (i = 0; i < 11; i++) out[i] = ' ';
		{ int t = n, digs[4], nd = 0; if (t == 0) digs[nd++] = 0; while (t) { digs[nd++] = t % 10; t /= 10; }
		  tail[tl++] = '~'; for (k = nd - 1; k >= 0; k--) tail[tl++] = (char)('0' + digs[k]); }
		p = bl; if (p + tl > 8) p = 8 - tl;                        /* fit base+~n in 8 */
		for (i = 0; i < p; i++) out[i] = base[i];
		for (i = 0; i < tl; i++) out[p + i] = (uint8_t)tail[i];
		for (i = 0; i < el; i++) out[8 + i] = ext[i];
		/* unique? scan dir shorts */
		di_init(&it, v, cluster, root_sec, root_left);
		while (di_next(&it, &lba, &off, &rec)) {
			if (rec[0] == 0x00) break;
			if (rec[0] == 0xE5 || rec[11] == 0x0F || (rec[11] & 0x08)) continue;
			{ int m = 1, j; for (j = 0; j < 11; j++) if (rec[j] != out[j]) { m = 0; break; } if (m) { taken = 1; break; } }
		}
		if (!taken) return;
	}
}

int fat_wr_put(fat_vol *v, const char *dirpath, const char *name,
	       const void *buf, uint32_t len)
{
	const uint8_t *src = buf;
	uint8_t short11[11], sec[512];
	uint32_t cluster, root_sec, root_left, cbytes;
	uint32_t need_clus, first = 0, prev = 0, i, off, nlen = 0;
	int is83, nlfn, need_slots, s;

	if (!v->wr) return -1;
	if (dir_head(v, dirpath, &cluster, &root_sec, &root_left) != 0) return -3;
	cbytes = (uint32_t)v->sec_per_clus * 512u;
	while (name[nlen]) nlen++;
	if (nlen == 0 || nlen > 255) return -2;

	is83 = (make_8_3(name, short11) == 0);
	nlfn = is83 ? 0 : (int)((nlen + 12u) / 13u);          /* 13 chars per LFN entry */
	need_slots = nlfn + 1;
	if (!is83) mangle_short(v, cluster, root_sec, root_left, name, short11);

	/* allocate + write the data chain */
	need_clus = len ? (len + cbytes - 1u) / cbytes : 0u;
	for (i = 0; i < need_clus; i++) {
		uint32_t c = alloc_clus(v, prev);
		if (!c) { if (first) free_chain(v, first); return -4; }   /* disk full */
		if (!first) first = c;
		prev = c;
	}
	off = 0;
	{
		uint32_t c = first;
		while (off < len && c >= 2) {
			uint32_t base = clus_lba(v, c);
			for (s = 0; s < (int)v->sec_per_clus && off < len; s++) {
				uint32_t n = len - off; if (n > 512u) n = 512u;
				for (i = 0; i < n; i++) sec[i] = src[off + i];
				for (; i < 512u; i++) sec[i] = 0;
				if (wsec(v, base + s, sec) != 0) { free_chain(v, first); return -5; }
				off += n;
			}
			c = get_fat(v, c);
		}
	}

	/* Find need_slots consecutive free slots and write LFN entries + short entry. */
	{
		dir_iter it; uint32_t lba, o; uint8_t *rec;
		uint32_t rl[24], ro[24]; int run = 0, found = 0, idx = 0, k;
		uint8_t cks = lfn_checksum(short11);
		static const int lo[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
		if (need_slots > 24) { free_chain(v, first); return -7; }
		for (;;) {                                    /* extend the dir if it is full */
			run = 0; found = 0;
			di_init(&it, v, cluster, root_sec, root_left);
			while (di_next(&it, &lba, &o, &rec)) {
				if (rec[0] == 0x00 || rec[0] == 0xE5) {
					if (run < 24) { rl[run] = lba; ro[run] = o; }
					if (++run >= need_slots) { found = 1; break; }
				} else run = 0;
			}
			if (found) break;
			/* Full: grow the dir by one zeroed cluster (cluster-chain dirs only; the
			 * FAT16 fixed root cannot grow) and rescan. */
			if (cluster == 0) { free_chain(v, first); return -7; }
			{
				uint32_t last = cluster, nx, nc; unsigned zi; uint8_t zb[512];
				while ((nx = get_fat(v, last)) >= 2 && nx < v->total_clusters + 2u) last = nx;
				nc = alloc_clus(v, last);
				if (!nc) { free_chain(v, first); return -7; }   /* disk full */
				for (k = 0; k < 512; k++) zb[k] = 0;
				for (zi = 0; zi < v->sec_per_clus; zi++)
					if (wsec(v, clus_lba(v, nc) + zi, zb) != 0) { free_chain(v, first); return -7; }
			}
		}

		for (s = nlfn; s >= 1; s--) {                 /* LFN entries, highest seq first */
			uint8_t db[512], *r; int cb = (s - 1) * 13;
			if (rsec(v, rl[idx], db) != 0) { free_chain(v, first); return -8; }
			r = db + ro[idx];
			for (k = 0; k < 32; k++) r[k] = 0;
			r[0] = (uint8_t)(s | (s == nlfn ? 0x40 : 0));
			r[11] = 0x0F; r[13] = cks;
			for (k = 0; k < 13; k++) {
				unsigned u; int ci = cb + k;
				if (ci < (int)nlen) u = (unsigned char)name[ci];
				else if (ci == (int)nlen) u = 0x0000; else u = 0xFFFF;
				r[lo[k]] = (uint8_t)(u & 0xff); r[lo[k] + 1] = (uint8_t)(u >> 8);
			}
			if (wsec(v, rl[idx], db) != 0) { free_chain(v, first); return -8; }
			idx++;
		}
		{                                             /* the short 8.3 entry */
			uint8_t db[512], *r;
			if (rsec(v, rl[idx], db) != 0) { free_chain(v, first); return -9; }
			r = db + ro[idx];
			for (k = 0; k < 32; k++) r[k] = 0;
			for (k = 0; k < 11; k++) r[k] = short11[k];
			r[11] = 0x20;                         /* archive */
			wr16(r + 20, (uint16_t)(first >> 16));
			wr16(r + 26, (uint16_t)(first & 0xFFFF));
			wr32(r + 28, len);
			if (wsec(v, rl[idx], db) != 0) { free_chain(v, first); return -9; }
		}
	}

	/* Replace: NOW that the new entry is fully on disk, delete any OLD entry
	 * (long or short) with the same name and free its chain. Doing this AFTER
	 * the new write means a power loss at any point leaves the old file intact
	 * (before the new dirent lands) or both entries valid (after) - the save is
	 * never lost to a mid-write crash. The just-written entry is identified by
	 * its data chain head `first` and skipped so we do not delete ourselves. */
	{
		dir_iter it; uint32_t lba, o; uint8_t *rec;
		uint32_t g_lba[24], g_off[24]; int gn = 0;
		char lfnacc[300]; int lfnlen = 0;
		static const int lo[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
		di_init(&it, v, cluster, root_sec, root_left);
		while (di_next(&it, &lba, &o, &rec)) {
			if (rec[0] == 0x00) break;
			if (rec[0] == 0xE5) { gn = 0; lfnlen = 0; continue; }
			if (rec[11] == 0x0F) {                       /* LFN part */
				int seq = rec[0] & 0x1F, cb = (seq - 1) * 13, k;
				if (gn < 24) { g_lba[gn] = lba; g_off[gn] = o; gn++; }
				for (k = 0; k < 13; k++) {
					unsigned u = rec[lo[k]] | (rec[lo[k] + 1] << 8);
					if (cb + k < 299) lfnacc[cb + k] = (u && u != 0xFFFF) ? (char)(u & 0x7f) : 0;
				}
				if (cb + 13 > lfnlen) lfnlen = cb + 13;
				continue;
			}
			if (rec[11] & 0x08) { gn = 0; lfnlen = 0; continue; }   /* volume label */
			{
				char eff[300]; int k, match;
				uint32_t of = ((uint32_t)rd16(rec + 20) << 16) | rd16(rec + 26);
				if (gn > 0) { for (k = 0; k < lfnlen && lfnacc[k]; k++) eff[k] = lfnacc[k]; eff[k] = 0; }
				else n11_to_name(rec, eff);
				match = ci_eq(eff, name);
				if (match && of != first) {          /* never delete the entry we just wrote */
					for (k = 0; k < gn; k++) slot_del(v, g_lba[k], g_off[k]);
					slot_del(v, lba, o);
					if (of >= 2) free_chain(v, of);
				}
				gn = 0; lfnlen = 0;
			}
		}
	}

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

/* ---- directory creation (mkdir -p) - 8.3 names only (saves/states/gba) ---- */

/* Write one 32-byte record into the first free slot of the parent dir (cluster
 * chain OR FAT16 fixed root), growing a cluster-chain dir by a zeroed cluster if
 * it is full. Returns 0 on ok. */
static int put_dir_slot(fat_vol *v, uint32_t pcluster, uint32_t proot_sec,
			uint32_t proot_left, const uint8_t rec[32])
{
	dir_iter it; uint32_t lba, o; uint8_t *r;
	for (;;) {
		di_init(&it, v, pcluster, proot_sec, proot_left);
		while (di_next(&it, &lba, &o, &r)) {
			if (r[0] == 0x00 || r[0] == 0xE5) {
				uint8_t db[512]; int k;
				if (rsec(v, lba, db) != 0) return -1;
				for (k = 0; k < 32; k++) db[o + k] = rec[k];
				return wsec(v, lba, db);
			}
		}
		if (pcluster == 0) return -2;             /* FAT16 fixed root cannot grow */
		{
			uint32_t last = pcluster, nx, nc; unsigned zi; uint8_t zb[512]; int k;
			while ((nx = get_fat(v, last)) >= 2 && nx < v->total_clusters + 2u) last = nx;
			nc = alloc_clus(v, last);
			if (!nc) return -3;
			for (k = 0; k < 512; k++) zb[k] = 0;
			for (zi = 0; zi < v->sec_per_clus; zi++)
				if (wsec(v, clus_lba(v, nc) + zi, zb) != 0) return -3;
		}
	}
}

/* Create directory <name> (8.3) in an already-resolved parent. parent_first is
 * the parent dir's first cluster, used for the child's ".." (0 if parent = root). */
static int mkdir_in(fat_vol *v, uint32_t pcluster, uint32_t proot_sec,
		    uint32_t proot_left, uint32_t parent_first, const char *name)
{
	uint8_t s11[11], rec[32], db[512];
	uint32_t c; unsigned zi; int k;
	if (make_8_3(name, s11) != 0) return -1;      /* our dir names are all 8.3 */
	c = alloc_clus(v, 0);                          /* content cluster (EOC, unlinked) */
	if (!c) return -2;
	for (k = 0; k < 512; k++) db[k] = 0;           /* zero the whole cluster first */
	for (zi = 0; zi < v->sec_per_clus; zi++)
		if (wsec(v, clus_lba(v, c) + zi, db) != 0) return -3;
	{                                              /* first sector: "." and ".." */
		uint8_t *e0 = db, *e1 = db + 32; int i;
		for (k = 0; k < 512; k++) db[k] = 0;
		for (i = 0; i < 11; i++) e0[i] = ' ';
		e0[0] = '.'; e0[11] = 0x10;
		wr16(e0 + 20, (uint16_t)(c >> 16)); wr16(e0 + 26, (uint16_t)(c & 0xFFFF));
		for (i = 0; i < 11; i++) e1[i] = ' ';
		e1[0] = '.'; e1[1] = '.'; e1[11] = 0x10;
		wr16(e1 + 20, (uint16_t)(parent_first >> 16));
		wr16(e1 + 26, (uint16_t)(parent_first & 0xFFFF));
	}
	if (wsec(v, clus_lba(v, c), db) != 0) return -3;
	{                                              /* the parent's entry for this dir */
		int i;
		for (i = 0; i < 32; i++) rec[i] = 0;
		for (i = 0; i < 11; i++) rec[i] = s11[i];
		rec[11] = 0x10;                        /* directory attribute */
		wr16(rec + 20, (uint16_t)(c >> 16));
		wr16(rec + 26, (uint16_t)(c & 0xFFFF));
		wr32(rec + 28, 0);                     /* dirs carry size 0 */
	}
	return put_dir_slot(v, pcluster, proot_sec, proot_left, rec);
}

/* mkdir -p: create every missing component of an absolute path of 8.3 names.
 * Idempotent (existing components are descended into). 0 on success. */
int fat_wr_mkpath(fat_vol *v, const char *path)
{
	char cur[256];
	const char *p = path;
	int curlen;
	if (!v->wr) return -1;
	cur[0] = '/'; cur[1] = 0; curlen = 1;
	while (*p == '/') p++;
	while (*p) {
		char child[256]; int ci = 0, n = 0;
		fat_dir d;
		int parent_is_root = (curlen == 1);
		int i;
		for (i = 0; i < curlen; i++) child[ci++] = cur[i];
		if (!parent_is_root) child[ci++] = '/';
		while (*p && *p != '/' && ci < 255) { child[ci++] = *p++; n++; }
		child[ci] = 0;
		while (*p == '/') p++;
		if (n == 0) break;
		if (fat_opendir(v, child, &d) != 0) {         /* missing -> create it */
			char comp[64]; int k = 0;
			uint32_t pcl, prs, prl, parent_first;
			for (i = ci - n; i < ci; i++) comp[k++] = child[i];
			comp[k] = 0;
			if (dir_head(v, cur, &pcl, &prs, &prl) != 0) return -2;
			parent_first = parent_is_root ? 0u : pcl;
			{
				int rc = mkdir_in(v, pcl, prs, prl, parent_first, comp);
				if (rc != 0) return rc;
			}
		}
		for (i = 0; i <= ci; i++) cur[i] = child[i];  /* descend: cur = child */
		curlen = ci;
	}
	return 0;
}
