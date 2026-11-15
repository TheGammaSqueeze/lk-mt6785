/*
 * GammaOS Pico display, part 2 of 4: the frame buffers and the game layer's buffers mapped write
 * combined, and the cache clean every later line of the including mt_disp_drv.c goes through
 * (arch_clean_cache_range becomes disp_clean_range). Needs the file's fb_addr and fb_size.
 */
/* The panel's two frame buffers and the game layer's two buffers (rsz present, RSZ_BUF0/1) are
 * mapped write combined on the Konkr (fb_wc_apply, at display init; oem fbwc switches it for A/B
 * checks): the display reads what the CPU wrote with no cache to clean, so a clean of them is
 * skipped, which took 170 us of every game frame at 1999 MHz. The CPU writes them in long runs
 * (no slower uncached); the transitions and the Pico overlay that read some back were checked on
 * the device. Every clean in this file goes through here; other ranges are cleaned as always. */
volatile int g_fb_wc, g_dbg_fb_wc_boot, g_dbg_fb_wc_res = 99;
static volatile int s_fb_wc_req;
static void fb_wc_apply(void);
static int disp_wc_range(addr_t start, size_t len)
{
	addr_t fb0 = (addr_t)fb_addr, fb1 = fb0 + 2u * fb_size;
	if (start >= fb0 && start + len <= fb1) return 1;
	/* the game layer's two buffers (ayaneo_rsz_present RSZ_BUF0 / RSZ_BUF1) */
	if (start >= 0x55000000u && start + len <= 0x55000000u + fb_size) return 1;
	if (start >= 0x55900000u && start + len <= 0x55900000u + fb_size) return 1;
	return 0;
}
static void disp_clean_range(addr_t start, size_t len)
{
	if (g_fb_wc && disp_wc_range(start, len))
		return;
	(arch_clean_cache_range)(start, len);
}
#define arch_clean_cache_range(a, n) disp_clean_range((addr_t)(a), (size_t)(n))
