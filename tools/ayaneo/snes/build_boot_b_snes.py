#!/usr/bin/env python3
"""
Build the boot_b image for the in-LK SNES Classic menu.

Layout (must match emu/snes/snes_driver.c):
  0x00000000  boot animation blob ("GBA1", shared with the emulator builds)
  0x00400000  SNES asset payload: ["SNSZ"][u32 rawlen][u32 complen][raw-deflate(blob)]

The pack blob (from pack_snes.py) is stored raw-DEFLATE compressed and decompressed
into DRAM at boot by LK's zunzip (inflateInit2(-MAX_WBITS)).

Usage:
  build_boot_b_snes.py <snes_pack.bin> <out_boot_b.img> [--anim FILE]
"""
import sys, os, struct, zlib, argparse

SNES_OFF = 0x00400000
SNES_MAGIC = b"SNSZ"

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_ANIM = os.path.join(HERE, "..", "gba_anim", "logo_anim_mp4.bin")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blob")
    ap.add_argument("out")
    ap.add_argument("--anim", default=DEF_ANIM)
    a = ap.parse_args()

    blob = open(a.blob, "rb").read()
    anim = open(a.anim, "rb").read() if os.path.exists(a.anim) else b""
    if len(anim) > SNES_OFF:
        raise SystemExit("anim blob %d B overlaps SNES payload @0x%x" % (len(anim), SNES_OFF))

    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = co.compress(blob) + co.flush()
    payload = SNES_MAGIC + struct.pack("<II", len(blob), len(comp)) + comp

    total = SNES_OFF + len(payload)
    buf = bytearray(total)
    buf[0:len(anim)] = anim
    buf[SNES_OFF:SNES_OFF + len(payload)] = payload
    open(a.out, "wb").write(buf)

    print("wrote %s" % a.out)
    print("  anim    %8d B @0x%08x" % (len(anim), 0))
    print("  pack    %8d B raw -> %d B deflate @0x%08x" % (len(blob), len(comp), SNES_OFF))
    print("  total   %8d B (%.1f MB)" % (total, total / 1048576.0))


if __name__ == "__main__":
    main()
