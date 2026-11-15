/*
 * GammaOS Pico: one panel command sent while the link runs in video mode (included by each
 * board's ddp_dsi.c after its command helpers).
 */
/* One panel command while the link runs in video mode, actually sent. DSI_set_cmdq_V2's video
 * branch only loads VM_CMD_CON and never sets VM_CMD_EN, and a second call overwrites the first
 * before anything left: live writes after boot never reached the panel. Here: load the packet
 * (DCS ids below 0xB0, generic above, as the init table is sent), enable it for the vertical
 * front porch, wait for VM_CMD_DONE, clear the flag and disable. Up to 15 parameter bytes (the
 * VM data window is 16). Returns 0 when sent, -1 on a timeout (about 50 ms, three refreshes). */
int ayaneo_dsi_vm_cmd(unsigned char cmd, unsigned int n, const unsigned char *p)
{
	volatile unsigned int *con = (volatile unsigned int *)(DSI_REG_BASE[0] + DISP_REG_DSI_VM_CMD_CON);
	volatile unsigned int *sta = (volatile unsigned int *)(DSI_REG_BASE[0] + DISP_REG_DSI_INTSTA);
	volatile unsigned int *dat = (volatile unsigned int *)(DSI_REG_BASE[0] + DISP_REG_DSI_VM_CMD_DATA0);
	unsigned int v, id, i, t0;
	extern unsigned int gpt4_get_current_tick(void);
	if (n > 15) return -1;
	if (n > 1) {
		unsigned char buf[16] = {0};
		buf[0] = cmd;
		for (i = 0; i < n; i++) buf[1 + i] = p[i];
		for (i = 0; i < 4; i++)
			dat[i] = buf[i * 4] | (buf[i * 4 + 1] << 8) | (buf[i * 4 + 2] << 16) | ((unsigned int)buf[i * 4 + 3] << 24);
		id = (cmd < 0xB0) ? 0x39 : 0x29;                       /* DCS / generic long write */
		v = (1u << 1) | (id << 8) | ((n + 1u) << 16);          /* LONG_PKT, word count */
	} else {
		if (cmd < 0xB0) id = n ? 0x15 : 0x05; else id = n ? 0x23 : 0x13;
		v = (id << 8) | ((unsigned int)cmd << 16) | ((unsigned int)(n ? p[0] : 0) << 24);
	}
	/* the kernel driver's order (mtk_dsi_vm_cmdq + the GCE send): load the packet with
	 * VM_CMD_EN and TS_VFP_EN, clear VM_CMD_DONE, then pulse VM_CMD_START (DSI_START bit 16) */
	*con = v | (1u << 5) | 1u;
	*sta = *sta & ~(1u << 5);
	{
		volatile unsigned int *start = (volatile unsigned int *)(DSI_REG_BASE[0] + 0x00);
		*start = *start & ~(1u << 16);
		*start = *start | (1u << 16);
	}
	t0 = gpt4_get_current_tick();
	while (!(*sta & (1u << 5))) {
		if (gpt4_get_current_tick() - t0 > 650000u) { volatile unsigned int *start = (volatile unsigned int *)(DSI_REG_BASE[0] + 0x00); *start = *start & ~(1u << 16); return -1; }
	}
	*sta = *sta & ~(1u << 5);
	{ volatile unsigned int *start = (volatile unsigned int *)(DSI_REG_BASE[0] + 0x00); *start = *start & ~(1u << 16); }
	return 0;
}
