#!/usr/bin/env python3
"""
Generate the GBA cartridge placeholder art that the SNES-style menu draws in
place of each game's boxart (the "game card icon"). Sized 228x160 to match the
SNES thumbnail window so it fills the card screen 1:1. Output is an RGBA PNG.

Usage: gen_gba_cart.py <out.png>
"""
import sys
from PIL import Image, ImageDraw

W, H = 228, 160


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/gba_cart.png"
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    # window background: soft vertical indigo gradient so the cart reads on any card
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=(26 + int(14 * t), 22 + int(10 * t),
                                       54 + int(24 * t), 255))

    # cartridge body (rounded), inset from the window
    bx0, by0, bx1, by1 = 34, 14, W - 34, H - 10
    body = (58, 60, 78, 255)
    edge = (120, 132, 176, 255)
    # body vertical gradient inside a rounded mask
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([bx0, by0, bx1, by1], radius=16, fill=255)
    grad = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gp = grad.load()
    for y in range(H):
        t = y / (H - 1)
        r = int(70 - 26 * t); g = int(72 - 26 * t); b = int(92 - 30 * t)
        for x in range(W):
            gp[x, y] = (r, g, b, 255)
    im.paste(grad, (0, 0), mask)
    d.rounded_rectangle([bx0, by0, bx1, by1], radius=16, outline=edge, width=3)
    # top bevel highlight
    d.arc([bx0 + 3, by0 + 3, bx1 - 3, by1 - 3], start=185, end=355,
          fill=(170, 182, 220, 200), width=2)

    # label plate
    lx0, ly0, lx1, ly1 = bx0 + 14, by0 + 12, bx1 - 14, int(H * 0.52)
    d.rounded_rectangle([lx0, ly0, lx1, ly1], radius=8,
                        fill=(232, 234, 244, 255), outline=(150, 156, 176, 255), width=2)
    # a stylised "GBA" mark on the label
    d.text(((lx0 + lx1) // 2 - 16, (ly0 + ly1) // 2 - 6), "GBA",
           fill=(70, 78, 120, 255))

    # notch top-right (cart shape cue)
    d.rectangle([bx1 - 30, by0, bx1 - 6, by0 + 12], fill=(46, 48, 64, 255))
    d.rounded_rectangle([bx1 - 28, by0 + 2, bx1 - 8, by0 + 11], radius=4,
                        fill=(66, 68, 88, 255))

    # metallic ridged connector along the bottom
    cy0 = int(H * 0.66)
    for i in range(9):
        cx = bx0 + 16 + i * ((bx1 - bx0 - 32) / 8.0)
        for yy in range(cy0, by1 - 6):
            tt = (yy - cy0) / max(1, (by1 - 6 - cy0))
            c = int(120 - 60 * tt)
            d.line([(cx - 5, yy), (cx + 5, yy)], fill=(c, c + 6, c + 22, 255))

    im.save(out)
    print("wrote", out, im.size)


if __name__ == "__main__":
    main()
