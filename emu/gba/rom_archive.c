/*
 * rom_archive.c - games kept in .zip and .7z archives (rom_archive.h); the ZIP side here, 7z in
 * rom_7z.c.
 *
 * A ZIP is read from its end: the end record gives the central directory, which lists every file
 * with its method, sizes, CRC and where its local header is (the ZIP64 records when a field is
 * full). The ROM is the first file whose name has one of the system's extensions. Stored files
 * are read as they are, deflated ones run through zlib in pieces from the card straight into the
 * ROM memory, and the CRC of what came out is checked against the directory's.
 */
#include <string.h>
#include <lib/zlib.h>
#include "rom_archive.h"
#include "pico_libmem.h"

#define RA_SCRATCH  (4u << 20)          /* the archives' own tables, reading pieces, the LZMA model */
#define RA_PIECE    (256u << 10)        /* bytes read from the card at a time */

/* ---- the shared parts ----------------------------------------------------------------------- */
#ifdef __arm__
#include <kernel/mutex.h>
#include <kernel/thread.h>
static mutex_t s_mtx;
static volatile int s_mtx_up;
static void ra_lock(void)
{
	enter_critical_section();
	if (!s_mtx_up) { mutex_init(&s_mtx); s_mtx_up = 1; }
	exit_critical_section();
	mutex_acquire(&s_mtx);
}
static void ra_unlock(void) { mutex_release(&s_mtx); }
#else
static void ra_lock(void) {}
static void ra_unlock(void) {}
#endif

int ra_scratch_get(struct ra_scratch *s)
{
	static uint8_t *buf;
	if (!buf) {
#ifdef __arm__
		buf = (uint8_t *)pico_lib_alloc(RA_SCRATCH);
#else
		extern void *malloc(unsigned long);
		buf = (uint8_t *)malloc(RA_SCRATCH);
#endif
	}
	s->buf = buf;
	s->size = buf ? RA_SCRATCH : 0;
	return buf ? 0 : -1;
}

int ra_read(fat_file *f, uint64_t off, void *buf, uint32_t len)
{
	if (off > f->size || len > f->size - off) return RA_ERR_DAMAGED;   /* past the file: it is cut */
	return fat_read(f, (uint32_t)off, buf, len) == len ? RA_OK : RA_ERR_READ;
}

/* does a file's name end with one of exts (".gba .agb"), any case; folders never */
int ra_ext_match(const char *name, const char *exts)
{
	int L = (int)strlen(name);
	if (!L || name[L - 1] == '/' || name[L - 1] == '\\') return 0;
	while (*exts) {
		int n = 0, i;
		while (exts[n] && exts[n] != ' ') n++;
		if (L > n) {
			for (i = 0; i < n; i++) {
				char a = name[L - n + i], b = exts[i];
				if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
				if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
				if (a != b) break;
			}
			if (i == n) return 1;
		}
		exts += n;
		while (*exts == ' ') exts++;
	}
	return 0;
}

void ra_tick(void)
{
	extern void mtk_wdt_restart(void) __attribute__((weak));
	if (mtk_wdt_restart) mtk_wdt_restart();   /* a big unpack runs for seconds: the 10 s watchdog */
}

const char *rom_archive_error_text(int err)
{
	switch (err) {
	case RA_ERR_READ:     return "This game could not be read from the card";
	case RA_ERR_DAMAGED:  return "This archive is damaged";
	case RA_ERR_NOROM:    return "No game was found in this archive";
	case RA_ERR_METHOD:   return "This archive uses a compression the console cannot open";
	case RA_ERR_PASSWORD: return "This archive is password protected";
	case RA_ERR_TOOBIG:   return "This game is too large for this system";
	case RA_ERR_HUGE:     return "This archive is too large to open";
	case RA_ERR_SPLIT:    return "Archives split into parts are not supported";
	default:              return "This game could not be started";
	}
}

/* ---- ZIP -------------------------------------------------------------------------------------- */
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

