/*
 * rom_lzma.h - LZMA and LZMA2 streams unpacked into memory (the coders of the 7z archives a card
 * can hold its games in; rom_7z.c).
 */
#ifndef ROM_LZMA_H
#define ROM_LZMA_H

#include <stdint.h>

/* where the packed bytes come from: fill gives the next piece (a pointer and its length), 0 when
 * there is none (the stream was cut short) */
typedef struct {
	int (*fill)(void *ctx, const uint8_t **buf, uint32_t *len);
	void *ctx;
} rl_source;

/* Unpack an LZMA stream (props: the coder's 5 bytes, lc/lp/pb and the dictionary size) or an
 * LZMA2 stream (props: its 1 byte) into out, until want bytes are there. out is the dictionary
 * too, so it holds everything from the stream's start. probs is scratch for the model (see
 * rl_probs_size). tick, when set, is called now and then during a long one. Returns 0, -1 for
 * damaged or cut data, -2 for properties it does not take, -3 when probs is too small. */
int rl_lzma(const uint8_t *props, uint32_t nprops, rl_source *src, uint8_t *out, uint32_t want,
	    uint16_t *probs, uint32_t probs_cap, void (*tick)(void));
int rl_lzma2(const uint8_t *props, uint32_t nprops, rl_source *src, uint8_t *out, uint32_t want,
	     uint16_t *probs, uint32_t probs_cap, void (*tick)(void));

/* the model's size in probabilities for literal context lc + lp (both coders) */
uint32_t rl_probs_size(unsigned lc_plus_lp);

#endif
