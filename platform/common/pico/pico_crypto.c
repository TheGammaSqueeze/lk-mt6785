/*
 * pico_crypto.c - see pico_crypto.h. Plain C, from the standards; byte order handled by hand so
 * the result does not depend on the CPU.
 */
#include "pico_crypto.h"
#include <string.h>

void pico_wipe(void *p, size_t len)
{
	volatile uint8_t *b = (volatile uint8_t *)p;
	while (len--)
		*b++ = 0;
}

static uint32_t load_be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t rotl(uint32_t v, unsigned int n) { return v << n | v >> (32 - n); }

/* ---- SHA-1 ---- */

/* one 64 byte block into the state: 80 rounds over the expanded message words */
static void sha1_block(uint32_t st[5], const uint8_t *blk)
{
	uint32_t w[80], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f, k, t;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = load_be32(blk + 4 * i);
	for (; i < 80; i++)
		w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	for (i = 0; i < 80; i++) {
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
		else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
		t = rotl(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rotl(b, 30); b = a; a = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void pico_sha1_start(struct pico_sha1 *c)
{
	c->state[0] = 0x67452301u;
	c->state[1] = 0xEFCDAB89u;
	c->state[2] = 0x98BADCFEu;
	c->state[3] = 0x10325476u;
	c->state[4] = 0xC3D2E1F0u;
	c->total = 0;
	c->have = 0;
}

void pico_sha1_add(struct pico_sha1 *c, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	c->total += len;
	if (c->have) {
		size_t take = 64 - c->have;
		if (take > len) take = len;
		memcpy(c->block + c->have, p, take);
		c->have += (unsigned int)take;
		p += take;
		len -= take;
		if (c->have < 64)
			return;
		sha1_block(c->state, c->block);
		c->have = 0;
	}
	for (; len >= 64; p += 64, len -= 64)
		sha1_block(c->state, p);
	memcpy(c->block, p, len);
	c->have = (unsigned int)len;
}

void pico_sha1_end(struct pico_sha1 *c, uint8_t out[PICO_SHA1_LEN])
{
	uint64_t bits = c->total * 8;
	int i;
	c->block[c->have++] = 0x80;
	if (c->have > 56) {
		memset(c->block + c->have, 0, 64 - c->have);
		sha1_block(c->state, c->block);
		c->have = 0;
	}
	memset(c->block + c->have, 0, 56 - c->have);
	for (i = 0; i < 8; i++)
		c->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	sha1_block(c->state, c->block);
	for (i = 0; i < 5; i++)
		store_be32(out + 4 * i, c->state[i]);
	pico_wipe(c, sizeof *c);
}

/* ---- HMAC-SHA-1 ---- */

void pico_hmac_sha1_start(struct pico_hmac_sha1 *h, const void *key, size_t key_len)
{
	uint8_t pad[64];
	uint8_t short_key[PICO_SHA1_LEN];
	int i;
	if (key_len > 64) {			/* a long key is hashed first */
		struct pico_sha1 k;
		pico_sha1_start(&k);
		pico_sha1_add(&k, key, key_len);
		pico_sha1_end(&k, short_key);
		key = short_key;
		key_len = PICO_SHA1_LEN;
	}
	memset(pad, 0, sizeof pad);
	memcpy(pad, key, key_len);
	for (i = 0; i < 64; i++) pad[i] ^= 0x36;
	pico_sha1_start(&h->inner);
	pico_sha1_add(&h->inner, pad, 64);
	for (i = 0; i < 64; i++) pad[i] ^= 0x36 ^ 0x5C;
	pico_sha1_start(&h->outer);
	pico_sha1_add(&h->outer, pad, 64);
	pico_wipe(pad, sizeof pad);
	pico_wipe(short_key, sizeof short_key);
}

void pico_hmac_sha1_add(struct pico_hmac_sha1 *h, const void *data, size_t len)
{
	pico_sha1_add(&h->inner, data, len);
}

void pico_hmac_sha1_end(struct pico_hmac_sha1 *h, uint8_t out[PICO_SHA1_LEN])
{
	uint8_t in_code[PICO_SHA1_LEN];
	pico_sha1_end(&h->inner, in_code);
	pico_sha1_add(&h->outer, in_code, sizeof in_code);
	pico_sha1_end(&h->outer, out);
	pico_wipe(in_code, sizeof in_code);
}

void pico_hmac_sha1(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[PICO_SHA1_LEN])
{
	struct pico_hmac_sha1 h;
	pico_hmac_sha1_start(&h, key, key_len);
	pico_hmac_sha1_add(&h, data, len);
	pico_hmac_sha1_end(&h, out);
}

/* ---- PBKDF2 ---- */

void pico_pbkdf2_sha1(const void *pass, size_t pass_len, const void *salt, size_t salt_len,
		      unsigned int rounds, uint8_t *out, size_t out_len)
{
	struct pico_hmac_sha1 keyed, h;		/* keyed: the two pad blocks already taken, reused each round */
	uint8_t u[PICO_SHA1_LEN], acc[PICO_SHA1_LEN], num[4];
	uint32_t block = 1;
	unsigned int r, i;

	pico_hmac_sha1_start(&keyed, pass, pass_len);
	while (out_len) {
		size_t take = out_len < PICO_SHA1_LEN ? out_len : PICO_SHA1_LEN;
		store_be32(num, block);
		h = keyed;
		pico_hmac_sha1_add(&h, salt, salt_len);
		pico_hmac_sha1_add(&h, num, 4);
		pico_hmac_sha1_end(&h, u);
		memcpy(acc, u, sizeof acc);
		for (r = 1; r < rounds; r++) {
			h = keyed;
			pico_hmac_sha1_add(&h, u, sizeof u);
			pico_hmac_sha1_end(&h, u);
			for (i = 0; i < PICO_SHA1_LEN; i++)
				acc[i] ^= u[i];
		}
		memcpy(out, acc, take);
		out += take;
		out_len -= take;
		block++;
	}
	pico_wipe(&keyed, sizeof keyed);
	pico_wipe(u, sizeof u);
	pico_wipe(acc, sizeof acc);
}

/* ---- the 802.11 PRF ---- */

void pico_prf_sha1(const void *key, size_t key_len, const char *label, const void *data, size_t data_len,
		   uint8_t *out, size_t out_len)
{
	struct pico_hmac_sha1 h;
	uint8_t code[PICO_SHA1_LEN], zero = 0, i = 0;
	while (out_len) {
		size_t take = out_len < PICO_SHA1_LEN ? out_len : PICO_SHA1_LEN;
		pico_hmac_sha1_start(&h, key, key_len);
		pico_hmac_sha1_add(&h, label, strlen(label));
		pico_hmac_sha1_add(&h, &zero, 1);
		pico_hmac_sha1_add(&h, data, data_len);
		pico_hmac_sha1_add(&h, &i, 1);
		pico_hmac_sha1_end(&h, code);
		memcpy(out, code, take);
		out += take;
		out_len -= take;
		i++;
	}
	pico_wipe(code, sizeof code);
}

/* ---- AES-128 ---- */

/* the S-box and its inverse, built once from the field inverse and the affine map */
static uint8_t s_box[256], s_inv[256];
static int s_box_ready;

static uint8_t gmul(uint8_t a, uint8_t b)	/* multiplication in GF(2^8), x^8 + x^4 + x^3 + x + 1 */
{
	uint8_t r = 0;
	while (b) {
		if (b & 1) r ^= a;
		a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
		b >>= 1;
	}
	return r;
}

static void build_boxes(void)
{
	int x;
	for (x = 0; x < 256; x++) {
		uint8_t inv = 0, s;
		int y;
		if (x)
			for (y = 1; y < 256; y++)
				if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
		s = inv;
		s = (uint8_t)(s ^ (uint8_t)(inv << 1 | inv >> 7) ^ (uint8_t)(inv << 2 | inv >> 6) ^
			      (uint8_t)(inv << 3 | inv >> 5) ^ (uint8_t)(inv << 4 | inv >> 4) ^ 0x63);
		s_box[x] = s;
		s_inv[s] = (uint8_t)x;
	}
	s_box_ready = 1;
}

void pico_aes128_key(struct pico_aes128 *a, const uint8_t key[16])
{
	uint8_t rcon = 1;
	int i;
	if (!s_box_ready) build_boxes();
	for (i = 0; i < 4; i++)
		a->rk[i] = load_be32(key + 4 * i);
	for (i = 4; i < 44; i++) {
		uint32_t t = a->rk[i - 1];
		if (i % 4 == 0) {
			t = (uint32_t)s_box[(t >> 16) & 0xFF] << 24 | (uint32_t)s_box[(t >> 8) & 0xFF] << 16 |
			    (uint32_t)s_box[t & 0xFF] << 8 | s_box[t >> 24];
			t ^= (uint32_t)rcon << 24;
			rcon = gmul(rcon, 2);
		}
		a->rk[i] = a->rk[i - 4] ^ t;
	}
}

static void add_round_key(uint8_t s[16], const uint32_t *rk)
{
	int c;
	for (c = 0; c < 4; c++) {
		s[4 * c] ^= (uint8_t)(rk[c] >> 24);
		s[4 * c + 1] ^= (uint8_t)(rk[c] >> 16);
		s[4 * c + 2] ^= (uint8_t)(rk[c] >> 8);
		s[4 * c + 3] ^= (uint8_t)rk[c];
	}
}

/* the state is column after column: byte 4c + r is row r of column c */
static void shift_rows(uint8_t s[16], int back)
{
	uint8_t t[16];
	int r, c;
	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
			t[4 * c + r] = s[4 * ((back ? c - r + 4 : c + r) % 4) + r];
	memcpy(s, t, 16);
}

static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

static void mix_columns(uint8_t s[16], int back)
{
	if (!back) {		/* forward, the hot path: 2a + 3b + c + d per row, with xtime only */
		int c;
		for (c = 0; c < 4; c++) {
			uint8_t *col = s + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
			uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
			col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
			col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
			col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
			col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
		}
		return;
	}
	{
	static const uint8_t fwd[4] = { 2, 3, 1, 1 }, inv[4] = { 14, 11, 13, 9 };
	const uint8_t *m = back ? inv : fwd;
	int c, r;
	for (c = 0; c < 4; c++) {
		uint8_t col[4];
		memcpy(col, s + 4 * c, 4);
		for (r = 0; r < 4; r++)
			s[4 * c + r] = (uint8_t)(gmul(col[0], m[(4 - r) % 4]) ^ gmul(col[1], m[(5 - r) % 4]) ^
						 gmul(col[2], m[(6 - r) % 4]) ^ gmul(col[3], m[(7 - r) % 4]));
	}
	}
}

void pico_aes128_encrypt(const struct pico_aes128 *a, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	int round, i;
	memcpy(s, in, 16);
	add_round_key(s, a->rk);
	for (round = 1; round <= 10; round++) {
		for (i = 0; i < 16; i++) s[i] = s_box[s[i]];
		shift_rows(s, 0);
		if (round < 10) mix_columns(s, 0);
		add_round_key(s, a->rk + 4 * round);
	}
	memcpy(out, s, 16);
	pico_wipe(s, sizeof s);
}

void pico_aes128_decrypt(const struct pico_aes128 *a, const uint8_t in[16], uint8_t out[16])
{
	uint8_t s[16];
	int round, i;
	memcpy(s, in, 16);
	add_round_key(s, a->rk + 40);
	for (round = 9; round >= 0; round--) {
		shift_rows(s, 1);
		for (i = 0; i < 16; i++) s[i] = s_inv[s[i]];
		add_round_key(s, a->rk + 4 * round);
		if (round > 0) mix_columns(s, 1);
	}
	memcpy(out, s, 16);
	pico_wipe(s, sizeof s);
}

/* ---- RFC 3394 key unwrapping ---- */

int pico_aes128_unwrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out)
{
	struct pico_aes128 a;
	uint8_t chk[8], b[16];
	size_t n, i;
	int j, k, bad = 0;
	if (in_len < 24 || in_len % 8)
		return -1;
	n = in_len / 8 - 1;
	pico_aes128_key(&a, kek);
	memcpy(chk, in, 8);
	memcpy(out, in + 8, in_len - 8);
	for (j = 5; j >= 0; j--) {
		for (i = n; i >= 1; i--) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;
			memcpy(b, chk, 8);
			for (k = 0; k < 8; k++)
				b[7 - k] ^= (uint8_t)(t >> (8 * k));
			memcpy(b + 8, out + 8 * (i - 1), 8);
			pico_aes128_decrypt(&a, b, b);
			memcpy(chk, b, 8);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	for (k = 0; k < 8; k++)
		bad |= chk[k] ^ 0xA6;
	pico_wipe(&a, sizeof a);
	pico_wipe(b, sizeof b);
	return bad ? -1 : 0;
}

/* ======================= for TLS 1.3 ======================= */

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t k256[64] = {	/* the first 32 bits of the cube roots' fractions of the first 64 primes */
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr(uint32_t v, unsigned int n) { return v >> n | v << (32 - n); }

static void sha256_block(uint32_t st[8], const uint8_t *blk)
{
	uint32_t w[64], v[8], t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = load_be32(blk + 4 * i);
	for (; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, st, sizeof v);
	for (i = 0; i < 64; i++) {
		t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k256[i] + w[i];
		t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
		v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		st[i] += v[i];
}

void pico_sha256_start(struct pico_sha256 *c)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
					0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(c->state, iv, sizeof iv);
	c->total = 0;
	c->have = 0;
}

void pico_sha256_add(struct pico_sha256 *c, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	c->total += len;
	if (c->have) {
		size_t take = 64 - c->have;
		if (take > len) take = len;
		memcpy(c->block + c->have, p, take);
		c->have += (unsigned int)take;
		p += take;
		len -= take;
		if (c->have < 64)
			return;
		sha256_block(c->state, c->block);
		c->have = 0;
	}
	for (; len >= 64; p += 64, len -= 64)
		sha256_block(c->state, p);
	memcpy(c->block, p, len);
	c->have = (unsigned int)len;
}

void pico_sha256_end(struct pico_sha256 *c, uint8_t out[PICO_SHA256_LEN])
{
	uint64_t bits = c->total * 8;
	int i;
	c->block[c->have++] = 0x80;
	if (c->have > 56) {
		memset(c->block + c->have, 0, 64 - c->have);
		sha256_block(c->state, c->block);
		c->have = 0;
	}
	memset(c->block + c->have, 0, 56 - c->have);
	for (i = 0; i < 8; i++)
		c->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	sha256_block(c->state, c->block);
	for (i = 0; i < 8; i++)
		store_be32(out + 4 * i, c->state[i]);
	pico_wipe(c, sizeof *c);
}

void pico_sha256_peek(const struct pico_sha256 *c, uint8_t out[PICO_SHA256_LEN])
{
	struct pico_sha256 copy = *c;
	pico_sha256_end(&copy, out);
}

void pico_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[PICO_SHA256_LEN])
{
	struct pico_sha256 h;
	uint8_t pad[64], k[PICO_SHA256_LEN], inner[PICO_SHA256_LEN];
	int i;
	if (key_len > 64) {
		pico_sha256_start(&h);
		pico_sha256_add(&h, key, key_len);
		pico_sha256_end(&h, k);
		key = k;
		key_len = sizeof k;
	}
	memset(pad, 0, sizeof pad);
	memcpy(pad, key, key_len);
	for (i = 0; i < 64; i++) pad[i] ^= 0x36;
	pico_sha256_start(&h);
	pico_sha256_add(&h, pad, 64);
	pico_sha256_add(&h, data, len);
	pico_sha256_end(&h, inner);
	for (i = 0; i < 64; i++) pad[i] ^= 0x36 ^ 0x5C;
	pico_sha256_start(&h);
	pico_sha256_add(&h, pad, 64);
	pico_sha256_add(&h, inner, sizeof inner);
	pico_sha256_end(&h, out);
	pico_wipe(pad, sizeof pad);
	pico_wipe(k, sizeof k);
	pico_wipe(inner, sizeof inner);
}

/* ---- HKDF (RFC 5869) ---- */

void pico_hkdf_extract(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len, uint8_t prk[32])
{
	static const uint8_t zeros[32];
	if (!salt || !salt_len) { salt = zeros; salt_len = sizeof zeros; }
	pico_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

void pico_hkdf_expand(const uint8_t prk[32], const void *info, size_t info_len, uint8_t *out, size_t out_len)
{
	uint8_t t[PICO_SHA256_LEN + 256 + 1], block[PICO_SHA256_LEN];
	size_t tl = 0;
	uint8_t i = 1;
	if (info_len > 256) return;
	while (out_len) {
		size_t take = out_len < PICO_SHA256_LEN ? out_len : PICO_SHA256_LEN;
		/* T(i) = HMAC(PRK, T(i-1) | info | i) */
		memcpy(t + tl, info, info_len);
		t[tl + info_len] = i;
		pico_hmac_sha256(prk, 32, t, tl + info_len + 1, block);
		memcpy(out, block, take);
		memcpy(t, block, PICO_SHA256_LEN);
		tl = PICO_SHA256_LEN;
		out += take;
		out_len -= take;
		i++;
	}
	pico_wipe(t, sizeof t);
	pico_wipe(block, sizeof block);
}

/* ---- AES-128-GCM (SP 800-38D) ---- */

/* a field element of GF(2^128) as GCM orders its bits: hi holds bytes 0..7 */
struct gf128 { uint64_t hi, lo; };

static struct gf128 gf_load(const uint8_t *p)
{
	struct gf128 v;
	v.hi = (uint64_t)load_be32(p) << 32 | load_be32(p + 4);
	v.lo = (uint64_t)load_be32(p + 8) << 32 | load_be32(p + 12);
	return v;
}

static void gf_store(uint8_t *p, struct gf128 v)
{
	store_be32(p, (uint32_t)(v.hi >> 32)); store_be32(p + 4, (uint32_t)v.hi);
	store_be32(p + 8, (uint32_t)(v.lo >> 32)); store_be32(p + 12, (uint32_t)v.lo);
}

/* times x: one step right in GCM's bit order, the bit that falls off folded back with
 * x^128 = x^7 + x^2 + x + 1 (0xE1 at the top) */
static struct gf128 gf_times_x(struct gf128 v)
{
	uint64_t carry = v.lo & 1;
	v.lo = v.lo >> 1 | v.hi << 63;
	v.hi >>= 1;
	if (carry) v.hi ^= 0xE1ull << 56;
	return v;
}

/* GHASH state: the key's multiples for every 4 bit pattern, and the running value */
struct ghash {
	struct gf128 m[16];
	struct gf128 y;
};

static void ghash_start(struct ghash *g, const struct pico_aes128 *a)
{
	uint8_t zero[16] = { 0 }, hb[16];
	int i, j;
	pico_aes128_encrypt(a, zero, hb);
	/* m[8] = H, m[4] = H.x, m[2] = H.x^2, m[1] = H.x^3; the rest are sums of those */
	g->m[0].hi = g->m[0].lo = 0;
	g->m[8] = gf_load(hb);
	g->m[4] = gf_times_x(g->m[8]);
	g->m[2] = gf_times_x(g->m[4]);
	g->m[1] = gf_times_x(g->m[2]);
	for (i = 2; i < 16; i <<= 1)
		for (j = 1; j < i; j++) {
			g->m[i + j].hi = g->m[i].hi ^ g->m[j].hi;
			g->m[i + j].lo = g->m[i].lo ^ g->m[j].lo;
		}
	g->y.hi = g->y.lo = 0;
	pico_wipe(hb, sizeof hb);
}

/* y = (y + block) . H, four bits of the block at a time from its highest power of x down */
static void ghash_block(struct ghash *g, const uint8_t blk[16])
{
	uint8_t x[16];
	struct gf128 z = { 0, 0 };
	int i, k;
	gf_store(x, g->y);
	for (i = 0; i < 16; i++) x[i] ^= blk[i];
	for (i = 15; i >= 0; i--) {
		unsigned int nib[2];
		nib[0] = x[i] & 15u;
		nib[1] = x[i] >> 4;
		for (k = 0; k < 2; k++) {
			z = gf_times_x(gf_times_x(gf_times_x(gf_times_x(z))));
			z.hi ^= g->m[nib[k]].hi;
			z.lo ^= g->m[nib[k]].lo;
		}
	}
	g->y = z;
}

static void ghash_add(struct ghash *g, const uint8_t *p, size_t len)
{
	uint8_t blk[16];
	for (; len >= 16; p += 16, len -= 16)
		ghash_block(g, p);
	if (len) {
		memset(blk, 0, sizeof blk);
		memcpy(blk, p, len);
		ghash_block(g, blk);
	}
}

/* counter mode from the counter block ctr (its last 32 bits count up) */
static void gcm_ctr(const struct pico_aes128 *a, uint8_t ctr[16], const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t ks[16];
	size_t i;
	while (len) {
		uint32_t c = load_be32(ctr + 12) + 1;
		size_t take = len < 16 ? len : 16;
		store_be32(ctr + 12, c);
		pico_aes128_encrypt(a, ctr, ks);
		for (i = 0; i < take; i++)
			out[i] = in[i] ^ ks[i];
		in += take;
		out += take;
		len -= take;
	}
	pico_wipe(ks, sizeof ks);
}

static void gcm_tag(const struct pico_aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		    const uint8_t *ct, size_t len, uint8_t tag[16])
{
	struct ghash g;
	uint8_t lens[16], j0[16], ek[16];
	int i;
	ghash_start(&g, a);
	ghash_add(&g, aad, aad_len);
	ghash_add(&g, ct, len);
	store_be32(lens, (uint32_t)((uint64_t)aad_len * 8 >> 32));
	store_be32(lens + 4, (uint32_t)((uint64_t)aad_len * 8));
	store_be32(lens + 8, (uint32_t)((uint64_t)len * 8 >> 32));
	store_be32(lens + 12, (uint32_t)((uint64_t)len * 8));
	ghash_block(&g, lens);
	memcpy(j0, nonce, 12);
	store_be32(j0 + 12, 1);
	pico_aes128_encrypt(a, j0, ek);
	gf_store(tag, g.y);
	for (i = 0; i < 16; i++) tag[i] ^= ek[i];
	pico_wipe(&g, sizeof g);
}

void pico_gcm128_seal(const struct pico_aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		      const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16])
{
	uint8_t ctr[16];
	memcpy(ctr, nonce, 12);
	store_be32(ctr + 12, 1);
	gcm_ctr(a, ctr, in, len, out);
	gcm_tag(a, nonce, aad, aad_len, out, len, tag);
}

int pico_gcm128_open(const struct pico_aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		     const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16])
{
	uint8_t want[16], ctr[16], diff = 0;
	int i;
	gcm_tag(a, nonce, aad, aad_len, in, len, want);
	for (i = 0; i < 16; i++) diff |= want[i] ^ tag[i];
	if (diff) return -1;
	memcpy(ctr, nonce, 12);
	store_be32(ctr + 12, 1);
	gcm_ctr(a, ctr, in, len, out);
	return 0;
}

/* ---- X25519 (RFC 7748) ----
 * A number mod p = 2^255 - 19 as sixteen 16 bit limbs (least first) held in 64 bit signed
 * words, so sums and products have room before their carries are passed on. 2^256 = 38 mod p. */
typedef int64_t fe[16];

static void fe_carry(fe a)
{
	int i;
	int64_t c;
	for (i = 0; i < 16; i++) {
		c = a[i] >> 16;				/* floor, also for a negative limb */
		a[i] -= c << 16;
		if (i < 15) a[i + 1] += c;
		else a[0] += 38 * c;
	}
}

static void fe_add(fe o, const fe a, const fe b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fe_sub(fe o, const fe a, const fe b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void fe_mul(fe o, const fe a, const fe b)
{
	int64_t t[31];
	int i, j;
	memset(t, 0, sizeof t);
	for (i = 0; i < 16; i++)
		for (j = 0; j < 16; j++)
			t[i + j] += a[i] * b[j];
	for (i = 16; i < 31; i++)			/* limb 16+k is worth 2^256 . 2^(16k) = 38 . 2^(16k) */
		t[i - 16] += 38 * t[i];
	for (i = 0; i < 16; i++) o[i] = t[i];
	fe_carry(o);
	fe_carry(o);
}

static void fe_sq(fe o, const fe a) { fe_mul(o, a, a); }

/* swap a and b when bit is 1, with no branch on the bit */
static void fe_swap(fe a, fe b, int bit)
{
	int64_t m = -(int64_t)bit, t;
	int i;
	for (i = 0; i < 16; i++) {
		t = m & (a[i] ^ b[i]);
		a[i] ^= t;
		b[i] ^= t;
	}
}

/* 1/a = a^(p-2): p-2 has every bit set from 254 down to 0 except bits 2 and 4 */
static void fe_inv(fe o, const fe a)
{
	fe c;
	int i;
	memcpy(c, a, sizeof c);
	for (i = 253; i >= 0; i--) {
		fe_sq(c, c);
		if (i != 2 && i != 4) fe_mul(c, c, a);
	}
	memcpy(o, c, sizeof c);
}

static void fe_load(fe o, const uint8_t in[32])
{
	int i;
	for (i = 0; i < 16; i++) o[i] = in[2 * i] | (int64_t)in[2 * i + 1] << 8;
	o[15] &= 0x7FFF;				/* the top bit is not part of a point */
}

/* fully reduced into 32 bytes */
static void fe_store(uint8_t out[32], const fe a)
{
	fe t, m;
	int i, k, borrow;
	memcpy(t, a, sizeof t);
	fe_carry(t); fe_carry(t); fe_carry(t);
	for (k = 0; k < 2; k++) {			/* t - p, kept when it does not go below zero */
		m[0] = t[0] - 0xFFED;
		for (i = 1; i < 15; i++) {
			m[i] = t[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xFFFF;
		}
		m[15] = t[15] - 0x7FFF - ((m[14] >> 16) & 1);
		m[14] &= 0xFFFF;
		borrow = (int)((m[15] >> 16) & 1);
		fe_swap(t, m, 1 - borrow);
	}
	for (i = 0; i < 16; i++) {
		out[2 * i] = (uint8_t)t[i];
		out[2 * i + 1] = (uint8_t)(t[i] >> 8);
	}
}

void pico_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
	uint8_t k[32];
	fe x1, x2, z2, x3, z3, a, aa, b, bb, e, c, d, da, cb, t;
	static const fe a24 = { 0xDB41, 1 };		/* 121665 */
	int i, bit, swap = 0;
	memcpy(k, scalar, 32);
	k[0] &= 248; k[31] &= 127; k[31] |= 64;
	fe_load(x1, point);
	memset(x2, 0, sizeof x2); x2[0] = 1;
	memset(z2, 0, sizeof z2);
	memcpy(x3, x1, sizeof x3);
	memset(z3, 0, sizeof z3); z3[0] = 1;
	for (i = 254; i >= 0; i--) {
		bit = (k[i >> 3] >> (i & 7)) & 1;
		swap ^= bit;
		fe_swap(x2, x3, swap);
		fe_swap(z2, z3, swap);
		swap = bit;
		fe_add(a, x2, z2);
		fe_sq(aa, a);
		fe_sub(b, x2, z2);
		fe_sq(bb, b);
		fe_sub(e, aa, bb);
		fe_add(c, x3, z3);
		fe_sub(d, x3, z3);
		fe_mul(da, d, a);
		fe_mul(cb, c, b);
		fe_add(t, da, cb);
		fe_sq(x3, t);
		fe_sub(t, da, cb);
		fe_sq(t, t);
		fe_mul(z3, x1, t);
		fe_mul(x2, aa, bb);
		fe_mul(t, a24, e);
		fe_add(t, aa, t);
		fe_mul(z2, e, t);
	}
	fe_swap(x2, x3, swap);
	fe_swap(z2, z3, swap);
	fe_inv(z2, z2);
	fe_mul(x2, x2, z2);
	fe_store(out, x2);
	pico_wipe(k, sizeof k);
}

void pico_x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
	static const uint8_t nine[32] = { 9 };
	pico_x25519(out, scalar, nine);
}

/* ---- SHA-512 / SHA-384 (FIPS 180-4) ---- */

static const uint64_t k512[80] = {	/* the first 64 bits of the cube roots' fractions of the first 80 primes */
	0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
	0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
	0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
	0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
	0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
	0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
	0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
	0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
	0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
	0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
	0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
	0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
	0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
	0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
	0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
	0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
	0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
	0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
	0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
	0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
};

static uint64_t rotr64(uint64_t v, unsigned int n) { return v >> n | v << (64 - n); }
static uint64_t load_be64(const uint8_t *p) { return (uint64_t)load_be32(p) << 32 | load_be32(p + 4); }

static void sha512_block(uint64_t st[8], const uint8_t *blk)
{
	uint64_t w[80], v[8], t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = load_be64(blk + 8 * i);
	for (; i < 80; i++) {
		uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
		uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	memcpy(v, st, sizeof v);
	for (i = 0; i < 80; i++) {
		t1 = v[7] + (rotr64(v[4], 14) ^ rotr64(v[4], 18) ^ rotr64(v[4], 41)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k512[i] + w[i];
		t2 = (rotr64(v[0], 28) ^ rotr64(v[0], 34) ^ rotr64(v[0], 39)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
		v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
		v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
	}
	for (i = 0; i < 8; i++)
		st[i] += v[i];
}

void pico_sha512_start(struct pico_sha512 *c)
{
	static const uint64_t iv[8] = {
		0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
		0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull };
	memcpy(c->state, iv, sizeof iv);
	c->total = 0;
	c->have = 0;
	c->out_len = 64;
}

void pico_sha384_start(struct pico_sha512 *c)
{
	static const uint64_t iv[8] = {
		0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
		0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };
	memcpy(c->state, iv, sizeof iv);
	c->total = 0;
	c->have = 0;
	c->out_len = 48;
}

void pico_sha512_add(struct pico_sha512 *c, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	c->total += len;
	if (c->have) {
		size_t take = 128 - c->have;
		if (take > len) take = len;
		memcpy(c->block + c->have, p, take);
		c->have += (unsigned int)take;
		p += take;
		len -= take;
		if (c->have < 128)
			return;
		sha512_block(c->state, c->block);
		c->have = 0;
	}
	for (; len >= 128; p += 128, len -= 128)
		sha512_block(c->state, p);
	memcpy(c->block, p, len);
	c->have = (unsigned int)len;
}

void pico_sha512_end(struct pico_sha512 *c, uint8_t *out)
{
	uint64_t bits = c->total * 8;
	unsigned int i;
	c->block[c->have++] = 0x80;
	if (c->have > 112) {
		memset(c->block + c->have, 0, 128 - c->have);
		sha512_block(c->state, c->block);
		c->have = 0;
	}
	memset(c->block + c->have, 0, 128 - c->have);	/* the length's top 64 bits stay 0 */
	for (i = 0; i < 8; i++)
		c->block[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
	sha512_block(c->state, c->block);
	for (i = 0; i < c->out_len; i++)
		out[i] = (uint8_t)(c->state[i / 8] >> (56 - 8 * (i % 8)));
	pico_wipe(c, sizeof *c);
}
