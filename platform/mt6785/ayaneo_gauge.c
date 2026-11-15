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

/* The pack reads about 3 % high at the top (the stock gauge shows 100 at a real 97, its
 * PSEUDO100 is 97): map so a 97 % real charge shows as 100. */
#define GAUGE_PSEUDO100 97
/* The profile's resistance column (1450 and up) overshoots on this pack: 30 minutes of fast
 * forward at 500 mA read 4.283 V and the correction pushed the figure above the top of the
 * table (100 % all along). Two readings a minute apart (4283 mV at 518 mA, 4277 mV at
 * 167 mA) put the effective resistance near 20 mOhm; a flat 25 mOhm is used. */
#define GAUGE_R_EFF_MOHM 25

#include "../common/pico/ayaneo_gauge.inc"
