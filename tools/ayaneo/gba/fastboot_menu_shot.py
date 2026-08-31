#!/usr/bin/env python3
# Reconstruct a PNG from the `fastboot oem menu-shot` output (the on-device SNES
# menu debug channel, emu/gba/menu_fastboot.c). Usage:
#
#   fastboot oem menu-shot 2>&1 | python3 tools/ayaneo/gba/fastboot_menu_shot.py out.png
#   # (or a bigger image:  fastboot oem menu-shot:8 2>&1 | ... )
#
# fastboot prints each INFO line as "(bootloader) NNN:hex..." (or "NNN+hex" for a
# row continuation). Each pixel is 4 hex chars of RGB565. We stitch the rows back
# into an image and upscale x8 so it is easy to eyeball.
import sys, re
from PIL import Image

OUT = sys.argv[1] if len(sys.argv) > 1 else "menu_shot.png"
rows = {}          # row index -> list of (r,g,b)
for line in sys.stdin:
    line = line.strip()
    line = re.sub(r"^\(bootloader\)\s*", "", line)
    m = re.match(r"^(\d+)([:+])([0-9a-fA-F]+)$", line)
    if not m:
        continue
    idx, sep, hx = int(m.group(1)), m.group(2), m.group(3)
    px = []
    for i in range(0, len(hx) - 3, 4):
        v = int(hx[i:i+4], 16)
        r = ((v >> 11) & 0x1f) << 3
        g = ((v >> 5) & 0x3f) << 2
        b = (v & 0x1f) << 3
        px.append((r, g, b))
    rows.setdefault(idx, [])
    rows[idx].extend(px)          # ':' starts a row, '+' continues it

if not rows:
    sys.stderr.write("no shot rows found on stdin (pipe `fastboot oem menu-shot 2>&1`)\n")
    sys.exit(1)

h = max(rows) + 1
w = max(len(v) for v in rows.values())
img = Image.new("RGB", (w, h), (0, 0, 0))
for y, px in rows.items():
    for x, c in enumerate(px):
        img.putpixel((x, y), c)
img = img.resize((w * 8, h * 8), Image.NEAREST)
img.save(OUT)
print("wrote %s (%dx%d source, x8)" % (OUT, w, h))
