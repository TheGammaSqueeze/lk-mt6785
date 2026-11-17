/*
 * pico_bt.inc - Bluetooth in the GammaOS Pico LK: the part every console with a MediaTek
 * connectivity subsystem (CONNSYS) shares. Included by the console's own file
 * (platform/mt6768/block_bt.c for the Konkr Pocket Block), which defines first:
 *
 *   R32(a), GPT_US(t)             register access, 13 MHz GPT ticks to microseconds
 *   PICO_BT_BTIF_BASE             the byte pipe's registers
 *   PICO_BT_IRQ_ID                its interrupt (GIC numbering, active low); the platform's
 *                                 interrupt dispatch calls pico_bt_irq() for it
 *   PICO_BT_CHIP_ID               the SoC id the firmware is told (0x6768)
 *   PICO_BT_MODEL                 the console model in the advertising (2 Block, 1 KPA)
 *   PICO_BT_FW_RAM_MCU/_BT/_WIFI, PICO_BT_FW_PATCH_MCU   the firmware file names
 *   BT_EMI_PA, BT_EMI_SIZE        the firmware's window of DRAM
 *   int pico_bt_plat_start(void)  supplies, power domain, the window's remap, the CPU out of
 *                                 reset: 0, 1 power on failed, 2 the CPU did not start
 *   void pico_bt_plat_off(void)   everything of its back off
 *   int pico_bt_plat_powered(void)
 *   bt_wait()                     a register poll with a deadline
 *
 * In here: the bring-up log, the firmware from the card into the window, the polled byte pipe
 * (BTIF), its framing (STP), the control commands (WMT) up to the Bluetooth function on, HCI,
 * the console's LE identity and advertising, the scan, and the diagnostics on the bootloader
 * channel. Bluetooth costs nothing until something asks for it: nothing here runs at boot.
 */
static char s_line[128];

/* ---- what is up ---- */
static int s_bt_up;			/* the whole bring-up done: HCI usable */

/* everything off (supplies, domain, clocks), from any state */
static void pico_bt_off(void)
{
	pico_bt_plat_off();
	s_bt_up = 0;
}

/* ---- a log of the bring-up, read back with oem btlog ---- */
#define BT_LOG_LINES	48
#define BT_LOG_LEN	60
static char s_log[BT_LOG_LINES][BT_LOG_LEN];
static unsigned int s_log_n;		/* lines written, the oldest dropped past BT_LOG_LINES */

static void bt_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_log[s_log_n % BT_LOG_LINES], BT_LOG_LEN, fmt, ap);
	va_end(ap);
	s_log_n++;
}

/* ---- the firmware's window of DRAM ----
 * CONNSYS reads its RAM images from DRAM it sees at 0xF0000000, which the remap register points
 * at a 16 MB page of the AP's DRAM. Android puts it at 0xBE000000, 4 MB (block_bt.h). The rewind
 * arena leaves it out (ayaneo_rewind.c); here it is checked to be DRAM the preloader marked usable. */
extern BOOT_ARGUMENT *g_boot_arg;
extern fat_vol *gba_sd_menu_vol(void);

#define BT_EMI_FW_VIEW		0xF0000000u	/* the window's address as CONNSYS sees it */
#define BT_FW_DIR		"/bios/connsys/"
#define BT_ROM_HDR		48		/* the "_hdr.bin" header before each RAM image (with its 4 CRC words) */
#define BT_BUILDTIME_BT		0x68300		/* where each image's build time is noted, as Android */
#define BT_BUILDTIME_WIFI	0x68310
#define BT_BUILDTIME_MCU	0x68320

static int s_emi_mapped;

/* 0 when the window is free DRAM the emulator never uses, and mapped (uncached) */
static int bt_emi_window(void)
{
	unsigned int i;
	int ok = 0;

	if (s_emi_mapped)
		return 0;
	if (!g_boot_arg)
		return -1;
	for (i = 0; i < g_boot_arg->mblock_info.mblock_num && i < 128u; i++) {
		unsigned long long s = g_boot_arg->mblock_info.mblock[i].start;
		unsigned long long e = s + g_boot_arg->mblock_info.mblock[i].size;
		if (s <= BT_EMI_PA && BT_EMI_PA + (unsigned long long)BT_EMI_SIZE <= e)
			ok = 1;
	}
	if (!ok) {
		bt_log("emi window %08x not usable DRAM", BT_EMI_PA);
		return -2;
	}
	if (arch_mmu_map((uint64_t)BT_EMI_PA, (vaddr_t)BT_EMI_PA,
			 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, BT_EMI_SIZE) != 0) {
		bt_log("emi map failed");
		return -3;
	}
	s_emi_mapped = 1;
	return 0;
}

static unsigned char s_bounce[4096];	/* card reads land here, then go to the window */

/* one RAM image from the card into the window at the offset its header names */
static int bt_load_ram_image(fat_vol *v, const char *name, unsigned int buildtime_off)
{
	char path[80];
	fat_file f;
	unsigned char hdr[BT_ROM_HDR];
	unsigned int view, off, size, pos;

	snprintf(path, sizeof path, "%s%s", BT_FW_DIR, name);
	if (fat_open(v, path, &f) != 0 || f.size <= BT_ROM_HDR) {
		bt_log("fw missing: %s", name);
		return -1;
	}
	if (fat_read(&f, 0, hdr, BT_ROM_HDR) != BT_ROM_HDR || memcmp(hdr + 16, "ALPS", 4) != 0) {
		bt_log("fw header bad: %s", name);
		return -2;
	}
	/* bytes 24..27: where CONNSYS sees the image, little endian; the low byte is a flag */
	view = (unsigned)hdr[0x19] << 8 | (unsigned)hdr[0x1a] << 16 | (unsigned)hdr[0x1b] << 24;
	off = view - BT_EMI_FW_VIEW;
	size = f.size - BT_ROM_HDR;
	if (view < BT_EMI_FW_VIEW || off + size > BT_EMI_SIZE) {
		bt_log("fw place bad: %s %08x", name, view);
		return -3;
	}
	for (pos = 0; pos < size; ) {
		unsigned int n = size - pos > sizeof s_bounce ? sizeof s_bounce : size - pos;
		if (fat_read(&f, BT_ROM_HDR + pos, s_bounce, n) != n) {
			bt_log("fw read failed: %s", name);
			return -4;
		}
		memcpy((void *)(addr_t)(BT_EMI_PA + off + pos), s_bounce, n);
		pos += n;
	}
	memcpy((void *)(addr_t)(BT_EMI_PA + buildtime_off), hdr, 16);
	bt_log("fw %s at +%06x %u bytes", name + 7, off, size);
	return 0;
}

/* ---- the byte pipe to CONNSYS (BTIF), polled ---- */
#define BTIF_BASE		PICO_BT_BTIF_BASE
#define BTIF_RBR		0x00		/* read: a received byte; write: a byte to send */
#define BTIF_IER		0x04
#define BTIF_FIFOCTRL		0x08		/* bit 2 clears the send FIFO, bit 1 the receive one */
#define BTIF_FAKELCR		0x0c
#define BTIF_LSR		0x14		/* bit 0 a byte waiting, bit 5 room to send, bit 6 all sent */
#define BTIF_SLEEP_EN		0x48
#define BTIF_DMA_EN		0x4c
#define BTIF_TRI_LVL		0x60
#define BTIF_WAK		0x64		/* bit 0: the wake line to CONNSYS */
#define BTIF_HANDSHAKE		0x6c
#define LSR_DR			(1u << 0)
#define LSR_THRE		(1u << 5)
#define LSR_TEMT		(1u << 6)

static void btif_init(void)
{
	unsigned int b = BTIF_BASE;
	R32(b + BTIF_FAKELCR) = 0;
	R32(b + BTIF_HANDSHAKE) |= 1u;
	R32(b + BTIF_FIFOCTRL) = 1u << 1;
	R32(b + BTIF_FIFOCTRL) = 0;
	R32(b + BTIF_FIFOCTRL) = 1u << 2;
	R32(b + BTIF_FIFOCTRL) = 0;
	R32(b + BTIF_TRI_LVL) = 8u | (1u << 4);	/* send level 8, receive level 1, no loop back */
	R32(b + BTIF_DMA_EN) = 1u << 2;		/* no DMA (polled), auto reset */
	R32(b + BTIF_IER) = 0;			/* polled: no interrupts */
	R32(b + BTIF_SLEEP_EN) = 0;
}

/* the wake line: low, a little longer than a 32 kHz period, high */
static void btif_wake(void)
{
	R32(BTIF_BASE + BTIF_WAK) &= ~1u;
	udelay(150);
	R32(BTIF_BASE + BTIF_WAK) |= 1u;
}

static int btif_put(const unsigned char *p, unsigned int n)
{
	unsigned int t0 = gpt4_get_current_tick();
	while (n) {
		if (R32(BTIF_BASE + BTIF_LSR) & LSR_THRE) {
			R32(BTIF_BASE + BTIF_RBR) = *p++;
			n--;
			t0 = gpt4_get_current_tick();
		} else if (GPT_US(gpt4_get_current_tick() - t0) > 20000) {
			return -1;
		}
	}
	return 0;
}

/* one received byte, or -1 after us microseconds without one */
static int btif_get(unsigned int us)
{
	unsigned int t0 = gpt4_get_current_tick(), w;
	while (!(R32(BTIF_BASE + BTIF_LSR) & LSR_DR)) {
		w = GPT_US(gpt4_get_current_tick() - t0);
		if (w > us)
			return -1;
		if (w > 200)		/* a longer wait: the CPU to the other threads meanwhile */
			thread_yield();
	}
	return (int)(R32(BTIF_BASE + BTIF_RBR) & 0xffu);
}

/* ---- STP: the framing on the pipe ----
 * A frame: [0x80 | seq << 3 | ack] [type << 4 | length >> 8] [length & 0xff] [check] payload
 * [crc low] [crc high]. In the simple ("mandatory") mode used until the firmware is set up the
 * sequence numbers, check byte and CRC are all zero; in full mode the check byte is the sum of
 * the first three, the CRC is CRC-16/ARC of the payload, and each side acknowledges the other's
 * frames (window of 7, 3 bit sequence numbers): the ack field names the last frame taken, 7
 * before the first. A frame of length 0 is an acknowledgement only. Full mode opens with four
 * 0x7f bytes from CONNSYS (its sync pattern), skipped here. */
#define STP_TYPE_BT		0
#define STP_TYPE_WMT		4
#define STP_MAX			2048

static int s_stp_full;			/* 0 simple mode, 1 full mode */
static unsigned int s_tx_seq;		/* our next sequence number */
static unsigned int s_rx_ack = 7;	/* the last of CONNSYS's frames we took, 7 = none yet */
static unsigned int s_rx_expect;	/* the sequence number its next new frame carries */
static unsigned int s_peer_ack = 7;	/* the last of our frames CONNSYS has, 7 = none yet */
static void stp_queue_one(unsigned int us);

static unsigned int crc16_arc(const unsigned char *p, unsigned int n)
{
	unsigned int crc = 0, i;
	while (n--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc & 1u) ? (crc >> 1) ^ 0xa001u : crc >> 1;
	}
	return crc & 0xffffu;
}

