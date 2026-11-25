/*
 * pico_bigrun.h - a game core's emulation on a big CPU core (opt in, off by default).
 *
 * Every system has a Big Core option in its in-game menu. With it on, and on a console whose
 * firmware can power the other cores (platform/common/pico/pico_smp.h), the core's emulation runs
 * on one big core while the frontend (picture, sound, the card, the menus) stays on the boot core.
 * One guest serves every system: it lives from its first use until the console resets or powers
 * off, asleep between frames and between games.
 *
 * Two shapes of work:
 *   - a call: a function that returns (one emulated frame of the NES, SNES, Genesis and GB cores);
 *   - a loop: a function that never returns and hands each frame over with
 *     pico_bigrun_guest_yield (the GBA core's CPU loop).
 * The frontend waits for each piece of work, so the two sides never run the core at once.
 *
 * Code that runs on the guest is unprivileged and alone on its core: no scheduler, no interrupts
 * of the LK's. It must not sleep, lock or reach the console's shared buses (the PMIC, the clock
 * chip); what a core asks the LK for while it runs (the pad, the time) is latched on the boot core
 * before the frame and handed over from the latch (pico_bigrun_on_guest tells which side asks).
 */
#ifndef PICO_BIGRUN_H
#define PICO_BIGRUN_H

/* the systems, in the order the settings keep them (the per core CPU clock's order) */
enum { PICO_BIG_GBA, PICO_BIG_GBC, PICO_BIG_SNES, PICO_BIG_GENESIS, PICO_BIG_NES, PICO_BIG_NSYS };

/* The system's option is on (or forced by `oem smp:gba`) and a big core can be used now. */
int  pico_bigrun_wanted(int sys);

/* A game starts on the big core: the guest is started if it is not up yet, takes the clock the
 * menu chose and drops the code it had cached where the cores load (call it once the game's core
 * is in memory). 0 = ready, the guest idle at its top; else the game runs on the boot core. */
int  pico_bigrun_open(void);

/* A call on the guest, waited for. 0 = it returned. -1 = the guest faulted or stalled in it: the
 * core's state is torn, the game must close without running the core again (pico_bigrun_close
 * then puts the guest back on its feet). */
int  pico_bigrun_call(void (*fn)(void *), void *arg);

/* The two halves of a call, for a loop: kick sends the guest into fn (or, when it already sits in
 * that loop's yield, lets it go on), join waits for the frame's end as pico_bigrun_call does. */
void pico_bigrun_kick(void (*fn)(void *), void *arg);
int  pico_bigrun_join(void);

/* The game is over. A guest that died in it is started afresh; one that cannot be leaves the big
 * core unused until the next power on. */
void pico_bigrun_close(void);

/* On the guest, in a loop `me`: the frame is done; returns when the next one is asked for. Does
 * not return when the guest was given other work meanwhile (it leaves the loop for good). */
void pico_bigrun_guest_yield(void (*me)(void *));

/* True when the caller runs on the guest. */
int  pico_bigrun_on_guest(void);

/* The menu rows every system shows: the option and the big core's clock. */
enum { PICO_BIG_ROW_ON, PICO_BIG_ROW_CLOCK };
const char *pico_bigrun_row_label(int row);
/* the row's value written at p (the end is returned); active: this game runs on the big core */
char *pico_bigrun_row_value(int sys, int row, int active, char *p);
/* Left/Right/A on a row. 1 = a setting changed (the caller persists the settings); *note, when
 * set, is a line for the menu's status. Call it between frames only. */
int  pico_bigrun_row_change(int sys, int row, int dir, int act, const char **note);

/* Load testing (oem smp:noio,1): a running hash of the sound a game's core makes, frame by frame,
 * noted after 600, 1200 and 1800 frames with sound, and the time those frames took in the core
 * (t0: the 13 MHz tick before the frame). oem smp:fh reads them. A game started from reset with
 * no buttons pressed gives the same three hashes on the boot core and on the big core. */
void pico_bigrun_test_reset(void);
/* the game's system RAM (pico_core_memory.h), hashed too at the 1800th frame; after the reset */
struct pico_core_memory;
void pico_bigrun_test_memory(const struct pico_core_memory *m);
void pico_bigrun_test_frame(unsigned t0, const void *pcm, unsigned bytes);

#endif
