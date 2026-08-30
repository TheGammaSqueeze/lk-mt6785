#!/bin/bash
#
# Build the GBA-from-SD-card LK for the AYANEO Pocket Air Mini (MT6785 /
# k85v1_64) and package the flashable artifacts. This is the SD+menu variant of
# the branch lk-gba-emu-sd-card: ROMs/BIOS/saves come from the microSD and the
# ROM-select screen is the SNES-Classic-mini-style carousel (emu/gba/gba_menu.c).
#
# IMPORTANT: this passes AYANEO_GBA_SD=yes (which implies AYANEO_GBA and defines
# AYANEO_GBA_SD). The plain build_ayaneo_gba.sh only sets AYANEO_GBA=yes and does
# NOT link the SD flow or the carousel menu - use THIS script for this branch.
#
# Produces:
#   out/lk_a_gba_sd_signed.img -> flash to lk_a   (bootloader + emulator + menu)
#   out/gba_menu_boot_b.img    -> flash to boot_b (anim + chime + menu asset pack)
#
# Usage:
#   ./build_ayaneo_gba_sd.sh [clean]
#
set -e

PROJECT=k85v1_64
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

[ "$1" = "clean" ] && rm -rf "build-$PROJECT" out emu/gba/obj emu/gba/libgpsp.a

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building gpSP core archive (dynarec)"
emu/gba/build_core_gba.sh >/dev/null

echo ">> Building $PROJECT (AYANEO_GBA_SD=yes, jobs=$JOBS)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" AYANEO_GBA_SD=yes -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
[ -f "$BUILT" ] || { echo "!! build did not produce $BUILT" >&2; exit 1; }

# sanity: the carousel object must be linked in this variant
if [ ! -f "build-$PROJECT/emu/gba/gba_menu.o" ]; then
	echo "!! gba_menu.o was not built - AYANEO_GBA_SD did not take effect" >&2
	exit 1
fi

mkdir -p out
echo ">> Signing LK"
python3 tools/ayaneo/sign_lk.py "$BUILT" out/lk_a_gba_sd_signed.img

echo ">> Packaging menu asset pack"
python3 tools/ayaneo/gba/build_menu_pack.py out/gbamenu.pack

echo ">> Packaging boot_b (animation + chime + menu pack)"
python3 tools/ayaneo/gba/build_menu_boot_b.py out/gbamenu.pack out/gba_menu_boot_b.img

echo
echo ">> Done."
echo "   Signed LK:  out/lk_a_gba_sd_signed.img -> fastboot flash lk_a    out/lk_a_gba_sd_signed.img"
echo "   boot_b:     out/gba_menu_boot_b.img    -> fastboot flash boot_b  out/gba_menu_boot_b.img"
