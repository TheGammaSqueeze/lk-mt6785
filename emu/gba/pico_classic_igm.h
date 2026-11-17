/*
 * pico_classic_igm.h - the in-game menu in the Classic themes' look (the SNES-style menus and their
 * themes, GBA Indigo among them): a sheet that slides down over the dimmed game, framed and lettered
 * like the menu's own Options page, the pages as tabs under its header with the theme's cyan box on
 * the one shown, the theme's blue bar on the focused row and its switches for the on/off values.
 *
 * The menu's pack is gone once a game loads (its memory is the game's), so what the sheet draws
 * with (three of the menu's fonts and a few dozen of its sprites, in the theme's colours) is copied
 * as a game starts into memory no game and no opening writes: the kit. A game started before the
 * menu ever ran (Resume on Power On) has no kit, and keeps the older panel.
 *
 * pico_igm.h's calls hand over to these when a Classic theme is in use (pico_current.c).
 */
#ifndef PICO_CLASSIC_IGM_H
#define PICO_CLASSIC_IGM_H

#include "menu/snes_pack.h"
#include "pico_igm.h"

/* a game starts from a Classic theme's menu: copy the kit out of its pack (the theme applied) */
void pico_classic_kit_take(const snes_pack *pk);

/* the kit is there for this game (the language's texts read too); 0 keeps the older panel */
int  pico_classic_igm_on(void);
void pico_classic_igm_open(const struct igm_core *c);
void pico_classic_igm_dismiss(void);
int  pico_classic_igm_frame(unsigned int keys);
void pico_classic_igm_overlay(int open);

#endif
