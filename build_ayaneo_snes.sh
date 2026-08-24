#!/bin/bash
#
# Build the in-LK SNES/SFC Classic home menu for the AYANEO Pocket Air Mini
# (MT6785 / k85v1_64) and package the flashable artifacts.
#
# Produces:
#   out/lk_a_snes_signed.img  -> flash to lk_a   (release)
#   out/lk_a_snes_debug.img   -> flash to lk_a   (debug logging)
#   out/snes_boot_b.img       -> flash to boot_b (anim + compressed asset pack)
#
# Requires a processed firmware asset dir (see the snes-mini-emu web app tooling);
# the firmware is copyright, so the packed blob is not committed - regenerate it.
#
# Usage:  ./build_ayaneo_snes.sh [ASSET_DIR]
#
set -e
PROJECT=k85v1_64
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
ASSETS="${1:-/work/snesmini/snes-mini-emu/web/public/assets}"
[ -d "$ASSETS" ] || { echo "!! asset dir not found: $ASSETS" >&2; exit 1; }
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p out

echo ">> Packing assets from $ASSETS"
python3 tools/ayaneo/snes/pack_snes.py "$ASSETS" out/snes_pack.bin --rgb565

echo ">> Building $PROJECT release (AYANEO_SNES=yes)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" AYANEO_SNES=yes -j"$JOBS"
python3 tools/ayaneo/sign_lk.py "build-$PROJECT/lk.img" out/lk_a_snes_signed.img

echo ">> Building $PROJECT debug (AYANEO_DEBUG_LOGGING=yes)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" AYANEO_SNES=yes AYANEO_DEBUG_LOGGING=yes -j"$JOBS"
python3 tools/ayaneo/sign_lk.py "build-$PROJECT/lk.img" out/lk_a_snes_debug.img

echo ">> Packaging boot_b (anim + compressed pack)"
python3 tools/ayaneo/snes/build_boot_b_snes.py out/snes_pack.bin out/snes_boot_b.img

echo
echo ">> Done."
echo "   Release LK:  out/lk_a_snes_signed.img  -> fastboot flash lk_a   out/lk_a_snes_signed.img"
echo "   Debug LK:    out/lk_a_snes_debug.img   -> fastboot flash lk_a   out/lk_a_snes_debug.img"
echo "   boot_b:      out/snes_boot_b.img       -> fastboot flash boot_b out/snes_boot_b.img"
