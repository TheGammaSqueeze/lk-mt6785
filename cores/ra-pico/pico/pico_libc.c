/* ra-pico: what the C library needs from an operating system, inside the bootloader.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 *
 * The blob links newlib for strings, formatting and maths. This file is the rest:
 *
 *   - the heap, over the arena the LK hands to heap_init (the rest of the RA slot). rcheevos
 *     allocates as it reads the server's answers (a game's set, its achievements' logic, the list
 *     the menu asks for) and frees them again when a game unloads. A first fit, address ordered
 *     free list with coalescing over a bump top, every block 16 byte aligned and handed out zeroed.
 *   - newlib's system calls, as stubs: no files, no console; the clock is the LK's.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include "pico_libc.h"

/* ---- heap ------------------------------------------------------------------------------------- */

#define HEAP_ALIGN 16u

/* Every block starts with this header, 16 bytes so payloads stay aligned. size is the payload's
 * capacity; next links the free list (address ordered) and is unused while the block is in use. */
struct block {
	uint32_t size;
	uint32_t magic;
	struct block *next;
	uint32_t pad;
};

#define HDR        ((uint32_t) sizeof(struct block))
#define MAGIC_USED 0x52414855u   /* "RAHU" */
#define MAGIC_FREE 0x46524545u

static uint8_t *h_base, *h_top, *h_end;
static struct block *h_free;
static uint32_t h_peak, h_live;

void pico_heap_init(void *base, unsigned size)
{
	uintptr_t b = ((uintptr_t) base + HEAP_ALIGN - 1u) & ~(uintptr_t) (HEAP_ALIGN - 1u);
	h_base = h_top = (uint8_t *) b;
	h_end = (uint8_t *) base + size;
	if (h_end < h_base) h_end = h_base;
	h_free = NULL;
	h_peak = h_live = 0;
}

unsigned pico_heap_used(void) { return h_live; }
unsigned pico_heap_peak(void) { return h_peak; }
unsigned pico_heap_size(void) { return (unsigned) (h_end - h_base); }

static int heap_owns(const void *p)
{
	return (const uint8_t *) p >= h_base + HDR && (const uint8_t *) p < h_top;
}

/* a free block that ends at the top goes back to the bump pointer */
static void heap_trim_top(void)
{
	struct block **pp = &h_free, **last = NULL;
	while (*pp) { last = pp; pp = &(*pp)->next; }
	if (last && (uint8_t *) *last + HDR + (*last)->size == h_top) {
		h_top = (uint8_t *) *last;
		*last = NULL;
	}
}

static struct block *heap_take(uint32_t cap)
{
	struct block **pp, *b;
	for (pp = &h_free; (b = *pp) != NULL; pp = &b->next) {
		if (b->size < cap) continue;
		if (b->size >= cap + HDR + HEAP_ALIGN) {   /* split: the tail stays free */
			struct block *t = (struct block *) ((uint8_t *) b + HDR + cap);
			t->size = b->size - cap - HDR;
			t->magic = MAGIC_FREE;
			t->next = b->next;
			*pp = t;
			b->size = cap;
		} else {
			*pp = b->next;
		}
		return b;
	}
	if ((size_t) (h_end - h_top) < (size_t) cap + HDR) return NULL;
	b = (struct block *) h_top;
	h_top += HDR + cap;
	if ((uint32_t) (h_top - h_base) > h_peak) h_peak = (uint32_t) (h_top - h_base);
	b->size = cap;
	return b;
}

void *malloc(size_t n)
{
	uint32_t cap;
	struct block *b;
	if (n > 0x7FFFFFF0u) { errno = ENOMEM; return NULL; }
	cap = ((uint32_t) n + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u);
	if (!cap) cap = HEAP_ALIGN;
	b = heap_take(cap);
	if (!b) { errno = ENOMEM; return NULL; }
	b->magic = MAGIC_USED;
	b->next = NULL;
	h_live += b->size;
	memset((uint8_t *) b + HDR, 0, cap);
	return (uint8_t *) b + HDR;
}

