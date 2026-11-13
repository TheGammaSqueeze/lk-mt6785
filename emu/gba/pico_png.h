/*
 * pico_png.h - a PNG picture turned into the menu's box art (.ART, gba_boxart.h).
 */
#ifndef PICO_PNG_H
#define PICO_PNG_H

/* Decode the PNG in png[0..len) (every colour type and bit depth, interlaced or not) and write
 * it as a GART file (RGB565) to out: shrunk so its larger side is at most `maxside` (a smaller
 * picture keeps its size), transparent parts laid over black. png[] is reused as it goes (its
 * image data is gathered in place). work is scratch memory: maxside * maxside * 16 bytes plus two
 * rows of the picture (up to 8 bytes a pixel); tick, when set, is called now and then during a
 * long decode. Returns the .ART's length, or negative: -1 not a PNG, -2 a form it does not take,
 * -3 damaged data, -4 too big for work or out. */
int pico_png_to_art(unsigned char *png, unsigned int len, int maxside,
		    unsigned char *work, unsigned int work_cap,
		    unsigned char *out, unsigned int out_cap, void (*tick)(void));

#endif
