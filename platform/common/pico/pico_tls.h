/*
 * pico_tls.h - a TLS client for the Pico's downloads, TLS 1.3 (RFC 8446) and TLS 1.2 (RFC 5246),
 * the server picks. 1.3: TLS_AES_128_GCM_SHA256 with X25519. 1.2: ECDHE (X25519) with ECDSA or
 * RSA certificates and AES-128-GCM (RFC 5288, 8422), or RSA key exchange with AES-128-GCM for
 * old servers; the extended master secret (RFC 7627) when the server has it, no renegotiation.
 * Server name indication. It runs over any byte stream through two callbacks (the console's TCP,
 * or a socket when tested on a PC).
 *
 * What is checked: the server's Finished message (the handshake was not changed on the way).
 * Given trusted roots, also the server's certificate chain (pico_pki), its host name, the dates
 * (when the clock is known) and its CertificateVerify signature: any failure ends the
 * connection, and t->verified says it all held. Without roots the connection is private but its
 * server is not proven (verified stays 0).
 */
#ifndef PICO_TLS_H
#define PICO_TLS_H

#include <stdint.h>
#include "pico_crypto.h"

/* the byte stream under the connection: send takes len bytes (how many it took, -1 broken);
 * recv gives up to cap bytes (how many, 0 none waiting yet, -1 closed) */
struct pico_tls_io {
	int (*send)(void *ctx, const uint8_t *data, unsigned int len);
	int (*recv)(void *ctx, uint8_t *buf, unsigned int cap);
	void *ctx;
};

enum { PICO_TLS_HANDSHAKE, PICO_TLS_OPEN, PICO_TLS_CLOSED, PICO_TLS_FAILED };

/* what to trust: DER root certificates one after another, and the time now (seconds since 1970
 * UTC, 0 unknown: dates are then not checked) */
struct pico_tls_trust {
	const uint8_t *roots;
	unsigned int roots_len;
	long long now;
};

#define PICO_TLS_CERTS_MAX	16384		/* the server's certificate list kept for checking */

#define PICO_TLS_RECORD_MAX	(16384 + 256)	/* largest encrypted record body allowed */
#define PICO_TLS_OUT_MAX	4096		/* largest plaintext we put in one record */

struct pico_tls {
	struct pico_tls_io io;
	int state;
	const char *why;			/* what failed, when FAILED */
	uint8_t alert;				/* the alert the server sent, 0 none */

	struct pico_sha256 transcript;
	uint8_t secret[32];			/* our ECDHE private key, then the handshake/master secrets */
	uint8_t c_secret[32], s_secret[32];	/* the traffic secrets in use */
	struct pico_aes128 c_key, s_key;
	uint8_t c_iv[12], s_iv[12];
	uint64_t c_seq, s_seq;
	int s_encrypted, c_encrypted;		/* records are protected (after ServerHello) */
	int keys_app;				/* the application keys are in use */
	uint8_t s_finished_want[32];

	/* a record coming in */
	uint8_t rec[5 + PICO_TLS_RECORD_MAX];
	unsigned int rec_have;

	/* handshake messages being read across records: the 4 byte header, then the body */
	uint8_t hs_hdr[4];
	unsigned int hs_hdr_have, hs_body_len, hs_body_have;
	uint8_t hs_small[1024];			/* bodies we read (ServerHello, CertificateVerify, Finished, KeyUpdate) */

	struct pico_tls_trust trust;		/* roots 0: no certificate checks */
	const char *host;
	uint8_t certs[PICO_TLS_CERTS_MAX];	/* the Certificate message's body */
	unsigned int certs_len;
	uint8_t cv_hash[32];			/* the transcript up to CertificateVerify */
	int cv_done;

	/* decrypted application data waiting to be read */
	uint8_t *app;
	unsigned int app_len;

	uint8_t out[5 + 8 + PICO_TLS_OUT_MAX + 1 + 16];

	/* TLS 1.2 */
	int v12, suite, ems;			/* the server chose 1.2, its suite, extended master secret */
	uint8_t c_random[32], s_random[32];
	uint8_t master[48];
	uint8_t seed[32];			/* for the random bytes 1.2 needs beyond the ClientHello's */
	uint8_t ske[600];			/* ServerKeyExchange, checked at ServerHelloDone */
	unsigned int ske_len;
	int verified;				/* the chain, the name and the handshake signature all checked */
};

/* starts a connection to host: entropy is 64 unguessable bytes (our random and key) */
int pico_tls_start(struct pico_tls *t, const struct pico_tls_io *io, const char *host, const uint8_t entropy[64],
		   const struct pico_tls_trust *trust);
/* reads what has arrived and moves the connection on: its state (PICO_TLS_*) */
int pico_tls_step(struct pico_tls *t);
/* sends len bytes (in records of up to PICO_TLS_OUT_MAX): how many went, -1 broken */
int pico_tls_send(struct pico_tls *t, const void *data, unsigned int len);
/* takes up to cap decrypted bytes: how many; 0 none waiting; -1 none and the server is done */
int pico_tls_recv(struct pico_tls *t, void *buf, unsigned int cap);
/* says goodbye (close_notify) */
void pico_tls_close(struct pico_tls *t);

#endif
