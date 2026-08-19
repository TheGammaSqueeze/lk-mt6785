#!/bin/bash
#
# Build and sign LK for the AYANEO Pocket Air Mini (MT6785 / k85v1_64).
#
# Produces a signed, flashable image at out/lk_a_signed.img using only files
# contained in this repository (bundled toolchain, keys and cert blobs).
#
# Usage:
#   ./build_ayaneo_pocket_air_mini.sh          # build + sign
#   ./build_ayaneo_pocket_air_mini.sh clean    # remove build output first
#
set -e

PROJECT=k85v1_64
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

if [ "$1" = "clean" ]; then
	echo ">> Cleaning build-$PROJECT and out/"
	rm -rf "build-$PROJECT" out
fi

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building $PROJECT (jobs=$JOBS)"
make "$PROJECT" -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
if [ ! -f "$BUILT" ]; then
	echo "!! build did not produce $BUILT" >&2
	exit 1
fi

mkdir -p out
echo ">> Signing $BUILT"
python3 tools/ayaneo/sign_lk.py "$BUILT" out/lk_a_signed.img

echo
echo ">> Done."
echo "   Unsigned build: $BUILT"
echo "   Signed image:   out/lk_a_signed.img"
echo
echo "   Flash with:  fastboot flash lk_a out/lk_a_signed.img"
echo "   (or use SP Flash Tool). Keep a stock lk image and SP Flash Tool ready"
echo "   for recovery; a bad LK bricks until reflashed."
