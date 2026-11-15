/*
 * GammaOS Pico DSI helpers (platform/common/pico): the live vertical front porch and MIPI rate,
 * the refresh read back from them, the scan phase and line counts the presenter paces against,
 * and the porch each core asks for. Included as text by each board's ddp_dsi.c, which defines the
 * board's line constant (AYANEO_DSI_LINE_CONST, AYANEO_DSI_FIXED_LINES, AYANEO_DSI_VFP_MAX, the
 * MIPI rates) above it and has the DSI and MIPI TX register maps.
 */
/* AYANEO: live vertical-front-porch change for PER-CORE refresh switching. In continuous
 * VDO mode the DSI engine re-reads DSI_VFP_NL each frame, so writing it changes vtotal
 * (hence the panel refresh) at the next frame boundary - MTK's dynamic-fps mechanism, no
 * DSI reset needed. The SNES session runs the panel near its native 60.0988 Hz (vfp 17 ->
 * vtotal 993 -> ~60.11 Hz, 0.02% off) so a vsync-locked present is smooth; GB/GBC/GBA and
 * the menu restore the stock vfp 23 (vtotal 999 -> 59.749 Hz, ~= the 59.7275 GB/GBA rate).
 * Single DSI (DSI0) panel. Clamped to a safe porch range. */
void ayaneo_dsi_set_vfp(unsigned int vfp)
{
	if (vfp < 4u)   vfp = 4u;
	if (vfp > AYANEO_DSI_VFP_MAX) vfp = AYANEO_DSI_VFP_MAX;   /* pairmini 256: was 200 (50.75Hz floor);
	                                 256 -> vtotal 1232 -> ~48.4Hz floor, clears worst-case PAL Genesis
	                                 vfp 225 (non-shipped const) with margin. Konkr: see the define. */
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL, vfp);
}

/* MIPI data rate, live: rewrite the DSI PLL fractional PCW (posdiv/pcw_ratio stay in the
 * 250-500 MHz band, ratio 8) and pulse SDM_PCW_CHG so the SDM takes the new word. Every line
 * then takes 1/rate longer, which lowers the refresh with the porches untouched. Experiment
 * knob (oem dr:KHZ) for a 50 Hz PAL mode on the Konkr; 0 reads back nothing and returns 0. */
#if defined(KONKR_KPA)
volatile unsigned int g_dsi_dr_khz = 354000u;   /* the live MIPI rate: the stock 354 Mbps until ayaneo_dsi_retime */
#else
volatile unsigned int g_dsi_dr_khz = AYANEO_GBA_DR_KHZ;   /* the live MIPI rate (refresh scales with it) */
#endif
unsigned int ayaneo_dsi_set_dr_khz(unsigned int khz)
{
	unsigned int num, r0, r1, r2, pcw, tmp, ratio, posdiv;
	if (khz < 250001u || khz > 999999u) return 0;
	if (khz == g_dsi_dr_khz) return 1;
	if (khz >= 500000u) { ratio = 4u; posdiv = 2u; } else { ratio = 8u; posdiv = 3u; }
	num = khz * ratio;
	r0 = num % 26000u; r1 = (256u * r0) % 26000u; r2 = (256u * r1) % 26000u;
	pcw = num / 26000u;
	tmp = ((pcw & 0xFF) << 24) | (((256u * r0 / 26000u) & 0xFF) << 16)
	    | (((256u * r1 / 26000u) & 0xFF) << 8) | ((256u * r2 / 26000u) & 0xFF);
	MIPITX_OUTREG32(DSI_PHY_REG[0] + MIPITX_PLL_CON0, tmp);
	MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON1, FLD_RG_DSI_PLL_POSDIV, posdiv);
	MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON1, FLD_RG_DSI_PLL_SDM_PCW_CHG, 1);
	MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON1, FLD_RG_DSI_PLL_SDM_PCW_CHG, 0);
	g_dsi_dr_khz = khz;
	return tmp;
}
#if defined(KONKR_KPA)
/* GammaOS takes over the running panel (st7123_gammaos_timing, from the SD gate): the PHY timings
 * and the spread spectrum depth worked out for the new PLL clock, the vertical sync, back porch and
 * front porch, then the MIPI rate. All of them take effect live (measured: the PHY timing words
 * change the line length at once), so no DSI restart is needed. The DSI context keeps the new values,
 * so anything that reads them later sees the timing in use.
 *
 * The spread spectrum matters for the refresh: its depth is a fixed PCW step worked out from the boot
 * rate (DSI_DPHY_clk_setting, delta1 = 2), so the 354 Mbps step left in place at 478 Mbps spreads
 * 1.48% instead of 2% and the average clock ran 0.26% fast (59.884 Hz where the boot at PLL 239 gave
 * 59.7275). Rewritten for the new clock it is the register state the line constant was measured on.
 * The new depth is only taken cleanly while the spread spectrum is off: written while it ran, the
 * refresh landed anywhere from 59.732 to 59.785 Hz from one boot to the next, and a 10 us off pulse
 * did not reset it. Off for the whole retime plus 1 ms (about 30 modulation periods of 0x1B1 reference
 * clocks), then on: 59.7276 Hz, the boot at PLL 239's 59.7276. Later live rate changes
 * (ayaneo_dsi_set_dr_khz) keep the depth, as they always did. */
