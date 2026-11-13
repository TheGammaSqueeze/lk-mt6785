/*
 * rom_7z.c - the ROM out of a .7z archive (rom_archive.h).
 *
 * A 7z starts with a 32 byte header that says where its tables are, at the end. The tables may be
 * packed themselves (an "encoded header": a stream to unpack first, then read). They give the
 * packed streams, the folders (each a chain of coders over packed streams, unpacking to one
 * stream), how each folder's stream splits into files (substreams), with sizes and CRCs, then the
 * files' names. The ROM is the first file whose name has one of the system's extensions; its
 * folder is unpacked from the start up to the end of the ROM (into the ROM memory itself, which is
 * the LZMA dictionary too), the ROM moved to the front, and its CRC checked.
 *
 * Unpacked: folders of one coder, Copy, LZMA or LZMA2. A filter chain (BCJ and the like), PPMd,
 * BZip2 or Deflate inside a 7z is RA_ERR_METHOD, AES is RA_ERR_PASSWORD.
 */
#include <string.h>
#include <lib/zlib.h>
#include "rom_archive.h"
#include "rom_lzma.h"

#define SZ_PIECE      (256u << 10)
#define SZ_PROBS      (512u << 10)       /* bytes of LZMA model: lc + lp up to 8 */
#define SZ_MAX_FOLD   4096
#define SZ_MAX_PACK   8192

enum { SZ_COPY, SZ_LZMA, SZ_LZMA2, SZ_OTHER, SZ_AES };

/* ---- reading the tables, with every length checked ---------------------------------------- */
typedef struct { const uint8_t *p, *end; int bad; } cur;
static uint32_t c_byte(cur *c) { if (c->p >= c->end) { c->bad = 1; return 0; } return *c->p++; }
static uint64_t c_num(cur *c)
{	/* a 7z number: the first byte's high bits say how many more bytes follow */
	uint32_t first = c_byte(c), mask = 0x80;
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++) {
		if (!(first & mask)) return v | ((uint64_t)(first & (mask - 1)) << (8 * i));
		v |= (uint64_t)c_byte(c) << (8 * i);
		mask >>= 1;
	}
	return v;
}
static uint32_t c_u32(cur *c) { uint32_t v = c_byte(c); v |= c_byte(c) << 8; v |= c_byte(c) << 16; return v | (c_byte(c) << 24); }
static void c_skip(cur *c, uint64_t n) { if (n > (uint64_t)(c->end - c->p)) { c->bad = 1; c->p = c->end; } else c->p += n; }
/* a vector of n bits, first in the high bit; all set when the "all defined" byte says so */
static void c_bits(cur *c, uint8_t *out, uint32_t n, int all_byte)
{
	uint32_t i, b = 0;
	if (all_byte && c_byte(c)) { for (i = 0; i < n; i++) out[i] = 1; return; }
	for (i = 0; i < n; i++) {
		if (!(i & 7)) b = c_byte(c);
		out[i] = (uint8_t)((b >> (7 - (i & 7))) & 1u);
	}
}

/* the scratch memory as a stack of tables for one archive */
typedef struct { uint8_t *base; uint32_t size, used; } sz_mem;
static void *mem_take(sz_mem *m, uint32_t n)
{
	void *p;
	n = (n + 7u) & ~7u;
	if (n > m->size - m->used) return 0;
	p = m->base + m->used;
	m->used += n;
	memset(p, 0, n);
	return p;
}

typedef struct {
	int method;
	uint8_t props[8];
	uint32_t nprops;
	uint32_t pack_first;            /* its packed stream's index */
	uint64_t usize;
	uint32_t crc; int has_crc;
	uint32_t nsub;                  /* files in it */
} sz_folder;

typedef struct {
	uint64_t pack_pos, *pack_size;
	uint32_t npack;
	sz_folder *fold;
	uint32_t nfold;
	uint64_t *sub_size;             /* every folder's files' sizes, folder after folder */
	uint32_t *sub_crc;
	uint8_t *sub_has_crc;
	uint32_t nsub;
	int err;
} sz_streams;

