/*
 * Minimal read-only FAT16/FAT32 reader for the GBA-from-SD-card boot flow.
 *
 * Self-contained and block-callback driven: the caller supplies a sector-read
 * function (mmc_wrap_bread on-device, a file on the host test harness) so the
 * same code is validated offline against a real FAT image. Supports an MBR
 * partition table or a bare VBR ("superfloppy"), FAT16 + FAT32, subdirectories,
 * and VFAT long file names (needed for real ROM names). Write support is a
 * separate module (saves/states); this half only reads.
 *
 * No libc dependency beyond the few mem* helpers used internally.
 */
#ifndef FAT_RO_H
#define FAT_RO_H

#include <stdint.h>

/* ---- names: the card keeps them in UTF-16, the menus take UTF-8 ----
 * Small enough to live here, so every user of the card code (and the host tests) has them. */

/* one code point onto UTF-8 text of length o in out[cap] (room kept for an ending 0): the new
 * length, or -1 when it does not fit whole */
static inline int fat_u8_put(char *out, int o, int cap, uint32_t c)
{
	int len = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
	if (o + len > cap - 1) return -1;
	if (len == 1) out[o++] = (char)c;
	else if (len == 2) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
	else if (len == 3) {
		out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63));
		out[o++] = (char)(0x80 | (c & 63));
	} else {
		out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63));
		out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
	}
	return o;
}

/* the code point at u[*i] of n UTF-16 units, moving past it: a surrogate pair as one, a surrogate
 * without its other half as U+FFFD */
static inline uint32_t fat_u16_next(const uint16_t *u, int n, int *i)
{
	uint32_t c = u[(*i)++];
	if (c >= 0xD800 && c < 0xDC00 && *i < n && u[*i] >= 0xDC00 && u[*i] < 0xE000)
		return 0x10000u + ((c - 0xD800u) << 10) + (u[(*i)++] - 0xDC00u);
	return c >= 0xD800 && c < 0xE000 ? 0xFFFDu : c;
}

/* UTF-16 units to UTF-8: n units, stopping early at a 0. The output always ends in a 0 and holds
 * only whole characters (one that would not fit is left out, with everything after it). Returns
 * the length in bytes. */
static inline int fat_u16_to_u8(const uint16_t *u, int n, char *out, int cap)
{
	int i = 0, o = 0;
	if (cap < 1) return 0;
	while (i < n && u[i]) {
		int r = fat_u8_put(out, o, cap, fat_u16_next(u, n, &i));
		if (r < 0) break;
		o = r;
	}
	out[o] = 0;
	return o;
}

/* A name that did not fit whole (out holds its first o bytes, whole characters): its extension,
 * found in t (its last nt UTF-16 units), put back at the end in place of what comes before it,
 * so a long name still ends in ".gba". Returns the new length. */
static inline int fat_name_keep_ext(char *out, int o, int cap, const uint16_t *t, int nt)
{
	char ext[72];
	int e = 0, d = -1, i, k;
	for (i = nt - 1; i >= 0; i--) if (t[i] == '.') { d = i; break; }
	if (d < 0) return o;
	for (i = d; i < nt; ) {
		int r = fat_u8_put(ext, e, (int)sizeof ext, fat_u16_next(t, nt, &i));
		if (r < 0) return o;
		e = r;
	}
	if (e > cap - 1) return o;
	while (o > 0 && o + e > cap - 1) {	/* one whole character less at a time */
		o--;
		while (o > 0 && (out[o] & 0xC0) == 0x80) o--;
	}
	for (k = 0; k < e; k++) out[o + k] = ext[k];
	o += e;
	out[o] = 0;
	return o;
}

/* a whole name of n UTF-16 units (ending early at a 0) as fat_dirent.name holds it: UTF-8, and
 * one too long for cap keeps its extension (fat_name_keep_ext) */
static inline int fat_u16_name(const uint16_t *u, int n, char *out, int cap)
{
	int i = 0, o = 0, k;
	if (cap < 1) return 0;
	for (k = 0; k < n && u[k]; k++) ;
	n = k;
	while (i < n) {
		int r = fat_u8_put(out, o, cap, fat_u16_next(u, n, &i));
		if (r < 0) break;
		o = r;
	}
	out[o] = 0;
	if (i < n) o = fat_name_keep_ext(out, o, cap, u + (n > 16 ? n - 16 : 0), n > 16 ? 16 : n);
	return o;
}

/* the next code point of UTF-8 text, moving past it; a byte that does not start a whole UTF-8
 * character reads as that byte (Latin-1), so any text can be walked */
