# AYANEO Pocket Air Mini signing tools

Self-contained signing for the from-source LK. No external files required.

- `sign_lk.py` - grafts the device cert1/cert2 partitions onto the freshly
  built lk.img, recomputes the cert2 data/header SHA-256 hashes, re-signs each
  cert2 (RSA-PSS, salt 32) and pads to the 2 MB lk_a size. Needs Python 3 and
  the openssl CLI only.
- `keys/` - MTK default test keys (img_prvk/img_pubk). This device verifies LK
  against the MTK default test chain, so these are the correct signing keys.
- `certs/` - the device cert1+cert2 partition blobs (cert1 root is reused
  verbatim; cert2 hashes and signature are recomputed by sign_lk.py).

Run via the top-level `build_ayaneo_pocket_air_mini.sh`, or directly:

    python3 tools/ayaneo/sign_lk.py build-k85v1_64/lk.img out/lk_a_signed.img