static int coder_method(const uint8_t *id, uint32_t n)
{
	if (n == 1 && id[0] == 0x00) return SZ_COPY;
	if (n == 1 && id[0] == 0x21) return SZ_LZMA2;
	if (n == 3 && id[0] == 0x03 && id[1] == 0x01 && id[2] == 0x01) return SZ_LZMA;
	if (n == 4 && id[0] == 0x06 && id[1] == 0xF1 && id[2] == 0x07 && id[3] == 0x01) return SZ_AES;
	return SZ_OTHER;
}

static int read_pack_info(cur *c, sz_streams *s, sz_mem *m)
{
	uint32_t i, id;
	s->pack_pos = c_num(c);
	s->npack = (uint32_t)c_num(c);
	if (c->bad || s->npack > SZ_MAX_PACK) return RA_ERR_HUGE;
	if (!(s->pack_size = (uint64_t *)mem_take(m, s->npack * 8u + 8u))) return RA_ERR_HUGE;
	while ((id = c_byte(c)) != 0 && !c->bad) {
		if (id == 0x09) for (i = 0; i < s->npack; i++) s->pack_size[i] = c_num(c);
		else if (id == 0x0A) {	/* the packed streams' CRCs: not needed, the unpacked ones are checked */
			uint8_t *def = (uint8_t *)mem_take(m, s->npack + 1);
			if (!def) return RA_ERR_HUGE;
			c_bits(c, def, s->npack, 1);
			for (i = 0; i < s->npack; i++) if (def[i]) c_u32(c);
		} else c_skip(c, c_num(c));
	}
	return c->bad ? RA_ERR_DAMAGED : RA_OK;
}

static int read_folders(cur *c, sz_streams *s, sz_mem *m)
{
	uint32_t i, j, id, pack_next = 0;
	uint32_t *nout;
	if (c_byte(c) != 0x0B) return RA_ERR_DAMAGED;
	s->nfold = (uint32_t)c_num(c);
	if (c->bad || s->nfold > SZ_MAX_FOLD) return RA_ERR_HUGE;
	if (c_byte(c) != 0) return RA_ERR_METHOD;          /* folders kept elsewhere: never written */
	s->fold = (sz_folder *)mem_take(m, s->nfold * (uint32_t)sizeof(sz_folder) + 8u);
	nout = (uint32_t *)mem_take(m, s->nfold * 4u + 4u);
	if (!s->fold || !nout) return RA_ERR_HUGE;
	for (i = 0; i < s->nfold; i++) {
		sz_folder *f = &s->fold[i];
		uint32_t ncoders = (uint32_t)c_num(c), tin = 0, tout = 0, npacked;
		int aes = 0;
		if (c->bad || ncoders == 0 || ncoders > 32) return RA_ERR_DAMAGED;
		f->method = SZ_OTHER;
		for (j = 0; j < ncoders; j++) {
			uint32_t flag = c_byte(c), idlen = flag & 0x0Fu, nin = 1, no = 1, k;
			uint8_t idb[16];
			int meth;
			if (flag & 0x80u) return RA_ERR_METHOD;   /* alternative methods: never written */
			for (k = 0; k < idlen; k++) { uint32_t b = c_byte(c); if (k < sizeof idb) idb[k] = (uint8_t)b; }
			meth = coder_method(idb, idlen > sizeof idb ? 0 : idlen);
			if (meth == SZ_AES) aes = 1;
			if (flag & 0x10u) { nin = (uint32_t)c_num(c); no = (uint32_t)c_num(c); }
			if (flag & 0x20u) {
				uint32_t np = (uint32_t)c_num(c);
				if (ncoders == 1 && np <= sizeof f->props) {
					for (k = 0; k < np; k++) f->props[k] = (uint8_t)c_byte(c);
					f->nprops = np;
				} else c_skip(c, np);
			}
			if (ncoders == 1 && nin == 1 && no == 1) f->method = meth;
			tin += nin; tout += no;
			if (c->bad || tin > 64 || tout > 64) return RA_ERR_DAMAGED;
		}
		if (aes) f->method = SZ_AES;
		if (tout == 0) return RA_ERR_DAMAGED;
		for (j = 0; j + 1 < tout; j++) { c_num(c); c_num(c); }   /* the bind pairs */
		npacked = tin - (tout - 1);
		if (npacked > 1) for (j = 0; j < npacked; j++) c_num(c);
		f->pack_first = pack_next;
		pack_next += npacked;
		nout[i] = tout;
		f->nsub = 1;
	}
	if (pack_next > s->npack) return RA_ERR_DAMAGED;
	if (c_byte(c) != 0x0C) return RA_ERR_DAMAGED;
	for (i = 0; i < s->nfold; i++)	/* one size per coder output; a lone coder's is the folder's */
		for (j = 0; j < nout[i]; j++) {
			uint64_t v = c_num(c);
			if (j + 1 == nout[i]) s->fold[i].usize = v;
		}
	while ((id = c_byte(c)) != 0 && !c->bad) {
		if (id == 0x0A) {
			uint8_t *def = (uint8_t *)mem_take(m, s->nfold + 1);
			if (!def) return RA_ERR_HUGE;
			c_bits(c, def, s->nfold, 1);
			for (i = 0; i < s->nfold; i++)
				if (def[i]) { s->fold[i].crc = c_u32(c); s->fold[i].has_crc = 1; }
		} else c_skip(c, c_num(c));
	}
	return c->bad ? RA_ERR_DAMAGED : RA_OK;
}

