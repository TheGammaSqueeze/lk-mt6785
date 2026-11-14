#!/usr/bin/env python3
"""
Make the Noto subsets in art/fonts the names glyph pack is drawn from (names.py, build_names.py):
Noto Sans CJK JP, KR and SC Regular, Noto Sans Hebrew and Noto Naskh Arabic UI, each cut to the
characters names.py gives it. Run once, or after names.py changes; build_names.py needs only the
subsets.

    subset_names.py [--noto DIR]

DIR holds the Noto fonts (SIL Open Font License 1.1) laid out as AOSP's external/noto-fonts:
notosanscjk/NotoSansCJK-Regular.ttc, notosanshebrew/NotoSansHebrew-Regular.ttf and
notonaskharabicui/NotoNaskhArabicUI-Regular.ttf.
"""
import argparse, os
from fontTools.ttLib import TTCollection, TTFont
import names
from subset_fonts import cut


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--noto", default=names.NOTO_CJK)
    a = ap.parse_args()
    by = {}
    for c, src in names.wanted():
        by.setdefault(src, []).append(chr(c))
    # what Inter lacks of its ranges is drawn from the JP face
    inter = names.fontsrc.cmap(names.fontsrc.inter_path("Regular"))
    by["JP"] = by.get("JP", []) + [ch for ch in by.get("inter", []) if ord(ch) not in inter]
    ttc = os.path.join(a.noto, "notosanscjk", "NotoSansCJK-Regular.ttc")
    for face, idx in names.FACE_INDEX.items():
        cut(TTCollection(ttc).fonts[idx], "".join(by.get(face, [])), names.names_path(face))
    cut(TTFont(os.path.join(a.noto, "notosanshebrew", "NotoSansHebrew-Regular.ttf")), "".join(by["he"]),
        names.names_path("he"))
    cut(TTFont(os.path.join(a.noto, "notonaskharabicui", "NotoNaskhArabicUI-Regular.ttf")), "".join(by["ar"]),
        names.names_path("ar"))


if __name__ == "__main__":
    main()
