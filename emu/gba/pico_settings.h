/*
 * pico_settings.h - the settings every menu theme offers, in one place: their names, the line
 * that says more about them, the sub-pages they open, their values as shown and what a press does
 * to them. Each theme lays them out its own way (pico_current.c, pico_classic_set.c); the ones
 * that depend on the theme itself (which theme, its colour, the language picked but not yet
 * applied) are worked by the theme and only named here.
 */
#ifndef PICO_SETTINGS_H
#define PICO_SETTINGS_H

enum {
	PS_BRIGHT, PS_VOLUME, PS_BFI, PS_THEME, PS_COLOUR, PS_BIOS, PS_RESUME, PS_BOOT_OS,
	PS_SKIP_INTRO, PS_INTRO_BG, PS_MUTE_BIOS, PS_INTRO_RESUME, PS_RESUME_BOOT, PS_RESUME_OPEN,
	PS_LANGUAGE, PS_CLOCK, PS_YEAR, PS_MONTH, PS_DAY, PS_HOUR, PS_MINUTE, PS_ZONE, PS_CLOCK12,
	PS_ABOUT, PS_MUTE_MENU, PS_N
};

/* the sub-pages a row opens */
enum { PSP_NONE, PSP_BIOS, PSP_RESUME, PSP_CLOCK, PSP_N };

/* how a row answers: Left/Right step a value, a switch flips (Left, Right or A), a row opens a
 * page, an action runs on A, the theme picks its own (theme, colour, language), a figure only shows */
enum { PSK_STEP, PSK_SWITCH, PSK_PAGE, PSK_ACTION, PSK_PICK, PSK_INFO };

const char *pico_set_name(int id);        /* English: put through tr() to show */
const char *pico_set_about(int id);       /* the line that says more, or 0 */
int  pico_set_kind(int id);
int  pico_set_page(int id);               /* the page it opens (PSP_*), PSP_NONE for none */
int  pico_set_page_row(int page);         /* the row that opens a page */
const char *pico_set_page_name(int page);
/* its value as shown (translated), "" for a page or the theme's own; *num a level 0..100 where
 * there is one (brightness, volume), else -1 */
void pico_set_value(int id, char *buf, int n, int *num);
/* a press on it: dir -1/+1 for Left/Right, act 1 for A; 1 when something changed (and was saved).
 * Boot to Android does not come back. */
int  pico_set_change(int id, int dir, int act);
/* a switch's state (1 on), -1 when the row is not a switch */
int  pico_set_switch(int id);

#endif