static int read_substreams(cur *c, sz_streams *s, sz_mem *m)
{
	uint32_t i, j, k, id, ndig = 0;
	id = c_byte(c);
	if (id == 0x0D) {
		for (i = 0; i < s->nfold; i++) s->fold[i].nsub = (uint32_t)c_num(c);
		id = c_byte(c);
	}
	for (i = 0, s->nsub = 0; i < s->nfold; i++) {
		s->nsub += s->fold[i].nsub;
		if (s->nsub > (1u << 20)) return RA_ERR_HUGE;
	}
	s->sub_size = (uint64_t *)mem_take(m, s->nsub * 8u + 8u);
	s->sub_crc = (uint32_t *)mem_take(m, s->nsub * 4u + 4u);
	s->sub_has_crc = (uint8_t *)mem_take(m, s->nsub + 1u);
	if (!s->sub_size || !s->sub_crc || !s->sub_has_crc) return RA_ERR_HUGE;
	/* the sizes: given for all but each folder's last, which is what remains */
	for (i = 0, k = 0; i < s->nfold; i++) {
		uint64_t sum = 0;
		uint32_t n = s->fold[i].nsub;
		if (!n) continue;
		for (j = 0; j + 1 < n; j++) {
			uint64_t v = id == 0x09 ? c_num(c) : 0;
			s->sub_size[k++] = v;
			sum += v;
		}
		if (sum > s->fold[i].usize) return RA_ERR_DAMAGED;
		s->sub_size[k++] = s->fold[i].usize - sum;
	}
	if (id == 0x09) id = c_byte(c);
	/* the CRCs: a folder of one file with its own CRC has it already */
	for (i = 0, k = 0; i < s->nfold; i++) {
		if (s->fold[i].nsub == 1 && s->fold[i].has_crc) { s->sub_crc[k] = s->fold[i].crc; s->sub_has_crc[k] = 1; }
		else ndig += s->fold[i].nsub;
		k += s->fold[i].nsub;
	}
	while (id != 0 && !c->bad) {
		if (id == 0x0A) {
			uint8_t *def = (uint8_t *)mem_take(m, ndig + 1);
			uint32_t d = 0;
			if (!def) return RA_ERR_HUGE;
			c_bits(c, def, ndig, 1);
			for (i = 0, k = 0; i < s->nfold; i++) {
				if (s->fold[i].nsub == 1 && s->fold[i].has_crc) { k++; continue; }
				for (j = 0; j < s->fold[i].nsub; j++, k++, d++)
					if (def[d]) { s->sub_crc[k] = c_u32(c); s->sub_has_crc[k] = 1; }
			}
		} else c_skip(c, c_num(c));
		id = c_byte(c);
	}
	return c->bad ? RA_ERR_DAMAGED : RA_OK;
}

