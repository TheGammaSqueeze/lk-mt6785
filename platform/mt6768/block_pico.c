#include <arch/arm/mmu.h>
/*
 * GammaOS Pico on the Konkr Pocket Block: the board's own small pieces the shared Pico code calls.
 *
 * Here now: the L2 / R2 toggles (rewind, fast forward), the boot hooks around the emulator, the
 * CPU clock as CPU 0 really runs it, the battery level from its voltage, and the AUXADC read the
 * diagnostics use, and the power LED. The accelerometer is block_accel.c, the rumble motor
 * block_rumble.c, the fuel gauge block_gauge.c, the backlight strobe (BFI) ddp_pwm.c. The Block
 * has no MT6360.
 */
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <kernel/mutex.h>
#include <string.h>
#include <debug.h>
#include <printf.h>

extern unsigned int gpt4_get_current_tick(void);

/* ---- L2 and R2: plain buttons, toggling rewind and fast forward (GPIO 15 and 9, active low) ---- */
#define KPA_GPIO_L2    15u
#define KPA_GPIO_R2    9u
#include "../common/pico/pico_joypad_toggles.inc"

/* ---- the buttons ----
 * Every button shorts its GPIO to ground when pressed (read on the console: a held SELECT and A read
 * 0, as released ones do while pulled down). The LK's boot pin table, from the stock device tree,
 * leaves them pulled DOWN, so a released button reads 0 too, the same as a pressed one; Android's
 * gamepad state pulls them up. This puts them as Android has them: GPIO mode, input, Schmitt
 * trigger, pull up. L2 and R2 are set up by their toggle code (pico_joypad_toggles.inc). The pull
 * up charges each line slowly (1 ms after it a released button still read 0), so it is done in
 * platform_init, right after the device tree's defaults, and a later call (the SD gate's combo
 * read) waits until 300 ms have passed since then. */
