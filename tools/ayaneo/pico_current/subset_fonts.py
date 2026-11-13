#!/usr/bin/env python3
"""
Make the Noto subsets in art/fonts that the menus draw Japanese, Korean, Chinese, Arabic, Persian
and Hebrew from (fontsrc.py), keeping only the characters the language files and the languages'
names use. Run again after a translation changes; the pack builders stop at a missing glyph.

    subset_fonts.py [--noto DIR]

DIR holds the Noto fonts (SIL Open Font License 1.1), laid out as AOSP's external/noto-fonts:
notosanscjk/NotoSansCJK-VF.otf.ttc, notonaskharabicui/NotoNaskhArabicUI-{Regular,Bold}.ttf,
notosanshebrew/NotoSansHebrew-{Regular,Bold}.ttf. Noto Sans CJK is variable: each face is cut at
the weights of Inter the menu fonts use (Light 300, Regular 400, Medium 500, SemiBold 600). Needs
fontTools 4.40 or newer (cutting CFF2 variable fonts); the packs build from the subsets alone.
"""
import argparse, json, os
from fontTools.ttLib import TTCollection, TTFont
from fontTools.varLib import instancer
from fontTools import subset
import fontsrc

DEFAULT_NOTO = "/work/GammaOSNextDistribution-A14/external/noto-fonts"
FACE_INDEX = {"JP": 0, "KR": 1, "SC": 2, "TC": 3}   # the faces' order in NotoSansCJK-VF.otf.ttc


def language_text(code):
    """Every character a language's menus can show: its texts and their capitals."""
    path = os.path.join(fontsrc.I18N, code + ".json")
    if not os.path.exists(path):
        return ""
    out = []
    for v in json.load(open(path, encoding="utf-8")).values():
        for t in (v.values() if isinstance(v, dict) else [v]):
            t = fontsrc.shape(t, code)
            out.append(t + t.upper())
    return "".join(out)


def cut(font, chars, out):
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.glyph_names = False
    opts.hinting = False
    s = subset.Subsetter(opts)
    s.populate(unicodes=sorted({ord(c) for c in chars if ord(c) >= 0x20}))
    s.subset(font)
    font.save(out)
    print("%s: %d characters, %d bytes" % (os.path.basename(out), len(set(chars)), os.path.getsize(out)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--noto", default=DEFAULT_NOTO)
    a = ap.parse_args()
    langs = fontsrc.languages()
    names = "".join(fontsrc.shape(l["name"], l["code"]) for l in langs)
    # CJK: per face, the language written in it and every language's name (a name's own face draws
    # it in the base pack, but each face keeps all CJK names for the language packs too)
    ttc = os.path.join(a.noto, "notosanscjk", "NotoSansCJK-VF.otf.ttc")
    for face, idx in FACE_INDEX.items():
        chars = names + "".join(language_text(l["code"]) for l in langs if l["cjk"] == face)
        chars = "".join(c for c in chars if ord(c) >= 0x2E80)   # Inter draws the rest
        for weight in fontsrc.WEIGHTS:
            font = TTCollection(ttc).fonts[idx]
            font = instancer.instantiateVariableFont(font, {"wght": fontsrc.CJK_WGHT[weight]})
            cut(font, chars, fontsrc.cjk_path(face, weight))
    arabic = names + language_text("ar") + language_text("fa")
    hebrew = names + language_text("he")
    for weight in ("Regular", "Bold"):
        cut(TTFont(os.path.join(a.noto, "notonaskharabicui", "NotoNaskhArabicUI-%s.ttf" % weight)),
            "".join(c for c in arabic if 0x600 <= ord(c) <= 0x6FF or ord(c) >= 0xFB50),
            os.path.join(fontsrc.FDIR, "NotoNaskhArabicUI-%s.ttf" % weight))
        cut(TTFont(os.path.join(a.noto, "notosanshebrew", "NotoSansHebrew-%s.ttf" % weight)),
            "".join(c for c in hebrew if 0x590 <= ord(c) <= 0x5FF),
            os.path.join(fontsrc.FDIR, "NotoSansHebrew-%s.ttf" % weight))
    with open(os.path.join(fontsrc.FDIR, "OFL-Noto.txt"), "w", encoding="utf-8") as out:   # their licences
        for d in ("notosanscjk", "notonaskharabicui", "notosanshebrew"):
            out.write("==== %s ====\n" % d)
            out.write(open(os.path.join(a.noto, d, "NOTICE"), encoding="utf-8").read().rstrip() + "\n\n")


if __name__ == "__main__":
    main()
