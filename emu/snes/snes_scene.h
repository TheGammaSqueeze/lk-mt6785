/*
 * Runtime scene graph built from a packed SceneEntry. Nodes carry mutable
 * transform/color/visibility (driven by the animator and the menu logic) and a
 * small set of per-node overrides for their drawable component (sprite swap, uv
 * scroll, size tween). Allocated from a bump arena; never freed piecemeal.
 */
#ifndef SNES_SCENE_H
#define SNES_SCENE_H

#include "snes_pack.h"

/* overrides applied to a node's primary drawable component at draw time */
typedef struct {
	unsigned char has_size;
	float w, h;
	float uvx, uvy;                 /* additive uv offset (parallax) */
	const snes_spr_entry *ov_spr;   /* resource/animated-sprite swap (NULL = use packed) */
	const snes_img_entry *ov_img;
} snes_rvis;

typedef struct snes_rnode {
	const snes_node *def;           /* packed definition (comps read from here) */
	float tf[6];                    /* mutable transform [a,b,tx,c,d,ty] */
	float col[4];                   /* mutable color 0..1 */
	unsigned char enabled, visible;
	short min_layer;                /* memoized subtree min drawable layer */
	unsigned char base_layer_bg;    /* subtree forced behind (name=="BG") */
	struct snes_rnode *parent, *child, *sib;   /* first-child / next-sibling */
	snes_rvis vis;
} snes_rnode;

typedef struct {
	const snes_pack *pk;
	const snes_scene_entry *sc;
	const uint32_t *comp_offs;   /* scene comp-offset table base */
	snes_rnode *root;
	/* bump arena for rnodes */
	snes_rnode *pool;
	unsigned pool_cap, pool_used;
} snes_scene;

/* a node's i-th component record (packed). */
static inline const snes_comp *snes_node_comp(const snes_scene *s,
					      const snes_node *nd, unsigned i)
{
	return (const snes_comp *)(s->pk->base + s->comp_offs[nd->first_comp + i]);
}

/* Build a runtime tree for scene `sc` into arena pool[cap]. Returns root or NULL. */
snes_rnode *snes_scene_build(snes_scene *s, const snes_pack *pk,
			     const snes_scene_entry *sc,
			     snes_rnode *pool, unsigned cap);

/* find a descendant by name (depth-first) from the scene root, NULL if none. */
snes_rnode *snes_scene_find(snes_scene *s, const char *name);

/* world matrix (product up the parent chain) into out[6]. */
void snes_node_world(const snes_rnode *n, float out[6]);

/* 2x3 matrix multiply r = A*B */
void snes_mat_mul(const float A[6], const float B[6], float out[6]);

#endif
