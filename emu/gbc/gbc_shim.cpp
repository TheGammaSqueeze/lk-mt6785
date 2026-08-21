/*
 * Freestanding C++ runtime shim for the gambatte core running inside LK.
 *
 * LK is C with a tiny heap; the emulator needs a few MB and a working
 * operator new/delete plus a handful of libstdc++/ABI symbols that the core
 * pulls in via STL templates. We satisfy those here with a bump allocator over
 * a caller-provided DRAM region (compiled with -fno-exceptions -fno-rtti so no
 * exception/RTTI runtime is needed).
 */
#include <stddef.h>
#include <stdint.h>

extern "C" void *memset(void *, int, size_t);

/* ---- bump allocator over a DRAM region set by the LK driver ---- */
static uint8_t *g_base;
static uint8_t *g_ptr;
static uint8_t *g_end;

extern "C" void gbc_heap_init(void *base, unsigned size)
{
	g_base = (uint8_t *)base;
	g_ptr  = g_base;
	g_end  = g_base + size;
}

extern "C" unsigned gbc_heap_used(void)
{
	return (unsigned)(g_ptr - g_base);
}

static void *bump(size_t n)
{
	n = (n + 15u) & ~(size_t)15u;		/* 16-byte align */
	if (g_ptr + n > g_end)
		return 0;			/* out of arena */
	void *p = g_ptr;
	g_ptr += n;
	return p;
}

/* operator new/delete. delete is a no-op: the emulator allocates once at load
 * and lives for the whole session, so a bump-only arena is fine. */
void *operator new(size_t n)		{ return bump(n); }
void *operator new[](size_t n)		{ return bump(n); }
void  operator delete(void *)		{}
void  operator delete[](void *)		{}
void  operator delete(void *, size_t)	{}
void  operator delete[](void *, size_t)	{}

/* ---- ABI / libstdc++ stubs the core references ---- */
extern "C" {

void __cxa_pure_virtual(void) { for (;;) {} }
void *__dso_handle = 0;
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }

/* gambatte's optional libretro logging - no-op in LK */
void gambatte_log(int, const char *, ...) {}

}

namespace std {
/* STL containers call these on error paths; with -fno-exceptions they must not
 * return. Halt (should never happen with a valid ROM and sized arena). */
void __throw_length_error(char const *)  { for (;;) {} }
void __throw_bad_alloc()                 { for (;;) {} }
void __throw_out_of_range(char const *)  { for (;;) {} }
void __throw_out_of_range_fmt(char const *, ...) { for (;;) {} }
void __throw_logic_error(char const *)   { for (;;) {} }
void __throw_bad_function_call()         { for (;;) {} }
}
