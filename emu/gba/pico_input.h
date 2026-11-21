/*
 * pico_input.h - the console's buttons for every core: which button plays which of the game's
 * buttons, the hotkeys, and the in-game menu's Controls page.
 *
 * Every key line the board has is read, the ones no core used before included (the Konkr Pocket
 * Block's MODE, second side key, back and volume keys, the Konkr Pocket Advance's spare lines): a
 * line with no button behind it never reads pressed. The player gives a button its job by pressing
 * it on the Controls page, so the spare ones need no names.
 *
 * Each system has its map (gammaos/controls/<system>.map), and a game may have its own beside it
 * (gammaos/controls/<system>/<game>.map), kept until the player puts it back to the system's. With
 * neither, the buttons are as they always were. A map is text: a line "<job>=<button>[+<button>]",
 * "<job>=-" for none.
 *
 * The game's buttons: each takes one of the console's (several may share one). A hotkey takes one
 * to three pressed together; while they are held the game does not see them. The menu key, the
 * power key and SELECT + START + L + R (the restart every core keeps) are not remapped.
 *
 * Everything runs on the game's thread: the cores' pad readers ask pico_input_pad (from inside the
 * core's frame too), the runner calls pico_input_frame once a frame for the hotkeys.
 */
#ifndef PICO_INPUT_H
#define PICO_INPUT_H

#include "fat_ro.h"

/* the console's buttons, bits of a mask; the spare lines follow PB_EXTRA */
enum { PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_L2, PB_R2,
       PB_START, PB_SELECT, PB_MENU, PB_EXTRA, PB_MAX = 24 };

/* the pad the cores take, libretro's joypad bits (RETRO_DEVICE_ID_JOYPAD_*) */
enum { PJ_B, PJ_Y, PJ_SELECT, PJ_START, PJ_UP, PJ_DOWN, PJ_LEFT, PJ_RIGHT, PJ_A, PJ_X, PJ_L, PJ_R,
       PJ_L2, PJ_R2, PJ_L3, PJ_R3 };

/* the hotkeys */
enum { PH_FF, PH_FF_HOLD, PH_RW, PH_RW_HOLD, PH_SAVE, PH_LOAD, PH_RESTART, PH_MENU, PH_SPECIAL, PH_COUNT };

struct igm_core;

/* the key lines set up as inputs (pull up, Schmitt trigger); once is enough, more is harmless */
void pico_input_init(void);
/* the console's buttons held now, a PB_ mask (a test's buttons from oem input ORed in) */
unsigned int pico_input_raw(void);
/* how many buttons the board has (PB_EXTRA + its spare lines) */
int pico_input_buttons(void);
/* a button's name: "A", "L2", "Menu", the spare ones "Button 1".. (buf holds 24) */
const char *pico_input_button_name(int b, char *buf);

/* a game starts (system GBA_CONSOLE_*, game its file name as the card has it): its map is read. igm is
 * the core's in-game menu (its save, load and restart run the hotkeys); special_item, one of its
 * items the Special hotkey runs (the Disk System's disk side), or -1 */
void pico_input_game(fat_vol *v, int system, const char *game, const struct igm_core *igm, int special_item);
void pico_input_game_end(void);
/* a netplay session runs: the game's buttons go on, the hotkeys that would change one console's
 * game only (states, restart) do not */
void pico_input_netplay(int on);

/* the game's pad now, PJ_ bits (turbo pulsed, a hotkey's buttons left out, the buttons that closed
 * the menu held back until released). The same inside one frame, whoever asks. */
unsigned int pico_input_pad(void);
/* the same in the GBA's key bits (A 1, B 2, Select 4, Start 8, Right, Left, Up, Down, R 0x100, L 0x200) */
unsigned int pico_input_gba_keys(void);
/* the same in gambatte's (A 1, B 2, Select 4, Start 8, Right 0x10, Left 0x20, Up 0x40, Down 0x80) */
unsigned int pico_input_gb_keys(void);

/* once a frame on the game's thread (menu_open: the in-game menu is up): the hotkeys pressed now run */
void pico_input_frame(int menu_open);
/* the in-game menu has closed: every button held now stays out of the game until it is released */
void pico_input_hold_back(void);
/* a hotkey's buttons are all held now (the fast forward and rewind toggles and holds read this) */
int  pico_input_hot_down(int hk);
/* the button is given a job it did not have (the volume keys then leave the volume alone) */
int  pico_input_taken(int b);
int  pico_input_key_taken(int code);           /* the same for a keypad key, by its code */

/* the Controls page (IGM_CTL): its rows, like the Cheats page's */
int  pico_input_page(void);                    /* a game is running: the page is offered */
void pico_input_page_open(void);
int  pico_input_rows(void);
const char *pico_input_row_title(int i);       /* English (the menu translates) */
const char *pico_input_row_value(int i);       /* what it is set to, or 0 */
int  pico_input_row_head(int i);               /* a group's heading (never focused) */
/* Left / Right (dir) or A (dir 0) on row i: 1 when something changed. A on a job listens for the
 * button (or buttons, for a hotkey) to give it */
int  pico_input_row_change(int i, int dir);
/* the page is listening for a button, or waits for the buttons to be let go after it: the menu
 * leaves the keys alone, and calls pico_input_listen each frame */
int  pico_input_busy(void);
void pico_input_listen(void);

/* oem input */
void pico_input_diag(char *out, int cap);
void pico_input_debug(const char *arg);

#endif
