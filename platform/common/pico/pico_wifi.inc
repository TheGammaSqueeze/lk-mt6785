/*
 * pico_wifi.inc - Wi-Fi in the GammaOS Pico LK: the part every console with this family of
 * MediaTek connectivity subsystem (CONNSYS) shares. Included by the console's own file after
 * pico_bt.inc, whose power-up it rides on: CONNSYS running is all the Wi-Fi block needs before its
 * driver takes over. The console's file defines first:
 *
 *   PICO_WIFI_FW_RAM          the Wi-Fi CPU's RAM code file name (in BT_FW_DIR on the card)
 *   WIFI_DMA_PA, WIFI_DMA_SIZE  the DRAM the Wi-Fi block's DMA works in: its rings, their
 *                             buffers and the RAM code while it is sent (cut out of rewind)
 *
 * In here: the bus taken from the low power controller, the DMA rings, the RAM code (its parts
 * meant for the firmware window copied there, the rest sent down the download ring), the start
 * command and the ready bits, and the bootloader commands to drive and watch it.
 *
 * The Wi-Fi block sits in the CONNSYS register window at 0x18000000: its DMA engine at +0x4000,
 * the host side of the low power controller at +0x7000, the always-on misc registers (where the
 * firmware says it is ready) at +0xC0000.
 */

/* ---- registers ---- */
#define WF_WIN			0x18000000u
#define WF_DMA			(WF_WIN + 0x4000u)
#define WF_DMA_INT_STA		(WF_DMA + 0x200u)
#define WF_DMA_INT_MASK		(WF_DMA + 0x204u)
#define WF_DMA_CFG		(WF_DMA + 0x208u)
#define WF_DMA_IDX_RESET	(WF_DMA + 0x20Cu)
#define WF_DMA_TX_HOLD		(WF_DMA + 0x224u)
#define WF_TXQ(n)		(WF_DMA + 0x300u + 0x10u * (n))	/* +0 ring, +4 size, +8 ours, +C the DMA's */
#define WF_RXQ(n)		(WF_DMA + 0x400u + 0x10u * (n))
#define WF_TXQ_HI(n)		(WF_DMA + 0x500u + 4u * (n))	/* a ring's address bits past 32 */
#define WF_RXQ_HI(n)		(WF_DMA + 0x580u + 4u * (n))
#define WF_OWN			(WF_WIN + 0x7000u)
#define WF_OWN_FW		(1u << 0)		/* read: the firmware has the bus */
#define WF_OWN_TAKE		(1u << 1)		/* write: the host takes it */
#define WF_READY_REG		(WF_WIN + 0xC1140u)
#define WF_READY_BITS		(3u << 1)		/* both set: the RAM code runs */

/* the DMA engine's settings (WF_DMA_CFG) */
#define DMA_TX_ON		(1u << 0)
#define DMA_TX_BUSY		(1u << 1)
#define DMA_RX_ON		(1u << 2)
#define DMA_RX_BUSY		(1u << 3)
#define DMA_BURST_MASK		(3u << 4)
#define DMA_BURST_LONG		(3u << 4)
#define DMA_TX_DONE_BACK	(1u << 6)		/* the DMA writes the done bit back into a TX entry */
#define DMA_RING3_DIRECT	(1u << 9)		/* ring 3 skips the scheduler: the download */
#define DMA_MULTI_MASK		(3u << 10)
#define DMA_MULTI		(2u << 10)
#define DMA_LITTLE_ENDIAN	(1u << 12)
#define DMA_NO_CLOCK_GATE	(1u << 30)

/* ---- the DMA memory ----
 * A ring is an array of 16 byte entries: word 0 the buffer's address, word 1 its length in bits
 * 16..29 with "last piece" at bit 30 and "done" at bit 31 (a second buffer's length and last bit
 * below, unused here), words 2 and 3 a second buffer and the high address bits, unused. */
#define WF_TXN			32			/* download ring entries */
#define WF_RXN			8			/* entries of the answer ring (1) */
#define WF_RX0N			32			/* entries of the frame ring (0): beacons come in bursts */
#define WF_BUF			4096u			/* each entry's buffer */
#define WF_TXQ_RING		0x00000u		/* ring 3 entries */
#define WF_CMDQ_RING		0x00800u		/* ring 15 entries (commands once the RAM code runs) */
#define WF_DATAQ_RING		0x01400u		/* ring 0 entries (802.11 frames out) */
#define WF_DATAQ_N		64			/* ring 0 entries (frames out); at most 64: s_wf_tx_busy */
#define WF_RXQ0_RING		0x01000u		/* RX ring 0 entries (data: kept fed) */
#define WF_RXQ1_RING		0x01200u		/* RX ring 1 entries (the firmware's answers) */
#define WF_TX_BUFS		0x10000u		/* WF_TXN buffers */
#define WF_RX0_BUFS		0x80000u		/* WF_RX0N buffers (128 KB) */
#define WF_RX1_BUFS		0x50000u
#define WF_CMD_BUFS		0x60000u		/* WF_TXN buffers for ring 15 */
#define WF_DATA_BUFS		0xA0000u		/* WF_DATAQ_N buffers for ring 0: descriptor, append, frame */
#define WF_DATA_STRIDE		2048u			/* each: 32 + 32 + an Ethernet frame (1514) */
#define WF_FW_STAGE		0x100000u		/* the RAM code file while it is sent */
#define WF_NET_WORK		0x1C0000u		/* the network stack's memory (TCP buffers, downloads) */
#define WF_NET_WORK_LEN		0x040000u		/* PICO_TCP_SLOTS * PICO_TCP_MEM */
#define WF_ROOTS_AREA		0x030000u		/* free after the download ring's buffers: trusted roots */
#define WF_ROOTS_AREA_LEN	0x020000u
#define WF_LIST_AREA		0x0C0000u		/* free after the data ring's buffers: the game list */
#define WF_LIST_AREA_LEN	0x040000u
#define WF_FW_MAX		(WIFI_DMA_SIZE - WF_FW_STAGE)
#define WF_DESC_DONE		(1u << 31)
#define WF_DESC_LAST		(1u << 30)

#define WF_AT(off)		((volatile unsigned int *)(addr_t)(WIFI_DMA_PA + (off)))

/* ---- the commands the boot ROM takes before the RAM code runs ----
 * Each packet: a 32 byte header the DMA reads (total length, port and queue, format 0xA0, kind:
 * 2 a command, 3 image data), then a 32 byte header the ROM reads (length past the first header,
 * port 0x8000, command id, 0xA0, sequence), then the command's own fields or image bytes. */
#define WF_HDR			64u
#define WF_CHUNK		2048u			/* image bytes a packet carries */
#define WF_CMD_DL_CONFIG	1			/* address, length, mode */
#define WF_CMD_START		2			/* override flags, address */
#define WF_PORT_CMD		0x8000u
#define WF_PORT_DATA		0xF800u
#define WF_KIND_CMD		2
#define WF_KIND_DATA		3
#define WF_EVT_RESULT		1			/* answer: status (0 good), the command id */
#define WF_DL_ENCRYPTED		(1u << 0)		/* a download config's mode bits */
#define WF_DL_RESET		(1u << 3)
#define WF_DL_SCRAMBLE		(1u << 6)
#define WF_DL_ACK		(1u << 31)

/* the RAM code file: regions one after another, then one 40 byte record each, then a 36 byte
 * tail (the region count is its third byte) */
#define WF_TAIL			36u
#define WF_REGION_REC		40u
#define WF_FEAT_ENCRYPTED	0x01u
#define WF_FEAT_KEY		0x06u
#define WF_FEAT_SCRAMBLE	0x10u
#define WF_FEAT_IN_WINDOW	0x80u			/* lives in the firmware window: copied there */

static int s_wf_up;			/* the RAM code runs */
static int s_wf_station;		/* our station network is on (wf_station_on) */
static int s_wf_linked;			/* joined: open, or WPA2 with the keys in */
static int s_wf_coop;			/* the network service drives the radio: waits sleep, not spin */
static volatile int s_wf_abort;		/* the transfer running was given up (netsvc_http_abort): it ends now */
#include <kernel/mutex.h>
static mutex_t s_ns_info;		/* the network service's state (pico_netsvc.h, below) */
static long long s_ns_time;		/* the network's time for the menu to set (0 none), as of s_ns_time_tick */
static unsigned int s_ns_time_tick;
/* the time as the menu thread last read it (0 not known), as of s_now_tick: other threads use it
 * with the time since, so only the menu thread ever talks to the RTC (behind the PMIC wrapper,
 * which two threads at once can wedge: both consoles hung that way on 11-18) */
static long long s_now_utc;
static unsigned int s_now_tick;
static long long wf_time_now(void)
{
	long long t;
	unsigned int tick;
	mutex_acquire(&s_ns_info);
	t = s_now_utc;
	tick = s_now_tick;
	mutex_release(&s_ns_info);
	return t ? t + (long long)(GPT_US(gpt4_get_current_tick() - tick) / 1000000u) : 0;
}
static int s_wf_join_dbm;		/* the joined AP's signal when it was joined */
static unsigned long long s_wf_tx_busy;	/* ring 0 entries the radio may still read (wf_tx_freed) */
static unsigned int s_wf_tx_reused;	/* entries reused with no report (should stay 0) */
static int s_wf_link_protect;		/* frames out are encrypted */
static int s_wf_mapped;
static unsigned int s_wf_tx;		/* our next ring 3 entry */
static unsigned int s_wf_cmdq;		/* our next ring 15 entry */
static unsigned int s_wf_dataq;		/* our next ring 0 entry */
static unsigned int s_wf_rx1;		/* the last answer entry handed back */
static unsigned int s_wf_rx0;		/* the last frame entry handed back */
static unsigned char s_wf_seq;
static unsigned char s_wf_pkt[WF_HDR + WF_CHUNK];

static void wf_barrier(void) { __asm__ volatile("dsb" ::: "memory"); }

static unsigned int wf_rd32le(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}
static void wf_wr32le(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* 0 when the DMA memory is DRAM the preloader marked usable, and mapped (uncached) */
static int wf_dma_window(void)
{
	unsigned int i;
	int ok = 0;

	if (s_wf_mapped)
		return 0;
	if (!g_boot_arg)
		return -1;
	for (i = 0; i < g_boot_arg->mblock_info.mblock_num && i < 128u; i++) {
		unsigned long long s = g_boot_arg->mblock_info.mblock[i].start;
		unsigned long long e = s + g_boot_arg->mblock_info.mblock[i].size;
		if (s <= WIFI_DMA_PA && WIFI_DMA_PA + (unsigned long long)WIFI_DMA_SIZE <= e)
			ok = 1;
	}
	if (!ok) {
		bt_log("wifi dma %08x not usable DRAM", WIFI_DMA_PA);
		return -2;
	}
	if (arch_mmu_map((uint64_t)WIFI_DMA_PA, (vaddr_t)WIFI_DMA_PA,
			 MMU_MEMORY_TYPE_NORMAL | MMU_MEMORY_AP_P_RW_U_NA, WIFI_DMA_SIZE) != 0) {
		bt_log("wifi dma map failed");
		return -3;
	}
	s_wf_mapped = 1;
	return 0;
}

/* the bus from the low power controller to us */
static int wf_take_bus(void)
{
	R32(WF_OWN) = WF_OWN_TAKE;
	if (bt_wait(WF_OWN, WF_OWN_FW, 0, 100000) != 0) {
		bt_log("wifi bus not ours: %08x", R32(WF_OWN));
		return -1;
	}
	return 0;
}

/* the DMA engine stopped, every ring index back to 0 */
static int wf_dma_stop(void)
{
	R32(WF_DMA_CFG) &= ~(DMA_TX_ON | DMA_RX_ON);
	if (bt_wait(WF_DMA_CFG, DMA_TX_BUSY | DMA_RX_BUSY, 0, 100000) != 0) {
		bt_log("wifi dma stays busy: %08x", R32(WF_DMA_CFG));
		return -1;
	}
	R32(WF_DMA_IDX_RESET) = 0xffffffffu;
	return 0;
}

/* a receive ring of cnt entries: every one an empty buffer the DMA may fill */
static void wf_rx_ring(unsigned int n, unsigned int ring_off, unsigned int bufs_off, unsigned int cnt)
{
	unsigned int i;
	for (i = 0; i < cnt; i++) {
		volatile unsigned int *d = WF_AT(ring_off + 16u * i);
		d[0] = WIFI_DMA_PA + bufs_off + WF_BUF * i;
		d[1] = WF_BUF << 16;
		d[2] = 0;
		d[3] = 0;
	}
	wf_barrier();
	R32(WF_RXQ(n)) = WIFI_DMA_PA + ring_off;
	R32(WF_RXQ_HI(n)) = 0;
	R32(WF_RXQ(n) + 4) = cnt;
	R32(WF_RXQ(n) + 8) = cnt - 1;
}

/* the rings set up and the DMA engine on, ring 3 straight to the firmware */
static void wf_dma_start(void)
{
	unsigned int i, c;

	for (i = 0; i < WF_TXN; i++) {
		volatile unsigned int *d = WF_AT(WF_TXQ_RING + 16u * i);
		d[0] = d[1] = d[2] = d[3] = 0;
	}
	wf_barrier();
	R32(WF_TXQ(3)) = WIFI_DMA_PA + WF_TXQ_RING;
	R32(WF_TXQ_HI(3)) = 0;
	R32(WF_TXQ(3) + 4) = WF_TXN;
	R32(WF_TXQ(3) + 8) = 0;
	s_wf_tx = 0;
	for (i = 0; i < WF_TXN; i++) {
		volatile unsigned int *d = WF_AT(WF_CMDQ_RING + 16u * i);
		d[0] = d[1] = d[2] = d[3] = 0;
	}
	wf_barrier();
	R32(WF_TXQ(15)) = WIFI_DMA_PA + WF_CMDQ_RING;
	R32(WF_TXQ_HI(15)) = 0;
	R32(WF_TXQ(15) + 4) = WF_TXN;
	R32(WF_TXQ(15) + 8) = 0;
	s_wf_cmdq = 0;
	for (i = 0; i < WF_DATAQ_N; i++) {
		volatile unsigned int *d = WF_AT(WF_DATAQ_RING + 16u * i);
		d[0] = d[1] = d[2] = d[3] = 0;
	}
	wf_barrier();
	R32(WF_TXQ(0)) = WIFI_DMA_PA + WF_DATAQ_RING;
	R32(WF_TXQ_HI(0)) = 0;
	R32(WF_TXQ(0) + 4) = WF_DATAQ_N;
	R32(WF_TXQ(0) + 8) = 0;
	s_wf_dataq = 0;
	s_wf_tx_busy = 0;
	wf_rx_ring(0, WF_RXQ0_RING, WF_RX0_BUFS, WF_RX0N);
	wf_rx_ring(1, WF_RXQ1_RING, WF_RX1_BUFS, WF_RXN);
	s_wf_rx1 = WF_RXN - 1;
	s_wf_rx0 = WF_RX0N - 1;
	R32(WF_DMA_INT_MASK) = 0;		/* polled */
	R32(WF_DMA_TX_HOLD) = 0;
	c = R32(WF_DMA_CFG) & ~(DMA_BURST_MASK | DMA_MULTI_MASK);
	R32(WF_DMA_CFG) = c | DMA_TX_ON | DMA_RX_ON | DMA_BURST_LONG | DMA_TX_DONE_BACK | DMA_RING3_DIRECT |
			  DMA_MULTI | DMA_LITTLE_ENDIAN | DMA_NO_CLOCK_GATE;
}

/* one packet down TX ring n (3 or 15), whose entries are at ring_off, buffers at bufs_off and our
 * next entry *idx; 0 once the DMA has taken it */
static int wf_send_on(unsigned int n, unsigned int ring_off, unsigned int bufs_off, unsigned int *idx,
		      const unsigned char *pkt, unsigned int len)
{
	unsigned int i = *idx, next = (i + 1) % WF_TXN, t0;
	volatile unsigned int *d = WF_AT(ring_off + 16u * i);
	unsigned char *buf = (unsigned char *)(addr_t)(WIFI_DMA_PA + bufs_off + WF_BUF * i);

	if (len > WF_BUF)
		return -1;
	memcpy(buf, pkt, len);
	d[0] = WIFI_DMA_PA + bufs_off + WF_BUF * i;
	d[1] = len << 16 | WF_DESC_LAST;
	d[2] = 0;
	d[3] = 0;
	wf_barrier();
	*idx = next;
	R32(WF_TXQ(n) + 8) = next;
	t0 = gpt4_get_current_tick();
	while (R32(WF_TXQ(n) + 12) != next) {
		if (GPT_US(gpt4_get_current_tick() - t0) > 200000u) {
			bt_log("wifi tx%u stuck: ours %u dma %u cfg %08x", n, next, R32(WF_TXQ(n) + 12), R32(WF_DMA_CFG));
			return -2;
		}
	}
	return 0;
}
/* the download ring: the boot ROM's packets */
static int wf_send(const unsigned char *pkt, unsigned int len)
{
	return wf_send_on(3, WF_TXQ_RING, WF_TX_BUFS, &s_wf_tx, pkt, len);
}

/* len bytes out of the Wi-Fi memory (mapped without the cache): a byte at a time it costs a bus
 * read each, so whole words, four per turn, while both sides allow it */
static void wf_copy_in(unsigned char *dst, const unsigned char *src, unsigned int len)
{
	if (!(((addr_t)dst | (addr_t)src) & 3u)) {
		const volatile unsigned int *s = (const volatile unsigned int *)src;
		unsigned int *d = (unsigned int *)dst;
		for (; len >= 16; len -= 16, s += 4, d += 4) {
			unsigned int a = s[0], b = s[1], c = s[2], e = s[3];
			d[0] = a; d[1] = b; d[2] = c; d[3] = e;
		}
		for (; len >= 4; len -= 4)
			*d++ = *s++;
		dst = (unsigned char *)d;
		src = (const unsigned char *)s;
	}
	while (len--)
		*dst++ = *src++;
}

/* the next answer from ring 1 into out (the whole received buffer, at most cap bytes): its length,
 * or -1 when none came within timeout_us */
static int wf_answer(unsigned char *out, unsigned int cap, unsigned int timeout_us)
{
	unsigned int i = (s_wf_rx1 + 1) % WF_RXN, len, t0 = gpt4_get_current_tick();
	volatile unsigned int *d = WF_AT(WF_RXQ1_RING + 16u * i);

	while (!(d[1] & WF_DESC_DONE)) {
		if (GPT_US(gpt4_get_current_tick() - t0) > timeout_us)
			return -1;
		if (s_wf_coop && timeout_us >= 1000)
			thread_sleep(1);		/* the service thread waits asleep: the menu and games run */
	}
	len = (d[1] >> 16) & 0x3fffu;
	wf_copy_in(out, (const unsigned char *)(addr_t)(WIFI_DMA_PA + WF_RX1_BUFS + WF_BUF * i), len < cap ? len : cap);
	d[1] = WF_BUF << 16;			/* back to the DMA */
	wf_barrier();
	s_wf_rx1 = i;
	R32(WF_RXQ(1) + 8) = i;
	return (int)len;
}

/* answers already waiting (the ROM may note image packets too): read and dropped, so the next
 * command reads its own; how many there were */
static unsigned int wf_drain(void)
{
	unsigned char ev[32];
	unsigned int n = 0;
	while (n < WF_RXN && wf_answer(ev, sizeof ev, 2000) >= 0)
		n++;
	return n;
}

/* the next received frame from ring 0 into out (at most cap bytes): its length, or -1 when none
 * is waiting */
static int wf_frame(unsigned char *out, unsigned int cap)
{
	unsigned int i = (s_wf_rx0 + 1) % WF_RX0N, len;
	volatile unsigned int *d = WF_AT(WF_RXQ0_RING + 16u * i);

	if (!(d[1] & WF_DESC_DONE))
		return -1;
	len = (d[1] >> 16) & 0x3fffu;
	wf_copy_in(out, (const unsigned char *)(addr_t)(WIFI_DMA_PA + WF_RX0_BUFS + WF_BUF * i), len < cap ? len : cap);
	d[1] = WF_BUF << 16;
	wf_barrier();
	s_wf_rx0 = i;
	R32(WF_RXQ(0) + 8) = i;
	return (int)len;
}

/* the two headers of a packet for the boot ROM into s_wf_pkt; its total length */
static unsigned int wf_packet(int data, unsigned int cid, unsigned int body)
{
	unsigned char *p = s_wf_pkt;
	unsigned int total = WF_HDR + body, port = data ? WF_PORT_DATA : WF_PORT_CMD;

	memset(p, 0, WF_HDR);
	p[0] = (unsigned char)total; p[1] = (unsigned char)(total >> 8);
	p[2] = (unsigned char)port; p[3] = (unsigned char)(port >> 8);
	p[5] = 0xA0;
	p[7] = data ? WF_KIND_DATA : WF_KIND_CMD;
	p[32] = (unsigned char)(total - 32); p[33] = (unsigned char)((total - 32) >> 8);
	p[34] = 0x00; p[35] = 0x80;
	p[36] = (unsigned char)cid;
	p[37] = 0xA0;
	p[39] = s_wf_seq++;
	return total;
}

/* a command with its fields, then its answer: 0 when the ROM says done */
static int wf_command(unsigned int cid, const unsigned char *fields, unsigned int n)
{
	unsigned char ev[64];
	unsigned int len = wf_packet(0, cid, n);
	int got;

	unsigned int stale = wf_drain();
	if (stale)
		bt_log("wifi cmd %u: %u answers were waiting", cid, stale);
	memcpy(s_wf_pkt + WF_HDR, fields, n);
	if (wf_send(s_wf_pkt, len) != 0)
		return -1;
	got = wf_answer(ev, sizeof ev, 500000);
	if (got < 0) {
		bt_log("wifi cmd %u: no answer (int %08x)", cid, R32(WF_DMA_INT_STA));
		return -2;
	}
	/* a 16 byte receive header, then length, 0xE000, event id, sequence, 2 spare, the fields */
	if (got < 26 || ev[18] != 0x00 || ev[19] != 0xE0 || ev[20] != WF_EVT_RESULT) {
		bt_log("wifi cmd %u: answer %d: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x", cid, got,
		       ev[16], ev[17], ev[18], ev[19], ev[20], ev[21], ev[24], ev[25], ev[26], ev[27]);
		return -3;
	}
	if (ev[24] != 0) {
		bt_log("wifi cmd %u: status %u", cid, ev[24]);
		return -4;
	}
	return 0;
}

/* one region of the RAM code to the firmware: where it goes, then its bytes */
static int wf_send_region(unsigned int addr, const unsigned char *src, unsigned int len, unsigned int feat)
{
	unsigned char f[12];
	unsigned int mode = WF_DL_ACK, pos;

	if (feat & WF_FEAT_ENCRYPTED) {
		mode |= WF_DL_RESET | WF_DL_ENCRYPTED | (feat & WF_FEAT_KEY);
		if (feat & WF_FEAT_SCRAMBLE)
			mode |= WF_DL_SCRAMBLE;
	}
	wf_wr32le(f, addr);
	wf_wr32le(f + 4, len);
	wf_wr32le(f + 8, mode);
	if (wf_command(WF_CMD_DL_CONFIG, f, sizeof f) != 0)
		return -1;
	for (pos = 0; pos < len; pos += WF_CHUNK) {
		unsigned int n = len - pos < WF_CHUNK ? len - pos : WF_CHUNK;
		unsigned int total = wf_packet(1, 0, n);
		memcpy(s_wf_pkt + WF_HDR, src + pos, n);
		if (wf_send(s_wf_pkt, total) != 0)
			return -2;
	}
	return 0;
}

/* the RAM code from the card: the window's regions copied there, the others sent; then start */
static int wf_load_ram_code(void)
{
	char path[80];
	fat_file f;
	fat_vol *v = gba_sd_menu_vol();
	unsigned char *img = (unsigned char *)(addr_t)(WIFI_DMA_PA + WF_FW_STAGE), *tail, start[8];
	unsigned int size, regions, i, off;

	snprintf(path, sizeof path, "%s%s", BT_FW_DIR, PICO_WIFI_FW_RAM);
	if (!v || fat_open(v, path, &f) != 0) {
		bt_log("wifi fw missing: %s", PICO_WIFI_FW_RAM);
		return -1;
	}
	size = f.size;
	if (size <= WF_TAIL || size > WF_FW_MAX || fat_read(&f, 0, img, size) != size) {
		bt_log("wifi fw unreadable: %u bytes", size);
		return -2;
	}
	tail = img + size - WF_TAIL;
	regions = tail[2];
	if (!regions || regions > 8 || size < WF_TAIL + regions * WF_REGION_REC) {
		bt_log("wifi fw tail bad: %u regions", regions);
		return -3;
	}
	{	/* its version (10 letters) and build time (14 digits), not ended in the file */
		char ver[11], built[15];
		memcpy(ver, tail + 7, 10); ver[10] = 0;
		memcpy(built, tail + 17, 14); built[14] = 0;
		bt_log("wifi fw %s %s, %u regions", ver, built, regions);
	}
	for (i = 0, off = 0; i < regions; i++) {
		const unsigned char *rec = tail - (regions - i) * WF_REGION_REC;
		unsigned int addr = wf_rd32le(rec + 16), len = wf_rd32le(rec + 20), feat = rec[24];
		if (off + len > size - WF_TAIL - regions * WF_REGION_REC) {
			bt_log("wifi fw region %u past the end", i);
			return -4;
		}
		if (feat & WF_FEAT_IN_WINDOW) {
			unsigned int at = addr - BT_EMI_FW_VIEW;
			if (addr < BT_EMI_FW_VIEW || at + len > BT_EMI_SIZE) {
				bt_log("wifi fw region %u outside the window: %08x", i, addr);
				return -5;
			}
			memcpy((void *)(addr_t)(BT_EMI_PA + at), img + off, len);
			bt_log("wifi region %u window +%06x %u", i, at, len);
		} else {
			if (wf_send_region(addr, img + off, len, feat) != 0) {
				bt_log("wifi region %u to %08x failed", i, addr);
				return -6;
			}
			bt_log("wifi region %u sent to %08x %u", i, addr, len);
		}
		off += len;
	}
	wf_barrier();
	wf_wr32le(start, 0);			/* no override: the firmware's own entry */
	wf_wr32le(start + 4, 0);
	if (wf_command(WF_CMD_START, start, sizeof start) != 0)
		return -7;
	return 0;
}

/* Wi-Fi from nothing to the RAM code running: CONNSYS up (Bluetooth's bring-up when it is not),
 * the bus ours, the rings, the RAM code, the ready bits. 0 when it runs. */
static void wf_roots_load(void);	/* the trusted roots, read at every start (below) */

static int pico_wifi_up(void)
{
	int rc;

	s_wf_up = 0;
	s_wf_station = 0;
	s_wf_linked = 0;
	if (!s_bt_up && (rc = pico_bt_up()) != 0) {
		bt_log("wifi: connsys not up (%d)", rc);
		return -1;
	}
	if (wf_dma_window() != 0)
		return -2;
	memset((void *)(addr_t)WIFI_DMA_PA, 0, WF_FW_STAGE);
	if (wf_take_bus() != 0)
		return -3;
	bt_log("wifi own %08x cfg %08x ready %08x", R32(WF_OWN), R32(WF_DMA_CFG), R32(WF_READY_REG));
	if (wf_dma_stop() != 0)
		return -4;
	wf_dma_start();
	bt_log("wifi dma on: cfg %08x", R32(WF_DMA_CFG));
	if ((rc = wf_load_ram_code()) != 0)
		return -10 + rc;
	wf_roots_load();
	if (bt_wait(WF_READY_REG, WF_READY_BITS, WF_READY_BITS, 2000000) != 0) {
		bt_log("wifi not ready: %08x", R32(WF_READY_REG));
		return -20;
	}
	R32(WF_DMA_CFG) &= ~DMA_RING3_DIRECT;	/* the download is over */
	bt_log("wifi ram code runs: ready %08x", R32(WF_READY_REG));
	s_wf_up = 1;
	return 0;
}

/* ---- talking to the running RAM code ----
 * A command: the same 32 byte header the DMA reads (kind 2), then a 32 byte command header (its
 * length past the first header, port 0x8000, command id, 0xA0, set 1 / query 0, sequence, spare,
 * extended id, the route 3 (host to firmware), option, version, spare), then its fields; on ring
 * 15. An answer: a receive header (16 or 20 bytes), then its length, 0xE000, event id, sequence,
 * version, spare, extended id, 2 spare, route, then its fields; on RX ring 1. */
#define WF_ROUTE_H2F		3
#define WF_EVT_TYPE		0xE000u
#define WF_CMD_CAPS		0x8A			/* what the firmware and the chip are (query) */
#define WF_EVT_CAPS		0xEC			/* its answer: tagged elements */

static int s_wf_evt_at;			/* where the answer header starts in a received buffer */
/* while set, the 802.11 frames the firmware passes up the answer ring (scan results) go here */
static void (*s_wf_frame_sink)(const unsigned char *b, int len);
#define WF_SW_KIND_MASK		0xE00Fu			/* a software packet's kind, upper half of word 0 */
#define WF_SW_EVENT		0xE000u
#define WF_SW_FRAME		0xE001u

/* a command for the running firmware: 0 once the DMA has taken it */
static int wf_fw_command(unsigned int cid, int set, const unsigned char *fields, unsigned int n)
{
	unsigned char *p = s_wf_pkt;
	unsigned int total = WF_HDR + n;

	if (n > WF_CHUNK)
		return -1;
	memset(p, 0, WF_HDR);
	p[0] = (unsigned char)total; p[1] = (unsigned char)(total >> 8);
	p[2] = (unsigned char)WF_PORT_CMD; p[3] = (unsigned char)(WF_PORT_CMD >> 8);
	p[5] = 0xA0;
	p[7] = WF_KIND_CMD;
	p[32] = (unsigned char)(total - 32); p[33] = (unsigned char)((total - 32) >> 8);
	p[34] = 0x00; p[35] = 0x80;
	p[36] = (unsigned char)cid;
	p[37] = 0xA0;
	p[38] = set ? 1 : 0;
	p[39] = s_wf_seq++;
	p[42] = WF_ROUTE_H2F;
	if (n)
		memcpy(p + WF_HDR, fields, n);
	return wf_send_on(15, WF_CMDQ_RING, WF_CMD_BUFS, &s_wf_cmdq, p, total);
}

/* ring 0's buffers the radio may still read: bit i for entry i (its frame's token is i) */
#define WF_RX_MSDU_REPORT	6			/* receive kind: sent frames done with */

/* an MSDU report: the tokens of frames whose buffers are free again. Byte 2 (low 7 bits) the
 * count, byte 6 (low 3 bits) the format: 0 two 16 bit ids per word, 1 one id per word, from
 * byte 8 on */
static void wf_tx_freed(const unsigned char *b, int len)
{
	unsigned int count = b[2] & 0x7Fu, ver = b[6] & 7u, k, id;
	for (k = 0; k < count; k++) {
		unsigned int at = ver == 0 ? 8 + 2 * k : 8 + 4 * k;
		if ((int)at + 2 > len)
			break;
		id = ((unsigned)b[at] | (unsigned)b[at + 1] << 8) & 0x7FFFu;
		if (id < WF_DATAQ_N)
			s_wf_tx_busy &= ~(1ull << id);
	}
}

/* the next answer with event id eid into buf (cap bytes): the offset of its fields in buf and their
 * length in *n, or -1 when none came within timeout_us (others met on the way are dropped) */
static int wf_fw_event(unsigned int eid, unsigned char *buf, unsigned int cap, unsigned int *n,
		       unsigned int timeout_us)
{
	unsigned int t0 = gpt4_get_current_tick();
	for (;;) {
		unsigned int left = timeout_us - GPT_US(gpt4_get_current_tick() - t0);
		int got, at;
		if (GPT_US(gpt4_get_current_tick() - t0) >= timeout_us)
			return -1;
		got = wf_answer(buf, cap, left);
		if (got < 0)
			return -1;
		if (got > (int)cap)
			got = (int)cap;
		if (((buf[3] >> 5) & 7u) == WF_RX_MSDU_REPORT) {	/* the radio is done with sent frames */
			wf_tx_freed(buf, got);
			continue;
		}
		if ((((unsigned)buf[2] | (unsigned)buf[3] << 8) & WF_SW_KIND_MASK) == WF_SW_FRAME) {
			if (s_wf_frame_sink)
				s_wf_frame_sink(buf, got);
			continue;
		}
		/* the receive header is 16 or 20 bytes: the answer header's type word says which */
		at = -1;
		if (got >= 28 && buf[18] == 0x00 && buf[19] == 0xE0)
			at = 16;
		else if (got >= 32 && buf[22] == 0x00 && buf[23] == 0xE0)
			at = 20;
		if (at < 0) {
			bt_log("wifi rx %d: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", got, buf[0], buf[1],
			       buf[2], buf[3], buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23]);
			continue;
		}
		s_wf_evt_at = at;
		if (buf[at + 4] != eid) {
			bt_log("wifi event %02x (ext %02x) while waiting for %02x", buf[at + 4], buf[at + 8], eid);
			continue;
		}
		*n = (unsigned)got - (unsigned)at - 12u;
		return at + 12;
	}
}

