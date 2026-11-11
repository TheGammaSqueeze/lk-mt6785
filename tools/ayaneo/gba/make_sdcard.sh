#!/bin/bash
# Build the SD card template in out/sdcard: the menu pack, the cores and boot.cfg (build_sdcard.py).
# Run from the LK tree root by build_konkr_kpa.sh and build_ayaneo_gba_sd.sh.
#
# The cores are built in their own repositories, next to the LK tree by default (CORE_REPOS, default
# the directory two levels above the LK root): armGBA-pico, gpsp-pico, gambatte-pico, snes9x-pico,
# genesis-plus-gx-pico. A core whose blob is missing is built with its build.sh; a core that cannot
# be found is left off the template with a warning. SNES_ASSETS overrides the menu asset tree.
# Returns 1 (and builds nothing) when the menu assets are missing.
set -e
ROOT="$(pwd)"
SNES_ASSETS="${SNES_ASSETS:-$ROOT/tools/ayaneo/snes/assets}"
if [ ! -d "$SNES_ASSETS" ]; then
	echo "!! SNES_ASSETS dir not found: $SNES_ASSETS (no card template)" >&2
	exit 1
fi
mkdir -p out

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
