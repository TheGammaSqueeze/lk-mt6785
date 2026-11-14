#!/bin/bash
# Build the Konkr Pocket Advance GammaOS Pico release from a clone of this repository.
#
#   tools/release/make_release_kpa.sh [version]        (default version: the VERSION file)
#
# Output in out/release/:
#   gammaos-pico-kpa-<version>-flash.zip    the LK for lk_a and lk_b: SP Flash Tool packages (GammaOS Pico and
#                                      back to stock) and fastboot scripts for Windows and Linux
#   gammaos-pico-kpa-<version>-sdcard.zip   the microSD card: menu, the five emulator cores, freely
#                                      licensed demo games with box art, licences and credits
#   gammaos-pico-kpa-windows-tools.zip      with MTK_DRIVER set: SP Flash Tool v5.1924 and MediaTek's
#                                      USB driver for the Windows install (make_windows_tools_kpa.sh)
#   SHA256SUMS, RELEASE_NOTES.md
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
R=out/release
FILES=$R/files
FLASH=gammaos-pico-kpa-$VER-flash
CARD=gammaos-pico-kpa-$VER-sdcard
STOCK=tools/konkr/stock

echo ">> GammaOS Pico for the Konkr Pocket Advance, release $VER"
if [ -e .git ]; then
	echo ">> Fetching the core submodules"
	git submodule update --init --recursive cores
fi
(cd "$STOCK" && sha256sum -c --quiet SHA256SUMS) || { echo "!! $STOCK does not match its SHA256SUMS" >&2; exit 1; }

# the LK shows its commit in Settings > About, with a "+" when it was built from changed files
if [ -e .git ] && ! git diff --quiet HEAD -- && [ -z "$RELEASE_DIRTY" ]; then
	echo "!! the tree has changes that are not committed: commit them first (RELEASE_DIRTY=1 to go on)" >&2
	exit 1
fi
./build_konkr_kpa.sh
[ -f out/lk_a_kpa_signed.img ] && [ -d out/sdcard/cores ] || { echo "!! the LK build did not produce the image and the card template" >&2; exit 1; }
for c in armgba-pico gpsp-pico gambatte-pico snes9x-pico genesis-plus-gx-pico; do
	[ -f out/sdcard/cores/$c.blob ] || { echo "!! the card template has no $c core" >&2; exit 1; }
done

