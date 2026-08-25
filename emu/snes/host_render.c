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
#include "snes_menu.h"

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
	chrome = malloc((size_t)SNES_VW * SNES_VH * 4);
	home_pool = malloc(sizeof(snes_rnode) * 4096);
	bg_pool = malloc(sizeof(snes_rnode) * 256);
	if (snes_menu_init(&menu, &pk, home_pool, 4096, bg_pool, 256, wp, chrome) != 0) {
		fprintf(stderr, "menu init failed\n"); return 1;
	}
	t.fb = fb; t.pitch = W; t.W = W; t.H = H;
	t.offx = (W - SNES_VW) / 2; t.offy = (H - SNES_VH) / 2;

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
