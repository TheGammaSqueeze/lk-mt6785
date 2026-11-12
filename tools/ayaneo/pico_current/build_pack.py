#!/usr/bin/env python3
"""
Build the card pack of the Current menu theme for one screen size.

    build_pack.py <W>x<H> <out.pack> --theme NAME=DIR [--theme NAME=DIR ...]

The --theme list is the card's theme list in menu order (the same as pack_snes.py gets); the
pack carries their names so the theme's own Settings can switch to any of them. Needs Pillow and
NumPy.

The layout is drawn in design units, 720 units tall (pico_current.c works in the same units),
so everything here is rendered at the exact pixel size the LK draws it at, for this screen.

Pack: ["SNSZ"][u32 raw length][u32 compressed length][raw deflate of]:
  header   'PCUR', u32 version, u32 entry count, u32 screen W | H << 16
  entries  48 bytes each: char name[32], u16 type, u16 flags, u16 w, u16 h, u32 offset, u32 length
  type 1   image: u16 RGB565[w*h], then u8 alpha[w*h] when flags & 1
  type 2   alpha mask: u8 alpha[w*h] (drawn in a colour)
  type 3   font: see write_font
  type 4   text: UTF-8, no terminator
Offsets are from the start of the raw pack and 4-byte aligned.
"""
import argparse, os, struct, zlib
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
SYSTEMS = ["gb", "gbc", "gba", "snes", "genesis", "sms", "gg", "sg"]
T_IMAGE, T_MASK, T_FONT, T_TEXT = 1, 2, 3, 4
CHARS = [c for c in range(0x20, 0x7F)] + [c for c in range(0xA0, 0x100)] + [0x2022, 0x2026]

# font roles: (name, size in units, weight)
FONTS = [
    ("brand", 16, "SemiBold"), ("sub", 15, "Light"), ("realm", 15, "Medium"),
    ("rail", 16, "Regular"), ("railf", 16, "Medium"),
    ("tag13", 13, "SemiBold"), ("tag11", 11, "SemiBold"),
    ("count", 21, "Regular"), ("lib", 19, "Light"), ("button", 15, "SemiBold"),
    ("row", 22, "Regular"), ("rowf", 22, "Medium"), ("info", 15, "Regular"),
    ("srow", 21, "Regular"), ("srowf", 21, "Medium"), ("sval", 16, "Regular"),
    ("dname", 29, "Regular"), ("dnum", 38, "Light"), ("dunit", 20, "Light"),
    ("hintc", 10, "SemiBold"), ("hint", 13, "Medium"), ("empty", 19, "Light"),
]


class Pack:
    def __init__(self):
        self.entries, self.data = [], bytearray()

    def add(self, name, kind, w, h, payload, flags=0):
        assert len(name) < 32, name
        while len(self.data) % 4:
            self.data.append(0)
        self.entries.append((name, kind, flags, w, h, len(self.data), len(payload)))
        self.data += payload

    def image(self, name, im):
        a = np.asarray(im.convert("RGBA")).astype(np.uint16)
        h, w = a.shape[:2]
        rgb = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
        alpha = a[:, :, 3].astype(np.uint8)
        has_alpha = bool((alpha != 255).any())
        payload = rgb.astype("<u2").tobytes() + (alpha.tobytes() if has_alpha else b"")
        self.add(name, T_IMAGE, w, h, payload, 1 if has_alpha else 0)

    def mask(self, name, im):
        a = im.getchannel("A") if im.mode == "RGBA" else im.convert("L")
        self.add(name, T_MASK, a.width, a.height, a.tobytes())

    def text(self, name, s):
        self.add(name, T_TEXT, 0, 0, s.encode("utf-8"))

    def write(self, path, screen):
        hdr = struct.pack("<4sIII", b"PCUR", 1, len(self.entries), screen[0] | (screen[1] << 16))
        table = bytearray()
        base = len(hdr) + 48 * len(self.entries)
        for name, kind, flags, w, h, off, ln in self.entries:
            table += struct.pack("<32sHHHHII", name.encode(), kind, flags, w, h, base + off, ln)
        raw = hdr + bytes(table) + bytes(self.data)
        comp = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = comp.compress(raw) + comp.flush()
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "wb") as f:
            f.write(struct.pack("<4sII", b"SNSZ", len(raw), len(z)) + z)
        return len(raw), len(z)


