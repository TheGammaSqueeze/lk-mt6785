#!/usr/bin/env python3
"""Complete the menu fonts' character sets (run once; the results are committed).

The menu's bitmap fonts (assets/fonts/*.font.json + *_0.png, BMFont layout) only hold the
characters their original screens needed: title.font has no digit but 2, no J, Q, U, '&' or
'.', so a game called "Super Mario Advance 3" showed as "Super Mario Advance". This tool adds
every missing printable ASCII character (and, for the title font, every glyph its twin l.font
has) to each atlas, and every letter GammaOS's own texts need in the languages of the Language
page (the Boot to OS entries in pico_current/i18n/classic, and the texts the classic menu takes
from pico_current/i18n: its relabelled rows, sort notes and Beta tag).

Each missing glyph comes from the source that draws most like the target, measured over the
characters both already have (the overlap of their shapes once scaled to the same height):
  - another of the menu's fonts (title.font and l.font are the same design at the same size,
    m.font and s.font the family at smaller sizes), scaled by the ratio of their glyph heights;
  - else DejaVu Sans, rendered at the size whose capitals match the target's.
A source's glyphs are then given the target's look: a smaller size scaled up has soft edges, so
its edges are sharpened and its strokes thickened (both searched together) until its glyphs fill
their boxes as much as the target's and show as few half covered edge pixels.
New glyphs go into rows below the existing ones; the atlas grows to the next power of two.

  python3 tools/ayaneo/snes/fill_font_glyphs.py            # rewrite the fonts in place
  python3 tools/ayaneo/snes/fill_font_glyphs.py --check    # report what is missing, write nothing
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "assets", "fonts")
TARGETS = ["title.font", "l.font", "m.font", "s.font"]
FAMILY = ["title.font", "l.font", "m.font", "s.font"]
FALLBACK_TTF = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
ASCII = [chr(c) for c in range(0x21, 0x7F)]
I18N = os.path.join(HERE, "..", "pico_current", "i18n")
CLASSIC_LANGS = ["fr", "de", "es", "it", "nl", "pt", "ru"]          # the Language page's, but English
CLASSIC_TEXTS = ["Mute BIOS Audio", "Mute BIOS", "Mute Menu Audio", "Mute Menu", "Sort: Title",
                 "Sort: Publisher", "Sort: Players", "Sort: Release", "Sort A to Z", "Sort Z to A", "Beta"]


def gammaos_letters():
    """The letters of GammaOS's texts in the classic menu's languages."""
    out = []
    for code in CLASSIC_LANGS:
        d = json.load(open(os.path.join(I18N, code + ".json"), encoding="utf-8"))
        out += [d[k] for k in CLASSIC_TEXTS if isinstance(d.get(k), str)]
    for name in sorted(os.listdir(os.path.join(I18N, "classic"))):
        if name.startswith("eur_"):
            out += json.load(open(os.path.join(I18N, "classic", name), encoding="utf-8")).values()
    return sorted({ch for t in out for ch in t if ch.isprintable() and not ch.isspace() and ord(ch) > 0x7E})
PAD = 2


class Font:
    def __init__(self, name):
        self.name = name
        self.json_path = os.path.join(FONTS, name + ".json")
        self.png_path = os.path.join(FONTS, name + "_0.png")
        self.j = json.load(open(self.json_path))
        self.img = Image.open(self.png_path).convert("RGBA")
        self.base = int(self.j["common"]["base"])

    def has(self, ch):
        return str(ord(ch)) in self.j["chars"]

    def glyph(self, ch):
        g = self.j["chars"][str(ord(ch))]
        a = self.img.crop((g["x"], g["y"], g["x"] + g["w"], g["y"] + g["h"])).split()[3]
        return a, g

    def ink_height(self, ch):
        a, _ = self.glyph(ch)
        bb = a.point(lambda v: 255 if v > 80 else 0).getbbox()
        return (bb[3] - bb[1]) if bb else 0


def overlap(a, b):
    """Shape overlap (intersection over union) of two alpha images, b scaled to a's ink height."""
    ba = a.point(lambda v: 255 if v > 80 else 0).getbbox()
    bb = b.point(lambda v: 255 if v > 80 else 0).getbbox()
    if not ba or not bb:
        return 0.0
    A = a.crop(ba)
    B = b.crop(bb)
    k = A.height / B.height
    B = B.resize((max(1, round(B.width * k)), A.height), Image.LANCZOS)
    X = np.zeros((A.height, max(A.width, B.width)))
    Y = np.zeros_like(X)
    X[:, :A.width] = np.asarray(A) / 255.0
    Y[:, :B.width] = np.asarray(B) / 255.0
    u = np.maximum(X, Y).sum()
    return float(np.minimum(X, Y).sum() / u) if u else 0.0


