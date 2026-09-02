#!/bin/bash
# Build the snes9x core as a standalone LOADABLE BLOB for boot_b (snes_core_abi.h).
# Links the snes9x objects (build_core.sh -> libsnes9x.a) + the ABI export provider
# (snes_core_exports.cpp) + the C++ runtime shim (snes_shim.cpp) + a bundled libc
# (snes_blob_libc.c) + libstdc++ + libm + libgcc into a flat binary at a fixed DRAM VMA
# (snes_core_blob.ld), with a 20-byte header LK reads to find the entry and BSS span.
# Output: core_snes.blob.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CXX=arm-none-eabi-g++
CC=arm-none-eabi-gcc
LD=arm-none-eabi-ld
OC=arm-none-eabi-objcopy
OBJDIR="$DIR/obj"

INC="-I$DIR -I$DIR/apu -I$DIR/apu/bapu -I$DIR/libretro -I$DIR/libretro/libretro-common/include"
COMMON="-march=armv7-a -mfloat-abi=soft -mthumb-interwork -ffreestanding \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
        -fno-short-enums -mno-unaligned-access -Os -fno-common -D__LIBRETRO__ $INC"

# core archive must exist (build_core.sh)
[ -f "$DIR/libsnes9x.a" ] || { echo "missing $DIR/libsnes9x.a - run emu/snes9x/build_core.sh first" >&2; exit 1; }

$CXX $COMMON -Wall -c "$DIR/snes_core_exports.cpp" -o "$OBJDIR/snes_core_exports.o"
$CXX $COMMON -Wall -c "$DIR/snes_shim.cpp"         -o "$OBJDIR/snes_shim.o"
$CC  $COMMON -std=gnu99 -Wall -c "$DIR/snes_blob_libc.c"  -o "$OBJDIR/snes_blob_libc.o"
$CC  $COMMON -std=gnu99 -w    -c "$DIR/snes_blob_stubs.c" -o "$OBJDIR/snes_blob_stubs.o"

LIBGCC=$($CC $COMMON -print-libgcc-file-name)
LIBM=$($CC $COMMON -print-file-name=libm.a)
LIBSTDCXX=$($CXX $COMMON -print-file-name=libstdc++.a)

# Flat link at the fixed VMA. --gc-sections trims unreferenced libstdc++/core objects.
# libstdc++/libm/libgcc are archives (pulled on demand); our objects + libsnes9x.a first.
$LD -T "$DIR/snes_core_blob.ld" --gc-sections \
	"$OBJDIR/snes_core_exports.o" "$OBJDIR/snes_shim.o" "$OBJDIR/snes_blob_libc.o" "$OBJDIR/snes_blob_stubs.o" \
	--start-group "$DIR/libsnes9x.a" "$LIBSTDCXX" "$LIBM" "$LIBGCC" --end-group \
	-o "$OBJDIR/core_snes.blob.elf"
$OC -O binary "$OBJDIR/core_snes.blob.elf" "$DIR/core_snes.blob"

sz=$(stat -c%s "$DIR/core_snes.blob")
span=$(od -An -tx4 -j16 -N4 "$DIR/core_snes.blob" | tr -d ' ')
echo "built $DIR/core_snes.blob : $sz bytes flat (~$((sz/1024)) KiB), DRAM span 0x$span (~$((0x$span/1024/1024)) MiB incl BSS)"
echo "header:"; od -An -tx4 -N20 "$DIR/core_snes.blob"
