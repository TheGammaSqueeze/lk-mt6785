/*
 * pico_png.c - a PNG picture turned into the menu's box art (pico_png.h).
 *
 * The chunks are walked once: the header, the palette and its transparency are kept, and the
 * image data chunks are moved together at the front of the file's own buffer, so zlib reads one
 * run. Scanlines then come out of zlib one at a time, are unfiltered against the one before and
 * added into the smaller picture straight away (each pixel to the output pixel it falls in, a
 * plain average), so a large picture needs two rows of memory, not the whole image. Interlaced
 * pictures are the same seven times over, each pass a small picture of its own.
 */
#include <string.h>
#include <lib/zlib.h>
#include "pico_png.h"

#define MAX_SIDE   16384u

struct png_info {
	unsigned int w, h, depth, ctype, interlace;
	unsigned int chans, bits;            /* samples a pixel, bits a pixel */
	unsigned char pal[256][4];           /* palette, with its alpha */
	unsigned int npal;
	int key;                             /* a transparent colour is set (tRNS, grey or RGB) */
	unsigned int key_r, key_g, key_b;    /* as raw samples */
};

static unsigned int be32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static int depth_ok(unsigned int ctype, unsigned int d)
{
	switch (ctype) {
	case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;   /* grey */
	case 3: return d == 1 || d == 2 || d == 4 || d == 8;              /* palette */
	case 2: case 4: case 6: return d == 8 || d == 16;                 /* RGB, grey + alpha, RGBA */
	}
	return 0;
}

/* a scanline's filter undone, against the line above (zeros for the first) */
static int unfilter(unsigned char *row, const unsigned char *up, unsigned int n, unsigned int bpp,
		    unsigned int type)
{
	unsigned int i;
	switch (type) {
	case 0: break;
	case 1: for (i = bpp; i < n; i++) row[i] = (unsigned char)(row[i] + row[i - bpp]); break;
	case 2: for (i = 0; i < n; i++) row[i] = (unsigned char)(row[i] + up[i]); break;
	case 3:
		for (i = 0; i < n; i++) {
			unsigned int left = i >= bpp ? row[i - bpp] : 0;
			row[i] = (unsigned char)(row[i] + ((left + up[i]) >> 1));
		}
		break;
	case 4:
		for (i = 0; i < n; i++) {
			int a = i >= bpp ? row[i - bpp] : 0, b = up[i], c = i >= bpp ? up[i - bpp] : 0;
			int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p,
			    pc = p > c ? p - c : c - p;
			row[i] = (unsigned char)(row[i] + ((pa <= pb && pa <= pc) ? a : pb <= pc ? b : c));
		}
		break;
	default: return -1;
	}
	return 0;
}

/* sample i of a row packed at `depth` bits (1, 2, 4 or 8), first sample in the high bits */
static unsigned int packed(const unsigned char *row, unsigned int i, unsigned int depth)
{
	unsigned int bit = i * depth;
	return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1u);
}

/* pixel i of an unfiltered row as 8 bit RGBA */
static void pixel(const struct png_info *pi, const unsigned char *row, unsigned int i, unsigned char *c)
{
	unsigned int d = pi->depth;
	c[0] = c[1] = c[2] = 0;
	c[3] = 255;
	switch (pi->ctype) {
	case 0: {
		unsigned int raw = d == 16 ? (unsigned)(row[2 * i] << 8 | row[2 * i + 1]) : d == 8 ? row[i] : packed(row, i, d);
		c[0] = c[1] = c[2] = (unsigned char)(d == 16 ? raw >> 8 : d == 8 ? raw : raw * 255u / ((1u << d) - 1u));
		if (pi->key && raw == pi->key_r) c[3] = 0;
		break;
	}
	case 2:
		if (d == 8) {
			c[0] = row[3 * i]; c[1] = row[3 * i + 1]; c[2] = row[3 * i + 2];
			if (pi->key && c[0] == pi->key_r && c[1] == pi->key_g && c[2] == pi->key_b) c[3] = 0;
		} else {
			const unsigned char *p = row + 6 * i;
			c[0] = p[0]; c[1] = p[2]; c[2] = p[4];
			if (pi->key && (unsigned)(p[0] << 8 | p[1]) == pi->key_r &&
			    (unsigned)(p[2] << 8 | p[3]) == pi->key_g && (unsigned)(p[4] << 8 | p[5]) == pi->key_b)
				c[3] = 0;
		}
		break;
	case 3: {
		unsigned int k = d == 8 ? row[i] : packed(row, i, d);
		if (k < pi->npal) memcpy(c, pi->pal[k], 4);
		else c[0] = c[1] = c[2] = 0;            /* outside the palette: black */
		break;
	}
	case 4:
		if (d == 8) { c[0] = c[1] = c[2] = row[2 * i]; c[3] = row[2 * i + 1]; }
		else { c[0] = c[1] = c[2] = row[4 * i]; c[3] = row[4 * i + 2]; }
		break;
	case 6:
		if (d == 8) memcpy(c, row + 4 * i, 4);
		else { const unsigned char *p = row + 8 * i; c[0] = p[0]; c[1] = p[2]; c[2] = p[4]; c[3] = p[6]; }
		break;
	}
}

