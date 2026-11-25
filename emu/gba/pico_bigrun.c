/*
 * pico_bigrun.c - a game core's emulation on a big CPU core (pico_bigrun.h).
 *
 * The guest is 32 bit code of the LK's, unprivileged, on a core of its own under a small host
 * (pico_smp.c). The two sides meet through two counters in the shared block: the frontend raises
 * GO, the guest answers with DONE when the work it was given is finished (a call returned, a loop
 * reached its frame's end). The guest sleeps in wfe between the two; each request ends in a sev.
 *
 * A guest that stalls is cut off by its own core: it arms the core's timer when a piece of work
 * starts, and the host holds it when the timer fires or when it faults. A held guest is started
 * again from its top without powering the core off (a core powered off and on again after the
 * boot core's clock moved resets the console).
 */
#include <string.h>
#include "pico_bigrun.h"
#include "pico_i18n.h"
#include "pico_core_memory.h"
#include "../../platform/common/pico/pico_smp.h"

extern unsigned gpt4_get_current_tick(void);   /* 13 MHz */
extern int  ayaneo_get_bigcore(int sys);
extern void ayaneo_set_bigcore(int sys, int on);
extern unsigned ayaneo_get_bigcore_mhz(void);
extern void ayaneo_set_bigcore_mhz(unsigned mhz);

/* the shared block's words (the host's use of them: pico_smp_gbahost64.S.src) */
#define W_GO    24
#define W_DONE  27
#define W_STOP  28
#define W_FAULT 37
#define W_STALL 39   /* oem smp:stall: the watchdog's and the fault path's tests */

/* where the cores load (gba, gbc, snes, genesis and nes _core_loader.c): the guest's cached code
 * there is dropped at each game start, a slot may hold another core than the last time */
#define CORE_SLOTS_START 0x4E400000u
#define CORE_SLOTS_END   0x50000000u

volatile int g_pico_big_force = -1;    /* oem smp:gba: 0 off, 1 on for every system; -1 the settings */
volatile unsigned int g_dbg_smp_deaths, g_dbg_smp_recovered;   /* oem smp:gst */
volatile unsigned int g_dbg_smp_wait_us;

static volatile int s_up;       /* the guest was started in this power on and is alive */
static volatile int s_broken;   /* it could not be recovered: boot core only until the next power on */
static volatile int s_dead;     /* it died in this game; pico_bigrun_close starts it again */
static void (*volatile s_fn)(void *);   /* the work the guest was last given */
static void *volatile s_arg;
static void *s_top[8];          /* the guest's top, where it leaves a loop to */

/* ---- on the guest ---- */

/* The frame watchdog: this core's virtual timer, armed while a piece of work runs. If it does not
 * end within half a second the timer's interrupt takes the core into the host, which holds the
 * guest (a frame takes a few milliseconds; the counter runs at 13 MHz). */
static inline void watchdog(int on)
{
	if (on) __asm__ volatile("mcr p15, 0, %0, c14, c3, 0" : : "r"(6500000u));   /* CNTV_TVAL */
	__asm__ volatile("mcr p15, 0, %0, c14, c3, 1" : : "r"(on ? 1u : 0u));       /* CNTV_CTL */
}

static void guest_wait(void)
{
	volatile unsigned *c = g_pico_smp_comm;
	while (c[W_GO] == c[W_DONE]) {
		if (c[W_STOP]) {        /* the console resets or powers off: this core off first */
			__asm__ volatile("push {r7}\n\tmovw r7, #0x00ff\n\tmovt r7, #0x000f\n\tsvc 0\n\tpop {r7}" : : : "memory");
		}
		__asm__ volatile("wfe");    /* asleep until the frontend's sev (a kick, a stop) */
	}
	__asm__ volatile("dmb" : : : "memory");   /* the work's function and argument were written before GO */
	watchdog(1);
	if (c[W_STALL] == 2u) __asm__ volatile(".inst.n 0xde00");   /* oem smp:stall,2: a guest fault's test */
	if (c[W_STALL]) for (;;) ;    /* oem smp:stall: the watchdog's test */
}

static void guest_main(void)
{
	volatile unsigned *c = g_pico_smp_comm;
	__builtin_setjmp(s_top);
	for (;;) {
		void (*f)(void *);
		guest_wait();
		f = s_fn;
		if (f) f(s_arg);        /* a call returns here; a loop never does */
		watchdog(0);
		c[W_DONE] = c[W_GO];
	}
}

void pico_bigrun_guest_yield(void (*me)(void *))
{
	volatile unsigned *c = g_pico_smp_comm;
	watchdog(0);                    /* the frame is done */
	c[W_DONE] = c[W_GO];
	PICO_SMP_MARK(15, c[W_DONE]);
	guest_wait();
	if (s_fn != me) __builtin_longjmp(s_top, 1);   /* other work: this loop's game is over */
}

