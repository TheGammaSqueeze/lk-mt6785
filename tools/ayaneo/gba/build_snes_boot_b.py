#!/usr/bin/env python3
"""
Build the boot_b image for the GBA-from-SD flow using the REAL SNES-mini menu
asset pack (from tools/ayaneo/snes/pack_snes.py). Same base layout as the GBA-SD
boot_b; the SD flow reads the ROM from the SD card, so the boot_b ROM region is
free and holds the SNES pack:
  0x00000000  video animation blob ("GBA1")   - LK boot animation (shared)
  0x01000000  audio chime blob   ("ABA1")      - LK boot chime (shared)
  0x01100000  SNES asset pack: "SNSZ" u32 rawlen u32 complen  raw-deflate(pack)
  0x01C00000  gpSP core blob (core_gba.blob "GCBA") - loaded to DRAM by gba_core_loader.c
              (the old save/state region: saves are on the SD card now, so it is free)

gba_snes_menu.c reads the partition at 0x01100000, inflates it to DRAM, and
snes_pack_open()s it.

Usage: build_snes_boot_b.py <snes_pack.bin> <out_boot_b.img> [--anim F] [--audio F]
"""
import sys, os, struct, zlib, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_ANIM  = os.path.join(HERE, "..", "gba_anim", "logo_anim_mp4.bin")
DEF_AUDIO = os.path.join(HERE, "..", "audio", "boot_audio.bin")
DEF_BLOB  = os.path.join(HERE, "..", "..", "..", "emu", "gba", "core_gba.blob")

AUDIO_OFF = 0x01000000
PACK_OFF  = 0x01100000
STATE_OFF = 0x01C00000
BLOB_OFF  = 0x01C00000            # gpSP core blob (matches GBA_CORE_BLOB_OFF in the loader)
SNES_MAGIC = 0x5A534E53          # "SNSZ" little-endian


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--anim", default=DEF_ANIM)
    ap.add_argument("--audio", default=DEF_AUDIO)
    ap.add_argument("--blob", default=DEF_BLOB)
    a = ap.parse_args()

    pack = open(a.pack, "rb").read()
    anim = open(a.anim, "rb").read() if os.path.exists(a.anim) else b""
    audio = open(a.audio, "rb").read() if os.path.exists(a.audio) else b""
    blob = open(a.blob, "rb").read() if os.path.exists(a.blob) else b""
    if not blob:
        raise SystemExit("core blob %s missing - run emu/gba/build_core_blob.sh first" % a.blob)

    if len(anim) > AUDIO_OFF:
        raise SystemExit("animation blob %d B overlaps audio @0x%x" % (len(anim), AUDIO_OFF))
    if AUDIO_OFF + len(audio) > PACK_OFF:
        raise SystemExit("audio blob %d B overlaps pack @0x%x" % (len(audio), PACK_OFF))

    co = zlib.compressobj(9, zlib.DEFLATED, -15)         # raw deflate (no zlib hdr)
    comp = co.compress(pack) + co.flush()
    hdr = struct.pack("<III", SNES_MAGIC, len(pack), len(comp))
    pack_end = PACK_OFF + len(hdr) + len(comp)
    if pack_end > BLOB_OFF:
        raise SystemExit("SNES pack (%d B deflate) overruns the core blob region "
                         "@0x%x by %d B; downscale art in pack_snes.py" %
                         (len(comp), BLOB_OFF, pack_end - BLOB_OFF))

    end = BLOB_OFF + len(blob)
    buf = bytearray(end)
    buf[0:len(anim)] = anim
    buf[AUDIO_OFF:AUDIO_OFF + len(audio)] = audio
    buf[PACK_OFF:PACK_OFF + len(hdr)] = hdr
    buf[PACK_OFF + len(hdr):pack_end] = comp
    buf[BLOB_OFF:BLOB_OFF + len(blob)] = blob
    open(a.out, "wb").write(buf)

    print("wrote %s (%d B):" % (a.out, end))
    print("  anim  %8d B @0x%08x" % (len(anim), 0))
    print("  audio %8d B @0x%08x" % (len(audio), AUDIO_OFF))
    print("  snes  %8d B raw -> %d B deflate @0x%08x (ends 0x%08x)" %
          (len(pack), len(comp), PACK_OFF, pack_end))
    print("  blob  %8d B @0x%08x (gpSP core)" % (len(blob), BLOB_OFF))


if __name__ == "__main__":
    main()
