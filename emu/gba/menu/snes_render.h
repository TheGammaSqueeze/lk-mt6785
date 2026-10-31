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
	/* Native resolution scale: the whole design (after the view transform) is scaled by
	 * base into the framebuffer, screen'' = base * (vsx*X + vdx). 0 or 1 = design pixels
	 * (pairmini, 1280 wide); 0.75 draws the 1280 wide design natively on a 960 wide canvas
	 * (Konkr) with no post pass. Caches built through a target inherit its base. */
	float     base;
} snes_target;
static inline float snes_target_base(const snes_target *t) { return (t->base > 0.0f) ? t->base : 1.0f; }
/* design width in framebuffer pixels for this target */
static inline int snes_target_vw(const snes_target *t) { return (int)((float)SNES_VW * snes_target_base(t) + 0.5f); }
static inline int snes_target_vh(const snes_target *t) { return (int)((float)SNES_VH * snes_target_base(t) + 0.5f); }
/* Which layout a W x H target gets: 1 = 4:3, 2 = 3:2, 0 = the native 16:9 design letterboxed. */
static inline int snes_aspect_for(int W, int H)
{
	int r = H > 0 ? W * 1000 / H : 1778;
	if (r > 1318 && r < 1348) return 1;
	if (r > 1485 && r < 1515) return 2;
	return 0;
}
/* Point a target at a frame buffer and derive the native scale from its width: the design is
 * authored at 1280 wide, so a 960 wide canvas renders everything at 0.75 with no later resize.
 * The design's 720 rows are centred (offy) and the identity view is set. */
static inline void snes_target_fit(snes_target *t, uint32_t *fb, int pitch, int W, int H)
{
	t->fb = fb; t->pitch = pitch; t->W = W; t->H = H;
	t->base = (W > 0 && W != SNES_VW) ? (float)W / (float)SNES_VW : 1.0f;
	t->offx = (W - snes_target_vw(t)) / 2; t->offy = (H - snes_target_vh(t)) / 2;
	t->vsx = 1.0f; t->vsy = 1.0f; t->vdx = 0.0f; t->vdy = 0.0f;
}

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
void snes_blit_raw(snes_target *t, const uint32_t *pix, int w, int h, float cx, float cy, float dim);
/* same with an explicit source pitch (pixels) and dest size w x h already in fb pixels */
void snes_blit_raw_p(snes_target *t, const uint32_t *pix, int pitch, int w, int h, float cx, float cy, float dim);
/* Same, source-over with a GLOBAL alpha on top of the tile's own alpha (crossfades between two
 * cached tiles of identical silhouette: dark tile first, then the active tile at alpha). */
void snes_blit_raw_pa(snes_target *t, const uint32_t *pix, int pitch, int w, int h, float cx, float cy, float dim, float alpha);
/* Source-over composite of a full-fb-size straight-RGBA overlay (same pitch/offx/offy
 * as t, rendered 1:1) onto the target, rows [y0,y1) only. Blits a pre-rendered static
 * overlay (e.g. the resume panel) far cheaper than re-walking its scene subtree. */
void snes_composite(snes_target *t, const uint32_t *ov, int y0, int y1);
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
