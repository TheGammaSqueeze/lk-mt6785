#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 TheGammaSqueeze
# ra-pico: build the RetroAchievements blob for the GammaOS LK (Konkr Pocket Block and Advance).
#
#   ./build.sh            -> out/ra-pico.blob (copy it to the SD card as /cores/ra-pico.blob)
#   ./build.sh clean
#   tools/harness.sh      -> the same glue and rcheevos on the PC, against a test server
#
# Built: RetroAchievements' rcheevos (rc_client, its server calls, the achievement runtime, ROM
# hashing for cartridge systems, rc_libretro's memory maps), libretro-common's MD5 that RetroArch's
# rcheevos uses, the glue in pico/ra_core.c and the C runtime in pico/pico_libc.c (sources.sh).
# rcheevos is compiled unchanged.
#
# Target: the LK calls the blob in 32 bit ARM state on a Cortex-A55 (MT6768 and MT6785): ARMv8-A,
# hardware floating point with the softfp calling convention, as the core blobs. -Os: the
# achievements are checked once a frame and cost little; the size matters more.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
OUT=$ROOT/out
CC=${CC:-arm-none-eabi-gcc}
OBJCOPY=${OBJCOPY:-arm-none-eabi-objcopy}
export LC_ALL=C

if [ "${1:-}" = clean ]; then rm -rf "$OUT"; exit 0; fi
. "$ROOT/sources.sh"

RC_REV=$(sed -n 's/^#define RCHEEVOS_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)/\2/p' "$RC/src/rc_version.h" | paste -sd.)
PICO_REV=$(git describe --always --dirty 2>/dev/null || echo unknown)
VERSION="rcheevos ${RC_REV} (pico ${PICO_REV})"

rm -rf "$OUT/obj"
mkdir -p "$OUT/obj"
TUNE=${TUNE:-cortex-a55}
ARCH="-march=armv8-a+simd -mtune=$TUNE -marm -mfpu=neon-fp-armv8 -mfloat-abi=softfp"
BASE="$ARCH -std=gnu11 ${OPT:--Os} -flto -ffunction-sections -fdata-sections -fno-common -DNDEBUG $RC_DEFS ${EXTRA_CFLAGS:-}"
# GCC's own stdint.h comes before newlib's here, so newlib's inttypes.h never sees the 64 bit types
# and leaves PRId64 out, which libretro-common's headers insist on: newlib's types first
INC="-include sys/_stdint.h $RC_INC -I$ROOT/pico -I$LRC/include"
# the C runtime outside LTO (newlib's members reference its symbols only at the final link), and
# -fno-builtin as any malloc must be
RT_FLAGS="${BASE/-flto/} -fno-builtin -Wall -Wextra -I$ROOT/pico"

OBJS=()
obj() { echo "$OUT/obj/$(echo "${1%.c}" | tr '/' '_').o"; }
for f in $RC_SRCS; do
	o=$(obj "$f")
	$CC $BASE -w $INC -c "$RC/$f" -o "$o"
	OBJS+=("$o")
done
$CC $BASE -Wall -Wextra -Wno-unused-parameter $INC -c "$ROOT/pico/ra_core.c" -o "$OUT/obj/ra_core.o"
$CC $RT_FLAGS -c "$ROOT/pico/pico_libc.c" -o "$OUT/obj/pico_libc.o"
OBJS+=("$OUT/obj/ra_core.o" "$OUT/obj/pico_libc.o")

$CC $BASE -nostartfiles -Wl,--gc-sections -Wl,-T,"$ROOT/pico/blob.ld" -Wl,-Map,"$OUT/ra-pico.map" \
	-o "$OUT/ra-pico.elf" "${OBJS[@]}" -lm -lc -lgcc
$OBJCOPY -O binary "$OUT/ra-pico.elf" "$OUT/ra-pico.blob"

python3 - "$OUT/ra-pico.blob" "$VERSION" <<'PY'
import struct, sys, hashlib
d = open(sys.argv[1], 'rb').read()
magic, ver, entry, load, span = struct.unpack_from('<5I', d)
assert magic == 0x31414152 and ver == 2, "bad header"
assert load == len(d), "load size %d != file %d" % (load, len(d))
assert entry < load, "entry outside the image"
assert span <= 0x100000, "span %d is over the first MiB of the slot" % span
print("ra-pico.blob: %s" % sys.argv[2])
print("  file %d bytes, span %d bytes, heap %d bytes, entry +0x%x" % (len(d), span, 0x400000 - span, entry))
print("  sha256 %s" % hashlib.sha256(d).hexdigest())
PY
