#ifndef SNES_RENDER_H
#define SNES_RENDER_H

#include "snes_scene.h"

/* CLOVER virtual render space (matches the web renderer). */
#define SNES_VW 1280
#define SNES_VH 720

typedef struct {
	uint32_t *fb;        /* BGRA8888 (stored as 0xAARRGGBB u32) */
	unsigned  pitch;     /* pixels per row */
	int       W, H;      /* framebuffer size */
	int       offx, offy;/* top-left of the virtual 1280x720 area in the fb */
} snes_target;

/* Draw a whole scene into the target (does not clear). */
void snes_render_scene(snes_target *t, snes_scene *s);
/* Draw the subtree rooted at n at its authored world position. */
void snes_render_node(snes_target *t, snes_scene *s, snes_rnode *n);

/* number of drawables emitted by the last snes_render_scene (diagnostic). */
int snes_render_count(void);

/* diagnostic test quads through the blitter (coordinate calibration). */
void snes_render_debug_markers(snes_target *t);

/* direct-draw helpers (screen space; cx,cy = centre in the 1280x720 virtual space) */
void snes_blit_tex(snes_target *t, const snes_pack *pk, const snes_img_entry *im,
		   float cx, float cy, float w, float h, float alpha);
void snes_blit_spr(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
		   float cx, float cy, float scale, float alpha);
void snes_fill_quad(snes_target *t, float cx, float cy, float w, float h,
		    float r, float g, float b, float a);
/* draw text with a pack font (by font-name hash); align 0 left/1 centre/2 right. */
void snes_draw_text(snes_target *t, const snes_pack *pk, uint32_t font_hash,
		    float x, float y, float scale, uint32_t argb, int align,
		    const char *text);

#endif
