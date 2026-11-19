/*
 * pico_random.c - see pico_random.h. GammaOS code.
 *
 * Every word from the hardware is also stirred into a small pool (SHA-256), and the pool is what
 * stands in when the hardware fails, together with fresh timer jitter: a bad word is never used
 * and the fallback still carries everything good seen before it.
 */
#include "pico_random.h"
#include "pico_crypto.h"
#include <string.h>

extern unsigned int get_rnd(unsigned int *rnd);	/* the trusted firmware's random word (app/mt_boot) */
extern unsigned int gpt4_get_current_tick(void);

static uint8_t s_pool[PICO_SHA256_LEN];
static uint32_t s_last, s_stir;
static unsigned int s_hw, s_fallback;

/* one word from the generator, 0 when the call refused or the word looks broken */
static int hw_word(uint32_t *w)
{
	unsigned int v = 0, rc;
	rc = get_rnd(&v);
	if (rc != 0 || v == 0 || v == 0xFFFFFFFFu || v == s_last) return 0;
	s_last = v;
	*w = v;
	return 1;
}

/* the pool, stirred with what is given and 64 timer reads; its next 32 bytes go to out */
static void pool_next(const void *add, unsigned int add_len, uint8_t out[PICO_SHA256_LEN])
{
	struct pico_sha256 h;
	unsigned int i, t;
	pico_sha256_start(&h);
	pico_sha256_add(&h, s_pool, sizeof s_pool);
	if (add_len) pico_sha256_add(&h, add, add_len);
	for (i = 0; i < 64; i++) {
		t = gpt4_get_current_tick();
		pico_sha256_add(&h, &t, sizeof t);
	}
	s_stir++;
	pico_sha256_add(&h, &s_stir, sizeof s_stir);
	pico_sha256_end(&h, s_pool);
	/* what goes out is a second hash of the pool, so the pool itself is never handed out */
	pico_hmac_sha256(s_pool, sizeof s_pool, "pico random out", 15, out);
}

int pico_random(void *out, unsigned int len)
{
	uint8_t *o = (uint8_t *)out, block[PICO_SHA256_LEN];
	uint32_t w;
	unsigned int k;
	int all_hw = 1;
	while (len) {
		k = len < 4 ? len : 4;
		if (hw_word(&w)) {
			s_hw++;
			memcpy(o, &w, k);
			if ((s_hw & 15) == 1) pool_next(&w, sizeof w, block);	/* keep the pool fed now and then */
		} else {
			s_fallback++;
			all_hw = 0;
			pool_next(0, 0, block);
			memcpy(o, block, k);
		}
		o += k;
		len -= k;
	}
	memset(block, 0, sizeof block);
	return all_hw;
}

uint32_t pico_random32(void)
{
	uint32_t w;
	pico_random(&w, sizeof w);
	return w;
}

void pico_random_probe(uint32_t *words, uint32_t *codes, unsigned int n)
{
	unsigned int i, v;
	for (i = 0; i < n; i++) {
		v = 0;
		codes[i] = get_rnd(&v);
		words[i] = v;
	}
}

void pico_random_counts(unsigned int *hw, unsigned int *fallback)
{
	*hw = s_hw;
	*fallback = s_fallback;
}
