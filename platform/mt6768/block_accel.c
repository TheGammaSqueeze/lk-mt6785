/*
 * block_accel.c - the Konkr Pocket Block's accelerometer: a Sensortek STK8321 on I2C1 at 0x0F
 * (SCL GPIO81, SDA GPIO84), read in the bootloader for the motion games: tilt
 * cartridges and the gyro cartridges, whose rotation the cores work out from the accelerometer.
 *
 * The same entry points as the Konkr Pocket Advance's MC3416 driver (platform/mt6785/ayaneo_accel.c)
 * and the same smoothing (platform/common/pico/pico_accel_filter.inc), so the shared emulator code
 * drives both consoles alike.
 *
 * The chip's data registers hold each axis left aligned in 16 bits, low byte first, whatever the
 * part's resolution, so at the 4 g range one g reads 8192; a quarter of that gives the 2048 counts
 * to a g the cores expect. The chip is reset and set up at every start, as Android leaves it in
 * its own range and FIFO mode, and it is put back to suspend when a motion game ends; Android's
 * driver sets it up from a reset of its own at the next boot.
 *
 * Axes come out in the device frame Android uses for this board (the device tree's direction 7):
 * x = -chip y, y = -chip x, z = -chip z, so x points right and y to the far edge as the console
 * is held, z out of the screen.
 */
#include <platform/mt_typedefs.h>
#include <platform/mt_i2c.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <string.h>
#include <printf.h>

extern void mdelay(unsigned long msec);
extern int mt_set_gpio_mode(unsigned int pin, unsigned int mode);
extern int mt_set_gpio_pull_enable(unsigned int pin, unsigned int enable);
extern int mt_set_gpio_pull_select(unsigned int pin, unsigned int select);

#define ACC_ADDR        0x0f
#define ACC_REG_CHIP    0x00   /* the part's id */
#define ACC_REG_XOUT    0x02   /* X, Y, Z: 16 bits each, low byte first, left aligned */
#define ACC_REG_RANGE   0x0f   /* bits 3..0: 0x3 2 g, 0x5 4 g, 0x8 8 g */
#define ACC_REG_BW      0x10   /* filter bandwidth and output rate */
#define ACC_REG_POWER   0x11   /* 0x00 measuring, bit 7 suspend */
#define ACC_REG_RESET   0x14   /* 0xb6 resets every register */
#define ACC_RANGE_4G    0x05
#define ACC_BW_125HZ    0x0c   /* 125 Hz bandwidth, 250 readings a second: fresh at every frame */
#define ACC_POWER_ON    0x00
#define ACC_SUSPEND     0x80
#define ACC_RESET_KEY   0xb6

/* the ids of the parts this driver reads alike (all left aligned, the same range codes) */
static int acc_known_id(unsigned id)
{
	return id == 0x23 || id == 0x25 || id == 0x26 || id == 0x86 || id == 0x87;
}

/* ---- I2C1: only this chip is on the bus; the lock keeps a fastboot query and the game apart ---- */
static mutex_t s_acc_mtx;
static volatile int s_acc_mtx_up;
static void acc_lock(void)
{
	if (!s_acc_mtx_up) {
		enter_critical_section();
		if (!s_acc_mtx_up) { mutex_init(&s_acc_mtx); s_acc_mtx_up = 1; }
		exit_critical_section();
	}
	mutex_acquire(&s_acc_mtx);
}
static void acc_unlock(void) { mutex_release(&s_acc_mtx); }

/* The boot leaves both pins in another function (mode 2); Android's pin setup for the bus
 * (i2c1_pin) is mode 1 with the pull up on. Done once, before the first transfer, and left so:
 * it is what Android sets again at its start. */
static void acc_pins(void)
{
	static const unsigned int pins[] = { 81, 84 };
	static int done;
	unsigned int i;
	if (done) return;
	for (i = 0; i < sizeof pins / sizeof pins[0]; i++) {
		unsigned int p = pins[i] | 0x80000000u;
		mt_set_gpio_pull_enable(p, 1);
		mt_set_gpio_pull_select(p, 1);
		mt_set_gpio_mode(p, 1);
	}
	done = 1;
}

static void acc_bus(mt_i2c *i2c)
{
	acc_pins();
	memset(i2c, 0, sizeof *i2c);
	i2c->id = I2C1;
	i2c->addr = ACC_ADDR;
	i2c->mode = FS_MODE;
	i2c->speed = 400;
}