def coverage(a):
    """How much of its ink box a glyph fills (its stroke weight, independent of size)."""
    bb = a.point(lambda v: 255 if v > 80 else 0).getbbox()
    if not bb:
        return 0.0
    box = np.asarray(a.crop(bb), dtype=np.float64) / 255.0
    return float(box.mean())


def embolden(a, px):
    """Grow the strokes by px pixels (fractions too: done at 4x). Returns the image and the margin
    added on each side."""
    if px <= 0:
        return a, 0
    up, pad = 4, int(np.ceil(px)) + 1
    big = Image.new("L", ((a.width + 2 * pad) * up, (a.height + 2 * pad) * up))
    big.paste(a.resize((a.width * up, a.height * up), Image.LANCZOS), (pad * up, pad * up))
    n = int(round(px * up))
    big = big.filter(ImageFilter.MaxFilter(2 * n + 1))
    return big.resize((a.width + 2 * pad, a.height + 2 * pad), Image.LANCZOS), pad


def sharpen(a, gain):
    """Steepen the edges: alpha moved away from half coverage by gain."""
    if gain == 1.0:
        return a
    return a.point(lambda v: max(0, min(255, int(round((v - 128) * gain + 128)))))


def softness(a):
    """The share of a glyph's ink pixels that are only partly covered (its edge softness)."""
    v = np.asarray(a, dtype=np.float64) / 255.0
    ink = (v > 0.1).sum()
    return float(((v > 0.1) & (v < 0.9)).sum() / ink) if ink else 0.0


def restyle(a, look):
    px, gain = look
    a, pad = embolden(a, px)
    return sharpen(a, gain), pad


def match_look(target, glyphs):
    """The emboldening (0 to 1.5 px) and edge gain (1 to 3) that bring the source's glyphs (char,
    image at the target's size) closest to the target's own weight and edge softness."""
    if not glyphs:
        return (0.0, 1.0)
    want_c = np.mean([coverage(target.glyph(c)[0]) for c, _ in glyphs])
    want_s = np.mean([softness(target.glyph(c)[0]) for c, _ in glyphs])
    best, best_err = (0.0, 1.0), None
    for px in np.arange(0.0, 1.51, 0.25):
        for gain in np.arange(1.0, 3.01, 0.25):
            styled = [restyle(a, (px, gain))[0] for _, a in glyphs]
            err = abs(np.mean([coverage(x) for x in styled]) - want_c) / max(want_c, 1e-6) + \
                  abs(np.mean([softness(x) for x in styled]) - want_s) / max(want_s, 1e-6)
            if best_err is None or err < best_err:
                best, best_err = (float(px), float(gain)), err
    return best

class FamilySource:
    def __init__(self, font, target):
        self.font = font
        self.label = font.name
        common = [c for c in ASCII if c.isalnum() and target.has(c) and font.has(c)]
        ratios = [target.ink_height(c) / font.ink_height(c) for c in common if font.ink_height(c)]
        self.k = float(np.median(ratios)) if ratios else 1.0
        if abs(self.k - 1.0) < 0.02:
            self.k = 1.0
        self.score = float(np.mean([overlap(target.glyph(c)[0], font.glyph(c)[0]) for c in common])) if common else 0.0
        self.look = match_look(target, [(c, self.scaled(c)) for c in common])

    def has(self, ch):
        return self.font.has(ch)

    def scaled(self, ch):
        a, _ = self.font.glyph(ch)
        if self.k != 1.0:
            a = a.resize((max(1, round(a.width * self.k)), max(1, round(a.height * self.k))), Image.LANCZOS)
        return a

    def make(self, ch, target):
        _, g = self.font.glyph(ch)
        k = self.k
        a, pad = restyle(self.scaled(ch), self.look)
        above = self.font.base - int(g["yo"])          # glyph top to baseline, in the source
        return a, {
            "xo": round(int(g["xo"]) * k) - pad,
            "yo": target.base - round(above * k) - pad,
            "xadv": round(int(g["xadv"]) * k + self.look[0]),
        }


