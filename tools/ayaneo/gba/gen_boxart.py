#!/usr/bin/env python3
"""
Convert GBA box-front PNGs into compact .ART tiles the LK menu loads from the SD
card, one per ROM. The menu draws these in the card boxart window in place of the
generated placeholder cartridge.

.ART format (little-endian):
  off 0  : magic  "GART"  (4 bytes, 0x47 0x41 0x52 0x54)
  off 4  : u16    version (1)
  off 6  : u16    format  (0 = RGB565 LE)
  off 8  : u16    width
  off 10 : u16    height
  off 12 : width*height * u16  RGB565 pixels, row-major top-to-bottom

Each source image is scaled aspect-preserving so its LARGER side is at most
--max-dim (default 224, the boxart window), then encoded RGB565. The output file
is named by the source stem (the No-Intro title), so a ROM named identically on
the SD card matches its art by basename: /roms/gba/boxart/<romstem>.ART.

Usage:
  gen_boxart.py --zip /work/gbaart/109869101_ABeezy1388NintendoGameBoyAdvanceBoxArt.zip \
                --outdir out/boxart [--max-dim 224] [--limit N]
  gen_boxart.py --dir <folder-of-pngs> --outdir out/boxart

Push a tile to the device SD with the existing fastboot path:
  fastboot -s <serial> oem sd-put:/roms/gba/boxart/<romstem>.ART < out/boxart/<stem>.ART
"""
import sys, os, io, struct, zipfile, argparse
from PIL import Image

MAGIC = b"GART"


def to_565(im):
    im = im.convert("RGB")
    w, h = im.size
    px = im.load()
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = (v >> 8) & 0xFF
            i += 2
    return w, h, bytes(out)


def convert_one(im, max_dim):
    w, h = im.size
    if max(w, h) > max_dim:
        if w >= h:
            nw, nh = max_dim, max(1, round(h * max_dim / w))
        else:
            nw, nh = max(1, round(w * max_dim / h)), max_dim
        im = im.resize((nw, nh), Image.LANCZOS)
    w, h, data = to_565(im)
    return struct.pack("<4sHHHH", MAGIC, 1, 0, w, h) + data


def iter_pngs_zip(zpath):
    with zipfile.ZipFile(zpath) as z:
        for n in z.namelist():
            if n.lower().endswith(".png") and "/Box - Front/" in n:
                yield os.path.splitext(os.path.basename(n))[0], z.read(n)


def iter_pngs_dir(d):
    for fn in sorted(os.listdir(d)):
        if fn.lower().endswith(".png"):
            with open(os.path.join(d, fn), "rb") as f:
                yield os.path.splitext(fn)[0], f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip")
    ap.add_argument("--dir")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--max-dim", type=int, default=224)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    if not a.zip and not a.dir:
        ap.error("need --zip or --dir")
    os.makedirs(a.outdir, exist_ok=True)
    src = iter_pngs_zip(a.zip) if a.zip else iter_pngs_dir(a.dir)
    n = 0
    for stem, raw in src:
        try:
            im = Image.open(io.BytesIO(raw))
            blob = convert_one(im, a.max_dim)
        except Exception as e:
            print("skip", stem, e, file=sys.stderr)
            continue
        with open(os.path.join(a.outdir, stem + ".ART"), "wb") as f:
            f.write(blob)
        n += 1
        if n % 50 == 0:
            print("...", n)
        if a.limit and n >= a.limit:
            break
    print("wrote", n, "tiles to", a.outdir)


if __name__ == "__main__":
    main()
