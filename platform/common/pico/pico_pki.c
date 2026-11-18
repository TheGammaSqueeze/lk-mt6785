/*
 * pico_pki.c - see pico_pki.h. From RFC 8017 (RSA), X.690 (DER) and RFC 5280 (X.509).
 */
#include "pico_pki.h"
#include "pico_crypto.h"
#include <string.h>

/* ---- hashes ---- */

unsigned int pki_hash_len(int hash)
{
	return hash == PKI_HASH_SHA256 ? 32 : hash == PKI_HASH_SHA384 ? 48 : hash == PKI_HASH_SHA512 ? 64 : 0;
}

void pki_hash(int hash, const uint8_t *data, size_t len, uint8_t *out)
{
	if (hash == PKI_HASH_SHA256) {
		struct pico_sha256 h;
		pico_sha256_start(&h);
		pico_sha256_add(&h, data, len);
		pico_sha256_end(&h, out);
	} else {
		struct pico_sha512 h;
		if (hash == PKI_HASH_SHA384) pico_sha384_start(&h); else pico_sha512_start(&h);
		pico_sha512_add(&h, data, len);
		pico_sha512_end(&h, out);
	}
}

/* ---- big numbers: 32 bit limbs, least significant first ---- */

#define BN_LIMBS	129			/* 4096 bits and one spare */

/* big-endian bytes into k limbs (0 when they do not fit) */
static int bn_load(uint32_t *a, unsigned int k, const uint8_t *b, size_t len)
{
	size_t i;
	while (len && !*b) { b++; len--; }
	if (len > 4u * k) return 0;
	memset(a, 0, 4u * k);
	for (i = 0; i < len; i++)
		a[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
	return 1;
}

static int bn_cmp(const uint32_t *a, const uint32_t *b, unsigned int k)
{
	while (k--) {
		if (a[k] != b[k]) return a[k] > b[k] ? 1 : -1;
	}
	return 0;
}

/* a -= b, the borrow out */
static uint32_t bn_sub(uint32_t *a, const uint32_t *b, unsigned int k)
{
	uint64_t borrow = 0;
	unsigned int i;
	for (i = 0; i < k; i++) {
		uint64_t d = (uint64_t)a[i] - b[i] - borrow;
		a[i] = (uint32_t)d;
		borrow = (d >> 32) & 1;
	}
	return (uint32_t)borrow;
}

/* Montgomery product r = a.b / 2^(32k) mod n (n0 = -1/n mod 2^32), operand scanning */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *n, unsigned int k, uint32_t n0)
{
	uint32_t t[BN_LIMBS + 2];
	unsigned int i, j;
	memset(t, 0, 4u * (k + 2));
	for (i = 0; i < k; i++) {
		uint64_t c = 0, s;
		uint32_t m;
		for (j = 0; j < k; j++) {
			s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + c;
			t[j] = (uint32_t)s;
			c = s >> 32;
		}
		s = (uint64_t)t[k] + c;
		t[k] = (uint32_t)s;
		t[k + 1] = (uint32_t)(s >> 32);
		m = t[0] * n0;				/* makes the lowest limb vanish */
		s = (uint64_t)t[0] + (uint64_t)m * n[0];
		c = s >> 32;
		for (j = 1; j < k; j++) {
			s = (uint64_t)t[j] + (uint64_t)m * n[j] + c;
			t[j - 1] = (uint32_t)s;
			c = s >> 32;
		}
		s = (uint64_t)t[k] + c;
		t[k - 1] = (uint32_t)s;
		t[k] = t[k + 1] + (uint32_t)(s >> 32);
	}
	if (t[k] || bn_cmp(t, n, k) >= 0)
		bn_sub(t, n, k);
	memcpy(r, t, 4u * k);
}

/* out = s^e mod n, as n_len big-endian bytes: 0, -1 when s is not below n */
static int rsa_public(const uint8_t *nb, size_t n_len, const uint8_t *eb, size_t e_len, const uint8_t *sig,
		      size_t sig_len, uint8_t *out)
{
	static uint32_t n[BN_LIMBS], s[BN_LIMBS], r2[BN_LIMBS], x[BN_LIMBS], acc[BN_LIMBS], one[BN_LIMBS];
	unsigned int k, i, top;
	uint32_t n0, inv;
	int bit;
	size_t ei;
	while (n_len && !*nb) { nb++; n_len--; }
	k = (unsigned int)((n_len + 3) / 4);
	if (!k || k > BN_LIMBS - 1 || !(nb[n_len - 1] & 1)) return -1;	/* RSA moduli are odd */
	if (!bn_load(n, k, nb, n_len) || !bn_load(s, k, sig, sig_len) || bn_cmp(s, n, k) >= 0) return -1;
	/* -1/n mod 2^32 by Newton's steps (each doubles the bits right) */
	inv = n[0];
	for (i = 0; i < 5; i++) inv *= 2u - n[0] * inv;
	n0 = 0u - inv;
	/* R^2 mod n with R = 2^(32k): 1 doubled 64k times */
	memset(r2, 0, 4u * k);
	r2[0] = 1;
	for (i = 0; i < 64u * k; i++) {
		uint32_t carry = 0;
		unsigned int j;
		for (j = 0; j < k; j++) {
			uint32_t v = r2[j];
			r2[j] = v << 1 | carry;
			carry = v >> 31;
		}
		if (carry || bn_cmp(r2, n, k) >= 0) bn_sub(r2, n, k);
	}
	mont_mul(x, s, r2, n, k, n0);			/* s in Montgomery form */
	memcpy(acc, x, 4u * k);
	while (e_len && !*eb) { eb++; e_len--; }
	if (!e_len) return -1;
	top = 8u * (unsigned int)e_len - 1;
	while (!(eb[0] & (0x80 >> (7 - top % 8)))) top--;	/* the highest set bit of e */
	for (bit = (int)top - 1; bit >= 0; bit--) {
		ei = e_len - 1 - (size_t)bit / 8;
		mont_mul(acc, acc, acc, n, k, n0);
		if (eb[ei] & (1u << (bit % 8)))
			mont_mul(acc, acc, x, n, k, n0);
	}
	memset(one, 0, 4u * k);
	one[0] = 1;
	mont_mul(acc, acc, one, n, k, n0);		/* out of Montgomery form */
	for (i = 0; i < n_len; i++)
		out[n_len - 1 - i] = (uint8_t)(acc[i / 4] >> (8 * (i % 4)));
	return 0;
}

