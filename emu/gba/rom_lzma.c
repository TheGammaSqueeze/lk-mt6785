/*
 * rom_lzma.c - LZMA and LZMA2 streams unpacked into memory (rom_lzma.h).
 *
 * LZMA is a range coder over an adaptive model: each decision (a literal or a match, which of the
 * four recent distances, the bits of a length or a distance) is a bit coded against an 11 bit
 * probability that moves toward what it sees. The model is laid out in one array of
 * probabilities (the RL_* offsets below). LZMA2 cuts an LZMA stream into chunks, each either
 * stored or LZMA, and says per chunk whether the model, its properties or the dictionary start
 * afresh. The output buffer is the dictionary: a match copies from bytes already written there.
 */
#include <string.h>
#include "rom_lzma.h"

#define RL_STATES     12
#define RL_POS_STATES 16                  /* at most 1 << pb, pb <= 4 */
#define RL_LEN_SIZE   (2 + 16 * 8 + 16 * 8 + 256)
/* where each part of the model starts in the probabilities */
#define RL_MATCH      0                                   /* [state][pos state]: literal or match */
#define RL_REP        (RL_MATCH + RL_STATES * RL_POS_STATES)  /* [state]: a new distance or a recent one */
#define RL_REP0       (RL_REP + RL_STATES)                /* [state]: the latest distance or an older */
#define RL_REP1       (RL_REP0 + RL_STATES)
#define RL_REP2       (RL_REP1 + RL_STATES)
#define RL_REP0_LONG  (RL_REP2 + RL_STATES)               /* [state][pos state]: one byte or longer */
#define RL_SLOT       (RL_REP0_LONG + RL_STATES * RL_POS_STATES)   /* [length class][64]: distance slot */
#define RL_SPECIAL    (RL_SLOT + 4 * 64)                  /* the low bits of the middle distances */
#define RL_ALIGN      (RL_SPECIAL + 115)                  /* the low 4 bits of the long ones */
#define RL_LEN        (RL_ALIGN + 16)                     /* a new match's length */
#define RL_REPLEN     (RL_LEN + RL_LEN_SIZE)              /* a recent distance's length */
#define RL_LITERAL    (RL_REPLEN + RL_LEN_SIZE)           /* 0x300 for each literal context */

uint32_t rl_probs_size(unsigned lc_plus_lp)
{
	return RL_LITERAL + (0x300u << lc_plus_lp);
}

/* ---- the range decoder -------------------------------------------------------------------- */
typedef struct {
	rl_source *src;
	const uint8_t *in, *in_end;
	uint32_t range, code;
	uint32_t taken;                        /* bytes read so far */
	int bad;                               /* the input ran out */
} rl_rc;

static uint32_t rc_byte(rl_rc *r)
{
	if (r->in == r->in_end) {
		uint32_t n = 0;
		if (r->bad || !r->src->fill(r->src->ctx, &r->in, &n) || !n) { r->bad = 1; r->in = r->in_end; return 0; }
		r->in_end = r->in + n;
	}
	r->taken++;
	return *r->in++;
}
static int rc_start(rl_rc *r)
{
	int i;
	r->range = 0xFFFFFFFFu;
	r->code = 0;
	if (rc_byte(r) != 0) return -1;       /* a stream starts with a 0 */
	for (i = 0; i < 4; i++) r->code = (r->code << 8) | rc_byte(r);
	return r->bad || r->code == r->range ? -1 : 0;
}
static inline void rc_fill(rl_rc *r)
{
	if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rc_byte(r); }
}
static inline unsigned rc_bit(rl_rc *r, uint16_t *p)
{
	uint32_t bound = (r->range >> 11) * *p;
	unsigned b;
	if (r->code < bound) { r->range = bound; *p = (uint16_t)(*p + ((2048u - *p) >> 5)); b = 0; }
	else { r->range -= bound; r->code -= bound; *p = (uint16_t)(*p - (*p >> 5)); b = 1; }
	rc_fill(r);
	return b;
}
/* bits with no model, highest first */
static uint32_t rc_plain(rl_rc *r, unsigned n)
{
	uint32_t v = 0;
	while (n--) {
		r->range >>= 1;
		if (r->code >= r->range) { r->code -= r->range; v = (v << 1) | 1u; }
		else v <<= 1;
		rc_fill(r);
	}
	return v;
}
/* n bits through a tree of probabilities (p[1] the root), highest bit first */
static uint32_t rc_tree(rl_rc *r, uint16_t *p, unsigned n)
{
	uint32_t m = 1;
	unsigned i;
	for (i = 0; i < n; i++) m = (m << 1) | rc_bit(r, &p[m]);
	return m - (1u << n);
}
/* the same tree read lowest bit first */
static uint32_t rc_tree_rev(rl_rc *r, uint16_t *p, unsigned n)
{
	uint32_t m = 1, v = 0;
	unsigned i;
	for (i = 0; i < n; i++) {
		unsigned b = rc_bit(r, &p[m]);
		m = (m << 1) | b;
		v |= (uint32_t)b << i;
	}
	return v;
}

