/*
 * Save/state persistence to the microSD, matched to the ROM file name:
 *   <rombase>.gba  ->  saves/gba/<rombase>.sav , states/gba/<rombase>.st<slot>
 * Thin glue over fat_ro (read) + fat_wr (write). The emu save/load path calls
 * these when running an SD ROM; the gpSP state/sav BUFFERS are unchanged.
 */
#ifndef GBA_SD_SAVE_H
#define GBA_SD_SAVE_H

#include "fat_ro.h"

/* Read <dir>/<rombase>.<ext> into dst (<= cap). Returns bytes read, 0 if absent. */
uint32_t gba_sd_read_named(fat_vol *v, const char *dir, const char *romname,
			   const char *ext, unsigned char *dst, uint32_t cap);

/* Write <dir>/<rombase>.<ext> with len bytes (create or replace). 0 = ok. */
int gba_sd_write_named(fat_vol *v, const char *dir, const char *romname,
		       const char *ext, const void *buf, uint32_t len);

/* The file name <rombase>.<ext> the reads and writes above use for a game (legacy: the older form
 * that cut the name at its last dot, whatever followed it). */
void gba_sd_file_name(const char *rom, const char *ext, char *out, int cap, int legacy);

/* A game's key (gba_rom_entry.name, UTF-8) as versions before 1.1 filed it: every character
 * outside ASCII as '_', one for each UTF-16 unit it takes (so two for one beyond U+FFFF), cut at
 * 127. Returns 1 when that differs from the key (it holds a character outside ASCII), 0 when it
 * is the same; out (cap bytes) gets it either way. Reads of a game's files fall back to it, and
 * writes always go under the key, so a game's files move to it the first time they are written. */
int gba_sd_legacy_key(const char *key, char *out, int cap);

/* The underscore forms the library's games have, so a game reads files under its form only when
 * it is its alone: two games of one form (two names of the same shape in other scripts, or a name
 * that is that form itself) leave such files as they are, for neither can tell whose they are.
 * The listing (sd_fat.c) calls begin, then add with every game's key. */
void gba_sd_legacy_begin(void);
void gba_sd_legacy_add(const char *key);
/* 1 when two or more of the games listed have this underscore form (or nothing is listed yet):
 * no game reads files under it */
int  gba_sd_legacy_shared(const char *legacy);
/* gba_sd_legacy_key, then 1 only when that form differs from the key and is the game's alone */
int  gba_sd_legacy_own(const char *key, char *out, int cap);

/* Is the resume record rec (the name ayaneo_resume_mark kept, its first 99 bytes) the game of this
 * key? 2: it is the key; 1: it is the key's form before 1.1 (gba_sd_legacy_key) and no other
 * game has that form (gba_sd_legacy_own); 0: no. A caller looks for a 2 first. */
int gba_sd_resume_is(const char *key, const char *rec);

/* Convenience wrappers for the fixed layout. slot 0..9 -> ".st0".."st9". */
uint32_t gba_sd_load_sav(fat_vol *v, const char *rom, unsigned char *dst, uint32_t cap);
int      gba_sd_write_sav(fat_vol *v, const char *rom, const void *buf, uint32_t len);
uint32_t gba_sd_load_state(fat_vol *v, const char *rom, int slot, unsigned char *dst, uint32_t cap);
int      gba_sd_write_state(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len);
/* The same writes off the caller's thread (card_writer.c): the data is copied at once, the file is
 * on the card a little later. status, when given, gets ok or fail then (a menu's note). */
int      gba_sd_write_state_bg(fat_vol *v, const char *rom, int slot, const void *buf, uint32_t len,
			       char *status, int cap, const char *ok, const char *fail);
int      gba_sd_write_sav_bg(fat_vol *v, const char *rom, const void *buf, uint32_t len);
/* "<rombase>.<ext>", as the card keeps a game's file under (out holds cap bytes) */
void     gba_sd_state_name(const char *romname, const char *ext, char *out, int cap);
int      gba_sd_write_named_bg(fat_vol *v, const char *dir, const char *romname, const char *ext,
			       const void *buf, uint32_t len);

/* GammaOS Pico menu settings on the SD card (/gba/settings.bin). 0 = ok. */
int gba_sd_settings_load(fat_vol *v);
int gba_sd_settings_save(fat_vol *v);
int gba_sd_settings_write(fat_vol *v, const unsigned char *b, int n);

#endif