/* ---- the bootloader channel ---- */
/* oem wifiup - Wi-Fi up to the RAM code running (CONNSYS first when it is off), then oem btlog */
static void cmd_wifiup(const char *arg, void *data, unsigned sz)
{
	char l[64];
	int rc = pico_wifi_up();
	snprintf(l, sizeof l, "wifi up rc=%d (oem btlog for the steps)", rc);
	fastboot_info(l);
	fastboot_okay("");
}

static unsigned char s_wf_evt[WF_BUF];

/* ---- the console's network identity and a scan ---- */
#define WF_CMD_BSS_ACTIVATE	0x11			/* a network of ours on or off */
#define WF_CMD_SCAN		0x03			/* scan request (version 2) */
#define WF_EVT_SCAN_DONE	0x0D
#define WF_NET_STATION		0			/* network kinds: a station (1 is P2P / access point) */
#define WF_SCAN_LEN		1188			/* the scan request's fields, all of them */
#define WF_SCAN_ACTIVE		1
#define WF_SCAN_ANY_SSID	0x01
#define WF_SCAN_ALL_CHANNELS	0			/* the firmware's own list: 2.4 and 5 GHz */

static unsigned char s_wf_mac[6];

/* the console's Wi-Fi address: from its serial number (FNV-1a, like the Bluetooth identity, with a
 * different start), a locally administered unicast address */
static void wf_make_mac(void)
{
	unsigned long long h = 0xcbf29ce484222325ull ^ 0x5749464900000000ull;	/* "WIFI" */
	const char *p;
	int i;
	for (p = sn_buf; *p; p++) {
		h ^= (unsigned char)*p;
		h *= 0x100000001b3ull;
	}
	for (i = 0; i < 6; i++)
		s_wf_mac[i] = (unsigned char)(h >> (8 * i));
	s_wf_mac[0] = (unsigned char)((s_wf_mac[0] & 0xfc) | 0x02);
}

/* our station network (index 0) on, with our address */
/* what the radio passes up: frames to us, group frames, broadcast (the firmware's default
 * leaves frames to us out) */
#define WF_CMD_RX_FILTER	0x0A
#define WF_RX_TO_US		0x01u
#define WF_RX_GROUP		0x02u
#define WF_RX_BROADCAST		0x08u
static int wf_rx_filter(unsigned int what)
{
	unsigned char f[4 + 64];
	memset(f, 0, sizeof f);
	wf_wr32le(f, what);
	return wf_fw_command(WF_CMD_RX_FILTER, 1, f, sizeof f);
}

static int wf_station_on(void)
{
	unsigned char f[12];
	memset(f, 0, sizeof f);
	f[0] = 0;			/* network index */
	f[1] = 1;			/* on */
	f[2] = WF_NET_STATION;
	f[3] = 0;			/* own address slot */
	memcpy(f + 4, s_wf_mac, 6);
	f[10] = 1;			/* the broadcast entry of the station table: the first free one */
	return wf_fw_command(WF_CMD_BSS_ACTIVATE, 1, f, sizeof f);
}

#define WF_SCAN_MAX		40
#define WF_SEEN_RATES		16
#define WF_SEEN_SEC		64
static struct wf_seen {
	unsigned char bssid[6], ssid_len, channel, secured;
	signed char dbm;
	char ssid[33];
	unsigned short cap;				/* the AP's capability field */
	unsigned char nrates, rates[WF_SEEN_RATES];	/* its rates (500 kbit/s units, top bit: basic) */
	unsigned char rsn_len, rsn[WF_SEEN_SEC];	/* its RSN element's body (WPA2/WPA3), 0: none */
	unsigned char wpa_len;				/* the length of its older WPA element, 0: none */
	unsigned char ht;				/* it says HT: its HT capability element in ht_cap */
	unsigned char ht_cap[26];			/* capability 0, A-MPDU 2, MCS set 3..18, ... */
	unsigned char wmm;				/* it does WMM (QoS): HT rates need it */
	unsigned char dtim;				/* beacons per DTIM (its TIM element), 0 unknown */
	unsigned short beacon;				/* its beacon interval, in TU */
} s_wf_seen[WF_SCAN_MAX];
static unsigned int s_wf_nseen, s_wf_frames, s_wf_other;

/* The frame in a received buffer, from the hardware (kind 2) or passed up by the firmware
 * (software, frame): the 16 byte receive header, the groups it says are there (4, 1, 2, 3: 16,
 * 16, 8 and 24 bytes), maybe 2 bytes of padding, then the frame. The hardware turns data frames
 * from our AP into Ethernet form (to, from, type, then the payload): with eth, *eth says which
 * this is; without, such frames are passed over. Its start, or 0; its length in *n, the signal in
 * *dbm (from group 3's RCPI), the channel in *ch. */
/* the receive descriptor before the groups: 16 bytes on the Block's CONNSYS (IP set 1), 20 on the
 * KPA's (its console file sets it) */
#ifndef PICO_WIFI_RXD_LEN
#define PICO_WIFI_RXD_LEN	16
#endif

static const unsigned char *wf_frame_in(const unsigned char *b, int len, int *n, int *dbm, unsigned int *ch,
					int *eth)
{
	unsigned int w = (unsigned)b[2] | (unsigned)b[3] << 8;
	unsigned int kind = w >> 13 & 7u, groups = w >> 9 & 0xfu, at = PICO_WIFI_RXD_LEN, rcpi = 0;
	const unsigned char *g3 = 0;

	if (len < 40 || (kind != 2 && (w & WF_SW_KIND_MASK) != WF_SW_FRAME))
		return 0;
	if (groups & 8) at += 16;
	if (groups & 1) at += 16;
	if (groups & 2) at += 8;
	if (groups & 4) { g3 = b + at; at += 24; }
	if (b[6] & 0x40) at += 2;
	if (eth) *eth = (b[6] & 0x80) ? 1 : 0;
	else if (b[6] & 0x80) return 0;
	if ((int)at + ((b[6] & 0x80) ? 14 : 24) > len) return 0;
	if (g3) rcpi = wf_rd32le(g3 + 12) & 0xffu;
	*dbm = (int)((rcpi > 220 ? 220 : rcpi) >> 1) - 110;
	*ch = b[5];
	*n = len - (int)at;
	return b + at;
}

/* a received buffer during a scan: a beacon or probe answer noted (the strongest copy of each) */
static void wf_note_frame(const unsigned char *b, int len)
{
	unsigned int pos, ch, i;
	const unsigned char *f;
	struct wf_seen *e = 0;
	int dbm, flen;

	s_wf_frames++;
	if (!(f = wf_frame_in(b, len, &flen, &dbm, &ch, 0))) { s_wf_other++; return; }
	if (flen < 36 || (f[0] != 0x80 && f[0] != 0x50)) return;	/* beacons and probe answers only */
	len = flen;
	for (i = 0; i < s_wf_nseen; i++)
		if (!memcmp(s_wf_seen[i].bssid, f + 16, 6)) { e = &s_wf_seen[i]; break; }
	if (!e) {
		if (s_wf_nseen >= WF_SCAN_MAX) return;
		e = &s_wf_seen[s_wf_nseen++];
		memset(e, 0, sizeof *e);
		memcpy(e->bssid, f + 16, 6);
		e->dbm = -127;
	}
	if (dbm > e->dbm) e->dbm = (signed char)dbm;
	e->cap = (unsigned short)(f[24 + 10] | f[24 + 11] << 8);
	e->secured = (e->cap & 0x10) ? 1 : 0;		/* capability: privacy */
	e->channel = (unsigned char)ch;
	e->nrates = 0;
	e->rsn_len = e->wpa_len = e->ht = e->wmm = 0;
	e->beacon = (unsigned short)(f[24 + 8] | f[24 + 9] << 8);
	for (pos = 24 + 12; pos + 2 <= (unsigned)len; ) {	/* the elements */
		unsigned int id = f[pos], n = f[pos + 1], k;
		const unsigned char *v = f + pos + 2;
		if (pos + 2 + n > (unsigned)len) break;
		if (id == 0 && n <= 32) { memcpy(e->ssid, v, n); e->ssid[n] = 0; e->ssid_len = (unsigned char)n; }
		if (id == 3 && n == 1) e->channel = v[0];
		if (id == 1 || id == 50)			/* supported rates, extended rates */
			for (k = 0; k < n && e->nrates < WF_SEEN_RATES; k++)
				e->rates[e->nrates++] = v[k];
		if (id == 48 && n <= WF_SEEN_SEC) { memcpy(e->rsn, v, n); e->rsn_len = (unsigned char)n; }
		if (id == 221 && n >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1)
			e->wpa_len = (unsigned char)n;		/* Microsoft OUI, type 1: WPA */
		if (id == 45 && n >= 26) { e->ht = 1; memcpy(e->ht_cap, v, 26); }
		if (id == 221 && n >= 5 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 2 && v[4] <= 1)
			e->wmm = 1;				/* Microsoft OUI, type 2: WMM information/parameters */
		if (id == 5 && n >= 2) e->dtim = v[1];
		pos += 2 + n;
	}
}

/* Our station network on, a scan of every channel the firmware knows; the networks heard in
 * s_wf_seen. 0 when the scan finished, else what did not go (-1 network, -2 scan, -3 no end). */
static int wf_scan(unsigned int *ms)
{
	static unsigned char req[WF_SCAN_LEN], fr[WF_BUF];
	unsigned int t0, done = 0, n;
	int at;

	wf_make_mac();
	s_wf_nseen = s_wf_frames = s_wf_other = 0;
	if (!s_wf_station) {
		if (wf_station_on() != 0)
			return -1;
		s_wf_station = 1;
		t0 = gpt4_get_current_tick();
		while (GPT_US(gpt4_get_current_tick() - t0) < 200000u)	/* whatever the firmware says to that */
			if (wf_fw_event(0xFF, s_wf_evt, sizeof s_wf_evt, &n, 20000) >= 0)
				break;
	}
	s_wf_frame_sink = wf_note_frame;
	memset(req, 0, sizeof req);
	req[0] = s_wf_seq;		/* the scan's own number */
	req[1] = 0;			/* network index */
	req[2] = WF_SCAN_ACTIVE;
	req[3] = WF_SCAN_ANY_SSID;
	req[5] = 2;			/* probe requests per channel */
	req[158] = WF_SCAN_ALL_CHANNELS;
	if (wf_fw_command(WF_CMD_SCAN, 1, req, sizeof req) != 0) {
		s_wf_frame_sink = 0;
		return -2;
	}
	t0 = gpt4_get_current_tick();
	while (!done && GPT_US(gpt4_get_current_tick() - t0) < 12000000u) {
		int got;
		while ((got = wf_frame(fr, sizeof fr)) >= 0)
			wf_note_frame(fr, got < (int)sizeof fr ? got : (int)sizeof fr);
		if ((at = wf_fw_event(WF_EVT_SCAN_DONE, s_wf_evt, sizeof s_wf_evt, &n, 5000)) >= 0)
			done = 1;
	}
	while ((at = wf_frame(fr, sizeof fr)) >= 0)
		wf_note_frame(fr, at < (int)sizeof fr ? at : (int)sizeof fr);
	s_wf_frame_sink = 0;
	*ms = GPT_US(gpt4_get_current_tick() - t0) / 1000u;
	return done ? 0 : -3;
}

/* oem wifiscan - the networks heard on every channel the firmware knows */
static void cmd_wifiscan(const char *arg, void *data, unsigned sz)
{
	char l[96];
	unsigned int i, ms = 0;
	int rc;

	if (!s_wf_up || !s_bt_up) { fastboot_fail("wifi is off (oem wifiup)"); return; }
	rc = wf_scan(&ms);
	if (rc == -1 || rc == -2) { fastboot_fail(rc == -1 ? "wifiscan: network on did not go" : "wifiscan: scan did not go"); return; }
	snprintf(l, sizeof l, "scan %s in %u ms: %u networks, %u frames (%u not beacons)", rc ? "TIMED OUT" : "done",
		 ms, s_wf_nseen, s_wf_frames, s_wf_other);
	fastboot_info(l);
	snprintf(l, sizeof l, "our mac %02x:%02x:%02x:%02x:%02x:%02x", s_wf_mac[0], s_wf_mac[1], s_wf_mac[2], s_wf_mac[3],
		 s_wf_mac[4], s_wf_mac[5]);
	fastboot_info(l);
	for (i = 0; i < s_wf_nseen; i++) {
		struct wf_seen *e = &s_wf_seen[i];
		snprintf(l, sizeof l, "ch %3u %4d dBm %s %02x:%02x:%02x:%02x:%02x:%02x %s", e->channel, e->dbm,
			 e->secured ? "lock" : "open", e->bssid[0], e->bssid[1], e->bssid[2], e->bssid[3], e->bssid[4],
			 e->bssid[5], e->ssid_len ? e->ssid : "(hidden)");
		fastboot_info(l);
	}
	fastboot_okay("");
}

/* ---- joining a network (an access point) ----
 * A management frame goes down the firmware's command ring: the TX descriptor (32 bytes) and the
 * frame in one buffer, to the firmware's queue (port 1, queue 1), which sends it. A data frame
 * goes out on TX ring 0: the descriptor, its append (32 bytes: the frame's token and where the
 * frame lies) and the frame; the ring entry covers the first 64 bytes and the hardware fetches
 * the frame through the append. */
#define WF_CMD_CHANNEL		0x1C			/* channel privilege: the radio on a channel for us */
#define WF_EVT_STA_RECORD	0x0Cu		/* the station record is made */
#define WF_EVT_CHANNEL		0x10			/* its grant */
#define WF_CMD_NET_INFO		0x12			/* our network's settings */
#define WF_CMD_STA_RECORD	0x13			/* a peer's record */
#define WF_NET_INFO_LEN		116
#define WF_STA_RECORD_LEN	168
#define WF_STA_AP		0x41			/* record kind: legacy (bit 0) access point (bit 6) */
#define WF_AP_TABLE_INDEX	2			/* the station table entry of the AP (1 is broadcast) */
#define WF_RATES_BG		0x3FCFu			/* rate bits: 1, 2, 5.5, 11, then 6 to 54 Mbit/s */
#define WF_RATES_A		0x3FC0u
#define WF_BASIC_BG		0x000Fu
#define WF_BASIC_A		0x0540u			/* 6, 12, 24 */
#define WF_PHY_BGN		0x13u			/* HR-DSSS, ERP, HT */
#define WF_PHY_ANAC		0x38u			/* OFDM, HT, VHT */
#define WF_TXD			32u
#define WF_APPEND		32u


