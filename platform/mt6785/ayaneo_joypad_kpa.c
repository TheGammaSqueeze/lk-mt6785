/*
 * Konkr Pocket Advance: no analog sticks, no analog triggers.
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

int ayaneo_joypad_ff_level(void)
{
	return 0;
}

int ayaneo_joypad_rewind_level(void)
{
	return 0;
}
