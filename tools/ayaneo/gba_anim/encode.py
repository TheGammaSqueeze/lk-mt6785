import glob, zlib, struct, numpy as np
from PIL import Image
RES=(640,360)
files=sorted(glob.glob('/tmp/gba60/f_*.png'))     # full 5s content, no baked fade
frames=[np.asarray(Image.open(f).convert('RGB').resize(RES, Image.LANCZOS),dtype=np.uint16) for f in files]
w,h=RES
def to565(a):
    r,g,b=a[...,0],a[...,1],a[...,2]
    return ((r>>3)<<11|(g>>2)<<5|(b>>3)).astype('<u2').tobytes()
blob=bytearray(struct.pack('<4sIHHHH', b'GBA1',1,w,h,len(frames),60)); mx=0
for fr in frames:
    co=zlib.compressobj(9,zlib.DEFLATED,-15); c=co.compress(to565(fr))+co.flush()
    blob+=struct.pack('<I',len(c))+c; mx=max(mx,len(c))
open('logo_anim.bin','wb').write(blob)
print("640x360 60fps content-only frames=%d blob=%.2f MB maxframe=%.0f KB"%(len(frames),len(blob)/1e6,mx/1024))
