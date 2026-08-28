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

int main(int argc, char **argv)
{
	const char *packf = argc > 1 ? argv[1] : "out/snes_pack.bin";
	const char *outf  = argc > 2 ? argv[2] : "/tmp/snes_host.ppm";
	int frames = argc > 3 ? atoi(argv[3]) : 60;
	const char *nav = argc > 4 ? argv[4] : "";
	FILE *f = fopen(packf, "rb");
	long len; void *blob;
	snes_pack pk; snes_menu menu; snes_target t = {0}; snes_input in;
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
	menu.fcc = calloc((size_t)W * H, 4);   /* focused-card body cache (panel-sized) */
	menu.fcc_ready = 0;
	t.fb = fb; t.pitch = W; t.W = W; t.H = H;
	t.offx = (W - SNES_VW) / 2; t.offy = (H - SNES_VH) / 2;
	snes_target_view(&t, 1.0f, 1.0f, 0.0f, 0.0f);
	if (getenv("SNES_ASPECT43")) { menu.aspect = 1; menu.chrome_ready = 0; }  /* rebuild chrome for 4:3 */

	/* Split-build validation: `host_render <pack> - vsplit` proves the 2-core card-strip
	 * split is exact at ARBITRARY split rows - a full snes_menu_build_cardcache must equal two
	 * disjoint band builds ([0,mid)+[mid,H)) pixel for pixel for every mid. Exactness at any
	 * mid is what lets the driver pick a CONTENT-BALANCED split (mid at the middle of the
	 * non-empty band) so cpu0 and cpu1 do equal work, which is what actually lets the clock
	 * drop for the low-power 2-core steady state. Reports the content band + per-split balance. */
	if (argc > 3 && strcmp(argv[3], "vsplit") == 0) {
		int LW = SNES_L2_W, LH = SNES_L2_BAND_H, s, allbad = 0, cy0, cy1, cmid, si;
		uint32_t *A = calloc((size_t)LW * LH, 4);
		uint32_t *B = calloc((size_t)LW * LH, 4);
		snes_target ct = {0};
		int splits[5];
		memset(&in, 0, sizeof(in));
		for (s = 0; s < 240; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);   /* settle attract */
		ct.pitch = LW; ct.W = LW; ct.H = LH;
		ct.offx = SNES_L2_MARGIN;
		ct.offy = ((H - SNES_VH) / 2) - SNES_L2_BAND_Y0;
		snes_target_view(&ct, 1.0f, 1.0f, 0.0f, 0.0f);
		ct.fb = A;
		snes_menu_build_cardcache(&menu, &ct);                    /* full (untouched path) */
		cy0 = menu.cc_y0; cy1 = menu.cc_y1;                       /* non-empty content band */
		cmid = (cy0 + cy1) / 2;                                   /* content-balanced split row */
		printf("VSPLIT: content band rows [%d,%d] (%d non-empty of %d); buffer-mid=%d content-mid=%d\n",
		       cy0, cy1, cy1 - cy0 + 1, LH, LH / 2, cmid);
		/* test exactness at buffer-mid, content-mid, and asymmetric points */
		splits[0] = LH / 2; splits[1] = cmid; splits[2] = LH / 4; splits[3] = (3 * LH) / 4; splits[4] = 1;
		for (si = 0; si < 5; si++) {
			int mid = splits[si], bad = 0, first = -1, top, bot;
			for (i = 0; i < LW * LH; i++) B[i] = 0xDEADBEEF;      /* poison to catch unwritten rows */
			ct.fb = B;
			snes_menu_build_cardcache_band(&menu, &ct, 0, mid);
			snes_menu_build_cardcache_band(&menu, &ct, mid, LH);
			for (i = 0; i < LW * LH; i++)
				if (A[i] != B[i]) { bad++; if (first < 0) first = i; }
			if (bad) allbad++;
			/* per-core balance = non-empty rows each side of the split */
			top = (cy1 < mid ? cy1 : mid - 1) - cy0 + 1; if (top < 0) top = 0;
			bot = cy1 - (cy0 > mid ? cy0 : mid) + 1;     if (bot < 0) bot = 0;
			printf("  mid=%4d: %s  (cpu0 rows=%d, cpu1 rows=%d)%s\n",
			       mid, bad ? "FAIL" : "PASS", top, bot,
			       bad ? "" : (si == 1 ? "  <- content-balanced" : ""));
			if (bad)
				printf("    first diff @ row %d col %d: full=0x%08x split=0x%08x\n",
				       first / LW, first % LW, A[first], B[first]);
		}
		printf("VSPLIT: %s\n", allbad ? "FAIL (split is NOT exact at some point)"
		                              : "PASS (split exact at every point; content-balanced split is safe)");
		return allbad ? 2 : 0;
	}

	/* Per-frame render-split validation: `host_render <pack> - rsplit [nav]` proves the
	 * PRIMARY low-power lever - the band split of snes_menu_render that bc_dispatch runs every
	 * frame - is pixel-exact vs a single full render, at several settled states along a nav
	 * path. snes_menu_render is more complex than the strip build (background + cards + cursor +
	 * transitions), so a seam bug here is both more likely and more visible; catch it on host
	 * before the coherency work enables the real 2-core render. Also checks render determinism. */
	if (argc > 3 && strcmp(argv[3], "rsplit") == 0) {
		const char *rnav = argc > 4 ? argv[4] : "RRRLLDDUU";
		int mid = (H / 2) & ~15, s, ni, det = 0, allbad = 0, checks = 0;
		uint32_t *A = calloc((size_t)W * H, 4);
		uint32_t *A2 = calloc((size_t)W * H, 4);
		uint32_t *B = calloc((size_t)W * H, 4);
		memset(&in, 0, sizeof(in));
		for (s = 0; s < 240; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);   /* settle attract */
		for (ni = 0; ni <= (int)strlen(rnav); ni++) {
			int bad = 0, first = -1, d2 = 0;
			/* full render twice (determinism), then two band halves */
			t.fb = A;  for (i = 0; i < W * H; i++) A[i]  = 0xFF000000u; snes_target_band(&t, 0, 0); snes_menu_render(&menu, &t);
			t.fb = A2; for (i = 0; i < W * H; i++) A2[i] = 0xFF000000u; snes_target_band(&t, 0, 0); snes_menu_render(&menu, &t);
			for (i = 0; i < W * H; i++) if (A[i] != A2[i]) d2++;
			t.fb = B;  for (i = 0; i < W * H; i++) B[i]  = 0xFF000000u;
			snes_target_band(&t, 0, mid); snes_menu_render(&menu, &t);
			snes_target_band(&t, mid, H); snes_menu_render(&menu, &t);
			snes_target_band(&t, 0, 0);
			for (i = 0; i < W * H; i++) if (A[i] != B[i]) { bad++; if (first < 0) first = i; }
			checks++; if (bad) allbad++; if (d2) det++;
			printf("  state %2d (after '%.*s'): split %s (%d diff), determinism %s (%d diff)%s\n",
			       ni, ni, rnav, bad ? "FAIL" : "PASS", bad, d2 ? "FAIL" : "OK", d2,
			       bad ? "" : "");
			if (bad)
				printf("    first diff @ row %d col %d: full=0x%08x split=0x%08x\n",
				       first / W, first % W, A[first], B[first]);
			/* advance to the next state by pressing the next nav key (like real HW) */
			if (ni < (int)strlen(rnav)) {
				char c = rnav[ni];
				memset(&in, 0, sizeof(in));
				in.right = (c == 'R'); in.left = (c == 'L'); in.up = (c == 'U'); in.down = (c == 'D');
				in.a = (c == 'A'); in.b = (c == 'B');
				snes_menu_update(&menu, &in, 1.0f / 60.0f);
				memset(&in, 0, sizeof(in));
				for (s = 0; s < 40; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);   /* settle */
			}
		}
		printf("RSPLIT: %s (%d states, mid=%d; %d split-fail, %d nondeterministic)\n",
		       (allbad || det) ? "FAIL" : "PASS", checks, mid, allbad, det);
		return (allbad || det) ? 2 : 0;
	}

	/* State-pack completeness: `host_render <pack> - statepack [nav]` proves the
	 * SNES_STATE_NWORDS packed fields FULLY determine the home-carousel (state==0) render -
	 * the correctness prerequisite for the MMIO 2-core split (cpu0 packs+publishes these
	 * words, the worker unpacks into its snapshot and renders its band). Method per state:
	 * render S1 -> A, pack S1; navigate to S2 (perturbs the dynamic fields); unpack S1 back;
	 * re-render -> B. A==B means the pack captured every render-relevant dynamic field; a
	 * diff means a field is MISSING from SNES_STATE_FIELDS (the pixel points at which). */
	if (argc > 3 && strcmp(argv[3], "statepack") == 0) {
		const char *rnav = argc > 4 ? argv[4] : "RRLLDDUURRL";
		int s, ni, allbad = 0, checks = 0, skipped = 0;
		uint32_t *A = calloc((size_t)W * H, 4);
		uint32_t *B = calloc((size_t)W * H, 4);
		uint32_t buf[SNES_STATE_NWORDS];
		memset(&in, 0, sizeof(in));
		for (s = 0; s < 240; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);   /* settle attract */
		for (ni = 0; ni < (int)strlen(rnav); ni++) {
			char c = rnav[ni];
			int bad = 0, first = -1;
			if (menu.state != 0) { skipped++; }   /* split only targets the home carousel */
			t.fb = A; for (i = 0; i < W * H; i++) A[i] = 0xFF000000u; snes_target_band(&t, 0, 0); snes_menu_render(&menu, &t);
			snes_menu_pack_state(&menu, buf);      /* capture S1 dynamic state */
			/* perturb to S2 */
			memset(&in, 0, sizeof(in));
			in.right = (c == 'R'); in.left = (c == 'L'); in.up = (c == 'U'); in.down = (c == 'D'); in.a = (c == 'A'); in.b = (c == 'B');
			snes_menu_update(&menu, &in, 1.0f / 60.0f);
			memset(&in, 0, sizeof(in));
			for (s = 0; s < 40; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);
			snes_menu_unpack_state(&menu, buf);    /* restore S1 dynamic (over S2) */
			t.fb = B; for (i = 0; i < W * H; i++) B[i] = 0xFF000000u; snes_target_band(&t, 0, 0); snes_menu_render(&menu, &t);
			for (i = 0; i < W * H; i++) if (A[i] != B[i]) { bad++; if (first < 0) first = i; }
			checks++; if (bad && menu.state == 0) allbad++;
			printf("  state %2d (nav '%c', state=%d): restore %s (%d px differ)%s\n",
			       ni, c, menu.state, bad ? "FAIL" : "PASS", bad,
			       (bad && menu.state != 0) ? " [non-home; ignored]" : "");
			if (bad && first >= 0)
				printf("    first diff @ row %d col %d: A=0x%08x B=0x%08x\n", first / W, first % W, A[first], B[first]);
			/* advance to S2 for the next iteration */
			memset(&in, 0, sizeof(in));
			in.right = (c == 'R'); in.left = (c == 'L'); in.up = (c == 'U'); in.down = (c == 'D'); in.a = (c == 'A'); in.b = (c == 'B');
			snes_menu_update(&menu, &in, 1.0f / 60.0f);
			memset(&in, 0, sizeof(in));
			for (s = 0; s < 40; s++) snes_menu_update(&menu, &in, 1.0f / 60.0f);
		}
		printf("STATEPACK: %s (%d home-state checks, %d words/frame, %d non-home skipped)\n",
		       allbad ? "FAIL - a render-relevant dynamic field is MISSING from SNES_STATE_FIELDS"
		              : "PASS - the packed fields fully determine the home-carousel render (split is correct)",
		       checks - skipped, SNES_STATE_NWORDS, skipped);
		return allbad ? 2 : 0;
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

	/* Multicore band-split verification: with "split" as the 5th arg, render the
	 * SAME frame in two scanline bands into a second buffer and byte-compare to
	 * the whole-frame render above. This proves the AYANEO_BIGCORE_EXPT split is
	 * output-correct (union of the per-core bands == the single-core frame) on
	 * the host, without needing the real second core. Exit code = mismatch count
	 * clamped to 0/1. */
	if (argc > 5 && strcmp(argv[5], "split") == 0) {
		uint32_t *fb2 = calloc((size_t)W * H, 4);
		int sy = (H / 2) & ~15;   /* split on a 16px (>=64B) boundary */
		snes_target ta = t, tb = t;
		long diff = 0; int k;
		/* Flatten the wallpaper so the frame is DETERMINISTIC across the three
		 * render calls below (the scrolling wallpaper phase would otherwise
		 * advance per snes_menu_render call and desync full-vs-split - a test
		 * artifact, not a split bug). On device each frame renders once. */
		{ size_t wn = (size_t)WP_CACHE_W * WP_CACHE_H, wk;
		  for (wk = 0; wk < wn; wk++) wp[wk] = 0xFF204060u; menu.wp_ready = 1; }
		for (i = 0; i < W * H; i++) fb[i] = 0xFF000000u;
		snes_menu_render(&menu, &t);          /* re-render the full frame, flat wp */
		for (i = 0; i < W * H; i++) fb2[i] = 0xFF000000u;
		/* Isolation A: a SINGLE full-covering band [0,H] must equal the no-band
		 * render (proves the band-clip code path itself does not alter output). */
		{
			uint32_t *fbf = calloc((size_t)W * H, 4); long d2 = 0; int kk;
			snes_target tf = t; tf.fb = fbf;
			for (kk = 0; kk < W * H; kk++) fbf[kk] = 0xFF000000u;
			snes_target_band(&tf, 0, H);
			snes_menu_render(&menu, &tf);
			for (kk = 0; kk < W * H; kk++) if (fb[kk] != fbf[kk]) d2++;
			fprintf(stderr, "  [isolation] full-band[0,H] vs no-band: %ld px differ -> %s\n",
				d2, d2 == 0 ? "clip code OK" : "CLIP CODE BUG");
			free(fbf);
		}
		ta.fb = fb2; snes_target_band(&ta, 0, sy);
		snes_menu_render(&menu, &ta);        /* top band */
		tb.fb = fb2; snes_target_band(&tb, sy, H);
		snes_menu_render(&menu, &tb);        /* bottom band */
		{
			int minx = W, miny = H, maxx = -1, maxy = -1, xx, yy;
			for (yy = 0; yy < H; yy++) for (xx = 0; xx < W; xx++)
				if (fb[yy * W + xx] != fb2[yy * W + xx]) {
					diff++;
					if (xx < minx) minx = xx; if (xx > maxx) maxx = xx;
					if (yy < miny) miny = yy; if (yy > maxy) maxy = yy;
				}
			fprintf(stderr, "SPLIT verify: %ld/%d px differ (cut y=%d) bbox x[%d..%d] y[%d..%d] -> %s\n",
				diff, W * H, sy, minx, maxx, miny, maxy,
				diff == 0 ? "IDENTICAL" : "MISMATCH");
			{ int shown = 0;
			  for (yy = 0; yy < H && shown < 6; yy++) for (xx = 0; xx < W && shown < 6; xx++)
				if (fb[yy*W+xx] != fb2[yy*W+xx]) {
					fprintf(stderr, "  (%d,%d) full=%08x split=%08x\n",
						xx, yy, fb[yy*W+xx], fb2[yy*W+xx]); shown++;
				}
			}
		}
		free(fb2);
		return diff == 0 ? 0 : 1;
	}

	/* OVL layering verification: `host_render <pack> <outdir> N nav layers` renders
	 * the SAME state two ways and byte-compares them:
	 *   reference = the normal single-buffer render (already in fb, above)
	 *   layered   = L0 (framebuffer, cards+cursor skipped) + L2 (cursorless card
	 *               cache, straight alpha) + L3 (live cursor, straight alpha),
	 *               software-composited with the SAME straight-alpha source-over the
	 *               MT6785 OVL performs. Pixel-identity (modulo AA-edge rounding from
	 *               the premult build + un-premult round-trip) proves the split is
	 *               output-correct before any device flash. Writes ref/comp/diff PPMs. */
	if (argc > 5 && strcmp(argv[5], "layers") == 0) {
		const char *outdir = argv[2];
		uint32_t *l0 = calloc((size_t)W * H, 4), *cc = calloc((size_t)SNES_L2_W * SNES_L2_BAND_H, 4);
		uint32_t *cur = calloc((size_t)W * H, 4), *comp = calloc((size_t)W * H, 4);
		snes_target t0 = t, t2 = t, t3 = t;
		long diff = 0; int minx = W, miny = H, maxx = -1, maxy = -1, xx, yy;
		char path[512]; FILE *pf;
		/* Emulate the OVL src_x pan: the L2 layer is built SETTLED (cont_shift=0) once,
		 * then panned by the live cont_shift. A panel pixel (x,y) reads L2 buffer pixel
		 * (pan + x, y - BAND_Y0) with pan = MARGIN - round(cont_shift*viewscale). */
		float vscale = menu.aspect ? 1.18519f : 1.0f;   /* ASP_CONTENT_S */
		int pan = SNES_L2_MARGIN - (int)(menu.cont_shift * vscale + (menu.cont_shift >= 0 ? 0.5f : -0.5f));
		fprintf(stderr, "  [pan] cont_shift=%.1f sel_world=%.1f pan=%d ngames=%d focus=%d\n", menu.cont_shift, menu.sel_world, pan, menu.ngames, menu.focus);
		/* Quantise cont_shift to the INTEGER OVL src_x (mirrors the driver). The OVL
		 * pans the strip in whole panel pixels, so an integer src_x preserves the
		 * settled buffer's sub-pixel phase and the panned boxart stays crisp; the
		 * carousel moves in ~1px steps (<=0.5px from the smooth ideal, imperceptible
		 * under motion). Render the focused card (L3) AND the single-buffer REFERENCE
		 * at this same quantised shift, so the correctness test is "layered composite
		 * == single-buffer render of the SAME quantised frame" (not of the smooth
		 * ideal, which the integer pan intentionally approximates). */
		menu.cont_shift = (float)(SNES_L2_MARGIN - pan) / vscale;
		for (i = 0; i < W * H; i++) fb[i] = 0xFF000000u;
		snes_menu_render(&menu, &t);   /* re-render the reference at the quantised shift */
		/* L0: everything except the card bodies + focus/slide cursor */
		for (i = 0; i < W * H; i++) l0[i] = 0xFF000000u;
		t0.fb = l0; t0.ovl_split = 1;
		snes_menu_render(&menu, &t0);
		/* L2: SETTLED cursorless card strip in the WIDE band buffer */
		t2.fb = cc; t2.W = SNES_L2_W; t2.H = SNES_L2_BAND_H; t2.pitch = SNES_L2_W;
		t2.offx = SNES_L2_MARGIN;
		t2.offy = (menu.aspect ? 0 : (H - SNES_VH) / 2) - SNES_L2_BAND_Y0;
		t2.cache_layer = 0;
		snes_menu_build_cardcache(&menu, &t2);
		if (getenv("SNES_TIMECC")) {
			struct timespec ta, tb; int rep = 200, q;
			t3.fb = cur;
			clock_gettime(CLOCK_MONOTONIC, &ta);
			for (q = 0; q < rep; q++) snes_menu_build_cardcache(&menu, &t2);
			clock_gettime(CLOCK_MONOTONIC, &tb);
			double us = ((tb.tv_sec-ta.tv_sec)*1e9 + (tb.tv_nsec-ta.tv_nsec))/1e3/rep;
			fprintf(stderr, "  [TIMECC] build_cardcache = %.1f us/call (host)\n", us);
			clock_gettime(CLOCK_MONOTONIC, &ta);
			for (q = 0; q < rep; q++) snes_menu_render_cursor_layer(&menu, &t3, 0);
			clock_gettime(CLOCK_MONOTONIC, &tb);
			us = ((tb.tv_sec-ta.tv_sec)*1e9 + (tb.tv_nsec-ta.tv_nsec))/1e3/rep;
			fprintf(stderr, "  [TIMECC] render_cursor_layer = %.1f us/call (host)\n", us);
		}
		{ /* L2 buffer card-center columns (buffer x) + their equivalent panel x at this pan */
			int bx, byy, on, run0 = -1; char msg[512]; int ml = 0;
			ml += snprintf(msg+ml, sizeof(msg)-ml, "  [L2buf] card cols(bufx->panelx):");
			for (bx = 0; bx < SNES_L2_W; bx++) {
				on = 0;
				for (byy = 40; byy < 340; byy++) if (cc[(size_t)byy*SNES_L2_W+bx] >> 24) { on = 1; break; }
				if (on && run0 < 0) run0 = bx;
				else if (!on && run0 >= 0) {
					if (bx - run0 > 30) { int c = (run0+bx)/2; ml += snprintf(msg+ml, sizeof(msg)-ml, " %d->%d", c, c - pan); }
					run0 = -1;
				}
			}
			fprintf(stderr, "%s\n", msg);
		}
		/* L3: live selection cursor (straight alpha) */
		t3.fb = cur;
		snes_menu_render_cursor_layer(&menu, &t3, 0);
		/* composite L0 + L2(panned band) + L3 with straight (coverage) source-over */
		for (yy = 0; yy < H; yy++) for (xx = 0; xx < W; xx++) {
			int by = yy - SNES_L2_BAND_Y0, bx = pan + xx;
			uint32_t bs, c2 = 0, c3;
			unsigned r, g, b, a, ia;
			i = yy * W + xx;
			bs = l0[i]; c3 = cur[i];
			r = (bs >> 16) & 0xff; g = (bs >> 8) & 0xff; b = bs & 0xff;
			if (by >= 0 && by < SNES_L2_BAND_H && bx >= 0 && bx < SNES_L2_W)
				c2 = cc[(size_t)by * SNES_L2_W + bx];
			a = c2 >> 24; if (a) { ia = 255 - a;
				r = (((c2 >> 16) & 0xff) * a + r * ia + 127) / 255;
				g = (((c2 >> 8) & 0xff) * a + g * ia + 127) / 255;
				b = ((c2 & 0xff) * a + b * ia + 127) / 255; }
			a = c3 >> 24; if (a) { ia = 255 - a;
				r = (((c3 >> 16) & 0xff) * a + r * ia + 127) / 255;
				g = (((c3 >> 8) & 0xff) * a + g * ia + 127) / 255;
				b = ((c3 & 0xff) * a + b * ia + 127) / 255; }
			comp[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
		/* diff comp vs reference fb, tracking bbox + worst 40x40 tile mean */
		{
			int tx, ty, wtx = -1, wty = -1; double wtmean = 0;
			for (i = 0; i < W * H; i++) {
				uint32_t p = comp[i], q = fb[i];
				int dr = ((p>>16)&0xff)-((q>>16)&0xff), dg = ((p>>8)&0xff)-((q>>8)&0xff), db = (p&0xff)-(q&0xff);
				if (dr||dg||db) { diff++; xx = i % W; yy = i / W;
					if (xx<minx)minx=xx; if (xx>maxx)maxx=xx; if (yy<miny)miny=yy; if (yy>maxy)maxy=yy; }
			}
			for (ty = 0; ty < H; ty += 40) for (tx = 0; tx < W; tx += 40) {
				double s = 0; int cnt = 0;
				for (yy = ty; yy < ty+40 && yy < H; yy++) for (xx = tx; xx < tx+40 && xx < W; xx++) {
					uint32_t p = comp[yy*W+xx], q = fb[yy*W+xx];
					int dr = ((p>>16)&0xff)-((q>>16)&0xff), dg = ((p>>8)&0xff)-((q>>8)&0xff), db = (p&0xff)-(q&0xff);
					s += (dr<0?-dr:dr) + (dg<0?-dg:dg) + (db<0?-db:db); cnt++;
				}
				if (cnt && s/cnt > wtmean) { wtmean = s/cnt; wtx = tx; wty = ty; }
			}
			/* the device only layers in the steady home state; a mismatch in any
			 * other state is a test artifact (the device renders it single-buffer). */
			int layerable = (menu.state == 0 && menu.open_y == 0.0f && !menu.closing);
			fprintf(stderr, "LAYERS verify (%s) state=%d slide=%.2f%s: %ld/%d px differ  bbox x[%d..%d] y[%d..%d]  band y[%d..%d]  worstTile(%d,%d) mean=%.2f -> %s\n",
				menu.aspect ? "4:3" : "16:9", menu.state, menu.cur_slide_t,
				layerable ? " LAYERABLE" : " (not layered on device)",
				diff, W*H, minx, maxx, miny, maxy,
				menu.cc_y0, menu.cc_y1, wtx, wty, wtmean,
				diff == 0 ? "IDENTICAL" : (!layerable ? "n/a (non-layered state)" : (wtmean < 6.0 ? "OK (edge rounding only)" : "MISMATCH")));
			{ int shown = 0;
			  for (i = 0; i < W*H && shown < 6; i++) if (comp[i] != fb[i]) {
				fprintf(stderr, "  (%d,%d) ref=%08x comp=%08x  L0=%08x L2=%08x L3=%08x\n",
					(int)(i%W), (int)(i/W), fb[i], comp[i], l0[i], cc[i], cur[i]); shown++; }
			}
		}
		#define WRITE_PPM(nm, buf) do { snprintf(path, sizeof(path), "%s/%s.ppm", outdir, nm); \
			pf = fopen(path, "wb"); if (pf) { fprintf(pf, "P6\n%d %d\n255\n", W, H); \
			for (yy = 0; yy < H; yy++) for (xx = 0; xx < W; xx++) { uint32_t p = (buf)[yy*W+xx]; \
			unsigned char rgb[3] = { (p>>16)&0xff, (p>>8)&0xff, p&0xff }; fwrite(rgb,1,3,pf); } fclose(pf); } } while (0)
		WRITE_PPM("ref", fb); WRITE_PPM("comp", comp); WRITE_PPM("l0", l0); WRITE_PPM("l2", cc); WRITE_PPM("l3", cur);
		#undef WRITE_PPM
		return diff == 0 ? 0 : 1;
	}

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
