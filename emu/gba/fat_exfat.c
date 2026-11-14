/* exFAT backend - see fat_exfat.h. */
#include "fat_exfat.h"

/* ---- small helpers (no libc in LK) ---- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void ex_memcpy(void *d, const void *s, unsigned n)
{ unsigned char *dp = d; const unsigned char *sp = s; while (n--) *dp++ = *sp++; }
static void ex_memset(void *d, int c, unsigned n) { unsigned char *dp = d; while (n--) *dp++ = (unsigned char)c; }

static int rsec(fat_vol *v, uint32_t lba, uint8_t *b) { return v->rd(v->ctx, lba, 1, b) == 1 ? 0 : -1; }
static int wsec(fat_vol *v, uint32_t lba, const uint8_t *b) { return v->wr && v->wr(v->ctx, lba, 1, b) == 1 ? 0 : -1; }
static uint32_t clus_lba(fat_vol *v, uint32_t n) { return v->ex_heap_lba + ((n - 2u) << v->ex_spc_shift); }
static uint32_t cbytes(fat_vol *v) { return v->ex_spc * 512u; }
static int clus_ok(fat_vol *v, uint32_t n) { return n >= 2u && n < v->total_clusters + 2u; }

/* entry types */
#define ET_BITMAP   0x81
#define ET_UPCASE   0x82
#define ET_LABEL    0x83
#define ET_FILE     0x85
#define ET_STREAM   0xC0
#define ET_NAME     0xC1
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define SF_ALLOC    0x01   /* stream GeneralSecondaryFlags: AllocationPossible */
#define SF_NOFAT    0x02   /* stream GeneralSecondaryFlags: NoFatChain (contiguous) */

/* ---- FAT (one sector cached) ---- */
static uint32_t fat_next(fat_vol *v, uint32_t n, uint8_t *fatb, uint32_t *fatb_sec)
{
	uint32_t sec = v->ex_fat_lba + (n >> 7), off = (n & 127u) * 4u, val;
	if (!clus_ok(v, n)) return 0;
	if (sec != *fatb_sec) { if (v->rd(v->ctx, sec, 1, fatb) != 1) return 0; *fatb_sec = sec; }
	val = rd32(fatb + off);
	return (val >= 0xFFFFFFF8u || !clus_ok(v, val)) ? 0 : val;
}
/* the cluster that holds byte offset `pos` of a chain / run starting at `first` */
static uint32_t clus_at(fat_vol *v, uint32_t first, int nofat, uint32_t pos, uint8_t *fatb, uint32_t *fatb_sec)
{
	uint32_t skip = pos / cbytes(v), c = first, i;
	if (nofat) return clus_ok(v, first + skip) ? first + skip : 0;
	for (i = 0; i < skip && c; i++) c = fat_next(v, c, fatb, fatb_sec);
	return c;
}

/* ---- mount ---- */
int exfat_mount(fat_vol *v, uint32_t part_lba, const uint8_t *b)
{
	uint32_t fat_off = rd32(b + 0x50), fat_len = rd32(b + 0x54), heap_off = rd32(b + 0x58);
	uint32_t ccount = rd32(b + 0x5C), root = rd32(b + 0x60);
	uint8_t bps_shift = b[0x6C], spc_shift = b[0x6D], nfats = b[0x6E];
	uint16_t flags = rd16(b + 0x6A);
	if (bps_shift != 9) return -1;                      /* 512 byte sectors only */
	if (spc_shift > 16 || nfats < 1 || nfats > 2) return -1;
	if (fat_off == 0 || fat_len == 0 || heap_off == 0 || ccount == 0) return -1;
	if (heap_off < fat_off + fat_len * nfats) return -1;
	if (root < 2u || root >= ccount + 2u) return -1;
	v->is_exfat = 1; v->is_fat32 = 0;
	v->part_lba = part_lba;
	v->ex_spc_shift = spc_shift; v->ex_spc = 1u << spc_shift;
	v->ex_fat_len = fat_len;
	v->ex_fat_lba = part_lba + fat_off + ((flags & 1u) && nfats == 2 ? fat_len : 0u);   /* ActiveFat */
	v->ex_heap_lba = part_lba + heap_off;
	v->total_clusters = ccount;
	v->root_cluster = root;
	v->sec_per_clus = (uint8_t)(v->ex_spc > 255u ? 255u : v->ex_spc);   /* informational only */
	v->num_fats = nfats;
	v->bytes_per_sec = 512;
	v->ex_bitmap_clus = 0; v->ex_bitmap_len = 0; v->ex_upcase_clus = 0; v->ex_upcase_len = 0;
	/* the root holds the bitmap and up-case entries; walk its first cluster(s) for them */
	{
		uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu, c = root, hops = 0;
		while (clus_ok(v, c) && hops++ < 64u) {
			uint32_t s;
			for (s = 0; s < v->ex_spc; s++) {
				int i;
				uint8_t sb[512];
				if (rsec(v, clus_lba(v, c) + s, sb) != 0) return -2;
				for (i = 0; i < 16; i++) {
					const uint8_t *e = sb + i * 32;
					if (e[0] == 0) goto done;
					if (e[0] == ET_BITMAP && !(e[1] & 1u)) { v->ex_bitmap_clus = rd32(e + 0x14); v->ex_bitmap_len = rd32(e + 0x18); }
					else if (e[0] == ET_UPCASE) { v->ex_upcase_clus = rd32(e + 0x14); v->ex_upcase_len = rd32(e + 0x18); }
				}
			}
			c = fat_next(v, c, fatb, &fatb_sec);
		}
	}
done:
	if (!clus_ok(v, v->ex_bitmap_clus) || v->ex_bitmap_len < (ccount + 7u) / 8u) return -3;
	return 0;
}

