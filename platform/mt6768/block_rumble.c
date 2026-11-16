/*
 * block_rumble.c - the Konkr Pocket Block's vibration motor, for rumble cartridges (WarioWare:
 * Twisted!, Drill Dozer, the Game Boy Color's MBC5 rumble carts).
 *
 * A VCARE VMD5xx on I2C5 at 0x58 (the device tree's vmd5xx_vibrator@58, motor-ctrl-gpios GPIO158,
 * buzz-force-level 3), the same family as the Konkr Pocket Advance's VMD501A, so the same driver
 * (platform/common/pico/pico_rumble_vmd.inc). Android drives the motor through this chip only: its
 * PWM vibrator on GPIO108 stands down when the VMD5xx is there. The drive bytes are the ones
 * Android's driver loads on this console. I2C5 also carries the power LED (block_pico.c): every
 * transfer takes its lock.
 */
#include <platform/mt_typedefs.h>
#include <platform/mt_i2c.h>
#include <platform/mt_gpio.h>
#include <string.h>
#include <printf.h>
#include <kernel/thread.h>

extern unsigned gpt4_get_current_tick(void);
extern void block_i2c5_lock(void), block_i2c5_unlock(void);

#define VIB_ADDR      0x58
#define VIB_PIN       (158u | 0x80000000u)
#define VIB_REG_ID    0x00
#define VIB_REG_RESET 0x10    /* 0x5a before the drive bytes */
#define VIB_REG_DRIVE 0x02    /* nine bytes */
#define VIB_SETTLE    (13000u * 10u)   /* 10 ms in GPT ticks */

/* registers 0x02..0x0a as Android sets them on the Block */
static const unsigned char s_block_drive[9] = { 0x40, 0x36, 0x5e, 0x86, 0x7d, 0x12, 0x18, 0x00, 0x04 };
#define VIB_DRIVE_BYTES s_block_drive
#define VIB_NAME "vmd5xx@5"

static void vib_i2c(mt_i2c *i2c)
{
	memset(i2c, 0, sizeof(*i2c));
	i2c->id = I2C5;
	i2c->addr = VIB_ADDR;
	i2c->mode = FS_MODE;
	i2c->speed = 400;
}
static void vib_lock(void) { block_i2c5_lock(); }
static void vib_unlock(void) { block_i2c5_unlock(); }

#include "../common/pico/pico_rumble_vmd.inc"