static int read_streams(cur *c, sz_streams *s, sz_mem *m)
{
	uint32_t id;
	int rc, have_sub = 0;
	memset(s, 0, sizeof *s);
	while ((id = c_byte(c)) != 0 && !c->bad) {
		if (id == 0x06) rc = read_pack_info(c, s, m);
		else if (id == 0x07) rc = read_folders(c, s, m);
		else if (id == 0x08) { rc = read_substreams(c, s, m); have_sub = 1; }
		else return RA_ERR_DAMAGED;
		if (rc != RA_OK) return rc;
	}
	if (c->bad) return RA_ERR_DAMAGED;
	if (!have_sub && s->nfold) {	/* no substreams: a file a folder */
		cur none = { (const uint8_t *)"", (const uint8_t *)"", 0 };
		uint8_t z = 0;
		none.p = &z; none.end = &z + 1;
		if ((rc = read_substreams(&none, s, m)) != RA_OK) return rc;
	}
	return RA_OK;
}

/* ---- unpacking a folder ------------------------------------------------------------------- */
typedef struct { fat_file *f; uint64_t at, left; uint8_t *piece; int err; } sz_src;
static int sz_fill(void *ctx, const uint8_t **buf, uint32_t *len)
{
	sz_src *s = (sz_src *)ctx;
	uint32_t n = s->left < SZ_PIECE ? (uint32_t)s->left : SZ_PIECE;
	int rc;
	if (!n) return 0;
	if ((rc = ra_read(s->f, s->at, s->piece, n)) != RA_OK) { s->err = rc; return 0; }
	s->at += n; s->left -= n;
	*buf = s->piece; *len = n;
	ra_tick();
	return 1;
}
/* the first want bytes of folder fi into out */
static int folder_unpack(fat_file *f, const sz_streams *s, uint32_t fi, uint8_t *out, uint32_t want, sz_mem *m)
{
	const sz_folder *fo = &s->fold[fi];
	uint64_t at = 32 + s->pack_pos;
	uint32_t i;
	sz_src src;
	rl_source rs;
	int rc;
	if (fo->method == SZ_AES) return RA_ERR_PASSWORD;
	if (fo->method == SZ_OTHER) return RA_ERR_METHOD;
	if (fo->pack_first >= s->npack) return RA_ERR_DAMAGED;
	for (i = 0; i < fo->pack_first; i++) at += s->pack_size[i];
	memset(&src, 0, sizeof src);
	src.f = f; src.at = at; src.left = s->pack_size[fo->pack_first];
	if (at > f->size || src.left > f->size - at) return RA_ERR_DAMAGED;
	if (fo->method == SZ_COPY) {
		if (want > src.left) return RA_ERR_DAMAGED;
		for (i = 0; i < want; ) {
			uint32_t n = want - i < SZ_PIECE ? want - i : SZ_PIECE;
			if ((rc = ra_read(f, at + i, out + i, n)) != RA_OK) return rc;
			i += n;
			ra_tick();
		}
		return RA_OK;
	}
	{
		uint32_t mark = m->used;
		uint16_t *probs = (uint16_t *)mem_take(m, SZ_PROBS);
		src.piece = (uint8_t *)mem_take(m, SZ_PIECE);
		if (!probs || !src.piece) return RA_ERR_HUGE;
		rs.fill = sz_fill; rs.ctx = &src;
		rc = fo->method == SZ_LZMA ? rl_lzma(fo->props, fo->nprops, &rs, out, want, probs, SZ_PROBS / 2, ra_tick)
					   : rl_lzma2(fo->props, fo->nprops, &rs, out, want, probs, SZ_PROBS / 2, ra_tick);
		m->used = mark;
	}
	if (src.err) return src.err;
	return rc == 0 ? RA_OK : rc == -1 ? RA_ERR_DAMAGED : RA_ERR_METHOD;
}