int pico_bigrun_on_guest(void)
{
	/* the guest's stack is the free megabyte below the shared block; nothing else runs there */
	unsigned a = (unsigned)__builtin_frame_address(0);
	return a >= PICO_FREE_MB_BASE && a < PICO_SMP_GUEST_STACK_TOP;
}

/* the guest's cached code for [r[0], r[1]) dropped: the host's cache flush call */
static void flush_job(void *a)
{
	const unsigned *r = (const unsigned *)a;
	register unsigned r0 __asm__("r0") = r[0];
	register unsigned r1 __asm__("r1") = r[1];
	__asm__ volatile("push {r7}\n\tmovw r7, #0x0002\n\tmovt r7, #0x000f\n\tsvc 0\n\tpop {r7}"
			 : "+r"(r0) : "r"(r1) : "memory");
}

/* ---- on the boot core ---- */

static int usable(void) { return !s_broken && !s_dead && pico_smp_available(); }

int pico_bigrun_wanted(int sys)
{
	int on = g_pico_big_force >= 0 ? g_pico_big_force : ayaneo_get_bigcore(sys);
	return on && usable();
}

/* the clock the settings hold, as one of the console's levels (the top one when none is kept) */
static void clock_from_settings(void)
{
	int i, n = pico_smp_big_levels();
	unsigned want = ayaneo_get_bigcore_mhz();
	if (!n) return;
	for (i = 0; i < n; i++) if (pico_smp_big_level_mhz(i) / 10u == want / 10u) break;
	if (i == n) i = n - 1;
	pico_smp_big_clock(pico_smp_big_level_mhz(i));
}

void pico_bigrun_kick(void (*fn)(void *), void *arg)
{
	volatile unsigned *c = g_pico_smp_comm;
	s_arg = arg;
	s_fn = fn;
	__asm__ volatile("dsb" : : : "memory");
	c[W_GO]++;
	__asm__ volatile("dsb\n\tsev" : : : "memory");   /* wake the guest */
	PICO_SMP_MARK(13, c[W_GO]);
}

int pico_bigrun_join(void)
{
	volatile unsigned *c = g_pico_smp_comm;
	unsigned w0 = gpt4_get_current_tick();
	int r = 0;
	while (c[W_DONE] != c[W_GO]) {
		if (c[W_FAULT] || gpt4_get_current_tick() - w0 > 13u * 1000000u) {
			/* The guest faulted or its watchdog cut it off (the host holds it either way), or it
			 * gave nothing for a second and the host does not hold it: then the big core is left
			 * alone for the rest of this power on. */
			s_dead = 1; g_dbg_smp_deaths++;
			if (!pico_smp_guest_held()) s_broken = 1;
			r = -1;
			break;
		}
		__asm__ volatile("yield");
	}
	g_dbg_smp_wait_us = (gpt4_get_current_tick() - w0) / 13u;
	PICO_SMP_MARK(14, g_dbg_smp_wait_us);
	return r;
}

int pico_bigrun_call(void (*fn)(void *), void *arg)
{
	if (s_dead || s_broken || !s_up) return -1;
	pico_bigrun_kick(fn, arg);
	return pico_bigrun_join();
}

void pico_bigrun_close(void)
{
	if (!s_dead) return;
	s_dead = 0;
	if (!s_broken && pico_smp_guest_restart((unsigned)guest_main, PICO_SMP_GUEST_STACK_TOP) == 0) {
		s_fn = 0;
		g_dbg_smp_recovered++;
	} else {
		s_up = 0; s_broken = 1;
	}
}

int pico_bigrun_open(void)
{
	static unsigned range[2];
	unsigned core = g_pico_smp_gba_core;
	if (!usable()) return -1;
	clock_from_settings();          /* no work runs on the guest now */
	if (s_up && !pico_smp_core_on(core)) {
		/* powered off behind our back (a bring up command): not powered again in this power on */
		s_up = 0; s_broken = 1;
		return -1;
	}
	if (!s_up) {
		s_fn = 0;
		if (pico_smp_guest_start(core, (unsigned)guest_main, PICO_SMP_GUEST_STACK_TOP) != 0) return -1;
		s_up = 1;
	}
	/* the first work of every game: it also takes a guest still parked in the last game's loop
	 * back to its top */
	range[0] = CORE_SLOTS_START; range[1] = CORE_SLOTS_END;
	if (pico_bigrun_call(flush_job, range) != 0) {
		pico_bigrun_close();
		return -1;
	}
	return 0;
}

