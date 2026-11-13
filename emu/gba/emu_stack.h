/*
 * emu_stack.h - the emulator threads' stacks, out of the LK heap.
 *
 * The heap is only what the LK image leaves below its end of RAM (a few hundred KB), and each
 * core's thread wants 256 KB: the stacks live in a fixed DRAM window instead (ayaneo_emu_stack,
 * ayaneo_rewind.c), a slot per core, so playing one core never leaves another without room.
 */
#ifndef EMU_STACK_H
#define EMU_STACK_H

#include <kernel/thread.h>

enum { EMU_STACK_GBC, EMU_STACK_SNES, EMU_STACK_GENESIS };

extern void *ayaneo_emu_stack(unsigned int slot, unsigned int size);

/* the thread on its slot's stack; on the heap as before if the window is not there */
static inline thread_t *emu_thread_create(const char *name, thread_start_routine entry, void *arg,
					  int priority, unsigned int slot, unsigned int stack_size)
{
	void *stack = ayaneo_emu_stack(slot, stack_size);
	if (stack)
		return thread_create_on_stack(name, entry, arg, priority, stack, stack_size);
	return thread_create(name, entry, arg, priority, stack_size);
}

#endif
