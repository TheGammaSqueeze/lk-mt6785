#!/usr/bin/env python3
"""
Generate the GBA cartridge placeholder art that the SNES-style menu draws in
place of each game's boxart (the "game card icon"). Sized 228x160 to match the
SNES thumbnail window so it fills the card screen 1:1. Output is an RGBA PNG.

Usage: gen_gba_cart.py <out.png>
"""
import sys
from PIL import Image, ImageDraw, ImageFont

W, H = 228, 160

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
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    # window backdrop: soft indigo so the cart reads on any card background
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=(22 + int(12 * t), 20 + int(9 * t),
                                       46 + int(22 * t), 255))

    # cartridge silhouette: real GBA carts have a wide top that steps in at the
    # shoulders to a slightly narrower body. Draw as a rounded body with two top
    # shoulder cuts.
    bx0, by0, bx1, by1 = 30, 12, W - 30, H - 8
    sh = 16                       # shoulder inset depth
    mask = Image.new("L", (W, H), 0)
    md = ImageDraw.Draw(mask)
    md.rounded_rectangle([bx0, by0 + sh, bx1, by1], radius=14, fill=255)
    md.rounded_rectangle([bx0 + 12, by0, bx1 - 12, by0 + sh + 14], radius=10, fill=255)

    # body vertical gradient (charcoal, lighter at top) painted through the mask
    grad = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gp = grad.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(74 - 34 * t); g = int(76 - 34 * t); b = int(86 - 36 * t)
        for x in range(W):
            gp[x, y] = (r, g, b, 255)
    im.paste(grad, (0, 0), mask)

    # outline + top bevel highlight (trace the mask edge subtly)
    edge = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ed = ImageDraw.Draw(edge)
    ed.rounded_rectangle([bx0, by0 + sh, bx1, by1], radius=14,
                         outline=(150, 158, 190, 255), width=3)
    ed.rounded_rectangle([bx0 + 12, by0, bx1 - 12, by0 + sh + 14], radius=10,
                         outline=(150, 158, 190, 255), width=3)
    im.alpha_composite(edge)

    # label plate with the GBA wordmark
    lx0, ly0, lx1, ly1 = bx0 + 16, by0 + sh + 8, bx1 - 16, int(H * 0.60)
    d.rounded_rectangle([lx0, ly0, lx1, ly1], radius=8,
                        fill=(236, 238, 246, 255), outline=(150, 156, 176, 255), width=2)
    cx = (lx0 + lx1) // 2
    ctext(d, cx, ly0 + 6, "GBA", font(30), (58, 66, 120, 255))
    ctext(d, cx, ly1 - 18, "GAME BOY ADVANCE", font(9), (110, 116, 150, 255))

    # thumb notch (bottom-centre grip cut of a GBA cart)
    d.rounded_rectangle([cx - 18, by1 - 12, cx + 18, by1 - 2], radius=4,
                        fill=(40, 42, 54, 255))

    # metallic connector ridges just under the label
    cy0 = ly1 + 8
    for i in range(9):
        rx = lx0 + 6 + i * ((lx1 - lx0 - 12) / 8.0)
        for yy in range(cy0, by1 - 16):
            tt = (yy - cy0) / max(1, (by1 - 16 - cy0))
            c = int(120 - 60 * tt)
            d.line([(rx - 4, yy), (rx + 4, yy)], fill=(c, c + 6, c + 22, 255))

    im.save(out)
    print("wrote", out, im.size)


if __name__ == "__main__":
    main()
