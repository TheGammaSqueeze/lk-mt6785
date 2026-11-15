/*
 * AYANEO Pocket Air Mini boot audio (experiment).
 *
 * Plays a short PCM "boot sound" out the loudspeaker at the start of the LK
 * boot animation. There is no audio support in stock LK, so this brings up the
 * whole path from scratch, faithfully translated from the mt6785 kernel driver:
 *
 *   audio power domain (SPM MTCMOS)  ->  AFE clocks + DL1 downlink memif
 *   ->  interconnect DL1 -> ADDA DL  ->  MT6359 codec analog (main DAC -> LOL)
 *   ->  external class-D speaker amp (spk_en / spk2_en GPIOs)
 *
 * The PCM is stored, deflate-compressed, in the boot_b partition at a fixed
 * offset (past the video animation blob), decoded into a DMA buffer, and the
 * AFE memif DMAs it to the codec. The memif is a hardware ring, so a small
 * teardown thread stops playback after the clip length elapses ("play once").
 *
 * Fixed format: 48 kHz, stereo, 16-bit little-endian PCM.
 */

#include <platform/mt_typedefs.h>
#include <platform/mt_reg_base.h>
#include <platform/spm.h>
#include <platform/mt_gpio.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <string.h>
#include <debug.h>
#include <part_interface.h>
#if defined(AYANEO_AUDIO_TRACE) || defined(AYANEO_DEBUG_LOGGING)
#include <platform/boot_mode.h>		/* g_boot_arg->log_enable (force UART) */
/* Force LK's UART console on so _dprintf/GBA_ATRACE output reaches the wire even
 * when the boot chime never runs (uart_putc gates on log_enable). Called from the
 * GBA emulator entry so its bring-up traces are always visible. */
void ayaneo_gba_force_uart(void)
{
	g_boot_arg->log_enable = 1;
	g_boot_arg->meta_log_disable = 0;
}
#endif

/* Diagnostic trace that survives a release build. The dprintf macro is compiled
 * out when DEBUGLEVEL==0 (release), so call _dprintf directly - it still emits,
 * gated only by uart_putc's log_enable, which the trace build force-enables. */
#if defined(AYANEO_AUDIO_TRACE) || defined(AYANEO_DEBUG_LOGGING)
extern int _dprintf(const char *fmt, ...);
#define ATRACE(...)	_dprintf(__VA_ARGS__)
#else
#define ATRACE(...)	do {} while (0)
#endif

/* ---- externs (provided elsewhere in LK) ---- */
extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
extern signed int pwrap_write(unsigned int adr, unsigned int wdata);
extern void mdelay(unsigned long msec);
extern void udelay(unsigned long usec);
extern void arch_clean_cache_range(addr_t start, size_t len);
extern int zunzip(unsigned char *src, unsigned long *lenp, void *dst,
		  int dstlen, int offset);
extern time_t current_time(void);
extern S32 mt_set_gpio_mode(u32 pin, u32 mode);
#ifndef GP
#define GP(n) ((n) | 0x80000000u)   /* mt_gpio pin handle */
#endif
extern S32 mt_set_gpio_dir(u32 pin, u32 dir);
extern S32 mt_set_gpio_out(u32 pin, u32 output);
extern S32 mt_get_gpio_out(u32 pin);
extern S32 mt_get_gpio_dir(u32 pin);
extern S32 mt_get_gpio_mode(u32 pin);

/* ================= tunables ================= */

/* boot_b layout: video animation blob at 0, audio blob at this offset. */
#define AYANEO_AUDIO_PART	"boot_b"
#define AYANEO_AUDIO_OFF	0x01000000u	/* 16 MB into boot_b */

/* blob header: 'ABA1', ver, rate, pcm_bytes, ch, bits, comp_len, then deflate */
#define AYANEO_AUDIO_MAGIC	0x31414241u	/* "ABA1" little-endian */
#define AYANEO_AUDIO_HDR	24u

/* speaker amp enable GPIOs (from device dtbo ext_spk_control) */
#define AYANEO_SPK_EN_GPIO	24
#define AYANEO_SPK2_EN_GPIO	21

/* max decoded PCM we will play (bytes). 48k*2ch*2B = 192000 B/s -> ~5.4 s. */
#define AYANEO_PCM_MAX		(1024u * 1024u)
/* max compressed payload we will read from the partition. */
#define AYANEO_AUDIO_COMPMAX	(768u * 1024u)

/* ================= AFE register access ================= */

#define AFE_REG(off)		(AUDIO_BASE + (off))
#define afe_r(off)		DRV_Reg32(AFE_REG(off))
#define afe_w(off, v)		DRV_WriteReg32(AFE_REG(off), (v))
static inline void afe_rmw(unsigned int off, unsigned int mask, unsigned int val)
{
	afe_w(off, (afe_r(off) & ~mask) | (val & mask));
}

/* AFE register offsets (mt6785-reg.h) */
#define AUDIO_TOP_CON0			0x0000
#define AUDIO_TOP_CON1			0x0004
#define AFE_DAC_CON0			0x0010
#define AFE_CONN0			0x0020   /* O0 = I2S3 ch1 (the DAI the kernel calls I2S3 is the I2S4 register block) */
#define AFE_CONN1			0x0024   /* O1 = I2S3 ch2 */
#define AFE_CONN3			0x002c
#define AFE_CONN4			0x0030
#define AFE_I2S_CON3			0x0040   /* I2S3 out: bit0 EN, bit1 WLEN (1 = 32 bit), bit3 FMT (1 = I2S), bits 11:8 rate (10 = 48 kHz), bit12 HD */
#define AFE_DL1_CON0			0x004c
#define AFE_DL1_BASE_MSB		0x0050
#define AFE_DL1_BASE			0x0054
#define AFE_DL1_CUR			0x005c
#define AFE_DL1_END_MSB			0x0060
#define AFE_DL1_END			0x0064
#define AFE_ADDA_DL_SRC2_CON0		0x0108
#define AFE_ADDA_DL_SRC2_CON1		0x010c
#define AFE_ADDA_UL_DL_CON0		0x0124
#define AFE_ADDA_PREDIS_CON0		0x0260
#define AFE_ADDA_PREDIS_CON1		0x0264
#define AFE_ADDA_DL_SDM_DCCOMP_CON	0x0c50
#define AFE_ADDA_DL_SDM_AUTO_RESET_CON	0x0c74
#define AFE_ADDA_MTKAIF_CFG0		0x0e00
#define AFE_AUD_PAD_TOP			0x0e40

/* ================= MT6359 codec register access (via pmic-wrap) ================= */

static inline unsigned int pmic_r(unsigned int reg)
{
	unsigned int v = 0;
	pwrap_read(reg, &v);
	return v & 0xffff;
}
static inline void pmic_w(unsigned int reg, unsigned int val)
{
	pwrap_write(reg, val & 0xffff);
}
static inline void pmic_rmw(unsigned int reg, unsigned int mask, unsigned int val)
{
	pmic_w(reg, (pmic_r(reg) & ~mask) | (val & mask));
}