/* the descriptor in front of a frame: 32 bytes, the long form. len: the frame's; table: its station
 * table entry; to_firmware: through the firmware's queue (management), else the radio's */
static void wf_frame_desc(unsigned char *t, const unsigned char *frame, unsigned int len, unsigned int table,
			  int to_firmware, int band5)
{
	unsigned int hdr = 24;
	memset(t, 0, WF_TXD);
	t[0] = (unsigned char)(WF_TXD + len); t[1] = (unsigned char)((WF_TXD + len) >> 8);
	t[2] = (unsigned char)((4 + hdr) >> 1);			/* ether-type offset, in 16 bit words */
	t[3] = to_firmware ? (unsigned char)(0x80 | 1u << 2) : 0;	/* firmware queue 1, or the radio's queue 0 */
	t[4] = (unsigned char)table;
	t[5] = (unsigned char)((hdr >> 1) | 2u << 5 | 0x80u);	/* 802.11 header length, normal 802.11, long form */
	t[7] = to_firmware ? 2 : 0;				/* a frame the firmware sends; our address slot 0 */
	t[8] = (unsigned char)(((frame[0] >> 4) & 0xfu) | ((frame[0] >> 2) & 3u) << 4);	/* subtype, type */
	t[12] = 0x00; t[13] = 0x78;				/* up to 15 tries */
	if (to_firmware) {
		/* management goes at the lowest rate of the band: 1 Mbit/s CCK, 6 Mbit/s OFDM */
		t[11] |= 0x80;					/* rate given here */
		t[26] = band5 ? 0x4B : 0x00;
	}
}

/* one management frame out through the firmware's queue (the command ring, the descriptor and
 * the frame in one buffer) to the station table entry table; 0 once the DMA has taken it */
static int wf_send_mgmt(const unsigned char *frame, unsigned int len, unsigned int table, int band5)
{
	static unsigned char pkt[WF_BUF];
	if (len < 24 || len + WF_TXD > WF_BUF)
		return -1;
	wf_frame_desc(pkt, frame, len, table, 1, band5);
	memcpy(pkt + WF_TXD, frame, len);
	return wf_send_on(15, WF_CMDQ_RING, WF_CMD_BUFS, &s_wf_cmdq, pkt, WF_TXD + len);
}

/* One Ethernet frame (to, from, type, payload) out through the radio's queue (ring 0) to the AP:
 * the descriptor (Ethernet form: the radio makes the 802.11 header and the LLC/SNAP header,
 * protect: it encrypts with the keys in), then the append that points the DMA at the frame. The
 * buffers go round the ring, so a frame's buffer is reused WF_DATAQ_N frames later. 0 once the
 * DMA has taken its descriptor. */
static int wf_send_eth(const unsigned char *frame, unsigned int len, int protect)
{
	unsigned int i = s_wf_dataq, next = (i + 1) % WF_DATAQ_N, t0, pa;
	unsigned char *buf = (unsigned char *)(addr_t)(WIFI_DMA_PA + WF_DATA_BUFS + WF_DATA_STRIDE * i), *t, *a;
	volatile unsigned int *d = WF_AT(WF_DATAQ_RING + 16u * i);

	if (len < 14 || len > 1514 || len + WF_TXD + WF_APPEND > WF_DATA_STRIDE)
		return -1;
	t0 = gpt4_get_current_tick();
	while (next == R32(WF_TXQ(0) + 12)) {		/* the ring is full: the DMA has not taken the oldest */
		if (GPT_US(gpt4_get_current_tick() - t0) > 200000u) {
			bt_log("wifi tx0 stuck: ours %u dma %u", next, R32(WF_TXQ(0) + 12));
			return -2;
		}
	}
	if (s_wf_tx_busy & (1ull << i)) {	/* still being read: give the radio up to 20 ms */
		unsigned int n, t1 = gpt4_get_current_tick();
		while ((s_wf_tx_busy & (1ull << i)) && GPT_US(gpt4_get_current_tick() - t1) < 20000u)
			wf_fw_event(0xFF, s_wf_evt, sizeof s_wf_evt, &n, 50);
		if (s_wf_tx_busy & (1ull << i))
			s_wf_tx_reused++;		/* no report came: reused anyway (counted) */
	}
	pa = WIFI_DMA_PA + WF_DATA_BUFS + WF_DATA_STRIDE * i;
	t = buf;
	memset(t, 0, WF_TXD + WF_APPEND);
	t[0] = (unsigned char)(WF_TXD + len); t[1] = (unsigned char)((WF_TXD + len) >> 8);
	t[2] = (4 + 12) >> 1;				/* ether-type offset, in 16 bit words */
	t[3] = 0;					/* the radio's port, best effort queue */
	t[4] = WF_AP_TABLE_INDEX;
	t[5] = 0x10u | 0x80u;				/* not 802.11: Ethernet II; long form */
	t[7] = 0;					/* descriptor only, the frame through the append */
	t[12] = (unsigned char)(protect ? 0x02 : 0x00);	/* protected: encrypt */
	t[13] = 0x78;					/* up to 15 tries */
	a = buf + WF_TXD;				/* the append: the token (our entry) and the frame */
	a[0] = (unsigned char)i; a[1] = 0x80;
	wf_wr32le(a + 8, pa + WF_TXD + WF_APPEND);
	a[12] = (unsigned char)len; a[13] = (unsigned char)(0x80 | ((len >> 8) & 0x0f));
	s_wf_tx_busy |= 1ull << i;
	memcpy(buf + WF_TXD + WF_APPEND, frame, len);
	d[0] = pa;
	d[1] = (WF_TXD + WF_APPEND) << 16 | WF_DESC_LAST;
	d[2] = 0;
	d[3] = 0;
	wf_barrier();
	s_wf_dataq = next;
	R32(WF_TXQ(0) + 8) = next;			/* the DMA takes it in its own time */
	return 0;
}

#define WF_CMD_POWER_SAVE	0x05			/* power save profile */
#define WF_CMD_PM_CONNECTED	0x16			/* the power side: we are connected */
#define WF_PHY_LEGACY_BG	0x03u			/* HR-DSSS, ERP: until we offer HT ourselves */
#define WF_PHY_LEGACY_A		0x08u			/* OFDM */
#define WF_LISTEN		10			/* our listen interval, in beacons */
#define WF_PHY_HT		0x10u			/* PHY set bit: HT */
/* our HT capability: 20 MHz, short guard interval at 20 MHz, SM power save off (bits 2..3 = 3) */
#define WF_HT_CAP		0x002Cu
#define WF_HT_AMPDU		0x03u			/* A-MPDU: up to 64 KB, no spacing */
#define WF_CH_TOKEN		1			/* our channel request's token */
#define WF_STATE_NONE		0			/* station record states: nothing yet */
#define WF_STATE_AUTHED		1			/* authenticated */
#define WF_STATE_ASSOCIATED	2			/* associated */
#define WF_AUTH_OPEN		0			/* network info: authentication kinds */
#define WF_AUTH_WPA2_PSK	7
#define WF_ENC_NONE		1			/* network info: encryption */
#define WF_ENC_TKIP		4
#define WF_ENC_CCMP		6

static unsigned char s_wf_join_bssid[6];
static unsigned int s_wf_join_want;		/* the answer awaited: 0xB0 authentication, 0x10 association */
static int s_wf_join_status = -1;		/* its status, -1 none yet */
static unsigned int s_wf_join_aid;		/* the association number the AP gave us */
static int s_wf_join_reason;			/* the AP sent us away: its reason (or -1 for none given), else 0 */
static unsigned int s_wf_join_seen, s_wf_join_seen_m2;	/* every frame the join read; as of our second key message */
static unsigned int s_wf_join_data;		/* data frames from the AP to us since association */
static unsigned int s_wf_join_eapol;		/* key (EAPOL) frames from it */
static unsigned char s_wf_eapol[512];		/* the last one, from its 802.1X header on */
static unsigned int s_wf_eapol_len;
static unsigned int s_wf_key_pending;		/* key frames waiting in s_wf_eapol */
/* the security element we offered: the key handshake repeats it */
static unsigned char s_wf_join_ie[24];
static unsigned int s_wf_join_ie_len;

/* a received buffer while joining: the AP's answers to us, a send-off, its key messages */
static void wf_join_frame(const unsigned char *b, int len)
{
	const unsigned char *f;
	int flen, dbm, eth;
	unsigned int ch;
	s_wf_join_seen++;
	if (!(f = wf_frame_in(b, len, &flen, &dbm, &ch, &eth)))
		return;
	if (eth) {					/* data in Ethernet form: to 0, from 6, type 12 */
		if (memcmp(f, s_wf_mac, 6) || memcmp(f + 6, s_wf_join_bssid, 6))
			return;
		s_wf_join_data++;
		if (f[12] == 0x88 && f[13] == 0x8E && flen - 14 <= (int)sizeof s_wf_eapol) {
			memcpy(s_wf_eapol, f + 14, (unsigned)(flen - 14));
			s_wf_eapol_len = (unsigned)(flen - 14);
			s_wf_join_eapol++;
			s_wf_key_pending = 1;
		}
		return;
	}
	if (flen < 24 || memcmp(f + 4, s_wf_mac, 6) || memcmp(f + 10, s_wf_join_bssid, 6))
		return;
	if ((f[0] & 0x0C) == 0x08) {			/* data left in 802.11 form */
		/* The AP's first key message comes moments after the association's answer, and the
		 * radio passes what comes before it has taken in the association as it is, in 802.11
		 * form: the header (24, a QoS field 2 more, HT control 4 more with the order bit), then
		 * LLC/SNAP for type 0x888E and the EAPOL frame. Read there it starts the handshake;
		 * left unread, the AP gave up after its retries (reason 15, 11-18). */
		int hl = 24;
		s_wf_join_data++;
		if (f[1] & 0x40)			/* protected: not a handshake message */
			return;
		if (f[0] & 0x80) {
			hl += 2;
			if (f[1] & 0x80) hl += 4;
		}
		if (flen >= hl + 8 + 4 && f[hl] == 0xAA && f[hl + 1] == 0xAA && f[hl + 2] == 0x03 && !f[hl + 3] &&
		    !f[hl + 4] && !f[hl + 5] && f[hl + 6] == 0x88 && f[hl + 7] == 0x8E &&
		    flen - hl - 8 <= (int)sizeof s_wf_eapol) {
			memcpy(s_wf_eapol, f + hl + 8, (unsigned)(flen - hl - 8));
			s_wf_eapol_len = (unsigned)(flen - hl - 8);
			s_wf_join_eapol++;
			s_wf_key_pending = 1;
		}
		return;
	}
	if ((f[0] == 0xC0 || f[0] == 0xA0) && flen >= 26) {	/* deauthentication, disassociation */
		s_wf_join_reason = (int)((unsigned)f[24] | (unsigned)f[25] << 8);
		if (!s_wf_join_reason) s_wf_join_reason = -1;
		return;
	}
	if (f[0] != s_wf_join_want || flen < 30)
		return;
	if (s_wf_join_want == 0xB0) {			/* algorithm 24, transaction 26, status 28 */
		if (f[26] == 2)
			s_wf_join_status = (int)((unsigned)f[28] | (unsigned)f[29] << 8);
	} else {					/* capability 24, status 26, association number 28 */
		s_wf_join_status = (int)((unsigned)f[26] | (unsigned)f[27] << 8);
		s_wf_join_aid = ((unsigned)f[28] | (unsigned)f[29] << 8) & 0x3FFFu;
	}
}

/* ---- the WPA2 key handshake (we are the supplicant) ----
 * The AP sends EAPOL-Key frames to us in Ethernet form (wf_join_frame keeps the last one); we
 * answer as 802.11 data frames down the firmware's queue. Fields in a key frame, from its 802.1X
 * header: 0 version, 1 type (3 key), 2 length (BE), 4 descriptor (2 RSN), 5 key information
 * (BE), 7 key length, 9 replay counter (8), 17 nonce (32), 49 IV (16), 65 RSC (8), 73 spare (8),
 * 81 MIC (16), 97 key data length (BE), 99 key data. */
#include "pico_crypto.h"
#include "pico_random.h"
#include "pico_net.h"
#include "pico_tls.h"
#include "../../../emu/gba/pico_clock.h"
#include "../../../emu/gba/sd_fat.h"
#include "../../../emu/gba/gba_sd_save.h"
#include "../../../emu/gba/gba_boxart_sd.h"
#include "../../../emu/gba/pico_artimport.h"

#define WF_KEY_HDR		99
#define WF_KI_VERSION		0x0007u		/* key information: descriptor version (2: HMAC-SHA-1, AES) */
#define WF_KI_PAIRWISE		0x0008u
#define WF_KI_INSTALL		0x0040u
#define WF_KI_ACK		0x0080u
#define WF_KI_MIC		0x0100u
#define WF_KI_SECURE		0x0200u
#define WF_KI_ENCRYPTED		0x1000u
#define WF_CMD_ADD_KEY		0x07			/* a key into the radio */
#define WF_CIPHER_TKIP		2
#define WF_CIPHER_CCMP		4

static char s_wf_pass[64];			/* the passphrase (oem wifipass): RAM only, never on the card or in a log */
static unsigned int s_wf_pass_len;
static unsigned char s_wf_pmk[32];		/* its key for the network named s_wf_pmk_ssid */
static char s_wf_pmk_ssid[33];
static int s_wf_pmk_ok;

static const struct wf_seen *s_wf_key_ap;	/* the AP the handshake is with, 0 none */
static unsigned char s_wf_anonce[32], s_wf_snonce[32], s_wf_replay[8];
static unsigned char s_wf_ptk[48];		/* KCK 0..15, KEK 16..31, TK 32..47 */
static int s_wf_replay_seen, s_wf_ptk_ok;
static int s_wf_keys_in;			/* 1 pairwise and group keys are in: the link is up */
static const char *s_wf_key_why;		/* why the handshake stopped, 0 still going */
static unsigned int s_wf_nonce_count;

/* the PMK for ssid from the passphrase, worked out once per network */
static int wf_pmk_for(const struct wf_seen *ap)
{
	if (s_wf_pmk_ok && !strcmp(s_wf_pmk_ssid, ap->ssid))
		return 0;				/* worked out already, or a saved network's */
	if (!s_wf_pass_len)
		return -1;
	pico_pbkdf2_sha1(s_wf_pass, s_wf_pass_len, ap->ssid, ap->ssid_len, 4096, s_wf_pmk, sizeof s_wf_pmk);
	memcpy(s_wf_pmk_ssid, ap->ssid, sizeof s_wf_pmk_ssid);
	s_wf_pmk_ok = 1;
	return 0;
}

/* 32 bytes nobody can guess: the SoC's random generator (pico_random), the timer's low bits read
 * many times (their jitter), our address, the AP's nonce and a count, hashed */
static void wf_new_nonce(unsigned char out[32])
{
	struct pico_sha1 h;
	unsigned char d[PICO_SHA1_LEN], hw[20];
	unsigned int i, t, part;
	for (part = 0; part < 2; part++) {
		pico_sha1_start(&h);
		pico_random(hw, sizeof hw);
		pico_sha1_add(&h, hw, sizeof hw);
		for (i = 0; i < 64; i++) {
			t = gpt4_get_current_tick();
			pico_sha1_add(&h, &t, sizeof t);
		}
		pico_sha1_add(&h, s_wf_mac, 6);
		pico_sha1_add(&h, s_wf_anonce, sizeof s_wf_anonce);
		s_wf_nonce_count++;
		pico_sha1_add(&h, &s_wf_nonce_count, sizeof s_wf_nonce_count);
		pico_sha1_add(&h, &part, sizeof part);
		pico_sha1_end(&h, d);
		memcpy(out + 16 * part, d, 16);
	}
	memset(hw, 0, sizeof hw);
}

/* lower address/nonce first, as the key derivation wants them */
static void wf_put_ordered(unsigned char *out, const unsigned char *a, const unsigned char *b, unsigned int n)
{
	int first_a = memcmp(a, b, n) < 0;
	memcpy(out, first_a ? a : b, n);
	memcpy(out + n, first_a ? b : a, n);
}

static void wf_make_ptk(const struct wf_seen *ap)
{
	unsigned char d[12 + 64];
	wf_put_ordered(d, ap->bssid, s_wf_mac, 6);
	wf_put_ordered(d + 12, s_wf_anonce, s_wf_snonce, 32);
	pico_prf_sha1(s_wf_pmk, sizeof s_wf_pmk, "Pairwise key expansion", d, sizeof d, s_wf_ptk, sizeof s_wf_ptk);
	pico_wipe(d, sizeof d);
	s_wf_ptk_ok = 1;
}

/* the MIC of a key frame (its MIC field zero while it is worked out) with the KCK */
static void wf_key_mic(const unsigned char *k, unsigned int len, unsigned char mic[16])
{
	unsigned char code[PICO_SHA1_LEN];
	pico_hmac_sha1(s_wf_ptk, 16, k, len, code);
	memcpy(mic, code, 16);
}

/* a key frame of ours to the AP: key information ki, the replay counter as the AP's, nonce (or
 * zero), key data (or none); the MIC with the KCK. 0 sent. */
/* how the handshake's answers go before the keys are in (oem netsvc:m2:mgmt|eth|both, a test):
 * 0 an 802.11 data frame down the firmware's management queue, 1 Ethernet form on the data ring
 * (the radio sends it as any data to the AP, unencrypted), 2 both. The data ring: on the KPA an
 * answer down the management queue now and then never reached the AP (9 of 10 joins; the AP
 * repeated its first message, receiving went on), on the data ring 10 of 10. */
static int s_wf_key_path = 1;

static int wf_key_send(const struct wf_seen *ap, unsigned int ki, const unsigned char *nonce,
		       const unsigned char *data, unsigned int data_len)
{
	/* in Ethernet form on the data ring: the radio adds the 802.11 (QoS) header itself */
	static unsigned char f[14 + WF_KEY_HDR + 64];
	unsigned char *k = f + 14;
	unsigned int body = WF_KEY_HDR - 4 + data_len, len = WF_KEY_HDR + data_len;
	if (data_len > 64)
		return -1;
	memset(f, 0, sizeof f);
	memcpy(f, ap->bssid, 6);
	memcpy(f + 6, s_wf_mac, 6);
	f[12] = 0x88; f[13] = 0x8E;			/* EAPOL */
	k[0] = 1; k[1] = 3;				/* 802.1X version 1, a key */
	k[2] = (unsigned char)(body >> 8); k[3] = (unsigned char)body;
	k[4] = 2;					/* RSN */
	k[5] = (unsigned char)(ki >> 8); k[6] = (unsigned char)ki;
	memcpy(k + 9, s_wf_replay, 8);
	if (nonce) memcpy(k + 17, nonce, 32);
	k[97] = (unsigned char)(data_len >> 8); k[98] = (unsigned char)data_len;
	if (data_len) memcpy(k + WF_KEY_HDR, data, data_len);
	wf_key_mic(k, len, k + 81);
	if (s_wf_keys_in)
		return wf_send_eth(f, 14 + len, 1);	/* a new group key's answer goes encrypted */
	if (s_wf_key_path) {
		int rc = wf_send_eth(f, 14 + len, 0);
		if (s_wf_key_path == 1) return rc;
	}
	{	/* during the handshake: an 802.11 data frame down the firmware's queue */
		static unsigned char w[32 + WF_KEY_HDR + 64];
		memset(w, 0, 32);
		w[0] = 0x08; w[1] = 0x01;		/* data, to the distribution system */
		memcpy(w + 4, ap->bssid, 6);
		memcpy(w + 10, s_wf_mac, 6);
		memcpy(w + 16, ap->bssid, 6);
		w[24] = 0xAA; w[25] = 0xAA; w[26] = 0x03;	/* LLC/SNAP, type 0x888E */
		w[30] = 0x88; w[31] = 0x8E;
		memcpy(w + 32, k, len);
		return wf_send_mgmt(w, 32 + len, WF_AP_TABLE_INDEX, ap->channel > 14);
	}
}

/* An EAPOL-Start to the AP (802.1X version 1, type Start, no body), as an 802.11 data frame down
 * the firmware's queue like the handshake's own answers: an AP that sent its first key message
 * before the radio was told of the association (that message lost) starts the handshake again
 * on it, where otherwise it may wait long or give up without a word. */
static void wf_eapol_start(const struct wf_seen *ap)
{
	unsigned char w[36];
	memset(w, 0, sizeof w);
	w[0] = 0x08; w[1] = 0x01;
	memcpy(w + 4, ap->bssid, 6);
	memcpy(w + 10, s_wf_mac, 6);
	memcpy(w + 16, ap->bssid, 6);
	w[24] = 0xAA; w[25] = 0xAA; w[26] = 0x03;
	w[30] = 0x88; w[31] = 0x8E;
	w[32] = 1; w[33] = 1;				/* version 1, Start; length 0 */
	wf_send_mgmt(w, sizeof w, WF_AP_TABLE_INDEX, ap->channel > 14);
}

/* a key into the radio: pairwise (the AP's entry) or group (the broadcast entry, key id) */
static int wf_key_install(const struct wf_seen *ap, int pairwise, unsigned int cipher, unsigned int id,
			  const unsigned char *key, unsigned int key_len, const unsigned char *rsc)
{
	unsigned char c[64];
	int rc;
	memset(c, 0, sizeof c);
	c[0] = 1;					/* add */
	c[1] = pairwise ? 1 : 0;			/* our sending key */
	c[2] = pairwise ? 1 : 0;			/* pairwise or group */
	c[3] = 0;					/* we are not the authenticator */
	memcpy(c + 4, ap->bssid, 6);
	c[10] = 0;					/* network 0 */
	c[11] = (unsigned char)cipher;
	c[12] = (unsigned char)id;
	c[13] = (unsigned char)key_len;
	c[14] = pairwise ? WF_AP_TABLE_INDEX : 1;
	memcpy(c + 16, key, key_len);
	if (cipher == WF_CIPHER_TKIP && key_len == 32) {	/* our receive MIC key first, then sending */
		memcpy(c + 16 + 16, key + 24, 8);
		memcpy(c + 16 + 24, key + 16, 8);
	}
	if (rsc) memcpy(c + 48, rsc, 8);
	rc = wf_fw_command(WF_CMD_ADD_KEY, 1, c, sizeof c);
	pico_wipe(c, sizeof c);
	return rc;
}

/* the group key from the key data's elements (already decrypted): 0 found */
static int wf_find_gtk(const unsigned char *d, unsigned int n, unsigned int *id, const unsigned char **gtk,
		       unsigned int *gtk_len)
{
	unsigned int at = 0;
	while (at + 2 <= n) {
		unsigned int t = d[at], l = d[at + 1];
		if (t == 0xDD && l == 0 && at + 2 == n) break;	/* padding */
		if (at + 2 + l > n) break;
		if (t == 0xDD && l >= 6 + 16 && d[at + 2] == 0x00 && d[at + 3] == 0x0F && d[at + 4] == 0xAC &&
		    d[at + 5] == 1) {				/* GTK element: key id, spare, the key */
			*id = d[at + 6] & 3u;
			*gtk = d + at + 8;
			*gtk_len = l - 6;
			return 0;
		}
		at += 2 + l;
	}
	return -1;
}

/* the key data of a key frame, decrypted with the KEK into out (len in *n): 0 */
static int wf_key_data(const unsigned char *k, unsigned int len, unsigned int ki, unsigned char *out, unsigned int *n)
{
	unsigned int dl = (unsigned)k[97] << 8 | k[98];
	if (WF_KEY_HDR + dl > len || dl > 256)
		return -1;
	if (!(ki & WF_KI_ENCRYPTED)) {
		memcpy(out, k + WF_KEY_HDR, dl);
		*n = dl;
		return 0;
	}
	if (pico_aes128_unwrap(s_wf_ptk + 16, k + WF_KEY_HDR, dl, out) != 0)
		return -1;
	*n = dl - 8;
	return 0;
}

