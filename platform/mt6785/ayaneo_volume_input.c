/*
 * Volume control input, one implementation per board. See the header.
 */
#include <platform/ayaneo_volume_input.h>
#include <platform/mt_gpio.h>
#include <platform/mtk_key.h>

#ifdef KONKR_KPA

/* Rotary volume wheel: dtbo volume-encoder, gpios = <&pio 15 ACTIVE_LOW>, <&pio 6 ACTIVE_LOW>,
 * rotary-encoder,encoding = "gray", steps = 24, half-period, relative-axis. */
#define ENC_GPIO_A	15
#define ENC_GPIO_B	6
#define GP(n)		((n) | 0x80000000u)

static int s_inited;
static unsigned s_prev;		/* last sampled 2 bit state */
static int s_qacc;		/* quarter steps accumulated towards the next detent (2 per detent) */
static int s_lastdir;		/* sign of the last unambiguous transition */
static volatile int s_steps;	/* detents decoded and not yet consumed (+ = kernel positive = volume up) */

static void enc_pin_init(unsigned gpio)
{
	mt_set_gpio_mode(GP(gpio), 0);
	mt_set_gpio_dir(GP(gpio), 0);
	mt_set_gpio_smt(GP(gpio), 1);
	mt_set_gpio_pull_enable(GP(gpio), 1);
	mt_set_gpio_pull_select(GP(gpio), 1);	/* pull-up, the wheel shorts to ground */
}

/* Same arithmetic as the kernel's rotary_encoder_get_state(): walk the two pins in DT order
 * (A then B), honour GPIO_ACTIVE_LOW, undo the gray coding, keep two bits. */
static unsigned enc_state(void)
{
	unsigned ret = 0;
	int i;
	for (i = 0; i < 2; i++) {
		int val = !mt_get_gpio_in(GP(i == 0 ? ENC_GPIO_A : ENC_GPIO_B));	/* active low */
		if (ret & 1)
			val = !val;
		ret = (ret << 1) | (val & 1);
	}
	return ret & 3;
}

void ayaneo_volume_input_init(void)
{
	if (s_inited)
		return;
	enc_pin_init(ENC_GPIO_A);
	enc_pin_init(ENC_GPIO_B);
	s_prev = enc_state();
	s_qacc = 0; s_lastdir = 0; s_steps = 0;
	s_inited = 1;
}

/* Quadrature accumulator. The kernel's half-period handler decides the direction at the
 * intermediate (odd) state and emits the step at the next even state; polled once per frame
 * (16.7 ms) the intermediate state of a brisk detent is often missed and the step then went
 * out with a STALE direction, which is the jitter and the reversals the user saw. Here every
 * observed transition carries its own direction (+1 / -1 quarter step; the kernel's sign:
 * 0 -> 1 is negative), only a jump of two states is ambiguous and takes the last direction,
 * contact bounce cancels in the accumulator, and a detent is two quarter steps. This is
 * cheap (two GPIO reads) and is also called from the frame end wait loop, so the effective
 * sample rate is tens of microseconds for most of every frame. */
void ayaneo_volume_sample(void)
{
	unsigned st, d;
	if (!s_inited)
		ayaneo_volume_input_init();
	st = enc_state();
	d = (st - s_prev) & 3u;
	if (d == 0) return;
	if (d == 1u)      { s_qacc -= 1; s_lastdir = -1; }
	else if (d == 3u) { s_qacc += 1; s_lastdir = 1; }
	else              { s_qacc += 2 * s_lastdir; }   /* skipped a state: same direction as before */
	s_prev = st;
	while (s_qacc >= 2)  { s_qacc -= 2; s_steps++; }
	while (s_qacc <= -2) { s_qacc += 2; s_steps--; }
}

/* One detent per call (the callers step the volume once per frame); further detents drain on
 * the following frames. */
int ayaneo_volume_dir(void)
{
	ayaneo_volume_sample();
	if (s_steps > 0) { s_steps--; return 1; }
	if (s_steps < 0) { s_steps++; return -1; }
	return 0;
}

