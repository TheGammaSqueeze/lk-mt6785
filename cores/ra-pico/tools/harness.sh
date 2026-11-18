#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 TheGammaSqueeze
# ra-pico: build and run the PC harness (tools/ra_harness.c) over ra_core.c and rcheevos.
#   tools/harness.sh            build with the PC's compiler and run
#   SAN=1 tools/harness.sh      the same with the address and undefined behaviour checkers
#   tools/harness.sh blob       the built blob (./build.sh) itself, under qemu-arm
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
. "$ROOT/sources.sh"
OUT=$ROOT/out/harness
mkdir -p "$OUT"
if [ "${1:-}" = blob ]; then
	arm-linux-gnueabihf-gcc -O1 -g -static -Wall -Wno-unused-parameter -DRA_HARNESS_BLOB -I"$ROOT/pico" \
		-o "$OUT/ra_harness_arm" "$ROOT/tools/ra_harness.c"
	exec qemu-arm-static "$OUT/ra_harness_arm" "$ROOT/out/ra-pico.blob"
fi
FL="-O1 -g -std=gnu11 -Wall -Wno-unused-parameter $RC_DEFS $RC_INC -I$ROOT/pico"
[ "${SAN:-0}" = 1 ] && FL="$FL -fsanitize=address,undefined -fno-omit-frame-pointer"
SRCS=()
for f in $RC_SRCS; do SRCS+=("$RC/$f"); done
gcc $FL -w -o "$OUT/ra_harness" "${SRCS[@]}" "$LRC/utils/md5.c" \
	"$ROOT/pico/ra_core.c" "$ROOT/tools/host_libc.c" "$ROOT/tools/ra_harness.c" -lm
"$OUT/ra_harness"
