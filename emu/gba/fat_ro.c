/* Minimal read-only FAT16/FAT32 reader - see fat_ro.h. */
#include "fat_ro.h"

/* ---- tiny mem helpers (no libc dependency in LK) ---- */
static void fr_memcpy(void *d, const void *s, unsigned n)
{ unsigned char *dp = d; const unsigned char *sp = s; while (n--) *dp++ = *sp++; }
static int fr_memcmp(const void *a, const void *b, unsigned n)
{ const unsigned char *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
static char fr_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* read one 512B sector into v->sec_buf */
static int read_sec(fat_vol *v, uint32_t lba)
{ return v->rd(v->ctx, lba, 1, v->sec_buf) == 1 ? 0 : -1; }

/* absolute LBA of data cluster N (>=2) */
static uint32_t clus_lba(fat_vol *v, uint32_t n)
{ return v->data_start + (n - 2) * v->sec_per_clus; }

/* next cluster in the chain, or 0 on end-of-chain / error */
static uint32_t next_cluster(fat_vol *v, uint32_t n)
{
	uint32_t byteoff = v->is_fat32 ? n * 4u : n * 2u;
	uint32_t sec = v->fat_start + byteoff / 512u;
	uint32_t off = byteoff % 512u;
	uint32_t val;
	uint8_t fatb[512];
	if (v->rd(v->ctx, sec, 1, fatb) != 1) return 0;
	if (v->is_fat32) {
		val = rd32(fatb + off) & 0x0FFFFFFFu;
		return (val >= 0x0FFFFFF8u) ? 0 : val;
	}
	val = rd16(fatb + off);
	return (val >= 0xFFF8u) ? 0 : val;
}

static int parse_bpb(fat_vol *v, const uint8_t *b, uint32_t part_lba)
{
	uint16_t bps = rd16(b + 0x0B);
	uint8_t  spc = b[0x0D];
	uint16_t reserved = rd16(b + 0x0E);
	uint8_t  nfats = b[0x10];
	uint16_t root_ents = rd16(b + 0x11);
	uint32_t tot16 = rd16(b + 0x13), tot32 = rd32(b + 0x20);
	uint16_t fatsz16 = rd16(b + 0x16);
	uint32_t fatsz = fatsz16;
	uint32_t total, root_dir_secs, first_data, dataclus;
	/* FATSz16==0 is the definitive FAT32 marker: the volume was laid out with a
	 * 32-bit FAT + a cluster-based root dir. This is more reliable than the spec's
	 * cluster-count threshold, which misreads a small-but-FAT32-formatted volume. */
	int fat32 = (fatsz16 == 0);
	if (bps != 512 || spc == 0 || nfats == 0) return -1;
	if (fat32) fatsz = rd32(b + 0x24);
	total = tot16 ? tot16 : tot32;
	if (total == 0 || fatsz == 0) return -1;
	root_dir_secs = ((uint32_t)root_ents * 32u + (bps - 1)) / bps;
	first_data = reserved + (uint32_t)nfats * fatsz + root_dir_secs;
	if (first_data >= total) return -1;
	dataclus = (total - first_data) / spc;
	v->bytes_per_sec = bps;
	v->sec_per_clus = spc;
	v->num_fats = nfats;
	v->part_lba = part_lba;
	v->fat_start = part_lba + reserved;
	v->fat_sectors = fatsz;
	v->data_start = part_lba + first_data;            /* == LBA of cluster 2 */
	v->total_clusters = dataclus;
	v->is_fat32 = (uint8_t)fat32;
	if (!fat32 && dataclus < 4085u) return -1;        /* FAT12 unsupported */
	if (v->is_fat32) {
		v->root_cluster = rd32(b + 0x2C);
		v->root_start = 0; v->root_sectors = 0;
	} else {
		v->root_cluster = 0;
		v->root_start = part_lba + reserved + (uint32_t)nfats * fatsz;
		v->root_sectors = root_dir_secs;
	}
	return 0;
}

void fat_set_writer(fat_vol *v, fat_write_fn wr) { v->wr = wr; }

/* "EXFAT   " (8 bytes at VBR offset 3) marks an exFAT volume - which SDXC cards
 * ship with by default and this reader does not support. Detect it so the caller
 * can tell the user to reformat FAT32 instead of failing with a generic error. */
static int is_exfat_vbr(const uint8_t *s)
{
	static const char sig[8] = { 'E','X','F','A','T',' ',' ',' ' };
	int i;
	for (i = 0; i < 8; i++) if (s[3 + i] != (uint8_t)sig[i]) return 0;
	return 1;
}

int fat_mount(fat_vol *v, fat_read_fn rd, void *ctx)
{
	v->wr = 0;
	uint8_t s0[512];
	int i;
	v->rd = rd; v->ctx = ctx; v->mounted = 0;
	v->fatw_valid = 0; v->fatw_dirty = 0;   /* drop any stale write-back FAT cache */
	if (rd(ctx, 0, 1, s0) != 1) return -1;
	if (s0[510] != 0x55 || s0[511] != 0xAA) return -2;
	/* Bare VBR ("superfloppy")? A VBR starts with a jump (0xEB/0xE9) and has a
	 * plausible BPB. Try it first; else walk the MBR partition table. */
	if (is_exfat_vbr(s0)) return -4;                  /* bare exFAT superfloppy */
	if ((s0[0] == 0xEB || s0[0] == 0xE9) && parse_bpb(v, s0, 0) == 0) {
		v->mounted = 1; return 0;
	}
	for (i = 0; i < 4; i++) {
		const uint8_t *e = s0 + 0x1BE + i * 16;
		uint8_t type = e[4];
		uint32_t lba = rd32(e + 8);
		uint8_t vbr[512];
		if (type == 0 || lba == 0) continue;
		/* FAT16: 0x04/0x06/0x0E ; FAT32: 0x0B/0x0C ; 0x07 = exFAT/NTFS */
		if (type == 0x07) {                       /* likely exFAT - confirm + report */
			if (rd(ctx, lba, 1, vbr) == 1 && is_exfat_vbr(vbr)) return -4;
			continue;
		}
		if (type != 0x04 && type != 0x06 && type != 0x0E &&
		    type != 0x0B && type != 0x0C) continue;
		if (rd(ctx, lba, 1, vbr) != 1) continue;
		if (vbr[510] != 0x55 || vbr[511] != 0xAA) continue;
		if (parse_bpb(v, vbr, lba) == 0) { v->mounted = 1; return 0; }
	}
	return -3;
}

/* ---- directory iteration ---- */
static void dir_init(fat_vol *v, fat_dir *d, uint32_t first_clus)
{
	d->v = v; d->end = 0; d->hops = 0;
	d->ent_in_sec = 0; d->sec_in_clus = 0;
	if (first_clus == 0 && !v->is_fat32) {            /* FAT16 fixed root */
		d->cluster = 0;
		d->root_sec = v->root_start; d->root_left = v->root_sectors;
	} else {
		d->cluster = first_clus ? first_clus : v->root_cluster;
		d->root_sec = 0; d->root_left = 0;
	}
}

/* fetch the next raw 32-byte directory record into out[]; 0 at end */
static int next_raw(fat_dir *d, uint8_t out[32])
{
	fat_vol *v = d->v;
	for (;;) {
		uint32_t lba;
		if (d->end) return 0;
		if (d->cluster == 0) {                    /* FAT16 root */
			if (d->root_left == 0) { d->end = 1; return 0; }
			lba = d->root_sec;
		} else {
			if (d->cluster < 2 || d->cluster >= v->total_clusters + 2) { d->end = 1; return 0; }
			lba = clus_lba(v, d->cluster) + d->sec_in_clus;
		}
		if (read_sec(v, lba) != 0) { d->end = 1; return 0; }
		{
			uint8_t *rec = v->sec_buf + d->ent_in_sec * 32;
			d->ent_in_sec++;
			if (d->ent_in_sec >= 16) {        /* advance to next sector */
				d->ent_in_sec = 0;
				if (d->cluster == 0) { d->root_sec++; d->root_left--; }
				else {
					d->sec_in_clus++;
					if (d->sec_in_clus >= v->sec_per_clus) {
						d->sec_in_clus = 0;
						d->cluster = next_cluster(v, d->cluster);
						if (d->cluster == 0 || ++d->hops > v->total_clusters) d->end = 1;
					}
				}
			}
			if (rec[0] == 0x00) { d->end = 1; return 0; }   /* end of dir */
			fr_memcpy(out, rec, 32);
			return 1;
		}
	}
}

/* decode the 13 UTF-16 units of one LFN record into ascii dst[0..12] (bit7 stripped) */
static void lfn_chunk(const uint8_t *rec, char *dst)
{
	static const int off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
	int i;
	for (i = 0; i < 13; i++) {
		uint16_t u = rd16(rec + off[i]);
		dst[i] = (u == 0 || u == 0xFFFF) ? 0 : (u < 0x80 ? (char)u : '_');
	}
}

int fat_opendir(fat_vol *v, const char *path, fat_dir *d)
{
	fat_dir cur;
	const char *p = path;
	if (!v->mounted) return -1;
	dir_init(v, &cur, v->is_fat32 ? v->root_cluster : 0);
	while (*p == '/') p++;
	while (*p) {                                      /* walk each component */
		char comp[256]; int n = 0;
		fat_dirent e; int found = 0;
		while (*p && *p != '/' && n < 255) comp[n++] = *p++;
		comp[n] = 0;
		while (*p == '/') p++;
		if (n == 0) break;
		while (fat_readdir(&cur, &e)) {
			if (e.is_dir) {
				int i, eq = 1;
				for (i = 0; comp[i] || e.name[i]; i++)
					if (fr_upper(comp[i]) != fr_upper(e.name[i])) { eq = 0; break; }
				if (eq) { dir_init(v, &cur, e.first_clus); found = 1; break; }
			}
		}
		if (!found) return -2;
	}
	*d = cur;
	return 0;
}

int fat_readdir(fat_dir *d, fat_dirent *e)
{
	uint8_t rec[32];
	char lfn[260];
	int have_lfn = 0, i;
	lfn[0] = 0;
	while (next_raw(d, rec)) {
		uint8_t attr = rec[11];
		if (rec[0] == 0xE5) { have_lfn = 0; lfn[0] = 0; continue; }   /* deleted */
		if (attr == 0x0F) {                            /* LFN record */
			int seq = (rec[0] & 0x1F);
			char chunk[13];
			if (seq >= 1 && seq <= 20) {
				lfn_chunk(rec, chunk);
				fr_memcpy(lfn + (seq - 1) * 13, chunk, 13);
				if ((seq - 1) * 13 + 13 > have_lfn) have_lfn = (seq - 1) * 13 + 13;
				lfn[have_lfn < 260 ? have_lfn : 259] = 0;
			}
			continue;
		}
		if (attr & 0x08) { have_lfn = 0; lfn[0] = 0; continue; }      /* volume label */
		/* short entry: build the name */
		if (have_lfn && lfn[0]) {
			int j = 0; while (lfn[j] && j < 255) { e->name[j] = lfn[j]; j++; } e->name[j] = 0;
		} else {
			char nm[13]; int k = 0;
			for (i = 0; i < 8 && rec[i] != ' '; i++) nm[k++] = rec[i];
			if (rec[8] != ' ') {
				nm[k++] = '.';
				for (i = 8; i < 11 && rec[i] != ' '; i++) nm[k++] = rec[i];
			}
			nm[k] = 0;
			for (i = 0; i <= k; i++) e->name[i] = nm[i];
		}
		e->is_dir = (attr & 0x10) ? 1 : 0;
		e->size = rd32(rec + 28);
		e->first_clus = ((uint32_t)rd16(rec + 20) << 16) | rd16(rec + 26);
		/* skip "." and ".." */
		if (e->name[0] == '.' && (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0))) {
			have_lfn = 0; lfn[0] = 0; continue;
		}
		return 1;
	}
	return 0;
}

