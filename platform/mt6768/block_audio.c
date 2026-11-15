/*
 * GammaOS Pico audio on the Konkr Pocket Block (MT6768 AFE, MT6358 codec in the PMIC).
 *
 * The settings (platform/common/pico/ayaneo_settings.inc), the 48 kHz ring and every core's stream
 * (pico_audio_ring.inc, pico_audio_stream.inc) are the KPA's, included as text. What is the Block's
 * own is the hardware under them: PICO_AUD_HW_ON starts the path and the ring's playback,
 * PICO_AUD_DMA_CUR() is the address being played, audio_shutdown stops it, and the jack_* calls
 * look after the headphone jack.
 *
 * The path is the one Android uses for the speakers (vendor audio_device.xml: speaker path
 * "int_hp_buf", HPL/HPR Mux "LoudSPK Playback", "Ext_Speaker_Amp Switch"): the AFE's DL1 memif
 * reads the ring from DRAM, the interconnect hands DL1 to the ADDA downlink, the ADDA link carries
 * it over the AUD_* pads (GPIO136..139) to the MT6358, whose DAC feeds the left channel through the
 * line out buffer into the HPL buffer and the right channel straight into the HPR buffer; the two
 * headphone buffers drive a plain stereo amplifier that GPIO160 switches on. Every register value
 * here is the state the Block's own Android left in the hardware while it played a tone through the
 * speakers (captured from the AFE and PMIC regmaps, /work/konkrblock/audio_capture); the order the
 * codec is brought up in keeps its regulators and bias ahead of its drivers, and its drivers ahead
 * of the DAC, so the buffers do not thump.
 *
 * Should the audio clocks not be running, the ring is "played" by the 13 MHz clock instead
 * (the silent layer this file started as): the games keep their real timing, only without sound.
 */

#include <platform/mt_typedefs.h>
#include <platform/mt_gpio.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <string.h>
#include <debug.h>
#include <part_interface.h>

extern void arch_clean_cache_range(addr_t start, size_t len);
extern unsigned int gpt4_get_current_tick(void);
extern void mdelay(unsigned long msec);
extern void udelay(unsigned long usec);
extern signed int pwrap_read(unsigned int adr, unsigned int *rdata);
extern signed int pwrap_write(unsigned int adr, unsigned int wdata);

/* boot_b, where a build without the card flow would keep the settings block (the Block's is 32 MB) */
#define AYANEO_AUDIO_PART	"boot_b"
#ifndef AYANEO_AUDIO_VOLUME
#define AYANEO_AUDIO_VOLUME	40
#endif

/* the boot chime is not part of the card build: nothing plays before the GBA BIOS intro */
static volatile int s_audio_active;

#include "../common/pico/ayaneo_settings.inc"

/* ---------- the MT6768 audio front end (AFE, 0x11220000) ---------- */
#define AFE_BASE			0x11220000u
#define AUDIO_TOP_CON0			0x000	/* clock gates of the AFE's blocks */
#define AUDIO_TOP_CON3			0x00c
#define AFE_DAC_CON0			0x010	/* bit 0 AFE on, bit 1 DL1 on */
#define AFE_DAC_CON1			0x014	/* bits 3:0 DL1 rate */
#define AFE_CONN3			0x02c	/* ADDA DL left input: bit 5 DL1 left */
#define AFE_CONN4			0x030	/* ADDA DL right input: bit 6 DL1 right */
#define AFE_DL1_BASE			0x040
#define AFE_DL1_CUR			0x044
#define AFE_DL1_END			0x048
#define AFE_CONN_24BIT			0x06c
#define AFE_MEMIF_MSB			0x0cc
#define AFE_ADDA_DL_SRC2_CON0		0x108
#define AFE_ADDA_DL_SRC2_CON1		0x10c
#define AFE_ADDA_UL_DL_CON0		0x124	/* bit 0 ADDA on */
#define AFE_ADDA_NEWIF_CFG0		0x138
#define AFE_ADDA_NEWIF_CFG2		0x140
#define AFE_ADDA_DL_SDM_DCCOMP_CON	0xc50
#define AFE_ADDA_DL_SDM_TEST		0xc54
#define AFE_ADDA_MTKAIF_TX_CFG1		0xe14
#define AFE_ADDA_MTKAIF_RX_CFG0		0xe20
#define AFE_ADDA_MTKAIF_RX_CFG1		0xe24
#define AFE_ADDA_MTKAIF_RX_CFG2		0xe28
#define AFE_AUD_PAD_TOP			0xe40

