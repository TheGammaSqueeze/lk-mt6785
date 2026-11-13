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

/* mkdir -p for an absolute path of 8.3 component names (e.g. "/saves/gba").
 * Idempotent: existing components are descended into, missing ones created with
 * proper "." / ".." entries. Lets the save path work on a card that only has
 * roms/gba, without silently dropping saves. Returns 0 on success, negative on
 * error (no writer / disk full / a component name is not 8.3). */
int fat_wr_mkpath(fat_vol *v, const char *path);

/* Delete the file <name> in <dirpath> whose data starts at cluster <first_clus> (both as
 * fat_readdir reported them), freeing its clusters; another file of that name is left alone.
 * A folder can go the same way once it is empty (the caller checks: what is in it would be
 * lost). FAT16/32 and exFAT. Returns 0 on success (also when it was not there), negative on
 * error. */
int fat_wr_delete(fat_vol *v, const char *dirpath, const char *name, uint32_t first_clus);

#endif
