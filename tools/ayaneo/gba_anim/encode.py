import glob, zlib, struct, numpy as np
from PIL import Image

FADE_FRAMES = 15          # ~0.5s fade to black at 30fps
files = sorted(glob.glob('/tmp/gba_frames/f_*.png'))
imgs = [np.asarray(Image.open(f).convert('RGB'), dtype=np.float32) for f in files]
h, w = imgs[0].shape[:2]

# append fade-to-black frames from the last rendered frame
last = imgs[-1]
frames = list(imgs)
for i in range(1, FADE_FRAMES+1):
    frames.append(last * (1.0 - i/float(FADE_FRAMES)))

def to565(a):
    a = np.clip(a,0,255).astype(np.uint16)
    r,g,b = a[...,0],a[...,1],a[...,2]
    return ((r>>3)<<11 | (g>>2)<<5 | (b>>3)).astype('<u2').tobytes()

blob = bytearray()
# header: magic, ver, w, h, nframes, fps
n = len(frames)
blob += struct.pack('<4sIHHHH', b'GBA1', 1, w, h, n, 30)
# reserve nothing else; frames stored sequentially as [u32 len][zlib bytes]
for fr in frames:
    co=zlib.compressobj(9, zlib.DEFLATED, -15); c=co.compress(to565(fr))+co.flush()
    blob += struct.pack('<I', len(c)) + c

open('/work/gba_enc/logo_anim.bin','wb').write(blob)
print("frames(incl fade): %d  res %dx%d  blob: %.2f MB (%d bytes)" % (n, w, h, len(blob)/1e6, len(blob)))