int fat_open(fat_vol *v, const char *path, fat_file *f)
{
	const char *slash = 0, *p = path;
	char dirpath[256]; int dn;
	fat_dir d; fat_dirent e;
	if (!v->mounted) return -1;
	for (; *p; p++) if (*p == '/') slash = p;         /* last slash */
	dn = slash ? (int)(slash - path) : 0;
	if (dn >= 256) return -1;
	for (p = path; p < path + dn; p++) dirpath[p - path] = *p;
	dirpath[dn] = 0;
	if (fat_opendir(v, dn ? dirpath : "/", &d) != 0) return -2;
	{
		const char *fname = slash ? slash + 1 : path;
		while (fat_readdir(&d, &e)) {
			if (!e.is_dir) {
				int i, eq = 1;
				for (i = 0; fname[i] || e.name[i]; i++)
					if (fr_upper(fname[i]) != fr_upper(e.name[i])) { eq = 0; break; }
				if (eq) { f->v = v; f->first_clus = e.first_clus; f->size = e.size; return 0; }
			}
		}
	}
	return -3;
}

/* next_cluster with a caller-held one-sector FAT cache: a sequential chain walk
 * mostly hits the same FAT sector (128 FAT32 entries per sector), so this turns
 * thousands of FAT-sector reads into a handful. fatb/fatb_sec persist across
 * calls; *fatb_sec == 0xFFFFFFFF means empty. */
