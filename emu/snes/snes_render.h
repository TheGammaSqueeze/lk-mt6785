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
	/* Optional ABSOLUTE-framebuffer scanline band clip [band_y0, band_y1) in
	 * panel rows (already including offy). When band_y1 > band_y0 the blitter
	 * writes only rows in [band_y0, band_y1); a core renders the FULL z-sorted
	 * draw list but only touches its own disjoint horizontal strip. band_y1==0
	 * (the default, set by snes_target_view / zero-init) means "no band clip"
	 * (whole framebuffer). Used by the AYANEO_BIGCORE_EXPT multicore split so N
	 * cores partition the framebuffer into cache-line-disjoint bands. */
	int       band_y0, band_y1;
	/* OVL hardware-layering support (state-0 idle home 60fps path, OVL_LAYERS.md).
	 * cache_layer: 1 = render into a PREMULTIPLIED-alpha layer buffer (source-over,
	 * preserves alpha) instead of onto an opaque framebuffer; used to build the
	 * cursorless card-strip cache (later un-premultiplied to straight alpha for the
	 * OVL). ovl_split: 1 = this is the L0 (framebuffer) pass of a layered render, so
	 * snes_menu_render SKIPS the card bodies and the focus/slide cursor (they are
	 * composited by the OVL from the L2/L3 layers instead). */
	int       cache_layer;
	int       ovl_split;
	/* l3_focus: 1 = this cache_layer pass is the L3 focus overlay, so draw_card
	 * renders the FULL focused card with the BLUE active frame (over the L2 dark
	 * body). Keeps the focus colour off L2 so dead-zone walks never rebuild it. */
	int       l3_focus;
} snes_target;

/* Restrict a target to an absolute framebuffer scanline band [y0, y1) (panel
 * rows). Pass (0,0) to clear the band (whole framebuffer). */
void snes_target_band(snes_target *t, int y0, int y1);

/* Set the per-view-group aspect transform on a target (see snes_target). Call
 * with (1,1,0,0) to reset to the native no-op. */
void snes_target_view(snes_target *t, float sx, float sy, float dx, float dy);

/* Draw a whole scene into the target (does not clear). */
void snes_render_scene(snes_target *t, snes_scene *s);
/* Draw the subtree rooted at n at its authored world position. */
void snes_render_node(snes_target *t, snes_scene *s, snes_rnode *n);

/* Per-core render scratch (z-sort + draw list). Opaque; size lives in the .c.
 * Multicore only: each core owns one, so collect/sort never share mutable state.
 * The single-core path uses a private file-scope instance instead. */
typedef struct snes_render_ctx snes_render_ctx;
/* Explicit-context variants of the two entry points above: collect into `ctx`
 * (its own private z-sort + draw list) then paint into `t` honouring t's band
 * clip. Call from a secondary core with its own ctx + a disjoint band. */
void snes_render_scene_ctx(snes_render_ctx *ctx, snes_target *t, snes_scene *s);
void snes_render_node_ctx(snes_render_ctx *ctx, snes_target *t, snes_scene *s,
			  snes_rnode *n);

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
