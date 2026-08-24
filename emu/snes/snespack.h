/*
 * snespack - binary asset pack shared contract (see PACK_FORMAT.md).
 * Consumed in place by the LK engine; produced by tools/ayaneo/snes/pack_snes.py.
 * All little-endian. Offsets are byte offsets from the blob start. String refs are
 * u32 byte offsets into the string pool (NUL-terminated UTF-8).
 */
#ifndef SNESPACK_H
#define SNESPACK_H

#include <stdint.h>

#define SNESPACK_MAGIC   0x534E4553u   /* 'SNES' */
#define SNESPACK_VERSION 1u

/* flags (header.flags) */
#define SNESPACK_F_RGB565 0x1u         /* images are RGB565 + separate A8 (else RGBA8888) */

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t total_size;
	uint32_t flags;

	uint32_t strpool_off, strpool_len;
	uint32_t res_off,   res_count;     /* ResEntry[]  (pow2, open addressed) */
	uint32_t img_off,   img_count;     /* ImgEntry[]  */
	uint32_t spr_off,   spr_count;     /* SprEntry[]  */
	uint32_t font_off,  font_count;    /* FontEntry[] */
	uint32_t scene_off, scene_count;   /* SceneEntry[] */
	uint32_t anim_off,  anim_count;    /* AnimEntry[] */
	uint32_t sanim_off, sanim_count;   /* SAnimEntry[] */
	uint32_t snd_off,   snd_count;     /* SndEntry[] */
	uint32_t str_off,   str_count;     /* StrTable[] (per locale) */
	uint32_t game_off,  game_count;    /* GameRec offsets: game_off -> u32[game_count] of GameRec offs */
	uint32_t init_off;                 /* InitBlock */
} snes_header;

/* ---- resource hash table ---- */
enum {
	RES_NONE = 0, RES_SPRITE, RES_TEXTURE, RES_SPRITESHEET, RES_SCENEANIM,
	RES_SPRITEANIM, RES_SOUND, RES_FONT, RES_SCENE, RES_RAW, RES_RENDERTARGET
};
typedef struct {
	uint32_t id_hash;   /* FNV-1a of full id string; 0 = empty slot */
	uint32_t id_str;    /* strpool off of the id (collision verify) */
	uint16_t type;      /* RES_* */
	uint16_t index;     /* index into the per-type table */
} snes_res_entry;

/* ---- images (decoded) ---- */
#define SNES_IMG_TILE   0x1u   /* wallpaper: wrap-tile when drawn larger than source */
#define SNES_IMG_RGB565 0x2u
typedef struct {
	uint16_t w, h;
	uint16_t flags;
	uint16_t pad;
	uint32_t pixels;    /* off -> w*h*4 RGBA8888 (or w*h*3 rgb565+a8) */
} snes_img_entry;

/* ---- sprites (atlas frame) ---- */
typedef struct {
	uint16_t img;                 /* ImgEntry index */
	uint16_t sx, sy, sw, sh;      /* frame rect */
	int16_t  px, py;              /* pivot, native texture px */
} snes_spr_entry;

/* ---- fonts (BMFont) ---- */
typedef struct {
	uint16_t page;        /* ImgEntry index */
	uint16_t line_height;
	uint16_t base;
	uint16_t glyph_count;
	uint32_t glyphs;      /* off -> snes_glyph[glyph_count], sorted by cp */
} snes_font_entry;
typedef struct {
	uint32_t cp;
	uint16_t x, y, w, h;
	int16_t  xo, yo, xadv, pad;
} snes_glyph;

/* ---- scenes ---- */
typedef struct {
	uint32_t name;        /* strpool off (basename) */
	uint32_t nodes;       /* off -> snes_node[node_count], flattened depth-first */
	uint32_t node_count;
	uint32_t comp_offs;   /* off -> u32[comp_count] of blob byte-offsets to comp records */
	uint32_t comp_count;
} snes_scene_entry;
/* node.first_child/child_count index the scene's node[] (contiguous run).
 * node.first_comp/comp_count index the scene's comp_offs[] table (each entry a
 * byte offset to a variable-size snes_comp* record). */

/* node script class (scriptType). Only classes the C menu logic cares about are
 * enumerated; everything else is SCRIPT_GENERIC (still rendered, no behavior). */
enum {
	SCRIPT_NONE = 0, SCRIPT_GENERIC,
	SCRIPT_MAIN, SCRIPT_HOMEMENU, SCRIPT_GAMETITLELIST, SCRIPT_GAME_CARD,
	SCRIPT_MENUBARU, SCRIPT_MENUBAR_CM, SCRIPT_HUD, SCRIPT_THUMBNAIL,
	SCRIPT_HOMEMENU_SORTORDER, SCRIPT_HOME_FLOATING_CARDS, SCRIPT_RESUMEDUMMY,
	SCRIPT_RESUMEMENU, SCRIPT_OPTION, SCRIPT_OPTION_DISPLAY, SCRIPT_OPTION_SETTING,
	SCRIPT_OPTION_SETTING_ITEM, SCRIPT_OPTION_LANGUAGES, SCRIPT_COPYRIGHT,
	SCRIPT_COPYRIGHT_TEXT, SCRIPT_MANUAL, SCRIPT_FADE, SCRIPT_DIALOG,
	SCRIPT_BUTTON, SCRIPT_CURSOR, SCRIPT_POSITION_CHANGER, SCRIPT_EFFECT_SMOKE,
	SCRIPT_LBL_COMPONENT, SCRIPT_SND_COMPONENT
};