static int stp_send(unsigned int type, const unsigned char *p, unsigned int n)
{
	unsigned char h[4], c[2] = { 0, 0 };
	if (n > STP_MAX)
		return -1;
	if (s_stp_full) {
		unsigned int crc = crc16_arc(p, n), t0 = gpt4_get_current_tick();
		/* at most 6 of our frames unacknowledged (the window is 7): wait for CONNSYS's acks,
		 * keeping whatever it sends meanwhile */
		while (((s_tx_seq - ((s_peer_ack + 1u) & 7u)) & 7u) >= 6u) {
			if (GPT_US(gpt4_get_current_tick() - t0) > 500000u)
				return -3;
			stp_queue_one(20000);
		}
		h[0] = (unsigned char)(0x80u | (s_tx_seq << 3) | s_rx_ack);
		h[3] = 0;		/* filled below */
		c[0] = (unsigned char)crc;
		c[1] = (unsigned char)(crc >> 8);
		s_tx_seq = (s_tx_seq + 1u) & 7u;
	} else {
		h[0] = 0x80;
	}
	h[1] = (unsigned char)((type << 4) | ((n >> 8) & 0x0fu));
	h[2] = (unsigned char)n;
	h[3] = s_stp_full ? (unsigned char)(h[0] + h[1] + h[2]) : 0;
	if (btif_put(h, 4) || btif_put(p, n) || btif_put(c, 2))
		return -2;
	return 0;
}

/* in full mode: tell CONNSYS which of its frames we have */
static void stp_ack(void)
{
	unsigned char h[4];
	h[0] = (unsigned char)(0x80u | s_rx_ack);
	h[1] = 0;
	h[2] = 0;
	h[3] = h[0];
	btif_put(h, 4);
}

volatile unsigned int g_bt_rx_junk;	/* bytes dropped while looking for a frame start */
static unsigned char s_junk_first[16];	/* the first of them, for oem btlog */
volatile unsigned int g_bt_rx_dup;	/* frames CONNSYS sent again */

/* the next frame with a payload off the pipe; its type in *type, payload in buf (up to cap),
 * length returned, -1 after us microseconds without one */
static int stp_recv_raw(unsigned int *type, unsigned char *buf, unsigned int cap, unsigned int us)
{
	unsigned int t0 = gpt4_get_current_tick();
	for (;;) {
		unsigned char h[4];
		unsigned int len, i;
		int b;
		if (GPT_US(gpt4_get_current_tick() - t0) > us)	/* bytes that never make a frame */
			return -4;
		b = btif_get(us);
		if (b < 0)
			return -1;
		if ((b & 0xc0) != 0x80) {
			if (g_bt_rx_junk < 16)
				s_junk_first[g_bt_rx_junk] = (unsigned char)b;
			g_bt_rx_junk++;
			continue;
		}
		h[0] = (unsigned char)b;
		for (i = 1; i < 4; i++) {
			b = btif_get(10000);
			if (b < 0)
				return -2;
			h[i] = (unsigned char)b;
		}
		len = ((h[1] & 0x0fu) << 8) | h[2];
		if (s_stp_full && ((h[0] + h[1] + h[2]) & 0xff) != h[3]) {
			g_bt_rx_junk += 4;
			continue;
		}
		if (s_stp_full)
			s_peer_ack = h[0] & 7u;		/* the last of our frames CONNSYS has */
		if (len == 0)
			continue;			/* an acknowledgement of ours */
		for (i = 0; i < len + 2; i++) {		/* the payload and the CRC */
			b = btif_get(10000);
			if (b < 0)
				return -3;
			if (i < len && i < cap)
				buf[i] = (unsigned char)b;
		}
		*type = (h[1] >> 4) & 7u;
		if (s_stp_full) {
			unsigned int seq = (h[0] >> 3) & 7u;
			if (seq != s_rx_expect) {	/* a frame resent: already taken, ack it again */
				g_bt_rx_dup++;
				stp_ack();
				continue;
			}
			s_rx_ack = seq;
			s_rx_expect = (seq + 1u) & 7u;
			stp_ack();
		}
		return (int)(len < cap ? len : cap);
	}
}

/* Frames read while stp_send waits for room in the window are kept here, in order, and handed
 * out by stp_recv before anything newer off the pipe. */
#define STP_QUEUE	6
static struct { unsigned int type, len; unsigned char d[512]; } s_q[STP_QUEUE];
static unsigned int s_q_head, s_q_n;
volatile unsigned int g_bt_q_drop;	/* frames lost to a full queue */

static void stp_queue_one(unsigned int us)
{
	unsigned int slot, type;
	int len;
	if (s_q_n == STP_QUEUE) {		/* full: the oldest gives way */
		s_q_head = (s_q_head + 1) % STP_QUEUE;
		s_q_n--;
		g_bt_q_drop++;
	}
	slot = (s_q_head + s_q_n) % STP_QUEUE;
	len = stp_recv_raw(&type, s_q[slot].d, sizeof s_q[slot].d, us);
	if (len < 0)
		return;
	s_q[slot].type = type;
	s_q[slot].len = (unsigned)len;
	s_q_n++;
}

static int stp_recv(unsigned int *type, unsigned char *buf, unsigned int cap, unsigned int us)
{
	if (s_q_n) {
		unsigned int n = s_q[s_q_head].len < cap ? s_q[s_q_head].len : cap;
		*type = s_q[s_q_head].type;
		memcpy(buf, s_q[s_q_head].d, n);
		s_q_head = (s_q_head + 1) % STP_QUEUE;
		s_q_n--;
		return (int)n;
	}
	return stp_recv_raw(type, buf, cap, us);
}

/* ---- WMT: CONNSYS's control commands ---- */
static unsigned char s_evt[512];

/* send a WMT command, wait for its event; the event's length, negative on failure */
static int wmt_cmd(const unsigned char *cmd, unsigned int n, unsigned int us)
{
	unsigned int type;
	int len;
	unsigned int t0 = gpt4_get_current_tick(), left;
	if (stp_send(STP_TYPE_WMT, cmd, n) != 0)
		return -10;
	for (;;) {
		left = GPT_US(gpt4_get_current_tick() - t0);
		if (left >= us)
			return -11;
		len = stp_recv(&type, s_evt, sizeof s_evt, us - left);
		if (len < 0)
			return len;
		if (type == STP_TYPE_WMT && len >= 4 && s_evt[0] == 2 && s_evt[1] == cmd[1])
			return len;
		bt_log("skip type %u len %d %02x %02x", type, len, s_evt[0], s_evt[1]);
	}
}

/* a command whose event must read back exactly as expect */
static int wmt_step_us(const char *what, const unsigned char *cmd, unsigned int n,
		       const unsigned char *expect, unsigned int en, unsigned int us)
{
	int len = wmt_cmd(cmd, n, us);
	if (len < 0) {
		bt_log("%s: no event (%d)", what, len);
		return -1;
	}
	if ((unsigned)len < en || memcmp(s_evt, expect, en) != 0) {
		bt_log("%s: evt %02x %02x %02x %02x %02x %02x", what, s_evt[0], s_evt[1], s_evt[2],
		       s_evt[3], len > 4 ? s_evt[4] : 0, len > 5 ? s_evt[5] : 0);
		return -2;
	}
	bt_log("%s ok", what);
	return 0;
}

static int wmt_step(const char *what, const unsigned char *cmd, unsigned int n,
		    const unsigned char *expect, unsigned int en)
{
	return wmt_step_us(what, cmd, n, expect, en, 500000);
}

/* the MCU patch, over the pipe: a configure command with its address and size, then a raw 12
 * byte header and the patch body in 1000 byte pieces */
static int wmt_patch_mcu(fat_vol *v)
{
	static const char path[] = BT_FW_DIR PICO_BT_FW_PATCH_MCU;
	unsigned char cfg[21] = { 0x01, 0x01, 0x11, 0x00, 0x00 };
	unsigned char hdr[32], pda[12];
	unsigned int size, pos, addr;
	fat_file f;

	if (fat_open(v, path, &f) != 0 || f.size <= 30 ||
	    fat_read(&f, 0, hdr, sizeof hdr) != sizeof hdr || memcmp(hdr + 16, "ALPS", 4) != 0) {
		bt_log("mcu patch missing");
		return -1;
	}
	/* the patch file: a 28 byte header (build time, platform, versions, then the address it
	 * goes to, whose low byte is a flag and not part of it), a 2 byte checksum, then the body */
	addr = ((unsigned)hdr[24] | (unsigned)hdr[25] << 8 | (unsigned)hdr[26] << 16 |
		(unsigned)hdr[27] << 24) & ~0xffu;
	size = f.size - 30;
	cfg[5] = (unsigned char)addr;
	cfg[6] = (unsigned char)(addr >> 8);
	cfg[7] = (unsigned char)(addr >> 16);
	cfg[8] = (unsigned char)(addr >> 24);
	cfg[9] = (unsigned char)size;
	cfg[10] = (unsigned char)(size >> 8);
	cfg[11] = (unsigned char)(size >> 16);
	cfg[12] = (unsigned char)(size >> 24);
	cfg[17] = 0x2f; cfg[18] = 0x02; cfg[19] = 0x02; cfg[20] = 0x00;
	bt_log("mcu patch to %08x %u bytes", addr, size);
	{
		static const unsigned char ok[] = { 0x02, 0x01, 0x01, 0x00, 0x00 };
		if (wmt_step("patch cfg", cfg, sizeof cfg, ok, sizeof ok))
			return -2;
	}
	memset(pda, 0, sizeof pda);
	pda[0] = (unsigned char)(size + 12);
	pda[1] = (unsigned char)((size + 12) >> 8);
	pda[2] = (unsigned char)((size + 12) >> 16);
	pda[3] = (unsigned char)((size + 12) >> 24);
	if (btif_put(pda, sizeof pda))
		return -3;
	for (pos = 0; pos < size; ) {
		unsigned int n = size - pos > 1000 ? 1000 : size - pos;
		if (fat_read(&f, 30 + pos, s_bounce, n) != n || btif_put(s_bounce, n))
			return -4;
		pos += n;
	}
	bt_log("mcu patch sent %u bytes", size);
	return 0;
}

/* ---- the whole bring-up ---- */

