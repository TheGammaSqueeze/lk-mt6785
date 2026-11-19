/*
 * pico_random.h - random numbers for the Pico's keys, nonces and protocol ids.
 *
 * The SoC's true random generator belongs to the secure world: the LK asks the trusted firmware
 * for each 32-bit word (the same call the boot code uses for the kernel's address seed). Words
 * that fail a quick health check (the call refused, all zero, all ones, a repeat) are not used;
 * the timer's jitter, hashed, stands in for them, so callers always get their bytes.
 */
#ifndef PICO_RANDOM_H
#define PICO_RANDOM_H

#include <stdint.h>

/* Fills out with len random bytes. Returns 1 when every word came from the hardware generator,
 * 0 when some had to come from the timer's jitter instead. */
int pico_random(void *out, unsigned int len);

/* One random word (protocol ids, sequence numbers, ports). */
uint32_t pico_random32(void);

/* For oem rnd: words read straight from the generator with each call's result code. */
void pico_random_probe(uint32_t *words, uint32_t *codes, unsigned int n);

/* Words taken from the hardware and words that fell back since boot. */
void pico_random_counts(unsigned int *hw, unsigned int *fallback);

#endif
