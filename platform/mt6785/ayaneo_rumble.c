/*
 * ayaneo_rumble.c - the console's vibration motor, for rumble cartridges (WarioWare: Twisted!,
 * Drill Dozer, the Game Boy Color's MBC5 rumble carts).
 *
 * The motor is a small rotor driven by a VCARE VMD501A on i2c6 at 0x58 (the base device tree's
 * vmd501a@58, motor-ctrl-gpios GPIO127, motor type "rotor"). The chip answers 0x51 or 0x52 at
 * register 0x00. It is set up once over I2C: 0x5a into register 0x10, then, 10 ms later, nine
 * drive bytes from register 0x02 on, one row per strength, the middle row being what Android
 * uses. After that the motor runs while GPIO127 is high and stops when it is low, so a rumble
 * cartridge's motor bit maps straight onto the pin.
 *
 * The set up does not wait: the first call sends the 0x10 write and notes the time, and the
 * drive bytes go out on the first call after the 10 ms. The pin is driven from the first call
 * either way. A console without the chip still gets the pin, which is harmless.
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

enum { VIB_COLD, VIB_SETTLING, VIB_READY, VIB_ABSENT };
static int s_state = VIB_COLD;
static unsigned s_t0;
static int s_on = -1;          /* the pin as last driven, -1 before the first call */
volatile int g_dbg_vib_id = -1;   /* oem rumble: the chip id read at set up, -1 not read */
volatile unsigned int g_dbg_vib_ons;   /* oem rumble: times the motor was started */
/* oem rumble: the last runs, in calls (frames), on then off, to see how a game drives its motor */
static unsigned short s_runs[16][3];   /* on frames, off frames, motor writes while on */
static unsigned s_run_i, s_run_len, s_run_writes;

static void vib_i2c(mt_i2c *i2c)
{
	memset(i2c, 0, sizeof(*i2c));
	i2c->id = I2C6;
	i2c->addr = VIB_ADDR;
	i2c->mode = FS_MODE;
	i2c->speed = 400;
	i2c->pushpull = true;
}

static int vib_write(const unsigned char *buf, unsigned n)
{
	mt_i2c i2c;
	int rc;
	vib_i2c(&i2c);
	ayaneo_i2c6_lock();
	rc = i2c_write(&i2c, (unsigned char *)buf, n);
	ayaneo_i2c6_unlock();
	return rc;
}

static int vib_read_id(void)
{
	mt_i2c i2c;
	unsigned char buf[1] = { VIB_REG_ID };
	int rc;
	vib_i2c(&i2c);
	ayaneo_i2c6_lock();
	rc = i2c_write_read(&i2c, buf, 1, 1);
	ayaneo_i2c6_unlock();
	return rc < 0 ? -1 : buf[0];
}

static void vib_setup_step(void)
{
	if (s_state == VIB_COLD) {
		static const unsigned char reset[2] = { VIB_REG_RESET, 0x5a };
		mt_set_gpio_mode(VIB_PIN, 0);
		mt_set_gpio_out(VIB_PIN, 0);
		mt_set_gpio_dir(VIB_PIN, GPIO_DIR_OUT);
		g_dbg_vib_id = vib_read_id();
		if (g_dbg_vib_id != 0x51 && g_dbg_vib_id != 0x52) { s_state = VIB_ABSENT; return; }
		vib_write(reset, sizeof reset);
		s_t0 = gpt4_get_current_tick();
		s_state = VIB_SETTLING;
	} else if (s_state == VIB_SETTLING && (unsigned)(gpt4_get_current_tick() - s_t0) >= VIB_SETTLE) {
		unsigned char drive[10];
		drive[0] = VIB_REG_DRIVE;
		memcpy(drive + 1, s_drive[VIB_STRENGTH], 9);
		vib_write(drive, sizeof drive);
		s_state = VIB_READY;
	}
}

/* the motor on or off; called once a frame with what the core reports (0 off, else on: the
 * motor writes since the last frame where the core counts them), and with 0 when the game
 * pauses, ends or the console powers off */
void ayaneo_rumble_set(int on)
{
	unsigned writes = on > 0 ? (unsigned)on : 0;
	on = on ? 1 : 0;
	if (on == s_on) { if (s_run_len < 0xffff) s_run_len++; }
	else if (s_on >= 0) {
		/* a run ended: an on run opens a new entry, an off run closes it */
		if (s_on) {
			s_runs[s_run_i & 15][0] = (unsigned short)s_run_len;
			s_runs[s_run_i & 15][1] = 0;
			s_runs[s_run_i & 15][2] = (unsigned short)(s_run_writes > 0xffff ? 0xffff : s_run_writes);
			s_run_i++;
		} else {
			s_runs[(s_run_i - 1) & 15][1] = (unsigned short)s_run_len;
		}
		s_run_len = 1;
		s_run_writes = 0;
	}
	if (on) s_run_writes += writes;
	if (on && s_state != VIB_READY && s_state != VIB_ABSENT) vib_setup_step();
	if (s_state == VIB_COLD) return;   /* never started: the pin is still the boot default (low) */
	if (on == s_on) return;
	mt_set_gpio_out(VIB_PIN, (u32)on);
	if (on) g_dbg_vib_ons++;
	s_on = on;
}

void ayaneo_rumble_stop(void) { ayaneo_rumble_set(0); }

/* oem rumble: the chip, its state and how often the motor ran */
void ayaneo_rumble_report(void (*line)(const char *))
{
	char s[96];
	static const char *const st[] = { "not set up", "settling", "ready", "absent" };
	unsigned i, n = 0;
	snprintf(s, sizeof s, "rumble vmd501a@6-%02x id=%d state=%s on=%d starts=%u", VIB_ADDR, g_dbg_vib_id,
		 st[s_state], s_on, g_dbg_vib_ons);
	line(s);
	n = snprintf(s, sizeof s, "runs on/off frames,writes:");
	for (i = 0; i < 6 && n < sizeof s - 16; i++) {
		unsigned k = (s_run_i - 6 + i) & 15;
		n += snprintf(s + n, sizeof s - n, " %u/%u,%u", s_runs[k][0], s_runs[k][1], s_runs[k][2]);
	}
	line(s);
}

/* oem rumble:MS - run the motor for MS milliseconds (a test from the bootloader channel) */
void ayaneo_rumble_pulse(unsigned ms)
{
	unsigned t;
	while (s_state == VIB_COLD || s_state == VIB_SETTLING) {
		vib_setup_step();
		if (s_state == VIB_SETTLING) thread_sleep(2);
	}
	ayaneo_rumble_set(1);
	t = ms > 3000 ? 3000 : ms;
	thread_sleep(t);
	ayaneo_rumble_set(0);
}