#define AFE_DL1_RATE_48K		10u
#define AFE_ON				(1u << 0)
#define AFE_DL1_ON			(1u << 1)

static unsigned int afe_r(unsigned int off) { return *(volatile unsigned int *)(AFE_BASE + off); }
static void afe_w(unsigned int off, unsigned int v) { *(volatile unsigned int *)(AFE_BASE + off) = v; }
static void afe_rmw(unsigned int off, unsigned int mask, unsigned int v)
{
	afe_w(off, (afe_r(off) & ~mask) | (v & mask));
}

/* ---------- the MT6358 codec, over the PMIC wrapper ---------- */
#define PMIC_GPIO_MODE2			0x00d8	/* the PMIC side of the AUD_* pads */
#define PMIC_DCXO_CW14			0x07ac	/* bit 13: the crystal's buffer for the audio clock */
#define AUD_TOP_CKPDN_CON0		0x220c
#define AUDNCP_CLKDIV_CON0		0x223a
#define AUDNCP_CLKDIV_CON1		0x223c
#define AUDNCP_CLKDIV_CON2		0x223e
#define AUDNCP_CLKDIV_CON3		0x2240
#define AUDNCP_CLKDIV_CON4		0x2242
#define AFE_UL_DL_CON0			0x2288
#define AFE_DL_SRC2_CON0_L		0x228a
#define AUDIO_TOP_CON0_PMIC		0x2292
#define AFUNC_AUD_CON0			0x2296
#define AFUNC_AUD_CON2			0x229a
#define AUDENC_ANA_CON6			0x2394	/* bit 0: the audio clock squarer */
#define AUDDEC_ANA_CON0			0x2408
#define AUDDEC_ANA_CON1			0x240a
#define AUDDEC_ANA_CON2			0x240c
#define AUDDEC_ANA_CON4			0x2410
#define AUDDEC_ANA_CON6			0x2414
#define AUDDEC_ANA_CON7			0x2416
#define AUDDEC_ANA_CON8			0x2418
#define AUDDEC_ANA_CON9			0x241a
#define AUDDEC_ANA_CON10		0x241c
#define AUDDEC_ANA_CON11		0x241e
#define AUDDEC_ANA_CON12		0x2420
#define AUDDEC_ANA_CON13		0x2422
#define AUDDEC_ANA_CON14		0x2424
#define AUDDEC_ANA_CON15		0x2426
#define ZCD_CON0			0x2488
#define ZCD_CON1			0x248a	/* line out gain */
#define ZCD_CON2			0x248c	/* headphone buffer gains */
#define ZCD_CON3			0x248e
#define ZCD_CON5			0x2492

#define CODEC_GAIN_QUIET		0x0f9f	/* both channels well down, while the buffers settle */

static unsigned int pmic_r(unsigned int reg)
{
	unsigned int v = 0;
	pwrap_read(reg, &v);
	return v & 0xffffu;
}
static void pmic_w(unsigned int reg, unsigned int v) { pwrap_write(reg, v & 0xffffu); }
static void pmic_rmw(unsigned int reg, unsigned int mask, unsigned int v)
{
	pmic_w(reg, (pmic_r(reg) & ~mask) | (v & mask));
}

/* ---------- the audio clocks ---------- */
/* The MT6768 has no power domain of its own for audio: the AFE sits in the always on infra domain
 * (the kernel's power domain table has no entry for SPM 0x314, and the stock LK's audio MTCMOS
 * call is empty). What it needs is its bus clocks: the infra gates ifr_audio (status 0x94 bit 25)
 * and ifr_audio26m (status 0xac bit 4), opened through their clear registers, and the TOPCKGEN
 * muxes audio_sel (CLK_CFG_3 bits 25:24, gate bit 31) and aud_intbus_sel (CLK_CFG_4 bits 1:0,
 * gate bit 7), which the preloader leaves running. An access to the AFE with its bus clock gated
 * hangs the bus, so the clocks are checked before any of it. 0 when all are running. */
