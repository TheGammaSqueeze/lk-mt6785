#include "snes_scene.h"

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

void snes_mat_mul(const float A[6], const float B[6], float out[6])
{
	float r0 = A[0] * B[0] + A[1] * B[3];
	float r1 = A[0] * B[1] + A[1] * B[4];
	float r2 = A[0] * B[2] + A[1] * B[5] + A[2];
	float r3 = A[3] * B[0] + A[4] * B[3];
	float r4 = A[3] * B[1] + A[4] * B[4];
	float r5 = A[3] * B[2] + A[4] * B[5] + A[5];
	out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3; out[4] = r4; out[5] = r5;
}

void snes_node_world(const snes_rnode *n, float out[6])
{
	float m[6], p[6];
	int i;
	for (i = 0; i < 6; i++) m[i] = n->tf[i];
	for (n = n->parent; n; n = n->parent) {
		snes_mat_mul(n->tf, m, p);
		for (i = 0; i < 6; i++) m[i] = p[i];
	}
	for (i = 0; i < 6; i++) out[i] = m[i];
}

/* min drawable comp layer over a subtree (memoized). */
static short subtree_min_layer(const snes_pack *pk, const snes_scene_entry *sc,
			       const snes_node *nodes, const uint32_t *comp_offs,
			       const snes_node *nd)
{
	short mn = 32767;
	unsigned i;
	for (i = 0; i < nd->comp_count; i++) {
		const snes_comp *c = (const snes_comp *)(pk->base + comp_offs[nd->first_comp + i]);
		if (c->type == COMP_SPRITE || c->type == COMP_TEXTURE ||
		    c->type == COMP_ANIMATED_SPRITE || c->type == COMP_LABEL)
			if (c->layer < mn) mn = c->layer;
	}
	for (i = 0; i < nd->child_count; i++) {
		short cm = subtree_min_layer(pk, sc, nodes, comp_offs,
					     &nodes[nd->first_child + i]);
		if (cm < mn) mn = cm;
	}
	return (mn == 32767) ? 0 : mn;
}

snes_rnode *snes_scene_build(snes_scene *s, const snes_pack *pk,
			     const snes_scene_entry *sc,
			     snes_rnode *pool, unsigned cap)
{
	const snes_node *nodes = (const snes_node *)(pk->base + sc->nodes);
	const uint32_t *comp_offs = (const uint32_t *)(pk->base + sc->comp_offs);
	unsigned n = sc->node_count, i, j;
	if (n > cap)
		return 0;
	s->pk = pk;
	s->sc = sc;
	s->comp_offs = comp_offs;
	s->pool = pool;
	s->pool_cap = cap;
	s->pool_used = n;

	/* one rnode per packed node, same index */
	for (i = 0; i < n; i++) {
		const snes_node *d = &nodes[i];
		snes_rnode *r = &pool[i];
		r->def = d;
		for (j = 0; j < 6; j++) r->tf[j] = d->transform[j];
		r->col[0] = d->color[0] / 255.0f; r->col[1] = d->color[1] / 255.0f;
		r->col[2] = d->color[2] / 255.0f; r->col[3] = d->color[3] / 255.0f;
		r->enabled = (d->flags & SNES_NODE_ENABLED) ? 1 : 0;
		r->visible = (d->flags & SNES_NODE_VISIBLE) ? 1 : 0;
		r->parent = r->child = r->sib = 0;
		r->base_layer_bg = streq(snes_str(pk, d->name), "BG");
		r->vis.has_size = 0; r->vis.uvx = r->vis.uvy = 0;
		r->vis.ov_spr = 0; r->vis.ov_img = 0;
	}
	/* link children (contiguous run), set parents */
	for (i = 0; i < n; i++) {
		const snes_node *d = &nodes[i];
		snes_rnode *r = &pool[i];
		snes_rnode *prev = 0;
		for (j = 0; j < d->child_count; j++) {
			snes_rnode *c = &pool[d->first_child + j];
			c->parent = r;
			if (prev) prev->sib = c; else r->child = c;
			prev = c;
		}
	}
	for (i = 0; i < n; i++)
		pool[i].min_layer = subtree_min_layer(pk, sc, nodes, comp_offs, &nodes[i]);
	s->root = &pool[0];
	return s->root;
}

static snes_rnode *find_rec(snes_rnode *n, const snes_pack *pk, const char *name)
{
	snes_rnode *c, *r;
	if (streq(snes_str(pk, n->def->name), name))
		return n;
	for (c = n->child; c; c = c->sib)
		if ((r = find_rec(c, pk, name)))
			return r;
	return 0;
}

/* find by name given the scene (which carries the pack). */
snes_rnode *snes_scene_find(snes_scene *s, const char *name)
{
	return s->root ? find_rec(s->root, s->pk, name) : 0;
}
