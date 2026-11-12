#!/bin/bash
# Build the SD card template in out/sdcard: the menu pack, the cores and boot.cfg (build_sdcard.py).
# Run from the LK tree root by build_konkr_kpa.sh and build_ayaneo_gba_sd.sh.
#
# The cores are git submodules under cores/, each its own repository (upstream emulator as a
# submodule + GammaOS Pico patches + glue): armGBA-pico, gpsp-pico, gambatte-pico, snes9x-pico,
# genesis-plus-gx-pico. A core that is not checked out yet is fetched (with its upstream); a core
# whose checkout differs from the commit this tree pins is built as it is, with a note. Each core
# is built with its build.sh whenever its blob is missing, its commit changed since the last build
# or its tree has local changes, so the template never carries a stale blob. CORE_REPOS=<dir> takes
# the cores from other checkouts (core development); SNES_ASSETS overrides the menu asset tree.
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

# Menu themes (tools/ayaneo/snes/themes/<name>/): classic first, its atlas is the asset tree's own,
# then every other theme folder in name order, each with its own UI atlas. Picked on the Display page.
THEMES_DIR="$ROOT/tools/ayaneo/snes/themes"
THEME_ARGS=(--theme "classic=$THEMES_DIR/classic")
for t in "$THEMES_DIR"/*/; do
	t="${t%/}"; n="$(basename "$t")"
	[ "$n" = classic ] || THEME_ARGS+=(--theme "$n=$t")
done

echo ">> Packing menu assets"
python3 tools/ayaneo/snes/pack_snes.py "$SNES_ASSETS" out/snes_pack.bin \
	--rgb565 --gba-cart out/gba_cart.png "${THEME_ARGS[@]}" \
	--logo-gb      tools/ayaneo/snes/logos/gb.png \
	--logo-gbc     tools/ayaneo/snes/logos/gbc.png \
	--logo-gba     tools/ayaneo/snes/logos/gba.png \
	--logo-snes    tools/ayaneo/snes/logos/snes.png \
	--logo-genesis tools/ayaneo/snes/logos/genesis.png \
	--logo-sms     tools/ayaneo/snes/logos/sms.png \
	--logo-gg      tools/ayaneo/snes/logos/gg.png \
	--logo-sg      tools/ayaneo/snes/logos/sg.png

echo ">> Building the SD card template (out/sdcard)"
CORE_ARGS=()
for spec in armgba-pico:armGBA-pico gpsp-pico:gpsp-pico gambatte-pico:gambatte-pico \
	    snes9x-pico:snes9x-pico genesis-plus-gx-pico:genesis-plus-gx-pico; do
	name="${spec%%:*}"; dir="${spec#*:}"
	if [ -n "$CORE_REPOS" ]; then
		repo="$CORE_REPOS/$dir"
	else
		repo="$ROOT/cores/$dir"
		if [ ! -x "$repo/build.sh" ]; then
			echo "   fetching cores/$dir"
			git -C "$ROOT" submodule update --init --recursive -- "cores/$dir" ||
				{ echo "!! cores/$dir could not be fetched" >&2; }
		else
			pinned=$(git -C "$ROOT" ls-tree HEAD "cores/$dir" | awk '{print $3}')
			have=$(git -C "$repo" rev-parse HEAD 2>/dev/null)
			[ -n "$pinned" ] && [ "$pinned" != "$have" ] &&
				echo "   note: cores/$dir is at ${have:0:7}, this tree pins ${pinned:0:7} (git submodule update --recursive to follow)"
			[ -e "$repo/upstream/.git" ] ||
				git -C "$repo" submodule update --init --recursive >/dev/null 2>&1 || true
		fi
	fi
	blob="$repo/out/$name.blob"
	if [ -x "$repo/build.sh" ]; then
		head=$(git -C "$repo" rev-parse HEAD 2>/dev/null)
		dirty=$(git -C "$repo" status --porcelain --ignore-submodules=none 2>/dev/null | head -1)
		stamp="$repo/out/.built-commit"
		if [ ! -f "$blob" ] || [ -n "$dirty" ] || [ "$(cat "$stamp" 2>/dev/null)" != "$head" ]; then
			echo "   building $name (${head:0:7}${dirty:+, local changes})"
			if (cd "$repo" && ./build.sh >/dev/null 2>&1); then
				[ -z "$dirty" ] && echo "$head" > "$stamp" || rm -f "$stamp"
			else
				echo "!! $name: build.sh failed (run it in $repo to see why)" >&2
				rm -f "$blob" "$stamp"
			fi
		fi
	fi
	if [ -f "$blob" ]; then
		CORE_ARGS+=(--core "$name=$blob")
	else
		echo "!! $name: no $blob (core left off the card template)" >&2
	fi
done
python3 tools/ayaneo/gba/build_sdcard.py out/snes_pack.bin out/sdcard "${CORE_ARGS[@]}"

# A theme with a menu of its own (a 'layout' file in its folder) brings its card packs, one per
# screen it was drawn for: the Current theme's are built from tools/ayaneo/pico_current.
if [ -f "$THEMES_DIR/current/layout" ]; then
	echo ">> Building the Current theme's packs"
	for size in 960x640 1280x960; do
		python3 tools/ayaneo/pico_current/build_pack.py "$size" "out/sdcard/gammaos/current-$size.pack" \
			"${THEME_ARGS[@]}" || exit 1
	done
fi