/* MT6359 register addresses (mach/upmu_hw.h, base 0x0) */
#define MT6359_LDO_VAUD18_CON0		0x1c98
#define MT6359_GPIO_MODE2		0x00d8
#define MT6359_GPIO_MODE2_SET		0x00da
#define MT6359_GPIO_MODE2_CLR		0x00dc
#define MT6359_GPIO_MODE3		0x00de
#define MT6359_GPIO_MODE3_SET		0x00e0
#define MT6359_GPIO_MODE3_CLR		0x00e2
#define MT6359_DCXO_CW12		0x07a8
#define MT6359_AUDENC_ANA_CON23		0x2536
#define MT6359_AUD_TOP_CKPDN_CON0	0x230c
#define MT6359_AFE_UL_DL_CON0		0x2388
#define MT6359_AFE_DL_SRC2_CON0_L	0x238a
#define MT6359_AFE_TOP_CON0		0x2394
#define MT6359_AUDIO_TOP_CON0		0x2396
#define MT6359_AFUNC_AUD_CON0		0x239a
#define MT6359_AFUNC_AUD_CON2		0x239e
#define MT6359_AFE_NCP_CFG0		0x24de
#define MT6359_AUDDEC_ANA_CON0		0x2588
#define MT6359_AUDDEC_ANA_CON1		0x258a
#define MT6359_AUDDEC_ANA_CON2		0x258c
#define MT6359_AUDDEC_ANA_CON4		0x2590
#define MT6359_AUDDEC_ANA_CON9		0x259a
#define MT6359_AUDDEC_ANA_CON10		0x259c
#define MT6359_AUDDEC_ANA_CON11		0x259e
#define MT6359_AUDDEC_ANA_CON12		0x25a0
#define MT6359_AUDDEC_ANA_CON13		0x25a2
#define MT6359_AUDDEC_ANA_CON14		0x25a4
#define MT6359_ZCD_CON2			0x260c

/* MT6359 registers used by the 3.5mm headphone-jack detection + routing (addresses
 * from platform/include/platform/upmu_hw.h, PMIC base 0x0; this file keeps its own
 * local copies instead of pulling the whole header, as the block above already does). */
#define MT6359_AUD_TOP_RST_CON0		0x2320
#define MT6359_AUD_TOP_INT_STATUS0	0x2334
#define MT6359_AFE_NCP_CFG2		0x24e2
#define MT6359_AUDENC_ANA_CON16		0x2528	/* MICBIAS1 */
#define MT6359_AUDENC_ANA_CON18		0x252c	/* accdet comparator (mic mode / pole bits) */
#define MT6359_AUDENC_ANA_CON19		0x252e
#define MT6359_AUDENC_ANA_CON20		0x2530
#define MT6359_AUDENC_ANA_CON21		0x2532
#define MT6359_ACCDET_CON1		0x268a
#define MT6359_ACCDET_CON2		0x268c
#define MT6359_ACCDET_CON6		0x2694
#define MT6359_ACCDET_CON14		0x26a4
#define MT6359_ACCDET_CON15		0x26a6
#define MT6359_ACCDET_CON18		0x26ac
#define MT6359_ACCDET_CON19		0x26ae
#define MT6359_ACCDET_CON20		0x26b0
#define MT6359_ACCDET_CON25		0x26ba	/* [7:6] plug type: 0 = 3 pole, 1 = 4 pole (mic), 3 = none */
#define MT6359_ACCDET_CON26		0x26bc	/* [7:6] EINT0_MEM_IN: 0 = plugged, 3 = unplugged */

/* delay from boot-audio start to the first sample leaving the speaker */
#define AYANEO_AUDIO_DELAY_MS	250

/* playback volume, 0-100 percent (0 = silent). Set via the build (see the
 * project .mk); applied in code by scaling the decoded PCM so the stored asset
 * stays full-scale and the level is a flexible per-build/per-user knob. */
#ifndef AYANEO_AUDIO_VOLUME
#define AYANEO_AUDIO_VOLUME	40
#endif
/* software fade applied to the tail of the clip so the stream ends at zero */
#define AYANEO_AUDIO_FADE_MS	24

/* ================= state ================= */

static unsigned char s_pcm[AYANEO_PCM_MAX] __attribute__((aligned(4096)));
static volatile int s_audio_active;	/* audio thread is running */
static volatile int s_audio_stop_req;	/* request the thread to stop early */
static unsigned int s_audio_ms;

#include "../common/pico/ayaneo_settings.inc"

/*
 * playback_gpio_set: switch the MT6359 serial audio pads (mosi = downlink
 * data, clk, sync) into audio-interface mode. Without this the codec never
 * receives the DL samples over the MTKAIF link. This is the first step of the
 * kernel start_trim_hardware / mt_dl_gpio_event.
 */
static void mt6359_playback_gpio_set(void)
{
	pmic_w(MT6359_GPIO_MODE2_CLR, 0x0ffe);
	pmic_w(MT6359_GPIO_MODE2_SET, 0x0249);
	pmic_w(MT6359_GPIO_MODE2, 0x0249);		/* clk/data mosi */
	pmic_w(MT6359_GPIO_MODE3_CLR, 0x0006);
	pmic_w(MT6359_GPIO_MODE3_SET, 0x0001);
	pmic_rmw(MT6359_GPIO_MODE3, 0x0007, 0x0001);	/* sync mosi */
}

/* ---------- codec front-end common part (start_trim_hardware) ---------- */
static void mt6359_dl_common_on(void)
{
	mt6359_playback_gpio_set();
	/* audio 1.8V LDO supply (LDO_VAUD18) - powers the codec analog */
	pmic_rmw(MT6359_LDO_VAUD18_CON0, (0x1 << 0), (0x1 << 0));
	pmic_rmw(MT6359_AUDDEC_ANA_CON13, (0x1 << 4), 0);	/* clr RG_AUDGLB_PWRDN */
	pmic_rmw(MT6359_DCXO_CW12, (0x1 << 13), (0x1 << 13));	/* RG_XO_AUDIO_EN_M */
	pmic_rmw(MT6359_AUDENC_ANA_CON23, (0x1 << 1) | (0x1 << 0), (0x1 << 0)); /* CLKSQ */
	pmic_rmw(MT6359_AUD_TOP_CKPDN_CON0, 0x0066, 0);		/* topck on */
	mdelay(1);						/* >=250us */
	pmic_rmw(MT6359_AUDIO_TOP_CON0, 0x00ff, 0);		/* release digital clocks */
	mdelay(1);						/* >=250us */
	pmic_w(MT6359_AFUNC_AUD_CON2, 0x0006);			/* sdm fifo clk on */
	pmic_w(MT6359_AFUNC_AUD_CON0, 0xcba1);			/* scrambler on */
	pmic_w(MT6359_AFUNC_AUD_CON2, 0x0003);			/* sdm power on */
	pmic_w(MT6359_AFUNC_AUD_CON2, 0x000b);			/* sdm fifo enable */
	pmic_w(MT6359_AFE_NCP_CFG0, 0xc800);
	pmic_w(MT6359_AFE_NCP_CFG0, 0xc801);			/* ncp on */
	pmic_rmw(MT6359_AFE_UL_DL_CON0, 0xc001, 0x0001);	/* afe enable */
	pmic_w(MT6359_AFE_DL_SRC2_CON0_L, 0x0001);		/* codec DL on */
	pmic_w(MT6359_AFE_TOP_CON0, 0x0000);			/* normal path */
	pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x1 << 8), (0x1 << 8)); /* RG_AUDREFN_DERES_EN */

	/* DL analog LDO rails (DL Power Supply chain): without these the DAC and
	 * HP output stages have no supply and produce no analog signal. */
	pmic_rmw(MT6359_AUDDEC_ANA_CON14, (0x1 << 0), (0x1 << 0));	/* LCLDO_DEC_EN */
	pmic_rmw(MT6359_AUDDEC_ANA_CON14, (0x1 << 2), (0x1 << 2));	/* LCLDO_DEC_REMOTE_SENSE */
	pmic_rmw(MT6359_AUDDEC_ANA_CON14, (0x1 << 4), (0x1 << 4));	/* NVREG_EN */
	mdelay(1);							/* NV regulator settle >=100us */
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x1 << 8), 0);		/* IBIST on (clr pwrdn) */
}

/* ramp: HP main output stage 0->7 (both L/R), hp_main_output_ramp(true) */
static void hp_main_output_ramp_up(void)
{
	int i;
	for (i = 0; i <= 7; i++) {
		pmic_rmw(MT6359_AUDDEC_ANA_CON1, (0x7 << 8) | (0x7 << 12),
			 ((unsigned)i << 8) | ((unsigned)i << 12));
		udelay(600);
	}
}

