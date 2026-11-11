#!/bin/bash
#
# Build + sign the GBA/GB/GBC/SNES/Genesis-from-SD LK for the AYANEO Pocket Air Mini (MT6785 /
# k85v1_64) and build the SD card template. The LK owns only lk_a on the eMMC; the menu pack, the
# cores, the settings and the boot mode (gammaos/boot.cfg) live on the card.
#
# Produces:
#   out/lk_a_gba_sd_signed.img -> flash to lk_a
#   out/sdcard/                -> copy its contents to the root of the card
#
# The cores are git submodules under cores/ (each its own -pico repository), fetched and built by
# tools/ayaneo/gba/make_sdcard.sh as needed; CORE_REPOS=<dir> uses other checkouts instead.
#
# Usage:
#   ./build_ayaneo_gba_sd.sh [clean]
#
set -e

PROJECT=k85v1_64
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

[ "$1" = "clean" ] && rm -rf "build-$PROJECT" out

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building $PROJECT (AYANEO_GBA_SD=yes, jobs=$JOBS)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" AYANEO_GBA_SD=yes -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
[ -f "$BUILT" ] || { echo "!! build did not produce $BUILT" >&2; exit 1; }

# sanity: the SNES-menu driver object must be linked in this variant
if [ ! -f "build-$PROJECT/emu/gba/gba_snes_menu.o" ]; then
	echo "!! gba_snes_menu.o was not built - AYANEO_GBA_SD did not take effect" >&2
	exit 1
fi

mkdir -p out
echo ">> Signing LK"
python3 tools/ayaneo/sign_lk.py "$BUILT" out/lk_a_gba_sd_signed.img

if tools/ayaneo/gba/make_sdcard.sh; then
	# Non-fatal host suite on the fresh pack: every menu state non-blank, audio assets resolve,
	# launch returns the right ROM, and (with the ARM cross gcc + qemu) the NEON blitter matches.
	if command -v gcc >/dev/null 2>&1; then
		echo ">> Validating the pack (host suite: states + audio + launch + NEON)"
		if bash tools/ayaneo/gba/validate_menu.sh out/snes_pack.bin 2>&1 | sed 's/^/   /'; then
			echo "   validation OK"
		else
			echo "!! validation FAILED - check the failing state above / pack_snes.py" >&2
		fi
	fi
else
	echo "!! LK built and signed; no card template" >&2
fi

echo
echo ">> Done."
echo "   Signed LK:     out/lk_a_gba_sd_signed.img -> fastboot flash lk_a out/lk_a_gba_sd_signed.img"
echo "   Card template: out/sdcard/                -> copy to the root of the card"
