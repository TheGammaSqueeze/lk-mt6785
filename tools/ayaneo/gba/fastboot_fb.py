#!/usr/bin/env python3
# Reconstruct a PNG from a framebuffer uploaded by `fastboot get_staged` (the
# on-device SNES menu debug channel, emu/gba/menu_fastboot.c cmd_upload).
#
#   fastboot -s 0123456789ABCDEF get_staged fb.bin
#   python3 tools/ayaneo/gba/fastboot_fb.py fb.bin out.png
#
# Layout: 16-byte header [magic 'AYFB', W, H, pitch] (u32 LE) then pitch*H BGRA
# (0xAARRGGBB) pixels. We crop to WxH using the pitch stride.
import sys, struct
from PIL import Image

BIN = sys.argv[1] if len(sys.argv) > 1 else "fb.bin"
OUT = sys.argv[2] if len(sys.argv) > 2 else "fb.png"
d = open(BIN, "rb").read()
magic, W, H, pitch = struct.unpack_from("<IIII", d, 0)
if magic != 0x42465941:
    sys.stderr.write("bad magic 0x%08x (not an AYFB framebuffer dump)\n" % magic)
    sys.exit(1)
px = d[16:]
img = Image.new("RGB", (W, H))
out = img.load()
for y in range(H):
    row = (y * pitch) * 4
    for x in range(W):
        o = row + x * 4
        b = px[o]; g = px[o + 1]; r = px[o + 2]     # stored 0xAARRGGBB little-endian -> B,G,R,A
        out[x, y] = (r, g, b)
img.save(OUT)
print("wrote %s (%dx%d, pitch=%d)" % (OUT, W, H, pitch))