/* ramp: HP aux feedback loop gain 0->0xf, hp_aux_feedback_loop_gain_ramp(true) */
static void hp_aux_fb_gain_ramp_up(void)
{
	int i;
	for (i = 0; i <= 0xf; i++) {
		pmic_rmw(MT6359_AUDDEC_ANA_CON9, (0xf << 12), ((unsigned)i << 12));
		udelay(600);
	}
}

/* ramp headset volume from idx 30 (-22dB) down to idx 8 (0dB), ZCD_CON2 */
static void headset_volume_ramp_to_0db(void)
{
	int idx;
	for (idx = 29; idx >= 8; idx--) {
		pmic_rmw(MT6359_ZCD_CON2, 0x0f9f,
			 ((unsigned)idx << 7) | (unsigned)idx);
		udelay(600);
	}
}

/* pull-down HPL/R to AVSS28 disable: i 7->0, hp_pull_down(false) */
static void hp_pull_down_off(void)
{
#if defined(KONKR_KPA)
	return;
#endif
	int i;
	for (i = 7; i >= 0; i--) {
		pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x7 << 12), ((unsigned)i << 12));
		udelay(120);
	}
}

/* pull-down HPL/R to AVSS28 enable: i 0->7, hp_pull_down(true). Actively clamps
 * the HP outputs to VCM so there is no charge for the amp to discharge (pop). */
static void hp_pull_down_on(void)
{
#if defined(KONKR_KPA)
	return;
#endif
	int i;
	for (i = 0; i <= 7; i++) {
		pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x7 << 12), ((unsigned)i << 12));
		udelay(120);
	}
}

/* ---------- headphone driver power-up (mtk_hp_enable, non-hifi) ---------- */
/*
 * The loudspeaker external class-D amp is wired to the MT6359 headphone
 * outputs (confirmed on the live device: HPL/HPR Mux = "Audio Playback",
 * LOL/RCV Mux = Open). So we bring up the normal headphone path fed from the
 * main DAC, exactly like the kernel mt_hp_event -> mtk_hp_enable.
 */
static void mt6359_hp_on(void)
{
#if defined(KONKR_KPA)
	return;   /* no headphone path on the Konkr: the AW88166 amps on I2S3 are the speakers */
#endif
	mt6359_dl_common_on();

	/* bias (non-hifi): DRBIAS_HP=5uA, IBIAS_ZCD=3uA, IBIAS_HP=4uA */
	pmic_rmw(MT6359_AUDDEC_ANA_CON11, (0x7 << 7), (0x1 << 7));
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x3 << 6), 0);	/* IBIAS_ZCD=3uA */
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x3 << 0), 0);	/* IBIAS_HP=4uA */

	pmic_w(MT6359_AUDDEC_ANA_CON10, 0x0087);		/* HP damp / HPRN-HPLN 4K */
	pmic_w(MT6359_AUDDEC_ANA_CON4, 0x0000);			/* fb cap 15pF (<96k) */
	pmic_w(MT6359_AUDDEC_ANA_CON2, 0xf133);			/* HPP/N STB enhance */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x000c);			/* HP aux output stage */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x003c);			/* HP aux feedback loop */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0c00);			/* HP aux CMFB loop */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30c0);			/* HP driver bias */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30f0);			/* HP driver core */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x00fc);			/* short main->aux */
	/* hp_in_pair_current(true): non-hifi -> no-op */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0e00);			/* HP main CMFB loop */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0200);			/* disable aux CMFB */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x00ff);			/* HP main output stage */
	hp_main_output_ramp_up();
	hp_aux_fb_gain_ramp_up();
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x77cf);			/* disable aux fb loop */
	headset_volume_ramp_to_0db();
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x77c3);			/* disable aux output stage */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x7703);			/* unshort main->aux */
	mdelay(1);						/* >=100us */
	pmic_rmw(MT6359_AUDDEC_ANA_CON13, (0x1 << 0), (0x1 << 0)); /* AUD_CLK / RSTB */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30ff);			/* enable Audio DAC */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0xf200);			/* DAC low-noise off */
	mdelay(1);						/* >=100us */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x32ff);			/* HPL MUX -> audio DAC */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x3aff);			/* HPR MUX -> audio DAC */
	hp_pull_down_off();
}

/* ---------- AFE + memif setup ---------- */
static void afe_dl1_setup(unsigned int phys, unsigned int bytes)
{
	unsigned int con0;

	/* clocks: clear PDN gates (afe / dac / dac_predis / adc) */
	afe_rmw(AUDIO_TOP_CON0,
		(1u << 2) | (1u << 24) | (1u << 25) | (1u << 26), 0);

	/* master AFE on */
	afe_rmw(AFE_DAC_CON0, (1u << 0), (1u << 0));

	/* DL1 memif buffer */
	afe_w(AFE_DL1_BASE_MSB, 0);
	afe_w(AFE_DL1_BASE, phys);
	afe_w(AFE_DL1_CUR, 0);
	afe_w(AFE_DL1_END_MSB, 0);
	afe_w(AFE_DL1_END, phys + bytes - 1);

	/* DL1 format: 48k (mode=10), stereo (mono=0), 16-bit (hd=0), pbuf/minlen */
	con0 = afe_r(AFE_DL1_CON0);
	con0 &= ~((0xfu << 24) | (1u << 8) | (0x3u << 0) | (0x3u << 12) | (0xfu << 20));
	con0 |= (10u << 24) | (3u << 12) | (3u << 20);
	afe_w(AFE_DL1_CON0, con0);

#if defined(KONKR_KPA)
	/* Konkr: the speakers are two AW88166 smart amps on I2S3 (GPIO67 BCK, GPIO68 LRCK, GPIO71
	 * DO, mode 1), not the MT6359 headphone path. Route DL1 to I2S3 and clock the output as
	 * the stock kernel does (I2S format, 48 kHz, 32 bit slots = 64fs BCK, normal jitter mode). */
	mt_set_gpio_mode(GP(67), 1);
	mt_set_gpio_mode(GP(68), 1);
	mt_set_gpio_mode(GP(71), 1);
	afe_rmw(AFE_CONN0, (1u << 5), (1u << 5));	/* I_DL1_CH1 -> O0 */
	afe_rmw(AFE_CONN1, (1u << 6), (1u << 6));	/* I_DL1_CH2 -> O1 */
	afe_rmw(AFE_I2S_CON3, 0xffffeffeu, (10u << 8) | (1u << 3) | (1u << 1));
	afe_rmw(AFE_I2S_CON3, 1u, 1u);			/* I2S3 enable */
	/* start DL1 memif (DL1_ON bit4 of AFE_DAC_CON0) */
	afe_rmw(AFE_DAC_CON0, (1u << 4), (1u << 4));
	{ extern void ayaneo_aw88166_start(void); ayaneo_aw88166_start(); }   /* amps lock to the running BCK */
	return;
#endif
	/* interconnect: DL1 ch1 -> ADDA_DL_CH1 (CONN3), ch2 -> ADDA_DL_CH2 (CONN4) */
	afe_rmw(AFE_CONN3, (1u << 5), (1u << 5));	/* I_DL1_CH1 */
	afe_rmw(AFE_CONN4, (1u << 6), (1u << 6));	/* I_DL1_CH2 */

	/* ADDA downlink SRC: 48k in, x8, mute-off ch1/2, gain-on, SRC on */
	afe_w(AFE_ADDA_PREDIS_CON0, 0);
	afe_w(AFE_ADDA_PREDIS_CON1, 0);
	afe_w(AFE_ADDA_DL_SRC2_CON0, 0x83001803);
	afe_w(AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);
	/* SDM: 2nd order, ATTGAIN normal (0x1d) */
	afe_rmw(AFE_ADDA_DL_SDM_DCCOMP_CON, (0x3fu << 1), (0x1du << 1));
	afe_w(AFE_ADDA_DL_SDM_AUTO_RESET_CON, 0x00000400);
	afe_rmw(AFE_ADDA_DL_SDM_AUTO_RESET_CON, (1u << 31), (1u << 31));
	/* MTKAIF: protocol 2 CLK P2 (this device). CFG0 protocol-2, pad 0x38. */
	afe_w(AFE_ADDA_MTKAIF_CFG0, 0x00010000);
	afe_w(AFE_AUD_PAD_TOP, 0x00000038);
	/* ADDA master on */
	afe_rmw(AFE_ADDA_UL_DL_CON0, (1u << 0), (1u << 0));

	/* start DL1 memif (DL1_ON bit4 of AFE_DAC_CON0) */
	afe_rmw(AFE_DAC_CON0, (1u << 4), (1u << 4));
}