/* ---- directory iteration ---- */
static void dir_set(fat_vol *v, fat_dir *d, uint32_t first, int nofat, uint32_t len)
{
	d->v = v; d->end = 0; d->hops = 0; d->ent_in_sec = 0; d->sec_in_clus = 0;
	d->cluster = first; d->ex_first = first; d->ex_nofat = (uint8_t)nofat; d->ex_len = len; d->ex_pos = 0;
	d->root_sec = 0; d->root_left = 0;
}
/* next raw 32 byte entry; 0 at end */
static int next_raw(fat_dir *d, uint8_t out[32])
{
	fat_vol *v = d->v;
	uint8_t fatb[512], sb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	if (d->end) return 0;
	if (d->ex_len && d->ex_pos >= d->ex_len) { d->end = 1; return 0; }
	if (!clus_ok(v, d->cluster)) { d->end = 1; return 0; }
	if (rsec(v, clus_lba(v, d->cluster) + d->sec_in_clus, sb) != 0) { d->end = 1; return 0; }
	ex_memcpy(out, sb + d->ent_in_sec * 32, 32);
	d->ex_pos += 32;
	if (++d->ent_in_sec >= 16) {
		d->ent_in_sec = 0;
		if (++d->sec_in_clus >= v->ex_spc) {
			d->sec_in_clus = 0;
			if (d->ex_nofat) d->cluster++;
			else d->cluster = fat_next(v, d->cluster, fatb, &fatb_sec);
			if (!clus_ok(v, d->cluster) || ++d->hops > v->total_clusters) d->end = 1;
		}
	}
	if (out[0] == 0) { d->end = 1; return 0; }
	return 1;
}

int exfat_readdir(fat_dir *d, fat_dirent *e)
{
	uint8_t rec[32];
	while (next_raw(d, rec)) {
		int secs, got_stream = 0, nlen = 0, ni = 0, k, uo = 0, ufull = 0;
		uint8_t attr;
		uint32_t hi = 0;                                 /* a high surrogate waiting for its pair */
		uint16_t tail[16];                               /* the name's last units: its extension */
		if (rec[0] != ET_FILE) continue;                 /* deleted, bitmap, label, up-case, unknown */
		secs = rec[1]; attr = rec[4];
		e->name[0] = 0; e->size = 0; e->first_clus = 0; e->ex_nofat = 0; e->ex_dir_len = 0;
		e->is_dir = (attr & ATTR_DIR) ? 1 : 0;
		for (k = 0; k < secs; k++) {
			uint8_t s[32]; int j;
			if (!next_raw(d, s)) return 0;
			if (s[0] == ET_STREAM) {
				got_stream = 1;
				nlen = s[3];
				e->ex_nofat = (s[1] & SF_NOFAT) ? 1 : 0;
				e->first_clus = rd32(s + 0x14);
				e->size = rd32(s + 0x18);                /* low 32 bits: our files are far smaller */
				e->ex_dir_len = e->size;
				if (rd32(s + 0x1C)) e->size = 0xFFFFFFFFu;
			} else if (s[0] == ET_NAME) {
				/* UTF-8 made as the units come, up to the last character that fits whole */
				for (j = 0; j < 15 && ni < nlen && ni < 255; j++, ni++) {
					uint32_t u = rd16(s + 2 + j * 2), c;
					tail[ni & 15] = (uint16_t)u;
					if (u >= 0xD800 && u < 0xDC00) { c = hi ? 0xFFFDu : 0; hi = u; }
					else if (u >= 0xDC00 && u < 0xE000) { c = hi ? 0x10000u + ((hi - 0xD800u) << 10) + (u - 0xDC00u) : 0xFFFDu; hi = 0; }
					else {
						if (hi && !ufull) { int r = fat_u8_put(e->name, uo, 256, 0xFFFDu); if (r < 0) ufull = 1; else uo = r; }
						c = u; hi = 0;
					}
					if (c && !ufull) { int r = fat_u8_put(e->name, uo, 256, c); if (r < 0) ufull = 1; else uo = r; }
				}
			}
		}
		if (hi && !ufull) { int r = fat_u8_put(e->name, uo, 256, 0xFFFDu); if (r >= 0) uo = r; else ufull = 1; }
		e->name[uo] = 0;
		if (ufull) {	/* too long for the name: it keeps its extension */
			uint16_t t[16];
			int nt = ni < 16 ? ni : 16;
			for (k = 0; k < nt; k++) t[k] = tail[(ni - nt + k) & 15];
			uo = fat_name_keep_ext(e->name, uo, 256, t, nt);
		}
		if (!got_stream || ni == 0) continue;
		if (e->is_dir) e->size = 0;
		return 1;
	}
	return 0;
}

/* a directory a listing found (its entry), positioned at its start */
void exfat_dir_at(fat_vol *v, fat_dir *d, const fat_dirent *e)
{
	dir_set(v, d, e->first_clus, e->ex_nofat, e->ex_dir_len);
}