def fit(im, bw, bh):
    """Scale to fit inside bw x bh, keeping the aspect (never upscaled past the box)."""
    k = min(bw / im.width, bh / im.height)
    return im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)


def write_font(pack, name, path, px):
    """Font entry: u16 px, ascent, descent, count, atlas w, atlas h, kern count, pad;
    then per glyph u32 code, s16 xoff, s16 yoff (from the pen at the baseline), u16 x, y, w, h,
    u32 advance (1/64 px); then kerning u32 (first << 16 | second), s32 (1/64 px), sorted;
    then the u8 atlas."""
    f = ImageFont.truetype(path, px)
    asc, desc = f.getmetrics()
    glyphs = []
    for c in CHARS:
        ch = chr(c)
        bb = f.getbbox(ch, anchor="ls")
        w, h = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
        glyphs.append([c, bb[0], bb[1], w, h, round(f.getlength(ch) * 64)])
    # shelf-pack the glyphs into a 512-wide atlas
    aw, x, y, shelf = 512, 0, 0, 0
    for g in glyphs:
        if x + g[3] + 1 > aw:
            x, y, shelf = 0, y + shelf + 1, 0
        g.append(x); g.append(y)
        x += g[3] + 1
        shelf = max(shelf, g[4])
    ah = y + shelf + 1
    atlas = Image.new("L", (aw, ah), 0)
    d = ImageDraw.Draw(atlas)
    for c, xo, yo, w, h, adv, gx, gy in glyphs:
        if w and h:
            d.text((gx - xo, gy - yo), chr(c), font=f, fill=255, anchor="ls")
    kern = []
    base = {c: f.getlength(chr(c)) for c in range(0x20, 0x7F)}
    for a in range(0x21, 0x7F):
        for b in range(0x21, 0x7F):
            k = f.getlength(chr(a) + chr(b)) - base[a] - base[b]
            if abs(k) >= 0.25:
                kern.append(((a << 16) | b, round(k * 64)))
    blob = bytearray(struct.pack("<HHHHHHHH", px, asc, desc, len(glyphs), aw, ah, len(kern), 0))
    for c, xo, yo, w, h, adv, gx, gy in glyphs:
        blob += struct.pack("<IhhHHHHI", c, xo, yo, gx, gy, w, h, adv)
    for key, k in sorted(kern):
        blob += struct.pack("<Ii", key, k)
    blob += atlas.tobytes()
    pack.add(name, T_FONT, aw, ah, bytes(blob))


def soft_ellipse(w, h, stops):
    """A radial alpha ramp filling a w x h ellipse: stops of (radius 0..1, alpha 0..1)."""
    im = Image.new("L", (w, h), 0)
    px = im.load()
    for y in range(h):
        for x in range(w):
            r = (((x + .5) / w * 2 - 1) ** 2 + ((y + .5) / h * 2 - 1) ** 2) ** .5
            a = 0.0
            for (r0, a0), (r1, a1) in zip(stops, stops[1:]):
                if r0 <= r <= r1:
                    a = a0 + (a1 - a0) * (r - r0) / (r1 - r0)
                    break
            px[x, y] = round(255 * a)
    return im


def ring(d, stroke, scale=4):
    """An antialiased circle outline, d px across."""
    big = Image.new("L", (d * scale, d * scale), 0)
    dr = ImageDraw.Draw(big)
    s = stroke * scale
    dr.ellipse((s / 2, s / 2, d * scale - s / 2, d * scale - s / 2), outline=255, width=max(1, round(s)))
    return big.resize((d, d), Image.LANCZOS)


