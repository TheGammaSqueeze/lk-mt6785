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
#include <string.h>
#include <debug.h>
#include <part_interface.h>
#ifdef AYANEO_AUDIO_TRACE
#include <platform/boot_mode.h>		/* g_boot_arg->log_enable (force UART) */
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
#define AFE_CONN3			0x002c
#define AFE_CONN4			0x0030
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

/* ---- persisted user settings (audio volume + screen brightness) ----
 * Stored in a single 512-byte block in boot_b at 30 MB, past the ROM (20-28 MB)
 * and the save state (28-30 MB); boot_b is 32 MB. Applied to both the boot chime
 * and the game, and to the backlight during the animation and in-game. */
#define AYANEO_SET_OFF		0x01E00000u	/* 30 MB into boot_b */
#define AYANEO_SET_MAGIC	0x54455341u	/* "ASET" LE */
#define AYANEO_SET_VER		2u
#define AYANEO_BL_MIN		16		/* keep the panel visible (never 0) */
#define AYANEO_BL_MAX		255		/* mt65xx LCD level is 0-255 */
#define AYANEO_BL_STEP		8		/* fine granularity (~30 steps) */
#ifndef AYANEO_BL_DEFAULT
#define AYANEO_BL_DEFAULT	200
#endif

extern void ayaneo_apply_backlight(int level);	/* mt_disp_drv.c: drive the LCD */

/* All settings are persisted in the boot_b block. Field byte offsets:
 * 0 magic, 4 version, 8 volume, 12 brightness, 16 load-state-on-boot,
 * 20 skip-boot-anim/chime, 24 lcd-filter, 28 color-correction, 32 dark-filter. */
static volatile int s_gbc_vol = AYANEO_AUDIO_VOLUME;	/* audio level, 0-100 */
static volatile int s_brightness = AYANEO_BL_DEFAULT;	/* LCD level, 0-255 */
static volatile int s_load_on_boot = 1;		/* resume save state after boot */
static volatile int s_skip_boot = 0;		/* skip the animation + chime */
static volatile int s_lcd_filter = 0;		/* 0 off, 1 scanlines, 2 grid, 3 both */
static volatile int s_color_correct = 0;	/* CGB colour correction on/off */
static volatile int s_dark_filter = 0;		/* 0-5 dark filter level */
static int s_settings_loaded;

/* ---------- little-endian helpers ---------- */
static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
static unsigned int rd16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static void wr32(unsigned char *p, unsigned int v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* ---------- persisted volume + brightness ---------- */

int ayaneo_gbc_audio_get_volume(void) { return s_gbc_vol; }
void ayaneo_gbc_audio_set_volume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	s_gbc_vol = v;
}

/* apply a backlight level (clamped) to the hardware and remember it */
static void ayaneo_set_brightness(int level)
{
	if (level < AYANEO_BL_MIN) level = AYANEO_BL_MIN;
	if (level > AYANEO_BL_MAX) level = AYANEO_BL_MAX;
	s_brightness = level;
	ayaneo_apply_backlight(level);
}

/* nudge brightness by one step (dir +1/-1); returns the new level as 0-100% */
int ayaneo_brightness_step(int dir)
{
	ayaneo_set_brightness(s_brightness + dir * AYANEO_BL_STEP);
	return s_brightness * 100 / AYANEO_BL_MAX;
}
int ayaneo_brightness_pct(void) { return s_brightness * 100 / AYANEO_BL_MAX; }

/* ---- boot behaviour + video settings (all persisted) ---- */
int ayaneo_get_load_on_boot(void)   { return s_load_on_boot; }
void ayaneo_set_load_on_boot(int v) { s_load_on_boot = v ? 1 : 0; }
int ayaneo_get_skip_boot(void)      { return s_skip_boot; }
void ayaneo_set_skip_boot(int v)    { s_skip_boot = v ? 1 : 0; }
int ayaneo_get_lcd_filter(void)     { return s_lcd_filter; }
void ayaneo_set_lcd_filter(int v)   { s_lcd_filter = (v < 0) ? 0 : (v > 3 ? 3 : v); }
int ayaneo_get_color_correct(void)  { return s_color_correct; }
void ayaneo_set_color_correct(int v){ s_color_correct = v ? 1 : 0; }
int ayaneo_get_dark_filter(void)    { return s_dark_filter; }
void ayaneo_set_dark_filter(int v)  { s_dark_filter = (v < 0) ? 0 : (v > 5 ? 5 : v); }

/* load the persisted settings from boot_b (once). Missing/invalid -> keep the
 * compile-time defaults. Does not touch the hardware; callers apply brightness. */
