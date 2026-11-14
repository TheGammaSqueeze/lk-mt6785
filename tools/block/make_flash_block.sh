#!/bin/bash
# SP Flash Tool v5 packages for the Konkr Pocket Block LK, Download Only, lk_a only.
#
#   tools/block/make_flash_block.sh <stock package dir> <signed lk image> <out dir>
#
# <out dir>/gammaos/   our LK on lk_a
# <out dir>/stock/     the stock lk.img on lk_a again (the way back)
# Each has the stock V1.1.8 scatter with only lk_a ticked and given a file, the stock preloader
# (unticked: SP Flash Tool reads its memory setup to connect, it is not written) and a
# Checksum.ini whose sums SP Flash Tool checks (the 16-bit sum of every byte of each file).
set -e
STOCK="$1"; LK="$2"; OUT="$3"
[ -d "$STOCK" ] && [ -f "$LK" ] && [ -n "$OUT" ] || { sed -n 2,9p "$0"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
SCATTER="$HERE/../release/kpa_scatter.py"
PRELOADER=preloader_m8695_bw01.bin

sum16() { python3 -c "import sys; print('0x%04X' % (sum(open(sys.argv[1],'rb').read()) & 0xffff))" "$1"; }

pack() {   # pack <dir> <lk file to ship> <name in the package> <checksum switch 0|1>
	local d="$1" src="$2" name="$3" check="$4"
	rm -rf "$d"; mkdir -p "$d"
	cp "$src" "$d/$name"
	cp "$STOCK/$PRELOADER" "$d/"
	python3 "$SCATTER" "$STOCK/MT6768_Android_scatter.txt" "$d/MT6768_Android_scatter.txt" "$name" lk_a
	printf '[VERSION]\r\nVERSION=V1\r\nTOOL=v5.1820.00\r\n[IsEnableChecksum]\r\nCHECKSUM_SWITCH=%s\r\n[CheckSum]\r\npreloader=%s\r\nlk_a=%s\r\n' \
		"$check" "$(sum16 "$d/$PRELOADER")" "$(sum16 "$d/$name")" > "$d/Checksum.ini"
}

# Our package changes with every build while SP Flash Tool keeps the sums it read when the scatter
# was loaded, so its check is off there (the sums in the file are right); the stock one keeps it.
pack "$OUT/gammaos" "$LK" lk_block.img 0
pack "$OUT/stock" "$STOCK/lk.img" lk.img 1
(cd "$OUT" && sha256sum gammaos/* stock/* > SHA256SUMS)
echo ">> $OUT/gammaos (our LK) and $OUT/stock (the stock LK), lk_a only"