struct zip_rom { uint32_t method, flags, crc; uint64_t csize, usize, data; };

/* The upper half of code page 437, the character set a zip entry's name is in when it is not
 * marked UTF-8 (and does not read as UTF-8): the Unicode code point of bytes 0x80..0xFF. */
static const uint16_t k_cp437[128] = {
	0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
	0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
	0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
	0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
	0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};
/* do n bytes read as UTF-8, every character whole */
static int is_utf8(const uint8_t *s, uint32_t n)
{
	uint32_t i = 0;
	while (i < n) {
		uint32_t more = s[i] < 0x80 ? 0 : (s[i] & 0xE0) == 0xC0 ? 1 : (s[i] & 0xF0) == 0xE0 ? 2 : (s[i] & 0xF8) == 0xF0 ? 3 : 4, k;
		if (more == 4 || (more && (s[i] < 0xC2 || i + more >= n))) return 0;
		for (k = 1; k <= more; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
		i += 1 + more;
	}
	return 1;
}
/* a zip entry's name (n bytes) in UTF-8: as it is when the entry says it is UTF-8 (flag bit 11)
 * or it reads as UTF-8, else from code page 437 */
static void zip_name_utf8(const uint8_t *raw, uint32_t n, int utf8_flag, char *out, int cap)
{
	uint32_t i;
	int o = 0;
	if (utf8_flag || is_utf8(raw, n)) {
		for (i = 0; i < n && o < cap - 1; i++) out[o++] = (char)raw[i];
		out[o] = 0;
		return;
	}
	for (i = 0; i < n; i++) {
		uint32_t c = raw[i] < 0x80 ? raw[i] : k_cp437[raw[i] - 0x80];
		if (c < 0x80) { if (o + 1 > cap - 1) break; out[o++] = (char)c; }
		else if (c < 0x800) { if (o + 2 > cap - 1) break; out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
		else {
			if (o + 3 > cap - 1) break;
			out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
		}
	}
	out[o] = 0;
}

/* the ROM's entry: found through the central directory, its data placed through its local header */
static int zip_find(fat_file *f, const char *exts, unsigned member, struct ra_scratch *sc, struct zip_rom *z,
		    void (*each)(void *ctx, const char *name), void *ctx, uint32_t *count)
{
	uint8_t *b = sc->buf;
	uint32_t tail = f->size < 65557u ? f->size : 65557u, i, found = 0, seen = 0;
	uint64_t entries, cd_size, cd_off;
	int rc, eocd = -1;
	if (f->size < 22) return RA_ERR_DAMAGED;
	/* the end record: the last one whose comment fits. Most archives have no comment, so the
	 * last kilobyte is read first (listing thousands of archives), the most a comment allows
	 * when it is not there */
	{
		uint32_t t = f->size < 1024u ? f->size : 1024u;
		for (;;) {
			if ((rc = ra_read(f, f->size - t, b, t)) != RA_OK) return rc;
			for (i = t - 22 + 1; i-- > 0; )
				if (le32(b + i) == 0x06054b50u && i + 22 + le16(b + i + 20) <= t) { eocd = (int)i; break; }
			if (eocd >= 0 || t == tail) break;
			t = tail;
		}
	}
	if (eocd < 0) return RA_ERR_DAMAGED;
	if (le16(b + eocd + 4) != 0 && le16(b + eocd + 4) != 0xFFFF) return RA_ERR_SPLIT;
	if (le16(b + eocd + 6) != 0 && le16(b + eocd + 6) != 0xFFFF) return RA_ERR_SPLIT;
	entries = le16(b + eocd + 10); cd_size = le32(b + eocd + 12); cd_off = le32(b + eocd + 16);
	if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {	/* ZIP64 */
		uint8_t z64[56];
		uint64_t at;
		if (eocd < 20 || le32(b + eocd - 20) != 0x07064b50u) return RA_ERR_DAMAGED;
		if (le32(b + eocd - 20 + 4) != 0) return RA_ERR_SPLIT;
		at = le64(b + eocd - 20 + 8);
		if ((rc = ra_read(f, at, z64, sizeof z64)) != RA_OK) return rc;
		if (le32(z64) != 0x06064b50u) return RA_ERR_DAMAGED;
		if (le32(z64 + 16) != 0 || le32(z64 + 20) != 0) return RA_ERR_SPLIT;
		entries = le64(z64 + 32); cd_size = le64(z64 + 40); cd_off = le64(z64 + 48);
	}
	if (cd_size > sc->size) return RA_ERR_HUGE;
	if (cd_off > f->size || cd_size > f->size - cd_off) return RA_ERR_DAMAGED;
	if ((rc = ra_read(f, cd_off, b, (uint32_t)cd_size)) != RA_OK) return rc;
	for (i = 0; entries-- > 0; ) {
		uint32_t nlen, xlen, clen, k;
		char name[256];
		if (i + 46 > cd_size || le32(b + i) != 0x02014b50u) return RA_ERR_DAMAGED;
		nlen = le16(b + i + 28); xlen = le16(b + i + 30); clen = le16(b + i + 32);
		if (i + 46 + nlen + xlen + clen > cd_size) return RA_ERR_DAMAGED;
		for (k = 0; k < nlen && k < sizeof name - 1; k++) name[k] = (char)b[i + 46 + k];
		name[k] = 0;
		if (ra_ext_match(name, exts) && (each || seen++ == member) && !found) {
			const uint8_t *x = b + i + 46 + nlen, *xe = x + xlen;
			uint64_t loc = le32(b + i + 42);
			if (each) {	/* listed by its name in UTF-8 */
				char un[512];
				zip_name_utf8(b + i + 46, nlen < 255 ? nlen : 255, le16(b + i + 8) & 0x800u, un, sizeof un);
				each(ctx, un); (*count)++; i += 46 + nlen + xlen + clen; continue;
			}
			found = 1;
			z->flags = le16(b + i + 8); z->method = le16(b + i + 10); z->crc = le32(b + i + 16);
			z->csize = le32(b + i + 20); z->usize = le32(b + i + 24);
			if (le16(b + i + 34) != 0 && le16(b + i + 34) != 0xFFFF) return RA_ERR_SPLIT;
			while (x + 4 <= xe) {	/* the ZIP64 field: the full sizes and place, as needed */
				uint32_t id = le16(x), n = le16(x + 2);
				const uint8_t *v = x + 4;
				if (x + 4 + n > xe) return RA_ERR_DAMAGED;
				if (id == 0x0001) {
					if (z->usize == 0xFFFFFFFFu && v + 8 <= x + 4 + n) { z->usize = le64(v); v += 8; }
					if (z->csize == 0xFFFFFFFFu && v + 8 <= x + 4 + n) { z->csize = le64(v); v += 8; }
					if (loc == 0xFFFFFFFFu && v + 8 <= x + 4 + n) { loc = le64(v); v += 8; }
				}
				x += 4 + n;
			}
			z->data = loc;   /* the local header for now */
		}
		i += 46 + nlen + xlen + clen;
	}
	if (each) return RA_OK;
	if (!found) return RA_ERR_NOROM;
	if (z->flags & 0x41u) return RA_ERR_PASSWORD;          /* encrypted, or strongly encrypted */
	if (z->method == 99) return RA_ERR_PASSWORD;            /* AES */
	if (z->method != 0 && z->method != 8) return RA_ERR_METHOD;
	if (z->method == 0 && z->csize != z->usize) return RA_ERR_DAMAGED;
	{	/* the local header: its name and extra field come before the data */
		uint8_t lh[30];
		if ((rc = ra_read(f, z->data, lh, sizeof lh)) != RA_OK) return rc;
		if (le32(lh) != 0x04034b50u) return RA_ERR_DAMAGED;
		z->data += 30 + le16(lh + 26) + le16(lh + 28);
		if (z->data > f->size || z->csize > f->size - z->data) return RA_ERR_DAMAGED;
	}
	return RA_OK;
}

/* the ROM out of the ZIP: all of it (crc checked), or its first cap bytes (head) */
static int zip_unpack(fat_file *f, struct ra_scratch *sc, const struct zip_rom *z, uint8_t *dst,
		      uint32_t cap, int head, uint32_t *size)
{
	uint32_t want = z->usize < cap ? (uint32_t)z->usize : cap, done = 0;
	int rc;
	if (!head && z->usize > cap) return RA_ERR_TOOBIG;
	if (z->method == 0) {
		while (done < want) {
			uint32_t n = want - done < RA_PIECE ? want - done : RA_PIECE;
			if ((rc = ra_read(f, z->data + done, dst + done, n)) != RA_OK) return rc;
			done += n;
			ra_tick();
		}
	} else {
		z_stream s;
		uint64_t left = z->csize, at = z->data;
		int r = Z_OK;
		memset(&s, 0, sizeof s);
		if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return RA_ERR_START;
		s.next_out = dst;
		s.avail_out = want;
		while (r != Z_STREAM_END && s.avail_out) {
			if (!s.avail_in) {
				uint32_t n = left < RA_PIECE ? (uint32_t)left : RA_PIECE;
				if (!n) { inflateEnd(&s); return RA_ERR_DAMAGED; }   /* cut short */
				if ((rc = ra_read(f, at, sc->buf, n)) != RA_OK) { inflateEnd(&s); return rc; }
				at += n; left -= n;
				s.next_in = sc->buf; s.avail_in = n;
				ra_tick();
			}
			r = inflate(&s, Z_NO_FLUSH);
			if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&s); return RA_ERR_DAMAGED; }
		}
		done = want - s.avail_out;
		inflateEnd(&s);
		if (!head && (r != Z_STREAM_END || done != z->usize)) return RA_ERR_DAMAGED;
	}
	if (!head && crc32(0L, dst, done) != z->crc) return RA_ERR_DAMAGED;
	*size = done;
	return RA_OK;
}

