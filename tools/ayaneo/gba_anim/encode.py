import glob, zlib, struct, numpy as np
from PIL import Image
RES=(352,198); FADE=12
files=sorted(glob.glob('/tmp/gba_frames/f_*.png'))
frames=[np.asarray(Image.open(f).convert('RGB').resize(RES, Image.LANCZOS), dtype=np.float32) for f in files]
last=frames[-1]
for i in range(1,FADE+1): frames.append(last*(1.0-i/float(FADE)))
w,h=RES
def to565(a):
    a=np.clip(a,0,255).astype(np.uint16); r,g,b=a[...,0],a[...,1],a[...,2]
    return ((r>>3)<<11|(g>>2)<<5|(b>>3)).astype('<u2').tobytes()
blob=bytearray(struct.pack('<4sIHHHH', b'GBA1',1,w,h,len(frames),30))
for fr in frames:
    co=zlib.compressobj(9,zlib.DEFLATED,-15); c=co.compress(to565(fr))+co.flush()
    blob+=struct.pack('<I',len(c))+c
open('logo_anim.bin','wb').write(blob)
print("res %dx%d frames %d blob %.3f MB"%(w,h,len(frames),len(blob)/1e6))
# wrap with MTK logo header (patched size)
hdr=bytearray(open('/work/svoboda_lk/tools/ayaneo/gba_anim/logo_mtk_header.bin','rb').read(512))
hdr[4:8]=struct.pack('<I',len(blob))
open('logo_anim_wrapped.bin','wb').write(bytes(hdr)+blob)
print("wrapped %.3f MB (header+blob)"%((512+len(blob))/1e6))