extern int mt_set_gpio_mode(unsigned int pin, unsigned int mode);
extern int mt_set_gpio_dir(unsigned int pin, unsigned int dir);
extern int mt_set_gpio_smt(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_enable(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_select(unsigned int pin, unsigned int select);
extern void udelay(unsigned long usec);
#define BUTTONS_SETTLE_TICKS	(13000u * 300u)		/* 300 ms of the 13 MHz GPT */
static unsigned int s_buttons_up_tick;
void block_buttons_init(void)
{
	/* A 4, B 5, X 3, Y 6, L1 14, R1 10, START 7, SELECT 24, d-pad 25 27 28 26, MODE 17, and the
	 * gpio-keys node's four (volume down 16, back 19, the two side keys 22 and 21), which Android
	 * sets up the same way (pull up, input) */
	static const unsigned char pins[] = { 4, 5, 3, 6, 14, 10, 7, 24, 25, 27, 28, 26, 17, 16, 19, 22, 21 };
	static int done;
	unsigned int i;

	if (done) {
		/* a reader right after the pull up waits until the lines have had their time: on this
		 * board they took over 1 ms (a debounce capacitor on each), well under 300 */
		while ((unsigned int)(gpt4_get_current_tick() - s_buttons_up_tick) < BUTTONS_SETTLE_TICKS)
			udelay(1000);
		return;
	}
	for (i = 0; i < sizeof(pins); i++) {
		unsigned int p = pins[i] | 0x80000000u;
		mt_set_gpio_mode(p, 0);
		mt_set_gpio_dir(p, 0);
		mt_set_gpio_smt(p, 1);
		mt_set_gpio_pull_enable(p, 1);
		mt_set_gpio_pull_select(p, 1);
	}
	s_buttons_up_tick = gpt4_get_current_tick();
	done = 1;
}

/* ---- the boot around the emulator ---- */
#include <platform/boot_mode.h>
/* a normal boot with a card in may go to the GammaOS menu (the card's boot.cfg decides) */
int ayaneo_logo_defer(void)
{
	extern int gba_sd_card_present(void);
	return (g_boot_mode == NORMAL_BOOT || g_boot_mode == SW_REBOOT) && gba_sd_card_present();
}

/* the last step a game launch reached (gba_driver.c, snes_sd_run.c), for the hang recorder */
volatile unsigned g_block_launch_mark;

#ifdef BOOT_TRACE
/* The scheduler's last switches, for a lock up that stops every thread: kernel/thread.c notes
 * each switch here, a page of the pstore carve-out the kernel uses only once it runs (below the
 * boot log at 0x4D0E0000), mapped non-cacheable so every note is in DRAM when a watchdog reset
 * comes. The next GammaOS Pico start writes them into its boot log (oem savenow / savedtrace). */
#define BLOCK_SCHED_PA		0x4D0DF000u
#define BLOCK_SCHED_MAGIC	0x53434844u	/* "SCHD" */
#define BLOCK_SCHED_N		32u
#define BLOCK_IRQ_N		64u
#define BLOCK_CORE_MARK		(*(volatile unsigned int *)(BLOCK_SCHED_PA + 0x800u))	/* a debug core's progress */
struct block_sched_log {
	unsigned int magic, next;
	struct { unsigned int tick; char name[12]; } e[BLOCK_SCHED_N];
	unsigned int irq_next;
	struct { unsigned int tick, irq, pc; } q[BLOCK_IRQ_N];	/* the last interrupts */
};
static volatile struct block_sched_log *s_sched;

void block_sched_note(const char *name)
{
	volatile struct block_sched_log *l = s_sched;
	unsigned int i, k;
	extern unsigned int gpt4_get_current_tick(void);

	if (!l)
		return;
	k = l->next % BLOCK_SCHED_N;
	l->e[k].tick = gpt4_get_current_tick();
	for (i = 0; i < sizeof(l->e[k].name) - 1u && name[i]; i++)
		l->e[k].name[i] = name[i];
	l->e[k].name[i] = 0;
	l->next = k + 1u;
}

void block_irq_note(unsigned int irq, unsigned int pc)
{
	volatile struct block_sched_log *l = s_sched;
	unsigned int k;
	extern unsigned int gpt4_get_current_tick(void);

	if (!l)
		return;
	k = l->irq_next % BLOCK_IRQ_N;
	l->q[k].tick = gpt4_get_current_tick();
	l->q[k].irq = irq;
	l->q[k].pc = pc;
	l->irq_next = k + 1u;
}

/* the switches and interrupts the boot before left, oldest first, into this boot's log; then a
 * fresh log */
static void block_sched_report_and_start(void)
{
	extern int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count);
	volatile struct block_sched_log *l = (volatile struct block_sched_log *)BLOCK_SCHED_PA;
	char line[128];
	unsigned int i;

	if (arch_mmu_map((uint64_t)BLOCK_SCHED_PA, (vaddr_t)BLOCK_SCHED_PA,
			 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, 0x1000u) != 0)
		return;
	if (l->magic == BLOCK_SCHED_MAGIC) {
		_dputs("[SCHED] the last switches of the boot before, oldest first:\n");
		for (i = 0; i < BLOCK_SCHED_N; i++) {
			unsigned int k = (l->next + i) % BLOCK_SCHED_N;
			l->e[k].name[sizeof(l->e[k].name) - 1u] = 0;
			snprintf(line, sizeof(line), "[SCHED] %u us %s\n", l->e[k].tick / 13u,
				 (const char *)l->e[k].name);
			_dputs(line);
		}
		snprintf(line, sizeof(line), "[CORE] last progress mark %x, memset %x bytes at %x (call %u from %x)\n",
			 BLOCK_CORE_MARK, (&BLOCK_CORE_MARK)[2], (&BLOCK_CORE_MARK)[1], (&BLOCK_CORE_MARK)[3],
			 (&BLOCK_CORE_MARK)[4]);
		_dputs(line);
		snprintf(line, sizeof(line), "[CORE] constructor %x at %x; fault path step %x\n", (&BLOCK_CORE_MARK)[5],
			 (&BLOCK_CORE_MARK)[6], (&BLOCK_CORE_MARK)[8]);
		_dputs(line);
		_dputs("[IRQ] the last interrupts of the boot before, oldest first:\n");
		for (i = 0; i < BLOCK_IRQ_N; i++) {
			unsigned int k = (l->irq_next + i) % BLOCK_IRQ_N;
			snprintf(line, sizeof(line), "[IRQ] %u us irq %u pc %08x\n", l->q[k].tick / 13u,
				 l->q[k].irq, l->q[k].pc);
			_dputs(line);
		}
	}
	for (i = 0; i < BLOCK_IRQ_N; i++)
		l->q[i].tick = l->q[i].irq = l->q[i].pc = 0;
	l->irq_next = 0;
	for (i = 0; i < 9u; i++)
		(&BLOCK_CORE_MARK)[i] = 0;
	for (i = 0; i < BLOCK_SCHED_N; i++) {
		l->e[i].tick = 0;
		l->e[i].name[0] = 0;
	}
	l->next = 0;
	l->magic = BLOCK_SCHED_MAGIC;
	s_sched = l;
}

/* The bring-up build's hang recorder for GammaOS Pico: a thread at the top priority looks every
 * 100 ms, writes a line with what a game launch has reached to the boot log when it changes (and
 * every 2 s) (kept in RAM over a
 * warm reset) and feeds a 10 s hardware watchdog, re-arming it when a core switched it off. A
 * lock up (interrupts off, a core gone) stops the feeding, the watchdog does a warm reset, and
 * the next boot (the menu again) has the last lines: oem savenow, then oem savedtrace. The
 * lines go straight to the log (_dputs): at the KPA's log level dprintf and _dprintf print nothing. */
#include <kernel/thread.h>
#include <kernel/mutex.h>
#define BLOCK_WDT_MODE		0x10007000u	/* TOPRGU WDT_MODE, bit 0 = enabled */
#define BLOCK_HANG_TICK_MS	100u	/* a look every 100 ms: a line when the launch step changes */
#define BLOCK_HANG_BEAT_TICKS	20u	/* and one every 2 s regardless */

static int block_hang_recorder(void *arg)
{
	extern void mtk_wdt_arm(unsigned int seconds);
	extern void mtk_wdt_restart(void);
	extern void boot_trace_flush(void);
	extern unsigned int gpt4_get_current_tick(void);
	extern volatile unsigned g_snes_dbg_stage, g_snes_dbg_frames, g_snes_dbg_exit;
	extern volatile unsigned g_snes_dbg_loaderr, g_snes_dbg_romsz, g_snes_dbg_loadrc;
	unsigned int beat = 0, ticks = 0, last_mark = ~0u, last_stage = ~0u;
	char line[128];

	(void)arg;
	for (;;) {
		unsigned int mark = g_block_launch_mark, stage = g_snes_dbg_stage;

		if (!(*(volatile unsigned int *)BLOCK_WDT_MODE & 1u))
			mtk_wdt_arm(10);
		else
			mtk_wdt_restart();
		if (mark != last_mark || stage != last_stage || ++ticks >= BLOCK_HANG_BEAT_TICKS) {
			ticks = 0;
			last_mark = mark;
			last_stage = stage;
			snprintf(line, sizeof(line),
				 "[HANG] %u ms beat %u launch %x core %x snes stage %u frames %u exit %u loaderr %u rom %u rc %u\n",
				 gpt4_get_current_tick() / 13000u, beat++, mark, s_sched ? BLOCK_CORE_MARK : 0u, stage, g_snes_dbg_frames,
				 g_snes_dbg_exit, g_snes_dbg_loaderr, g_snes_dbg_romsz, g_snes_dbg_loadrc);
			_dputs(line);
			boot_trace_flush();	/* the ring out of the data cache: a watchdog reset does not write it back */
		}
		thread_sleep(BLOCK_HANG_TICK_MS);
	}
	return 0;
}
#endif

/* GammaOS Pico runs from here on: the boot is recorded as one that reached its system, and the
 * bring-up build starts its hang recorder */
void ayaneo_emu_boot_started(void)
{
#ifdef BOOT_TRACE
	extern void boot_trace_mark(unsigned int flag);
	thread_t *t;
	boot_trace_mark(0x20u);
	block_sched_report_and_start();
	t = thread_create("block_hang", block_hang_recorder, NULL, HIGHEST_PRIORITY - 1, 4096);
	if (t)
		thread_resume(t);
	_dputs("[BLOCK] GammaOS Pico running, hang recorder on\n");
#endif
}

/* the boot goes to Android after all: the held back logo now, and the game audio, if a core
 * started it, stopped */
void ayaneo_emu_boot_abandoned(void)
{
	extern void ayaneo_show_deferred_logo(void);
	extern void ayaneo_gbc_audio_shutdown(void);
	extern void block_cpu_rail_restore(void);
	ayaneo_show_deferred_logo();
	ayaneo_gbc_audio_shutdown();
	block_cpu_rail_restore();	/* the kernel starts from the rail and clock the preloader left */
}

/* ---- the CPU clock ----
 * The LK and every core run on CPU 0, a Cortex-A55. Its clock is measured rather than read from a
 * PLL: which of the MT6768 ARM PLLs drives the A55 cluster is still to be confirmed on the device
 * (at the top OPPs the frequency hopping block showed armpll at 2.0 GHz, the A75s' top, and
 * armpll_l at 1.8 GHz, the A55s'). A loop of dependent adds retires one a cycle on the A55, timed
 * against the 13 MHz GPT. The clock is not changed yet: the A55 rail's voltage at boot is not
 * known, and no voltage is ever changed, so a higher clock waits until it has been read. */

static unsigned int measure_cpu_mhz(void)
{
	unsigned int t0, t1, i, x = 0;
	t0 = gpt4_get_current_tick();
	for (i = 0; i < 4096u; i++) {
		__asm__ volatile(
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			"add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
			: "+r"(x));
	}
	t1 = gpt4_get_current_tick();
	if (t1 == t0)
		return 0;
	/* 4096 * 32 adds in (t1 - t0) ticks of 1/13 us */
	return (unsigned int)((4096ull * 32ull * 13ull) / (unsigned long long)(t1 - t0));
}

/* ---- the CPU clock ----
 * The LK runs on CPU0, an A55 of cluster 0, whose clock is the APMIXED PLL with CON1 at 0x1000C21C
 * (measured: lowering that word lowers the core in step; the PLL at 0x20C is left alone). Output
 * = 26 MHz x PCW / 2^14 / 2^POSDIV (CON1 bits 21:0 and 26:24), a new word taken on the rising edge
 * of bit 31.
 *
 * Each clock gets the voltage the Block's own table gives it (the device tree's cluster0_opp, the
 * table Android scales by), on the MT6358's two CPU bucks VPROC11 and VPROC12 (0.5 V + 6.25 mV a
 * code, both set alike). The preloader leaves them at 0.8 V, which is the floor here: the rail is
 * raised for the faster steps (up to 1.00625 V for 1800 MHz) and brought back to 0.8 V for the
 * rest, never lower. The SRAM supplies follow by themselves: the PMIC's tracking holds each
 * VSRAM_PROC 100 mV over its buck, between 0.85 V and 1.11875 V, as the preloader set it up.
 *
 * The order is the safe one: up = the voltage first, checked (both bucks reporting the new code,
 * both SRAM supplies over it), then the clock; down = the clock first, then the voltage. Every
 * PMIC step starts with the link's own test word (0x5AA5 at DEW_READ_TEST): if the PMIC cannot be
 * read correctly nothing is written, and the clock stays within what the rail is known to hold. */
#define BLOCK_CPU_PLL_CON1	0x1000C21Cu
#define BLOCK_CPU_MIN_MHZ	500u
#define BLOCK_CPU_MAX_MHZ	1800u

#define PMIC_DEW_READ_TEST	0x040eu		/* reads 0x5AA5 while the link is sound */
#define PMIC_VPROC11_DBG0	0x139eu		/* bits 6:0 the code the buck runs at */
#define PMIC_VPROC11_ELR0	0x13a6u		/* bits 6:0 the code asked for */
#define PMIC_VPROC12_DBG0	0x141eu
#define PMIC_VPROC12_ELR0	0x1426u
#define PMIC_VSRAM11_DBG0	0x1b60u		/* bits 14:8 the SRAM supply's code */
#define PMIC_VSRAM11_TRACK0	0x1b64u		/* bits 1 and 2: tracking its buck while on */
#define PMIC_VSRAM12_DBG0	0x1ba2u
#define PMIC_VSRAM12_TRACK0	0x1b6cu
#define VPROC_UV(code)		(500000u + 6250u * (code))
#define VPROC_CODE_FOR(uv)	(((uv) - 500000u + 6249u) / 6250u)
#define VPROC_BOOT_CODE		0x30u		/* 0.8 V, as the preloader leaves it */
#define VPROC_MAX_CODE		0x52u		/* 1.0125 V: the table's top (1.00625 V) rounded up */
#define VPROC_STEP_CODES	8u		/* 50 mV a step, each one checked */
#define VSRAM_TRACK_OFFSET	16u		/* the SRAM supply runs this many codes over its buck */

static const struct { unsigned short mhz; unsigned int uv; } s_cpu_opp[] = {
	{ 500, 650000 }, { 774, 675000 }, { 850, 700000 }, { 900, 718750 }, { 950, 731250 },
	{ 999, 743750 }, { 1050, 762500 }, { 1100, 775000 }, { 1175, 800000 }, { 1275, 831250 },
	{ 1325, 843750 }, { 1375, 856250 }, { 1450, 881250 }, { 1500, 893750 }, { 1625, 931250 },
	{ 1800, 1006250 },
};

extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
extern signed int pwrap_write(unsigned int adr, unsigned int wdata);

volatile unsigned int g_dbg_cpu_cap_mhz, g_dbg_cpu_rail_uv;	/* the clock the rail allows, the rail */
volatile unsigned int g_dbg_cpu_vfail;				/* voltage steps refused or not taken */
static unsigned int s_rail_code;	/* the bucks' code as last set and checked; 0 = not read yet */
static mutex_t s_cpu_lock;
static int s_cpu_lock_up;

static int pmic_link_ok(void)
{
	unsigned int v = 0;
	return pwrap_read(PMIC_DEW_READ_TEST, &v) == 0 && (v & 0xffffu) == 0x5aa5u;
}

/* both bucks' running code (the lower of the two), or -1 when the PMIC cannot be read correctly */
static int vproc_code_now(void)
{
	unsigned int a = 0, b = 0;
	if (!pmic_link_ok() || pwrap_read(PMIC_VPROC11_DBG0, &a) || pwrap_read(PMIC_VPROC12_DBG0, &b) ||
	    !pmic_link_ok())
		return -1;
	a &= 0x7fu;
	b &= 0x7fu;
	return (int)(a < b ? a : b);
}

/* is the PMIC holding both SRAM supplies over the bucks at `code` (tracking on, and there)? */
static int vsram_follows(unsigned int code)
{
	unsigned int t11 = 0, t12 = 0, s11 = 0, s12 = 0, want = code + VSRAM_TRACK_OFFSET;
	if (pwrap_read(PMIC_VSRAM11_TRACK0, &t11) || pwrap_read(PMIC_VSRAM12_TRACK0, &t12) ||
	    pwrap_read(PMIC_VSRAM11_DBG0, &s11) || pwrap_read(PMIC_VSRAM12_DBG0, &s12))
		return 0;
	if ((t11 & 6u) != 6u || (t12 & 6u) != 6u)
		return 0;
	s11 = (s11 >> 8) & 0x7fu;
	s12 = (s12 >> 8) & 0x7fu;
	if (want > 0x63u) want = 0x63u;		/* the tracking's own ceiling, 1.11875 V */
	return s11 >= want && s12 >= want;
}

/* move both bucks to `code`, one checked step at a time; returns the code they reached */
static unsigned int vproc_move(unsigned int from, unsigned int code)
{
	unsigned int cur = from;
	while (cur != code) {
		unsigned int next = code > cur ? (code - cur > VPROC_STEP_CODES ? cur + VPROC_STEP_CODES : code)
					       : (cur - code > VPROC_STEP_CODES ? cur - VPROC_STEP_CODES : code);
		unsigned int e11 = 0, e12 = 0, t;
		int now = -1;
		if (!pmic_link_ok() || pwrap_read(PMIC_VPROC11_ELR0, &e11) || pwrap_read(PMIC_VPROC12_ELR0, &e12)) {
			g_dbg_cpu_vfail++;
			break;
		}
		{	/* VPROC11 is the big cluster's buck: when one of its cores runs (opt in,
			 * platform/common/pico/pico_smp.c) it is never taken below what that core's
			 * clock needs */
			extern volatile unsigned g_pico_big_rail_code;
			unsigned int big = g_pico_big_rail_code;
			pwrap_write(PMIC_VPROC11_ELR0, (e11 & 0xff80u) | (big > next ? big : next));
		}
		pwrap_write(PMIC_VPROC12_ELR0, (e12 & 0xff80u) | next);
		for (t = 0; t < 100u; t++) {		/* the buck slews in tens of microseconds */
			udelay(10);
			now = vproc_code_now();
			if (now == (int)next && (next < cur || vsram_follows(next)))
				break;
		}
		if (now != (int)next || (next > cur && !vsram_follows(next))) {
			g_dbg_cpu_vfail++;
			if (now >= 0) cur = (unsigned int)now;
			break;
		}
		cur = next;
		udelay(20);				/* settle before anything leans on it */
	}
	g_dbg_cpu_rail_uv = VPROC_UV(cur);
	return cur;
}

static unsigned int opp_code_for(unsigned int mhz)
{
	unsigned int i, uv = s_cpu_opp[sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0]) - 1].uv, code;
	for (i = 0; i < sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0]); i++)
		if (s_cpu_opp[i].mhz >= mhz) { uv = s_cpu_opp[i].uv; break; }
	code = VPROC_CODE_FOR(uv);
	if (code < VPROC_BOOT_CODE) code = VPROC_BOOT_CODE;
	if (code > VPROC_MAX_CODE) code = VPROC_MAX_CODE;
	return code;
}

