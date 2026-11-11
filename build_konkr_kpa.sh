#!/bin/bash
# Build + sign the Konkr Pocket Advance LK (GBA/GB/GBC/SNES/Genesis from the microSD card)
# and build the SD card template (menu pack, cores, boot.cfg).
#
#   ./build_konkr_kpa.sh [clean]
#
# Output:
#   out/lk_a_kpa_signed.img    flash to lk_a (2 MB, signed with the MTK test key the board uses);
#                              the only eMMC partition GammaOS Pico writes
#   out/sdcard/                the SD card template: gammaos/ (boot.cfg, menu.pack), cores/,
#                              bios/, roms/; copy its contents to the root of the card
#
# The cores are git submodules under cores/ (each its own -pico repository), fetched and built by
# tools/ayaneo/gba/make_sdcard.sh as needed; CORE_REPOS=<dir> uses other checkouts instead.
# Nothing here touches a device.
set -e

PROJECT=k85v1_64_kpa
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

[ "$1" = "clean" ] && rm -rf "build-$PROJECT" out

JOBS="$(nproc 2>/dev/null || echo 4)"

echo ">> Building $PROJECT (jobs=$JOBS)"
rm -f "build-$PROJECT/lk" "build-$PROJECT/lk.img"
make "$PROJECT" -j"$JOBS"

BUILT="build-$PROJECT/lk.img"
[ -f "$BUILT" ] || { echo "!! build did not produce $BUILT" >&2; exit 1; }
if [ ! -f "build-$PROJECT/emu/gba/gba_snes_menu.o" ]; then
	echo "!! gba_snes_menu.o was not built - the SD flow did not take effect" >&2
	exit 1
fi
if ! strings "build-$PROJECT/lk" | grep -q "st7123p_hd640_boe_lcm_drv"; then
	echo "!! the ST7123 panel driver is not in the image" >&2
	exit 1
fi

mkdir -p out
echo ">> Signing LK with the Konkr cert partitions + stock lk_main_dtb"
python3 tools/ayaneo/sign_lk.py --device konkr "$BUILT" out/lk_a_kpa_signed.img

tools/ayaneo/gba/make_sdcard.sh || echo "!! LK built and signed; no card template" >&2

echo
echo ">> Done: out/lk_a_kpa_signed.img and out/sdcard/"
