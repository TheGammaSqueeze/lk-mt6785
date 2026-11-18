/*
 * pico_crypto.h - the small set of cryptography the Pico's Wi-Fi needs to join a WPA2 network:
 * SHA-1 (FIPS 180-4), HMAC (RFC 2104), PBKDF2 (RFC 8018), the 802.11 key derivation function
 * built on HMAC-SHA-1, AES-128 (FIPS 197) and AES key unwrapping (RFC 3394).
 *
 * Written from the standards for GammaOS. No allocation, no globals: every call works on what
 * it is given.
 */
#ifndef PICO_CRYPTO_H
#define PICO_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#define PICO_SHA1_LEN	20

struct pico_sha1 {
	uint32_t state[5];
	uint64_t total;			/* bytes taken so far */
	uint8_t block[64];
	unsigned int have;		/* bytes waiting in block */
};

void pico_sha1_start(struct pico_sha1 *c);
void pico_sha1_add(struct pico_sha1 *c, const void *data, size_t len);
void pico_sha1_end(struct pico_sha1 *c, uint8_t out[PICO_SHA1_LEN]);

/* HMAC-SHA-1 in pieces: start with the key, add the message, end with the code */
struct pico_hmac_sha1 {
	struct pico_sha1 inner, outer;
};
void pico_hmac_sha1_start(struct pico_hmac_sha1 *h, const void *key, size_t key_len);
void pico_hmac_sha1_add(struct pico_hmac_sha1 *h, const void *data, size_t len);
void pico_hmac_sha1_end(struct pico_hmac_sha1 *h, uint8_t out[PICO_SHA1_LEN]);
/* the same in one call */
void pico_hmac_sha1(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[PICO_SHA1_LEN]);

/* PBKDF2 with HMAC-SHA-1: out_len bytes from the passphrase and salt after rounds rounds (WPA:
 * the passphrase, the network's name, 4096 rounds, 32 bytes) */
void pico_pbkdf2_sha1(const void *pass, size_t pass_len, const void *salt, size_t salt_len,
		      unsigned int rounds, uint8_t *out, size_t out_len);

/* The 802.11 PRF: out_len bytes of HMAC-SHA-1(key, label 0 data i) for i = 0, 1, ... */
void pico_prf_sha1(const void *key, size_t key_len, const char *label, const void *data, size_t data_len,
		   uint8_t *out, size_t out_len);

/* AES-128 with its round keys worked out once */
struct pico_aes128 {
	uint32_t rk[44];
};
void pico_aes128_key(struct pico_aes128 *a, const uint8_t key[16]);
void pico_aes128_encrypt(const struct pico_aes128 *a, const uint8_t in[16], uint8_t out[16]);
void pico_aes128_decrypt(const struct pico_aes128 *a, const uint8_t in[16], uint8_t out[16]);

/* RFC 3394 key unwrapping with a 128 bit key: in is in_len bytes (a multiple of 8, at least 24),
 * out gets in_len - 8. 0 when the check value matches, -1 when it does not (wrong key or data). */
int pico_aes128_unwrap(const uint8_t kek[16], const uint8_t *in, size_t in_len, uint8_t *out);

/* ---- for TLS 1.3 ---- */

#define PICO_SHA256_LEN	32

struct pico_sha256 {
	uint32_t state[8];
	uint64_t total;
	uint8_t block[64];
	unsigned int have;
};
void pico_sha256_start(struct pico_sha256 *c);
void pico_sha256_add(struct pico_sha256 *c, const void *data, size_t len);
void pico_sha256_end(struct pico_sha256 *c, uint8_t out[PICO_SHA256_LEN]);
/* the hash so far without ending c (a TLS transcript is hashed at several points) */
void pico_sha256_peek(const struct pico_sha256 *c, uint8_t out[PICO_SHA256_LEN]);

void pico_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[PICO_SHA256_LEN]);

/* HKDF with SHA-256 (RFC 5869) */
void pico_hkdf_extract(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len, uint8_t prk[32]);
void pico_hkdf_expand(const uint8_t prk[32], const void *info, size_t info_len, uint8_t *out, size_t out_len);

/* AES-128-GCM (NIST SP 800-38D) with a 12 byte nonce and a 16 byte tag: seal encrypts len bytes
 * of in to out and writes the tag; open checks the tag first (constant time) and decrypts only
 * when it matches: 0, or -1 for a wrong tag. in and out may be the same. */
void pico_gcm128_seal(const struct pico_aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		      const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16]);
int  pico_gcm128_open(const struct pico_aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
		      const uint8_t *in, size_t len, uint8_t *out, const uint8_t tag[16]);

/* X25519 (RFC 7748): out = scalar * point (both 32 bytes, little-endian) */
void pico_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* out = scalar * the base point (9) */
void pico_x25519_base(uint8_t out[32], const uint8_t scalar[32]);

/* SHA-512 and SHA-384 (FIPS 180-4): one engine, 384 starts elsewhere and keeps 48 bytes */
struct pico_sha512 {
	uint64_t state[8];
	uint64_t total;
	uint8_t block[128];
	unsigned int have, out_len;
};
void pico_sha512_start(struct pico_sha512 *c);
void pico_sha384_start(struct pico_sha512 *c);
void pico_sha512_add(struct pico_sha512 *c, const void *data, size_t len);
void pico_sha512_end(struct pico_sha512 *c, uint8_t *out);	/* 64 or 48 bytes */

/* zeroes len bytes in a way the compiler keeps (for keys) */
void pico_wipe(void *p, size_t len);

#endif
