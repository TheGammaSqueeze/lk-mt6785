/*
 * ayaneo_accel.c - the console's accelerometer: an mCube MC3416 on I2C1 at 0x4c (SCL GPIO61,
 * SDA GPIO62, the base device tree's scl/sda-gpio-id), read in the bootloader for the motion
 * games: tilt cartridges and WarioWare Twisted's gyro.
 *
 * The chip is read by driving the two pins by hand. Writes through the LK's I2C controller
 * reach this chip but its reads come back wrong, while the same transfers made by hand read
 * it cleanly. The chip is the only device on the bus, and the pins go back to their I2C
 * function after every transfer, so Android's driver finds the bus as the preloader left it.
 *
 * Axes come out in the device frame Android uses for this board (the device tree's
 * direction 7): x and y in the screen plane, z out of the screen, 2048 counts to a g. The range
 * is set at every start: the chip powers up at 2 g (16384 counts to a g) and only keeps Android's
 * 16 g setting until it loses power, so the scale must not depend on what booted before.
 */
#include <platform/mt_gpio.h>
#include <string.h>
#include <printf.h>

extern void udelay(unsigned long usec);

#define ACC_ADDR      0x4c
#define ACC_SCL       (61u | 0x80000000u)
#define ACC_SDA       (62u | 0x80000000u)
#define ACC_REG_MODE  0x07     /* 0x41 wake, 0x43 standby */
#define ACC_REG_XOUT  0x0d     /* X, Y, Z: 16 bits each, low byte first */
#define ACC_REG_CHIP  0x18     /* 0xa0 on the MC3416 */
#define ACC_REG_RANGE 0x20     /* bits 6..4 the range: 0x30 is 16 g, 2048 counts to a g */
#define ACC_RANGE_16G 0x30
#define ACC_CHIP_ID   0xa0

/* ---- I2C by hand, open drain: a line is pulled low as an output and let go as an input for the
 * bus pull-ups to raise; a slave may hold the clock low, so a let-go clock is waited for ---- */
static void ln_lo(u32 pin) { mt_set_gpio_out(pin, 0); mt_set_gpio_dir(pin, GPIO_DIR_OUT); }
static void ln_hi(u32 pin) { mt_set_gpio_dir(pin, GPIO_DIR_IN); }
static int ln_in(u32 pin) { return mt_get_gpio_in(pin) ? 1 : 0; }
static void half(void) { udelay(3); }              /* a bit takes two halves: about 150 kHz */
static void scl_hi(void)
{
	int t = 1000;
	ln_hi(ACC_SCL);
	while (!ln_in(ACC_SCL) && --t) udelay(1);
	half();
}
static void i2c_start(void) { ln_hi(ACC_SDA); scl_hi(); ln_lo(ACC_SDA); half(); ln_lo(ACC_SCL); half(); }
static void i2c_stop(void) { ln_lo(ACC_SDA); half(); scl_hi(); ln_hi(ACC_SDA); half(); }
static int i2c_put(unsigned v)                      /* 1 when the slave acknowledged */
{
	int i, ack;
	for (i = 7; i >= 0; i--) {
		if ((v >> i) & 1) ln_hi(ACC_SDA); else ln_lo(ACC_SDA);
		half(); scl_hi(); ln_lo(ACC_SCL); half();
	}
	ln_hi(ACC_SDA); half(); scl_hi();
	ack = !ln_in(ACC_SDA);
	ln_lo(ACC_SCL); half();
	return ack;
}
static unsigned i2c_get(int more)                   /* more: acknowledge, another byte follows */
{
	unsigned v = 0;
	int i;
	ln_hi(ACC_SDA);
	for (i = 0; i < 8; i++) { scl_hi(); v = (v << 1) | (unsigned)ln_in(ACC_SDA); ln_lo(ACC_SCL); half(); }
	if (more) ln_lo(ACC_SDA); else ln_hi(ACC_SDA);
	half(); scl_hi(); ln_lo(ACC_SCL); half(); ln_hi(ACC_SDA);
	return v;
}

/* the pins in GPIO mode for one transfer, then back to whatever they were (their I2C function) */
static u32 s_mode_scl, s_mode_sda;
static void bus_take(void)
{
	s_mode_scl = (u32)mt_get_gpio_mode(ACC_SCL);
	s_mode_sda = (u32)mt_get_gpio_mode(ACC_SDA);
	mt_set_gpio_mode(ACC_SCL, 0);
	mt_set_gpio_mode(ACC_SDA, 0);
	ln_hi(ACC_SCL);
	ln_hi(ACC_SDA);
}
static void bus_give(void)
{
	mt_set_gpio_mode(ACC_SCL, s_mode_scl);
	mt_set_gpio_mode(ACC_SDA, s_mode_sda);
}

static int acc_read(unsigned char reg, unsigned char *buf, int n)
{
	int ok, k;
	bus_take();
	i2c_start();
	ok = i2c_put(ACC_ADDR << 1) && i2c_put(reg);
	if (ok) {
		i2c_start();
		ok = i2c_put((ACC_ADDR << 1) | 1);
		for (k = 0; ok && k < n; k++) buf[k] = (unsigned char)i2c_get(k < n - 1);
	}
	i2c_stop();
	bus_give();
	return ok ? 0 : -1;
}

