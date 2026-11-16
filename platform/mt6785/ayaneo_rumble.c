/*
 * ayaneo_rumble.c - the console's vibration motor, for rumble cartridges (WarioWare: Twisted!,
 * Drill Dozer, the Game Boy Color's MBC5 rumble carts).
 *
 * The motor is a small rotor driven by a VCARE VMD501A on i2c6 at 0x58 (the base device tree's
 * vmd501a@58, motor-ctrl-gpios GPIO127, motor type "rotor"). The driver is shared with the
 * Konkr Pocket Block (platform/common/pico/pico_rumble_vmd.inc); this file gives it the bus, the
 * pin and the drive bytes: one row per strength, the middle row being what Android uses.
 */
#include <platform/mt_typedefs.h>
#include <platform/mt_i2c.h>
#include <platform/mt_gpio.h>
#include <string.h>
#include <printf.h>
#include <kernel/thread.h>

extern unsigned gpt4_get_current_tick(void);
extern void ayaneo_i2c6_lock(void), ayaneo_i2c6_unlock(void);   /* mt_leds.c: the bus is shared */

#define VIB_ADDR      0x58
#define VIB_PIN       (127u | 0x80000000u)
#define VIB_REG_ID    0x00
#define VIB_REG_RESET 0x10    /* 0x5a before the drive bytes */
#define VIB_REG_DRIVE 0x02    /* nine bytes */
#define VIB_SETTLE    (13000u * 10u)   /* 10 ms in GPT ticks */

/* the drive bytes per strength, weak to strong; Android sets the middle one */
static const unsigned char s_drive[3][9] = {
	{ 0x40, 0x28, 0x7a, 0x86, 0x7d, 0x12, 0x00, 0x00, 0x04 },
	{ 0x40, 0x30, 0x6e, 0x86, 0x7d, 0x12, 0x00, 0x00, 0x04 },
	{ 0x40, 0x36, 0x5e, 0x86, 0x7d, 0x12, 0x00, 0x00, 0x04 },
};
#define VIB_STRENGTH 1

static void vib_i2c(mt_i2c *i2c)
{
	memset(i2c, 0, sizeof(*i2c));
	i2c->id = I2C6;
	i2c->addr = VIB_ADDR;
	i2c->mode = FS_MODE;
	i2c->speed = 400;
	i2c->pushpull = true;
}

static void vib_lock(void) { ayaneo_i2c6_lock(); }
static void vib_unlock(void) { ayaneo_i2c6_unlock(); }
#define VIB_DRIVE_BYTES s_drive[VIB_STRENGTH]
#define VIB_NAME "vmd501a@6"

#include "../common/pico/pico_rumble_vmd.inc"
