/*
 * pico_pki.h - what checking a TLS server's certificates takes: big number arithmetic for RSA,
 * RSA signature checks (PKCS#1 v1.5 and PSS, RFC 8017), a DER reader and an X.509 certificate
 * parser (RFC 5280). Written for GammaOS from those standards.
 */
#ifndef PICO_PKI_H
#define PICO_PKI_H

#include <stdint.h>
#include <stddef.h>

/* hashes a signature can use */
enum { PKI_HASH_NONE, PKI_HASH_SHA256, PKI_HASH_SHA384, PKI_HASH_SHA512 };
unsigned int pki_hash_len(int hash);
void pki_hash(int hash, const uint8_t *data, size_t len, uint8_t *out);

/* RSA (modulus n of n_len bytes, big-endian; exponent e likewise): does sig (n_len bytes) sign
 * the message whose hash is digest? 0 yes, -1 no. RSA keys up to 4096 bits. */
int pki_rsa_pkcs1(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
		  size_t sig_len, int hash, const uint8_t *digest);
int pki_rsa_pss(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *sig,
		size_t sig_len, int hash, const uint8_t *digest);

/* the raw RSA public operation: out = in^e mod n, all n_len bytes big-endian (in below n): 0,
 * -1 when in is not below n (TLS 1.2's RSA key exchange encrypts with it) */
int pki_rsa_public(const uint8_t *n, size_t n_len, const uint8_t *e, size_t e_len, const uint8_t *in, size_t in_len,
		   uint8_t *out);

/* ECDSA (P-256 or P-384, PKI_KEY_EC_*): does sig (DER: SEQUENCE { r, s }) sign the message whose
 * hash (hash_len bytes) is digest, under the public point (04 x y)? 0 yes, -1 no. */
int pki_ecdsa(int curve, const uint8_t *point, size_t point_len, const uint8_t *sig, size_t sig_len,
	      const uint8_t *digest, size_t hash_len);

/* ECDH on P-256 or P-384 (PKI_KEY_EC_*), for TLS 1.2 key exchange: a key pair from seed (at least
 * the curve's size of unguessable bytes): priv (32 or 48 bytes) and pub (04 x y, *pub_len); and
 * the shared secret (x of priv . peer, 32 or 48 bytes) once the peer's point is checked to be on
 * the curve. 0, or -1. */
int pki_ec_keypair(int curve, const uint8_t *seed, size_t seed_len, uint8_t *priv, uint8_t *pub, size_t *pub_len);
int pki_ec_shared(int curve, const uint8_t *priv, const uint8_t *peer, size_t peer_len, uint8_t *out);

/* ---- DER ---- */
struct der {
	const uint8_t *p;	/* the content */
	size_t len;
	unsigned int tag;	/* the element's tag byte */
	const uint8_t *whole;	/* tag, length and content */
	size_t whole_len;
};
/* the next element of in (in moves past it): 0, or -1 when nothing (well formed) is left */
int der_next(struct der *in, struct der *out);

/* ---- X.509 ---- */
enum { PKI_KEY_NONE, PKI_KEY_RSA, PKI_KEY_EC_P256, PKI_KEY_EC_P384 };
enum { PKI_SIG_NONE, PKI_SIG_RSA_PKCS1, PKI_SIG_RSA_PSS, PKI_SIG_ECDSA };

struct pki_cert {
	struct der tbs;				/* the signed part (whole: its bytes as signed) */
	int sig_kind, sig_hash;			/* how the issuer signed it */
	const uint8_t *sig;			/* the signature (without the bit string's pad byte) */
	size_t sig_len;
	struct der issuer, subject;		/* names, compared byte for byte */
	long long not_before, not_after;	/* seconds since 1970 UTC */
	int key_kind;
	const uint8_t *key_n, *key_e;		/* RSA: modulus, exponent (leading zeros dropped) */
	size_t key_n_len, key_e_len;
	const uint8_t *key_point;		/* EC: the public point (04 || x || y) */
	size_t key_point_len;
	struct der san;				/* the subject alternative names' content, len 0 none */
	struct der cn;				/* the subject's common name, len 0 none */
	int is_ca, path_len;			/* basic constraints (path_len -1: no limit) */
	int key_usage;				/* the key usage bits, -1 when absent */
};

/* parses one DER certificate: 0, or -1 when it is not one we can read */
int pki_cert_parse(const uint8_t *der, size_t len, struct pki_cert *c);
/* does issuer's key sign c? 0 yes, -1 no */
int pki_cert_signed_by(const struct pki_cert *c, const struct pki_cert *issuer);
/* is host (lower case) one of c's names (wildcards one label deep)? 1 yes */
int pki_cert_names(const struct pki_cert *c, const char *host);

/* A server's chain (chain[0] its own certificate, then the ones that sign it) checked for host:
 * every link signed by the next by a CA allowed to sign, the leaf named host, each in date at now
 * (seconds since 1970 UTC; 0: the time is not known, not checked), and the chain ending in one of
 * roots (DER certificates one after another) or signed by one. 0 when it all holds, else what
 * failed. */
const char *pki_chain_check(const struct pki_cert *chain, int n, const char *host, long long now,
			    const uint8_t *roots, size_t roots_len);

#endif