/* the fastest clock of the table the rail at `code` supplies */
static unsigned int cap_for_code(unsigned int code)
{
	unsigned int uv = VPROC_UV(code), cap = BLOCK_CPU_MIN_MHZ, i;
	for (i = 0; i < sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0]); i++)
		if (s_cpu_opp[i].uv <= uv && s_cpu_opp[i].mhz > cap)
			cap = s_cpu_opp[i].mhz;
	return cap;
}

unsigned int ayaneo_get_cpu_mhz(void)
{
	unsigned int con1 = *(volatile unsigned int *)BLOCK_CPU_PLL_CON1;
	unsigned long long f = 26000000ull * (con1 & 0x3FFFFFu);
	f >>= 14;
	f >>= (con1 >> 24) & 7u;
	return (unsigned int)(f / 1000000ull);
}

/* the PLL to `mhz`, in steps of at most 200 MHz so the core never takes one large jump */
static void cpu_pll_to(unsigned int mhz)
{
	volatile unsigned int *con1 = (volatile unsigned int *)BLOCK_CPU_PLL_CON1;
	unsigned int cur = ayaneo_get_cpu_mhz();
	for (;;) {
		unsigned int c = *con1, posdiv = (c >> 24) & 7u, step;
		unsigned long long pcw;
		if (cur + 200u < mhz) step = cur + 200u;
		else if (mhz + 200u < cur) step = cur - 200u;
		else step = mhz;
		pcw = (((unsigned long long)step * 1000000ull) << 14) << posdiv;
		pcw /= 26000000ull;
		if (pcw > 0x3FFFFFu) pcw = 0x3FFFFFu;
		c = (c & ~0x803FFFFFu) | (unsigned int)pcw;
		*con1 = c;
		*con1 = c | 0x80000000u;
		udelay(20);
		if (step == mhz)
			break;
		cur = step;
	}
}