static void afe_dl1_stop(void)
{
#if defined(KONKR_KPA)
	{ extern void ayaneo_aw88166_stop(void); ayaneo_aw88166_stop(); }   /* mute + power down before the clocks stop */
	afe_rmw(AFE_DAC_CON0, (1u << 4), 0);		/* DL1_ON = 0 */
	afe_rmw(AFE_I2S_CON3, 1u, 0);			/* I2S3 off */
	return;
#endif
	afe_rmw(AFE_DAC_CON0, (1u << 4), 0);		/* DL1_ON = 0 */
	afe_rmw(AFE_ADDA_DL_SRC2_CON0, (1u << 0), 0);	/* DL_2_SRC_ON = 0 */
	afe_rmw(AFE_ADDA_UL_DL_CON0, (1u << 0), 0);	/* ADDA_AFE_ON = 0 */
}

static void spk_amp_enable(int on)
{
#ifdef KONKR_KPA
	/* On the Konkr GPIO24/GPIO21 are the RESET lines of the two AW883xx smart amps (i2c6 0x34 /
	 * 0x35, fed over I2S3), not enables for a dumb class-D stage. Toggling them here would drop
	 * the amps in and out of reset with nothing configured behind them. Leave them alone until
	 * the AW883xx bring-up exists; the MT6359 headphone path below plays into nothing. */
	(void)on;
	return;
#endif
	mt_set_gpio_mode(AYANEO_SPK_EN_GPIO, 0);
	mt_set_gpio_dir(AYANEO_SPK_EN_GPIO, 1);
	mt_set_gpio_out(AYANEO_SPK_EN_GPIO, on ? 1 : 0);
	mt_set_gpio_mode(AYANEO_SPK2_EN_GPIO, 0);
	mt_set_gpio_dir(AYANEO_SPK2_EN_GPIO, 1);
	mt_set_gpio_out(AYANEO_SPK2_EN_GPIO, on ? 1 : 0);
}

/*
 * Smoothly ramp the HP output gain down to mute (ZCD_CON2) so the DAC and amp
 * can be cut without a click. from idx 8 (0dB) up to 0x1f (-40dB, mute).
 */
static void mt6359_hp_mute_ramp(void)
{
#if defined(KONKR_KPA)
	return;
#endif
	int idx;

	for (idx = 9; idx <= 0x1f; idx++) {
		if (idx <= 30 || idx == 0x1f)
			pmic_rmw(MT6359_ZCD_CON2, 0x0f9f,
				 ((unsigned)idx << 7) | (unsigned)idx);
		udelay(600);
	}
}

/*
 * Scale the decoded PCM by the configured volume (0-100%). Linear, in place,
 * on the 16-bit stereo buffer. vol>=100 is a no-op (leaves it full-scale).
 */
static void apply_volume(unsigned int pcm_bytes, unsigned int vol)
{
	short *s = (short *)s_pcm;
	unsigned int n = pcm_bytes / 2;		/* samples (both channels) */
	unsigned int i, q;

	if (vol >= 100)
		return;
	q = vol * 256u / 100u;			/* 8.8 fixed-point gain */
	for (i = 0; i < n; i++)
		s[i] = (short)((s[i] * (int)q) >> 8);
}

/*
 * Fade the tail of the clip to zero so the waveform ends at silence: this
 * removes the DC step / click that a hard DMA stop on a non-zero sample would
 * make. In-place on the 16-bit stereo buffer.
 */
static void apply_tail_fade(unsigned int pcm_bytes, unsigned int rate)
{
	short *s = (short *)s_pcm;
	unsigned int frames = pcm_bytes / 4;		/* stereo, 16-bit */
	unsigned int fade = rate * AYANEO_AUDIO_FADE_MS / 1000;
	unsigned int i, div;

	if (fade == 0 || fade > frames)
		fade = frames;
	div = (fade > 1) ? (fade - 1) : 1;
	for (i = 0; i < fade; i++) {
		unsigned int f = frames - fade + i;
		int sc = (int)(((fade - 1 - i) * 256) / div);	/* 256 -> 0 */

		s[f * 2 + 0] = (short)((s[f * 2 + 0] * sc) >> 8);
		s[f * 2 + 1] = (short)((s[f * 2 + 1] * sc) >> 8);
	}
}

/* pop/hiss-free shutdown. */
static void audio_shutdown(void)
{
	/*
	 * The ring just wrapped, but the codec pipeline (AFE FIFO -> SRC -> MTKAIF
	 * -> DAC) still holds the last few ms of the clip. If we cut the amp now it
	 * is mid-transient and pops. So FIRST ramp the HP gain to mute while the
	 * amp is still on (a smooth, inaudible taper), then let the pipeline fully
	 * drain to silence so the HP output settles at VCM.
	 */
	mt6359_hp_mute_ramp();
	mdelay(30);			/* drain the codec pipeline to VCM */

	/* actively clamp the HP outputs to VCM so the amp has no charge to dump */
	hp_pull_down_on();

	/* HP output is now silent, settled and clamped: cut the amp (quietest). */
	spk_amp_enable(0);
	mdelay(10);			/* let the amp fully discharge */

	/* inaudible now (amp off): stop the DMA/ADDA and park the DAC off so the
	 * codec cannot idle-hiss before the kernel re-inits it. */
	afe_dl1_stop();
	pmic_rmw(MT6359_AUDDEC_ANA_CON9, 0x0001, 0);	/* DAC low-noise off */
	pmic_rmw(MT6359_AUDDEC_ANA_CON0, 0x000f, 0);	/* disable audio DAC */
#ifdef AYANEO_DEBUG_LOGGING
	dprintf(CRITICAL, "AYANEO_AUD: shutdown done t=%u\n",
		(unsigned int)current_time());
#endif
}

/* 3.5mm headphone jack: detection + speaker/headphone routing (defined further down,
 * next to the streaming-audio state it shares). Declared here so the bring-up sites
 * above (the boot chime, in non-SD builds) can call the init. */
void jack_detect_init(void);
void jack_poll(void);

/* The boot chime (a clip in boot_b, played with the old boot animation) is retired: the GBA BIOS
 * boot from the card is the intro, and the SD build reads nothing from boot_b. */
#if !defined(AYANEO_GBA_SD)
/*
 * The whole audio life-cycle on its own thread so it never stalls the boot
 * animation: load + decode the clip, wait the start delay, bring up the codec,
 * play exactly once, then shut down cleanly.
 */
