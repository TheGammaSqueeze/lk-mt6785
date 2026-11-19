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
	--logo-sg      tools/ayaneo/snes/logos/sg.png \
	--logo-nes     tools/ayaneo/snes/logos/nes.png

echo ">> Building the SD card template (out/sdcard)"
CORE_ARGS=()
for spec in armgba-pico:armGBA-pico gpsp-pico:gpsp-pico gambatte-pico:gambatte-pico \
	    snes9x-pico:snes9x-pico genesis-plus-gx-pico:genesis-plus-gx-pico armnes-pico:armNES-pico; do
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
# RetroAchievements (cores/ra-pico): in this tree, not a submodule; built from its sources each time
# (rcheevos comes from cores/armNES-pico, fetched above)
if (cd "$ROOT/cores/ra-pico" && ./build.sh >/dev/null 2>&1); then
	CORE_ARGS+=(--core "ra-pico=$ROOT/cores/ra-pico/out/ra-pico.blob")
else
	echo "!! ra-pico: build.sh failed (run it in cores/ra-pico to see why)" >&2
fi
python3 tools/ayaneo/gba/build_sdcard.py out/snes_pack.bin out/sdcard "${CORE_ARGS[@]}"

# HTTPS's trusted roots for the consoles with Wi-Fi (pico_tls): this PC's root bundle as DER. The LK
# keeps at most 128 KB of them (WF_ROOTS_AREA_LEN) and trusts none from a file cut short, so a
# bundle that has grown past that stops the template here instead of shipping a card that trusts
# nothing.
ROOTS_SRC=${ROOTS_SRC:-/etc/ssl/certs/ca-certificates.crt}
mkdir -p out/sdcard/bios/certs
python3 tools/ayaneo/pico_current/make_roots.py "$ROOTS_SRC" out/sdcard/bios/certs/roots.der
if [ "$(stat -c %s out/sdcard/bios/certs/roots.der)" -ge 131072 ]; then
	echo "!! roots.der is 128 KB or more: the LK would trust none of it" >&2
	exit 1
fi

# The Wi-Fi and Bluetooth chip's firmware (tools/release/connsys: each console's stock vendor
# firmware, both consoles' sets on the one card): the LK loads it from bios/connsys/. A set missing
# would ship a card on which Wi-Fi and Bluetooth do not start, so that stops the template here.
mkdir -p out/sdcard/bios/connsys
for v in 1a_1 2a_1; do
	for f in soc1_0_ram_mcu_${v}_hdr.bin soc1_0_ram_bt_${v}_hdr.bin soc1_0_ram_wifi_${v}_hdr.bin \
		 soc1_0_patch_mcu_${v}_hdr.bin WIFI_RAM_CODE_soc1_0_${v}.bin; do
		[ -f "tools/release/connsys/$f" ] || { echo "!! tools/release/connsys/$f is missing" >&2; exit 1; }
		cp "tools/release/connsys/$f" out/sdcard/bios/connsys/
	done
done
(cd tools/release/connsys && sha256sum -c --quiet SHA256SUMS) || { echo "!! the connsys firmware does not match SHA256SUMS" >&2; exit 1; }

# A theme with a menu of its own (a 'layout' file in its folder) brings its card packs, one per
# screen it was drawn for: the Current theme's are built from tools/ayaneo/pico_current. 960x640
# is the Konkr Pocket Advance's screen, 960x720 the Konkr Pocket Block's, 1280x960 the Pocket Air
# Mini's.
if [ -f "$THEMES_DIR/current/layout" ]; then
	echo ">> Building the Current theme's packs"
	for size in 960x640 960x720 1280x960; do
		python3 tools/ayaneo/pico_current/build_pack.py "$size" "out/sdcard/gammaos/current-$size.pack" \
			--igm "out/sdcard/gammaos/current-igm-$size.pack" \
			"${THEME_ARGS[@]}" || exit 1
	done
fi
# Game names in any script: the glyphs past the menu fonts' own, drawn once at a master size that the
# LK scales to each font that shows names (tools/ayaneo/pico_current/build_names.py)
echo ">> Building the names glyph pack"
python3 tools/ayaneo/pico_current/build_names.py out/sdcard/gammaos/names.pack || exit 1
# The menus' languages: a pack per language and screen, its words and the glyphs the fonts lack
# (tools/ayaneo/pico_current/i18n). Any theme reads them; English needs none. The Classic themes'
# letters go only to the Konkr screens: the Pocket Air Mini's LK has no Classic themes (emu/pico.mk).
echo ">> Building the language packs"
for size in 960x640 960x720 1280x960; do
	classic=
	[ "$size" = 1280x960 ] && classic=--no-classic
	python3 tools/ayaneo/pico_current/build_lang.py "$size" out/sdcard/gammaos $classic || exit 1
done
