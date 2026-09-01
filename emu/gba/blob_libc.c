/*
 * blob_libc.c - the minimal libc bundled into the loadable core blob.
 *
 * When the gpSP core is linked INTO lk_a, these come from LK's own libc. As a standalone
 * boot_b blob it must carry them itself. gba_shim.c already stubs the file/time runtime
 * (fopen/fread/fflush/time/localtime); this fills the rest: mem/string ops (live, real
 * implementations) plus a bump allocator, and no-op stubs for the printf family (used
 * only by dead debug/trace paths - LK has no console). Compiled ONLY into the blob.
 */
typedef unsigned long size_t;

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dd = d; const unsigned char *ss = s;
	while (n--) *dd++ = *ss++;
	return d;
}
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dd = d; const unsigned char *ss = s;
	if (dd == ss || n == 0) return d;
	if (dd < ss) { while (n--) *dd++ = *ss++; }
	else { dd += n; ss += n; while (n--) *--dd = *--ss; }
	return d;
}
void *memset(void *d, int c, size_t n)
{
	unsigned char *dd = d;
	while (n--) *dd++ = (unsigned char)c;
	return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	while (n--) { if (*x != *y) return *x - *y; x++; y++; }
	return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) { a++; b++; n--; }
	return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n)
{
	char *r = d;
	while (n && (*d = *s)) { d++; s++; n--; }
	while (n--) *d++ = 0;
	return r;
}
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
char *strrchr(const char *s, int c) { const char *r = 0; for (; *s; s++) if (*s == (char)c) r = s; return (char *)((c == 0) ? s : r); }
/* strcasecmp, sscanf, strtol are provided by gba_shim.c (the core runtime shim). */

/* Bump allocator from a fixed blob heap. gpSP mostly uses fixed HAVE_MMAP buffers; this
 * covers any incidental malloc (config parsing etc.) safely without ever needing free. */
static unsigned char s_heap[256 * 1024] __attribute__((aligned(8)));
static unsigned long s_heap_used;
void *malloc(size_t n)
{
	unsigned long a = (s_heap_used + 7u) & ~7ul;
	if (a + n > sizeof(s_heap)) return 0;
	s_heap_used = a + n;
	return &s_heap[a];
}
void *calloc(size_t nm, size_t sz) { size_t n = nm * sz; void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void free(void *p) { (void)p; }
void *realloc(void *p, size_t n) { void *q = malloc(n); if (q && p) memcpy(q, p, n); return q; }

/* printf family: dead debug/trace paths only (no console in LK). No-op stubs. */
int printf(const char *f, ...) { (void)f; return 0; }
int sprintf(char *b, const char *f, ...) { (void)f; if (b) b[0] = 0; return 0; }
int snprintf(char *b, size_t n, const char *f, ...) { (void)f; if (b && n) b[0] = 0; return 0; }
int puts(const char *s) { (void)s; return 0; }
/* sscanf, strcasecmp, strtol come from gba_shim.c. */
