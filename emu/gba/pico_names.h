/*
 * pico_names.h - game names in any script for the Current theme's menus (pico_names.c).
 */
#ifndef PICO_NAMES_H
#define PICO_NAMES_H

#include <stdint.h>

/* a glyph as pico_current.c's fonts hold them: the code point, the offset from the pen at the
 * baseline, its place in the atlas and its advance in 1/64 px */
typedef struct { uint32_t code; int16_t xo, yo; uint16_t x, y, w, h; uint32_t adv; } pico_nglyph;

/* Read the glyphs the names (UTF-8, as the menus draw them: n of them, each stride bytes after the
 * last) use from /gammaos/names.pack, the ones not read yet. Cheap when nothing new is in them. */
void pico_names_prepare(const char *names, int stride, int n);
void pico_names_prepare_one(const char *name);

/* The glyphs read so far scaled for a font of px pixels (medium: the focused rows' weight, where
 * the pack has it): count, glyphs sorted by code point, atlas and its width; 0 when there are
 * none. Valid until the next pico_names_prepare that reads more. */
int pico_names_set(int px, int medium, const pico_nglyph **g, const uint8_t **atlas, int *aw);

/* Changes whenever more glyphs were read, so the fonts know to take their sets again. */
unsigned int pico_names_serial(void);

/* A name as the menus draw it, into out (cap bytes): Arabic letters joined, and a name with right
 * to left letters in drawing order (pico_bidi); any other name is returned as it is. */
const char *pico_names_label(const char *s, char *out, int cap);

#endif /* PICO_NAMES_H */
