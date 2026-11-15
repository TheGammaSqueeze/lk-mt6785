#!/bin/bash
# Build + sign the Konkr Pocket Block LK (MT8786 on the mt6768 platform): GammaOS Pico from the
# microSD card, and the stock Android 12 without it. The card template comes from
# ./build_konkr_kpa.sh (one card for both consoles).
#
#   ./build_konkr_block.sh [clean]
#
# Output:
#   out/lk_a_block_signed.img   flash to lk_a (2 MB, signed with the MTK test key the board uses)
#
# Nothing here touches a device.
set -e

PROJECT=k68v1_64_block
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

[ "$1" = "clean" ] && rm -rf "build-$PROJECT"

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building $PROJECT (jobs=$JOBS)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
[ -f "$BUILT" ] || { echo "!! build did not produce $BUILT" >&2; exit 1; }
if ! strings "build-$PROJECT/lk" | grep -q "jd9168s_960x720_dsi_vdo_m8695_bw01_drv"; then
	echo "!! the JD9168S panel driver is not in the image" >&2
	exit 1
fi
if ! strings "build-$PROJECT/lk" | grep -q "#BOOTCONFIG"; then
	echo "!! the vendor_boot / bootconfig support is not in the image" >&2
	exit 1
fi

mkdir -p out
echo ">> Signing LK with the Block cert partitions + stock lk_main_dtb"
python3 tools/ayaneo/sign_lk.py --device block "$BUILT" out/lk_a_block_signed.img

echo
echo ">> Done: out/lk_a_block_signed.img"