/* resolve a directory path; fills d (position at its start) */
int exfat_opendir(fat_vol *v, const char *path, fat_dir *d)
{
	fat_dir cur;
	const char *p = path;
	dir_set(v, &cur, v->root_cluster, 0, 0);
	while (*p == '/') p++;
	while (*p) {
		char comp[256]; int n = 0, found = 0;
		fat_dirent e;
		while (*p && *p != '/' && n < 255) comp[n++] = *p++;
		comp[n] = 0;
		while (*p == '/') p++;
		if (n == 0) break;
		while (exfat_readdir(&cur, &e)) {
			if (e.is_dir && fat_name_eq(comp, e.name)) { dir_set(v, &cur, e.first_clus, e.ex_nofat, e.ex_dir_len); found = 1; break; }
		}
		if (!found) return -2;
	}
	*d = cur;
	return 0;
}

static void split_path(const char *path, char *dir, const char **fname)
{
	const char *slash = 0, *p;
	int dn, i;
	for (p = path; *p; p++) if (*p == '/') slash = p;
	dn = slash ? (int)(slash - path) : 0;
	if (dn > 255) dn = 255;
	for (i = 0; i < dn; i++) dir[i] = path[i];
	dir[dn] = 0;
	if (dn == 0) { dir[0] = '/'; dir[1] = 0; }
	*fname = slash ? slash + 1 : path;
}

int exfat_open(fat_vol *v, const char *path, fat_file *f)
{
	char dir[256]; const char *fname;
	fat_dir d; fat_dirent e;
	split_path(path, dir, &fname);
	if (exfat_opendir(v, dir, &d) != 0) return -2;
	while (exfat_readdir(&d, &e)) {
		if (!e.is_dir && fat_name_eq(fname, e.name)) {
			f->v = v; f->first_clus = e.first_clus; f->size = e.size; f->ex_nofat = e.ex_nofat;
			return 0;
		}
	}
	return -3;
}

uint32_t exfat_read(fat_file *f, uint32_t off, void *buf, uint32_t len)
{
	fat_vol *v = f->v;
	uint32_t cb = cbytes(v), got = 0, clus, inclus;
	uint8_t *out = buf;
	uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	if (off >= f->size || !f->first_clus) return 0;
	if (off + len > f->size) len = f->size - off;
	clus = clus_at(v, f->first_clus, f->ex_nofat, off, fatb, &fatb_sec);
	inclus = off % cb;
	while (len && clus_ok(v, clus)) {
		if (inclus == 0 && len >= cb) {
			/* whole clusters: coalesce a contiguous run into one transfer */
			uint32_t want = len / cb, run = 1, c = clus, succ, nsec;
			if (f->ex_nofat) { run = want; succ = clus + want; }
			else {
				succ = fat_next(v, c, fatb, &fatb_sec);
				while (run < want && succ == c + 1u) { c = succ; run++; succ = fat_next(v, c, fatb, &fatb_sec); }
			}
			nsec = run << v->ex_spc_shift;
			if (v->rd(v->ctx, clus_lba(v, clus), nsec, out + got) != nsec) return got;
			got += nsec * 512u; len -= nsec * 512u;
			clus = succ;
			continue;
		}
		{
			uint32_t sec = inclus / 512u, so = inclus % 512u, lba = clus_lba(v, clus) + sec;
			while (sec < v->ex_spc && len) {
				if (so == 0 && len >= 512u) {
					uint32_t maxsec = v->ex_spc - sec, wantsec = len / 512u, nsec = wantsec < maxsec ? wantsec : maxsec;
					if (v->rd(v->ctx, lba, nsec, out + got) != nsec) return got;
					got += nsec * 512u; len -= nsec * 512u; sec += nsec; lba += nsec;
				} else {
					uint32_t n = 512u - so; uint8_t sb[512];
					if (n > len) n = len;
					if (rsec(v, lba, sb) != 0) return got;
					ex_memcpy(out + got, sb + so, n);
					got += n; len -= n; so = 0; sec++; lba++;
				}
			}
			inclus = 0;
			if (len) clus = f->ex_nofat ? clus + 1u : fat_next(v, clus, fatb, &fatb_sec);
		}
	}
	return got;
}

/* ======================= write side ======================= */