int pki_rsa_public(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *in, size_t in_len,
		   uint8_t *out)
{
	while (n_len && !*n) { n++; n_len--; }
	return rsa_public(n, n_len, e, e_len, in, in_len, out);
}

/* the DER DigestInfo before the hash (RFC 8017 9.2 note 1) */
static const uint8_t *digest_info(int hash, size_t *len)
{
	static const uint8_t sha256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
	static const uint8_t sha384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
	static const uint8_t sha512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
	*len = sizeof sha256;
	return hash == PKI_HASH_SHA256 ? sha256 : hash == PKI_HASH_SHA384 ? sha384 : sha512;
}

static uint8_t s_em[512];

int pki_rsa_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
		  size_t sig_len, int hash, const uint8_t *digest)
{
	size_t di_len, hl = pki_hash_len(hash), i, pad;
	const uint8_t *di;
	while (n_len && !*n) { n++; n_len--; }
	if (!hl || n_len > sizeof s_em || rsa_public(n, n_len, e, e_len, sig, sig_len, s_em) != 0) return -1;
	di = digest_info(hash, &di_len);
	if (n_len < 11 + di_len + hl) return -1;
	/* 00 01 FF .. FF 00 DigestInfo hash */
	pad = n_len - 3 - di_len - hl;
	if (s_em[0] != 0 || s_em[1] != 1) return -1;
	for (i = 0; i < pad; i++)
		if (s_em[2 + i] != 0xFF) return -1;
	if (s_em[2 + pad] != 0) return -1;
	if (memcmp(s_em + 3 + pad, di, di_len) || memcmp(s_em + 3 + pad + di_len, digest, hl)) return -1;
	return 0;
}

int pki_rsa_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
		size_t sig_len, int hash, const uint8_t *digest)
{
	static uint8_t db[512], mprime[8 + 64 + 512];
	size_t hl = pki_hash_len(hash), mod_bits, em_bits, em_len, db_len, i, ps;
	const uint8_t *em, *h;
	uint8_t want[64], counter[4], block[64 + 4], mask[64];
	uint32_t c;
	while (n_len && !*n) { n++; n_len--; }
	if (!hl || n_len > sizeof s_em || rsa_public(n, n_len, e, e_len, sig, sig_len, s_em) != 0) return -1;
	mod_bits = 8 * n_len;
	for (i = 0x80; i && !(n[0] & i); i >>= 1) mod_bits--;
	em_bits = mod_bits - 1;
	em_len = (em_bits + 7) / 8;
	em = s_em + (n_len - em_len);
	if (em_len < n_len && s_em[0]) return -1;
	if (em_len < hl + 2 || em[em_len - 1] != 0xBC) return -1;
	db_len = em_len - hl - 1;
	h = em + db_len;
	if (8 * em_len > em_bits && (em[0] >> (8 - (8 * em_len - em_bits)))) return -1;	/* the unused top bits are 0 */
	/* MGF1: the mask from H with a counter */
	for (i = 0, c = 0; i < db_len; c++) {
		size_t take, k;
		counter[0] = (uint8_t)(c >> 24); counter[1] = (uint8_t)(c >> 16); counter[2] = (uint8_t)(c >> 8); counter[3] = (uint8_t)c;
		memcpy(block, h, hl);
		memcpy(block + hl, counter, 4);
		pki_hash(hash, block, hl + 4, mask);
		take = db_len - i < hl ? db_len - i : hl;
		for (k = 0; k < take; k++) db[i + k] = em[i + k] ^ mask[k];
		i += take;
	}
	db[0] &= (uint8_t)(0xFF >> (8 * em_len - em_bits));
	/* DB = 00 .. 00 01 salt */
	for (ps = 0; ps < db_len && !db[ps]; ps++) ;
	if (ps == db_len || db[ps] != 1) return -1;
	ps++;
	memset(mprime, 0, 8);
	memcpy(mprime + 8, digest, hl);
	memcpy(mprime + 8 + hl, db + ps, db_len - ps);
	pki_hash(hash, mprime, 8 + hl + db_len - ps, want);
	return memcmp(want, h, hl) ? -1 : 0;
}

/* ---- ECDSA over P-256 and P-384 (SEC 1, FIPS 186-4) ---- */

#define EC_LIMBS 12

/* arithmetic mod m, in Montgomery form (x stands for x.R mod m, R = 2^(32k)) */
struct modm {
	uint32_t m[EC_LIMBS], r2[EC_LIMBS], one[EC_LIMBS];	/* one: R mod m, the form of 1 */
	unsigned int k;
	uint32_t m0;
};

static void modm_init(struct modm *z, const uint8_t *be, unsigned int bytes)
{
	uint32_t inv;
	unsigned int i, j;
	z->k = bytes / 4;
	bn_load(z->m, z->k, be, bytes);
	inv = z->m[0];
	for (i = 0; i < 5; i++) inv *= 2u - z->m[0] * inv;
	z->m0 = 0u - inv;
	memset(z->r2, 0, sizeof z->r2);
	z->r2[0] = 1;
	for (i = 0; i < 64u * z->k; i++) {		/* 2^(64k) mod m by doubling */
		uint32_t carry = 0;
		for (j = 0; j < z->k; j++) {
			uint32_t v = z->r2[j];
			z->r2[j] = v << 1 | carry;
			carry = v >> 31;
		}
		if (carry || bn_cmp(z->r2, z->m, z->k) >= 0) bn_sub(z->r2, z->m, z->k);
	}
	memset(z->one, 0, sizeof z->one);
	z->one[0] = 1;
	mont_mul(z->one, z->one, z->r2, z->m, z->k, z->m0);
}

static void mm(const struct modm *z, uint32_t *o, const uint32_t *a, const uint32_t *b) { mont_mul(o, a, b, z->m, z->k, z->m0); }

