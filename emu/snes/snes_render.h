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
	/* per-view-group aspect transform applied to every blit's screen coords
	 * (X,Y): screen' = (vsx*X + vdx, vsy*Y + vdy). Defaults (1,1,0,0) = no-op so
	 * native 16:9 (letterboxed via offy) is unchanged. The 4:3 adaptation sets
	 * these per view group (top/bottom bars pinned, content zoomed, wallpaper
	 * filled, banner squashed). See snes_target_view(). */
	float     vsx, vsy, vdx, vdy;
} snes_target;

/* Set the per-view-group aspect transform on a target (see snes_target). Call
 * with (1,1,0,0) to reset to the native no-op. */
void snes_target_view(snes_target *t, float sx, float sy, float dx, float dy);

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
void snes_blit_spr_tint(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			float cx, float cy, float scale, float alpha,
			float tr, float tg, float tb);
void snes_blit_spr_tint_flip(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			     float cx, float cy, float scale, float alpha,
			     float tr, float tg, float tb, int hflip, int vflip);
void snes_blit_spr_wh_tint(snes_target *t, const snes_pack *pk, const snes_spr_entry *sp,
			   float cx, float cy, float dw, float dh, float alpha,
			   float tr, float tg, float tb);
void snes_blit_tex_tint(snes_target *t, const snes_pack *pk, const snes_img_entry *im,
			float cx, float cy, float w, float h, float alpha,
			float tr, float tg, float tb);
void snes_blit_raw(snes_target *t, const uint32_t *pix, int w, int h, float cx, float cy);
void snes_fill_quad(snes_target *t, float cx, float cy, float w, float h,
		    float r, float g, float b, float a);
/* draw text with a pack font (by font-name hash); align 0 left/1 centre/2 right. */
void snes_draw_text(snes_target *t, const snes_pack *pk, uint32_t font_hash,
		    float x, float y, float scale, uint32_t argb, int align,
		    const char *text);
/* measured advance width of a string in a font at a scale (screen px). */
float snes_text_width(const snes_pack *pk, uint32_t font_hash, float scale,
		      const char *text);

#endif
