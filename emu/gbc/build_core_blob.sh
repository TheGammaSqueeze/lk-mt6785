#!/bin/bash
# Build the gambatte GB/GBC core as a standalone LOADABLE BLOB for boot_b (gbc_core_abi.h).
#
# Links the gambatte objects (built by build_core.sh into obj/) + the ABI export provider
# (gbc_core_exports.cpp) + a bundled minimal libc (gbc_blob_libc.c) + libgcc + libm into a
# flat binary at a fixed DRAM VMA (gbc_core_blob.ld), with a 20-byte header LK reads to find
# the entry and BSS span. Output: core_gbc.blob. Self-contained: the only external calls are
# the 2 imports (read_buttons, host_time) LK passes to gbc_core_blob_init().
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CXX=arm-none-eabi-g++
CC=arm-none-eabi-gcc
LD=arm-none-eabi-ld
OC=arm-none-eabi-objcopy
OBJDIR="$DIR/obj"
CORE="$DIR/libgambatte"

INC="-I$CORE/src -I$CORE/include -I$DIR/common -I$DIR/common/resample \
     -I$CORE/libretro -I$CORE/libretro-common/include -I$DIR"
CXXFLAGS="-march=armv7-a -mfloat-abi=soft -mthumb-interwork -ffreestanding \
       -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
       -fno-short-enums -mno-unaligned-access -Os -fno-common -D__LIBRETRO__ -DVIDEO_RGB565 $INC"
CFLAGS="-march=armv7-a -mfloat-abi=soft -mthumb-interwork -ffreestanding \
       -fno-builtin -fno-common -fno-short-enums -Os -I$DIR"

# The gambatte core objects must already be built by build_core.sh.
[ -f "$OBJDIR/cpu.o" ] || { echo "missing $OBJDIR/*.o - run emu/gbc/build_core.sh first" >&2; exit 1; }

# ABI export provider (C++) + bundled libc (C).
$CXX $CXXFLAGS -Wall -c "$DIR/gbc_core_exports.cpp" -o "$OBJDIR/gbc_core_exports.o"
$CC  $CFLAGS   -Wall -c "$DIR/gbc_blob_libc.c"       -o "$OBJDIR/gbc_blob_libc.o"

LIBGCC=$($CC $CFLAGS -print-libgcc-file-name)
LIBM=$($CC $CFLAGS -print-file-name=libm.a)

# Link every gambatte object (obj/*.o) + our two + libgcc + libm at the fixed VMA.
$LD -T "$DIR/gbc_core_blob.ld" "$OBJDIR"/*.o "$LIBM" "$LIBGCC" -o "$OBJDIR/core_gbc.blob.elf"
$OC -O binary "$OBJDIR/core_gbc.blob.elf" "$DIR/core_gbc.blob"

sz=$(stat -c%s "$DIR/core_gbc.blob")
span=$(od -An -tx4 -j16 -N4 "$DIR/core_gbc.blob" | tr -d ' ')
echo "built $DIR/core_gbc.blob : $sz bytes flat (~$((sz/1024)) KiB), DRAM span 0x$span (~$((0x$span/1024/1024)) MiB incl BSS)"
echo "header:"; od -An -tx4 -N20 "$DIR/core_gbc.blob"
