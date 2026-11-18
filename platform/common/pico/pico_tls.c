/*
 * pico_tls.c - see pico_tls.h. Written for GammaOS from RFC 8446 (TLS 1.3) and RFC 5246, 5288,
 * 8422 and 7627 (TLS 1.2); the record and handshake layouts, the key schedules and the labels are
 * the RFCs'.
 */
#include "pico_tls.h"
#include "pico_pki.h"
#include <string.h>

#define REC_CCS		20
#define REC_ALERT	21
#define REC_HANDSHAKE	22
#define REC_APP		23

#define HS_SERVER_HELLO		2
#define HS_NEW_TICKET		4
#define HS_HELLO_REQUEST	0
#define HS_ENCRYPTED_EXT	8
#define HS_CERTIFICATE		11
#define HS_SERVER_KX		12
#define HS_CERT_REQUEST		13
#define HS_HELLO_DONE		14
#define HS_CERT_VERIFY		15
#define HS_CLIENT_KX		16
#define HS_FINISHED		20
#define HS_CERT_STATUS		22
#define HS_KEY_UPDATE		24

#define SUITE_AES128_GCM_SHA256	0x1301		/* TLS 1.3 */
#define SUITE_ECDHE_ECDSA_GCM	0xC02B		/* TLS 1.2: ECDHE, ECDSA, AES-128-GCM, SHA-256 */
#define SUITE_ECDHE_RSA_GCM	0xC02F		/*          ECDHE, RSA, ... */
#define SUITE_RSA_GCM		0x009C		/*          RSA key exchange, AES-128-GCM, SHA-256 */
#define GROUP_X25519		0x001D

