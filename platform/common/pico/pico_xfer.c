/*
 * pico_xfer.c - sending a file from one GammaOS Pico console's card to another's over the
 * Bluetooth link (pico_bt.inc: pico_link_send / pico_link_recv carry whole messages, in order).
 *
 * The exchange, each message's first byte its kind:
 *   sender                                receiver
 *   'O' size(4) crc32(4) path\0    ->
 *                                   <-    'A' (accepted)  or  'N' reason(1)
 *   'D' offset(4) bytes...         ->     (in order, up to XFER_CHUNK bytes each)
 *                                   <-    'K' bytes_held(4), every XFER_ACK_EVERY bytes
 *   'F'                            ->
 *                                   <-    'C' status(1): 0 written, 1 checksum wrong, 2 card
 *   'E'                            ->     nothing more to send (the receiver stops waiting)
 *   'Z' (either side, any time): cancelled
 * 'N' reasons: 0 turned down, 1 no room, 2 the card has a file of that name (never overwritten:
 * a friend's save must not replace this console's own).
 * The sender keeps at most XFER_WINDOW bytes the receiver has not confirmed, so the receiving
 * queue never overflows. The receiver gathers the file in the rewind arena's largest island
 * (hundreds of MB the menu never uses; the next game start sets rewind up afresh), checks its
 * CRC-32, then writes it to its card in one go (fat_wr_put), so a broken transfer never leaves
 * half a file behind. Transfers run from the menu, never while a game runs.
 *
 * One transfer at a time, on a worker thread of its own; callers watch pico_xfer_status().
 */
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <string.h>
#include <malloc.h>
#include <printf.h>
#include "../../../emu/gba/fat_ro.h"
#include "../../../emu/gba/fat_wr.h"
#include "pico_xfer.h"
#include "pico_link.h"

extern unsigned int gpt4_get_current_tick(void);
extern int gba_sd_mount(fat_vol *v);
extern unsigned int ayaneo_rewind_map(void);
extern unsigned int ayaneo_rewind_island(unsigned int i, unsigned int *size);
extern int pico_link_send(const void *p, unsigned int n);
extern int pico_link_recv(void *buf, unsigned int cap);
extern int pico_link_state(void);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern void fastboot_register(const char *prefix, void (*handle)(const char *arg, void *data, unsigned sz),
			      unsigned allowed_when_security_on, unsigned forbidden_when_lock_on);

#define GPT_US(t)		((t) / 13u)
#define LINK_OPEN		4		/* pico_link_state(): the link is up */
#define XFER_CHUNK		240		/* data bytes per 'D' message (247 less 1 + 4 header) */
#define XFER_ACK_EVERY		(16u * 1024u)
#define XFER_WINDOW		(32u * 1024u)
#define XFER_BUF_MAX		0x10000000u	/* 256 MB: far beyond any cartridge game */

static struct pico_xfer_status s_xs;
static volatile int s_xf_cancel;
static thread_t *s_xf_thread;
static char s_xf_path[XFER_PATH_MAX];
/* the sender's read buffer, on the heap while it sends: the KPA LK's static data must stay small */
#define XF_CHUNK_SZ 4096u
static unsigned char *s_chunk;

const struct pico_xfer_status *pico_xfer_status(void) { return &s_xs; }
int pico_xfer_busy(void) { return s_xf_thread != 0; }

/* ---- CRC-32 (the zip one), table made once ---- */
static unsigned int s_crc_tab[256];
static void crc_init(void)
{
	unsigned int i, k, c;
	if (s_crc_tab[1])
		return;
	for (i = 0; i < 256; i++) {
		for (c = i, k = 0; k < 8; k++)
			c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		s_crc_tab[i] = c;
	}
}
static unsigned int crc_more(unsigned int crc, const unsigned char *p, unsigned int n)
{
	crc = ~crc;
	while (n--)
		crc = s_crc_tab[(crc ^ *p++) & 0xffu] ^ (crc >> 8);
	return ~crc;
}

