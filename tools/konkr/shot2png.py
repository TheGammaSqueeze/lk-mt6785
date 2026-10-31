#!/usr/bin/env python3
"""Decode an `oem shot[:step]` fastboot dump (RGB565 hex, 13 px per INFO line, rows as
"<row>:" first chunk then "<row>+" continuations) into a PNG. Physical geometry is what the
LK dumps (640x960 portrait for the frame buffer); pass --turn to show it landscape (960x640)
the way the panel is mounted (ccw by default, --cw for the other way)."""
import re, sys
from PIL import Image

def decode(path):
    rows = {}
    w = h = None
    for line in open(path, errors='replace'):
        line = line.strip()
        m = re.match(r'\(bootloader\) shot: \S+ (\d+)x(\d+) pitch=\d+ step=(\d+) -> (\d+)x(\d+)', line)
        if m:
            w, h = int(m.group(4)), int(m.group(5)); continue
        m = re.match(r'\(bootloader\) (\d+)([:+])([0-9a-f]+)$', line)
        if not m: continue
        r = int(m.group(1)); hexs = m.group(3)
        px = [int(hexs[i:i+4], 16) for i in range(0, len(hexs) - len(hexs) % 4, 4)]
        rows.setdefault(r, []).extend(px)
    if w is None:
        w = max(len(v) for v in rows.values()); h = max(rows) + 1
    img = Image.new('RGB', (w, h))
    put = img.load()
    for r, px in rows.items():
        for x, v in enumerate(px[:w]):
            if r < h:
                put[x, r] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    return img

def decode_bin(path):
    """A `fastboot get_staged` dump from `oem shotstage`: "SHOT", w, h, kind (u32 LE), RGB565."""
    import struct
    b = open(path, 'rb').read()
    magic, w, h, kind = struct.unpack('<4sIII', b[:16])
    assert magic == b'SHOT', magic
    img = Image.frombytes('RGB', (w, h), b[16:16 + w * h * 2], 'raw', 'BGR;16')
    return img

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    img = decode_bin(src) if open(src, 'rb').read(4) == b'SHOT' else decode(src)
    if '--turn' in sys.argv:
        img = img.transpose(Image.ROTATE_270 if '--cw' in sys.argv else Image.ROTATE_90)
    img.save(dst)
    print(dst, img.size)
