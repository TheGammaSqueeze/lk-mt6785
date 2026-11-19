/*
 * pico_scrape.h - box art from the network for the games on the card that have none: from
 * ScreenScraper (matched by the ROM's CRC, when the build carries our developer credentials) or
 * from the libretro thumbnails (matched by name). The PNGs go into each game's boxart folder and
 * the art importer turns them into the menu's own form at the end.
 *
 * It runs on the menu thread, a small step every frame (pico_scrape_step), and asks the network
 * service (pico_netsvc.h) for the transfers, so the menu stays live and can show its progress.
 */
#ifndef PICO_SCRAPE_H
#define PICO_SCRAPE_H

#include "sd_fat.h"

/* SCRAPE_BOTH: libretro's thumbnails first (by the game's name, static files: quick), ScreenScraper
 * for the games libretro does not have (by CRC, one request at a time for our account) */
enum { SCRAPE_SCREENSCRAPER, SCRAPE_LIBRETRO, SCRAPE_BOTH };

struct pico_scrape_info {
	int running;
	int done, total;		/* games looked at, games without art when it started */
	int found, missing, failed;	/* art saved, none on the source, errors */
	int made;			/* turned into the menu's form at the end */
	const char *game;		/* the game in hand */
	const char *why;		/* why it stopped, "" none */
};

/* this build can use ScreenScraper (its credentials were generated into it) */
int  pico_scrape_has_screenscraper(void);
/* starts over the card's games: source SCRAPE_*, region "wor", "us", "eu" or "jp" (the first
 * tried; the others follow) */
void pico_scrape_start(fat_vol *v, const gba_rom_entry *roms, int nrom, int source, const char *region);
/* every frame while running: 1 still going, 0 finished */
int  pico_scrape_step(void);
void pico_scrape_cancel(void);
void pico_scrape_get(struct pico_scrape_info *info);

#endif
