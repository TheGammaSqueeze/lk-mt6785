import glob, zlib, struct, numpy as np
from PIL import Image
FADE=24                       # ~0.4s fade at 60fps
files=sorted(glob.glob('/tmp/gba60/f_*.png'))
content=files[:240]           # first 4s of the 5s (fade starts 1s earlier)
frames=[np.asarray(Image.open(f).convert('RGB'),dtype=np.float32) for f in content]
last=frames[-1]
for i in range(1,FADE+1): frames.append(last*(1.0-i/float(FADE)))
h,w=frames[0].shape[:2]
def to565(a):
    a=np.clip(a,0,255).astype(np.uint16); r,g,b=a[...,0],a[...,1],a[...,2]
    return ((r>>3)<<11|(g>>2)<<5|(b>>3)).astype('<u2').tobytes()
blob=bytearray(struct.pack('<4sIHHHH', b'GBA1',1,w,h,len(frames),60))
mx=0
for fr in frames:
    co=zlib.compressobj(9,zlib.DEFLATED,-15); c=co.compress(to565(fr))+co.flush()
    blob+=struct.pack('<I',len(c))+c; mx=max(mx,len(c))
open('logo_anim.bin','wb').write(blob)
print("1280x720 60fps frames=%d blob=%.2f MB maxframe=%.0f KB"%(len(frames),len(blob)/1e6,mx/1024))