volatile int g_dbg_block_accel_err;	/* transfers the chip did not take */

static int acc_read(unsigned char reg, unsigned char *buf, int n)
{
	mt_i2c i2c;
	unsigned char b[8];
	int r;
	if (n < 1 || n > (int)sizeof b) return -1;
	acc_bus(&i2c);
	b[0] = reg;
	acc_lock();
	r = i2c_write_read(&i2c, b, 1, (U32)n);
	acc_unlock();
	if (r != 0) { g_dbg_block_accel_err++; return -1; }
	memcpy(buf, b, (size_t)n);
	return 0;
}

static int acc_write(unsigned char reg, unsigned char val)
{
	mt_i2c i2c;
	unsigned char b[2];
	int r;
	acc_bus(&i2c);
	b[0] = reg;
	b[1] = val;
	acc_lock();
	r = i2c_write(&i2c, b, 2);
	acc_unlock();
	if (r != 0) { g_dbg_block_accel_err++; return -1; }
	return 0;
}

/* ---- the chip ---- */
static int s_awake;
#include "../common/pico/pico_accel_filter.inc"

/* 0 when the chip answered as a known part and is now measuring */
int ayaneo_accel_start(void)
{
	unsigned char id = 0;
	if (s_awake) return 0;
	if (acc_read(ACC_REG_CHIP, &id, 1) != 0 || !acc_known_id(id)) return -1;
	acc_write(ACC_REG_RESET, ACC_RESET_KEY);   /* the chip does not answer the reset itself */
	mdelay(5);
	if (acc_read(ACC_REG_CHIP, &id, 1) != 0 || !acc_known_id(id)) return -1;
	if (acc_write(ACC_REG_RANGE, ACC_RANGE_4G) != 0) return -1;
	if (acc_write(ACC_REG_BW, ACC_BW_125HZ) != 0) return -1;
	if (acc_write(ACC_REG_POWER, ACC_POWER_ON) != 0) return -1;
	mdelay(10);                                /* the first readings */
	s_awake = 1;
	s_lp_primed = 0;                           /* the average starts again from the first reading */
	return 0;
}

/* suspend: when a motion game ends, before power off and before a reset */
void ayaneo_accel_stop(void)
{
	if (!s_awake) return;
	acc_write(ACC_REG_POWER, ACC_SUSPEND);
	s_awake = 0;
}

/* the console's gravity in the device frame at 2048 counts to a g; 0 on success */
int ayaneo_accel_read(int v[3])
{
	unsigned char d[6];
	int rx, ry, rz;
	if (!s_awake || acc_read(ACC_REG_XOUT, d, 6) != 0) return -1;
	rx = (short)(d[0] | d[1] << 8) >> 2;
	ry = (short)(d[2] | d[3] << 8) >> 2;
	rz = (short)(d[4] | d[5] << 8) >> 2;
	v[0] = -ry;
	v[1] = -rx;
	v[2] = -rz;
	return 0;
}

/* oem accel:RR[:VV] - one register of the chip, read or written; -1 on a bus error */
int ayaneo_accel_reg(unsigned reg, int val)
{
	unsigned char v = 0;
	if (val >= 0 && acc_write((unsigned char)reg, (unsigned char)val) != 0) return -1;
	if (acc_read((unsigned char)reg, &v, 1) != 0) return -1;
	return v;
}

/* oem accel: the chip's id and one reading */
void ayaneo_accel_probe_report(void (*line)(const char *))
{
	char s[112];
	unsigned char id = 0;
	int was = s_awake, v[3] = {0, 0, 0}, rc_id, rc_start, rc_read;
	rc_id = acc_read(ACC_REG_CHIP, &id, 1);
	rc_start = ayaneo_accel_start();
	rc_read = ayaneo_accel_read(v);
	if (!was) ayaneo_accel_stop();
	snprintf(s, sizeof s, "accel stk8321@1-%02x id rc=%d %02x start=%d read=%d bus errors=%d",
		 ACC_ADDR, rc_id, id, rc_start, rc_read, g_dbg_block_accel_err);
	line(s);
	snprintf(s, sizeof s, "accel x=%d y=%d z=%d (device frame, ~2048 per g)", v[0], v[1], v[2]);
	line(s);
}