#define SNES_NODE_ENABLED 0x1u
#define SNES_NODE_VISIBLE 0x2u
typedef struct {
	uint32_t name;        /* strpool off */
	float    transform[6];/* [a,b,tx,c,d,ty] */
	uint8_t  color[4];    /* RGBA 0..255 */
	uint8_t  flags;       /* SNES_NODE_* */
	uint8_t  script;      /* SCRIPT_* */
	int16_t  zindex;
	uint16_t first_child, child_count;
	uint16_t first_comp,  comp_count;
	uint32_t props;       /* off -> PropBlock, 0 if none */
} snes_node;

/* components: a common header then a type-specific body. `size` is the total
 * record length (>= sizeof header, 4-aligned) so the engine can skip unknowns. */
enum {
	COMP_TEXTURE = 0, COMP_SPRITE, COMP_ANIMATED_SPRITE, COMP_LABEL,
	COMP_ANIMATOR, COMP_CAMERA, COMP_SOUND, COMP_SCRIPT
};
#define SNES_COMP_ENABLED 0x1u
#define SNES_COMP_VISIBLE 0x2u
#define SNES_COMP_HAS_SIZE 0x4u
#define SNES_COMP_HFLIP    0x8u
#define SNES_COMP_VFLIP    0x10u
#define SNES_COMP_HAS_SHADOW 0x20u
#define SNES_COMP_WORDWRAP 0x40u
#define SNES_COMP_TILE     0x80u
typedef struct {
	uint16_t size;        /* total record bytes */
	uint8_t  type;        /* COMP_* */
	uint8_t  flags;       /* SNES_COMP_* */
	int16_t  zindex;
	int16_t  layer;
	uint8_t  blend;       /* 0 normal, 1 additive */
	uint8_t  script;      /* SCRIPT_* for COMP_SCRIPT, else 0 */
	/* type-specific body follows (see packer / engine) */
} snes_comp;

/* sprite/texture body (COMP_SPRITE, COMP_TEXTURE, COMP_ANIMATED_SPRITE) */
typedef struct {
	snes_comp c;
	uint32_t  res_hash;   /* sprite id / texture id / spriteanim id hash */
	float     size_w, size_h;   /* valid if SNES_COMP_HAS_SIZE */
	float     uv_off_x, uv_off_y, uv_rep_x, uv_rep_y;
} snes_comp_visual;

/* label body (COMP_LABEL) */
enum { ANCHOR_LEFT = 0, ANCHOR_CENTER, ANCHOR_RIGHT, ANCHOR_TOP, ANCHOR_MIDDLE, ANCHOR_BOTTOM };
typedef struct {
	snes_comp c;
	uint32_t  text;       /* strpool off (already localised key kept too) */
	uint32_t  text_key;   /* strpool off of the @key (or 0) for relocalise */
	uint32_t  font_hash;
	uint8_t   h_anchor, v_anchor, alignment, mag_linear;
	float     wrap_width;
	int16_t   shadow_dx, shadow_dy;
	uint8_t   shadow_col[4];
} snes_comp_label;

/* animator body (COMP_ANIMATOR) */
typedef struct {
	snes_comp c;
	uint32_t  anim_hash;
	float     speed;
	uint8_t   looped, manual, pad0, pad1;
} snes_comp_animator;

/* sound body (COMP_SOUND) */
typedef struct {
	snes_comp c;
	uint32_t  snd_hash;
	uint8_t   autoplay, loop, is_bgm, pad;
	float     volume;
} snes_comp_sound;

/* ---- scene animations ---- */
enum {
	PROP_NONE = 0, PROP_POS_X, PROP_POS_Y, PROP_SCALE_X, PROP_SCALE_Y,
	PROP_ROT, PROP_ALPHA, PROP_WIDTH, PROP_TEX_OFF_X, PROP_VISIBLE, PROP_TEXTURE
};
typedef struct {
	uint32_t duration_ms;
	uint32_t tracks;      /* off -> snes_track[track_count] */
	uint32_t track_count;
} snes_anim_entry;
typedef struct {
	uint32_t node_path;   /* off -> u16 len then u32[len] strpool offs of names */
	uint16_t prop;        /* PROP_* */
	uint8_t  type;        /* 0 float, 1 bool, 2 resource */
	uint8_t  pad;
	uint32_t keys;        /* off -> snes_key[key_count] */
	uint32_t key_count;
} snes_track;
typedef struct { uint32_t time_ms; uint32_t value; } snes_key; /* value: f32 bits | bool | res hash */

/* ---- sprite animations ---- */
typedef struct { uint16_t sheet_img, frame_count; uint32_t frames; } snes_sanim_entry;
typedef struct { float dur_ms; uint16_t spr; uint16_t pad; } snes_sanim_frame;

/* ---- sounds ---- */
typedef struct {
	uint32_t rate, frames;
	uint8_t  channels, fmt, pad0, pad1;   /* fmt 0 = s16 */
	uint32_t loop_start, loop_end;
	uint32_t pcm;
} snes_snd_entry;

/* ---- strings + roster + init ---- */
typedef struct { uint32_t locale; uint32_t pairs; uint32_t pair_count; } snes_str_table;
typedef struct { uint32_t key; uint32_t val; } snes_str_pair;

typedef struct {
	uint32_t code, name, publisher, sort_title, sort_publisher, copyright;
	uint8_t  players, simultaneous, pad0, pad1;
	uint32_t release;     /* yyyymmdd */
	uint16_t thumb_img, small_img;
} snes_game_rec;

typedef struct {
	uint32_t default_scene_hash;
	uint32_t region;      /* 0 usa, 1 eur, 2 jpn */
	uint32_t locale;      /* strpool off, e.g. "usa_en" */
} snes_init_block;

/* FNV-1a 32-bit over a NUL-terminated string (id hashing). */
static inline uint32_t snes_hash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

#endif /* SNESPACK_H */
