/*
 * genesis_blob_stubs.c - freestanding fills for symbols GPGX's libretro layer references but the
 * blob does not functionally need (file I/O, path/strl helpers) plus a few libc pieces the shared
 * snes libc lacked (strstr/strdup/setjmp/longjmp/_ctype_/crc32) and no-op stubs for the excluded
 * yx5200 (DFPlayer MP3 cart). The ROM is fed from memory (GET_GAME_INFO_EXT) and saves go through
 * the ABI SRAM/state exports, so all real file access is stubbed to "no file".
 */
typedef unsigned long size_t;
extern void *memcpy(void *, const void *, size_t);
extern size_t strlen(const char *);
void *malloc(size_t);   /* genesis_shim.c bump allocator */

/* ---- libc gaps (the shared snes_blob_libc.c lacks these) ---- */
char *strstr(const char *h, const char *n)
{
	if (!*n) return (char *)h;
	for (; *h; h++) {
		const char *a = h, *b = n;
		while (*a && *b && *a == *b) { a++; b++; }
		if (!*b) return (char *)h;
	}
	return 0;
}
char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = (char *)malloc(n);
	if (d) memcpy(d, s, n);
	return d;
}

/* ARM EABI setjmp/longjmp (soft-float): save r4-r11, sp, lr. GPGX's Musashi 68000 can longjmp on a
 * bus/address error path; normal cartridge games never trigger it, but the symbols must resolve and
 * work if it does. jmp_buf from newlib setjmp.h is >= these 10 words. */
__attribute__((naked)) int setjmp(void *buf)
{
	__asm__ volatile(
		"stmia r0!, {r4-r11}\n"
		"str   sp, [r0], #4\n"
		"str   lr, [r0], #4\n"
		"mov   r0, #0\n"
		"bx    lr\n");
}
__attribute__((naked)) void longjmp(void *buf, int val)
{
	__asm__ volatile(
		"ldmia r0!, {r4-r11}\n"
		"ldr   sp, [r0], #4\n"
		"ldr   lr, [r0], #4\n"
		"movs  r0, r1\n"
		"moveq r0, #1\n"
		"bx    lr\n");
}

/* newlib-style ctype table (C locale). Indexed as _ctype_[c + 1]; index 0 covers EOF. */
#define _U 01
#define _L 02
#define _N 04
#define _S 010
#define _P 020
#define _C 040
#define _X 0100
#define _B 0200
const char _ctype_[1 + 256] = {
	0,
	_C,_C,_C,_C,_C,_C,_C,_C,                                  /* 00-07 */
	_C,_C|_S,_C|_S,_C|_S,_C|_S,_C|_S,_C,_C,                   /* 08-0F (tab..cr) */
	_C,_C,_C,_C,_C,_C,_C,_C,                                  /* 10-17 */
	_C,_C,_C,_C,_C,_C,_C,_C,                                  /* 18-1F */
	_S|_B,_P,_P,_P,_P,_P,_P,_P,                               /* 20-27 (space) */
	_P,_P,_P,_P,_P,_P,_P,_P,                                  /* 28-2F */
	_N,_N,_N,_N,_N,_N,_N,_N,                                  /* 30-37 */
	_N,_N,_P,_P,_P,_P,_P,_P,                                  /* 38-3F */
	_P,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U,                /* 40-47 (A-F) */
	_U,_U,_U,_U,_U,_U,_U,_U,                                  /* 48-4F */
	_U,_U,_U,_U,_U,_U,_U,_U,                                  /* 50-57 */
	_U,_U,_U,_P,_P,_P,_P,_P,                                  /* 58-5F */
	_P,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L,                /* 60-67 (a-f) */
	_L,_L,_L,_L,_L,_L,_L,_L,                                  /* 68-6F */
	_L,_L,_L,_L,_L,_L,_L,_L,                                  /* 70-77 */
	_L,_L,_L,_P,_P,_P,_P,_C,                                  /* 78-7F (DEL) */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* 80-8F */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* 90-9F */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* A0-AF */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* B0-BF */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* C0-CF */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* D0-DF */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                          /* E0-EF */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0                           /* F0-FF */
};

/* zlib crc32 (GPGX uses it for ROM CRC / cheats). crc32(0,NULL,0) returns 0 to init. */
unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{
	unsigned int k;
	if (!buf) return 0;
	crc = ~crc;
	while (len--) {
		crc ^= *buf++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (unsigned int)-(int)(crc & 1u));
	}
	return ~crc;
}

/* ---- retro strl helpers (renamed to dodge BSD libc) ---- */
size_t strlcpy_retro__(char *dst, const char *src, size_t size)
{
	size_t n = 0;
	if (size) { while (n < size - 1 && src[n]) { dst[n] = src[n]; n++; } dst[n] = 0; }
	while (src[n]) n++;
	return n;
}
size_t strlcat_retro__(char *dst, const char *src, size_t size)
{
	size_t dl = 0;
	while (dl < size && dst[dl]) dl++;
	return dl + strlcpy_retro__(dst + dl, src, size > dl ? size - dl : 0);
}
size_t fill_pathname_join(char *out, const char *dir, const char *path, size_t size)
{
	size_t n = strlcpy_retro__(out, dir, size);
	if (n && n < size && out[n - 1] != '/') { out[n++] = '/'; out[n] = 0; }
	return strlcat_retro__(out, path, size);
}

/* ---- file I/O: no filesystem in the blob; every access reports "no file" ---- */
void *filestream_open(const char *p, unsigned m, unsigned h) { (void)p;(void)m;(void)h; return 0; }
long  filestream_read(void *f, void *b, long n)  { (void)f;(void)b;(void)n; return 0; }
long  filestream_write(void *f, const void *b, long n) { (void)f;(void)b;(void)n; return 0; }
long  filestream_seek(void *f, long o, int w)    { (void)f;(void)o;(void)w; return -1; }
long  filestream_tell(void *f)                   { (void)f; return -1; }
int   filestream_close(void *f)                  { (void)f; return -1; }
char *filestream_gets(void *f, char *b, unsigned n) { (void)f;(void)b;(void)n; return 0; }
void  filestream_vfs_init(const void *i)         { (void)i; }
void *rfopen(const char *p, const char *m)       { (void)p;(void)m; return 0; }
long  rfread(void *b, long s, long n, void *f)   { (void)b;(void)s;(void)n;(void)f; return 0; }
long  rfwrite(const void *b, long s, long n, void *f) { (void)b;(void)s;(void)n;(void)f; return 0; }
int   rfseek(void *f, long o, int w)             { (void)f;(void)o;(void)w; return -1; }
long  rftell(void *f)                            { (void)f; return -1; }
int   rfclose(void *f)                           { (void)f; return -1; }
char *rfgets(char *b, int n, void *f)            { (void)b;(void)n;(void)f; return 0; }

/* ---- yx5200 (DFPlayer MP3 cart, excluded): no-op so md_cart still links ---- */
void yx5200_init(void)   {}
void yx5200_reset(void)  {}
void yx5200_write(unsigned a, unsigned d) { (void)a; (void)d; }
int  yx5200_update(int *b, int n) { (void)b; (void)n; return 0; }
int  yx5200_context_save(unsigned char *s) { (void)s; return 0; }
int  yx5200_context_load(unsigned char *s) { (void)s; return 0; }
