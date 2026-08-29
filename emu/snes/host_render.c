/*
 * Host render harness (x86) - compiles the portable engine + menu module and
 * renders the menu to a PPM, so the output can be validated against the web app
 * locally without the device. Not part of the LK build.
 *
 *   build:  emu/snes/build_host.sh
 *   run:    ./host_render <snes_pack.bin> <out.ppm> [frames] [nav]
 *           nav = a string of L/R/U/D/A/B/S/T applied one per frame before render
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "snes_menu.h"

/* free-running microsecond counter for the phase profiler (g_perf_tick hook) */
static unsigned prof_now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)((unsigned long long)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull);
}

int main(int argc, char **argv)
{
	const char *packf = argc > 1 ? argv[1] : "out/snes_pack.bin";
	const char *outf  = argc > 2 ? argv[2] : "/tmp/snes_host.ppm";
	int frames = argc > 3 ? atoi(argv[3]) : 60;
	const char *nav = argc > 4 ? argv[4] : "";
	FILE *f = fopen(packf, "rb");
	long len; void *blob;
	snes_pack pk; snes_menu menu; snes_target t; snes_input in;
	uint32_t *fb, *wp, *chrome; snes_rnode *home_pool, *bg_pool;
	int W = 1280, H = 960, i, x, y;
	if (!f) { fprintf(stderr, "open %s failed\n", packf); return 1; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	blob = malloc(len); if (fread(blob, 1, len, f) != (size_t)len) return 1; fclose(f);
	if (snes_pack_open(&pk, blob, len) != 0) { fprintf(stderr, "bad pack\n"); return 1; }

	fb = calloc((size_t)W * H, 4);
	wp = malloc((size_t)WP_CACHE_W * WP_CACHE_H * 4);
	chrome = malloc((size_t)SNES_VW * 960 * 4);   /* panel-tall so 4:3 chrome fits */
	home_pool = malloc(sizeof(snes_rnode) * 4096);
	bg_pool = malloc(sizeof(snes_rnode) * 256);
	if (snes_menu_init(&menu, &pk, home_pool, 4096, bg_pool, 256, wp, chrome) != 0) {
		fprintf(stderr, "menu init failed\n"); return 1;
	}
	t.fb = fb; t.pitch = W; t.W = W; t.H = H;
	t.offx = (W - SNES_VW) / 2; t.offy = (H - SNES_VH) / 2;
	snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
	if (getenv("SNES_ASPECT43")) { menu.aspect = 1; menu.chrome_ready = 0; }  /* rebuild chrome for 4:3 */

	/* Card-tile cache for the 60fps single-buffer path (disable with SNES_NOCACHE). */
	{
		static int ctgi[128];
		int cap = menu.ngames; if (cap > 128) cap = 128; if (cap < 1) cap = 1;
		if (!getenv("SNES_NOCACHE")) {
			uint32_t *ctbuf = malloc((size_t)cap * SNES_CT_W * SNES_CT_H * 4);
			snes_menu_set_ctile(&menu, ctbuf, ctgi, cap);
		}
	}

	/* Cache exactness check: `host_render <pack> - ctcheck [nav]` renders each SETTLED
	 * state along a nav path both with the tile cache and with a live direct render, and
	 * diffs. Settled cards sit at integer positions so the cache must be pixel-exact
	 * (native) / within the baked <=1px 4:3 resample. */
	if (argc > 3 && strcmp(argv[3], "ctcheck") == 0) {
		const char *rnav = argc > 4 ? argv[4] : "RRRLLDDUU";
		uint32_t *A = malloc((size_t)W * H * 4), *B = malloc((size_t)W * H * 4);
		int ni, s, worst = 0, worstpx = 0, nstates = 0;
		int ctgi2[128]; int cap2 = menu.ngames > 128 ? 128 : menu.ngames;
		uint32_t *ctb2 = malloc((size_t)(cap2<1?1:cap2) * SNES_CT_W * SNES_CT_H * 4);
		memset(&in, 0, sizeof(in));
		for (s = 0; s < 240; s++) snes_menu_update(&menu, &in, 1.0f/60.0f);
		for (ni = 0; ni <= (int)strlen(rnav); ni++) {
			int px, dmax = 0, dcnt = 0, bx0=W, by0=H, bx1=-1, by1=-1;
			/* settle fully so cards sit at integer positions */
			for (s = 0; s < 40; s++) { memset(&in,0,sizeof(in)); snes_menu_update(&menu,&in,1.0f/60.0f); }
			/* A = live (cache off) */
			snes_menu_set_ctile(&menu, 0, 0, 0);
			for (i=0;i<W*H;i++) fb[i]=0xFF000000u; snes_menu_render(&menu,&t);
			for (i=0;i<W*H;i++) A[i]=fb[i];
			/* B = cached */
			snes_menu_set_ctile(&menu, ctb2, ctgi2, cap2<1?1:cap2);
			for (i=0;i<W*H;i++) fb[i]=0xFF000000u; snes_menu_render(&menu,&t);
			for (i=0;i<W*H;i++) B[i]=fb[i];
			for (px=0; px<W*H; px++) {
				int dr=((A[px]>>16)&0xff)-((B[px]>>16)&0xff); int dg=((A[px]>>8)&0xff)-((B[px]>>8)&0xff); int db=(A[px]&0xff)-(B[px]&0xff);
				if(dr<0)dr=-dr; if(dg<0)dg=-dg; if(db<0)db=-db;
				{ int d=dr>dg?dr:dg; if(db>d)d=db; if(d){dcnt++; if(d>dmax)dmax=d;
				  { int xx=px%W, yy=px/W; if(xx<bx0)bx0=xx; if(xx>bx1)bx1=xx; if(yy<by0)by0=yy; if(yy>by1)by1=yy; } } }
			}
			if (dmax>worst) worst=dmax; if (dcnt>worstpx) worstpx=dcnt; nstates++;
			fprintf(stderr, "  state %2d (after '%.*s'): diff_px=%d maxchan=%d bbox=[%d,%d..%d,%d]\n",
				ni, ni, rnav, dcnt, dmax, bx0,by0,bx1,by1);
			if (ni < (int)strlen(rnav)) {
				char c=rnav[ni]; memset(&in,0,sizeof(in));
				in.right=(c=='R'); in.left=(c=='L'); in.up=(c=='U'); in.down=(c=='D');
				snes_menu_update(&menu,&in,1.0f/60.0f);
			}
		}
		fprintf(stderr, "CTCHECK aspect=%d states=%d WORST diff_px=%d maxchan=%d  (%s)\n",
			menu.aspect, nstates, worstpx, worst,
			worst==0 ? "PIXEL-EXACT" : worst<=16 ? "within <=1px resample" : "REGRESSION");
		return 0;
	}

	/* Phase profiler: `host_render <pack> - prof [nav]` settles the attract, then holds
	 * RIGHT for N frames (moving carousel = worst case) timing each render phase via the
	 * g_perf[] hook (0 wp, 1 chrome, 2 carousel, 3 filmstrip, 4 rest). Prints the average
	 * per-phase wall time. x86 absolute us are not device-accurate, but the phase RATIO and
	 * the total are a good first-order guide to where the single-buffer frame cost lives. */
	if (argc > 3 && strcmp(argv[3], "prof") == 0) {
		const char *pnav = argc > 4 ? argv[4] : "R";
		int s, fi, PN = 120; double acc[5] = {0,0,0,0,0}; struct timespec t0, t1;
		extern unsigned (*g_perf_tick)(void);
		extern unsigned g_perf[8];
		g_perf_tick = prof_now_us;
		memset(&in, 0, sizeof(in));
		for (s = 0; s < 240; s++) snes_menu_update(&menu, &in, 1.0f/60.0f);   /* settle */
		for (fi = 0; fi < PN; fi++) {
			int k; char c = pnav[0];
			memset(&in, 0, sizeof(in));
			in.right = (c=='R'); in.left = (c=='L'); in.up = (c=='U'); in.down = (c=='D');
			snes_menu_update(&menu, &in, 1.0f/60.0f);
			for (i = 0; i < W * H; i++) fb[i] = 0xFF000000u;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			snes_menu_render(&menu, &t);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			for (k = 0; k < 5; k++) acc[k] += g_perf[k];
			(void)t0; (void)t1;
		}
		{ double tot = 0; int k; const char *nm[5] = {"wp","chrome","carousel","filmstrip","rest"};
		  for (k = 0; k < 5; k++) tot += acc[k];
		  fprintf(stderr, "PROF nav=%s frames=%d aspect=%d  (host x86 us/frame; ratios guide)\n",
			  pnav, PN, menu.aspect);
		  for (k = 0; k < 5; k++)
			fprintf(stderr, "  %-10s %8.1f us  %5.1f%%\n", nm[k], acc[k]/PN, 100.0*acc[k]/tot);
		  fprintf(stderr, "  %-10s %8.1f us  total\n", "FRAME", tot/PN); }
		return 0;
	}

	/* Transition-capture mode: `host_render <pack> <outdir> seq <prefixNav> <transKeys> <nframes>`
	 * mirrors the web frame-stepper (tools/_web_car_right_mid.mjs). It settles the
	 * attract, walks <prefixNav> to reach the starting state, then steps at
	 * dt=0.0333 (30fps, MATCHING the web capture): frame 0 = pre-press, then for
	 * each key in <transKeys> presses it for one step, then free-runs the rest to
	 * <nframes>. Each frame is written outdir/NNN.ppm so the validator can diff the
	 * transition frame-by-frame against /tmp/web_car_mid/NNN.png. */
	if (argc > 3 && strcmp(argv[3], "seq") == 0) {
		const char *outdir = argv[2];
		const char *prefix = argc > 4 ? argv[4] : "RRR";
		const char *tk     = argc > 5 ? argv[5] : "R";
		int nframes = argc > 6 ? atoi(argv[6]) : 20;
		int flatwp = argc > 7 && strcmp(argv[7], "flat") == 0;
		const float DT = 0.0333f;
		int s, fi = 0; size_t ni; char path[512];
		for (s = 0; s < 240; s++) { memset(&in, 0, sizeof(in)); snes_menu_update(&menu, &in, 1.0f/60.0f); }
		for (ni = 0; ni < strlen(prefix); ni++) {   /* walk to the start state */
			memset(&in, 0, sizeof(in));
			if (prefix[ni]=='R') in.right=1; else if (prefix[ni]=='L') in.left=1;
			else if (prefix[ni]=='U') in.up=1; else if (prefix[ni]=='D') in.down=1;
			else if (prefix[ni]=='A') in.a=1; else if (prefix[ni]=='B') in.b=1;
			snes_menu_update(&menu, &in, 1.0f/60.0f);
			memset(&in, 0, sizeof(in));
			for (s = 0; s < 30; s++) snes_menu_update(&menu, &in, 1.0f/60.0f);
		}
		for (; fi < nframes; fi++) {
			memset(&in, 0, sizeof(in));
			if (fi >= 1 && (size_t)(fi-1) < strlen(tk)) {   /* press the transition key on its frame */
				char c = tk[fi-1];
				if (c=='R') in.right=1; else if (c=='L') in.left=1;
				else if (c=='U') in.up=1; else if (c=='D') in.down=1;
				else if (c=='A') in.a=1; else if (c=='B') in.b=1;
			}
			snes_menu_update(&menu, &in, DT);
			if (flatwp) { size_t k, nn = (size_t)WP_CACHE_W * WP_CACHE_H;
				for (k = 0; k < nn; k++) wp[k] = 0xFF204060u; menu.wp_ready = 1; }
			for (i = 0; i < W * H; i++) fb[i] = 0xFF000000u;
			snes_menu_render(&menu, &t);
			snprintf(path, sizeof(path), "%s/%03d.ppm", outdir, fi);
			f = fopen(path, "wb");
			if (f) {
				fprintf(f, "P6\n%d %d\n255\n", W, H);
				for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
					uint32_t p = fb[y*W+x];
					unsigned char rgb[3] = { (p>>16)&0xff, (p>>8)&0xff, p&0xff };
					fwrite(rgb, 1, 3, f);
				}
				fclose(f);
			}
		}
		fprintf(stderr, "seq wrote %d frames to %s\n", nframes, outdir);
		return 0;
	}

	/* Drive the nav sequence like real hardware: each key is pressed for one
	 * frame then released, followed by settle frames so animations/transitions
	 * complete and repeated same-keys still edge-trigger. `frames` is the
	 * settle count per key (and the final settle). */
	{
		int settle = frames > 0 ? frames : 30, j, s;
		size_t ni;
		memset(&in, 0, sizeof(in));
		for (s = 0; s < settle; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);
		for (ni = 0; ni < strlen(nav); ni++) {
			char c = nav[ni];
			memset(&in, 0, sizeof(in));
			if (c == 'L') in.left = 1; else if (c == 'R') in.right = 1;
			else if (c == 'U') in.up = 1; else if (c == 'D') in.down = 1;
			else if (c == 'A') in.a = 1; else if (c == 'B') in.b = 1;
			else if (c == 'S') in.select = 1; else if (c == 'T') in.start = 1;
			snes_menu_update(&menu, &in, 1.0f / 60.0f);   /* pressed 1 frame */
			memset(&in, 0, sizeof(in));
			for (j = 0; j < settle; j++) snes_menu_update(&menu, &in, 1.0f / 60.0f);
		}
	}
	/* Validation aid: with "flat" as the 5th arg, replace the scrolling
	 * wallpaper cache with a constant colour. The UI (opaque chrome, cards,
	 * text) renders identically either way, so diffing a normal render against
	 * a flat one yields a precise UI mask - letting the validator score only UI
	 * pixels and ignore the wallpaper, whose scroll phase is non-deterministic
	 * (time-based) and can never match a single web screenshot. */
	if (argc > 5 && strcmp(argv[5], "flat") == 0) {
		size_t n = (size_t)WP_CACHE_W * WP_CACHE_H, k;
		for (k = 0; k < n; k++) wp[k] = 0xFF204060u;
		menu.wp_ready = 1;
	}
	/* clear letterbox + render final frame */
	for (i = 0; i < W * H; i++) fb[i] = 0xFF000000u;
	snes_menu_render(&menu, &t);

	/* write PPM (P6) - fb is 0xAARRGGBB */
	f = fopen(outf, "wb");
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			uint32_t p = fb[y * W + x];
			unsigned char rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	fprintf(stderr, "wrote %s (%dx%d, %d games)\n", outf, W, H, pk.hdr->game_count);
	return 0;
}