/* ---- load testing ---- */

extern volatile int g_gba_test_noio;
/* oem smp:fh: frames with sound, the hash now, at 600, 1200 and 1800, and the frames' time (us) */
volatile unsigned int g_dbg_big_fh[7];   /* [6]: the system RAM's hash at 1800 */
static const unsigned char *s_test_ram;
static unsigned s_test_ram_size;

void pico_bigrun_test_reset(void)
{
	g_dbg_big_fh[0] = 0; g_dbg_big_fh[1] = 2166136261u;
	g_dbg_big_fh[2] = g_dbg_big_fh[3] = g_dbg_big_fh[4] = g_dbg_big_fh[5] = g_dbg_big_fh[6] = 0;
	s_test_ram = 0; s_test_ram_size = 0;
}

void pico_bigrun_test_memory(const struct pico_core_memory *m)
{
	unsigned i;
	if (!m) return;
	s_test_ram = (const unsigned char *)m->system_ram; s_test_ram_size = m->system_ram_size;
	for (i = 0; (!s_test_ram || !s_test_ram_size) && m->descriptors && i < m->num_descriptors; i++)
		if ((m->descriptors[i].flags & PICO_MEMDESC_SYSTEM_RAM) && m->descriptors[i].ptr) {
			s_test_ram = (const unsigned char *)m->descriptors[i].ptr + m->descriptors[i].offset;
			s_test_ram_size = (unsigned)m->descriptors[i].len;
		}
	if (s_test_ram_size > 0x100000u) s_test_ram_size = 0x100000u;
}

void pico_bigrun_test_frame(unsigned t0, const void *pcm, unsigned bytes)
{
	const unsigned char *b = (const unsigned char *)pcm;
	unsigned us = (gpt4_get_current_tick() - t0) / 13u;
	unsigned h = g_dbg_big_fh[1], n, i;
	if (!g_gba_test_noio || !pcm || !bytes) return;
	for (i = 0; i < bytes; i++) h = (h ^ b[i]) * 16777619u;
	g_dbg_big_fh[1] = h;
	g_dbg_big_fh[5] += us;
	n = ++g_dbg_big_fh[0];
	if (n == 600u) g_dbg_big_fh[2] = h;
	else if (n == 1200u) g_dbg_big_fh[3] = h;
	else if (n == 1800u) {
		unsigned r = 2166136261u;
		g_dbg_big_fh[4] = h;
		for (i = 0; s_test_ram && i < s_test_ram_size; i++) r = (r ^ s_test_ram[i]) * 16777619u;
		g_dbg_big_fh[6] = r;
	}
}

/* ---- the menu rows ---- */

static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putu(char *p, unsigned v)
{
	char t[12]; int n = 0;
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = (char)('0' + v % 10u); v /= 10u; }
	while (n) *p++ = t[--n];
	return p;
}

const char *pico_bigrun_row_label(int row)
{
	return row == PICO_BIG_ROW_ON ? "Big Core" : "Big Core Clock";
}

char *pico_bigrun_row_value(int sys, int row, int active, char *p)
{
	if (!pico_smp_available() || (row == PICO_BIG_ROW_CLOCK && !pico_smp_big_levels()))
		return put(p, tr_igm("Not available"));
	if (row == PICO_BIG_ROW_ON) {
		int on = ayaneo_get_bigcore(sys);
		p = put(p, on ? "On" : "Off");
		if ((on != 0) != (active != 0)) { p = put(p, " "); p = put(p, tr_igm("(next game)")); }
		return p;
	}
	p = putu(p, g_pico_smp_gba_mhz);
	return put(p, " MHz");
}

int pico_bigrun_row_change(int sys, int row, int dir, int act, const char **note)
{
	if (note) *note = 0;
	if (row == PICO_BIG_ROW_ON) {
		if (!dir && !act) return 0;
		if (!pico_smp_available()) { if (note) *note = "Needs the multi-core firmware patch"; return 0; }
		ayaneo_set_bigcore(sys, !ayaneo_get_bigcore(sys));
		if (note) *note = "Takes effect at the next game start";
		return 1;
	} else {
		int i, n = pico_smp_big_levels();
		if (!dir || !n || !pico_smp_available()) return 0;
		for (i = 0; i < n; i++) if (pico_smp_big_level_mhz(i) >= g_pico_smp_gba_mhz) break;
		i += dir > 0 ? 1 : -1;
		if (i < 0) i = 0;
		if (i >= n) i = n - 1;
		pico_smp_big_clock(pico_smp_big_level_mhz(i));   /* the guest is idle while the menu is up */
		ayaneo_set_bigcore_mhz(g_pico_smp_gba_mhz);
		return 1;
	}
}
