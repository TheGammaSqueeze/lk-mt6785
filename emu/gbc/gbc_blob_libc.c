/*
 * gbc_blob_libc.c - the tiny libc the bundled gambatte core references, compiled into
 * the GB/GBC blob. The core needs only these few; __aeabi_* come from libgcc and powf
 * from libm (both linked into the blob), and the C++ runtime lives in gbc_shim.cpp.
 */
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dd = (unsigned char *)d;
	const unsigned char *ss = (const unsigned char *)s;
	while (n--) *dd++ = *ss++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dd = (unsigned char *)d;
	const unsigned char *ss = (const unsigned char *)s;
	if (dd == ss || n == 0) return d;
	if (dd < ss) { while (n--) *dd++ = *ss++; }
	else { dd += n; ss += n; while (n--) *--dd = *--ss; }
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *dd = (unsigned char *)d;
	while (n--) *dd++ = (unsigned char)c;
	return d;
}

int strcmp(const char *a, const char *b)
{
	while (*a && (*a == *b)) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* newlib libm's powf sets errno via *__errno(); give it somewhere to write. The core
 * ignores errno (color-correction gamma), so a single throwaway cell is fine. */
int *__errno(void)
{
	static int e;
	return &e;
}
