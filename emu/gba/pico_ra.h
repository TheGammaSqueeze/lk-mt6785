/*
 * pico_ra.h - RetroAchievements in GammaOS Pico (the consoles with Wi-Fi): the blob that runs
 * rcheevos (cores/ra-pico), the account and what is kept on the card, the server calls, and the
 * cards shown over the game. Softcore only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * Threads: everything here runs on the thread that runs the menu and the games' frontends, which
 * is also the one thread that uses the card (files written during a game go through the card
 * writer). The network is used from the menu and before a game's first frame, never during play:
 * unlocks made while playing wait on the card and go out from the menu.
 */
#ifndef PICO_RA_H
#define PICO_RA_H

#include "sd_fat.h"
#include "pico_core_memory.h"

enum { PICO_RA_SIGNED_OUT, PICO_RA_SIGNING_IN, PICO_RA_SIGNED_IN, PICO_RA_NO_BLOB };

/* the card is there (the menu's start): the account and the unlocks still to send */
void pico_ra_card(fat_vol *v);
int  pico_ra_state(void);
const char *pico_ra_user(void);			/* the name signed in ("" none) */
const char *pico_ra_note(void);			/* the last outcome to show ("" none) */
unsigned pico_ra_points(void);			/* softcore points, signed in */
unsigned pico_ra_waiting(void);			/* unlocks on the card not yet sent */

/* the menu: sign in with a password (the token is kept, never the password), sign out */
void pico_ra_sign_in(const char *user, const char *password);
void pico_ra_sign_out(void);
void pico_ra_send_now(void);			/* kept unlocks to the server at the next chance */
/* every menu frame: server calls, and the unlocks waiting sent while there is a network */
void pico_ra_menu_tick(void);

/* a game: the menu picked one of this system (GBA_CONSOLE_*); its core is running with this ROM
 * and memory (before its first frame: may wait a few seconds for the network); every committed
 * frame; the in-game menu open (once a frame); the game ends */
void pico_ra_game_begin(int type);
void pico_ra_core_ready(const void *rom, unsigned rom_size, const struct pico_core_memory *mem);
void pico_ra_frame(void);
void pico_ra_paused(void);
/* hardcore: after pico_ra_frame, 1 once when the game must restart from its power on (the runner
 * resets its core, then pico_ra_game_reset); the game was reset; hardcore holds now (the gates) */
int  pico_ra_reset_wanted(void);
void pico_ra_game_reset(void);
int  pico_ra_hardcore(void);
/* the in-game menu may pause the game now (hardcore limits how often): 0 not yet, a card says so */
int  pico_ra_may_pause(void);
void pico_ra_game_end(void);
/* a save state was loaded by the player: rcheevos' counts start again from it */
void pico_ra_state_loaded(void);
/* a save state saved / loaded (dir, the game's name, the state's extension): its progress is kept
 * beside it, and brought back with it (counts start again when there is none) */
void pico_ra_state_saved(fat_vol *v, const char *dir, const char *rom, const char *state_ext);
void pico_ra_state_loaded_from(fat_vol *v, const char *dir, const char *rom, const char *state_ext);
/* a save state: rcheevos' progress, kept beside it (size 0: none) */
unsigned pico_ra_progress_size(void);
int  pico_ra_progress_save(void *buf, unsigned cap);
void pico_ra_progress_load(const void *buf, unsigned len);

/* the card to draw over the game now: 1 there is one */
enum { PICO_RA_CARD_UNLOCK, PICO_RA_CARD_MASTERED, PICO_RA_CARD_GAME, PICO_RA_CARD_PROGRESS, PICO_RA_CARD_NOTE };
struct pico_ra_card {
	int kind;
	char head[48];			/* "Achievement unlocked" */
	char title[96];			/* the achievement or the game */
	char line[96];			/* its description, or "12 of 40 achievements" */
	int points;			/* -1 none */
	char badge[16];			/* its badge's name ("" none): pico_ra_badge */
	unsigned alpha;			/* 0..255: it fades in and out */
};
int  pico_ra_card_now(struct pico_ra_card *c);
/* a badge of the game being played, 48 x 48 RGB565, or 0 (not fetched yet) */
#define PICO_RA_BADGE 48
const unsigned short *pico_ra_badge(const char *name);

/* the achievements of the game running, for the in-game menu */
int  pico_ra_game_loaded(void);
int  pico_ra_list(void);			/* builds the list: how many */
const char *pico_ra_row_title(int i);		/* row i of that list: its title, its state ("Unlocked", */
const char *pico_ra_row_value(int i);		/* "23/50", "10 points") and its description */
const char *pico_ra_row_text(int i);
const char *pico_ra_short(void);		/* "12/40", for the menu's Achievements row */
/* the page's account and settings rows come first: a switch row (-1 none, else on 1 / off 0) flips
 * with Left, Right or A (1 changed); the page is there for a game of a system RA knows */
int  pico_ra_row_switch(int i);
int  pico_ra_row_change(int i);
int  pico_ra_igm_page(void);
int  pico_ra_item(int i, unsigned *id, char *title, unsigned tcap, char *line, unsigned lcap, int *unlocked, unsigned *points);
void pico_ra_game_summary(char *line, unsigned cap);

#endif