static int ayaneo_audio_thread(void *arg)
{
	unsigned char hdr[AYANEO_AUDIO_HDR];
	unsigned int magic, rate, pcm_bytes, ch, bits, clen;
	unsigned long zlen;
	unsigned char *comp;
	unsigned int t0;
	ssize_t got;

	t0 = (unsigned int)current_time();

	got = partition_read(AYANEO_AUDIO_PART, AYANEO_AUDIO_OFF, hdr, AYANEO_AUDIO_HDR);
	if (got != (ssize_t)AYANEO_AUDIO_HDR)
		goto done;

	magic     = rd32(hdr + 0);
	rate      = rd32(hdr + 8);
	pcm_bytes = rd32(hdr + 12);
	ch        = rd16(hdr + 16);
	bits      = rd16(hdr + 18);
	clen      = rd32(hdr + 20);

	if (magic != AYANEO_AUDIO_MAGIC || pcm_bytes == 0 ||
	    pcm_bytes > AYANEO_PCM_MAX || clen == 0 || clen > AYANEO_AUDIO_COMPMAX ||
	    clen * 2 > AYANEO_PCM_MAX ||	/* keep the tail scratch safe (see below) */
	    ch != 2 || bits != 16 || rate == 0) {
#ifdef AYANEO_DEBUG_LOGGING
		dprintf(CRITICAL, "AYANEO_AUD: bad blob magic=0x%x rate=%u pcm=%u clen=%u ch=%u bits=%u\n",
			magic, rate, pcm_bytes, clen, ch, bits);
#endif
		goto done;
	}

	/*
	 * Scratch for the compressed payload lives at the tail of s_pcm (no extra
	 * memory - LK's heap is tight and a separate buffer starves the secure-
	 * image verify). Place it at PCM_MAX-clen: decode writes forward from 0
	 * and reads forward from there; because the data compresses (clen*2 <=
	 * PCM_MAX, checked above) the read pointer always stays ahead of the write
	 * pointer, so zunzip never clobbers unread bytes (which corrupted the tail
	 * into static before).
	 */
	comp = (pcm_bytes + clen <= AYANEO_PCM_MAX)
	     ? s_pcm + pcm_bytes			/* no overlap at all */
	     : s_pcm + AYANEO_PCM_MAX - clen;		/* safe tail overlap */
	got = partition_read(AYANEO_AUDIO_PART, AYANEO_AUDIO_OFF + AYANEO_AUDIO_HDR,
			     comp, clen);
	if (got != (ssize_t)clen)
		goto done;

	zlen = clen;
	if (zunzip(comp, &zlen, s_pcm, (int)pcm_bytes, 0) != 0)
		goto done;

	/* use the persisted volume so the chime matches the level set in-game */
	ayaneo_settings_load();
	if (s_gbc_vol == 0)			/* silent: skip playback entirely */
		goto done;
	apply_volume(pcm_bytes, (unsigned int)s_gbc_vol);
	apply_tail_fade(pcm_bytes, rate);
	arch_clean_cache_range((addr_t)s_pcm, pcm_bytes);

	/* exact clip duration -> play once (no replay of the ring buffer) */
	s_audio_ms = (unsigned int)(((unsigned long long)(pcm_bytes / 4)
				    * 1000ull) / rate);

	/* hold off the requested start delay (measured from thread start) */
	while (!s_audio_stop_req &&
	       (unsigned int)current_time() - t0 < AYANEO_AUDIO_DELAY_MS)
		thread_sleep(20);
	if (s_audio_stop_req)
		goto done;

	/* power up: audio MTCMOS domain, codec analog, then AFE memif */
	spm_mtcmos_ctrl_audio(STA_POWER_ON);
	mt6359_hp_on();
	afe_dl1_setup((unsigned int)(addr_t)s_pcm, pcm_bytes);
	mdelay(2);			/* let the HP output settle before the amp */
	spk_amp_enable(1);
	jack_detect_init();		/* bring the accdet comparator up once the path is live */
	ATRACE("AYANEO_AUD: playing rate=%u pcm=%u ms=%u aud_on=%d VAUD18=0x%x CON14=0x%x CON0(rb)=0x%x DAC_CON0=0x%x CON9=0x%x amp_out=%d\n",
		rate, pcm_bytes, s_audio_ms,
		!!(DRV_Reg32(PWR_STATUS) & AUDIO_PWR_STA_MASK),
		pmic_r(MT6359_LDO_VAUD18_CON0), pmic_r(MT6359_AUDDEC_ANA_CON14),
		pmic_r(MT6359_AUDDEC_ANA_CON0), afe_r(AFE_DAC_CON0),
		pmic_r(MT6359_AUDDEC_ANA_CON9), mt_get_gpio_out(0x80000000u | 24));

	/*
	 * Play exactly once. The memif is a hardware ring, so watch the DMA read
	 * pointer (AFE_DL1_CUR): the instant it wraps back toward the buffer start
	 * (cur decreases) one full pass is done - stop immediately so it never
	 * repeats. This is position-based, so it cannot drift or be fooled by
	 * scheduling latency the way a time estimate could.
	 */
	{
		/*
		 * Play exactly once. The memif is a hardware ring; watching the DMA read
		 * pointer (AFE_DL1_CUR) wrap tells us one full pass is done. But the very
		 * first sample of prev must not be taken before the DMA has actually
		 * started moving: in the release build there is no logging delay between
		 * afe_dl1_setup/amp-enable and this read, so prev can be sampled at t=0
		 * and the next read already reads lower, faking a wrap and cutting the
		 * chime to silence. Two guards make this timing-independent:
		 *   1) only accept a wrap once at least half the clip has elapsed, and
		 *   2) a hard backstop at the exact clip length + margin, so we always
		 *      stop after one pass even if the wrap edge is missed.
		 * s_audio_ms is the exact clip duration (from the sample count).
		 */
		unsigned int start_ms = (unsigned int)current_time();
		unsigned int prev = afe_r(AFE_DL1_CUR);
		int seen_progress = 0;
		ATRACE("AYANEO_AUD: chime play begin cur=0x%x ms=%u t=%u\n",
			prev, s_audio_ms, start_ms);
		while (!s_audio_stop_req) {
			unsigned int cur = afe_r(AFE_DL1_CUR);
			unsigned int elapsed = (unsigned int)current_time() - start_ms;

			if (cur != prev)
				seen_progress = 1;
			/* wrap, but only once we are past the midpoint of the clip */
			if (seen_progress && cur < prev && elapsed * 2u >= s_audio_ms)
				break;
			/* backstop: never exceed one clip length (+ small margin) */
			if (elapsed >= s_audio_ms + 40u)
				break;
			prev = cur;
			thread_sleep(4);
		}
		ATRACE("AYANEO_AUD: chime done stop_req=%d last=0x%x t=%u\n",
			s_audio_stop_req, prev, (unsigned)current_time());
	}

	audio_shutdown();
done:
	s_audio_active = 0;
	return 0;
}

#endif /* !AYANEO_GBA_SD */

/*
 * Public entry: spawn the audio thread. Returns immediately so the boot
 * animation is never blocked. Safe to call once, after the display is up.
 */
void ayaneo_boot_audio_start(void)
{
#if defined(AYANEO_GBA_SD)
	return;				/* no boot chime (see ayaneo_audio_thread) */
#else
	thread_t *t;

	if (s_mute_bios)		/* user muted the boot chime (persisted setting) */
		return;

#ifdef AYANEO_AUDIO_TRACE
	/* diagnostic: force LK's UART console on even in a release build so the
	 * chime bring-up register dump reaches the wire (uart_putc gates on both). */
	g_boot_arg->log_enable = 1;
	g_boot_arg->meta_log_disable = 0;
#endif
	if (s_audio_active)
		return;
	s_audio_stop_req = 0;
	s_audio_active = 1;
	/*
	 * HIGH_PRIORITY so the teardown (ring-wrap detection + mute/amp-off) runs
	 * promptly even while the HIGH_PRIORITY animation is busy; otherwise the
	 * thread gets starved, the ring buffer loops, and the clip audibly repeats.
	 */
	t = thread_create("ayaneo_audio", &ayaneo_audio_thread, NULL,
			  HIGH_PRIORITY, DEFAULT_STACK_SIZE);
	if (t)
		thread_resume(t);
	else
		s_audio_active = 0;
	ATRACE("AYANEO_AUD: boot chime start spawned=%d t=%u\n",
		!!t, (unsigned)current_time());
#endif
}

void ayaneo_boot_audio_stop(void)
{
	int guard = 0;

	if (!s_audio_active)
		return;
	/* ask the thread to stop and let it run its pop-free shutdown (bounded) */
	s_audio_stop_req = 1;
	while (s_audio_active && guard++ < 400)
		thread_sleep(5);
}

/* the board's hardware behind the shared ring and streams */
#define PICO_AUD_DMA_CUR()		afe_r(AFE_DL1_CUR)
#define PICO_AUD_HW_ON(pa, bytes)	do { spm_mtcmos_ctrl_audio(STA_POWER_ON); mt6359_hp_on(); afe_dl1_setup((pa), (bytes)); mdelay(2); spk_amp_enable(1); } while (0)
#include "../common/pico/pico_audio_ring.inc"


