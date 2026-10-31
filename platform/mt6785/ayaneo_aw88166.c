/*
 * Konkr Pocket Advance speaker amplifiers: two Awinic AW88166 smart amps on i2c6 (0x34 left,
 * reset GPIO24; 0x35 right, reset GPIO21) fed by AFE I2S3 (48 kHz, 64fs BCK, 32 bit slots).
 *
 * The stock vendor firmware aw883xx_acf.bin (project A2066) carries, per amp, the Music profile:
 * a register table, a 4 KB DSP firmware and a 2.2 KB DSP config. tools/konkr/aw883xx_acf.py
 * extracts them into ayaneo_aw88166_acf.h and this file replays what the kernel driver does
 * (sound/soc/codecs/aw88166.c): registers (mute kept on), DSP bypass + memory clock from the
 * oscillator while the firmware and config stream through DSPMADD/DSPMDAT, memory clock back
 * to the DSP clock, then power up, PLL and status checks, DSP enable, unmute. The I2S clocks
 * must be running before the start sequence (the PLL locks to BCK).
 */
#include <platform/mt_typedefs.h>
#include <platform/mt_i2c.h>
#include <platform/mt_gpio.h>
#include <string.h>
#include <debug.h>
#include <kernel/thread.h>
extern unsigned gpt4_get_current_tick(void);
#include <stdio.h>
#include "ayaneo_aw88166_acf.h"

extern void udelay(unsigned long usec);
extern void mdelay(unsigned long msec);
extern S32 mt_set_gpio_mode(u32 pin, u32 mode);
extern S32 mt_set_gpio_dir(u32 pin, u32 dir);
extern S32 mt_set_gpio_out(u32 pin, u32 output);
#define GP(n) ((n) | 0x80000000u)

#define AW_ID_REG       0x00
#define AW_SYSST_REG    0x01
#define AW_SYSINT_REG   0x02
#define AW_SYSINTM_REG  0x03
#define AW_SYSCTRL_REG  0x04
#define AW_SYSCTRL2_REG 0x05
#define AW_PWMCTRL3_REG 0x16
#define AW_DBGCTRL_REG  0x18
#define AW_DSPMADD_REG  0x40
#define AW_DSPMDAT_REG  0x41
#define AW_CHIP_ID      0x2066
#define AW_DSP_FW_ADDR  0x8980
#define AW_DSP_CFG_ADDR 0x9B00
/* SYSCTRL bits */
#define SC_PWDN   (1u << 0)
#define SC_AMPPD  (1u << 1)
#define SC_DSPBY  (1u << 2)
#define SC_HMUTE  (1u << 8)
/* DBGCTRL */
#define DBG_MEMCLK_DAP (1u << 3)   /* 0 = oscillator, 1 = DSP clock */
/* SYSST bits */
#define ST_PLLS   (1u << 0)
#define ST_CLKS   (1u << 4)
#define ST_SWS    (1u << 8)
#define ST_BSTS   (1u << 9)
#define ST_DSPS   (1u << 12)

struct aw_amp {
	unsigned char addr;
	unsigned gpio_reset;
	const unsigned short (*regs)[2];
	unsigned nregs;
	const unsigned char *fw; unsigned fwlen;
	const unsigned char *cfg; unsigned cfglen;
	/* state for oem aw */
	unsigned short id, sysst, sysctrl;
	int step;          /* how far the bring-up got (oem aw) */
	int rc;            /* last i2c return */
	int loaded;        /* registers + DSP image are in the chip: a restart is power/unmute only */
	unsigned t[8];     /* ms since boot at: thread start, reset, regs, fw, cfg, power on, unmute */
};
#define AW_T(a, i) ((a)->t[i] = gpt4_get_current_tick() / 13000u)

static struct aw_amp s_amp[2] = {
	{ 0x34, 24u, aw88166_reg_dev0, sizeof(aw88166_reg_dev0) / sizeof(aw88166_reg_dev0[0]),
	  aw88166_dspfw_dev0, sizeof aw88166_dspfw_dev0, aw88166_dspcfg_dev0, sizeof aw88166_dspcfg_dev0, 0, 0, 0, 0, 0, 0 },
	{ 0x35, 21u, aw88166_reg_dev1, sizeof(aw88166_reg_dev1) / sizeof(aw88166_reg_dev1[0]),
	  aw88166_dspfw_dev1, sizeof aw88166_dspfw_dev1, aw88166_dspcfg_dev1, sizeof aw88166_dspcfg_dev1, 0, 0, 0, 0, 0, 0 },
};
static int s_started;