void DSI_PHY_TIMCONFIG(DISP_MODULE_ENUM module, void* cmdq, LCM_DSI_PARAMS *dsi_params);
void ayaneo_dsi_retime(unsigned int vsa, unsigned int vbp, unsigned int vfp, unsigned int pll_clock)
{
	LCM_DSI_PARAMS *p = &_dsi_context[0].dsi_params;
	p->PLL_CLOCK = pll_clock;
	p->vertical_sync_active = vsa;
	p->vertical_backporch = vbp;
	p->vertical_frontporch = vfp;
	if (p->ssc_disable != 1)
		MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON2, FLD_RG_DSI_PLL_SDM_SSC_EN, 0);
	DSI_PHY_TIMCONFIG(DISP_MODULE_DSI0, NULL, p);
	if (p->ssc_disable != 1) {   /* the boot's formula (DSI_DPHY_clk_setting) for the new clock */
		unsigned int dr = pll_clock * 2u;
		unsigned int ratio = dr >= 1000u ? 2u : dr >= 500u ? 4u : 8u;
		unsigned int delta1 = p->ssc_range ? p->ssc_range : 2u;
		unsigned int pdelta1 = (delta1 * (dr / 2u) * ratio * 262144u + 281664u) / 563329u;
		MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON3, FLD_RG_DSI_PLL_SDM_SSC_DELTA, pdelta1);
		MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON3, FLD_RG_DSI_PLL_SDM_SSC_DELTA1, pdelta1);
	}
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VSA_NL, vsa);
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VBP_NL, vbp);
	DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL, vfp);
	ayaneo_dsi_set_dr_khz(AYANEO_GBA_DR_KHZ);
	if (p->ssc_disable != 1) {
		udelay(1000);
		MIPITX_OUTREGBIT(DSI_PHY_REG[0] + MIPITX_PLL_CON2, FLD_RG_DSI_PLL_SDM_SSC_EN, 1);
	}
}
#endif
/* oem dsitime: the timing the DSI is running now (stock on an Android boot, GammaOS's once the SD
 * gate retimed it), in lines short enough for a fastboot INFO packet: 0 the vertical line counts,
 * the PLL clock and the rate, 1 the PHY timing words, 2 the PLL word and the spread spectrum depth. */
