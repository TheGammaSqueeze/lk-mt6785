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

# The ROM-select screen is the REAL SNES-Classic-mini menu; its asset pack is
# built from the (copyright, user-supplied) snes-mini firmware asset tree. Point
# SNES_ASSETS at it (the same dist/assets or web/public tree the web app uses).
SNES_ASSETS="${SNES_ASSETS:-/work/snesmini/snes-mini-emu/web/public/assets}"
if [ -d "$SNES_ASSETS" ]; then
	echo ">> Generating GBA cartridge placeholder"
	python3 tools/ayaneo/gba/gen_gba_cart.py out/gba_cart.png

	echo ">> Packing SNES menu assets (with GBA cart placeholder)"
	python3 tools/ayaneo/snes/pack_snes.py "$SNES_ASSETS" out/snes_pack.bin \
		--rgb565 --gba-cart out/gba_cart.png

	echo ">> Packaging boot_b (animation + chime + SNES pack)"
	python3 tools/ayaneo/gba/build_snes_boot_b.py out/snes_pack.bin out/gba_menu_boot_b.img
else
	echo "!! SNES_ASSETS dir not found: $SNES_ASSETS" >&2
	echo "   set SNES_ASSETS=<snes-mini asset tree> to build the menu boot_b." >&2
	echo "   (LK still built; it falls back to the plain list without the pack.)" >&2
fi

echo
echo ">> Done."
echo "   Signed LK:  out/lk_a_gba_sd_signed.img -> fastboot flash lk_a    out/lk_a_gba_sd_signed.img"
echo "   boot_b:     out/gba_menu_boot_b.img    -> fastboot flash boot_b  out/gba_menu_boot_b.img"