#define BLOCK_INFRA_AO			0x10001000u
#define BLOCK_TOPCKGEN			0x10000000u
static int audio_clocks_on(void)
{
	volatile unsigned int *infra = (volatile unsigned int *)BLOCK_INFRA_AO;
	volatile unsigned int *ck = (volatile unsigned int *)BLOCK_TOPCKGEN;

	infra[0x8c / 4] = 1u << 25;	/* ifr_audio */
	infra[0xa8 / 4] = 1u << 4;	/* ifr_audio26m */
	udelay(10);
	if ((infra[0x94 / 4] & (1u << 25)) || (infra[0xac / 4] & (1u << 4)))
		return -1;
	if ((ck[0x70 / 4] & (1u << 31)) || (ck[0x80 / 4] & (1u << 7)))
		return -1;
	return 0;
}

/* ---------- the codec, speakers on ---------- */
/* the line out and headphone buffer gains, as Android ran the speakers (ZCD_CON1, ZCD_CON2) */
#define CODEC_GAIN_LO			0x0000
#define CODEC_GAIN_HP			0x050a

/* Raise a gain register from where it is to target, one step of each half at a time: the
 * buffers follow a gain change at once, and a jump is heard. */
static void codec_gain_ramp(unsigned int reg, unsigned int target)
{
	unsigned int v = pmic_r(reg), l = v & 0x1f, r = (v >> 7) & 0x1f;
	unsigned int tl = target & 0x1f, tr = (target >> 7) & 0x1f;

	while (l != tl || r != tr) {
		if (l > tl) l--; else if (l < tl) l++;
		if (r > tr) r--; else if (r < tr) r++;
		pmic_w(reg, (v & ~((0x1fu << 7) | 0x1fu)) | (r << 7) | l);
		udelay(100);
	}
	pmic_w(reg, target);
}

