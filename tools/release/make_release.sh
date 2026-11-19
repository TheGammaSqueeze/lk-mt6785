#!/bin/bash
# Build the GammaOS Pico release for the Konkr Pocket Advance and the Konkr Pocket Block from a clone
# of this repository.
#
#   tools/release/make_release.sh [version]        (default version: the VERSION file)
#   CONSOLES="kpa" tools/release/make_release.sh   (one console only; default "kpa block")
#
# Output in out/release/, for each console (kpa, block):
#   gammaos-pico-<console>-<version>-flash.zip    the LK: SP Flash Tool packages (GammaOS Pico and back
#                                                 to stock), mtkclient scripts and fastboot scripts
#   gammaos-pico-<console>-<version>-sdcard.zip   the microSD card: menu, the emulator cores, freely
#                                                 licensed demo games with box art, licences and
#                                                 credits (the same card for both consoles: it holds
#                                                 every screen size's packs)
#   gammaos-pico-<console>-windows-tools.zip      with MTK_DRIVER set: SP Flash Tool v5.1924 and
#                                                 MediaTek's USB driver (make_windows_tools.sh)
# and one SHA256SUMS and RELEASE_NOTES.md for all of them.
#
# Needs: the LK build's needs (Linux, Python 3 with Pillow and numpy, openssl), arm-none-eabi
# gcc/g++ for the cores, git and network access for the core submodules and the demo games
# (downloads are checked against pinned sha256 sums, tools/release/demo_games.json). The zips are
# written with sorted entries and fixed timestamps, so they differ only where the files do (the LK
# image carries its build time).
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
# the version is the VERSION file's (the one the LK shows in Settings > About); an argument overrides it
VER="${1:-$(head -1 VERSION 2>/dev/null)}"
[ -n "$VER" ] || { echo "!! no version: VERSION is missing and none was given" >&2; exit 1; }
CONSOLES="${CONSOLES:-kpa block}"
R=out/release
FILES=$R/files

# Each console: its name, the signed LK the build writes, the stock files (lk.img, the preloader and
# the scatter, from its own firmware package), the LK partitions GammaOS Pico writes and the
# shoulder button of the bootloader combo. The Advance runs either LK slot, so both get it; the
# Block's preloader starts lk_a, and its factory never writes lk_b, so only lk_a is written there.
console_vars() {
	case "$1" in
	kpa)
		NAME="Konkr Pocket Advance"; LK=out/lk_a_kpa_signed.img; STOCK=tools/konkr/stock
		PRELOADER=preloader_k85v1_64.bin; SCATTER=MT6785_Android_scatter.txt; PARTS="lk_a lk_b"
		STOCK_FW="BW03" ;;
	block)
		NAME="Konkr Pocket Block"; LK=out/lk_a_block_signed.img; STOCK=tools/block/stock
		PRELOADER=preloader_m8695_bw01.bin; SCATTER=MT6768_Android_scatter.txt; PARTS="lk_a"
		STOCK_FW="V02" ;;
	*) echo "!! unknown console $1 (kpa or block)" >&2; exit 1 ;;
	esac
}

echo ">> GammaOS Pico release $VER for: $CONSOLES"
if [ -e .git ]; then
	echo ">> Fetching the core submodules"
	git submodule update --init --recursive cores
fi
for c in $CONSOLES; do
	console_vars $c
	(cd "$STOCK" && sha256sum -c --quiet SHA256SUMS) || { echo "!! $STOCK does not match its SHA256SUMS" >&2; exit 1; }
done

# the LK shows its commit in Settings > About, with a "+" when it was built from changed files
if [ -e .git ] && ! git diff --quiet HEAD -- && [ -z "$RELEASE_DIRTY" ]; then
	echo "!! the tree has changes that are not committed: commit them first (RELEASE_DIRTY=1 to go on)" >&2
	exit 1
fi
# the Advance's build also makes the card template, which both consoles share
./build_konkr_kpa.sh
[ -f out/lk_a_kpa_signed.img ] && [ -d out/sdcard/cores ] || { echo "!! the LK build did not produce the image and the card template" >&2; exit 1; }
case " $CONSOLES " in *" block "*)
	./build_konkr_block.sh
	[ -f out/lk_a_block_signed.img ] || { echo "!! the Block's LK build did not produce its image" >&2; exit 1; } ;;
esac
for c in armgba-pico gpsp-pico gambatte-pico snes9x-pico genesis-plus-gx-pico armnes-pico ra-pico; do
	[ -f out/sdcard/cores/$c.blob ] || { echo "!! the card template has no $c core" >&2; exit 1; }
