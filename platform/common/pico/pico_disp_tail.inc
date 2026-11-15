/*
 * GammaOS Pico display, part 4 of 4: the write combined mapping itself (fb_wc_apply, oem fbwc),
 * and the display settings an LK without the emulator still keeps.
 */
/* Map the frame buffers and the game layer's buffers write combined (1) or write back (0). At
 * display init, and for oem fbwc by the pacer between frames (from rot_pace), when the frame just
 * written has been cleaned and nothing is drawing: the lines are written back and dropped, then
 * remapped, so no stale line is ever written over the display's data. Only for frame buffers the
 * LK maps at their physical address. ayaneo_fb_wc returns the state, or a negative error (no frame
 * buffer, not identity mapped, the map failed, no present ran to apply it). */
static volatile int s_fb_wc_res;
static void fb_wc_apply(void)
{
	extern void arch_clean_invalidate_cache_range(addr_t start, size_t len);
	extern int arch_mmu_map(uint64_t paddr, vaddr_t vaddr, uint flags, uint count);
	int on = s_fb_wc_req - 1;
	unsigned int base, end;
	s_fb_wc_req = 0;
	if (!fb_addr || !fb_size) { s_fb_wc_res = -1; g_dbg_fb_wc_res = s_fb_wc_res; return; }
	if ((unsigned int)fb_addr != (unsigned int)fb_addr_pa) { s_fb_wc_res = -2; g_dbg_fb_wc_res = s_fb_wc_res; return; }
	if (!on) g_fb_wc = 0;                     /* cleans resume before the mapping turns cached */
	{
		unsigned int r[3][2], i;
		r[0][0] = (unsigned int)fb_addr; r[0][1] = 2u * fb_size;
		r[1][0] = 0x55000000u; r[1][1] = fb_size;
		r[2][0] = 0x55900000u; r[2][1] = fb_size;
		for (i = 0; i < 3; i++) {
			base = r[i][0] & ~0xFFFu;
			end = (r[i][0] + r[i][1] + 0xFFFu) & ~0xFFFu;
			(arch_clean_invalidate_cache_range)(base, end - base);
			if (arch_mmu_map(base, base, (on ? MMU_MEMORY_TYPE_NORMAL : MMU_MEMORY_TYPE_NORMAL_WRITE_BACK) |
					 MMU_MEMORY_AP_P_RW_U_NA, end - base) != 0) { s_fb_wc_res = -3; g_dbg_fb_wc_res = s_fb_wc_res; return; }
		}
	}
	g_fb_wc = on ? 1 : 0;
	s_fb_wc_res = g_fb_wc;
	g_dbg_fb_wc_res = s_fb_wc_res;
}
int ayaneo_fb_wc(int on, unsigned int *va, unsigned int *pa, unsigned int *size)
{
	int i;
	if (va) *va = (unsigned int)fb_addr;
	if (pa) *pa = (unsigned int)fb_addr_pa;
	if (size) *size = fb_size;
	if (on < 0) return g_fb_wc;
	s_fb_wc_res = -4;
	s_fb_wc_req = (on ? 1 : 0) + 1;
	for (i = 0; i < 50 && s_fb_wc_req; i++) thread_sleep(10);
	return s_fb_wc_res;
}

#if !(defined(AYANEO_RAINBOW_BOOT) && (defined(AYANEO_GBC) || defined(AYANEO_GBA)))
/* An LK without the emulator (the plain Pocket Air Mini build): the shared settings code
 * (ayaneo_audio.c) still loads and stores the display settings, with nothing here to drive. */
volatile int g_show_fps;
volatile int g_gba_aspect;
volatile int g_gbc_aspect;
void ayaneo_set_overdrive(int on, int kup, int kdn, int thr, int cap)
{
	(void)on; (void)kup; (void)kdn; (void)thr; (void)cap;
}
void ayaneo_set_overdrive_caps(int up, int dn, int head)
{
	(void)up; (void)dn; (void)head;
}
void ayaneo_set_overdrive_curve(int kuphi, int kuplo, int gam)
{
	(void)kuphi; (void)kuplo; (void)gam;
}
#endif
