/* ra-pico PC harness: the blob's heap calls, on the PC's own malloc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2026 TheGammaSqueeze
 */
#include "pico_libc.h"

void pico_heap_init(void *base, unsigned size) { (void)base; (void)size; }
unsigned pico_heap_used(void) { return 0; }
unsigned pico_heap_peak(void) { return 0; }
unsigned pico_heap_fails(void) { return 0; }
unsigned pico_heap_size(void) { return 0; }
