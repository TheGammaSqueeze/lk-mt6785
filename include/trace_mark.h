/*
 * trace_mark.h - a named point in the boot log of a BOOT_TRACE build (platform/common/boot_trace.c),
 * with the caller's address; nothing in any other build.
 */
#ifndef __TRACE_MARK_H__
#define __TRACE_MARK_H__

#ifdef BOOT_TRACE
#include <debug.h>
#define TRACE_MARK(what) dprintf(CRITICAL, "[MARK] %s (%s:%d)\n", what, __FILE__, __LINE__)
#else
#define TRACE_MARK(what) do { } while (0)
#endif

#endif /* __TRACE_MARK_H__ */