/* allocation bitmap: one cached sector, write-back */
typedef struct { fat_vol *v; uint8_t buf[512]; uint32_t sec; int valid, dirty; } bm_t;
static uint32_t bm_sector_lba(fat_vol *v, uint32_t byteoff)
{
	/* the bitmap is a plain file in the cluster heap; it is small (ClusterCount/8 bytes) and
	 * mkfs places it contiguously, but follow the chain to be safe */
	uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	uint32_t c = clus_at(v, v->ex_bitmap_clus, 0, byteoff, fatb, &fatb_sec);
	if (!c) return 0;
	return clus_lba(v, c) + (byteoff % cbytes(v)) / 512u;
}
static int bm_flush(bm_t *b) { if (b->valid && b->dirty) { if (wsec(b->v, b->sec, b->buf) != 0) return -1; b->dirty = 0; } return 0; }
static int bm_load(bm_t *b, uint32_t byteoff)
{
	uint32_t lba = bm_sector_lba(b->v, byteoff & ~511u);
	if (!lba) return -1;
	if (b->valid && b->sec == lba) return 0;
	if (bm_flush(b) != 0) return -1;
	if (rsec(b->v, lba, b->buf) != 0) { b->valid = 0; return -1; }
	b->sec = lba; b->valid = 1;
	return 0;
}
static int bm_get(bm_t *b, uint32_t clus)   /* 1 = allocated, -1 = error */
{
	uint32_t bit = clus - 2u;
	if (bm_load(b, bit / 8u) != 0) return -1;
	return (b->buf[(bit / 8u) & 511u] >> (bit & 7u)) & 1u;
}
static int bm_set(bm_t *b, uint32_t clus, int on)
{
	uint32_t bit = clus - 2u;
	if (bm_load(b, bit / 8u) != 0) return -1;
	if (on) b->buf[(bit / 8u) & 511u] |= (uint8_t)(1u << (bit & 7u));
	else    b->buf[(bit / 8u) & 511u] &= (uint8_t)~(1u << (bit & 7u));
	b->dirty = 1;
	return 0;
}
/* find `n` free clusters in a row; 0 if none */
static uint32_t bm_find_run(bm_t *b, uint32_t n)
{
	fat_vol *v = b->v;
	uint32_t c, run = 0, start = 0;
	for (c = 2u; c < v->total_clusters + 2u; c++) {
		int a = bm_get(b, c);
		if (a < 0) return 0;
		if (!a) { if (run == 0) start = c; if (++run >= n) return start; }
		else run = 0;
	}
	return 0;
}

/* FAT writes for chained files (rare: only when no contiguous run is free) */
static int fat_set(fat_vol *v, uint32_t n, uint32_t val)
{
	uint8_t b[512]; uint32_t sec = v->ex_fat_lba + (n >> 7), off = (n & 127u) * 4u;
	if (rsec(v, sec, b) != 0) return -1;
	wr32(b + off, val);
	return wsec(v, sec, b);
}

/* ---- entry set helpers ---- */
static uint16_t set_checksum(const uint8_t *ents, int nents)
{
	uint16_t s = 0; int i;
	for (i = 0; i < nents * 32; i++) {
		if (i == 2 || i == 3) continue;
		s = (uint16_t)(((s & 1u) ? 0x8000u : 0u) + (s >> 1) + ents[i]);
	}
	return s;
}
/* up-case one UTF-16 unit through the volume's table (compressed form: a 0xFFFF marker
 * followed by a count means "identity for the next count code points"). ASCII only needs
 * the plain rule, but the hash must match what Windows computes, so read the table. */
static uint16_t upcase_unit(fat_vol *v, uint16_t ch)
{
	uint32_t pos = 0, code = 0, fpos;
	uint8_t fatb[512], sb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	if (!v->ex_upcase_clus) return (ch >= 'a' && ch <= 'z') ? (uint16_t)(ch - 32) : ch;
	for (fpos = 0; fpos + 2u <= v->ex_upcase_len; ) {
		uint32_t c = clus_at(v, v->ex_upcase_clus, 0, fpos, fatb, &fatb_sec);
		uint32_t lba, so; uint16_t u;
		if (!c) break;
		lba = clus_lba(v, c) + (fpos % cbytes(v)) / 512u; so = fpos % 512u;
		if (rsec(v, lba, sb) != 0) break;
		u = rd16(sb + so);
		fpos += 2;
		if (u == 0xFFFF) {                                /* run of identity mappings */
			uint32_t cnt;
			c = clus_at(v, v->ex_upcase_clus, 0, fpos, fatb, &fatb_sec);
			if (!c) break;
			lba = clus_lba(v, c) + (fpos % cbytes(v)) / 512u; so = fpos % 512u;
			if (rsec(v, lba, sb) != 0) break;
			cnt = rd16(sb + so);
			fpos += 2;
			if (ch >= code && ch < code + cnt) return ch;
			code += cnt;
			continue;
		}
		if (code == ch) return u;
		code++;
		pos++;
		if (code > ch) break;
	}
	return (ch >= 'a' && ch <= 'z') ? (uint16_t)(ch - 32) : ch;
}
static uint16_t name_hash(fat_vol *v, const char *name, int nlen)
{
	uint16_t h = 0; int i;
	for (i = 0; i < nlen; i++) {
		uint16_t u = upcase_unit(v, fat_u8_unit_at(name, i));
		h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (h >> 1) + (u & 0xffu));
		h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (h >> 1) + (u >> 8));
	}
	return h;
}
/* a fixed, valid exFAT timestamp: 2026-01-01 00:00:00 (year since 1980 in bits 25..31) */
static uint32_t fixed_time(void) { return ((2026u - 1980u) << 25) | (1u << 21) | (1u << 16); }

/* ---- directory slot iterator over physical entries (position aware) ---- */
typedef struct { fat_dir d; uint32_t lba; int ent; } dit_t;
static void dit_init(dit_t *it, fat_vol *v, const fat_dir *dir) { it->d = *dir; dir_set(v, &it->d, dir->ex_first, dir->ex_nofat, dir->ex_len); }
/* next physical entry: 1 with rec/lba/ent, 0 when the directory's allocation ends (past the
 * 0x00 terminator too, so free slots after the end are visible) */