volatile unsigned int g_dbg_cpu_mhz_asked;	/* the last clock a core asked for */
/* A netplay session (pico_netplay.c, user 11-16: "in multiplayer mode, we set the clock on the
 * system to the highest"): the CPU at its top clock for the whole session, whatever a core asks;
 * what it asks is kept and comes back when the session ends. */
static int s_cpu_netplay;
void ayaneo_set_cpu_mhz(unsigned int mhz);
void pico_cpu_netplay(int on)
{
	unsigned int asked = g_dbg_cpu_mhz_asked;
	s_cpu_netplay = on;
	ayaneo_set_cpu_mhz(on ? BLOCK_CPU_MAX_MHZ : (asked ? asked : BLOCK_CPU_MAX_MHZ));
	g_dbg_cpu_mhz_asked = asked;			/* the core's own choice, for the end */
}
void ayaneo_set_cpu_mhz(unsigned int mhz)
{
	unsigned int want, have, cap;
	int now;

	if (!s_cpu_lock_up) {
		enter_critical_section();
		if (!s_cpu_lock_up) { mutex_init(&s_cpu_lock); s_cpu_lock_up = 1; }
		exit_critical_section();
	}
	mutex_acquire(&s_cpu_lock);
	g_dbg_cpu_mhz_asked = mhz;
	if (s_cpu_netplay) mhz = BLOCK_CPU_MAX_MHZ;	/* netplay: the top clock throughout */
	if (mhz > BLOCK_CPU_MAX_MHZ) mhz = BLOCK_CPU_MAX_MHZ;
	if (mhz < BLOCK_CPU_MIN_MHZ) mhz = BLOCK_CPU_MIN_MHZ;

	now = vproc_code_now();
	if (now < 0) {
		/* the PMIC cannot be trusted now: no voltage change, and the clock no higher than what
		 * the rail was last known to hold (the preloader's 0.8 V before anything was set) */
		g_dbg_cpu_vfail++;
		have = s_rail_code ? s_rail_code : VPROC_BOOT_CODE;
	} else {
		have = (unsigned int)now;
		want = opp_code_for(mhz);
		if (want > have)
			have = vproc_move(have, want);		/* up: the voltage before the clock */
	}
	s_rail_code = have;
	cap = cap_for_code(have);
	g_dbg_cpu_cap_mhz = cap;
	g_dbg_cpu_rail_uv = VPROC_UV(have);
	if (mhz > cap) mhz = cap;
	cpu_pll_to(mhz);
	if (now >= 0) {
		want = opp_code_for(mhz);
		if (want < have) {				/* down: the clock first, then the voltage */
			s_rail_code = vproc_move(have, want);
			g_dbg_cpu_cap_mhz = cap_for_code(s_rail_code);
		}
	}
	mutex_release(&s_cpu_lock);
}