int pico_bt_up(void)
{
	static const unsigned char reset[] = { 0x01, 0x07, 0x01, 0x00, 0x04 };
	static const unsigned char reset_ok[] = { 0x02, 0x07, 0x01, 0x00, 0x00 };
	static const unsigned char qstp[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const unsigned char qstp_ok[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04 };
	static const unsigned char setstp[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xdb, 0x0e, 0x68, 0x01 };
	static const unsigned char setstp_ok[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
	static const unsigned char qstp_full_ok[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xdb, 0x0e, 0x68, 0x01 };
	static const unsigned char chipid[] = { 0x01, 0x02, 0x05, 0x00, 0x11,
		PICO_BT_CHIP_ID & 0xff, PICO_BT_CHIP_ID >> 8, 0x00, 0x00 };
	static const unsigned char chipid_ok[] = { 0x02, 0x02, 0x01, 0x00, 0x00 };
	static const unsigned char rf_cal[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	static const unsigned char rf_cal_ok[] = { 0x02, 0x14, 0x02, 0x00, 0x00, 0x01 };
	static const unsigned char bt_on[] = { 0x01, 0x06, 0x02, 0x00, 0x00, 0x01 };
	static const unsigned char bt_on_ok[] = { 0x02, 0x06, 0x01, 0x00, 0x00 };
	fat_vol *v = gba_sd_menu_vol();
	unsigned int r;

	if (s_bt_up)
		return 0;
	s_log_n = 0;
	if (pico_bt_plat_powered()) {	/* a failed try before: from cold again */
		pico_bt_off();
		thread_sleep(10);
	}
	if (!v) {
		bt_log("no card");
		return -1;
	}
	if (bt_emi_window() != 0)
		return -2;
	memset((void *)(addr_t)BT_EMI_PA, 0, BT_EMI_SIZE);
	if (bt_load_ram_image(v, PICO_BT_FW_RAM_MCU, BT_BUILDTIME_MCU) ||
	    bt_load_ram_image(v, PICO_BT_FW_RAM_BT, BT_BUILDTIME_BT) ||
	    bt_load_ram_image(v, PICO_BT_FW_RAM_WIFI, BT_BUILDTIME_WIFI))
		return -3;
	/* the console's part: supplies, power domain, the window's remap, the CPU let out of reset */
	r = (unsigned int)pico_bt_plat_start();
	if (r != 0)
		return -4 - (int)r;			/* -5 power on, -6 the CPU did not start */
	thread_sleep(20);

	btif_init();
	btif_wake();
	s_stp_full = 0;
	s_tx_seq = 0;
	s_rx_ack = 7;
	s_rx_expect = 0;
	s_peer_ack = 7;
	s_q_n = 0;
	g_bt_rx_junk = 0;
	g_bt_rx_dup = 0;
	if (wmt_patch_mcu(v) != 0)
		return -6;
	if (wmt_step("wmt reset", reset, sizeof reset, reset_ok, sizeof reset_ok) ||
	    wmt_step("query stp", qstp, sizeof qstp, qstp_ok, sizeof qstp_ok) ||
	    wmt_step("set stp", setstp, sizeof setstp, setstp_ok, sizeof setstp_ok))
		return -7;
	/* the chip needs a few ms to switch its framing; until then a full mode frame is read the
	 * simple way and its sequence number is lost to both sides. The first full mode frame asks
	 * for the options just set. */
	thread_sleep(10);
	s_stp_full = 1;
	if (wmt_step("query stp full", qstp, sizeof qstp, qstp_full_ok, sizeof qstp_full_ok))
		return -11;
	if (wmt_step("chip id", chipid, sizeof chipid, chipid_ok, sizeof chipid_ok))
		return -8;
	/* the radio calibrates itself before any function runs (about a second on Android) */
	{	/* its event carries the calibration results (about 450 bytes): checked by its
		 * type, opcode and status only */
		int len = wmt_cmd(rf_cal, sizeof rf_cal, 3000000);
		if (len < 6 || s_evt[0] != rf_cal_ok[0] || s_evt[1] != rf_cal_ok[1] ||
		    s_evt[4] != rf_cal_ok[4] || s_evt[5] != rf_cal_ok[5]) {
			bt_log("rf calibration: rc %d evt %02x %02x %02x %02x", len, s_evt[0], s_evt[1],
			       s_evt[4], s_evt[5]);
			return -10;
		}
		bt_log("rf calibration ok, %d bytes", len);
	}
	if (wmt_step_us("bt on", bt_on, sizeof bt_on, bt_on_ok, sizeof bt_on_ok, 3000000))
		return -9;
	s_bt_up = 1;
	return 0;
}

/* one HCI command (its H4 type byte first), the first event back into ev; its length */
int pico_bt_hci(const unsigned char *cmd, unsigned int n, unsigned char *ev, unsigned int cap)
{
	unsigned int type;
	int len;
	if (!s_bt_up || stp_send(STP_TYPE_BT, cmd, n) != 0)
		return -1;
	{
		unsigned int t0 = gpt4_get_current_tick();
		do {
			len = stp_recv(&type, ev, cap, 1000000);
		} while (len >= 0 && type != STP_TYPE_BT && GPT_US(gpt4_get_current_tick() - t0) < 1000000);
	}
	return len;
}

/* oem btup - the whole bring-up (firmware from /bios/connsys/ on the card), then its log */
static void cmd_btup(const char *arg, void *data, unsigned sz)
{
	int rc;
	(void)arg; (void)data; (void)sz;
	rc = pico_bt_up();
	snprintf(s_line, sizeof s_line, "bt up rc=%d (oem btlog for the steps)", rc);
	fastboot_info(s_line);
	fastboot_okay("");
}

/* oem btlog[:FIRST] - the bring-up log, 12 lines from FIRST (default: the last 12) */
static void cmd_btlog(const char *arg, void *data, unsigned sz)
{
	unsigned int first, i, oldest = s_log_n > BT_LOG_LINES ? s_log_n - BT_LOG_LINES : 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg >= '0' && *arg <= '9') {
		first = 0;
		for (; *arg >= '0' && *arg <= '9'; arg++) first = first * 10 + (unsigned)(*arg - '0');
	} else {
		first = s_log_n > 12 ? s_log_n - 12 : 0;
	}
	if (first < oldest)
		first = oldest;
	snprintf(s_line, sizeof s_line, "log %u lines, junk %u, resent %u", s_log_n, g_bt_rx_junk,
		 g_bt_rx_dup);
	fastboot_info(s_line);
	if (g_bt_rx_junk) {
		unsigned int k, l = (unsigned)snprintf(s_line, sizeof s_line, "junk:");
		for (k = 0; k < 16 && k < g_bt_rx_junk; k++)
			l += (unsigned)snprintf(s_line + l, sizeof s_line - l, " %02x", s_junk_first[k]);
		fastboot_info(s_line);
	}
	for (i = first; i < s_log_n && i < first + 12; i++) {
		snprintf(s_line, sizeof s_line, "%u %s", i, s_log[i % BT_LOG_LINES]);
		fastboot_info(s_line);
	}
	fastboot_okay("");
}

/* oem bthci:HEX - one HCI command, H4 type byte first (01 03 0c 00 = Reset), its event back */
static void cmd_bthci(const char *arg, void *data, unsigned sz)
{
	unsigned char cmd[64], ev[64];
	unsigned int n = 0, i;
	int len;
	(void)data; (void)sz;
	while (*arg && n < sizeof cmd) {
		int hi, lo;
		while (*arg == ' ' || *arg == ':') arg++;
		if (!arg[0] || !arg[1]) break;
		hi = arg[0] <= '9' ? arg[0] - '0' : (arg[0] | 0x20) - 'a' + 10;
		lo = arg[1] <= '9' ? arg[1] - '0' : (arg[1] | 0x20) - 'a' + 10;
		cmd[n++] = (unsigned char)(hi << 4 | lo);
		arg += 2;
	}
	len = pico_bt_hci(cmd, n, ev, sizeof ev);
	if (len < 0) {
		snprintf(s_line, sizeof s_line, "hci: no event (%d)", len);
		fastboot_info(s_line);
	}
	for (i = 0; len > 0 && i < (unsigned)len; i += 16) {
		unsigned int k, l = (unsigned)snprintf(s_line, sizeof s_line, "%02u:", i);
		for (k = i; k < (unsigned)len && k < i + 16; k++)
			l += (unsigned)snprintf(s_line + l, sizeof s_line - l, " %02x", ev[k]);
		fastboot_info(s_line);
	}
	fastboot_okay("");
}

/* oem btwmt:[w]HEX - one WMT command (w first: pulse the wake line before), its event back */
static void cmd_btwmt(const char *arg, void *data, unsigned sz)
{
	unsigned char cmd[64];
	unsigned int n = 0, i;
	int len;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == 'w') { btif_wake(); arg++; }
	while (*arg && n < sizeof cmd) {
		int hi, lo;
		while (*arg == ' ' || *arg == ':') arg++;
		if (!arg[0] || !arg[1]) break;
		hi = arg[0] <= '9' ? arg[0] - '0' : (arg[0] | 0x20) - 'a' + 10;
		lo = arg[1] <= '9' ? arg[1] - '0' : (arg[1] | 0x20) - 'a' + 10;
		cmd[n++] = (unsigned char)(hi << 4 | lo);
		arg += 2;
	}
	len = pico_bt_plat_powered() ? wmt_cmd(cmd, n, 2000000) : -20;
	snprintf(s_line, sizeof s_line, "wmt rc %d full %d seq %u ack %u lsr %02x", len, s_stp_full,
		 s_tx_seq, s_rx_ack, R32(BTIF_BASE + BTIF_LSR) & 0xffu);
	fastboot_info(s_line);
	for (i = 0; len > 0 && i < (unsigned)len; i += 16) {
		unsigned int k, l = (unsigned)snprintf(s_line, sizeof s_line, "%02u:", i);
		for (k = i; k < (unsigned)len && k < i + 16; k++)
			l += (unsigned)snprintf(s_line + l, sizeof s_line - l, " %02x", s_evt[k]);
		fastboot_info(s_line);
	}
	fastboot_okay("");
}

/* one HCI command and its Command Complete (or Command Status); other events read meanwhile
 * (advertising reports) are dropped; 0 when the controller took it */
static int bt_hci_ok(const unsigned char *cmd, unsigned int n)
{
	unsigned char ev[64];
	unsigned int type, t0 = gpt4_get_current_tick();
	int len;
	if (stp_send(STP_TYPE_BT, cmd, n) != 0)
		return -1;
	while (GPT_US(gpt4_get_current_tick() - t0) < 1000000) {
		len = stp_recv(&type, ev, sizeof ev, 1000000);
		if (len < 0)
			return -2;
		if (type != STP_TYPE_BT || len < 4 || ev[0] != 0x04)
			continue;
		if (ev[1] == 0x0e && len >= 7 && ev[4] == cmd[1] && ev[5] == cmd[2])
			return ev[6] ? -3 : 0;			/* Command Complete: status */
		if (ev[1] == 0x0f && len >= 7 && ev[5] == cmd[1] && ev[6] == cmd[2])
			return ev[3] ? -3 : 0;			/* Command Status */
	}
	return -4;
}

/* ---- who this console is on the air ----
 * An LE random static address made from the console's serial number (sn_buf, the one fastboot
 * reports): the same on every power on, different between consoles, and nothing to store. Its
 * top two bits are 1, as a static address's must be. */
extern char sn_buf[];
static unsigned char s_le_addr[6];	/* least significant byte first, as HCI carries it */

static void bt_le_identity(void)
{
	unsigned long long h = 0xcbf29ce484222325ull;	/* FNV-1a over the serial */
	const char *p;
	unsigned int i;
	for (p = sn_buf; *p; p++) {
		h ^= (unsigned char)*p;
		h *= 0x100000001b3ull;
	}
	for (i = 0; i < 6; i++)
		s_le_addr[i] = (unsigned char)(h >> (8 * i));
	s_le_addr[5] |= 0xc0;
	if (s_le_addr[0] == 0xff && s_le_addr[1] == 0xff && s_le_addr[2] == 0xff)
		s_le_addr[0] = 0xfe;			/* never the all-ones address */
}

/* The advertising a GammaOS Pico sends: the flags, its name, and manufacturer data that marks
 * it as one (company id 0xFFFF, the one for tests and unregistered use), the link protocol's
 * version and the console model, so consoles pick each other out of a scan. */
#define PICO_ADV_MAGIC0		'G'
#define PICO_ADV_MAGIC1		'P'
#define PICO_LINK_VERSION	1

/* what this console offers the others, in its advertising (pico_link.h PICO_OFFER_*), and for a
 * host the game's system: Join and Send list only the consoles that fit */
static unsigned char s_adv_offer, s_adv_system;

/* advertising on (1) or off (0); 0 when the controller took every command */
/* 0 off, 1 presence only (not connectable), 2 connectable (a link may be opened to it) */
int pico_bt_advertise(int on)
{
	unsigned char params[] = { 0x01, 0x06, 0x20, 0x0f,
		0xa0, 0x00, 0xa0, 0x00,		/* every 100 ms */
		0x03,				/* 0x03 presence only, 0x00 connectable */
		0x01,				/* our address: the random one */
		0x00, 0, 0, 0, 0, 0, 0,		/* no peer */
		0x07,				/* all three advertising channels */
		0x00 };
	unsigned char addr[10] = { 0x01, 0x05, 0x20, 0x06 };
	unsigned char data[36] = { 0x01, 0x08, 0x20, 0x20 };
	unsigned char en[5] = { 0x01, 0x0a, 0x20, 0x01, 0x00 };
	unsigned int k = 5;

	if (!s_bt_up)
		return -1;
	if (!on) {
		en[4] = 0;
		return bt_hci_ok(en, sizeof en);
	}
	if (on == 2) {
		params[4] = params[6] = 0x30;	/* every 30 ms while waiting for a link */
		params[8] = 0x00;
	}
	bt_le_identity();
	memcpy(addr + 4, s_le_addr, 6);
	data[k++] = 2; data[k++] = 0x01; data[k++] = 0x06;	/* flags: LE only, general discovery */
	data[k++] = 5; data[k++] = 0x09;			/* name */
	data[k++] = 'P'; data[k++] = 'i'; data[k++] = 'c'; data[k++] = 'o';
	data[k++] = 9; data[k++] = 0xff;			/* manufacturer data */
	data[k++] = 0xff; data[k++] = 0xff;
	data[k++] = PICO_ADV_MAGIC0; data[k++] = PICO_ADV_MAGIC1;
	data[k++] = PICO_LINK_VERSION; data[k++] = PICO_BT_MODEL;
	data[k++] = s_adv_offer; data[k++] = s_adv_system;
	data[4] = (unsigned char)(k - 5);			/* the significant length */
	if (bt_hci_ok(addr, sizeof addr) || bt_hci_ok(params, sizeof params) ||
	    bt_hci_ok(data, sizeof data))
		return -2;
	en[4] = 1;
	return bt_hci_ok(en, sizeof en) ? -3 : 0;
}

/* a GammaOS Pico's marker in advertising data: its model, 0 when it is not one; what it offers
 * and its system into *offer / *system when given (0 from a console that does not say) */
static int bt_pico_info(const unsigned char *d, unsigned int n, unsigned char *offer, unsigned char *system)
{
	unsigned int i = 0;
	while (i + 1 < n && d[i]) {
		unsigned int l = d[i];
		if (i + 1 + l > n)
			break;
		if (l >= 7 && d[i + 1] == 0xff && d[i + 2] == 0xff && d[i + 3] == 0xff &&
		    d[i + 4] == PICO_ADV_MAGIC0 && d[i + 5] == PICO_ADV_MAGIC1) {
			if (offer) *offer = l >= 9 ? d[i + 8] : 0;
			if (system) *system = l >= 9 ? d[i + 9] : 0;
			return d[i + 7] ? d[i + 7] : 1;
		}
		i += 1 + l;
	}
	return 0;
}
static int bt_pico_model(const unsigned char *d, unsigned int n) { return bt_pico_info(d, n, 0, 0); }

/* oem btadv:1|0 - advertise as a GammaOS Pico (or stop), with the address it uses */
static void cmd_btadv(const char *arg, void *data, unsigned sz)
{
	int on, rc;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	on = *arg == '0' ? 0 : *arg == '2' ? 2 : 1;
	rc = pico_bt_advertise(on);
	snprintf(s_line, sizeof s_line, "advertise %s rc %d addr %02x:%02x:%02x:%02x:%02x:%02x",
		 on ? "on" : "off", rc, s_le_addr[5], s_le_addr[4], s_le_addr[3], s_le_addr[2],
		 s_le_addr[1], s_le_addr[0]);
	fastboot_info(s_line);
	fastboot_okay("");
}

/* oem btscan[:SECONDS] - a passive LE scan (default 3 s): how many devices advertise nearby,
 * the first few addresses with their signal strength */
static void cmd_btscan(const char *arg, void *data, unsigned sz)
{
	static const unsigned char mask[] = { 0x01, 0x01, 0x0c, 0x08,		/* events, with LE meta */
		0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const unsigned char params[] = { 0x01, 0x0b, 0x20, 0x07,	/* passive, 10 ms / 10 ms */
		0x00, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };
	static const unsigned char on[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x00 };
	static const unsigned char off[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	static unsigned char seen[32][6];
	static signed char rssi[32];
	static unsigned char model[32];
	unsigned int picos = 0;
	unsigned char ev[300];
	unsigned int secs = 3, n = 0, reports = 0, type, i, t0;
	int len;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg >= '1' && *arg <= '9') secs = (unsigned)(*arg - '0');
	if (!s_bt_up) { fastboot_fail("bluetooth is off (oem btup)"); return; }
	if (bt_hci_ok(mask, sizeof mask) || bt_hci_ok(params, sizeof params) || bt_hci_ok(on, sizeof on)) {
		fastboot_fail("scan setup refused");
		return;
	}
	t0 = gpt4_get_current_tick();
	while (GPT_US(gpt4_get_current_tick() - t0) < secs * 1000000u) {
		len = stp_recv(&type, ev, sizeof ev, 200000);
		if (len < 0 || type != STP_TYPE_BT)
			continue;
		/* LE Meta, Advertising Report: 04 3e len 02 num [type addrtype addr6 dlen data rssi] */
		if (len >= 14 && ev[0] == 0x04 && ev[1] == 0x3e && ev[3] == 0x02) {
			const unsigned char *a = ev + 7;
			unsigned int dlen = ev[13];
			reports++;
			for (i = 0; i < n; i++)
				if (!memcmp(seen[i], a, 6)) break;
			if (i == n && n < 32) {
				memcpy(seen[n], a, 6);
				rssi[n] = 14u + dlen < (unsigned)len ? (signed char)ev[14 + dlen] : 0;
				model[n] = (unsigned char)bt_pico_model(ev + 14,
					14u + dlen <= (unsigned)len ? dlen : (unsigned)len - 14u);
				if (model[n]) picos++;
				n++;
			}
		}
	}
	bt_hci_ok(off, sizeof off);
	snprintf(s_line, sizeof s_line, "scan %us: %u reports, %u devices, %u Pico", secs, reports, n,
		 picos);
	fastboot_info(s_line);
	for (i = 0; i < n && i < 8; i++) {
		snprintf(s_line, sizeof s_line, "%02x:%02x:%02x:%02x:%02x:%02x rssi %d%s", seen[i][5],
			 seen[i][4], seen[i][3], seen[i][2], seen[i][1], seen[i][0], rssi[i],
			 model[i] == 1 ? " Pico KPA" : model[i] ? " Pico Block" : "");
		fastboot_info(s_line);
	}
	fastboot_okay("");
}


/* ---- a link between two consoles (first test form) ----
 * One console advertises connectable and waits (oem btlink:p), the other finds it and opens an
 * LE connection to it (oem btlink:c). The data are plain ACL frames on channel 0x0040, which no
 * Bluetooth stack on either side interprets: both ends are this code. Each frame's first byte
 * says what it is: 'P' a ping (echoed back as it is), 'T' bulk data (counted), 'E' the end of
 * the bulk data (answered with 'R' and the count). */
#define LINK_CID		0x0040
#define LINK_MAX_PAYLOAD	243		/* 251 byte ACL data, less the 4 byte channel header, less 4 */

static unsigned int s_link_handle = 0xffff;	/* 0xffff: no link */
volatile unsigned int g_bt_link_status;		/* why a link did not open: HCI status (0x100 + a refusal) */
static unsigned int s_link_credits;		/* ACL buffers free in our controller */
static unsigned int s_link_credits_max;		/* how many it has */
static unsigned int s_link_credit_tick;		/* GPT tick a buffer last came back or went out */
volatile unsigned int g_bt_credit_restores;	/* times the buffers were taken back as free (below) */
static unsigned int s_link_phy_tx = 1, s_link_phy_rx = 1, s_link_dl = 27;	/* radio and frame size */
static unsigned char s_rx[300];

/* the next frame from the controller, events and ACL alike (H4 type first); its length */
static int bt_rx(unsigned int us)
{
	unsigned int type;
	int len = stp_recv(&type, s_rx, sizeof s_rx, us);
	if (len >= 0 && type != STP_TYPE_BT)
		return 0;			/* a WMT message: nothing for the link */
	return len;
}

/* bookkeeping every received frame needs: freed buffers, the link dropping */
static void link_note(int len)
{
	if (len >= 5 && s_rx[0] == 0x04 && s_rx[1] == 0x13) {	/* Number of Completed Packets */
		unsigned int i, n = s_rx[3];
		for (i = 0; i < n && 6u + 4u * i < (unsigned)len; i++)
			s_link_credits += (unsigned)(s_rx[6 + 4 * i] | s_rx[7 + 4 * i] << 8);
		if (s_link_credits_max && s_link_credits > s_link_credits_max)
			s_link_credits = s_link_credits_max;	/* a late one after a restore */
		s_link_credit_tick = gpt4_get_current_tick();
	}
	if (len >= 6 && s_rx[0] == 0x04 && s_rx[1] == 0x05)	/* Disconnection Complete */
		s_link_handle = 0xffff;
	if (len >= 9 && s_rx[0] == 0x04 && s_rx[1] == 0x3e && s_rx[3] == 0x0c && !s_rx[4]) {
		s_link_phy_tx = s_rx[7];			/* LE PHY Update Complete */
		s_link_phy_rx = s_rx[8];
	}
	if (len >= 8 && s_rx[0] == 0x04 && s_rx[1] == 0x3e && s_rx[3] == 0x07)
		s_link_dl = (unsigned)(s_rx[6] | s_rx[7] << 8);	/* LE Data Length Change: tx octets */
}

/* one ACL frame to the other console; 0 when the controller took it */
static int link_send(const unsigned char *p, unsigned int n)
{
	unsigned char f[5 + 4 + LINK_MAX_PAYLOAD + 4];
	unsigned int t0 = gpt4_get_current_tick();
	int len;
	if (s_link_handle == 0xffff || n > LINK_MAX_PAYLOAD + 4)
		return -1;
	while (!s_link_credits) {		/* wait for a buffer of the controller's */
		if (GPT_US(gpt4_get_current_tick() - t0) > 500000)
			return -2;
		len = bt_rx(10000);
		if (len > 0) link_note(len);
		if (s_link_handle == 0xffff)
			return -3;
	}
	f[0] = 0x02;				/* ACL */
	f[1] = (unsigned char)s_link_handle;
	f[2] = (unsigned char)((s_link_handle >> 8) & 0x0f);	/* first fragment, host to controller */
	f[3] = (unsigned char)(n + 4);
	f[4] = (unsigned char)((n + 4) >> 8);
	f[5] = (unsigned char)n;		/* the channel header: length, channel */
	f[6] = (unsigned char)(n >> 8);
	f[7] = (unsigned char)LINK_CID;
	f[8] = (unsigned char)(LINK_CID >> 8);
	memcpy(f + 9, p, n);
	if (stp_send(STP_TYPE_BT, f, 9 + n) != 0)
		return -4;
	s_link_credits--;
	s_link_credit_tick = gpt4_get_current_tick();
	return 0;
}

/* A link frame can reach us in pieces: the controller hands over each piece the radio carried,
 * the first with the frame's channel header (its whole length), the rest marked as continuing
 * it. They are joined here. Returns the frame's payload once it is whole (NULL otherwise: not
 * link data, or not complete yet), its length in *n. */
static unsigned char s_reasm[LINK_MAX_PAYLOAD + 64];
static unsigned int s_reasm_have, s_reasm_want;		/* bytes joined, bytes the frame has */
volatile unsigned int g_bt_link_bad;			/* pieces that did not fit a frame */

static const unsigned char *link_payload(int len, unsigned int *n)
{
	unsigned int pb, dlen;
	const unsigned char *d;
	if (len < 5 || s_rx[0] != 0x02)
		return 0;
	pb = (s_rx[2] >> 4) & 3u;			/* 2 (or 0) a frame's first piece, 1 a later one */
	dlen = (unsigned)(s_rx[3] | s_rx[4] << 8);
	d = s_rx + 5;
	if (5u + dlen > (unsigned)len)
		return 0;
	if (pb != 1) {
		unsigned int l2;
		if (dlen < 4) { g_bt_link_bad++; return 0; }
		l2 = (unsigned)(d[0] | d[1] << 8);
		if ((unsigned)(d[2] | d[3] << 8) != LINK_CID || l2 > sizeof s_reasm) {
			s_reasm_want = 0;
			return 0;
		}
		s_reasm_want = l2;
		s_reasm_have = 0;
		d += 4;
		dlen -= 4;
	} else if (!s_reasm_want) {
		return 0;				/* a piece of a frame we did not take */
	}
	if (s_reasm_have + dlen > s_reasm_want) {
		g_bt_link_bad++;
		s_reasm_want = 0;
		return 0;
	}
	memcpy(s_reasm + s_reasm_have, d, dlen);
	s_reasm_have += dlen;
	if (s_reasm_have < s_reasm_want)
		return 0;
	*n = s_reasm_want;
	s_reasm_want = 0;
	return s_reasm;
}

/* the LE event mask and our address, before any link */
static int link_prepare(void)
{
	static const unsigned char mask[] = { 0x01, 0x01, 0x0c, 0x08,
		0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const unsigned char le_mask[] = { 0x01, 0x01, 0x20, 0x08,	/* every LE event */
		0xff, 0xff, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00 };
	static const unsigned char rbuf[] = { 0x01, 0x02, 0x20, 0x00 };	/* LE buffer count */
	unsigned char addr[10] = { 0x01, 0x05, 0x20, 0x06 };
	unsigned char ev[64];
	if (bt_hci_ok(mask, sizeof mask) || bt_hci_ok(le_mask, sizeof le_mask))
		return -1;
	bt_le_identity();
	memcpy(addr + 4, s_le_addr, 6);
	if (bt_hci_ok(addr, sizeof addr))
		return -2;
	if (pico_bt_hci(rbuf, sizeof rbuf, ev, sizeof ev) >= 10)
		s_link_credits = ev[9];		/* 04 0e 07 01 02 20 00 len_lo len_hi count */
	if (!s_link_credits)
		s_link_credits = 4;
	s_link_credits_max = s_link_credits;
	return 0;
}

/* wait for the link to open: LE Connection Complete; its handle, or -1 */
static int link_wait_open(unsigned int us, unsigned int *interval)
{
	unsigned int t0 = gpt4_get_current_tick();
	while (GPT_US(gpt4_get_current_tick() - t0) < us) {
		int len = bt_rx(100000);
		if (len <= 0)
			continue;
		link_note(len);
		/* LE Connection Complete: 04 3e len 01 status handle(2) role peer_type peer(6)
		 * interval(2) ...; the enhanced form (0x0a, sent while its event is enabled) carries
		 * two more addresses before the interval */
		if (len >= 19 && s_rx[0] == 0x04 && s_rx[1] == 0x3e &&
		    (s_rx[3] == 0x01 || (s_rx[3] == 0x0a && len >= 31))) {
			unsigned int io = s_rx[3] == 0x01 ? 15 : 27;
			if (s_rx[4] != 0) {
				g_bt_link_status = s_rx[4];
				return -2;
			}
			s_link_handle = (unsigned)(s_rx[5] | s_rx[6] << 8) & 0x0fff;
			*interval = (unsigned)(s_rx[io] | s_rx[io + 1] << 8);
			return (int)s_link_handle;
		}
		/* the Command Status of LE Create Connection: a refusal ends the wait */
		if (len >= 7 && s_rx[0] == 0x04 && s_rx[1] == 0x0f && s_rx[5] == 0x0d && s_rx[6] == 0x20 &&
		    s_rx[3] != 0) {
			g_bt_link_status = 0x100 | s_rx[3];
			return -3;
		}
	}
	return -1;
}

/* one command on an open link and its Command Complete / Status; the controller's status
 * (0 taken), 0x100 when no answer came; other frames read meanwhile get their bookkeeping */
/* A frame read while waiting for something else (a command's answer, the other console's
 * features): when it carries the other console's message it goes to the queue as usual. After a
 * role swap the new central sends at once, and these waits had thrown its first pieces away. */
static void rxq_put(const unsigned char *p, unsigned int n);
extern volatile unsigned int g_bt_link_rx;
static void link_keep(int len)
{
	const unsigned char *p;
	unsigned int n;
	p = link_payload(len, &n);
	if (p && n) {
		g_bt_link_rx++;
		rxq_put(p, n);
	}
}

static unsigned int link_cmd(const unsigned char *cmd, unsigned int n)
{
	unsigned int t0 = gpt4_get_current_tick();
	if (stp_send(STP_TYPE_BT, cmd, n) != 0)
		return 0x100;
	while (GPT_US(gpt4_get_current_tick() - t0) < 500000u) {
		int len = bt_rx(50000);
		if (len <= 0)
			continue;
		link_note(len);
		if (len >= 7 && s_rx[0] == 0x04 && s_rx[1] == 0x0e && s_rx[4] == cmd[1] && s_rx[5] == cmd[2])
			return s_rx[6];
		if (len >= 7 && s_rx[0] == 0x04 && s_rx[1] == 0x0f && s_rx[5] == cmd[1] && s_rx[6] == cmd[2])
			return s_rx[3];
		link_keep(len);
	}
	return 0x100;
}

/* ask for the fast radio (2M) and full size frames; the results come in their own events */
static void link_tune(void)
{
	unsigned char dl[] = { 0x01, 0x22, 0x20, 0x06, 0, 0, 251, 0x00, 0x48, 0x08 };
	unsigned char phy[] = { 0x01, 0x32, 0x20, 0x07, 0, 0, 0x00, 0x02, 0x02, 0x00, 0x00 };
	unsigned char rf[] = { 0x01, 0x16, 0x20, 0x02, 0, 0 };
	unsigned int a, b, t0, feat = 0;
	dl[4] = phy[4] = rf[4] = (unsigned char)s_link_handle;
	dl[5] = phy[5] = rf[5] = (unsigned char)(s_link_handle >> 8);
	/* the controller turns these down until it has the other side's feature list: ask for it,
	 * wait for LE Read Remote Features Complete (04 3e len 04 status handle(2) features(8)) */
	if (link_cmd(rf, sizeof rf) == 0) {
		t0 = gpt4_get_current_tick();
		while (!feat && GPT_US(gpt4_get_current_tick() - t0) < 1000000u) {
			int len = bt_rx(50000);
			if (len <= 0)
				continue;
			link_note(len);
			if (len >= 15 && s_rx[0] == 0x04 && s_rx[1] == 0x3e && s_rx[3] == 0x04) {
				feat = 0x10000u | s_rx[7] | (unsigned)s_rx[8] << 8;	/* "got it" + bits 0..15 */
				continue;
			}
			link_keep(len);	/* the other console may already be sending */
		}
	}
	bt_log("peer features %04x", feat & 0xffffu);
	a = link_cmd(dl, sizeof dl);
	b = link_cmd(phy, sizeof phy);
	bt_log("frame size request %x, 2M request %x", a, b);
}

static void link_close(void)
{
	unsigned char d[] = { 0x01, 0x06, 0x04, 0x03, 0, 0, 0x13 };
	unsigned int t0 = gpt4_get_current_tick();
	if (s_link_handle == 0xffff)
		return;
	d[4] = (unsigned char)s_link_handle;
	d[5] = (unsigned char)(s_link_handle >> 8);
	stp_send(STP_TYPE_BT, d, sizeof d);
	while (s_link_handle != 0xffff && GPT_US(gpt4_get_current_tick() - t0) < 1000000) {
		int len = bt_rx(100000);
		if (len > 0) link_note(len);
	}
	s_link_handle = 0xffff;
}

/* the waiting side: advertise connectable, then answer pings and count bulk data */
static void link_peripheral(unsigned int secs)
{
	unsigned int interval = 0, pings = 0, bytes = 0, t0;
	int len;
	unsigned int n;
	const unsigned char *p;

	if (link_prepare() || pico_bt_advertise(2)) {
		fastboot_fail("link setup refused");
		return;
	}
	if (link_wait_open(secs * 1000000u, &interval) < 0) {
		pico_bt_advertise(0);
		fastboot_fail("no console connected");
		return;
	}
	s_link_phy_tx = s_link_phy_rx = 1;
	s_link_dl = 27;
	g_bt_link_bad = 0;
	snprintf(s_line, sizeof s_line, "link open, handle %u interval %u x1.25 ms", s_link_handle,
		 interval);
	fastboot_info(s_line);
	link_tune();				/* full size frames for our own sending too */
	t0 = gpt4_get_current_tick();
	while (s_link_handle != 0xffff && GPT_US(gpt4_get_current_tick() - t0) < 30000000u) {
		len = bt_rx(200000);
		if (len <= 0)
			continue;
		link_note(len);
		p = link_payload(len, &n);
		if (!p || !n)
			continue;
		if (p[0] == 'P') {
			unsigned char echo[LINK_MAX_PAYLOAD + 4];
			memcpy(echo, p, n);
			if (!link_send(echo, n)) pings++;
		} else if (p[0] == 'T') {
			bytes += n;
		} else if (p[0] == 'E') {
			unsigned char r[5] = { 'R', (unsigned char)bytes, (unsigned char)(bytes >> 8),
					       (unsigned char)(bytes >> 16), (unsigned char)(bytes >> 24) };
			link_send(r, sizeof r);
		}
	}
	snprintf(s_line, sizeof s_line, "pings echoed %u, bulk bytes %u, link %s", pings, bytes,
		 s_link_handle == 0xffff ? "closed by peer" : "still open");
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "phy %u/%u frame %u bad %u qdrop %u resent %u", s_link_phy_tx,
		 s_link_phy_rx, s_link_dl, g_bt_link_bad, g_bt_q_drop, g_bt_rx_dup);
	fastboot_info(s_line);
	link_close();
	fastboot_okay("");
}

/* the opening side: find a console, open a link at the given interval (1.25 ms units, 6..400),
 * time pings, stream data */
static void link_central(unsigned int ival)
{
	static const unsigned char sparams[] = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00,
						 0x01, 0x00 };		/* active scan, our random address */
	static const unsigned char son[] = { 0x01, 0x0c, 0x20, 0x02, 0x01, 0x01 };
	static const unsigned char soff[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x00 };
	unsigned char conn[] = { 0x01, 0x0d, 0x20, 0x19,
		0x10, 0x00, 0x10, 0x00,			/* scan 10 ms in 10 ms */
		0x00,					/* the peer named here */
		0x01, 0, 0, 0, 0, 0, 0,			/* its address: random */
		0x01,					/* ours: random */
		0x06, 0x00, 0x06, 0x00,			/* interval 7.5 ms */
		0x00, 0x00,				/* no skipped events */
		0x64, 0x00,				/* drop after 1 s of silence */
		0x00, 0x00, 0x00, 0x00 };
	unsigned char peer[6];
	unsigned int found = 0, interval = 0, t0, i, rt_min = ~0u, rt_max = 0, rt_sum = 0, rt_n = 0;
	unsigned int sent = 0, got = 0, us;
	int len;
	unsigned int n;
	const unsigned char *p;

	if (link_prepare() || bt_hci_ok(sparams, sizeof sparams) || bt_hci_ok(son, sizeof son)) {
		fastboot_fail("link setup refused");
		return;
	}
	t0 = gpt4_get_current_tick();
	while (!found && GPT_US(gpt4_get_current_tick() - t0) < 5000000u) {
		len = bt_rx(200000);
		if (len >= 14 && s_rx[0] == 0x04 && s_rx[1] == 0x3e && s_rx[3] == 0x02 &&
		    s_rx[5] == 0x00 &&			/* connectable advertising */
		    bt_pico_model(s_rx + 14, 14u + s_rx[13] <= (unsigned)len ? s_rx[13] : 0)) {
			memcpy(peer, s_rx + 7, 6);
			found = 1;
		}
	}
	bt_hci_ok(soff, sizeof soff);
	if (!found) {
		fastboot_fail("no console waiting for a link");
		return;
	}
	memcpy(conn + 10, peer, 6);
	conn[17] = conn[19] = (unsigned char)ival;		/* interval min = max */
	conn[18] = conn[20] = (unsigned char)(ival >> 8);
	{	/* the drop timeout: 1 s, or 6 intervals when that is longer (10 ms units) */
		unsigned int to = ival * 125u * 6u / 1000u + 1u;
		if (to < 100u) to = 100u;
		conn[23] = (unsigned char)to;
		conn[24] = (unsigned char)(to >> 8);
	}
	conn[27] = conn[28] = 0xff;				/* connection events as long as they can be */
	stp_send(STP_TYPE_BT, conn, sizeof conn);
	if (link_wait_open(3000000u, &interval) < 0) {
		snprintf(s_line, sizeof s_line, "the link did not open, status %x", g_bt_link_status);
		fastboot_info(s_line);
		fastboot_fail("no link");
		return;
	}
	s_link_phy_tx = s_link_phy_rx = 1;
	s_link_dl = 27;
	g_bt_link_bad = 0;
	snprintf(s_line, sizeof s_line, "link to %02x:%02x:%02x:%02x:%02x:%02x interval %u x1.25 ms",
		 peer[5], peer[4], peer[3], peer[2], peer[1], peer[0], interval);
	fastboot_info(s_line);
	link_tune();
	/* settle: the PHY and length changes go through */
	t0 = gpt4_get_current_tick();
	while (GPT_US(gpt4_get_current_tick() - t0) < 300000u) {
		len = bt_rx(50000);
		if (len > 0) link_note(len);
	}
	/* round trips: 50 pings of 16 bytes */
	for (i = 0; i < 50 && s_link_handle != 0xffff; i++) {
		unsigned char ping[16] = { 'P', (unsigned char)i };
		unsigned int ts = gpt4_get_current_tick(), done = 0;
		if (link_send(ping, sizeof ping))
			break;
		while (!done && GPT_US(gpt4_get_current_tick() - ts) < 200000u) {
			len = bt_rx(20000);
			if (len <= 0) continue;
			link_note(len);
			p = link_payload(len, &n);
			if (p && n >= 2 && p[0] == 'P' && p[1] == (unsigned char)i)
				done = 1;
		}
		if (done) {
			us = GPT_US(gpt4_get_current_tick() - ts);
			rt_sum += us; rt_n++;
			if (us < rt_min) rt_min = us;
			if (us > rt_max) rt_max = us;
		}
	}
	snprintf(s_line, sizeof s_line, "pings %u/50: round trip min %u avg %u max %u us", rt_n,
		 rt_n ? rt_min : 0, rt_n ? rt_sum / rt_n : 0, rt_max);
	fastboot_info(s_line);
	/* bulk: 2 s of full size frames, then the count from the other side */
	t0 = gpt4_get_current_tick();
	while (s_link_handle != 0xffff && GPT_US(gpt4_get_current_tick() - t0) < 2000000u) {
		unsigned char blk[LINK_MAX_PAYLOAD + 4];
		memset(blk, 0x5a, sizeof blk);
		blk[0] = 'T';
		if (link_send(blk, sizeof blk))
			break;
		sent += sizeof blk;
		while ((len = bt_rx(0)) > 0) link_note(len);	/* free buffers as they come */
	}
	us = GPT_US(gpt4_get_current_tick() - t0);
	{
		unsigned char e[1] = { 'E' };
		unsigned int te = gpt4_get_current_tick();
		link_send(e, 1);
		while (!got && GPT_US(gpt4_get_current_tick() - te) < 2000000u) {
			len = bt_rx(50000);
			if (len <= 0) continue;
			link_note(len);
			p = link_payload(len, &n);
			if (p && n >= 5 && p[0] == 'R')
				got = (unsigned)(p[1] | p[2] << 8 | p[3] << 16 | p[4] << 24) + 1;
		}
	}
	if (got) got--;
	snprintf(s_line, sizeof s_line, "bulk sent %u in %u ms, peer got %u: %u kB/s", sent, us / 1000,
		 got, us ? (unsigned)((unsigned long long)got * 1000u / us) : 0);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "phy %u/%u frame %u bad %u qdrop %u resent %u", s_link_phy_tx,
		 s_link_phy_rx, s_link_dl, g_bt_link_bad, g_bt_q_drop, g_bt_rx_dup);
	fastboot_info(s_line);
	link_close();
	fastboot_okay("");
}

