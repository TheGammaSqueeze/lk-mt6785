#!/usr/bin/env python3
"""
Overlay-diff my LK menu render against the web reference for every menu state.

For each state: render it with the host harness (host_render), crop the 1280x960
frame to the 1280x720 content region (letterbox offy=120), and compare to the
matching /work/webref/<state>.png. Emits, per state:
  /work/snesdiff/<state>_mine.png   my render (content region)
  /work/snesdiff/<state>_web.png    web reference
  /work/snesdiff/<state>_diff.png   per-pixel abs-diff heatmap
  /work/snesdiff/<state>_sbs.png    side-by-side (web | mine | diff)
and prints a mean-abs-diff score (0=identical). Also writes a montage grid.

Usage: validate.py <pack.bin> <host_render_bin> [state ...]
"""
import os, sys, subprocess
from PIL import Image, ImageChops, ImageDraw

WEB = "/work/webref"
OUT = "/work/snesdiff"
os.makedirs(OUT, exist_ok=True)

# state -> host_render nav string (auto-release + settle between keys)
STATES = {
    "home": "", "carousel_r3": "RRR",
    "menubar_display": "U", "menubar_options": "UR", "menubar_lang": "URR",
    "menubar_copy": "URRR", "menubar_manual": "URRRR",
    "sub_display": "UA", "sub_options": "URA", "sub_language": "URRA",
    "sub_copyright": "URRRA", "sub_manual": "URRRRA",
    "resume": "D",
}
SETTLE = 40   # frames per key (settle transitions)

def render_mine(pack, binp, nav, ppm):
    subprocess.run([binp, pack, ppm, str(SETTLE), nav],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    im = Image.open(ppm).convert("RGB")
    return im.crop((0, 120, 1280, 840))   # content region (offy=120, 720 tall)

def score(a, b):
    d = ImageChops.difference(a, b)
    hist = d.histogram()
    # mean abs diff across all channels
    tot = sum(i % 256 * hist[i] for i in range(len(hist)))
    n = sum(hist)
    return tot / n if n else 0

def main():
    pack, binp = sys.argv[1], sys.argv[2]
    only = sys.argv[3:]
    rows = []
    for name, nav in STATES.items():
        if only and name not in only:
            continue
        webp = os.path.join(WEB, name + ".png")
        if not os.path.exists(webp):
            print(f"{name:18s} NO WEB REF"); continue
        web = Image.open(webp).convert("RGB").resize((1280, 720))
        mine = render_mine(pack, binp, nav, f"/tmp/mine_{name}.ppm")
        diff = ImageChops.difference(web, mine)
        # amplify diff for visibility
        dv = diff.point(lambda p: min(255, p * 3))
        s = score(web, mine)
        web.save(f"{OUT}/{name}_web.png"); mine.save(f"{OUT}/{name}_mine.png")
        dv.save(f"{OUT}/{name}_diff.png")
        sbs = Image.new("RGB", (1280, 720 * 3 + 40), (20, 20, 20))
        sbs.paste(web, (0, 0)); sbs.paste(mine, (0, 720 + 20)); sbs.paste(dv, (0, 1480))
        sbs.save(f"{OUT}/{name}_sbs.png")
        rows.append((name, s))
        print(f"{name:18s} meanAbsDiff={s:6.2f}")
    if rows:
        worst = sorted(rows, key=lambda r: -r[1])
        print("\nWORST FIRST:")
        for n, s in worst:
            print(f"  {n:18s} {s:6.2f}")

if __name__ == "__main__":
    main()