/* back to the preloader's 0.8 V and the clock it supplies, before the kernel takes the rail */
void block_cpu_rail_restore(void)
{
	ayaneo_set_cpu_mhz(1175);
}

/* ---- run-ahead, as it runs (oem ra) ----
 * Each core (1 GBC, 2 SNES, 3 Genesis, 4 NES; GBA has its own in oem diag) reports every frame that ran
 * ahead: how many look-ahead frames, and the time of the save and those frames, the part run-ahead
 * adds to a frame. Read and cleared by oem ra. */
volatile unsigned int g_block_ra_n[5], g_block_ra_frames[5], g_block_ra_us[5], g_block_ra_max[5];
void block_ra_note(int core, unsigned int frames, unsigned int t0_tick)
{
	extern unsigned int gpt4_get_current_tick(void);
	unsigned int us = (gpt4_get_current_tick() - t0_tick) / 13u;
	if (core < 0 || core > 4)
		return;
	g_block_ra_n[core]++;
	g_block_ra_frames[core] += frames;
	g_block_ra_us[core] += us;
	if (us > g_block_ra_max[core])
		g_block_ra_max[core] = us;
}

/* ---- oem pwmscope: the backlight PWM as it leaves the SoC ----
 * DISP_PWM0 drives the backlight boost through GPIO43, and that pad's input level reads the PWM
 * itself. Sampled in a tight loop with interrupts off for `ms` (at most 200), each rising edge timed
 * on the 13 MHz GPT: the period, so the PWM clock and divider can be checked, and the fraction of
 * samples high, the duty. */
