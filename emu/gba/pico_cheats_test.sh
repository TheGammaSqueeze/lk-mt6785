#!/bin/bash
# pico_cheats_test.sh <card dir with gammaos/cheats/*.gcd> - pico_cheats.c on the PC (pico_cheats_test.c)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
INC=$(mktemp -d); trap 'rm -rf "$INC"' EXIT
mkdir -p "$INC/lib"; echo '#include <zlib.h>' > "$INC/lib/zlib.h"
gcc -O1 -g -Wall -Wno-unused-function -fsanitize=address,undefined -I"$DIR" -I"$INC" -o "$INC/t" \
	"$DIR/pico_cheats_test.c" "$DIR/pico_cheats.c" -lz
CARD=$(mktemp -d); trap 'rm -rf "$INC" "$CARD"' EXIT
mkdir -p "$CARD/gammaos"; cp -r "${1:?card dir}/gammaos/cheats" "$CARD/gammaos/"
"$INC/t" "$CARD"
