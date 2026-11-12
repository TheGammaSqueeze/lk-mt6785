/*
 * pico_igm.h - the in-game menu in the Current theme's style (pico_current.c), shared by every
 * core. A core describes its rows once (which page each lives on and how it behaves) and keeps its
 * own label, value and change handlers; the menu does the layout, the pages, the input and the
 * animation, and draws on the live overlay layer over the running game.
 */
#ifndef PICO_IGM_H
#define PICO_IGM_H

/* the pages: the top list and the settings grouped under it */
enum { IGM_TOP, IGM_DISPLAY, IGM_SOUND, IGM_PLAY, IGM_SYSTEM, IGM_CORE, IGM_NPAGES };

/* how a row behaves: a value Left/Right changes (A too, for the on/off ones), an action A runs,
 * a figure only shown, or a link to a page (its item is the page) */
enum { IGM_VALUE, IGM_ACTION, IGM_INFO, IGM_LINK };

/* the keys a core hands over each frame (held, not edges) */
enum { IGM_K_UP = 1, IGM_K_DOWN = 2, IGM_K_LEFT = 4, IGM_K_RIGHT = 8, IGM_K_A = 16, IGM_K_B = 32 };

struct igm_row { short item; unsigned char page, kind; };

struct igm_core {
	const struct igm_row *rows;
	int nrows;
	/* the core's items with a part of their own (-1 when it has none): the save slot is changed
	 * with Left/Right on the save and load rows rather than shown as a row */
	int slot_item, save_item, load_item, reset_item, exit_item;
	const char *(*label)(int item);
	const char *(*value)(int item, char *buf);      /* buf holds 48 */
	int (*change)(int item, int dir, int act);       /* the core's handler; 1 closes the menu */
	char *status;                                    /* the core's status line: shown, then cleared */
	/* the core's own options (the IGM_CORE page), or 0 */
	int (*opt_n)(void);
	const char *(*opt_label)(int i);
	const char *(*opt_value)(int i, char *buf);
	void (*opt_change)(int i, int dir);
	int system;                                      /* GBA_CONSOLE_* */
	const char *game;                                /* the game's file name */
};

/* the Current theme is in use and the menu's pack loads (loaded on the first call of a game) */
int  pico_igm_on(void);
/* the menu opens (the core sets its own open flag) */
void pico_igm_open(const struct igm_core *c);
/* a tap of the menu key while open: the menu closes, animated */
void pico_igm_dismiss(void);
/* once a frame while the core's open flag is set: -1 still open, else it has closed and the
 * bits say which buttons to hold back from the game until released (1 A, 2 B) */
int  pico_igm_frame(unsigned int keys);
/* every frame, open or not, where the core puts its overlay: the menu's layer */
void pico_igm_overlay(int open);

#endif
