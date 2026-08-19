#!/usr/bin/env python3
"""
Self-contained LK signer for the AYANEO Pocket Air Mini (MT6785 / k85v1_64).

Takes the freshly built, unsigned lk.img (two partitions: lk + lk_main_dtb),
grafts the device cert1/cert2 partitions after each code block, recomputes the
cert2 data and header SHA-256 hashes, re-signs each cert2 TBS with the MTK
default test image key (RSA-PSS, salt length 32), and pads to the 2 MB lk_a
partition size.

Everything it needs ships in this directory:
  keys/img_prvk.pem, keys/img_pubk.pem   - MTK default test keys
  certs/lk_cert.bin, certs/dtb_cert.bin  - device cert1+cert2 partition blobs

Only depends on Python 3 and the openssl CLI.

Usage:
  tools/ayaneo/sign_lk.py <in_lk.img> <out_signed.img>
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
KEY = os.path.join(HERE, "keys", "img_prvk.pem")
PUBKEY = os.path.join(HERE, "keys", "img_pubk.pem")
LK_CERT = os.path.join(HERE, "certs", "lk_cert.bin")
DTB_CERT = os.path.join(HERE, "certs", "dtb_cert.bin")

MAGIC = bytes.fromhex("88168858")
ALIGN = 16
PART_SIZE = 2 * 1024 * 1024  # lk_a = 0x200000

# MTK custom OIDs carrying the image hashes inside cert2 (DER encoded).
OID_DATA_HASH = bytes.fromhex("060760867693160201")    # 2.16.886.2454.2.1
OID_HEADER_HASH = bytes.fromhex("060760867693160204")  # 2.16.886.2454.2.4


def align_up(x, a=ALIGN):
    return (x + a - 1) // a * a


def parse_len(data, off):
    first = data[off]
    if first < 0x80:
        return first, 1
    n = first & 0x7F
    val = 0
    for i in range(n):
        val = (val << 8) | data[off + 1 + i]
    return val, 1 + n


def find_hash_offset(cert, oid):
    idx = cert.find(oid)
    if idx == -1:
        raise ValueError("OID %s not found in cert2" % oid.hex())
    vs = idx + len(oid)
    if cert[vs] != 0x03:
        raise ValueError("expected BIT STRING after OID")
    length, lb = parse_len(cert, vs + 1)
    return vs + 1 + lb + 1  # tag + len + unused-bits byte -> 32-byte hash


def find_tbs(cert):
    assert cert[0] == 0x30
    _, olb = parse_len(cert, 1)
    tbs_start = 1 + olb
    assert cert[tbs_start] == 0x30
    tlen, tlb = parse_len(cert, tbs_start + 1)
    return tbs_start, 1 + tlb + tlen


def find_sig_offset(cert):
    assert cert[0] == 0x30
    _, olb = parse_len(cert, 1)
    pos = 1 + olb
    assert cert[pos] == 0x30
    tlen, tlb = parse_len(cert, pos + 1)
    pos += 1 + tlb + tlen
    assert cert[pos] == 0x30  # sigAlg
    slen, slb = parse_len(cert, pos + 1)
    pos += 1 + slb + slen
    assert cert[pos] == 0x03  # signature BIT STRING
    blen, blb = parse_len(cert, pos + 1)
    return pos + 1 + blb + 1, blen - 1


def sign_tbs(tbs):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(tbs)
        tbs_path = f.name
    sig_path = tbs_path + ".sig"
    try:
        subprocess.run(
            ["openssl", "dgst", "-sha256",
             "-sigopt", "rsa_padding_mode:pss",
             "-sigopt", "rsa_pss_saltlen:-1",
             "-sign", KEY, "-out", sig_path, tbs_path],
            check=True, capture_output=True)
        with open(sig_path, "rb") as f:
            return f.read()
    finally:
        for p in (tbs_path, sig_path):
            try:
                os.unlink(p)
            except OSError:
                pass


def verify(tbs, sig):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(tbs)
        tbs_path = f.name
    with tempfile.NamedTemporaryFile(suffix=".sig", delete=False) as f:
        f.write(sig)
        sig_path = f.name
    try:
        r = subprocess.run(
            ["openssl", "dgst", "-sha256",
             "-sigopt", "rsa_padding_mode:pss",
             "-sigopt", "rsa_pss_saltlen:-1",
             "-verify", PUBKEY, "-signature", sig_path, tbs_path],
            capture_output=True)
        return r.returncode == 0
    finally:
        for p in (tbs_path, sig_path):
            try:
                os.unlink(p)
            except OSError:
                pass


def scan(data):
    parts = []
    i = 0
    while True:
        j = data.find(MAGIC, i)
        if j < 0:
            break
        name = data[j + 8:j + 24].split(b"\x00")[0].decode()
        isz = struct.unpack("<I", data[j + 4:j + 8])[0]
        parts.append((name, j, isz))
        i = j + 4
    return parts


def graft(built):
    parts = scan(built)
    names = [p[0] for p in parts]
    if names != ["lk", "lk_main_dtb"]:
        raise SystemExit("unexpected partitions in build: %s" % names)
    (_, lk_off, lk_sz), (_, dtb_off, dtb_sz) = parts
    lk_end = lk_off + 512 + align_up(lk_sz)
    dtb_end = dtb_off + 512 + align_up(dtb_sz)
    lk_cert = open(LK_CERT, "rb").read()
    dtb_cert = open(DTB_CERT, "rb").read()
    return built[:lk_end] + lk_cert + built[lk_end:dtb_end] + dtb_cert


def resign(img):
    img = bytearray(img)
    parts = scan(img)
    # pair each cert2 with the preceding code partition
    last_code = None
    for name, off, isz in parts:
        if name == "cert1":
            continue
        if name == "cert2":
            code_off, code_sz = last_code
            header = bytes(img[code_off:code_off + 512])
            data = bytes(img[code_off + 512:code_off + 512 + code_sz])
            data = data + b"\x00" * (align_up(len(data)) - len(data))
            data_hash = hashlib.sha256(data).digest()
            header_hash = hashlib.sha256(header).digest()
            cert = bytearray(img[off + 512:off + 512 + isz])
            d_off = find_hash_offset(cert, OID_DATA_HASH)
            h_off = find_hash_offset(cert, OID_HEADER_HASH)
            cert[d_off:d_off + 32] = data_hash
            cert[h_off:h_off + 32] = header_hash
            tbs_off, tbs_len = find_tbs(cert)
            sig = sign_tbs(bytes(cert[tbs_off:tbs_off + tbs_len]))
            if len(sig) != 256 or not verify(bytes(cert[tbs_off:tbs_off + tbs_len]), sig):
                raise SystemExit("signature verify failed")
            s_off, s_len = find_sig_offset(cert)
            cert[s_off:s_off + s_len] = sig
            img[off + 512:off + 512 + isz] = cert
            print("  signed cert2 for '%s': data+header hash + signature OK" %
                  (["lk", "lk_main_dtb"][0 if code_off == parts[0][1] else 1]))
        else:
            last_code = (off, isz)
    return bytes(img)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    built = open(sys.argv[1], "rb").read()
    print("Grafting device cert partitions...")
    img = graft(built)
    print("Re-signing cert2 chains...")
    img = resign(img)
    if len(img) > PART_SIZE:
        raise SystemExit("signed image %d exceeds lk_a size %d" % (len(img), PART_SIZE))
    img = img + b"\x00" * (PART_SIZE - len(img))
    with open(sys.argv[2], "wb") as f:
        f.write(img)
    print("Wrote signed image: %s (%d bytes)" % (sys.argv[2], len(img)))


if __name__ == "__main__":
    main()
