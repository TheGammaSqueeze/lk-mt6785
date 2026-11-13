/*
 * exFAT backend for the SD card file API in fat_ro.h / fat_wr.h.
 *
 * fat_mount detects an exFAT volume and hands over to exfat_mount; every public call then
 * dispatches here when v->is_exfat is set, so the cores keep using fat_opendir / fat_readdir /
 * fat_open / fat_read / fat_wr_put / fat_wr_mkpath unchanged. Reading follows cluster chains
 * or contiguous runs (the NoFatChain flag), names are decoded from UTF-16 to the same ASCII
 * form the FAT reader produces. Writing replaces a whole file like the FAT writer: allocate in
 * the bitmap (a contiguous run when one exists, so no FAT writes are needed), write the data,
 * append a file / stream / name entry set with its checksums, then remove the old set and
 * free its clusters, so a power cut never loses the previous file.
 */
#ifndef FAT_EXFAT_H
#define FAT_EXFAT_H

#include "fat_ro.h"

int      exfat_mount(fat_vol *v, uint32_t part_lba, const uint8_t *vbr);
int      exfat_opendir(fat_vol *v, const char *path, fat_dir *d);
int      exfat_readdir(fat_dir *d, fat_dirent *e);
void     exfat_dir_at(fat_vol *v, fat_dir *d, const fat_dirent *e);
int      exfat_open(fat_vol *v, const char *path, fat_file *f);
uint32_t exfat_read(fat_file *f, uint32_t off, void *buf, uint32_t len);
int      exfat_wr_put(fat_vol *v, const char *dirpath, const char *name, const void *data, uint32_t len);
int      exfat_wr_mkpath(fat_vol *v, const char *path);
int      exfat_wr_delete(fat_vol *v, const char *dirpath, const char *name, uint32_t first_clus);

#endif
