#!/bin/bash
# Build the host core benchmark (core_bench.cpp, synthetic in-memory ROM) natively, reusing the
# core objects that emu/snes9x/build_host_test.sh compiles. Add -pg for a gprof profile build.
#   tools/ayaneo/snes/build_core_bench.sh [--prof]  &&  /tmp/s9x_core_bench [frames]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SNES="$DIR/../../../emu/snes9x"
PROF=""
[ "$1" = "--prof" ] && PROF="-pg"

# Compile the core objects if not present (build_host_test builds them into /tmp/s9x_host_obj).
if [ ! -d /tmp/s9x_host_obj ] || [ -z "$(ls -A /tmp/s9x_host_obj 2>/dev/null)" ]; then
	"$SNES/build_host_test.sh" >/dev/null 2>&1 || true
fi

INC="-I$SNES -I$SNES/apu -I$SNES/apu/bapu -I$SNES/libretro -I$SNES/libretro/libretro-common/include"
F="-D__LIBRETRO__ -DVIDEO_RGB565 -O2 -w -fno-strict-aliasing $PROF $INC"
g++ $F "$DIR/core_bench.cpp" /tmp/s9x_host_obj/*.o -o /tmp/s9x_core_bench -lm
echo "built /tmp/s9x_core_bench${PROF:+ (profiling)} - run: /tmp/s9x_core_bench [frames]"