/* ================= 3.5mm headphone jack: detection + routing =================
 *
 * The Konkr plays game audio out two AW88166 smart amps on I2S3 (see afe_dl1_setup).
 * When a headset is plugged into the 3.5mm jack the stock path instead feeds the
 * MT6359 internal headphone driver from the DL1 -> ADDA downlink. There is no GPIO
 * or analog switch: the amps are simply muted/stopped and the codec HP path is
 * brought up (and the reverse on unplug). We detect the plug with the MT6359 ACCDET
 * EINT0 comparator, polled (no interrupts) on ACCDET_CON26 EINT0_MEM_IN, and switch
 * the route without ever stopping the DL1 memif the game ring depends on - only the
 * interconnect and the codec move, so the DMA read pointer never glitches.
 *
 * Everything here runs behind one small state machine and changes nothing until a
 * headset is actually detected. The register sequences are written from the
 * project's own clean-room note (notes/hpjack_findings.md); the note's section is
 * cited at each write.
 */

extern unsigned gpt4_get_current_tick(void);
extern S32 mt_set_gpio_pull_enable(u32 pin, u32 en);
extern S32 mt_set_gpio_pull_select(u32 pin, u32 sel);

/* HP analog volume (ZCD_CON2 index): idx0 = +8dB, 1dB/step, idx9 = -1dB (stock 100%
 * ceiling), idx30 = -22dB, idx31 = mute. We never go above idx9 and keep a quiet
 * floor for earbuds; the user's 0..100 software volume maps onto this range (note 3). */
#define JACK_HP_IDX_CEIL	9	/* loudest we allow: stock 100% = -1 dB */
#define JACK_HP_IDX_FLOOR	30	/* quietest active step: -22 dB */
#define JACK_HP_IDX_MUTE	0x1f	/* -40 dB, mute */
#define JACK_ZCD_MASK		0x0f9f	/* HPR [11:7] | HPL [4:0] */

/* throttle: poll the comparator no more than ~every 100 ms (13 MHz tick counter). */
#define JACK_POLL_TICKS		1300000u

static volatile int s_jack_inited;		/* accdet comparator has been set up */
static volatile int s_jack_route_picked;	/* the boot-time route has been chosen */
static volatile int s_jack_last = -1;		/* debounced plug: -1 unknown, 0 out, 1 in */
static volatile int s_jack_route;		/* active route: 0 speaker, 1 headphone */
static volatile int s_jack_hp_cur_idx = JACK_HP_IDX_FLOOR;	/* live HP analog index */
static unsigned s_jack_poll_tick;

/* diag externs the oem jack command reads (menu_fastboot.c) */
volatile int g_jack_route;			/* 0 speaker, 1 headphone */
volatile int g_jack_plugged = -1;		/* debounced: -1 unknown, 0 out, 1 in */

/* raw reads for the diag command (pmic_r is file-static) */
unsigned ayaneo_jack_con26(void)   { return pmic_r(MT6359_ACCDET_CON26); }
int ayaneo_jack_mem_in(void)       { return (int)((pmic_r(MT6359_ACCDET_CON26) >> 6) & 0x3); }
int ayaneo_jack_plugged_state(void){ return s_jack_last; }
int ayaneo_jack_route_state(void)  { return s_jack_route; }
int ayaneo_jack_pole(void)         { return (int)((pmic_r(MT6359_ACCDET_CON25) >> 6) & 0x3); }

/* one EINT0_MEM_IN read: 0 = plugged, 3 = unplugged, 1/2 = no-change code. */
static int jack_mem_in(void)
{
	return (int)((pmic_r(MT6359_ACCDET_CON26) >> 6) & 0x3);
}

/*
 * Debounced plug state. Requires two equal reads; keeps the last known state in a
 * static and returns it on a transitional (1/2) or unsettled read. -1 until the
 * first definite reading. 1 = headset in, 0 = out.
 */
int jack_plugged(void)
{
	int a = jack_mem_in(), b;
	udelay(50);
	b = jack_mem_in();
	if (a != b)
		return s_jack_last;		/* not two equal reads: keep last known */
	if (a == 0)
		s_jack_last = 1;
	else if (a == 3)
		s_jack_last = 0;
	/* a == 1 or 2: no-change code, keep last */
	return s_jack_last;
}

/* map the 0..100 software volume onto the HP analog index, idx9 as the ceiling. */
static int jack_hp_idx_for_vol(int vol)
{
	int idx;
	if (vol < 0) vol = 0;
	if (vol > 100) vol = 100;
	idx = JACK_HP_IDX_CEIL + (100 - vol) * (JACK_HP_IDX_FLOOR - JACK_HP_IDX_CEIL) / 100;
	if (idx < JACK_HP_IDX_CEIL) idx = JACK_HP_IDX_CEIL;
	if (idx > JACK_HP_IDX_FLOOR) idx = JACK_HP_IDX_FLOOR;
	return idx;
}

/* HP output pull-down to VCM: on clamps (0 -> 7), off releases (7 -> 0). Konkr's
 * copy; the file's hp_pull_down_* early-return on Konkr so the jack path has its own. */
static void jack_hp_pull_down(int on)
{
	int i;
	if (on)
		for (i = 0; i <= 7; i++) {
			pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x7 << 12), ((unsigned)i << 12));
			udelay(120);
		}
	else
		for (i = 7; i >= 0; i--) {
			pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x7 << 12), ((unsigned)i << 12));
			udelay(120);
		}
}

/* ramp the HP volume from the current index towards a target, one step / 600us. */
static void jack_hp_vol_ramp_to(int target)
{
	if (target < 0) target = 0;
	if (target > JACK_HP_IDX_MUTE) target = JACK_HP_IDX_MUTE;
	while (s_jack_hp_cur_idx != target) {
		s_jack_hp_cur_idx += (target > s_jack_hp_cur_idx) ? 1 : -1;
		pmic_rmw(MT6359_ZCD_CON2, JACK_ZCD_MASK,
			 ((unsigned)s_jack_hp_cur_idx << 7) | (unsigned)s_jack_hp_cur_idx);
		udelay(600);
	}
}

/*
 * Bring the MT6359 headphone driver up from the main DAC, ramped so there is no
 * click (note: "MT6359 HP power up"). Adapted from the file's non-Konkr mt6359_hp_on
 * body: same codec front-end + driver writes, but it starts with the volume preset
 * at -22 dB (idx30) and ramps up to the level mapped from the software volume.
 */
static void jack_hp_codec_up(void)
{
	int target = jack_hp_idx_for_vol(s_gbc_vol);

	mt6359_dl_common_on();			/* supplies, clocks, NCP, AFE DL, DL LDO rails */

	/* preset the HP volume at the quiet floor before the driver comes up (note 3). */
	s_jack_hp_cur_idx = JACK_HP_IDX_FLOOR;
	pmic_w(MT6359_ZCD_CON2,
	       ((unsigned)JACK_HP_IDX_FLOOR << 7) | (unsigned)JACK_HP_IDX_FLOOR);

	/* bias (non-hifi): DRBIAS_HP = 5uA, IBIAS_ZCD = 3uA, IBIAS_HP = 4uA */
	pmic_rmw(MT6359_AUDDEC_ANA_CON11, (0x7 << 7), (0x1 << 7));
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x3 << 6), 0);
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x3 << 0), 0);

	pmic_w(MT6359_AUDDEC_ANA_CON10, 0x0087);	/* HP damp / HPRN-HPLN 4K */
	pmic_w(MT6359_AUDDEC_ANA_CON4, 0x0000);		/* fb cap 15pF (<96k) */
	pmic_w(MT6359_AUDDEC_ANA_CON2, 0xf133);		/* HPP/N STB enhance, pull-down armed */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x000c);		/* HP aux output stage */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x003c);		/* HP aux feedback loop */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0c00);		/* HP aux CMFB loop */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30c0);		/* HP driver bias */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30f0);		/* HP driver core */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x00fc);		/* short main -> aux */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0e00);		/* HP main CMFB loop */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0200);		/* disable aux CMFB */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x00ff);		/* HP main output stage */
	hp_main_output_ramp_up();
	hp_aux_fb_gain_ramp_up();
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x77cf);		/* disable aux fb loop */
	jack_hp_vol_ramp_to(target);			/* -22 dB -> mapped level (click-safe) */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x77c3);		/* disable aux output stage */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x7703);		/* unshort main -> aux */
	mdelay(1);
	pmic_rmw(MT6359_AUDDEC_ANA_CON13, (0x1 << 0), (0x1 << 0));	/* AUD_CLK / RSTB */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30ff);		/* enable audio DAC */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0xf200);		/* DAC low-noise off */
	mdelay(1);
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x32ff);		/* HPL MUX -> audio DAC */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x3aff);		/* HPR MUX -> audio DAC */
	jack_hp_pull_down(0);				/* release the output pull-down */
}

