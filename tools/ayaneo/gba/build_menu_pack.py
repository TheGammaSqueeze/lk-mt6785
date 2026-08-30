#!/usr/bin/env python3
"""
Build the GBA carousel-menu asset pack (snespack binary format, reused by the
imported emu/gba/menu render engine). Generates placeholder art procedurally
(indigo/purple parallax wallpaper + a GBA-cartridge tile template), renders a
BMFont from DejaVuSans-Bold, and synthesizes the move/confirm SFX + a short
looping music bed. Box art is not available yet, so games render on the cart
template with their title text; real art becomes a drop-in image later.

Output: a single little-endian blob the LK engine mmaps (snes_pack_open). Emits
only the tables the GBA menu needs (strpool, res, img, spr, font, snd); scene/
anim/game tables are empty. Resource ids are hashed FNV-1a, matching the reader.

Usage: build_menu_pack.py <out.pack>
"""
import sys, struct, math, zlib
from PIL import Image, ImageDraw, ImageFont

MAGIC, VERSION = 0x534E4553, 1
# header: magic,ver,total,flags + 11 (off,count) pairs + init + (oss_off,count)
HEADER_FMT = "<IIII" + "II" * 11 + "I" + "II"
HEADER_SIZE = struct.calcsize(HEADER_FMT)          # 116
RES_TEXTURE, RES_SOUND, RES_FONT, RES_SPRITE = 2, 6, 7, 1
IMG_TILE, IMG_RGB565 = 0x1, 0x2
FONT_TTF = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def fnv1a(s):
    h = 2166136261
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


class Blob:
    def __init__(self):
        self.buf = bytearray(HEADER_SIZE)
        self.buf.append(0)                          # off 0 == empty string
        self._strs = {}
    def align(self, a=4):
        while len(self.buf) % a:
            self.buf.append(0)
    def write(self, data):
        off = len(self.buf); self.buf += data; return off
    def str(self, s):
        if not s:
            return 0
        if s in self._strs:
            return self._strs[s]
        off = len(self.buf); self.buf += s.encode("utf-8") + b"\x00"
        self._strs[s] = off; return off


