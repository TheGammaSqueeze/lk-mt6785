/*
 * Battery gauge for the Konkr Pocket Block, as on the Konkr Pocket Advance (the shared part is
 * platform/common/pico/ayaneo_gauge.inc): the MT6358 fuel gauge current and the Block's own pack
 * profile turn the pack voltage into a state of charge, coulomb counted between samples.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <platform/upmu_common.h>
#include <platform/upmu_hw.h>
#include <platform/mt_gpt.h>

extern int get_bat_sense_volt(int times);
extern unsigned int gpt4_get_current_tick(void);
extern unsigned int pmic_upmu_get_rgs_chrdet(void);

/* The Block's pack profile at 25 C from its device tree (battery0_profile_t1, TEMPERATURE_T1 = 25):
 * 100 rows of charge drawn in 0.1 mAh, open circuit voltage in 0.1 mV, internal resistance in
 * 0.1 mOhm */
#define GAUGE_ROWS 100
#define GAUGE_QMAX_01MAH 36400u   /* the profile's last row */
static const unsigned short s_prof_mah[GAUGE_ROWS] = {
	    0,   368,   735,  1103,  1471,  1838,  2206,  2574,  2941,  3309,
	 3677,  4044,  4412,  4780,  5147,  5515,  5883,  6251,  6618,  6986,
	 7354,  7721,  8089,  8457,  8824,  9192,  9560,  9927, 10295, 10663,
	11030, 11398, 11766, 12133, 12501, 12869, 13236, 13604, 13972, 14339,
	14707, 15075, 15442, 15810, 16178, 16545, 16913, 17281, 17648, 18016,
	18384, 18752, 19119, 19487, 19855, 20222, 20590, 20958, 21325, 21693,
	22061, 22428, 22796, 23164, 23531, 23899, 24267, 24634, 25002, 25370,
	25737, 26105, 26473, 26840, 27208, 27576, 27943, 28311, 28679, 29046,
	29414, 29782, 30149, 30517, 30885, 31253, 31620, 31988, 32356, 32723,
	33091, 33459, 33826, 34194, 34562, 34929, 35297, 35665, 36032, 36400,
};
static const unsigned short s_prof_ocv[GAUGE_ROWS] = {
	43954, 43547, 43395, 43260, 43134, 43014, 42892, 42771, 42653, 42532,
	42414, 42294, 42172, 42054, 41935, 41817, 41700, 41580, 41462, 41348,
	41234, 41120, 41006, 40895, 40782, 40674, 40566, 40460, 40354, 40251,
	40147, 40041, 39939, 39838, 39744, 39648, 39558, 39465, 39373, 39279,
	39187, 39093, 39005, 38916, 38834, 38754, 38680, 38610, 38544, 38482,
	38424, 38370, 38314, 38262, 38215, 38170, 38121, 38080, 38042, 38000,
	37960, 37924, 37889, 37856, 37823, 37788, 37758, 37730, 37701, 37674,
	37646, 37618, 37590, 37561, 37529, 37499, 37462, 37423, 37387, 37348,
	37302, 37257, 37209, 37151, 37088, 37022, 36952, 36880, 36813, 36763,
	36729, 36693, 36642, 36570, 36455, 36246, 35856, 35251, 34392, 33032,
};
static const unsigned short s_prof_r[GAUGE_ROWS] = {
	 1290,  1295,  1292,  1284,  1278,  1280,  1277,  1271,  1269,  1264,
	 1264,  1255,  1252,  1255,  1255,  1249,  1250,  1235,  1232,  1232,
	 1232,  1232,  1226,  1231,  1225,  1225,  1222,  1216,  1218,  1221,
	 1220,  1215,  1217,  1218,  1216,  1209,  1216,  1218,  1218,  1218,
	 1218,  1217,  1215,  1218,  1218,  1217,  1218,  1225,  1225,  1225,
	 1225,  1229,  1223,  1218,  1223,  1225,  1223,  1231,  1239,  1233,
	 1247,  1247,  1247,  1249,  1255,  1253,  1253,  1263,  1264,  1272,
	 1280,  1284,  1288,  1286,  1285,  1299,  1296,  1295,  1302,  1303,
	 1310,  1311,  1319,  1327,  1325,  1328,  1337,  1346,  1355,  1364,
	 1381,  1401,  1410,  1427,  1441,  1460,  1490,  1558,  1684,  1890,
};

/* MT6358 fuel gauge current, the conversion the platform gauge code uses (mt_gauge.c): 381470 per
 * LSB in 0.1 uA steps at a 10 mOhm sense, scaled to the board's 5 mOhm (R_FG_VALUE) and its
 * CAR_TUNE_VALUE of 98 %. Result in mA, positive while discharging, negative while charging. */
#define UNIT_FGCURRENT_MT6358 381470LL
#define R_FG_01MOHM 50
#define CAR_TUNE_PCT 98
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
	v = v * UNIT_FGCURRENT_MT6358 / 100000LL;                           /* 0.1 mA at 10 mOhm */
	v = v * 100 / R_FG_01MOHM;                                          /* the board's sense resistor */
	v = v * CAR_TUNE_PCT / 100;
	return (int)(-v / 10);                                              /* mA, discharge positive */
}

/* the Block's device tree keeps PSEUDO100 at 100: the profile's top is the full charge */
#define GAUGE_PSEUDO100 100
/* the effective resistance between the terminal voltage and the open circuit one, measured on
 * this console (block_gauge notes in the commit) */
#define GAUGE_R_EFF_MOHM 25

#include "../common/pico/ayaneo_gauge.inc"