static int dit_next(dit_t *it, uint8_t rec[32])
{
	fat_dir *d = &it->d; fat_vol *v = d->v;
	uint8_t fatb[512], sb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	if (d->end) return 0;
	if (d->ex_len && d->ex_pos >= d->ex_len) { d->end = 1; return 0; }
	if (!clus_ok(v, d->cluster)) { d->end = 1; return 0; }
	it->lba = clus_lba(v, d->cluster) + d->sec_in_clus; it->ent = (int)d->ent_in_sec;
	if (rsec(v, it->lba, sb) != 0) { d->end = 1; return 0; }
	ex_memcpy(rec, sb + d->ent_in_sec * 32, 32);
	d->ex_pos += 32;
	if (++d->ent_in_sec >= 16) {
		d->ent_in_sec = 0;
		if (++d->sec_in_clus >= v->ex_spc) {
			d->sec_in_clus = 0;
			if (d->ex_nofat) d->cluster++;
			else d->cluster = fat_next(v, d->cluster, fatb, &fatb_sec);
			if (!clus_ok(v, d->cluster) || ++d->hops > v->total_clusters) d->end = 1;
		}
	}
	return 1;
}
static int write_entry(fat_vol *v, uint32_t lba, int ent, const uint8_t rec[32])
{
	uint8_t b[512];
	if (rsec(v, lba, b) != 0) return -1;
	ex_memcpy(b + ent * 32, rec, 32);
	return wsec(v, lba, b);
}

/* free the clusters of a file: bitmap bits (+ nothing in the FAT for a contiguous run; the
 * chain entries of a chained file are left, exFAT ignores FAT contents of free clusters) */
static void free_clusters(fat_vol *v, bm_t *bm, uint32_t first, int nofat, uint32_t len)
{
	uint32_t n = len ? (len + cbytes(v) - 1u) / cbytes(v) : 0u, i, c = first;
	uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	for (i = 0; i < n && clus_ok(v, c); i++) {
		uint32_t nx = nofat ? c + 1u : fat_next(v, c, fatb, &fatb_sec);
		bm_set(bm, c, 0);
		c = nx;
	}
}

/* resolve dirpath to a fat_dir plus, for a subdirectory, the location of its stream entry in
 * the parent (needed to grow the directory); the root has none (lba 0) */
static int dir_resolve(fat_vol *v, const char *dirpath, fat_dir *out, uint32_t *str_lba, int *str_ent)
{
	fat_dir cur; const char *p = dirpath;
	*str_lba = 0; *str_ent = 0;
	dir_set(v, &cur, v->root_cluster, 0, 0);
	while (*p == '/') p++;
	while (*p) {
		char comp[256]; int n = 0, found = 0;
		while (*p && *p != '/' && n < 255) comp[n++] = *p++;
		comp[n] = 0;
		while (*p == '/') p++;
		if (n == 0) break;
		{
			dit_t it; uint8_t rec[32];
			dit_init(&it, v, &cur);
			while (dit_next(&it, rec)) {
				int secs, k, nlen = 0, ni = 0; uint16_t nm[255]; uint8_t attr;
				uint32_t first = 0, dlen = 0, slba = 0; int sent = 0, nofat = 0;
				if (rec[0] == 0) break;
				if (rec[0] != ET_FILE) continue;
				secs = rec[1]; attr = rec[4];
				for (k = 0; k < secs; k++) {
					uint8_t s[32]; int j;
					if (!dit_next(&it, s)) break;
					if (s[0] == ET_STREAM) { nlen = s[3]; nofat = (s[1] & SF_NOFAT) ? 1 : 0; first = rd32(s + 0x14); dlen = rd32(s + 0x18); slba = it.lba; sent = it.ent; }
					else if (s[0] == ET_NAME) for (j = 0; j < 15 && ni < nlen && ni < 255; j++, ni++) nm[ni] = rd16(s + 2 + j * 2);
				}
				if ((attr & ATTR_DIR) && fat_u16_is(comp, nm, ni)) {
					dir_set(v, &cur, first, nofat, dlen); *str_lba = slba; *str_ent = sent; found = 1; break;
				}
			}
		}
		if (!found) return -1;
	}
	*out = cur;
	return 0;
}

/* rewrite a directory's stream entry in its parent (lengths + NoFatChain flag) and refresh
 * the set checksum, which covers the stream entry */
static int update_stream(fat_vol *v, uint32_t str_lba, int str_ent, uint32_t new_len, int nofat)
{
	uint8_t b[512], *e, fb[512], set[19 * 32];
	uint32_t flba = str_lba; int fent = str_ent - 1, secs, k; uint32_t lba2; int e2;
	if (rsec(v, str_lba, b) != 0) return -1;
	e = b + str_ent * 32;
	e[1] = (uint8_t)((e[1] & ~SF_NOFAT) | (nofat ? SF_NOFAT : 0));
	wr32(e + 0x08, new_len); wr32(e + 0x0C, 0); wr32(e + 0x18, new_len); wr32(e + 0x1C, 0);
	if (wsec(v, str_lba, b) != 0) return -1;
	/* the file entry precedes the stream entry (same or previous sector) */
	if (fent < 0) { flba = str_lba - 1u; fent = 15; }
	if (rsec(v, flba, fb) != 0) return -1;
	secs = fb[fent * 32 + 1];
	if (secs < 2 || secs > 18) return -1;
	ex_memcpy(set, fb + fent * 32, 32);
	lba2 = flba; e2 = fent;
	for (k = 1; k <= secs; k++) {
		uint8_t sb[512];
		if (++e2 >= 16) { e2 = 0; lba2++; }
		if (rsec(v, lba2, sb) != 0) return -1;
		ex_memcpy(set + k * 32, sb + e2 * 32, 32);
	}
	wr16(set + 2, set_checksum(set, secs + 1));
	ex_memcpy(fb + fent * 32, set, 32);
	return wsec(v, flba, fb);
}

