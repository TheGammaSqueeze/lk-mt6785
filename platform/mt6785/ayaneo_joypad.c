/*
 * AYANEO pairmini analog joypad access from LK.
 *
 * Reverse-engineered from the stock kernel mtk-gamepad / sgm58031 driver
 * (see memory analog-input-lk). Hardware:
 *   - Sticks lx/ly/rx/ry  = SoC AUXADC channels 1/2/3/4 (12-bit).
 *   - Triggers LT/RT      = SGM58031 (16-bit ADS1115-compatible ADC) on I2C6 @ 0x48,
 *                           single-ended AIN0 (LT) / AIN1 (RT).
 * Both are powered by the mt6360 sub-PMIC LDO1 ("VFP", 3.3V) + an active-high
 * enable on GPIO15, which are OFF in the bootloader - so nothing reads until
 * ayaneo_joypad_power() runs.
 */
#include <platform/mt_typedefs.h>
#include <platform/mt_i2c.h>
#include <platform/mt_gpio.h>
#include <string.h>

extern int  mt6360_ldo_config_interface(u8 addr, u8 data, u8 mask, u8 shift);
extern int  iio_read_channel_processed(int channel, int *val);   /* mtk_auxadc.c */
extern void udelay(unsigned long usec);

#define JOY_AMUX_EN_GPIO   15u          /* gamepad@48 DT enable-gpios, active high */
#define SGM58031_I2C_ADDR  0x48         /* 7-bit */
#define SGM_REG_CONV       0x00         /* conversion result (16-bit BE) */
#define SGM_REG_CONFIG     0x01         /* config (16-bit BE) */
/* Config words the stock driver writes (OS=1 single-shot, PGA +/-4.096V, 860SPS): */
#define SGM_CFG_LT         0xc3e2       /* MUX=100 single-ended AIN0 = left trigger */
#define SGM_CFG_RT         0xd3e2       /* MUX=101 single-ended AIN1 = right trigger */

static int s_powered;
static int s_sgm_inited;

/* Fill a fresh mt_i2c client for the SGM58031 on I2C6. */
static void sgm_i2c_setup(mt_i2c *i2c)
{
	memset(i2c, 0, sizeof(*i2c));
	i2c->id = I2C6;
	i2c->addr = SGM58031_I2C_ADDR;
	i2c->mode = FS_MODE;             /* 400 kHz fast mode */
	i2c->speed = 400;
	i2c->pushpull = true;
}

/* Write a 16-bit big-endian value to an SGM58031 register. */
static int sgm_write_reg(mt_i2c *i2c, unsigned char reg, unsigned int val)
{
	unsigned char buf[3];
	buf[0] = reg;
	buf[1] = (unsigned char)(val >> 8);
	buf[2] = (unsigned char)(val & 0xff);
	return i2c_write(i2c, buf, 3);
}

/* One-time SGM58031 configuration, mirroring the stock sgm58031_init: threshold regs plus the
 * SGM58031-specific Config1 register 0x04 = 0x0080 - WITHOUT which conversions never complete
 * (OS stays 0, result 0). Must run after the rail is up. */
static void sgm58031_init(void)
{
	mt_i2c i2c;
	if (s_sgm_inited)
		return;
	sgm_i2c_setup(&i2c);
	sgm_write_reg(&i2c, 0x02, 0x0000);   /* Lo_thresh */
	udelay(10000);
	sgm_write_reg(&i2c, 0x03, 0xffff);   /* Hi_thresh */
	udelay(10000);
	sgm_write_reg(&i2c, 0x04, 0x0080);   /* Config1 (SGM58031-specific) */
	udelay(50000);
	s_sgm_inited = 1;
}

/* Bring up the analog rail once: mt6360 LDO1 (VFP) = 3.30V + enable, then drive the
 * MUX/enable GPIO15 high. Same sequence the stock kernel driver uses. Idempotent. */
void ayaneo_joypad_power(void)
{
	if (s_powered)
		return;
	mt6360_ldo_config_interface(0x1b, 0xd0, 0xff, 0);   /* LDO1 VOUT reg 0x1b = 0xd0 = 3.30V */
	mt6360_ldo_config_interface(0x17, 0x40, 0x40, 0);   /* LDO1 EN reg 0x17 bit 0x40 */
	mt_set_gpio_mode(JOY_AMUX_EN_GPIO, GPIO_MODE_00);
	mt_set_gpio_dir(JOY_AMUX_EN_GPIO, GPIO_DIR_OUT);
	mt_set_gpio_out(JOY_AMUX_EN_GPIO, GPIO_OUT_ONE);
	udelay(30000);                                      /* let the 3.3V rail + sensors settle */
	s_powered = 1;
}

/* Read a stick axis: AUXADC channel 1..4 (lx,ly,rx,ry). Returns raw 12-bit, or -1. */
int ayaneo_joypad_stick(int ch)
{
	int v = -1;
	if (!s_powered)
		ayaneo_joypad_power();
	if (iio_read_channel_processed(ch, &v) != 0)
		return -1;
	return v;
}

/* One SGM58031 single-shot read on the given config word. Returns the signed 16-bit
 * conversion (unsigned range here, ~13500..16500 for a trigger), or negative on error. */
static int sgm58031_read(unsigned int config)
{
	mt_i2c i2c;
	unsigned char buf[4];
	int ret;

	sgm_i2c_setup(&i2c);

	/* start a single-shot conversion on the selected MUX channel */
	buf[0] = SGM_REG_CONFIG;
	buf[1] = (unsigned char)(config >> 8);
	buf[2] = (unsigned char)(config & 0xff);
	ret = i2c_write(&i2c, buf, 3);
	if (ret != I2C_OK)
		return -1;

	/* poll the config OS bit (bit15) until the conversion completes (up to ~50 ms) */
	{
		int i;
		for (i = 0; i < 100; i++) {
			udelay(500);
			buf[0] = SGM_REG_CONFIG;
			if (i2c_write_read(&i2c, buf, 1, 2) != I2C_OK)
				return -2;
			if (buf[0] & 0x80)          /* OS=1 -> conversion done */
				break;
		}
	}

	/* point at the conversion register and read the 16-bit BE result */
	buf[0] = SGM_REG_CONV;
	ret = i2c_write_read(&i2c, buf, 1, 2);
	if (ret != I2C_OK)
		return -3;
	return (int)(((unsigned)buf[0] << 8) | buf[1]);
}

/* Read a trigger: lr=0 left (AIN0), lr=1 right (AIN1). Raw 16-bit, or negative on error. */
int ayaneo_joypad_trigger(int lr)
{
	if (!s_powered)
		ayaneo_joypad_power();
	sgm58031_init();
	return sgm58031_read(lr ? SGM_CFG_RT : SGM_CFG_LT);
}