/* ---- the model ---------------------------------------------------------------------------- */
typedef struct {
	uint16_t *p;
	unsigned lc, lp, pb;
	uint32_t state, rep[4];
	uint8_t *out;
	uint32_t pos;                          /* bytes written */
	uint32_t start;                        /* where the dictionary starts (an LZMA2 reset) */
	uint32_t tick_at;
	void (*tick)(void);
} rl_dec;

static void model_reset(rl_dec *d)
{
	uint32_t i, n = rl_probs_size(d->lc + d->lp);
	for (i = 0; i < n; i++) d->p[i] = 1024;    /* a half */
	d->state = 0;
	d->rep[0] = d->rep[1] = d->rep[2] = d->rep[3] = 0;
}
static int props_set(rl_dec *d, unsigned b)
{
	if (b >= 9 * 5 * 5) return -2;
	d->lc = b % 9; b /= 9;
	d->lp = b % 5;
	d->pb = b / 5;
	return d->pb > 4 ? -2 : 0;
}

/* a length: 0..271, two short tables by position, then a long one */
static uint32_t len_read(rl_rc *r, uint16_t *p, unsigned ps)
{
	if (!rc_bit(r, &p[0])) return rc_tree(r, p + 2 + ps * 8, 3);
	if (!rc_bit(r, &p[1])) return 8 + rc_tree(r, p + 2 + 128 + ps * 8, 3);
	return 16 + rc_tree(r, p + 2 + 256, 8);
}
/* a new match's distance (0 the byte just before) for a length class 0..3 */
static uint32_t dist_read(rl_rc *r, rl_dec *d, uint32_t len)
{
	uint32_t slot = rc_tree(r, d->p + RL_SLOT + (len < 3 ? len : 3) * 64, 6), direct, dist;
	if (slot < 4) return slot;
	direct = (slot >> 1) - 1;
	dist = (2u | (slot & 1u)) << direct;
	if (slot < 14) return dist + rc_tree_rev(r, d->p + RL_SPECIAL + dist - slot, direct);
	dist += rc_plain(r, direct - 4) << 4;
	return dist + rc_tree_rev(r, d->p + RL_ALIGN, 4);
}

/* Unpack until d->pos reaches end. 0 there, 1 at an end mark, -1 for damaged data. */
static int rl_run(rl_dec *d, rl_rc *r, uint32_t end)
{
	uint8_t *out = d->out;
	uint32_t pb_mask = (1u << d->pb) - 1, lp_mask = (1u << d->lp) - 1;
	while (d->pos < end) {
		uint32_t ps = d->pos & pb_mask, len;
		if (r->bad) return -1;
		if (d->tick && d->pos >= d->tick_at) { d->tick(); d->tick_at = d->pos + (1u << 20); }
		if (!rc_bit(r, &d->p[RL_MATCH + d->state * RL_POS_STATES + ps])) {
			/* a literal, in the context of the byte before it and the position */
			uint32_t prev = d->pos > d->start ? out[d->pos - 1] : 0, sym = 1;
			uint16_t *lit = d->p + RL_LITERAL + 0x300u * (((d->pos & lp_mask) << d->lc) + (prev >> (8 - d->lc)));
			if (d->state >= 7) {	/* after a match: the byte at the latest distance guides it */
				uint32_t mb = d->pos - d->start > d->rep[0] ? out[d->pos - d->rep[0] - 1] : 0;
				while (sym < 0x100) {
					unsigned mbit = (mb >> 7) & 1u, b;
					mb <<= 1;
					b = rc_bit(r, &lit[((1u + mbit) << 8) + sym]);
					sym = (sym << 1) | b;
					if (b != mbit) break;
				}
			}
			while (sym < 0x100) sym = (sym << 1) | rc_bit(r, &lit[sym]);
			out[d->pos++] = (uint8_t)sym;
			d->state = d->state < 4 ? 0 : d->state < 10 ? d->state - 3 : d->state - 6;
			continue;
		}
		if (!rc_bit(r, &d->p[RL_REP + d->state])) {	/* a new distance */
			d->rep[3] = d->rep[2]; d->rep[2] = d->rep[1]; d->rep[1] = d->rep[0];
			len = len_read(r, d->p + RL_LEN, ps);
			d->state = d->state < 7 ? 7 : 10;
			d->rep[0] = dist_read(r, d, len);
			if (d->rep[0] == 0xFFFFFFFFu) return r->bad ? -1 : 1;   /* the end mark */
		} else {				/* one of the four recent ones */
			if (!rc_bit(r, &d->p[RL_REP0 + d->state])) {
				if (!rc_bit(r, &d->p[RL_REP0_LONG + d->state * RL_POS_STATES + ps])) {
					/* one byte from the latest distance */
					if (d->pos - d->start <= d->rep[0]) return -1;
					d->state = d->state < 7 ? 9 : 11;
					out[d->pos] = out[d->pos - d->rep[0] - 1];
					d->pos++;
					continue;
				}
			} else {
				uint32_t dist;
				if (!rc_bit(r, &d->p[RL_REP1 + d->state])) dist = d->rep[1];
				else {
					if (!rc_bit(r, &d->p[RL_REP2 + d->state])) dist = d->rep[2];
					else { dist = d->rep[3]; d->rep[3] = d->rep[2]; }
					d->rep[2] = d->rep[1];
				}
				d->rep[1] = d->rep[0];
				d->rep[0] = dist;
			}
			len = len_read(r, d->p + RL_REPLEN, ps);
			d->state = d->state < 7 ? 8 : 11;
		}
		/* copy len + 2 bytes from rep[0] + 1 back; what passes the end is not needed */
		if (d->pos - d->start <= d->rep[0] || r->bad) return -1;
		len += 2;
		if (len > end - d->pos) len = end - d->pos;
		{
			const uint8_t *from = out + d->pos - d->rep[0] - 1;
			uint8_t *to = out + d->pos;
			uint32_t k;
			for (k = 0; k < len; k++) to[k] = from[k];
		}
		d->pos += len;
	}
	return r->bad ? -1 : 0;
}