static uint32_t nextc_cached(fat_vol *v, uint32_t n, uint8_t *fatb, uint32_t *fatb_sec)
{
	uint32_t byteoff = v->is_fat32 ? n * 4u : n * 2u;
	uint32_t sec = v->fat_start + byteoff / 512u;
	uint32_t off = byteoff % 512u;
	uint32_t val;
	if (sec != *fatb_sec) {
		if (v->rd(v->ctx, sec, 1, fatb) != 1) return 0;
		*fatb_sec = sec;
	}
	if (v->is_fat32) { val = rd32(fatb + off) & 0x0FFFFFFFu; return (val >= 0x0FFFFFF8u) ? 0 : val; }
	val = rd16(fatb + off);
	return (val >= 0xFFF8u) ? 0 : val;
}

uint32_t fat_read(fat_file *f, uint32_t off, void *buf, uint32_t len)
{
	fat_vol *v = f->v;
	uint32_t cbytes = (uint32_t)v->sec_per_clus * 512u;
	uint32_t got = 0, clus = f->first_clus, skip, i;
	uint8_t *out = buf;
	uint8_t fatb[512]; uint32_t fatb_sec = 0xFFFFFFFFu;   /* local FAT-sector cache */
	uint32_t inclus;
	if (off >= f->size) return 0;
	if (off + len > f->size) len = f->size - off;
	skip = off / cbytes;                              /* clusters to skip */
	for (i = 0; i < skip && clus; i++) clus = nextc_cached(v, clus, fatb, &fatb_sec);
	if (!clus) return 0;
	inclus = off % cbytes;                            /* byte offset within cluster */
	while (len && clus >= 2) {
		/* Coalesce a run of physically CONTIGUOUS clusters (clus, clus+1, ...)
		 * into ONE big block transfer straight into the caller buffer. A ROM is
		 * usually contiguous, so a 16MB load becomes a couple of transfers
		 * instead of ~4096 (one per cluster) plus ~4096 FAT walks. Only when we
		 * are at a cluster boundary and want at least a whole cluster. */
		if (inclus == 0 && len >= cbytes) {
			uint32_t want_clus = len / cbytes;
			uint32_t c = clus, run = 1;
			uint32_t succ = nextc_cached(v, c, fatb, &fatb_sec);
			uint32_t nsec;
			while (run < want_clus && succ == c + 1u) {
				c = succ; run++;
				succ = nextc_cached(v, c, fatb, &fatb_sec);
			}
			nsec = run * (uint32_t)v->sec_per_clus;
			if (v->rd(v->ctx, clus_lba(v, clus), nsec, out + got) != nsec) return got;
			got += nsec * 512u; len -= nsec * 512u;
			clus = succ;                              /* successor of the last cluster in the run */
			continue;
		}
		/* Partial cluster (a non-zero head offset, or a final tail < one cluster):
		 * read whole sectors in bulk, bouncing through sec_buf only for a partial
		 * head/tail sector. */
		{
			uint32_t sec = inclus / 512u, so = inclus % 512u;
			uint32_t lba = clus_lba(v, clus) + sec;
			while (sec < v->sec_per_clus && len) {
				if (so == 0 && len >= 512u) {
					uint32_t maxsec = v->sec_per_clus - sec;
					uint32_t wantsec = len / 512u;
					uint32_t nsec = wantsec < maxsec ? wantsec : maxsec;
					if (v->rd(v->ctx, lba, nsec, out + got) != nsec) return got;
					got += nsec * 512u; len -= nsec * 512u;
					sec += nsec; lba += nsec;
				} else {
					uint32_t n = 512u - so; if (n > len) n = len;
					if (read_sec(v, lba) != 0) return got;
					fr_memcpy(out + got, v->sec_buf + so, n);
					got += n; len -= n; so = 0; sec++; lba++;
				}
			}
			inclus = 0;
			if (len) clus = nextc_cached(v, clus, fatb, &fatb_sec);
		}
	}
	return got;
}