int ayaneo_dsi_timing_report(int line, char *out, int n)
{
	unsigned int b = DSI_REG_BASE[0];
	if (line == 0)
		return snprintf(out, n, "dsi: vsa=%u vbp=%u vfp=%u pll=%u rate=%u kHz",
				AS_UINT32(b + DISP_REG_DSI_VSA_NL), AS_UINT32(b + DISP_REG_DSI_VBP_NL),
				AS_UINT32(b + DISP_REG_DSI_VFP_NL), _dsi_context[0].dsi_params.PLL_CLOCK, g_dsi_dr_khz);
	if (line == 1)
		return snprintf(out, n, "timcon %08x %08x %08x %08x",
				AS_UINT32(b + DISP_REG_DSI_PHY_TIMCON0), AS_UINT32(b + DISP_REG_DSI_PHY_TIMCON1),
				AS_UINT32(b + DISP_REG_DSI_PHY_TIMCON2), AS_UINT32(b + DISP_REG_DSI_PHY_TIMCON3));
	return snprintf(out, n, "pll %08x %08x ssc %08x %08x", AS_UINT32(DSI_PHY_REG[0] + MIPITX_PLL_CON0),
			AS_UINT32(DSI_PHY_REG[0] + MIPITX_PLL_CON1), AS_UINT32(DSI_PHY_REG[0] + MIPITX_PLL_CON2),
			AS_UINT32(DSI_PHY_REG[0] + MIPITX_PLL_CON3));
}
/* the line constant for the live rate: lines per second scale with the MIPI rate */
static unsigned int dsi_line_const(void)
{
	return (unsigned int)(((unsigned long long)AYANEO_DSI_LINE_CONST * g_dsi_dr_khz + AYANEO_DSI_REF_DR_KHZ / 2u) / AYANEO_DSI_REF_DR_KHZ);
}
/* the MIPI rate that puts the panel on refresh_milli at vtotal lines per refresh */
static unsigned int dsi_dr_for_refresh(unsigned int refresh_milli, unsigned int vtotal)
{
	/* in half milli-Hz: the GBA's 59.7275 Hz is carried as 59727 everywhere, the half is put back */
	unsigned long long n = ((unsigned long long)refresh_milli * 2u + (refresh_milli == 59727u)) * AYANEO_DSI_REF_DR_KHZ * vtotal;
	unsigned int khz = (unsigned int)((n + AYANEO_DSI_LINE_CONST) / (2ull * AYANEO_DSI_LINE_CONST));
	/* measured on the device (build88): the 500-1000 MHz band (ratio 4, posdiv 2) runs the panel
	 * 0.165% slower than the same formula gives in the 250-500 band (119.25 Hz asked as 119.45) */
	if (khz >= 500000u) khz = (unsigned int)(((unsigned long long)khz * 1001650u + 500000u) / 1000000u);
	return khz;
}

/* Horizontal front porch word count, live (VDO mode re-reads it per line): a longer line at the
 * same data rate lowers the refresh without a longer vertical blank. Experiment knob for a true
 * 50 Hz PAL mode on the Konkr (the long vertical porch faded the panel). Returns the old value. */
unsigned int ayaneo_dsi_set_hfp_wc(unsigned int wc)
{
	unsigned int old = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_HFP_WC);
	if (wc) DSI_OUTREG32(NULL, DSI_REG_BASE[0] + DISP_REG_DSI_HFP_WC, wc & 0xFFFu);
	return old;
}
void ayaneo_dsi_line_words(unsigned int *hsa, unsigned int *hbp, unsigned int *hfp, unsigned int *ps)
{
	*hsa = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_HSA_WC);
	*hbp = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_HBP_WC);
	*hfp = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_HFP_WC);
	*ps  = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_PSCON);
}

/* Where the scan is in video mode (STATE_DBG7 bits 12..15): 1 front porch, 2 active lines, 4 back
 * porch, 8 sync. The next frame's display settings are taken when the front porch ends, so the
 * pacer checks for 1 before it hands over a frame after the frame end (mt_disp_drv.c). */
