#!/bin/bash
#
# Build the in-LK GBA (gpSP, ARM dynarec) experiment for the AYANEO Pocket Air
# Mini (MT6785 / k85v1_64) and package the flashable artifacts.
#
# Produces:
#   out/lk_a_gba_signed.img  -> flash to lk_a  (the bootloader + emulator)
#   out/gba_boot_b.img       -> flash to boot_b (animation + chime + compressed ROM)
#
# Usage:
#   ./build_ayaneo_gba.sh [clean] [ROM.gba]
#
set -e

PROJECT=k85v1_64
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ROM="${2:-/work/Pokemon Emerald Version.gba}"
[ "$1" = "clean" ] && rm -rf "build-$PROJECT" out emu/gba/obj emu/gba/libgpsp.a
[ -f "$ROM" ] || { echo "!! ROM not found: $ROM" >&2; exit 1; }

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building gpSP core archive (dynarec)"
emu/gba/build_core_gba.sh >/dev/null

echo ">> Building $PROJECT (AYANEO_GBA=yes, jobs=$JOBS)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" AYANEO_GBA=yes -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
[ -f "$BUILT" ] || { echo "!! build did not produce $BUILT" >&2; exit 1; }

mkdir -p out
echo ">> Signing LK"
python3 tools/ayaneo/sign_lk.py "$BUILT" out/lk_a_gba_signed.img

echo ">> Packaging boot_b (animation + chime + compressed ROM)"
python3 tools/ayaneo/gba/build_boot_b_gba.py "$ROM" out/gba_boot_b.img

echo
echo ">> Done."
echo "   Signed LK:  out/lk_a_gba_signed.img   -> fastboot flash lk_a    out/lk_a_gba_signed.img"
echo "   boot_b:     out/gba_boot_b.img        -> fastboot flash boot_b  out/gba_boot_b.img"