class Packer:
    def __init__(self):
        self.b = Blob()
        self.res = []      # (id, type, index)
        self.imgs = []     # (w,h,flags,pixels_off)
        self.sprs = []     # (img, sx,sy,sw,sh, px,py)
        self.fonts = []    # (page,line_height,base,glyph_count,glyphs_off)
        self.snds = []     # (rate,frames,channels,fmt,loop_start,loop_end,pcm_off)

    # ---- images: RGBA8888 top-down ----
    def add_image(self, rid, im, tile=False):
        im = im.convert("RGBA")
        w, h = im.size
        self.b.align(4)
        px_off = self.b.write(im.tobytes())         # RGBA rows top-down
        idx = len(self.imgs)
        self.imgs.append((w, h, IMG_TILE if tile else 0, px_off))
        if rid:
            self.res.append((rid, RES_TEXTURE, idx))
        return idx

    def add_sprite(self, rid, img_idx, rect=None, pivot=(0, 0)):
        w = self.imgs[img_idx][0]; h = self.imgs[img_idx][1]
        sx, sy, sw, sh = rect if rect else (0, 0, w, h)
        idx = len(self.sprs)
        self.sprs.append((img_idx, sx, sy, sw, sh, pivot[0], pivot[1]))
        self.res.append((rid, RES_SPRITE, idx))
        return idx

    # ---- BMFont: render glyphs into an atlas, record metrics ----
    def add_font(self, rid, ttf, size, chars=None):
        if chars is None:
            chars = [chr(c) for c in range(32, 127)]
        font = ImageFont.truetype(ttf, size)
        asc, desc = font.getmetrics()
        line_h = asc + desc
        pad = 2
        # lay glyphs left-to-right into rows of a square-ish atlas
        cells = []
        for ch in chars:
            m = font.getmask(ch)
            gw, gh = m.size
            try:
                box = font.getbbox(ch)
                xo, yo = box[0], box[1]
                xadv = int(round(font.getlength(ch)))
            except Exception:
                xo, yo, xadv = 0, 0, gw
            cells.append((ch, gw, gh, xo, yo, xadv))
        atlas_w = 512
        x = pad; y = pad; rowh = 0; placed = []
        for (ch, gw, gh, xo, yo, xadv) in cells:
            if x + gw + pad > atlas_w:
                x = pad; y += rowh + pad; rowh = 0
            placed.append((ch, x, y, gw, gh, xo, yo, xadv))
            x += gw + pad; rowh = max(rowh, gh)
        atlas_h = 1
        while atlas_h < y + rowh + pad:
            atlas_h *= 2
        atlas = Image.new("RGBA", (atlas_w, atlas_h), (255, 255, 255, 0))
        d = ImageDraw.Draw(atlas)
        glyphs = []
        for (ch, gx, gy, gw, gh, xo, yo, xadv) in placed:
            # draw glyph white (tinted at blit time); position so bbox top-left
            # lands at (gx,gy)
            d.text((gx - xo, gy - yo), ch, font=font, fill=(255, 255, 255, 255))
            glyphs.append((ord(ch), gx, gy, gw, gh, xo, yo, xadv))
        page = self.add_image("", atlas)            # font page (no res id)
        glyphs.sort(key=lambda g: g[0])
        self.b.align(4)
        goff = len(self.b.buf)
        for (cp, gxx, gyy, gw, gh, xo, yo, xadv) in glyphs:
            self.b.write(struct.pack("<IHHHHhhhh", cp, gxx, gyy, gw, gh,
                                     xo, yo, xadv, 0))
        idx = len(self.fonts)
        self.fonts.append((page, line_h, asc, len(glyphs), goff))
        self.res.append((rid, RES_FONT, idx))
        return idx

    # ---- sounds: s16 mono/stereo PCM ----
    def add_sound(self, rid, samples, rate, channels=1, loop=None):
        import array
        a = array.array("h", [max(-32768, min(32767, int(s))) for s in samples])
        if sys.byteorder == "big":
            a.byteswap()
        self.b.align(4)
        pcm_off = self.b.write(a.tobytes())
        frames = len(samples) // channels
        ls, le = (loop if loop else (0, frames))
        idx = len(self.snds)
        self.snds.append((rate, frames, channels, 0, ls, le, pcm_off))
        self.res.append((rid, RES_SOUND, idx))
        return idx

    # ---- finalize ----
    def build(self):
        b = self.b
        # per-type tables
        b.align(4); img_off = len(b.buf)
        for (w, h, fl, px) in self.imgs:
            b.write(struct.pack("<HHHHI", w, h, fl, 0, px))
        b.align(4); spr_off = len(b.buf)
        for (im, sx, sy, sw, sh, px, py) in self.sprs:
            b.write(struct.pack("<HHHHHhh", im, sx, sy, sw, sh, px, py))
        b.align(4); font_off = len(b.buf)
        for (pg, lh, ba, gc, go) in self.fonts:
            b.write(struct.pack("<HHHHI", pg, lh, ba, gc, go))
        b.align(4); snd_off = len(b.buf)
        for (rate, fr, ch, fmt, ls, le, pc) in self.snds:
            b.write(struct.pack("<IIBBBBIII", rate, fr, ch, fmt, 0, 0, ls, le, pc))
        # resource hash table: pow2 >= 2*count, open-addressed linear probe
        n = 1
        while n < max(1, len(self.res) * 2):
            n *= 2
        table = [None] * n
        for (rid, typ, index) in self.res:
            hh = fnv1a(rid)
            i = hh & (n - 1)
            while table[i] is not None:
                i = (i + 1) & (n - 1)
            table[i] = (hh, b.str(rid), typ, index)
        b.align(4); res_off = len(b.buf)
        for slot in table:
            if slot is None:
                b.write(struct.pack("<IIHH", 0, 0, 0, 0))
            else:
                hh, so, typ, index = slot
                b.write(struct.pack("<IIHH", hh, so, typ, index))
        total = len(b.buf)
        hdr = struct.pack(HEADER_FMT,
                          MAGIC, VERSION, total, 0,
                          0, b.buf and 1 or 0,        # strpool_off=0, len (unused by reader beyond base)
                          res_off, n,
                          img_off, len(self.imgs),
                          spr_off, len(self.sprs),
                          font_off, len(self.fonts),
                          0, 0,                        # scene
                          0, 0,                        # anim
                          0, 0,                        # sanim
                          snd_off, len(self.snds),
                          0, 0,                        # str
                          0, 0,                        # game
                          0,                           # init
                          0, 0)                        # oss
        b.buf[0:HEADER_SIZE] = hdr
        return bytes(b.buf)


