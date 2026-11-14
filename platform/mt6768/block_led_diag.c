/*
 * block_led_diag.c - bring-up checkpoints on the Konkr Block's RGB LED, for a boot that stops
 * before the panel, the log or USB can tell anything (BLOCK_LED_DIAG builds).
 *
 * The LED is an Awinic AW2023 at 0x45 on I2C5 (pads GPIO48/GPIO49). It is driven by hand on the
 * two pins with a busy-loop delay, so it works from the LK's first C code: no timer, no I2C
 * driver, MMU on or off. Which pad is the clock is found by asking the chip on both orders.
 * The pads are switched to GPIO for each transfer and given their function back after it: the
 * board's GPIO defaults put them in I2C mode, and the LK's own I2C5 users need them so.
 * Each checkpoint shows a solid colour and holds it a moment, so the sequence can be followed;
 * the colour the LED keeps is the last checkpoint reached.
 */
#include <platform/mt_gpio.h>

extern void mdelay(unsigned long msec);

#define LED_ADDR     0x45
#define PAD_A        (48u | 0x80000000u)
#define PAD_B        (49u | 0x80000000u)

static unsigned int s_scl, s_sda;
static int s_state;              /* 0 not tried, 1 working, -1 no chip answered */

static void wait_half(void)
{
	volatile unsigned int i;

	for (i = 0; i < 3000; i++)
		;
}

static void line_low(unsigned int pin)
{
	mt_set_gpio_out(pin, 0);
	mt_set_gpio_dir(pin, GPIO_DIR_OUT);
}

static void line_release(unsigned int pin)
{
	mt_set_gpio_dir(pin, GPIO_DIR_IN);   /* the bus pull-up raises it */
}

static void bus_start(void)
{
	line_release(s_sda);
	line_release(s_scl);
	wait_half();
	line_low(s_sda);
	wait_half();
	line_low(s_scl);
	wait_half();
}

static void bus_stop(void)
{
	line_low(s_sda);
	wait_half();
	line_release(s_scl);
	wait_half();
	line_release(s_sda);
	wait_half();
}

static int bus_byte(unsigned int v)   /* 1 when acknowledged */
{
	int i, ack;

	for (i = 7; i >= 0; i--) {
		if ((v >> i) & 1)
			line_release(s_sda);
		else
			line_low(s_sda);
		wait_half();
		line_release(s_scl);
		wait_half();
		line_low(s_scl);
	}
	line_release(s_sda);
	wait_half();
	line_release(s_scl);
	wait_half();
	ack = mt_get_gpio_in(s_sda) == 0;
	line_low(s_scl);
	wait_half();
	return ack;
}

static unsigned int s_mode_scl, s_mode_sda;

static void pads_take(void)
{
	s_mode_scl = (unsigned int)mt_get_gpio_mode(s_scl);
	s_mode_sda = (unsigned int)mt_get_gpio_mode(s_sda);
	mt_set_gpio_mode(s_scl, 0);
	mt_set_gpio_mode(s_sda, 0);
	line_release(s_scl);
	line_release(s_sda);
}

static void pads_give(void)
{
	mt_set_gpio_mode(s_scl, s_mode_scl);
	mt_set_gpio_mode(s_sda, s_mode_sda);
}

static int led_write(unsigned int reg, unsigned int val)
{
	int ok;

	pads_take();
	bus_start();
	ok = bus_byte(LED_ADDR << 1) && bus_byte(reg) && bus_byte(val);
	bus_stop();
	pads_give();
	return ok;
}

static int try_pins(unsigned int scl, unsigned int sda)
{
	int ok;

	s_scl = scl;
	s_sda = sda;
	pads_take();
	wait_half();
	bus_start();
	ok = bus_byte(LED_ADDR << 1);
	bus_stop();
	pads_give();
	return ok;
}

/* Checkpoint `stage` (1 to 8): the LED blinks `stage` times, then stays on in the stage's colour
 * (bit 0 red, bit 1 green, bit 2 blue of the stage number; stage 8 white). The count is what to
 * read: it does not depend on how the colour channels are wired. The watchdog is kicked while
 * blinking so the pauses never count as a hang. */
void block_led_stage(unsigned int stage)
{
	extern void mtk_wdt_restart(void);
	unsigned int colour = (stage & 7) ? (stage & 7) : 7, i;

	if (s_state == 0) {
		if (try_pins(PAD_A, PAD_B) || try_pins(PAD_B, PAD_A)) {
			s_state = 1;
			led_write(0x00, 0x55);       /* soft reset */
			wait_half();
			led_write(0x01, 0x03);       /* chip on, hardware charge indication off */
			led_write(0x31, 0x03);       /* channel currents */
			led_write(0x32, 0x03);
			led_write(0x33, 0x03);
			led_write(0x34, 0xff);       /* full PWM on every channel */
			led_write(0x35, 0xff);
			led_write(0x36, 0xff);
		} else {
			s_state = -1;
		}
	}
	if (s_state != 1)
		return;
	led_write(0x30, 0);
	mdelay(600);                          /* a gap before the count starts */
	for (i = 0; i < stage; i++) {
		mtk_wdt_restart();
		led_write(0x30, colour);
		mdelay(150);
		led_write(0x30, 0);
		mdelay(150);
	}
	mtk_wdt_restart();
	led_write(0x30, colour);              /* stays on until the next checkpoint */
}

/* A fine grained checkpoint inside a step: a solid colour at once (bit 0 red, bit 1 green, bit 2
 * blue), no gap and no blinks, so it costs no time; the colour left on names the last one. */
void block_led_mark(unsigned int colour)
{
	if (s_state != 1)
		return;
	led_write(0x30, colour & 7);
}