static void madd(const struct modm *z, uint32_t *o, const uint32_t *a, const uint32_t *b)
{
	uint64_t c = 0;
	unsigned int i;
	for (i = 0; i < z->k; i++) {
		c += (uint64_t)a[i] + b[i];
		o[i] = (uint32_t)c;
		c >>= 32;
	}
	if (c || bn_cmp(o, z->m, z->k) >= 0) bn_sub(o, z->m, z->k);
}

static void msub(const struct modm *z, uint32_t *o, const uint32_t *a, const uint32_t *b)
{
	uint32_t t[EC_LIMBS];
	memcpy(t, a, 4u * z->k);
	if (bn_sub(t, b, z->k)) {			/* went below zero: add m back */
		uint64_t c = 0;
		unsigned int i;
		for (i = 0; i < z->k; i++) {
			c += (uint64_t)t[i] + z->m[i];
			t[i] = (uint32_t)c;
			c >>= 32;
		}
	}
	memcpy(o, t, 4u * z->k);
}

static int mzero(const struct modm *z, const uint32_t *a)
{
	unsigned int i;
	for (i = 0; i < z->k; i++) if (a[i]) return 0;
	return 1;
}

/* o = a^(m-2) = 1/a for prime m (Fermat), all in Montgomery form */
static void minv(const struct modm *z, uint32_t *o, const uint32_t *a)
{
	uint32_t e[EC_LIMBS], acc[EC_LIMBS];
	int bit;
	memcpy(e, z->m, 4u * z->k);
	e[0] -= 2;					/* m is odd and large: no borrow */
	memcpy(acc, z->one, 4u * z->k);
	for (bit = 32 * (int)z->k - 1; bit >= 0; bit--) {
		mm(z, acc, acc, acc);
		if (e[bit / 32] >> (bit % 32) & 1) mm(z, acc, acc, a);
	}
	memcpy(o, acc, 4u * z->k);
}

struct curve {
	struct modm p, n;
	uint32_t b[EC_LIMBS], gx[EC_LIMBS], gy[EC_LIMBS];	/* Montgomery form mod p */
	unsigned int bytes;
};

