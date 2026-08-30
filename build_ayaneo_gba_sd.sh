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

# sanity: the SNES-menu driver object must be linked in this variant
if [ ! -f "build-$PROJECT/emu/gba/gba_snes_menu.o" ]; then
	echo "!! gba_snes_menu.o was not built - AYANEO_GBA_SD did not take effect" >&2
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

	# The pack (hence boot_b) is byte-deterministic: it only changes when the SNES
	# assets or the cart art change. Report whether it differs from the last build so
	# the user can skip re-flashing the ~22MB boot_b on code-only (lk_a) changes.
	BOOT_B_CHANGED=1
	PACK_SHA="$(sha256sum out/snes_pack.bin | cut -d' ' -f1)"
	if [ -f out/.snes_pack.sha ] && [ "$(cat out/.snes_pack.sha)" = "$PACK_SHA" ]; then
		BOOT_B_CHANGED=0
	fi
	echo "$PACK_SHA" > out/.snes_pack.sha

	# Non-fatal smoke test: build the host renderer and render the home + a submenu
	# from the freshly-packed blob, so an asset-pipeline break (bad pack, missing
	# resource, render crash) is caught here rather than only on device.
	if command -v gcc >/dev/null 2>&1; then
		echo ">> Smoke-testing the pack (host render)"
		if bash emu/gba/menu/build_host.sh >/dev/null 2>&1; then
			# render a nav path, then verify the frame is not blank (>1% of the
			# pixel bytes non-zero) so a "pack opens but renders nothing" regression
			# is caught, not just a crash. Pure-stdlib check (no PIL dependency).
			if GBA_ROSTER=6 emu/gba/menu/host_render out/snes_pack.bin \
				/tmp/gba_smoke.ppm 60 "URA" >/dev/null 2>&1 \
				&& python3 -c "import sys;d=open('/tmp/gba_smoke.ppm','rb').read();i=0
for _ in range(3): i=d.index(b'\n',i)+1
b=d[i:];sys.exit(0 if sum(x!=0 for x in b)>len(b)//100 else 1)"; then
				echo "   smoke test OK (home + Display submenu rendered, non-blank)"
			else
				echo "!! smoke test FAILED: pack missing/blank render - check pack_snes.py" >&2
			fi
		else
			echo "   (host renderer build skipped - not fatal)"
		fi
	fi
else
	echo "!! SNES_ASSETS dir not found: $SNES_ASSETS" >&2
	echo "   set SNES_ASSETS=<snes-mini asset tree> to build the menu boot_b." >&2
	echo "   (LK still built; it falls back to the plain list without the pack.)" >&2
fi

echo
echo ">> Done."
echo "   Signed LK:  out/lk_a_gba_sd_signed.img -> fastboot flash lk_a    out/lk_a_gba_sd_signed.img"
echo "   boot_b:     out/gba_menu_boot_b.img    -> fastboot flash boot_b  out/gba_menu_boot_b.img"
if [ "${BOOT_B_CHANGED:-1}" = "0" ]; then
	echo "   NOTE: boot_b assets are UNCHANGED since the last build - you can skip"
	echo "         re-flashing boot_b and just flash lk_a (a code-only change)."
fi