/* ---- the archive ---------------------------------------------------------------------------- */
int rom_7z_open(fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size, int mode,
		void (*each)(void *ctx, const char *name), void *ctx)
{
	static const uint8_t sig[6] = { '7', 'z', 0xBC, 0xAF, 0x27, 0x1C };
	struct ra_scratch sc;
	sz_mem m;
	uint8_t h[32], *tab;
	uint64_t next_off, next_size;
	sz_streams st;
	cur c;
	uint32_t id, nfiles = 0, i, round;
	uint8_t *empty = 0;
	const uint8_t *names = 0, *names_end = 0;
	int rc;
	if (ra_scratch_get(&sc) != 0) return RA_ERR_HUGE;
	m.base = sc.buf; m.size = sc.size; m.used = 0;
	if ((rc = ra_read(f, 0, h, 32)) != RA_OK) return rc == RA_ERR_DAMAGED ? RA_ERR_DAMAGED : rc;
	if (memcmp(h, sig, 6) != 0 || h[6] != 0) return RA_ERR_DAMAGED;
	if (crc32(0L, h + 12, 20) != (h[8] | (h[9] << 8) | (h[10] << 16) | ((uint32_t)h[11] << 24))) return RA_ERR_DAMAGED;
	next_off = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint64_t)h[15] << 24) | ((uint64_t)(h[16] | (h[17] << 8) | (h[18] << 16) | ((uint32_t)h[19] << 24)) << 32);
	next_size = h[20] | (h[21] << 8) | (h[22] << 16) | ((uint64_t)h[23] << 24) | ((uint64_t)(h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24)) << 32);
	if (next_size == 0) return RA_ERR_NOROM;            /* an empty archive */
	if (next_size > sc.size / 4) return RA_ERR_HUGE;
	if (32 + next_off > f->size || next_size > f->size - 32 - next_off) return RA_ERR_DAMAGED;
	if (!(tab = (uint8_t *)mem_take(&m, (uint32_t)next_size))) return RA_ERR_HUGE;
	if ((rc = ra_read(f, 32 + next_off, tab, (uint32_t)next_size)) != RA_OK) return rc;
	if (crc32(0L, tab, (uint32_t)next_size) != (h[28] | (h[29] << 8) | (h[30] << 16) | ((uint32_t)h[31] << 24)))
		return RA_ERR_DAMAGED;
	c.p = tab; c.end = tab + next_size; c.bad = 0;
	for (round = 0; ; round++) {	/* the tables, packed or not */
		id = c_byte(&c);
		if (id == 0x01) break;
		if (id != 0x17 || round > 3) return RA_ERR_DAMAGED;
		if ((rc = read_streams(&c, &st, &m)) != RA_OK) return rc;
		if (!st.nfold || st.fold[0].usize > sc.size / 4) return st.nfold ? RA_ERR_HUGE : RA_ERR_DAMAGED;
		{
			uint32_t n = (uint32_t)st.fold[0].usize;
			uint8_t *un = (uint8_t *)mem_take(&m, n + 1);
			if (!un) return RA_ERR_HUGE;
			if ((rc = folder_unpack(f, &st, 0, un, n, &m)) != RA_OK) return rc;
			if (st.fold[0].has_crc && crc32(0L, un, n) != st.fold[0].crc) return RA_ERR_DAMAGED;
			c.p = un; c.end = un + n; c.bad = 0;
		}
	}
	memset(&st, 0, sizeof st);
	while ((id = c_byte(&c)) != 0 && !c.bad) {
		if (id == 0x02) {	/* archive properties: skipped */
			uint32_t t;
			while ((t = c_byte(&c)) != 0 && !c.bad) c_skip(&c, c_num(&c));
		} else if (id == 0x03) return RA_ERR_METHOD;   /* additional streams: never written */
		else if (id == 0x04) { if ((rc = read_streams(&c, &st, &m)) != RA_OK) return rc; }
		else if (id == 0x05) {
			uint32_t t;
			nfiles = (uint32_t)c_num(&c);
			if (c.bad || nfiles > (1u << 20)) return RA_ERR_HUGE;
			if (!(empty = (uint8_t *)mem_take(&m, nfiles + 1))) return RA_ERR_HUGE;
			while ((t = (uint32_t)c_num(&c)) != 0 && !c.bad) {
				uint64_t n = c_num(&c);
				const uint8_t *at = c.p;
				if (n > (uint64_t)(c.end - c.p)) return RA_ERR_DAMAGED;
				if (t == 0x0E) { cur v = { at, at + n, 0 }; c_bits(&v, empty, nfiles, 0); }
				else if (t == 0x11) {
					if (n < 1 || at[0] != 0) return RA_ERR_METHOD;   /* names kept elsewhere */
					names = at + 1; names_end = at + n;
				}
				c_skip(&c, n);
			}
		} else return RA_ERR_DAMAGED;
	}
	if (c.bad) return RA_ERR_DAMAGED;
	if (!names) return RA_ERR_NOROM;
	{	/* the files in order: the ones with data take the substreams in turn */
		const uint8_t *q = names;
		uint32_t stream = 0, fi, k, sub_first, seen = 0;
		for (i = 0; i < nfiles; i++) {
			char nm[256];
			int n = 0, has = !empty[i];
			while (q + 1 < names_end && (q[0] | q[1])) {
				uint32_t u = q[0] | (q[1] << 8);
				if (n < 255) nm[n++] = u < 0x80 ? (char)u : '_';
				q += 2;
			}
			if (q + 1 >= names_end) return RA_ERR_DAMAGED;
			q += 2;
			nm[n] = 0;
			if (!has) continue;
			if (stream >= st.nsub) return RA_ERR_DAMAGED;
			if (ra_ext_match(nm, exts)) {
				if (mode == 3) { each(ctx, nm); (*size)++; }
				else if (seen++ == member) break;
			}
			stream++;
		}
		if (mode == 3) return RA_OK;
		if (i == nfiles) return RA_ERR_NOROM;
		/* its folder, and where it starts in that folder's stream */
		for (fi = 0, sub_first = 0; fi < st.nfold && sub_first + st.fold[fi].nsub <= stream; fi++)
			sub_first += st.fold[fi].nsub;
		if (fi == st.nfold) return RA_ERR_DAMAGED;
		{
			uint64_t off = 0, rsize = st.sub_size[stream];
			uint32_t want;
			for (k = sub_first; k < stream; k++) off += st.sub_size[k];
			if (st.fold[fi].method == SZ_AES) return RA_ERR_PASSWORD;
			if (st.fold[fi].method == SZ_OTHER) return RA_ERR_METHOD;
			if (mode == 0) { *size = rsize > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)rsize; return RA_OK; }
			if (mode == 1 && off + rsize > cap) return RA_ERR_TOOBIG;
			if (mode == 2) {
				if (off >= cap) return RA_ERR_TOOBIG;
				if (rsize > cap - off) rsize = cap - off;
			}
			want = (uint32_t)(off + rsize);
			if ((rc = folder_unpack(f, &st, fi, dst, want, &m)) != RA_OK) return rc;
			if (off) memmove(dst, dst + off, (uint32_t)rsize);
			if (mode == 1 && st.sub_has_crc[stream] && crc32(0L, dst, (uint32_t)rsize) != st.sub_crc[stream])
				return RA_ERR_DAMAGED;
			*size = (uint32_t)rsize;
		}
	}
	return RA_OK;
}