static void codec_speakers_on(void)
{
	unsigned int i;

	/* the codec's clocks: the crystal's audio buffer, the AUD_* pads on the PMIC side, the codec
	 * clock gates, the clock squarer */
	pmic_rmw(PMIC_DCXO_CW14, 1u << 13, 1u << 13);
	pmic_w(PMIC_GPIO_MODE2, 0x0249);
	pmic_rmw(AUD_TOP_CKPDN_CON0, 0x0066, 0);
	pmic_rmw(AUDENC_ANA_CON6, 1u, 1u);
	udelay(250);

	/* the digital downlink: its clocks, then the source and the interface on */
	pmic_w(AUDIO_TOP_CON0_PMIC, 0x002b);
	pmic_w(AFUNC_AUD_CON2, 0x000b);
	pmic_w(AFUNC_AUD_CON0, 0xcba1);
	pmic_w(AFE_DL_SRC2_CON0_L, 0x0001);
	pmic_w(AFE_UL_DL_CON0, 0x0001);

	/* the analog side starts quiet: protections off, references, both gains far down */
	pmic_rmw(AUDDEC_ANA_CON4, 1u << 6, 1u << 6);		/* headphone CMFB out of reset */
	pmic_w(AUDDEC_ANA_CON0, 0x3000);
	pmic_rmw(AUDDEC_ANA_CON7, 1u << 4, 1u << 4);
	pmic_w(AUDDEC_ANA_CON2, 0xc000);
	pmic_w(ZCD_CON2, CODEC_GAIN_QUIET);
	pmic_w(ZCD_CON1, CODEC_GAIN_QUIET);

	/* the negative charge pump (26 MHz / 43), then the local regulators */
	pmic_w(AUDNCP_CLKDIV_CON1, 0x0001);
	pmic_w(AUDNCP_CLKDIV_CON2, 0x002c);
	pmic_w(AUDNCP_CLKDIV_CON0, 0x0001);
	pmic_w(AUDNCP_CLKDIV_CON4, 0x0002);
	pmic_w(AUDNCP_CLKDIV_CON3, 0x0000);
	udelay(300);
	pmic_rmw(AUDDEC_ANA_CON14, 0x1055, 0x1055);
	pmic_w(AUDDEC_ANA_CON15, 0x0001);
	udelay(150);

	/* bias currents and the output stage stabilisers */
	pmic_w(ZCD_CON0, 0x0000);
	pmic_w(AUDDEC_ANA_CON12, 0x0055);
	pmic_w(AUDDEC_ANA_CON11, 0x4900);
	pmic_w(AUDDEC_ANA_CON2, 0xc033);

	/* the line out buffer: stabiliser, bias, core */
	pmic_rmw(AUDDEC_ANA_CON7, 1u << 8, 1u << 8);
	pmic_rmw(AUDDEC_ANA_CON7, 1u << 1, 1u << 1);
	pmic_rmw(AUDDEC_ANA_CON7, 1u << 0, 1u << 0);

	/* the headphone buffers in speaker use: left fed by the line out, right by the right DAC;
	 * brought up on their auxiliary stage, then handed to the main one */
	pmic_rmw(AUDDEC_ANA_CON0, 0x3u << 8, 0x1u << 8);
	pmic_rmw(AUDDEC_ANA_CON0, 0x3u << 10, 0x2u << 10);
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0x0c);
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0x3c);
	pmic_rmw(AUDDEC_ANA_CON9, 0xff00, 0x0c00);
	pmic_rmw(AUDDEC_ANA_CON0, 0xf0, 0xc0);
	pmic_rmw(AUDDEC_ANA_CON0, 0xf0, 0xf0);
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0xfc);
	pmic_rmw(AUDDEC_ANA_CON9, 0xff00, 0x0e00);
	pmic_rmw(AUDDEC_ANA_CON9, 0xff00, 0x0200);
	pmic_w(AUDDEC_ANA_CON10, 0x0000);
	pmic_w(AUDDEC_ANA_CON6, 0x0010);
	pmic_w(AUDDEC_ANA_CON7, 0x0010);
	pmic_rmw(AUDDEC_ANA_CON7, 0x3u, 0x3u);			/* line out bias + core again */
	pmic_rmw(AUDDEC_ANA_CON7, 1u << 8, 1u << 8);
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0xff);			/* main output stage on */
	for (i = 1; i <= 0x3f; i++) {				/* its strength, step by step */
		pmic_rmw(AUDDEC_ANA_CON1, 0x3f00, i << 8);
		udelay(60);
	}
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0xcf);			/* auxiliary feedback off */
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0xc3);			/* auxiliary stage off */
	pmic_rmw(AUDDEC_ANA_CON1, 0xff, 0x03);			/* main and auxiliary apart */
	mdelay(1);

	/* the DAC: its clock, the converter, its low noise mode, then the line out takes it */
	pmic_rmw(AUDDEC_ANA_CON13, 1u, 1u);
	pmic_rmw(AUDDEC_ANA_CON0, 0xf, 0xf);
	pmic_rmw(AUDDEC_ANA_CON9, 1u, 1u);
	udelay(150);
	pmic_rmw(AUDDEC_ANA_CON7, 0x3u << 2, 0x2u << 2);

	/* the state Android ran it in, word for word (with what its codec driver set at boot: CON4 bit 0,
	 * CON8, CON13 bit 4 off, ZCD_CON3), then the gains up to its speaker level */
	pmic_rmw(AUDDEC_ANA_CON4, 1u, 1u);
	pmic_w(AUDDEC_ANA_CON8, 0x0037);
	pmic_w(AUDDEC_ANA_CON9, 0xf201);
	pmic_w(AUDDEC_ANA_CON11, 0x4901);
	pmic_w(AUDDEC_ANA_CON13, 0x0001);
	pmic_w(ZCD_CON3, 0x000a);
	pmic_w(ZCD_CON5, 0x2a2a);
	codec_gain_ramp(ZCD_CON1, CODEC_GAIN_LO);
	codec_gain_ramp(ZCD_CON2, CODEC_GAIN_HP);
	pmic_w(ZCD_CON0, 0x000b);
}