volatile unsigned int g_dbg_scope_min_ns, g_dbg_scope_max_ns;	/* rise to rise, the last scope */
int block_pwm_scope(unsigned int ms, unsigned int *rises, unsigned int *period_ns,
		    unsigned int *high_permille, unsigned int *samples, int irqs_on)
{
	extern unsigned int gpt4_get_current_tick(void);
	volatile unsigned int *din = (volatile unsigned int *)0x10005210u;   /* GPIO 32..63 inputs */
	unsigned int t0, now, span, prev, v, n = 0, hi = 0, nr = 0, first = 0, last = 0;
	unsigned int dmin = ~0u, dmax = 0;
	if (ms < 1u) ms = 1u;
	if (ms > (irqs_on ? 2000u : 200u)) ms = irqs_on ? 2000u : 200u;
	span = ms * 13000u;
	if (!irqs_on)
		enter_critical_section();
	prev = (*din >> 11) & 1u;
	t0 = gpt4_get_current_tick();
	while ((now = gpt4_get_current_tick()) - t0 < span) {
		v = (*din >> 11) & 1u;
		n++;
		hi += v;
		if (v && !prev) {
			if (!nr) first = now;
			else {
				unsigned int d = now - last;
				if (d < dmin) dmin = d;
				if (d > dmax) dmax = d;
			}
			last = now;
			nr++;
		}
		prev = v;
	}
	if (!irqs_on)
		exit_critical_section();
	g_dbg_scope_min_ns = nr > 1u ? (unsigned int)((unsigned long long)dmin * 1000ull / 13ull) : 0u;
	g_dbg_scope_max_ns = nr > 1u ? (unsigned int)((unsigned long long)dmax * 1000ull / 13ull) : 0u;
	*rises = nr;
	*period_ns = nr > 1u ? (unsigned int)(((unsigned long long)(last - first) * 1000ull) / 13ull / (nr - 1u)) : 0u;
	*high_permille = n ? (unsigned int)((unsigned long long)hi * 1000ull / n) : 0u;
	*samples = n;
	return 0;
}

