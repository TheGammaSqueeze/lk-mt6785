#!/usr/bin/env python3
"""Put the release's demo games on a card template: ROMs, box art and their licences.

    demo_games.py <manifest.json> <card dir> <download cache dir>

Each manifest entry names a freely licensed homebrew game: where to download it (checked against a
pinned sha256), the ROM inside the download (checked too), the licence text to ship with it, the
credit line, and optionally licensed cover art. Without cover art a title card is drawn. Writes
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


def title_card(title, system):
    colour = SYSTEMS[system][1]
    w, h = 224, 224
    im = Image.new("RGB", (w, h), colour)
    d = ImageDraw.Draw(im)
    d.rectangle([6, 6, w - 7, h - 7], outline=(235, 235, 235), width=3)

    def font(sz):
        try:
            return ImageFont.truetype(FONT, sz)
        except OSError:
            return ImageFont.load_default()
    for sz in (34, 30, 26, 22, 18):   # the largest size the title fits at, in at most 4 lines
        f = font(sz)
        lines = textwrap.wrap(title, width=max(6, int(190 / (sz * 0.55))))
        if len(lines) <= 4 and all(d.textlength(l, font=f) <= 196 for l in lines):
            break
    y = (h - 24 - len(lines) * (sz + 6)) // 2   # a little high: the menu badges the system bottom right
    for l in lines:
        d.text(((w - d.textlength(l, font=f)) / 2, y), l, font=f, fill=(255, 255, 255))
        y += sz + 6
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
        if art:
            im = Image.open(io.BytesIO(member(fetch(art["url"], art["sha256"], cache), art.get("member"))))
        else:
            im = title_card(g["title"], sysdir)
        with open(os.path.join(card, "roms", sysdir, "boxart", stem + ".ART"), "wb") as f:
            f.write(convert_one(im, 224))
        lic = member(fetch(g["licence_url"], g["licence_sha256"], cache), g.get("licence_member"))
        os.makedirs(os.path.join(card, "licences", "games"), exist_ok=True)
        with open(os.path.join(card, "licences", "games", stem + ".txt"), "wb") as f:
            f.write(("%s (%s)\n%s\nLicence: %s\nSource: %s\n\n" % (
                g["title"], SYSTEMS[sysdir][0], g["credit"], g["licence"], g["home"])).encode())
            f.write(lic)
        credits.append("%-34s %-22s %s, %s" % (g["title"], SYSTEMS[sysdir][0], g["credit"], g["licence"]))
        print("  %s/%s (%d bytes, %s)" % (sysdir, fname, len(rom), "cover art" if art else "title card"))
    os.makedirs(os.path.join(card, "licences", "games"), exist_ok=True)
    with open(os.path.join(card, "licences", "games", "CREDITS.txt"), "w") as f:
        f.write("\n".join(credits) + "\n")


if __name__ == "__main__":
    main()
