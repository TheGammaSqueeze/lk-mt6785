"""
Where the menu fonts' glyphs come from, for the Current theme's packs (build_pack.py) and the
language packs (build_lang.py).

Inter draws Latin, Greek and Cyrillic. What it lacks comes from the Noto subsets in art/fonts
(subset_fonts.py makes them from the full Noto fonts, keeping only the characters the menus use):
Noto Sans CJK in the face of the language (Japanese, Korean, Simplified or Traditional Chinese) and
at Inter's weight, Noto Naskh Arabic UI and Noto Sans Hebrew in regular or bold.

Arabic and Persian are joined here (arabic_reshaper, to the presentation forms the fonts carry) and
their vowel marks dropped; the LK puts any text with right-to-left letters in drawing order when it
draws it (pico_bidi in emu/gba/pico_i18n.c), so texts stay in reading order in the packs.
"""
import json, os
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
FDIR = os.path.join(HERE, "art", "fonts")
I18N = os.path.join(HERE, "i18n")
WEIGHTS = ("Light", "Regular", "Medium", "SemiBold")
CJK_WGHT = {"Light": 300, "Regular": 400, "Medium": 500, "SemiBold": 600}
CJK_FACES = ("JP", "KR", "SC", "TC")


def languages():
    return json.load(open(os.path.join(I18N, "languages.json"), encoding="utf-8"))["languages"]


def is_mark(c):
    """Arabic and Hebrew vowel and cantillation marks: left out of menu text."""
    return 0x591 <= c <= 0x5C7 or 0x610 <= c <= 0x61A or 0x64B <= c <= 0x65F or c == 0x670 or 0x6D6 <= c <= 0x6ED


def shape(text, lang):
    """A text as it goes in a pack: Arabic and Persian letters joined, marks dropped."""
    text = "".join(ch for ch in text if not is_mark(ord(ch)))
    if lang in ("ar", "fa"):
        import arabic_reshaper
        text = arabic_reshaper.reshape(text)
    return text


def inter_path(weight):
    return os.path.join(FDIR, "Inter-%s.ttf" % weight)


def cjk_path(face, weight):
    return os.path.join(FDIR, "NotoSansCJK%s-%s.otf" % (face, weight))


def arabic_path(weight):
    return os.path.join(FDIR, "NotoNaskhArabicUI-%s.ttf" % ("Bold" if weight == "SemiBold" else "Regular"))


def hebrew_path(weight):
    return os.path.join(FDIR, "NotoSansHebrew-%s.ttf" % ("Bold" if weight == "SemiBold" else "Regular"))


_cmaps = {}


def cmap(path):
    if path not in _cmaps:
        _cmaps[path] = set(TTFont(path, fontNumber=0).getBestCmap()) if os.path.exists(path) else set()
    return _cmaps[path]


_fonts = {}


def pil_font(path, px):
    key = (path, px)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(path, px)
    return _fonts[key]


def source(code, weight, face):
    """The font file a code point is drawn from: Inter, then the CJK face, Arabic, Hebrew."""
    for path in (inter_path(weight), cjk_path(face or "JP", weight), arabic_path(weight), hebrew_path(weight)):
        if code in cmap(path):
            return path
    return None


def glyph(code, weight, px, face):
    """[code, xoff, yoff (from the pen at the baseline), w, h, advance in 1/64 px, image] or None."""
    path = source(code, weight, face)
    if not path:
        return None
    f = pil_font(path, px)
    ch = chr(code)
    bb = f.getbbox(ch, anchor="ls")
    w, h = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
    im = None
    if w and h:
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-bb[0], -bb[1]), ch, font=f, fill=255, anchor="ls")
    return [code, bb[0], bb[1], w, h, round(f.getlength(ch) * 64), im]


def shelf_pack(glyphs, aw=512):
    """Places each glyph (list as glyph() makes it) in a 512-wide atlas: appends x, y; returns the atlas."""
    x, y, shelf = 0, 0, 0
    for g in glyphs:
        if x + g[3] + 1 > aw:
            x, y, shelf = 0, y + shelf + 1, 0
        g.append(x)
        g.append(y)
        x += g[3] + 1
        shelf = max(shelf, g[4])
    atlas = Image.new("L", (aw, y + shelf + 1), 0)
    for g in glyphs:
        if g[6] is not None:
            atlas.paste(g[6], (g[7], g[8]))
    return atlas


def native_names():
    """Each language's name as the Language row draws it, with the CJK face it is drawn in."""
    return [(shape(l["name"], l["code"]), l["cjk"]) for l in languages()]