static void put16(uint8_t *p, unsigned int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned int get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

static int fail(struct pico_tls *t, const char *why)
{
	if (t->state != PICO_TLS_FAILED) {
		t->state = PICO_TLS_FAILED;
		t->why = why;
	}
	return -1;
}

/* HKDF-Expand-Label (RFC 8446 7.1): "tls13 " + label, the context, out_len bytes */
static void expand_label(const uint8_t secret[32], const char *label, const uint8_t *ctx, unsigned int ctx_len,
			 uint8_t *out, unsigned int out_len)
{
	uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
	unsigned int ll = (unsigned)strlen(label), n = 0;
	put16(info, out_len); n = 2;
	info[n++] = (uint8_t)(6 + ll);
	memcpy(info + n, "tls13 ", 6); n += 6;
	memcpy(info + n, label, ll); n += ll;
	info[n++] = (uint8_t)ctx_len;
	if (ctx_len) memcpy(info + n, ctx, ctx_len);
	n += ctx_len;
	pico_hkdf_expand(secret, info, n, out, out_len);
}

/* Derive-Secret over a transcript hash */
static void derive(const uint8_t secret[32], const char *label, const uint8_t hash[32], uint8_t out[32])
{
	expand_label(secret, label, hash, 32, out, 32);
}

static void empty_hash(uint8_t out[32])
{
	struct pico_sha256 h;
	pico_sha256_start(&h);
	pico_sha256_end(&h, out);
}

/* a traffic secret into its key and IV */
static void traffic_keys(const uint8_t secret[32], struct pico_aes128 *key, uint8_t iv[12])
{
	uint8_t k[16];
	expand_label(secret, "key", 0, 0, k, 16);
	expand_label(secret, "iv", 0, 0, iv, 12);
	pico_aes128_key(key, k);
	pico_wipe(k, sizeof k);
}

static void record_nonce(const uint8_t iv[12], uint64_t seq, uint8_t nonce[12])
{
	int i;
	memcpy(nonce, iv, 12);
	for (i = 0; i < 8; i++)
		nonce[11 - i] ^= (uint8_t)(seq >> (8 * i));
}

/* TLS 1.2's PRF with SHA-256 (RFC 5246 5): out_len bytes of P_SHA256(secret, label | seed) */
static void prf12(const uint8_t *secret, unsigned int slen, const char *label, const uint8_t *seed, unsigned int seed_len,
		  uint8_t *out, unsigned int out_len)
{
	uint8_t a[32 + 32 + 64], block[32];
	unsigned int ll = (unsigned)strlen(label), ls = ll + seed_len;
	/* a holds A(i), then label | seed after it */
	memcpy(a + 32, label, ll);
	memcpy(a + 32 + ll, seed, seed_len);
	pico_hmac_sha256(secret, slen, a + 32, ls, a);		/* A(1) */
	while (out_len) {
		unsigned int take = out_len < 32 ? out_len : 32;
		pico_hmac_sha256(secret, slen, a, 32 + ls, block);
		memcpy(out, block, take);
		out += take;
		out_len -= take;
		pico_hmac_sha256(secret, slen, a, 32, a);	/* A(i+1) */
	}
	pico_wipe(a, sizeof a);
	pico_wipe(block, sizeof block);
}

/* more random bytes than the ClientHello's 64 (TLS 1.2's premaster secret, RSA padding) */
static void more_random(struct pico_tls *t, const char *what, uint8_t *out, unsigned int len)
{
	prf12(t->seed, 32, what, t->c_random, 32, out, len);
}

/* one handshake message of ours: into the transcript, then out */
static int send_handshake(struct pico_tls *t, const uint8_t *msg, unsigned int len);

/* one record out: inner type and content (encrypted once we have our keys) */
static int send_record(struct pico_tls *t, unsigned int type, const uint8_t *data, unsigned int len)
{
	uint8_t *r = t->out, nonce[12];
	unsigned int total;
	if (len > PICO_TLS_OUT_MAX)
		return fail(t, "record too big");
	if (t->c_encrypted && t->v12) {
		/* 1.2 GCM: an 8 byte explicit nonce (our sequence number), the type outside;
		 * additional data: sequence, type, version, plain length */
		uint8_t aad[13];
		int i;
		memmove(r + 13, data, len);
		r[0] = (uint8_t)type;
		put16(r + 1, 0x0303);
		put16(r + 3, 8 + len + 16);
		for (i = 0; i < 8; i++) r[5 + i] = aad[i] = (uint8_t)(t->c_seq >> (56 - 8 * i));
		aad[8] = (uint8_t)type; put16(aad + 9, 0x0303); put16(aad + 11, len);
		memcpy(nonce, t->c_iv, 4);
		memcpy(nonce + 4, r + 5, 8);
		pico_gcm128_seal(&t->c_key, nonce, aad, 13, r + 13, len, r + 13, r + 13 + len);
		t->c_seq++;
		total = 5 + 8 + len + 16;
	} else if (!t->c_encrypted) {
		r[0] = (uint8_t)type;
		/* the ClientHello's record says 1.0 (as servers expect); 1.2 records after it say 1.2 */
		put16(r + 1, type == REC_HANDSHAKE && !t->v12 ? 0x0301 : 0x0303);
		put16(r + 3, len);
		memmove(r + 5, data, len);
		total = 5 + len;
	} else {
		memmove(r + 5, data, len);
		r[5 + len] = (uint8_t)type;		/* the real type rides inside */
		r[0] = REC_APP;
		put16(r + 1, 0x0303);
		put16(r + 3, len + 1 + 16);
		record_nonce(t->c_iv, t->c_seq++, nonce);
		pico_gcm128_seal(&t->c_key, nonce, r, 5, r + 5, len + 1, r + 5, r + 5 + len + 1);
		total = 5 + len + 1 + 16;
	}
	if (t->io.send(t->io.ctx, r, total) != (int)total)
		return fail(t, "the connection broke");
	return 0;
}

static int send_handshake(struct pico_tls *t, const uint8_t *msg, unsigned int len)
{
	pico_sha256_add(&t->transcript, msg, len);
	return send_record(t, REC_HANDSHAKE, msg, len);
}

int pico_tls_start(struct pico_tls *t, const struct pico_tls_io *io, const char *host, const uint8_t entropy[64],
		   const struct pico_tls_trust *trust)
{
	uint8_t *m, pub[32];
	unsigned int n, hl = (unsigned)strlen(host), ext_at, i;
	static const unsigned int sig_algs[] = {
		0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501, 0x0806, 0x0601	/* ECDSA, RSA-PSS, RSA PKCS#1 */
	};
	memset(t, 0, sizeof *t);
	t->io = *io;
	t->host = host;
	if (trust) t->trust = *trust;
	t->state = PICO_TLS_HANDSHAKE;
	if (hl > 255) return fail(t, "host name too long");
	memcpy(t->secret, entropy + 32, 32);		/* our X25519 private key */
	pico_x25519_base(pub, t->secret);
	memcpy(t->c_random, entropy, 32);
	pico_hmac_sha256(entropy, 64, "pico tls seed", 13, t->seed);
	pico_sha256_start(&t->transcript);

	/* ClientHello, built where the record body goes */
	m = t->out + 5 + 512;				/* (send_record moves it into place) */
	n = 4;
	put16(m + n, 0x0303); n += 2;			/* legacy version: TLS 1.2 */
	memcpy(m + n, entropy, 32); n += 32;		/* random */
	m[n++] = 32;					/* a session id, as middleboxes like to see */
	memcpy(m + n, entropy, 16); memcpy(m + n + 16, pub, 16); n += 32;
	put16(m + n, 8); n += 2;			/* our suites, best first */
	put16(m + n, SUITE_AES128_GCM_SHA256); n += 2;
	put16(m + n, SUITE_ECDHE_ECDSA_GCM); n += 2;
	put16(m + n, SUITE_ECDHE_RSA_GCM); n += 2;
	put16(m + n, SUITE_RSA_GCM); n += 2;
	m[n++] = 1; m[n++] = 0;				/* no compression */
	ext_at = n; n += 2;
	/* server name */
	put16(m + n, 0); put16(m + n + 2, hl + 5); put16(m + n + 4, hl + 3); m[n + 6] = 0; put16(m + n + 7, hl);
	memcpy(m + n + 9, host, hl); n += 9 + hl;
	/* supported groups: X25519 for the key exchange; P-256 and P-384 because in TLS 1.2 this list
	 * also says which curves a server's ECDSA certificate may use (RFC 8422 5.1.1) */
	put16(m + n, 10); put16(m + n + 2, 8); put16(m + n + 4, 6); put16(m + n + 6, GROUP_X25519);
	put16(m + n + 8, 0x0017); put16(m + n + 10, 0x0018); n += 12;
	/* signature algorithms */
	put16(m + n, 13); put16(m + n + 2, 2 + 2 * 8); put16(m + n + 4, 2 * 8); n += 6;
	for (i = 0; i < 8; i++) { put16(m + n, sig_algs[i]); n += 2; }
	/* supported versions: TLS 1.3, 1.2 */
	put16(m + n, 43); put16(m + n + 2, 5); m[n + 4] = 4; put16(m + n + 5, 0x0304); put16(m + n + 7, 0x0303); n += 9;
	/* for 1.2: uncompressed points only, the extended master secret, no renegotiation yet */
	put16(m + n, 11); put16(m + n + 2, 2); m[n + 4] = 1; m[n + 5] = 0; n += 6;
	put16(m + n, 23); put16(m + n + 2, 0); n += 4;
	put16(m + n, 0xFF01); put16(m + n + 2, 1); m[n + 4] = 0; n += 5;
	/* key share: our X25519 public key */
	put16(m + n, 51); put16(m + n + 2, 2 + 4 + 32); put16(m + n + 4, 4 + 32); put16(m + n + 6, GROUP_X25519);
	put16(m + n + 8, 32); memcpy(m + n + 10, pub, 32); n += 42;
	/* application protocol: HTTP/1.1 */
	put16(m + n, 16); put16(m + n + 2, 11); put16(m + n + 4, 9); m[n + 6] = 8; memcpy(m + n + 7, "http/1.1", 8); n += 15;
	put16(m + ext_at, n - ext_at - 2);
	m[0] = 1; m[1] = 0; put16(m + 2, n - 4);	/* handshake header: ClientHello */
	return send_handshake(t, m, n);
}

/* ServerHello: its key share, then the handshake keys */
static int server_hello(struct pico_tls *t, const uint8_t *b, unsigned int len)
{
	/* the random a HelloRetryRequest carries instead (SHA-256 of "HelloRetryRequest") */
	static const uint8_t retry[32] = {
		0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
		0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };
	unsigned int at, sid, ext_end, version = 0, suite;
	const uint8_t *key = 0;
	uint8_t shared[32], early[32], derived[32], hs[32], eh[32], th[32];
	static const uint8_t zeros[32];
	if (len < 38) return fail(t, "short ServerHello");
	if (!memcmp(b + 2, retry, 32)) return fail(t, "the server wants a key exchange we do not offer");
	sid = b[34];
	at = 35 + sid;
	if (at + 3 + 2 > len) return fail(t, "short ServerHello");
	suite = get16(b + at);
	at += 3;					/* suite, compression */
	ext_end = at + 2 + get16(b + at);
	at += 2;
	if (ext_end > len) return fail(t, "short ServerHello");
	while (at + 4 <= ext_end) {
		unsigned int type = get16(b + at), elen = get16(b + at + 2);
		const uint8_t *e = b + at + 4;
		if (at + 4 + elen > ext_end) break;
		if (type == 43 && elen == 2) version = get16(e);
		if (type == 51 && elen == 36 && get16(e) == GROUP_X25519 && get16(e + 2) == 32) key = e + 4;
		if (type == 23) t->ems = 1;
		if (type == 0xFF01 && (elen != 1 || e[0])) return fail(t, "bad renegotiation info");
		at += 4 + elen;
	}
	memcpy(t->s_random, b + 2, 32);
	if (version != 0x0304) {
		/* TLS 1.2. A server that could speak 1.3 marks its random when something made it go
		 * down (RFC 8446 4.1.3): then someone changed our ClientHello on the way */
		if (get16(b) != 0x0303) return fail(t, "the server speaks neither TLS 1.3 nor 1.2");
		if (!memcmp(b + 2 + 24, "DOWNGRD", 7) && (b[2 + 31] == 0 || b[2 + 31] == 1))
			return fail(t, "the connection was pushed down from TLS 1.3 (tampered with?)");
		if (suite != SUITE_ECDHE_ECDSA_GCM && suite != SUITE_ECDHE_RSA_GCM && suite != SUITE_RSA_GCM)
			return fail(t, "the server picked a TLS 1.2 cipher we do not have");
		t->v12 = 1;
		t->suite = (int)suite;
		return 0;
	}
	if (suite != SUITE_AES128_GCM_SHA256) return fail(t, "the server picked another cipher");
	if (!key) return fail(t, "no key share from the server");

	pico_x25519(shared, t->secret, key);
	empty_hash(eh);
	pico_hkdf_extract(0, 0, zeros, 32, early);
	derive(early, "derived", eh, derived);
	pico_hkdf_extract(derived, 32, shared, 32, hs);
	pico_sha256_peek(&t->transcript, th);
	derive(hs, "c hs traffic", th, t->c_secret);
	derive(hs, "s hs traffic", th, t->s_secret);
	memcpy(t->secret, hs, 32);			/* the handshake secret, for the master secret later */
	traffic_keys(t->s_secret, &t->s_key, t->s_iv);
	traffic_keys(t->c_secret, &t->c_key, t->c_iv);
	t->s_encrypted = 1;
	t->s_seq = t->c_seq = 0;
	pico_wipe(shared, sizeof shared);
	pico_wipe(early, sizeof early);
	pico_wipe(hs, sizeof hs);
	return 0;
}

/* the server's Finished checked: our Finished, then the application keys */
static int server_finished(struct pico_tls *t, const uint8_t *b, unsigned int len)
{
	uint8_t th[32], eh[32], derived[32], master[32], fk[32], msg[4 + 32], c_ap[32], diff = 0;
	static const uint8_t zeros[32];
	static const uint8_t ccs[1] = { 1 };
	int i;
	if (len != 32) return fail(t, "bad Finished");
	for (i = 0; i < 32; i++) diff |= b[i] ^ t->s_finished_want[i];
	if (diff) return fail(t, "the server's Finished does not match (handshake tampered with?)");
	pico_sha256_peek(&t->transcript, th);		/* ClientHello .. server Finished */
	empty_hash(eh);
	derive(t->secret, "derived", eh, derived);
	pico_hkdf_extract(derived, 32, zeros, 32, master);
	derive(master, "c ap traffic", th, c_ap);
	derive(master, "s ap traffic", th, t->s_secret);
	/* our Finished, under the client handshake keys (a ChangeCipherSpec first, for middleboxes) */
	expand_label(t->c_secret, "finished", 0, 0, fk, 32);
	msg[0] = HS_FINISHED; msg[1] = 0; msg[2] = 0; msg[3] = 32;
	pico_hmac_sha256(fk, 32, th, 32, msg + 4);
	t->c_encrypted = 0;
	if (send_record(t, REC_CCS, ccs, 1) != 0) return -1;
	t->c_encrypted = 1;
	if (send_record(t, REC_HANDSHAKE, msg, sizeof msg) != 0) return -1;
	/* from here on the application keys */
	memcpy(t->c_secret, c_ap, 32);
	traffic_keys(t->c_secret, &t->c_key, t->c_iv);
	traffic_keys(t->s_secret, &t->s_key, t->s_iv);
	t->c_seq = t->s_seq = 0;
	t->keys_app = 1;
	t->state = PICO_TLS_OPEN;
	pico_wipe(master, sizeof master);
	pico_wipe(fk, sizeof fk);
	pico_wipe(c_ap, sizeof c_ap);
	pico_wipe(t->secret, sizeof t->secret);
	return 0;
}

/* KeyUpdate: the server's next secret; when asked, ours too */
static int key_update(struct pico_tls *t, const uint8_t *b, unsigned int len)
{
	uint8_t next[32], msg[5];
	if (len != 1) return fail(t, "bad KeyUpdate");
	expand_label(t->s_secret, "traffic upd", 0, 0, next, 32);
	memcpy(t->s_secret, next, 32);
	traffic_keys(t->s_secret, &t->s_key, t->s_iv);
	t->s_seq = 0;
	if (b[0] == 1) {
		msg[0] = HS_KEY_UPDATE; msg[1] = 0; msg[2] = 0; msg[3] = 1; msg[4] = 0;
		if (send_record(t, REC_HANDSHAKE, msg, sizeof msg) != 0) return -1;
		expand_label(t->c_secret, "traffic upd", 0, 0, next, 32);
		memcpy(t->c_secret, next, 32);
		traffic_keys(t->c_secret, &t->c_key, t->c_iv);
		t->c_seq = 0;
	}
	pico_wipe(next, sizeof next);
	return 0;
}

/* the server's certificates from its Certificate message (1.3: a context byte and extensions per
 * certificate; 1.2: neither): 0 and *n parsed, or -1 */
static struct pki_cert s_chain[6];
static int parse_chain(struct pico_tls *t, int *n)
{
	unsigned int at = t->v12 ? 0 : 1u + t->certs[0], list_end;
	*n = 0;
	if (t->certs_len < 4 || at + 3 > t->certs_len) return fail(t, "no certificate from the server");
	list_end = at + 3 + ((unsigned)t->certs[at] << 16 | (unsigned)t->certs[at + 1] << 8 | t->certs[at + 2]);
	at += 3;
	if (list_end > t->certs_len) return fail(t, "bad certificate list");
	while (at + 3 <= list_end && *n < 6) {
		unsigned int cl = (unsigned)t->certs[at] << 16 | (unsigned)t->certs[at + 1] << 8 | t->certs[at + 2];
		if (at + 3 + cl + (t->v12 ? 0 : 2) > list_end) return fail(t, "bad certificate list");
		if (pki_cert_parse(t->certs + at + 3, cl, &s_chain[*n]) != 0) return fail(t, "a certificate we cannot read");
		(*n)++;
		at += 3 + cl;
		if (!t->v12) at += 2 + get16(t->certs + at);	/* its extensions */
	}
	return *n ? 0 : fail(t, "no certificate from the server");
}

/* a signature by the server's own key (the leaf's) over the hash of content: 0 it checks */
static int leaf_signed(const struct pki_cert *leaf, unsigned int alg, const uint8_t *content, unsigned int clen,
		       const uint8_t *sig, unsigned int sl)
{
	uint8_t digest[64];
	int hash = (alg >> 8) == 4 || alg == 0x0804 ? PKI_HASH_SHA256 : (alg >> 8) == 5 || alg == 0x0805 ? PKI_HASH_SHA384 :
		   (alg >> 8) == 6 || alg == 0x0806 ? PKI_HASH_SHA512 : PKI_HASH_NONE;
	if (hash == PKI_HASH_NONE) return -1;
	pki_hash(hash, content, clen, digest);
	if ((alg & 0xFF) == 0x03)			/* ECDSA */
		return (leaf->key_kind == PKI_KEY_EC_P256 || leaf->key_kind == PKI_KEY_EC_P384) ?
		       pki_ecdsa(leaf->key_kind, leaf->key_point, leaf->key_point_len, sig, sl, digest, pki_hash_len(hash)) : -1;
	if (leaf->key_kind != PKI_KEY_RSA) return -1;
	if ((alg & 0xFF) == 0x01)			/* RSA PKCS#1 v1.5 (TLS 1.2 only) */
		return pki_rsa_pkcs1(leaf->key_n, leaf->key_n_len, leaf->key_e, leaf->key_e_len, sig, sl, hash, digest);
	if (alg >= 0x0804 && alg <= 0x0806)		/* RSA-PSS */
		return pki_rsa_pss(leaf->key_n, leaf->key_n_len, leaf->key_e, leaf->key_e_len, sig, sl, hash, digest);
	return -1;
}

/* TLS 1.3: the server's chain and its CertificateVerify signature (RFC 8446 4.4.3) */
static int certificate_verify(struct pico_tls *t, const uint8_t *b, unsigned int len)
{
	static uint8_t content[64 + 34 + 32];
	unsigned int alg, sl;
	int n;
	const char *why;
	if (!t->trust.roots) return 0;
	if (parse_chain(t, &n) != 0) return -1;
	why = pki_chain_check(s_chain, n, t->host, t->trust.now, t->trust.roots, t->trust.roots_len);
	if (why) return fail(t, why);
	if (len < 4) return fail(t, "bad CertificateVerify");
	alg = get16(b);
	sl = get16(b + 2);
	if (4 + sl != len || (alg & 0xFF) == 0x01) return fail(t, "bad CertificateVerify");
	memset(content, 0x20, 64);
	memcpy(content + 64, "TLS 1.3, server CertificateVerify", 33);
	content[97] = 0;
	memcpy(content + 98, t->cv_hash, 32);
	if (leaf_signed(&s_chain[0], alg, content, sizeof content, b + 4, sl) != 0)
		return fail(t, "the server's handshake signature does not check");
	t->cv_done = 1;
	return 0;
}

/* TLS 1.2, at ServerHelloDone: the chain, the server's signed key, then our flight: our key
 * (ClientKeyExchange), the master secret and keys, ChangeCipherSpec, Finished */
static int client_flight12(struct pico_tls *t)
{
	static uint8_t msg[4 + 2 + 512], em[512], signed_part[64 + 4 + 97];
	static const uint8_t ccs[1] = { 1 };
	uint8_t pms[48], seed[64], keys[40], th[32], fin[4 + 12];
	unsigned int pms_len, ml, i;
	int n;
	const struct pki_cert *leaf;
	if (parse_chain(t, &n) != 0) return -1;
	leaf = &s_chain[0];
	if (t->trust.roots) {
		const char *why = pki_chain_check(s_chain, n, t->host, t->trust.now, t->trust.roots, t->trust.roots_len);
		if (why) return fail(t, why);
	}
	if (t->suite != SUITE_RSA_GCM) {
		/* ServerKeyExchange: curve type 3 (named), the group, its key, then the signature over
		 * both randoms and those parameters. X25519, P-256 or P-384, as the server chose. */
		const uint8_t *k = t->ske;
		unsigned int alg, sl, group, kl, params;
		if (t->ske_len < 4 || k[0] != 3) return fail(t, "the server's key is not one we can use");
		group = get16(k + 1);
		kl = k[3];
		params = 4 + kl;
		if (t->ske_len < params + 4) return fail(t, "bad ServerKeyExchange");
		alg = get16(k + params);
		sl = get16(k + params + 2);
		if (params + 4 + sl != t->ske_len) return fail(t, "bad ServerKeyExchange");
		if (t->trust.roots) {
			if (64 + params > sizeof signed_part) return fail(t, "bad ServerKeyExchange");
			memcpy(signed_part, t->c_random, 32);
			memcpy(signed_part + 32, t->s_random, 32);
			memcpy(signed_part + 64, k, params);
			if (leaf_signed(leaf, alg, signed_part, 64 + params, k + params + 4, sl) != 0)
				return fail(t, "the server's key signature does not check");
		}
		if (group == GROUP_X25519 && kl == 32) {
			pico_x25519(pms, t->secret, k + 4);
			pms_len = 32;
			pico_x25519_base(msg + 5, t->secret);
			msg[4] = 32;
			ml = 4 + 1 + 32;
		} else if (group == 0x0017 || group == 0x0018) {
			int curve = group == 0x0017 ? PKI_KEY_EC_P256 : PKI_KEY_EC_P384;
			uint8_t priv[48], seedb[48];
			size_t pl;
			more_random(t, "ecdhe", seedb, sizeof seedb);
			if (pki_ec_keypair(curve, seedb, sizeof seedb, priv, msg + 5, &pl) != 0 ||
			    pki_ec_shared(curve, priv, k + 4, kl, pms) != 0)
				return fail(t, "the server's key is not a point on its curve");
			pms_len = curve == PKI_KEY_EC_P256 ? 32 : 48;
			msg[4] = (uint8_t)pl;
			ml = 4 + 1 + (unsigned)pl;
			pico_wipe(priv, sizeof priv);
			pico_wipe(seedb, sizeof seedb);
		} else {
			return fail(t, "the server chose a key exchange group we do not have");
		}
	} else {
		/* RSA key exchange: 48 secret bytes (03 03, then random), PKCS#1 v1.5 type 2 padded,
		 * encrypted to the server's key */
		unsigned int k = (unsigned)leaf->key_n_len, ps;
		if (leaf->key_kind != PKI_KEY_RSA || k < 64 || k > sizeof em) return fail(t, "the server's key is not RSA");
		pms[0] = 3; pms[1] = 3;
		more_random(t, "premaster", pms + 2, 46);
		pms_len = 48;
		ps = k - 3 - 48;
		em[0] = 0; em[1] = 2;
		more_random(t, "padding", em + 2, ps);
		for (i = 0; i < ps; i++)			/* the padding has no zero byte */
			if (!em[2 + i]) em[2 + i] = (uint8_t)(0x5A + i);
		em[2 + ps] = 0;
		memcpy(em + 3 + ps, pms, 48);
		put16(msg + 4, k);
		if (pki_rsa_public(leaf->key_n, k, leaf->key_e, leaf->key_e_len, em, k, msg + 6) != 0)
			return fail(t, "could not encrypt to the server's key");
		ml = 4 + 2 + k;
	}
	msg[0] = HS_CLIENT_KX; msg[1] = 0; put16(msg + 2, ml - 4);
	if (send_handshake(t, msg, ml) != 0) return -1;
	/* the master secret: over the transcript so far (RFC 7627) when the server has that,
	 * else over both randoms */
	if (t->ems) {
		pico_sha256_peek(&t->transcript, th);
		prf12(pms, pms_len, "extended master secret", th, 32, t->master, 48);
	} else {
		memcpy(seed, t->c_random, 32);
		memcpy(seed + 32, t->s_random, 32);
		prf12(pms, pms_len, "master secret", seed, 64, t->master, 48);
	}
	memcpy(seed, t->s_random, 32);
	memcpy(seed + 32, t->c_random, 32);
	prf12(t->master, 48, "key expansion", seed, 64, keys, sizeof keys);
	pico_aes128_key(&t->c_key, keys);
	pico_aes128_key(&t->s_key, keys + 16);
	memcpy(t->c_iv, keys + 32, 4);
	memcpy(t->s_iv, keys + 36, 4);
	pico_wipe(pms, sizeof pms);
	pico_wipe(keys, sizeof keys);
	/* ChangeCipherSpec, then our Finished under the new keys */
	if (send_record(t, REC_CCS, ccs, 1) != 0) return -1;
	t->c_encrypted = 1;
	t->c_seq = 0;
	pico_sha256_peek(&t->transcript, th);
	fin[0] = HS_FINISHED; fin[1] = 0; fin[2] = 0; fin[3] = 12;
	prf12(t->master, 48, "client finished", th, 32, fin + 4, 12);
	return send_handshake(t, fin, sizeof fin);
}

static int handshake_message(struct pico_tls *t, unsigned int type, const uint8_t *b, unsigned int len)
{
	if (t->v12 && !t->keys_app) {
		switch (type) {
		case HS_CERTIFICATE:
		case HS_CERT_STATUS:
		case HS_NEW_TICKET:
			return 0;
		case HS_SERVER_KX:
			if (len > sizeof t->ske) return fail(t, "ServerKeyExchange too big");
			memcpy(t->ske, b, len);
			t->ske_len = len;
			return 0;
		case HS_CERT_REQUEST:
			return fail(t, "the server wants a client certificate");
		case HS_HELLO_DONE:
			return client_flight12(t);
		case HS_FINISHED: {
			uint8_t diff = 0;
			int i;
			if (len != 12 || !t->s_encrypted) return fail(t, "bad Finished");
			for (i = 0; i < 12; i++) diff |= b[i] ^ t->s_finished_want[i];
			if (diff) return fail(t, "the server's Finished does not match (handshake tampered with?)");
			t->keys_app = 1;
			t->state = PICO_TLS_OPEN;
			t->verified = t->trust.roots != 0;
			pico_wipe(t->secret, sizeof t->secret);
			return 0;
		}
		default:
			return fail(t, "unexpected handshake message");
		}
	}
	if (t->v12) {					/* after the handshake: no renegotiation */
		if (type == HS_HELLO_REQUEST || type == HS_NEW_TICKET) return 0;
		return fail(t, "unexpected handshake message");
	}
	if (!t->keys_app) {
		switch (type) {
		case HS_SERVER_HELLO:
			if (t->s_encrypted) return fail(t, "a second ServerHello");
			return server_hello(t, b, len);
		case HS_ENCRYPTED_EXT:
		case HS_CERTIFICATE:
			return t->s_encrypted ? 0 : fail(t, "unexpected handshake message");
		case HS_CERT_VERIFY:
			if (!t->s_encrypted) return fail(t, "unexpected handshake message");
			return certificate_verify(t, b, len);
		case HS_CERT_REQUEST:
			return fail(t, "the server wants a client certificate");
		case HS_FINISHED:
			if (t->trust.roots && !t->cv_done) return fail(t, "the server proved nothing about itself");
			if (server_finished(t, b, len) != 0) return -1;
			t->verified = t->cv_done;
			return 0;
		default:
			return fail(t, "unexpected handshake message");
		}
	}
	if (type == HS_NEW_TICKET) return 0;		/* no resumption: tickets are not kept */
	if (type == HS_KEY_UPDATE) return key_update(t, b, len);
	return fail(t, "unexpected handshake message");
}

/* handshake bytes from a record: messages put together across records, each hashed into the
 * transcript (until the application keys) as it passes */
static int handshake_bytes(struct pico_tls *t, const uint8_t *p, unsigned int len)
{
	while (len) {
		unsigned int take;
		if (t->hs_hdr_have < 4) {
			t->hs_hdr[t->hs_hdr_have++] = *p++;
			len--;
			if (t->hs_hdr_have < 4)
				continue;
			t->hs_body_len = (unsigned)t->hs_hdr[1] << 16 | (unsigned)t->hs_hdr[2] << 8 | t->hs_hdr[3];
			t->hs_body_have = 0;
			if (t->hs_hdr[0] == HS_CERT_VERIFY && !t->keys_app)
				pico_sha256_peek(&t->transcript, t->cv_hash);	/* signed: everything before it */
			if (t->hs_hdr[0] == HS_CERTIFICATE && !t->keys_app) {
				t->certs_len = 0;
				if (t->trust.roots && t->hs_body_len > sizeof t->certs)
					return fail(t, "the server's certificates are too big");
			}
			if (t->hs_hdr[0] == HS_FINISHED && !t->keys_app) {
				/* what the server's Finished must be: over the transcript before it */
				uint8_t th[32], fk[32];
				pico_sha256_peek(&t->transcript, th);
				if (t->v12) {
					prf12(t->master, 48, "server finished", th, 32, t->s_finished_want, 12);
				} else {
					expand_label(t->s_secret, "finished", 0, 0, fk, 32);
					pico_hmac_sha256(fk, 32, th, 32, t->s_finished_want);
					pico_wipe(fk, sizeof fk);
				}
			}
			if (!t->keys_app)
				pico_sha256_add(&t->transcript, t->hs_hdr, 4);
			if (t->hs_hdr[0] != HS_CERTIFICATE && t->hs_hdr[0] != HS_NEW_TICKET &&
			    t->hs_hdr[0] != HS_CERT_VERIFY && t->hs_hdr[0] != HS_ENCRYPTED_EXT &&
			    t->hs_body_len > sizeof t->hs_small)
				return fail(t, "handshake message too big");
		}
		take = t->hs_body_len - t->hs_body_have;
		if (take > len) take = len;
		if (take) {
			if (!t->keys_app)
				pico_sha256_add(&t->transcript, p, take);
			if (t->hs_body_have + take <= sizeof t->hs_small)
				memcpy(t->hs_small + t->hs_body_have, p, take);
			if (t->hs_hdr[0] == HS_CERTIFICATE && !t->keys_app && t->hs_body_have + take <= sizeof t->certs) {
				memcpy(t->certs + t->hs_body_have, p, take);
				t->certs_len = t->hs_body_have + take;
			}
			t->hs_body_have += take;
			p += take;
			len -= take;
		}
		if (t->hs_body_have == t->hs_body_len) {
			unsigned int type = t->hs_hdr[0], blen = t->hs_body_len;
			t->hs_hdr_have = 0;
			if (handshake_message(t, type, t->hs_small, blen > sizeof t->hs_small ? 0 : blen) != 0)
				return -1;
		}
	}
	return 0;
}

/* one whole record in t->rec */
static int record(struct pico_tls *t)
{
	unsigned int type = t->rec[0], len = get16(t->rec + 3), inner = type;
	uint8_t *body = t->rec + 5, nonce[12];
	if (type == REC_CCS && !t->keys_app) {
		if (t->v12) {				/* 1.2: the server's keys from here on */
			if (len != 1 || body[0] != 1 || !t->c_encrypted || t->s_encrypted)
				return fail(t, "unexpected ChangeCipherSpec");
			t->s_encrypted = 1;
			t->s_seq = 0;
		}
		return 0;				/* 1.3: middlebox compatibility, ignored */
	}
	if (t->s_encrypted && t->v12) {
		/* 1.2 GCM: explicit nonce (8), data, tag (16); the type is the record's own */
		uint8_t aad[13];
		int i;
		if (len < 8 + 16 || (type != REC_HANDSHAKE && type != REC_ALERT && type != REC_APP))
			return fail(t, "bad record");
		for (i = 0; i < 8; i++) aad[i] = (uint8_t)(t->s_seq >> (56 - 8 * i));
		aad[8] = (uint8_t)type; aad[9] = t->rec[1]; aad[10] = t->rec[2]; put16(aad + 11, len - 24);
		memcpy(nonce, t->s_iv, 4);
		memcpy(nonce + 4, body, 8);
		if (pico_gcm128_open(&t->s_key, nonce, aad, 13, body + 8, len - 24, body + 8, body + len - 16) != 0)
			return fail(t, "a record failed its check");
		t->s_seq++;
		body += 8;
		len -= 24;
	} else if (t->s_encrypted) {
		if (type != REC_APP || len < 17) return fail(t, "unprotected record after the handshake began");
		record_nonce(t->s_iv, t->s_seq++, nonce);
		if (pico_gcm128_open(&t->s_key, nonce, t->rec, 5, body, len - 16, body, body + len - 16) != 0)
			return fail(t, "a record failed its check");
		len -= 16;
		while (len && !body[len - 1]) len--;	/* padding */
		if (!len) return fail(t, "empty record");
		inner = body[--len];
	} else if (type != REC_HANDSHAKE && type != REC_ALERT) {
		return fail(t, "unexpected record");
	}
	if (inner == REC_HANDSHAKE)
		return handshake_bytes(t, body, len);
	if (inner == REC_ALERT) {
		if (len < 2) return fail(t, "bad alert");
		t->alert = body[1];
		if (body[1] == 0) { t->state = PICO_TLS_CLOSED; return 0; }	/* close_notify */
		return fail(t, "the server sent an alert");
	}
	if (inner == REC_APP && t->keys_app) {
		t->app = body;
		t->app_len = len;
		return 0;
	}
	return fail(t, "unexpected record");
}

int pico_tls_step(struct pico_tls *t)
{
	while (t->state == PICO_TLS_HANDSHAKE || (t->state == PICO_TLS_OPEN && !t->app_len)) {
		unsigned int need = t->rec_have < 5 ? 5 : 5 + get16(t->rec + 3);
		int n;
		if (t->rec_have >= 5 && need - 5 > PICO_TLS_RECORD_MAX)
			return fail(t, "record too big"), t->state;
		n = t->io.recv(t->io.ctx, t->rec + t->rec_have, need - t->rec_have);
		if (n < 0) {
			if (t->state == PICO_TLS_OPEN) t->state = PICO_TLS_CLOSED;	/* gone without a goodbye */
			else fail(t, "the connection closed during the handshake");
			break;
		}
		if (!n) break;
		t->rec_have += (unsigned)n;
		if (t->rec_have < 5 || t->rec_have < 5 + get16(t->rec + 3))
			continue;
		t->rec_have = 0;
		if (record(t) != 0) break;
	}
	return t->state;
}

int pico_tls_send(struct pico_tls *t, const void *data, unsigned int len)
{
	const uint8_t *p = (const uint8_t *)data;
	unsigned int sent = 0;
	if (t->state != PICO_TLS_OPEN) return -1;
	while (sent < len) {
		unsigned int n = len - sent > PICO_TLS_OUT_MAX ? PICO_TLS_OUT_MAX : len - sent;
		if (send_record(t, REC_APP, p + sent, n) != 0) return -1;
		sent += n;
	}
	return (int)sent;
}

int pico_tls_recv(struct pico_tls *t, void *buf, unsigned int cap)
{
	unsigned int n;
	if (!t->app_len) {
		pico_tls_step(t);
		if (!t->app_len)
			return t->state == PICO_TLS_OPEN || t->state == PICO_TLS_HANDSHAKE ? 0 : -1;
	}
	n = t->app_len < cap ? t->app_len : cap;
	memcpy(buf, t->app, n);
	t->app += n;
	t->app_len -= n;
	return (int)n;
}

void pico_tls_close(struct pico_tls *t)
{
	static const uint8_t bye[2] = { 1, 0 };		/* warning, close_notify */
	if (t->state == PICO_TLS_OPEN)
		send_record(t, REC_ALERT, bye, 2);
	if (t->state == PICO_TLS_OPEN) t->state = PICO_TLS_CLOSED;
	pico_wipe(t->c_secret, sizeof t->c_secret);
	pico_wipe(t->s_secret, sizeof t->s_secret);
	pico_wipe(&t->c_key, sizeof t->c_key);
	pico_wipe(&t->s_key, sizeof t->s_key);
}