/* grow a directory by one cluster. A contiguous directory extends in place when the next
 * cluster is free; otherwise it is converted to a FAT chain (its clusters linked in the FAT,
 * NoFatChain cleared in the parent) and the new cluster linked on. The root is always chained
 * and has no length to maintain. */
static int dir_grow(fat_vol *v, bm_t *bm, fat_dir *dir, uint32_t str_lba, int str_ent)
{
	uint32_t cb = cbytes(v), nclus = dir->ex_len ? (dir->ex_len + cb - 1u) / cb : 0u, nc, s;
	uint8_t zb[512];
	ex_memset(zb, 0, 512);
	if (dir->ex_nofat && dir->ex_len) {
		nc = dir->ex_first + nclus;
		if (clus_ok(v, nc) && bm_get(bm, nc) == 0) {
			if (bm_set(bm, nc, 1) != 0) return -1;
		} else {
			uint32_t i;
			nc = bm_find_run(bm, 1u);
			if (!nc) return -1;
			for (i = 0; i + 1u < nclus; i++) if (fat_set(v, dir->ex_first + i, dir->ex_first + i + 1u) != 0) return -1;
			if (fat_set(v, dir->ex_first + nclus - 1u, nc) != 0 || fat_set(v, nc, 0xFFFFFFFFu) != 0) return -1;
			if (bm_set(bm, nc, 1) != 0) return -1;
			dir->ex_nofat = 0;
		}
	} else {
		uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu, last = dir->ex_first, nx, hops = 0;
		while ((nx = fat_next(v, last, fatb, &fatb_sec)) != 0 && hops++ < v->total_clusters) last = nx;
		nc = bm_find_run(bm, 1u);
		if (!nc) return -1;
		if (bm_set(bm, nc, 1) != 0 || fat_set(v, nc, 0xFFFFFFFFu) != 0 || fat_set(v, last, nc) != 0) return -1;
	}
	for (s = 0; s < v->ex_spc; s++) if (wsec(v, clus_lba(v, nc) + s, zb) != 0) return -1;
	if (str_lba) {
		if (update_stream(v, str_lba, str_ent, dir->ex_len + cb, dir->ex_nofat) != 0) return -1;
		dir->ex_len += cb;
	}
	return 0;
}

/* find `need` consecutive free entries (deleted or past the end); returns 1 and the position
 * (lba, ent of the first) through the two out parameters */
static int find_slots(fat_vol *v, fat_dir *dir, int need, uint32_t *lba, int *ent)
{
	dit_t it; uint8_t rec[32]; int run = 0; uint32_t rl = 0; int re = 0;
	dit_init(&it, v, dir);
	while (dit_next(&it, rec)) {
		if (!(rec[0] & 0x80)) {                      /* free (0x00 end or deleted) */
			if (run == 0) { rl = it.lba; re = it.ent; }
			if (++run >= need) { *lba = rl; *ent = re; return 1; }
		} else run = 0;
	}
	return 0;
}
/* write `n` entries starting at (lba, ent), crossing sectors (the directory's sectors are
 * consecutive inside a cluster; the slot search guarantees the run stays inside the
 * directory's allocation) */
static int write_entries(fat_vol *v, fat_dir *dir, uint32_t lba, int ent, const uint8_t *ents, int n)
{
	int i;
	uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
	for (i = 0; i < n; i++) {
		if (write_entry(v, lba, ent, ents + i * 32) != 0) return -1;
		/* on to the next entry's place only when there is one: a set that ends on the last
		 * entry of the directory's last sector must not look past its allocation (that failed
		 * the write after the set was on the card, which then pointed at freed clusters) */
		if (i + 1 == n) break;
		if (++ent >= 16) {
			ent = 0;
			/* next sector: within the cluster, or the next cluster of the directory */
			{
				uint32_t rel = lba - v->ex_heap_lba, c = (rel >> v->ex_spc_shift) + 2u, s = rel & (v->ex_spc - 1u);
				if (s + 1u < v->ex_spc) lba++;
				else {
					uint32_t nc = dir->ex_nofat ? c + 1u : fat_next(v, c, fatb, &fatb_sec);
					if (!clus_ok(v, nc)) return -1;
					lba = clus_lba(v, nc);
				}
			}
		}
	}
	return 0;
}

/* build the file + stream + name entries for a UTF-8 name of nlen UTF-16 units */
static int build_set(fat_vol *v, uint8_t *set, const char *name, int nlen, uint16_t attr,
		     uint32_t first, uint32_t len, int nofat)
{
	int nents = (nlen + 14) / 15, k, i;
	uint32_t t = fixed_time();
	if (nlen < 1 || nlen > 255) return -1;
	ex_memset(set, 0, (2 + nents) * 32);
	set[0] = ET_FILE; set[1] = (uint8_t)(1 + nents);
	wr16(set + 4, attr);
	wr32(set + 8, t); wr32(set + 12, t); wr32(set + 16, t);
	set[32 + 0] = ET_STREAM;
	set[32 + 1] = (uint8_t)(SF_ALLOC | (nofat && first ? SF_NOFAT : 0));
	set[32 + 3] = (uint8_t)nlen;
	wr16(set + 32 + 4, name_hash(v, name, nlen));
	wr32(set + 32 + 8, len); wr32(set + 32 + 12, 0);
	wr32(set + 32 + 0x14, first);
	wr32(set + 32 + 0x18, len); wr32(set + 32 + 0x1C, 0);
	for (k = 0; k < nents; k++) {
		uint8_t *e = set + (2 + k) * 32;
		e[0] = ET_NAME;
		for (i = 0; i < 15; i++) {
			int ci = k * 15 + i;
			wr16(e + 2 + i * 2, ci < nlen ? fat_u8_unit_at(name, ci) : 0u);
		}
	}
	wr16(set + 2, set_checksum(set, 2 + nents));
	return 2 + nents;
}