static void aw_i2c(mt_i2c *i2c, unsigned char addr)
{
	memset(i2c, 0, sizeof(*i2c));
	i2c->id = I2C6;
	i2c->addr = addr;
	i2c->mode = FS_MODE;   /* 400 kHz */
	i2c->speed = 400;
	i2c->pushpull = true;
}

static int aw_wr(struct aw_amp *a, unsigned char reg, unsigned short val)
{
	mt_i2c i2c; unsigned char buf[3];
	aw_i2c(&i2c, a->addr);
	buf[0] = reg; buf[1] = (unsigned char)(val >> 8); buf[2] = (unsigned char)val;
	a->rc = i2c_write(&i2c, buf, 3);
	return a->rc;
}

static int aw_rd(struct aw_amp *a, unsigned char reg, unsigned short *val)
{
	mt_i2c i2c; unsigned char buf[2];
	aw_i2c(&i2c, a->addr);
	buf[0] = reg; buf[1] = 0;
	a->rc = i2c_write_read(&i2c, buf, 1, 2);
	if (a->rc < 0) return a->rc;
	*val = (unsigned short)((buf[0] << 8) | buf[1]);
	return 0;
}

/* keep the bits in `keep`, set `val` */
static int aw_bits(struct aw_amp *a, unsigned char reg, unsigned short keep, unsigned short val)
{
	unsigned short v = 0;
	if (aw_rd(a, reg, &v) < 0) return -1;
	return aw_wr(a, reg, (unsigned short)((v & keep) | val));
}

/* DSP memory: set the address once, then every 16 bit word written to DSPMDAT auto increments */
volatile int g_dbg_aw_dma = -1;   /* oem led: 1 the DSP image went over the AP DMA, 0 it fell back to FIFO writes */
extern void arch_clean_cache_range(addr_t start, size_t len);
static unsigned char s_aw_dma_buf[1 + 240] __attribute__((aligned(64)));
/* the bring-up thread starts at DEFAULT_PRIORITY to overlap the boot; once a core is feeding the
 * audio ring it steps down to LOW_PRIORITY so it only uses the frame-wait sleeps (sharing the
 * CPU round robin with the presenter underran the ring at the second amp's unmute) */
static void aw_yield_to_core(void)
{
	extern volatile unsigned int g_dbg_asub_done;
	static int low;
	if (!low && g_dbg_asub_done) { low = 1; thread_set_priority(LOW_PRIORITY); }
}
static int aw_stream(struct aw_amp *a, unsigned short base, const unsigned char *data, unsigned len)
{
	unsigned i;
	if (aw_wr(a, AW_DSPMADD_REG, base) < 0) return -1;
	/* DMA: 120 words per transaction instead of three (the chip auto increments the DSP address);
	 * the first failure drops to the FIFO path below, re-seeding the address register */
	if (g_dbg_aw_dma != 0) {
		int ok = 1;
		for (i = 0; i + 2u <= len; ) {
			mt_i2c i2c; unsigned n = 0;
			aw_i2c(&i2c, a->addr);
			s_aw_dma_buf[0] = AW_DSPMDAT_REG;
			while (n < 240u && i + 2u <= len) { s_aw_dma_buf[1 + n] = data[i]; s_aw_dma_buf[2 + n] = data[i + 1]; n += 2u; i += 2u; }
			arch_clean_cache_range((addr_t)s_aw_dma_buf, sizeof s_aw_dma_buf);
			aw_yield_to_core();
			if (i2c_write_dma(&i2c, s_aw_dma_buf, 1 + n) != 0) { ok = 0; break; }
		}
		if (ok) { g_dbg_aw_dma = 1; return 0; }
		g_dbg_aw_dma = 0;
		if (aw_wr(a, AW_DSPMADD_REG, base) < 0) return -1;
	}
	/* three 16 bit words per transaction (register + 6 data bytes fit the 8 byte I2C FIFO);
	 * the chip auto increments the DSP address per word */
	for (i = 0; i + 2u <= len; ) {
		mt_i2c i2c; unsigned char buf[7]; unsigned n = 0;
		aw_i2c(&i2c, a->addr);
		buf[0] = AW_DSPMDAT_REG;
		while (n < 6u && i + 2u <= len) { buf[1 + n] = data[i]; buf[2 + n] = data[i + 1]; n += 2u; i += 2u; }
		a->rc = i2c_write(&i2c, buf, 1 + n);
		if (a->rc < 0) return -1;
	}
	return 0;
}

static void aw_reset(struct aw_amp *a)
{
	mt_set_gpio_mode(GP(a->gpio_reset), 0);
	mt_set_gpio_dir(GP(a->gpio_reset), 1);
	mt_set_gpio_out(GP(a->gpio_reset), 0);
	mdelay(1);
	mt_set_gpio_out(GP(a->gpio_reset), 1);
	mdelay(2);
}

