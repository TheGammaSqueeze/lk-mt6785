/*
 * pico_netplay.c - two GammaOS Pico consoles playing one game together, in lockstep, over the
 * Bluetooth link (pico_bt.inc: pico_link_send / pico_link_recv carry whole messages, in order,
 * nothing lost).
 *
 * Both consoles run the same game from the same starting state. Every frame each one sends its
 * own pad and runs the frame only with both pads in hand, so both emulators see the same inputs
 * on the same frames and stay identical without sending any picture or sound. The host is player
 * one, the guest player two.
 *
 * A pad pressed on frame f is used on frame f + delay on both consoles: the delay hides the
 * link's travel time (about 7.5 ms one way at the 7.5 ms connection interval, under half a frame),
 * so with a delay of 1 or 2 frames the other console's pad is nearly always there in time and
 * neither console waits. Messages:
 *   'P' frame(4) count(1) pads(2 each)   the sender's pads for frames frame .. frame+count-1
 *                                  (the last few again in each message, so one late radio
 *                                  event costs no wait)
 *   'Q'                            the session ends
 * A pad's top bit (PICO_NP_RESET, the right stick's click, which no core reads) set in the
 * host's pad is a reset of the game: both consoles reset it before the frame that pad is for,
 * so they stay in step (pico_np_ask_reset).
 *   'H' frame(4) crc32(4)          the sender's whole emulator state, hashed before that frame
 *                                  (every 2 s): the two must match, else the games have parted
 * and before the first frame (pico_np_begin):
 *   'G' size(4)                    the guest is ready, its core's state size: the host's must be
 *                                  the same (the same core build), else 'X' and no session
 *   'S' offset(4) bytes...         the host's state, in order
 *   'T' size(4) crc32(4)           the state is whole: the guest loads it
 *   'R'                            ready (both send it; the first frame runs once both have it)
 * A runner calls pico_np_frame(local_pad) once per frame and gets back the pads for players one
 * and two for this frame. While waiting it sleeps, so the other threads keep the CPU.
 */
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <string.h>
#include <printf.h>
#include "pico_netplay.h"
#include "pico_link.h"

extern unsigned int gpt4_get_current_tick(void);
extern int pico_link_send(const void *p, unsigned int n);
extern int pico_link_recv(void *buf, unsigned int cap);
extern unsigned int pico_link_rx_tick(void);
extern void pico_cpu_netplay(int on);		/* the platform's CPU clock (pll.c, block_pico.c) */
extern int pico_link_state(void);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern void fastboot_register(const char *prefix, void (*handle)(const char *arg, void *data, unsigned sz),
			      unsigned allowed_when_security_on, unsigned forbidden_when_lock_on);

#define GPT_US(t)		((t) / 13u)
#define LINK_OPEN		4
#define NP_RING			64		/* frames of pads kept, each side */
#define NP_RESEND		4		/* pads repeated in each message */
#define NP_WAIT_MAX_US		3000000u	/* the other console silent this long: the session ends */
#define NP_HOLD_COOL		300u		/* frames between two holds (pico_np_frame) */
#define NP_TRIM_MAX		5000		/* the most the guest's frame period is trimmed, ppm */
#define NP_TRIM_I_MAX		4000		/* of which the steady clock difference, ppm */
#define NP_TRIM_STEP		40		/* the rate is rewritten when the trim moved this much */
volatile int g_np_trim_on = 1;		/* the guest trims its frame clock (oem nptrim:0 turns it off) */

struct np_session {
	int on, host;
	unsigned int delay;			/* frames between pressing and using */
	unsigned int frame;			/* the frame about to run */
	unsigned short mine[NP_RING], theirs[NP_RING];
	unsigned int arrived[NP_RING];		/* GPT tick each of their pads first arrived */
	unsigned int theirs_upto;		/* their pads known for frames below this */
	/* what it cost */
	unsigned int waits, wait_us_max, wait_us_sum, msgs_in, msgs_out;
	unsigned int waits_long;		/* over 8 ms: the frame likely missed its refresh */
	/* the state hashes: ours and theirs for the last few checked frames, and the verdicts */
	unsigned int my_hf[4], my_hc[4], th_hf[4], th_hc[4], same, differ, first_differ;
	/* a test pad ORed into this console's own (oem nppad) */
	unsigned int test_pad, test_frames;
	/* keeping the two frame clocks in step: how far ahead their pads arrive here and there
	 * (frames x16, averaged), the frame period, and the one-refresh holds taken */
	int lead16, their_lead16;
	unsigned int last_tick, period_us, holds, hold_cool;
	/* the guest follows the host's clock by trimming its frame period (their_by_time: the
	 * other console says it does) */
	int their_by_time, trim_i, trim_ppm;
	unsigned int per_min, per_max, per_long, per_n;	/* frame periods seen (oem npstat) */
	unsigned long long per_sum;
};
static void np_hash_compare(void);
static struct np_session s_np;
static int s_np_reset_ask;		/* the host's menu asked for a reset: it goes in the next pad */