unsigned int ayaneo_dsi_scan(void)
{
	return (AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_STATE_DBG7) >> 12) & 0xFu;
}
/* the live vertical front porch and total lines per refresh */
void ayaneo_dsi_vlines(unsigned int *vfp, unsigned int *vtotal)
{
	unsigned int f = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL);
	*vfp = f;
	*vtotal = AYANEO_DSI_FIXED_LINES + f;
}

/* Read back the live DSI vertical-front-porch (drives vtotal, hence the panel refresh in
 * VDO mode). Used to validate the per-core LCM refresh switch: 23 = menu/GB/GBC/GBA
 * (59.749 Hz), 17 = SNES (60.11 Hz). */
unsigned int ayaneo_dsi_get_vfp(void)
{
	return AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL);
}

/* Panel refresh rate in milli-Hz (e.g. 59749 = 59.749 Hz), derived from the live vfp. In VDO mode
 * vtotal = VSA+VBP+VACTIVE+VFP; on this st7703_hd720 timing the non-vfp lines sum to 976 and the line
 * rate is ~59.684 kHz (calibrated to the two documented points: vfp 23 -> vtotal 999 -> 59.749 Hz,
 * vfp 17 -> vtotal 993 -> 60.11 Hz). Read-only; the per-core LCM refresh switch will build on this. */
unsigned int ayaneo_dsi_refresh_milli(void)
{
	unsigned int vfp = AS_UINT32(DSI_REG_BASE[0] + DISP_REG_DSI_VFP_NL);
	unsigned int vtotal = AYANEO_DSI_FIXED_LINES + vfp;
	if (!vtotal) return 0;
	return dsi_line_const() / vtotal;   /* line_rate(milliHz*lines) / vtotal = refresh in milliHz */
}
/* the rate one emulated frame is shown at: the panel refresh over the vsync divider */
unsigned int ayaneo_dsi_core_refresh_milli(void)
{
	extern volatile unsigned int g_vsync_div;
	unsigned int d = g_vsync_div ? g_vsync_div : 1u;
	return ayaneo_dsi_refresh_milli() / d;
}

/* AYANEO: pick the vfp that lands the panel refresh on a target fps (milli-Hz) and apply it live.
 * vtotal = round(AYANEO_DSI_LINE_CONST / fps_milli); vfp = vtotal - 976 (non-vfp lines sum to 976 on
 * this st7703_hd720 timing). Rounding is (const + fps/2) / fps. The [4,256] clamp and the single
 * register write live in ayaneo_dsi_set_vfp(), which this calls, so the poke is never duplicated.
 * Real-time safe: in continuous VDO mode DSI_VFP_NL is re-read at the next frame boundary, no DSI
 * reset. Do NOT wait_for_vsync here (double-wait = 30fps). Returns the pre-clamp vfp so callers can
 * log it. Guards fps_milli==0. */
unsigned int ayaneo_dsi_set_fps_milli(unsigned int fps_milli)
{
	unsigned int line_const = dsi_line_const();
	unsigned int vtotal, vfp;
	if (fps_milli == 0u) return 0u;
	vtotal = (line_const + fps_milli / 2u) / fps_milli;   /* rounded */
	if (vtotal < AYANEO_DSI_FIXED_LINES + 4u) vtotal = AYANEO_DSI_FIXED_LINES + 4u;   /* keep vfp subtraction non-negative */
	vfp = vtotal - AYANEO_DSI_FIXED_LINES;
	ayaneo_dsi_set_vfp(vfp);   /* single write + [4,max] clamp */
	return vfp;
}

/* Pure form of the above for the cores: the vfp that puts the panel on fps_milli (no register
 * write, no clamp beyond the porch floor). Keeps the board timing in one place instead of each
 * core carrying its own copy of the line constant and the fixed line count. */