# ---------- procedural asset generation ----------
def gen_wallpaper(w=256, h=256):
    """Seamless-tiling indigo/purple wallpaper with a soft diagonal weave motif."""
    im = Image.new("RGBA", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            # base vertical gradient indigo -> deep purple
            t = y / (h - 1)
            r = int(24 + 34 * t)
            g = int(18 + 10 * t)
            bl = int(56 + 60 * t)
            # seamless diagonal weave using sines of tau*x/w etc.
            wv = (math.sin(2 * math.pi * (x * 2) / w) +
                  math.sin(2 * math.pi * (y * 2) / h) +
                  math.sin(2 * math.pi * (x + y) / w))
            d = int(10 * wv)
            px[x, y] = (max(0, min(255, r + d)),
                        max(0, min(255, g + d // 2)),
                        max(0, min(255, bl + d)), 255)
    return im


def gen_cart(w=300, h=220):
    """A GBA-cartridge-style placeholder tile: rounded dark body, top label plate,
    ridged bottom connector. Title text is drawn by the menu at runtime."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    body = (40, 40, 52, 255)
    edge = (90, 100, 140, 255)
    d.rounded_rectangle([6, 6, w - 6, h - 6], radius=18, fill=body, outline=edge, width=3)
    # top label plate (where boxart/title goes)
    d.rounded_rectangle([20, 20, w - 20, int(h * 0.62)], radius=10,
                        fill=(230, 232, 240, 255), outline=(150, 155, 170, 255), width=2)
    # notch top-right (cart shape cue)
    d.rectangle([w - 60, 6, w - 6, 26], fill=body)
    d.rounded_rectangle([w - 58, 8, w - 8, 24], radius=6, fill=(60, 62, 78, 255))
    # bottom connector ridges
    by = int(h * 0.70)
    for i in range(10):
        cx = 30 + i * ((w - 60) / 9.0)
        d.rectangle([cx - 6, by, cx + 6, by + 26], fill=(70, 72, 90, 255))
    return im


def synth_blip(rate, ms, f0, f1, vol=0.35, square=True):
    n = int(rate * ms / 1000.0); out = []
    for i in range(n):
        t = i / rate
        f = f0 + (f1 - f0) * (i / max(1, n))
        ph = 2 * math.pi * f * t
        s = (1.0 if math.sin(ph) >= 0 else -1.0) if square else math.sin(ph)
        env = min(1.0, (n - i) / (rate * 0.02))          # short decay
        env = min(env, min(1.0, i / (rate * 0.004)))     # tiny attack
        out.append(s * env * vol * 32767)
    return out


def synth_music(rate, seconds=8.0, vol=0.22):
    """Simple looping chiptune bed: arpeggio over a slow bass, square+triangle."""
    n = int(rate * seconds); out = [0.0] * n
    # A minor-ish arpeggio pattern (Hz), stepped 8 per bar
    arp = [220.0, 261.63, 329.63, 261.63, 220.0, 329.63, 392.0, 329.63]
    bass = [110.0, 110.0, 146.83, 146.83, 164.81, 164.81, 130.81, 130.81]
    step = rate * seconds / (len(arp) * 4)               # 4 bars over the loop
    for i in range(n):
        t = i / rate
        si = int(i / step) % len(arp)
        fa = arp[si]; fb = bass[si]
        # square lead
        lead = 1.0 if math.sin(2 * math.pi * fa * t) >= 0 else -1.0
        # triangle-ish bass
        bph = (fb * t) % 1.0
        bs = 4 * abs(bph - 0.5) - 1.0
        env = 0.5 + 0.5 * math.sin(2 * math.pi * (i % step) / step - math.pi / 2)
        out[i] = (lead * 0.5 * env + bs * 0.5) * vol * 32767
    return out


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: build_menu_pack.py <out.pack>")
    out = sys.argv[1]
    rate = 22050
    p = Packer()
    wp = p.add_image("gbamenu/wallpaper", gen_wallpaper(), tile=True)
    p.add_sprite("gbamenu/wallpaper.spr", wp)
    cart = p.add_image("gbamenu/cart", gen_cart())
    p.add_sprite("gbamenu/cart.spr", cart, pivot=(150, 110))
    p.add_font("gbamenu/font", FONT_TTF, 34)
    p.add_font("gbamenu/font_big", FONT_TTF, 48)
    p.add_sound("gbamenu/sfx_move", synth_blip(rate, 60, 520, 700), rate)
    p.add_sound("gbamenu/sfx_confirm", synth_blip(rate, 160, 440, 880, vol=0.4), rate)
    music = synth_music(rate, 8.0)
    p.add_sound("gbamenu/music", music, rate, loop=(0, len(music)))
    blob = p.build()
    open(out, "wb").write(blob)
    print("wrote %s: %d bytes (imgs=%d sprs=%d fonts=%d snds=%d)" %
          (out, len(blob), len(p.imgs), len(p.sprs), len(p.fonts), len(p.snds)))


if __name__ == "__main__":
    main()
