#!/usr/bin/env python3
"""
Encode opaque RGB frames (e.g. ffmpeg-extracted MP4 frames) into a GBA1 blob,
with a baked fade-out to black on the tail.

Usage:
  encode_mp4.py --src '/tmp/gbamp4/f_*.png' -o logo_anim_mp4.bin [--fps 60] [--fade 36]
"""
import glob, zlib, struct, argparse
import numpy as np
from PIL import Image


def to565(a):
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype('<u2').tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True)
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--fade', type=int, default=36)   # ~0.6s fade at 60fps
    ap.add_argument('--fade-to', choices=['black', 'white'], default='white')
    ap.add_argument('--limit', type=int, default=0, help='cap to first N frames')
    args = ap.parse_args()

    fadeval = 255.0 if args.fade_to == 'white' else 0.0

    files = sorted(glob.glob(args.src))
    if not files:
        raise SystemExit("no frames match %s" % args.src)
    if args.limit:
        files = files[:args.limit]

    frames = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files]
    n = len(frames)
    h, w = frames[0].shape[0], frames[0].shape[1]

    fade = min(args.fade, n)
    for i in range(fade):
        idx = n - fade + i
        t = (i + 1) / float(fade)          # 0 -> 1, fade to the chosen colour
        frames[idx] = frames[idx] * (1.0 - t) + fadeval * t

    blob = bytearray(struct.pack('<4sIHHHH', b'GBA1', 1, w, h, n, args.fps))
    mx = 0
    for fr in frames:
        px = np.clip(fr, 0, 255).astype(np.uint16)
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        c = co.compress(to565(px)) + co.flush()
        blob += struct.pack('<I', len(c)) + c
        mx = max(mx, len(c))

    open(args.out, 'wb').write(blob)
    print("%dx%d frames=%d fps=%d blob=%.2f MB maxframe=%.0f KB -> %s"
          % (w, h, n, args.fps, len(blob) / 1e6, mx / 1024, args.out))


if __name__ == '__main__':
    main()
