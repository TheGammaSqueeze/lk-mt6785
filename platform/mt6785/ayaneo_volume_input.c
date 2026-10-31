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
static unsigned s_last_stable;	/* last state seen with bit0 clear (a detent) */
static int s_dir;		/* direction decided at the half step */

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
	s_last_stable = enc_state() & ~1u;
	s_dir = 0;
	s_inited = 1;
}

/* The kernel's half-period handler, run by polling instead of edge interrupts. It is
 * correct as long as the poll is faster than the wheel moves through one half step; at
 * frame rate (8 to 17 ms) that holds for hand turned 24 detent wheels, and a step that is
 * fast enough to skip a half state is dropped rather than misread. */
int ayaneo_volume_dir(void)
{
	unsigned state;
	int dir = 0;

	if (!s_inited)
		ayaneo_volume_input_init();

	state = enc_state();
	if (state & 1) {
		s_dir = (int)((s_last_stable - state + 1) % 4) - 1;
	} else if (state != s_last_stable) {
		dir = s_dir;
		s_last_stable = state;
	}
	return dir;
}

#else	/* AYANEO Pocket Air Mini: MTK keypad volume rocker */

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

#endif