rm -rf "$FILES" "$R"/*.zip "$R"/SHA256SUMS "$R"/RELEASE_NOTES.md
WINTOOLS=gammaos-pico-kpa-windows-tools.zip
mkdir -p "$FILES/$FLASH" "$FILES/$CARD"
LK_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
LK_COMMIT_FULL=$(git rev-parse HEAD 2>/dev/null || echo lk-gba-emu-sd-card-kpa)   # the screenshots in the notes

# ---- the flash package ----
echo ">> Flash package"
F="$FILES/$FLASH"
mkdir -p "$F/spflash/gammaos" "$F/spflash/stock" "$F/mtkclient" "$F/fastboot"
for which in gammaos stock; do
	# SP Flash Tool v5: the text scatter, ticking lk_a and lk_b (V6 does not take these packages)
	if [ $which = gammaos ]; then img=out/lk_a_kpa_signed.img; label="GammaOS Pico"; else img="$STOCK/lk.img"; label=stock; fi
	cp "$img" "$F/spflash/$which/lk_kpa_$which.img"
	cp "$STOCK/preloader_k85v1_64.bin" "$F/spflash/$which/"
	python3 tools/release/kpa_scatter.py "$STOCK/MT6785_Android_scatter.txt" \
		"$F/spflash/$which/MT6785_Android_scatter.txt" lk_kpa_$which.img lk_a lk_b
	cp "$img" "$F/mtkclient/lk_kpa_$which.img"
	cp "$img" "$F/fastboot/lk_kpa_$which.img"
	printf '#!/bin/sh\n# Write the %s LK to lk_a and lk_b with mtkclient (Linux), no unlocked bootloader needed.\n# Power the console off and unplug it, run this, then plug the console in when mtkclient\n# waits for it. See README.txt for installing mtkclient.\ncd "$(dirname "$0")"\nMTK="${MTK:-mtk}"   # or MTK="python3 /path/to/mtkclient/mtk.py"\ncommand -v ${MTK%%%% *} >/dev/null 2>&1 || { echo "mtkclient (mtk) not found: see README.txt"; exit 1; }\nif $MTK w lk_a,lk_b lk_kpa_%s.img,lk_kpa_%s.img; then\n\techo "Done. Unplug the console and hold POWER to start it."\nelse\n\techo "Writing failed. Do not unplug yet if a write was in progress; run this again."\nfi\n' \
		"$label" "$which" "$which" > "$F/mtkclient/flash_$which.sh"
	chmod +x "$F/mtkclient/flash_$which.sh"
done
for which in gammaos stock; do
	if [ $which = gammaos ]; then label="GammaOS Pico"; else label=stock; fi
	printf '@echo off\r\nrem Flash the %s LK to lk_a and lk_b. The console must be in its bootloader (fastboot).\r\nfastboot flash lk_a lk_kpa_%s.img || goto :fail\r\nfastboot flash lk_b lk_kpa_%s.img || goto :fail\r\nfastboot reboot\r\necho Done.\r\ngoto :eof\r\n:fail\r\necho Flashing failed. Do not restart the console; run this again.\r\n' \
		"$label" "$which" "$which" > "$F/fastboot/flash_$which.bat"
	printf '#!/bin/sh\n# Flash the %s LK to lk_a and lk_b. The console must be in its bootloader (fastboot).\ncd "$(dirname "$0")"\nfastboot flash lk_a lk_kpa_%s.img && fastboot flash lk_b lk_kpa_%s.img && fastboot reboot && echo Done. ||\n\techo "Flashing failed. Do not restart the console; run this again."\n' \
		"$label" "$which" "$which" > "$F/fastboot/flash_$which.sh"
	chmod +x "$F/fastboot/flash_$which.sh"
done
sed "s/@VERSION@/$VER/; s/@COMMIT@/$LK_COMMIT/" tools/release/README_flash.txt > "$F/README.txt"
(cd "$F" && find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)

# ---- the SD card package ----
echo ">> SD card package"
S="$FILES/$CARD"
cp -r out/sdcard/. "$S/"
python3 tools/release/demo_games.py tools/release/demo_games.json "$S" "$R/cache"
for dir in armGBA-pico gpsp-pico gambatte-pico snes9x-pico genesis-plus-gx-pico; do
	d="$S/licences/cores/$dir"
	mkdir -p "$d"
	cp cores/$dir/LICENSE "$d/LICENSE"
	[ -f cores/$dir/THIRD_PARTY_NOTICES.md ] && cp cores/$dir/THIRD_PARTY_NOTICES.md "$d/"
	printf 'Built from https://github.com/TheGammaSqueeze/%s at commit %s\n(the emulator upstream is its submodule, the GammaOS Pico changes are in patches/).\n' \
		"$dir" "$(git -C cores/$dir rev-parse HEAD)" > "$d/SOURCE.txt"
done
# the menus' fonts (Inter, Noto), whose glyphs the menu, language and names packs are drawn from
mkdir -p "$S/licences/fonts"
cp tools/ayaneo/pico_current/art/fonts/OFL.txt "$S/licences/fonts/Inter-OFL.txt"
cp tools/ayaneo/pico_current/art/fonts/OFL-Noto.txt "$S/licences/fonts/Noto-OFL.txt"
sed "s/@VERSION@/$VER/; s/@COMMIT@/$LK_COMMIT/" tools/release/README_sdcard.txt > "$S/README.txt"

# ---- zips, checksums, notes ----
echo ">> Zips"
python3 tools/release/det_zip.py "$S" "$R/$CARD.zip"
python3 tools/release/det_zip.py "$F" "$R/$FLASH.zip"
if [ -n "${MTK_DRIVER:-}" ]; then
	tools/release/make_windows_tools_kpa.sh "$R/$WINTOOLS"
else
	echo "!! MTK_DRIVER is not set: no $WINTOOLS (the Windows install needs it in the release)" >&2
fi
(cd "$R" && sha256sum "$FLASH.zip" "$CARD.zip" $([ -f "$WINTOOLS" ] && echo "$WINTOOLS") > SHA256SUMS)
sed "s/@VERSION@/$VER/g; s/@COMMIT@/$LK_COMMIT/g; s/@COMMIT_FULL@/$LK_COMMIT_FULL/g" tools/release/RELEASE_NOTES.md > "$R/RELEASE_NOTES.md"
python3 - "$R/RELEASE_NOTES.md" tools/release/demo_games.json <<'PY'
import json, sys
notes, games = sys.argv[1], json.load(open(sys.argv[2]))
names = {"gb": "Game Boy", "gbc": "Game Boy Color", "gba": "Game Boy Advance", "snes": "SNES",
         "genesis": "Mega Drive / Genesis", "sms": "Master System", "gg": "Game Gear", "sg": "SG-1000"}
rows = "\n".join("| %s | %s | %s | %s |" % (g["title"], names[g["system"]], g["credit"], g["licence"]) for g in games)
text = open(notes).read().replace("@GAMES@", "| Game | System | By | Licence |\n|---|---|---|---|\n" + rows)
open(notes, "w").write(text)
PY
echo
cat "$R/SHA256SUMS"
echo ">> Done: $R/$FLASH.zip, $R/$CARD.zip, $R/RELEASE_NOTES.md"
