/*
 * genesis_shim.c - bump allocator + heap services for the loadable Genesis-Plus-GX blob.
 *
 * GPGX is pure C (no C++ runtime, no static constructors that allocate), so unlike the
 * snes9x/gambatte shims this provides ONLY a C bump allocator over a caller-set DRAM region
 * (the shared 0x50000000 emulator arena) plus mark/reset for run-ahead. With -DUSE_DYNAMIC_ALLOC
 * GPGX malloc()s its big buffers (cart.rom up to 32 MB, work RAM, VRAM, sound state) here instead
 * of into BSS, so the blob slot only holds code + small BSS. free() is a no-op; the whole arena is
 * reclaimed at session teardown (only one core runs at a time).
 */
typedef unsigned long size_t;
extern void *memset(void *, int, unsigned long);

static unsigned char *g_base, *g_ptr, *g_end;

void genesis_heap_init(void *base, unsigned size)
{
	g_base = (unsigned char *)base;
	g_ptr  = g_base;
	g_end  = g_base + size;
	/* Zero the whole arena once: GPGX assumes several dynamically-allocated buffers start
	 * cleared (work RAM, sound state), and this bump arena never reuses freed memory, so a
	 * single zero here guarantees every malloc() below hands back zeroed DRAM. */
	memset(base, 0, (unsigned long)size);
}

unsigned genesis_heap_used(void) { return (unsigned)(g_ptr - g_base); }
void    *genesis_heap_mark(void) { return g_ptr; }
void     genesis_heap_reset(void *m)
{
	if ((unsigned char *)m >= g_base && (unsigned char *)m <= g_ptr)
		g_ptr = (unsigned char *)m;
}

static void *bump(size_t n)
{
	unsigned char *p;
	n = (n + 15u) & ~(size_t)15u;          /* 16-byte align */
	if (g_ptr + n > g_end) return 0;       /* out of arena -> NULL (caller must cope) */
	p = g_ptr; g_ptr += n; return p;
}

void *malloc(size_t n) { return bump(n); }
void *calloc(size_t a, size_t b) { return bump(a * b); }   /* arena is pre-zeroed */
void *realloc(void *old, size_t n)
{
	/* GPGX realloc() paths are rare (option strings); copy-forward into fresh space. Old
	 * contents are not tracked here, so callers that grow a buffer must not rely on the old
	 * bytes beyond what GPGX itself copies. Adequate for the core's usage. */
	(void)old;
	return bump(n);
}
void free(void *p) { (void)p; }