/* oem btlink:p[:SECONDS] wait for a link (default 20 s), oem btlink:c open one */
static void cmd_btlink(const char *arg, void *data, unsigned sz)
{
	unsigned int secs = 20;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (!s_bt_up) { fastboot_fail("bluetooth is off (oem btup)"); return; }
	if (*arg == 'c') {
		unsigned int ival = 6;
		if (arg[1] == ':') {
			ival = 0;
			for (arg += 2; *arg >= '0' && *arg <= '9'; arg++) ival = ival * 10 + (unsigned)(*arg - '0');
			if (ival < 6 || ival > 400) ival = 6;
		}
		link_central(ival);
		return;
	}
	if (arg[0] && arg[1] == ':') {
		secs = 0;
		for (arg += 2; *arg >= '0' && *arg <= '9'; arg++) secs = secs * 10 + (unsigned)(*arg - '0');
		if (!secs || secs > 60) secs = 20;
	}
	link_peripheral(secs);
}

/* ---- the Bluetooth service: a thread that owns the controller while Bluetooth is on ----
 * Started when a Pico screen needs Bluetooth, stopped when it no longer does; nothing of it
 * exists otherwise. It brings the controller up (about 1.35 s, off the caller's thread), then
 * sleeps on the byte pipe's interrupt: the interrupt handler masks the pipe's receive interrupt
 * and wakes the thread, which takes every frame waiting, sends what is queued, re-arms the
 * interrupt and sleeps again. With nothing arriving it costs no time at all; the emulator,
 * on the same single core, only loses the few microseconds each frame takes.
 *
 * Callers use the queues: pico_link_send() puts a message on the outgoing queue,
 * pico_link_recv() takes one off the incoming queue. Messages are up to LINK_MSG_MAX bytes,
 * delivered whole and in order (the link layer joins the radio's pieces). */
