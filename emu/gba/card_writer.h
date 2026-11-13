/*
 * card_writer.h - whole files written to the card off the game's thread. A save state written from
 * the in-game menu held the game for a tenth of a second or more (a whole card write), the sound
 * ran dry and played its last moment again. The writer copies the file at once into memory of its
 * own and a low priority thread puts it on the card while the game runs on.
 */
#ifndef CARD_WRITER_H
#define CARD_WRITER_H

#include <stdint.h>
#include "fat_ro.h"

/* Queues dir/name (the folder made when missing) with a copy of buf. status, when given, is set to
 * ok or fail once the file is on the card (the in-game menu's note). Waits only when earlier files
 * still fill the writer's memory; writes on the caller's thread when there is no writer. 0 queued
 * (or written), -1 a write here failed. */
int  card_writer_put(fat_vol *v, const char *dir, const char *name, const void *buf, uint32_t len,
		     char *status, int status_cap, const char *ok, const char *fail);
/* Returns once every queued file is on the card (or after 5 s): before a file is read back, a game
 * ends or the console powers off. */
void card_writer_flush(void);
/* files still to write (or being written) */
int  card_writer_pending(void);

#endif
