/*
 * pico_fold.h - games' names in UTF-8 compared and shown without their accents.
 *
 * A Latin letter with a diacritic folds to its plain ASCII letter (U+00E9 to 'e', U+0141 to
 * 'L'), so "Pokemon" and the same name with an acute accent sort together, and a menu whose
 * fonts hold only ASCII can still show the name. Names macOS writes come decomposed (the letter,
 * then a combining mark U+0300..U+036F): the marks fold to nothing, so both spellings come out
 * the same.
 */
#ifndef PICO_FOLD_H
#define PICO_FOLD_H

#include <stdint.h>

/* the next code point of UTF-8 text, moving past it (a byte that does not start a whole UTF-8
 * character reads as itself, Latin-1) */
uint32_t pico_utf8_next(const char **ps);

/* the plain ASCII letter of a Latin letter with a diacritic, case kept; the first letter of a
 * ligature or a letter like ae, oe, sharp s or thorn; fullwidth ASCII as ASCII; 0 for a
 * combining mark (U+0300..U+036F: it is dropped); any other code point as it is */
uint32_t pico_fold_cp(uint32_t cp);

/* an ASCII rendition of a UTF-8 name for menus without the glyphs: the folded letters, every
 * other character outside ASCII as one '_'; out always ends in a 0. Returns the length. */
int pico_fold_ascii(const char *utf8, char *out, int cap);

/* the order of two UTF-8 names: by their folded code points, ASCII letters in either case alike;
 * any character outside ASCII that does not fold comes after all of ASCII, by code point.
 * Negative, 0 or positive. */
int pico_fold_cmp(const char *a, const char *b);

#endif
