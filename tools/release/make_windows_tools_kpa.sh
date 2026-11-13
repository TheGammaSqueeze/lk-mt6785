#!/bin/bash
# The Windows install's tools for the Konkr Pocket Advance release, as MediaTek ships them:
# SP Flash Tool v5.1924 (the GammaOS Pico flash packages are for SP Flash Tool v5; V6 does not take
# them) and MediaTek's USB driver installer (MediaTek SP Driver 5.23.07.02).
#
#   MTK_DRIVER=/path/to/MTK\ Driver\ Setup.exe tools/release/make_windows_tools_kpa.sh <out.zip>
#
# SP Flash Tool is downloaded from spflashtool.com, the archive the tool itself names as its home,
# and checked against a pinned sha256 before it is unpacked (the archive's password is the site's
# name). The driver installer has no stable download address, so its path is given and checked
# against its own pinned sha256. Unpacking the RAR5 archive needs 7-Zip 21 or newer for Linux
# (7zz) or unrar; SEVENZIP=/path/to/7zz picks one that is not on PATH.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?usage: MTK_DRIVER=<MTK Driver Setup.exe> $0 <out.zip>}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT";; esac

SPFT_URL=https://spflashtool.com/files/SP_Flash_Tool_v5.1924_Win.rar
SPFT_SHA=a00de51f112d9268d10100708345056053dd36cf030006b79a26061009f7cf32
SPFT_PASS=spflashtool.com
DRV_SHA=3cf1997d5a33411e2385e1e492689adc88b87255845a99e9c12965361602df98
DRV_NAME="MTK Driver Setup.exe"

[ -f "${MTK_DRIVER:-}" ] || { echo "!! MTK_DRIVER must name MediaTek's driver installer ($DRV_NAME)" >&2; exit 1; }
echo "$DRV_SHA  $MTK_DRIVER" | sha256sum -c --quiet || { echo "!! $MTK_DRIVER is not MediaTek SP Driver 5.23.07.02" >&2; exit 1; }

X="${SEVENZIP:-$(command -v 7zz || true)}"
[ -n "$X" ] || command -v unrar >/dev/null || { echo "!! needs 7zz (7-Zip 21+ for Linux) or unrar to unpack RAR5" >&2; exit 1; }

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
CACHE="$ROOT/out/release/cache"; mkdir -p "$CACHE"
RAR="$CACHE/SP_Flash_Tool_v5.1924_Win.rar"
if ! echo "$SPFT_SHA  $RAR" | sha256sum -c --quiet >/dev/null 2>&1; then
	echo ">> Downloading SP Flash Tool v5.1924"
	# the site sends the archive only to a request that comes from its download page
	curl -fsSL -A "Mozilla/5.0" -e https://spflashtool.com/download/ -o "$RAR" "$SPFT_URL"
	echo "$SPFT_SHA  $RAR" | sha256sum -c --quiet || { echo "!! the SP Flash Tool download does not match its sha256" >&2; exit 1; }
fi
if [ -n "$X" ]; then "$X" x -y -bd -p"$SPFT_PASS" -o"$W/x" "$RAR" > /dev/null
else (mkdir -p "$W/x" && cd "$W/x" && unrar x -inul -p"$SPFT_PASS" "$RAR"); fi

T="$W/gammaos-pico-kpa-windows-tools"
mkdir -p "$T/MediaTek USB driver"
mv "$W/x/SP_Flash_Tool_v5.1924_Win" "$T/SP Flash Tool v5.1924"
# the archive's Driver folder holds only links to a third-party file host; the installer itself is
# given beside it instead
rm -rf "$T/SP Flash Tool v5.1924/Driver"
cp "$MTK_DRIVER" "$T/MediaTek USB driver/$DRV_NAME"
cp "$ROOT/tools/release/README_windows_tools.txt" "$T/README.txt"
(cd "$T" && find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||' | xargs -d '\n' sha256sum > SHA256SUMS)
python3 "$ROOT/tools/release/det_zip.py" "$T" "$OUT"
echo ">> $OUT ($(find "$T" -type f | wc -l) files)"