static void np_put32(unsigned char *p, unsigned int v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned int np_get32(const unsigned char *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* take every message waiting; 0, or -1 when the other console ended the session */
static unsigned char s_np_msg[256];
static int np_take(void)
{
	unsigned char *m = s_np_msg;
	int n;
	while ((n = pico_link_recv(m, sizeof s_np_msg)) > 0) {
		s_np.msgs_in++;
		if (m[0] == 'Q')
			return -1;
		if (m[0] == 'H' && n >= 9) {
			unsigned int f = np_get32(m + 1), k = (f / 120u) % 4u;
			s_np.th_hf[k] = f;
			s_np.th_hc[k] = np_get32(m + 5);
			np_hash_compare();
			continue;
		}
		if (m[0] == 'P' && n >= 6) {
			unsigned int f = np_get32(m + 1), c = m[5], i, t = pico_link_rx_tick();
			if (6u + 2u * c > (unsigned)n)
				continue;			/* not whole: not ours */
			/* the pads continue what is known (messages come in order): anything new
			 * beyond the frames already held extends them */
			if (f > s_np.theirs_upto)
				continue;
			for (i = 0; i < c; i++) {
				unsigned int fi = f + i;
				if (fi < s_np.theirs_upto)
					continue;		/* had it */
				s_np.theirs[fi % NP_RING] = (unsigned short)(m[6 + 2 * i] | m[7 + 2 * i] << 8);
				s_np.arrived[fi % NP_RING] = t;
				s_np.theirs_upto = fi + 1u;
			}
			if ((unsigned)n >= 9u + 2u * c) {	/* how early our pads reach them, their pacing */
				s_np.their_lead16 = (short)(m[6 + 2 * c] | m[7 + 2 * c] << 8);
				s_np.their_by_time = m[8 + 2 * c] & 1;
			}
		}
	}
	return 0;
}

/* start a session on an open link; host 1 is player one. Both consoles call it before their
 * first frame, with the same delay. 0 started */
static int np_start(int host, unsigned int delay, int drain);
int pico_np_start(int host, unsigned int delay) { return np_start(host, delay, 1); }

static int np_start(int host, unsigned int delay, int drain)
{
	unsigned int i;
	if (pico_link_state() != LINK_OPEN || delay < 1 || delay > 8)
		return -1;
	memset(&s_np, 0, sizeof s_np);
	{ extern void ayaneo_pace_trim(int ppm); ayaneo_pace_trim(0); }
	s_np.host = host;
	s_np.delay = delay;
	/* the first delay frames run with no buttons on both consoles */
	for (i = 0; i < delay; i++)
		s_np.mine[i] = s_np.theirs[i] = 0;
	s_np.theirs_upto = delay;
	while (drain && pico_link_recv(s_np_msg, sizeof s_np_msg) > 0)
		;				/* nothing older belongs to this session */
	s_np.on = 1;
	pico_cpu_netplay(1);			/* the top clock for the whole session (user 11-16) */
	return 0;
}

void pico_np_stop(void)
{
	unsigned char q = 'Q';
	extern void ayaneo_pace_trim(int ppm);
	ayaneo_pace_trim(0);			/* the frame clock is its own again */
	if (!s_np.on)
		return;
	pico_cpu_netplay(0);			/* and the clock the game chose */
	if (pico_link_state() == LINK_OPEN)
		pico_link_send(&q, 1);
	s_np.on = 0;
}

int pico_np_active(void) { return s_np.on; }
int pico_np_is_host(void) { return s_np.on && s_np.host; }
void pico_np_ask_reset(void) { if (s_np.on && s_np.host) s_np_reset_ask = 1; }
int pico_np_take_reset(unsigned int *p1)
{
	int r = !!(*p1 & PICO_NP_RESET);
	*p1 &= ~PICO_NP_RESET;
	return r;
}

/* the other console went quiet or left: the session ends here, the frame clock is ours again */
static void np_lost(void)
{
	extern void ayaneo_pace_trim(int ppm);
	ayaneo_pace_trim(0);
	if (s_np.on)
		pico_cpu_netplay(0);
	s_np.on = 0;
}

/* one frame: this console's pad now in, the pads for players one and two for this frame out
 * (*p1, *p2). 0 to run the frame, -1 the session is over (the link or the other side ended) */
int pico_np_frame(unsigned int local, unsigned int *p1, unsigned int *p2)
{
	extern void ayaneo_pace_hold(void);
	extern void ayaneo_pace_trim(int ppm);
	unsigned char m[9 + 2 * NP_RESEND];
	int trims = !s_np.host && g_np_trim_on, ahead;	/* the guest follows the host's clock */
	unsigned int f = s_np.frame, use = f + s_np.delay, first, c, i, t0, w, now;
	int sample;

	if (!s_np.on)
		return -1;
	/* Each console shows frames at its own frame clock, and two clocks never match exactly
	 * (the two panels' PAL rates measured 0.3 % apart): the faster one drifts ahead until it
	 * waits for the other's pads every frame. How far ahead this console is shows as their
	 * pads reaching us later (relative) than ours reach them: ahead, in 16ths of a frame, is
	 * that difference (0 when both are in step).
	 * The guest follows the host's clock smoothly: its frame period (the panel's data rate, or
	 * the deadline of a time paced core) is trimmed by a few parts per thousand at most,
	 * proportional to how far ahead it is plus what that has added up to (the clocks' steady
	 * difference), the rate rewritten only when the trim moved.
	 * The safety net, and the way when the other console does not trim: a console more than a
	 * frame ahead keeps its next present back one frame, which moves a whole frame of slack
	 * from the other console to this one (so the other never sees a reason to hold in turn). */
	ahead = s_np.their_lead16 - s_np.lead16;
	if (f > 120u && trims) {
		int ppm;
		s_np.trim_i += ahead * 2;
		if (s_np.trim_i > NP_TRIM_I_MAX) s_np.trim_i = NP_TRIM_I_MAX;
		if (s_np.trim_i < -NP_TRIM_I_MAX) s_np.trim_i = -NP_TRIM_I_MAX;
		ppm = ahead * 250 + s_np.trim_i;
		if (ppm > NP_TRIM_MAX) ppm = NP_TRIM_MAX;
		if (ppm < -NP_TRIM_MAX) ppm = -NP_TRIM_MAX;
		if (ppm - s_np.trim_ppm > NP_TRIM_STEP || s_np.trim_ppm - ppm > NP_TRIM_STEP) {
			s_np.trim_ppm = ppm;
			ayaneo_pace_trim(ppm);
		}
	}
	if (s_np.hold_cool)
		s_np.hold_cool--;
	else if (f > 120u && ahead > (trims || s_np.their_by_time ? 48 : 16)) {
		ayaneo_pace_hold();		/* this frame's present waits one frame more */
		s_np.holds++;
		s_np.hold_cool = NP_HOLD_COOL;
	}
	if (s_np.test_frames) {			/* oem nppad */
		local |= s_np.test_pad;
		s_np.test_frames--;
	}
	local &= ~PICO_NP_RESET;		/* only the host's own ask sets it */
	if (s_np_reset_ask && s_np.host) local |= PICO_NP_RESET;
	s_np_reset_ask = 0;
	/* this console's pad, for the frame delay frames ahead, with the last few again */
	s_np.mine[use % NP_RING] = (unsigned short)local;
	first = use + 1u > NP_RESEND ? use + 1u - NP_RESEND : 0u;
	if (first < s_np.delay) first = s_np.delay;
	c = use - first + 1u;
	m[0] = 'P';
	np_put32(m + 1, first);
	m[5] = (unsigned char)c;
	for (i = 0; i < c; i++) {
		unsigned int v = s_np.mine[(first + i) % NP_RING];
		m[6 + 2 * i] = (unsigned char)v;
		m[7 + 2 * i] = (unsigned char)(v >> 8);
	}
	m[6 + 2 * c] = (unsigned char)s_np.lead16;
	m[7 + 2 * c] = (unsigned char)((unsigned)s_np.lead16 >> 8);
	m[8 + 2 * c] = (unsigned char)(trims ? 1 : 0);	/* this console follows the other's clock */
	if (pico_link_send(m, 9 + 2 * c) == 0)
		s_np.msgs_out++;
	/* their pad for this frame: usually here already */
	t0 = gpt4_get_current_tick();
	if (np_take() < 0) { np_lost(); return -1; }
	for (;;) {
		if (np_take() < 0) { np_lost(); return -1; }
		if (s_np.theirs_upto > f)
			break;
		w = GPT_US(gpt4_get_current_tick() - t0);
		if (w > NP_WAIT_MAX_US || pico_link_state() != LINK_OPEN) { np_lost(); return -1; }
		thread_sleep(1);
	}
	now = gpt4_get_current_tick();
	w = GPT_US(now - t0);
	/* the frame period, from one call to the next (a hold is not a period) */
	if (s_np.last_tick) {
		unsigned int d = GPT_US(t0 - s_np.last_tick);
		if (!s_np.per_min || d < s_np.per_min) s_np.per_min = d;
		if (d > s_np.per_max) s_np.per_max = d;
		if (d > 30000u) s_np.per_long++;
		s_np.per_sum += d; s_np.per_n++;
		if (d > 8000u && d < 40000u)
			s_np.period_us = s_np.period_us ? (s_np.period_us * 15u + d) / 16u : d;
	}
	s_np.last_tick = s_np.hold_cool == NP_HOLD_COOL ? 0u : t0;	/* the held frame is no period */
	/* how long before this frame needed it their pad arrived (after: negative), in 16ths of a
	 * frame; the first frames' pads were never sent */
	if (f >= s_np.delay && s_np.period_us) {
		sample = (int)(t0 - s_np.arrived[f % NP_RING]) / 13;
		sample = sample * 16 / (int)s_np.period_us;
		s_np.lead16 += (sample - s_np.lead16) / 16;
	}
	if (w > 8000u)
		s_np.waits_long++;
	if (w > 1000u) {
		s_np.waits++;
		s_np.wait_us_sum += w;
		if (w > s_np.wait_us_max) s_np.wait_us_max = w;
	}
	if (s_np.host) { *p1 = s_np.mine[f % NP_RING]; *p2 = s_np.theirs[f % NP_RING]; }
	else           { *p1 = s_np.theirs[f % NP_RING]; *p2 = s_np.mine[f % NP_RING]; }
	s_np.frame = f + 1u;
	return 0;
}



/* ---- keeping watch that both games stay the same ----
 * The runner hashes its whole emulator state every 120 frames, before the frame runs, and hands
 * the hash here: it goes to the other console and is compared with its hash of the same frame. */
static unsigned int s_np_crc_tab[256];
unsigned int pico_np_crc(const void *p, unsigned int n)
{
	const unsigned char *b = (const unsigned char *)p;
	unsigned int c = 0xffffffffu, i, k;
	if (!s_np_crc_tab[1])
		for (i = 0; i < 256; i++) {
			for (c = i, k = 0; k < 8; k++)
				c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			s_np_crc_tab[i] = c;
		}
	c = 0xffffffffu;
	while (n--)
		c = s_np_crc_tab[(c ^ *b++) & 0xffu] ^ (c >> 8);
	return ~c;
}

/* the frame about to run, so a runner knows when a hash is due (every 120 frames) */
unsigned int pico_np_frame_no(void) { return s_np.frame; }

static void np_hash_compare(void)
{
	unsigned int k;
	for (k = 0; k < 4; k++) {
		if (!s_np.my_hf[k] || s_np.my_hf[k] != s_np.th_hf[k])
			continue;
		if (s_np.my_hc[k] == s_np.th_hc[k]) s_np.same++;
		else {
			if (!s_np.differ) s_np.first_differ = s_np.my_hf[k];
			s_np.differ++;
		}
		s_np.my_hf[k] = s_np.th_hf[k] = 0;	/* compared once */
	}
}

/* this console's state hash for the frame about to run */
void pico_np_hash(unsigned int frame, unsigned int crc)
{
	unsigned char m[9];
	unsigned int k = (frame / 120u) % 4u;
	if (!s_np.on)
		return;
	s_np.my_hf[k] = frame;
	s_np.my_hc[k] = crc;
	m[0] = 'H';
	np_put32(m + 1, frame);
	np_put32(m + 5, crc);
	pico_link_send(m, 9);
	np_hash_compare();
}

/* ---- arming and the start of a session ----
 * A Pico screen (or oem npnes) arms the next game launch as a netplay session on an open link;
 * the game's runner asks pico_np_armed() after loading the game, and calls pico_np_begin() with
 * a buffer for the core's state: the host fills it (the runner saved it), the guest receives it
 * and loads it, so both consoles start from the same state. */
static int s_np_armed_host = -1;	/* -1 not armed, 0 guest, 1 host */
static unsigned int s_np_armed_delay = 2;

void pico_np_arm(int host, unsigned int delay)
{
	s_np_armed_host = host;
	s_np_armed_delay = delay;
}
/* whether the next launch is armed, without using the arming up */
int pico_np_is_armed(void) { return s_np_armed_host >= 0; }

/* -1 when the next launch is not a netplay session, else 1 host / 0 guest; the arming is used up */
int pico_np_armed(unsigned int *delay)
{
	int h = s_np_armed_host;
	s_np_armed_host = -1;
	if (h >= 0 && pico_link_state() != LINK_OPEN)
		return -1;
	if (delay) *delay = s_np_armed_delay;
	return h;
}

static unsigned int np_crc32(const unsigned char *p, unsigned int n)
{
	unsigned int c = 0xffffffffu, k;
	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
	}
	return ~c;
}

/* a message out, waiting while the link's queue is full (5 s at most); 0 sent */
static int np_send(const unsigned char *m, unsigned int n)
{
	unsigned int t0 = gpt4_get_current_tick();
	int rc;
	while ((rc = pico_link_send(m, n)) == -2) {
		if (GPT_US(gpt4_get_current_tick() - t0) > 5000000u)
			return -2;
		thread_sleep(1);
	}
	return rc;
}

/* wait for one message of kind k (others dropped: they are from before), up to us; its
 * length, 0 on none */
/* ---- the start's progress, for the screen a theme shows meanwhile (pico_link_ui.c) ----
 * Each step of a start says what it does (PICO_NP_*, pico_netplay.h) and how far it is; the
 * function a theme set is called with it at most every 100 ms and may cancel (1): the start then
 * ends (the other console is told with 'X'). */
static volatile int s_np_phase;
static volatile unsigned int s_np_done, s_np_total, s_np_phase_tick;
static int (*s_np_prog_fn)(void);
static unsigned int s_np_prog_tick;
static int s_np_cancel, s_np_other_gone;	/* this console cancelled; the other did ('X') */

void pico_np_set_progress(int (*fn)(void)) { s_np_prog_fn = fn; }

void pico_np_progress(int *phase, unsigned int *done, unsigned int *total, unsigned int *kbs)
{
	unsigned int ms = GPT_US(gpt4_get_current_tick() - s_np_phase_tick) / 1000u;
	*phase = s_np_phase;
	*done = s_np_done;
	*total = s_np_total;
	*kbs = ms > 300u ? (unsigned int)((unsigned long long)s_np_done / ms) : 0u;	/* bytes per ms = KB/s */
}

/* 1 when the start is to end (cancelled) */
static int np_tick(int force)
{
	unsigned int now = gpt4_get_current_tick();
	if (!s_np_prog_fn || s_np_cancel)
		return s_np_cancel;
	if (!force && GPT_US(now - s_np_prog_tick) < 100000u)
		return 0;
	s_np_prog_tick = now;
	if (s_np_prog_fn()) {
		unsigned char x = 'X';
		s_np_cancel = 1;
		np_send(&x, 1);
	}
	return s_np_cancel;
}

static void np_phase(int phase, unsigned int total)
{
	s_np_phase = phase;
	s_np_done = 0;
	s_np_total = total;
	s_np_phase_tick = gpt4_get_current_tick();
	np_tick(1);
}

static int np_wait_kind(unsigned char k, unsigned int us)
{
	unsigned int t0 = gpt4_get_current_tick();
	int n;
	for (;;) {
		n = pico_link_recv(s_np_msg, sizeof s_np_msg);
		if (n > 0 && s_np_msg[0] == k)
			return n;
		if (n > 0 && s_np_msg[0] == 'X') {
			s_np_other_gone = 1;		/* the other console gave up */
			return 0;
		}
		if (pico_link_state() != LINK_OPEN || GPT_US(gpt4_get_current_tick() - t0) > us || np_tick(0))
			return 0;
		if (n <= 0)
			thread_sleep(1);
	}
}

volatile int g_np_begin_rc;			/* oem npstat: how the last start went */
volatile unsigned int g_np_begin_info;		/* the state's size (host) or bytes received (guest) */

/* A state or a cartridge goes in 240 byte pieces ('S' offset data, then 'T' size CRC-32). The
 * receiving console may be busy for a moment and its link queue holds only so many messages, so
 * the pieces go in a window: the receiver answers 'A' with the pieces it has every NP_ACK_EVERY,
 * and the sender keeps at most NP_WINDOW unanswered. */
#define NP_WINDOW		56u		/* below the link queue (64): never more in flight than it holds */
#define NP_ACK_EVERY		24u
/* Each console numbers what it sends and what it receives from the start of a session, in the
 * same order on both sides; an 'A' carries the number of the piece of data it answers, so a late
 * answer to the previous one (sent after its sender stopped waiting) is told apart. */
static unsigned int s_blob_out, s_blob_in;
static int np_blob_send(const unsigned char *b, unsigned int size)
{
	unsigned char m[256];
	unsigned int pos, sent = 0, acked = 0, seq = ++s_blob_out;
	/* a big one goes faster with this console as the link's central (pico_link_lead) */
	if (size >= 65536u) {
		extern int pico_link_lead(void);
		pico_link_lead();
	}
	for (pos = 0; pos < size; pos += 240u) {
		unsigned int c = size - pos > 240u ? 240u : size - pos;
		while (sent - acked >= NP_WINDOW) {	/* wait for the receiver to catch up */
			unsigned int k;
			if (!np_wait_kind('A', 5000000u))
				return -2;
			k = np_get32(s_np_msg + 5);
			if (np_get32(s_np_msg + 1) == seq && k > acked && k <= sent)
				acked = k;
		}
		m[0] = 'S';
		np_put32(m + 1, pos);
		memcpy(m + 5, b + pos, c);
		if (np_send(m, 5 + c) != 0)
			return -2;
		sent++;
		s_np_done = pos + c;
		if (np_tick(0))
			return -11;			/* cancelled */
	}
	m[0] = 'T';
	np_put32(m + 1, size);
	np_put32(m + 5, pico_np_crc(b, size));
	if (np_send(m, 9) != 0)
		return -2;
	/* the receiver says it has it all and checked it: what follows (the next state, a cartridge)
	 * must not pile up in its queue while it checks a large one */
	for (;;) {
		if (!np_wait_kind('A', 20000000u))
			return -2;
		if (np_get32(s_np_msg + 1) == seq && np_get32(s_np_msg + 5) == 0xFFFFFFFFu)
			return 0;
	}
}

/* receive one state into b (cap bytes); its size in *got. 0 whole and checked */
static int np_blob_recv(unsigned char *b, unsigned int cap, unsigned int *got)
{
	unsigned int have = 0, pos, pieces = 0, seq = ++s_blob_in;
	unsigned char a[9];
	int n;
	for (;;) {
		unsigned int t0 = gpt4_get_current_tick();
		while ((n = pico_link_recv(s_np_msg, sizeof s_np_msg)) <= 0) {
			if (pico_link_state() != LINK_OPEN || GPT_US(gpt4_get_current_tick() - t0) > 20000000u)
				return -3;
			if (np_tick(0))
				return -11;		/* cancelled */
			thread_sleep(1);
		}
		if (s_np_msg[0] == 'X') {
			s_np_other_gone = 1;
			return -8;
		}
		if (s_np_msg[0] == 'S' && n > 5) {
			pos = np_get32(s_np_msg + 1);
			if (pos != have || have + (unsigned)(n - 5) > cap)
				return -4;
			memcpy(b + have, s_np_msg + 5, (unsigned)(n - 5));
			have += (unsigned)(n - 5);
			g_np_begin_info = have;
			s_np_done = have;
			if (++pieces % NP_ACK_EVERY == 0) {
				a[0] = 'A';
				np_put32(a + 1, seq);
				np_put32(a + 5, pieces);
				if (np_send(a, 9) != 0)
					return -2;
			}
		} else if (s_np_msg[0] == 'T' && n >= 9) {
			if (np_get32(s_np_msg + 1) != have || pico_np_crc(b, have) != np_get32(s_np_msg + 5))
				return -5;
			*got = have;
			a[0] = 'A';			/* all here: the end, for the sender */
			np_put32(a + 1, seq);
			np_put32(a + 5, 0xFFFFFFFFu);
			return np_send(a, 9) ? -2 : 0;
		}
	}
}

/* Bring both consoles to the same state and the same first frame. host: state holds size bytes
 * to send; guest: state receives up to cap bytes, *got its size. 0 ready (the session started) */
static int np_begin(int host, unsigned int delay, void *state, unsigned int size, unsigned int cap,
		    unsigned int *got);
int pico_np_begin(int host, unsigned int delay, void *state, unsigned int size, unsigned int cap,
		  unsigned int *got)
{
	g_np_begin_info = size;
	g_np_begin_rc = np_begin(host, delay, state, size, cap, got);
	if (g_np_begin_rc && s_np_cancel) {
		g_np_begin_rc = -11;					/* cancelled here: */
		pico_link_flush(500u);		/* the 'X' reaches the other console before Bluetooth goes */
	}
	else if (g_np_begin_rc && s_np_other_gone) g_np_begin_rc = -8;	/* cancelled there */
	s_np_phase = PICO_NP_IDLE;
	return g_np_begin_rc;
}

static int np_begin(int host, unsigned int delay, void *state, unsigned int size, unsigned int cap,
		    unsigned int *got)
{
	unsigned char *b = (unsigned char *)state, m[16];
	int rc;

	if (pico_link_state() != LINK_OPEN)
		return -1;
	s_blob_out = s_blob_in = 0;
	s_np_cancel = s_np_other_gone = 0;
	np_phase(PICO_NP_WAITING, 0);
	/* Nothing is thrown away here: the two consoles reach this point at different times (one
	 * card is slower), and what the first one sends must still be there for the second. The
	 * guest says when it is ready ('G'); the host sends its state only then. */
	if (host) {
		if (!np_wait_kind('G', 30000000u))
			return -7;			/* the guest never came */
		if (np_get32(s_np_msg + 1) != size) {	/* another core build: the games would part */
			g_np_begin_info = np_get32(s_np_msg + 1);
			m[0] = 'X';
			np_send(m, 1);
			return -8;
		}
		np_phase(PICO_NP_SEND_STATE, size);
		if ((rc = np_blob_send(b, size)) != 0)
			return rc;
	} else {
		unsigned int have = 0;
		m[0] = 'G';
		np_put32(m + 1, size);			/* our core's state size: the host checks it */
		if (np_send(m, 5) != 0)
			return -2;
		np_phase(PICO_NP_RECV_STATE, size);
		if ((rc = np_blob_recv(b, cap, &have)) != 0)
			return rc;
		if (got) *got = have;
	}
	/* both ready: each says so and waits for the other */
	np_phase(PICO_NP_STARTING, 0);
	m[0] = 'R';
	if (np_send(m, 1) != 0)
		return -2;
	if (!np_wait_kind('R', 30000000u))
		return -6;
	/* not emptied: the other console may have run its first frame and sent its pads already */
	return np_start(host, delay, 0);
}

/* ---- the start of a linked session (GB/GBC/GBA link cable) ----
 * Both consoles run both machines, so each needs the other's machine: each sends its own state
 * (its cartridge save in it) and receives the other's. When the two play different games (Pokemon
 * Red and Green) each console also needs the other's cartridge, as a link cable needs nothing but
 * one cartridge in each console: a console takes it from its own card when it has the same game
 * there, and otherwise the other console sends it, into memory for this session only (never
 * written to the card). The guest's 'G' carries its state size, its cartridge's CRC-32, size and
 * load flags; the host answers 'Y' with whether the games differ, whether it has the guest's
 * (ROM_HAVE, ROM_SEND: send it, ROM_NO_ROOM: it cannot take one that big), and its own flags; the
 * guest answers 'Z' the same about the host's. A ROM_NO_ROOM stops both (-12). Then the guest's
 * cartridge goes to the host if wanted, the host's to the guest if wanted, the guest's state to
 * the host, the host's state to the guest, and the host's clock ('W': the seconds the
 * cartridges' clocks start from, so both see the same time); then both say ready. */
enum { ROM_SEND, ROM_HAVE, ROM_NO_ROOM };
static long s_np_clock0;		/* the session's clock start (the host's), seconds */
static long s_np_guest_clock0;		/* the guest's own clock when it said 'G' */
static long s_np_host_at_g;		/* the host's clock when that 'G' came in */

/* the name, size and CRC-32 of a cartridge, packed after off in a message: name length (1), name,
 * size (4), CRC (4); the bytes used */
static unsigned int np_put_rom(unsigned char *m, const char *name, unsigned int size, unsigned int crc)
{
	unsigned int n = 0;
	while (name && name[n] && n < 120u) n++;
	m[0] = (unsigned char)n;
	memcpy(m + 1, name, n);
	np_put32(m + 1 + n, size);
	np_put32(m + 5 + n, crc);
	return 9u + n;
}
static void np_get_rom(const unsigned char *m, char *name, unsigned int *size, unsigned int *crc)
{
	unsigned int n = m[0] < 120u ? m[0] : 120u;
	memcpy(name, m + 1, n);
	name[n] = 0;
	*size = np_get32(m + 1 + n);
	*crc = np_get32(m + 5 + n);
}

/* the other console's cartridge from this console's card, when it has the same game (name, size
 * and CRC-32 all agree): 1 found and in place */
static int np_rom_here(const struct pico_np_pair *pp, const char *name, unsigned int size, unsigned int crc)
{
	if (!pp->find_rom || !size || size > pp->rom_cap)
		return 0;
	if (pp->find_rom(name, size, crc, pp->their_rom, pp->rom_cap) != 0)
		return 0;
	*pp->their_rom_size = size;
	return 1;
}

/* where this console stands with the other's cartridge, when the games differ: on its card
 * (ROM_HAVE, in place), to be sent (ROM_SEND), or too big for the room it has (ROM_NO_ROOM) */
static int np_rom_need(const struct pico_np_pair *pp, const char *name, unsigned int size, unsigned int crc)
{
	if (np_rom_here(pp, name, size, crc))
		return ROM_HAVE;
	return size && pp->their_rom && size <= pp->rom_cap ? ROM_SEND : ROM_NO_ROOM;
}

/* the other console's cartridge over the link, into memory: checked against the size and CRC-32
 * it said it has */
static int np_rom_recv(const struct pico_np_pair *pp, unsigned int size, unsigned int crc)
{
	unsigned int have = 0;
	int rc;
	np_phase(PICO_NP_RECV_GAME, size);
	if ((rc = np_blob_recv((unsigned char *)pp->their_rom, pp->rom_cap, &have)) != 0)
		return rc;
	if (have != size || pico_np_crc(pp->their_rom, have) != crc)
		return -5;
	*pp->their_rom_size = have;
	return 0;
}
static int np_rom_send(const struct pico_np_pair *pp)
{
	np_phase(PICO_NP_SEND_GAME, pp->rom_size);
	return np_blob_send((const unsigned char *)pp->rom, pp->rom_size);
}

static int np_begin_pair(int host, unsigned int delay, const struct pico_np_pair *pp)
{
	extern long gba_host_time(void);
	unsigned char m[200];
	char their_name[124];
	unsigned int id = pico_np_crc(pp->rom, pp->rom_size), trade, their_size, their_crc, len;
	int rc, i_have, they_have;
	if (pico_link_state() != LINK_OPEN)
		return -1;
	s_blob_out = s_blob_in = 0;
	s_np_cancel = s_np_other_gone = 0;
	np_phase(PICO_NP_WAITING, 0);
	*pp->their_rom_size = 0;
	if (host) {
		if (!np_wait_kind('G', 30000000u))
			return -7;
		*pp->their_rom_flags = np_get32(s_np_msg + 5);
		s_np_guest_clock0 = (long)np_get32(s_np_msg + 9);
		s_np_host_at_g = gba_host_time();
		np_get_rom(s_np_msg + 13, their_name, &their_size, &their_crc);
		trade = their_crc != id || their_size != pp->rom_size;	/* different games */
		i_have = trade ? np_rom_need(pp, their_name, their_size, their_crc) : ROM_HAVE;
		m[0] = 'Y';
		np_put32(m + 1, trade);
		np_put32(m + 5, pp->rom_flags);
		m[9] = (unsigned char)i_have;
		len = 10u + np_put_rom(m + 10, pp->rom_name, pp->rom_size, id);
		if (np_send(m, len) != 0)
			return -2;
		if (!np_wait_kind('Z', 60000000u))	/* the guest looked on its card */
			return -7;
		they_have = trade ? s_np_msg[1] : ROM_HAVE;
		if (i_have == ROM_NO_ROOM || they_have == ROM_NO_ROOM)
			return -12;			/* a cartridge too big for the other's room */
		if (i_have == ROM_SEND && (rc = np_rom_recv(pp, their_size, their_crc)) != 0)
			return rc;
		if (they_have == ROM_SEND && (rc = np_rom_send(pp)) != 0)
			return rc;
		np_phase(PICO_NP_RECV_STATE, pp->msize);
		if ((rc = np_blob_recv((unsigned char *)pp->theirs, pp->cap, pp->got)) != 0)
			return rc;
		np_phase(PICO_NP_SEND_STATE, pp->msize);
		if ((rc = np_blob_send((const unsigned char *)pp->mine, pp->msize)) != 0)
			return rc;
		s_np_clock0 = gba_host_time();
		m[0] = 'W';
		np_put32(m + 1, (unsigned int)s_np_clock0);
		np_put32(m + 5, (unsigned int)s_np_host_at_g);
		if (np_send(m, 9) != 0)
			return -2;
	} else {
		m[0] = 'G';
		np_put32(m + 1, pp->msize);
		np_put32(m + 5, pp->rom_flags);
		s_np_guest_clock0 = gba_host_time();
		np_put32(m + 9, (unsigned int)s_np_guest_clock0);
		len = 13u + np_put_rom(m + 13, pp->rom_name, pp->rom_size, id);
		if (np_send(m, len) != 0)
			return -2;
		if (!np_wait_kind('Y', 30000000u))
			return -7;
		trade = np_get32(s_np_msg + 1);
		*pp->their_rom_flags = np_get32(s_np_msg + 5);
		they_have = s_np_msg[9];
		np_get_rom(s_np_msg + 10, their_name, &their_size, &their_crc);
		i_have = trade ? np_rom_need(pp, their_name, their_size, their_crc) : ROM_HAVE;
		if (!trade) they_have = ROM_HAVE;
		m[0] = 'Z';
		m[1] = (unsigned char)i_have;
		if (np_send(m, 2) != 0)
			return -2;
		if (i_have == ROM_NO_ROOM || they_have == ROM_NO_ROOM)
			return -12;			/* a cartridge too big for the other's room */
		if (they_have == ROM_SEND && (rc = np_rom_send(pp)) != 0)
			return rc;
		if (i_have == ROM_SEND && (rc = np_rom_recv(pp, their_size, their_crc)) != 0)
			return rc;
		np_phase(PICO_NP_SEND_STATE, pp->msize);
		if ((rc = np_blob_send((const unsigned char *)pp->mine, pp->msize)) != 0)
			return rc;
		np_phase(PICO_NP_RECV_STATE, pp->msize);
		if ((rc = np_blob_recv((unsigned char *)pp->theirs, pp->cap, pp->got)) != 0)
			return rc;
		if (!np_wait_kind('W', 20000000u))
			return -3;
		s_np_clock0 = (long)np_get32(s_np_msg + 1);
		s_np_host_at_g = (long)np_get32(s_np_msg + 5);
	}
	np_phase(PICO_NP_STARTING, 0);
	m[0] = 'R';
	if (np_send(m, 1) != 0)
		return -2;
	if (!np_wait_kind('R', 30000000u))
		return -6;
	return np_start(host, delay, 0);
}

int pico_np_begin_pair(int host, unsigned int delay, const struct pico_np_pair *pp)
{
	g_np_begin_info = pp->msize;
	g_np_begin_rc = np_begin_pair(host, delay, pp);
	if (g_np_begin_rc && s_np_cancel) {
		g_np_begin_rc = -11;					/* cancelled here: */
		pico_link_flush(500u);		/* the 'X' reaches the other console before Bluetooth goes */
	}
	else if (g_np_begin_rc && s_np_other_gone) g_np_begin_rc = -8;	/* cancelled there */
	s_np_phase = PICO_NP_IDLE;
	return g_np_begin_rc;
}

/* The cartridges' clock while a session runs: the host's time at the start plus the frames run
 * (at the Game Boy's 59.7275 a second), the same on both consoles; 0 when no session runs. */
/* how far the guest's clock was ahead of the host's at the start (seconds, the same number on
 * both consoles): the guest's machine runs its cartridge clock that much from the session's */
long pico_np_clock_gap(void) { return s_np_guest_clock0 - s_np_host_at_g; }

long pico_np_clock(void)
{
	if (!s_np.on)
		return 0;
	return s_np_clock0 + (long)((unsigned long long)s_np.frame * 10000u / 597275u);
}

/* this console's pads for the frame, as the runner's port reader hands them to the core */
static unsigned int s_np_pad[2];
void pico_np_set_pads(unsigned int p1, unsigned int p2) { s_np_pad[0] = p1; s_np_pad[1] = p2; }
unsigned int pico_np_pad(unsigned int port) { return port < 2 ? s_np_pad[port] : 0u; }

/* ---- the self test: oem btnp:h|g[:SECONDS[:DELAY]] ----
 * Lockstep at 60 frames a second for SECONDS (default 10) with a known pad pattern on each side:
 * every pad that comes in is checked against what the other side must have sent for that frame,
 * which proves the exchange keeps both consoles on the same inputs. */
static unsigned int np_test_pad(int host, unsigned int frame)
{
	unsigned int x = frame * 2654435761u ^ (host ? 0x1234u : 0xabcdu);
	return (x >> 7) & 0x0fffu;
}

static void cmd_btnp(const char *arg, void *data, unsigned sz)
{
	char line[128];
	unsigned int secs = 10, delay = 2, frames, f, bad = 0, t_next, p1, p2, late = 0;
	int host;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	host = *arg == 'h';
	if (*arg) arg++;
	if (*arg == ':') {
		secs = 0;
		for (arg++; *arg >= '0' && *arg <= '9'; arg++) secs = secs * 10 + (unsigned)(*arg - '0');
	}
	if (*arg == ':') {
		delay = 0;
		for (arg++; *arg >= '0' && *arg <= '9'; arg++) delay = delay * 10 + (unsigned)(*arg - '0');
	}
	if (!secs || secs > 60) secs = 10;
	if (pico_np_start(host, delay) != 0) {
		fastboot_fail("no open link (oem btsvc:p / :c first)");
		return;
	}
	frames = secs * 60u;
	t_next = gpt4_get_current_tick();
	for (f = 0; f < frames; f++) {
		unsigned int want_p1, want_p2;
		/* a frame every 16.67 ms, as a game's would come */
		while ((int)(gpt4_get_current_tick() - t_next) < 0)
			thread_sleep(1);
		if (GPT_US(gpt4_get_current_tick() - t_next) > 16667u) late++;
		t_next += 13u * 16667u;
		if (pico_np_frame(np_test_pad(host, f + delay), &p1, &p2) != 0)
			break;
		want_p1 = f < delay ? 0 : np_test_pad(1, f);
		want_p2 = f < delay ? 0 : np_test_pad(0, f);
		if (p1 != want_p1 || p2 != want_p2)
			bad++;
	}
	pico_np_stop();
	snprintf(line, sizeof line, "%s: %u/%u frames, %u wrong pads, delay %u", host ? "host" : "guest",
		 f, frames, bad, delay);
	fastboot_info(line);
	snprintf(line, sizeof line, "waits %u (max %u us, avg %u us), late frames %u", s_np.waits,
		 s_np.wait_us_max, s_np.waits ? s_np.wait_us_sum / s_np.waits : 0, late);
	fastboot_info(line);
	snprintf(line, sizeof line, "messages out %u in %u", s_np.msgs_out, s_np.msgs_in);
	fastboot_info(line);
	fastboot_okay("");
}

/* oem npnes:h|g[:DELAY] - the next game launch is a netplay session (host or guest) */
/* oem npparts: the last eight checks in parts (gbc_sd_run.c): frame, the cable, machine 0, machine 1 */
volatile unsigned int g_np_parts[8][4];
static void cmd_npparts(const char *arg, void *data, unsigned sz)
{
	char line[64];
	unsigned int i;
	(void)arg; (void)data; (void)sz;
	for (i = 0; i < 8; i++) {
		snprintf(line, sizeof line, "%u %08x %08x %08x", g_np_parts[i][0], g_np_parts[i][1], g_np_parts[i][2], g_np_parts[i][3]);
		fastboot_info(line);
	}
	fastboot_okay("");
}

static void cmd_nparm(const char *arg, void *data, unsigned sz)
{
	char line[64];
	unsigned int delay = 2;
	int host;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	host = *arg == 'h';
	if (arg[0] && arg[1] == ':') delay = (unsigned)(arg[2] - '0');
	if (delay < 1 || delay > 8) delay = 2;
	if (pico_link_state() != LINK_OPEN) { fastboot_fail("no open link"); return; }
	pico_np_arm(host, delay);
	snprintf(line, sizeof line, "next launch: netplay %s, delay %u", host ? "host" : "guest", delay);
	fastboot_info(line);
	fastboot_okay("");
}

/* two numbers a runner reports for oem npstat (the SNES core's arena use, now and at its peak) */
static volatile unsigned int s_np_note[2];
void pico_np_note(unsigned int now, unsigned int peak) { s_np_note[0] = now; s_np_note[1] = peak; }
/* the link cable's traffic (GB/GBC): bytes each machine clocked out */
static volatile unsigned int s_np_cable[2], s_np_cable_dbg[4];
void pico_np_cable_note(unsigned int a, unsigned int b) { s_np_cable[0] = a; s_np_cable[1] = b; }
/* what the linked machines saw: the pads ports 0 and 1 read last, the machine watched, selected */
void pico_np_cable_dbg(unsigned int p0, unsigned int p1, unsigned int view, unsigned int sel)
{
	s_np_cable_dbg[0] = p0; s_np_cable_dbg[1] = p1; s_np_cable_dbg[2] = view; s_np_cable_dbg[3] = sel;
}

/* oem npstat - the session's state and what it cost */
/* oem npdump:0|1 - the runner writes the next hashed state to the card (/states/gba/np_dump.bin), to
 * compare the two consoles' states byte by byte when their hashes part */
volatile int g_np_dump;
static void cmd_npdump(const char *arg, void *data, unsigned sz)
{
	(void)data; (void)sz;
	if (arg && *arg == ':') arg++;
	if (arg && (*arg == '0' || *arg == '1'))
		g_np_dump = *arg - '0';
	fastboot_okay(g_np_dump ? "dump on" : "dump off");
}

/* oem nptrim:0|1 - whether the guest trims its frame clock to follow the host's */
static void cmd_nptrim(const char *arg, void *data, unsigned sz)
{
	(void)data; (void)sz;
	if (arg && (*arg == ':')) arg++;
	if (arg && (*arg == '0' || *arg == '1'))
		g_np_trim_on = *arg - '0';
	fastboot_okay(g_np_trim_on ? "trim on" : "trim off");
}

static void cmd_npstat(const char *arg, void *data, unsigned sz)
{
	char line[128];
	(void)arg; (void)data; (void)sz;
	snprintf(line, sizeof line, "netplay %s %s frame %u delay %u", s_np.on ? "on" : "off",
		 s_np.host ? "host" : "guest", s_np.frame, s_np.delay);
	fastboot_info(line);
	snprintf(line, sizeof line, "waits %u (%u over 8 ms, avg %u us, max %u us)", s_np.waits,
		 s_np.waits_long, s_np.waits ? s_np.wait_us_sum / s_np.waits : 0, s_np.wait_us_max);
	fastboot_info(line);
	snprintf(line, sizeof line, "holds %u, lead x16 ours %d theirs %d", s_np.holds, s_np.lead16,
		 s_np.their_lead16);
	fastboot_info(line);
	snprintf(line, sizeof line, "period %u us (mean %u, %u..%u, %u long)", s_np.period_us,
		 s_np.per_n ? (unsigned)(s_np.per_sum / s_np.per_n) : 0u, s_np.per_min, s_np.per_max,
		 s_np.per_long);
	fastboot_info(line);
	snprintf(line, sizeof line, "trim %d ppm (steady part %d)", s_np.trim_ppm, s_np.trim_i);
	fastboot_info(line);
	snprintf(line, sizeof line, "msgs out %u in %u", s_np.msgs_out, s_np.msgs_in);
	fastboot_info(line);
	snprintf(line, sizeof line, "last start rc %d, state %u bytes, link %d", g_np_begin_rc,
		 g_np_begin_info, pico_link_state());
	fastboot_info(line);
	snprintf(line, sizeof line, "state hashes: %u same, %u differ (first at frame %u)", s_np.same,
		 s_np.differ, s_np.first_differ);
	fastboot_info(line);
	{	/* the GBA link cable inside the pair, while a session has it */
		extern unsigned gba_np_stat(unsigned what);
		if (gba_np_stat(4)) {
			snprintf(line, sizeof line, "gba cable: clocked %u/%u, delivered %u/%u",
				 gba_np_stat(0), gba_np_stat(1), gba_np_stat(2), gba_np_stat(3));
			fastboot_info(line);
		}
	}
	if (s_np_cable[0] || s_np_cable[1]) {
		snprintf(line, sizeof line, "link cable: machine 1 sent %u bytes, machine 2 %u", s_np_cable[0],
			 s_np_cable[1]);
		fastboot_info(line);
		snprintf(line, sizeof line, "pads read %02x %02x, watching %u, selected %u, now %02x %02x",
			 s_np_cable_dbg[0], s_np_cable_dbg[1], s_np_cable_dbg[2], s_np_cable_dbg[3],
			 s_np_pad[0], s_np_pad[1]);
		fastboot_info(line);
	}
	if (s_np_note[1]) {
		snprintf(line, sizeof line, "core arena %u KiB now, %u KiB peak", s_np_note[0] / 1024u,
			 s_np_note[1] / 1024u);
		fastboot_info(line);
	}
	fastboot_okay("");
}

/* oem nppad:HEX:FRAMES - a test pad (RETRO_DEVICE_ID_JOYPAD_* bits) ORed into this console's
 * own for FRAMES frames of the running session */
static void cmd_nppad(const char *arg, void *data, unsigned sz)
{
	unsigned int v = 0, n = 0;
	(void)data; (void)sz;
	while (*arg == ' ' || *arg == ':') arg++;
	for (; (*arg >= '0' && *arg <= '9') || (*arg >= 'a' && *arg <= 'f'); arg++)
		v = (v << 4) | (unsigned)(*arg <= '9' ? *arg - '0' : *arg - 'a' + 10);
	if (*arg == ':')
		for (arg++; *arg >= '0' && *arg <= '9'; arg++) n = n * 10 + (unsigned)(*arg - '0');
	s_np.test_pad = v;
	s_np.test_frames = n ? n : 60;
	fastboot_okay("");
}

void pico_np_register_cmds(void)
{
	fastboot_register("oem nppad", cmd_nppad, 1, 0);
	fastboot_register("oem btnp", cmd_btnp, 1, 0);
	fastboot_register("oem npnes", cmd_nparm, 1, 0);
	fastboot_register("oem npparts", cmd_npparts, 1, 0);
	fastboot_register("oem npstat", cmd_npstat, 1, 0);
	fastboot_register("oem nptrim", cmd_nptrim, 1, 0);
	fastboot_register("oem npdump", cmd_npdump, 1, 0);
}
