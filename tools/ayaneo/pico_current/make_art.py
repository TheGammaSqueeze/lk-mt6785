#!/usr/bin/env python3
"""
Prepare the art of the Current menu theme (tools/ayaneo/pico_current/art/), once.

Run by hand when the art changes; build_pack.py turns the committed result into the card's
packs and needs only Pillow. This step also needs NumPy and CairoSVG.

    make_art.py <design dir> <photos dir>
    make_art.py --icons

<design dir> is the Current design package (fonts, platform logos, the GBA and GBC photos);
<photos dir> holds the other hardware photos, with transparent backgrounds, as
gb.png snes.png genesis.png mastersystem.png gamegear.png sg1000.png, and the SG-1000 mark
as sg1000.svg (see CREDITS.md for where each comes from).

Writes:
  art/hardware/<system>.png   the console photo, cropped to its pixels, at most 720 px wide
  art/logos/<system>.png      the platform mark in white, cropped, 1100 px wide
  art/icons/<name>.png        the rail, settings, in-game menu and button icons, white strokes, 96 px
  art/fonts/                  Inter and its licence
"""
import io, os, shutil, sys
import numpy as np
from PIL import Image
import cairosvg

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
SYSTEMS = ["gb", "gbc", "gba", "snes", "genesis", "sms", "gg", "sg"]
DESIGN_LOGO = {"gb": "gb", "gbc": "gbc", "gba": "gba", "snes": "snes", "genesis": "genesis",
               "sms": "mastersystem", "gg": "gamegear"}
PHOTO = {"gb": "gb.png", "snes": "snes.png", "genesis": "genesis.png", "sms": "mastersystem.png",
         "gg": "gamegear.png", "sg": "sg1000.png"}


