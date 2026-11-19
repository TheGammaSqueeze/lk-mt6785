/*
 * pico_classic_ui.h - the parts the Classic themes' own screens are drawn with: the Options page's
 * frame, the theme's cyan cursor box, its blue bar, switches, arrows, radio buttons and button
 * icons, the menu's fonts, and the easings the menu moves with. Both the home's settings pages
 * (pico_classic_set.c, from the menu's pack) and the in-game sheet (pico_classic_igm.c, from the
 * kit a game start leaves) draw with them, so the two look the same.
 *
 * Coordinates are the menu's design units: 1280 across, as tall as the screen's shape makes it.
 */
#ifndef PICO_CLASSIC_UI_H
#define PICO_CLASSIC_UI_H

#include <stdint.h>
#include "menu/snes_render.h"

enum {
	KS_CORNER, KS_LINE, KS_BAR, KS_SW_ON, KS_SW_OFF, KS_AR_L, KS_AR_R, KS_DPAD, KS_BTN_A, KS_BTN_B,
	KS_IC_DISPLAY, KS_IC_OPTION, KS_IC_RESUME, KS_UP, KS_DOWN,
	KS_CUR1, KS_CUR2, KS_CUR3, KS_CUR4, KS_CUR6, KS_CUR7, KS_CUR8, KS_CUR9,
	KS_RADIO_OFF, KS_RADIO_ON, KS_CHECK, KS_IC_LANGUAGE, KS_IC_SYSTEM, KS_IC_LINK,
	KS_N
};
enum { CF_L, CF_M, CF_S };   /* the menu's fonts: large, middle, small */

/* where each sprite lies in the image it is drawn from */
struct cui_set {
	uint16_t img;                  /* the atlas image's index in its pack */
	uint16_t r[KS_N][4];           /* x y w h in it; w 0: the theme has none */
	uint32_t line_edge, line_core; /* the frame line's two colours, for the upright sides */
};

/* the sprites of a menu pack as they lie in its atlas (the theme's pixels applied); 0 when the
 * pack lacks the frame's parts */
int  cui_set_from_pack(struct cui_set *set, const snes_pack *pk);
/* draw with this pack and set into this target from now on */
void cui_use(const snes_pack *pk, const struct cui_set *set, snes_target *t);
/* the in-game kit (pico_classic_kit_take): its pack opened into pk and its set; 0 when there is
 * none for the theme in use */
int  cui_kit_open(snes_pack *pk, const struct cui_set **set);

float cui_clamp01(float t);
float cui_out_expo(float t);
float cui_in_expo(float t);
float cui_smooth(float t);

void  cui_spr(int k, float cx, float cy, float w, float h, float a, float r, float g, float b);
void  cui_spr_x(int k, float cx, float cy, float scale, float a);   /* at a scale of its own size */
void  cui_spr_flip(int k, float cx, float cy, float scale, int hf, int vf);
void  cui_quad(float x, float y, float w, float h, uint32_t argb, float a);
void  cui_fill(float x, float y, float w, float h, uint32_t argb);   /* opaque, straight into the picture */
void  cui_tex(const snes_img_entry *im, float cx, float cy, float w, float h, float a);   /* a whole image */
int   cui_has(int k);
const char *cui_say(int f, const char *en);   /* tr() where the font has its letters, else English */
float cui_text_w(int f, float sc, const char *s);
float cui_text(int f, float x, float cy, float sc, uint32_t argb, int align, const char *s, float a);
const char *cui_fit(int f, float sc, const char *s, float maxw, char *out, unsigned cap);
/* the theme's cyan box round (cx, cy), glowing with the clock (seconds) as the menubar's does */
void  cui_cursor_box(float cx, float cy, float w, float h, float a, float clock);
/* the Options page's frame round x0..x1, y0..y1, its inside black */
void  cui_frame(float x0, float y0, float x1, float y1);
/* button hints right-aligned to x_right at y (icons KS_*, labels already translated); where
 * they start */
float cui_hints(float x_right, float y, int n, const int *icons, const char *const *labels);
uint32_t cui_font(int f);   /* the font's hash */
/* the language pack's added letters for the l, m and s fonts (c_l, c_m, c_s), for the letters the
 * fonts lack; again after another pack is read (none: the fonts as they are) */
void  cui_lang_fonts(void);
/* game names in any script: whether the card's names pack is there (after pico_names_prepare), and
 * its glyphs added to every font of pk, nslots slots from slot0 (home menu 4, 8; in-game kit 12, 4) */
int   cui_names_ready(void);
void  cui_names_fonts(const snes_pack *pk, int slot0, int nslots);
/* a name cut at its last whole letter (a cut at a byte count may end inside one) */
void  cui_utf8_whole(char *s);
/* a text in drawing order (right-to-left letters turned), in one of a few rotating buffers */
const char *cui_order(const char *s);

/* A screen of the LK's own in the menubar pages' shape (the Link screens): the frame, the icon and
 * the title with the button hints at the right, a line, the subtitle, a list with the blue bar under
 * the focused row and each row's value at the right, a status wrapped across the page, and a bar of
 * how far something has come (the cursor's cyan). Texts are UTF-8 in the language in use (the
 * caller folds what the fonts lack); top is the frame's top in design units (648 tall). */
struct cui_sheet {
	int icon;                       /* KS_* */
	const char *title, *subtitle, *status;
	int nrows, sel;
	const char *rows[8], *values[8];
	int busy;                       /* something goes on: dots after the title, one lit at a time */
	unsigned int done, total;       /* the bar (total 0: none) */
	const char *progress;           /* the line under the bar ("1.2 MB of 4 MB") */
	int nhints;
	char hint_btn[4];               /* 'A', 'B' (anything else: the direction pad) */
	const char *hint_label[4];
	float clock;                    /* seconds, for what moves */
};
void  cui_sheet(float top, const struct cui_sheet *s);

#endif