static void codec_speakers_off(void)
{
	pmic_w(ZCD_CON1, CODEC_GAIN_QUIET);
	pmic_w(ZCD_CON2, CODEC_GAIN_QUIET);
	pmic_rmw(AUDDEC_ANA_CON7, 0x3u << 2, 0);		/* line out off the DAC */
	pmic_rmw(AUDDEC_ANA_CON0, 0xf, 0);			/* DAC off */
	pmic_rmw(AUDDEC_ANA_CON0, 0xff0, 0);			/* buffers off, muxes open */
	pmic_rmw(AUDDEC_ANA_CON7, 0x3u, 0);
	pmic_w(AFE_UL_DL_CON0, 0x0000);
	pmic_w(AFE_DL_SRC2_CON0_L, 0x0000);
}

/* ---------- the speaker amplifier ---------- */
#define BLOCK_SPK_AMP_GPIO		(160u | 0x80000000u)
static void spk_amp(int on)
{
	mt_set_gpio_mode(BLOCK_SPK_AMP_GPIO, 0);
	mt_set_gpio_dir(BLOCK_SPK_AMP_GPIO, GPIO_DIR_OUT);
	mt_set_gpio_out(BLOCK_SPK_AMP_GPIO, on ? GPIO_OUT_ONE : GPIO_OUT_ZERO);
}

/* ---------- the ring's playback ---------- */
#define BLOCK_AUD_HZ		48000u
#define BLOCK_GPT_HZ		13000000u
static volatile int s_play_on, s_play_hw;
static unsigned int s_play_base, s_play_frames;	/* the ring: its address and length in frames */
static unsigned int s_play_last;		/* silent fallback: GPT tick of the last reading */
static unsigned long long s_play_ticks;		/* silent fallback: ticks played since the start */
volatile int g_dbg_block_audio;			/* 1 hardware playing, -1 its clocks would not run */

static void play_start(unsigned int pa, unsigned int bytes)
{
	unsigned int i;

	s_play_base = pa;
	s_play_frames = bytes / 4u;
	s_play_ticks = 0;
	s_play_last = gpt4_get_current_tick();
	s_play_hw = 0;
	if (audio_clocks_on()) {
		g_dbg_block_audio = -1;
		s_play_on = 1;
		return;
	}

	/* the AUD_* pads to the PMIC (clock, sync, two data lines) */
	for (i = 136; i <= 139; i++) {
		mt_set_gpio_mode(i | 0x80000000u, 1);
		mt_set_gpio_smt(i | 0x80000000u, 1);
	}

	/* AFE clocks and its ADDA link, as Android ran them */
	afe_w(AUDIO_TOP_CON0, 0xa0fd4038);
	afe_w(AUDIO_TOP_CON3, 0x60000004);
	afe_w(AFE_ADDA_NEWIF_CFG0, 0x03f87200);
	afe_w(AFE_ADDA_NEWIF_CFG2, 0x03117180);
	afe_w(AFE_ADDA_MTKAIF_TX_CFG1, 0x00000063);
	afe_w(AFE_ADDA_MTKAIF_RX_CFG0, 0x00000730);
	afe_w(AFE_ADDA_MTKAIF_RX_CFG1, 0x00034330);
	afe_w(AFE_ADDA_MTKAIF_RX_CFG2, 0x00000030);
	afe_w(AFE_AUD_PAD_TOP, 0x00000030);

	codec_speakers_on();

	/* DL1 over the ring, 48 kHz stereo 16 bit, into the ADDA downlink */
	afe_rmw(AFE_DAC_CON0, AFE_DL1_ON, 0);
	afe_w(AFE_DL1_BASE, pa);
	afe_w(AFE_DL1_END, pa + bytes - 1u);
	afe_rmw(AFE_DAC_CON1, 0xfu, AFE_DL1_RATE_48K);
	afe_w(AFE_MEMIF_MSB, 0x01ff0000);
	afe_w(AFE_CONN_24BIT, 0xffffffff);
	afe_rmw(AFE_CONN3, 1u << 5, 1u << 5);
	afe_rmw(AFE_CONN4, 1u << 6, 1u << 6);
	afe_w(AFE_ADDA_DL_SRC2_CON0, 0x83001803);
	afe_w(AFE_ADDA_DL_SRC2_CON1, 0xf74f0000);
	afe_w(AFE_ADDA_DL_SDM_DCCOMP_CON, 0x0700701d);
	afe_w(AFE_ADDA_DL_SDM_TEST, 0x00007010);
	afe_rmw(AFE_ADDA_UL_DL_CON0, 1u, 1u);
	afe_rmw(AFE_DAC_CON0, AFE_ON | AFE_DL1_ON, AFE_ON | AFE_DL1_ON);
	mdelay(2);

	spk_amp(1);
	s_play_hw = 1;
	s_play_on = 1;
	g_dbg_block_audio = 1;
}

