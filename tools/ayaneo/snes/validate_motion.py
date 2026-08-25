#!/usr/bin/env python3
"""
Frame-by-frame MOTION validation: diff our host `seq` render of a transition
against the deterministic web capture (/work/webmotion/<name>, produced by
snesref/tools/motion_web.mjs). Both step at the same fixed dt (0.0333) from the
same settled start, so frame N corresponds on both sides.

For each transition:
  - run host_render seq (normal + flat) -> /tmp/mine_motion/<name>/NNN.ppm
  - per frame: UI-mask the wallpaper (mine==flat), score meanAbsDiff/3 over UI
    pixels vs the web frame, print the per-frame curve
  - write /work/motiondiff/<name>_montage.png (rows of web|mine|diff at sampled
    frames) so the transition can be eyeballed end to end.

Usage: validate_motion.py <pack.bin> <host_render_bin> [name ...]
       (default: every transition present under /work/webmotion)
"""
import os, sys, json, subprocess
import numpy as np
from PIL import Image, ImageChops, ImageDraw

WEB = "/work/webmotion"
OUT = "/work/motiondiff"
MINE = "/tmp/mine_motion"
os.makedirs(OUT, exist_ok=True)

def load_ppm(path):
    return Image.open(path).convert("RGB").crop((0, 120, 1280, 840))  # content region

def run_seq(pack, binp, name, meta, flat):
    outdir = os.path.join(MINE, name + ("_flat" if flat else ""))
    os.makedirs(outdir, exist_ok=True)
    prefix = "".join(meta["prefix"]) or "."      # "." = empty prefix placeholder
    keys = "".join(meta["keys"])
    n = int(meta["frames"])
    args = [binp, pack, outdir, "seq", prefix if prefix != "." else "", keys, str(n)]
    if flat:
        args.append("flat")
    subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return outdir

def score_frame(web, mine, flat):
    wn = np.asarray(web).astype(int)
    mn = np.asarray(mine).astype(int)
    fn = np.asarray(flat).astype(int)
    ui = np.all(mn == fn, axis=2)                # wallpaper-independent UI pixels
    d = np.abs(wn - mn).mean(2)
    return (float(d[ui].mean() / 3.0) if ui.any() else 0.0), ui

def montage(name, webs, mines, diffs, scores):
    # sample up to 8 frames evenly across the transition
    n = len(webs)
    idxs = sorted(set(int(round(i * (n - 1) / 7.0)) for i in range(min(8, n))))
    cw, ch = 1280 // 4, 720 // 4          # 320x180 cells keep montage < 2000px
    rows = len(idxs)
    canvas = Image.new("RGB", (cw * 3 + 16, (ch + 14) * rows), (18, 18, 18))
    dr = ImageDraw.Draw(canvas)
    for r, fi in enumerate(idxs):
        y = r * (ch + 14) + 2
        for c, im in enumerate([webs[fi], mines[fi], diffs[fi]]):
            canvas.paste(im.resize((cw, ch)), (c * (cw + 6), y))
        dr.text((4, y + 2), f"f{fi:02d} {scores[fi]:.1f}", fill=(255, 255, 0))
    canvas.save(f"{OUT}/{name}_montage.png")

def validate(pack, binp, name):
    wdir = os.path.join(WEB, name)
    meta_p = os.path.join(wdir, "meta.json")
    if not os.path.exists(meta_p):
        print(f"{name}: no web capture (run motion_web.mjs {name})"); return
    meta = json.load(open(meta_p))
    n = int(meta["frames"])
    mdir = run_seq(pack, binp, name, meta, False)
    fdir = run_seq(pack, binp, name, meta, True)
    webs, mines, diffs, scores = [], [], [], []
    for fi in range(n):
        wp = os.path.join(wdir, f"{fi:03d}.png")
        mp = os.path.join(mdir, f"{fi:03d}.ppm")
        fp = os.path.join(fdir, f"{fi:03d}.ppm")
        if not (os.path.exists(wp) and os.path.exists(mp)):
            break
        web = Image.open(wp).convert("RGB").resize((1280, 720))
        mine = load_ppm(mp); flat = load_ppm(fp)
        s, ui = score_frame(web, mine, flat)
        dv = ImageChops.difference(web, mine).point(lambda p: min(255, p * 3))
        webs.append(web); mines.append(mine); diffs.append(dv); scores.append(s)
    if not scores:
        print(f"{name}: no frames"); return
    montage(name, webs, mines, diffs, scores)
    curve = " ".join(f"{s:.0f}" for s in scores)
    print(f"{name:14s} frames={len(scores)} mean={np.mean(scores):5.2f} "
          f"max={np.max(scores):5.2f} @f{int(np.argmax(scores))}")
    print(f"  per-frame: {curve}")

def main():
    if len(sys.argv) < 3:
        print("usage: validate_motion.py <pack.bin> <host_render> [name ...]"); return
    pack, binp = sys.argv[1], sys.argv[2]
    only = sys.argv[3:]
    names = only or sorted(d for d in os.listdir(WEB)
                           if os.path.isdir(os.path.join(WEB, d))) if os.path.isdir(WEB) else []
    if not names:
        print("no web motion captures under", WEB, "- run snesref/tools/motion_web.mjs first"); return
    for name in names:
        validate(pack, binp, name)

if __name__ == "__main__":
    main()
