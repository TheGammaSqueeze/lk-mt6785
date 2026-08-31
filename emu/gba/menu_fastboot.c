/*
 * Fastboot debug channel for the SNES-Classic-mini menu (AYANEO_GBA_SD).
 *
 * The USB fastboot stack is brought up ALONGSIDE the running emulator (see
 * ayaneo_fastboot_usb_start in mt_boot.c), so these work over USB while the
 * menu/game is live - no reboot into fastboot mode. They let me diagnose the
 * display issues host_render can never reproduce (banding is a scanout artifact,
 * timings are A55-specific) by reading the LIVE panel + metrics and driving nav:
 *
 *   fastboot get_staged fb.bin      - upload the CURRENTLY DISPLAYED framebuffer as
 *                                     raw bytes (16-byte header: magic,W,H,pitch;
 *                                     then pitch*H BGRA pixels). Multi-MB in one
 *                                     transfer. Reconstruct a PNG with
 *                                     tools/ayaneo/gba/fastboot_fb.py.
 *   fastboot oem key:<L R U D A B S T [ ]>
 *                                   - inject one button into the live menu (held a
 *                                     few frames = a clean press), so I can navigate
 *                                     over USB and screenshot each state.
 *   fastboot oem diag               - live render metrics: last/peak render us, loop
 *                                     fps, present vs gate-skipped counts, focus.
 */
extern void fastboot_register(const char *prefix,
			      void (*handle)(const char *arg, void *data, unsigned sz),
			      int allow_locked, int need_download);
extern void fastboot_info(const char *reason);
extern void fastboot_okay(const char *info);
extern void fastboot_fail(const char *reason);
extern int  fastboot_upload(void *buf, unsigned size);
extern int  snprintf(char *str, unsigned long size, const char *fmt, ...);
extern void *memcpy(void *dst, const void *src, unsigned long n);
extern void thread_sleep(unsigned ms);

/* live panel + metrics + input injection (display driver + gba_snes_menu.c) */
extern const unsigned int *ayaneo_canvas_front(unsigned int *pitch, unsigned int *W, unsigned int *H);
extern volatile unsigned int g_dbg_render_us, g_dbg_peak_us, g_dbg_fps;
extern volatile unsigned int g_dbg_present_cnt, g_dbg_skip_cnt;
extern volatile int g_dbg_focus, g_dbg_last_static;
extern volatile int g_inject_btn, g_inject_frames;

static char lbuf[128];

static void cmd_diag(const char *arg, void *data, unsigned sz)
{
	unsigned pc = g_dbg_present_cnt, sk = g_dbg_skip_cnt;
	(void)arg; (void)data; (void)sz;
	snprintf(lbuf, sizeof lbuf, "diag: render=%uus peak=%uus fps=%u focus=%d last=%s",
		 g_dbg_render_us, g_dbg_peak_us, g_dbg_fps, g_dbg_focus,
		 g_dbg_last_static ? "static" : "changed");
	fastboot_info(lbuf);
	snprintf(lbuf, sizeof lbuf, "diag: present=%u skip=%u skip%%=%u",
		 pc, sk, (pc + sk) ? (sk * 100u / (pc + sk)) : 0u);
	fastboot_info(lbuf);
	fastboot_okay("diag done");
}

static int keycode(const char *s)
{
	while (*s == ' ' || *s == ':') s++;
	switch (*s) {
	case 'L': return 0; case 'R': return 1; case 'U': return 2; case 'D': return 3;
	case 'A': return 4; case 'B': return 5; case 'S': return 6; case 'T': return 7;
	case '[': return 8; case ']': return 9;
	default:  return -1;
	}
}

static void cmd_key(const char *arg, void *data, unsigned sz)
{
	int k = keycode(arg), w = 0;
	(void)data; (void)sz;
	if (k < 0) { fastboot_fail("key: use one of L R U D A B S T [ ]"); return; }
	g_inject_btn = k;
	g_inject_frames = 3;                       /* held ~3 frames = a clean press edge */
	while (g_inject_frames > 0 && w < 400) { thread_sleep(2); w++; }  /* wait consumed */
	thread_sleep(220);                         /* let the nav animation settle */
	snprintf(lbuf, sizeof lbuf, "key: code=%d focus=%d render=%uus", k, g_dbg_focus, g_dbg_render_us);
	fastboot_info(lbuf);
	fastboot_okay("key done");
}

/* `fastboot get_staged <file>` triggers this: upload the live front buffer. */
static void cmd_upload(const char *arg, void *data, unsigned sz)
{
	unsigned int pitch, W, H, bytes;
	const unsigned int *fb;
	unsigned int *hdr = (unsigned int *)data;   /* data = download_base staging (128MB) */
	(void)arg; (void)sz;

	fb = ayaneo_canvas_front(&pitch, &W, &H);
	bytes = pitch * H * 4u;
	hdr[0] = 0x42465941u;                        /* 'AYFB' */
	hdr[1] = W; hdr[2] = H; hdr[3] = pitch;
	memcpy((unsigned char *)data + 16, fb, bytes);
	if (fastboot_upload(data, 16u + bytes) != 0) { fastboot_fail("upload failed"); return; }
	fastboot_okay("");
}

void gba_menu_fastboot_register(void)
{
	fastboot_register("oem diag", cmd_diag, 1, 0);
	fastboot_register("oem key:", cmd_key, 1, 0);
	fastboot_register("upload", cmd_upload, 1, 0);
}