/* the address the ring is being played from now */
static unsigned int play_cur(void)
{
	unsigned int now, frame, cur;

	if (!s_play_on || !s_play_frames)
		return s_play_base;
	if (s_play_hw) {
		cur = afe_r(AFE_DL1_CUR);
		if (cur >= s_play_base && cur < s_play_base + s_play_frames * 4u)
			return cur & ~3u;
		return s_play_base;
	}
	now = gpt4_get_current_tick();
	s_play_ticks += (unsigned int)(now - s_play_last);	/* read at least once a 330 s GPT period */
	s_play_last = now;
	frame = (unsigned int)((s_play_ticks * BLOCK_AUD_HZ / BLOCK_GPT_HZ) % s_play_frames);
	return s_play_base + frame * 4u;
}

static void audio_shutdown(void)
{
	if (s_play_hw) {
		spk_amp(0);
		mdelay(1);
		afe_rmw(AFE_DAC_CON0, AFE_DL1_ON, 0);
		codec_speakers_off();
		afe_rmw(AFE_ADDA_UL_DL_CON0, 1u, 0);
		afe_rmw(AFE_ADDA_DL_SRC2_CON0, 1u, 0);
		s_play_hw = 0;
	}
	s_play_on = 0;
	g_dbg_block_audio = 0;
}

/* ---------- the headphone jack ----------
 * The Block's speakers hang off the MT6358's own headphone buffers (through the amplifier GPIO160
 * switches on), so the jack is the same output with the amplifier off: headphones in, the amp goes
 * off and the buffers are set as Android sets them for a wired headset; out, the speaker setting
 * comes back. Every value below is the Block's own, read from its Android with headphones in and
 * out (playing a tone each way, /work/konkrblock/audio_capture).
 *
 * The plug is seen by the MT6358's accessory detector, EINT0 comparator: Android sets it up and
 * takes its interrupt; here the same set up, and the comparator's output (ACCDET_CON18 bit 14,
 * 1 = plugged) is polled every 100 ms, two equal reads in a row to change the route. */
#define AUDDEC_ELR_0			0x242a	/* the headphone buffers' DC trim */
#define AUDENC_ANA_CON10		0x239c	/* mic bias 1 (set, not powered) */
#define AUDENC_ANA_CON11		0x239e	/* the detector's comparator: thresholds, pull lows */
#define ACCDET_CON1			0x250a	/* bit 2: EINT0 on */
#define ACCDET_CON18			0x252c	/* bit 14: EINT0 comparator out, 1 = plugged */
#define JACK_PLUGGED_BIT		(1u << 14)
/* the headphone setting, as Android ran a wired headset */
#define HP_CON0_MUX			0x0a00	/* both buffers on the DAC (bits 11:8) */
#define HP_CON7_LO			0x0110	/* the line out stage off */
#define HP_GAIN				0x0489
#define HP_ZCD_CON3			0x001f
#define HP_ZCD_CON5			0x2929
/* This console's DC trim for each route, from the calibration Android runs at boot (offset_L 6,
 * offset_R 0): the LK does not run that calibration, and another console's would differ a little,
 * at worst a soft pop on the switch. */
#define HP_ELR_0			0x1f10
#define SPK_CON0_MUX			0x0900	/* left from the line out, right from the DAC */
#define SPK_CON7_LO			0x011b
#define SPK_ELR_0			0x1018

static volatile int s_jack_up;			/* the detector is set up */
static volatile int s_jack_route;		/* 0 speakers, 1 headphones */
static int s_jack_seen = -1, s_jack_votes;
static unsigned int s_jack_tick;
volatile int g_jack_route, g_jack_plugged = -1;	/* oem jack */

