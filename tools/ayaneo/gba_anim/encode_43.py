#!/usr/bin/env python3
"""
Encode the 4:3 RGBA render into a GBA1 animation blob, compositing the alpha
frames over a solid background (white or black) and baking a fade-out to that
same colour on the tail.

Usage:
  encode_43.py --bg white -o logo_anim_white.bin
  encode_43.py --bg black -o logo_anim_black.bin
"""
import glob, zlib, struct, argparse
import numpy as np
from PIL import Image

SRC = '/tmp/gba43/f_*.png'
DROP_TAIL = 40        # last 40 frames
FADE_FRAMES = 36      # ~0.6s fade-out at 60fps
FPS = 60


def to565(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype('<u2').tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bg', choices=['white', 'black'], required=True)
    ap.add_argument('-o', '--out', required=True)
    args = ap.parse_args()

    bgval = 255.0 if args.bg == 'white' else 0.0

    files = sorted(glob.glob(SRC))
    if DROP_TAIL:
        files = files[:-DROP_TAIL]
    if not files:
        raise SystemExit("no frames in %s" % SRC)

    frames = []
    for f in files:
        im = np.asarray(Image.open(f).convert('RGBA'), dtype=np.float32)
        rgb = im[..., :3]
        a = im[..., 3:4] / 255.0
        comp = rgb * a + bgval * (1.0 - a)     # composite over the bg colour
        frames.append(comp)

    n = len(frames)
    h, w = frames[0].shape[0], frames[0].shape[1]

    # bake the fade-out to the bg colour over the last FADE_FRAMES frames
    for i in range(min(FADE_FRAMES, n)):
        idx = n - min(FADE_FRAMES, n) + i
        t = (i + 1) / float(min(FADE_FRAMES, n))   # 0 -> 1
        frames[idx] = frames[idx] * (1.0 - t) + bgval * t

    blob = bytearray(struct.pack('<4sIHHHH', b'GBA1', 1, w, h, n, FPS))
    mx = 0
    for fr in frames:
        px = np.clip(fr, 0, 255).astype(np.uint16)
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        c = co.compress(to565(px)) + co.flush()
        blob += struct.pack('<I', len(c)) + c
        mx = max(mx, len(c))

    open(args.out, 'wb').write(blob)
    print("bg=%s %dx%d frames=%d fps=%d blob=%.2f MB maxframe=%.0f KB -> %s"
          % (args.bg, w, h, n, FPS, len(blob) / 1e6, mx / 1024, args.out))


if __name__ == '__main__':
    main()
