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

/* Read `count` 512-byte sectors starting at absolute LBA `lba` into `buf`.
 * Returns the number of sectors read (== count on success). */
typedef unsigned (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *buf);

typedef struct {
	fat_read_fn rd;
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
	uint8_t  is_fat32;
	uint8_t  mounted;
	uint8_t  sec_buf[512];     /* scratch sector for FAT/dir reads */
} fat_vol;

typedef struct {
	char name[256];           /* long name if present, else 8.3 (ASCII) */
	uint32_t size;            /* bytes (0 for dirs) */
	uint32_t first_clus;
	uint8_t  is_dir;
} fat_dirent;

typedef struct {
	fat_vol *v;
	uint32_t cluster;         /* current cluster (0 = FAT16 fixed root) */
	uint32_t sec_in_clus;     /* sector index within the cluster */
	uint32_t ent_in_sec;      /* dir-entry index within the sector (0..15) */
	uint32_t root_sec;        /* FAT16 root: absolute sector cursor */
	uint32_t root_left;       /* FAT16 root: sectors remaining */
	uint8_t  end;
} fat_dir;

typedef struct {
	fat_vol *v;
	uint32_t first_clus;
	uint32_t size;
} fat_file;

/* Probe the block device (MBR or bare VBR), locate the first FAT16/32 volume,
 * parse its BPB. Returns 0 on success, negative on failure (no card / not FAT). */
int fat_mount(fat_vol *v, fat_read_fn rd, void *ctx);

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
