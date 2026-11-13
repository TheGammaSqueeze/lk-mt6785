/*
 * pico_libmem.c - memory for the game library (pico_libmem.h): a bump allocator over
 * ayaneo_library_arena. Every table is taken once, the first time its module needs it.
 */
#include <string.h>
#include <kernel/thread.h>
#include "pico_libmem.h"

extern void *ayaneo_library_arena(unsigned int *size);

static unsigned char *s_base;
static unsigned int s_size, s_used;

void *pico_lib_alloc(unsigned int size)
{
	void *p = 0;
	enter_critical_section();
	if (!s_base) s_base = (unsigned char *)ayaneo_library_arena(&s_size);
	size = (size + 63u) & ~63u;
	if (s_base && size <= s_size - s_used) {
		p = s_base + s_used;
		s_used += size;
	}
	exit_critical_section();
	if (p) memset(p, 0, size);
	return p;
}

unsigned int pico_lib_left(void)
{
	return s_base ? s_size - s_used : 0;
}
