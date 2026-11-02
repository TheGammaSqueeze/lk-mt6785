/*
 * Battery gauge for the Konkr: a sample of the pack voltage and the PMIC fuel gauge current,
 * the stock pack profile (open circuit voltage and internal resistance against charge drawn)
 * to turn the voltage under load into a state of charge, a slow filter so the figure walks
 * rather than jumps, and a time estimate from the average current.
 *
 * All sampling happens on the LED thread (ayaneo_gauge_sample every few seconds), the cores
 * and menus only read the cached results. The auxadc and the pwrap reads block for a few ms
 * each, so they never run on a frame.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <platform/upmu_common.h>
#include <platform/upmu_hw.h>
#include <platform/mt_gpt.h>

extern int get_bat_sense_volt(int times);
extern unsigned int gpt4_get_current_tick(void);
extern unsigned int pmic_upmu_get_rgs_chrdet(void);

#include "ayaneo_gauge_profile.inc"

/* MT6359 fuel gauge current: the same conversion as the platform gauge code (unit 381470
 * = 0.1 mA per LSB x 1e5 at 10 mOhm sense, scaled to the board's 5 mOhm sense resistor,
 * R_FG 50 in 0.1 mOhm). Result in mA, positive while discharging, negative while charging. */
#define UNIT_FGCURRENT_MT6359 381470LL
#define R_FG_01MOHM 50
static int fg_current_ma(int *ok)
{
	unsigned int raw, n = 0;
	long long v;
	pmic_set_register_value(PMIC_FG_SW_READ_PRE, 1);
	while (pmic_get_register_value(PMIC_FG_LATCHDATA_ST) == 0 && ++n < 1000) ;
	raw = pmic_get_register_value(PMIC_FG_CURRENT_OUT);
	pmic_set_register_value(PMIC_FG_SW_CLEAR, 1);
	pmic_set_register_value(PMIC_FG_SW_READ_PRE, 0);
	n = 0;
	while (pmic_get_register_value(PMIC_FG_LATCHDATA_ST) != 0 && ++n < 1000) ;
	pmic_set_register_value(PMIC_FG_SW_CLEAR, 0);
	*ok = n < 1000;
	v = (raw > 32767u) ? (long long)raw - 65535LL : (long long)raw;   /* two's complement, charging positive at the chip */
	v = v * UNIT_FGCURRENT_MT6359 / 100000LL;                           /* 0.1 mA at 10 mOhm */
	v = v * 100 / R_FG_01MOHM;                                          /* the board's sense resistor */
	return (int)(-v / 10);                                              /* mA, discharge positive */
}

/* charge drawn (0.1 mAh) for an open circuit voltage, by linear interpolation in the profile */
static unsigned int drawn_for_ocv(int ocv_01mv)
{
	int i;
	if (ocv_01mv >= s_prof_ocv[0]) return 0;
	if (ocv_01mv <= s_prof_ocv[GAUGE_ROWS - 1]) return GAUGE_QMAX_01MAH;
	for (i = 0; i < GAUGE_ROWS - 1; i++) {
		int v0 = s_prof_ocv[i], v1 = s_prof_ocv[i + 1];
		if (ocv_01mv <= v0 && ocv_01mv > v1) {
			unsigned int q0 = s_prof_mah[i], q1 = s_prof_mah[i + 1];
			return q0 + (unsigned int)((long long)(v0 - ocv_01mv) * (long long)(q1 - q0) / (v0 - v1));
		}
	}
	return GAUGE_QMAX_01MAH;
}
/* internal resistance (0.1 mOhm) at a charge drawn, nearest row */
static unsigned int resistance_at(unsigned int drawn_01mah)
{
	int i;
	for (i = 0; i < GAUGE_ROWS - 1; i++)
		if (drawn_01mah < s_prof_mah[i + 1]) return s_prof_r[i];
	return s_prof_r[GAUGE_ROWS - 1];
}

/* cached results */
static volatile int s_vbat_mv, s_cur_ma, s_ocv_mv, s_cur_ok;
static volatile int s_soc_raw_x10;      /* from the compensated voltage, tenths of a percent */
static volatile int s_soc_filt_x10;     /* the tracked figure: coulomb count steered by the voltage estimate */
static int s_soc_cc_x1000;              /* the same in thousandths, the integration variable */
static unsigned int s_cc_last_tick;
static volatile int s_pct_shown = -1;   /* the monotonic figure the menus show */
static volatile int s_cur_avg_ma;       /* average draw (mA), for the time estimate */
static volatile int s_chr_prev = -1;
static volatile unsigned int s_samples;
static volatile unsigned int s_cur_n;     /* current samples since the charger last changed */
volatile unsigned int g_dbg_gauge_raw;  /* "7h 20m left", "1h 05m to full", "" when unknown; returns the length */
int ayaneo_gauge_time_str(char *out, int n)
{
	int tf = 0, m = ayaneo_gauge_minutes(&tf);
	if (m < 0 || n < 20) { if (n) out[0] = 0; return 0; }
	if (m > 99 * 60 + 59) m = 99 * 60 + 59;
	return snprintf(out, n, "%dh %02dm %s", m / 60, m % 60, tf ? "to full" : "left");
}