static inline uint32_t fat_u8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = s[0];
	if (c >= 0xC2 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
		c = ((c & 31) << 6) | (s[1] & 63); *ps += 2;
	} else if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
		c = ((c & 15) << 12) | ((uint32_t)(s[1] & 63) << 6) | (s[2] & 63); *ps += 3;
	} else if (c >= 0xF0 && c < 0xF5 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		c = ((c & 7) << 18) | ((uint32_t)(s[1] & 63) << 12) | ((uint32_t)(s[2] & 63) << 6) | (s[3] & 63); *ps += 4;
	} else {
		*ps += 1;
	}
	return c;
}

/* how many UTF-16 units a UTF-8 name takes */
static inline int fat_u8_units(const char *s)
{
	int n = 0;
	while (*s) n += fat_u8_next(&s) >= 0x10000 ? 2 : 1;
	return n;
}

/* the UTF-16 unit at index idx of a UTF-8 name (0 past its end): the writers take a long name a
 * unit at a time this way, with no copy of it in UTF-16 on their stack */
static inline uint16_t fat_u8_unit_at(const char *s, int idx)
{
	int n = 0;
	while (*s) {
		uint32_t c = fat_u8_next(&s);
		if (c >= 0x10000) {
			c -= 0x10000;
			if (n == idx) return (uint16_t)(0xD800 + (c >> 10));
			if (n + 1 == idx) return (uint16_t)(0xDC00 + (c & 0x3FF));
			n += 2;
		} else {
			if (n == idx) return (uint16_t)c;
			n++;
		}
	}
	return 0;
}

/* UTF-8 to UTF-16 units, a code point above U+FFFF as a surrogate pair; no ending 0 is written.
 * Returns the number of units, or -1 when they do not fit in cap. */
static inline int fat_u8_to_u16(const char *s, uint16_t *out, int cap)
{
	int n = 0;
	while (*s) {
		uint32_t c = fat_u8_next(&s);
		if (c >= 0x10000) {
			if (n + 2 > cap) return -1;
			c -= 0x10000;
			out[n++] = (uint16_t)(0xD800 + (c >> 10));
			out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
		} else {
			if (n + 1 > cap) return -1;
			out[n++] = (uint16_t)c;
		}
	}
	return n;
}

/* names equal, ASCII letters in either case (every other byte as it is) */
static inline int fat_name_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
		char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
		if (x != y) return 0;
	}
	return *a == *b;
}

/* a name still in UTF-16 (n units, ending early at a 0) equal to a UTF-8 one, ASCII letters in
 * either case, with no copy of it made */
static inline int fat_u16_is(const char *comp, const uint16_t *u, int n)
{
	const char *p = comp;
	int i = 0, k;
	for (k = 0; k < n && u[k]; k++) ;
	n = k;
	while (i < n && *p) {
		uint32_t a = fat_u8_next(&p), b = fat_u16_next(u, n, &i);
		if (a >= 'a' && a <= 'z') a -= 32;
		if (b >= 'a' && b <= 'z') b -= 32;
		if (a != b) return 0;
	}
	return i == n && !*p;
}

/* Read `count` 512-byte sectors starting at absolute LBA `lba` into `buf`.
 * Returns the number of sectors read (== count on success). */
typedef unsigned (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *buf);
/* Write `count` sectors at absolute LBA. Returns count on success. Optional -
 * only needed for the write layer (fat_wr.c); leave 0 for read-only mounts. */
typedef unsigned (*fat_write_fn)(void *ctx, uint32_t lba, uint32_t count, const void *buf);