/* power up an amp whose registers and DSP image are already loaded (I2S clocks running) */
static int aw_power_on(struct aw_amp *a)
{
	unsigned i; unsigned short v = 0;
	a->step = 8;
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_PWDN, 0) < 0) return -1;
	mdelay(2);
	for (i = 0; i < 10u; i++) {
		if (aw_rd(a, AW_SYSST_REG, &a->sysst) < 0) return -1;
		if ((a->sysst & (ST_PLLS | ST_CLKS)) == (ST_PLLS | ST_CLKS)) break;
		mdelay(2);
	}
	if ((a->sysst & (ST_PLLS | ST_CLKS)) != (ST_PLLS | ST_CLKS)) return -1;
	a->step = 9;
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_AMPPD, 0) < 0) return -1;
	mdelay(1);
	{
		unsigned short check = ST_BSTS | ST_CLKS | ST_PLLS;
		if (aw_rd(a, AW_PWMCTRL3_REG, &v) == 0 && !(v & (1u << 13)))
			check |= ST_SWS;   /* noise gate off: the switching flag must be up too */
		for (i = 0; i < 10u; i++) {
			if (aw_rd(a, AW_SYSST_REG, &a->sysst) < 0) return -1;
			if ((a->sysst & check) == check) break;
			mdelay(2);
		}
		if ((a->sysst & check) != check) return -1;
	}
	/* DSP on: bypass -> working, then wait for its status */
	a->step = 10;
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_DSPBY, SC_DSPBY) < 0) return -1;
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_DSPBY, 0) < 0) return -1;
	mdelay(1);
	for (i = 0; i < 5u; i++) {
		if (aw_rd(a, AW_SYSST_REG, &a->sysst) < 0) return -1;
		if (!(a->sysst & ST_DSPS)) break;
		mdelay(2);
	}
	a->step = 11;
	aw_rd(a, AW_SYSINT_REG, &v);   /* clear the interrupt status */
	AW_T(a, 5);
	return 0;
}

/* the last step, done for both amps back to back so the speakers start together */
static int aw_unmute(struct aw_amp *a)
{
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_HMUTE, 0) < 0) return -1;
	aw_rd(a, AW_SYSCTRL_REG, &a->sysctrl);
	a->step = 12;
	AW_T(a, 6);
	return 0;
}
volatile unsigned g_dbg_aw_up_ms[2], g_dbg_aw_unmute_ms, g_dbg_aw_create_ms;   /* oem diag */

static int aw_bringup(struct aw_amp *a)
{
	unsigned i; unsigned short v = 0;
	a->step = 1;
	AW_T(a, 0);
	aw_reset(a);
	if (aw_rd(a, AW_ID_REG, &a->id) < 0) return -1;
	AW_T(a, 1);
	a->step = 2;
	if (a->id != AW_CHIP_ID) return -1;
	/* profile registers; the interrupt mask stays fully masked and the mute bit stays set */
	a->step = 3;
	for (i = 0; i < a->nregs; i++) {
		unsigned char reg = (unsigned char)a->regs[i][0];
		unsigned short val = a->regs[i][1];
		if (reg == AW_SYSINTM_REG) val = 0xffff;
		if (reg == AW_SYSCTRL_REG) val |= SC_HMUTE;
		if (aw_wr(a, reg, val) < 0) return -1;
	}
	AW_T(a, 2);
	/* DSP bypassed and its memory on the oscillator while loading */
	a->step = 4;
	if (aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_DSPBY, SC_DSPBY) < 0) return -1;
	if (aw_bits(a, AW_DBGCTRL_REG, (unsigned short)~DBG_MEMCLK_DAP, 0) < 0) return -1;
	/* SRAM address register check */
	a->step = 5;
	if (aw_wr(a, AW_DSPMADD_REG, 0x5555) < 0 || aw_rd(a, AW_DSPMADD_REG, &v) < 0 || v != 0x5555) return -1;
	if (aw_wr(a, AW_DSPMADD_REG, 0xaaaa) < 0 || aw_rd(a, AW_DSPMADD_REG, &v) < 0 || v != 0xaaaa) return -1;
	a->step = 6;
	if (aw_stream(a, AW_DSP_FW_ADDR, a->fw, a->fwlen) < 0) return -1;
	AW_T(a, 3);
	a->step = 7;
	if (aw_stream(a, AW_DSP_CFG_ADDR, a->cfg, a->cfglen) < 0) return -1;
	AW_T(a, 4);
	if (aw_bits(a, AW_DBGCTRL_REG, (unsigned short)~DBG_MEMCLK_DAP, DBG_MEMCLK_DAP) < 0) return -1;
	a->loaded = 1;
	/* power up: the PLL locks to the I2S clocks, which must already be running */
	return aw_power_on(a);
}