/* one key frame from the AP (s_wf_eapol): the next step of the handshake */
static void wf_key_step(void)
{
	static unsigned char k[512], plain[256];
	const struct wf_seen *ap = s_wf_key_ap;
	unsigned int len = s_wf_eapol_len, ki, n, id, gtk_len, cipher;
	const unsigned char *gtk;
	unsigned char mic[16], got[16];
	int newer;

	s_wf_key_pending = 0;
	if (!ap || s_wf_key_why || len < WF_KEY_HDR || len > sizeof k)
		return;
	memcpy(k, s_wf_eapol, len);
	if (k[1] != 3 || k[4] != 2)
		return;					/* not an RSN key frame */
	n = 4u + ((unsigned)k[2] << 8 | k[3]);		/* the frame's own length (no trailing padding) */
	if (n < WF_KEY_HDR || n > len)
		return;
	len = n;
	ki = (unsigned)k[5] << 8 | k[6];
	if ((ki & WF_KI_VERSION) != 2) { s_wf_key_why = "the AP uses the older key format (TKIP-only): not yet"; return; }
	newer = !s_wf_replay_seen || memcmp(k + 9, s_wf_replay, 8) > 0;
	if (!newer && (ki & WF_KI_MIC)) return;		/* an old or repeated frame */
	cipher = ap->rsn_len && s_wf_join_ie[13] == 2 ? WF_CIPHER_TKIP : WF_CIPHER_CCMP;

	if ((ki & WF_KI_PAIRWISE) && (ki & WF_KI_ACK) && !(ki & WF_KI_MIC)) {	/* message 1 */
		memcpy(s_wf_replay, k + 9, 8);
		s_wf_replay_seen = 1;
		if (!s_wf_ptk_ok || memcmp(s_wf_anonce, k + 17, 32)) {
			memcpy(s_wf_anonce, k + 17, 32);
			wf_new_nonce(s_wf_snonce);
			wf_make_ptk(ap);
		}
		if (wf_key_send(ap, 2u | WF_KI_PAIRWISE | WF_KI_MIC, s_wf_snonce, s_wf_join_ie, s_wf_join_ie_len) != 0)
			s_wf_key_why = "our second message did not go";
		s_wf_join_seen_m2 = s_wf_join_seen;
		bt_log("wifi keys: 1 in, 2 out");
		return;
	}
	if (!s_wf_ptk_ok || !(ki & WF_KI_MIC))
		return;
	memcpy(got, k + 81, 16);				/* every later frame carries a MIC: check it */
	memset(k + 81, 0, 16);
	wf_key_mic(k, len, mic);
	if (memcmp(mic, got, 16)) {
		s_wf_key_why = "the AP's key frame failed its check (wrong passphrase?)";
		return;
	}
	if ((ki & WF_KI_PAIRWISE) && (ki & WF_KI_ACK) && (ki & WF_KI_INSTALL)) {	/* message 3 */
		if (memcmp(s_wf_anonce, k + 17, 32)) { s_wf_key_why = "the AP changed its nonce"; return; }
		if (wf_key_data(k, len, ki, plain, &n) != 0 || wf_find_gtk(plain, n, &id, &gtk, &gtk_len) != 0) {
			s_wf_key_why = "no group key in the third message";
			return;
		}
		memcpy(s_wf_replay, k + 9, 8);
		if (wf_key_send(ap, 2u | WF_KI_PAIRWISE | WF_KI_MIC | WF_KI_SECURE, 0, 0, 0) != 0) {
			s_wf_key_why = "our fourth message did not go";
			return;
		}
		bt_log("wifi keys: 3 in, 4 out, group key %u (%u bytes)", id, gtk_len);
		/* the fourth message must leave unencrypted: let it go before the keys are in */
		{
			unsigned int t0 = gpt4_get_current_tick();
			while (GPT_US(gpt4_get_current_tick() - t0) < 30000u) ;
		}
		if (wf_key_install(ap, 1, cipher, 0, s_wf_ptk + 32, cipher == WF_CIPHER_TKIP ? 32 : 16, 0) != 0 ||
		    wf_key_install(ap, 0, ap->rsn[5] == 2 ? WF_CIPHER_TKIP : WF_CIPHER_CCMP, id, gtk, gtk_len, k + 65) != 0) {
			s_wf_key_why = "the keys did not go into the radio";
			pico_wipe(plain, sizeof plain);
			return;
		}
		pico_wipe(plain, sizeof plain);
		s_wf_keys_in = 1;
		return;
	}
	if (!(ki & WF_KI_PAIRWISE) && (ki & WF_KI_ACK) && (ki & WF_KI_SECURE)) {	/* a new group key */
		if (wf_key_data(k, len, ki, plain, &n) != 0 || wf_find_gtk(plain, n, &id, &gtk, &gtk_len) != 0)
			return;
		memcpy(s_wf_replay, k + 9, 8);
		wf_key_send(ap, 2u | WF_KI_MIC | WF_KI_SECURE, 0, 0, 0);
		wf_key_install(ap, 0, ap->rsn[5] == 2 ? WF_CIPHER_TKIP : WF_CIPHER_CCMP, id, gtk, gtk_len, k + 65);
		pico_wipe(plain, sizeof plain);
		bt_log("wifi keys: new group key %u", id);
	}
}

/* oem wifipass:PASSPHRASE - the passphrase for the next wifijoin (8 to 63 characters), kept in
 * RAM only */
static void cmd_wifipass(const char *arg, void *data, unsigned sz)
{
	size_t n;
	while (*arg == ':' || *arg == ' ') arg++;
	n = strlen(arg);
	if (n && (n < 8 || n > 63)) { fastboot_fail("a WPA passphrase is 8 to 63 characters"); return; }
	pico_wipe(s_wf_pass, sizeof s_wf_pass);
	pico_wipe(s_wf_pmk, sizeof s_wf_pmk);
	s_wf_pmk_ok = 0;
	memcpy(s_wf_pass, arg, n);
	s_wf_pass_len = (unsigned int)n;
	fastboot_okay(n ? "passphrase kept for this session" : "passphrase cleared");
}

/* reads what comes in for up to us microseconds, running the key handshake as its frames come:
 * until the awaited answer or a send-off (stop_on_answer), else until the keys are in or the
 * handshake stopped; the answer's status, -1 none */
static int wf_join_wait(unsigned int us, int stop_on_answer)
{
	static unsigned char fr[WF_BUF];
	unsigned int t0 = gpt4_get_current_tick(), n;
	while (GPT_US(gpt4_get_current_tick() - t0) < us) {
		int got;
		if (stop_on_answer && (s_wf_join_status >= 0 || s_wf_join_reason))
			break;
		if (!stop_on_answer && (s_wf_keys_in || s_wf_key_why))
			break;
		while ((got = wf_frame(fr, sizeof fr)) >= 0) {
			if (got > (int)sizeof fr) got = (int)sizeof fr;
			wf_join_frame(fr, got);
		}
		wf_fw_event(0xFF, s_wf_evt, sizeof s_wf_evt, &n, 5000);
		/* a key message that came during the authentication or the association waits until the
		 * radio has been told of the association: an answer sent before then did not reach the
		 * AP (the handshake stalled after its first message, 11-18) */
		if (s_wf_key_pending && !stop_on_answer)
			wf_key_step();
	}
	return s_wf_join_status;
}

/* a rate in 500 kbit/s units to its bit in our rate sets (1, 2, 5.5, 11, then 6 to 54 Mbit/s
 * from bit 6), or 16 when it is none of them */
static unsigned int wf_rate_bit(unsigned int r)
{
	static const unsigned char units[14] = { 2, 4, 11, 22, 0, 0, 12, 18, 24, 36, 48, 72, 96, 108 };
	unsigned int i;
	for (i = 0; i < 14; i++)
		if (units[i] && units[i] == r)
			return i;
	return 16;
}

/* the rates both of us have (oper) and the AP's must-have ones among them (basic) */
static void wf_ap_rates(const struct wf_seen *ap, int band5, unsigned int *oper, unsigned int *basic)
{
	unsigned int ours = band5 ? WF_RATES_A : WF_RATES_BG, o = 0, b = 0, i, bit;
	for (i = 0; i < ap->nrates; i++) {
		bit = wf_rate_bit(ap->rates[i] & 0x7Fu);
		if (bit >= 16 || !(ours & 1u << bit))
			continue;
		o |= 1u << bit;
		if (ap->rates[i] & 0x80u)
			b |= 1u << bit;
	}
	if (!o) o = ours;
	if (!b) b = (band5 ? WF_BASIC_A : WF_BASIC_BG) & o;
	*oper = o;
	*basic = b;
}

/* the security to use with ap: the network info's kinds (*auth, *enc) and our RSN element in
 * s_wf_join_ie. 0, or -1 with *why when the AP wants something we do not do yet */
static int wf_pick_security(const struct wf_seen *ap, unsigned char *auth, unsigned char *enc, const char **why)
{
	static const unsigned char oui[3] = { 0x00, 0x0F, 0xAC };
	const unsigned char *p = ap->rsn, *pair = 0, *akm = 0;
	unsigned int n = ap->rsn_len, at, cnt, i, caps = 0, sae = 0;
	unsigned char *ie = s_wf_join_ie;

	s_wf_join_ie_len = 0;
	if (!ap->secured) { *auth = WF_AUTH_OPEN; *enc = WF_ENC_NONE; return 0; }
	if (!n) { *why = ap->wpa_len ? "WPA (TKIP) only: not yet" : "WEP: not supported"; return -1; }
	if (n < 6 || p[0] != 1 || p[1] != 0 || memcmp(p + 2, oui, 3) || (p[5] != 2 && p[5] != 4)) {
		*why = "its group cipher is not one we know";
		return -1;
	}
	at = 6;
	if (at + 2 <= n) {				/* pairwise ciphers: CCMP first, else TKIP */
		cnt = p[at] | p[at + 1] << 8;
		at += 2;
		for (i = 0; i < cnt && at + 4 <= n; i++, at += 4)
			if (!memcmp(p + at, oui, 3) && (p[at + 3] == 4 || (p[at + 3] == 2 && !pair)))
				pair = p + at;
	} else {
		pair = p + 2;				/* none listed: CCMP is meant (the group's) */
	}
	if (!pair) { *why = "no pairwise cipher we know"; return -1; }
	if (at + 2 <= n) {				/* key management: PSK */
		cnt = p[at] | p[at + 1] << 8;
		at += 2;
		for (i = 0; i < cnt && at + 4 <= n; i++, at += 4) {
			if (memcmp(p + at, oui, 3)) continue;
			if (p[at + 3] == 2) akm = p + at;
			if (p[at + 3] == 8) sae = 1;
		}
	}
	if (!akm) { *why = sae ? "WPA3 (SAE) only: not yet" : "enterprise (802.1X): not supported"; return -1; }
	if (at + 2 <= n) caps = p[at] | p[at + 1] << 8;
	if (caps & 0x40u) { *why = "it requires protected management frames: not yet"; return -1; }

	ie[0] = 48; ie[1] = 20;
	ie[2] = 1; ie[3] = 0;				/* version 1 */
	memcpy(ie + 4, p + 2, 4);			/* its group cipher */
	ie[8] = 1; ie[9] = 0; memcpy(ie + 10, pair, 4);
	ie[14] = 1; ie[15] = 0; memcpy(ie + 16, akm, 4);
	ie[20] = 0; ie[21] = 0;				/* capabilities */
	s_wf_join_ie_len = 22;
	*auth = WF_AUTH_WPA2_PSK;
	*enc = pair[3] == 4 ? WF_ENC_CCMP : WF_ENC_TKIP;
	return 0;
}

/* the radio on ap's channel for us (release 0), or given back (release 1); 0 granted / sent */
static int wf_channel(const struct wf_seen *ap, int release)
{
	unsigned char ch[24];
	unsigned int n;
	int at;
	memset(ch, 0, sizeof ch);
	ch[0] = 0; ch[1] = WF_CH_TOKEN; ch[2] = (unsigned char)(release ? 1 : 0);	/* network 0, request or give back */
	ch[3] = ap->channel;
	ch[5] = ap->channel > 14 ? 2 : 1;
	ch[9] = 0;					/* to join */
	wf_wr32le(ch + 12, 4000);
	if (wf_fw_command(WF_CMD_CHANNEL, 1, ch, sizeof ch) != 0)
		return -1;
	if (release)
		return 0;
	at = wf_fw_event(WF_EVT_CHANNEL, s_wf_evt, sizeof s_wf_evt, &n, 1000000);
	if (at >= 0 && s_wf_evt[at + 2] == 0)
		return 0;
	bt_log("wifi channel %u: %s %d", ap->channel, at >= 0 ? "refused, status" : "no answer", at >= 0 ? s_wf_evt[at + 2] : -1);
	return -1;
}
/* the channel for a join: a refusal right after the last try gave it back (the firmware may still
 * be letting it go) is asked for once more, the channel given back first and a moment later */
static int wf_channel_take(const struct wf_seen *ap)
{
	if (wf_channel(ap, 0) == 0)
		return 0;
	wf_channel(ap, 1);
	thread_sleep(150);
	return wf_channel(ap, 0);
}

/* HT with this AP: it has HT and WMM (20 MHz). On 5 GHz only where the console's file allows it:
 * the KPA's CONNSYS joins at HT on 5 GHz but no data gets through (legacy rates work), the
 * Block's is fine */
#ifndef PICO_WIFI_HT_5GHZ
#define PICO_WIFI_HT_5GHZ	1
#endif
static int wf_use_ht(const struct wf_seen *ap)
{
	return ap->ht && ap->wmm && (ap->channel <= 14 || PICO_WIFI_HT_5GHZ);
}
static int wf_use_qos(const struct wf_seen *ap) { return ap->wmm; }

/* the AP's station record at state (WF_STATE_*), with its association number; answer: wait for
 * the firmware to say it is made. 0 sent (and answered) */
static int wf_ap_record(const struct wf_seen *ap, unsigned int state, unsigned int aid, int answer)
{
	static unsigned char rec[WF_STA_RECORD_LEN];
	int band5 = ap->channel > 14;
	unsigned int oper, basic, n;
	wf_ap_rates(ap, band5, &oper, &basic);
	memset(rec, 0, sizeof rec);
	rec[0] = 0; rec[1] = WF_STA_AP;
	memcpy(rec + 2, ap->bssid, 6);
	rec[8] = (unsigned char)aid; rec[9] = (unsigned char)(aid >> 8);
	rec[10] = WF_LISTEN;
	rec[12] = 0;					/* our network 0 */
	rec[13] = band5 ? WF_PHY_LEGACY_A : WF_PHY_LEGACY_BG;
	rec[14] = (unsigned char)oper; rec[15] = (unsigned char)(oper >> 8);
	rec[16] = (unsigned char)basic; rec[17] = (unsigned char)(basic >> 8);
	rec[20] = (unsigned char)state;
	rec[23] = 1;					/* version */
	if (wf_use_qos(ap)) rec[18] = 1;		/* QoS */
	if (wf_use_ht(ap)) {				/* the AP's HT side as we will use it: 20 MHz, one stream */
		/* only what a one-antenna radio can use toward it: short guard interval at 20 MHz and its
		 * SM power save mode (no LDPC, 40 MHz, greenfield or STBC) */
		unsigned int cap = ((unsigned)ap->ht_cap[0] | (unsigned)ap->ht_cap[1] << 8) & 0x002Cu;
		rec[13] |= WF_PHY_HT;
		rec[21] = ap->ht_cap[3];		/* MCS 0..7 it takes */
		rec[24] = ap->ht_cap[3];
		rec[40] = (unsigned char)cap; rec[41] = (unsigned char)(cap >> 8);
		rec[48] = ap->ht_cap[2];
		rec[72] = 1;				/* short guard interval when both have it */
		rec[76] = 1; rec[77] = 1;		/* A-MPDU out and in (TCP holds data that arrives early) */
	}
	rec[51] = answer ? 1 : 0;
	rec[54] = WF_AP_TABLE_INDEX;
	rec[55] = 1;					/* the broadcast entry */
	if (wf_fw_command(WF_CMD_STA_RECORD, 1, rec, sizeof rec) != 0)
		return -1;
	if (answer && wf_fw_event(WF_EVT_STA_RECORD, s_wf_evt, sizeof s_wf_evt, &n, 500000) < 0)
		return -1;
	return 0;
}

/* our network's settings: infrastructure, that AP, its channel, the security; connected or not */
static int wf_net_info(const struct wf_seen *ap, int connected, unsigned char auth, unsigned char enc)
{
	static unsigned char net[WF_NET_INFO_LEN];
	int band5 = ap->channel > 14;
	unsigned int oper, basic;
	wf_ap_rates(ap, band5, &oper, &basic);
	memset(net, 0, sizeof net);
	net[0] = 0;
	net[1] = connected ? 0 : 1;
	net[2] = 0;					/* infrastructure */
	net[3] = ap->ssid_len;
	memcpy(net + 4, ap->ssid, ap->ssid_len);
	memcpy(net + 36, ap->bssid, 6);
	net[44] = (unsigned char)oper; net[45] = (unsigned char)(oper >> 8);
	net[46] = (unsigned char)basic; net[47] = (unsigned char)(basic >> 8);
	net[48] = 0;					/* the AP's record */
	net[52] = band5 ? 3 : 1;			/* non-HT PHY: OFDM or ERP */
	net[53] = auth;
	net[54] = enc;
	net[55] = band5 ? WF_PHY_LEGACY_A : WF_PHY_LEGACY_BG;
	net[58] = 1;					/* broadcast entry */
	net[68] = 0; net[69] = band5 ? 2 : 1; net[70] = ap->channel;	/* RLM: network, band, primary channel */
	net[71] = 0;					/* no second channel: 20 MHz */
	net[88] = 1; net[89] = 1;			/* one stream each way */
	net[93] = 1;
	if (wf_use_qos(ap)) net[42] = 1;		/* a QoS network */
	if (wf_use_ht(ap)) net[55] |= WF_PHY_HT;
	return wf_fw_command(WF_CMD_NET_INFO, 1, net, sizeof net);
}

/* the association request to ap into f: its length */
static unsigned int wf_assoc_request(unsigned char *f, const struct wf_seen *ap)
{
	int band5 = ap->channel > 14;
	unsigned int ours = band5 ? WF_RATES_A : WF_RATES_BG, cap, at, i, nr = 0, bit;
	unsigned char r[WF_SEEN_RATES];

	memset(f, 0, 24);
	f[0] = 0x00;					/* management, association request */
	memcpy(f + 4, ap->bssid, 6);
	memcpy(f + 10, s_wf_mac, 6);
	memcpy(f + 16, ap->bssid, 6);
	cap = 0x0001u | (ap->cap & 0x0010u);		/* ESS, privacy as the AP has it */
	if (!band5) cap |= ap->cap & (0x0020u | 0x0400u);	/* short preamble, short slot */
	f[24] = (unsigned char)cap; f[25] = (unsigned char)(cap >> 8);
	f[26] = WF_LISTEN; f[27] = 0;
	at = 28;
	f[at++] = 0; f[at++] = ap->ssid_len;
	memcpy(f + at, ap->ssid, ap->ssid_len);
	at += ap->ssid_len;
	for (i = 0; i < ap->nrates; i++) {		/* its rates that we have, basic marks kept */
		bit = wf_rate_bit(ap->rates[i] & 0x7Fu);
		if (bit < 16 && (ours & 1u << bit))
			r[nr++] = ap->rates[i];
	}
	if (!nr) {
		static const unsigned char bg[8] = { 0x82, 0x84, 0x8B, 0x96, 12, 18, 24, 36 };
		static const unsigned char a[8] = { 0x8C, 18, 0x98, 36, 0xB0, 72, 96, 108 };
		memcpy(r, band5 ? a : bg, 8);
		nr = 8;
	}
	f[at++] = 1; f[at++] = (unsigned char)(nr > 8 ? 8 : nr);
	memcpy(f + at, r, nr > 8 ? 8 : nr);
	at += nr > 8 ? 8 : nr;
	if (nr > 8) {
		f[at++] = 50; f[at++] = (unsigned char)(nr - 8);
		memcpy(f + at, r + 8, nr - 8);
		at += nr - 8;
	}
	memcpy(f + at, s_wf_join_ie, s_wf_join_ie_len);
	at += s_wf_join_ie_len;
	if (wf_use_ht(ap)) {
		f[at++] = 45; f[at++] = 26;		/* HT capabilities */
		memset(f + at, 0, 26);
		f[at] = (unsigned char)WF_HT_CAP; f[at + 1] = (unsigned char)(WF_HT_CAP >> 8);
		f[at + 2] = WF_HT_AMPDU;
		f[at + 3] = 0xFF;			/* we take MCS 0..7 */
		f[at + 15] = 0x01;			/* our sending MCS set is the same */
		at += 26;
	}
	if (wf_use_qos(ap)) {
		f[at++] = 221; f[at++] = 7;		/* WMM information: version 1, no power save queues */
		f[at++] = 0x00; f[at++] = 0x50; f[at++] = 0xF2; f[at++] = 2; f[at++] = 0; f[at++] = 1; f[at++] = 0;
	}
	return at;
}

/* the firmware keeps the radio awake (no power save: the AP never holds our frames) */
static int wf_stay_awake(void)
{
	unsigned char f[4] = { 0, 0, 0, 0 };		/* network 0, profile 0: always awake */
	return wf_fw_command(WF_CMD_POWER_SAVE, 1, f, sizeof f);
}

/* the firmware's power side told we are connected to ap with association number aid */
static int wf_tell_connected(const struct wf_seen *ap, unsigned int aid)
{
	unsigned char f[12];
	unsigned int bi = ap->beacon ? ap->beacon : 100;
	memset(f, 0, sizeof f);
	f[0] = 0;
	f[1] = ap->dtim ? ap->dtim : 1;
	f[2] = (unsigned char)aid; f[3] = (unsigned char)(aid >> 8);
	f[4] = (unsigned char)bi; f[5] = (unsigned char)(bi >> 8);
	return wf_fw_command(WF_CMD_PM_CONNECTED, 1, f, sizeof f);
}

/* Joins ap: the channel, the AP's record, our network, open system authentication, then the
 * association. say (may be 0) hears each step. 0 associated (the AP's key handshake comes
 * next for a secured network); -1 with *why at the step that failed. */
/* A deauthentication from us to the AP (reason 3: we are leaving). Before each join: an AP that
 * still holds us from a session that ended without a word (a restart, a flash, the power) answers
 * the new authentication and association but never starts the key handshake for a station it
 * believes is already connected; told we left, it starts afresh. And when we leave or turn Wi-Fi
 * off, so the next join finds it clean. */
static int s_wf_join_band5;
static void wf_deauth(const unsigned char bssid[6], int band5)
{
	unsigned char d[26];
	memset(d, 0, sizeof d);
	d[0] = 0xC0;
	memcpy(d + 4, bssid, 6);
	memcpy(d + 10, s_wf_mac, 6);
	memcpy(d + 16, bssid, 6);
	d[24] = 3;
	wf_send_mgmt(d, sizeof d, WF_AP_TABLE_INDEX, band5);
}

/* we are leaving the AP joined (Wi-Fi off, Leave): it is told */
static void wf_leave_ap(void)
{
	if (s_wf_linked)
		wf_deauth(s_wf_join_bssid, s_wf_join_band5);
}

