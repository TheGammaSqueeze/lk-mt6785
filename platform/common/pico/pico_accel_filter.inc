/*
 * pico_accel_filter.inc - smoothing of the accelerometer for the cores that take the reading as a
 * cartridge's sensor would. Included by each console's accelerometer driver
 * (platform/mt6785/ayaneo_accel.c, platform/mt6768/block_accel.c); the driver clears
 * s_lp_primed when the chip starts measuring again.
 *
 * A reading is split in two: a slow part, an average that follows gravity as the console is
 * tilted, and a fast part, what is left (hand shake, the bump at the end of a quick tilt, a
 * flick). The fast part only passes beyond a dead zone, less the dead zone, so handling the
 * console no longer looks like a flick to a game while a deliberate flick still does. Both
 * settings can be changed live (oem accelf) while they are being tuned. Readings are in the
 * device frame at 2048 counts to a g.
 */
static long long s_lp[3];   /* ayaneo_accel_filter's average, counts << 8 */
static int s_lp_primed;

volatile unsigned int g_accel_smooth = 64;   /* weight of a new reading in the average, 256ths */
volatile unsigned int g_accel_flick = 900;   /* the fast part's dead zone, counts (2048 to a g) */

static int fast_part(int d)
{
	int z = (int)g_accel_flick;
	if (d > z) return d - z;
	if (d < -z) return d + z;
	return 0;
}

void ayaneo_accel_filter(int v[3])
{
	unsigned a = g_accel_smooth;
	int i;
	if (a < 1) a = 1;
	if (a > 256) a = 256;
	for (i = 0; i < 3; i++) {
		long long in = (long long)v[i] << 8;
		int slow;
		if (!s_lp_primed) s_lp[i] = in;
		else s_lp[i] += (in - s_lp[i]) * (long long)a / 256;
		slow = (int)(s_lp[i] / 256);
		v[i] = slow + fast_part(v[i] - slow);
	}
	s_lp_primed = 1;
}