/* The bring-up (two amps, about 6 KB of DSP data each over I2C plus the status waits) runs
 * on its own thread so the first video frame and the audio DMA do not wait for it; the
 * amps stay muted until they are ready, which only delays the first sound slightly. */
static volatile int s_starting;
static int aw_start_thread(void *arg);
void ayaneo_aw88166_start(void)
{
	thread_t *t;
	if (s_started || s_starting) return;
	s_starting = 1;
	g_dbg_aw_create_ms = gpt4_get_current_tick() / 13000u;
	/* DEFAULT_PRIORITY: it shares the CPU round robin with the boot/menu/emulator threads (1 ms
	 * tick) instead of waiting for their sleeps; the DSP image goes over DMA so the whole
	 * bring-up is a few tens of ms of CPU and the amps are up before the first sound */
	t = thread_create("aw88166", &aw_start_thread, NULL, DEFAULT_PRIORITY, 16384);
	if (t) { thread_resume(t); return; }
	aw_start_thread(NULL);   /* no thread: do it inline */
}
static int aw_start_thread(void *arg)
{
	int k;
	(void)arg;
	for (k = 0; k < 2; k++) {
		int r = s_amp[k].loaded ? aw_power_on(&s_amp[k]) : aw_bringup(&s_amp[k]);
		if (r < 0 && s_amp[k].loaded) { s_amp[k].loaded = 0; r = aw_bringup(&s_amp[k]); }   /* fall back to a full load */
		g_dbg_aw_up_ms[k] = gpt4_get_current_tick() / 13000u;
		if (r < 0)
			dprintf(CRITICAL, "AW88166 %d (0x%02x): bring-up stopped at step %d id=0x%04x sysst=0x%04x rc=%d\n",
				k, s_amp[k].addr, s_amp[k].step, s_amp[k].id, s_amp[k].sysst, s_amp[k].rc);
	}
	for (k = 0; k < 2; k++)
		if (s_amp[k].step == 11 && aw_unmute(&s_amp[k]) == 0)
			dprintf(CRITICAL, "AW88166 %d (0x%02x): up, sysst=0x%04x sysctrl=0x%04x\n",
				k, s_amp[k].addr, s_amp[k].sysst, s_amp[k].sysctrl);
	g_dbg_aw_unmute_ms = gpt4_get_current_tick() / 13000u;
	s_started = 1;
	s_starting = 0;
	return 0;
}

/* mute, amp off, power down (before the I2S clocks stop) */
void ayaneo_aw88166_stop(void)
{
	int k;
	while (s_starting) thread_sleep(5);   /* let a bring-up in flight finish first */
	if (!s_started) return;
	for (k = 0; k < 2; k++) {
		struct aw_amp *a = &s_amp[k];
		if (a->step < 8) continue;
		aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_HMUTE, SC_HMUTE);
		mdelay(1);
		aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_AMPPD, SC_AMPPD);
		aw_bits(a, AW_SYSCTRL_REG, (unsigned short)~SC_PWDN, SC_PWDN);
	}
	s_started = 0;
}

int ayaneo_aw88166_started(void) { return s_started; }

/* oem aw: one line per amp */
void ayaneo_aw88166_diag(int k, char *buf, unsigned cap)
{
	struct aw_amp *a = &s_amp[k & 1];
	unsigned short st = 0, sc = 0;
	if (s_started) { aw_rd(a, AW_SYSST_REG, &st); aw_rd(a, AW_SYSCTRL_REG, &sc); }
	snprintf(buf, cap, "aw%d 0x%02x: id=0x%04x step=%d rc=%d sysst=0x%04x sysctrl=0x%04x", k & 1, a->addr, a->id, a->step, a->rc,
		 s_started ? st : a->sysst, s_started ? sc : a->sysctrl);
}
/* bring-up timeline in ms since boot: start reset regs fw cfg power unmute */
void ayaneo_aw88166_times(int k, char *buf, unsigned cap)
{
	struct aw_amp *a = &s_amp[k & 1];
	snprintf(buf, cap, "aw%d t: %u %u %u %u %u %u %u (thread %u)", k & 1, a->t[0], a->t[1], a->t[2], a->t[3], a->t[4], a->t[5], a->t[6], g_dbg_aw_create_ms);
}

/* oem aw:reinit - redo the whole bring-up (full reload) with the I2S clocks as they are */
void ayaneo_aw88166_reinit(void)
{
	ayaneo_aw88166_stop();
	s_amp[0].loaded = 0; s_amp[1].loaded = 0;
	ayaneo_aw88166_start();
}