#include <kernel/event.h>
#define LINK_MSG_MAX		(LINK_MAX_PAYLOAD + 4)
#define LINK_QUEUE		64		/* the KPA LK has ~0.5 MB of heap left: 160 kept it from booting */
#define BTIF_IER_RX		(1u << 0)

enum { SVC_OFF, SVC_STARTING, SVC_READY, SVC_FAILED };
enum { LINK_NONE, LINK_WAITING, LINK_FINDING, LINK_OPENING, LINK_OPEN, LINK_LISTING };	/* pico_link.h */

/* the consoles heard while listing (pico_link_peers) */
#include "pico_link.h"
#define LINK_PEERS		8
static struct pico_peer s_peers[LINK_PEERS];
static int s_npeers;
static unsigned char s_connect_addr[6];

struct link_msg { unsigned short n; unsigned int t; unsigned char d[LINK_MSG_MAX]; };	/* t: GPT tick it arrived */
static struct link_msg s_txq[LINK_QUEUE], s_rxq[LINK_QUEUE];
static volatile unsigned int s_txq_head, s_txq_n, s_rxq_head, s_rxq_n;

static event_t s_svc_ev;
static thread_t *s_svc_thread;
static volatile int s_svc_state = SVC_OFF, s_svc_stop, s_link_state = LINK_NONE, s_link_want;
static unsigned char s_link_peer[6];
/* Swapping roles (pico_link_lead). With these controllers the peripheral of a link sends one
 * packet per connection event (~30 KB/s) and the central several (~110 KB/s, 11-17), so a console
 * about to send a lot becomes the central first: it asks with a link message of its own
 * (LINK_SWAP_MSG), the central lets its queue and the controller's buffers empty, drops the link
 * and advertises, and the asker connects to it. While it happens the link reads as open: callers
 * keep queueing, the messages go once the new link is up. */
