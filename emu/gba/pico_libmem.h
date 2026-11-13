/*
 * pico_libmem.h - memory for the game library and the menus' tables of it.
 *
 * A card can hold tens of thousands of games: their list, their names as the menus show them, the
 * per system indexes, where each box art is and the play times live in a DRAM window of their own
 * (ayaneo_library_arena), handed out once per power on and never given back.
 */
#ifndef PICO_LIBMEM_H
#define PICO_LIBMEM_H

#define PICO_GAMES_MAX 32768          /* games the library lists at most (indexes fit 16 bits) */

/* size bytes, 64 byte aligned and zeroed, or 0 when the window is missing or full */
void *pico_lib_alloc(unsigned int size);
/* bytes still free (oem diag) */
unsigned int pico_lib_left(void);

#endif
