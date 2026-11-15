/*
 * GammaOS Pico audio, the streams (platform/common/pico): init, pause and shutdown of the game
 * audio, and each core's submit (GB/GBC 2097152 Hz box resample, GBA and SNES linear resample to
 * the panel's rate, the menu's 48 kHz mix). Included as text by each board's audio file after the
 * ring, which defines PICO_AUD_HW_ON(pa, bytes) (power the path and loop the DMA over the ring),
 * audio_shutdown(), and the headphone jack's jack_detect_init / jack_poll / jack_teardown_for_android.
 */
/* is the boot chime still playing? (emulator waits for it before taking over) */
int ayaneo_boot_audio_active(void) { return s_audio_active; }

/*
 * Tear the streaming audio down to a clean, cold state (mute -> drain -> amp off
 * -> stop DMA -> park DAC), exactly like the boot chime does when it finishes.
 * Called before a save-state power-off: mt_power_off() is a soft off that keeps
 * the PMIC audio registers latched, so without this the amp/DAC were left
 * enabled and the NEXT boot's chime bring-up came up wrong (silent). A hard
 * reset cold-resets the block, which is why a forced reboot masked the bug.
 */
void ayaneo_gbc_audio_shutdown(void)
{
	if (!s_gbc_audio_on)
		return;
	s_gbc_audio_on = 0;
	timer_cancel(&s_ring_sweep);
	jack_teardown_for_android();	/* clean accdet + HP analog state before Android */
	audio_shutdown();
}

/*
 * Pause/resume the game audio (used for fast-forward). On pause we fill the ring
 * with silence so the looping DMA plays nothing; on resume we re-seat the write
 * cursor a fixed lead ahead of the hardware read pointer so latency stays sane.
 */
void ayaneo_gbc_audio_pause(int on)
{
	if (!s_gbc_audio_on)
		return;
	if (on) {
		s_gbc_paused = 1;
		memset(s_gbc_ring, 0, sizeof(s_gbc_ring));
		arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	} else {
		unsigned cur = PICO_AUD_DMA_CUR();
		unsigned base = (unsigned)(addr_t)s_gbc_ring;
		unsigned f = (cur >= base) ? (cur - base) / 4 : 0;
		s_gbc_widx = (f + GBC_RING_FRAMES / 4) & (GBC_RING_FRAMES - 1);
		s_gbc_paused = 0;
	}
}

void ayaneo_gbc_audio_init(void)
{
	if (s_gbc_audio_on)
		return;

	memset(s_gbc_ring, 0, sizeof(s_gbc_ring));
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	s_gbc_widx = GBC_RING_FRAMES / 4;	/* ~85 ms of lead over the DMA */
	s_rs_accl = s_rs_accr = 0;
	s_rs_n = 0;
	s_rs_phase = 0;

	/* full codec/AFE bring-up, memif looping over the ring */
	PICO_AUD_HW_ON((unsigned int)(addr_t)s_gbc_ring, sizeof(s_gbc_ring));
	s_swept = 0;
	s_gbc_audio_on = 1;
	jack_detect_init();		/* accdet up + pick the boot-time speaker/headphone route */
	timer_initialize(&s_ring_sweep);
	timer_set_periodic(&s_ring_sweep, 4, ring_sweep, 0);
}

/* Resample `count` stereo samples (each u32 = L|R<<16 at 2097152 Hz) to 48 kHz
 * and write them into the ring. Volume from AYANEO_AUDIO_VOLUME. */
