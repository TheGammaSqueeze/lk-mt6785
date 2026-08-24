# SNES Classic menu asset pack (`snespack`) - binary format

The LK menu port cannot parse JSON/PNG at runtime, so a host packer
(`tools/ayaneo/snes/pack_snes.py`) converts a processed firmware asset directory
(the same `web/public/fw/<name>/` or `web/public/assets/` tree the web app uses)
into a single little-endian binary blob stored in `boot_b`. The LK engine mmaps
the blob and reads it in place (no allocation for the tables; images are already
decoded).

Design goals: zero runtime parsing, GUID lookup in O(1), images pre-decoded to a
GPU-free blit format, everything 4-byte aligned so the engine casts structs in
place. The firmware is copyright, so the packed blob and any extracted assets are
git-ignored; the packer regenerates it from a user-supplied image.

All offsets are byte offsets from the start of the blob unless noted. All ints are
little-endian. Strings are stored in a single string pool and referenced by a
`u32` byte offset into it (NUL-terminated UTF-8).

## Coordinate space / images

The engine renders in the CLOVER virtual space (1280x720, +Y up, origin centre),
exactly like the web renderer, and centres that on the 1280x960 panel (letterbox
top/bottom) so output matches the web app 1:1 (the web app is the reference).

Images are stored decoded as **RGBA8888** rows, top-down, premultiplied-alpha =
NO (straight alpha, matching the Canvas multiply-then-mask tint math). Large
wallpaper/background images are downscaled by the packer to their on-screen size
(<= panel) to save space; UI atlases and fonts are kept 1:1. Every image records
whether it is a `/wallpaper/` tiling texture.

## Top-level layout

```
magic      u32   'SNES' (0x534E4553)
version    u32   1
total_size u32   blob length in bytes
flags      u32   bit0 = images are RGB565+A8 (default 0 = RGBA8888)

strpool_off u32  ; strpool_len u32     one NUL-terminated UTF-8 pool
res_off     u32  ; res_count  u32      resource hash table (GUID -> resource)
img_off     u32  ; img_count  u32      decoded image table
spr_off     u32  ; spr_count  u32      sprite (atlas frame) table
font_off    u32  ; font_count u32      font table
scene_off   u32  ; scene_count u32     scene table (binary node trees)
anim_off    u32  ; anim_count u32      scene-animation table
sanim_off   u32  ; sanim_count u32     sprite-animation table
snd_off     u32  ; snd_count  u32      sound table (PCM or ADPCM)
str_off     u32  ; str_count  u32      localized string table (per locale)
game_off    u32  ; game_count u32      game roster
init_off    u32                        InitBlock (defaultScene guid off, region)
```

## GUIDs

CLOVER GUIDs are 36-char strings ("ff9fb46c-...") and sprite ids append
".<frame>.png". The packer hashes the full id string (FNV-1a 32-bit) into a
`ResEntry` open-addressing table sized to the next pow2 >= 2*res_count. Collisions
resolve by linear probe; each entry also stores the string-pool offset of the full
id so the engine verifies the match.

```
ResEntry (16 bytes)
  id_hash  u32     FNV-1a of the full id string (0 = empty slot)
  id_str   u32     strpool offset of the id (for collision verify)
  type     u16     RES_* enum
  index    u16     index into the per-type table below
```

`RES_*`: 0 none, 1 sprite, 2 texture(image), 3 spritesheet, 4 sceneanim,
5 spriteanim, 6 sound, 7 font, 8 scene, 9 raw(string table), 10 rendertarget.

## Images (`ImgEntry`, decoded pixels)

```
ImgEntry (16 bytes)
  w       u16
  h       u16
  flags   u16      bit0 tile(wallpaper) ; bit1 rgb565
  pad     u16
  pixels  u32      offset to w*h*4 (RGBA8888) or w*h*3 (RGB565+A8) bytes
```

## Sprites (`SprEntry`, an atlas frame)

```
SprEntry (16 bytes)
  img     u16      ImgEntry index of the atlas/texture
  sx u16 ; sy u16 ; sw u16 ; sh u16     frame rect in the atlas
  px i16 ; py i16                        pivot in native texture px
```
Full sprites (a whole texture drawn as a sprite) point at the image with the full
rect and pivot = w/2,h/2.

## Fonts (`FontEntry` + glyph table, BMFont)