static int wf_join_ap(const struct wf_seen *ap, void (*say)(const char *), const char **why)
{
	static unsigned char f[256];
	unsigned char auth[30], kind, enc;
	char l[96];
	unsigned int len;
	int band5 = ap->channel > 14;

	if (wf_pick_security(ap, &kind, &enc, why) != 0)
		return -1;
	if (s_wf_join_ie_len && wf_pmk_for(ap) != 0) { *why = "it needs a passphrase (oem wifipass)"; return -1; }
	s_wf_key_ap = ap;
	s_wf_key_pending = 0;
	s_wf_replay_seen = s_wf_ptk_ok = s_wf_keys_in = 0;
	s_wf_key_why = 0;
	memcpy(s_wf_join_bssid, ap->bssid, 6);
	s_wf_join_reason = 0;
	s_wf_join_data = 0;
	s_wf_join_seen = s_wf_join_seen_m2 = 0;
	s_wf_join_eapol = 0;
	s_wf_eapol_len = 0;
	if (wf_stay_awake() != 0) { *why = "the power profile did not go"; return -1; }
	if (wf_rx_filter(WF_RX_TO_US | WF_RX_GROUP | WF_RX_BROADCAST) != 0) { *why = "the receive filter did not go"; return -1; }
	if (wf_channel_take(ap) != 0) { *why = "the radio did not get its channel"; return -1; }
	if (wf_ap_record(ap, WF_STATE_NONE, 0, 1) != 0) { *why = "the AP's record was not made"; goto out; }
	if (wf_net_info(ap, 0, kind, enc) != 0) { *why = "our network's settings did not go"; goto out; }
	s_wf_join_band5 = band5;

	/* first a goodbye for whatever the AP may still hold of us (wf_deauth), a moment for it */
	s_wf_join_want = 0;
	s_wf_join_status = -1;
	wf_deauth(ap->bssid, band5);
	wf_join_wait(40000, 0);
	s_wf_join_reason = 0;

	/* open system authentication, transaction 1 */
	memset(auth, 0, sizeof auth);
	auth[0] = 0xB0;
	memcpy(auth + 4, ap->bssid, 6);
	memcpy(auth + 10, s_wf_mac, 6);
	memcpy(auth + 16, ap->bssid, 6);
	auth[26] = 1;
	s_wf_join_want = 0xB0;
	s_wf_join_status = -1;
	if (wf_send_mgmt(auth, sizeof auth, WF_AP_TABLE_INDEX, band5) != 0) { *why = "authentication did not go"; goto out; }
	if (wf_join_wait(1000000, 1) != 0) {
		*why = s_wf_join_status > 0 ? "the AP refused the authentication" : "no answer to the authentication";
		bt_log("wifi auth status %d reason %d", s_wf_join_status, s_wf_join_reason);
		goto out;
	}
	if (say) say("authenticated");
	if (wf_ap_record(ap, WF_STATE_AUTHED, 0, 0) != 0) { *why = "the AP's record did not go"; goto out; }

	/* the association */
	len = wf_assoc_request(f, ap);
	s_wf_join_want = 0x10;
	s_wf_join_status = -1;
	if (wf_send_mgmt(f, len, WF_AP_TABLE_INDEX, band5) != 0) { *why = "the association request did not go"; goto out; }
	if (wf_join_wait(1000000, 1) != 0) {
		snprintf(l, sizeof l, "association: status %d, sent away %d", s_wf_join_status, s_wf_join_reason);
		bt_log("wifi %s", l);
		*why = s_wf_join_status > 0 ? "the AP refused the association" : "no answer to the association";
		if (say && s_wf_join_status > 0) say(l);
		goto out;
	}
	if (wf_ap_record(ap, WF_STATE_ASSOCIATED, s_wf_join_aid, 0) != 0 || wf_net_info(ap, 1, kind, enc) != 0 ||
	    wf_tell_connected(ap, s_wf_join_aid) != 0) {
		*why = "the firmware was not told of the association";
		goto out;
	}
	snprintf(l, sizeof l, "associated: number %u, %s, %s", s_wf_join_aid,
		 s_wf_join_ie_len ? (enc == WF_ENC_CCMP ? "WPA2 (CCMP)" : "WPA2 (TKIP)") : "open",
		 wf_use_ht(ap) ? "HT" : "legacy rates");
	if (say) say(l);
	return 0;					/* the caller gives the channel back (wf_channel) */
out:
	wf_channel(ap, 1);
	return -1;
}

static void wf_say(const char *l) { fastboot_info(l); }

/* oem wifijoin:SSID - scan, then join the strongest network of that name as far as we can:
 * authentication and association; for a secured one, then watches for the AP's first key
 * message */
static int wf_connect_to(struct wf_seen *best, void (*say)(const char *), const char **why);

/* Joins the strongest network named ssid (the passphrase or PMK already set for it): scan (twice
 * when the first misses it), authentication, association, the key handshake. A name several APs
 * or both bands carry is tried at each, strongest first (up to three), until one links: one band
 * may not start the key handshake with us while the other does. say (may be 0) hears each step.
 * 0 linked; else -1 with *why (the last try's). */
static int wf_connect(const char *ssid, void (*say)(const char *), const char **why)
{
	struct wf_seen *cand[3];
	unsigned int i, ms, tries, k, n = 0;

	*why = "";
	if (!s_wf_up || !s_bt_up) { *why = "Wi-Fi is off"; return -1; }
	if (!*ssid || strlen(ssid) > 32) { *why = "no network name"; return -1; }
	for (tries = 0; tries < 2 && !n; tries++) {	/* one scan can miss a quiet AP: a second look */
		if (wf_scan(&ms) < 0) { *why = "the scan failed"; return -1; }
		for (i = 0; i < s_wf_nseen; i++) {
			struct wf_seen *c = &s_wf_seen[i];
			if (strcmp(c->ssid, ssid)) continue;
			/* the three best, strongest first: a weaker one than all three is left out */
			if (n == 3 && c->dbm <= cand[2]->dbm) continue;
			k = n < 3 ? n++ : 2;
			for (; k > 0 && cand[k - 1]->dbm < c->dbm; k--) cand[k] = cand[k - 1];
			cand[k] = c;
		}
	}
	if (!n) { *why = "the network is not in range"; return -1; }
	for (k = 0; k < n; k++) {
		int rc = wf_connect_to(cand[k], say, why);
		if (rc == -2) {			/* the handshake alone failed: that AP once more, afresh */
			if (say) say("keys: trying that access point again");
			rc = wf_connect_to(cand[k], say, why);
		}
		if (rc == 0) return 0;
		if (say && rc == -1 && *why && **why) {	/* (a handshake failure has said its own) */
			char l[96];
			snprintf(l, sizeof l, "failed: %s", *why);
			say(l);
		}
		if (say && k + 1 < n) say("trying the next access point of that name");
	}
	return -1;
}

static void wf_net_stop(void);	/* (below: the link stopped, kept connections forgotten) */
static int wf_connect_to(struct wf_seen *best, void (*say)(const char *), const char **why)
{
	char l[96];
	if (say) {
		snprintf(l, sizeof l, "joining %s: %02x:%02x:%02x:%02x:%02x:%02x ch %u %d dBm", best->ssid, best->bssid[0],
			 best->bssid[1], best->bssid[2], best->bssid[3], best->bssid[4], best->bssid[5], best->channel, best->dbm);
		say(l);
	}
	s_wf_linked = 0;
	wf_net_stop();
	s_wf_frame_sink = wf_join_frame;
	if (wf_join_ap(best, say, why) != 0) {
		s_wf_frame_sink = 0;
		return -1;
	}
	if (s_wf_join_ie_len) {
		wf_join_wait(1200000, 0);		/* the AP's key handshake */
		if (!s_wf_keys_in && !s_wf_join_eapol && !s_wf_key_why && !s_wf_join_reason) {
			if (say) say("keys: nothing yet, asking the AP to start (EAPOL-Start)");
			wf_eapol_start(best);
			wf_join_wait(2500000, 0);
		}
		if (!s_wf_keys_in) {
			*why = s_wf_key_why ? s_wf_key_why : s_wf_join_eapol ? "the key handshake did not finish" :
			       "the network never started the key handshake";
			if (say) {
				snprintf(l, sizeof l, "keys: %s", *why);
				say(l);
				snprintf(l, sizeof l, "keys: eapol %u data %u away %d heard %u/%u", s_wf_join_eapol, s_wf_join_data,
					 s_wf_join_reason, s_wf_join_seen, s_wf_join_seen - s_wf_join_seen_m2);
				say(l);
			}
			wf_channel(best, 1);
			s_wf_frame_sink = 0;
			return -2;			/* (joined, but no keys: the same AP may do it the next time) */
		}
		if (say) {
			snprintf(l, sizeof l, "connected: keys in (%s)", s_wf_join_ie[13] == 2 ? "TKIP" : "CCMP");
			say(l);
		}
	}
	s_wf_linked = 1;
	s_wf_link_protect = s_wf_join_ie_len != 0;
	s_wf_join_dbm = best->dbm;
	wf_channel(best, 1);
	s_wf_frame_sink = 0;
	return 0;
}

static void cmd_wifijoin(const char *arg, void *data, unsigned sz)
{
	char l[96];
	const char *why;
	while (*arg == ':' || *arg == ' ') arg++;
	if (wf_connect(arg, wf_say, &why) != 0) {
		if (!s_wf_up) { fastboot_fail("wifi is off (oem wifiup)"); return; }
		snprintf(l, sizeof l, "not joined: %s", why);
		fastboot_info(l);
	}
	fastboot_okay("");
}

