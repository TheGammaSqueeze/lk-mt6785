#!/usr/bin/env python3
# Render a preview video of the GBA menu + the live punch-hole launch transition,
# HOST-SIMULATED (the device is usually unreachable). It uses the real menu frames
# from host_render and reproduces gba_punch_composite's exact geometry (game inside a
# growing circle over the frozen menu snapshot). The "game underneath" is a mock
# animated scene (no real ROM here) so the reveal reads as live gameplay.
#
#   python3 tools/ayaneo/gba/make_demo_video.py out/snes_pack.bin /mnt/c/pairmini/gba_menu_demo.mp4
import subprocess, sys, os, tempfile
import numpy as np
from PIL import Image

PACK = sys.argv[1] if len(sys.argv) > 1 else "out/snes_pack.bin"
OUT  = sys.argv[2] if len(sys.argv) > 2 else "/tmp/gba_menu_demo.mp4"
HR   = "emu/gba/menu/host_render"
W, H = 1280, 960
# GBA game placement, matching gba_punch.h / ayaneo_gbc_show_frame (S=5, centred).
S, SRCW, SRCH = 5, 240, 160
XOFF, YOFF = (W - SRCW*S)//2, (H - SRCH*S)//2      # 40, 80
CX, CY = W//2, H//2
MAXR = 820
PUNCH_FRAMES = 48
FPS = 30

def read_ppm(path):
    d = open(path, "rb").read()
    i = 0
    for _ in range(3):
        i = d.index(b"\n", i) + 1
    return np.frombuffer(d[i:], np.uint8).reshape(H, W, 3).copy()

def menu_frame(nav):
    """render one settled menu state via host_render (GBA roster of 6)."""
    ppm = tempfile.mktemp(suffix=".ppm")
    env = dict(os.environ, GBA_ROSTER="6")
    subprocess.run([HR, PACK, ppm, "40", nav], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    f = read_ppm(ppm); os.unlink(ppm); return f

def mock_game(i):
    """a vivid animated 240x160 scene so the reveal clearly reads as live gameplay:
    scrolling striped sky, a ground line, and a bouncing player square."""
    g = np.zeros((SRCH, SRCW, 3), np.uint8)
    yy = np.arange(SRCH)[:, None]
    g[..., 0] = np.clip(40 + yy*0.4, 0, 120).astype(np.uint8)          # sky R
    g[..., 1] = np.clip(120 + yy*0.3, 0, 200).astype(np.uint8)         # sky G
    g[..., 2] = np.clip(220 - yy*0.6, 40, 255).astype(np.uint8)        # sky B
    xs = (np.arange(SRCW) + i*4) % 48                                   # scrolling stripes
    g[:110][:, xs < 24] = (g[:110][:, xs < 24] * 0.82).astype(np.uint8)
    g[112:] = (60, 170, 70)                                             # ground
    g[110:112] = (30, 110, 40)
    px = 30 + (i*6) % (SRCW-60)                                         # player x
    py = 92 - int(abs(((i % 24) - 12)) * 1.2)                           # bounce
    g[py:py+16, px:px+16] = (250, 220, 60)                             # player
    g[py+4:py+8, px+3:px+7] = (30, 30, 30)                             # eye
    return g

def game_full(i):
    """upscale the mock game to 1200x800 on a black 1280x960 canvas (letterbox)."""
    canvas = np.zeros((H, W, 3), np.uint8)
    up = np.asarray(Image.fromarray(mock_game(i)).resize((SRCW*S, SRCH*S), Image.NEAREST))
    canvas[YOFF:YOFF+SRCH*S, XOFF:XOFF+SRCW*S] = up
    return canvas

# precompute the circle distance grid
Y, X = np.ogrid[:H, :W]
DIST2 = (X - CX)**2 + (Y - CY)**2

def composite(snap, gi, radius):
    mask = DIST2 <= radius*radius
    return np.where(mask[..., None], game_full(gi), snap)

def label_frame(text):
    from PIL import ImageDraw, ImageFont
    img = Image.new("RGB", (W, H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 64)
    except Exception:
        font = ImageFont.load_default()
    tw = d.textlength(text, font=font)
    d.text(((W - tw) / 2, H / 2 - 40), text, fill=(235, 235, 235), font=font)
    return np.asarray(img)

def main():
    if not os.path.exists(HR):
        subprocess.run(["bash", "emu/gba/menu/build_host.sh"], check=True)
    frames = []
    # 1) menu: browse the carousel a few games, hold each briefly
    navs = ["", "R", "RR", "RRR", "RRR", "RRR"]
    for k, nav in enumerate(navs):
        f = menu_frame(nav)
        hold = 18 if k in (0, len(navs)-1) else 8
        frames += [f]*hold
    snap = frames[-1]                          # the launch frame = punch snapshot
    # 2) SNAPPY transition at real speed: on device it is time-paced to ~180ms, so at
    #    30fps that is ~6 steps (radius linear in time). This is the true feel.
    SNAPPY_N = 6
    for i in range(SNAPPY_N):
        r = MAXR * (i+1) // SNAPPY_N
        frames.append(composite(snap, i, r))
    for i in range(SNAPPY_N, SNAPPY_N+24):     # ~0.8s of the full game
        frames.append(game_full(i))
    # 3) slow-motion replay so the 180ms transition is actually watchable
    frames += [label_frame("SLOW MOTION REPLAY")]*18
    SLOW_N = 40
    for i in range(SLOW_N):
        r = MAXR * (i+1) // SLOW_N
        frames.append(composite(snap, i//4, r))
    for i in range(SLOW_N, SLOW_N+16):
        frames.append(game_full(i))

    tmp = tempfile.mkdtemp()
    for n, f in enumerate(frames):
        Image.fromarray(f).save("%s/f%04d.png" % (tmp, n))
    subprocess.run(["ffmpeg", "-y", "-framerate", str(FPS), "-i", "%s/f%%04d.png" % tmp,
                    "-vf", "scale=640:480", "-pix_fmt", "yuv420p", "-crf", "23", OUT],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote %s (%d frames, %.1fs)" % (OUT, len(frames), len(frames)/FPS))

if __name__ == "__main__":
    main()