int rl_lzma(const uint8_t *props, uint32_t nprops, rl_source *src, uint8_t *out, uint32_t want,
	    uint16_t *probs, uint32_t probs_cap, void (*tick)(void))
{
	rl_dec d;
	rl_rc r;
	int rc;
	if (nprops < 5) return -2;
	memset(&d, 0, sizeof d);
	memset(&r, 0, sizeof r);
	if (props_set(&d, props[0]) != 0) return -2;
	if (rl_probs_size(d.lc + d.lp) > probs_cap) return -3;
	d.p = probs; d.out = out; d.tick = tick;
	model_reset(&d);
	r.src = src;
	if (rc_start(&r) != 0) return -1;
	rc = rl_run(&d, &r, want);
	if (rc < 0) return -1;
	return d.pos == want ? 0 : -1;         /* an end mark before the size given: cut short */
}

int rl_lzma2(const uint8_t *props, uint32_t nprops, rl_source *src, uint8_t *out, uint32_t want,
	     uint16_t *probs, uint32_t probs_cap, void (*tick)(void))
{
	rl_dec d;
	rl_rc r;
	int have_props = 0;
	if (nprops < 1 || props[0] > 40) return -2;
	memset(&d, 0, sizeof d);
	memset(&r, 0, sizeof r);
	d.p = probs; d.out = out; d.tick = tick;
	r.src = src;
	while (d.pos < want) {
		uint32_t c = rc_byte(&r), usize, csize;
		if (r.bad) return -1;
		if (c == 0) break;                     /* the end of the stream */
		if (c == 1 || c == 2) {                /* stored bytes (1: the dictionary starts here) */
			uint32_t k;
			usize = ((rc_byte(&r) << 8) | rc_byte(&r)) + 1;
			if (r.bad) return -1;
			if (c == 1) d.start = d.pos;
			for (k = 0; k < usize; k++) {
				uint32_t b = rc_byte(&r);
				if (r.bad) return -1;
				if (d.pos < want) out[d.pos++] = (uint8_t)b;
			}
			continue;
		}
		if (c < 0x80) return -1;
		usize = ((c & 0x1Fu) << 16) + (rc_byte(&r) << 8);
		usize = usize + rc_byte(&r) + 1;
		csize = ((rc_byte(&r) << 8) | rc_byte(&r)) + 1;
		if (r.bad) return -1;
		if (((c >> 5) & 3u) == 3) d.start = d.pos;   /* the dictionary starts afresh */
		if (((c >> 5) & 3u) >= 2) {            /* new properties */
			if (props_set(&d, rc_byte(&r)) != 0 || r.bad) return -1;
			if (d.lc + d.lp > 4) return -2;    /* LZMA2 never has more */
			if (rl_probs_size(d.lc + d.lp) > probs_cap) return -3;
			have_props = 1;
		}
		if (!have_props) return -1;
		if (((c >> 5) & 3u) >= 1) model_reset(&d);
		{
			uint32_t at = r.taken, end = d.pos + usize > want ? want : d.pos + usize;
			if (rc_start(&r) != 0) return -1;
			if (rl_run(&d, &r, end) != 0) return -1;   /* no end marks inside LZMA2 */
			if (end == want) break;
			/* on to the next chunk where its packed size says: past it is damage */
			if (r.taken - at > csize) return -1;
			while (r.taken - at < csize) { rc_byte(&r); if (r.bad) return -1; }
		}
	}
	return d.pos == want ? 0 : -1;
}
