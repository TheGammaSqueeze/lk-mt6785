#!/usr/bin/env python3
"""
Build the names glyph pack (/gammaos/names.pack on the card): the glyphs of the characters game
names can hold beyond the menu fonts' own, drawn once at a master size, for the LK to scale to
each menu font that shows names (emu/gba/pico_names.c). What the names on a card use is all the
LK reads of it.

    build_names.py <out.pack>

Layout (little endian):
  header, 56 bytes: "PNAM", u16 version (2), u16 master px, s16 ascent, s16 descent,
      u32 glyph count, u32 block count, u32 index offset, u32 block table offset,
      u32 joining table offset, u32 joining count, u32 ligature table offset, u32 ligature count,
      u32 composition table offset, u32 composition count, 4 bytes 0
  index, 16 bytes a glyph, sorted by key: u32 key (code point, weight << 24: 0 Regular,
      1 Medium), s8 x offset, s8 y offset (from the pen at the baseline), u8 width, u8 height,
      u16 advance (1/64 px), u16 block, u32 offset of its rows in the block
  block table, 12 bytes a block: u32 offset in the file, u32 deflated length, u32 length
  joining table, 10 bytes a letter: u16 Arabic letter, then its isolated, final, initial and
      medial forms (0 where it has none: a letter without an initial form joins on its right only)
  ligature table, 12 bytes: u16 lam, u16 alef, u16 isolated, u16 final, 4 bytes 0
  composition table, 8 bytes a pair, sorted by key: u32 key (letter << 16 | combining mark),
      u32 the precomposed character (Unicode's canonical compositions in the BMP: a name written
      decomposed, as macOS does, is put back together before it is drawn)
  the blocks: raw deflate of the glyphs' rows, 4 bits a pixel, high nibble first, rows padded to a
      byte; 64 glyphs a block, in index order
Medium is there for what Inter draws (the focused rows' weight); the LK draws the rest of a focused
row from Regular.
"""
import os, struct, sys, unicodedata, zlib
from PIL import Image, ImageDraw
import fontsrc, names

MASTER_PX = 36
PER_BLOCK = 64


def source_path(code, src, weight):
    if src == "inter":
        p = fontsrc.inter_path(weight)
        if code in fontsrc.cmap(p):
            return p
        return names.names_path("JP") if weight == "Regular" else None
    if weight != "Regular":
        return None
    return names.names_path(src)


def render(code, path):
    f = fontsrc.pil_font(path, MASTER_PX)
    ch = chr(code)
    bb = f.getbbox(ch, anchor="ls")
    w, h = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
    im = None
    if w and h:
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-bb[0], -bb[1]), ch, font=f, fill=255, anchor="ls")
    adv = round(f.getlength(ch) * 64)
    if 0x300 <= code <= 0x36F:
        adv = 0        # a combining mark sits on the letter before it
    return bb[0], bb[1], w, h, adv, im


def rows4(im, w, h):
    out = bytearray()
    if im is None:
        return out
    px = im.load()
    for y in range(h):
        for x in range(0, w, 2):
            a = (px[x, y] * 15 + 127) // 255
            b = (px[x + 1, y] * 15 + 127) // 255 if x + 1 < w else 0
            out.append((a << 4) | b)
    return out


def compositions():
    """[(letter << 16 | mark, composed)]: the canonical pairs of the BMP that NFC puts together."""
    out = []
    for cp in range(0x80, 0x10000):
        d = unicodedata.decomposition(chr(cp))
        if not d or d.startswith("<"):
            continue
        parts = [int(x, 16) for x in d.split()]
        if len(parts) == 2 and unicodedata.normalize("NFC", chr(parts[0]) + chr(parts[1])) == chr(cp):
            out.append(((parts[0] << 16) | parts[1], cp))
    return sorted(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    glyphs = []
    for weight, wbit in (("Regular", 0), ("Medium", 1)):
        for code, src in names.wanted():
            path = source_path(code, src, weight)
            if not path or code not in fontsrc.cmap(path):
                continue
            xo, yo, w, h, adv, im = render(code, path)
            assert -128 <= xo < 128 and -128 <= yo < 128 and w < 256 and h < 256 and adv < 65536, hex(code)
            glyphs.append((code | (wbit << 24), xo, yo, w, h, adv, rows4(im, w, h)))
    glyphs.sort(key=lambda g: g[0])
    forms, ligs = names.arabic_forms()
    have = {g[0] for g in glyphs}
    join = []
    for letter in sorted(forms):
        f = forms[letter]
        row = [f.get(k, 0) if f.get(k, 0) in have else 0 for k in ("isolated", "final", "initial", "medial")]
        if row[0]:
            join.append([letter] + row)
    lig = []
    for (lam, alef), f in sorted(ligs.items()):
        iso, fin = f.get("isolated", 0), f.get("final", 0)
        if iso in have and fin in have:
            lig.append((lam, alef, iso, fin))

    index, blocks, data = bytearray(), [], []
    for b in range(0, len(glyphs), PER_BLOCK):
        raw = bytearray()
        for i, g in enumerate(glyphs[b:b + PER_BLOCK]):
            index += struct.pack("<IbbBBHHI", g[0], g[1], g[2], g[3], g[4], g[5], b // PER_BLOCK, len(raw))
            raw += g[6]
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(bytes(raw)) + c.flush()
        blocks.append((len(z), len(raw)))
        data.append(z)
    asc, desc = fontsrc.pil_font(fontsrc.inter_path("Regular"), MASTER_PX).getmetrics()
    comp = compositions()
    hdr_len = 56
    index_off = hdr_len
    table_off = index_off + len(index)
    join_off = table_off + 12 * len(blocks)
    lig_off = join_off + 10 * len(join)
    comp_off = lig_off + 12 * len(lig)
    data_off = comp_off + 8 * len(comp)
    table, pos = bytearray(), data_off
    for (zl, rl), z in zip(blocks, data):
        table += struct.pack("<III", pos, zl, rl)
        pos += zl
    out = bytearray(struct.pack("<4sHHhhIIIIIIIIII4x", b"PNAM", 2, MASTER_PX, asc, -desc, len(glyphs), len(blocks),
                                index_off, table_off, join_off, len(join), lig_off, len(lig), comp_off, len(comp)))
    assert len(out) == hdr_len
    out += index + table
    for j in join:
        out += struct.pack("<5H", *j)
    for l in lig:
        out += struct.pack("<4H4x", *l)
    for k, c in comp:
        out += struct.pack("<II", k, c)
    for z in data:
        out += z
    with open(sys.argv[1], "wb") as fo:
        fo.write(out)
    print("names.pack: %d glyphs (%d Medium), %d blocks, %d joined letters, %d ligatures, %d compositions, %d bytes"
          % (len(glyphs), sum(1 for g in glyphs if g[0] >> 24), len(blocks), len(join), len(lig), len(comp), len(out)))


if __name__ == "__main__":
    main()