/* oem volstep: queue detents as if the wheel had turned them (testing without hands on the wheel) */
void ayaneo_volume_inject(int steps)
{
	s_steps += steps;
}

#else	/* AYANEO Pocket Air Mini: MTK keypad volume rocker */

void ayaneo_volume_sample(void) { }

void ayaneo_volume_input_init(void)
{
}

int ayaneo_volume_dir(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);	/* VOL_UP */
	int vd = mtk_detect_key(0x00);	/* VOL_DOWN */
	int dir = 0;

	if (vu && !vu_prev) dir = +1;
	else if (vd && !vd_prev) dir = -1;
	vu_prev = vu;
	vd_prev = vd;
	return dir;
}

void ayaneo_volume_inject(int steps) { (void)steps; }   /* the rocker has no queue to fill */

#endif

/* Give the rest of the system the CPU while a core waits for its next frame. LK schedules
 * strictly by priority, so while the emulator spins at HIGH_PRIORITY the helper threads
 * (speaker amp bring-up, power LED, settings writer, card listing) never run at all; a
 * 1 ms tick (ENABLE_IO_THREAD) makes a short sleep cheap. Sleep in 1 ms slices while more
 * than ~3 ms remain before target (a 13 MHz tick), decoding the volume wheel at each wake,
 * then let the caller spin the fine remainder. */
#include <kernel/thread.h>
extern unsigned gpt4_get_current_tick(void);
volatile unsigned g_dbg_idle_sleep_max_us, g_dbg_idle_sleeps;   /* oem led: the longest 1 ms sleep seen */
void ayaneo_idle_until(unsigned target_tick)
{
	/* the tick is 1 ms on the Konkr (kernel/timer.c), so a 1 ms sleep returns within ~2 ms;
	 * stop sleeping 4 ms before the deadline and let the caller spin the rest */
	extern int critical_section_count;
	if (critical_section_count > 0) return;   /* interrupts off: a sleep would never return, let the caller spin */
	if ((int)(target_tick - gpt4_get_current_tick()) > 2 * 13000000) return;   /* over 2 s away: a stale target, never sleep on it */
	while ((int)(target_tick - gpt4_get_current_tick()) > 4 * 13000) {
		unsigned t0 = gpt4_get_current_tick(), d;
		thread_sleep(1);
		d = (gpt4_get_current_tick() - t0) / 13u;
		if (d > g_dbg_idle_sleep_max_us) g_dbg_idle_sleep_max_us = d;
		g_dbg_idle_sleeps++;
		ayaneo_volume_sample();
	}
}

/* Power key: hold for 1.2 s to power off, with a progress bar on the overlay layer (skipped while
 * a core's own menu owns the layer). Called once per frame by every core and the carousel;
 * returns 1 when the hold completed, the caller then saves and powers off. A short press does
 * nothing (it used to power off at once). */
extern int pmic_detect_powerkey(void);
extern void ayaneo_power_bar(int pct);
extern void ayaneo_menu_overlay(void (*paint)(unsigned int *, unsigned int, unsigned int, unsigned int), int open);
int ayaneo_power_hold(int menu_open, int use_overlay)
{
	static unsigned t0; static int shown;
	unsigned now = gpt4_get_current_tick();
	int pk = pmic_detect_powerkey();
	if (!pk) {
		if (shown) { if (!menu_open && use_overlay) ayaneo_menu_overlay(0, 0); shown = 0; }
		ayaneo_power_bar(-1);
		t0 = 0;
		return 0;
	}
	if (!t0) { t0 = now ? now : 1u; return 0; }
	if (now - t0 >= 15600000u) { ayaneo_power_bar(-1); return 1; }
	ayaneo_power_bar((int)((now - t0) / 156000u));
	if (!menu_open && use_overlay) {
		ayaneo_menu_overlay(0, 1);   /* no painter of our own: the bar borrows the layer */
		shown = 1;
	}
	return 0;
}