void ayaneo_settings_load(void)
{
	unsigned char b[64];
	unsigned int ver;

	if (s_settings_loaded)
		return;
	/* If storage is not up yet (early boot), leave s_settings_loaded clear so a
	 * later call retries; only commit once we have actually read the block. */
	if (partition_read(AYANEO_AUDIO_PART, AYANEO_SET_OFF, b, sizeof(b)) != (ssize_t)sizeof(b))
		return;
	s_settings_loaded = 1;			/* storage worked: decide once */
	if (rd32(b) != AYANEO_SET_MAGIC)
		return;				/* no/invalid blob: keep defaults */
	ver = rd32(b + 4);
	{
		int vol = (int)rd32(b + 8);
		int bl  = (int)rd32(b + 12);

		if (vol >= 0 && vol <= 100)
			s_gbc_vol = vol;
		if (bl >= AYANEO_BL_MIN && bl <= AYANEO_BL_MAX)
			s_brightness = bl;
	}
	if (ver >= 2) {			/* new fields; older blobs keep the defaults */
		s_load_on_boot  = rd32(b + 16) ? 1 : 0;
		s_skip_boot     = rd32(b + 20) ? 1 : 0;
		ayaneo_set_lcd_filter((int)rd32(b + 24));
		s_color_correct = rd32(b + 28) ? 1 : 0;
		ayaneo_set_dark_filter((int)rd32(b + 32));
	}
}

/* write all current settings back to boot_b (block-aligned) */
void ayaneo_settings_save(void)
{
	static unsigned char b[512] __attribute__((aligned(64)));

	memset(b, 0, sizeof(b));
	wr32(b + 0, AYANEO_SET_MAGIC);
	wr32(b + 4, AYANEO_SET_VER);
	wr32(b + 8, (unsigned int)s_gbc_vol);
	wr32(b + 12, (unsigned int)s_brightness);
	wr32(b + 16, (unsigned int)s_load_on_boot);
	wr32(b + 20, (unsigned int)s_skip_boot);
	wr32(b + 24, (unsigned int)s_lcd_filter);
	wr32(b + 28, (unsigned int)s_color_correct);
	wr32(b + 32, (unsigned int)s_dark_filter);
	s_settings_loaded = 1;			/* our value is now authoritative */
	arch_clean_cache_range((addr_t)b, sizeof(b));
	partition_write(AYANEO_AUDIO_PART, AYANEO_SET_OFF, b, sizeof(b));
}

/* ensure settings are loaded, then push the brightness to the panel. Called by
 * the display path right after the backlight is turned on (animation frame 1,
 * or the Select-skip emulator init) so the persisted level is used from boot. */
void ayaneo_apply_persisted_brightness(void)
{
	ayaneo_settings_load();
	ayaneo_apply_backlight(s_brightness);
}

/* The persisted LCD level for mt65xx_backlight_on() to use instead of the
 * cust default, so every backlight-on during boot (there are several, in
 * platform.c / boot_mode.c) honours the saved brightness rather than snapping
 * to full. Lazily loads settings; falls back to the default until storage is up. */
int ayaneo_backlight_level_for_boot(void)
{
	ayaneo_settings_load();
	return s_brightness;
}

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
	afe_rmw(AFE_DAC_CON0, (1u << 4), 0);		/* DL1_ON = 0 */
	afe_rmw(AFE_ADDA_DL_SRC2_CON0, (1u << 0), 0);	/* DL_2_SRC_ON = 0 */
	afe_rmw(AFE_ADDA_UL_DL_CON0, (1u << 0), 0);	/* ADDA_AFE_ON = 0 */
}

