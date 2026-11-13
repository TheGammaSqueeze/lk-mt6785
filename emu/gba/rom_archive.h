/*
 * rom_archive.h - games kept in .zip and .7z archives on the card.
 *
 * An archive in a system folder is listed as the game it holds (sd_fat.c). Its ROM is the first
 * file inside with one of the system's extensions; at launch it is unpacked straight into the
 * core's ROM memory and its CRC checked. Anything that goes wrong is one of the RA_ERR_* reasons,
 * which the menus show in words (gba_rom_error_text).
 */
#ifndef ROM_ARCHIVE_H
#define ROM_ARCHIVE_H

#include <stdint.h>
#include "fat_ro.h"

enum { RA_NONE, RA_ZIP, RA_7Z };   /* gba_rom_entry.arc */

enum {
	RA_OK = 0,
	RA_ERR_READ = -1,        /* the card could not be read */
	RA_ERR_DAMAGED = -2,     /* not a valid archive, or its data does not match its CRC */
	RA_ERR_NOROM = -3,       /* no file with the system's extensions inside */
	RA_ERR_METHOD = -4,      /* packed in a way this console does not unpack */
	RA_ERR_PASSWORD = -5,    /* encrypted */
	RA_ERR_TOOBIG = -6,      /* the game is larger than the system's ROM memory */
	RA_ERR_HUGE = -7,        /* the archive's own tables do not fit the memory for them */
	RA_ERR_SPLIT = -8,       /* an archive in parts (.zip.001, .z01...) */
	RA_ERR_START = -9        /* the core would not start the game */
};

/* The ROMs in an archive file (f: the archive; exts: the system's extensions, space separated,
 * as ".gba .agb"; member: which of the files with those extensions, in the archive's order, 0 the
 * first; folders inside do not matter). Checked without unpacking (probe; *size gets the ROM's
 * size), or unpacked into dst (load: the whole ROM, *size its length; head: its first cap bytes
 * at most, no CRC check, for a look at its header). RA_OK or an RA_ERR_* code. */
int rom_archive_probe(int kind, fat_file *f, const char *exts, unsigned member, uint32_t *size);
int rom_archive_load(int kind, fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size);
int rom_archive_head(int kind, fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size);
/* Every ROM in the archive, in order: each(ctx, its name inside, folders and all). Returns how
 * many, or an RA_ERR_* code (the archive could not be read). */
int rom_archive_list(int kind, fat_file *f, const char *exts, void (*each)(void *ctx, const char *name), void *ctx);

/* the menus' words for an RA_ERR_* code (English; the menus translate it) */
const char *rom_archive_error_text(int err);

/* for rom_7z.c: the shared scratch memory, and reading the archive */
struct ra_scratch { uint8_t *buf; uint32_t size; };
int ra_scratch_get(struct ra_scratch *s);
int ra_read(fat_file *f, uint64_t off, void *buf, uint32_t len);
int ra_ext_match(const char *name, const char *exts);
void ra_tick(void);
/* mode 0 probe, 1 load, 2 head, 3 list (each called for every ROM; *size gets how many) */
int rom_7z_open(fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size, int mode,
		void (*each)(void *ctx, const char *name), void *ctx);

#endif