/* exactly n bytes out of the stream; 0, or -1 when it ends or breaks first */
static int pull(z_stream *z, unsigned char *dst, unsigned int n)
{
	z->next_out = dst;
	z->avail_out = n;
	while (z->avail_out) {
		int r = inflate(z, Z_SYNC_FLUSH);
		if (z->avail_out == 0) break;
		if (r != Z_OK) return -1;               /* the end, an error, or no input left */
	}
	return 0;
}

int pico_png_to_art(unsigned char *png, unsigned int len, int maxside,
		    unsigned char *work, unsigned int work_cap,
		    unsigned char *out, unsigned int out_cap, void (*tick)(void))
{
	static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	/* the seven interlace passes: first column and row, steps across and down */
	static const unsigned char pass7[7][4] = {
		{ 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
		{ 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
	};
	static const unsigned char pass1[1][4] = { { 0, 0, 1, 1 } };
	struct png_info pi;
	unsigned int pos = 8, idat = 0, have_hdr = 0, ow, oh, i, npass, p, stride;
	unsigned int *acc;
	unsigned char *row, *up;
	const unsigned char (*passes)[4];
	z_stream z;
	int rc = 0;

	if (len < 8 || memcmp(png, sig, 8) != 0) return -1;
	memset(&pi, 0, sizeof pi);
	for (i = 0; i < 256; i++) pi.pal[i][3] = 255;

	/* the chunks: header, palette, transparency; the image data gathered at the front */
	while (pos + 12 <= len) {
		unsigned int n = be32(png + pos), type = be32(png + pos + 4);
		unsigned char *d = png + pos + 8;
		if (n > len - pos - 12) {               /* cut short: what data there is is kept */
			if (type == 0x49444154u) { memmove(png + idat, d, len - pos - 8); idat += len - pos - 8; }
			break;
		}
		if (type == 0x49484452u) {              /* IHDR */
			if (n < 13) return -3;
			pi.w = be32(d); pi.h = be32(d + 4); pi.depth = d[8]; pi.ctype = d[9]; pi.interlace = d[12];
			if (!pi.w || !pi.h || pi.w > MAX_SIDE || pi.h > MAX_SIDE) return -2;
			if (!depth_ok(pi.ctype, pi.depth) || d[10] || d[11] || pi.interlace > 1) return -2;
			pi.chans = pi.ctype == 2 ? 3 : pi.ctype == 4 ? 2 : pi.ctype == 6 ? 4 : 1;
			pi.bits = pi.chans * pi.depth;
			have_hdr = 1;
		} else if (type == 0x504C5445u) {       /* PLTE */
			pi.npal = n / 3 > 256 ? 256 : n / 3;
			for (i = 0; i < pi.npal; i++) { pi.pal[i][0] = d[3 * i]; pi.pal[i][1] = d[3 * i + 1]; pi.pal[i][2] = d[3 * i + 2]; }
		} else if (type == 0x74524E53u) {       /* tRNS */
			if (pi.ctype == 3) for (i = 0; i < n && i < 256; i++) pi.pal[i][3] = d[i];
			else if (pi.ctype == 0 && n >= 2) { pi.key = 1; pi.key_r = d[0] << 8 | d[1]; }
			else if (pi.ctype == 2 && n >= 6) {
				pi.key = 1; pi.key_r = d[0] << 8 | d[1]; pi.key_g = d[2] << 8 | d[3]; pi.key_b = d[4] << 8 | d[5];
			}
		} else if (type == 0x49444154u) {       /* IDAT: after the data before it (moves only back) */
			memmove(png + idat, d, n);
			idat += n;
		} else if (type == 0x49454E44u) {       /* IEND */
			break;
		}
		pos += n + 12;
	}
	if (!have_hdr || !idat) return -3;
	if (pi.ctype == 3 && !pi.npal) return -3;
	/* the 8 bit key of an 8 bit RGB picture compares with the samples as they are */
	if (pi.ctype == 2 && pi.depth == 8) { pi.key_r &= 255; pi.key_g &= 255; pi.key_b &= 255; }

	/* the size it is drawn at: the larger side maxside, never larger than it was */
	ow = pi.w; oh = pi.h;
	if (pi.w > (unsigned)maxside || pi.h > (unsigned)maxside) {
		if (pi.w >= pi.h) { ow = maxside; oh = (pi.h * (unsigned)maxside * 2u + pi.w) / (2u * pi.w); }
		else { oh = maxside; ow = (pi.w * (unsigned)maxside * 2u + pi.h) / (2u * pi.h); }
		if (!ow) ow = 1;
		if (!oh) oh = 1;
	}
	if (out_cap < 12u + ow * oh * 2u) return -4;
	stride = (pi.w * pi.bits + 7u) / 8u;
	if (work_cap < ow * oh * 16u + 2u * (stride + 1u) + 16u) return -4;
	acc = (unsigned int *)work;                 /* R, G, B, count for each output pixel */
	memset(acc, 0, ow * oh * 16u);
	row = work + ow * oh * 16u;
	up = row + stride + 8u;

	memset(&z, 0, sizeof z);
	z.next_in = png;
	z.avail_in = idat;
	if (inflateInit(&z) != Z_OK) return -4;
	passes = pi.interlace ? pass7 : pass1;
	npass = pi.interlace ? 7 : 1;
	for (p = 0; p < npass && !rc; p++) {
		unsigned int x0 = passes[p][0], y0 = passes[p][1], dx = passes[p][2], dy = passes[p][3];
		unsigned int pw = pi.w > x0 ? (pi.w - x0 + dx - 1u) / dx : 0;
		unsigned int ph = pi.h > y0 ? (pi.h - y0 + dy - 1u) / dy : 0;
		unsigned int n = (pw * pi.bits + 7u) / 8u, bpp = pi.bits >= 8 ? pi.bits / 8u : 1u, y;
		if (!pw || !ph) continue;               /* a pass a small picture has none of */
		memset(up, 0, n + 1u);
		for (y = 0; y < ph; y++) {
			unsigned int sy = y0 + y * dy, oy = sy * oh / pi.h, x;
			unsigned char *t;
			if (pull(&z, row, n + 1u) != 0 || unfilter(row + 1, up + 1, n, bpp, row[0]) != 0) { rc = -3; break; }
			for (x = 0; x < pw; x++) {
				unsigned char c[4];
				unsigned int *a = acc + 4u * (oy * ow + (x0 + x * dx) * ow / pi.w);
				pixel(&pi, row + 1, x, c);
				a[0] += c[0] * c[3] / 255u;   /* over black */
				a[1] += c[1] * c[3] / 255u;
				a[2] += c[2] * c[3] / 255u;
				a[3]++;
			}
			t = row; row = up; up = t;
			if (tick && (y & 63u) == 63u) tick();
		}
	}
	inflateEnd(&z);
	if (rc) return rc;

	out[0] = 'G'; out[1] = 'A'; out[2] = 'R'; out[3] = 'T';   /* GBA_ART_MAGIC (gba_boxart.h) */
	out[4] = 1; out[5] = 0;                     /* version */
	out[6] = 0; out[7] = 0;                     /* RGB565 */
	out[8] = (unsigned char)ow; out[9] = (unsigned char)(ow >> 8);
	out[10] = (unsigned char)oh; out[11] = (unsigned char)(oh >> 8);
	for (i = 0; i < ow * oh; i++) {
		unsigned int *a = acc + 4u * i, n = a[3] ? a[3] : 1u;
		unsigned int r = (a[0] + n / 2u) / n, g = (a[1] + n / 2u) / n, b = (a[2] + n / 2u) / n;
		unsigned int v = ((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3);
		out[12 + 2 * i] = (unsigned char)v;
		out[13 + 2 * i] = (unsigned char)(v >> 8);
	}
	return (int)(12u + ow * oh * 2u);
}