static void spk_amp_enable(int on)
{
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

/*
 * Public entry: spawn the audio thread. Returns immediately so the boot
 * animation is never blocked. Safe to call once, after the display is up.
 */
void ayaneo_boot_audio_start(void)
{
	thread_t *t;

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

/* ================= streaming audio for the GBC emulator ================= */
/*
 * The emulator produces stereo audio at the Game Boy's native 2097152 Hz; we
 * box-resample it to 48 kHz and stream it into a looping AFE DL ring that plays
 * continuously. The run loop calls ayaneo_gbc_audio_submit() each frame to keep
 * the ring fed ahead of the DMA read pointer.
 */
#define GBC_RING_FRAMES		16384u		/* power of two, ~341 ms @48k */
#define GBC_SRC_HZ		2097152u
#define GBC_DST_HZ		48000u

static short s_gbc_ring[GBC_RING_FRAMES * 2] __attribute__((aligned(64)));
static volatile int s_gbc_audio_on;
static volatile int s_gbc_paused;			/* silence while set */
/* s_gbc_vol / the volume getters+setters live up top with the persisted settings */
static unsigned s_gbc_widx;			/* ring write cursor, in frames */
static long long s_rs_accl, s_rs_accr;		/* box-filter accumulators */
static unsigned s_rs_n, s_rs_phase;		/* samples accumulated, phase */

/* is the boot chime still playing? (emulator waits for it before taking over) */
int ayaneo_boot_audio_active(void) { return s_audio_active; }

/*
 * Tear the streaming audio down to a clean, cold state (mute -> drain -> amp off
 * -> stop DMA -> park DAC), exactly like the boot chime does when it finishes.
 * Called before a save-state power-off: mt_power_off() is a soft off that keeps
 * the PMIC audio registers latched, so without this the amp/DAC were left
 * enabled and the NEXT boot's chime bring-up came up wrong (silent). A hard
 * reset cold-resets the block, which is why a forced reboot masked the bug.
 */
void ayaneo_gbc_audio_shutdown(void)
{
	if (!s_gbc_audio_on)
		return;
	s_gbc_audio_on = 0;
	audio_shutdown();
}

/*
 * Pause/resume the game audio (used for fast-forward). On pause we fill the ring
 * with silence so the looping DMA plays nothing; on resume we re-seat the write
 * cursor a fixed lead ahead of the hardware read pointer so latency stays sane.
 */
void ayaneo_gbc_audio_pause(int on)
{
	if (!s_gbc_audio_on)
		return;
	if (on) {
		s_gbc_paused = 1;
		memset(s_gbc_ring, 0, sizeof(s_gbc_ring));
		arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	} else {
		unsigned cur = afe_r(AFE_DL1_CUR);
		unsigned base = (unsigned)(addr_t)s_gbc_ring;
		unsigned f = (cur >= base) ? (cur - base) / 4 : 0;
		s_gbc_widx = (f + GBC_RING_FRAMES / 4) & (GBC_RING_FRAMES - 1);
		s_gbc_paused = 0;
	}
}

void ayaneo_gbc_audio_init(void)
{
	if (s_gbc_audio_on)
		return;

	memset(s_gbc_ring, 0, sizeof(s_gbc_ring));
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	s_gbc_widx = GBC_RING_FRAMES / 4;	/* ~85 ms of lead over the DMA */
	s_rs_accl = s_rs_accr = 0;
	s_rs_n = 0;
	s_rs_phase = 0;

	/* full codec/AFE bring-up, memif looping over the ring */
	spm_mtcmos_ctrl_audio(STA_POWER_ON);
	mt6359_hp_on();
	afe_dl1_setup((unsigned int)(addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	mdelay(2);
	spk_amp_enable(1);
	s_gbc_audio_on = 1;
}

/* Write `frames` interleaved stereo s16 samples that are ALREADY at 48 kHz
 * straight into the ring (no resample). Used by the SNES menu mixer, which
 * mixes at the AFE rate itself. Volume is applied here from s_gbc_vol. */
void ayaneo_snes_audio_submit(const short *stereo, unsigned frames)
{
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned i;

	if (!s_gbc_audio_on || s_gbc_paused)
		return;

	for (i = 0; i < frames; i++) {
		int l = stereo[i * 2 + 0];
		int r = stereo[i * 2 + 1];
		unsigned idx;
		if (q < 256) { l = (l * (int)q) >> 8; r = (r * (int)q) >> 8; }
		idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
		s_gbc_ring[idx * 2 + 0] = (short)l;
		s_gbc_ring[idx * 2 + 1] = (short)r;
		s_gbc_widx++;
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}

/* Resample `count` stereo samples (each u32 = L|R<<16 at 2097152 Hz) to 48 kHz
 * and write them into the ring. Volume from AYANEO_AUDIO_VOLUME. */
void ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count)
{
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned i;

	if (!s_gbc_audio_on || s_gbc_paused)
		return;

	for (i = 0; i < count; i++) {
		int l = (short)(samples[i] & 0xffff);
		int r = (short)(samples[i] >> 16);

		s_rs_accl += l;
		s_rs_accr += r;
		s_rs_n++;
		s_rs_phase += GBC_DST_HZ;
		if (s_rs_phase >= GBC_SRC_HZ) {
			int ol, or_;
			unsigned idx;

			s_rs_phase -= GBC_SRC_HZ;
			ol  = (int)(s_rs_accl / (long long)s_rs_n);
			or_ = (int)(s_rs_accr / (long long)s_rs_n);
			s_rs_accl = s_rs_accr = 0;
			s_rs_n = 0;
			if (q < 256) { ol = (ol * (int)q) >> 8; or_ = (or_ * (int)q) >> 8; }
			idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
			s_gbc_ring[idx * 2 + 0] = (short)ol;
			s_gbc_ring[idx * 2 + 1] = (short)or_;
			s_gbc_widx++;
		}
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}