static void put32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned int get32(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* a message out, waiting while the link's queue is full; 0 sent */
static int xf_send(const unsigned char *m, unsigned int n)
{
	unsigned int t0 = gpt4_get_current_tick();
	int rc;
	while ((rc = pico_link_send(m, n)) == -2) {
		if (s_xf_cancel || GPT_US(gpt4_get_current_tick() - t0) > 5000000u)
			return -2;
		thread_sleep(1);
	}
	return rc;
}

/* the next message in, up to us microseconds; its length, 0 when none came */
static int xf_recv(unsigned char *m, unsigned int cap, unsigned int us)
{
	unsigned int t0 = gpt4_get_current_tick();
	int n;
	while (!(n = pico_link_recv(m, cap))) {
		if (s_xf_cancel || pico_link_state() != LINK_OPEN ||
		    GPT_US(gpt4_get_current_tick() - t0) > us)
			return 0;
		thread_sleep(1);
	}
	return n;
}

static void xf_fail(int err)
{
	unsigned char z = 'Z';
	if (pico_link_state() == LINK_OPEN && err != 7)
		pico_link_send(&z, 1);
	s_xs.error = err;
	s_xs.state = XF_FAILED;
}

/* ---- the sending side ---- */
static int xf_sender(void *arg)
{
	unsigned char m[256];
	fat_vol v;
	fat_file f;
	unsigned int crc = 0, pos, acked = 0, t0, plen;
	int n;
	(void)arg;

	crc_init();
	s_chunk = malloc(XF_CHUNK_SZ);
	if (!s_chunk) { xf_fail(XFE_READ); goto out; }
	{	/* the sender as the link's central: the peripheral sends at a third of the speed */
		extern int pico_link_lead(void);
		pico_link_lead();
	}
	if (gba_sd_mount(&v) != 0 || fat_open(&v, s_xf_path, &f) != 0 || !f.size) {
		xf_fail(1);				/* no such file */
		goto out;
	}
	s_xs.size = f.size;
	for (pos = 0; pos < f.size; ) {		/* the checksum first: the offer carries it */
		unsigned int k = f.size - pos > XF_CHUNK_SZ ? XF_CHUNK_SZ : f.size - pos;
		if (fat_read(&f, pos, s_chunk, k) != k) { xf_fail(2); goto out; }
		crc = crc_more(crc, s_chunk, k);
		pos += k;
	}
	plen = (unsigned)strlen(s_xf_path) + 1;
	m[0] = 'O';
	put32(m + 1, f.size);
	put32(m + 5, crc);
	memcpy(m + 9, s_xf_path, plen);
	s_xs.state = XF_WAITING;
	if (xf_send(m, 9 + plen) != 0) { xf_fail(3); goto out; }
	n = xf_recv(m, sizeof m, 60000000u);	/* the other side's answer: a person may decide */
	if (n >= 1 && m[0] == 'N') {		/* turned down: the link goes on (no 'Z') */
		s_xs.error = n >= 2 && m[1] == 2 ? XFE_THERE : XFE_REFUSED;
		s_xs.state = XF_FAILED;
		goto out;
	}
	if (n < 1 || m[0] != 'A') { xf_fail(XFE_GONE); goto out; }

	s_xs.state = XF_SENDING;
	t0 = gpt4_get_current_tick();
	for (pos = 0; pos < f.size; ) {
		unsigned int k, i;
		/* the window: wait for the receiver to confirm what it holds */
		while (pos - acked >= XFER_WINDOW) {
			n = xf_recv(m, sizeof m, 5000000u);
			if (n >= 5 && m[0] == 'K') acked = get32(m + 1);
			else if (n >= 1 && m[0] == 'Z') { xf_fail(7); goto out; }
			else if (n == 0) { xf_fail(6); goto out; }
		}
		k = f.size - pos > XF_CHUNK_SZ ? XF_CHUNK_SZ : f.size - pos;
		if (k > XFER_WINDOW - (pos - acked)) k = XFER_WINDOW - (pos - acked);
		if (fat_read(&f, pos, s_chunk, k) != k) { xf_fail(2); goto out; }
		for (i = 0; i < k; i += XFER_CHUNK) {
			unsigned int c = k - i > XFER_CHUNK ? XFER_CHUNK : k - i;
			m[0] = 'D';
			put32(m + 1, pos + i);
			memcpy(m + 5, s_chunk + i, c);
			if (xf_send(m, 5 + c) != 0) { xf_fail(3); goto out; }
		}
		pos += k;
		s_xs.done = pos;
		while ((n = pico_link_recv(m, sizeof m)) > 0) {	/* confirmations as they come */
			if (n >= 5 && m[0] == 'K') acked = get32(m + 1);
			else if (m[0] == 'Z') { xf_fail(7); goto out; }
		}
	}
	m[0] = 'F';
	if (xf_send(m, 1) != 0) { xf_fail(3); goto out; }
	for (;;) {				/* its check and its card write */
		n = xf_recv(m, sizeof m, 60000000u);
		if (n >= 2 && m[0] == 'C') break;
		if (n == 0 || m[0] == 'Z') { xf_fail(n ? 7 : 6); goto out; }
	}
	s_xs.ms = GPT_US(gpt4_get_current_tick() - t0) / 1000u;
	if (m[1] != 0) { xf_fail(10 + m[1]); goto out; }
	s_xs.state = XF_DONE;
out:
	free(s_chunk);
	s_chunk = 0;
	s_xf_thread = 0;
	return 0;
}

/* ---- the receiving side ---- */
static int xf_receiver(void *arg)
{
	unsigned char m[256], *buf;
	unsigned int size, crc, got = 0, last_k = 0, t0, cap = 0, pa;
	int n, i, accept = (int)(addr_t)arg;
	fat_vol v;
	char dir[XFER_PATH_MAX];
	const char *name, *slash = 0;

	crc_init();
	ayaneo_rewind_map();			/* the arena, mapped on first use */
	pa = ayaneo_rewind_island(0, &cap);
	buf = (unsigned char *)(addr_t)pa;
	if (cap > XFER_BUF_MAX)
		cap = XFER_BUF_MAX;
	s_xs.state = XF_WAITING;
	do {					/* an offer, or the end */
		n = xf_recv(m, sizeof m - 1, 600000000u);
		if (s_xf_cancel || pico_link_state() != LINK_OPEN) { xf_fail(5); goto out; }
		if (n >= 1 && m[0] == 'E') { s_xs.state = XF_END; goto out; }
	} while (n < 10 || m[0] != 'O');
	m[n] = 0;
	size = get32(m + 1);
	crc = get32(m + 5);
	strncpy(s_xs.path, (const char *)m + 9, sizeof s_xs.path - 1);
	s_xs.size = size;
	if (!accept || !pa || size == 0 || size > cap || s_xs.path[0] != '/') {
		m[0] = 'N';
		m[1] = size > cap ? 1 : 0;
		xf_send(m, 2);
		s_xs.error = XFE_REFUSED;
		s_xs.state = XF_FAILED;
		goto out;
	}
	{	/* a file of that name here already: kept as it is, never overwritten */
		fat_file f;
		if (gba_sd_mount(&v) == 0 && fat_open(&v, s_xs.path, &f) == 0) {
			m[0] = 'N';
			m[1] = 2;
			xf_send(m, 2);
			s_xs.error = XFE_THERE;
			s_xs.state = XF_FAILED;
			goto out;
		}
	}
	m[0] = 'A';
	if (xf_send(m, 1) != 0) { xf_fail(3); goto out; }

	s_xs.state = XF_RECEIVING;
	t0 = gpt4_get_current_tick();
	for (;;) {
		n = xf_recv(m, sizeof m, 10000000u);
		if (n == 0) { xf_fail(6); goto out; }
		if (m[0] == 'Z') { xf_fail(7); goto out; }
		if (m[0] == 'F') break;
		if (m[0] != 'D' || n < 6)
			continue;
		if (get32(m + 1) != got || got + (unsigned)(n - 5) > size) { xf_fail(8); goto out; }
		memcpy(buf + got, m + 5, (unsigned)(n - 5));
		got += (unsigned)(n - 5);
		s_xs.done = got;
		if (got - last_k >= XFER_ACK_EVERY || got == size) {
			m[0] = 'K';
			put32(m + 1, got);
			if (xf_send(m, 5) != 0) { xf_fail(3); goto out; }
			last_k = got;
		}
	}
	s_xs.ms = GPT_US(gpt4_get_current_tick() - t0) / 1000u;
	m[0] = 'C';
	if (got != size || crc_more(0, buf, size) != crc) {
		m[1] = 1;
		xf_send(m, 2);
		s_xs.error = 11;
		s_xs.state = XF_FAILED;
		goto out;
	}
	/* the card: the folder made as needed, the file written whole */
	s_xs.state = XF_WRITING;
	for (i = 0; s_xs.path[i]; i++)
		if (s_xs.path[i] == '/') slash = s_xs.path + i;
	name = slash + 1;
	if (slash == s_xs.path) { dir[0] = '/'; dir[1] = 0; }
	else {
		memcpy(dir, s_xs.path, (unsigned)(slash - s_xs.path));
		dir[slash - s_xs.path] = 0;
	}
	if (!*name || gba_sd_mount(&v) != 0 || !v.wr || fat_wr_mkpath(&v, dir) != 0 ||
	    fat_wr_put(&v, dir, name, buf, size) != 0) {
		m[1] = 2;
		xf_send(m, 2);
		s_xs.error = 12;
		s_xs.state = XF_FAILED;
		goto out;
	}
	m[1] = 0;
	xf_send(m, 2);
	s_xs.state = XF_DONE;
out:
	s_xf_thread = 0;
	return 0;
}

/* send the card's file at path to the other console; 0 started */
int pico_xfer_send(const char *path)
{
	if (s_xf_thread || pico_link_state() != LINK_OPEN || strlen(path) >= XFER_PATH_MAX)
		return -1;
	memset(&s_xs, 0, sizeof s_xs);
	strncpy(s_xf_path, path, sizeof s_xf_path - 1);
	strncpy(s_xs.path, path, sizeof s_xs.path - 1);
	s_xf_cancel = 0;
	s_xf_thread = thread_create("pico_xfer", xf_sender, 0, DEFAULT_PRIORITY, 16384);
	if (!s_xf_thread)
		return -2;
	thread_resume(s_xf_thread);
	return 0;
}

/* wait for the other console's offer; accept 1 takes it (0 turns it down) */
int pico_xfer_receive(int accept)
{
	if (s_xf_thread || pico_link_state() != LINK_OPEN)
		return -1;
	memset(&s_xs, 0, sizeof s_xs);
	s_xf_cancel = 0;
	s_xf_thread = thread_create("pico_xfer", xf_receiver, (void *)(addr_t)accept, DEFAULT_PRIORITY, 16384);
	if (!s_xf_thread)
		return -2;
	thread_resume(s_xf_thread);
	return 0;
}

void pico_xfer_cancel(void) { s_xf_cancel = 1; }

int pico_xfer_end(void)
{
	unsigned char e = 'E';
	if (s_xf_thread || pico_link_state() != LINK_OPEN)
		return -1;
	if (pico_link_send(&e, 1) != 0)
		return -1;
	pico_link_flush(500u);			/* out before the link goes */
	return 0;
}

/* oem btxfer[:s:PATH|:r|:x] - send a card file, wait to receive one, cancel; alone: progress */
static void cmd_btxfer(const char *arg, void *data, unsigned sz)
{
	static const char *const st[] = { "idle", "waiting", "sending", "receiving", "writing", "done", "failed", "end" };
	char line[96];
	int rc = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	if (arg[0] == 's' && arg[1] == ':') rc = pico_xfer_send(arg + 2);
	else if (arg[0] == 'r') rc = pico_xfer_receive(1);
	else if (arg[0] == 'x') pico_xfer_cancel();
	else if (arg[0] == 'e') rc = pico_xfer_end();
	snprintf(line, sizeof line, "xfer %s rc %d err %d: %u/%u bytes %u ms", st[s_xs.state % 8], rc,
		 s_xs.error, s_xs.done, s_xs.size, s_xs.ms);
	fastboot_info(line);
	if (s_xs.ms) {
		snprintf(line, sizeof line, "%u kB/s", (unsigned)((unsigned long long)s_xs.size / s_xs.ms));
		fastboot_info(line);
	}
	snprintf(line, sizeof line, "%.58s", s_xs.path);
	fastboot_info(line);
	fastboot_okay("");
}

void pico_xfer_register_cmds(void)
{
	fastboot_register("oem btxfer", cmd_btxfer, 1, 0);
}
