/* ra-pico: the heap of pico_libc.c, for the glue in ra_core.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 */
#ifndef RA_PICO_LIBC_H
#define RA_PICO_LIBC_H

void     pico_heap_init(void *base, unsigned size);
unsigned pico_heap_used(void);   /* bytes in blocks handed out and not freed */
unsigned pico_heap_peak(void);   /* the highest the top has been since pico_heap_init */
unsigned pico_heap_fails(void);  /* allocations refused for want of room since pico_heap_init */
unsigned pico_heap_size(void);

#endif