/* ---- the battery: the MT6358 fuel gauge (block_gauge.c) ---- */

/* ---- the AUXADC, for the diagnostics (millivolts) ---- */
extern int IMM_GetOneChannelValue_Cali(int Channel, int *voltage);
int iio_read_channel_processed(int channel, int *val)
{
	int mv = 0;
	if (IMM_GetOneChannelValue_Cali(channel, &mv) != 0)
		return -1;
	*val = mv;
	return 0;
}

/* ---- the power LED: an Awinic AW2023 at 0x45 on I2C5 ----
 * The same register set as the Konkr Pocket Advance's AW2033, so the same use: the shared LED
 * policy (gba_driver.c led_thread: charging, full, on battery, low; the latency light) calls these
 * three. GCR1 keeps CHGDIS set, so the chip never lights red on its own from the charger pin.
 * I2C5 also carries the rumble motor: every transfer takes the bus lock. */
#include <platform/mt_i2c.h>
#define BLOCK_LED_ADDR	0x45
static mutex_t s_i2c5_mtx;
static volatile int s_i2c5_up;
void block_i2c5_lock(void)
{
	if (!s_i2c5_up) {
		enter_critical_section();
		if (!s_i2c5_up) { mutex_init(&s_i2c5_mtx); s_i2c5_up = 1; }
		exit_critical_section();
	}
	mutex_acquire(&s_i2c5_mtx);
}
void block_i2c5_unlock(void) { mutex_release(&s_i2c5_mtx); }

static int block_led_write(unsigned char reg, unsigned char val)
{
	mt_i2c i2c = {0};
	unsigned char buf[2];
	int r;
	i2c.id = I2C5;
	i2c.addr = BLOCK_LED_ADDR;
	i2c.mode = FS_MODE;
	i2c.speed = 400;
	buf[0] = reg;
	buf[1] = val;
	block_i2c5_lock();
	r = i2c_write(&i2c, buf, 2);
	block_i2c5_unlock();
	return r;
}

volatile int g_dbg_block_led_err;	/* transfers the LED did not take */
void ayaneo_charge_led(int r, int g, int b)
{
	static int inited;
	int err = 0;
	if (!inited) {
		err |= block_led_write(0x00, 0x55);	/* soft reset */
		err |= block_led_write(0x01, 0x03);	/* GCR1: chip on, hardware charge indication off */
		err |= block_led_write(0x31, 0x03);	/* channel 0 (red) current */
		err |= block_led_write(0x32, 0x03);	/* channel 1 (green) */
		err |= block_led_write(0x33, 0x03);	/* channel 2 (blue) */
		inited = !err;
	}
	err |= block_led_write(0x34, (unsigned char)r);	/* PWM levels */
	err |= block_led_write(0x35, (unsigned char)g);
	err |= block_led_write(0x36, (unsigned char)b);
	err |= block_led_write(0x30, (unsigned char)((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0)));
	if (err) g_dbg_block_led_err++;
}

/* the channel enable bits only, the levels as set before: the latency light's quick switch */
void ayaneo_led_enable(int mask)
{
	if (block_led_write(0x30, (unsigned char)(mask & 7))) g_dbg_block_led_err++;
}

/* power off: dark, the chip in standby with the charge indication still off */
void ayaneo_charge_led_standby(void)
{
	ayaneo_charge_led(0, 0, 0);
	block_led_write(0x01, 0x02);	/* CHIPEN 0, CHGDIS 1 */
}

/* the backlight strobe (BFI): ddp_pwm.c with platform/common/pico/pico_pwm_strobe.inc */

