#!/usr/bin/env python3
"""
Build the menus' language packs for one screen size, one per language but English:
<out dir>/lang-<code>-<W>x<H>.pack, which the LK reads from /gammaos (emu/gba/pico_i18n.c).

    build_lang.py <W>x<H> <out dir> [--only CODE ...]

i18n/strings.json lists the English of every text the menus look up; i18n/<code>.json gives each
one in the language: a text, or for a text with a count its forms ("one", "two", "few", "many",
"zero", "other"). A text the language file lacks stays English (and is listed as a warning).

A pack is a menu pack (build_pack.Pack) with:
  "strings"  type 5: u32 count, then count x (u32 FNV-1a hash of the English, u32 offset of the
             text from the entry's start), sorted by hash, then the texts, UTF-8, each ending in a
             0. A count's other forms are under "<English>#one", "#two", "#few", "#many", "#zero";
             the English itself holds "other" (or "many" for languages without it).
  "f_<role>" type 3: for every menu font (build_pack.FONTS), the glyphs of this language that the
             font in the theme's packs lacks, sorted by code point, laid out as a font entry
             without kerning.

The texts are checked: their placeholders (%d, %s, %02d) as the English has them, no dashes but
the hyphen, every letter drawable, and the ones in tight places measured against their room.
"""
import argparse, json, os, re, struct, sys
import build_pack as bp
import fontsrc

T_STRINGS = 5
FORMS = ("zero", "one", "two", "few", "many", "other")
PLACEHOLDER = re.compile(r"%0?\d*[ds]")
DASHES = "".join(map(chr, (0x2012, 0x2013, 0x2014, 0x2015)))   # figure, en, em and quotation dashes
ROLE = {name: (size, weight) for name, size, weight in bp.FONTS}
# where a kind of text is drawn: (font role, the most units it may take on the Konkr's 960 x 640,
# whether it is drawn in capitals with 2 units between letters); None: not measured
ROOM = {
    "rail": ("rail", 76, False),
    "heading": ("tag13", 330, True),
    "hint": ("hint", 150, False),
    "label": ("srow", 300, False),
    "value": ("sval", 200, False),
    "status": ("info", 380, False),
}


def fnv(s, h=2166136261):
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def load_keys():
    return json.load(open(os.path.join(fontsrc.I18N, "strings.json"), encoding="utf-8"))["strings"]


def texts_of(code, keys, warn):
    """[(lookup key, text in pack form, the English, kind)] for a language, checked."""
    path = os.path.join(fontsrc.I18N, code + ".json")
    data = json.load(open(path, encoding="utf-8"))
    out, known = [], {k["en"] for k in keys}
    for extra in data:
        if extra not in known:
            warn("%s: %r is not a menu text (strings.json)" % (code, extra))
    for k in keys:
        en, v = k["en"], data.get(k["en"])
        if v is None:
            warn("%s: no text for %r, it stays English" % (code, en))
            continue
        want = PLACEHOLDER.findall(en)
        forms = v if isinstance(v, dict) else {"other": v}
        if isinstance(v, dict):
            bad = [f for f in forms if f not in FORMS]
            if bad or "one" not in k and len(forms) > 1:
                raise SystemExit("%s: %r has forms %s" % (code, en, sorted(forms)))
        for form, t in forms.items():
            if not isinstance(t, str) or not t.strip():
                raise SystemExit("%s: %r (%s) is empty" % (code, en, form))
            if PLACEHOLDER.findall(t) != want:
                raise SystemExit("%s: %r (%s) has placeholders %s, the English %s"
                                 % (code, en, form, PLACEHOLDER.findall(t), want))
            if any(c in t for c in DASHES):
                raise SystemExit("%s: %r has a dash other than the hyphen" % (code, en))
        plain = forms.get("other") or forms.get("many") or next(iter(forms.values()))
        out.append((en, fontsrc.shape(plain, code), en, k["kind"]))
        for form, t in forms.items():
            if form != "other" and isinstance(v, dict):
                out.append((en + "#" + form, fontsrc.shape(t, code), en, k["kind"]))
    return out


