#!/usr/bin/env python3
"""
GammaOS secure-firmware CPC-arming patcher for MediaTek tee.img (BL31).

Background
----------
On these MediaTek parts the BL31 (ARM Trusted Firmware, carried inside the GFH
"atf" partition of tee.img) does not arm the CPU power controller (SPMC/CPC) at
cold boot. It arms it only when the non-secure kernel later fires the arming
SMC, and that same SMC hands control to the kernel and never returns to the
bootloader. So a bootloader that wants to power a second CPU core with PSCI
CPU_ON finds the controller unarmed and the request hangs.

The firmware already contains a one-shot CPC-arm routine (the wrapper the kernel
SMC calls). It is self-gated by a flag byte: the first caller arms the
controller and sets the flag, every later caller just returns. This tool makes
the cold-boot core call that wrapper once, by overwriting a single droppable
log-print branch in bl31_main with a branch-and-link to the wrapper. Because the
wrapper is one-shot, the later real kernel-boot SMC becomes a no-op, so a normal
Android/Linux boot is unaffected.

After the 4-byte code edit, the "atf" data no longer matches the SHA-256 stored
in its cert2 (X.509, MTK custom OIDs), so this tool recomputes cert2's data and
header hashes and re-signs its TBS with the image key the preloader verifies
against (RSA-PSS, SHA-256, salt 32). On a test-key / unlocked device the default
MTK test key under tools/ayaneo/keys works; a device fused to a production key
needs that key instead.

This is first-party GammaOS tooling. The cert re-sign mirrors our own LK signer
(tools/ayaneo/sign_lk.py). Only Python 3 and the openssl CLI are needed.

Per-device addresses
--------------------
The patch-site and wrapper addresses differ per firmware build, so they live in
a profile (see profiles/konkr_kpa.json and the README for how to find them with
a disassembler). Point --profile at the matching one, or pass the four numbers
on the command line.

Usage
-----
  gammaos_cpc_patch.py --profile profiles/konkr_kpa.json \
      --in stock_tee.img --out out_dir/

Writes into out_dir/ (names from the profile's "name"):
  <name>_stock.img    verbatim copy of the input (your rollback image)
  <name>_control.img  re-signed only, no code change (flash this first)
  <name>_armcpc.img   the 4-byte patch + re-sign (flash after the control boots)

SAFETY: flashing tee is brick-risky; the only recovery is the SoC download tool
(e.g. SP Flash Tool). Keep <name>_stock.img. Flash the control image first and
confirm the device still boots (that proves the preloader accepts the re-sign),
then flash the armcpc image. See README.md.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_KEY = os.path.join(HERE, "..", "..", "ayaneo", "keys", "img_prvk.pem")
DEFAULT_PUB = os.path.join(HERE, "..", "..", "ayaneo", "keys", "img_pubk.pem")

GFH_MAGIC = bytes.fromhex("88168858")   # little-endian GFH partition magic
PART_HEADER = 512                        # partition header bytes before the data
ALIGN = 16

# MTK custom OIDs that carry the image hashes inside cert2 (DER-encoded prefix).
OID_DATA_HASH = bytes.fromhex("060760867693160201")
OID_HEADER_HASH = bytes.fromhex("060760867693160204")


def align_up(x, a=ALIGN):
    return (x + a - 1) // a * a


def bl_encode(site, target):
    """AArch64 BL = 0x94000000 | ((target-site)>>2 & 0x03FFFFFF)."""
    return (0x94000000 | (((target - site) >> 2) & 0x03FFFFFF)) & 0xFFFFFFFF


# ---- minimal DER walker ----------------------------------------------------
def der_len(buf, off):
    first = buf[off]
    if first < 0x80:
        return first, 1
    n = first & 0x7F
    val = 0
    for i in range(n):
        val = (val << 8) | buf[off + 1 + i]
    return val, 1 + n


def oid_hash_slot(cert, oid):
    pos = cert.find(oid)
    if pos < 0:
        raise ValueError("cert2 OID %s not present" % oid.hex())
    bs = pos + len(oid)
    if cert[bs] != 0x03:
        raise ValueError("expected BIT STRING after hash OID")
    _, lb = der_len(cert, bs + 1)
    return bs + 1 + lb + 1          # skip tag, length, one unused-bits byte


def tbs_span(cert):
    assert cert[0] == 0x30
    _, olb = der_len(cert, 1)
    start = 1 + olb
    assert cert[start] == 0x30
    tlen, tlb = der_len(cert, start + 1)
    return start, 1 + tlb + tlen


def sig_slot(cert):
    assert cert[0] == 0x30
    _, olb = der_len(cert, 1)
    pos = 1 + olb
    assert cert[pos] == 0x30                 # tbsCertificate
    tlen, tlb = der_len(cert, pos + 1)
    pos += 1 + tlb + tlen
    assert cert[pos] == 0x30                 # signatureAlgorithm
    slen, slb = der_len(cert, pos + 1)
    pos += 1 + slb + slen
    assert cert[pos] == 0x03                 # signatureValue BIT STRING
    blen, blb = der_len(cert, pos + 1)
    return pos + 1 + blb + 1, blen - 1


# ---- openssl RSA-PSS SHA-256 (salt 32) -------------------------------------
def _openssl(args, data):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        path = f.name
    try:
        return subprocess.run(args + [path], capture_output=True), path
    finally:
        pass


def pss_sign(tbs, key):
    with tempfile.NamedTemporaryFile(suffix=".tbs", delete=False) as f:
        f.write(tbs)
        tbs_path = f.name
    sig_path = tbs_path + ".sig"
    try:
        subprocess.run(
            ["openssl", "dgst", "-sha256",
             "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32",
             "-sign", key, "-out", sig_path, tbs_path],
            check=True, capture_output=True)
        return open(sig_path, "rb").read()
    finally:
        for p in (tbs_path, sig_path):
            try:
                os.unlink(p)
            except OSError:
                pass


def pss_verify(tbs, sig, pub):
    with tempfile.NamedTemporaryFile(suffix=".tbs", delete=False) as f:
        f.write(tbs)
        tbs_path = f.name
    with tempfile.NamedTemporaryFile(suffix=".sig", delete=False) as f:
        f.write(sig)
        sig_path = f.name
    try:
        r = subprocess.run(
            ["openssl", "dgst", "-sha256",
             "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32",
             "-verify", pub, "-signature", sig_path, tbs_path],
            capture_output=True)
        return r.returncode == 0
    finally:
        for p in (tbs_path, sig_path):
            try:
                os.unlink(p)
            except OSError:
                pass


# ---- GFH partitions + re-sign ----------------------------------------------
def scan_parts(img):
    parts = []
    i = 0
    while True:
        j = img.find(GFH_MAGIC, i)
        if j < 0:
            break
        name = img[j + 8:j + 24].split(b"\x00")[0].decode("latin1")
        isz = struct.unpack("<I", img[j + 4:j + 8])[0]
        parts.append((name, j, isz))
        i = j + 4
    return parts


def resign(img, key, pub):
    """Recompute and re-sign the cert2 that protects the preceding code part."""
    img = bytearray(img)
    last_code = None
    for name, off, isz in scan_parts(img):
        if name == "cert1":
            continue
        if name == "cert2":
            code_off, code_sz = last_code
            header = bytes(img[code_off:code_off + PART_HEADER])
            data = bytes(img[code_off + PART_HEADER:code_off + PART_HEADER + code_sz])
            data = data + b"\x00" * (align_up(len(data)) - len(data))
            cert = bytearray(img[off + PART_HEADER:off + PART_HEADER + isz])
            ds = oid_hash_slot(cert, OID_DATA_HASH)      # explicit slice: cert[s:][:32] writes a copy
            hs = oid_hash_slot(cert, OID_HEADER_HASH)
            cert[ds:ds + 32] = hashlib.sha256(data).digest()
            cert[hs:hs + 32] = hashlib.sha256(header).digest()
            ts, tl = tbs_span(cert)
            sig = pss_sign(bytes(cert[ts:ts + tl]), key)
            if not pss_verify(bytes(cert[ts:ts + tl]), sig, pub):
                raise SystemExit("cert2 re-sign did not verify under the public key")
            so, sl = sig_slot(cert)
            if len(sig) != sl:
                raise SystemExit("re-signed signature length %d != slot %d" % (len(sig), sl))
            cert[so:so + sl] = sig
            img[off + PART_HEADER:off + PART_HEADER + isz] = cert
        else:
            last_code = (off, isz)
    return bytes(img)


def apply_patch(img, prof):
    img = bytearray(img)
    foff = prof["patch_site"] + prof["payload_base"]
    cur = struct.unpack("<I", img[foff:foff + 4])[0]
    if cur != prof["old_insn"]:
        raise SystemExit("patch site 0x%x holds 0x%08x, expected 0x%08x - wrong "
                         "profile for this firmware?" % (foff, cur, prof["old_insn"]))
    new = bl_encode(prof["patch_site"], prof["wrapper"])
    img[foff:foff + 4] = struct.pack("<I", new)
    return bytes(img), foff, cur, new


def main():
    ap = argparse.ArgumentParser(description="Arm the MediaTek CPC at BL31 cold boot (Android-safe).")
    ap.add_argument("--in", dest="inp", required=True, help="stock tee.img")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--profile", help="device profile JSON (payload_base, patch_site, wrapper, old_insn, name)")
    ap.add_argument("--payload-base", type=lambda s: int(s, 0))
    ap.add_argument("--patch-site", type=lambda s: int(s, 0))
    ap.add_argument("--wrapper", type=lambda s: int(s, 0))
    ap.add_argument("--old-insn", type=lambda s: int(s, 0))
    ap.add_argument("--name", default="tee")
    ap.add_argument("--key", default=DEFAULT_KEY)
    ap.add_argument("--pub", default=DEFAULT_PUB)
    a = ap.parse_args()

    if a.profile:
        prof = json.load(open(a.profile))
        for k in ("payload_base", "patch_site", "wrapper", "old_insn"):
            if isinstance(prof.get(k), str):
                prof[k] = int(prof[k], 0)   # allow "0x..." hex strings in the profile
    else:
        for k in ("payload_base", "patch_site", "wrapper", "old_insn"):
            if getattr(a, k.replace("payload_base", "payload_base")) is None and getattr(a, k, None) is None:
                ap.error("need --profile or all of --payload-base --patch-site --wrapper --old-insn")
        prof = {"payload_base": a.payload_base, "patch_site": a.patch_site,
                "wrapper": a.wrapper, "old_insn": a.old_insn, "name": a.name}
    name = prof.get("name", a.name)
    os.makedirs(a.out, exist_ok=True)
    stock = open(a.inp, "rb").read()

    so = os.path.join(a.out, name + "_stock.img")
    co = os.path.join(a.out, name + "_control.img")
    ao = os.path.join(a.out, name + "_armcpc.img")

    open(so, "wb").write(stock)
    print("stock copy  -> %s (%d bytes, keep this as your rollback)" % (so, len(stock)))
    open(co, "wb").write(resign(stock, a.key, a.pub))
    print("resign only -> %s (flash FIRST, must still boot)" % co)
    patched, foff, old, new = apply_patch(stock, prof)
    print("code patch  file 0x%x: 0x%08x -> 0x%08x (bl -> wrapper 0x%x)"
          % (foff, old, new, prof["wrapper"]))
    open(ao, "wb").write(resign(patched, a.key, a.pub))
    print("patch+sign  -> %s (flash after the control boots)" % ao)


if __name__ == "__main__":
    main()