static int jack_comparator(void) { return (pmic_r(ACCDET_CON18) & JACK_PLUGGED_BIT) ? 1 : 0; }

static void jack_route(int hp)
{
	if (hp == s_jack_route)
		return;
	codec_gain_ramp(ZCD_CON2, CODEC_GAIN_QUIET);		/* down before anything moves */
	if (hp) {
		spk_amp(0);
		codec_gain_ramp(ZCD_CON1, CODEC_GAIN_QUIET);
		pmic_rmw(AUDDEC_ANA_CON0, 0xf00, HP_CON0_MUX);
		pmic_w(AUDDEC_ANA_CON7, HP_CON7_LO);
		pmic_w(ZCD_CON3, HP_ZCD_CON3);
		pmic_w(ZCD_CON5, HP_ZCD_CON5);
		pmic_w(AUDDEC_ELR_0, HP_ELR_0);
		codec_gain_ramp(ZCD_CON2, HP_GAIN);
	} else {
		pmic_w(AUDDEC_ELR_0, SPK_ELR_0);
		pmic_w(ZCD_CON5, 0x2a2a);
		pmic_w(ZCD_CON3, 0x000a);
		pmic_w(AUDDEC_ANA_CON7, SPK_CON7_LO);
		pmic_rmw(AUDDEC_ANA_CON0, 0xf00, SPK_CON0_MUX);
		codec_gain_ramp(ZCD_CON1, CODEC_GAIN_LO);
		codec_gain_ramp(ZCD_CON2, CODEC_GAIN_HP);
		spk_amp(1);
	}
	s_jack_route = hp;
	g_jack_route = hp;
}

/* the detector as Android sets it up (its values with the jack empty), EINT0 switched on last;
 * then the route the jack calls for at start */
static void jack_detect_init(void)
{
	if (!s_jack_up) {
		pmic_w(AUDENC_ANA_CON10, 0x0064);
		pmic_w(AUDENC_ANA_CON11, 0x0c07);
		pmic_w(0x250c, 0x0808);		/* CON2: the comparators' PWM, EINT0's on */
		pmic_w(0x250e, 0x04ff);		/* CON3: PWM width */
		pmic_w(0x2510, 0x04ff);		/* CON4: PWM threshold */
		pmic_w(0x2512, 0x81f0);		/* CON5: rise and fall delays */
		pmic_w(0x2514, 0x0800);		/* CON6..CON10: debounce times */
		pmic_w(0x2516, 0x0800);
		pmic_w(0x2518, 0x0010);
		pmic_w(0x251a, 0x0020);
		pmic_w(0x251c, 0x0044);
		pmic_w(0x251e, 0x4bbb);		/* CON11: the states it starts from */
		pmic_w(0x2522, 0x700e);		/* CON13: the switches under software control */
		pmic_w(0x2526, 0x2609);		/* CON15: EINT0 debounce and PWM */
		pmic_w(ACCDET_CON1, 0x0004);	/* EINT0 on */
		s_jack_up = 1;
		mdelay(20);			/* the comparator's first decision */
	}
	s_jack_seen = jack_comparator();
	s_jack_votes = 0;
	g_jack_plugged = s_jack_seen;
	s_jack_tick = gpt4_get_current_tick();
	jack_route(s_jack_seen);
}

/* called from the audio submit (every frame): a read every 100 ms, a change after two in a row */
static void jack_poll(void)
{
	unsigned int now = gpt4_get_current_tick();
	int p;
	if (!s_jack_up || now - s_jack_tick < 1300000u)
		return;
	s_jack_tick = now;
	p = jack_comparator();
	if (p == s_jack_seen) {
		s_jack_votes = 0;
		return;
	}
	if (++s_jack_votes < 2)
		return;
	s_jack_seen = p;
	s_jack_votes = 0;
	g_jack_plugged = p;
	jack_route(p);
}

/* Android sets the detector and the route up itself */
static void jack_teardown_for_android(void) { }

#define PICO_AUD_DMA_CUR()		play_cur()
#define PICO_AUD_HW_ON(pa, bytes)	play_start((pa), (bytes))
#include "../common/pico/pico_audio_ring.inc"
#include "../common/pico/pico_audio_stream.inc"