def strings_entry(texts):
    rows = sorted((fnv(key), text) for key, text, _, _ in texts)
    for a, b in zip(rows, rows[1:]):
        if a[0] == b[0]:
            raise SystemExit("two texts hash the same (%08x): %r %r" % (a[0], a[1], b[1]))
    head = 4 + 8 * len(rows)
    body, table = bytearray(), bytearray(struct.pack("<I", len(rows)))
    for h, text in rows:
        table += struct.pack("<II", h, head + len(body))
        body += text.encode("utf-8") + b"\0"
    return bytes(table + body)


def capitals(text, code):
    """The capitals a heading drawn in capitals needs (pico_upper: Turkish i takes its dot)."""
    out = []
    for ch in text:
        if code == "tr" and ch == "i":
            out.append("İ")
        elif code == "tr" and ch == "ı":
            out.append("I")
        else:
            u = ch.upper()
            if len(u) == 1:
                out.append(u)
    return "".join(out)


def width_units(text, role, upper, face, s):
    size, weight = ROLE[role]
    px = max(1, round(size * s))
    w = 0
    for ch in (text.upper() if upper else text):
        g = fontsrc.glyph(ord(ch), weight, px, face)
        w += (g[5] / 64.0 if g else 0) + (2 * s if upper else 0)
    return w / s


def font_extra(pack, role, px, weight, codes, face):
    glyphs = []
    for c in codes:
        g = fontsrc.glyph(c, weight, px, face)
        if not g:
            raise SystemExit("no font has U+%04X (%s): run subset_fonts.py" % (c, chr(c)))
        glyphs.append(g)
    atlas = fontsrc.shelf_pack(glyphs)
    aw, ah = atlas.size
    blob = bytearray(struct.pack("<HHHHHHHH", px, 0, 0, len(glyphs), aw, ah, 0, 0))
    for c, xo, yo, w, h, adv, im, gx, gy in glyphs:
        blob += struct.pack("<IhhHHHHI", c, xo, yo, gx, gy, w, h, adv)
    blob += atlas.tobytes()
    pack.add("f_" + role, bp.T_FONT, aw, ah, bytes(blob))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("size")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    W, H = (int(v) for v in a.size.lower().split("x"))
    s = H / 720.0
    keys = load_keys()
    warnings = []
    have = set(bp.CHARS) | {c for c, _ in bp.name_codes()}
    for lang in fontsrc.languages()[1:]:
        code, face = lang["code"], lang["cjk"]
        if a.only and code not in a.only:
            continue
        if not os.path.exists(os.path.join(fontsrc.I18N, code + ".json")):
            warnings.append("%s: no language file, no pack" % code)
            continue
        texts = texts_of(code, keys, warnings.append)
        # what may not fit: measured on the Konkr's screen, the tightest the menus are drawn on
        for key, text, en, kind in texts:
            room = ROOM.get(kind)
            if room and W * 2 == H * 3:
                role, most, upper = room
                wu = width_units(text, role, upper, face, s)
                if wu > most:
                    warnings.append("%s: %r is %d units wide, room for %d (%s)" % (code, text, wu, most, en))
        codes = set()
        for _, text, _, _ in texts:
            codes |= {ord(c) for c in text + capitals(text, code) if ord(c) >= 0x20}
        codes = sorted(codes - have)
        pack = bp.Pack()
        pack.add("strings", T_STRINGS, 0, 0, strings_entry(texts))
        for role, size, weight in bp.FONTS:
            font_extra(pack, role, max(1, round(size * s)), weight, codes, face)
        out = os.path.join(a.out, "lang-%s-%dx%d.pack" % (code, W, H))
        raw, z = pack.write(out, (W, H))
        print("lang %s %dx%d: %d texts, %d extra glyphs, %d bytes (%d raw)" % (code, W, H, len(texts), len(codes), z, raw))
        if raw > 0x00600000 or z > 0x00200000:
            raise SystemExit("%s: the pack outgrows the LK's room for it (6 MB raw, 2 MB packed)" % code)
    for w in warnings:
        print("warning:", w, file=sys.stderr)


if __name__ == "__main__":
    main()