/* oem gauge */

/* The pack reads about 3 % high at the top (the stock gauge shows 100 at a real 97, its
 * PSEUDO100 is 97): map so a 97 % real charge shows as 100. */
#define GAUGE_PSEUDO100 97

/* history: one entry per minute (time in s since boot, vbat, current, filtered %), the last
 * GAUGE_HIST entries, so a spell on battery can be reviewed after the cable is back */
#define GAUGE_HIST 120
static struct { unsigned int t_s; short vbat, cur; signed char pct, chr; } s_hist[GAUGE_HIST];
static unsigned int s_hist_n, s_hist_last_s;
static unsigned int s_last_tick;
static unsigned int s_up_ms, s_up_tick;   /* running uptime: the 13 MHz counter wraps every 330 s */

int ayaneo_gauge_sample(void)
{
	int chr = pmic_upmu_get_rgs_chrdet() ? 1 : 0;
	int ok = 0, cur, vbat, ocv, soc_x10;
	long sum = 0; int i;
	unsigned int drawn, r, now = gpt4_get_current_tick();

	/* the menus and the charging screen call this freely: one real sample per 2 s is plenty
	 * (each costs a few ms of auxadc and pwrap on the calling thread) */
	if (s_samples && now - s_last_tick < 26000000u) return ayaneo_gauge_pct();
	s_last_tick = now;
	if (s_samples) s_up_ms += (now - s_up_tick) / 13000u;   /* wrap safe: samples come at least every 10 s */
	s_up_tick = now;

	for (i = 0; i < 4; i++) sum += get_bat_sense_volt(1);
	vbat = (int)(sum / 4);
	cur = fg_current_ma(&ok);
	if (!ok) cur = s_cur_ma;   /* keep the previous reading */
	/* open circuit voltage: the terminal voltage plus the drop over the internal resistance
	 * (a discharge current lowers the terminal voltage, a charge current raises it) */
	/* The profile's resistance column (1450 and up) overshoots on this pack: 30 minutes of fast
	 * forward at 500 mA read 4.283 V and the correction pushed the figure above the top of the
	 * table (100 % all along). Two readings a minute apart (4283 mV at 518 mA, 4277 mV at
	 * 167 mA) put the effective resistance near 20 mOhm; a flat 25 mOhm is used. */
	drawn = drawn_for_ocv(vbat * 10);
	r = resistance_at(drawn); (void)r;
	ocv = vbat + (int)((long long)cur * 25LL / 1000LL);
	drawn = drawn_for_ocv(ocv * 10);
	soc_x10 = (int)((long long)(GAUGE_QMAX_01MAH - drawn) * 1000LL / GAUGE_QMAX_01MAH);
	soc_x10 = soc_x10 * 100 / GAUGE_PSEUDO100;
	if (soc_x10 > 1000) soc_x10 = 1000;
	if (soc_x10 < 0) soc_x10 = 0;

	s_vbat_mv = vbat; s_cur_ma = cur; s_ocv_mv = ocv; s_cur_ok = ok; s_soc_raw_x10 = soc_x10;
	/* a charger change restarts the current average and releases the shown figure: the
	 * average must not pass through zero on the way from charge to discharge (an estimate
	 * divided by a near zero current read as days) */
	if (chr != s_chr_prev) { s_chr_prev = chr; s_pct_shown = -1; s_cur_n = 0; }
	/* Coulomb counting steered by the voltage: the fuel gauge current integrated over the
	 * sample interval moves the figure (500 mA on a 4000 mAh pack is 1 % every 4.8 minutes),
	 * and the open circuit estimate pulls it back slowly, harder when the current is small
	 * (then the voltage is trustworthy), so the two never drift apart for long. At the top of
	 * the curve the voltage alone is too flat to show the first percents going. */
	if (s_samples == 0) {
		s_soc_filt_x10 = soc_x10;
		s_soc_cc_x1000 = soc_x10 * 100;
		s_cc_last_tick = now;
	} else {
		unsigned int dt = now - s_cc_last_tick;                   /* 13 MHz ticks */
		int blend = (cur > -150 && cur < 150) ? 8 : 64;
		s_cc_last_tick = now;
		if (dt > 13000000u * 60u) dt = 13000000u * 60u;            /* a gap: count at most a minute */
		if (ok) {
			/* thousandths of a percent: mA * s * 100000 / (mAh * 3600) */
			long long d = (long long)cur * (long long)(dt / 13000u) * 100000LL / ((long long)GAUGE_QMAX_01MAH / 10 * 3600LL * 1000LL);
			s_soc_cc_x1000 -= (int)d;
		}
		s_soc_cc_x1000 += (soc_x10 * 100 - s_soc_cc_x1000) / blend;
		if (s_soc_cc_x1000 > 100000) s_soc_cc_x1000 = 100000;
		if (s_soc_cc_x1000 < 0) s_soc_cc_x1000 = 0;
		s_soc_filt_x10 = (s_soc_cc_x1000 + 50) / 100;
	}
	if (ok) {
		if (s_cur_n == 0) s_cur_avg_ma = cur;
		else if (s_cur_n < 4) s_cur_avg_ma += (cur - s_cur_avg_ma) / 2;   /* settle fast after a change */
		else s_cur_avg_ma += (cur - s_cur_avg_ma) / 8;                    /* then about a minute at 10 s */
		s_cur_n++;
	}
	s_samples++;
	{
		unsigned int t_s = s_up_ms / 1000u;
		if (s_hist_n == 0 || t_s - s_hist_last_s >= 60u) {
			unsigned int k = s_hist_n % GAUGE_HIST;
			s_hist[k].t_s = t_s; s_hist[k].vbat = (short)vbat; s_hist[k].cur = (short)cur;
			s_hist[k].pct = (signed char)((s_soc_filt_x10 + 5) / 10); s_hist[k].chr = (signed char)chr;
			s_hist_n++; s_hist_last_s = t_s;
		}
	}
	{
		int p = (s_soc_filt_x10 + 5) / 10;
		if (s_pct_shown < 0) s_pct_shown = p;
		else if (chr) { if (p > s_pct_shown) s_pct_shown = p; }
		else          { if (p < s_pct_shown) s_pct_shown = p; }
	}
	return s_pct_shown;
}