/*
 * Tear the HP driver down in the note's power-down order: gain ramp to mute while
 * the driver is still live, clamp to VCM, then open the mux, cut the DAC/driver,
 * drop the ADDA front-end and the supplies. Leaves the codec cold so it cannot
 * idle-hiss and so the speaker path comes back clean.
 */
static void jack_hp_codec_down(void)
{
	jack_hp_vol_ramp_to(JACK_HP_IDX_MUTE);	/* mute first, no click */
	mdelay(10);				/* let the output settle to VCM */
	jack_hp_pull_down(1);			/* clamp HP outputs to VCM */

	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x30ff);	/* HPL/HPR MUX -> open (off the DAC) */
	pmic_w(MT6359_AUDDEC_ANA_CON1, 0x0000);	/* HP output/aux stages off */
	pmic_w(MT6359_AUDDEC_ANA_CON0, 0x0000);	/* HP driver + audio DAC off */
	pmic_w(MT6359_AUDDEC_ANA_CON9, 0x0000);	/* CMFB loops off */

	/* ADDA downlink + codec DL off (reverse of mt6359_dl_common_on's DL enables) */
	afe_rmw(AFE_ADDA_DL_SRC2_CON0, (1u << 0), 0);
	afe_rmw(AFE_ADDA_UL_DL_CON0, (1u << 0), 0);
	pmic_w(MT6359_AFE_DL_SRC2_CON0_L, 0x0000);
	pmic_rmw(MT6359_AFE_UL_DL_CON0, 0x0001, 0);

	/* supplies down */
	pmic_rmw(MT6359_AUDDEC_ANA_CON2, (0x1 << 8), 0);			/* AUDREFN_DERES off */
	pmic_rmw(MT6359_AUDDEC_ANA_CON14, (0x1 << 4) | (0x1 << 2) | (0x1 << 0), 0); /* NVREG/LCLDO off */
	pmic_rmw(MT6359_AUDDEC_ANA_CON12, (0x1 << 8), (0x1 << 8));	/* IBIST power down */
	pmic_rmw(MT6359_AUDDEC_ANA_CON13, (0x1 << 4), (0x1 << 4));	/* RG_AUDGLB_PWRDN */
	pmic_rmw(MT6359_DCXO_CW12, (0x1 << 13), 0);			/* XO_AUDIO off */
	pmic_rmw(MT6359_LDO_VAUD18_CON0, (0x1 << 0), 0);		/* VAUD18 off */
}

/*
 * Switch the live route. The DL1 memif (AFE_DAC_CON0 bit4) is never touched, so the
 * DMA read pointer the game ring tracks never jumps - only the interconnect, the I2S3
 * engine and the codec move. Amps are muted/off before, and the HP driver comes up
 * ramped, so the switch is click-safe.
 */
void jack_route(int to_headphones)
{
	if (to_headphones == s_jack_route)
		return;

	if (to_headphones) {
		/* speaker -> headphone */
		{ extern void ayaneo_aw88166_stop(void); ayaneo_aw88166_stop(); }	/* mute + stop the amps */
		afe_rmw(AFE_CONN0, (1u << 5), 0);	/* drop DL1ch1 -> O0 (I2S3) */
		afe_rmw(AFE_CONN1, (1u << 6), 0);	/* drop DL1ch2 -> O1 (I2S3) */
		afe_rmw(AFE_I2S_CON3, 1u, 0);		/* I2S3 engine off */

		/* AP serial-audio data pins to the codec (note 2): mode1 on. 189/190 are
		 * the clk/ws of the same link and should already be mode1 from boot. */
		mt_set_gpio_mode(GP(189), 1);
		mt_set_gpio_mode(GP(190), 1);
		mt_set_gpio_mode(GP(191), 1);
		mt_set_gpio_mode(GP(192), 1);

		/* interconnect DL1 -> ADDA downlink (note 2) */
		afe_rmw(AFE_CONN3, (1u << 5), (1u << 5));	/* DL1ch1 -> O3 (ADDA_DL_CH1) */
		afe_rmw(AFE_CONN4, (1u << 6), (1u << 6));	/* DL1ch2 -> O4 (ADDA_DL_CH2) */

		/* ADDA downlink SRC @48k. DL_SRC2 enable (bit0) comes last, after ADDA
		 * master, per the note; and the SDM attenuation-gain is [5:0] here, not the
		 * [6:1] the file's non-Konkr branch writes (note: that is a bug). */
		afe_w(AFE_ADDA_PREDIS_CON0, 0);
		afe_w(AFE_ADDA_PREDIS_CON1, 0);
		afe_w(AFE_ADDA_DL_SRC2_CON0, 0x83001802);	/* configured, DL_2_SRC_ON still 0 */
		afe_w(AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);
		afe_rmw(AFE_ADDA_DL_SDM_DCCOMP_CON, 0x3fu, 0x1du);	/* ATTGAIN [5:0] = 0x1d */
		afe_w(AFE_ADDA_DL_SDM_AUTO_RESET_CON, 0x00000400);
		afe_rmw(AFE_ADDA_DL_SDM_AUTO_RESET_CON, (1u << 31), (1u << 31));
		afe_w(AFE_ADDA_MTKAIF_CFG0, 0x00010000);
		afe_w(AFE_AUD_PAD_TOP, 0x00000038);
		afe_rmw(AFE_ADDA_UL_DL_CON0, (1u << 0), (1u << 0));	/* ADDA master on */
		afe_rmw(AFE_ADDA_DL_SRC2_CON0, (1u << 0), (1u << 0));	/* DL_2_SRC_ON -> 0x83001803 */

		jack_hp_codec_up();			/* MT6359 HP driver, ramped */
	} else {
		/* headphone -> speaker */
		jack_hp_codec_down();			/* tear the HP codec down cleanly */

		/* idle the AP data pins: mode0 + pulldown (note 2) */
		mt_set_gpio_mode(GP(191), 0);
		mt_set_gpio_mode(GP(192), 0);
		mt_set_gpio_pull_enable(GP(191), 1); mt_set_gpio_pull_select(GP(191), 0);
		mt_set_gpio_pull_enable(GP(192), 1); mt_set_gpio_pull_select(GP(192), 0);

		afe_rmw(AFE_CONN3, (1u << 5), 0);	/* drop DL1ch1 -> O3 (ADDA) */
		afe_rmw(AFE_CONN4, (1u << 6), 0);	/* drop DL1ch2 -> O4 (ADDA) */

		/* restore the I2S3 speaker route exactly as afe_dl1_setup does today */
		mt_set_gpio_mode(GP(67), 1);
		mt_set_gpio_mode(GP(68), 1);
		mt_set_gpio_mode(GP(71), 1);
		afe_rmw(AFE_CONN0, (1u << 5), (1u << 5));	/* DL1ch1 -> O0 */
		afe_rmw(AFE_CONN1, (1u << 6), (1u << 6));	/* DL1ch2 -> O1 */
		afe_rmw(AFE_I2S_CON3, 0xffffeffeu, (10u << 8) | (1u << 3) | (1u << 1));
		afe_rmw(AFE_I2S_CON3, 1u, 1u);			/* I2S3 engine on */
		{ extern void ayaneo_aw88166_start(void); ayaneo_aw88166_start(); }	/* amps lock to the BCK */
	}

	s_jack_route = to_headphones;
	g_jack_route = to_headphones;
}

