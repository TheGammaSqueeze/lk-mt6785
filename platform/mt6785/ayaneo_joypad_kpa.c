/*
 * Konkr Pocket Advance: no analog sticks, no analog triggers. L2 and R2 are plain buttons
 * (GPIO94 = BTN_TL2, GPIO93 = BTN_TR2 in the device tree, active low) and work as TOGGLES: one
 * press turns rewind (L2) or fast forward (R2) on at full level, the next press turns it off.
 * The cores keep reading the pairmini's analog "trigger level" (0..255) unchanged.
 *
 * The pairmini implementation (ayaneo_joypad.c) powers an ADC rail, drives GPIO15 as an
 * output and talks to an SGM58031 at i2c6 address 0x48. On the KPA GPIO15 is one phase of
 * the volume encoder and nothing answers at 0x48 (the kernel's gamepad@48 node is a shell),
 * so every entry point here is a neutral no-op: sticks centred, triggers released, dpad
 * emulation off. The callers keep working unchanged.
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
#define KPA_GPIO_L2    94u
#define KPA_GPIO_R2    93u
#define KPA_PRESSED(g) (mt_get_gpio_in(KPA_GP(g)) == 0)

/* A press has to read pressed for this long, without a break, before it counts. */
#define KPA_TOGGLE_HOLD_TICKS (13000u * 25u)    /* 25 ms */
/* and a second press within this long of the last flip is ignored (contact bounce) */
#define KPA_TOGGLE_GUARD_TICKS (13000u * 150u)  /* 150 ms */

/* stable: the level that last held long enough; cand/cand_t0: the level being timed now */
struct kpa_toggle { int on, armed, stable, cand; unsigned cand_t0, t_flip; unsigned gpio; };
static struct kpa_toggle s_ff_tog = { .gpio = KPA_GPIO_R2 }, s_rw_tog = { .gpio = KPA_GPIO_L2 };
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
		t->stable = t->cand = KPA_PRESSED(t->gpio);   /* held at start: not a press */
		t->cand_t0 = tick;
		return t->on;
	}
	now = KPA_PRESSED(t->gpio);
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
	return kpa_toggle_poll(&s_ff_tog) ? 255 : 0;   /* R2 toggles fast forward */
}

int ayaneo_joypad_rewind_level(void)
{
	return kpa_toggle_poll(&s_rw_tog) ? 255 : 0;   /* L2 toggles rewind */
}

/* a game start or exit clears both toggles so nothing stays latched across sessions */
void ayaneo_joypad_kpa_reset_toggles(void)
{
	s_ff_tog.on = 0; s_rw_tog.on = 0;
}