/* the KPA's SD and display rails are on an MT6360 sub PMIC; the Block has none */
int mt6360_ldo_config_interface(unsigned char addr, unsigned char data, unsigned char mask,
				unsigned char shift)
{
	(void)addr; (void)data; (void)mask; (void)shift;
	return -1;
}

/* oem stacktest (menu_fastboot.c): a thread on the SNES slot of the emulator stack window, as
 * snes_sd_session makes it (or, on_window 0, a thread of the LK heap), clears the SNES heap byte
 * by byte as the snes9x core's heap_init does. The time it took in ms, negative if not done
 * within 5 s. */
#include "../../emu/gba/emu_stack.h"
static volatile int s_stacktest_done;
static int stacktest_fn(void *arg)
{
	volatile unsigned char *p = (volatile unsigned char *)0x50800000u;
	unsigned int i;

	(void)arg;
	for (i = 0; i < 0x2800000u; i++)
		p[i] = 0;
	s_stacktest_done = 1;
	return 0;
}

int block_stacktest(int on_window)
{
	thread_t *t;
	int ms;

	s_stacktest_done = 0;
	if (on_window)
		t = emu_thread_create("stacktest", stacktest_fn, NULL, DEFAULT_PRIORITY, EMU_STACK_SNES, 262144);
	else
		t = thread_create("stacktest", stacktest_fn, NULL, DEFAULT_PRIORITY, 262144);
	if (!t)
		return -1;
	thread_resume(t);
	for (ms = 0; ms < 5000 && !s_stacktest_done; ms += 50)
		thread_sleep(50);
	return s_stacktest_done ? ms : -ms;
}

/* oem hz (menu_fastboot.c): the panel's real refresh in micro Hz, 0 without frame starts. RDMA0's
 * frame start flag (INT_STATUS bit 1, which nothing else clears) is timed at every edge for 5 s
 * at the top priority on the 13 MHz clock, a refresh whose edge was missed counted from the gap,
 * and the period fitted by least squares over all of them. */
static unsigned int s_hz_t[400];
static unsigned short s_hz_k[400];
unsigned int block_measure_hz(unsigned int *edges, unsigned int *refreshes)
{
	extern unsigned int ayaneo_dsi_refresh_milli(void);
	volatile unsigned int *st = (volatile unsigned int *)(0x1400D000u + 0x004u);
	unsigned int t0, now, n = 0, k = 0, est, i, model = ayaneo_dsi_refresh_milli();
	double sx = 0, sy = 0, sxx = 0, sxy = 0, slope;

	est = (unsigned int)(13000000000ull / (model ? model : 60000u));
	thread_set_priority(HIGHEST_PRIORITY);		/* the emulator must not delay the timestamps */
	*st = ~2u;
	t0 = gpt4_get_current_tick();
	while (((now = gpt4_get_current_tick()) - t0) < 65000000u && n < 400u) {
		if (*st & 2u) {
			*st = ~2u;
			if (n)
				k += (now - s_hz_t[n - 1u] + est / 2u) / est;
			s_hz_t[n] = now;
			s_hz_k[n] = (unsigned short)k;
			n++;
		}
	}
	thread_set_priority(DEFAULT_PRIORITY);
	*edges = n;
	*refreshes = n ? s_hz_k[n - 1u] + 1u : 0u;
	if (n < 3u)
		return 0;
	for (i = 0; i < n; i++) {
		double x = s_hz_k[i], y = (double)(s_hz_t[i] - s_hz_t[0]);
		sx += x; sy += y; sxx += x * x; sxy += x * y;
	}
	slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);	/* ticks a refresh */
	return (unsigned int)(13000000.0 * 1000000.0 / slope);
}

/* oem cpupll:REG,MHZ (menu_fastboot.c, bring-up): the clock of the core the LK runs on, measured
 * before and after writing MHZ (at most 1100, below every rail's floor) to the APMIXED PLL whose
 * CON1 is at REG (0x20c or 0x21c), to tell which PLL clocks it; the old word is put back. */
void block_cpu_pll_probe(unsigned int reg, unsigned int mhz, unsigned int *before, unsigned int *during)
{
	volatile unsigned int *con1 = (volatile unsigned int *)(0x1000C000u + reg);
	unsigned int old = *con1, posdiv = (old >> 24) & 7u, pcw;

	*before = measure_cpu_mhz();
	if ((reg != 0x20cu && reg != 0x21cu) || mhz < 500u || mhz > 1100u) {
		*during = 0;
		return;
	}
	pcw = (unsigned int)((((unsigned long long)mhz << 14) << posdiv) / 26u);
	*con1 = (old & ~0x803FFFFFu) | (pcw & 0x3FFFFFu);
	*con1 = (old & ~0x803FFFFFu) | (pcw & 0x3FFFFFu) | 0x80000000u;
	udelay(30);
	*during = measure_cpu_mhz();
	*con1 = old & ~0x80000000u;
	*con1 = old | 0x80000000u;
	udelay(30);
}