void free(void *p)
{
	struct block *b, **pp, *prev = NULL;
	if (!p || !heap_owns(p)) return;
	b = (struct block *) ((uint8_t *) p - HDR);
	if (b->magic != MAGIC_USED) return;
	b->magic = MAGIC_FREE;
	h_live -= b->size;
	for (pp = &h_free; *pp && *pp < b; pp = &(*pp)->next) prev = *pp;
	b->next = *pp;
	*pp = b;
	if (b->next && (uint8_t *) b + HDR + b->size == (uint8_t *) b->next) {   /* merge the next */
		b->size += HDR + b->next->size;
		b->next = b->next->next;
	}
	if (prev && (uint8_t *) prev + HDR + prev->size == (uint8_t *) b) {      /* and the previous */
		prev->size += HDR + b->size;
		prev->next = b->next;
	}
	heap_trim_top();
}

void *calloc(size_t a, size_t b)
{
	if (b && a > 0x7FFFFFF0u / b) { errno = ENOMEM; return NULL; }
	return malloc(a * b);   /* malloc hands out zeroed memory */
}

void *realloc(void *p, size_t n)
{
	struct block *b;
	void *q;
	if (!p) return malloc(n);
	if (!n) { free(p); return NULL; }
	if (!heap_owns(p)) return NULL;
	b = (struct block *) ((uint8_t *) p - HDR);
	if (b->magic != MAGIC_USED) return NULL;
	if (n <= b->size) return p;
	q = malloc(n);
	if (!q) return NULL;
	memcpy(q, p, b->size);
	free(p);
	return q;
}

/* ---- the system calls -------------------------------------------------------------------------- */

int _open(const char *path, int flags, int mode) { (void) path; (void) flags; (void) mode; errno = ENOENT; return -1; }
int _read(int fd, void *buf, size_t n) { (void) fd; (void) buf; (void) n; errno = EBADF; return -1; }
off_t _lseek(int fd, off_t off, int whence) { (void) fd; (void) off; (void) whence; errno = EBADF; return -1; }
int _close(int fd) { (void) fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void) fd; memset(st, 0, sizeof *st); st->st_mode = S_IFCHR; return 0; }
int _stat(const char *path, struct stat *st) { (void) path; (void) st; errno = ENOENT; return -1; }
int _isatty(int fd) { (void) fd; return 1; }
int _write(int fd, const void *buf, size_t n) { (void) fd; (void) buf; return (int) n; }

extern long pico_host_seconds(void);   /* ra_core.c: the LK's wall clock */

int _gettimeofday(struct timeval *tv, void *tz)
{
	(void) tz;
	if (tv) { tv->tv_sec = (time_t) pico_host_seconds(); tv->tv_usec = 0; }
	return 0;
}
/* clock(): rcheevos times nothing with it here (the LK's clock comes through now_ms) */
struct tms;
long _times(struct tms *t) { (void) t; return 0; }
int _kill(int pid, int sig) { (void) pid; (void) sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
void *_sbrk(ptrdiff_t incr) { (void) incr; errno = ENOMEM; return (void *) -1; }   /* malloc is ours */
void _exit(int code) { (void) code; for (;;) __asm__ volatile("wfi"); }
void abort(void) { _exit(1); }
char *getenv(const char *name) { (void) name; return NULL; }

/* newlib's reentrant entry points, so its own malloc (which would need _sbrk) is never linked */
struct _reent;
void *_malloc_r(struct _reent *r, size_t n) { (void) r; return malloc(n); }
void _free_r(struct _reent *r, void *p) { (void) r; free(p); }
void *_calloc_r(struct _reent *r, size_t a, size_t b) { (void) r; return calloc(a, b); }
void *_realloc_r(struct _reent *r, void *p, size_t n) { (void) r; return realloc(p, n); }
