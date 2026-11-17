/*
 * pico_classic_set.h - the Classic themes' menubar as GammaOS has it: Link, Display, Settings,
 * Language and System, the same settings the Current theme offers (pico_settings.h), drawn in the
 * menu's own Options look (pico_classic_ui.h) and sliding in as the menu's pages do.
 */
#ifndef PICO_CLASSIC_SET_H
#define PICO_CLASSIC_SET_H

#include "menu/snes_menu.h"

/* the menubar's slots, left to right */
enum { CSLOT_LINK, CSLOT_DISPLAY, CSLOT_SETTINGS, CSLOT_LANGUAGE, CSLOT_SYSTEM };

struct pico_classic_themes {
	int n, cur;                          /* how many, the one in use */
	const snes_img_entry *const *thumb;  /* each one's thumbnail */
	const char *const *label;            /* each one's name (0: none) */
	unsigned int beta;                   /* a bit each: still being finished */
};

/* each run of the menu, after snes_menu_init: the slots become ours */
void pico_classic_set_begin(snes_menu *m, const snes_pack *pk, const struct pico_classic_themes *th);
/* what a page asked for (and clears it): a theme to switch to, a language to use; -1 none */
int  pico_classic_set_take_theme(void);
int  pico_classic_set_take_lang(void);

#endif