/* the curves' numbers (FIPS 186-4 D.1.2.3 and D.1.2.4), big-endian */
static const uint8_t P256_P[] = { 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t P256_N[] = { 0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 };
static const uint8_t P256_B[] = { 0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
static const uint8_t P256_GX[] = { 0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const uint8_t P256_GY[] = { 0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
static const uint8_t P384_P[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFE,0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF };
static const uint8_t P384_N[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xC7,0x63,0x4D,0x81,0xF4,0x37,0x2D,0xDF,0x58,0x1A,0x0D,0xB2,0x48,0xB0,0xA7,0x7A,0xEC,0xEC,0x19,0x6A,0xCC,0xC5,0x29,0x73 };
static const uint8_t P384_B[] = { 0xB3,0x31,0x2F,0xA7,0xE2,0x3E,0xE7,0xE4,0x98,0x8E,0x05,0x6B,0xE3,0xF8,0x2D,0x19,0x18,0x1D,0x9C,0x6E,0xFE,0x81,0x41,0x12,0x03,0x14,0x08,0x8F,0x50,0x13,0x87,0x5A,0xC6,0x56,0x39,0x8D,0x8A,0x2E,0xD1,0x9D,0x2A,0x85,0xC8,0xED,0xD3,0xEC,0x2A,0xEF };
static const uint8_t P384_GX[] = { 0xAA,0x87,0xCA,0x22,0xBE,0x8B,0x05,0x37,0x8E,0xB1,0xC7,0x1E,0xF3,0x20,0xAD,0x74,0x6E,0x1D,0x3B,0x62,0x8B,0xA7,0x9B,0x98,0x59,0xF7,0x41,0xE0,0x82,0x54,0x2A,0x38,0x55,0x02,0xF2,0x5D,0xBF,0x55,0x29,0x6C,0x3A,0x54,0x5E,0x38,0x72,0x76,0x0A,0xB7 };
static const uint8_t P384_GY[] = { 0x36,0x17,0xDE,0x4A,0x96,0x26,0x2C,0x6F,0x5D,0x9E,0x98,0xBF,0x92,0x92,0xDC,0x29,0xF8,0xF4,0x1D,0xBD,0x28,0x9A,0x14,0x7C,0xE9,0xDA,0x31,0x13,0xB5,0xF0,0xB8,0xC0,0x0A,0x60,0xB1,0xCE,0x1D,0x7E,0x81,0x9D,0x7A,0x43,0x1D,0x7C,0x90,0xEA,0x0E,0x5F };

static void to_mont(const struct modm *z, uint32_t *o, const uint8_t *be)
{
	uint32_t t[EC_LIMBS];
	bn_load(t, z->k, be, 4u * z->k);
	mm(z, o, t, z->r2);
}

static void curve_setup(struct curve *c, int which)
{
	int big = which == PKI_KEY_EC_P384;
	c->bytes = big ? 48 : 32;
	modm_init(&c->p, big ? P384_P : P256_P, c->bytes);
	modm_init(&c->n, big ? P384_N : P256_N, c->bytes);
	to_mont(&c->p, c->b, big ? P384_B : P256_B);
	to_mont(&c->p, c->gx, big ? P384_GX : P256_GX);
	to_mont(&c->p, c->gy, big ? P384_GY : P256_GY);
}

/* a point in Jacobian coordinates (x/z^2, y/z^3), z = 0 the point at infinity */
struct jpt { uint32_t x[EC_LIMBS], y[EC_LIMBS], z[EC_LIMBS]; };

/* doubling for a = -3 */
static void pt_double(const struct curve *c, struct jpt *o, const struct jpt *a)
{
	const struct modm *p = &c->p;
	uint32_t delta[EC_LIMBS], gamma[EC_LIMBS], beta[EC_LIMBS], alpha[EC_LIMBS], t[EC_LIMBS], u[EC_LIMBS];
	if (mzero(p, a->z)) { *o = *a; return; }
	mm(p, delta, a->z, a->z);
	mm(p, gamma, a->y, a->y);
	mm(p, beta, a->x, gamma);
	msub(p, t, a->x, delta);
	madd(p, u, a->x, delta);
	mm(p, alpha, t, u);
	madd(p, t, alpha, alpha);
	madd(p, alpha, t, alpha);			/* alpha = 3 (x - delta)(x + delta) */
	madd(p, t, a->y, a->z);
	mm(p, t, t, t);
	msub(p, t, t, gamma);
	msub(p, o->z, t, delta);			/* z3 = (y + z)^2 - gamma - delta */
	madd(p, u, beta, beta);
	madd(p, u, u, u);				/* 4 beta */
	mm(p, t, alpha, alpha);
	msub(p, t, t, u);
	msub(p, o->x, t, u);				/* x3 = alpha^2 - 8 beta */
	msub(p, u, u, o->x);
	mm(p, u, alpha, u);
	mm(p, gamma, gamma, gamma);
	madd(p, gamma, gamma, gamma);
	madd(p, gamma, gamma, gamma);
	madd(p, gamma, gamma, gamma);			/* 8 gamma^2 */
	msub(p, o->y, u, gamma);			/* y3 = alpha (4 beta - x3) - 8 gamma^2 */
}

static void pt_add(const struct curve *c, struct jpt *o, const struct jpt *a, const struct jpt *b)
{
	const struct modm *p = &c->p;
	uint32_t z1z1[EC_LIMBS], z2z2[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], s1[EC_LIMBS], s2[EC_LIMBS];
	uint32_t h[EC_LIMBS], i[EC_LIMBS], j[EC_LIMBS], r[EC_LIMBS], v[EC_LIMBS], t[EC_LIMBS];
	if (mzero(p, a->z)) { *o = *b; return; }
	if (mzero(p, b->z)) { *o = *a; return; }
	mm(p, z1z1, a->z, a->z);
	mm(p, z2z2, b->z, b->z);
	mm(p, u1, a->x, z2z2);
	mm(p, u2, b->x, z1z1);
	mm(p, s1, a->y, b->z); mm(p, s1, s1, z2z2);
	mm(p, s2, b->y, a->z); mm(p, s2, s2, z1z1);
	msub(p, h, u2, u1);
	msub(p, r, s2, s1);
	if (mzero(p, h)) {
		if (mzero(p, r)) { pt_double(c, o, a); return; }
		memset(o, 0, sizeof *o);		/* P + (-P) */
		return;
	}
	madd(p, r, r, r);				/* r = 2 (s2 - s1) */
	madd(p, t, h, h);
	mm(p, i, t, t);					/* i = (2h)^2 */
	mm(p, j, h, i);
	mm(p, v, u1, i);
	madd(p, t, a->z, b->z);
	mm(p, t, t, t);
	msub(p, t, t, z1z1);
	msub(p, t, t, z2z2);
	mm(p, o->z, t, h);				/* z3 = ((z1 + z2)^2 - z1z1 - z2z2) h */
	mm(p, t, r, r);
	msub(p, t, t, j);
	msub(p, t, t, v);
	msub(p, o->x, t, v);				/* x3 = r^2 - j - 2v */
	msub(p, t, v, o->x);
	mm(p, t, r, t);
	mm(p, s1, s1, j);
	madd(p, s1, s1, s1);
	msub(p, o->y, t, s1);				/* y3 = r (v - x3) - 2 s1 j */
}

/* an uncompressed point (04 x y) into Jacobian form, checked to lie on the curve
 * (y^2 = x^3 - 3x + b) with both coordinates below p: 0, or -1 */
static int load_point(const struct curve *c, const uint8_t *pt, size_t len, struct jpt *q)
{
	uint32_t x[EC_LIMBS], y[EC_LIMBS], t[EC_LIMBS], y2[EC_LIMBS];
	if (len != 1 + 2 * c->bytes || pt[0] != 0x04) return -1;
	bn_load(x, c->p.k, pt + 1, c->bytes);
	bn_load(y, c->p.k, pt + 1 + c->bytes, c->bytes);
	if (bn_cmp(x, c->p.m, c->p.k) >= 0 || bn_cmp(y, c->p.m, c->p.k) >= 0) return -1;
	memset(q, 0, sizeof *q);
	mm(&c->p, q->x, x, c->p.r2);
	mm(&c->p, q->y, y, c->p.r2);
	memcpy(q->z, c->p.one, sizeof q->z);
	mm(&c->p, y2, q->y, q->y);
	mm(&c->p, t, q->x, q->x);
	mm(&c->p, t, t, q->x);
	msub(&c->p, t, t, q->x); msub(&c->p, t, t, q->x); msub(&c->p, t, t, q->x);
	madd(&c->p, t, t, c->b);
	return bn_cmp(t, y2, c->p.k) ? -1 : 0;
}

/* a scalar (big-endian, k*4 bytes) mod n reduced from a hash: its leftmost bits as SEC 1 says */
static void hash_scalar(const struct curve *c, const uint8_t *digest, size_t hl, uint32_t *e)
{
	uint8_t buf[48];
	memset(buf, 0, sizeof buf);
	if (hl >= c->bytes) memcpy(buf, digest, c->bytes);
	else memcpy(buf + (c->bytes - hl), digest, hl);
	bn_load(e, c->n.k, buf, c->bytes);
	if (bn_cmp(e, c->n.m, c->n.k) >= 0) bn_sub(e, c->n.m, c->n.k);
}

/* an INTEGER from the signature into k limbs: 0, -1 when it is 0 or not below n */
static int sig_int(const struct curve *c, const struct der *v, uint32_t *out)
{
	const uint8_t *p = v->p;
	size_t len = v->len;
	if (v->tag != 0x02 || !len) return -1;
	while (len && !*p) { p++; len--; }
	if (!len || len > c->bytes || !bn_load(out, c->n.k, p, len)) return -1;
	return bn_cmp(out, c->n.m, c->n.k) < 0 ? 0 : -1;
}

int pki_ecdsa(int which, const uint8_t *point, size_t point_len, const uint8_t *sig, size_t sig_len,
	      const uint8_t *digest, size_t hash_len)
{
	static struct curve c;
	static int ready_kind;
	struct der in = { sig, sig_len, 0, 0, 0 }, seq, l, rv, sv;
	uint32_t r[EC_LIMBS], s[EC_LIMBS], e[EC_LIMBS], w[EC_LIMBS], u1[EC_LIMBS], u2[EC_LIMBS], t[EC_LIMBS];
	struct jpt g, q, gq, acc;
	int bit;
	unsigned int i;
	if (which != PKI_KEY_EC_P256 && which != PKI_KEY_EC_P384) return -1;
	if (ready_kind != which) { curve_setup(&c, which); ready_kind = which; }
	if (der_next(&in, &seq) != 0 || seq.tag != 0x30) return -1;
	l.p = seq.p; l.len = seq.len;
	if (der_next(&l, &rv) != 0 || der_next(&l, &sv) != 0) return -1;
	if (sig_int(&c, &rv, r) != 0 || sig_int(&c, &sv, s) != 0) return -1;

	if (load_point(&c, point, point_len, &q) != 0) return -1;

	/* w = 1/s, u1 = e w, u2 = r w (mod n; Montgomery form, then out of it) */
	hash_scalar(&c, digest, hash_len, e);
	mm(&c.n, s, s, c.n.r2);
	minv(&c.n, w, s);
	mm(&c.n, e, e, c.n.r2);
	mm(&c.n, u1, e, w);
	mm(&c.n, t, r, c.n.r2);
	mm(&c.n, u2, t, w);
	memset(t, 0, sizeof t); t[0] = 1;
	mm(&c.n, u1, u1, t);
	mm(&c.n, u2, u2, t);

	/* u1 G + u2 Q, both at once */
	memcpy(g.x, c.gx, sizeof g.x);
	memcpy(g.y, c.gy, sizeof g.y);
	memcpy(g.z, c.p.one, sizeof g.z);
	pt_add(&c, &gq, &g, &q);
	memset(&acc, 0, sizeof acc);
	for (bit = 32 * (int)c.n.k - 1; bit >= 0; bit--) {
		int b1 = (int)(u1[bit / 32] >> (bit % 32) & 1), b2 = (int)(u2[bit / 32] >> (bit % 32) & 1);
		pt_double(&c, &acc, &acc);
		if (b1 && b2) pt_add(&c, &acc, &acc, &gq);
		else if (b1) pt_add(&c, &acc, &acc, &g);
		else if (b2) pt_add(&c, &acc, &acc, &q);
	}
	if (mzero(&c.p, acc.z)) return -1;
	/* x = X / Z^2, out of Montgomery form, mod n, against r */
	minv(&c.p, t, acc.z);
	mm(&c.p, t, t, t);
	mm(&c.p, t, acc.x, t);
	memset(w, 0, sizeof w); w[0] = 1;
	mm(&c.p, t, t, w);
	for (i = c.n.k; i < EC_LIMBS; i++) t[i] = 0;
	if (bn_cmp(t, c.n.m, c.n.k) >= 0) bn_sub(t, c.n.m, c.n.k);
	return bn_cmp(t, r, c.n.k) ? -1 : 0;
}

/* ---- DER ---- */

int der_next(struct der *in, struct der *out)
{
	size_t len, hl = 2;
	const uint8_t *p = in->p;
	if (in->len < 2) return -1;
	len = p[1];
	if (len & 0x80) {
		unsigned int nb = len & 0x7F, i;
		if (!nb || nb > 4 || in->len < 2 + nb) return -1;
		len = 0;
		for (i = 0; i < nb; i++) len = len << 8 | p[2 + i];
		hl += nb;
	}
	if (len > in->len - hl) return -1;
	out->tag = p[0];
	out->p = p + hl;
	out->len = len;
	out->whole = p;
	out->whole_len = hl + len;
	in->p += hl + len;
	in->len -= hl + len;
	return 0;
}

/* the first element of a constructed one's content */
static int der_open(const struct der *outer, struct der *inner_list)
{
	inner_list->p = outer->p;
	inner_list->len = outer->len;
	return 0;
}

static int oid_is(const struct der *o, const uint8_t *want, size_t n)
{
	return o->tag == 0x06 && o->len == n && !memcmp(o->p, want, n);
}

/* OIDs (their DER content) */
static const uint8_t OID_RSA[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
static const uint8_t OID_RSA_PSS[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0A };
static const uint8_t OID_RSA_SHA256[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B };
static const uint8_t OID_RSA_SHA384[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C };
static const uint8_t OID_RSA_SHA512[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0D };
static const uint8_t OID_EC_KEY[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
static const uint8_t OID_ECDSA_SHA256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const uint8_t OID_ECDSA_SHA384[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03 };
static const uint8_t OID_ECDSA_SHA512[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x04 };
static const uint8_t OID_P256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
static const uint8_t OID_P384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
static const uint8_t OID_SHA256[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };
static const uint8_t OID_SHA384[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02 };
static const uint8_t OID_SHA512[] = { 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03 };
static const uint8_t OID_SAN[] = { 0x55, 0x1D, 0x11 };
static const uint8_t OID_BASIC[] = { 0x55, 0x1D, 0x13 };
static const uint8_t OID_KEY_USAGE[] = { 0x55, 0x1D, 0x0F };
static const uint8_t OID_CN[] = { 0x55, 0x04, 0x03 };
#define OID(o, w) oid_is(o, w, sizeof w)

/* a signature algorithm identifier: its kind and hash */
static int sig_alg(const struct der *alg, int *kind, int *hash)
{
	struct der l, oid, params;
	der_open(alg, &l);
	if (der_next(&l, &oid) != 0) return -1;
	*kind = PKI_SIG_NONE;
	*hash = PKI_HASH_NONE;
	if (OID(&oid, OID_RSA_SHA256)) { *kind = PKI_SIG_RSA_PKCS1; *hash = PKI_HASH_SHA256; }
	else if (OID(&oid, OID_RSA_SHA384)) { *kind = PKI_SIG_RSA_PKCS1; *hash = PKI_HASH_SHA384; }
	else if (OID(&oid, OID_RSA_SHA512)) { *kind = PKI_SIG_RSA_PKCS1; *hash = PKI_HASH_SHA512; }
	else if (OID(&oid, OID_ECDSA_SHA256)) { *kind = PKI_SIG_ECDSA; *hash = PKI_HASH_SHA256; }
	else if (OID(&oid, OID_ECDSA_SHA384)) { *kind = PKI_SIG_ECDSA; *hash = PKI_HASH_SHA384; }
	else if (OID(&oid, OID_ECDSA_SHA512)) { *kind = PKI_SIG_ECDSA; *hash = PKI_HASH_SHA512; }
	else if (OID(&oid, OID_RSA_PSS)) {
		/* the parameters name the hash: SEQUENCE { [0] { AlgorithmIdentifier } ... } */
		struct der pl, h0, hl, hoid;
		*kind = PKI_SIG_RSA_PSS;
		*hash = PKI_HASH_SHA256;		/* (the default would be SHA-1, which we refuse) */
		if (der_next(&l, &params) != 0 || params.tag != 0x30) return -1;
		der_open(&params, &pl);
		if (der_next(&pl, &h0) != 0 || h0.tag != 0xA0) return -1;
		der_open(&h0, &hl);
		if (der_next(&hl, &hoid) != 0 || hoid.tag != 0x30) return -1;
		der_open(&hoid, &hl);
		if (der_next(&hl, &hoid) != 0) return -1;
		if (OID(&hoid, OID_SHA384)) *hash = PKI_HASH_SHA384;
		else if (OID(&hoid, OID_SHA512)) *hash = PKI_HASH_SHA512;
		else if (!OID(&hoid, OID_SHA256)) return -1;
	}
	return *kind == PKI_SIG_NONE ? -1 : 0;
}

/* UTCTime YYMMDDHHMMSSZ or GeneralizedTime YYYYMMDDHHMMSSZ into seconds since 1970 */
static int der_time(const struct der *t, long long *out)
{
	const uint8_t *p = t->p;
	int y, mo, d, h, mi, s, i;
	long long days;
	static const int before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	if (t->tag == 0x17 && t->len == 13) {
		y = (p[0] - '0') * 10 + (p[1] - '0');
		y += y < 50 ? 2000 : 1900;
		p += 2;
	} else if (t->tag == 0x18 && t->len == 15) {
		y = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
		p += 4;
	} else {
		return -1;
	}
	for (i = 0; i < 10; i++) if (p[i] < '0' || p[i] > '9') return -1;
	mo = (p[0] - '0') * 10 + (p[1] - '0');
	d = (p[2] - '0') * 10 + (p[3] - '0');
	h = (p[4] - '0') * 10 + (p[5] - '0');
	mi = (p[6] - '0') * 10 + (p[7] - '0');
	s = (p[8] - '0') * 10 + (p[9] - '0');
	if (p[10] != 'Z' || mo < 1 || mo > 12) return -1;
	days = (long long)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
	days += before[mo - 1] + (d - 1);
	if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
	*out = ((days * 24 + h) * 60 + mi) * 60 + s;
	return 0;
}

/* the common name inside a Name: SET { SEQUENCE { OID, value } } ... */
static void find_cn(const struct der *name, struct der *cn)
{
	struct der l, set, sl, atv, al, oid, val;
	cn->len = 0;
	der_open(name, &l);
	while (der_next(&l, &set) == 0) {
		der_open(&set, &sl);
		while (der_next(&sl, &atv) == 0) {
			der_open(&atv, &al);
			if (der_next(&al, &oid) == 0 && der_next(&al, &val) == 0 && OID(&oid, OID_CN))
				*cn = val;
		}
	}
}

static int public_key(const struct der *spki, struct pki_cert *c)
{
	struct der l, alg, al, oid, params, bits, kl, n, e;
	der_open(spki, &l);
	if (der_next(&l, &alg) != 0 || der_next(&l, &bits) != 0 || bits.tag != 0x03 || bits.len < 2 || bits.p[0])
		return -1;
	der_open(&alg, &al);
	if (der_next(&al, &oid) != 0) return -1;
	if (OID(&oid, OID_RSA)) {
		struct der key = { bits.p + 1, bits.len - 1, 0, 0, 0 };
		struct der seq;
		if (der_next(&key, &seq) != 0 || seq.tag != 0x30) return -1;
		der_open(&seq, &kl);
		if (der_next(&kl, &n) != 0 || der_next(&kl, &e) != 0 || n.tag != 0x02 || e.tag != 0x02) return -1;
		c->key_kind = PKI_KEY_RSA;
		c->key_n = n.p; c->key_n_len = n.len;
		while (c->key_n_len && !*c->key_n) { c->key_n++; c->key_n_len--; }
		c->key_e = e.p; c->key_e_len = e.len;
		return 0;
	}
	if (OID(&oid, OID_EC_KEY)) {
		if (der_next(&al, &params) != 0) return -1;
		if (OID(&params, OID_P256)) c->key_kind = PKI_KEY_EC_P256;
		else if (OID(&params, OID_P384)) c->key_kind = PKI_KEY_EC_P384;
		else return -1;
		c->key_point = bits.p + 1;
		c->key_point_len = bits.len - 1;
		return 0;
	}
	return -1;
}

static void extensions(const struct der *ext_wrap, struct pki_cert *c)
{
	struct der l, seq, list, ext, el, oid, crit, val;
	der_open(ext_wrap, &l);
	if (der_next(&l, &seq) != 0) return;
	der_open(&seq, &list);
	while (der_next(&list, &ext) == 0) {
		der_open(&ext, &el);
		if (der_next(&el, &oid) != 0 || der_next(&el, &crit) != 0) continue;
		if (crit.tag == 0x01) { if (der_next(&el, &val) != 0) continue; }
		else val = crit;
		if (val.tag != 0x04) continue;
		{
			struct der inside = { val.p, val.len, 0, 0, 0 }, body;
			if (der_next(&inside, &body) != 0) continue;
			if (OID(&oid, OID_SAN)) {
				c->san = body;
			} else if (OID(&oid, OID_BASIC)) {
				struct der bl, x;
				der_open(&body, &bl);
				if (der_next(&bl, &x) == 0 && x.tag == 0x01 && x.len == 1) {
					c->is_ca = x.p[0] != 0;
					if (der_next(&bl, &x) == 0 && x.tag == 0x02 && x.len == 1) c->path_len = x.p[0];
				}
			} else if (OID(&oid, OID_KEY_USAGE)) {
				if (body.tag == 0x03 && body.len >= 2)
					c->key_usage = body.p[1] | (body.len > 2 ? body.p[2] << 8 : 0);
			}
		}
	}
}

int pki_cert_parse(const uint8_t *der, size_t len, struct pki_cert *c)
{
	struct der in = { der, len, 0, 0, 0 }, cert, cl, sigalg, sig, tl, x, validity, vl, t;
	memset(c, 0, sizeof *c);
	c->path_len = -1;
	c->key_usage = -1;
	if (der_next(&in, &cert) != 0 || cert.tag != 0x30) return -1;
	der_open(&cert, &cl);
	if (der_next(&cl, &c->tbs) != 0 || der_next(&cl, &sigalg) != 0 || der_next(&cl, &sig) != 0) return -1;
	if (sig_alg(&sigalg, &c->sig_kind, &c->sig_hash) != 0) return -1;
	if (sig.tag != 0x03 || sig.len < 2 || sig.p[0]) return -1;
	c->sig = sig.p + 1;
	c->sig_len = sig.len - 1;
	der_open(&c->tbs, &tl);
	if (der_next(&tl, &x) != 0) return -1;
	if (x.tag == 0xA0 && der_next(&tl, &x) != 0) return -1;	/* version, then the serial */
	if (der_next(&tl, &x) != 0) return -1;			/* signature (repeated) */
	if (der_next(&tl, &c->issuer) != 0 || der_next(&tl, &validity) != 0 || der_next(&tl, &c->subject) != 0)
		return -1;
	der_open(&validity, &vl);
	if (der_next(&vl, &t) != 0 || der_time(&t, &c->not_before) != 0) return -1;
	if (der_next(&vl, &t) != 0 || der_time(&t, &c->not_after) != 0) return -1;
	if (der_next(&tl, &x) != 0 || public_key(&x, c) != 0) return -1;
	while (der_next(&tl, &x) == 0)
		if (x.tag == 0xA3) extensions(&x, c);
	find_cn(&c->subject, &c->cn);
	return 0;
}

int pki_cert_signed_by(const struct pki_cert *c, const struct pki_cert *issuer)
{
	uint8_t digest[64];
	if (c->sig_hash == PKI_HASH_NONE) return -1;
	pki_hash(c->sig_hash, c->tbs.whole, c->tbs.whole_len, digest);
	if (issuer->key_kind == PKI_KEY_RSA && c->sig_kind == PKI_SIG_RSA_PKCS1)
		return pki_rsa_pkcs1(issuer->key_n, issuer->key_n_len, issuer->key_e, issuer->key_e_len, c->sig, c->sig_len,
				     c->sig_hash, digest);
	if (issuer->key_kind == PKI_KEY_RSA && c->sig_kind == PKI_SIG_RSA_PSS)
		return pki_rsa_pss(issuer->key_n, issuer->key_n_len, issuer->key_e, issuer->key_e_len, c->sig, c->sig_len,
				   c->sig_hash, digest);
	if ((issuer->key_kind == PKI_KEY_EC_P256 || issuer->key_kind == PKI_KEY_EC_P384) && c->sig_kind == PKI_SIG_ECDSA)
		return pki_ecdsa(issuer->key_kind, issuer->key_point, issuer->key_point_len, c->sig, c->sig_len, digest,
				 pki_hash_len(c->sig_hash));
	return -1;
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

/* one name against host: case does not count; "*." covers exactly one leftmost label */
static int name_match(const uint8_t *n, size_t nl, const char *host)
{
	size_t hl = strlen(host), i;
	if (nl >= 2 && n[0] == '*' && n[1] == '.') {
		const char *dot = strchr(host, '.');
		if (!dot || dot == host) return 0;
		host = dot;
		hl = strlen(host);
		n++;
		nl--;
	}
	if (nl != hl) return 0;
	for (i = 0; i < nl; i++)
		if (lower(n[i]) != lower((unsigned char)host[i])) return 0;
	return 1;
}

int pki_cert_names(const struct pki_cert *c, const char *host)
{
	struct der l, gn;
	if (c->san.len) {
		der_open(&c->san, &l);
		while (der_next(&l, &gn) == 0)
			if (gn.tag == 0x82 && name_match(gn.p, gn.len, host)) return 1;	/* dNSName */
		return 0;				/* names given: the common name does not count */
	}
	return c->cn.len && name_match(c->cn.p, c->cn.len, host);
}

/* ---- chains ---- */

static int same_der(const struct der *a, const struct der *b)
{
	return a->len == b->len && !memcmp(a->p, b->p, a->len);
}

/* may this certificate sign others? a CA, and (when it says what its key is for) keyCertSign */
static int can_sign(const struct pki_cert *c)
{
	return c->is_ca && (c->key_usage < 0 || (c->key_usage & 0x04));
}

/* is c one of the roots itself (same subject, same key)? 1 yes */
static int is_root(const struct pki_cert *c, const uint8_t *roots, size_t roots_len)
{
	struct der list = { roots, roots_len, 0, 0, 0 }, one;
	static struct pki_cert r;
	while (der_next(&list, &one) == 0) {
		if (pki_cert_parse(one.whole, one.whole_len, &r) != 0) continue;
		if (!same_der(&r.subject, &c->subject) || r.key_kind != c->key_kind) continue;
		if (r.key_kind == PKI_KEY_RSA ? (r.key_n_len == c->key_n_len && !memcmp(r.key_n, c->key_n, r.key_n_len))
					      : (r.key_point_len == c->key_point_len && !memcmp(r.key_point, c->key_point, r.key_point_len)))
			return 1;
	}
	return 0;
}

/* is c signed by one of the roots (its issuer's name, then the signature)? 1 yes */
static int root_signs(const struct pki_cert *c, const uint8_t *roots, size_t roots_len)
{
	struct der list = { roots, roots_len, 0, 0, 0 }, one;
	static struct pki_cert r;
	while (der_next(&list, &one) == 0) {
		if (pki_cert_parse(one.whole, one.whole_len, &r) != 0) continue;
		if (same_der(&r.subject, &c->issuer) && pki_cert_signed_by(c, &r) == 0)
			return 1;
	}
	return 0;
}

const char *pki_chain_check(const struct pki_cert *chain, int n, const char *host, long long now,
			    const uint8_t *roots, size_t roots_len)
{
	int i;
	if (n < 1) return "no certificate";
	if (!pki_cert_names(&chain[0], host)) return "the certificate is for another name";
	for (i = 0; i < n; i++) {
		if (now && now < chain[i].not_before) return "a certificate is not valid yet (clock?)";
		if (now && now > chain[i].not_after) return "a certificate has expired";
		if (i && is_root(&chain[i], roots, roots_len))
			return 0;			/* reached a trusted root the server sent too */
		if (root_signs(&chain[i], roots, roots_len))
			return 0;			/* signed by a trusted root */
		if (i + 1 >= n) break;
		if (!same_der(&chain[i].issuer, &chain[i + 1].subject)) return "the chain is out of order";
		if (!can_sign(&chain[i + 1])) return "a certificate in the chain may not sign others";
		if (pki_cert_signed_by(&chain[i], &chain[i + 1]) != 0) return "a signature in the chain does not check";
	}
	return "the chain does not end in a trusted root";
}

/* ---- ECDH (TLS 1.2 key exchange) ---- */

static struct curve s_dh_curve;
static int s_dh_kind;

static const struct curve *dh_curve(int which)
{
	if (which != PKI_KEY_EC_P256 && which != PKI_KEY_EC_P384) return 0;
	if (s_dh_kind != which) { curve_setup(&s_dh_curve, which); s_dh_kind = which; }
	return &s_dh_curve;
}

/* swap two points when bit is 1, by masks rather than a branch on the bit */
static void pt_swap(struct jpt *a, struct jpt *b, uint32_t bit)
{
	uint32_t m = 0u - bit, *x = (uint32_t *)a, *y = (uint32_t *)b, t;
	unsigned int i;
	for (i = 0; i < sizeof *a / 4; i++) {
		t = m & (x[i] ^ y[i]);
		x[i] ^= t;
		y[i] ^= t;
	}
}

/* k . p by a Montgomery ladder: the same double and add for every bit of k */
static void ec_mul(const struct curve *c, struct jpt *out, const uint32_t *k, const struct jpt *p)
{
	struct jpt r0, r1, t;
	int bit;
	uint32_t swap = 0, b;
	memset(&r0, 0, sizeof r0);			/* the point at infinity */
	r1 = *p;
	for (bit = 32 * (int)c->n.k - 1; bit >= 0; bit--) {
		b = k[bit / 32] >> (bit % 32) & 1u;
		pt_swap(&r0, &r1, swap ^ b);
		swap = b;
		pt_add(c, &t, &r0, &r1);
		pt_double(c, &r0, &r0);
		r1 = t;
	}
	pt_swap(&r0, &r1, swap);
	*out = r0;
}

/* a Jacobian point's affine x (and y) as big-endian bytes */
static int pt_affine(const struct curve *c, const struct jpt *p, uint8_t *x, uint8_t *y)
{
	uint32_t zi[EC_LIMBS], t[EC_LIMBS], one[EC_LIMBS];
	unsigned int i;
	if (mzero(&c->p, p->z)) return -1;
	memset(one, 0, sizeof one); one[0] = 1;
	minv(&c->p, zi, p->z);
	mm(&c->p, t, zi, zi);
	if (x) {
		uint32_t ax[EC_LIMBS];
		mm(&c->p, ax, p->x, t);
		mm(&c->p, ax, ax, one);
		for (i = 0; i < c->bytes; i++) x[c->bytes - 1 - i] = (uint8_t)(ax[i / 4] >> (8 * (i % 4)));
	}
	if (y) {
		uint32_t ay[EC_LIMBS];
		mm(&c->p, t, t, zi);
		mm(&c->p, ay, p->y, t);
		mm(&c->p, ay, ay, one);
		for (i = 0; i < c->bytes; i++) y[c->bytes - 1 - i] = (uint8_t)(ay[i / 4] >> (8 * (i % 4)));
	}
	return 0;
}

/* the private scalar from bytes: below n and not 0 */
static void dh_scalar(const struct curve *c, const uint8_t *b, uint32_t *k)
{
	unsigned int i;
	int zero = 1;
	bn_load(k, c->n.k, b, c->bytes);
	while (bn_cmp(k, c->n.m, c->n.k) >= 0) bn_sub(k, c->n.m, c->n.k);
	for (i = 0; i < c->n.k; i++) if (k[i]) zero = 0;
	if (zero) k[0] = 1;
}

int pki_ec_keypair(int which, const uint8_t *seed, size_t seed_len, uint8_t *priv, uint8_t *pub, size_t *pub_len)
{
	const struct curve *c = dh_curve(which);
	uint32_t k[EC_LIMBS];
	struct jpt g, q;
	unsigned int i;
	if (!c || seed_len < c->bytes) return -1;
	dh_scalar(c, seed, k);
	for (i = 0; i < c->bytes; i++) priv[c->bytes - 1 - i] = (uint8_t)(k[i / 4] >> (8 * (i % 4)));
	memcpy(g.x, c->gx, sizeof g.x);
	memcpy(g.y, c->gy, sizeof g.y);
	memcpy(g.z, c->p.one, sizeof g.z);
	ec_mul(c, &q, k, &g);
	pub[0] = 0x04;
	if (pt_affine(c, &q, pub + 1, pub + 1 + c->bytes) != 0) return -1;
	*pub_len = 1 + 2 * c->bytes;
	memset(k, 0, sizeof k);
	return 0;
}

int pki_ec_shared(int which, const uint8_t *priv, const uint8_t *peer, size_t peer_len, uint8_t *out)
{
	const struct curve *c = dh_curve(which);
	uint32_t k[EC_LIMBS];
	struct jpt p, s;
	int rc;
	if (!c || load_point(c, peer, peer_len, &p) != 0) return -1;
	dh_scalar(c, priv, k);
	ec_mul(c, &s, k, &p);
	rc = pt_affine(c, &s, out, 0);
	memset(k, 0, sizeof k);
	return rc;
}
