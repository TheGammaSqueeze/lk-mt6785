/*
 * pico_state_shot.h - the picture and the time of a save state, kept beside it on the card: the
 * game as it was when the state was made, small, and the moment it was made. The Classic menu's
 * Suspend Point List shows them on its cards.
 *
 * A state <game>.st0 .. st2 has its picture in <game>.pv0 .. pv2, the suspend point <game>.sus
 * in <game>.pvs, in the same folder. A state from before this has none: its card shows no picture.
 */
#ifndef PICO_STATE_SHOT_H
#define PICO_STATE_SHOT_H

#include <stdint.h>
#include "fat_ro.h"

#define PSHOT_W 160   /* the card's screen (the Suspend Point List's 160 x 120) */
#define PSHOT_H 120

/* the picture's file extension for a state's ("sus" -> "pvs", "st1" -> "pv1"); out holds 4 */
void pico_shot_ext(const char *state_ext, char *out);

/* Takes the game's picture as it is on the screen now and writes it, with the time, beside the
 * state dir/<game>.<state_ext>. bg: written off the caller's thread (a save from the in-game menu);
 * else on it (a suspend point on the way out, before the console may power off). When the frame
 * is gone, the picture kept as it went (pico_shot_keep) is used, and with none the
 * time is written alone. 0 written or queued. */
int  pico_shot_save(fat_vol *v, const char *dir, const char *rom, const char *state_ext, int bg);

/* The display lets a game's frame go (the game is over): its picture is kept for a suspend point
 * the core saves after that. */
void pico_shot_keep(void);

/* Reads the picture of dir/<game>.<state_ext>: its RGB565 pixels into pix (PSHOT_W * PSHOT_H at
 * most), its size (0 x 0: no picture) and when it was made (seconds since 1970 UTC, 0 not known).
 * 1 read, 0 none. */
int  pico_shot_load(fat_vol *v, const char *dir, const char *rom, const char *state_ext,
		    uint16_t *pix, int *w, int *h, long long *utc);

#endif
