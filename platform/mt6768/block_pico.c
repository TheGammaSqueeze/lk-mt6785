#include <arch/arm/mmu.h>
/*
 * GammaOS Pico on the Konkr Pocket Block: the board's own small pieces the shared Pico code calls.
 *
 * Here now: the L2 / R2 toggles (rewind, fast forward), the boot hooks around the emulator, the
 * CPU clock as CPU 0 really runs it, the battery level from its voltage, and the AUXADC read the
 * diagnostics use. Not yet driven, each reported as absent so the cores and menus leave it out:
 * the STK8321 accelerometer (i2c1 0x0F), the VMD5xx rumble motor (i2c5 0x58, GPIO158), the AW2023
 * LED (i2c5 0x45), the backlight strobe (BFI) and the MT6358 fuel gauge. The Block has no MT6360.
 */
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
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
	/* A 4, B 5, X 3, Y 6, L1 14, R1 10, START 7, SELECT 24, d-pad 25 27 28 26, MODE 17 */
	static const unsigned char pins[] = { 4, 5, 3, 6, 14, 10, 7, 24, 25, 27, 28, 26, 17 };
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
	ayaneo_show_deferred_logo();
	ayaneo_gbc_audio_shutdown();
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
 * of bit 31. Voltages are never changed: a clock asked for is held to the highest step of the
 * cluster's table (the Block's device tree, cluster0_opp) that the A55 rail already supplies,
 * read live from the MT6358 (VPROC11 and VPROC12 DBG0, the lower of the two: 0.5 V + 6.25 mV a
 * code). At the 0.8 V the preloader leaves that is 1175 MHz, about where the core boots, so the
 * cores' faster tiers run there and their slower ones save power, as on the Konkr. */
#define BLOCK_CPU_PLL_CON1	0x1000C21Cu
#define BLOCK_CPU_MIN_MHZ	500u
static const struct { unsigned short mhz; unsigned int uv; } s_cpu_opp[] = {
	{ 500, 650000 }, { 774, 675000 }, { 850, 700000 }, { 900, 718750 }, { 950, 731250 },
	{ 999, 743750 }, { 1050, 762500 }, { 1100, 775000 }, { 1175, 800000 }, { 1275, 831250 },
	{ 1325, 843750 }, { 1375, 856250 }, { 1450, 881250 }, { 1500, 893750 }, { 1625, 931250 },
	{ 1800, 1006250 },
};

/* the A55 rail's live voltage in uV (0 when it cannot be read) */
static unsigned int cpu_rail_uv(void)
{
	extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
	unsigned int a = 0, b = 0;
	if (pwrap_read(0x139e, &a) || pwrap_read(0x141e, &b))
		return 0;
	a &= 0x7fu;
	b &= 0x7fu;
	return 500000u + 6250u * (a < b ? a : b);
}

/* the fastest clock of the table the rail supplies now */
volatile unsigned int g_dbg_cpu_cap_mhz, g_dbg_cpu_rail_uv;
static unsigned int cpu_cap_mhz(void)
{
	unsigned int uv = cpu_rail_uv(), cap = BLOCK_CPU_MIN_MHZ, i;
	for (i = 0; i < sizeof(s_cpu_opp) / sizeof(s_cpu_opp[0]); i++)
		if (s_cpu_opp[i].uv <= uv && s_cpu_opp[i].mhz > cap)
			cap = s_cpu_opp[i].mhz;
	g_dbg_cpu_rail_uv = uv;
	g_dbg_cpu_cap_mhz = cap;
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

volatile unsigned int g_dbg_cpu_mhz_asked;	/* the last clock a core asked for */
void ayaneo_set_cpu_mhz(unsigned int mhz)
{
	volatile unsigned int *con1 = (volatile unsigned int *)BLOCK_CPU_PLL_CON1;
	unsigned int c = *con1, posdiv = (c >> 24) & 7u, cap = cpu_cap_mhz();
	unsigned long long pcw;

	g_dbg_cpu_mhz_asked = mhz;
	if (mhz > cap) mhz = cap;
	if (mhz < BLOCK_CPU_MIN_MHZ) mhz = BLOCK_CPU_MIN_MHZ;
	pcw = (((unsigned long long)mhz * 1000000ull) << 14) << posdiv;
	pcw /= 26000000ull;
	if (pcw > 0x3FFFFFu) pcw = 0x3FFFFFu;
	c = (c & ~0x803FFFFFu) | (unsigned int)pcw;
	*con1 = c;
	*con1 = c | 0x80000000u;
	udelay(20);
}

/* ---- the battery: its level from its voltage until the MT6358 gauge is read ---- */
extern int get_bat_volt(int times);
int ayaneo_gauge_pct(void)
{
	/* a resting Li-ion cell from 3.45 V (empty, as the stock shutdown level) to 4.35 V (the 4.4 V
	 * charge voltage, at rest); a rough guide only, the gauge replaces it */
	int mv = get_bat_volt(1), pct;
	if (mv <= 0)
		return 50;
	pct = (mv - 3450) * 100 / (4350 - 3450);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	return pct;
}

/* no time estimate without the gauge */
int ayaneo_gauge_time_str(char *out, int n)
{
	if (out && n > 0)
		out[0] = 0;
	return 0;
}

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

/* ---- not driven yet: reported absent ---- */
volatile unsigned int g_accel_flick = 900;
volatile unsigned int g_accel_smooth = 64;
int ayaneo_accel_start(void) { return -1; }
void ayaneo_accel_stop(void) { }
int ayaneo_accel_read(int v[3]) { v[0] = v[1] = v[2] = 0; return -1; }
void ayaneo_accel_filter(int v[3]) { (void)v; }
int ayaneo_accel_reg(unsigned reg, int val) { (void)reg; (void)val; return -1; }
void ayaneo_accel_probe_report(void (*line)(const char *))
{
	if (line) line("accel: the STK8321 (i2c1 0x0F) is not driven yet");
}

void ayaneo_rumble_set(int on) { (void)on; }
void ayaneo_rumble_stop(void) { ayaneo_rumble_set(0); }
void ayaneo_rumble_pulse(unsigned ms) { (void)ms; }
void ayaneo_rumble_report(void (*line)(const char *))
{
	if (line) line("rumble: the VMD5xx (i2c5 0x58, GPIO158) is not driven yet");
}

void ayaneo_led_enable(int mask) { (void)mask; }
void ayaneo_charge_led(int r, int g, int b) { (void)r; (void)g; (void)b; }
void ayaneo_charge_led_standby(void) { }

/* the backlight strobe (BFI): off */
void disp_pwm_strobe_mode(int on) { (void)on; }
void disp_pwm_strobe(unsigned int div, unsigned int duty) { (void)div; (void)duty; }
void disp_pwm_strobe_locked(unsigned int div, unsigned int duty) { (void)div; (void)duty; }
void disp_pwm_strobe_tick(void) { }

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
