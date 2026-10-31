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
extern unsigned int gpt4_get_current_tick(void);
#define KPA_GP(n)      ((n) | 0x80000000u)
#define KPA_GPIO_L2    94u
#define KPA_GPIO_R2    93u
#define KPA_PRESSED(g) (mt_get_gpio_in(KPA_GP(g)) == 0)

struct kpa_toggle { int on, last, armed; unsigned t_last; };
static struct kpa_toggle s_ff_tog, s_rw_tog;

/* Flip on the press edge; a 150 ms guard swallows contact bounce. Several reads per frame see
 * the same pin state, so a press flips the toggle exactly once. */
static int kpa_toggle_poll(struct kpa_toggle *t, unsigned gpio)
{
	int now = KPA_PRESSED(gpio);
	unsigned tick = gpt4_get_current_tick();
	if (!t->armed) { t->armed = 1; t->last = now; }
	if (now && !t->last && (unsigned)(tick - t->t_last) > 13000u * 150u) {
		t->on = !t->on;
		t->t_last = tick;
	}
	t->last = now;
	return t->on;
}

int ayaneo_joypad_ff_level(void)
{
	return kpa_toggle_poll(&s_ff_tog, KPA_GPIO_R2) ? 255 : 0;   /* R2 toggles fast forward */
}

int ayaneo_joypad_rewind_level(void)
{
	return kpa_toggle_poll(&s_rw_tog, KPA_GPIO_L2) ? 255 : 0;   /* L2 toggles rewind */
}

/* a game start or exit clears both toggles so nothing stays latched across sessions */
void ayaneo_joypad_kpa_reset_toggles(void)
{
	s_ff_tog.on = 0; s_rw_tog.on = 0;
}