/* delete the entry sets in `dir` named `name`: every file whose first cluster is not `clus`
 * (only = 0, a replace), or the file or (empty) folder that starts at `clus` (only = 1) */
static void delete_match(fat_vol *v, bm_t *bm, fat_dir *dir, const char *name, uint32_t clus, int only)
{
	dit_t it; uint8_t rec[32];
	dit_init(&it, v, dir);
	while (dit_next(&it, rec)) {
		uint32_t locs_lba[20]; int locs_ent[20], nl = 0;
		int secs, k, nlen = 0, ni = 0, nofat = 0; uint16_t nm[255]; uint32_t first = 0, dlen = 0;
		if (rec[0] == 0) break;
		if (rec[0] != ET_FILE) continue;
		locs_lba[nl] = it.lba; locs_ent[nl++] = it.ent;
		secs = rec[1];
		for (k = 0; k < secs; k++) {
			uint8_t s[32]; int j;
			if (!dit_next(&it, s)) return;
			if (nl < 20) { locs_lba[nl] = it.lba; locs_ent[nl++] = it.ent; }
			if (s[0] == ET_STREAM) { nlen = s[3]; nofat = (s[1] & SF_NOFAT) ? 1 : 0; first = rd32(s + 0x14); dlen = rd32(s + 0x18); }
			else if (s[0] == ET_NAME) for (j = 0; j < 15 && ni < nlen && ni < 255; j++, ni++) nm[ni] = rd16(s + 2 + j * 2);
		}
		if (ni && fat_u16_is(name, nm, ni) && (only ? first == clus : first != clus) && (only || !(rec[4] & ATTR_DIR))) {
			for (k = 0; k < nl; k++) {
				uint8_t b[512];
				if (rsec(v, locs_lba[k], b) != 0) return;
				b[locs_ent[k] * 32] &= 0x7F;                  /* clear the in-use bit */
				wsec(v, locs_lba[k], b);
			}
			if (first) free_clusters(v, bm, first, nofat, dlen);
		}
	}
}
static void delete_named(fat_vol *v, bm_t *bm, fat_dir *dir, const char *name, uint32_t keep)
{ delete_match(v, bm, dir, name, keep, 0); }
static void delete_one(fat_vol *v, bm_t *bm, fat_dir *dir, const char *name, uint32_t first)
{ delete_match(v, bm, dir, name, first, 1); }

/* delete the file `name` in `dirpath` that starts at cluster `first_clus`, and only that one */
int exfat_wr_delete(fat_vol *v, const char *dirpath, const char *name, uint32_t first_clus)
{
	fat_dir dir; uint32_t str_lba; int str_ent;
	bm_t bm;
	if (!v->wr) return -1;
	if (dir_resolve(v, dirpath, &dir, &str_lba, &str_ent) != 0) return -3;
	bm.v = v; bm.valid = 0; bm.dirty = 0;
	delete_one(v, &bm, &dir, name, first_clus);
	return bm_flush(&bm) != 0 ? -9 : 0;
}