static int unpack(int kind, fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap,
		  uint32_t *size, int mode, void (*each)(void *ctx, const char *name), void *ctx)
{
	struct ra_scratch sc;
	struct zip_rom z;
	int rc;
	*size = 0;
	if (ra_scratch_get(&sc) != 0) return RA_ERR_HUGE;
	ra_lock();
	if (kind == RA_7Z) rc = rom_7z_open(f, exts, member, dst, cap, size, mode, each, ctx);
	else if (kind != RA_ZIP) rc = RA_ERR_DAMAGED;
	else if ((rc = zip_find(f, exts, member, &sc, &z, mode == 3 ? each : 0, ctx, size)) == RA_OK && mode != 3) {
		if (mode == 0) *size = z.usize > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)z.usize;
		else rc = zip_unpack(f, &sc, &z, dst, cap, mode == 2, size);
	}
	ra_unlock();
	return rc;
}

int rom_archive_probe(int kind, fat_file *f, const char *exts, unsigned member, uint32_t *size)
{ return unpack(kind, f, exts, member, 0, 0, size, 0, 0, 0); }
int rom_archive_load(int kind, fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size)
{ return unpack(kind, f, exts, member, dst, cap, size, 1, 0, 0); }
int rom_archive_head(int kind, fat_file *f, const char *exts, unsigned member, uint8_t *dst, uint32_t cap, uint32_t *size)
{ return unpack(kind, f, exts, member, dst, cap, size, 2, 0, 0); }
int rom_archive_list(int kind, fat_file *f, const char *exts, void (*each)(void *ctx, const char *name), void *ctx)
{
	uint32_t n = 0;
	int rc = unpack(kind, f, exts, 0, 0, 0, &n, 3, each, ctx);
	return rc == RA_OK ? (int)n : rc;
}
