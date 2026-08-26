#!/usr/bin/env python3
"""
pack_snes.py - build a `snespack` binary blob from a processed CLOVER firmware
asset tree (the same dist/assets or web/public tree the web app consumes).

The LK menu engine mmaps the blob and reads structs in place, so every table is
laid out exactly as declared in emu/snes/snespack.h. All little-endian, all
records 4-byte aligned, string refs are byte offsets from the blob base (the
string pool base is the blob base, i.e. strpool_off = 0, so string offsets are
absolute like every other offset). The header occupies the first HEADER_SIZE
bytes and its space is reserved before anything else is written, so every offset
stored in the blob is final (no post-hoc patching).

Usage:
    pack_snes.py <asset_dir> <out.snespack> [--rgb565] [--report]

The firmware is copyright; the packed blob is git-ignored and regenerated from a
user-supplied asset dump.
"""

import os, json, struct, argparse, glob, wave
import numpy as np


def _read_wav_mono(path):
    """Read a 16-bit PCM WAV -> (rate, float32 mono in [-1,1], loop or None).
    Parses the RIFF chunks directly so the 'smpl' loop points survive (the stdlib
    wave module drops them). Downmixes to mono."""
    b = open(path, "rb").read()
    if b[0:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise ValueError("not a WAVE")
    o = 12
    rate = ch = bits = 0
    data = None
    loop = None
    while o + 8 <= len(b):
        cid = b[o:o + 4]
        sz = struct.unpack_from("<I", b, o + 4)[0]
        body = o + 8
        if cid == b"fmt ":
            ch = struct.unpack_from("<H", b, body + 2)[0]
            rate = struct.unpack_from("<I", b, body + 4)[0]
            bits = struct.unpack_from("<H", b, body + 14)[0]
        elif cid == b"data":
            data = b[body:body + sz]
        elif cid == b"smpl":
            nloops = struct.unpack_from("<I", b, body + 28)[0]
            if nloops:
                loop = (struct.unpack_from("<I", b, body + 44)[0],
                        struct.unpack_from("<I", b, body + 48)[0])
        o = body + sz + (sz & 1)
    if data is None or rate == 0 or bits != 16:
        raise ValueError("unsupported wav (rate=%d bits=%d)" % (rate, bits))
    s = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
    if ch > 1:
        s = s.reshape(-1, ch).mean(axis=1)
    # clamp loop to available frames
    if loop:
        n = len(s)
        loop = (min(loop[0], n), min(loop[1], n))
        if loop[1] <= loop[0]:
            loop = None
    return rate, s, loop

try:
    from PIL import Image
except ImportError:
    Image = None

MAGIC   = 0x534E4553
VERSION = 1
F_RGB565 = 0x1

HEADER_FMT = "<IIII" + "II" * 11 + "I" + "II"   # ...+ init_off + oss_off,oss_count
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 116

# snes_comp header is 10 bytes; the C bodies (snes_comp_visual etc.) place their
# first u32 at offset 12, so 2 pad bytes follow the header before any body.
COMP_HDR = 10
COMP_BODY_AT = 12

# image flags
IMG_TILE   = 0x1
IMG_RGB565 = 0x2

# resource types (match snespack.h enum)
RES_NONE, RES_SPRITE, RES_TEXTURE, RES_SPRITESHEET, RES_SCENEANIM, \
RES_SPRITEANIM, RES_SOUND, RES_FONT, RES_SCENE, RES_RAW, RES_RENDERTARGET = range(11)

# script classes (must match snespack.h enum order exactly)
SCRIPT_NAMES = [
    "NONE", "GENERIC",
    "MAIN", "HOMEMENU", "GAMETITLELIST", "GAME_CARD",
    "MENUBARU", "MENUBAR_CM", "HUD", "THUMBNAIL",
    "HOMEMENU_SORTORDER", "HOME_FLOATING_CARDS", "RESUMEDUMMY",
    "RESUMEMENU", "OPTION", "OPTION_DISPLAY", "OPTION_SETTING",
    "OPTION_SETTING_ITEM", "OPTION_LANGUAGES", "COPYRIGHT",
    "COPYRIGHT_TEXT", "MANUAL", "FADE", "DIALOG",
    "BUTTON", "CURSOR", "POSITION_CHANGER", "EFFECT_SMOKE",
    "LBL_COMPONENT", "SND_COMPONENT",
]
SCRIPT = {name: i for i, name in enumerate(SCRIPT_NAMES)}

# JSON scriptType -> SCRIPT_* class. "WorldNode" is NONE; unknown -> GENERIC.
SCRIPT_MAP = {
    "WorldNode": SCRIPT["NONE"],
    "Main": SCRIPT["MAIN"],
    "sys_homemenu": SCRIPT["HOMEMENU"],
    "sys_gametitlelist": SCRIPT["GAMETITLELIST"],
    "sys_game_card": SCRIPT["GAME_CARD"],
    "sys_menubarU": SCRIPT["MENUBARU"],
    "sys_menubar_cm": SCRIPT["MENUBAR_CM"],
    "sys_hud": SCRIPT["HUD"],
    "sys_thumbnail": SCRIPT["THUMBNAIL"],
    "sys_thumbnail_icon": SCRIPT["THUMBNAIL"],
    "sys_homemenu_sortorder": SCRIPT["HOMEMENU_SORTORDER"],
    "sys_home_floating_cards": SCRIPT["HOME_FLOATING_CARDS"],
    "sys_resumedummy": SCRIPT["RESUMEDUMMY"],
    "sys_resumemenu": SCRIPT["RESUMEMENU"],
    "sys_option": SCRIPT["OPTION"],
    "sys_option_display": SCRIPT["OPTION_DISPLAY"],
    "sys_option_setting": SCRIPT["OPTION_SETTING"],
    "sys_option_setting_item": SCRIPT["OPTION_SETTING_ITEM"],
    "sys_option_item": SCRIPT["OPTION_SETTING_ITEM"],
    "sys_option_languages": SCRIPT["OPTION_LANGUAGES"],
    "sys_copyright": SCRIPT["COPYRIGHT"],
    "sys_copyright_text": SCRIPT["COPYRIGHT_TEXT"],
    "sys_manual": SCRIPT["MANUAL"],
    "sys_fade": SCRIPT["FADE"],
    "sys_dialog": SCRIPT["DIALOG"],
    "sys_button": SCRIPT["BUTTON"],
    "sys_button_longpress": SCRIPT["BUTTON"],
    "sys_cursor": SCRIPT["CURSOR"],
    "position_changer": SCRIPT["POSITION_CHANGER"],
    "position_changer_node": SCRIPT["POSITION_CHANGER"],
    "effect_smoke": SCRIPT["EFFECT_SMOKE"],
    "lbl_component": SCRIPT["LBL_COMPONENT"],
    "snd_component": SCRIPT["SND_COMPONENT"],
}

# component types (match snespack.h enum). Only these 8 are serialized.
COMP_TEXTURE, COMP_SPRITE, COMP_ANIMATED_SPRITE, COMP_LABEL, \
COMP_ANIMATOR, COMP_CAMERA, COMP_SOUND, COMP_SCRIPT = range(8)

COMP_TYPE_MAP = {
    "TextureComponent": COMP_TEXTURE,
    "SpriteComponent": COMP_SPRITE,
    "AnimatedSpriteComponent": COMP_ANIMATED_SPRITE,
    "LabelComponent": COMP_LABEL,
    "AnimatorComponent": COMP_ANIMATOR,
    "CameraComponent": COMP_CAMERA,
    "SoundComponent": COMP_SOUND,
    "ScriptComponent": COMP_SCRIPT,
}

# node flags
NODE_ENABLED = 0x1
NODE_VISIBLE = 0x2
# comp flags
C_ENABLED    = 0x1
C_VISIBLE    = 0x2
C_HAS_SIZE   = 0x4
C_HFLIP      = 0x8
C_VFLIP      = 0x10
C_HAS_SHADOW = 0x20
C_WORDWRAP   = 0x40
C_TILE       = 0x80

ANCHOR = {"Left": 0, "Center": 1, "Right": 2, "Top": 3, "Middle": 4, "Bottom": 5}

PROP_MAP = {"LocalPositionX": 1, "LocalPositionY": 2, "LocalScaleX": 3,
            "LocalScaleY": 4, "Rotation": 5, "Alpha": 6, "Width": 7,
            "TextureOffsetX": 8, "Visible": 9, "Texture": 10}


def fnv1a(s):
    h = 2166136261
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


class Blob:
    """Append-only little-endian blob. The first HEADER_SIZE bytes are reserved
    for the header; a NUL follows so string offset 0 means the empty string.
    String offsets are absolute from the blob base (strpool_off = 0)."""
    def __init__(self):
        self.buf = bytearray(HEADER_SIZE)
        self.buf.append(0)          # off 0 == empty string
        self._strs = {}

    def align(self, a=4):
        while len(self.buf) % a:
            self.buf.append(0)

    def tell(self):
        return len(self.buf)

    def write(self, data):
        off = len(self.buf)
        self.buf += data
        return off

    def str(self, s):
        if not s:
            return 0
        if s in self._strs:
            return self._strs[s]
        off = len(self.buf)
        self.buf += s.encode("utf-8") + b"\x00"
        self._strs[s] = off
        return off


def clamp8(v):
    return max(0, min(255, int(round(v * 255))))


def color4(c):
    if not c:
        return bytes([255, 255, 255, 255])
    return bytes([clamp8(c[0]), clamp8(c[1]), clamp8(c[2]), clamp8(c[3])])


# ---------------------------------------------------------------------------
# scene emission (the load-bearing snes_scene_entry contract)
# ---------------------------------------------------------------------------

def emit_component(blob, comp):
    """Emit one variable-size component record: the 10-byte snes_comp header, 2
    pad bytes, then the type body (bodies begin at offset 12). Return the record
    byte offset, or None if `comp` is not one of the 8 COMP_* types (the caller
    then skips it, keeping node.comp_count consistent with what is emitted)."""
    ctype = COMP_TYPE_MAP.get(comp.get("type"))
    if ctype is None:
        return None

    flags = 0
    if comp.get("enabled", True):
        flags |= C_ENABLED
    if comp.get("visible", True):
        flags |= C_VISIBLE
    zindex = int(comp.get("viewDepth", 0))
    layer  = int(comp.get("layer", 0))
    blend  = 1 if int(comp.get("blendMode", 0)) == 1 else 0
    script = 0
    body = bytearray()

    if ctype in (COMP_TEXTURE, COMP_SPRITE, COMP_ANIMATED_SPRITE):
        if comp.get("hFlip"):
            flags |= C_HFLIP
        if comp.get("vFlip"):
            flags |= C_VFLIP
        if ctype == COMP_TEXTURE:
            res_id = comp.get("textureId", "")
            if int(comp.get("horizontalWrapMode", 0)) or int(comp.get("verticalWrapMode", 0)):
                flags |= C_TILE
        elif ctype == COMP_SPRITE:
            res_id = comp.get("spriteId", "")
        else:
            res_id = comp.get("animation", "")
        res_hash = fnv1a(res_id) if res_id else 0
        size = comp.get("size")
        if size:
            flags |= C_HAS_SIZE
            sw, sh = float(size[0]), float(size[1])
        else:
            sw = sh = 0.0
        uvo = comp.get("uvOffset", [0, 0])
        uvr = comp.get("uvRepeat", [1, 1])
        body += struct.pack("<Iffffff", res_hash, sw, sh,
                            float(uvo[0]), float(uvo[1]),
                            float(uvr[0]), float(uvr[1]))

    elif ctype == COMP_LABEL:
        script = SCRIPT["LBL_COMPONENT"]
        text = comp.get("text", "") or ""
        text_key = text if text.startswith("@") else ""
        text_off = blob.str(text)
        key_off = blob.str(text_key)
        font_hash = fnv1a(comp["fontResource"]) if comp.get("fontResource") else 0
        h_anchor = ANCHOR.get(comp.get("horizontalAnchor", "Left"), 0)
        v_anchor = ANCHOR.get(comp.get("verticalAnchor", "Top"), 3)
        alignment = ANCHOR.get(comp.get("alignment", "Left"), 0)
        mag_linear = 1 if int(comp.get("magFilterMode", 0)) == 1 else 0
        if comp.get("wordWrap"):
            flags |= C_WORDWRAP
        if comp.get("has_shadow"):
            flags |= C_HAS_SHADOW
        wrap_w = float(comp.get("wrapWidth", 0))
        sdx = int(comp.get("shadowoffsetx", 0))
        sdy = int(comp.get("shadowoffsety", 0))
        scol = color4(comp.get("shadowcolor"))
        body += struct.pack("<III", text_off, key_off, font_hash)
        body += struct.pack("<BBBB", h_anchor, v_anchor, alignment, mag_linear)
        body += struct.pack("<f", wrap_w)
        body += struct.pack("<hh", sdx, sdy)
        body += scol

    elif ctype == COMP_ANIMATOR:
        anim_hash = fnv1a(comp["animation"]) if comp.get("animation") else 0
        speed = float(comp.get("speed", 1.0))
        looped = 1 if comp.get("looped") else 0
        manual = 1 if comp.get("manual") else 0
        body += struct.pack("<IfBBBB", anim_hash, speed, looped, manual, 0, 0)

    elif ctype == COMP_SOUND:
        script = SCRIPT["SND_COMPONENT"]
        snd_hash = fnv1a(comp["soundId"]) if comp.get("soundId") else 0
        autoplay = 1 if comp.get("autoPlay") else 0
        loop = 1 if comp.get("loop") else 0
        props = comp.get("properties") or {}
        is_bgm = 1 if props.get("isBGM") else 0
        vol = float(comp.get("volume", 1.0))
        body += struct.pack("<IBBBBf", snd_hash, autoplay, loop, is_bgm, 0, vol)

    elif ctype == COMP_SCRIPT:
        script = SCRIPT_MAP.get(comp.get("scriptType"), SCRIPT["GENERIC"])
        # bare header

    elif ctype == COMP_CAMERA:
        pass  # bare header

    body_at = COMP_BODY_AT if body else COMP_HDR
    total = body_at + len(body)
    total += (-total) % 4
    assert total <= 0xFFFF, "component record exceeds u16 size field"
    blob.align(4)
    off = blob.tell()
    rec = bytearray(struct.pack("<HBBhhBB", total, ctype, flags, zindex, layer, blend, script))
    rec += b"\x00" * (body_at - COMP_HDR)     # pad header out to body offset 12
    rec += bytes(body)
    rec += b"\x00" * (total - len(rec))       # tail pad to the 4-aligned size
    assert len(rec) == total
    blob.write(rec)
    return off


def flatten_scene(root):
    """Depth-first flatten with CONTIGUOUS child runs: root at index 0, then for
    each node its children occupy a single contiguous block (authored order)."""
    flat = []

    def add(node):
        node["_idx"] = len(flat)
        flat.append(node)

    def walk(node):
        children = node.get("children", []) or []
        node["_first_child"] = len(flat) if children else 0
        node["_child_count"] = len(children)
        for ch in children:                 # reserve the contiguous run first
            add(ch)
        for ch in children:                 # then recurse into each child
            walk(ch)

    add(root)
    walk(root)
    return flat


def emit_scene(blob, scene_json, name):
    root = scene_json["rootWorldNode"]
    flat = flatten_scene(root)
    node_count = len(flat)

    # 1) component records + per-node run into the scene's comp_offs list
    comp_offsets = []
    for node in flat:
        node["_first_comp"] = len(comp_offsets)
        cnt = 0
        for comp in node.get("components", []) or []:
            off = emit_component(blob, comp)
            if off is None:
                continue
            comp_offsets.append(off)
            cnt += 1
        node["_comp_count"] = cnt

    # 2) u16 range checks
    max_first_child = max((n["_first_child"] for n in flat), default=0)
    max_first_comp = max((n["_first_comp"] for n in flat), default=0)
    over = []
    if max_first_child > 0xFFFF:
        over.append(("first_child", max_first_child))
    if max_first_comp > 0xFFFF:
        over.append(("first_comp", max_first_comp))

    # 3) comp_offs table: u32[comp_count] of blob byte-offsets to comp records
    blob.align(4)
    comp_offs_off = blob.tell()
    for o in comp_offsets:
        blob.write(struct.pack("<I", o))

    # 4) node table: snes_node[node_count]. Intern all name strings FIRST so the
    # string pool never grows in the middle of the fixed-stride node array.
    name_offs = [blob.str(node.get("name", "")) for node in flat]
    blob.align(4)
    nodes_off = blob.tell()
    for node, name_off in zip(flat, name_offs):
        tf = (list(node.get("transform", [1, 0, 0, 0, 1, 0])) + [0] * 6)[:6]
        a, b, tx, c, d, ty = tf
        col = color4(node.get("color"))
        flags = 0
        if node.get("enabled", True):
            flags |= NODE_ENABLED
        if node.get("visible", True):
            flags |= NODE_VISIBLE
        script = SCRIPT_MAP.get(node.get("scriptType", "WorldNode"), SCRIPT["GENERIC"])
        zindex = int(node.get("zIndex", 0))
        rec = struct.pack("<I", name_off)
        rec += struct.pack("<6f", float(a), float(b), float(tx),
                           float(c), float(d), float(ty))
        rec += col
        rec += struct.pack("<BBh", flags, script, zindex)
        rec += struct.pack("<HHHH", node["_first_child"], node["_child_count"],
                           node["_first_comp"], node["_comp_count"])
        rec += struct.pack("<I", 0)         # props (link block) not emitted
        assert len(rec) == 48, len(rec)
        blob.write(rec)

    entry = struct.pack("<IIIII", blob.str(name), nodes_off, node_count,
                        comp_offs_off, len(comp_offsets))
    rep = {
        "name": name, "node_count": node_count, "comp_count": len(comp_offsets),
        "max_first_child": max_first_child, "max_first_comp": max_first_comp,
        "overflow": over,
    }
    return entry, rep


# ---------------------------------------------------------------------------
# images / sprites / fonts / sounds / anims (supporting tables)
# ---------------------------------------------------------------------------

def decode_image(path, rgb565, tile):
    if Image is None or not os.path.exists(path):
        return None
    try:
        im = Image.open(path).convert("RGBA")
    except Exception:
        return None
    w, h = im.size
    flags = (IMG_TILE if tile else 0)
    arr = np.asarray(im, dtype=np.uint8)          # (h, w, 4) RGBA
    if rgb565:
        flags |= IMG_RGB565
        R = arr[:, :, 0].astype(np.uint16)
        G = arr[:, :, 1].astype(np.uint16)
        B = arr[:, :, 2].astype(np.uint16)
        v = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)   # RGB565
        # INTERLEAVED 3 bytes/px: 565 low, 565 high, alpha (matches engine src_rgba)
        out = np.empty((h, w, 3), dtype=np.uint8)
        out[:, :, 0] = (v & 0xff).astype(np.uint8)
        out[:, :, 1] = ((v >> 8) & 0xff).astype(np.uint8)
        out[:, :, 2] = arr[:, :, 3]
        return w, h, flags, out.tobytes()
    return w, h, flags, arr.tobytes()             # RGBA8888 interleaved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("asset_dir")
    ap.add_argument("out")
    ap.add_argument("--rgb565", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    asset_dir = args.asset_dir
    rgb565 = args.rgb565
    blob = Blob()

    def rp(path):
        return os.path.join(asset_dir, path.lstrip("/"))

    dep_path = os.path.join(asset_dir, "dependencies.json")
    resources = json.load(open(dep_path))["resources"] if os.path.exists(dep_path) else []

    res_map = {}          # id -> (RES_*, per-type index)
    img_entries = []      # (w,h,flags,pixels_off)

    def add_image(path, tile=False):
        dec = decode_image(path, rgb565, tile)
        if dec is None:
            return None
        w, h, flags, pixels = dec
        blob.align(4)
        px_off = blob.write(pixels)
        idx = len(img_entries)
        img_entries.append((w, h, flags, px_off))
        return idx

    def is_tile(path):
        return "/wallpaper/" in path.lower()

    # ---- images (TextureResource) ----
    for r in resources:
        if r["type"] != "TextureResource" or not r.get("path"):
            continue
        idx = add_image(rp(r["path"]), is_tile(r["path"]))
        if idx is not None:
            res_map[r["id"]] = (RES_TEXTURE, idx)

    tex_idx = {rid: ix for rid, (t, ix) in res_map.items() if t == RES_TEXTURE}

    # SpriteSheet: maps a texture id (its first dependency) to a decodable png.
    sheet_png = {}
    for r in resources:
        if r["type"] != "SpriteSheet" or not r.get("path"):
            continue
        deps = r.get("dependencies", [])
        if not deps:
            continue
        tid = deps[0]["id"]
        png = os.path.splitext(rp(r["path"]))[0] + ".png"
        if os.path.exists(png):
            sheet_png[tid] = png

    def texture_image(tid):
        if tid in tex_idx:
            return tex_idx[tid]
        png = sheet_png.get(tid)
        if png:
            ix = add_image(png, False)
            if ix is not None:
                tex_idx[tid] = ix
                return ix
        return None

    # ---- sprites (SpriteResource): geometry inline in `content` ----
    spr_entries = []
    for r in resources:
        if r["type"] != "SpriteResource":
            continue
        try:
            cj = json.loads(r.get("content", "") or "{}")
        except Exception:
            continue
        tid = cj.get("textureId")
        if not tid:
            continue
        img_idx = texture_image(tid)
        if img_idx is None:
            continue
        fr = cj.get("textureFrame", [0, 0, 0, 0])
        piv = cj.get("pivot", [0, 0])
        res_map[r["id"]] = (RES_SPRITE, len(spr_entries))
        spr_entries.append((img_idx, int(fr[0]), int(fr[1]), int(fr[2]),
                            int(fr[3]), int(piv[0]), int(piv[1])))

    # ---- fonts (FontResource, BMFont json) ----
    font_entries = []
    for r in resources:
        if r["type"] != "FontResource" or not r.get("path"):
            continue
        jp = rp(r["path"])
        if not os.path.exists(jp):
            cands = glob.glob(os.path.splitext(jp)[0] + "*.json")
            jp = cands[0] if cands else None
        if not jp or not os.path.exists(jp):
            continue
        try:
            fj = json.load(open(jp))
        except Exception:
            continue
        common = fj.get("common", {})
        pages = fj.get("pages", [])
        page_idx = 0
        # the json's pages[] often names a non-existent *_eur.fnt_0.png; the real
        # page is "<jsonbase>_0.png". Try that first, then pages[], then a glob.
        base = jp[:-5] if jp.endswith(".json") else jp
        cands = [base + "_0.png"] + [os.path.join(os.path.dirname(jp), p) for p in pages]
        pngpath = next((c for c in cands if c and os.path.exists(c)), None)
        if pngpath is None:
            g = glob.glob(base + "*_0.png") or glob.glob(os.path.join(os.path.dirname(jp), "*_0.png"))
            pngpath = g[0] if g else None
        if pngpath:
            pi = add_image(pngpath, False)
            if pi is not None:
                page_idx = pi
        else:
            print("  !! font page not found for %s" % r["path"])
        glyphs = []
        for cp, g in fj.get("chars", {}).items():
            glyphs.append((int(cp), int(g["x"]), int(g["y"]), int(g["w"]),
                           int(g["h"]), int(g["xo"]), int(g["yo"]), int(g["xadv"])))
        glyphs.sort(key=lambda t: t[0])
        blob.align(4)
        glyphs_off = blob.tell()
        for (cp, x, y, w2, h2, xo, yo, xadv) in glyphs:
            blob.write(struct.pack("<IHHHHhhhh", cp, x, y, w2, h2, xo, yo, xadv, 0))
        idx = len(font_entries)
        res_map[r["id"]] = (RES_FONT, idx)
        # also register the font under its basename (e.g. "title.font") so the
        # engine can look fonts up by name without knowing the GUID.
        res_map[os.path.basename(r["path"])] = (RES_FONT, idx)
        font_entries.append((page_idx, int(common.get("lineHeight", 0)),
                             int(common.get("base", 0)), len(glyphs), glyphs_off))

    # ---- sounds (SoundResource, WAV) ----
    # boot_b is tight and BGM at 44.1k stereo is ~9 MB, so downmix every sound to
    # MONO and resample to SND_RATE (24 kHz); the LK mixer upsamples x2 to the
    # 48 kHz AFE ring. Loop points (WAV 'smpl' chunk) are parsed and rescaled so
    # the BGM still loops seamlessly.
    SND_RATE = 24000
    snd_entries = []
    for r in resources:
        if r["type"] != "SoundResource" or not r.get("path"):
            continue
        wp = rp(r["path"])
        if not os.path.exists(wp):
            continue
        try:
            wav = _read_wav_mono(wp)     # -> (rate, float32 mono [-1,1], loop or None)
        except Exception as e:
            print("  !! sound skip %s: %s" % (r["path"], e))
            continue
        rate, mono, loop = wav
        # resample to SND_RATE (linear); mono stays mono
        if rate != SND_RATE and len(mono):
            n_out = max(1, int(round(len(mono) * SND_RATE / rate)))
            xi = np.linspace(0, len(mono) - 1, n_out, dtype=np.float64)
            mono = np.interp(xi, np.arange(len(mono)), mono).astype(np.float32)
            if loop:
                loop = (int(loop[0] * SND_RATE / rate), int(loop[1] * SND_RATE / rate))
        s16 = np.clip(mono * 32767.0, -32768, 32767).astype("<i2").tobytes()
        nframes = len(mono)
        ls, le = (loop if loop else (0, 0))
        blob.align(4)
        pcm_off = blob.write(s16)
        res_map[r["id"]] = (RES_SOUND, len(snd_entries))
        snd_entries.append((SND_RATE, nframes, 1, 0, ls, le, pcm_off))

    # ---- scene animations (SceneAnimation) ----
    anim_entries = []
    for r in resources:
        if r["type"] != "SceneAnimation" or not r.get("path"):
            continue
        ap_ = rp(r["path"])
        if not os.path.exists(ap_):
            continue
        try:
            aj = json.load(open(ap_))
        except Exception:
            continue
        track_recs = []
        for a in aj.get("animations", []):
            path_names = a.get("worldNodePath", [])
            # intern path-name strings first so the u32[] run stays contiguous
            path_offs = [blob.str(nm) for nm in path_names]
            blob.align(4)
            np_off = blob.tell()
            blob.write(struct.pack("<H", len(path_names)))
            blob.align(4)
            for po in path_offs:
                blob.write(struct.pack("<I", po))
            typ = {"float": 0, "bool": 1, "resource": 2}.get(a.get("type", "float"), 0)
            prop = PROP_MAP.get(a.get("propertyId", ""), 0)
            keys = a.get("keyCurves", [])
            blob.align(4)
            keys_off = blob.tell()
            for k in keys:
                t = max(0, int(float(k.get("time", 0)) * 1000)) & 0xFFFFFFFF
                val = k.get("value", [0])
                v = val[0] if isinstance(val, list) else val
                if typ == 2:
                    bits = fnv1a(str(v)) if v else 0
                elif typ == 1:
                    bits = 1 if v else 0
                else:
                    bits = struct.unpack("<I", struct.pack("<f", float(v)))[0]
                blob.write(struct.pack("<II", t, bits))
            track_recs.append((np_off, prop, typ, keys_off, len(keys)))
        blob.align(4)
        tracks_off = blob.tell()
        for (np_off, prop, typ, keys_off, kc) in track_recs:
            blob.write(struct.pack("<IHBBII", np_off, prop, typ, 0, keys_off, kc))
        dur = max(0, int(float(aj.get("duration", 0)) * 1000)) & 0xFFFFFFFF
        res_map[r["id"]] = (RES_SCENEANIM, len(anim_entries))
        anim_entries.append((dur, tracks_off, len(track_recs)))

    # ---- sprite animations (SpriteAnimationResource): frame lists ----
    # Each .spriteanim = {sprite_sheet:"sheet_<guid>", frames:[[ms,"name.png"],...]}.
    # A frame's sprite id is "<guid>.<name>" (how SpriteResources are keyed), so
    # map each frame name to its packed spr index and emit snes_sanim_frame[].
    sanim_entries = []
    for r in resources:
        if r["type"] != "SpriteAnimationResource":
            continue
        frame_off = 0
        fcount = 0
        p = rp(r["path"]) if r.get("path") else None
        if p and os.path.exists(p):
            try:
                sj = json.load(open(p))
            except Exception:
                sj = None
            if sj:
                sheet = sj.get("sprite_sheet", "")
                guid = sheet[6:] if sheet.startswith("sheet_") else sheet
                recs = []
                for fr in sj.get("frames", []):
                    if not isinstance(fr, (list, tuple)) or len(fr) < 2:
                        continue
                    sid = guid + "." + str(fr[1])
                    ent = res_map.get(sid)
                    if ent and ent[0] == RES_SPRITE:
                        recs.append((float(fr[0]), ent[1]))
                if recs:
                    blob.align(4)
                    frame_off = blob.tell()
                    for (ms, spr) in recs:
                        blob.write(struct.pack("<fHH", ms, spr, 0))
                    fcount = len(recs)
        res_map[r["id"]] = (RES_SPRITEANIM, len(sanim_entries))
        sanim_entries.append((0, fcount, frame_off))

    # ---- scenes ----
    scene_entries = []
    scene_reports = []
    scene_by_name = {}
    scene_dir = os.path.join(asset_dir, "scenes")
    for sf in sorted(glob.glob(os.path.join(scene_dir, "*.scn"))):
        name = os.path.basename(sf)
        try:
            sj = json.load(open(sf))
        except Exception:
            continue
        entry, rep = emit_scene(blob, sj, name)
        scene_by_name[name] = len(scene_entries)
        scene_entries.append(entry)
        scene_reports.append(rep)

    for r in resources:
        if r["type"] != "Scene" or not r.get("path"):
            continue
        nm = os.path.basename(r["path"])
        if nm in scene_by_name:
            res_map[r["id"]] = (RES_SCENE, scene_by_name[nm])

    for r in resources:
        if r["id"] in res_map:
            continue
        if r["type"] == "RawResource":
            res_map[r["id"]] = (RES_RAW, 0)
        elif r["type"] == "RenderTargetResource":
            res_map[r["id"]] = (RES_RENDERTARGET, 0)

    # ---- strings (per locale) ----
    str_tables = []
    strings_dir = os.path.join(asset_dir, "strings")
    if os.path.isdir(strings_dir):
        for locale in sorted(os.listdir(strings_dir)):
            lp = os.path.join(strings_dir, locale, "strings.lng")
            if not os.path.exists(lp):
                cands = glob.glob(os.path.join(strings_dir, locale, "*.lng"))
                lp = cands[0] if cands else None
            if not lp or not os.path.exists(lp):
                continue
            try:
                sj = json.load(open(lp))
            except Exception:
                continue
            items = [(blob.str(k), blob.str(v)) for k, v in sj.get("strings", {}).items()]
            blob.align(4)
            pairs_off = blob.tell()
            for (k, v) in items:
                blob.write(struct.pack("<II", k, v))
            str_tables.append((blob.str(locale), pairs_off, len(items)))

    # ---- game roster (real metadata from games/index.json) ----
    game_recs = []
    games_dir = os.path.join(asset_dir, "games")
    index = []
    try:
        index = json.load(open(os.path.join(games_dir, "index.json")))
    except Exception:
        index = []
    def _rel(d):
        try:
            p = str(d).replace("-", "")
            return int(p[:8]) if len(p) >= 8 and p[:8].isdigit() else 0
        except Exception:
            return 0
    for e in index:
        code = e.get("code", "")
        gp = os.path.join(games_dir, code)
        thumb = add_image(os.path.join(gp, code + ".png"))
        small = add_image(os.path.join(gp, code + "_small.png"))
        thumb = 0xFFFF if thumb is None else thumb
        small = 0xFFFF if small is None else small
        try:
            players = max(1, min(255, int(e.get("Players", "1") or 1)))
        except Exception:
            players = 1
        try:
            simul = max(0, min(255, int(e.get("Simultaneous", "0") or 0)))
        except Exception:
            simul = 0
        # intern all strings FIRST (blob.str writes into the blob + advances the
        # cursor), THEN capture rec_off, so game_offs points at the GameRec struct
        # and not at the interned string bytes.
        c_off  = blob.str(code)
        # NOTE: index.json ships display text double-UTF-8-encoded, but the web
        # oracle does NOT repair it - it renders the raw bytes and the mojibake
        # code points (Â, â, ...) are simply absent from the fonts, so they drop
        # out to zero width (the trademark glyphs vanish, the copyright sign
        # survives). We match that verbatim: keep the raw bytes, and make missing
        # glyphs advance 0 (see snes_draw_text) so the layout lines up 1:1.
        n_off  = blob.str(e.get("Name", code))
        p_off  = blob.str(e.get("SortRawPublisher", ""))
        st_off = blob.str(e.get("SortRawTitle", ""))
        cp_off = blob.str(e.get("Copyright", ""))
        blob.align(4)
        rec_off = blob.tell()
        rec = struct.pack("<IIIIII", c_off, n_off, p_off, st_off, p_off, cp_off)
        rec += struct.pack("<BBBB", players, simul, 0, 0)
        rec += struct.pack("<I", _rel(e.get("ReleaseDate", "")))
        rec += struct.pack("<HH", thumb & 0xFFFF, small & 0xFFFF)
        blob.write(rec)
        game_recs.append(rec_off)

    # ---- decorative frame thumbnails (option_display line2 Frame strip) ----
    # The DecorativeFrames Lua populates the horizontal frame strip at runtime
    # from frames/<theme>/<theme>_thumbnail.png. Pack each theme's thumbnail keyed
    # by "frame_thumb_<theme>" so the native render can look it up by name.
    frames_dir = os.path.join(asset_dir, "frames")
    if os.path.isdir(frames_dir):
        for folder in sorted(os.listdir(frames_dir)):
            tp = os.path.join(frames_dir, folder, folder + "_thumbnail.png")
            if os.path.exists(tp):
                fidx = add_image(tp, False)
                if fidx is not None:
                    res_map["frame_thumb_" + folder] = (RES_TEXTURE, fidx)
            # the two mode previews (Display line1 reskins on Apply): the 4:3/CRT
            # modes use <folder>_4_3_preview, DotByDot uses _pixel_perfect_preview.
            for suf, key in (("_4_3_preview", "frame_preview_43_"),
                             ("_pixel_perfect_preview", "frame_preview_pp_")):
                pp = os.path.join(frames_dir, folder, folder + suf + ".png")
                if os.path.exists(pp):
                    pidx = add_image(pp, False)
                    if pidx is not None:
                        res_map[key + folder] = (RES_TEXTURE, pidx)

    # ---- resource hash table (pow2, open addressed) ----
    n = len(res_map)
    cap = 1
    while cap < max(1, n) * 2:
        cap <<= 1
    table = [(0, 0, RES_NONE, 0)] * cap
    mask = cap - 1
    for id_str, (rtype, ridx) in res_map.items():
        h = fnv1a(id_str) or 1
        i = h & mask
        while table[i][0] != 0:
            i = (i + 1) & mask
        table[i] = (h, blob.str(id_str), rtype, ridx)

    # ---- fixed tail tables (all 4-aligned) ----
    blob.align(4)
    res_off = blob.tell()
    for (h, s, t, ix) in table:
        blob.write(struct.pack("<IIHH", h, s, t, ix))
    res_count = cap

    blob.align(4)
    img_off = blob.tell()
    for (w, h, flags, px) in img_entries:
        blob.write(struct.pack("<HHHHI", w, h, flags, 0, px))
    img_count = len(img_entries)

    blob.align(4)
    spr_off = blob.tell()
    for (im, sx, sy, sw, sh, px, py) in spr_entries:
        blob.write(struct.pack("<HHHHHhh", im, sx, sy, sw, sh, px, py))
    spr_count = len(spr_entries)

    blob.align(4)
    font_off = blob.tell()
    for (page, lh, base, gc, goff) in font_entries:
        blob.write(struct.pack("<HHHHI", page, lh, base, gc, goff))
    font_count = len(font_entries)

    blob.align(4)
    scene_off = blob.tell()
    for e in scene_entries:
        blob.write(e)
    scene_count = len(scene_entries)

    blob.align(4)
    anim_off = blob.tell()
    for (dur, toff, tc) in anim_entries:
        blob.write(struct.pack("<III", dur, toff, tc))
    anim_count = len(anim_entries)

    blob.align(4)
    sanim_off = blob.tell()
    for (si, fc, fr) in sanim_entries:
        blob.write(struct.pack("<HHI", si, fc, fr))
    sanim_count = len(sanim_entries)

    blob.align(4)
    snd_off = blob.tell()
    for (rate, frames, ch, fmt, ls, le, pcm) in snd_entries:
        blob.write(struct.pack("<IIBBHIII", rate, frames, ch, fmt, 0, ls, le, pcm))
    snd_count = len(snd_entries)

    blob.align(4)
    str_off = blob.tell()
    for (loc, poff, pc) in str_tables:
        blob.write(struct.pack("<III", loc, poff, pc))
    str_count = len(str_tables)

    blob.align(4)
    game_off = blob.tell()
    for o in game_recs:
        blob.write(struct.pack("<I", o))
    game_count = len(game_recs)

    # ---- init block ----
    init_json = {}
    ij = os.path.join(asset_dir, "init.json")
    if os.path.exists(ij):
        init_json = json.load(open(ij))
    default_scene = init_json.get("defaultScene", "")
    default_locale = blob.str("usa_en") if str_tables else 0
    blob.align(4)
    init_off = blob.tell()
    blob.write(struct.pack("<III", fnv1a(default_scene) if default_scene else 0,
                           0, default_locale))

    # ---- OSS licence text (Legal screen's Open Source Software tab) ----
    # data/static_legal.json ossLines[]: one array entry per rendered line (already
    # wrapped by the source). Intern each into the strpool, then emit a u32[] of
    # their offsets so the engine can render a scroll window by line index.
    oss_lines = []
    legal_json = os.path.join(asset_dir, "data", "static_legal.json")
    if os.path.exists(legal_json):
        try:
            oss_lines = json.load(open(legal_json)).get("ossLines", []) or []
        except Exception:
            oss_lines = []
    oss_str_offs = [blob.str(s) for s in oss_lines]
    blob.align(4)
    oss_off = blob.tell()
    for o in oss_str_offs:
        blob.write(struct.pack("<I", o))
    oss_count = len(oss_str_offs)

    # strpool base is the blob base so string offsets are absolute (matches every
    # other offset and the engine's snes_str = strpool + off with strpool = base).
    strpool_off = 0
    total_size = len(blob.buf)

    hdr = struct.pack(
        HEADER_FMT,
        MAGIC, VERSION, total_size, (F_RGB565 if rgb565 else 0),
        strpool_off, total_size,
        res_off, res_count,
        img_off, img_count,
        spr_off, spr_count,
        font_off, font_count,
        scene_off, scene_count,
        anim_off, anim_count,
        sanim_off, sanim_count,
        snd_off, snd_count,
        str_off, str_count,
        game_off, game_count,
        init_off,
        oss_off, oss_count,
    )
    blob.buf[:HEADER_SIZE] = hdr

    with open(args.out, "wb") as f:
        f.write(blob.buf)

    # ---- report ----
    max_nc = max((r["node_count"] for r in scene_reports), default=0)
    max_fc = max((r["max_first_child"] for r in scene_reports), default=0)
    max_fcp = max((r["max_first_comp"] for r in scene_reports), default=0)
    total_comp = sum(r["comp_count"] for r in scene_reports)
    overflows = [(r["name"], r["overflow"]) for r in scene_reports if r["overflow"]]

    print("wrote %s (%d bytes)" % (args.out, total_size))
    print("tables: res=%d(pow2) img=%d spr=%d font=%d scene=%d anim=%d sanim=%d snd=%d str=%d games=%d"
          % (res_count, img_count, spr_count, font_count, scene_count,
             anim_count, sanim_count, snd_count, str_count, game_count))
    print("scenes: max node_count=%d  max first_child=%d  max first_comp=%d  total comp records=%d"
          % (max_nc, max_fc, max_fcp, total_comp))
    if overflows:
        print("!! u16 OVERFLOW in:", overflows)
    else:
        print("u16 ranges OK (all first_child/first_comp < 65536)")

    if args.report:
        for r in scene_reports:
            print("  %-34s nodes=%-5d comps=%-4d first_child<=%d first_comp<=%d"
                  % (r["name"], r["node_count"], r["comp_count"],
                     r["max_first_child"], r["max_first_comp"]))


if __name__ == "__main__":
    main()
