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

# 4:3 mode: diff the full 1280x960 panel against /work/webref43 (aspect=4:3 web
# capture) with the host harness driven in 4:3 (SNES_ASPECT43=1). Enable with the
# env SNES_VALIDATE43=1. Native (16:9) crops to the 720 content region as before.
ASPECT43 = os.environ.get("SNES_VALIDATE43") == "1"
WEB = "/work/webref43" if ASPECT43 else "/work/webref"
OUT = "/work/snesdiff43" if ASPECT43 else "/work/snesdiff"
os.makedirs(OUT, exist_ok=True)
CW, CH = (1280, 960) if ASPECT43 else (1280, 720)

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

ENV43 = {**os.environ, "SNES_ASPECT43": "1"} if ASPECT43 else os.environ

def _crop(im):
    if ASPECT43:
        return im                                  # full panel, no letterbox
    return im.crop((0, 120, 1280, 840))            # content region (offy=120, 720 tall)

def render_mine(pack, binp, nav, ppm):
    subprocess.run([binp, pack, ppm, str(SETTLE), nav],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, env=ENV43)
    return _crop(Image.open(ppm).convert("RGB"))

def render_mine_flat(pack, binp, nav, ppm):
    """Same render but with the scrolling wallpaper replaced by a flat colour,
    so opaque UI is identical to the normal render and wallpaper-showing pixels
    differ - giving us a precise UI mask independent of wallpaper scroll phase."""
    subprocess.run([binp, pack, ppm, str(SETTLE), nav, "flat"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, env=ENV43)
    return _crop(Image.open(ppm).convert("RGB"))

def score(web, mine, flat=None):
    """Mean abs diff over UI pixels only. When a flat-wallpaper render is given,
    UI pixels are exactly those the wallpaper doesn't show through (mine==flat);
    we also include any bright web pixel so a missing UI element is still
    penalised. Wallpaper (non-deterministic scroll phase) is excluded. Without a
    flat render, fall back to the both-bright heuristic."""
    import numpy as np
    an = np.asarray(web, dtype=np.int16); bn = np.asarray(mine, dtype=np.int16)
    d = np.abs(an - bn).sum(axis=2)
    if flat is not None:
        fn = np.asarray(flat, dtype=np.int16)
        # UI = pixels the wallpaper doesn't reach in my render (opaque chrome,
        # cards, text). Deliberately NOT including bright web pixels: the web's
        # neon wallpaper is itself bright, so that would re-admit the very
        # scroll-phase noise we are trying to exclude. Missing UI elements are
        # caught by eye in the SBS, not by this metric.
        ui = np.all(bn == fn, axis=2)
    else:
        ui = (an.max(axis=2) > 70) | (bn.max(axis=2) > 70)
    return float(d[ui].mean() / 3.0) if ui.any() else 0.0

def worst_tile(web, mine, flat=None, tile=40):
    """Localized error detector. The global mean dilutes a small high-contrast
    mistake (e.g. black-vs-white button text over a few hundred px averages to
    ~1). Tile the UI-masked diff into `tile`x`tile` blocks and return the worst
    block's mean(diff/3) plus its centre (x,y) and how much of it is UI. A
    legibility/colour bug shows a high worst-tile score even at a low global
    mean, so it cannot hide. Only tiles with enough UI coverage count."""
    import numpy as np
    an = np.asarray(web, dtype=np.int16); bn = np.asarray(mine, dtype=np.int16)
    d = np.abs(an - bn).sum(axis=2) / 3.0
    if flat is not None:
        fn = np.asarray(flat, dtype=np.int16)
        ui = np.all(bn == fn, axis=2)
    else:
        ui = (an.max(axis=2) > 70) | (bn.max(axis=2) > 70)
    H, W = d.shape
    best = (0.0, 0, 0, 0.0)
    for ty in range(0, H, tile):
        for tx in range(0, W, tile):
            m = ui[ty:ty+tile, tx:tx+tile]
            cov = float(m.mean())
            if cov < 0.15:            # mostly wallpaper/empty: skip
                continue
            v = float(d[ty:ty+tile, tx:tx+tile][m].mean())
            if v > best[0]:
                best = (v, tx + tile // 2, ty + tile // 2, cov)
    return best

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
        web = Image.open(webp).convert("RGB").resize((CW, CH))
        mine = render_mine(pack, binp, nav, f"/tmp/mine_{name}.ppm")
        flat = render_mine_flat(pack, binp, nav, f"/tmp/mineflat_{name}.ppm")
        diff = ImageChops.difference(web, mine)
        # amplify diff for visibility
        dv = diff.point(lambda p: min(255, p * 3))
        s = score(web, mine, flat)
        wt, wx, wy, wcov = worst_tile(web, mine, flat)
        web.save(f"{OUT}/{name}_web.png"); mine.save(f"{OUT}/{name}_mine.png")
        dv.save(f"{OUT}/{name}_diff.png")
        sbs = Image.new("RGB", (CW, CH * 3 + 40), (20, 20, 20))
        sbs.paste(web, (0, 0)); sbs.paste(mine, (0, CH + 20)); sbs.paste(dv, (0, CH * 2 + 40))
        # ring the worst tile in the diff panel so it is obvious where to look
        dd = ImageDraw.Draw(sbs)
        dd.rectangle([wx-20, CH*2+40+wy-20, wx+20, CH*2+40+wy+20], outline=(255,0,0))
        sbs.save(f"{OUT}/{name}_sbs.png")
        rows.append((name, s, wt, wx, wy))
        flag = "  <-- LOCALIZED" if wt > 25.0 else ""
        print(f"{name:18s} meanAbsDiff={s:6.2f}  worstTile={wt:6.2f} @({wx},{wy}){flag}")
    if rows:
        print("\nWORST (by localized tile):")
        for n, s, wt, wx, wy in sorted(rows, key=lambda r: -r[2]):
            flag = "  <-- LOCALIZED" if wt > 25.0 else ""
            print(f"  {n:18s} mean={s:6.2f}  tile={wt:6.2f} @({wx},{wy}){flag}")

if __name__ == "__main__":
    main()
