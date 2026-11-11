#!/usr/bin/env python3
"""Build the GammaOS SD card template for the LK (the LK owns only lk_a on the eMMC; everything
else lives on the card).

    build_sdcard.py <menu pack> <out dir> --core NAME=PATH ...

Writes <out dir>/:
    gammaos/boot.cfg     boot=sd (the LK starts the GammaOS menu from this card)
    gammaos/menu.pack    "SNSZ" u32 rawlen u32 complen, then the raw-deflated menu pack
    cores/NAME.blob      each core given with --core (built in its own repository)
    bios/README.txt      where the optional GBA BIOS goes (its boot logo is the intro)
    roms/<system>/       one folder per system the menu lists
Copy the folder's contents to the root of a FAT32 or exFAT card.
"""
import argparse, os, shutil, struct, sys, zlib

SNES_MAGIC = 0x5A534E53                       # "SNSZ" little-endian
RAW_MAX = 0x51800000 - 0x50000000 - 2 * 1024 * 1024   # gba_snes_menu.c SNES_RAW_MAX
COMP_MAX = 8 * 1024 * 1024                    # gba_snes_menu.c SNES_COMP_MAX
# the core blob header: magic, version, entry, load size, total span (core_file.h)
CORE_MAGIC = {
    "armgba-pico": 0x41424743, "gpsp-pico": 0x41424743, "gambatte-pico": 0x31434247,
    "snes9x-pico": 0x31534E53, "genesis-plus-gx-pico": 0x31474553,
}
# the DRAM slot each core loads into (core_file_load's slot_size)
CORE_SLOT = {
    "armgba-pico": 0x400000, "gpsp-pico": 0x400000, "gambatte-pico": 0x800000,
    "snes9x-pico": 0x800000, "genesis-plus-gx-pico": 0x800000,
}
ROM_DIRS = ["gb", "gbc", "gba", "snes", "genesis", "sms", "gg", "sg"]

BOOT_CFG = ("# GammaOS boot mode for this card.\n"
            "#   boot=sd       start the GammaOS menu from this card\n"
            "#   boot=android  boot Android (hold SELECT at power on to come back)\n"
            "boot=sd\n")
BIOS_README = ("Put the Game Boy Advance BIOS here as gba_bios.bin (16384 bytes) for the GBA BIOS boot\n"
               "intro. Without it armGBA's own Open BIOS splash is the intro and GBA games run on\n"
               "armGBA's built-in BIOS. gpSP (when armGBA is not in cores/, or chosen in the game menu)\n"
               "needs this file.\n")


def check_core(name, data):
    if name not in CORE_MAGIC:
        sys.exit("unknown core %s (known: %s)" % (name, ", ".join(sorted(CORE_MAGIC))))
    if len(data) < 20:
        sys.exit("%s: too short for a core" % name)
    magic, ver, entry, load, span = struct.unpack("<5I", data[:20])
    if magic != CORE_MAGIC[name]:
        sys.exit("%s: magic 0x%08x, expected 0x%08x" % (name, magic, CORE_MAGIC[name]))
    # the LK accepts a file up to 16 bytes short of the load size (the dropped end pad)
    if not (20 <= entry < load <= span <= CORE_SLOT[name]) or len(data) + 16 < load:
        sys.exit("%s: header sizes do not fit (entry 0x%x load 0x%x span 0x%x file 0x%x slot 0x%x)" %
                 (name, entry, load, span, len(data), CORE_SLOT[name]))
    return ver


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack", help="the raw menu pack (pack_snes.py output)")
    ap.add_argument("out", help="the card template folder (emptied first)")
    ap.add_argument("--core", action="append", default=[], metavar="NAME=PATH")
    a = ap.parse_args()

    pack = open(a.pack, "rb").read()
    if not 0 < len(pack) <= RAW_MAX:
        sys.exit("menu pack is %d bytes, the menu takes at most %d" % (len(pack), RAW_MAX))
    co = zlib.compressobj(9, zlib.DEFLATED, -15)   # raw deflate (no zlib header)
    comp = co.compress(pack) + co.flush()
    if len(comp) > COMP_MAX:
        sys.exit("menu pack deflates to %d bytes, the menu takes at most %d" % (len(comp), COMP_MAX))

    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    for d in ["gammaos", "cores", "bios"] + ["roms/" + r for r in ROM_DIRS]:
        os.makedirs(os.path.join(a.out, d))
    with open(os.path.join(a.out, "gammaos", "menu.pack"), "wb") as f:
        f.write(struct.pack("<III", SNES_MAGIC, len(pack), len(comp)) + comp)
    with open(os.path.join(a.out, "gammaos", "boot.cfg"), "w", newline="\n") as f:
        f.write(BOOT_CFG)
    with open(os.path.join(a.out, "bios", "README.txt"), "w", newline="\n") as f:
        f.write(BIOS_README)
    for spec in a.core:
        name, _, path = spec.partition("=")
        data = open(path, "rb").read()
        ver = check_core(name, data)
        with open(os.path.join(a.out, "cores", name + ".blob"), "wb") as f:
            f.write(data)
        print("  cores/%s.blob  %d bytes, ABI %d" % (name, len(data), ver))
    print("  gammaos/menu.pack  %d bytes (%d raw)" % (12 + len(comp), len(pack)))


if __name__ == "__main__":
    main()