void ayaneo_gbc_audio_submit(const unsigned int *samples, unsigned count)
{
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned i;

	unsigned thr;

	if (!s_gbc_audio_on || s_gbc_paused)
		return;
	jack_poll();			/* throttled headphone-jack detection + route switch */
	aud_lead_note();
	{	/* a sample out each time the phase passes thr: a little lower when short of the lead */
		int err = aud_lean();
		thr = (unsigned)((long long)GBC_SRC_HZ * (AUD_LEAN_SPAN * 200 - err) / (AUD_LEAN_SPAN * 200));
	}

	for (i = 0; i < count; i++) {
		int l = (short)(samples[i] & 0xffff);
		int r = (short)(samples[i] >> 16);

		s_rs_accl += l;
		s_rs_accr += r;
		s_rs_n++;
		s_rs_phase += GBC_DST_HZ;
		if (s_rs_phase >= thr) {
			int ol, or_;
			unsigned idx;

			s_rs_phase -= thr;
			ol  = (int)(s_rs_accl / (long long)s_rs_n);
			or_ = (int)(s_rs_accr / (long long)s_rs_n);
			s_rs_accl = s_rs_accr = 0;
			s_rs_n = 0;
			if (q < 256) { ol = (ol * (int)q) >> 8; or_ = (or_ * (int)q) >> 8; }
			idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
			s_gbc_ring[idx * 2 + 0] = (short)ol;
			s_gbc_ring[idx * 2 + 1] = (short)or_;
			s_gbc_widx++;
		}
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}

/* ================= streaming audio for the GBA (gpSP) emulator =================
 * gpSP renders s16 interleaved stereo at its sound_frequency (65536 Hz by
 * default); box-resample it to the same 48 kHz ring the GBC path uses. Reuses
 * the ring, the codec/AFE bring-up (ayaneo_gbc_audio_init), pause and shutdown -
 * only the input format + source rate differ, so only this submit is new. */
#define GBA_SRC_HZ	32768u   /* armGBA's own rate; the glue no longer doubles it */

static int s_ga_pl, s_ga_pr, s_ga_prime;   /* the last input sample, for the linear interpolation */
static unsigned s_ga_src_hz = GBA_SRC_HZ;  /* the running core's rate: armGBA 32768, gpSP 65536 */
static unsigned s_ga_phase;                /* 16.16 position within the current input step */
/*
 * The resampler output increment is carried in 1/GA_FRAC-sample units (a
 * fractional DDS). An INTEGER increment (nom 48000) can only step the output
 * rate in ~1 Hz jumps, so it can never sit exactly on the true panel-vs-AFE
 * ratio - the recovery loop then has to toggle between two integers, a slow
 * wow/flutter. With GA_FRAC=256 the feed-forward base lands within ~0.004 Hz of
 * the true rate, the loop barely moves, and the residual dither is inaudible.
 */
#define GA_FRAC		256
static int s_ga_inc = (int)(GBC_DST_HZ * GA_FRAC);	/* output rate, 1/256 units */
static int s_ga_inc_base = (int)(GBC_DST_HZ * GA_FRAC);	/* calibrated base; trim floats a hair */

int ayaneo_gba_audio_drc_rate(void) { return s_ga_inc / GA_FRAC; }

/*
 * Calibrate the resampler to the measured panel refresh. The emulator is
 * vsync-locked, so it produces audio at panel_fps * 1097.25 samples/s; we pick a
 * FIXED output increment so that resamples exactly to the 48 kHz AFE rate -> zero
 * long-term drift AND a constant rate (no dynamic pitch wobble).
 *   inc(1x) = 48000 * 59.7275 / panel_fps = 48000 * 5972.75 / panel_hz100.
 * ayaneo_gba_audio_set_rate_milli takes the panel rate in milli-Hz (Hz*1000) for
 * the ~0.002% precision the fractional base needs; the Hz*100 wrapper is kept for
 * the coarse boot-time seed. inc256 = 48000*5972.75*256*10 / hz1000
 *   = 733931520000 / hz1000. (65536/1097.25 = 59.7275 exactly.)
 */
/* the loaded core's own audio rate (armGBA 32768, gpSP 65536), so the one resampler fits both */
void ayaneo_gba_audio_set_src_hz(unsigned hz)
{
	if (hz >= 8000u && hz <= 96000u) s_ga_src_hz = hz;
}

void ayaneo_gba_audio_set_rate_milli(int hz1000)
{
	long long inc;
	if (hz1000 < 50000 || hz1000 > 70000)	/* sanity: 50-70 Hz */
		return;
	inc = 733931520000LL / (long long)hz1000;
	if (inc < (long long)44000 * GA_FRAC) inc = (long long)44000 * GA_FRAC;
	if (inc > (long long)52000 * GA_FRAC) inc = (long long)52000 * GA_FRAC;
	s_ga_inc = (int)inc;
	s_ga_inc_base = (int)inc;
}

void ayaneo_gba_audio_set_rate(int panel_hz100)
{
	if (panel_hz100 < 5000 || panel_hz100 > 7000)	/* sanity: 50-70 Hz */
		return;
	ayaneo_gba_audio_set_rate_milli(panel_hz100 * 10);
	return;
}

/* `frames` stereo frames, s16 interleaved [L,R,L,R,...] at GBA_SRC_HZ. */
/* Rewind reverse-audio capture (GBA). While active, ayaneo_gba_audio_submit diverts the core's
 * per-frame audio (s16 stereo at the core's rate) into s_gba_rw_cap[] instead of the AFE ring; the GBA
 * rewind path then reverses + decimates + resubmits it. gpSP drains its ring in <=256-frame chunks
 * per rendered frame, so the callback fires a few times - accumulate until capture_end reads it. */
#define GBA_RW_CAP_MAX 4096u
static volatile int s_gba_rw_capture;
static short s_gba_rw_cap[GBA_RW_CAP_MAX * 2];
static volatile unsigned s_gba_rw_cap_n;
void ayaneo_gba_rw_capture_begin(void) { s_gba_rw_cap_n = 0; s_gba_rw_capture = 1; }
unsigned ayaneo_gba_rw_capture_end(const short **buf) { s_gba_rw_capture = 0; if (buf) *buf = s_gba_rw_cap; return s_gba_rw_cap_n; }

extern unsigned gpt4_get_current_tick(void);
volatile int g_dbg_alead_min, g_dbg_asnap_n, g_dbg_asnap_lead; volatile unsigned g_dbg_asnap_ms;   /* oem diag: ring lead low-water + emergency snaps */
void ayaneo_gba_audio_submit(const short *interleaved, unsigned frames)
{
	extern volatile unsigned int g_dbg_asub_calls, g_dbg_asub_done, g_dbg_asub_frames;
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned i;

	if (s_gba_rw_capture) {			/* rewind: divert into the capture buffer, do not ring */
		unsigned n = frames, room = GBA_RW_CAP_MAX - s_gba_rw_cap_n, k;
		if (n > room) n = room;
		for (k = 0; k < n * 2u; k++)
			s_gba_rw_cap[s_gba_rw_cap_n * 2u + k] = interleaved[k];
		s_gba_rw_cap_n += n;
		return;
	}
	g_dbg_asub_calls++;
	if (!s_gbc_audio_on || s_gbc_paused)
		return;
	g_dbg_asub_done++;
	g_dbg_asub_frames += frames;
	g_dbg_aud_in += frames;
	jack_poll();			/* throttled headphone-jack detection + route switch */
	aud_lead_note();

	/*
	 * The resample rate is calibrated once to the panel refresh (ayaneo_gba_audio_set_rate), and
	 * leans from there towards a quarter ring of lead as the other cores' do (aud_lean): by at most
	 * half a percent, in proportion to how far off the lead is, a pitch change too small to hear. A
	 * frame that came late (a code flush, a card write) used to cost a frame of lead for good: the
	 * old trim moved a few units a frame within a quarter percent, too slow to win it back, so a few
	 * of them took the cursor to the emergency jump, heard as the music skipping ahead. The jump
	 * stays, for a real emergency only.
	 */
	{
		unsigned cur  = PICO_AUD_DMA_CUR();
		unsigned base = (unsigned)(addr_t)s_gbc_ring;
		unsigned rd   = (cur >= base) ? (cur - base) / 4u : 0u;
		int lead = (int)((s_gbc_widx - rd) & (GBC_RING_FRAMES - 1)), snaps = g_dbg_asnap_n, err;
		if (lead >= (int)(GBC_RING_FRAMES / 2))
			lead -= (int)GBC_RING_FRAMES;
		if (g_dbg_asub_done < 2u || lead < g_dbg_alead_min) g_dbg_alead_min = lead;
		err = aud_lean();
		if (g_dbg_asnap_n != snaps) { g_dbg_asnap_lead = lead; g_dbg_asnap_ms = gpt4_get_current_tick() / 13000u; }
		s_ga_inc = s_ga_inc_base + (int)((long long)s_ga_inc_base * err / ((long long)AUD_LEAN_SPAN * 200));
	}

	{	/* one clean linear-interpolation resample from the core's rate to the ring's 48 kHz;
		 * step (input samples per output, 16.16) comes from the panel-calibrated output rate */
		unsigned step = (unsigned)(((unsigned long long)s_ga_src_hz << 24) / (unsigned)s_ga_inc);
		if (!s_ga_prime) { s_ga_pl = interleaved[0]; s_ga_pr = interleaved[1]; s_ga_prime = 1; }
		for (i = 0; i < frames; i++) {
			int cl = interleaved[i * 2 + 0], cr = interleaved[i * 2 + 1];
			while (s_ga_phase < 0x10000u) {
				int fl = s_ga_pl + (((cl - s_ga_pl) * (int)s_ga_phase) >> 16);
				int fr = s_ga_pr + (((cr - s_ga_pr) * (int)s_ga_phase) >> 16);
				unsigned idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
				if (q < 256) { fl = (fl * (int)q) >> 8; fr = (fr * (int)q) >> 8; }
				s_gbc_ring[idx * 2 + 0] = (short)fl;
				s_gbc_ring[idx * 2 + 1] = (short)fr;
				s_gbc_widx++;
				s_ga_phase += step;
			}
			s_ga_phase -= 0x10000u;
			s_ga_pl = cl; s_ga_pr = cr;
		}
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}

/*
 * Audio-mastered frame pacing for the GBA. The AFE DMA drains the ring at exactly
 * 48 kHz (a hardware clock), so instead of pacing video off the 13 MHz counter
 * (which lets the emulated rate drift against the audio clock -> ~1 s microstutter
 * as the ring is over/underfed, and a ~30 s wrap-around distortion when the write
 * cursor laps the read cursor), we busy-wait until the audio buffered ahead of the
 * DMA read pointer has drained to a target lead. That locks emulation to the real
 * 48 kHz clock: each frame produces ~804 output samples and we wait for exactly
 * that many to be consumed (~59.73 fps) with bounded, non-drifting latency. If the
 * emulator falls behind (a cache-flush hitch) the buffer is already below the lead
 * so we return at once to catch up; a 13 MHz safety cap avoids stalling if the DMA
 * ever wedges.
 */
extern unsigned gpt4_get_current_tick(void);
#define GBA_AUDIO_LEAD		3072u		/* frames buffered ahead (~64 ms) */
#define GBA_FRAME_TICKS		217663u		/* 13e6 * 280896 / 16777216 */

volatile unsigned int g_dbg_apace_us, g_dbg_apace_fill;   /* oem diag: time waited in the audio pacer, ring fill at entry */
void ayaneo_gba_audio_pace(void)
{
	unsigned base = (unsigned)(addr_t)s_gbc_ring;
	unsigned start;

	if (!s_gbc_audio_on || s_gbc_paused)
		return;
	start = gpt4_get_current_tick();
	{ unsigned cur = PICO_AUD_DMA_CUR(), rd = (cur >= base) ? (cur - base) / 4u : 0u; g_dbg_apace_fill = (s_gbc_widx - rd) & (GBC_RING_FRAMES - 1); }
	for (;;) {
		unsigned cur = PICO_AUD_DMA_CUR();
		unsigned rd  = (cur >= base) ? (cur - base) / 4u : 0u;
		unsigned fill = (s_gbc_widx - rd) & (GBC_RING_FRAMES - 1);

		if (fill <= GBA_AUDIO_LEAD)
			break;
		if (fill - GBA_AUDIO_LEAD > 4u * 48u && (int)(gpt4_get_current_tick() - start) < (int)GBA_FRAME_TICKS) {
			extern void ayaneo_idle_until(unsigned target_tick);   /* the excess drains at 48 frames per ms */
			ayaneo_idle_until(gpt4_get_current_tick() + (fill - GBA_AUDIO_LEAD) * 13000u / 48u);
			continue;
		}
		/* safety: never block more than ~3 frame periods if the DMA stalls */
		if ((int)(gpt4_get_current_tick() - start) > (int)(3u * GBA_FRAME_TICKS))
			break;
	}
	g_dbg_apace_us = (gpt4_get_current_tick() - start) / 13u;
}

/* ---- direct 48 kHz stereo submit for the carousel-menu mixer ----
 * The menu's snes_audio mixer already produces 48 kHz interleaved stereo s16
 * (= the ring rate GBC_DST_HZ), so write straight into the ring with no resample.
 * ayaneo_menu_audio_room() reports how many frames fit under the target lead over
 * the AFE read cursor so the caller keeps the ring fed without over/underrun. */
volatile int g_dbg_mlead_min = 99999;
int ayaneo_menu_audio_room(void)
{
	unsigned cur, base, rd; int lead, room;
	if (!s_gbc_audio_on || s_gbc_paused)
		return 0;
	cur  = PICO_AUD_DMA_CUR();
	base = (unsigned)(addr_t)s_gbc_ring;
	rd   = (cur >= base) ? (cur - base) / 4u : 0u;
	lead = (int)((s_gbc_widx - rd) & (GBC_RING_FRAMES - 1));
	if (lead >= (int)(GBC_RING_FRAMES / 2))
		lead -= (int)GBC_RING_FRAMES;
	if (lead < g_dbg_mlead_min) g_dbg_mlead_min = lead;   /* menu mixer low-water (oem led) */
	room = (int)GBA_AUDIO_LEAD - lead;
	if (room < 0) room = 0;
	if (room > (int)(GBC_RING_FRAMES / 4)) room = (int)(GBC_RING_FRAMES / 4);
	return room;
}

void ayaneo_menu_audio_submit(const short *stereo, unsigned frames)
{
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned i;
	if (!s_gbc_audio_on || s_gbc_paused)
		return;
	for (i = 0; i < frames; i++) {
		int l = stereo[i * 2 + 0], r = stereo[i * 2 + 1];
		unsigned idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
		if (q < 256) { l = (l * (int)q) >> 8; r = (r * (int)q) >> 8; }
		s_gbc_ring[idx * 2 + 0] = (short)l;
		s_gbc_ring[idx * 2 + 1] = (short)r;
		s_gbc_widx++;
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}

/* ---- SNES (snes9x) audio: linear-interp UPSAMPLE src_hz -> 48 kHz ring ----
 * snes9x outputs stereo s16 at its native ~32040 Hz (below the 48 kHz ring), so we
 * interpolate up rather than the GBA box-decimator. Video is paced at 60.0988 Hz off the
 * 13 MHz counter (not audio-mastered), so a light lead check snaps the write cursor only
 * near under/overrun; the fixed ratio keeps pitch constant in between. src_hz comes from
 * the core's av_info so it tracks the real output rate. */
static unsigned s_sn_phase;      /* 16.16 fractional position within the current input step */
static int      s_sn_pl, s_sn_pr;
static int      s_sn_prime;

void ayaneo_snes_audio_reset(void) { s_sn_phase = 0; s_sn_pl = s_sn_pr = 0; s_sn_prime = 0; }

/* Called at a rewind direction change (forward->reverse on entry, reverse->forward on release).
 * Resets every core's resampler carry state so the first sample after the flip is not a blend of
 * forward+reverse data (a one-sample straddle tick), and reseats the ring write cursor to a clean
 * ~85 ms lead over the live AFE read cursor (same as ayaneo_gbc_audio_pause(0)) so the stale audio
 * queued ahead of the read pointer is dropped and the emergency snap cannot fire on the first
 * post-flip submit. Does NOT wipe the ring (that would drop the small amount of still-valid audio
 * between the read and write cursors). Only the active core's state matters; resetting all is safe. */
void ayaneo_audio_reverse_flip(void)
{
	if (!s_gbc_audio_on)
		return;
	s_rs_accl = s_rs_accr = 0; s_rs_n = 0; s_rs_phase = 0;          /* GBC box decimator */
	s_ga_phase = 0; s_ga_prime = 0;                                 /* GBA resampler */
	ayaneo_snes_audio_reset();                                      /* SNES linear interpolator */
	{
		unsigned cur = PICO_AUD_DMA_CUR(), base = (unsigned)(addr_t)s_gbc_ring;
		unsigned f = (cur >= base) ? (cur - base) / 4u : 0u;
		s_gbc_widx = (f + GBC_RING_FRAMES / 4u) & (GBC_RING_FRAMES - 1u);
	}
}

void ayaneo_snes_audio_submit(const short *interleaved, unsigned frames, unsigned src_hz)
{
	int vol = s_gbc_vol;
	unsigned q = (vol >= 100) ? 256u : ((unsigned)vol * 256u / 100u);
	unsigned step, i;

	if (!s_gbc_audio_on || s_gbc_paused || !frames)
		return;
	if (src_hz < 8000u || src_hz > 96000u) src_hz = 32040u;   /* sanity */
	jack_poll();			/* throttled headphone-jack detection + route switch */
	aud_lead_note();
	step = (src_hz << 16) / GBC_DST_HZ;                        /* input samples per output (<1.0 => upsample) */

	{	/* the rate leans towards a quarter ring of lead (aud_lean) */
		int err = aud_lean();
		step = (unsigned)((long long)step - (long long)step * err / ((long long)AUD_LEAN_SPAN * 200));
	}

	if (!s_sn_prime) { s_sn_pl = interleaved[0]; s_sn_pr = interleaved[1]; s_sn_prime = 1; }

	for (i = 0; i < frames; i++) {
		int cl = interleaved[i * 2 + 0], cr = interleaved[i * 2 + 1];
		while (s_sn_phase < 0x10000u) {
			int fl = s_sn_pl + (((cl - s_sn_pl) * (int)s_sn_phase) >> 16);
			int fr = s_sn_pr + (((cr - s_sn_pr) * (int)s_sn_phase) >> 16);
			unsigned idx = s_gbc_widx & (GBC_RING_FRAMES - 1);
			if (q < 256) { fl = (fl * (int)q) >> 8; fr = (fr * (int)q) >> 8; }
			s_gbc_ring[idx * 2 + 0] = (short)fl;
			s_gbc_ring[idx * 2 + 1] = (short)fr;
			s_gbc_widx++;
			s_sn_phase += step;
		}
		s_sn_phase -= 0x10000u;
		s_sn_pl = cl; s_sn_pr = cr;
	}
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}

/* Zero the ENTIRE audio ring so the AFE DMA loops silence instead of replaying the
 * last BGM segment. Submitting silence at the write cursor is not enough (the DMA
 * wraps and re-reads older frames), so wipe the whole buffer. Used at the menu ->
 * game handoff, when nobody feeds the ring during the ROM load. */
void ayaneo_menu_audio_silence(void)
{
	memset(s_gbc_ring, 0, sizeof(s_gbc_ring));
	arch_clean_cache_range((addr_t)s_gbc_ring, sizeof(s_gbc_ring));
}
