/*
 * pico_artimport.h - box art the user puts on the card as PNG, turned into the menu's .ART.
 */
#ifndef PICO_ARTIMPORT_H
#define PICO_ARTIMPORT_H

#include "sd_fat.h"

/* Every <game>.png in the boxart folders beside the listed games (and in /roms/<system>/boxart)
 * becomes <game>.ART, sized for the menu, and the PNG is deleted. Returns how many were. */
int pico_artimport_run(fat_vol *v, const gba_rom_entry *roms, int nrom);

#endif
