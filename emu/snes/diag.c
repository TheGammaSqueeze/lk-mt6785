#include <stdio.h>
#include <stdlib.h>
#include "snes_menu.h"

static void names(snes_scene *s, snes_rnode *n, int depth)
{
	int i;
	const char *nm = snes_str(s->pk, n->def->name);
	for (i = 0; i < depth; i++) fputs("  ", stderr);
	fprintf(stderr, "%s (comps=%u en=%d)\n", nm ? nm : "?", n->def->comp_count, n->enabled);
	{ snes_rnode *ch; for (ch = n->child; ch; ch = ch->sib) names(s, ch, depth + 1); }
}

int main(int argc, char **argv)
{
	const char *packf = argc > 1 ? argv[1] : "out/snes_pack.bin";
	FILE *f = fopen(packf, "rb");
	long len; void *blob;
	snes_pack pk; snes_menu menu;
	uint32_t *wp; snes_rnode *home_pool, *bg_pool;
	unsigned i;
	if (!f) { fprintf(stderr, "open failed\n"); return 1; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	blob = malloc(len); if (fread(blob, 1, len, f) != (size_t)len) return 1; fclose(f);
	if (snes_pack_open(&pk, blob, len) != 0) { fprintf(stderr, "bad pack\n"); return 1; }
	fprintf(stderr, "scenes: %u\n", pk.hdr->scene_count);
	for (i = 0; i < pk.hdr->scene_count; i++)
		fprintf(stderr, "  scene[%u] = %s (nodes=%u)\n", i,
			snes_str(&pk, pk.scene[i].name), pk.scene[i].node_count);
	wp = malloc((size_t)WP_CACHE_W * WP_CACHE_H * 4);
	home_pool = malloc(sizeof(snes_rnode) * 4096);
	bg_pool = malloc(sizeof(snes_rnode) * 256);
	if (snes_menu_init(&menu, &pk, home_pool, 4096, bg_pool, 256, wp, malloc((size_t)SNES_VW*SNES_VH*4), malloc((size_t)SNES_VW*SNES_VH*4)) != 0) {
		fprintf(stderr, "init failed\n"); return 1;
	}
	fprintf(stderr, "--- HOME scene tree ---\n");
	names(&menu.home, menu.home.root, 0);
	return 0;
}
