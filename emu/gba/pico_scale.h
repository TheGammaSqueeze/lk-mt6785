/*
 * pico_scale.h - how a game's picture is laid on the screen, the options every system shares.
 *
 * Each system keeps its own Aspect Ratio row and choices (Pixel Perfect, Fit, Core, Stretch...).
 * On top of those, the same for every system and kept per system in the settings:
 *   - more ratios on that row: fixed ones the system does not offer itself, and Custom, whose two
 *     numbers are rows of their own;
 *   - Integer Scale: On keeps the picture's height at a whole multiple of the game's (and its
 *     width too where the pixels are square), Overscale takes the next whole multiple up and cuts
 *     the edges that no longer fit;
 *   - Filter Alignment: the LCD filter's lines follow the game's pixels (as before), or the
 *     screen's own, which keeps them even where the picture is not at a whole scale.
 * The display code reads the settings each frame (platform/common/pico/pico_disp.inc,
 * pico_view_of), so a change shows at once behind the menu.
 */
#ifndef PICO_SCALE_H
#define PICO_SCALE_H

/* the systems, in the order the settings keep them (Big Core's and the CPU clock's order) */
enum { PICO_SCALE_GBA, PICO_SCALE_GBC, PICO_SCALE_SNES, PICO_SCALE_GENESIS, PICO_SCALE_NES, PICO_SCALE_NSYS };

/* the shared ratios (the settings keep the number; 0 = the system's own choice stands) */
enum { PICO_RATIO_OWN, PICO_RATIO_4_3, PICO_RATIO_3_2, PICO_RATIO_16_9, PICO_RATIO_16_10, PICO_RATIO_5_4,
       PICO_RATIO_8_7, PICO_RATIO_10_9, PICO_RATIO_1_1, PICO_RATIO_CUSTOM, PICO_RATIO_COUNT };

/* The Aspect Ratio row. A system with n choices of its own, the current one at place pos of its
 * order, gets the shared ratios after them, less the ones in `own` (bit r set: the system offers
 * ratio r itself). The name is the shared ratio in force, or 0 while the system's own choice
 * stands. A step returns the place of the system's own choice to take, or -1 when a shared ratio
 * is in force after it (the system's choice is then left as it was). */
const char *pico_scale_aspect_name(int sys);
int  pico_scale_aspect_step(int sys, int dir, int pos, int n, unsigned own);

/* the other rows */
enum { PICO_SCALE_ROW_WHOLE, PICO_SCALE_ROW_ONSCREEN, PICO_SCALE_ROW_CUSTOM_W, PICO_SCALE_ROW_CUSTOM_H };
const char *pico_scale_row_label(int row);
char *pico_scale_row_value(int sys, int row, char *p);   /* written at p, the end returned */
/* Left/Right on a row: 1 = a setting changed (the caller persists the settings) */
int  pico_scale_row_change(int sys, int row, int dir);

#endif
