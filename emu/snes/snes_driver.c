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
#define SNES_CHROME_PA 0x53000000u  /* static chrome cache (1280*720*4) */
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
static void draw_perf(unsigned int *fb, unsigned int pitch)
{
	static unsigned acc_r, acc_p, n;
	acc_r += s_perf_render_us; acc_p += s_perf_present_us; n++;
	if (n >= 20) {
		unsigned ar = acc_r / n, ap = acc_p / n, tot = ar + ap;
		unsigned fps = tot ? (1000000u + tot / 2) / tot : 0;
		char *p = s_perf_str;
		p = u2s(p, fps); *p++ = 'f'; *p++ = ' ';
		*p++ = 'r'; p = u2s(p, ar); *p++ = ' ';
		*p++ = 'p'; p = u2s(p, ap); *p = 0;
		acc_r = acc_p = n = 0;
	}
	if (s_perf_str[0])
		ayaneo_text(fb, pitch, 10, 6, 2, 0xFF00FF66u, s_perf_str);
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
	ayaneo_apply_persisted_brightness();

	/* bring up the codec/AFE ring and start the looping home BGM */
	ayaneo_settings_load();          /* persisted volume + brightness */
	snes_audio_init(&s_mix);
	ayaneo_gbc_audio_init();
	if (s_menu.bgm) play_sound(s_menu.bgm, 1, 1);

	last = gpt4_get_current_tick();

	for (;;) {
		unsigned int pitch, W, H;
		unsigned int *fb = ayaneo_canvas_back(&pitch, &W, &H);
		snes_target t;
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
		/* clear the letterbox bars (the wallpaper covers the 720 region) */
		ayaneo_fill(fb, pitch, 0, 0, (int)W, t.offy, 0xFF000000u);
		ayaneo_fill(fb, pitch, 0, t.offy + SNES_VH, (int)W, t.offy, 0xFF000000u);

		poll_volume();
		snes_menu_update(&s_menu, &in, dt);
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
