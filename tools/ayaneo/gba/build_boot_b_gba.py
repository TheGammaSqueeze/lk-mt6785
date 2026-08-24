#!/usr/bin/env python3
"""
Build the boot_b image for the in-LK GBA (gpSP) build.

Layout (must match emu/gba/gba_driver.c and ayaneo_audio.c):
  0x00000000  video animation blob ("GBA1")        (shared with the GBC build)
  0x01000000  audio chime blob ("ABA1")            (shared with the GBC build)
  0x01100000  compressed ROM: "GBAR" u32 rawlen u32 complen  raw-deflate(rom)
  (0x01C00000 save state, 0x01D00000 .sav, 0x01E00000 settings - written at runtime)

The ROM is stored as raw DEFLATE (wbits=-15), which the LK zunzip()
(inflateInit2(-MAX_WBITS)) inflates into the DRAM ROM buffer at boot.

Usage:
  build_boot_b_gba.py <rom.gba> <out_boot_b.img> [--anim FILE] [--audio FILE]
"""
import sys, os, struct, zlib, argparse

AUDIO_OFF = 0x01000000
ROM_OFF   = 0x01100000
ROM_MAGIC = 0x52414247          # "GBAR" little-endian
ROM_COMPMAX = 10 * 1024 * 1024  # must match GBA_ROM_COMPMAX
STATE_OFF = 0x01C00000          # ROM must end before here

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_ANIM  = os.path.join(HERE, "..", "gba_anim", "logo_anim_mp4.bin")
DEF_AUDIO = os.path.join(HERE, "..", "audio", "boot_audio.bin")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--anim", default=DEF_ANIM)
    ap.add_argument("--audio", default=DEF_AUDIO)
    a = ap.parse_args()

    rom = open(a.rom, "rb").read()
    anim = open(a.anim, "rb").read()
    audio = open(a.audio, "rb").read()

    if len(anim) > AUDIO_OFF:
        raise SystemExit("animation blob %d B overlaps audio @0x%x" % (len(anim), AUDIO_OFF))
    if AUDIO_OFF + len(audio) > ROM_OFF:
        raise SystemExit("audio blob %d B overlaps ROM @0x%x" % (len(audio), ROM_OFF))

    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = co.compress(rom) + co.flush()
    if len(comp) > ROM_COMPMAX:
        raise SystemExit("compressed ROM %d B exceeds GBA_ROM_COMPMAX %d B" % (len(comp), ROM_COMPMAX))
    hdr = struct.pack("<III", ROM_MAGIC, len(rom), len(comp))
    if ROM_OFF + len(hdr) + len(comp) > STATE_OFF:
        raise SystemExit("ROM payload overruns the save-state region @0x%x" % STATE_OFF)

    total = ROM_OFF + len(hdr) + len(comp)
    buf = bytearray(total)
    buf[0:len(anim)] = anim
    buf[AUDIO_OFF:AUDIO_OFF + len(audio)] = audio
    buf[ROM_OFF:ROM_OFF + len(hdr)] = hdr
    buf[ROM_OFF + len(hdr):total] = comp
    open(a.out, "wb").write(buf)

    print("wrote %s" % a.out)
    print("  anim  %8d B @0x%08x" % (len(anim), 0))
    print("  audio %8d B @0x%08x" % (len(audio), AUDIO_OFF))
    print("  rom   %8d B raw -> %d B deflate @0x%08x (ends 0x%08x)"
          % (len(rom), len(comp), ROM_OFF, total))
    print("  total %8d B (%.1f MB)" % (total, total / 1048576.0))


if __name__ == "__main__":
    main()