/* oem wificap - the running firmware's capability answer, element by element (wifiup first) */
static void cmd_wificap(const char *arg, void *data, unsigned sz)
{
	char l[96];
	unsigned int n, count, pos, k;
	int at;
	const unsigned char *e;

	if (!s_wf_up || !s_bt_up) { fastboot_fail("wifi is off (oem wifiup)"); return; }
	if (wf_fw_command(WF_CMD_CAPS, 0, 0, 0) != 0) { fastboot_fail("wificap: the command did not go"); return; }
	at = wf_fw_event(WF_EVT_CAPS, s_wf_evt, sizeof s_wf_evt, &n, 500000);
	if (at < 0) { fastboot_fail("wificap: no answer (oem btlog)"); return; }
	e = s_wf_evt + at;
	count = (unsigned)e[0] | (unsigned)e[1] << 8;
	snprintf(l, sizeof l, "caps: %u elements, %u bytes (rx header %d)", count, n, s_wf_evt_at);
	fastboot_info(l);
	for (k = 0, pos = 4; k < count && pos + 8 <= n; k++) {
		unsigned int tag = wf_rd32le(e + pos), len = wf_rd32le(e + pos + 4);
		const unsigned char *b = e + pos + 8;
		if (pos + 8 + len > n)
			break;
		if (tag == 7 && len >= 6)
			snprintf(l, sizeof l, "mac %02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
		else if (tag == 6 && len >= 24) {
			char date[17];
			memcpy(date, b + 8, 16); date[16] = 0;
			snprintf(l, sizeof l, "fw %u.%u build %u branch %c%c%c%c date %s", b[1], b[0],
				 (unsigned)b[2] | (unsigned)b[3] << 8, b[4], b[5], b[6], b[7], date);
		} else if (tag == 5 && len >= 8)
			snprintf(l, sizeof l, "hw product %04x eco %u mac ip %08x", (unsigned)b[0] | (unsigned)b[1] << 8, b[2],
				 wf_rd32le(b + 4));
		else
			snprintf(l, sizeof l, "tag %02x len %u: %02x %02x %02x %02x %02x %02x %02x %02x", tag, len,
				 len > 0 ? b[0] : 0, len > 1 ? b[1] : 0, len > 2 ? b[2] : 0, len > 3 ? b[3] : 0,
				 len > 4 ? b[4] : 0, len > 5 ? b[5] : 0, len > 6 ? b[6] : 0, len > 7 ? b[7] : 0);
		fastboot_info(l);
		pos += 8 + len;
	}
	fastboot_okay("");
}


/* ---- the link: IP over the joined network (pico_net) ---- */

static unsigned long long s_wf_ms_us;		/* the millisecond clock: microseconds so far */
static unsigned int s_wf_ms_tick, s_wf_ms_started;

static unsigned int wf_now_ms(void)
{
	unsigned int t = gpt4_get_current_tick();
	if (!s_wf_ms_started) { s_wf_ms_tick = t; s_wf_ms_started = 1; }
	s_wf_ms_us += GPT_US(t - s_wf_ms_tick);
	s_wf_ms_tick = t;
	return (unsigned int)(s_wf_ms_us / 1000u);
}

static unsigned int s_wf_rx_peak;		/* most frames waiting on ring 0 at one look */

static int wf_link_send(const unsigned char *frame, unsigned int len)
{
	return wf_send_eth(frame, len, s_wf_link_protect);
}

/* a received buffer while linked: Ethernet frames to the stack, key frames to the handshake (a
 * new group key), 802.11 data turned into Ethernet form first */
static void wf_link_frame(const unsigned char *b, int len)
{
	static unsigned char e[1514];
	const unsigned char *f;
	int flen, dbm, eth;
	unsigned int ch, hl;
	if (!(f = wf_frame_in(b, len, &flen, &dbm, &ch, &eth)))
		return;
	if (!eth) {
		if (flen < 24 + 8 || (f[0] & 0x0C) != 0x08 || (f[1] & 0x03) != 0x02 || (f[1] & 0x40))
			return;				/* only plain data from the AP */
		hl = 24 + ((f[0] & 0x80) ? 2 : 0);
		if (flen < (int)hl + 8 || f[hl] != 0xAA || f[hl + 1] != 0xAA || f[hl + 2] != 0x03 ||
		    flen - (int)hl - 8 + 14 > (int)sizeof e)
			return;
		memcpy(e, f + 4, 6);			/* to: address 1 */
		memcpy(e + 6, f + 16, 6);		/* from: address 3 */
		memcpy(e + 12, f + hl + 6, (unsigned)flen - hl - 6);
		f = e;
		flen = flen - (int)hl - 8 + 14;
	}
	if (flen >= 14 && f[12] == 0x88 && f[13] == 0x8E) {
		if (flen - 14 <= (int)sizeof s_wf_eapol && !memcmp(f, s_wf_mac, 6)) {
			memcpy(s_wf_eapol, f + 14, (unsigned)(flen - 14));
			s_wf_eapol_len = (unsigned)(flen - 14);
			s_wf_key_pending = 1;
		}
		return;
	}
	pico_net_input(f, (unsigned)flen);
}

/* runs the link for up to us microseconds, or until done() says so: frames in, the stack's timers */
/* one turn of the link: frames in, acknowledgements out, events, the stack's timers */
/* where the radio's work stands, read without any lock by oem netsvc:crumb: [0] the step
 * (NSC_*), [1] link pumps run, [2] bytes of the transfer received, [3] the millisecond of the last
 * pump. A console that stops answering leaves these as its last word. */
enum { NSC_IDLE, NSC_WAIT, NSC_SCAN, NSC_JOIN, NSC_PUMP, NSC_DNS = 10, NSC_CONNECT, NSC_TLS, NSC_SEND, NSC_RECV,
       NSC_PARSE, NSC_DONE };
volatile unsigned int g_ns_crumb[4];

/* A transfer step that outlives its own time limit (they are all bounded) means something is
 * stuck, and USB may be stuck with it: the menu's thread, which keeps drawing, notes every thread's
 * state then (thread_snapshot), and again every 20 s while it lasts, for oem netsvc:stall once the
 * console answers again. Lock free: the service may be the stuck one. */
static char s_ns_stall[1536];
static unsigned int s_ns_stall_len, s_ns_stall_n;

void netsvc_stall_watch(void)
{
	static unsigned int step, since, pumps, noted, last_pumps, pump_seen, boxed;
	unsigned int s = g_ns_crumb[0], now = gpt4_get_current_tick() / 13000u, lim, at;
	if (g_ns_crumb[1] != last_pumps) { last_pumps = g_ns_crumb[1]; pump_seen = now; boxed = 0; }
	if (s != step) { step = s; since = now; pumps = g_ns_crumb[1]; noted = 0; pump_seen = now; return; }
	if (s < NSC_DNS || s > NSC_PARSE) return;
	/* in a transfer the link is pumped all the time: 8 s without a pump means the service is
	 * stuck inside something. Its thread's code addresses and the fastboot thread's state go in
	 * the black box on the card (oem blackbox, after any restart). Once per stuck spell. */
	if (!boxed && now - pump_seen > 8000u) {
		extern void ayaneo_bb_note(unsigned type, const char *msg, const unsigned *a, unsigned n);
		unsigned int a[8], w[6], wq, fwq;
		char msg[48];
		int st = thread_code_words("pico_net", w, 6, &wq), fst;
		unsigned int f[1];
		fst = thread_code_words("fastboot", f, 1, &fwq);
		memcpy(a, w, sizeof w);
		a[6] = (unsigned)fst;
		a[7] = fwq;
		snprintf(msg, sizeof msg, "net stuck s%u %us st%x wq%x b%u", s, (now - pump_seen) / 1000u,
			 (unsigned)st, wq, g_ns_crumb[2]);
		ayaneo_bb_note(0x4E45u, msg, a, 8);
		boxed = 1;
	}
	lim = s == NSC_RECV ? 65000u : s == NSC_TLS ? 18000u : 10000u;
	if ((now - since < lim && now - pump_seen < 8000u) || (noted && now - noted < 20000u)) return;
	noted = now;
	s_ns_stall_n++;
	at = (unsigned)snprintf(s_ns_stall, sizeof s_ns_stall, "#%u step %u for %u ms, pumps +%u, last pump %u ms ago\n",
				s_ns_stall_n, s, now - since, g_ns_crumb[1] - pumps, now - g_ns_crumb[3]);
	if (at < sizeof s_ns_stall)
		at += thread_snapshot(s_ns_stall + at, sizeof s_ns_stall - at);
	s_ns_stall_len = at < sizeof s_ns_stall ? at : sizeof s_ns_stall - 1;
}

static void wf_link_pump(void)
{
	static unsigned char fr[WF_BUF];
	unsigned int n;
	int got, k = 0;
	static unsigned int given;
	unsigned int now = gpt4_get_current_tick();
	g_ns_crumb[1]++;
	g_ns_crumb[3] = now / 13000u;
	/* Running flat out (a transfer), the link gives the other threads of its priority a turn
	 * every 2 ms. LK puts a thread taken off the CPU with time left in its turn back at the
	 * front of the queue, and so does every wake from a sleep: the menu and this service kept
	 * cutting in front of the fastboot thread, which sat ready for minutes (USB gone quiet,
	 * a black box record of 11-18 showed it), and of each other (a handshake crawling for
	 * minutes). A yield goes to the back and costs nothing when nobody else is ready. */
	if (GPT_US(now - given) > (s_wf_coop ? 4000u : 2000u)) {
		given = now;
		/* the service is above the menu's priority: a yield would come straight back, so while
		 * data flows it sleeps a moment every 4 ms (the menu and fastboot run meanwhile); the
		 * fastboot thread's own commands keep the yield */
		if (s_wf_coop) thread_sleep(1);
		else thread_yield();
	}
	/* at most a ringful at a time: a busy network still lets the events and timers have their turn */
	while (k < WF_RX0N && (got = wf_frame(fr, sizeof fr)) >= 0) {
		wf_link_frame(fr, got < (int)sizeof fr ? got : (int)sizeof fr);
		k++;
	}
	if (k > (int)s_wf_rx_peak) s_wf_rx_peak = (unsigned)k;	/* frames found at once (ring: WF_RX0N) */
	if (k) pico_net_flush();
	/* short while data flows; idle, the service thread sleeps a millisecond (a fastboot command spins) */
	wf_fw_event(0xFF, s_wf_evt, sizeof s_wf_evt, &n, k ? 20 : s_wf_coop ? 1000 : 500);
	if (s_wf_key_pending)
		wf_key_step();
	pico_net_poll();
}

static void wf_link_run(unsigned int us, int (*done)(void))
{
	unsigned int t0 = gpt4_get_current_tick();
	while (GPT_US(gpt4_get_current_tick() - t0) < us && !s_wf_abort) {
		if (done && done())
			break;
		wf_link_pump();
	}
}

static int wf_have_address(void) { return pico_net_up(0); }

/* the stack on the joined network */
static void wf_link_start(void)
{
	struct pico_net_link l;
	memcpy(l.mac, s_wf_mac, 6);
	l.send = wf_link_send;
	l.now_ms = wf_now_ms;
	l.work = (unsigned char *)(addr_t)(WIFI_DMA_PA + WF_NET_WORK);
	{	/* the TCP buffers must fit their stretch of the Wi-Fi memory (a build error if not) */
		typedef char wf_net_work_fits[PICO_TCP_SLOTS * PICO_TCP_MEM <= WF_NET_WORK_LEN ? 1 : -1];
		(void)sizeof(wf_net_work_fits);
	}
	l.work_len = PICO_TCP_SLOTS * PICO_TCP_MEM;
	pico_net_start(&l);
	s_wf_frame_sink = wf_link_frame;
}

static unsigned int s_wf_ping_seq;
static int s_wf_ping_wait;
static int wf_ping_back(void) { return pico_net_ping_answer((unsigned short)s_wf_ping_wait) >= 0; }

/* pings ip three times into the fastboot answer */
static void wf_ping_say(unsigned int ip)
{
	char l[64], t[16];
	int k, rtt;
	pico_net_ip_text(ip, t);
	for (k = 0; k < 3; k++) {
		s_wf_ping_wait = (int)(unsigned short)++s_wf_ping_seq;
		pico_net_ping(ip, (unsigned short)s_wf_ping_wait);
		wf_link_run(1500000, wf_ping_back);
		rtt = pico_net_ping_answer((unsigned short)s_wf_ping_wait);
		if (rtt >= 0) snprintf(l, sizeof l, "ping %s: %d ms", t, rtt);
		else snprintf(l, sizeof l, "ping %s: no answer", t);
		fastboot_info(l);
	}
}

static int wf_set_clock(char *l, unsigned int cap);

/* oem wifidhcp - an address for the joined network (DHCP), then pings the gateway and sets the
 * clock from the network */
static void cmd_wifidhcp(const char *arg, void *data, unsigned sz)
{
	struct pico_net_conf c;
	struct pico_net_stats st;
	char l[96], a[16], m[16], g[16], d[16];
	if (!s_wf_up || !s_wf_linked) { fastboot_fail("not joined (oem wifijoin)"); return; }
	wf_link_start();
	pico_net_dhcp("GammaOS-Pico");
	wf_link_run(15000000, wf_have_address);
	pico_net_get_stats(&st);
	if (!pico_net_up(&c)) {
		snprintf(l, sizeof l, "no address (frames in %u, out %u, udp in %u, offers %u, acks %u)", st.in, st.out,
			 st.udp_in, st.dhcp_offers, st.dhcp_acks);
		fastboot_info(l);
		fastboot_okay("");
		return;
	}
	pico_net_ip_text(c.ip, a); pico_net_ip_text(c.mask, m); pico_net_ip_text(c.gateway, g); pico_net_ip_text(c.dns[0], d);
	snprintf(l, sizeof l, "address %s mask %s gateway %s dns %s, %u s", a, m, g, d, c.lease_s);
	fastboot_info(l);
	wf_ping_say(c.gateway);
	snprintf(l, sizeof l, "frames in %u, out %u; buffers reused unreported %u", st.in, st.out, s_wf_tx_reused);
	fastboot_info(l);
	l[0] = 0;
	wf_set_clock(l, sizeof l);
	if (l[0]) fastboot_info(l);
	fastboot_okay("");
}

/* oem wifiping:A.B.C.D - three pings over the link */
static void cmd_wifiping(const char *arg, void *data, unsigned sz)
{
	unsigned int ip = 0, part = 0, dots = 0, digits = 0;
	while (*arg == ':' || *arg == ' ') arg++;
	for (; *arg; arg++) {
		if (*arg >= '0' && *arg <= '9') { part = part * 10 + (unsigned)(*arg - '0'); digits++; if (part > 255) break; }
		else if (*arg == '.' && digits) { ip = ip << 8 | part; part = 0; digits = 0; dots++; }
		else break;
	}
	if (*arg || dots != 3 || !digits) { fastboot_fail("wifiping:A.B.C.D"); return; }
	ip = ip << 8 | part;
	if (!pico_net_up(0)) { fastboot_fail("no address (oem wifidhcp)"); return; }
	wf_ping_say(ip);
	fastboot_okay("");
}

static int wf_dns_done(void) { return pico_net_dns_result(0) != 0; }
static int wf_time_done(void) { return pico_net_time_result(0) != 0; }

#define WF_TIME_SERVER		"pool.ntp.org"

/* the time from the network only (the RTC untouched): 0 and *t, or -1 */
static int wf_net_time(long long *t)
{
	unsigned int ip;
	if (pico_net_dns(WF_TIME_SERVER) != 0) return -1;
	wf_link_run(6000000, wf_dns_done);
	if (pico_net_dns_result(&ip) != 1 || pico_net_time(ip) != 0) return -1;
	wf_link_run(7000000, wf_time_done);
	return pico_net_time_result(t) == 1 ? 0 : -1;
}

/* the time from the network: the RTC takes it when it was unset or is more than 2 s off. A line
 * for the fastboot answer into l (cap bytes). 1 set, 0 already right, -1 no answer. */
static int wf_set_clock(char *l, unsigned int cap)
{
	long long t;
	pico_tm tm;
	if (wf_net_time(&t) != 0) { snprintf(l, cap, "time: no answer from %s", WF_TIME_SERVER); return -1; }
	pico_clock_split(t, &tm);
	/* the menu thread sets the RTC (netsvc_clock_apply): it reads it through the same PMIC
	 * wrapper, and two threads at once could wedge it */
	mutex_acquire(&s_ns_info);
	s_ns_time = t;
	s_ns_time_tick = gpt4_get_current_tick();
	mutex_release(&s_ns_info);
	snprintf(l, cap, "time: the network says %04d-%02d-%02d %02d:%02d:%02d UTC (the menu sets the clock)", tm.year,
		 tm.mon, tm.mday, tm.hour, tm.min, tm.sec);
	return 1;
}

/* oem wifintp - the time from the network into the RTC */
static void cmd_wifintp(const char *arg, void *data, unsigned sz)
{
	char l[96];
	if (!pico_net_up(0)) { fastboot_fail("no address (oem wifidhcp)"); return; }
	if (wf_set_clock(l, sizeof l) < 0 && !l[0]) snprintf(l, sizeof l, "time: failed");
	fastboot_info(l);
	fastboot_okay("");
}

/* oem wifidns:NAME - looks a name up */
static void cmd_wifidns(const char *arg, void *data, unsigned sz)
{
	unsigned int ip;
	char l[96], t[16];
	while (*arg == ':' || *arg == ' ') arg++;
	if (!pico_net_up(0)) { fastboot_fail("no address (oem wifidhcp)"); return; }
	if (pico_net_dns(arg) != 0) { fastboot_fail("wifidns:NAME"); return; }
	wf_link_run(8000000, wf_dns_done);
	if (pico_net_dns_result(&ip) == 1) {
		pico_net_ip_text(ip, t);
		snprintf(l, sizeof l, "%s is %s", arg, t);
	} else {
		snprintf(l, sizeof l, "%s: not found", arg);
	}
	fastboot_info(l);
	fastboot_okay("");
}

/* Downloads land where the RAM code file was staged (free once Wi-Fi runs): 768 KB. The game
 * list for scraping goes in the free stretch after the data ring's buffers. */
#define WF_GET_BUF		(WIFI_DMA_PA + WF_FW_STAGE)
#define WF_GET_CAP		0xC0000u
/* the answer's memory for this request: the 768 KB above (s_wf_rx_cap 0), or a caller's own for
 * answers that may be bigger (ScreenScraper's game information runs past a megabyte for a famous
 * game: 1.26 MB for Super Mario Bros.) */
static unsigned char *s_wf_rx_buf;
static unsigned int s_wf_rx_cap;
static unsigned char *wf_rx_buf(void) { return s_wf_rx_cap ? s_wf_rx_buf : (unsigned char *)(addr_t)WF_GET_BUF; }
static unsigned int wf_rx_cap(void) { return s_wf_rx_cap ? s_wf_rx_cap : WF_GET_CAP; }
#define WF_LIST_BUF		(WIFI_DMA_PA + WF_LIST_AREA)
#define WF_LIST_CAP		WF_LIST_AREA_LEN

static int s_wf_get_h = -1;
static unsigned int s_wf_get_len, s_wf_get_total;	/* bytes kept, bytes received */
static int s_wf_get_secure;				/* the download runs over TLS */
static char s_wf_user_agent[96];			/* the HTTP user agent ("" GammaOS-Pico) */
static struct pico_tls s_wf_tls;
/* HTTP keep-alive: the last two connections stay open for the next request to the same host (box
 * art asks the information host, then the picture host, game after game: a TCP and TLS handshake
 * apiece was half a second or more). The first keeps its TLS state above, the second takes one from
 * the heap when first needed. A connection is used again only on the same link, within 20 s. */
#define WF_CONNS	2
#define WF_CONN_IDLE_MS	20000u
static struct wf_conn {
	char host[64];
	int secure, h, alive;
	unsigned int gen, last;
	struct pico_tls *tls;
} s_wf_conn[WF_CONNS] = { { .h = -1 }, { .h = -1 } };	/* (no socket yet: 0 is a socket's number) */
static unsigned int s_wf_net_gen;		/* the link's generation: a stop makes every socket old */
static struct pico_tls *s_wf_tlsp = &s_wf_tls;	/* the TLS state of the connection in use */
static int s_wf_get_closed;			/* the server closed the connection in use */
static void wf_net_stop(void) { s_wf_net_gen++; pico_net_stop(); }

/* TLS over our TCP connection s_wf_get_h: a send waits (the link running) until TCP took it all */
static int wf_tls_send(void *ctx, const uint8_t *d, unsigned int n)
{
	unsigned int o = 0, t0 = gpt4_get_current_tick();
	(void)ctx;
	while (o < n) {
		int r = pico_tcp_send(s_wf_get_h, d + o, n - o);
		if (r < 0) return -1;
		o += (unsigned)r;
		if (o < n) {
			if (GPT_US(gpt4_get_current_tick() - t0) > 10000000u) return -1;
			wf_link_pump();
		}
	}
	return (int)n;
}
/* plain TCP: len bytes out, the link running until TCP took them all */
static int wf_tcp_send_all(const unsigned char *d, unsigned int n)
{
	return wf_tls_send(0, d, n);
}

static int wf_tls_recv(void *ctx, uint8_t *b, unsigned int cap)
{
	(void)ctx;
	return pico_tcp_recv(s_wf_get_h, b, cap);
}
static int wf_tls_ready(void) { return pico_tls_step(s_wf_tlsp) != PICO_TLS_HANDSHAKE; }

static unsigned int s_wf_roots_len;
/* the trusted roots (/bios/certs/roots.der, make_roots.py) into their stretch of the Wi-Fi memory,
 * at every start (the start clears that memory). On the caller's thread through the menu's own
 * card volume, like the firmware: the network service never touches the card. */
static void wf_roots_load(void)
{
	fat_vol *v = gba_sd_menu_vol();
	unsigned int n = 0;
	if (v)
		n = gba_sd_read_named(v, "/bios/certs", "roots", "der", (unsigned char *)(addr_t)(WIFI_DMA_PA + WF_ROOTS_AREA),
				      WF_ROOTS_AREA_LEN);
	if (n >= WF_ROOTS_AREA_LEN) n = 0;	/* cut short: not trusted at all */
	s_wf_roots_len = n;
	bt_log("wifi roots: %u bytes", n);
}

/* the trusted roots' length (wf_roots_load), 0 when the card has none (HTTPS then checks nothing
 * about the server and says so) */
static unsigned int wf_roots(void)
{
	return s_wf_roots_len;
}

/* 64 bytes nobody can guess: SHA-256 over the SoC's random generator (pico_random), the timer's
 * jitter read many times, our address and a count. The jitter stays in: should the generator ever
 * fail, the keys are no weaker than before it was used. */
static void wf_entropy(unsigned char out[64])
{
	static unsigned int count;
	struct pico_sha256 h;
	unsigned int part, i, t;
	unsigned char hw[32];
	for (part = 0; part < 2; part++) {
		pico_sha256_start(&h);
		pico_random(hw, sizeof hw);
		pico_sha256_add(&h, hw, sizeof hw);
		for (i = 0; i < 256; i++) {
			t = gpt4_get_current_tick();
			pico_sha256_add(&h, &t, sizeof t);
		}
		pico_sha256_add(&h, s_wf_mac, 6);
		count++;
		pico_sha256_add(&h, &count, sizeof count);
		pico_sha256_add(&h, &part, sizeof part);
		pico_sha256_end(&h, out + 32 * part);
	}
	memset(hw, 0, sizeof hw);
}
static int wf_tcp_opened(void) { return pico_tcp_state(s_wf_get_h) != PICO_TCP_CONNECTING; }
static const char *wf_http_header(const char *h, unsigned int hl, const char *name);
/* the answer is whole by its own framing (a Content-Length met, or a chunked body's last chunk):
 * a kept-alive connection is never closed by the server to say so */
static int wf_answer_whole(const unsigned char *buf, unsigned int len, unsigned int total)
{
	unsigned int i, hl = 0;
	const char *v;
	if (total != len) return 0;			/* (past the buffer: read on to the end, it fails anyway) */
	for (i = 0; i + 3 < len; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { hl = i + 4; break; }
	if (!hl) return 0;
	v = wf_http_header((const char *)buf, hl, "transfer-encoding:");
	if (v && !strncmp(v, "chunked", 7))
		return len >= hl + 5 && !memcmp(buf + len - 5, "0\r\n\r\n", 5);
	if ((v = wf_http_header((const char *)buf, hl, "content-length:")) != 0) {
		unsigned int cl = 0;
		while (*v >= '0' && *v <= '9') cl = cl * 10 + (unsigned)(*v++ - '0');
		return len >= hl + cl;
	}
	return 0;					/* no framing: the server's close ends it */
}
static int wf_get_more(void)
{
	unsigned char *buf = wf_rx_buf();
	unsigned int cap = wf_rx_cap();
	int n;
	for (;;) {
		/* the first cap bytes are kept; past that the last 16 KB are written over (counted) */
		unsigned int at = s_wf_get_len < cap ? s_wf_get_len : cap - 16384;
		unsigned int room = s_wf_get_len < cap ? cap - s_wf_get_len : 16384;
		n = s_wf_get_secure ? pico_tls_recv(s_wf_tlsp, buf + at, room) : pico_tcp_recv(s_wf_get_h, buf + at, room);
		if (n <= 0)
			break;
		if (s_wf_get_len < cap) s_wf_get_len += (unsigned)n;
		s_wf_get_total += (unsigned)n;
		g_ns_crumb[2] = s_wf_get_total;
	}
	if (n < 0) { s_wf_get_closed = 1; return 1; }	/* done: the server closed */
	return wf_answer_whole(buf, s_wf_get_len, s_wf_get_total);
}

/* a header's value in the answer's header block (name ends with ':'), case not counting: its
 * start, 0 when absent */
static const char *wf_http_header(const char *h, unsigned int hl, const char *name)
{
	unsigned int n = (unsigned)strlen(name), i, k;
	for (i = 0; i + n < hl; i++) {
		if (i && h[i - 1] != '\n') continue;
		for (k = 0; k < n; k++) {
			char a = h[i + k], b = name[k];
			if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
			if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
			if (a != b) break;
		}
		if (k == n) {
			i += n;
			while (i < hl && h[i] == ' ') i++;
			return h + i;
		}
	}
	return 0;
}

/* a chunked body (RFC 9112 7.1) put back together where it lies: its length, -1 when broken */
static int wf_unchunk(unsigned char *b, unsigned int len)
{
	unsigned int in = 0, out = 0;
	for (;;) {
		unsigned int size = 0, digits = 0;
		while (in < len) {
			unsigned char c = b[in];
			unsigned int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
					 c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
			if (d == 16) break;
			size = size * 16 + d;
			digits++;
			in++;
		}
		while (in < len && b[in] != '\n') in++;	/* chunk extensions, then the line's end */
		in++;
		if (!digits || in > len) return -1;
		if (!size) return (int)out;
		if (in + size > len) return -1;
		memmove(b + out, b + in, size);
		out += size;
		in += size + 2;				/* the CRLF after the data */
	}
}

static char s_wf_dns_host[96];
static unsigned int s_wf_dns_ip;

/* HTTP GET http://host/path (path already encoded): *status, the body at *body (*blen bytes) in
 * the download buffer. 0 an answer came (any status), -1 none (*why says what failed). */
#include <stdlib.h>				/* malloc: the second connection's TLS state */
static void wf_conn_drop(struct wf_conn *c);
/* a kept connection to host (TLS or not) that may be used again now, 0 none */
static struct wf_conn *wf_conn_find(const char *host, int secure)
{
	unsigned int now = gpt4_get_current_tick(), k;
	for (k = 0; k < WF_CONNS; k++) {
		struct wf_conn *c = &s_wf_conn[k];
		if (!c->alive || c->secure != secure || strcmp(c->host, host)) continue;
		if (c->gen == s_wf_net_gen && pico_tcp_state(c->h) == PICO_TCP_OPEN &&
		    (now - c->last) / 13000u < WF_CONN_IDLE_MS)
			return c;
		wf_conn_drop(c);			/* (old, or the link went: let it go) */
	}
	return 0;
}
/* closes a connection (its TLS first, over its own socket) */
static void wf_conn_drop(struct wf_conn *c)
{
	int h = s_wf_get_h;
	if (!c) return;
	if (c->h >= 0 && c->gen == s_wf_net_gen) {
		s_wf_get_h = c->h;			/* (the TLS close writes through wf_tls_send) */
		if (c->secure && c->tls) pico_tls_close(c->tls);
		pico_tcp_close(c->h);
		s_wf_get_h = h == c->h ? -1 : h;
	}
	c->alive = 0;
	c->h = -1;
}
/* a slot for a new connection: a free one, else the one used longest ago (closed first) */
static struct wf_conn *wf_conn_slot(void)
{
	struct wf_conn *c = 0;
	unsigned int k, now = gpt4_get_current_tick();
	for (k = 0; k < WF_CONNS && !c; k++) if (!s_wf_conn[k].alive) c = &s_wf_conn[k];
	if (!c) {
		c = &s_wf_conn[0];
		for (k = 1; k < WF_CONNS; k++) if (now - s_wf_conn[k].last > now - c->last) c = &s_wf_conn[k];
		wf_conn_drop(c);
	}
	if (!c->tls) {
		if (c == &s_wf_conn[0]) c->tls = &s_wf_tls;
		else if ((c->tls = (struct pico_tls *)malloc(sizeof(struct pico_tls))) == 0) {
			c = &s_wf_conn[0];		/* (no memory: one connection, as before) */
			wf_conn_drop(c);
			c->tls = &s_wf_tls;
		}
	}
	if (c->h >= 0) wf_conn_drop(c);		/* (a socket a failed request left: closed, never lost) */
	c->h = -1;
	return c;
}
static void wf_conn_drop_all(void)
{
	unsigned int k;
	for (k = 0; k < WF_CONNS; k++) wf_conn_drop(&s_wf_conn[k]);
}
static void wf_conn_keep(struct wf_conn *c, const char *host, int secure)
{
	strncpy(c->host, host, sizeof c->host - 1);
	c->host[sizeof c->host - 1] = 0;
	c->secure = secure;
	c->h = s_wf_get_h;
	c->gen = s_wf_net_gen;
	c->last = gpt4_get_current_tick();
	c->alive = strlen(host) < sizeof c->host;	/* (a name too long to keep: not kept) */
	if (!c->alive) wf_conn_drop(c);
}
/* oem netsvc:scrape?: where the HTTP requests' time went since the counts were cleared, ms: the
 * name, the connection, the TLS handshake, the request and its answer */
volatile unsigned int g_wf_step_ms[4];
static void wf_step_ms(int k, unsigned int *t)
{
	unsigned int now = gpt4_get_current_tick();
	g_wf_step_ms[k] += (now - *t) / 13000u;
	*t = now;
}
static int wf_http(const char *host, const char *path, int secure, const unsigned char *post, unsigned int post_len,
		   unsigned int *status, unsigned char **body, unsigned int *blen, const char **why);

/* HTTP(S) GET of host/path (path encoded): see wf_http */
static int wf_http_get(const char *host, const char *path, int secure, unsigned int *status, unsigned char **body,
		       unsigned int *blen, const char **why)
{
	return wf_http(host, path, secure, 0, 0, status, body, blen, why);
}

/* HTTP(S) to host/path: a GET, or a POST of post (post_len bytes, a form) when post is given.
 * *status, the body at *body (*blen bytes) in the download buffer. 0 an answer came (any
 * status), -1 none (*why says what failed). */
static int wf_http(const char *host, const char *path, int secure, const unsigned char *post, unsigned int post_len,
		   unsigned int *status, unsigned char **body, unsigned int *blen, const char **why)
{
	static char req[1024];
	unsigned char *buf = wf_rx_buf();
	unsigned int ip, hl = 0, i, tq = gpt4_get_current_tick();
	const char *v;
	struct wf_conn *cn;
	int reused, freed = 0;
	*status = 0; *body = buf; *blen = 0;
	g_ns_crumb[0] = NSC_DNS;
	g_ns_crumb[2] = 0;
	if (strcmp(host, s_wf_dns_host) || !s_wf_dns_ip) {	/* the last name looked up is kept */
		if (strlen(host) >= sizeof s_wf_dns_host || pico_net_dns(host) != 0) { *why = "bad name"; return -1; }
		wf_link_run(8000000, wf_dns_done);
		if (pico_net_dns_result(&ip) != 1) { *why = "name not found"; return -1; }
		strcpy(s_wf_dns_host, host);
		s_wf_dns_ip = ip;
	}
	if (strlen(path) + strlen(host) + 300 > sizeof req) { *why = "path too long"; return -1; }
	wf_step_ms(0, &tq);
again:
	cn = wf_conn_find(host, secure);
	if (cn) {					/* kept alive from the last request to this host */
		reused = 1;
		s_wf_get_h = cn->h;
		s_wf_get_secure = secure;
		s_wf_tlsp = cn->tls;
		cn->alive = 0;				/* (kept again only once this answer is whole) */
		goto send;
	}
	reused = 0;
	cn = wf_conn_slot();
	if (!cn) { *why = "no memory for a connection"; return -1; }
	g_ns_crumb[0] = NSC_CONNECT;
	s_wf_get_h = pico_tcp_connect(s_wf_dns_ip, secure ? 443 : 80);
	s_wf_get_secure = secure;
	s_wf_tlsp = cn->tls;
	if (s_wf_get_h < 0) {			/* every socket taken: the kept ones go, and once more */
		if (!freed) { freed = 1; wf_conn_drop_all(); goto again; }
		*why = "no connection slot";
		return -1;
	}
	/* the slot owns the socket from here: whatever ends this request closes it (or keeps it) */
	cn->h = s_wf_get_h;
	cn->gen = s_wf_net_gen;
	cn->secure = secure;
	wf_link_run(8000000, wf_tcp_opened);
	if (pico_tcp_state(s_wf_get_h) != PICO_TCP_OPEN) {
		pico_tcp_abort(s_wf_get_h);
		cn->h = -1;
		s_wf_dns_ip = 0;			/* look it up again next time */
		*why = "the server did not answer";
		return -1;
	}
	wf_step_ms(1, &tq);
	if (secure) {
		struct pico_tls_io io;
		unsigned char ent[64];
		io.send = wf_tls_send;
		io.recv = wf_tls_recv;
		io.ctx = 0;
		struct pico_tls_trust trust;
		wf_entropy(ent);
		trust.roots = (const uint8_t *)(addr_t)(WIFI_DMA_PA + WF_ROOTS_AREA);
		trust.roots_len = wf_roots();
		trust.now = wf_time_now();	/* never the RTC here: this may be the service's thread */
		g_ns_crumb[0] = NSC_TLS;
		if (!trust.roots_len) trust.roots = 0;
		pico_tls_start(s_wf_tlsp, &io, host, ent, &trust);
		pico_wipe(ent, sizeof ent);
		wf_link_run(15000000, wf_tls_ready);
		if (s_wf_tlsp->state != PICO_TLS_OPEN) {
			*why = s_wf_tlsp->state == PICO_TLS_FAILED ? s_wf_tlsp->why : "TLS handshake timed out";
			pico_tcp_abort(s_wf_get_h);
			cn->h = -1;
			return -1;
		}
	}
	wf_step_ms(2, &tq);
send:
	g_ns_crumb[0] = NSC_SEND;
	if (post)
		snprintf(req, sizeof req, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
			 "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n\r\n",
			 path, host, s_wf_user_agent[0] ? s_wf_user_agent : "GammaOS-Pico", post_len);
	else
		snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n\r\n",
			 path, host, s_wf_user_agent[0] ? s_wf_user_agent : "GammaOS-Pico");
	if (secure) {
		pico_tls_send(s_wf_tlsp, req, (unsigned)strlen(req));
		if (post && post_len) pico_tls_send(s_wf_tlsp, post, post_len);
	} else {
		pico_tcp_send(s_wf_get_h, req, (unsigned)strlen(req));
		if (post && post_len) wf_tcp_send_all(post, post_len);
	}
	s_wf_get_len = s_wf_get_total = 0;
	s_wf_get_closed = 0;
	g_ns_crumb[0] = NSC_RECV;
	wf_link_run(60000000, wf_get_more);
	wf_step_ms(3, &tq);
	g_ns_crumb[0] = NSC_PARSE;
	if (reused && s_wf_get_len == 0) {	/* the server had let the kept connection go: once more, afresh */
		wf_conn_drop(cn);
		goto again;
	}
	for (i = 0; i + 3 < s_wf_get_len; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { hl = i + 4; break; }
	/* kept for the next request when the answer ended by its framing and the server keeps it */
	if (!s_wf_get_closed && hl && wf_answer_whole(buf, s_wf_get_len, s_wf_get_total) &&
	    !((v = wf_http_header((const char *)buf, hl, "connection:")) && !strncmp(v, "close", 5)))
		wf_conn_keep(cn, host, secure);
	else
		wf_conn_drop(cn);
	hl = 0;
	for (i = 0; i + 3 < s_wf_get_len; i++)
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { hl = i + 4; break; }
	if (s_wf_get_len < wf_rx_cap()) buf[s_wf_get_len] = 0;	/* a text answer ends in a 0 for its readers */
	if (!hl || s_wf_get_len < 12 || memcmp(buf, "HTTP/1.", 7)) { *why = "no HTTP answer"; return -1; }
	*status = (unsigned)(buf[9] - '0') * 100u + (unsigned)(buf[10] - '0') * 10u + (unsigned)(buf[11] - '0');
	*body = buf + hl;
	*blen = s_wf_get_len - hl;
	if (s_wf_get_total > s_wf_get_len) {
		bt_log("http: answer of %u bytes, %u kept: too big", s_wf_get_total, s_wf_get_len);
		*why = "too big";
		return -1;
	}
	v = wf_http_header((const char *)buf, hl, "transfer-encoding:");
	if (v && !strncmp(v, "chunked", 7)) {
		int n = wf_unchunk(*body, *blen);
		if (n < 0) { *why = "broken chunks"; return -1; }
		*blen = (unsigned)n;
	} else if ((v = wf_http_header((const char *)buf, hl, "content-length:")) != 0) {
		unsigned int cl = 0;
		while (*v >= '0' && *v <= '9') cl = cl * 10 + (unsigned)(*v++ - '0');
		if (cl > *blen) { *why = "cut short"; return -1; }
		*blen = cl;
	}
	(*body)[*blen] = 0;					/* (room: the body never fills the buffer to its end) */
	return 0;
}

/* a GET for the fastboot commands: HOST/PATH, plain or over TLS */
static void wf_get_say(const char *arg, int secure)
{
	char host[96], l[96];
	const char *slash, *why = "";
	unsigned char *body;
	unsigned int status, blen, t0, ms;
	int n;
	while (*arg == ':' || *arg == ' ') arg++;
	if (!pico_net_up(0)) { fastboot_fail("no address (oem wifidhcp)"); return; }
	slash = strchr(arg, '/');
	n = slash ? (int)(slash - arg) : (int)strlen(arg);
	if (n <= 0 || n >= (int)sizeof host) { fastboot_fail("wifiget:HOST/PATH"); return; }
	memcpy(host, arg, (unsigned)n);
	host[n] = 0;
	t0 = wf_now_ms();
	if (wf_http_get(host, slash ? slash : "/", secure, &status, &body, &blen, &why) != 0 && !status) {
		snprintf(l, sizeof l, "wifiget: %s", why);
		fastboot_fail(l);
		return;
	}
	ms = wf_now_ms() - t0;
	snprintf(l, sizeof l, "HTTP %u: %u bytes received, body %u, in %u ms, %u KB/s%s%s", status, s_wf_get_total,
		 blen, ms, ms ? s_wf_get_total / ms : 0, *why ? ", " : "", why);
	if (secure) {
		fastboot_info(l);
		snprintf(l, sizeof l, "TLS: %s", s_wf_tlsp->verified ? "server verified (chain, name, dates, signature)" :
			 "NOT verified (no roots on the card: /bios/certs/roots.der)");
	}
	fastboot_info(l);
	{
		struct pico_net_stats st;
		pico_net_get_stats(&st);
		snprintf(l, sizeof l, "tcp segs %u held %u old %u no room %u, ring peak %u/%u", st.tcp_in, st.tcp_held,
			 st.tcp_old, st.tcp_no_room, s_wf_rx_peak, WF_RX0N);
		fastboot_info(l);

	}
	if (blen) {					/* the start of the body, printable */
		unsigned int k, m = blen > 60 ? 60 : blen;
		for (k = 0; k < m; k++) l[k] = (char)(body[k] >= 32 && body[k] < 127 ? body[k] : '.');
		l[m] = 0;
		fastboot_info(l);
	}
	fastboot_okay("");
}

/* ---- box art from the libretro thumbnail server (plain HTTP) ---- */
#define WF_ART_HOST		"thumbnails.libretro.com"

/* the server's folder for a system, 0 none */
static const char *wf_art_system(int console)
{
	switch (console) {
	case GBA_CONSOLE_GB:      return "Nintendo - Game Boy";
	case GBA_CONSOLE_GBC:     return "Nintendo - Game Boy Color";
	case GBA_CONSOLE_GBA:     return "Nintendo - Game Boy Advance";
	case GBA_CONSOLE_SNES:    return "Nintendo - Super Nintendo Entertainment System";
	case GBA_CONSOLE_NES:     return "Nintendo - Nintendo Entertainment System";
	case GBA_CONSOLE_GENESIS: return "Sega - Mega Drive - Genesis";
	case GBA_CONSOLE_SMS:     return "Sega - Master System - Mark III";
	case GBA_CONSOLE_GG:      return "Sega - Game Gear";
	case GBA_CONSOLE_SG:      return "Sega - SG-1000";
	default:                  return 0;
	}
}

/* text into a URL path: letters, digits and -._~ as they are, everything else as %XX of its
 * UTF-8 bytes; the server's own rule first turns &*:`<>?\|/ in a game's name into '_' (game) */
static unsigned int wf_url_add(char *out, unsigned int at, unsigned int cap, const char *text, int game)
{
	static const char hex[] = "0123456789ABCDEF";
	for (; *text && at + 4 < cap; text++) {
		unsigned char c = (unsigned char)*text;
		if (game && strchr("&*/:`<>?\\|\"", c)) c = '_';
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~", c))
			out[at++] = (char)c;
		else { out[at++] = '%'; out[at++] = hex[c >> 4]; out[at++] = hex[c & 15]; }
	}
	out[at] = 0;
	return at;
}

/* oem wifiart - box art for every game on the card that has none: the libretro thumbnail named
 * after the game goes into its boxart folder as a PNG, then into the menu's own form */
static void cmd_wifiart(const char *arg, void *data, unsigned sz)
{
	fat_vol *v = gba_sd_menu_vol();	/* the menu's own volume: never a second mount */
	static char path[640];
	static unsigned char probe[16];
	gba_rom_entry *roms = (gba_rom_entry *)(addr_t)WF_LIST_BUF;
	int nrom, total, i, got = 0, none = 0, had = 0, failed = 0, shown = 0, made;
	char dir[112], l[160];
	(void)arg; (void)data; (void)sz;
	if (!pico_net_up(0)) { fastboot_fail("no address (oem wifidhcp)"); return; }
	if (!v) { fastboot_fail("wifiart: no card"); return; }
	nrom = gba_sd_list_roms(v, roms, (int)(WF_LIST_CAP / sizeof *roms), &total);
	for (i = 0; i < nrom; i++) {
		const gba_rom_entry *r = &roms[i];
		const char *sys = wf_art_system((int)r->type), *home = gba_boxart_home_dir((int)r->type), *why = "";
		unsigned char *body;
		unsigned int status = 0, blen = 0, at;
		int rc = 0;
		if (!sys) continue;
		gba_boxart_dir(r, dir, sizeof dir);
		if (gba_sd_read_named(v, dir, r->name, "ART", probe, sizeof probe) >= 12 ||
		    (home && gba_sd_read_named(v, home, r->name, "ART", probe, sizeof probe) >= 12) ||
		    gba_sd_read_named(v, dir, r->name, "png", probe, sizeof probe) >= 8) {
			had++;
			continue;
		}
		/* /<system>/Named_Boxarts/<game>.png, the slashes as they are. The server names games
		 * the No-Intro way, with their region; a name without one is tried with the usual ones */
		{
			static const char *const regions[] = { "", " (USA)", " (USA, Europe)", " (World)", " (Europe)",
							       " (Japan)", " (USA, Australia)", " (Japan, USA)" };
			/* a card often has '_' where the name had an apostrophe ("Kirby_s Adventure"): that
			 * form is tried too */
			static char named[2][sizeof r->name];
			unsigned int k, f, forms = 1, tries = strchr(r->name, '(') ? 1 : sizeof regions / sizeof regions[0];
			strcpy(named[0], r->name);
			if (strchr(r->name, '_')) {
				char *c;
				strcpy(named[1], r->name);
				for (c = named[1]; *c; c++) if (*c == '_') *c = '\'';
				forms = 2;
			}
			for (f = 0; f < forms && status != 200 && rc == 0; f++)
				for (k = 0; k < tries; k++) {
					path[0] = '/';
					at = wf_url_add(path, 1, sizeof path, sys, 0);
					memcpy(path + at, "/Named_Boxarts/", 16);
					at += 15;
					at = wf_url_add(path, at, sizeof path, named[f], 1);
					at = wf_url_add(path, at, sizeof path, regions[k], 0);
					wf_url_add(path, at, sizeof path, ".png", 0);
					rc = wf_http_get(WF_ART_HOST, path, 0, &status, &body, &blen, &why);
					if (rc != 0 || status == 200)
						break;
				}
		}
		if (rc != 0) {
			failed++;
			snprintf(l, sizeof l, "%s: %s", r->uname, why);
		} else if (status == 200 && blen > 8 && body[0] == 0x89 && body[1] == 'P' && body[2] == 'N' && body[3] == 'G') {
			if (gba_sd_write_named(v, dir, r->name, "png", body, blen) == 0) { got++; snprintf(l, sizeof l, "%s: %u KB", r->uname, blen >> 10); }
			else { failed++; snprintf(l, sizeof l, "%s: the card write failed", r->uname); }
		} else {
			none++;
			snprintf(l, sizeof l, "%s: none (HTTP %u)", r->uname, status);
		}
		if (shown++ < 40) fastboot_info(l);
	}
	made = got ? pico_artimport_run(v, roms, nrom) : 0;
	snprintf(l, sizeof l, "box art: %d new (%d made for the menu), %d had art, %d not on the server, %d failed%s",
		 got, made, had, none, failed, total > nrom ? " (game list cut)" : "");
	fastboot_info(l);
	fastboot_okay("");
}

/* oem wifiget:HOST/PATH - an HTTP GET (port 80): the answer's status, size and speed */
static void cmd_wifiget(const char *arg, void *data, unsigned sz) { wf_get_say(arg, 0); }
/* oem wifihttps:HOST/PATH - the same over TLS 1.3 (port 443; the certificate is not checked yet) */
static void cmd_wifihttps(const char *arg, void *data, unsigned sz) { wf_get_say(arg, 1); }

/* oem wifiurl - a GET of a URL staged first ("http://host/path" or "https://host/path", then a
 * 0; fastboot stage <file>): a fastboot command holds 64 bytes, and a longer one leaves the
 * Block's USB session out of step until a power cycle */
static void cmd_wifiurl(const char *arg, void *data, unsigned sz)
{
	static char url[600];
	const char *d = (const char *)data, *u;
	unsigned int i;
	int secure;
	(void)arg;
	for (i = 0; d && i < sz && i < sizeof url - 1 && d[i]; i++) url[i] = d[i];
	if (!d || !i || i >= sz || i >= sizeof url - 1) { fastboot_fail("wifiurl: stage the URL, then a 0"); return; }
	url[i] = 0;
	if (!strncmp(url, "https://", 8)) { secure = 1; u = url + 8; }
	else if (!strncmp(url, "http://", 7)) { secure = 0; u = url + 7; }
	else { fastboot_fail("wifiurl: http:// or https://"); return; }
	wf_get_say(u, secure);
}


/* ======================= the network service (pico_netsvc.h) ======================= */
#include "pico_netsvc.h"
#include <kernel/mutex.h>
#include <kernel/event.h>

static mutex_t s_wf_radio;		/* whoever drives the radio: the service, a fastboot command */
static event_t s_ns_wake;
static thread_t *s_ns_thread;
static struct netsvc_status s_ns_st;
static struct netsvc_ap s_ns_aps[WF_SCAN_MAX];
static int s_ns_naps;
static enum { NS_NONE, NS_SCAN, NS_JOIN, NS_LEAVE, NS_OFF } s_ns_cmd;
static char s_ns_join_ssid[33], s_ns_join_pass[64];
static int s_ns_join_saved;		/* the join uses a saved network's PMK */
static int s_ns_auto;			/* after the next scan: join the strongest saved network */
static int s_ns_auto_left;		/* scans still to make for one when the last heard none */
static unsigned int s_ns_auto_tick;	/* when the last of those was */
static int s_ns_join_auto;		/* the join under way is one of those (not the player's) */
static int s_ns_auto_tries;		/* failed automatic joins that may still be tried again (each Wi-Fi start) */
#define NS_AUTO_TRIES 4			/* (a minute of them: a missed key handshake is not the end) */

/* the saved networks: name and PMK (never the passphrase) */
#define NS_SAVED_MAX	16
static struct { char ssid[33]; unsigned char pmk[32]; } s_ns_saved[NS_SAVED_MAX];
static int s_ns_nsaved;

/* an HTTP request handed to the service */
static enum { NSH_IDLE, NSH_QUEUED, NSH_RUNNING, NSH_DONE, NSH_FAILED } s_nsh_state;
static int s_nsh_drop;			/* the asker gave the running transfer up (netsvc_http_abort) */
static char s_nsh_host[96], s_nsh_path[640];
static int s_nsh_secure;
static unsigned char s_nsh_post[4096];
static unsigned int s_nsh_post_len, s_nsh_status, s_nsh_len;
static unsigned char *s_nsh_buf;		/* the job's own answer memory (0: the usual) */
static unsigned int s_nsh_cap;
static const unsigned char *s_nsh_body;
static const char *s_nsh_why;

static int pico_wifi_in_use(void) { return s_wf_up; }

static void ns_set_state(int state, const char *why)
{
	mutex_acquire(&s_ns_info);
	s_ns_st.state = state;
	if (why) s_ns_st.why = why;
	mutex_release(&s_ns_info);
}

static int ns_saved_find(const char *ssid)
{
	int i;
	for (i = 0; i < s_ns_nsaved; i++)
		if (!strcmp(s_ns_saved[i].ssid, ssid)) return i;
	return -1;
}

/* the last scan into the menu's list: usable kinds marked, saved ones marked, strongest first */
static void ns_take_scan(void)
{
	int i, j, n = 0;
	mutex_acquire(&s_ns_info);
	for (i = 0; i < (int)s_wf_nseen && n < WF_SCAN_MAX; i++) {
		const struct wf_seen *e = &s_wf_seen[i];
		struct netsvc_ap a;
		unsigned char k, en;
		const char *why;
		if (!e->ssid_len || !e->ssid[0]) continue;	/* hidden networks are not listed */
		for (j = 0; j < n; j++)			/* one row per name: the strongest AP */
			if (!strcmp(s_ns_aps[j].ssid, e->ssid)) break;
		if (j < n) { if (e->dbm > s_ns_aps[j].dbm) { s_ns_aps[j].dbm = e->dbm; s_ns_aps[j].channel = e->channel; } continue; }
		memset(&a, 0, sizeof a);
		memcpy(a.ssid, e->ssid, sizeof a.ssid);
		a.dbm = e->dbm;
		a.channel = e->channel;
		a.secured = e->secured;
		a.usable = wf_pick_security(e, &k, &en, &why) == 0;
		a.saved = ns_saved_find(e->ssid) >= 0;
		s_ns_aps[n++] = a;
	}
	for (i = 1; i < n; i++)				/* strongest first */
		for (j = i; j > 0 && s_ns_aps[j].dbm > s_ns_aps[j - 1].dbm; j--) {
			struct netsvc_ap t = s_ns_aps[j]; s_ns_aps[j] = s_ns_aps[j - 1]; s_ns_aps[j - 1] = t;
		}
	s_ns_naps = n;
	s_ns_st.scan_gen++;
	mutex_release(&s_ns_info);
}

static int wf_have_address(void);

/* join (the service thread, the radio held): the network, an address, the clock */
/* a join's steps (the AP tried, authentication, association, the keys) into oem btlog, so a join
 * made from the menu can be read back afterwards */
static void ns_join_say(const char *line) { bt_log("join: %s", line); }

/* an automatic join that failed (the key handshake missed, no address): another look and try in
 * 15 s, a few times a Wi-Fi start; the player's own join is not tried again */
static void ns_auto_again(void)
{
	if (!s_ns_join_auto || s_ns_auto_tries <= 0) return;
	s_ns_auto_tries--;
	s_ns_auto_left = 1;
	s_ns_auto_tick = gpt4_get_current_tick();
	bt_log("join: again in 15 s (%d more after that)", s_ns_auto_tries);
}

static void ns_join(void)
{
	const char *why = "";
	char l[96];
	struct pico_net_conf c;
	int k;
	mutex_acquire(&s_ns_info);
	memcpy(s_ns_st.ssid, s_ns_join_ssid, sizeof s_ns_st.ssid);
	s_ns_st.state = NETSVC_JOINING;
	s_ns_st.why = "";
	mutex_release(&s_ns_info);
	if (s_wf_linked) { wf_net_stop(); s_wf_linked = 0; }
	/* the key: a saved network's PMK, or the passphrase given */
	pico_wipe(s_wf_pmk, sizeof s_wf_pmk);
	s_wf_pmk_ok = 0;
	pico_wipe(s_wf_pass, sizeof s_wf_pass);
	s_wf_pass_len = 0;
	k = ns_saved_find(s_ns_join_ssid);
	if (s_ns_join_saved && k >= 0) {
		memcpy(s_wf_pmk, s_ns_saved[k].pmk, 32);
		memcpy(s_wf_pmk_ssid, s_ns_join_ssid, sizeof s_wf_pmk_ssid);
		s_wf_pmk_ok = 1;
	} else {
		s_wf_pass_len = (unsigned)strlen(s_ns_join_pass);
		memcpy(s_wf_pass, s_ns_join_pass, s_wf_pass_len);
	}
	pico_wipe(s_ns_join_pass, sizeof s_ns_join_pass);
	if (wf_connect(s_ns_join_ssid, ns_join_say, &why) != 0) {
		bt_log("join: failed: %s", why);
		ns_set_state(NETSVC_READY, why);
		ns_auto_again();
		return;
	}
	wf_link_start();
	pico_net_dhcp("GammaOS-Pico");
	wf_link_run(15000000, wf_have_address);
	if (!pico_net_up(&c)) {
		ns_set_state(NETSVC_READY, "no address from the network (DHCP)");
		ns_auto_again();
		return;
	}
	/* a joined secured network is saved (its PMK) */
	if (s_wf_pmk_ok && s_ns_join_saved == 0) {
		mutex_acquire(&s_ns_info);
		k = ns_saved_find(s_ns_join_ssid);
		if (k < 0 && s_ns_nsaved < NS_SAVED_MAX) k = s_ns_nsaved++;
		if (k < 0) k = NS_SAVED_MAX - 1;
		memcpy(s_ns_saved[k].ssid, s_ns_join_ssid, sizeof s_ns_saved[k].ssid);
		memcpy(s_ns_saved[k].pmk, s_wf_pmk, 32);
		s_ns_st.saved_gen++;
		mutex_release(&s_ns_info);
	}
	/* the network's time, for the menu thread to set the clock with (netsvc_clock_apply): the
	 * RTC sits behind the PMIC wrapper the menu reads it through, so only one thread touches it */
	{
		long long t;
		if (wf_net_time(&t) == 0) {
			mutex_acquire(&s_ns_info);
			s_ns_time = t;
			s_ns_time_tick = gpt4_get_current_tick();
			mutex_release(&s_ns_info);
		}
	}
	mutex_acquire(&s_ns_info);
	s_ns_st.state = NETSVC_ONLINE;
	s_ns_st.dbm = s_wf_join_dbm;
	s_ns_st.ip = c.ip;
	s_ns_st.gateway = c.gateway;
	s_ns_st.dns = c.dns[0];
	mutex_release(&s_ns_info);
}

/* the strongest saved network the last scan heard: its index in the list, -1 none */
static int ns_auto_pick(void)
{
	int i;
	for (i = 0; i < s_ns_naps; i++)		/* strongest first */
		if (s_ns_aps[i].saved && s_ns_aps[i].usable) return i;
	return -1;
}

static void ns_http(void)
{
	unsigned int status = 0, len = 0;
	unsigned char *body = 0;
	const char *why = "";
	int rc;
	mutex_acquire(&s_ns_info);
	s_nsh_state = NSH_RUNNING;
	s_wf_abort = 0;
	mutex_release(&s_ns_info);
	/* a transfer waits asleep too: this thread is above the menu's priority, so it is back the
	 * moment a sleep ends (it ran flat out at the menu's own priority, and a server taking its
	 * time to answer had the two of them trading the CPU so badly that a TLS handshake took a
	 * minute) */
	s_wf_coop = 1;
	s_wf_rx_buf = s_nsh_buf; s_wf_rx_cap = s_nsh_buf ? s_nsh_cap : 0;
	rc = wf_http(s_nsh_host, s_nsh_path, s_nsh_secure, s_nsh_post_len ? s_nsh_post : 0, s_nsh_post_len, &status, &body,
		     &len, &why);
	s_wf_rx_cap = 0; s_wf_rx_buf = 0;		/* (the answer stays where it is: the job's caller reads it there) */
	s_wf_coop = 1;
	mutex_acquire(&s_ns_info);
	s_nsh_status = status;
	s_nsh_body = body;
	s_nsh_len = len;
	s_nsh_why = s_wf_abort ? "stopped" : why;
	/* given up while it ran: nobody will read the answer, so the slot is free at once */
	s_nsh_state = s_nsh_drop ? NSH_IDLE : rc == 0 ? NSH_DONE : NSH_FAILED;
	s_nsh_drop = 0;
	s_wf_abort = 0;
	mutex_release(&s_ns_info);
}

/* a game is being played: the service sleeps (no link pumps, nothing sent), so it costs the game
 * nothing; the network stays joined and picks up where it was when the menu is back */
static volatile int s_ns_quiet;
/* The service's locks and its wake event, made once, by whichever comes first: the menu (its first
 * netsvc call can come before the fastboot commands are registered, at the same moment of the boot)
 * or the registration. A zeroed event signalled before this sent the KPA's boot round in a loop. */
static volatile int s_ns_inited;
static void ns_ready(void)
{
	if (s_ns_inited) return;
	enter_critical_section();
	if (!s_ns_inited) {
		mutex_init(&s_wf_radio);
		mutex_init(&s_ns_info);
		event_init(&s_ns_wake, false, EVENT_FLAG_AUTOUNSIGNAL);
		s_ns_inited = 1;
	}
	exit_critical_section();
}

void netsvc_quiet(int on)
{
	ns_ready();
	s_ns_quiet = on;
	if (!on) event_signal(&s_ns_wake, false);
}

/* a transfer asked for while online, and the network gone before it ran: it fails, rather than
 * wait for a network that may not come back (the asker would wait with it) */
static void ns_fail_queued(int online)
{
	if (online) return;
	mutex_acquire(&s_ns_info);
	if (s_nsh_state == NSH_QUEUED) {
		s_nsh_status = 0;
		s_nsh_len = 0;
		s_nsh_why = "not connected to a network";
		s_nsh_state = NSH_FAILED;
	}
	mutex_release(&s_ns_info);
}

static int ns_thread(void *arg)
{
	(void)arg;
	for (;;) {
		int cmd, online;
		g_ns_crumb[0] = NSC_WAIT;
		event_wait_timeout(&s_ns_wake, s_ns_quiet ? 1000 : s_ns_st.state == NETSVC_ONLINE ? 10 : 100);
		if (s_ns_quiet) continue;
		mutex_acquire(&s_ns_info);
		cmd = s_ns_cmd;
		s_ns_cmd = NS_NONE;
		mutex_release(&s_ns_info);
		mutex_acquire(&s_wf_radio);
		s_wf_coop = 1;
		if (!s_wf_up) {
			ns_fail_queued(0);
			if (s_ns_st.state != NETSVC_OFF && s_ns_st.state != NETSVC_STARTING && s_ns_st.state != NETSVC_FAILED)
				ns_set_state(NETSVC_OFF, 0);
			s_wf_coop = 0;
			mutex_release(&s_wf_radio);
			continue;
		}
		if (cmd == NS_SCAN && s_ns_st.state != NETSVC_ONLINE) {
			unsigned int ms;
			ns_set_state(NETSVC_SCANNING, 0);
			g_ns_crumb[0] = NSC_SCAN;
			if (wf_scan(&ms) == 0) ns_take_scan();
			ns_set_state(NETSVC_READY, 0);
			if (s_ns_auto) {
				int i = ns_auto_pick();
				s_ns_auto = 0;
				if (i >= 0) {
					memcpy(s_ns_join_ssid, s_ns_aps[i].ssid, sizeof s_ns_join_ssid);
					s_ns_join_saved = 1;
					s_ns_join_auto = 1;
					s_ns_auto_left = 0;
					cmd = NS_JOIN;
				} else {	/* a weak saved network a scan can miss: looked for again shortly */
					s_ns_auto_tick = gpt4_get_current_tick();
				}
			}
		}
		if (cmd == NS_NONE && s_ns_auto_left > 0 && s_ns_st.state == NETSVC_READY && s_ns_nsaved &&
		    GPT_US(gpt4_get_current_tick() - s_ns_auto_tick) > 15000000u) {
			/* Wi-Fi on and nothing joined yet: another look for a saved network, every 15 s, a few times */
			unsigned int ms;
			s_ns_auto_left--;
			s_ns_auto_tick = gpt4_get_current_tick();
			ns_set_state(NETSVC_SCANNING, 0);
			if (wf_scan(&ms) == 0) ns_take_scan();
			ns_set_state(NETSVC_READY, 0);
			{
				int i = ns_auto_pick();
				if (i >= 0) {
					memcpy(s_ns_join_ssid, s_ns_aps[i].ssid, sizeof s_ns_join_ssid);
					s_ns_join_saved = 1;
					s_ns_join_auto = 1;
					s_ns_auto_left = 0;
					cmd = NS_JOIN;
				}
			}
		}
		if (cmd == NS_JOIN) { g_ns_crumb[0] = NSC_JOIN; s_ns_auto_left = 0; ns_join(); }
		if (cmd == NS_LEAVE && s_wf_linked) {
			wf_leave_ap();
			wf_net_stop();
			s_wf_linked = 0;
			ns_set_state(NETSVC_READY, "");
		}
		online = s_ns_st.state == NETSVC_ONLINE;
		ns_fail_queued(online);
		if (online && s_nsh_state == NSH_QUEUED) {
			ns_http();
			g_ns_crumb[0] = NSC_DONE;
		} else if (online) {
			int i;
			g_ns_crumb[0] = NSC_PUMP;
			for (i = 0; i < 4; i++) wf_link_pump();	/* ARP answers, DHCP renewals, new group keys */
		}
		s_wf_coop = 0;
		mutex_release(&s_wf_radio);
	}
	return 0;
}

static void ns_wake(void) { event_signal(&s_ns_wake, false); }

void netsvc_wifi(int on)
{
	ns_ready();
	if (on) {
		if (s_wf_up) return;
		ns_set_state(NETSVC_STARTING, "");
		mutex_acquire(&s_wf_radio);		/* the start reads the card: the caller's thread */
		if (pico_wifi_up() != 0) {
			mutex_release(&s_wf_radio);
			ns_set_state(NETSVC_FAILED, "Wi-Fi did not start (its firmware in /bios/connsys?)");
			return;
		}
		mutex_release(&s_wf_radio);
		ns_set_state(NETSVC_READY, "");
		mutex_acquire(&s_ns_info);
		s_ns_cmd = NS_SCAN;
		s_ns_auto = 1;				/* a saved network in range is joined */
		s_ns_auto_left = 6;			/* looked for again for a minute and a half when missed */
		s_ns_auto_tries = NS_AUTO_TRIES;
		mutex_release(&s_ns_info);
		if (!s_ns_thread) {
			/* one above the menu's priority (the menu hardly sleeps: at its level this thread got
			 * the CPU's leftovers), and it waits asleep, so it costs nothing with nothing to do */
			s_ns_thread = thread_create("pico_net", ns_thread, 0, DEFAULT_PRIORITY + 1, 16384);
			if (s_ns_thread) thread_resume(s_ns_thread);
		}
		ns_wake();
		return;
	}
	mutex_acquire(&s_wf_radio);
	if (s_wf_up) {
		wf_leave_ap();
		wf_net_stop();
		s_wf_linked = 0;
		s_wf_up = 0;
		s_wf_station = 0;
		if (s_svc_state == SVC_OFF || s_svc_state == SVC_FAILED)	/* the Bluetooth link not using CONNSYS */
			pico_bt_off();
	}
	mutex_release(&s_wf_radio);
	ns_set_state(NETSVC_OFF, "");
}

void netsvc_scan(void)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	s_ns_cmd = NS_SCAN;
	mutex_release(&s_ns_info);
	ns_wake();
}

int netsvc_aps(struct netsvc_ap *out, int max)
{
	int n;
	ns_ready();
	mutex_acquire(&s_ns_info);
	n = s_ns_naps < max ? s_ns_naps : max;
	memcpy(out, s_ns_aps, sizeof *out * (unsigned)n);
	mutex_release(&s_ns_info);
	return n;
}

void netsvc_join(const char *ssid, const char *pass)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	strncpy(s_ns_join_ssid, ssid, sizeof s_ns_join_ssid - 1);
	s_ns_join_ssid[sizeof s_ns_join_ssid - 1] = 0;
	s_ns_join_saved = pass == 0;
	s_ns_join_auto = 0;			/* the player's own: tried once, as asked */
	pico_wipe(s_ns_join_pass, sizeof s_ns_join_pass);
	if (pass) strncpy(s_ns_join_pass, pass, sizeof s_ns_join_pass - 1);
	s_ns_cmd = NS_JOIN;
	s_ns_st.state = NETSVC_JOINING;
	memcpy(s_ns_st.ssid, s_ns_join_ssid, sizeof s_ns_st.ssid);
	mutex_release(&s_ns_info);
	ns_wake();
}

