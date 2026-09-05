/*
 * Emulator rewind ring (see ayaneo_rewind.c). A ring of periodic save-states in a high-DRAM
 * region we map ourselves, shared by all cores (GB/GBC, SNES, GBA). The left trigger drives
 * rewind with a press-depth speed curve, mirroring the right-trigger fast-forward.
 *
 * Per-session use:
 *   ayaneo_rewind_reset(state_size)         once the core's state_size() is known (also on
 *                                           reset / load-state - anything that breaks the timeline)
 *   every K committed frames, when not FF / not rewinding / not in-menu:
 *       void *p = ayaneo_rewind_capture_begin();
 *       if (p) { core->state_save(p); ayaneo_rewind_capture_commit(actual_size); }
 *   while the left trigger is held:
 *       if (!ayaneo_rewind_active()) ayaneo_rewind_begin();
 *       ayaneo_rewind_step() one-or-more times (press depth), then
 *       const void *s = ayaneo_rewind_cur(&sz); core->state_load(s, sz);
 *   on release: ayaneo_rewind_end();
 */
#ifndef AYANEO_REWIND_H
#define AYANEO_REWIND_H

/* size + validation (also used by the oem rewindtest / oem rewindring selftests) */
unsigned int   ayaneo_rewind_map(void);       /* map the region once; returns bytes (0 = unavailable) */
unsigned int   ayaneo_rewind_phys(void);
unsigned char *ayaneo_rewind_base(void);
unsigned int   ayaneo_rewind_region(void);

/* ring lifecycle */
unsigned int ayaneo_rewind_reset(unsigned int max_payload);  /* (re)size + clear; returns slot count */
int          ayaneo_rewind_ready(void);
int          ayaneo_rewind_active(void);      /* currently rewinding */
unsigned int ayaneo_rewind_slots(void);
unsigned int ayaneo_rewind_count(void);       /* valid stored states */

/* capture (forward play) */
void *ayaneo_rewind_capture_begin(void);      /* payload ptr, or NULL if not ready / rewinding */
void  ayaneo_rewind_capture_commit(unsigned int size);

/* rewind (left trigger held) */
int         ayaneo_rewind_begin(void);
int         ayaneo_rewind_step(void);         /* one state older; <0 when at the oldest */
const void *ayaneo_rewind_cur(unsigned int *size_out);
void        ayaneo_rewind_end(void);          /* commit cursor as the new head, resume forward */

#endif /* AYANEO_REWIND_H */