/* Vsync divider: the panel runs at g_vsync_div x the core rate and each emulated frame is
 * shown for that many refreshes (presents wait the extra vsyncs). 1 = panel at the core rate.
 * A 120 Hz class panel (the Konkr) is run at 2 so the porch stays near stock instead of a
 * 1500 line front porch. Runtime (oem vdiv:N) so both can be compared on the device. */
/* Konkr: 1 for the 60 Hz class cores (stock porch timing; 120 Hz made the BFI smear worse), 2 for a
 * core the porch cannot hold (PAL 50 Hz): the panel then runs at exactly twice the core rate from
 * the MIPI PLL with the porch at 4 lines. Set per core by ayaneo_dsi_vfp_for_fps. */
volatile unsigned int g_vsync_div = 1u;

/* The core frame rate the present pacer should hold (milli-Hz). Recorded by the two vfp
 * helpers below, which every core calls when it starts, so the pacer always knows the
 * rate behind the porch it is running. */
volatile unsigned int g_pace_fps_milli = 59727u;
unsigned int ayaneo_dsi_default_vfp(void);

unsigned int ayaneo_dsi_vfp_for_fps(unsigned int fps_milli)
{
	unsigned int vtotal, vfp;
	if (fps_milli == 0u) return 4u;
	g_pace_fps_milli = fps_milli;
#if defined(KONKR_KPA)
	if (fps_milli < AYANEO_DSI_1X_MIN_MILLI) {
		/* the blanking cannot stretch this far (PAL): panel at exactly 2 x the core rate from the
		 * PLL, every frame shown for two refreshes */
		g_vsync_div = 2u;
		ayaneo_dsi_set_dr_khz(dsi_dr_for_refresh(fps_milli * 2u, AYANEO_DSI_FIXED_LINES + AYANEO_DSI_VFP_2X));
		return AYANEO_DSI_VFP_2X;
	}
	/* the shorter scan: fixed lines, the data rate lands the refresh on the core rate */
	g_vsync_div = 1u;
	ayaneo_dsi_set_dr_khz(dsi_dr_for_refresh(fps_milli, AYANEO_DSI_SCAN_LINES));
	return AYANEO_DSI_SCAN_LINES - AYANEO_DSI_FIXED_LINES;
#else
	fps_milli *= (g_vsync_div ? g_vsync_div : 1u);
	vtotal = (AYANEO_DSI_LINE_CONST + fps_milli / 2u) / fps_milli;
#endif
	if (vtotal <= AYANEO_DSI_FIXED_LINES) return 4u;
	vfp = vtotal - AYANEO_DSI_FIXED_LINES;
	if (vfp > AYANEO_DSI_VFP_MAX)   /* porch the panel cannot hold: keep the resting porch, pace by time */
		return ayaneo_dsi_default_vfp();
	return vfp;
}

/* The resting vfp: the GBA's 59.7275 Hz, rounded to the nearest line at or above that rate (the
 * menu, GB/GBC and GBA all run here). 23 on the pairmini (59.749 Hz), 360 nominal on the Konkr. */
unsigned int ayaneo_dsi_default_vfp(void)
{
	/* floor the line count so the panel rests AT OR ABOVE the GBA rate (never slower than a
	 * real GBA); the pairmini value 23 (vtotal 999) is unchanged by this. */
#if defined(KONKR_KPA)
	/* the resting mode is the GBA rate, one refresh per frame (the boot rate and porch of the LCM):
	 * a PAL session's 50 Hz and 2x must not linger */
	return ayaneo_dsi_vfp_for_fps(59727u);
#else
	unsigned int fps = 59727u * (g_vsync_div ? g_vsync_div : 1u);
	unsigned int vtotal = AYANEO_DSI_LINE_CONST / fps;
	if (g_pace_fps_milli < 20000u || g_pace_fps_milli > 130000u) g_pace_fps_milli = 59727u;
	if (vtotal <= AYANEO_DSI_FIXED_LINES) return 4u;
	return vtotal - AYANEO_DSI_FIXED_LINES;
#endif
}
