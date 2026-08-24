#!/bin/bash
# Build the gpSP GBA core (with the ARM dynarec) + runtime shim into a static
# archive the LK link step pulls in. Compiled with arm-none-eabi-gcc, ABI-matched
# to LK's 32-bit armv7-a soft-float build.
#
# Key flags:
#   -DARM_ARCH -DHAVE_DYNAREC : enable the ARM dynamic recompiler (arm/arm_stub.S)
#   -DHAVE_MMAP               : make the translation caches plain pointer globals
#                               (cpu_threaded.c) that gba_wrap.c points into the
#                               DRAM arena; avoids the 11 MB static BSS arrays that
#                               would never fit LK's tiny heap/BSS.
#   -mno-unaligned-access     : LK runs with strict alignment; the emulator faults
#                               otherwise (learned on the GBC core).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CC=arm-none-eabi-gcc
AR=arm-none-eabi-ar

# The GBA BIOS header is copyrighted and not committed; generate it from your own
# dumped BIOS with gen_bios_header.py before building the LK image.
if [ ! -f "$DIR/gba_bios_data.h" ]; then
	echo "note: emu/gba/gba_bios_data.h missing - the LK build (gba_driver.c) needs it."
	echo "      generate it with: $DIR/gen_bios_header.py /path/to/gba_bios.bin"
fi

FLAGS="-march=armv7-a -marm -mfloat-abi=soft -mno-unaligned-access \
       -ffreestanding -fno-builtin -fno-common -fno-short-enums -O2 \
       -DARM_ARCH -DHAVE_DYNAREC -DHAVE_MMAP -DGBA_LK_SMALL_CACHE \
       -I$DIR -I$DIR/arm"

OBJDIR="$DIR/obj"
rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

# Core C sources (libretro.c / memmap_win32.c / gpsp_griffin.c intentionally
# excluded - we drive the core directly from gba_wrap.c).
SRCS="cpu cpu_threaded gba_memory main video sound input cheats gba_cc_lut"

for f in $SRCS; do
	$CC $FLAGS -c "$DIR/$f.c" -o "$OBJDIR/$f.o"
done

# dynarec assembly dispatch
$CC $FLAGS -c "$DIR/arm/arm_stub.S" -o "$OBJDIR/arm_stub.o"

# runtime shim + LK-side bridge
$CC $FLAGS -c "$DIR/gba_shim.c" -o "$OBJDIR/gba_shim.o"
$CC $FLAGS -c "$DIR/gba_wrap.c" -o "$OBJDIR/gba_wrap.o"

rm -f "$DIR/libgpsp.a"
$AR rcs "$DIR/libgpsp.a" "$OBJDIR"/*.o
echo "built $DIR/libgpsp.a ($(du -h "$DIR/libgpsp.a" | cut -f1))"

echo "=== undefined symbols (LK link must satisfy) ==="
$AR t "$DIR/libgpsp.a" >/dev/null
arm-none-eabi-nm "$DIR/libgpsp.a" 2>/dev/null | awk '$1=="U"{print $2}' | sort -u