#define LINK_SWAP_MSG		0xFE		/* first byte of the swap request, never an app's */
static int s_link_role;				/* 0 central, 1 peripheral (Connection Complete) */
static volatile int s_link_swap;		/* 1 we asked (to become central), 2 we were asked */
static volatile int s_link_lead_req;		/* a caller wants the lead (pico_link_lead) */
static unsigned int s_link_swap_tick;
volatile unsigned int g_bt_swaps;		/* swaps that completed */
static void svc_scan(int on);
static unsigned int s_link_timer;		/* GPT tick a waiting state began */
volatile unsigned int g_bt_svc_irqs, g_bt_svc_wakes, g_bt_svc_busy_us, g_bt_rxq_drop;
volatile unsigned int g_bt_link_rx, g_bt_link_tx;

/* the byte pipe's interrupt (from the platform's interrupt dispatch): mask it at the pipe, end
 * it at the interrupt controller (every handler here must, or the controller keeps it active
 * and holds back the timer behind it), wake the thread */
void pico_bt_irq(void)
{
	R32(BTIF_BASE + BTIF_IER) = 0;
	mt_irq_ack(PICO_BT_IRQ_ID);
	g_bt_svc_irqs++;
	event_signal(&s_svc_ev, false);
}

static void rxq_put(const unsigned char *p, unsigned int n)
{
	unsigned int slot;
	if (n > LINK_MSG_MAX)
		n = LINK_MSG_MAX;
	enter_critical_section();
	if (s_rxq_n == LINK_QUEUE) {			/* nobody reading: the oldest gives way */
		s_rxq_head = (s_rxq_head + 1) % LINK_QUEUE;
		s_rxq_n--;
		g_bt_rxq_drop++;
	}
	slot = (s_rxq_head + s_rxq_n) % LINK_QUEUE;
	exit_critical_section();
	memcpy(s_rxq[slot].d, p, n);
	s_rxq[slot].n = (unsigned short)n;
	s_rxq[slot].t = gpt4_get_current_tick();
	enter_critical_section();
	s_rxq_n++;
	exit_critical_section();
}

