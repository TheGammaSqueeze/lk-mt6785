#!/usr/bin/env python3
"""Rebuild dev/logo/block/ from the Block's stock logo.bin.

The LK draws its boot and charging pictures from the logo partition by index, and GammaOS leaves
that partition as the stock firmware wrote it. The BMP set here only feeds the build's own
logo.bin (never flashed); it is made from the stock images in the stock order so that both agree
image for image.

  tools/block/logo_from_stock.py <stock logo.bin> [out dir, default dev/logo/block]
"""
import os
import struct
import sys
import zlib

W, H = 960, 720

# (name, width, height) in the order of the stock container: the MediaTek list for a board with
# fast charging pictures (60 images). The small sizes were read off the pictures themselves.
FULL = (W, H)
NAMES = ([("uboot",) + FULL, ("battery",) + FULL, ("low_battery",) + FULL, ("charger_ov",) + FULL]
         + [("num_%d" % i, 21, 36) for i in range(10)]
         + [("num_percent", 27, 36)]
         + [("bat_animation_%02d" % i, 126, 22) for i in range(1, 11)]
         + [("bat_10_%02d" % i,) + FULL for i in range(1, 11)]
         + [("bat_bg",) + FULL, ("bat_img", 126, 1), ("bat_100",) + FULL, ("kernel",) + FULL,
            ("low_battery01",) + FULL, ("low_battery02",) + FULL, ("low_battery_remind",) + FULL,
            ("fast_charging_100",) + FULL]
         + [("fast_charging_ani-%02d" % i,) + FULL for i in range(1, 7)]
         + [("fast_charging_%02d" % i, 60, 90) for i in range(10)]
         + [("fast_charging_percent", 60, 90)])


def images(blob):
    """The decompressed pictures of an MTK logo container (512-byte header, then a table)."""
    body = blob[512:]
    count, total = struct.unpack("<II", body[:8])
    offs = struct.unpack("<%dI" % count, body[8:8 + 4 * count])
    for i, o in enumerate(offs):
        end = offs[i + 1] if i + 1 < count else total
        yield zlib.decompress(body[o:end])


def write_bmp(path, w, h, bgra):
    """A bottom-up 24-bit BMP, the format bmp_to_raw takes."""
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):
        line = bytearray()
        for x in range(w):
            p = (y * w + x) * 4
            line += bgra[p:p + 3]
        line += b"\0" * (stride - w * 3)
        rows.append(bytes(line))
    pix = b"".join(rows)
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(pix), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(pix), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(hdr + dib + pix)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "dev", "logo", "block")
    pics = list(images(open(sys.argv[1], "rb").read()))
    if len(pics) != len(NAMES):
        sys.exit("expected %d pictures, the container has %d" % (len(NAMES), len(pics)))
    os.makedirs(out, exist_ok=True)
    for (name, w, h), raw in zip(NAMES, pics):
        if len(raw) != w * h * 4:
            sys.exit("%s: %d bytes, expected %dx%d" % (name, len(raw), w, h))
        write_bmp(os.path.join(out, "block_%s.bmp" % name), w, h, raw)
    print("wrote %d pictures to %s" % (len(pics), out))


if __name__ == "__main__":
    main()
