/*
 * pico_coreopt.h - a core's own options (the ones it declares itself), for every core that lists
 * them: the choices saved per system on the card, handed to the core before each game loads, and
 * a Core Settings page in both in-game menus (the Current theme's, through struct igm_core's opt_*
 * calls, and the Classic panel's, drawn here).
 */
#ifndef PICO_COREOPT_H
#define PICO_COREOPT_H

#include "fat_ro.h"

/* as the core ABIs declare it (genesis_core_abi.h, nes_core_abi.h, snes_core_abi.h) */
#ifndef PICO_CORE_OPTION_DEFINED
#define PICO_CORE_OPTION_DEFINED
struct pico_core_option {
	const char *key;
	const char *label;
	const char *category;
	const char *const *values;
	const char *const *labels;
	int nvalues;
	int def;
};
#endif

/* the running core's list and its setter */
struct pico_coreopt_core {
	int (*count)(void);
	const struct pico_core_option *(*info)(int index);
	int (*set)(const char *key, const char *value);
};

/* A game starts on this core: the choices saved for system sys (a short name, "genesis") are read
 * from the card and given to the core (call it before the core loads the game). */
void pico_coreopt_begin(fat_vol *v, const char *sys, const struct pico_coreopt_core *c);
void pico_coreopt_end(void);

/* for struct igm_core (pico_igm.h): the page's rows */
int         pico_coreopt_n(void);
const char *pico_coreopt_label(int i);
const char *pico_coreopt_value(int i, char *buf);	/* buf holds 96 */
void        pico_coreopt_change(int i, int dir);		/* saved at once */

/* the Classic panel's Core Settings page */
void pico_coreopt_page_open(void);
int  pico_coreopt_page_is_open(void);
/* the menu's keys this frame (PICO_COREOPT_K_*, as edges); 1 when B closed the page */
enum { PICO_COREOPT_K_UP = 1, PICO_COREOPT_K_DOWN = 2, PICO_COREOPT_K_LEFT = 4, PICO_COREOPT_K_RIGHT = 8,
       PICO_COREOPT_K_B = 16 };
int  pico_coreopt_page_input(unsigned keys);
void pico_coreopt_page_paint(unsigned int *buf, unsigned int pitch, unsigned int W, unsigned int H);

#endif
