"""
The characters game names can hold beyond the menu fonts' own (ASCII and Latin-1), for the names
glyph pack (build_names.py) and the font subsets it is drawn from (subset_names.py), and which font
draws each.

Latin, Greek, Cyrillic, punctuation and symbols come from Inter, as every menu text does; what
Inter lacks of the symbols from Noto Sans CJK JP. Kana and the CJK punctuation and full width forms
come from Noto Sans CJK JP, the common Hangul syllables (KS X 1001) from its KR face, and the Han
characters in common use from its SC face: GB2312, Big5's frequent characters and JIS X 0208's
kanji together. Hebrew comes from Noto Sans Hebrew, and Arabic, as the joined forms the LK puts
together when it draws a name, from Noto Naskh Arabic UI.
"""
import os, unicodedata
import fontsrc

# what Inter draws (and Noto Sans CJK JP where Inter has not got it), past Latin-1
INTER_RANGES = [
    (0x0100, 0x024F),   # Latin Extended-A and -B
    (0x0250, 0x02FF),   # IPA, spacing modifier letters
    (0x0300, 0x036F),   # combining marks (names written decomposed)
    (0x0370, 0x03FF), (0x1F00, 0x1FFF),   # Greek
    (0x0400, 0x052F),   # Cyrillic
    (0x1E00, 0x1EFF),   # Latin Extended Additional (Vietnamese)
    (0x2000, 0x206F), (0x20A0, 0x20CF), (0x2100, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF),
    (0x2460, 0x24FF), (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF),
]
JP_RANGES = [
    (0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF), (0x31F0, 0x31FF),
    (0x3200, 0x32FF), (0x3300, 0x33FF), (0xFF00, 0xFFEF),
]
HEBREW_RANGES = [(0x05D0, 0x05EA), (0x05EF, 0x05F4)]
ARABIC_PLAIN = [0x060C, 0x061B, 0x061F] + list(range(0x0660, 0x066A)) + list(range(0x06F0, 0x06FA))

NOTO_CJK = "/work/GammaOSNextDistribution-A14/external/noto-fonts"
FACE_INDEX = {"JP": 0, "KR": 1, "SC": 2}   # in NotoSansCJK-Regular.ttc


def names_path(which):
    """The subsets in art/fonts the pack is drawn from (subset_names.py writes them)."""
    return os.path.join(fontsrc.FDIR, {"JP": "NamesCJKJP-Regular.otf", "KR": "NamesCJKKR-Regular.otf",
                                       "SC": "NamesCJKSC-Regular.otf", "he": "NamesHebrew-Regular.ttf",
                                       "ar": "NamesArabic-Regular.ttf"}[which])


def _codes(codec, leads, trails=range(0xA1, 0xFF)):
    out = set()
    for b1 in leads:
        for b2 in trails:
            try:
                s = bytes([b1, b2]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.add(ord(s))
    return out


def han():
    """GB2312, Big5's frequent characters (its first level) and JIS X 0208's kanji."""
    big5 = _codes("big5", range(0xA4, 0xC7), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
    s = _codes("gb2312", range(0xB0, 0xF8)) | _codes("euc_jp", range(0xB0, 0xF5)) | big5
    return sorted(c for c in s if 0x4E00 <= c <= 0x9FFF)


def hangul():
    """KS X 1001's syllables, and the compatibility jamo."""
    s = {c for c in _codes("euc_kr", range(0xB0, 0xC9)) if 0xAC00 <= c <= 0xD7A3}
    return sorted(s | set(range(0x3131, 0x318F)))


def arabic_forms():
    """The joined forms of the Arabic letters, read from Unicode's own decompositions of the
    presentation forms: {letter: {kind: form}} and {(lam, alef): {kind: ligature}}, kinds being
    isolated, final, initial and medial."""
    forms, ligs = {}, {}
    for cp in list(range(0xFB50, 0xFE00)) + list(range(0xFE70, 0xFF00)):
        d = unicodedata.decomposition(chr(cp)).split()
        if len(d) < 2 or not d[0].startswith("<"):
            continue
        kind = d[0].strip("<>")
        if kind not in ("isolated", "final", "initial", "medial"):
            continue
        base = [int(x, 16) for x in d[1:]]
        if len(base) == 1 and 0x0621 <= base[0] <= 0x06FF:
            forms.setdefault(base[0], {}).setdefault(kind, cp)
        elif len(base) == 2 and base[0] == 0x0644 and base[1] in (0x0622, 0x0623, 0x0625, 0x0627):
            ligs.setdefault(tuple(base), {}).setdefault(kind, cp)
    return forms, ligs


def wanted():
    """[(code, source)], source one of "inter", "JP", "KR", "SC", "he", "ar" (before checking the
    fonts have them)."""
    out = []
    for lo, hi in INTER_RANGES:
        out += [(c, "inter") for c in range(lo, hi + 1) if unicodedata.category(chr(c)) not in ("Cn", "Co", "Cs")]
    for lo, hi in JP_RANGES:
        out += [(c, "JP") for c in range(lo, hi + 1) if unicodedata.category(chr(c)) not in ("Cn", "Co", "Cs")]
    out += [(c, "SC") for c in han()]
    out += [(c, "KR") for c in hangul()]
    for lo, hi in HEBREW_RANGES:
        out += [(c, "he") for c in range(lo, hi + 1)]
    forms, ligs = arabic_forms()
    arabic = set(ARABIC_PLAIN)
    for f in list(forms.values()) + list(ligs.values()):
        arabic |= set(f.values())
    out += [(c, "ar") for c in sorted(arabic)]
    return out
