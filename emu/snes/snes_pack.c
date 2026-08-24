#include "snes_pack.h"

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

int snes_pack_open(snes_pack *p, const void *blob, unsigned len)
{
	const snes_header *h = (const snes_header *)blob;
	if (len < sizeof(*h) || h->magic != SNESPACK_MAGIC || h->version != SNESPACK_VERSION)
		return -1;
	p->base    = (const uint8_t *)blob;
	p->hdr     = h;
	p->strpool = (const char *)(p->base + h->strpool_off);
	p->res     = (const snes_res_entry *)(p->base + h->res_off);
	p->res_mask= h->res_count ? (h->res_count - 1) : 0;   /* res_count is pow2 */
	p->img     = (const snes_img_entry *)(p->base + h->img_off);
	p->spr     = (const snes_spr_entry *)(p->base + h->spr_off);
	p->font    = (const snes_font_entry *)(p->base + h->font_off);
	p->scene   = (const snes_scene_entry *)(p->base + h->scene_off);
	p->anim    = (const snes_anim_entry *)(p->base + h->anim_off);
	p->sanim   = (const snes_sanim_entry *)(p->base + h->sanim_off);
	p->snd     = (const snes_snd_entry *)(p->base + h->snd_off);
	p->str     = (const snes_str_table *)(p->base + h->str_off);
	p->game_offs = (const uint32_t *)(p->base + h->game_off);
	p->init    = (const snes_init_block *)(p->base + h->init_off);
	return 0;
}

const snes_res_entry *snes_res_find(const snes_pack *p, uint32_t hash)
{
	uint32_t mask = p->res_mask, i;
	if (!p->hdr->res_count || !hash)
		return 0;
	i = hash & mask;
	for (;;) {
		const snes_res_entry *e = &p->res[i];
		if (e->id_hash == 0)
			return 0;                 /* empty slot: not present */
		if (e->id_hash == hash)
			return e;
		i = (i + 1) & mask;
	}
}

static const snes_res_entry *find_typed(const snes_pack *p, uint32_t hash, int type)
{
	const snes_res_entry *e = snes_res_find(p, hash);
	return (e && e->type == type) ? e : 0;
}

const snes_img_entry *snes_res_img(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_TEXTURE);
	return e ? &p->img[e->index] : 0;
}
const snes_spr_entry *snes_res_spr(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_SPRITE);
	return e ? &p->spr[e->index] : 0;
}
const snes_font_entry *snes_res_font(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_FONT);
	return e ? &p->font[e->index] : 0;
}
const snes_anim_entry *snes_res_anim(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_SCENEANIM);
	return e ? &p->anim[e->index] : 0;
}
const snes_sanim_entry *snes_res_sanim(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_SPRITEANIM);
	return e ? &p->sanim[e->index] : 0;
}
const snes_snd_entry *snes_res_snd(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_SOUND);
	return e ? &p->snd[e->index] : 0;
}
const snes_scene_entry *snes_res_scene(const snes_pack *p, uint32_t hash)
{
	const snes_res_entry *e = find_typed(p, hash, RES_SCENE);
	return e ? &p->scene[e->index] : 0;
}

const uint8_t *snes_img_pixels(const snes_pack *p, const snes_img_entry *im)
{
	return im ? (const uint8_t *)(p->base + im->pixels) : 0;
}

const char *snes_text_locale(const snes_pack *p, int li, const char *key)
{
	const snes_str_table *t;
	const snes_str_pair *pr;
	uint32_t i;
	if (!key || li < 0 || (uint32_t)li >= p->hdr->str_count)
		return key ? key : "";
	t = &p->str[li];
	pr = (const snes_str_pair *)(p->base + t->pairs);
	for (i = 0; i < t->pair_count; i++)
		if (streq(snes_str(p, pr[i].key), key))
			return snes_str(p, pr[i].val);
	return key;
}
const char *snes_text(const snes_pack *p, const char *key)
{
	return snes_text_locale(p, 0, key);
}