def pill(w, h, rad, stroke, scale=4):
    big = Image.new("L", (w * scale, h * scale), 0)
    dr = ImageDraw.Draw(big)
    s = stroke * scale
    dr.rounded_rectangle((s / 2, s / 2, w * scale - s / 2, h * scale - s / 2), radius=rad * scale,
                         outline=255, width=max(1, round(s)))
    return big.resize((w, h), Image.LANCZOS)


def label_of(name, tdir):
    p = os.path.join(tdir, "label.txt")
    if os.path.exists(p):
        return open(p, encoding="utf-8").read().strip()
    return " ".join(w.capitalize() for w in name.split("-"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("size")
    ap.add_argument("out")
    ap.add_argument("--theme", dest="themes", action="append", default=[])
    a = ap.parse_args()
    W, H = (int(v) for v in a.size.lower().split("x"))
    s = H / 720.0                       # px per design unit
    uw = W / s                          # the layout's width in units
    p = max(38.0, uw * .048)            # side margin
    u = lambda v: max(1, round(v * s))
    pack = Pack()

    hw_w, hw_h = uw * .45, 720 * .40     # the hardware stage
    logo_w = min(uw * .39, 414)
    strip_w = min(123.0, (uw - 2 * p - 121) / len(SYSTEMS) - 19)
    lib_w = min(uw * .46, 440)
    art_w = min(uw * .34, 393)
    for sname in SYSTEMS:
        hw = Image.open(os.path.join(ART, "hardware", sname + ".png")).convert("RGBA")
        pack.image("hw_" + sname, fit(hw, u(hw_w), u(hw_h)))
        lg = Image.open(os.path.join(ART, "logos", sname + ".png")).convert("RGBA")
        pack.mask("logo_" + sname, fit(lg, u(logo_w), u(83)))
        pack.mask("slogo_" + sname, fit(lg, u(strip_w), u(33)))
        pack.mask("llogo_" + sname, fit(lg, u(lib_w), u(58)))
        pack.mask("alogo_" + sname, fit(lg, u(art_w * .72), u(64)))   # no box art: the mark in the art box

    for name, size in (("games", 34), ("resume", 34), ("settings", 34)):
        im = Image.open(os.path.join(ART, "icons", name + ".png"))
        pack.mask("ic_%s_f" % name, im.resize((u(size), u(size)), Image.LANCZOS))
        pack.mask("ic_%s" % name, im.resize((u(29), u(29)), Image.LANCZOS))
    for name in ("library", "play"):
        pack.mask("ic_" + name, Image.open(os.path.join(ART, "icons", name + ".png")).resize((u(18), u(18)), Image.LANCZOS))
    for name in ("sun", "sound", "bfi", "display", "colour", "power", "mute"):
        pack.mask("ic_" + name, Image.open(os.path.join(ART, "icons", name + ".png")).resize((u(39), u(39)), Image.LANCZOS))
    pack.mask("gamma", Image.open(os.path.join(ART, "icons", "gamma.png")).resize((u(26), u(26) * 128 // 128), Image.LANCZOS))

    pack.mask("shadow", soft_ellipse(u(hw_w * .9), u(hw_h * .26), [(0, .48), (.64, .16), (1, 0)]))
    pack.mask("glow", soft_ellipse(u(90), u(58), [(0, .16), (1, 0)]))
    pack.mask("hint_ring", ring(u(16.6), 1.2 * s))
    pack.mask("hint_pill", pill(u(27), u(16), 4 * s, 1.1 * s))

    fdir = os.path.join(ART, "fonts")
    for name, size, weight in FONTS:
        write_font(pack, "f_" + name, os.path.join(fdir, "Inter-%s.ttf" % weight), u(size))

    lines = []
    for spec in a.themes:
        name, tdir = spec.split("=", 1)
        lines.append("%s|%s" % (name, label_of(name, tdir)))
    pack.text("themes", "\n".join(lines))

    raw, z = pack.write(a.out, (W, H))
    print("current pack %dx%d: %d entries, %d bytes (%d raw)" % (W, H, len(pack.entries), z, raw))


if __name__ == "__main__":
    main()
