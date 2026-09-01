#!/bin/bash
# Build the gpSP GBA core as a standalone LOADABLE BLOB for boot_b (see gba_core_abi.h).
#
# Unlike build_core_gba.sh (which emits libgpsp.a to link INTO lk_a), this links the core
# + its ABI export provider (gba_core_exports.c) + a bundled minimal libc (blob_libc.c) +
# libgcc into a flat binary at a fixed DRAM VMA (core_blob.ld), with a 16-byte header LK
# reads to find the entry and BSS span. Output: core_gba.blob.
#
# The blob is fully self-contained: no newlib, no LK symbols except the 3 imports it hops
# through the imports table LK passes to gba_core_blob_init().
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CC=arm-none-eabi-gcc
LD=arm-none-eabi-ld
OC=arm-none-eabi-objcopy
OBJDIR="$DIR/obj"

FLAGS="-march=armv7-a -marm -mfloat-abi=soft -mno-unaligned-access \
       -ffreestanding -fno-builtin -fno-common -fno-short-enums -O2 \
       -DARM_ARCH -DHAVE_DYNAREC -DHAVE_MMAP -DGBA_LK_SMALL_CACHE \
       -I$DIR -I$DIR/arm"

# Core objects (must be built already by build_core_gba.sh, which populates $OBJDIR).
CORE_SRCS="cpu cpu_threaded gba_memory main video sound input cheats"
for f in $CORE_SRCS; do
	[ -f "$OBJDIR/$f.o" ] || { echo "missing $OBJDIR/$f.o - run build_core_gba.sh first" >&2; exit 1; }
done

# ABI export provider + bundled libc (blob-only).
$CC $FLAGS -Wall -c "$DIR/gba_core_exports.c" -o "$OBJDIR/gba_core_exports.o"
$CC $FLAGS -Wall -c "$DIR/blob_libc.c"        -o "$OBJDIR/blob_libc.o"

# The runtime shim (gba_shim.o) and arm_stub.o come from build_core_gba.sh too.
LIBGCC=$($CC $FLAGS -print-libgcc-file-name)

$LD -T "$DIR/core_blob.ld" "$OBJDIR"/*.o "$LIBGCC" -o "$OBJDIR/core_gba.blob.elf"
$OC -O binary "$OBJDIR/core_gba.blob.elf" "$DIR/core_gba.blob"

# Report footprint (header: magic ver entry span).
sz=$(stat -c%s "$DIR/core_gba.blob")
span=$(od -An -tx4 -j16 -N4 "$DIR/core_gba.blob" | tr -d ' ')
echo "built $DIR/core_gba.blob : $sz bytes flat (~$((sz/1024)) KiB), DRAM span 0x$span (~$((0x$span/1024/1024)) MiB incl BSS)"
echo "header:"; od -An -tx4 -N16 "$DIR/core_gba.blob"
