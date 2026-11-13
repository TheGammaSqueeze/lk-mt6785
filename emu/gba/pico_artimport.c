/*
 * pico_artimport.c - box art put on the card as PNG, made the menu's own (pico_artimport.h).
 *
 * A picture named after a game (<game>.png, or <game>.<rom extension>.png) in a boxart folder
 * beside the game, or in /roms/<system>/boxart, is decoded and shrunk to the menu's size
 * (pico_png.c), written next to it as <game>.ART, and the PNG is deleted once the .ART is safely
 * on the card. A picture that does not decode is left where it is. Run once a power on, before
 * the box art is loaded; the PNG is read whole into its own memory (ayaneo_import_arena).
 */
#include <stdio.h>
#include <string.h>
#include <platform.h>
#include "pico_artimport.h"
#include "pico_png.h"
#include "gba_boxart_sd.h"
#include "fat_wr.h"

extern void *ayaneo_import_arena(unsigned int *size);
extern void mtk_wdt_restart(void);
extern unsigned gpt4_get_current_tick(void);

#define ART_SIDE    224                 /* the box art window (gen_boxart.py --max-dim) */
#define MAX_FILES   128                 /* PNGs taken from a folder at a time */
#define MAX_DIRS    64
#define WORK_SIZE   0x00140000u         /* 224 * 224 sums + two rows of a 16384 wide picture */
#define OUT_SIZE    0x00020000u         /* a 224 x 224 .ART */

struct png_file { char name[256]; uint32_t clus, size; };

volatile unsigned int g_dbg_artimp_ok, g_dbg_artimp_fail, g_dbg_artimp_ms;   /* oem diag */
volatile int g_dbg_artimp_last;         /* the last failure: pico_png_to_art's code, -10 read, -11 write */

static int ends_png(const char *n)
{
	int L = (int)strlen(n);
	return L > 4 && n[L - 4] == '.' && (n[L - 3] | 32) == 'p' && (n[L - 2] | 32) == 'n' && (n[L - 1] | 32) == 'g';
}

static int same_ci(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) != (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b)) return 0;
	return *a == *b;
}

/* one folder's PNGs; how many were turned into .ART */
static int import_dir(fat_vol *v, const char *dir, struct png_file *list, unsigned char *work,
		      unsigned char *out, unsigned char *file, unsigned int file_cap)
{
	fat_dir d;
	fat_dirent e;
	int n = 0, i, done = 0;
	if (fat_opendir(v, dir, &d) != 0) return 0;
	while (n < MAX_FILES && fat_readdir(&d, &e)) {   /* gathered first: the folder changes below */
		if (e.is_dir || !e.size || e.name[0] == '.' || !ends_png(e.name)) continue;
		strncpy(list[n].name, e.name, sizeof list[n].name - 1);
		list[n].name[sizeof list[n].name - 1] = 0;
		list[n].clus = e.first_clus;
		list[n].size = e.size;
		n++;
	}
	for (i = 0; i < n; i++) {
		char path[400], stem[256];
		fat_file f;
		int len, k;
		mtk_wdt_restart();
		if (list[i].size > file_cap) { g_dbg_artimp_fail++; g_dbg_artimp_last = -4; continue; }
		snprintf(path, sizeof path, "%s/%s", dir, list[i].name);
		if (fat_open(v, path, &f) != 0 || fat_read(&f, 0, file, list[i].size) != list[i].size) {
			g_dbg_artimp_fail++; g_dbg_artimp_last = -10; continue;
		}
		len = pico_png_to_art(file, list[i].size, ART_SIDE, work, WORK_SIZE, out, OUT_SIZE, mtk_wdt_restart);
		if (len < 0) { g_dbg_artimp_fail++; g_dbg_artimp_last = len; continue; }
		/* <game>.png -> <game>.ART; gba_sd_write_named also drops a ROM extension left
		 * before the .png, the way the loader names it from the game */
		k = (int)strlen(list[i].name) - 4;
		memcpy(stem, list[i].name, (size_t)k);
		stem[k] = 0;
		if (gba_sd_write_named(v, dir, stem, "ART", out, (uint32_t)len) != 0) {
			g_dbg_artimp_fail++; g_dbg_artimp_last = -11; continue;
		}
		fat_wr_delete(v, dir, list[i].name, list[i].clus);
		g_dbg_artimp_ok++;
		done++;
	}
	/* a folder with more pictures than one gathering: again, while that turns any into art */
	return n == MAX_FILES && done ? done + import_dir(v, dir, list, work, out, file, file_cap) : done;
}

int pico_artimport_run(fat_vol *v, const gba_rom_entry *roms, int nrom)
{
	unsigned int size, t0 = gpt4_get_current_tick();
	unsigned char *a = (unsigned char *)ayaneo_import_arena(&size);
	struct png_file *list;
	char (*dirs)[112];
	unsigned char *work, *out, *file;
	int nd = 0, i, j, total = 0;
	if (!v || !a) return 0;
	list = (struct png_file *)a;
	dirs = (char (*)[112])(a + MAX_FILES * sizeof *list);
	work = (unsigned char *)dirs + MAX_DIRS * sizeof *dirs;
	work = (unsigned char *)(((unsigned long)work + 63u) & ~63ul);
	out = work + WORK_SIZE;
	file = out + OUT_SIZE;
	/* the boxart folders beside the games, and GammaOS's own for each system */
	for (i = 0; i < nrom + 8 && nd < MAX_DIRS; i++) {
		char dir[112];
		if (i < nrom) gba_boxart_dir(&roms[i], dir, sizeof dir);
		else if (gba_boxart_home_dir(i - nrom)) strcpy(dir, gba_boxart_home_dir(i - nrom));
		else continue;
		for (j = 0; j < nd && !same_ci(dirs[j], dir); j++) ;
		if (j == nd) strcpy(dirs[nd++], dir);
	}
	for (i = 0; i < nd; i++)
		total += import_dir(v, dirs[i], list, work, out, file, (unsigned int)(a + size - file));
	g_dbg_artimp_ms = (gpt4_get_current_tick() - t0) / 13000u;
	return total;
}