int exfat_wr_put(fat_vol *v, const char *dirpath, const char *name, const void *data, uint32_t len)
{
	fat_dir dir; uint32_t str_lba; int str_ent;
	bm_t bm; uint32_t cb, need, first = 0; int nofat = 1, nlen, nents, rc = 0;
	uint8_t set[19 * 32];
	const uint8_t *src = data;
	if (!v->wr) return -1;
	nlen = fat_u8_units(name);                        /* its length in UTF-16, as the card keeps it */
	if (nlen < 1 || nlen > 255) return -2;
	if (dir_resolve(v, dirpath, &dir, &str_lba, &str_ent) != 0) return -3;
	bm.v = v; bm.valid = 0; bm.dirty = 0;
	cb = cbytes(v);
	need = len ? (len + cb - 1u) / cb : 0u;
	/* allocate: a contiguous run first (no FAT chain), else a chain */
	if (need) {
		first = bm_find_run(&bm, need);
		if (first) {
			uint32_t i;
			for (i = 0; i < need; i++) if (bm_set(&bm, first + i, 1) != 0) return -4;
		} else {
			uint32_t prev = 0, i, c = 2u, got = 0;
			nofat = 0;
			for (i = 0; i < need; i++) {
				while (c < v->total_clusters + 2u && bm_get(&bm, c) != 0) c++;
				if (c >= v->total_clusters + 2u) { if (first) free_clusters(v, &bm, first, 0, got * cb); bm_flush(&bm); return -4; }
				if (bm_set(&bm, c, 1) != 0 || fat_set(v, c, 0xFFFFFFFFu) != 0) return -4;
				if (prev && fat_set(v, prev, c) != 0) return -4;
				if (!first) first = c;
				prev = c; got++; c++;
			}
		}
	}
	/* data */
	{
		uint32_t off = 0, c = first;
		uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;
		while (off < len && clus_ok(v, c)) {
			uint32_t remain = len - off, lba = clus_lba(v, c);
			if (remain >= cb) {
				uint32_t run = 1, cc = c, succ;
				if (nofat) { run = remain / cb; succ = c + run; }
				else { succ = fat_next(v, cc, fatb, &fatb_sec); while (run < remain / cb && succ == cc + 1u) { cc = succ; run++; succ = fat_next(v, cc, fatb, &fatb_sec); } }
				if (v->wr(v->ctx, lba, run << v->ex_spc_shift, src + off) != (run << v->ex_spc_shift)) { rc = -5; goto fail; }
				off += (run << v->ex_spc_shift) * 512u; c = succ;
			} else {
				uint32_t s = 0;
				while (s < v->ex_spc && off < len) {
					uint32_t rem2 = len - off;
					if (rem2 >= 512u) {
						uint32_t maxsec = v->ex_spc - s, nsec = rem2 / 512u; if (nsec > maxsec) nsec = maxsec;
						if (v->wr(v->ctx, lba + s, nsec, src + off) != nsec) { rc = -5; goto fail; }
						off += nsec * 512u; s += nsec;
					} else {
						uint8_t sec[512]; ex_memset(sec, 0, 512); ex_memcpy(sec, src + off, rem2);
						if (wsec(v, lba + s, sec) != 0) { rc = -5; goto fail; }
						off += rem2; s++;
					}
				}
				c = nofat ? c + 1u : fat_next(v, c, fatb, &fatb_sec);
			}
		}
	}
	/* the entry set */
	nents = build_set(v, set, name, nlen, ATTR_ARCH, first, len, nofat);
	if (nents < 0) { rc = -6; goto fail; }
	{
		uint32_t slba; int sent, tries = 0;
		while (!find_slots(v, &dir, nents, &slba, &sent)) {
			if (tries++ > 2 || dir_grow(v, &bm, &dir, str_lba, str_ent) != 0) { rc = -7; goto fail; }
		}
		if (write_entries(v, &dir, slba, sent, set, nents) != 0) { rc = -8; goto fail; }
	}
	/* the old file, if any, goes last: a power cut before this point leaves it intact */
	delete_named(v, &bm, &dir, name, first ? first : 0xFFFFFFFFu);
	if (bm_flush(&bm) != 0) rc = -9;
	return rc;
fail:
	if (first) free_clusters(v, &bm, first, nofat, len ? len : 1u);
	bm_flush(&bm);
	return rc;
}

/* mkdir -p (names in UTF-8) */
int exfat_wr_mkpath(fat_vol *v, const char *path)
{
	char cur[256]; const char *p = path; int curlen = 1;
	if (!v->wr) return -1;
	cur[0] = '/'; cur[1] = 0;
	while (*p == '/') p++;
	while (*p) {
		char child[256]; int ci = 0, n = 0, i;
		fat_dir d;
		for (i = 0; i < curlen; i++) child[ci++] = cur[i];
		if (curlen > 1) child[ci++] = '/';
		while (*p && *p != '/' && ci < 255) { child[ci++] = *p++; n++; }
		child[ci] = 0;
		while (*p == '/') p++;
		if (n == 0) break;
		/* looked for twice: a read that failed once does not make a second folder */
		if (exfat_opendir(v, child, &d) != 0 && exfat_opendir(v, child, &d) != 0) {
			char comp[256]; int k = 0;
			fat_dir parent; uint32_t str_lba; int str_ent;
			bm_t bm; uint32_t c, s, cb = cbytes(v); uint8_t zb[512], set[19 * 32]; int nents;
			uint32_t slba; int sent, tries = 0;
			int ulen;
			for (i = ci - n; i < ci; i++) comp[k++] = child[i];
			comp[k] = 0;
			ulen = fat_u8_units(comp);
			if (ulen < 1 || ulen > 255) return -4;
			if (dir_resolve(v, cur, &parent, &str_lba, &str_ent) != 0) return -2;
			bm.v = v; bm.valid = 0; bm.dirty = 0;
			c = bm_find_run(&bm, 1u);
			if (!c || bm_set(&bm, c, 1) != 0) { bm_flush(&bm); return -3; }
			ex_memset(zb, 0, 512);
			for (s = 0; s < v->ex_spc; s++) if (wsec(v, clus_lba(v, c) + s, zb) != 0) { bm_flush(&bm); return -3; }
			nents = build_set(v, set, comp, ulen, ATTR_DIR, c, cb, 1);
			if (nents < 0) { bm_flush(&bm); return -4; }
			while (!find_slots(v, &parent, nents, &slba, &sent)) {
				if (tries++ > 2 || dir_grow(v, &bm, &parent, str_lba, str_ent) != 0) { bm_set(&bm, c, 0); bm_flush(&bm); return -5; }
			}
			if (write_entries(v, &parent, slba, sent, set, nents) != 0) { bm_set(&bm, c, 0); bm_flush(&bm); return -6; }
			if (bm_flush(&bm) != 0) return -7;
		}
		for (i = 0; i <= ci; i++) cur[i] = child[i];
		curlen = ci;
	}
	return 0;
}
