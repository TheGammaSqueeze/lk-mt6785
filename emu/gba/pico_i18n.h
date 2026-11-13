/*
 * pico_i18n.h - the menus' languages. The words are English in the code; a language's pack on the
 * card (/gammaos/lang-<code>-<W>x<H>.pack, tools/ayaneo/pico_current/build_lang.py) holds what
 * each one reads in that language, keyed by a hash of the English, and the glyphs the menu fonts
 * lack for it. The pack is read once into memory of its own and kept for the whole power on, so
 * the in-game menu finds it with a game running.
 */
#ifndef PICO_I18N_H
#define PICO_I18N_H

#include <stdint.h>

/* The order is the saved setting's: never reorder, only add at the end. The first eight are the
 * classic theme's Language page in its own order (language01..08). */
enum { LANG_EN, LANG_FR, LANG_DE, LANG_ES, LANG_IT, LANG_NL, LANG_PT, LANG_RU, LANG_JA, LANG_KO,
       LANG_ZH_CN, LANG_ZH_TW, LANG_PL, LANG_TR, LANG_AR, LANG_HE, LANG_FA, LANG_N };

const char *pico_lang_code(int lang);   /* "en", "fr", ... "zh-cn" */
const char *pico_lang_name(int lang);   /* the language's name in itself (UTF-8, as drawn) */
int  pico_lang(void);                   /* the saved setting */
void pico_lang_set(int lang);           /* change it (the caller persists the settings) */

/* Reads the pack of the language set now for this screen size, unless it is already in memory.
 * English needs none. 0 when the words and glyphs are ready, else the menus stay English. */
int  pico_lang_load(unsigned W, unsigned H);

/* The text in the language in use, or en itself when it has none (or the pack is not there). */
const char *tr(const char *en);
/* A count's words: one is the English for 1 ("%d game"), other for the rest ("%d games"); the
 * language picks its own form for n (Russian and Polish have three, Arabic six). */
const char *tr_n(int n, const char *one, const char *other);

/* For the words the cores put together for their in-game menu rows (a battery's time left, a
 * run-ahead tier): translated only while the Current theme's in-game menu draws, which sets the
 * scope, since the cores' older panels draw the same rows in the 8x16 byte font, English only. */
const char *tr_igm(const char *en);
void pico_tr_igm_scope(int on);

/* A pack entry by name and type (3 a font), or 0: the menu fonts' extra glyphs. */
const uint8_t *pico_lang_entry(const char *name, int type, uint32_t *len);
/* Changes each time another pack is read, so the fonts know to look their extra glyphs up again. */
unsigned int pico_lang_serial(void);

/* A text with right-to-left letters (Hebrew, Arabic) in the order it is drawn, left to right, into
 * out (cap bytes); any other text is returned as it is. */
const char *pico_bidi(const char *s, char *out, int cap);

/* Capital of a letter for headings drawn in capitals: Latin, Cyrillic, and Turkish's dotted and
 * dotless i when the language is Turkish. */
uint32_t pico_upper(uint32_t c);

#endif