```
FontEntry (16 bytes)
  page        u16   ImgEntry index of the glyph page
  line_height u16
  base        u16
  glyph_count u16
  glyphs      u32   offset to Glyph[glyph_count], sorted by codepoint
Glyph (16 bytes)
  cp   u32   codepoint
  x u16 ; y u16 ; w u16 ; h u16
  xo i16 ; yo i16 ; xadv i16 ; pad i16
```
Glyph lookup is a binary search on `cp`.

## Scenes (`SceneEntry` -> binary node tree)

Each scene is the `rootWorldNode` tree flattened depth-first into a `Node[]`
array; children are a contiguous run referenced by (first_child, child_count).
Components live in a parallel `Comp[]` array referenced by (first_comp, comp_count).

```
SceneEntry (12 bytes)
  name    u32   strpool off (basename, e.g. "defaultscene.scn")
  nodes   u32   offset to NodeRec[node_count]
  node_count u32
NodeRec (48 bytes)
  name        u32   strpool off
  transform   6 x f32   [a,b,tx,c,d,ty]
  color       4 x u8    RGBA (0..255; engine divides by 255)
  flags       u8    bit0 enabled ; bit1 visible
  script      u8    SCRIPT_* enum (the scriptType, 0 = WorldNode/none)
  zindex      i16
  first_child u16 ; child_count u16
  first_comp  u16 ; comp_count  u16
  props       u32   offset to a PropBlock (links + kv), 0 if none
```
`transform` is stored as f32 for exactness with the JSON; the engine may convert
to fixed-point internally. Node/comp arrays are per-scene (indices are local).

```
CompRec (variable, tagged) - first field:
  type    u8    COMP_* enum
  flags   u8    bit0 enabled ; bit1 visible
  zindex  i16
  layer   i16
  blend   u8    0 normal, 1 additive
  ...type-specific fields (sprite id/ size/ flip; texture id/ uv; label text/font/
     anchors/wrap/shadow; animator anim/speed/loop; sound id/loop/isbgm; script
     scriptType+prop ref). Records are 4-byte aligned; a u16 size precedes the
     type-specific body so the engine can skip unknown comp types.
```

`SCRIPT_*` and `COMP_*` enums are defined once in `snespack.h` (shared by packer
output expectations and the engine). Properties that are cross-node links
(`{linkType,linkId}`) are stored in the PropBlock as (key strpool off, link kind,
target: scene-local node/comp index or a resource GUID hash).

## Scene animations (`AnimEntry`)

```
AnimEntry (12 bytes)
  duration_ms u32
  tracks      u32   offset to Track[track_count]
  track_count u32
Track (16 bytes)
  node_path u32   offset to a u16[len]+len of strpool offs (name path from anim root)
  prop      u16   PROP_* enum (LocalPositionX.. Alpha, Visible, Texture...)
  type      u8    0 float,1 bool,2 resource
  pad       u8
  keys      u32   offset to Key[key_count]
  key_count u32
Key (8 bytes)   time_ms u32 ; value f32 (float) | u32 bool | resource-hash
```
Linear interpolation between keys (matches animator.js).

## Sprite animations (`SAnimEntry`)

```
SAnimEntry (8 bytes)
  sheet_img u16 ; frame_count u16 ; frames u32
Frame (8 bytes)  dur_ms f32 ... spr u16 ; pad u16   (resolved to SprEntry index by packer)
```

## Sounds (`SndEntry`)

```
SndEntry (16 bytes)
  rate u32 ; frames u32 ; channels u8 ; fmt u8 (0 s16) ; pad u16
  loop_start u32 ; loop_end u32   (frames; from WAV smpl chunk; 0/0 = none)
  pcm u32
```
Only BGM + a handful of SFX; s16 stereo/mono, resampled by the packer to 48000 to
feed the existing AFE ring (see [[gbc-emulator-lk]] audio path).

## Strings (`StrTable` per locale) + roster (`GameRec`)

```
StrTable (12 bytes)  locale u32(strpool "usa_en") ; pairs u32 ; pair_count u32
Pair (8 bytes)       key u32(strpool) ; val u32(strpool)     sorted by key hash
GameRec (variable)   code, name, publisher, sort_title, sort_publisher (strpool),
                     players u8, simultaneous u8, release u32(yyyymmdd),
                     thumb_img u16, small_img u16, copyright u32(strpool)
```
`init`: defaultScene id hash, region (0 usa,1 eur,2 jpn), default locale strpool.
The `Exec`/ROM path is dropped (launch is stubbed).
