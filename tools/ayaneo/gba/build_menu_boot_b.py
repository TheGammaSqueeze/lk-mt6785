#!/usr/bin/env python3
"""
Build the boot_b image for the GBA-from-SD carousel menu.

Layout (same base as build_boot_b_gba.py; the SD flow reads the ROM from the SD
card, so the boot_b ROM region is free and holds the menu asset pack):
  0x00000000  video animation blob ("GBA1")   - LK boot animation (shared)
  0x01000000  audio chime blob   ("ABA1")      - LK boot chime (shared)
  0x01100000  menu asset pack: "GMNU" u32 rawlen u32 complen  raw-deflate(pack)
  (0x01C00000+ save/state/settings written at runtime - unchanged)

The pack (from build_menu_pack.py) is stored raw-DEFLATE compressed; LK reads the
partition at MENU_OFF, inflates it, and snes_pack_open()s it in place.

Usage: build_menu_boot_b.py <menu.pack> <out_boot_b.img> [--anim F] [--audio F]
"""
import sys, os, struct, zlib, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_ANIM  = os.path.join(HERE, "..", "gba_anim", "logo_anim_mp4.bin")
DEF_AUDIO = os.path.join(HERE, "..", "audio", "boot_audio.bin")

AUDIO_OFF = 0x01000000
MENU_OFF  = 0x01100000
STATE_OFF = 0x01C00000
MENU_MAGIC = 0x554E4D47          # "GMNU" little-endian


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("out")
    ap.add_argument("--anim", default=DEF_ANIM)
    ap.add_argument("--audio", default=DEF_AUDIO)
    a = ap.parse_args()

    pack = open(a.pack, "rb").read()
    anim = open(a.anim, "rb").read() if os.path.exists(a.anim) else b""
    audio = open(a.audio, "rb").read() if os.path.exists(a.audio) else b""

    if len(anim) > AUDIO_OFF:
        raise SystemExit("animation blob %d B overlaps audio @0x%x" % (len(anim), AUDIO_OFF))
    if AUDIO_OFF + len(audio) > MENU_OFF:
        raise SystemExit("audio blob %d B overlaps menu @0x%x" % (len(audio), MENU_OFF))

    co = zlib.compressobj(9, zlib.DEFLATED, -15)         # raw deflate (no zlib hdr)
    comp = co.compress(pack) + co.flush()
    hdr = struct.pack("<III", MENU_MAGIC, len(pack), len(comp))
    if MENU_OFF + len(hdr) + len(comp) > STATE_OFF:
        raise SystemExit("menu pack overruns the save-state region @0x%x" % STATE_OFF)

    total = MENU_OFF + len(hdr) + len(comp)
    buf = bytearray(total)
    buf[0:len(anim)] = anim
    buf[AUDIO_OFF:AUDIO_OFF + len(audio)] = audio
    buf[MENU_OFF:MENU_OFF + len(hdr)] = hdr
    buf[MENU_OFF + len(hdr):MENU_OFF + len(hdr) + len(comp)] = comp
    open(a.out, "wb").write(buf)

    print("wrote %s (%d B):" % (a.out, total))
    print("  anim  %8d B @0x%08x" % (len(anim), 0))
    print("  audio %8d B @0x%08x" % (len(audio), AUDIO_OFF))
    print("  menu  %8d B raw -> %d B deflate @0x%08x" % (len(pack), len(comp), MENU_OFF))


if __name__ == "__main__":
    main()
