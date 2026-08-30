/*
 * Host validation harness for the GBA carousel render. Compiles gba_menu.c's
 * pure gba_menu_render() against the real snes render engine, renders a frame to
 * a 1280x960 buffer, and writes it as PPM so the layout can be eyeballed off
 * device (which is usually unreachable). Build+run:
 *   gcc -O2 -I.. -I. host_render_gba.c ../gba_menu.c snes_pack.c snes_scene.c \
 *       snes_render.c -lm -o /tmp/gbarender && /tmp/gbarender <pack> <sel> <out.ppm>
 * The LK-only externs gba_menu_run references are stubbed below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "snes_pack.h"
#include "snes_render.h"
#include "../sd_fat.h"

extern void gba_menu_render(snes_target *t, const snes_pack *pk,
			    const gba_rom_entry *roms, int nrom, int sel, float posf,
			    float anim);

/* ---- stubs for the LK-only externs gba_menu.c declares (unused here) ---- */
unsigned int *ayaneo_canvas_back(unsigned int *p, unsigned int *w, unsigned int *h){(void)p;(void)w;(void)h;return 0;}
void ayaneo_canvas_present(void){}
unsigned menu_keys(void){return 0;}
void ayaneo_fill_blend(unsigned int*b,unsigned int p,int x,int y,int w,int h,unsigned int a,int al){(void)b;(void)p;(void)x;(void)y;(void)w;(void)h;(void)a;(void)al;}
void mtk_wdt_restart(void){}
void thread_sleep(unsigned m){(void)m;}
int zunzip(unsigned char*s,unsigned long*l,void*d,int dl,int o){(void)s;(void)l;(void)d;(void)dl;(void)o;return -1;}
int partition_read(const char*n,unsigned long long o,void*b,unsigned long l){(void)n;(void)o;(void)b;(void)l;return -1;}
unsigned char *gba_core_scratch_ptr(void){return 0;}
unsigned gba_core_scratch_size(void){return 0;}
int ayaneo_menu_audio_room(void){return 0;}
void ayaneo_menu_audio_submit(const short*s,unsigned f){(void)s;(void)f;}

int main(int argc, char **argv)
{
	const char *packf = argc > 1 ? argv[1] : "/tmp/gbamenu.pack";
	int sel = argc > 2 ? atoi(argv[2]) : 2;
	const char *out = argc > 3 ? argv[3] : "/tmp/gbamenu.ppm";
	FILE *f = fopen(packf, "rb");
	long n; void *blob; snes_pack pk;
	int W = 1280, H = 960; snes_target t;
	uint32_t *fb; int i;
	static gba_rom_entry roms[6];
	const char *names[6] = {
		"Pokemon Emerald Version.gba", "Stuck Pixel Test.gba",
		"The Legend of Zelda.gba", "Metroid Fusion.gba",
		"Advance Wars.gba", "Golden Sun.gba" };

	if (!f) { fprintf(stderr, "open %s\n", packf); return 1; }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	blob = malloc(n); if (fread(blob, 1, n, f) != (size_t)n) return 1; fclose(f);
	if (snes_pack_open(&pk, blob, n) != 0) { fprintf(stderr, "pack open failed\n"); return 1; }

	for (i = 0; i < 6; i++) { strncpy(roms[i].name, names[i], 127); roms[i].size = 1<<20; }

	fb = calloc((size_t)W * H, 4);
	t.fb = fb; t.pitch = W; t.W = W; t.H = H;
	t.offx = (W - SNES_VW) / 2; t.offy = (H - SNES_VH) / 2;

	gba_menu_render(&t, &pk, roms, 6, sel, (float)sel, 1.5f);

	f = fopen(out, "wb");
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (i = 0; i < W * H; i++) {
		uint32_t px = fb[i];                    /* 0xAARRGGBB */
		unsigned char rgb[3] = { (px>>16)&0xff, (px>>8)&0xff, px&0xff };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	printf("wrote %s (%dx%d), pack ok, sel=%d\n", out, W, H, sel);
	return 0;
}
