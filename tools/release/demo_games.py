#!/usr/bin/env python3
"""Put the release's demo games on a card template: ROMs, box art and their licences.

    demo_games.py <manifest.json> <card dir> <download cache dir>

Each manifest entry names a freely licensed homebrew game: where to download it (checked against a
pinned sha256), the ROM inside the download (checked too), the licence texts to ship with it (each
pinned), the credit line, and optionally the game's own art (a title screen or logo from the project, or a
title screen captured from the game, kept in tools/release/art) that the box cover is made from. Writes
roms/<system>/<file>, roms/<system>/boxart/<stem>.ART (the menu's box art format,
tools/ayaneo/gba/gen_boxart.py) and licences/games/<stem>.txt, plus licences/games/CREDITS.txt.
"""
import hashlib, io, json, os, sys, tarfile, textwrap, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "ayaneo", "gba"))
from gen_boxart import convert_one          # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

SYSTEMS = {   # card folder: system name (licence files, credits), title card colour
    "gb": ("Game Boy", (78, 92, 60)), "gbc": ("Game Boy Color", (96, 44, 140)),
    "gba": ("Game Boy Advance", (48, 52, 140)), "snes": ("Super Nintendo", (110, 110, 120)),
    "genesis": ("Mega Drive / Genesis", (20, 20, 24)), "sms": ("Master System", (150, 20, 24)),
    "gg": ("Game Gear", (36, 36, 44)), "sg": ("SG-1000", (120, 30, 30)),
}
FONT = os.path.join(ROOT, "cores", "armGBA-pico", "upstream", "armulators", "app", "fonts",
                    "ChakraPetch-Bold.ttf")


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def fetch(url, want, cache):
    """the bytes at url, from the cache when present; refused unless the sha256 matches"""
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, want)
    if os.path.exists(path):
        data = open(path, "rb").read()
        if sha256(data) == want:
            return data
    req = urllib.request.Request(url, headers={"User-Agent": "gammaos-release"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    got = sha256(data)
    if got != want:
        sys.exit("%s: sha256 %s, the manifest pins %s" % (url, got, want))
    with open(path, "wb") as f:
        f.write(data)
    return data


def member(data, name):
    """a file inside a zip or tar download (name None: the download itself)"""
    if not name:
        return data
    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            return z.read(name)
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        return t.extractfile(name).read()


def font(sz):
    try:
        return ImageFont.truetype(FONT, sz)
    except OSError:
        return ImageFont.load_default()


def fit_lines(d, text, width, sizes, max_lines):
    """the largest size (and its lines) at which text wraps into max_lines within width"""
    for sz in sizes:
        f = font(sz)
        lines = textwrap.wrap(text, width=max(6, int(width / (sz * 0.52))))
        if len(lines) <= max_lines and all(d.textlength(l, font=f) <= width for l in lines):
            return sz, f, lines
    return sz, f, lines


def cover(title, system, art):
    """a 224x224 box cover: a spine in the system's colour with its name, the game's own art framed
    (whole pixel steps for small pixel art), the title under it; without art the title fills the
    frame. The bottom 28 rows stay plain: the menu draws its system badge there."""
    name, colour = SYSTEMS[system]
    W = H = 224
    SP = 30
    im = Image.new("RGB", (W, H), tuple(int(c * 0.35) for c in colour))
    d = ImageDraw.Draw(im)
    spine = Image.new("RGB", (H, SP), colour)
    sd = ImageDraw.Draw(spine)
    f = font(15)
    sd.text(((H - sd.textlength(name.upper(), font=f)) / 2, (SP - 18) / 2), name.upper(), font=f,
            fill=(255, 255, 255))
    im.paste(spine.rotate(90, expand=True), (0, 0))
    x0, y0, x1, y1 = SP + 8, 8, W - 8, 140
    pw, ph = x1 - x0, y1 - y0
    d.rectangle([x0 - 2, y0 - 2, x1 + 1, y1 + 1], fill=(235, 235, 235))
    panel = Image.new("RGB", (pw, ph), (0, 0, 0) if art is not None else colour)
    if art is not None:
        a = art.convert("RGB")
        aw, ah = a.size
        k = min(pw / aw, ph / ah)
        if k >= 1:
            a = a.resize((aw * int(k), ah * int(k)), Image.NEAREST)
        else:
            a = a.resize((max(1, int(aw * k)), max(1, int(ah * k))), Image.LANCZOS)
        panel.paste(a, ((pw - a.size[0]) // 2, (ph - a.size[1]) // 2))
    else:
        pd = ImageDraw.Draw(panel)
        sz, f, lines = fit_lines(pd, title, pw - 12, (30, 26, 22, 18), 3)
        y = (ph - len(lines) * (sz + 4)) // 2
        for l in lines:
            pd.text(((pw - pd.textlength(l, font=f)) / 2, y), l, font=f, fill=(255, 255, 255))
            y += sz + 4
    im.paste(panel, (x0, y0))
    sz, f, lines = fit_lines(d, title, W - SP - 14, (19, 17, 15, 13, 12), 2)
    y = 145 + (50 - len(lines) * (sz + 3)) // 2
    for l in lines:
        d.text((SP + (W - SP - d.textlength(l, font=f)) / 2, y), l, font=f, fill=(255, 255, 255))
        y += sz + 3
    d.rectangle([0, 0, W - 1, H - 1], outline=(20, 20, 20))
    return im


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    games = json.load(open(sys.argv[1]))
    card, cache = sys.argv[2], sys.argv[3]
    credits = ["Demo games on this card. Each is free software or freely licensed and is",
               "redistributed under its licence; the full text is beside this file.", ""]
    for g in games:
        sysdir, fname = g["system"], g["file"]
        stem = os.path.splitext(fname)[0]
        rom = member(fetch(g["url"], g["sha256"], cache), g.get("member"))
        if sha256(rom) != g["rom_sha256"]:
            sys.exit("%s: the ROM's sha256 is %s, the manifest pins %s" % (fname, sha256(rom), g["rom_sha256"]))
        os.makedirs(os.path.join(card, "roms", sysdir, "boxart"), exist_ok=True)
        with open(os.path.join(card, "roms", sysdir, fname), "wb") as f:
            f.write(rom)
        art = g.get("art")
        if art and art.get("file"):   # art kept in this repository (tools/release/art)
            path = os.path.join(ROOT, art["file"])
            if sha256(open(path, "rb").read()) != art["sha256"]:
                sys.exit("%s: does not match its pinned sha256" % art["file"])
            pic = Image.open(path)
        elif art:
            pic = Image.open(io.BytesIO(member(fetch(art["url"], art["sha256"], cache), art.get("member"))))
        else:
            pic = None
        with open(os.path.join(card, "roms", sysdir, "boxart", stem + ".ART"), "wb") as f:
            f.write(convert_one(cover(g["title"], sysdir, pic), 224))
        os.makedirs(os.path.join(card, "licences", "games"), exist_ok=True)
        with open(os.path.join(card, "licences", "games", stem + ".txt"), "wb") as f:
            f.write(("%s (%s)\n%s\nLicence: %s\nSource: %s\n"
                     "The box art on the card (roms/%s/boxart/%s.ART) is made from the game's own %s\n"
                     "and is under the game's licence.\n" % (
                g["title"], SYSTEMS[sysdir][0], g["credit"], g["licence"], g["home"], sysdir, stem,
                "title screen as captured from the game" if art and art.get("file") else
                "art" if art else "title")).encode())
            for lic in g["licences"]:   # the licence texts as the project ships them
                f.write(("\n==== %s ====\n\n" % lic["url"]).encode())
                f.write(fetch(lic["url"], lic["sha256"], cache))
        credits.append("%-34s %-22s %s, %s" % (g["title"], SYSTEMS[sysdir][0], g["credit"], g["licence"]))
        print("  %s/%s (%d bytes, %s)" % (sysdir, fname, len(rom), "cover art" if art else "title card"))
    os.makedirs(os.path.join(card, "licences", "games"), exist_ok=True)
    with open(os.path.join(card, "licences", "games", "CREDITS.txt"), "w") as f:
        f.write("\n".join(credits) + "\n")


if __name__ == "__main__":
    main()
