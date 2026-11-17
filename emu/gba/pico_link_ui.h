/*
 * pico_link_ui.h - playing together and sharing games over Bluetooth, for every menu theme.
 *
 * The flow lives here once (pico_link_ui.c): the Link screen, Play together (this console hosts
 * a game and waits), Join (the hosts nearby, then the game), the guest's own game for link cable
 * systems, and later Send / Receive and the start's progress. A theme only draws what lui_view()
 * describes, in its own style, feeds the buttons in (lui_input) and carries out the actions it
 * gets back (launch a game armed for a session, open its library to pick one).
 */
#ifndef PICO_LINK_UI_H
#define PICO_LINK_UI_H

#include "sd_fat.h"

/* screens */
enum { LUI_CLOSED, LUI_HOME, LUI_HOST_WAIT, LUI_JOIN_LIST, LUI_JOIN_WAIT, LUI_MESSAGE,
       LUI_SEND_LIST,		/* Send: the consoles nearby ready to receive */
       LUI_SEND_RUN,		/* Send: the game and its save going over */
       LUI_RECEIVE,		/* Receive: waiting for a console to send, then the files coming in */
       LUI_JOIN_COPY };		/* Join: the host's game is not on this card: copy it first? */

/* buttons, as edges this frame (lui_input) */
enum { LUI_UP = 1, LUI_DOWN = 2, LUI_LEFT = 4, LUI_RIGHT = 8, LUI_A = 16, LUI_B = 32 };

/* what a theme is asked to do (lui_input's return) */
enum {
	LUI_ACT_NONE,
	LUI_ACT_CLOSE,		/* the screen is done: back to the theme's menu */
	LUI_ACT_LAUNCH,		/* launch lui_action_rom(), armed (lui_action_host(): host or guest) */
	LUI_ACT_PICK,		/* open the library on system lui_action_system() to pick the guest's game,
				 * then lui_picked(rom) (or lui_picked(-1) when backed out) */
	LUI_ACT_RELIST,		/* games arrived on the card: the theme's menu lists the card again
				 * (its run returns -4; the Link screen stays as it is) */
};

#define LUI_ROWS 8

struct lui_view {
	int screen;
	const char *title;			/* the screen's name */
	const char *subtitle;			/* a game, a console: may be NULL */
	int nrows;				/* a list: names, values (may be NULL), the selection */
	const char *rows[LUI_ROWS];
	const char *values[LUI_ROWS];
	int sel;
	const char *status;			/* a line under the list or alone (waiting, a reason) */
	int busy;				/* something is going on: a theme may animate */
	unsigned int done, total;		/* a bar: how far (total 0: no bar) */
	int nhints;				/* buttons: their codes ("A", "B") and what they do */
	const char *hint_code[4];
	const char *hint_label[4];
};

/* The Link screen (the theme's Link tab or entry), over this console's game list. */
void lui_open_home(const gba_rom_entry *roms, int nrom);
/* The same, with Play together for that game as its first row: for a theme whose library has no
 * button of its own for it (the Classic menu opens this on the game in focus). */
void lui_open_home_game(const gba_rom_entry *roms, int nrom, int rom);
/* Send: that game (and its save) to a console nearby whose Receive screen is open. */
void lui_open_send(const gba_rom_entry *roms, int nrom, int rom);
/* Receive: this console waits for one to send it games. */
void lui_open_receive(void);
/* Play together: this console hosts that game and waits for a friend. */
void lui_open_host(const gba_rom_entry *roms, int nrom, int rom);
/* Join a game: the hosts nearby. */
void lui_open_join(const gba_rom_entry *roms, int nrom);
int  lui_open(void);			/* a Link screen is up */
/* A theme's menu starts over the card's games (after a listing too: a screen in the middle of
 * something carries on over the new list). */
void lui_set_roms(const gba_rom_entry *roms, int nrom);
/* The screens' words in English rather than the menus' language: for a theme whose fonts do not
 * have that language's letters. */
void lui_set_english(int on);
void lui_close(void);			/* close it (a theme's menu starting over a Link screen
					 * another theme left open: Bluetooth off as well) */
/* A theme's menu starts (back from a game, or at power on): with no Link screen open, Bluetooth
 * goes off (a session that just ended left it on). */
void lui_menu_enter(void);
int  lui_busy_screen(void);		/* one in the middle of something (waiting, joining): the
					 * theme keeps it in front (no tab change) */

int  lui_input(unsigned int edges);	/* once a frame, LUI_* edges; returns LUI_ACT_* */
void lui_view(struct lui_view *v);	/* what to draw now */
void lui_picked(int rom);		/* the answer to LUI_ACT_PICK */

int  lui_action_rom(void);
int  lui_action_host(void);
int  lui_action_system(void);

/* The start of a session (states, maybe cartridges, crossing): what its screen shows. A theme
 * sets its painter once; it is called at most every 100 ms during a start, draws the whole screen
 * itself (the menu is gone by then: the game is loading) and returns 1 to cancel (B). */
struct lui_progress {
	const char *title;			/* "Playing together" */
	const char *mine, *theirs;		/* this console's game, the other's (NULL: the same) */
	const char *other;			/* the other console */
	const char *step;			/* what is going on now */
	unsigned int done, total;		/* bytes of this step (total 0: no bar) */
	unsigned int kbs, secs_left;		/* its speed, the time left (0: not known yet) */
	const char *title_en, *step_en;		/* the same two in English, for a theme drawing with an
						 * ASCII font (as its in-game menu does) */
};
void lui_set_painter(int (*paint)(const struct lui_progress *p));

/* the console names the screens use */
const char *lui_model_name(int model);
const char *lui_system_name(int type);

#endif