/* accdet comparator-state housekeeping on a confirmed plug change (note lines 19-21).
 * Keeps Android's accdet in step and clears the EINT latch. */
static void jack_eint_after_change(int plugged)
{
	if (plugged) {
		/* plug-in */
		pmic_rmw(MT6359_ACCDET_CON1, (1u << 6), 0);
		pmic_rmw(MT6359_ACCDET_CON19, (1u << 5), 0);
		pmic_rmw(MT6359_AUDENC_ANA_CON18, (1u << 11), (1u << 11));
	} else {
		/* plug-out */
		pmic_rmw(MT6359_ACCDET_CON14, 0xfu, 5u);
		pmic_rmw(MT6359_AUDENC_ANA_CON18, (1u << 11) | (1u << 12), 0);
		pmic_rmw(MT6359_ACCDET_CON1, (1u << 10), 0);
		pmic_rmw(MT6359_ACCDET_CON19, (1u << 5), (1u << 5));
		pmic_rmw(MT6359_ACCDET_CON1, (1u << 6), (1u << 6));
	}
	/* clear the EINT latch: pulse CON18 bit10, wait for bit2 to fall, write-1-clear int0 */
	pmic_rmw(MT6359_ACCDET_CON18, (1u << 10), (1u << 10));
	{ int g = 0; while (((pmic_r(MT6359_ACCDET_CON18) >> 2) & 1) && g++ < 100) udelay(4); }
	pmic_rmw(MT6359_ACCDET_CON18, (1u << 10), 0);
	pmic_w(MT6359_AUD_TOP_INT_STATUS0, (1u << 6));
}

/* ACCDET EINT0 comparator bring-up for plug detection (note section 1, steps
 * 1,3,4,5,7,8,9). Detection only; it does not disturb the running speaker path. */
static void jack_accdet_init(void)
{
	/* 1: sequence init - toggle CON1 bit1 */
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 1), (1u << 1));
	mdelay(2);
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 1), 0);
	mdelay(1);

	/* 3: EINT debounce times */
	pmic_rmw(MT6359_ACCDET_CON14, (0xfu << 4), (3u << 4));
	pmic_rmw(MT6359_ACCDET_CON14, (0xfu << 0) | (0xfu << 12), (5u << 0) | (5u << 12));
	pmic_w(MT6359_ACCDET_CON15, 0x000e);

	/* 4: pulse the accdet digital reset (must keep CON14) */
	pmic_rmw(MT6359_AUD_TOP_RST_CON0, (1u << 1), (1u << 1));
	pmic_rmw(MT6359_AUD_TOP_RST_CON0, (1u << 1), 0);

	/* 5: MICBIAS1 ~2.8 V so Android's accdet mic state matches */
	pmic_rmw(MT6359_AUDENC_ANA_CON16, (0x3u << 12) | (0x7u << 4), 0);
	pmic_rmw(MT6359_AUDENC_ANA_CON16,
		 (1u << 12) | (1u << 13) | (1u << 2), (1u << 12) | (1u << 13) | (1u << 2));

	/* 7: comparator mic mode 1 */
	pmic_rmw(MT6359_AUDENC_ANA_CON18, 0x000f, 0x000f);
	pmic_rmw(MT6359_AUDENC_ANA_CON19, (1u << 12), (1u << 12));
	pmic_rmw(MT6359_AUDENC_ANA_CON21, (0x3u << 11), (0x3u << 11));

	/* 8: EINT analog comparator path */
	pmic_rmw(MT6359_AUDENC_ANA_CON20, (1u << 2) | (1u << 8), (1u << 2) | (1u << 8));
	pmic_rmw(MT6359_AFE_NCP_CFG2, 0x3u, 0x3u);

	/* 9: EINT digital config, mode 4 (EINT 2.1), moisture off */
	pmic_w(MT6359_ACCDET_CON6, 0x0041);
	pmic_w(MT6359_ACCDET_CON19, 0x0027);
	pmic_rmw(MT6359_ACCDET_CON18, (1u << 12), (1u << 12));
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 12), (1u << 12));
	pmic_w(MT6359_ACCDET_CON20, 0x0100);
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 12), 0);
	pmic_w(MT6359_ACCDET_CON2, 0x0067);
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 6), (1u << 6));	/* ACCDET_SW_EN / EINT enable */
}

/*
 * Set the accdet comparator up once (idempotent), and once the game audio ring is
 * live pick the boot-time route from the first debounced read, so a console booted
 * with a headset already in comes up on headphones. Called from the bring-up sites.
 */
void jack_detect_init(void)
{
	if (!s_jack_inited) {
		jack_accdet_init();
		s_jack_inited = 1;
		s_jack_poll_tick = gpt4_get_current_tick();
	}
	if (s_gbc_audio_on && !s_jack_route_picked) {
		int i, p = -1;
		/* the comparator needs settling after enable (note: >=300 ms); exit as soon
		 * as a definite read lands so a warm comparator costs only one step */
		for (i = 0; i < 30; i++) {
			mdelay(10);
			p = jack_plugged();
			if (p >= 0)
				break;
		}
		g_jack_plugged = p;
		s_jack_route_picked = 1;
		if (p == 1)
			jack_route(1);
	}
}

/*
 * Throttled poll hook from the audio submit path. Cheap when nothing changed (one
 * comparator read every ~100 ms). On a confirmed plug change it does the accdet
 * housekeeping and switches the route; while on headphones it also eases the analog
 * gain one step towards the level mapped from the current software volume.
 */
void jack_poll(void)
{
	unsigned now;
	int p;

	if (!s_jack_inited || !s_gbc_audio_on)
		return;
	now = gpt4_get_current_tick();
	if ((unsigned)(now - s_jack_poll_tick) < JACK_POLL_TICKS)
		return;
	s_jack_poll_tick = now;

	p = jack_plugged();
	g_jack_plugged = p;
	if (p >= 0 && p != s_jack_route) {
		jack_eint_after_change(p);
		jack_route(p);
		return;
	}
	if (s_jack_route == 1) {
		int want = jack_hp_idx_for_vol(s_gbc_vol);
		if (want != s_jack_hp_cur_idx) {
			s_jack_hp_cur_idx += (want > s_jack_hp_cur_idx) ? 1 : -1;
			pmic_rmw(MT6359_ZCD_CON2, JACK_ZCD_MASK,
				 ((unsigned)s_jack_hp_cur_idx << 7) | (unsigned)s_jack_hp_cur_idx);
		}
	}
}

/*
 * Leave the accdet and HP analog registers in the clean state the kernel's accdet
 * re-init expects (note line 23), so the Android handoff is not confused. If we are
 * on the headphone route, tear the HP codec down first.
 */
static void jack_teardown_for_android(void)
{
	if (!s_jack_inited)
		return;
	if (s_jack_route == 1) {
		jack_hp_codec_down();
		s_jack_route = 0;
		g_jack_route = 0;
	}
	pmic_rmw(MT6359_AUDENC_ANA_CON18, (1u << 11) | (1u << 12), 0);	/* clear comparator mic bits */
	pmic_rmw(MT6359_AUDENC_ANA_CON16, (1u << 0), 0);		/* MICBIAS1 off */
	pmic_rmw(MT6359_ACCDET_CON1, (1u << 6), 0);			/* ACCDET_SW_EN off */
	pmic_rmw(MT6359_ACCDET_CON18, (1u << 2), 0);			/* clear the CON18 latch */
	s_jack_inited = 0;
	s_jack_route_picked = 0;
}

#include "../common/pico/pico_audio_stream.inc"