static int acc_write(unsigned char reg, unsigned char val)
{
	int ok;
	bus_take();
	i2c_start();
	ok = i2c_put(ACC_ADDR << 1) && i2c_put(reg) && i2c_put(val);
	i2c_stop();
	bus_give();
	return ok ? 0 : -1;
}

/* ---- the chip ---- */
static int s_awake;
static long long s_lp[3];   /* ayaneo_accel_filter's average, counts << 8 */
static int s_lp_primed;

/* 0 when the chip answered as an MC3416 and is now measuring */
int ayaneo_accel_start(void)
{
	unsigned char id = 0;
	if (s_awake) return 0;
	if (acc_read(ACC_REG_CHIP, &id, 1) != 0 || id != ACC_CHIP_ID) return -1;
	acc_write(ACC_REG_MODE, 0x43);   /* standby first, as a mode change asks */
	if (acc_write(ACC_REG_RANGE, ACC_RANGE_16G) != 0) return -1;
	if (acc_write(ACC_REG_MODE, 0x41) != 0) return -1;
	s_awake = 1;
	s_lp_primed = 0;                 /* the average starts again from the first reading */
	return 0;
}

/* back to standby: when a motion game ends, before power off and before a reset */
void ayaneo_accel_stop(void)
{
	if (!s_awake) return;
	acc_write(ACC_REG_MODE, 0x43);
	s_awake = 0;
}

/* the console's gravity in the device frame (direction 7: x = -raw y, y = -raw x, z = -raw z);
 * 0 on success */
int ayaneo_accel_read(int v[3])
{
	unsigned char d[6];
	int rx, ry, rz;
	if (!s_awake || acc_read(ACC_REG_XOUT, d, 6) != 0) return -1;
	rx = (short)(d[0] | d[1] << 8);
	ry = (short)(d[2] | d[3] << 8);
	rz = (short)(d[4] | d[5] << 8);
	v[0] = -ry;
	v[1] = -rx;
	v[2] = -rz;
	return 0;
}

/* ---- smoothing for the cores that take the reading as a cartridge's sensor would ----
 * A reading is split in two: a slow part, an average that follows gravity as the console is
 * tilted, and a fast part, what is left (hand shake, the bump at the end of a quick tilt, a
 * flick). The fast part only passes beyond a dead zone, less the dead zone, so handling the
 * console no longer looks like a flick to a game while a deliberate flick still does. Both
 * settings can be changed live (oem accelf) while they are being tuned. */
volatile unsigned int g_accel_smooth = 64;   /* weight of a new reading in the average, 256ths */
volatile unsigned int g_accel_flick = 900;   /* the fast part's dead zone, counts (2048 to a g) */

static int fast_part(int d)
{
	int z = (int)g_accel_flick;
	if (d > z) return d - z;
	if (d < -z) return d + z;
	return 0;
}

void ayaneo_accel_filter(int v[3])
{
	unsigned a = g_accel_smooth;
	int i;
	if (a < 1) a = 1;
	if (a > 256) a = 256;
	for (i = 0; i < 3; i++) {
		long long in = (long long)v[i] << 8;
		int slow;
		if (!s_lp_primed) s_lp[i] = in;
		else s_lp[i] += (in - s_lp[i]) * (long long)a / 256;
		slow = (int)(s_lp[i] / 256);
		v[i] = slow + fast_part(v[i] - slow);
	}
	s_lp_primed = 1;
}

/* oem accel:RR[:VV] - one register of the chip, read, or written with the chip held in standby
 * meanwhile (a mode change asks for it); -1 on a bus error */
int ayaneo_accel_reg(unsigned reg, int val)
{
	unsigned char v = 0;
	if (val >= 0) {
		int was = s_awake;
		acc_write(ACC_REG_MODE, 0x43);
		acc_write((unsigned char)reg, (unsigned char)val);
		if (was) acc_write(ACC_REG_MODE, 0x41);
	}
	if (acc_read((unsigned char)reg, &v, 1) != 0) return -1;
	return v;
}

/* oem accel: the chip's id and one reading */
void ayaneo_accel_probe_report(void (*line)(const char *))
{
	char s[96];
	unsigned char id = 0;
	int was = s_awake, v[3] = {0, 0, 0}, rc_id, rc_start, rc_read;
	rc_id = acc_read(ACC_REG_CHIP, &id, 1);
	rc_start = ayaneo_accel_start();
	udelay(20000);
	rc_read = ayaneo_accel_read(v);
	if (!was) ayaneo_accel_stop();
	snprintf(s, sizeof s, "accel mc3416@1-%02x id rc=%d %02x start=%d read=%d", ACC_ADDR, rc_id, id, rc_start, rc_read);
	line(s);
	snprintf(s, sizeof s, "accel x=%d y=%d z=%d (device frame, ~2048 per g)", v[0], v[1], v[2]);
	line(s);
}
