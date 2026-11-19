/*
 * pico_net_ui.h - the Network tab of the consoles with Wi-Fi, for every menu theme: Wi-Fi (the
 * networks around, joining with a password typed on the menu's keyboard, the saved ones), box
 * art from the network, and RetroAchievements' account.
 *
 * The flow lives here once (pico_net_ui.c); a theme draws what nui_view() describes in its own
 * style (a list of rows with values, a status line, a bar, or the keyboard), feeds the buttons in
 * (nui_input) and calls nui_tick() every frame while its menu runs.
 */
#ifndef PICO_NET_UI_H
#define PICO_NET_UI_H

#include "sd_fat.h"

enum { NUI_CLOSED, NUI_HOME, NUI_WIFI, NUI_KEYBOARD, NUI_ART, NUI_RA, NUI_SAVED };	/* SAVED: the saved networks, near or not */

/* buttons, as edges this frame */
enum { NUI_UP = 1, NUI_DOWN = 2, NUI_LEFT = 4, NUI_RIGHT = 8, NUI_A = 16, NUI_B = 32, NUI_X = 64, NUI_Y = 128 };

/* what a theme is asked to do (nui_input's return) */
enum { NUI_ACT_NONE, NUI_ACT_CLOSE, NUI_ACT_ART };	/* ART: box art changed: show it afresh */

#define NUI_ROWS 12

/* the keyboard: four rows of ten keys (kb_keys), then a row of wide keys (kb_wide) */
struct nui_key {
	const char *label;
	int col, span;			/* where it starts in the row of ten, how many columns */
};
struct nui_kb {
	const char *field;		/* what is typed ("Password for SkyBox") */
	char shown[72];			/* the text as shown (dots for a password) */
	const char *keys[4];		/* the rows of ten characters of the layout in use */
	struct nui_key wide[5];		/* the bottom row */
	int r, c;			/* the key in focus: r 0..4, c 0..9 */
	const char *note;		/* a line under the field ("8 to 63 characters"), may be NULL */
};

struct nui_view {
	int screen;
	const char *title;
	const char *subtitle;		/* may be NULL */
	int nrows;			/* a list: the rows in view, their values (may be NULL) and the selection */
	const char *rows[NUI_ROWS];
	const char *values[NUI_ROWS];
	int sel;			/* -1: none */
	int more_up, more_down;		/* the list goes on above / below */
	const char *status;		/* a line under the list or alone, may be NULL */
	int busy;			/* something is going on */
	unsigned int done, total;	/* a bar (total 0: none) */
	int nhints;
	const char *hint_code[4];
	const char *hint_label[4];
	struct nui_kb kb;		/* screen NUI_KEYBOARD */
};

void nui_open(void);			/* the Network tab's home */
int  nui_is_open(void);
void nui_close(void);
/* the menu starts over the card's games (box art); vol: the card */
void nui_set_roms(fat_vol *vol, const gba_rom_entry *roms, int nrom);
void nui_view(struct nui_view *v);
int  nui_input(int edges);
/* every frame of the menu: the service's news, the box art's steps, the saved networks and the
 * clock kept on the card and in the RTC. NUI_ACT_ART when new box art is on the card */
int  nui_tick(void);
/* the Network tab's home value lines, for a theme's tab or summary ("Connected to SkyBox") */
const char *nui_wifi_summary(void);

#endif