/* the next message from the other console: its length, 0 when none is waiting */
static unsigned int s_link_rx_tick;
int pico_link_recv(void *buf, unsigned int cap)
{
	unsigned int n;
	if (!s_rxq_n)
		return 0;
	n = s_rxq[s_rxq_head].n < cap ? s_rxq[s_rxq_head].n : cap;
	memcpy(buf, s_rxq[s_rxq_head].d, n);
	s_link_rx_tick = s_rxq[s_rxq_head].t;
	enter_critical_section();
	s_rxq_head = (s_rxq_head + 1) % LINK_QUEUE;
	s_rxq_n--;
	exit_critical_section();
	return (int)n;
}

/* the GPT tick the message pico_link_recv() returned last arrived at (netplay timing) */
unsigned int pico_link_rx_tick(void) { return s_link_rx_tick; }

/* a message to the other console: 0 queued, -1 no link, -2 queue full (try again) */
int pico_link_send(const void *p, unsigned int n)
{
	unsigned int slot;
	if (s_link_state != LINK_OPEN && !s_link_swap)
		return -1;				/* (during a swap it waits in the queue) */
	if (n > LINK_MSG_MAX)
		n = LINK_MSG_MAX;
	enter_critical_section();
	if (s_txq_n == LINK_QUEUE) {
		exit_critical_section();
		return -2;
	}
	slot = (s_txq_head + s_txq_n) % LINK_QUEUE;
	exit_critical_section();
	memcpy(s_txq[slot].d, p, n);
	s_txq[slot].n = (unsigned short)n;
	enter_critical_section();
	s_txq_n++;
	exit_critical_section();
	event_signal(&s_svc_ev, true);
	return 0;
}

int pico_link_state(void) { return s_link_swap ? LINK_OPEN : s_link_state; }	/* a swap reads as open */

/* Wait (up to ms) until everything queued has left: our queue empty and every buffer handed to
 * the controller answered as sent. For a last word before the link goes (a cancel): 0 all out, -1
 * still going when the time ran out or the link is gone. */
int pico_link_flush(unsigned int ms)
{
	unsigned int t0 = gpt4_get_current_tick();
	while (s_link_state == LINK_OPEN && (s_txq_n || s_link_credits != s_link_credits_max)) {
		if (GPT_US(gpt4_get_current_tick() - t0) > ms * 1000u)
			return -1;
		thread_sleep(1);
	}
	return s_link_state == LINK_OPEN ? 0 : -1;
}

/* Become the link's central before sending a lot (see LINK_SWAP_MSG); returns at once when already
 * the central. 0 the lead is ours (or the link stays as it was and works, only slower), -1 the
 * link was lost. Called from the sending thread; waits up to 12 s. */
int pico_link_lead(void)
{
	unsigned int t0 = gpt4_get_current_tick();
	if (s_link_state != LINK_OPEN || s_link_role == 0)
		return s_link_state == LINK_OPEN ? 0 : -1;
	s_link_lead_req = 1;
	if (s_svc_thread)
		event_signal(&s_svc_ev, true);
	while (GPT_US(gpt4_get_current_tick() - t0) < 12000000u) {
		if (!s_link_lead_req && !s_link_swap)
			return s_link_state == LINK_OPEN ? 0 : -1;
		thread_sleep(5);
	}
	s_link_lead_req = 0;
	return s_link_state == LINK_OPEN || s_link_swap ? 0 : -1;
}
int pico_bt_service_state(void) { return s_svc_state; }

/* wait for a link (1), go and find a waiting console (2), or close the link (0) */
void pico_link_offer(int offer, int system)
{
	s_adv_offer = (unsigned char)offer;
	s_adv_system = (unsigned char)system;
}

/* the consoles heard while listing, the freshest first, only those heard in the last 4 s */
int pico_link_peers(struct pico_peer *out, int max)
{
	unsigned int now = gpt4_get_current_tick();
	int i, j, n = 0;
	for (i = 0; i < s_npeers; i++) {
		struct pico_peer p = s_peers[i];
		if (GPT_US(now - p.seen_tick) > 4000000u)
			continue;
		/* the freshest first: the smallest time since it was heard */
		for (j = n; j > 0 && j - 1 < max && GPT_US(now - out[j - 1].seen_tick) > GPT_US(now - p.seen_tick); j--)
			if (j < max)
				out[j] = out[j - 1];
		if (j < max)
			out[j] = p;
		if (n < max)
			n++;
	}
	return n;
}

void pico_link_connect(const unsigned char addr[6])
{
	memcpy(s_connect_addr, addr, 6);
	pico_link_want(4);			/* request 5: the link to it */
}

void pico_link_want(int what)
{
	s_link_want = what + 1;			/* 0 = no request pending */
	if (s_svc_thread)
		event_signal(&s_svc_ev, true);
}

/* one frame from the controller, on the service thread */
static void svc_frame(int len)
{
	unsigned int n, iv;
	const unsigned char *p;

	link_note(len);
	if (s_link_state == LINK_OPEN && s_link_handle == 0xffff) {
		if (s_link_swap == 1) {			/* the central let go: find it, connect */
			svc_scan(1);
			s_link_state = LINK_FINDING;
			s_link_timer = gpt4_get_current_tick();
			return;
		}
		s_link_state = LINK_NONE;		/* the other console left */
		return;
	}
	if (len >= 19 && s_rx[0] == 0x04 && s_rx[1] == 0x3e && (s_rx[3] == 0x01 || s_rx[3] == 0x0a) &&
	    (s_link_state == LINK_WAITING || s_link_state == LINK_OPENING)) {
		if (s_rx[4] == 0) {
			unsigned int io = s_rx[3] == 0x01 ? 15 : 27;
			s_link_handle = (unsigned)(s_rx[5] | s_rx[6] << 8) & 0x0fff;
			iv = (unsigned)(s_rx[io] | s_rx[io + 1] << 8);
			s_link_role = s_rx[7];
			memcpy(s_link_peer, s_rx + 9, 6);
			s_reasm_have = s_reasm_want = 0;	/* nothing half joined from a link before */
			if (!s_link_swap)		/* nor whole messages from one: a fresh link starts
							 * empty (a swap keeps what waits to cross) */
				s_rxq_n = s_txq_n = 0;
			bt_log("link open, interval %u, %s", iv, s_link_role ? "peripheral" : "central");
			s_link_state = LINK_OPEN;
			if (s_link_swap) {
				s_link_swap = 0;
				g_bt_swaps++;
			}
			link_tune();
		} else {
			bt_log("link failed, status %02x", s_rx[4]);
			s_link_state = LINK_NONE;
		}
		return;
	}
	if (s_link_state == LINK_LISTING && len >= 14 && s_rx[0] == 0x04 && s_rx[1] == 0x3e &&
	    s_rx[3] == 0x02 && 14u + s_rx[13] <= (unsigned)len) {
		unsigned char offer = 0, system = 0, model;
		model = (unsigned char)bt_pico_info(s_rx + 14, s_rx[13], &offer, &system);
		if (model) {				/* a Pico: noted, or brought up to date */
			int k;
			for (k = 0; k < s_npeers && memcmp(s_peers[k].addr, s_rx + 7, 6); k++)
				;
			if (k == s_npeers && s_npeers < LINK_PEERS)
				s_npeers++;
			if (k < s_npeers) {
				memcpy(s_peers[k].addr, s_rx + 7, 6);
				s_peers[k].model = model;
				s_peers[k].offer = offer;
				s_peers[k].system = system;
				s_peers[k].rssi = 14u + s_rx[13] < (unsigned)len ? (signed char)s_rx[14 + s_rx[13]] : 0;
				s_peers[k].seen_tick = gpt4_get_current_tick();
			}
		}
		return;
	}
	if (s_link_state == LINK_FINDING && len >= 14 && s_rx[0] == 0x04 && s_rx[1] == 0x3e &&
	    s_rx[3] == 0x02 && s_rx[5] == 0x00 &&
	    bt_pico_model(s_rx + 14, 14u + s_rx[13] <= (unsigned)len ? s_rx[13] : 0) &&
	    (s_link_swap != 1 || !memcmp(s_rx + 7, s_link_peer, 6))) {	/* a swap: that console only */
		memcpy(s_link_peer, s_rx + 7, 6);
		s_link_state = LINK_OPENING;		/* the thread opens it next */
		return;
	}
	p = link_payload(len, &n);
	if (p && n == 2 && p[0] == LINK_SWAP_MSG && p[1] == 'S' && s_link_state == LINK_OPEN) {
		s_link_swap = 2;			/* the other console takes the lead */
		s_link_swap_tick = gpt4_get_current_tick();
		return;
	}
	if (p && n) {
		g_bt_link_rx++;
		rxq_put(p, n);
	}
}

/* scanning for a waiting console on (1) or off (0) */
static void svc_scan(int on)
{
	static const unsigned char sparams[] = { 0x01, 0x0b, 0x20, 0x07, 0x01, 0x10, 0x00, 0x10, 0x00,
						 0x01, 0x00 };
	unsigned char en[] = { 0x01, 0x0c, 0x20, 0x02, 0x00, 0x01 };
	if (on)
		link_cmd(sparams, sizeof sparams);
	en[4] = (unsigned char)(on ? 1 : 0);
	/* 2: every advertisement reported (listing keeps the consoles heard fresh); 1: each console
	 * once, the controller filtering the repeats (finding one to connect to) */
	en[5] = on == 2 ? 0x00 : 0x01;
	link_cmd(en, sizeof en);
}