class TtfSource:
    def __init__(self, path, target):
        self.path = path
        self.label = os.path.basename(path)
        common = [c for c in ASCII if c.isalnum() and target.has(c)]
        want = np.median([target.ink_height(c) for c in "EHT" if target.has(c)] or [target.ink_height(common[0])])
        size = 8
        while size < 200:
            f = ImageFont.truetype(path, size)
            l, t, r, b = f.getbbox("H", anchor="ls")
            if b - t >= want:
                break
            size += 1
        self.f = ImageFont.truetype(path, size)
        self.score = float(np.mean([overlap(target.glyph(c)[0], self.render(c)[0]) for c in common[:24]])) if common else 0.0
        self.look = match_look(target, [(c, self.render(c)[0]) for c in common[:24]])

    def has(self, ch):
        return self.f.getmask(ch).getbbox() is not None

    def render(self, ch):
        l, t, r, b = self.f.getbbox(ch, anchor="ls")
        im = Image.new("L", (max(1, r - l), max(1, b - t)))
        ImageDraw.Draw(im).text((-l, -t), ch, font=self.f, fill=255, anchor="ls")
        return im, (l, t)

    def make(self, ch, target):
        a, (l, t) = self.render(ch)
        a, pad = restyle(a, self.look)
        return a, {"xo": l - pad, "yo": target.base + t - pad, "xadv": round(self.f.getlength(ch) + self.look[0])}


def wanted(target, fonts):
    want = list(ASCII) + gammaos_letters()
    if target.name == "title.font":            # its twin's extra glyphs too (accents, Cyrillic)
        want += [chr(int(k)) for k in fonts["l.font"].j["chars"]]
    seen, out = set(), []
    for ch in want:
        if ch not in seen and not target.has(ch):
            seen.add(ch)
            out.append(ch)
    return out


def fill(target, fonts, check):
    missing = wanted(target, fonts)
    if not missing:
        print("%-10s complete" % target.name)
        return
    sources = [FamilySource(fonts[n], target) for n in FAMILY if n != target.name]
    sources.append(TtfSource(FALLBACK_TTF, target))
    sources.sort(key=lambda s: -s.score)
    print("%-10s missing %d; sources %s" % (target.name, len(missing),
          ", ".join("%s %.3f (+%.2f px, edge x%.2f)" % (s.label, s.score, s.look[0], s.look[1]) for s in sources)))
    made, used = [], {}
    for ch in missing:
        src = next((s for s in sources if s.has(ch)), None)
        if src is None:
            continue
        a, m = src.make(ch, target)
        made.append((ch, a, m))
        used.setdefault(src.label, []).append(ch)
    for label, chars in used.items():
        print("   from %-16s %s" % (label, "".join(chars)))
    if check or not made:
        return

    # shelf-pack the new glyphs under the existing ones
    W = target.img.width
    top = max(int(g["y"]) + int(g["h"]) for g in target.j["chars"].values()) + PAD
    x, y, row_h, place = PAD, top, 0, []
    for ch, a, m in made:
        if x + a.width + PAD > W:
            x, y, row_h = PAD, y + row_h + PAD, 0
        place.append((ch, a, m, x, y))
        x += a.width + PAD
        row_h = max(row_h, a.height)
    H = target.img.height
    while y + row_h + PAD > H:
        H *= 2
    img = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    img.paste(target.img, (0, 0))
    for ch, a, m, px, py in place:
        glyph = Image.merge("RGBA", (Image.new("L", a.size, 255),) * 3 + (a,))
        img.paste(glyph, (px, py))
        target.j["chars"][str(ord(ch))] = {"x": px, "y": py, "w": a.width, "h": a.height,
                                           "xo": m["xo"], "yo": m["yo"], "xadv": m["xadv"],
                                           "page": 0, "chnl": 15}
    target.j["common"]["scaleH"] = H
    target.j["chars"] = dict(sorted(target.j["chars"].items(), key=lambda kv: int(kv[0])))
    img.save(target.png_path)
    with open(target.json_path, "w") as f:
        json.dump(target.j, f)
    print("   %d glyphs added, atlas %dx%d" % (len(made), W, H))


def main():
    check = "--check" in sys.argv[1:]
    # sources are the atlases as they were, so no added glyph is used again for another font
    sources = {n: Font(n) for n in FAMILY}
    for n in TARGETS:
        fill(Font(n), sources, check)


if __name__ == "__main__":
    main()