typedef struct {
	fat_read_fn rd;
	fat_write_fn wr;          /* 0 for read-only; set via fat_set_writer for fat_wr.c */
	void *ctx;
	uint32_t part_lba;        /* absolute LBA of the volume's first sector */
	uint32_t fat_start;       /* absolute LBA of FAT #0 */
	uint32_t fat_sectors;     /* sectors per FAT */
	uint32_t data_start;      /* absolute LBA of cluster 2 minus 2*spc (see clus_lba) */
	uint32_t root_start;      /* FAT16: absolute LBA of the fixed root dir; FAT32: 0 */
	uint32_t root_sectors;    /* FAT16: root-dir sector count; FAT32: 0 */
	uint32_t root_cluster;    /* FAT32: root dir first cluster; FAT16: 0 */
	uint32_t total_clusters;  /* count of data clusters (for type + bounds) */
	uint16_t bytes_per_sec;   /* 512 assumed but stored/validated */
	uint8_t  sec_per_clus;
	uint8_t  num_fats;        /* FAT copies (mirror writes across all) */
	uint8_t  is_fat32;
	uint8_t  mounted;
	/* exFAT (fat_exfat.c): cluster size may exceed 255 sectors, so it has its own fields */
	uint8_t  is_exfat;
	uint8_t  ex_spc_shift;     /* log2 sectors per cluster */
	uint32_t ex_spc;           /* sectors per cluster */
	uint32_t ex_heap_lba;      /* absolute LBA of cluster 2 */
	uint32_t ex_fat_lba;       /* absolute LBA of the active FAT */
	uint32_t ex_fat_len;       /* FAT length in sectors */
	uint32_t ex_bitmap_clus;   /* allocation bitmap first cluster (chain) */
	uint32_t ex_bitmap_len;    /* bitmap length in bytes */
	uint32_t ex_upcase_clus;   /* up-case table first cluster */
	uint32_t ex_upcase_len;    /* up-case table length in bytes */
	/* fat_wr.c write-back cache of ONE FAT sector (FAT copy #0). A save touches
	 * the same FAT sector hundreds of times (alloc + mirror writes); caching it
	 * and flushing to all FAT copies once collapses that to a couple of writes.
	 * fat_ro.c only zeroes fatw_valid/dirty on mount; all real use is fat_wr.c. */
	uint8_t  fatw_buf[512];
	uint32_t fatw_sec;         /* absolute FAT#0 LBA held in fatw_buf */
	uint8_t  fatw_valid;       /* fatw_buf holds fatw_sec */
	uint8_t  fatw_dirty;       /* fatw_buf modified vs disk */
} fat_vol;

typedef struct {
	char name[256];           /* the long name, in UTF-8, else the 8.3 name; paths name entries this
				   * way. One longer than 255 bytes in UTF-8 is cut before its extension,
				   * at a whole character, and keeps the extension */
	uint32_t size;            /* bytes (0 for dirs) */
	uint32_t first_clus;
	uint8_t  is_dir;
	uint8_t  ex_nofat;        /* exFAT: the data is one contiguous run, the FAT holds no chain */
	uint32_t ex_dir_len;      /* exFAT: a directory's data length (its allocation) */
} fat_dirent;

typedef struct {
	fat_vol *v;
	uint32_t cluster;         /* current cluster (0 = FAT16 fixed root) */
	uint32_t sec_in_clus;     /* sector index within the cluster */
	uint32_t ent_in_sec;      /* dir-entry index within the sector (0..15) */
	uint32_t root_sec;        /* FAT16 root: absolute sector cursor */
	uint32_t root_left;       /* FAT16 root: sectors remaining */
	uint32_t hops;            /* cluster hops taken (loop guard vs a corrupt chain) */
	uint8_t  end;
	uint8_t  ex_nofat;        /* exFAT: contiguous directory */
	uint32_t ex_first;        /* exFAT: first cluster of the directory */
	uint32_t ex_len;          /* exFAT: directory data length (0 = unknown: the root) */
	uint32_t ex_pos;          /* exFAT: byte position within the directory */
} fat_dir;

typedef struct {
	fat_vol *v;
	uint32_t first_clus;
	uint32_t size;
	uint8_t  ex_nofat;        /* exFAT: contiguous file */
} fat_file;

/* Probe the block device (MBR or bare VBR), locate the first FAT16/32 volume,
 * parse its BPB. Returns 0 on success, negative on failure (no card / not FAT). */
int fat_mount(fat_vol *v, fat_read_fn rd, void *ctx);

/* Open the directory a fat_readdir entry names (one of two folders of the same name). */
int fat_opendir_entry(fat_vol *v, const fat_dirent *e, fat_dir *d);

/* Attach a sector-write callback so the volume can be modified via fat_wr.c. */
void fat_set_writer(fat_vol *v, fat_write_fn wr);

/* Open the directory at `path` ("/", "/roms/gba", case-insensitive). 0 on ok. */
int fat_opendir(fat_vol *v, const char *path, fat_dir *d);
/* Fetch the next real entry (skips volume-label/deleted/. and ..). Returns 1 if
 * `e` was filled, 0 at end. */
int fat_readdir(fat_dir *d, fat_dirent *e);

/* Open a file by full path. 0 on ok, negative if not found / is a dir. */
int fat_open(fat_vol *v, const char *path, fat_file *f);
/* Read `len` bytes at byte offset `off`. Returns bytes read (may be < len at EOF). */
uint32_t fat_read(fat_file *f, uint32_t off, void *buf, uint32_t len);

#endif
