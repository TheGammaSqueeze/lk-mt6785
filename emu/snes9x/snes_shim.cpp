/*
 * Freestanding C++ runtime shim for the snes9x core inside LK (see snes_core_abi.h).
 *
 * Provides: a bump allocator over a caller-set DRAM region (operator new/delete AND C
 * malloc/free/calloc, which snes9x's Memory.Init uses); a static-constructor runner for
 * .init_array (libstdc++ ios_base::Init/locale must run - snes9x uses std::string/
 * stringstream/map, unlike gambatte); and the __throw_ and ABI stubs. Defining the
 * __throw_ stubs here (resolved before libstdc++ throwing versions are pulled) keeps the
 * C++ exception unwinder out of the blob even though we link libstdc++.a for std::string.
 */
#include <stddef.h>
#include <stdint.h>

/* ---- bump allocator over a DRAM region set by the LK driver ---- */
static uint8_t *g_base, *g_ptr, *g_end;

extern "C" void *memset(void *, int, unsigned long);   /* bundled libc */

extern "C" void snes_heap_init(void *base, unsigned size)
{
	g_base = (uint8_t *)base;
	g_ptr  = g_base;
	g_end  = g_base + size;
	/* CRITICAL: snes9x relies on malloc'd memory being ZERO (host malloc returns zeroed
	 * pages; a garbage-filled allocator crashes it - verified on host). This bump arena
	 * never reuses freed memory, so zeroing the whole region once here guarantees every
	 * allocation starts zeroed. Without this the emulation runs but produces frozen-black
	 * output (uninitialised SNES RAM/APU state). */
	memset(base, 0, (unsigned long)size);
}
extern "C" unsigned snes_heap_used(void) { return (unsigned)(g_ptr - g_base); }

static void *bump(size_t n)
{
	n = (n + 15u) & ~(size_t)15u;   /* 16-byte align */
	if (!g_ptr || g_ptr + n > g_end) return 0;
	void *p = g_ptr; g_ptr += n; return p;
}

void *operator new(size_t n)          { return bump(n); }
void *operator new[](size_t n)        { return bump(n); }
void  operator delete(void *)         {}
void  operator delete[](void *)       {}
void  operator delete(void *, size_t) {}
void  operator delete[](void *, size_t){}

extern "C" {

void *malloc(size_t n) { return bump(n); }
void *calloc(size_t a, size_t b)
{
	size_t n = a * b;
	uint8_t *p = (uint8_t *)bump(n);
	if (p) for (size_t i = 0; i < n; i++) p[i] = 0;
	return p;
}
void  free(void *) {}                 /* bump arena: freed en masse at session teardown */
void *realloc(void *, size_t n) { return bump(n); }   /* copy-less: snes9x realloc paths are rare */

/* ---- static constructors: run everything the linker put in .init_array ----
 * libstdc++'s ios_base::Init and locale facets register here; snes9x's std::string/
 * stringstream/map need them constructed before first use. gambatte discarded init_array
 * (no static ctors it needed); snes9x cannot. */
typedef void (*ctor_t)(void);
extern ctor_t __init_array_start[] __attribute__((weak));
extern ctor_t __init_array_end[]   __attribute__((weak));

void snes_run_init_array(void)
{
	for (ctor_t *p = __init_array_start; p < __init_array_end; p++)
		if (*p) (*p)();
}

/* ---- ABI stubs ---- */
void  __cxa_pure_virtual(void) { for (;;) {} }
void *__dso_handle = 0;
int   __cxa_atexit(void (*)(void *), void *, void *) { return 0; }

}

/* snes9x's memmap.cpp calls LoadZip (C++ linkage). ROMs are fed as raw buffers here, so
 * report "not a zip" (matches the C stubs in snes_blob_stubs.c for the C-linkage I/O).
 * Signature must match memmap.h: bool8 LoadZip(const char*, uint32*, uint8*) where
 * uint32 == unsigned long and bool8/uint8 == unsigned char in this build. */
unsigned char LoadZip(const char *, unsigned long *, unsigned char *) { return 0; }

/* std::__throw_* are provided by libstdc++'s functexcept.o (pulled in anyway); do NOT
 * redefine them here or the link sees multiple definitions. */
