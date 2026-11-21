/*
 * GammaOS Pico joypad for a board with no analog sticks or triggers (platform/common/pico): sticks
 * centred, triggers released, no dpad emulation, and L2 / R2 as plain buttons that toggle rewind
 * and fast forward. Included as text by the board's joypad file, which defines its trigger pins
 * KPA_GPIO_L2 and KPA_GPIO_R2 (active low) above it.
 */
void ayaneo_joypad_power(void)
{
}

int ayaneo_joypad_stick(int ch)
{
	(void)ch;
	return -1;	/* "no reading", the same value the pairmini path uses for an ADC error */
}

int ayaneo_joypad_trigger(int lr)
{
	(void)lr;
	return -1;
}

void ayaneo_joypad_calibrate(int force)
{
	(void)force;
}

void ayaneo_joypad_poll(void)
{
}

unsigned int ayaneo_joypad_dpad(void)
{
	return 0;
}

extern int mt_get_gpio_in(unsigned int pin);
extern int mt_set_gpio_mode(unsigned int pin, unsigned int mode);
extern int mt_set_gpio_dir(unsigned int pin, unsigned int dir);
extern int mt_set_gpio_smt(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_enable(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_select(unsigned int pin, unsigned int select);
extern unsigned int gpt4_get_current_tick(void);
#define KPA_GP(n)      ((n) | 0x80000000u)
#define KPA_PRESSED(g) (mt_get_gpio_in(KPA_GP(g)) == 0)

/* A press has to read pressed for this long, without a break, before it counts. */
#define KPA_TOGGLE_HOLD_TICKS (13000u * 25u)    /* 25 ms */
/* and a second press within this long of the last flip is ignored (contact bounce) */
#define KPA_TOGGLE_GUARD_TICKS (13000u * 150u)  /* 150 ms */

/* stable: the level that last held long enough; cand/cand_t0: the level being timed now; hot: the
 * hotkey that presses it (emu/gba/pico_input.h: PH_FF, PH_RW), R2 and L2 unless the player moved it */
struct kpa_toggle { int on, armed, stable, cand; unsigned cand_t0, t_flip; unsigned gpio; int hot; };
static struct kpa_toggle s_ff_tog = { .gpio = KPA_GPIO_R2, .hot = 0 }, s_rw_tog = { .gpio = KPA_GPIO_L2, .hot = 2 };
/* the Controls page's map (pico_input.c), where it is built in: the hotkey's buttons all held */
extern int pico_input_hot_down(int hk) __attribute__((weak));
#define KPA_HOT_FF_HOLD 1
#define KPA_HOT_RW_HOLD 3
static int kpa_toggle_down(const struct kpa_toggle *t)
{
	return pico_input_hot_down ? pico_input_hot_down(t->hot) : KPA_PRESSED(t->gpio);
}
volatile unsigned int g_dbg_ff_flips, g_dbg_rw_flips;   /* oem ff: R2 and L2 toggle flips seen */

/* R2 and L2 are never set up by anything else in the LK, and left as they come out of reset they
 * float: noise read as presses and flipped fast forward or rewind on and off by itself (fast
 * forward that would not stop, rewind starting on its own). They get what every other button
 * gets: GPIO mode, input, Schmitt trigger, pull-up. */
static void kpa_toggle_pin(unsigned gpio)
{
	mt_set_gpio_mode(KPA_GP(gpio), 0);
	mt_set_gpio_dir(KPA_GP(gpio), 0);
	mt_set_gpio_smt(KPA_GP(gpio), 1);
	mt_set_gpio_pull_enable(KPA_GP(gpio), 1);
	mt_set_gpio_pull_select(KPA_GP(gpio), 1);
}

/* Flip when a press has held for KPA_TOGGLE_HOLD_TICKS: a glitch shorter than that, or a gap in a
 * press, only restarts the timing. Several reads per frame see the same press, so a press flips the
 * toggle exactly once; another press within KPA_TOGGLE_GUARD_TICKS of the flip is ignored. */
static int kpa_toggle_poll(struct kpa_toggle *t)
{
	int now;
	unsigned tick = gpt4_get_current_tick();
	if (!t->armed) {
		kpa_toggle_pin(t->gpio);
		t->armed = 1;
		t->stable = t->cand = kpa_toggle_down(t);   /* held at start: not a press */
		t->cand_t0 = tick;
		return t->on;
	}
	now = kpa_toggle_down(t);
	if (now != t->cand) {
		t->cand = now;
		t->cand_t0 = tick;
	} else if (now != t->stable && (unsigned)(tick - t->cand_t0) >= KPA_TOGGLE_HOLD_TICKS) {
		t->stable = now;
		if (now && (unsigned)(tick - t->t_flip) >= KPA_TOGGLE_GUARD_TICKS) {
			t->on = !t->on;
			t->t_flip = tick;
			if (t == &s_ff_tog) g_dbg_ff_flips++;
			else g_dbg_rw_flips++;
		}
	}
	return t->on;
}

/* oem ff:1|0 - set the fast forward toggle as an R2 press would (testing without the trigger) */
void ayaneo_joypad_ff_force(int on)
{
	s_ff_tog.on = on ? 1 : 0;
}

/* oem ff:r1|r0 - the same for the rewind toggle (L2) */
void ayaneo_joypad_rw_force(int on)
{
	s_rw_tog.on = on ? 1 : 0;
}

int ayaneo_joypad_ff_level(void)
{
	int on = kpa_toggle_poll(&s_ff_tog);   /* R2 toggles fast forward */
	if (pico_input_hot_down && pico_input_hot_down(KPA_HOT_FF_HOLD)) on = 1;   /* or a button held for it */
	return on ? 255 : 0;
}

int ayaneo_joypad_rewind_level(void)
{
	extern int pico_ra_hardcore(void) __attribute__((weak));
	int on = kpa_toggle_poll(&s_rw_tog);   /* L2 toggles rewind */
	if (pico_input_hot_down && pico_input_hot_down(KPA_HOT_RW_HOLD)) on = 1;   /* or a button held for it */
	if (on && pico_ra_hardcore && pico_ra_hardcore()) {	/* RetroAchievements' hardcore: no rewind */
		s_rw_tog.on = 0;
		return 0;
	}
	return on ? 255 : 0;
}

/* a game start or exit clears both toggles so nothing stays latched across sessions */
void ayaneo_joypad_kpa_reset_toggles(void)
{
	s_ff_tog.on = 0; s_rw_tog.on = 0;
}
