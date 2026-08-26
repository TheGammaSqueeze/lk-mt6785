/*
 * LK-side driver for the SNES Classic home menu. Thin: loads the asset pack from
 * boot_b into DRAM, hands the portable menu module (snes_menu.c) its work memory,
 * then each frame reads input, updates + renders the menu to the panel, and plays
 * queued sounds. All menu logic/drawing lives in the portable module so it can be
 * validated on the host against the web app.
 *
 * Reuses the GBC harness (display canvas/present, boot_b partition_read, boot
 * hook, watchdog) and provides the ayaneo_gbc_* entry names the hooks call.
 */
#include <debug.h>
#include <platform/mt_typedefs.h>
#include <kernel/thread.h>
#include <part_interface.h>

#include "snes_menu.h"
#include "snes_audio.h"

/* ---- LK primitives ---- */
extern time_t current_time(void);
/* free-running 13 MHz counter (13 ticks/us); current_time() is only 10 ms
 * resolution, far too coarse for a per-frame dt (it beats 10/20ms at 60Hz). */
extern unsigned gpt4_get_current_tick(void);
extern int  zunzip(unsigned char *src, unsigned long *lenp, void *dst, int dstlen, int offset);
extern void mtk_wdt_disable(void);
extern void mtk_wdt_restart(void);
extern void ayaneo_display_prepare(void);
extern unsigned int *ayaneo_canvas_back(unsigned int *pitch_w, unsigned int *W, unsigned int *H);
extern void ayaneo_canvas_present(void);
extern void ayaneo_fill(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int w, int h, unsigned int argb);
extern int  ayaneo_text(unsigned int *buf, unsigned int pitch_w,
			int x, int y, int scale, unsigned int argb, const char *s);
extern void ayaneo_apply_persisted_brightness(void);
extern void ayaneo_set_cpu_mhz(unsigned int mhz);
extern void bigcore_start(void);
extern unsigned bigcore_counter(void);
extern unsigned bigcore_raw_magic(void);
extern unsigned bigcore_raw_counter(void);
extern unsigned bigcore_cached_ok(void);
extern int g_bc_target, g_bc_psci_ret;
extern unsigned g_bc_mpidr, g_bc_pwrstat;

#ifdef AYANEO_BIGCORE_EXPT
/* ---- per-frame fork/join across cpu0 + the cached worker (cpu1) ----
 * The render split is proven output-correct on the host (host_render "split").
 * snes_menu_render is deterministic/non-mutating on the menu, uses a per-core
 * z-sort context (selected by MPIDR), and writes only its band's rows, so the
 * two cores can render disjoint scanline bands of the same frame concurrently.
 * Sync is WFE/SEV with a bounded-spin fallback so a wedged worker never hangs. */
#include "bigcore_comms.h"
static volatile struct bc_comms *const g_bc =
	(volatile struct bc_comms *)(unsigned long)BC_COMMS_PA;