done
[ -f out/sdcard/bios/certs/roots.der ] || { echo "!! the card template has no HTTPS roots" >&2; exit 1; }
[ "$(ls out/sdcard/bios/connsys 2>/dev/null | wc -l)" -ge 10 ] || { echo "!! the card template has no Wi-Fi and Bluetooth firmware (bios/connsys)" >&2; exit 1; }

rm -rf "$FILES" "$R"/*.zip "$R"/SHA256SUMS "$R"/RELEASE_NOTES.md
mkdir -p "$FILES"
LK_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
LK_COMMIT_FULL=$(git rev-parse HEAD 2>/dev/null || echo lk-gba-emu-sd-card-kpa)   # the screenshots in the notes

# ---- the card, built once ----
echo ">> SD card"
CARD_SRC="$FILES/card"
mkdir -p "$CARD_SRC"
cp -r out/sdcard/. "$CARD_SRC/"
python3 tools/release/demo_games.py tools/release/demo_games.json "$CARD_SRC" "$R/cache"
for dir in armGBA-pico gpsp-pico gambatte-pico snes9x-pico genesis-plus-gx-pico armNES-pico; do
	d="$CARD_SRC/licences/cores/$dir"
	mkdir -p "$d"
	cp cores/$dir/LICENSE "$d/LICENSE"
	[ -f cores/$dir/THIRD_PARTY_NOTICES.md ] && cp cores/$dir/THIRD_PARTY_NOTICES.md "$d/"
	printf 'Built from https://github.com/TheGammaSqueeze/%s at commit %s\n(the emulator upstream is its submodule, the GammaOS Pico changes are in patches/).\n' \
		"$dir" "$(git -C cores/$dir rev-parse HEAD)" > "$d/SOURCE.txt"
done
# RetroAchievements (cores/ra-pico, in this tree): rcheevos' licence and what the blob is made of
d="$CARD_SRC/licences/cores/ra-pico"
mkdir -p "$d"
cp cores/ra-pico/README.md "$d/README.md"
cp cores/armNES-pico/upstream/armulators/deps/rcheevos/LICENSE "$d/LICENSE-rcheevos"
printf 'Built from this release'"'"'s LK tree (lk-mt6785, cores/ra-pico) at commit %s.\n' "$LK_COMMIT_FULL" > "$d/SOURCE.txt"
# the menus' fonts (Inter, Noto), whose glyphs the menu, language and names packs are drawn from
mkdir -p "$CARD_SRC/licences/fonts"
cp tools/ayaneo/pico_current/art/fonts/OFL.txt "$CARD_SRC/licences/fonts/Inter-OFL.txt"
cp tools/ayaneo/pico_current/art/fonts/OFL-Noto.txt "$CARD_SRC/licences/fonts/Noto-OFL.txt"
sed "s/@VERSION@/$VER/; s/@COMMIT@/$LK_COMMIT/" tools/release/README_sdcard.txt > "$CARD_SRC/README.txt"

ASSETS=""
for c in $CONSOLES; do
	console_vars $c
	FLASH=gammaos-pico-$c-$VER-flash
	CARD=gammaos-pico-$c-$VER-sdcard
	WINTOOLS=gammaos-pico-$c-windows-tools.zip
	echo ">> $NAME: flash package"
	F="$FILES/$FLASH"
	mkdir -p "$F/spflash/gammaos" "$F/spflash/stock" "$F/mtkclient" "$F/fastboot"
	set -- $PARTS
	for which in gammaos stock; do
		if [ $which = gammaos ]; then img=$LK; label="GammaOS Pico"; else img="$STOCK/lk.img"; label=stock; fi
		name=lk_${c}_$which.img
		# SP Flash Tool v5: the text scatter with only the LK partitions ticked (V6 does not take
		# these packages), and a Checksum.ini with the 16-bit byte sums SP Flash Tool checks
		cp "$img" "$F/spflash/$which/$name"
		cp "$STOCK/$PRELOADER" "$F/spflash/$which/"
		python3 tools/release/lk_scatter.py "$STOCK/$SCATTER" "$F/spflash/$which/$SCATTER" $name $PARTS
		if [ $c = block ]; then
			sum16() { python3 -c "import sys; print('0x%04X' % (sum(open(sys.argv[1],'rb').read()) & 0xffff))" "$1"; }
			printf '[VERSION]\r\nVERSION=V1\r\nTOOL=v5.1820.00\r\n[IsEnableChecksum]\r\nCHECKSUM_SWITCH=1\r\n[CheckSum]\r\npreloader=%s\r\nlk_a=%s\r\n' \
				"$(sum16 "$F/spflash/$which/$PRELOADER")" "$(sum16 "$F/spflash/$which/$name")" > "$F/spflash/$which/Checksum.ini"
		fi
		cp "$img" "$F/mtkclient/$name"
		cp "$img" "$F/fastboot/$name"
		mparts=$(echo $PARTS | tr ' ' ,); mfiles=$(for p in $PARTS; do printf '%s,' $name; done); mfiles=${mfiles%,}
		printf '#!/bin/sh\n# Write the %s LK to %s with mtkclient (Linux), no unlocked bootloader needed.\n# Power the console off and unplug it, run this, then plug the console in when mtkclient\n# waits for it. See README.txt for installing mtkclient.\ncd "$(dirname "$0")"\nMTK="${MTK:-mtk}"   # or MTK="python3 /path/to/mtkclient/mtk.py"\ncommand -v ${MTK%%%% *} >/dev/null 2>&1 || { echo "mtkclient (mtk) not found: see README.txt"; exit 1; }\nif $MTK w %s %s; then\n\techo "Done. Unplug the console and hold POWER to start it."\nelse\n\techo "Writing failed. Do not unplug yet if a write was in progress; run this again."\nfi\n' \
			"$label" "$(echo $PARTS | sed 's/ / and /')" "$mparts" "$mfiles" > "$F/mtkclient/flash_$which.sh"
		chmod +x "$F/mtkclient/flash_$which.sh"
		bat=""; sh=""
		for p in $PARTS; do
			bat="${bat}fastboot flash $p $name || goto :fail\r\n"
			sh="${sh}fastboot flash $p $name && "
		done
		printf "@echo off\r\nrem Flash the %s LK to %s. The console must be in its bootloader (fastboot).\r\n${bat}fastboot reboot\r\necho Done.\r\ngoto :eof\r\n:fail\r\necho Flashing failed. Do not restart the console; run this again.\r\n" \
			"$label" "$(echo $PARTS | sed 's/ / and /')" > "$F/fastboot/flash_$which.bat"
		printf '#!/bin/sh\n# Flash the %s LK to %s. The console must be in its bootloader (fastboot).\ncd "$(dirname "$0")"\n%sfastboot reboot && echo Done. ||\n\techo "Flashing failed. Do not restart the console; run this again."\n' \
			"$label" "$(echo $PARTS | sed 's/ / and /')" "$sh" > "$F/fastboot/flash_$which.sh"
		chmod +x "$F/fastboot/flash_$which.sh"
	done
	sed "s/@VERSION@/$VER/; s/@COMMIT@/$LK_COMMIT/" tools/release/README_flash_$c.txt > "$F/README.txt"
	(cd "$F" && find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)

	echo ">> $NAME: card package"
	S="$FILES/$CARD"
	cp -r "$CARD_SRC" "$S"
	python3 tools/release/det_zip.py "$S" "$R/$CARD.zip"
	python3 tools/release/det_zip.py "$F" "$R/$FLASH.zip"
	ASSETS="$ASSETS $FLASH.zip $CARD.zip"
	if [ -n "${MTK_DRIVER:-}" ]; then
		tools/release/make_windows_tools.sh $c "$R/$WINTOOLS"
		ASSETS="$ASSETS $WINTOOLS"
	else
		echo "!! MTK_DRIVER is not set: no $WINTOOLS (the Windows install needs it in the release)" >&2
	fi
done

(cd "$R" && sha256sum $ASSETS > SHA256SUMS)
sed "s/@VERSION@/$VER/g; s/@COMMIT@/$LK_COMMIT/g; s/@COMMIT_FULL@/$LK_COMMIT_FULL/g" tools/release/RELEASE_NOTES.md > "$R/RELEASE_NOTES.md"
python3 - "$R/RELEASE_NOTES.md" tools/release/demo_games.json <<'PY'
import json, sys
notes, games = sys.argv[1], json.load(open(sys.argv[2]))
names = {"gb": "Game Boy", "gbc": "Game Boy Color", "gba": "Game Boy Advance", "snes": "SNES",
         "genesis": "Mega Drive / Genesis", "sms": "Master System", "gg": "Game Gear", "sg": "SG-1000",
         "nes": "NES"}
rows = "\n".join("| %s | %s | %s | %s |" % (g["title"], names[g["system"]], g["credit"], g["licence"]) for g in games)
text = open(notes).read().replace("@GAMES@", "| Game | System | By | Licence |\n|---|---|---|---|\n" + rows)
open(notes, "w").write(text)
PY
echo
cat "$R/SHA256SUMS"
echo ">> Done: out/release/ ($ASSETS ), RELEASE_NOTES.md"
