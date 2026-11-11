#!/bin/bash
# Build + sign the Konkr Pocket Advance LK (GBA/GB/GBC/SNES/Genesis from the microSD card)
# and pack the menu assets into a boot_b image.
#
#   ./build_konkr_kpa.sh [clean]
#
# Output:
#   out/lk_a_kpa_signed.img    flash to lk_a (2 MB, signed with the MTK test key the board uses);
#                              the only eMMC partition GammaOS writes
#   out/sdcard/                the SD card template: gammaos/ (boot.cfg, menu.pack), cores/,
#                              bios/, roms/; copy its contents to the root of the card
#
# The cores are built in their own repositories, next to this tree by default (CORE_REPOS, default
# the directory that holds the kpa folder): armGBA-pico, gpsp-pico, gambatte-pico, snes9x-pico,
# genesis-plus-gx-pico. A core whose blob is missing is built with its build.sh. Nothing here
# touches a device.
set -e

PROJECT=k85v1_64_kpa
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

[ "$1" = "clean" ] && rm -rf "build-$PROJECT" out emu/gba/obj emu/gba/libgpsp.a

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

SNES_ASSETS="${SNES_ASSETS:-$ROOT/tools/ayaneo/snes/assets}"
if [ -d "$SNES_ASSETS" ]; then
	echo ">> Generating GBA cartridge placeholder"
	python3 tools/ayaneo/gba/gen_gba_cart.py out/gba_cart.png

	echo ">> Packing menu assets"
	python3 tools/ayaneo/snes/pack_snes.py "$SNES_ASSETS" out/snes_pack.bin \
		--rgb565 --gba-cart out/gba_cart.png \
		--logo-gb      tools/ayaneo/snes/logos/gb.png \
		--logo-gbc     tools/ayaneo/snes/logos/gbc.png \
		--logo-gba     tools/ayaneo/snes/logos/gba.png \
		--logo-snes    tools/ayaneo/snes/logos/snes.png \
		--logo-genesis tools/ayaneo/snes/logos/genesis.png \
		--logo-sms     tools/ayaneo/snes/logos/sms.png \
		--logo-gg      tools/ayaneo/snes/logos/gg.png \
		--logo-sg      tools/ayaneo/snes/logos/sg.png

	echo ">> Building the SD card template (out/sdcard)"
	CORE_REPOS="${CORE_REPOS:-$(cd "$ROOT/../.." && pwd)}"
	CORE_ARGS=()
	for spec in armgba-pico:armGBA-pico gpsp-pico:gpsp-pico gambatte-pico:gambatte-pico \
		    snes9x-pico:snes9x-pico genesis-plus-gx-pico:genesis-plus-gx-pico; do
		name="${spec%%:*}"; repo="$CORE_REPOS/${spec#*:}"
		blob="$repo/out/$name.blob"
		if [ ! -f "$blob" ] && [ -x "$repo/build.sh" ]; then
			echo "   building $name in $repo"
			(cd "$repo" && ./build.sh >/dev/null)
		fi
		if [ -f "$blob" ]; then
			CORE_ARGS+=(--core "$name=$blob")
		else
			echo "!! $name: no $blob (core left off the card template)" >&2
		fi
	done
	python3 tools/ayaneo/gba/build_sdcard.py out/snes_pack.bin out/sdcard "${CORE_ARGS[@]}"
else
	echo "!! SNES_ASSETS dir not found: $SNES_ASSETS (LK built and signed; no card template)" >&2
fi

echo
echo ">> Done: out/lk_a_kpa_signed.img and out/sdcard/"