void netsvc_leave(void)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	s_ns_cmd = NS_LEAVE;
	mutex_release(&s_ns_info);
	ns_wake();
}

void netsvc_get(struct netsvc_status *st)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	*st = s_ns_st;
	if (!st->why) st->why = "";
	mutex_release(&s_ns_info);
}

int netsvc_saved_names(char names[][33], int max)
{
	int i, n;
	ns_ready();
	mutex_acquire(&s_ns_info);
	n = s_ns_nsaved < max ? s_ns_nsaved : max;
	for (i = 0; i < n; i++) memcpy(names[i], s_ns_saved[i].ssid, 33);
	mutex_release(&s_ns_info);
	return n;
}

void netsvc_forget(const char *ssid)
{
	int k;
	ns_ready();
	mutex_acquire(&s_ns_info);
	k = ns_saved_find(ssid);
	if (k >= 0) {
		pico_wipe(&s_ns_saved[k], sizeof s_ns_saved[k]);
		memmove(&s_ns_saved[k], &s_ns_saved[k + 1], sizeof s_ns_saved[0] * (unsigned)(s_ns_nsaved - k - 1));
		s_ns_nsaved--;
		s_ns_st.saved_gen++;
	}
	for (k = 0; k < s_ns_naps; k++)
		if (!strcmp(s_ns_aps[k].ssid, ssid)) s_ns_aps[k].saved = 0;
	mutex_release(&s_ns_info);
}

