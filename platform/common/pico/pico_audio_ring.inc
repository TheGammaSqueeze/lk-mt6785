/*
 * GammaOS Pico audio, the ring (platform/common/pico): the looping 48 kHz stereo ring every core
 * streams into, the write cursor's lead over the hardware read pointer and the rate lean that
 * keeps it, and the sweep that silences what has been played. Included as text by each board's
 * audio file, which defines PICO_AUD_DMA_CUR() (the address the DMA reads now) and the cache clean.
 */
/* ================= streaming audio for the GBC emulator ================= */
/*
 * The emulator produces stereo audio at the Game Boy's native 2097152 Hz; we
 * box-resample it to 48 kHz and stream it into a looping AFE DL ring that plays
 * continuously. The run loop calls ayaneo_gbc_audio_submit() each frame to keep
 * the ring fed ahead of the DMA read pointer.
 */
#define GBC_RING_FRAMES		16384u		/* power of two, ~341 ms @48k */
#define GBC_SRC_HZ		2097152u
#define GBC_DST_HZ		48000u

static short s_gbc_ring[GBC_RING_FRAMES * 2] __attribute__((aligned(64)));
static volatile int s_gbc_audio_on;
static volatile int s_gbc_paused;			/* silence while set */
/* s_gbc_vol / the volume getters+setters live up top with the persisted settings */
static unsigned s_gbc_widx;			/* ring write cursor, in frames */
static long long s_rs_accl, s_rs_accr;		/* box-filter accumulators */
static unsigned s_rs_n, s_rs_phase;		/* samples accumulated, phase */

/* oem diag aud: the lowest lead of the write cursor over the ring's read pointer seen at a submit
 * since the last read (frames of 48 kHz, negative = the read pointer had overtaken it: what played
 * was old audio), and the submits that found it under ~21 ms */
volatile int g_dbg_aud_low = 99999;
volatile unsigned int g_dbg_aud_short;
volatile unsigned int g_dbg_aud_in;   /* oem diag: frames a game handed the GBA resampler, since boot */
unsigned int ayaneo_aud_written(void) { return s_gbc_widx; }   /* frames written to the ring (wraps at 2^32) */
static void aud_lead_note(void)
{
	unsigned cur = PICO_AUD_DMA_CUR(), base = (unsigned)(addr_t)s_gbc_ring;
	unsigned rd = (cur >= base) ? (cur - base) / 4u : 0u;
	int lead = (int)((s_gbc_widx - rd) & (GBC_RING_FRAMES - 1));
	if (lead >= (int)(GBC_RING_FRAMES / 2)) lead -= (int)GBC_RING_FRAMES;
	if (lead < g_dbg_aud_low) g_dbg_aud_low = lead;
	if (lead < (int)(GBC_RING_FRAMES / 16)) g_dbg_aud_short++;
}

/* Hold the write cursor about a quarter of the ring ahead of the read pointer. A frame a game did
 * not make in time (a card write, a slow menu frame) leaves the lead one frame of sound shorter,
 * and with a fixed rate nothing gave it back: a few of them and the cursor reached the read
 * pointer, which played old sound and then jumped (the stutter). So the rate leans, by at most half
 * a percent (a pitch change too small to hear), towards that lead, filling a lost frame back in a
 * few seconds; the jump stays for a real emergency only. Returns how far short of the lead the
 * cursor is (frames, at most a sixteenth of the ring either way); 0 after a jump. */
#define AUD_LEAN_SPAN ((int)(GBC_RING_FRAMES / 16))   /* this far off or more: the full half percent */
extern volatile int g_dbg_asnap_n;
static int aud_lean(void)
{
	unsigned cur = PICO_AUD_DMA_CUR(), base = (unsigned)(addr_t)s_gbc_ring;
	unsigned rd = (cur >= base) ? (cur - base) / 4u : 0u;
	int lead = (int)((s_gbc_widx - rd) & (GBC_RING_FRAMES - 1)), err;
	if (lead >= (int)(GBC_RING_FRAMES / 2)) lead -= (int)GBC_RING_FRAMES;
	if (lead < (int)(GBC_RING_FRAMES / 32) || lead > (int)((GBC_RING_FRAMES * 7) / 16)) {
		s_gbc_widx = (rd + GBC_RING_FRAMES / 4) & (GBC_RING_FRAMES - 1);
		g_dbg_asnap_n++;
		return 0;
	}
	err = (int)(GBC_RING_FRAMES / 4) - lead;   /* > 0: short of the lead */
	if (err > AUD_LEAN_SPAN) err = AUD_LEAN_SPAN;
	if (err < -AUD_LEAN_SPAN) err = -AUD_LEAN_SPAN;
	return err;
}

/* The ring plays round and round: when a game stops feeding it (a card write, a slow frame, a
 * pause the game thread took), the read pointer comes back round to sound it already played and
 * played it again, heard as a short loop. So what has been played is cleared a few milliseconds
 * later: a stall then plays silence, and the lead logic above picks up again when the game does.
 * The write cursor stays ahead of the read pointer, so nothing still to play is touched. */
static timer_t s_ring_sweep;
static unsigned s_swept;                       /* the ring is silent from here to the read pointer */
volatile unsigned int g_dbg_swept;             /* oem persist: frames cleared after playing, since boot */
static enum handler_return ring_sweep(struct timer *t, time_t now, void *arg)
{
	unsigned cur, base = (unsigned)(addr_t)s_gbc_ring, rd, n;
	(void)t; (void)now; (void)arg;
	if (!s_gbc_audio_on || s_gbc_paused) return INT_NO_RESCHEDULE;
	cur = PICO_AUD_DMA_CUR();
	rd = (cur >= base) ? ((cur - base) / 4u) & (GBC_RING_FRAMES - 1) : 0u;
	rd &= ~15u;                                /* whole cache lines (16 frames) */
	n = (rd - s_swept) & (GBC_RING_FRAMES - 1);
	if (n > GBC_RING_FRAMES / 2) { s_swept = rd; return INT_NO_RESCHEDULE; }   /* not ours to judge */
	while (n) {
		unsigned run = GBC_RING_FRAMES - s_swept < n ? GBC_RING_FRAMES - s_swept : n;
		memset(&s_gbc_ring[s_swept * 2], 0, run * 4u);
		arch_clean_cache_range((addr_t)&s_gbc_ring[s_swept * 2], run * 4u);
		s_swept = (s_swept + run) & (GBC_RING_FRAMES - 1);
		g_dbg_swept += run;
		n -= run;
	}
	return INT_NO_RESCHEDULE;
}
