/*
 * Minimal FAT16/FAT32 write support for save/state persistence (GBA-from-SD).
 * Read-only mount via fat_ro.c; attach a writer with fat_set_writer() first.
 *
 * Scope: create-or-replace a whole file with a SHORT (8.3, uppercased) name in an
 * existing directory. That is all the save/state flow needs (fixed set of files,
 * rewritten in full each time). Long file names on write are intentionally not
 * supported - saves/states are named by us, so an 8.3-safe name is fine.
 */
#ifndef FAT_WR_H
#define FAT_WR_H

#include "fat_ro.h"

/* Create <name> (8.3, case-insensitive) in directory <dirpath>, replacing it if it
 * already exists, with `len` bytes from `buf`. Allocates a fresh cluster chain,
 * writes the data, and writes/updates the directory entry; the old chain (on
 * replace) is freed. Returns 0 on success, negative on error (no writer / dir
 * missing / disk full / name not 8.3). */
int fat_wr_put(fat_vol *v, const char *dirpath, const char *name,
	       const void *buf, uint32_t len);

#endif