int ayaneo_gauge_pct(void)        { return s_pct_shown < 0 ? 50 : s_pct_shown; }
int ayaneo_gauge_current_ma(void) { return s_cur_ma; }
int ayaneo_gauge_vbat_mv(void)    { return s_vbat_mv; }
int ayaneo_gauge_ready(void)      { return s_samples > 0; }
/* let a caller that already knows the charger state (the KPOC screen) force the clamp direction */
void ayaneo_gauge_reset(void)     { s_pct_shown = -1; s_samples = 0; s_cur_n = 0; }
/* the charger reports the pack full: pin the count there */
void ayaneo_gauge_set_full(void)  { s_soc_cc_x1000 = 100000; s_soc_filt_x10 = 1000; if (s_pct_shown >= 0) s_pct_shown = 100; }

/* Minutes left on battery (discharging) or minutes to full (charging), -1 when unknown
 * (no current reading yet, or a draw too small to divide by). */
int ayaneo_gauge_minutes(int *to_full)
{
	int cur = s_cur_avg_ma, pct = ayaneo_gauge_pct();
	long long mah;
	if (s_cur_n < 3) return -1;                    /* three readings since the last charger change */
	if (cur > 20) {                                /* discharging */
		if (to_full) *to_full = 0;
		mah = (long long)GAUGE_QMAX_01MAH * GAUGE_PSEUDO100 / 100 * pct / 1000;   /* mAh usable left */
		return (int)(mah * 60 / cur);
	}
	if (cur < -20) {                               /* charging */
		if (to_full) *to_full = 1;
		mah = (long long)GAUGE_QMAX_01MAH * GAUGE_PSEUDO100 / 100 * (100 - pct) / 1000;
		return (int)(mah * 60 / (-cur));
	}
	return -1;
}

/* oem gauge */
int ayaneo_gauge_report(char *out, int n)
{
	int tf = 0, m = ayaneo_gauge_minutes(&tf);
	if (n < 0)   /* second line */
		return snprintf(out, -n, "gauge: shown=%d avg=%d mA min=%d%s n=%u/%u",
				s_pct_shown, s_cur_avg_ma, m, tf ? " to full" : "", s_samples, s_cur_n);
	return snprintf(out, n, "gauge: vbat=%d cur=%d%s ocv=%d raw=%d.%d filt=%d.%d",
			s_vbat_mv, s_cur_ma, s_cur_ok ? "" : "(stale)", s_ocv_mv, s_soc_raw_x10 / 10, s_soc_raw_x10 % 10,
			s_soc_filt_x10 / 10, s_soc_filt_x10 % 10);
}

/* oem gauge:hist - the history, oldest first, one entry per line through emit() */
void ayaneo_gauge_hist(void (*emit)(const char *line), char *buf, int n)
{
	unsigned int i, first = s_hist_n > GAUGE_HIST ? s_hist_n - GAUGE_HIST : 0;
	for (i = first; i < s_hist_n; i++) {
		unsigned int k = i % GAUGE_HIST;
		snprintf(buf, n, "%5us vbat=%d cur=%d pct=%d chr=%d", s_hist[k].t_s, s_hist[k].vbat, s_hist[k].cur, s_hist[k].pct, s_hist[k].chr);
		emit(buf);
	}
}
