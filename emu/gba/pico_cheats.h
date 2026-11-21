/*
 * pico_cheats.h - cheat codes for every core, and the in-game menu's Cheats page.
 *
 * A game's cheats come from these places on the card:
 *   gammaos/cheats/<system>.gcd      the release's packs, made from libretro-database's cheat files
 *                                    (tools/cheats/build_cheats.py): found by the CRC-32 of the
 *                                    game's dump, or by its name for a dump no list has
 *   gammaos/cheats/<system>/<game>.cht   the player's own, in RetroArch's .cht form
 * Which of them are on is kept for each game in gammaos/cheats/<system>/<game>.on (their codes, a
 * line each), and they are put back on at the game's next start.
 *
 * The core decodes and runs the codes (its cheat_reset and cheat_add, in each core ABI). No cheats
 * in a netplay session: the other console's machine would not have them. While a cheat is on,
 * RetroAchievements waits (pico_ra.c): nothing unlocks, and its counts start again once every cheat
 * is off.
 */
#ifndef PICO_CHEATS_H
#define PICO_CHEATS_H

#include "fat_ro.h"

/* the core's two calls (0 for a core without them, or a session that may not have cheats) */
struct pico_cheat_core {
	void (*reset)(void);
	int  (*add)(const char *code);   /* 0 taken, negative not read */
};

/* a game has loaded (system GBA_CONSOLE_*, game its file name as the card has it, rom and size the
 * image the core runs): its cheats are found and those on put in force */
void pico_cheats_game(fat_vol *v, int system, const char *game, const void *rom, unsigned size,
		      const struct pico_cheat_core *core);
/* the game ends: nothing is in force any more */
void pico_cheats_game_end(void);
/* a cheat is in force now */
int  pico_cheats_on(void);

/* the in-game menu's Cheats page: offered whenever the core can take cheats. Its first row, Cheat
 * list, names the list in force and opens a chooser: the list found for the game, none, one of the
 * player's .cht files in the system's folder or any title of the pack (opened where the game's own
 * name would be). The pick is kept for the game (<game>.use). */
int  pico_cheats_page(void);
void pico_cheats_page_open(void);              /* the page is opened: on the game's list */
int  pico_cheats_choosing(void);               /* the page shows the chooser */
/* L / R on the page: the part of a long list before or after; 1 when it moved, 0 at the first or
 * last part (where the Classic menu goes on to the tab beside) */
int  pico_cheats_step_part(int dir);
int  pico_cheats_rows(void);                   /* the rows now (a part of a long list, see below) */
const char *pico_cheats_row_title(int i);      /* English for the page's own rows, else the cheat's */
const char *pico_cheats_row_value(int i);      /* "On", "Off", "Not read", the part shown; 0 none */
const char *pico_cheats_row_text(int i);       /* the focused row's words: its code */
int  pico_cheats_row_head(int i);              /* a group's heading (never focused) */
int  pico_cheats_row_switch(int i);            /* 1 on, 0 off, -1 not a switch */
/* Left / Right (dir) or A (dir 0) on row i: 1 when something changed */
int  pico_cheats_row_change(int i, int dir);
const char *pico_cheats_short(void);           /* the top page's Cheats row: how many are on ("2"), or 0 */

/* oem cheats: the state in a line (out holds cap); the command's argument; its lines (0 at the end);
 * and, once a frame on the game's thread, what the command asked for */
void pico_cheats_diag(char *out, int cap);
void pico_cheats_debug(const char *arg);
int  pico_cheats_debug_line(int i, char *buf, unsigned cap);
void pico_cheats_service(void);

#endif
