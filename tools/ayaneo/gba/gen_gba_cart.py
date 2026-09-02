#!/usr/bin/env python3
"""
Generate the GBA cartridge placeholder art the menu draws in place of each game's
boxart. Based on a Game Boy Advance cartridge line drawing (Noun Project, "Sanchez
El Burrito"): the SVG outline is rasterised, its body/label/background regions are
separated by flood fill, then shaded (charcoal body with a top highlight, light
label plate) and the system name is printed on the label.

Usage: gen_gba_cart.py <out.png>  [svg]
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SVG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gba_cartridge.svg")
SS = 4                      # supersample factor for anti-aliasing
OUT_W = 114                 # half res: the placeholder cart does not need full resolution
                            # (it is upscaled on the card), so store it at half the window width

FONTS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
]


def font(sz):
    for f in FONTS:
        try:
            return ImageFont.truetype(f, sz)
        except OSError:
            pass
    return ImageFont.load_default()


def ctext(d, cx, y, s, fnt, fill):
    b = d.textbbox((0, 0), s, font=fnt)
    d.text((cx - (b[2] - b[0]) / 2 - b[0], y), s, font=fnt, fill=fill)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/gba_cart.png"
    svg = sys.argv[2] if len(sys.argv) > 2 else SVG
    try:
        import cairosvg
    except ImportError:
        sys.stderr.write("gen_gba_cart.py needs cairosvg to rasterise the cartridge "
                         "SVG:\n    pip install cairosvg\n")
        sys.exit(3)
    if not os.path.exists(svg):
        sys.stderr.write("cartridge SVG not found: %s\n" % svg)
        sys.exit(3)

    # rasterise the SVG large, drop the bottom attribution band
    RW = 912
    cairosvg.svg2png(url=svg, write_to="/tmp/_cart_svg.png", output_width=RW)
    raw = Image.open("/tmp/_cart_svg.png").convert("RGBA")
    W, H = raw.size
    raw = raw.crop((0, 0, W, int(H * 0.86)))
    W, H = raw.size
    rpx = raw.load()

    # line mask (dark, opaque) + tight cart bbox
    line = Image.new("1", (W, H), 0)
    lp = line.load()
    x0, y0, x1, y1 = W, H, 0, 0
    for y in range(H):
        for x in range(W):
            r, g, b, a = rpx[x, y]
            if a > 110 and r < 150:
                lp[x, y] = 1
                x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y)

    # crop to the cart with a small margin
    m = 10
    cx0, cy0 = max(0, x0 - m), max(0, y0 - m)
    cx1, cy1 = min(W, x1 + m), min(H, y1 + m)
    line = line.crop((cx0, cy0, cx1, cy1))
    W, H = line.size
    lp = line.load()

    # region map: 255 = open, 0 = line. flood fill background (100) then label (150).
    fmap = Image.new("L", (W, H), 255)
    fp = fmap.load()
    for y in range(H):
        for x in range(W):
            if lp[x, y]:
                fp[x, y] = 0
    for corner in [(0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)]:
        if fp[corner] == 255:
            ImageDraw.floodfill(fmap, corner, 100, thresh=0)
    # the label is the big central rectangle: flood from the image centre
    if fp[W // 2, H // 2] == 255:
        ImageDraw.floodfill(fmap, (W // 2, H // 2), 150, thresh=0)

    # label bounding box (region==150) for text placement
    lx0, ly0, lx1, ly1 = W, H, 0, 0
    for y in range(H):
        for x in range(W):
            if fp[x, y] == 150:
                lx0 = min(lx0, x); ly0 = min(ly0, y); lx1 = max(lx1, x); ly1 = max(ly1, y)

    # compose the shaded cart
    out_im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    op = out_im.load()
    for y in range(H):
        t = y / (H - 1)
        # body: charcoal, lighter at the top (a soft highlight), darker at the bottom
        br = int(84 - 44 * t); bg = int(86 - 44 * t); bb = int(100 - 48 * t)
        for x in range(W):
            v = fp[x, y]
            if v == 100:                       # background
                op[x, y] = (0, 0, 0, 0)
            elif v == 0:                       # outline
                op[x, y] = (26, 28, 38, 255)
            elif v == 150:                     # label plate (subtle vertical shade)
                lt = (y - ly0) / max(1, (ly1 - ly0))
                c = int(238 - 16 * lt)
                op[x, y] = (c, c, min(255, c + 6), 255)
            else:                              # body
                op[x, y] = (br, bg, bb, 255)

    # system name on the label
    d = ImageDraw.Draw(out_im)
    lcx = (lx0 + lx1) // 2
    lh = ly1 - ly0
    ctext(d, lcx, ly0 + int(lh * 0.20), "GBA", font(int(lh * 0.40)), (54, 62, 116, 255))
    ctext(d, lcx, ly0 + int(lh * 0.66), "GAME BOY ADVANCE",
          font(int(lh * 0.14)), (110, 116, 150, 255))

    # downscale to final size (anti-alias), preserving the cart aspect
    fw = OUT_W
    fh = max(1, round(OUT_W * H / W))
    out_im = out_im.resize((fw, fh), Image.LANCZOS)
    out_im.save(out)
    print("wrote", out, out_im.size, "(label bbox %dx%d)" % (lx1 - lx0, ly1 - ly0))


if __name__ == "__main__":
    main()