/* The saved networks' file, /gammaos/net/wifi.dat: "GPWN", version 1, the count, then per
 * network its name (33 bytes) and PMK (32), the whole after the head masked with a keystream
 * from this console's serial (SHA-256 in counter mode), so a card read elsewhere shows no keys. */
#define NS_SAVED_DIR	"/gammaos/net"
/* a file's secrets on the card (the saved networks' keys, RetroAchievements' token) masked with
 * a stream from SHA-256 of label, this console's serial and a count: unreadable on another
 * console or to anyone who looks at the card. Applying it again unmasks. */
void netsvc_mask(void *data, unsigned int len, const char *label)
{
	unsigned char block[32], *p = (unsigned char *)data;
	unsigned int i, ctr = 0;
	for (i = 0; i < len; i++) {
		if (!(i % 32)) {
			struct pico_sha256 h;
			pico_sha256_start(&h);
			pico_sha256_add(&h, label, strlen(label));
			pico_sha256_add(&h, sn_buf, strlen(sn_buf));
			pico_sha256_add(&h, &ctr, sizeof ctr);
			pico_sha256_end(&h, block);
			ctr++;
		}
		p[i] ^= block[i % 32];
	}
	pico_wipe(block, sizeof block);
}

static void ns_mask(unsigned char *p, unsigned int len)
{
	netsvc_mask(p, len, "pico wifi saved");
}

void netsvc_saved_load(void *vol)
{
	static unsigned char f[8 + NS_SAVED_MAX * 65];
	unsigned int n, i, cnt;
	ns_ready();
	if (!vol) return;
	n = gba_sd_read_named((fat_vol *)vol, NS_SAVED_DIR, "wifi", "dat", f, sizeof f);
	if (n < 8 || memcmp(f, "GPWN", 4) || f[4] != 1) return;
	ns_mask(f + 8, n - 8);
	cnt = f[5];
	mutex_acquire(&s_ns_info);
	s_ns_nsaved = 0;
	for (i = 0; i < cnt && i < NS_SAVED_MAX && 8 + (i + 1) * 65 <= n; i++) {
		memcpy(s_ns_saved[i].ssid, f + 8 + i * 65, 33);
		s_ns_saved[i].ssid[32] = 0;
		memcpy(s_ns_saved[i].pmk, f + 8 + i * 65 + 33, 32);
		s_ns_nsaved++;
	}
	mutex_release(&s_ns_info);
	pico_wipe(f, sizeof f);
}

void netsvc_saved_store(void *vol)
{
	static unsigned char f[8 + NS_SAVED_MAX * 65];
	unsigned int i, len;
	ns_ready();
	if (!vol) return;
	memset(f, 0, sizeof f);
	memcpy(f, "GPWN", 4);
	f[4] = 1;
	mutex_acquire(&s_ns_info);
	f[5] = (unsigned char)s_ns_nsaved;
	for (i = 0; i < (unsigned)s_ns_nsaved; i++) {
		memcpy(f + 8 + i * 65, s_ns_saved[i].ssid, 33);
		memcpy(f + 8 + i * 65 + 33, s_ns_saved[i].pmk, 32);
	}
	len = 8 + (unsigned)s_ns_nsaved * 65;
	mutex_release(&s_ns_info);
	ns_mask(f + 8, len - 8);
	gba_sd_write_named((fat_vol *)vol, NS_SAVED_DIR, "wifi", "dat", f, len);
	pico_wipe(f, sizeof f);
}

/* a request is waiting or running (no lock: a look for the menu, which then gives it time) */
int netsvc_http_busy(void) { return s_nsh_state == NSH_QUEUED || s_nsh_state == NSH_RUNNING; }
int netsvc_http_begin(const char *host, const char *path, int secure, const void *post, unsigned int post_len)
{
	return netsvc_http_begin_into(host, path, secure, post, post_len, 0, 0);
}
int netsvc_http_begin_into(const char *host, const char *path, int secure, const void *post, unsigned int post_len,
			   void *buf, unsigned int cap)
{
	int ok = 0;
	if (buf && cap < 65536u) buf = 0;		/* (too small to be of use: the usual memory) */
	ns_ready();
	mutex_acquire(&s_ns_info);
	if (s_ns_st.state == NETSVC_ONLINE && s_nsh_state == NSH_IDLE && strlen(host) < sizeof s_nsh_host &&
	    strlen(path) < sizeof s_nsh_path && post_len <= sizeof s_nsh_post) {
		strcpy(s_nsh_host, host);
		strcpy(s_nsh_path, path);
		s_nsh_secure = secure;
		s_nsh_post_len = post ? post_len : 0;
		if (post && post_len) memcpy(s_nsh_post, post, post_len);
		s_nsh_buf = (unsigned char *)buf;
		s_nsh_cap = cap;
		s_nsh_state = NSH_QUEUED;
		ok = 1;
	}
	mutex_release(&s_ns_info);
	if (ok) ns_wake();
	return ok ? 0 : -1;
}

int netsvc_http_poll(unsigned int *status, const unsigned char **body, unsigned int *len, const char **why)
{
	int r;
	ns_ready();
	mutex_acquire(&s_ns_info);
	r = s_nsh_state == NSH_DONE ? 1 : s_nsh_state == NSH_FAILED ? -1 : s_nsh_state == NSH_IDLE ? -2 : 0;
	if (r == 1) { *status = s_nsh_status; *body = s_nsh_body; *len = s_nsh_len; }
	if (r == -1 && why) *why = s_nsh_why;
	mutex_release(&s_ns_info);
	return r;
}

void netsvc_http_release(void)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	if (s_nsh_state == NSH_RUNNING)
		s_nsh_drop = 1;		/* still running: the slot frees itself when it ends */
	else
		s_nsh_state = NSH_IDLE;
	mutex_release(&s_ns_info);
}

void netsvc_http_abort(void)
{
	ns_ready();
	mutex_acquire(&s_ns_info);
	if (s_nsh_state == NSH_RUNNING) {
		s_nsh_drop = 1;
		s_wf_abort = 1;		/* its wait ends at the next turn of the link */
	} else {
		s_nsh_state = NSH_IDLE;
	}
	mutex_release(&s_ns_info);
}

void netsvc_time_hint(void)
{
	long long t = pico_clock_is_set() ? pico_clock_utc() : 0;
	ns_ready();
	mutex_acquire(&s_ns_info);
	s_now_utc = t;
	s_now_tick = gpt4_get_current_tick();
	mutex_release(&s_ns_info);
}

int netsvc_clock_apply(void)
{
	long long t, had;
	unsigned int tick;
	ns_ready();
	mutex_acquire(&s_ns_info);
	t = s_ns_time;
	tick = s_ns_time_tick;
	s_ns_time = 0;
	mutex_release(&s_ns_info);
	if (!t) return 0;
	t += (long long)(GPT_US(gpt4_get_current_tick() - tick) / 1000000u);
	had = pico_clock_is_set() ? pico_clock_utc() : 0;
	if (had && had - t <= 2 && t - had <= 2) return 0;
	pico_clock_set_utc(t);
	return 1;
}

void netsvc_user_agent(const char *ua)
{
	ns_ready();
	strncpy(s_wf_user_agent, ua, sizeof s_wf_user_agent - 1);
}

/* oem netsvc:on|off|scan|st|join:SSID (the passphrase from oem wifipass)|get:HOST/PATH - the
 * service from the bootloader channel, for tests */
static void cmd_netsvc(const char *arg, void *data, unsigned sz)
{
	char l[96], t[16];
	struct netsvc_status st;
	while (*arg == ':' || *arg == ' ') arg++;
	if (!strncmp(arg, "on", 2)) netsvc_wifi(1);
	else if (!strncmp(arg, "off", 3)) netsvc_wifi(0);
	else if (!strncmp(arg, "scan", 4)) netsvc_scan();
	else if (!strncmp(arg, "join:", 5)) {
		char pass[64];
		memcpy(pass, s_wf_pass, sizeof pass);
		pass[s_wf_pass_len < 63 ? s_wf_pass_len : 63] = 0;
		netsvc_join(arg + 5, ns_saved_find(arg + 5) >= 0 && !s_wf_pass_len ? 0 : pass);
		pico_wipe(pass, sizeof pass);
	} else if (!strncmp(arg, "get:", 4)) {
		const char *slash = strchr(arg + 4, '/');
		char host[96];
		unsigned int n = slash ? (unsigned)(slash - (arg + 4)) : (unsigned)strlen(arg + 4);
		if (n >= sizeof host) n = sizeof host - 1;
		memcpy(host, arg + 4, n);
		host[n] = 0;
		snprintf(l, sizeof l, "queued: %d", netsvc_http_begin(host, slash ? slash : "/", 1, 0, 0));
		fastboot_info(l);
	} else if (!strncmp(arg, "canary", 6)) {
		/* the Wi-Fi memory nobody of ours writes while joined: after the rings, after the answer
		 * buffers, the game list's stretch. canary:set fills it with a pattern; canary reports what
		 * changed (by the radio, or a stray write of ours) */
		static const unsigned int gap[3][2] = { { 0x1800u, 0x10000u }, { 0x58000u, 0x60000u },
							 { WF_LIST_AREA, WF_LIST_AREA + WF_LIST_AREA_LEN } };
		unsigned int g, o;
		for (g = 0; g < 3; g++) {
			volatile unsigned int *w = WF_AT(gap[g][0]);
			unsigned int nw = (gap[g][1] - gap[g][0]) / 4u, bad = 0, first = 0, last = 0;
			for (o = 0; o < nw; o++) {
				unsigned int want = 0xC0DE0000u ^ (gap[g][0] + o * 4u);
				if (arg[6] == ':') { w[o] = want; continue; }
				if (w[o] != want) { if (!bad) first = o; last = o; bad++; }
			}
			snprintf(l, sizeof l, "gap %05x..%05x: %u words changed, first +%x last +%x", gap[g][0], gap[g][1], bad,
				 first * 4u, last * 4u);
			fastboot_info(l);
		}
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "rsn:", 4)) {
		/* a network's security element as the last scan heard it (each AP of the name), in hex */
		unsigned int i, k;
		for (i = 0; i < s_wf_nseen; i++) {
			const struct wf_seen *a = &s_wf_seen[i];
			if (strcmp(a->ssid, arg + 4)) continue;
			snprintf(l, sizeof l, "ch %u %d dBm wpa %u rsn %u bytes:", a->channel, a->dbm, a->wpa_len, a->rsn_len);
			fastboot_info(l);
			for (k = 0; k < a->rsn_len; k += 24) {	/* 24 bytes a line (an info line is short) */
				unsigned int j;
				char *o2 = l;
				for (j = k; j < a->rsn_len && j < k + 24; j++) o2 += snprintf(o2, 3, "%02x", a->rsn[j]);
				fastboot_info(l);
			}
		}
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "scrape?", 7)) {	/* the box art's counts and times (pico_scrape_report) */
		extern void pico_scrape_report(char *a, unsigned cap_a, char *b, unsigned cap_b) __attribute__((weak));
		char l2[96];
		if (pico_scrape_report) {
			extern const char *pico_scrape_fail_line(int i);
			int fi;
			pico_scrape_report(l, sizeof l, l2, sizeof l2);
			fastboot_info(l);
			fastboot_info(l2);
			snprintf(l2, sizeof l2, "ms dns %u tcp %u tls %u io %u", g_wf_step_ms[0], g_wf_step_ms[1], g_wf_step_ms[2],
				 g_wf_step_ms[3]);
			fastboot_info(l2);
			{ extern volatile unsigned int g_tls_ms[3];
			  snprintf(l2, sizeof l2, "tls max: chain %u sig %u kx %u", g_tls_ms[0], g_tls_ms[1], g_tls_ms[2]);
			  fastboot_info(l2); }
			{ extern volatile unsigned g_dbg_idle_crit, g_dbg_idle_stale, g_dbg_idle_calls, g_dbg_idle_sleeps;
			  snprintf(l2, sizeof l2, "idle calls %u sleeps %u crit %u stale %u", g_dbg_idle_calls, g_dbg_idle_sleeps,
				   g_dbg_idle_crit, g_dbg_idle_stale);
			  fastboot_info(l2); }

			for (fi = 0; fi < 3; fi++) if (pico_scrape_fail_line(fi)[0]) fastboot_info(pico_scrape_fail_line(fi));
		}
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "crumb", 5)) {
		/* no lock taken: this must answer even when the service or the menu is stuck */
		extern volatile unsigned int g_dbg_present_cnt, g_scrape_crumb;
		/* short: a fastboot info line keeps about 64 characters */
		snprintf(l, sizeof l, "s%u p%u b%u t%u n%u f%u x%x", g_ns_crumb[0], g_ns_crumb[1], g_ns_crumb[2],
			 g_ns_crumb[3], gpt4_get_current_tick() / 13000u, g_dbg_present_cnt, g_scrape_crumb);
		fastboot_info(l);
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "stall", 5)) {
		/* what netsvc_stall_watch noted, a line at a time, cut to what a fastboot line holds */
		unsigned int i = 0;
		if (!s_ns_stall_len) fastboot_info("no stall noted");
		while (i < s_ns_stall_len) {
			unsigned int j = i, k = 0;
			while (j < s_ns_stall_len && s_ns_stall[j] != '\n') j++;
			while (i < j) {
				k = j - i > 56 ? 56 : j - i;
				memcpy(l, s_ns_stall + i, k);
				l[k] = 0;
				fastboot_info(l);
				i += k;
			}
			i = j + 1;
		}
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "ua", 2)) {	/* the user agent the next request carries */
		fastboot_info(s_wf_user_agent[0] ? s_wf_user_agent : "GammaOS-Pico (none set)");
		fastboot_okay("");
		return;
	} else if (!strncmp(arg, "m2:", 3)) {
		s_wf_key_path = !strcmp(arg + 3, "eth") ? 1 : !strcmp(arg + 3, "both") ? 2 : 0;
		snprintf(l, sizeof l, "handshake answers: %s", s_wf_key_path == 1 ? "data ring" :
			 s_wf_key_path == 2 ? "both" : "management queue");
		fastboot_info(l);
	} else if (!strncmp(arg, "scrape", 6)) {
		extern volatile int g_dbg_nui_scrape;
		g_dbg_nui_scrape = strstr(arg, "stop") ? 2 : strstr(arg, ":lr") ? 3 : 1;	/* :lr from libretro's thumbnails */
	} else if (!strncmp(arg, "art", 3)) {
		extern volatile int g_dbg_nui_art;
		g_dbg_nui_art = 1;
	} else if (!strncmp(arg, "http", 4)) {
		unsigned int status, len;
		const unsigned char *body;
		const char *why = "";
		int r = netsvc_http_poll(&status, &body, &len, &why);
		snprintf(l, sizeof l, "http: %d status %u len %u %s", r, r == 1 ? status : 0, r == 1 ? len : 0, r < 0 ? why : "");
		fastboot_info(l);
		if (r == 1 || r == -1) netsvc_http_release();
	}
	netsvc_get(&st);
	pico_net_ip_text(st.ip, t);
	snprintf(l, sizeof l, "[%.8s] state %d ssid %s ip %s scans %u saved %u why %s", arg, st.state, st.ssid, t,
		 st.scan_gen, st.saved_gen, st.why);
	fastboot_info(l);
	if (!strncmp(arg, "aps", 3)) {
		static struct netsvc_ap a[WF_SCAN_MAX];
		int i, n = netsvc_aps(a, WF_SCAN_MAX);
		for (i = 0; i < n && i < 12; i++) {
			snprintf(l, sizeof l, "%4d dBm ch %3u %s%s%s %s", a[i].dbm, a[i].channel, a[i].secured ? "lock" : "open",
				 a[i].usable ? "" : " (no)", a[i].saved ? " saved" : "", a[i].ssid);
			fastboot_info(l);
		}
	}
	fastboot_okay("");
}

/* the fastboot commands drive the radio themselves: one at a time with the service */
#define WF_LOCKED(fn) static void fn##_locked(const char *a, void *d, unsigned z) \
	{ mutex_acquire(&s_wf_radio); fn(a, d, z); mutex_release(&s_wf_radio); }
WF_LOCKED(cmd_wifiup)
WF_LOCKED(cmd_wificap)
WF_LOCKED(cmd_wifiscan)
WF_LOCKED(cmd_wifijoin)
WF_LOCKED(cmd_wifidhcp)
WF_LOCKED(cmd_wifiping)
WF_LOCKED(cmd_wifidns)
WF_LOCKED(cmd_wifintp)
WF_LOCKED(cmd_wifiget)
WF_LOCKED(cmd_wifihttps)
WF_LOCKED(cmd_wifiurl)
WF_LOCKED(cmd_wifiart)

static void pico_wifi_register_cmds(void)
{
	ns_ready();
	fastboot_register("oem wifiurl", cmd_wifiurl_locked, 1, 0);
	fastboot_register("oem wificap", cmd_wificap_locked, 1, 0);
	fastboot_register("oem wifiscan", cmd_wifiscan_locked, 1, 0);
	fastboot_register("oem wifijoin", cmd_wifijoin_locked, 1, 0);
	fastboot_register("oem wifipass", cmd_wifipass, 1, 0);
	fastboot_register("oem wifidhcp", cmd_wifidhcp_locked, 1, 0);
	fastboot_register("oem wifiping", cmd_wifiping_locked, 1, 0);
	fastboot_register("oem wifidns", cmd_wifidns_locked, 1, 0);
	fastboot_register("oem wifintp", cmd_wifintp_locked, 1, 0);
	fastboot_register("oem wifiget", cmd_wifiget_locked, 1, 0);
	fastboot_register("oem wifihttps", cmd_wifihttps_locked, 1, 0);
	fastboot_register("oem wifiart", cmd_wifiart_locked, 1, 0);
	fastboot_register("oem wifiup", cmd_wifiup_locked, 1, 0);
	fastboot_register("oem netsvc", cmd_netsvc, 1, 0);
}