def out(*p):
    path = os.path.join(ART, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def svg_png(svg, width=None, height=None):
    data = cairosvg.svg2png(bytestring=svg.encode(), output_width=width, output_height=height)
    return Image.open(io.BytesIO(data)).convert("RGBA")


# ---- hardware photos -------------------------------------------------------------------------
def hardware(design, photos):
    for s in SYSTEMS:
        src = os.path.join(design, "assets", "hardware", s + ".png") if s in ("gba", "gbc") \
            else os.path.join(photos, PHOTO[s])
        im = Image.open(src).convert("RGBA")
        im = im.crop(im.getchannel("A").getbbox())
        if im.width > 720:
            im = im.resize((720, round(im.height * 720 / im.width)), Image.LANCZOS)
        im.save(out("hardware", s + ".png"), optimize=True)
        print("hardware", s, im.size)


# ---- platform marks --------------------------------------------------------------------------
def mark_from_svg(svg_text, knockout_dark=False):
    """White silhouette of a mark. knockout_dark cuts dark lettering out of a light plate
    (the Genesis mark), which a plain silhouette would fill in."""
    im = svg_png(svg_text, width=1100)
    a = np.asarray(im).astype(np.float32) / 255.0
    alpha = a[:, :, 3]
    if knockout_dark:
        lum = 0.299 * a[:, :, 0] + 0.587 * a[:, :, 1] + 0.114 * a[:, :, 2]
        alpha = alpha * np.clip((lum - 0.30) / 0.30, 0.0, 1.0)
    al = Image.fromarray((alpha * 255 + 0.5).astype(np.uint8))
    al = al.crop(al.getbbox())
    res = Image.new("RGBA", al.size, (244, 248, 255, 255))
    res.putalpha(al)
    return res


def logos(design, photos):
    for s in SYSTEMS:
        if s == "sg":
            svg = open(os.path.join(photos, "sg1000.svg"), encoding="utf-8").read()
            im = mark_from_svg(svg)
        elif s == "genesis":
            svg = open(os.path.join(design, "assets", "logos", "original", "genesis.svg"), encoding="utf-8").read()
            im = mark_from_svg(svg, knockout_dark=True)
        else:   # the package's white marks
            im = Image.open(os.path.join(design, "assets", "logos", DESIGN_LOGO[s] + "-ice.png")).convert("RGBA")
            im = im.crop(im.getchannel("A").getbbox())
        im.save(out("logos", s + ".png"), optimize=True)
        print("logo", s, im.size)


# ---- icons (24-unit stroke drawings) ---------------------------------------------------------
ICONS = {
    "games": "M5 10.7 Q5.5 7 9 7 H15 Q18.5 7 19.2 10.5 L21 16 Q21.4 19.6 18.7 19 Q17.5 18.8 15.2 15.7 "
             "H8.8 Q6.5 19 5 19 Q2.7 19 3.2 16 Z M6.8 11.6 H10.8 M8.8 9.6 V13.6 M15.9 10.6 h.1 M17.7 12.1 h.1",
    "resume": "M20.2 12 A8.2 8.2 0 1 1 3.8 12 A8.2 8.2 0 1 1 20.2 12 M12 6.6 V12 L15.7 13.9",
    "settings": "M3 7 H9 M13 7 H21 M3 16 H13 M17 16 H21 M11 4.5 V9.5 M15 13.5 V18.5",
    "library": "M3.5 5.5 V19 M8 4 V19 M13 5.5 V19 M16 5 L20.8 18.4",
    "play": "M8.4 4.6 L19.7 12 L8.4 19.4Z",
    "sun": "M15.5 12 A3.5 3.5 0 1 1 8.5 12 A3.5 3.5 0 1 1 15.5 12 M12 2 V5 M12 19 V22 M2 12 H5 M19 12 H22 "
           "M4.8 4.8 L7 7 M17 17 L19.2 19.2 M19.2 4.8 L17 7 M7 17 L4.8 19.2",
    "sound": "M3 10 H7 L12 5.5 V18.5 L7 14 H3Z M15.5 8.7 Q19 12 15.5 15.3 M18 6.3 Q24 12 18 17.7",
    "display": "M3 4.5 H21 V16.5 H3 Z M8 20 H16 M12 17 V20",
    "power": "M12 2.5 V11 M7.3 5.5 A8 8 0 1 0 16.7 5.5",
    "mute": "M3 10 H7 L12 5.5 V18.5 L7 14 H3Z M16 9.5 L21 14.5 M21 9.5 L16 14.5",
    "bfi": "M4 5 H20 V19 H4 Z M4 9.7 H20 M4 14.3 H20 M7 7.3 h.1 M7 16.7 h.1",
    "colour": "M12 3 A9 9 0 1 0 12 21 C13.6 21 14.1 19.9 13.4 18.8 C12.7 17.6 13.5 16 15.1 16 H17 "
              "A4 4 0 0 0 21 12 C21 6.9 17 3 12 3 Z M7.6 11.2 h.1 M9.9 7.4 h.1 M14.4 7.3 h.1 M17.1 10.4 h.1",
    # the in-game menu
    "save": "M12 3.5 V14.5 M7.5 10 L12 14.5 L16.5 10 M4 15.5 V18.8 Q4 20.5 5.7 20.5 H18.3 Q20 20.5 20 18.8 V15.5",
    "load": "M12 14.5 V3.5 M7.5 8 L12 3.5 L16.5 8 M4 15.5 V18.8 Q4 20.5 5.7 20.5 H18.3 Q20 20.5 20 18.8 V15.5",
    "speed": "M4.3 17.6 A8.6 8.6 0 1 1 19.7 17.6 M12 14 L15.9 9.3 M12 14 h.1",
    "system": "M7 7 H17 V17 H7 Z M10 10 H14 V14 H10 Z M9.6 3.6 V7 M14.4 3.6 V7 M9.6 17 V20.4 M14.4 17 V20.4 "
              "M3.6 9.6 H7 M3.6 14.4 H7 M17 9.6 H20.4 M17 14.4 H20.4",
    "reset": "M19.4 12.6 A7.5 7.5 0 1 1 17 6.4 M18.4 3.2 V7.2 H14.4",
    "exit": "M13.5 4 H18.4 Q20 4 20 5.6 V18.4 Q20 20 18.4 20 H13.5 M9.5 8 L5.5 12 L9.5 16 M5.5 12 H15",
    "chev": "M9.5 6 L15.5 12 L9.5 18",
    "back": "M14.5 6 L8.5 12 L14.5 18",
}
GAMMA = ('<g fill="none" stroke="#fff" stroke-linejoin="round" stroke-width="11">'
         '<path d="M4 70 L119 4 L136 14 L136 32 L39 88 L39 178 L113 220 L185 179 L185 121 L122 157 '
         'L94 140 L94 119 L198 58 L225 72 L225 200 L117 262 L4 197 Z"/>'
         '<path d="M4 70 L23 81 L23 187 L116 241 L206 189 L206 83 L225 72 M23 81 L136 14 M94 119 '
         'L122 135 L206 83 M122 135 V157"/></g>')


def icons():
    for name, d in ICONS.items():
        fill = "#fff" if name == "play" else "none"
        svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 24 24">'
               '<path d="%s" fill="%s" stroke="#fff" stroke-width="1.5" stroke-linecap="round" '
               'stroke-linejoin="round"/></svg>' % (d, fill))
        svg_png(svg).save(out("icons", name + ".png"), optimize=True)
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="-8 -8 245 280">%s</svg>'
           % GAMMA)
    svg_png(svg).save(out("icons", "gamma.png"), optimize=True)
    print("icons", len(ICONS) + 1)


def fonts(design):
    for n in ("Light", "Regular", "Medium", "SemiBold"):
        shutil.copy(os.path.join(design, "assets", "fonts", "Inter-%s.ttf" % n), out("fonts", "Inter-%s.ttf" % n))
    shutil.copy(os.path.join(design, "assets", "licenses", "Inter-OFL.txt"), out("fonts", "OFL.txt"))
    shutil.copy(os.path.join(design, "assets", "licenses", "Slate-LICENSE"), out("logos", "LICENSE-Slate"))
    shutil.copy(os.path.join(design, "assets", "licenses", "Slate-CREDITS.md"), out("logos", "CREDITS-Slate.md"))


def main():
    if sys.argv[1:] == ["--icons"]:   # the icons alone need neither source directory
        icons()
        return
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    design, photos = sys.argv[1], sys.argv[2]
    hardware(design, photos)
    logos(design, photos)
    icons()
    fonts(design)


if __name__ == "__main__":
    main()