/* what a request or a state needs doing; on the service thread */
static void svc_step(void)
{
	int want = s_link_want;
	if (want) {
		s_link_want = 0;
		if (s_link_state == LINK_WAITING) pico_bt_advertise(0);
		if (s_link_state == LINK_FINDING || s_link_state == LINK_LISTING) svc_scan(0);
		if (s_link_state == LINK_OPEN) link_close();
		s_link_state = LINK_NONE;
		s_link_credits = s_link_credits ? s_link_credits : 4;
		if (want == 2 && !pico_bt_advertise(2))
			s_link_state = LINK_WAITING;
		if (want == 3) {
			svc_scan(1);
			s_link_state = LINK_FINDING;
		}
		if (want == 4) {			/* list the consoles nearby */
			s_npeers = 0;
			svc_scan(2);
			s_link_state = LINK_LISTING;
		}
		if (want == 5) {			/* open a link to the one picked */
			memcpy(s_link_peer, s_connect_addr, 6);
			s_link_handle = 0xffff;
			s_link_state = LINK_OPENING;
		}
		s_link_timer = gpt4_get_current_tick();
	}
	if (s_link_state == LINK_OPENING && s_link_handle == 0xffff) {
		/* the connection event 6.25 to 7.5 ms long (units of 0.625 ms), most of the 7.5 ms
		 * interval: asked as 0 the controller kept to one exchange an event, and what the other
		 * console sends us crossed one 251 byte frame each 7.5 ms (~32 KB/s, 11-17) */
		unsigned char conn[] = { 0x01, 0x0d, 0x20, 0x19, 0x10, 0x00, 0x10, 0x00, 0x00,
			0x01, 0, 0, 0, 0, 0, 0, 0x01, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00, 0x64, 0x00,
			0x0a, 0x00, 0x0c, 0x00 };
		svc_scan(0);
		memcpy(conn + 10, s_link_peer, 6);
		if (link_cmd(conn, sizeof conn) != 0) {
			bt_log("link refused");
			s_link_state = LINK_NONE;
		}
		s_link_handle = 0xfffe;			/* asked: wait for the event */
		s_link_timer = gpt4_get_current_tick();
	}
	if (s_link_state == LINK_OPENING &&
	    GPT_US(gpt4_get_current_tick() - s_link_timer) > 5000000u) {
		static const unsigned char cancel[] = { 0x01, 0x0e, 0x20, 0x00 };
		link_cmd(cancel, sizeof cancel);
		bt_log("link did not open");
		s_link_handle = 0xffff;
		s_link_state = LINK_NONE;
	}
	/* Swapping roles: both sides first let their queue and the controller's buffers empty (a
	 * message still in them would be lost with the link), then the asker sends the request and
	 * the central lets go and advertises. Nothing for 10 s: the link is taken as lost. */
	{
		int drained = !s_txq_n && s_link_credits == s_link_credits_max;
		if (s_link_lead_req && s_link_state == LINK_OPEN && !s_link_swap) {
			if (s_link_role == 0) {
				s_link_lead_req = 0;		/* already the central */
			} else if (drained) {
				static const unsigned char rq[2] = { LINK_SWAP_MSG, 'S' };
				s_link_lead_req = 0;
				s_link_swap = 1;
				s_link_swap_tick = gpt4_get_current_tick();
				link_send(rq, sizeof rq);
			}
		}
		if (s_link_swap == 2 && s_link_state == LINK_OPEN && s_link_handle != 0xffff &&
		    (drained || GPT_US(gpt4_get_current_tick() - s_link_swap_tick) > 1000000u)) {
			link_close();
			if (!pico_bt_advertise(2))
				s_link_state = LINK_WAITING;
			else
				s_link_state = LINK_NONE;
			s_link_timer = gpt4_get_current_tick();
		}
		if (s_link_swap && GPT_US(gpt4_get_current_tick() - s_link_swap_tick) > 10000000u) {
			if (s_link_state == LINK_WAITING) pico_bt_advertise(0);
			if (s_link_state == LINK_FINDING) svc_scan(0);
			s_link_swap = 0;
			s_link_state = LINK_NONE;
			bt_log("role swap failed");
		}
	}
	/* The controller gives buffers back with an event; one lost on the way (the byte pipe is
	 * polled, a burst can overrun it) loses them for good, and after enough of those the link
	 * stopped sending for ever (11-17: a 16 MB netplay start stalled at 3.5 MB). Messages
	 * waiting, no buffer and nothing back for a second: the radio has long sent them all,
	 * so all are taken back as free. */
	if (s_link_state == LINK_OPEN && s_txq_n && !s_link_credits && s_link_credits_max &&
	    GPT_US(gpt4_get_current_tick() - s_link_credit_tick) > 1000000u) {
		s_link_credits = s_link_credits_max;
		s_link_credit_tick = gpt4_get_current_tick();
		g_bt_credit_restores++;
	}
	/* the outgoing queue, as far as the controller has buffers */
	while (s_link_state == LINK_OPEN && s_txq_n && s_link_credits) {
		if (link_send(s_txq[s_txq_head].d, s_txq[s_txq_head].n) != 0)
			break;
		g_bt_link_tx++;
		enter_critical_section();
		s_txq_head = (s_txq_head + 1) % LINK_QUEUE;
		s_txq_n--;
		exit_critical_section();
	}
}

static int svc_thread(void *arg)
{
	(void)arg;
	if (pico_bt_up() != 0 || link_prepare() != 0) {
		s_svc_state = SVC_FAILED;
		pico_bt_off();
		s_svc_thread = 0;
		return -1;
	}
	s_link_handle = 0xffff;
	s_svc_state = SVC_READY;
	mt_irq_set_sens(PICO_BT_IRQ_ID, MT65xx_LEVEL_SENSITIVE);
	mt_irq_set_polarity(PICO_BT_IRQ_ID, MT65xx_POLARITY_LOW);
	mt_irq_unmask(PICO_BT_IRQ_ID);
	while (!s_svc_stop) {
		unsigned int t0 = gpt4_get_current_tick();
		int len;
		g_bt_svc_wakes++;
		while ((len = bt_rx(0)) >= 0)
			if (len > 0)
				svc_frame(len);
		svc_step();
		g_bt_svc_busy_us += GPT_US(gpt4_get_current_tick() - t0);
		/* sleep until the pipe has a byte for us, a caller queues something, or 50 ms pass
		 * (the timers above) */
		R32(BTIF_BASE + BTIF_IER) = BTIF_IER_RX;
		/* awake again at once only for work it can do now: a byte waiting, a request, or
		 * queued messages the controller has buffers for. Messages waiting for buffers wait
		 * for the controller's event that frees them, which comes in through the interrupt */
		if (!(R32(BTIF_BASE + BTIF_LSR) & LSR_DR) && !s_link_want &&
		    !(s_txq_n && s_link_credits && s_link_state == LINK_OPEN))
			event_wait_timeout(&s_svc_ev, 50);
		R32(BTIF_BASE + BTIF_IER) = 0;
	}
	mt_irq_mask(PICO_BT_IRQ_ID);
	if (s_link_state == LINK_WAITING) pico_bt_advertise(0);
	if (s_link_state == LINK_FINDING) svc_scan(0);
	if (s_link_state == LINK_OPEN) link_close();
	s_link_state = LINK_NONE;
	pico_bt_off();
	s_svc_state = SVC_OFF;
	s_svc_thread = 0;
	return 0;
}

/* Bluetooth on, in the background; 0 started (or already on) */
int pico_bt_service_start(void)
{
	static int ev_init;
	if (s_svc_thread || s_svc_state == SVC_READY)
		return 0;
	if (!ev_init) {
		event_init(&s_svc_ev, false, EVENT_FLAG_AUTOUNSIGNAL);
		ev_init = 1;
	}
	s_svc_stop = 0;
	s_txq_n = s_rxq_n = 0;
	s_link_state = LINK_NONE;
	s_svc_state = SVC_STARTING;
	g_bt_svc_irqs = g_bt_svc_wakes = g_bt_svc_busy_us = 0;
	/* the default priority: it sleeps or yields whenever it waits, so it never holds the CPU,
	 * and a frame is handled as soon as it lands */
	/* A little above the menu and fastboot (DEFAULT), below the GBA CPU (HIGH). It sleeps on its
	 * event nearly all the time; at DEFAULT the menu thread kept it waiting for the rest of its
	 * time slice after the interrupt woke it, so each wake sent 8 frames and the radio idled in
	 * between (the KPA sent at ~30 KB/s, 11-17). */
	s_svc_thread = thread_create("pico_bt", svc_thread, 0, DEFAULT_PRIORITY + 4, 16384);
	if (!s_svc_thread) {
		s_svc_state = SVC_FAILED;
		return -1;
	}
	thread_resume(s_svc_thread);
	return 0;
}

/* Bluetooth fully off, waiting up to 2 s for the thread to finish */
void pico_bt_service_stop(void)
{
	unsigned int t0 = gpt4_get_current_tick();
	if (!s_svc_thread)
		return;
	s_svc_stop = 1;
	event_signal(&s_svc_ev, true);
	while (s_svc_thread && GPT_US(gpt4_get_current_tick() - t0) < 2000000u)
		thread_sleep(10);
}

/* oem btsvc[:1|0|p|c|s:TEXT|r] - the service: on, off, wait for a link, find one, send a
 * message, read one; alone: its state and its cost */
static void cmd_btsvc(const char *arg, void *data, unsigned sz)
{
	static const char *const sv[] = { "off", "starting", "ready", "failed" };
	static const char *const lk[] = { "none", "waiting", "finding", "opening", "open", "listing" };
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (*arg == 'l') pico_link_want(3);		/* list the Picos nearby */
	else if (*arg == 'o' && arg[1] == ':') {	/* o:OFFER:SYSTEM (decimal) what we advertise */
		const char *q = arg + 2;
		int offer = 0, system = 0;
		while (*q >= '0' && *q <= '9') offer = offer * 10 + (*q++ - '0');
		if (*q == ':') q++;
		while (*q >= '0' && *q <= '9') system = system * 10 + (*q++ - '0');
		pico_link_offer(offer, system);
	} else if (*arg == 'n') {			/* the Picos heard */
		struct pico_peer pp[LINK_PEERS];
		int i, n = pico_link_peers(pp, LINK_PEERS);
		for (i = 0; i < n; i++) {
			snprintf(s_line, sizeof s_line, "%02x:%02x:%02x:%02x:%02x:%02x model %u offer %u sys %u %d dBm",
				 pp[i].addr[5], pp[i].addr[4], pp[i].addr[3], pp[i].addr[2], pp[i].addr[1],
				 pp[i].addr[0], pp[i].model, pp[i].offer, pp[i].system, pp[i].rssi);
			fastboot_info(s_line);
		}
	} else if (*arg == 'k') {			/* open a link to the freshest one heard */
		struct pico_peer pp[1];
		if (pico_link_peers(pp, 1) == 1)
			pico_link_connect(pp[0].addr);
	} else if (*arg == '1') pico_bt_service_start();
	else if (*arg == '0') pico_bt_service_stop();
	else if (*arg == 'p') pico_link_want(1);
	else if (*arg == 'c') pico_link_want(2);
	else if (*arg == 'x') pico_link_want(0);
	else if (*arg == 's' && arg[1] == ':') {
		int rc = pico_link_send(arg + 2, (unsigned)strlen(arg + 2));
		snprintf(s_line, sizeof s_line, "send rc %d", rc);
		fastboot_info(s_line);
	} else if (*arg == 'r') {
		unsigned char m[64];
		int n = pico_link_recv(m, sizeof m - 1);
		m[n > 0 ? n : 0] = 0;
		snprintf(s_line, sizeof s_line, "recv %d: %s", n, n > 0 ? (char *)m : "");
		fastboot_info(s_line);
	}
	snprintf(s_line, sizeof s_line, "service %s, link %s", sv[s_svc_state & 3],
		 lk[s_link_state <= LINK_LISTING ? s_link_state : 0]);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "irqs %u wakes %u busy %u us rx %u tx %u q %u/%u",
		 g_bt_svc_irqs, g_bt_svc_wakes, g_bt_svc_busy_us, g_bt_link_rx, g_bt_link_tx,
		 s_rxq_n, s_txq_n);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "received messages dropped (queue full): %u", g_bt_rxq_drop);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "controller buffers %u of %u, taken back %u", s_link_credits,
		 s_link_credits_max, g_bt_credit_restores);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "radio: phy %u out / %u in, frame %u bytes out", s_link_phy_tx,
		 s_link_phy_rx, s_link_dl);
	fastboot_info(s_line);
	snprintf(s_line, sizeof s_line, "role %s, swaps %u", s_link_role ? "peripheral" : "central", g_bt_swaps);
	fastboot_info(s_line);
	fastboot_okay("");
}

/* the console-neutral commands; the console's file adds its own */
static void pico_bt_register_cmds(void)
{
	fastboot_register("oem btscan", cmd_btscan, 1, 0);
	fastboot_register("oem btadv", cmd_btadv, 1, 0);
	fastboot_register("oem btlink", cmd_btlink, 1, 0);
	fastboot_register("oem btsvc", cmd_btsvc, 1, 0);
	{ extern void pico_xfer_register_cmds(void); pico_xfer_register_cmds(); }	/* pico_xfer.c */
	{ extern void pico_np_register_cmds(void); pico_np_register_cmds(); }		/* pico_netplay.c */
	fastboot_register("oem btwmt:", cmd_btwmt, 1, 0);
	fastboot_register("oem btup", cmd_btup, 1, 0);
	fastboot_register("oem btlog", cmd_btlog, 1, 0);
	fastboot_register("oem bthci:", cmd_bthci, 1, 0);
}
