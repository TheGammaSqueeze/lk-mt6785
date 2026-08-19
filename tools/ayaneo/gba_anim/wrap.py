# Wrap logo_anim.bin with the 512-byte MTK "logo" image header so SP Flash Tool's
# download agent accepts the partition type (else STATUS_SEC_IMG_TYPE_MISMATCH).
# The LK player skips the 512-byte header (AYANEO_ANIM_HDR) when reading.
import struct
h = bytearray(open('logo_mtk_header.bin','rb').read(512))
blob = open('logo_anim.bin','rb').read()
h[4:8] = struct.pack('<I', len(blob))     # patch data-size field
open('logo_anim_wrapped.bin','wb').write(bytes(h)+blob)
print("wrote logo_anim_wrapped.bin (%d bytes) -> flash to 'logo' partition" % (512+len(blob)))
