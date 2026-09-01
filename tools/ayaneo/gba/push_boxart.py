#!/usr/bin/env python3
"""
Match a device's GBA ROMs to the No-Intro box-art library, generate .ART tiles
named by the *device* ROM basenames (so the menu loader finds them), and print the
fastboot commands to push them to the SD card.

The menu looks for /roms/gba/boxart/<romstem>.ART where romstem is the ROM's own
basename (which is usually NOT the No-Intro title), so the tiles must be renamed to
match. This fuzzy-matches each ROM name to the closest box front.

Get the device ROM list from the bootloader (SD is free there; while the menu runs
it holds the SD and sd-probe/sd-put fail):
    fastboot -s <serial> reboot bootloader
    fastboot -s <serial> oem sd-probe        # lists rom[0..7] names (cap 8)
Put those basenames (no .gba) in a text file, one per line, and run:
    push_boxart.py --roms roms.txt --zip <boxart.zip> --outdir out/devart [--push <serial>]

IMPORTANT (learned the hard way): pushing a VERY long filename over sd-put can wedge
the USB link mid-FAT-write and may corrupt the SD directory. Prefer a card reader for
bulk copies; if using sd-put, push one file at a time and power-cycle if it wedges.
"""
import sys, os, io, re, struct, zipfile, argparse, difflib, subprocess
import numpy as np
from PIL import Image

MAGIC = b"GART"


def norm(s):
    s = s.lower()
    s = re.sub(r"\([^)]*\)", "", s)         # drop (USA, Europe) region tags
    return re.sub(r"[^a-z0-9]+", " ", s).strip()


def to_art(im, md=224):
    im = im.convert("RGB")
    w, h = im.size
    if max(w, h) > md:
        if w >= h:
            nw, nh = md, max(1, round(h * md / w))
        else:
            nw, nh = max(1, round(w * md / h)), md
        im = im.resize((nw, nh), Image.LANCZOS)
    a = np.asarray(im, dtype=np.uint16)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    w, h = im.size
    return struct.pack("<4sHHHH", MAGIC, 1, 0, w, h) + v.astype("<u2").tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", required=True, help="text file of device ROM basenames (no .gba)")
    ap.add_argument("--zip", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--min-score", type=float, default=0.6)
    ap.add_argument("--push", help="device serial: also push each tile via fastboot sd-put")
    ap.add_argument("--max-push-name", type=int, default=32,
                    help="skip sd-put for .ART names longer than this (LFN write can wedge USB)")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    roms = [l.strip() for l in open(a.roms) if l.strip()]
    z = zipfile.ZipFile(a.zip)
    pngs = [n for n in z.namelist() if n.lower().endswith(".png") and "/Box - Front/" in n]
    idx = {norm(n.split("/")[-1][:-4]): n for n in pngs}
    keys = list(idx.keys())

    for dr in roms:
        nd = norm(dr)
        best, br = None, 0.0
        for k in keys:
            r = difflib.SequenceMatcher(None, nd, k).ratio()
            if nd in k or k in nd:
                r = max(r, 0.95)
            if r > br:
                br, best = r, k
        if br < a.min_score:
            print("SKIP %-40s (no match, best %.2f)" % (dr, br))
            continue
        im = Image.open(io.BytesIO(z.read(idx[best])))
        out = os.path.join(a.outdir, dr + ".ART")
        open(out, "wb").write(to_art(im))
        artname = dr + ".ART"
        print("OK   %-40s -> %s (%.2f)" % (dr, idx[best].split("/")[-1], br))
        if a.push:
            # A very long filename writes several LFN directory entries; that write
            # can starve the fastboot USB thread and wedge the link mid-write (it
            # stuck the device once). Skip wedge-prone names and tell the user to
            # copy those with a card reader instead.
            if len(artname) > a.max_push_name:
                print("     SKIP push (%d chars > %d, wedge risk) - copy with a card reader"
                      % (len(artname), a.max_push_name))
                continue
            subprocess.run(["fastboot", "-s", a.push, "stage", out],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            r = subprocess.run(["fastboot", "-s", a.push,
                                "oem", "sd-put:/roms/gba/boxart/%s" % artname],
                               capture_output=True, text=True)
            print("     push:", (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "?")


if __name__ == "__main__":
    main()
