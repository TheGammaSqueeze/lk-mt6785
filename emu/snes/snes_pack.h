/*
 * In-place accessor over a loaded snespack blob (see PACK_FORMAT.md / snespack.h).
 * The blob is decompressed into DRAM at boot; nothing here allocates.
 */
#ifndef SNES_PACK_H
#define SNES_PACK_H

#include "snespack.h"

typedef struct {
	const uint8_t   *base;      /* blob start */
	const snes_header *hdr;
	const char      *strpool;
	const snes_res_entry *res;  uint32_t res_mask;   /* pow2-1 */
	const snes_img_entry *img;
	const snes_spr_entry *spr;
	const snes_font_entry *font;
	const snes_scene_entry *scene;
	const snes_anim_entry *anim;
	const snes_sanim_entry *sanim;
	const snes_snd_entry *snd;
	const snes_str_table *str;
	const uint32_t  *game_offs;
	const snes_init_block *init;
	int locale;                 /* active locale index into str[] (default usa_en) */
} snes_pack;

/* Bind the accessor to a decompressed blob. Returns 0 ok, <0 on bad magic/version. */
int snes_pack_open(snes_pack *p, const void *blob, unsigned len);

static inline const char *snes_str(const snes_pack *p, uint32_t off)
{
	return off ? (const char *)(p->strpool + off) : "";
}
static inline const void *snes_at(const snes_pack *p, uint32_t off)
{
	return off ? (const void *)(p->base + off) : 0;
}

/* Resolve a resource id hash to its ResEntry (open-addressed probe). NULL if absent. */
const snes_res_entry *snes_res_find(const snes_pack *p, uint32_t hash);
/* Convenience typed lookups (return NULL / -1 on miss). */
const snes_img_entry  *snes_res_img(const snes_pack *p, uint32_t hash);   /* TEXTURE */
const snes_spr_entry  *snes_res_spr(const snes_pack *p, uint32_t hash);   /* SPRITE */
const snes_font_entry *snes_res_font(const snes_pack *p, uint32_t hash);
const snes_anim_entry *snes_res_anim(const snes_pack *p, uint32_t hash);
const snes_sanim_entry*snes_res_sanim(const snes_pack *p, uint32_t hash);
const snes_snd_entry  *snes_res_snd(const snes_pack *p, uint32_t hash);
const snes_scene_entry*snes_res_scene(const snes_pack *p, uint32_t hash);

/* image pixel base + whether rgb565 */
const uint8_t *snes_img_pixels(const snes_pack *p, const snes_img_entry *im);

/* localized string lookup by key (default/first locale). Returns the key if absent. */
const char *snes_text(const snes_pack *p, const char *key);
const char *snes_text_locale(const snes_pack *p, int locale_idx, const char *key);

#endif