static inline void bc_sev(void) { __asm__ volatile("sev" ::: "memory"); }
static inline void bc_wfe(void) { __asm__ volatile("wfe" ::: "memory"); }
static inline void bc_dmb(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static inline void bc_dsb(void) { __asm__ volatile("dsb ish" ::: "memory"); }

/* worker (cpu1): wait for a posted frame, render its band, signal done. */
void bc_worker_entry(void)
{
	unsigned last = 0;
	for (;;) {
		while (g_bc->go == last) bc_wfe();     /* park until cpu0 posts a frame */
		last = g_bc->go;
		bc_dmb();                              /* acquire job + menu state */
		{
			snes_target t = {0};
			t.fb = (unsigned int *)(unsigned long)g_bc->fb;
			t.pitch = g_bc->pitch; t.W = (int)g_bc->W; t.H = (int)g_bc->H;
			t.offx = (int)g_bc->offx; t.offy = (int)g_bc->offy;
			snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
			snes_target_band(&t, (int)g_bc->band_y0, (int)g_bc->band_y1);
			snes_menu_render((snes_menu *)(unsigned long)g_bc->menu_ptr, &t);
		}
		g_bc->counter = last;                  /* heartbeat = frames rendered */
		bc_dsb();                              /* release band writes + done */
		g_bc->done = last;
		bc_sev();                              /* wake cpu0 */
	}
}

static int bc_worker_ready(void) { return g_bc->cached_ok == 0xB16C0DE5u; }

/* Target little-cluster clock once the two cores are rendering in parallel. The
 * whole cluster (cpu0..5) shares one PLL/buck, so this clocks both workers. With
 * a 2-core split each core does ~half the frame, so we can drop from 2000 MHz and
 * still hold 60 fps at much lower dynamic power. Tune on HW: raise if the FPS HUD
 * dips below 60, lower toward ~1075 for minimum power. Single-core fallback keeps
 * the boot 2000 MHz. */
#define BC_MHZ 1200
static int s_bc_clk_set;

/* cpu0: fork the bottom band to the worker, render the top band, join. */
static unsigned s_bc_seq;
static void bc_dispatch(unsigned int *fb, unsigned pitch, int W, int H,
			snes_menu *menu, snes_target *tfull)
{
	int sy = (H / 2) & ~15;                    /* split on a 16px (>=64B) line */
	unsigned seq = ++s_bc_seq, spins = 0;
	g_bc->fb = (unsigned)(unsigned long)fb; g_bc->pitch = pitch;
	g_bc->W = (unsigned)W; g_bc->H = (unsigned)H;
	g_bc->offx = (unsigned)tfull->offx; g_bc->offy = (unsigned)tfull->offy;
	g_bc->band_y0 = (unsigned)sy; g_bc->band_y1 = (unsigned)H;   /* worker: bottom */
	g_bc->menu_ptr = (unsigned)(unsigned long)menu;
	bc_dsb();                                  /* release job (+ menu update) */
	g_bc->go = seq;
	bc_sev();                                  /* wake the worker */
	{	/* cpu0 renders the top band [0, sy) meanwhile */
		snes_target t0 = *tfull;
		snes_target_band(&t0, 0, sy);
		snes_menu_render(menu, &t0);
	}
	while (g_bc->done != seq) {                 /* join, bounded-spin fallback */
		if (++spins > 4000000u) {          /* worker missed the deadline */
			snes_target tb = *tfull;
			snes_target_band(&tb, sy, H);
			snes_menu_render(menu, &tb);
			break;
		}
		bc_wfe();
	}
	bc_dmb();                                  /* acquire the worker's band writes */
}
#endif
extern int  mt_power_off(void);
extern int  pmic_detect_powerkey(void);

/* ---- audio (ayaneo_audio.c): codec bring-up + direct 48 kHz ring submit ---- */
extern void ayaneo_gbc_audio_init(void);
extern void ayaneo_snes_audio_submit(const short *stereo, unsigned frames);
extern int  ayaneo_snes_audio_room(void);
extern void ayaneo_gbc_audio_set_volume(int v);
extern int  ayaneo_gbc_audio_get_volume(void);

/* ---- volume/brightness (MTK keypad volume keys; Select = brightness modifier) ---- */
extern int  mtk_detect_key(unsigned short key);
extern int  ayaneo_brightness_step(int dir);   /* dir +1/-1; returns new 0-100% */
extern int  ayaneo_brightness_pct(void);
extern void ayaneo_settings_load(void);
extern void ayaneo_settings_save(void);

/* ---- input (gpio-keys, active-low) ---- */
extern int mt_set_gpio_mode(unsigned pin, unsigned mode);
extern int mt_set_gpio_dir(unsigned pin, unsigned dir);
extern int mt_set_gpio_pull_enable(unsigned pin, unsigned en);
extern int mt_set_gpio_pull_select(unsigned pin, unsigned sel);
extern int mt_get_gpio_in(unsigned pin);
#define GP(n)      ((n) | 0x80000000u)
#define PRESSED(g) (mt_get_gpio_in(GP(g)) == 0)
#define K_LEFT 78
#define K_RIGHT 80
#define K_UP 89
#define K_DOWN 79
#define K_A 83
#define K_B 82
#define K_START 91
#define K_SELECT 90

/* ---- config / DRAM layout ---- */
#define SNES_PART     "boot_b"
#define SNES_OFF      0x00400000u
#define SNES_MAGIC    0x5A534E53u
#define SNES_BLOB_PA  0x50000000u
#define SNES_HOME_PA  0x50C00000u   /* home rnode pool */
#define SNES_BG_PA    0x50E00000u   /* bg rnode pool */
#define SNES_COMP_PA  0x51000000u   /* compressed staging */
#define SNES_WP_PA    0x52000000u   /* wallpaper cache (1536*720*4) */
#define SNES_CHROME_PA 0x53000000u  /* static chrome cache (up to 1280*960*4 in 4:3) */
#define SNES_RAW_MAX  (32u * 1024 * 1024)
#define SNES_COMP_MAX (16u * 1024 * 1024)
#define HOME_CAP (16u * 1024 * 1024 / (unsigned)sizeof(snes_rnode))
#define BG_CAP   (2u  * 1024 * 1024 / (unsigned)sizeof(snes_rnode))

static snes_pack s_pk;
static snes_menu s_menu;
static snes_mixer s_mix;
static short s_mixbuf[16384 * 2];  /* holds a full ring-half refill (~341 ms) */

/* Resolve a sound res-hash to its PCM + loop info and start a mixer voice. */
static void play_sound(uint32_t hash, int loop, int is_bgm)
{
	const snes_snd_entry *sn = snes_res_snd(&s_pk, hash);
	const int16_t *pcm;
	if (!sn || !sn->frames) return;
	pcm = (const int16_t *)(s_pk.base + sn->pcm);
	snes_audio_play(&s_mix, pcm, sn->frames, sn->rate,
			sn->loop_start, sn->loop_end, loop, 256, is_bgm);
}

/* on-screen volume/brightness slider (drawn for a short time after a change) */
static int s_osd_kind;    /* 0 none, 1 volume, 2 brightness */
static int s_osd_pct;
static int s_osd_ticks;

/* Poll the hardware volume keys. Plain Volume adjusts audio; Select + Volume
 * adjusts screen brightness. Persists the new value to boot_b. */
static void poll_volume(void)
{
	static int vu_prev, vd_prev;
	int vu = mtk_detect_key(0x11);      /* VolumeUp   */
	int vd = mtk_detect_key(0x00);      /* VolumeDown */
	int sel = PRESSED(K_SELECT);        /* brightness modifier */
	int dir = 0;
	if (vu && !vu_prev) dir = +1;
	else if (vd && !vd_prev) dir = -1;
	if (dir) {
		if (sel) {
			s_osd_pct = ayaneo_brightness_step(dir);
			s_osd_kind = 2;
		} else {
			ayaneo_gbc_audio_set_volume(ayaneo_gbc_audio_get_volume() + dir * 5);
			s_osd_pct = ayaneo_gbc_audio_get_volume();
			s_osd_kind = 1;
		}
		s_osd_ticks = 90;   /* ~1.4 s at 8 ms/frame */
		ayaneo_settings_save();
	}
	vu_prev = vu; vd_prev = vd;
}

/* draw the volume/brightness slider onto the canvas (letterbox top bar) */
static void draw_osd(unsigned int *fb, unsigned int pitch, int W)
{
	int bw = 360, bh = 34, bx = (W - bw) / 2, by = 18, fillw;
	if (s_osd_ticks <= 0) return;
	ayaneo_fill(fb, pitch, bx - 8, by - 8, bw + 16, bh + 16, 0xE0101418u);
	ayaneo_fill(fb, pitch, bx, by, bw, bh, 0xFF303840u);
	fillw = bw * (s_osd_pct < 0 ? 0 : s_osd_pct > 100 ? 100 : s_osd_pct) / 100;
	ayaneo_fill(fb, pitch, bx, by, fillw, bh, 0xFF37B0FFu);
	ayaneo_text(fb, pitch, bx, by - 20, 2, 0xFFFFFFFFu,
		    s_osd_kind == 2 ? "BRIGHTNESS" : "VOLUME");
	s_osd_ticks--;
}

/* ---- frame-time telemetry (top-left readout) ---- */
extern unsigned (*g_perf_tick)(void);  /* snes_menu.c per-phase profiler hook */
extern unsigned g_perf[8];             /* 0 wp,1 chrome,2 carousel,3 filmstrip,4 rest */
static unsigned perf_tick(void) { return gpt4_get_current_tick(); }
static unsigned s_perf_render_us;    /* last frame: update+render (CPU cost) */
static unsigned s_perf_present_us;   /* last frame: present incl. vsync wait */
static char s_perf_str[40] = "";
static char *u2s(char *p, unsigned v)
{
	char tmp[12]; int n = 0;
	if (v == 0) { *p++ = '0'; return p; }
	while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
	while (n) *p++ = tmp[--n];
	return p;
}
static char s_perf_str2[48] = "";
#ifdef AYANEO_DEBUG_LOGGING
static char s_perf_str3[48] = "";   /* experimental bigcore proof-of-life line (debug only) */
#endif
#ifdef AYANEO_DEBUG_LOGGING   /* i2s/u2h feed the debug-only bigcore line below */
static char *i2s(char *p, int v) { if (v < 0) { *p++ = '-'; v = -v; } return u2s(p, (unsigned)v); }
static char *u2h(char *p, unsigned v) {   /* compact hex, no leading zeros */
	static const char hx[] = "0123456789abcdef"; char t[8]; int n = 0;
	*p++ = '0'; *p++ = 'x';
	if (!v) { *p++ = '0'; return p; }
	while (v) { t[n++] = hx[v & 0xf]; v >>= 4; }
	while (n) *p++ = t[--n];
	return p;
}
#endif
static void draw_perf(unsigned int *fb, unsigned int pitch)
{
	static unsigned acc_r, acc_p, n, ap0, ap1, ap2, ap3, ap4;
	acc_r += s_perf_render_us; acc_p += s_perf_present_us; n++;
	ap0 += g_perf[0]/13; ap1 += g_perf[1]/13; ap2 += g_perf[2]/13;
	ap3 += g_perf[3]/13; ap4 += g_perf[4]/13;
#ifdef AYANEO_DEBUG_LOGGING
	{	/* EXPERIMENTAL multicore proof of life: boot MPIDR, PSCI target, ret, counter */
		char *p = s_perf_str3;
		*p++='B'; *p++='C'; *p++=' ';
		*p++='m'; p=u2h(p, g_bc_mpidr); *p++=' ';
		*p++='t'; p=i2s(p, g_bc_target); *p++=' ';
		*p++='r'; p=i2s(p, g_bc_psci_ret); *p++=' ';
		*p++='p'; p=u2h(p, g_bc_pwrstat); *p++=' ';
		/* raw (ungated) magic + counter: g=1 if handshake magic present, then the
		 * live counter regardless of magic, so one flash fully classifies (see
		 * bigcore.c bigcore_raw_*). */
		/* g = reached-the-stub (magic), k = came-up-cached (MMU+caches on),
		 * c = worker heartbeat. g1 k0 => reached but MMU-enable wedged. */
		*p++='g'; *p++=(bigcore_raw_magic()==0xB16C0DE5u)?'1':'0';
		*p++='k'; *p++=(bigcore_cached_ok()==0xB16C0DE5u)?'1':'0'; *p++=' ';
		*p++='c'; p=u2s(p, bigcore_raw_counter()); *p=0;
	}
#endif
	if (n >= 20) {
		unsigned ar = acc_r / n, ap = acc_p / n, tot = ar + ap;
		unsigned fps = tot ? (1000000u + tot / 2) / tot : 0;
		char *p = s_perf_str;
		p = u2s(p, fps); *p++ = 'f'; *p++ = ' ';
		*p++ = 'r'; p = u2s(p, ar); *p++ = ' ';
		*p++ = 'p'; p = u2s(p, ap); *p = 0;
		p = s_perf_str2;
		*p++='w'; p=u2s(p,ap0/n); *p++=' ';
		*p++='c'; p=u2s(p,ap1/n); *p++=' ';
		*p++='r'; p=u2s(p,ap2/n); *p++=' ';
		*p++='f'; p=u2s(p,ap3/n); *p++=' ';
		*p++='o'; p=u2s(p,ap4/n); *p=0;
		acc_r = acc_p = n = ap0 = ap1 = ap2 = ap3 = ap4 = 0;
	}
	if (s_perf_str[0]) {
		ayaneo_text(fb, pitch, 10, 6, 2, 0xFF00FF66u, s_perf_str);
		ayaneo_text(fb, pitch, 10, 26, 2, 0xFF00FF66u, s_perf_str2);
	}
#ifdef AYANEO_DEBUG_LOGGING
	ayaneo_text(fb, pitch, 10, 46, 2, 0xFF3060FFu, s_perf_str3);   /* blue, debug only */
#endif
}

static void dbg(const char *msg)
{
	unsigned int pitch, W, H, i;
	for (i = 0; i < 2; i++) {
		unsigned int *b = ayaneo_canvas_back(&pitch, &W, &H);
		ayaneo_fill(b, pitch, 0, 0, (int)W, 40, 0xFF000000u);
		ayaneo_text(b, pitch, 20, 8, 3, 0xFF40FF60u, msg);
		ayaneo_canvas_present();
	}
}

static int load_pack(void)
{
	unsigned char hdr[12];
	unsigned magic, rawlen, complen;
	unsigned char *comp = (unsigned char *)SNES_COMP_PA;
	unsigned char *blob = (unsigned char *)SNES_BLOB_PA;
	unsigned long zlen;
	if (partition_read(SNES_PART, SNES_OFF, hdr, 12) != 12) return -1;
	magic  = (unsigned)hdr[0] | (hdr[1]<<8) | (hdr[2]<<16) | ((unsigned)hdr[3]<<24);
	rawlen = (unsigned)hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((unsigned)hdr[7]<<24);
	complen= (unsigned)hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | ((unsigned)hdr[11]<<24);
	if (magic != SNES_MAGIC || rawlen == 0 || rawlen > SNES_RAW_MAX ||
	    complen == 0 || complen > SNES_COMP_MAX) return -2;
	if (partition_read(SNES_PART, SNES_OFF + 12, comp, complen) != (ssize_t)complen) return -3;
	zlen = complen;
	if (zunzip(comp, &zlen, blob, (int)rawlen, 0) != 0) return -4;
	if (snes_pack_open(&s_pk, blob, rawlen) != 0) return -5;
	return 0;
}

static void input_init(void)
{
	unsigned pins[8] = { K_LEFT, K_RIGHT, K_UP, K_DOWN, K_A, K_B, K_START, K_SELECT };
	int i;
	for (i = 0; i < 8; i++) {
		mt_set_gpio_mode(GP(pins[i]), 0); mt_set_gpio_dir(GP(pins[i]), 0);
		mt_set_gpio_pull_enable(GP(pins[i]), 1); mt_set_gpio_pull_select(GP(pins[i]), 1);
	}
}

static int snes_emu_thread(void *arg)
{
	int r;
	unsigned last;
	(void)arg;

	ayaneo_display_prepare();
	mtk_wdt_disable();
	dbg("SNES: loading pack");

	r = load_pack();
	if (r != 0) {
		char m[16]; m[0]='S';m[1]='N';m[2]='E';m[3]='S';m[4]=' ';m[5]='E';m[6]='R';
		m[7]='R';m[8]=' ';m[9]=(char)('0'-r);m[10]=0;
		dbg(m);
		for (;;) { mtk_wdt_restart(); thread_sleep(200); }
	}
	if (snes_menu_init(&s_menu, &s_pk, (snes_rnode *)SNES_HOME_PA, HOME_CAP,
			   (snes_rnode *)SNES_BG_PA, BG_CAP, (uint32_t *)SNES_WP_PA,
			   (uint32_t *)SNES_CHROME_PA) != 0) {
		dbg("SNES ERR: menu init");
		for (;;) { mtk_wdt_restart(); thread_sleep(200); }
	}

	input_init();
	ayaneo_set_cpu_mhz(2000);
	bigcore_start();   /* EXPERIMENTAL: try to bring up a big A76 core (proof of life) */
	ayaneo_apply_persisted_brightness();

	/* bring up the codec/AFE ring and start the looping home BGM */
	ayaneo_settings_load();          /* persisted volume + brightness */
	snes_audio_init(&s_mix);
	ayaneo_gbc_audio_init();
	if (s_menu.bgm) play_sound(s_menu.bgm, 1, 1);

	g_perf_tick = perf_tick;      /* enable the render per-phase profiler */
	last = gpt4_get_current_tick();

	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t = {0};   /* zero-init incl. the band clip (band_y0/y1) */
		snes_input in;
		unsigned t_frame0 = gpt4_get_current_tick();
		/* 13 MHz counter: dt in seconds = ticks / 13e6 (unsigned wrap-safe) */
		float dt = (float)(t_frame0 - last) / 13000000.0f;
		if (dt <= 0) dt = 0.016f; if (dt > 0.1f) dt = 0.1f;
		last = t_frame0;

		in.left = PRESSED(K_LEFT); in.right = PRESSED(K_RIGHT);
		in.up = PRESSED(K_UP); in.down = PRESSED(K_DOWN);
		in.a = PRESSED(K_A); in.b = PRESSED(K_B);
		in.start = PRESSED(K_START); in.select = PRESSED(K_SELECT);

		t.fb = fb; t.pitch = pitch; t.W = (int)W; t.H = (int)H;
		t.offx = ((int)W - SNES_VW) / 2; t.offy = ((int)H - SNES_VH) / 2;
		snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
		/* the panel is physically 1280x960 (4:3); fill it natively instead of
		 * letterboxing the 720 design. Enable the 4:3 adaptation whenever the panel
		 * is at least 960 tall and rebuild the chrome cache on the transition. */
		{
			int want = ((int)H >= 960) ? 1 : 0;
			if (want != s_menu.aspect) { s_menu.aspect = want; s_menu.chrome_ready = 0; }
		}
		/* clear the letterbox bars (the wallpaper covers the 720 region) */
		ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
		ayaneo_fill(fb, pitch, 0, t.offy + SNES_VH, (int)W, t.offy, 0xFF000000u);

		poll_volume();
		snes_menu_update(&s_menu, &in, dt);
#ifdef AYANEO_BIGCORE_EXPT
		/* Split the render across cpu0 + the cached worker when it is up and the
		 * chrome cache is already built (so the two cores never build it at once).
		 * Falls back to single-core otherwise. */
		if (bc_worker_ready() && s_menu.chrome_ready) {
			if (!s_bc_clk_set) { ayaneo_set_cpu_mhz(BC_MHZ); s_bc_clk_set = 1; }
			bc_dispatch(fb, pitch, (int)W, (int)H, &s_menu, &t);
		} else
#endif
			snes_menu_render(&s_menu, &t);
		s_perf_render_us = (gpt4_get_current_tick() - t_frame0) / 13u;
		draw_osd(fb, pitch, (int)W);
		draw_perf(fb, pitch);

		/* start any queued one-shot SFX, then mix a frame's worth of audio
		 * and push it to the AFE ring (keeps the ring fed ahead of the DMA) */
		{
			uint32_t h;
			int need;
			while ((h = snes_menu_next_sound(&s_menu)) != 0)
				play_sound(h, 0, 0);
			/* self-clocked: top the ring up to its target lead over the DMA
			 * read cursor, so 15-20 fps can't starve it into replaying */
			need = ayaneo_snes_audio_room();
			if (need > 16384) need = 16384;
			if (need > 0) {
				snes_audio_mix(&s_mix, s_mixbuf, (unsigned)need);
				ayaneo_snes_audio_submit(s_mixbuf, (unsigned)need);
			}
		}

		{
			unsigned t_pre = gpt4_get_current_tick();
			ayaneo_canvas_present();   /* blocks on FRAME_DONE = vsync, pacing
						    * the loop to 60Hz and yielding the CPU. */
			s_perf_present_us = (gpt4_get_current_tick() - t_pre) / 13u;
		}
		mtk_wdt_restart();
		{
			static int armed;
			int p = pmic_detect_powerkey();
			if (!p) armed = 1; else if (armed) mt_power_off();
		}
	}
	return 0;
}

/* ---- entry points the boot/charging hooks call ---- */
void ayaneo_gbc_start(void)
{
	thread_t *t = thread_create("ayaneo_snes", &snes_emu_thread, NULL,
				    DEFAULT_PRIORITY, 65536);
	if (t) thread_resume(t);
}
void ayaneo_gbc_charging_screen(void) { }
int  ayaneo_gbc_select_held(void) { return 0; }
