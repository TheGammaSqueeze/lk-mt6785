#!/bin/bash
# Build the snes9x libretro core into a static archive for the loadable boot_b blob.
# Mirrors emu/gbc/build_core.sh: arm-none-eabi-g++, 32-bit armv7-a soft-float, freestanding.
# The ENTIRE upstream core compiles with these flags (verified); file I/O (zip/VFS/MSU1
# packs) is deliberately excluded and stubbed on the blob side - the ROM is fed as a
# buffer via the libretro retro_load_game path, exactly like the gambatte port.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CXX=arm-none-eabi-g++
CC=arm-none-eabi-gcc
AR=arm-none-eabi-ar

INC="-I$DIR -I$DIR/apu -I$DIR/apu/bapu -I$DIR/libretro -I$DIR/libretro/libretro-common/include"
COMMON="-march=armv7-a -mfloat-abi=soft -mthumb-interwork -ffreestanding \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
        -fno-short-enums -mno-unaligned-access -Os -fno-common -D__LIBRETRO__ $INC"
CXXFLAGS="$COMMON"
CFLAGS="$COMMON -std=gnu99"

OBJDIR="$DIR/obj"
rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

# C sources (special chips, NTSC filter, DSP). zipfile.c is excluded (file I/O).
C_SRCS="c4emu msu1 srtc obc1 bsflash tile bsx spc7110 fxemu sdd1 seta sa1hw dsp filter/snes_ntsc"
# C++ sources (CPU/PPU/APU/DMA/GFX + libretro entry). loadzip.cpp excluded (zip I/O).
CXX_SRCS="apu/apu apu/bapu/dsp/sdsp apu/bapu/smp/smp apu/bapu/smp/smp_state \
          cheats2 clip controls cpu cpuexec cpuops crosshairs dma gfx globals \
          memmap ppu sa1 sa1cpu snapshot sha256 bml fscompat libretro/libretro"

for f in $C_SRCS; do
	o="$OBJDIR/$(echo $f | tr '/' '_').o"
	$CC $CFLAGS -c "$DIR/$f.c" -o "$o"
done
for f in $CXX_SRCS; do
	o="$OBJDIR/$(echo $f | tr '/' '_').o"
	$CXX $CXXFLAGS -c "$DIR/$f.cpp" -o "$o"
done

rm -f "$DIR/libsnes9x.a"
$AR rcs "$DIR/libsnes9x.a" "$OBJDIR"/*.o
echo "built $DIR/libsnes9x.a ($(du -h "$DIR/libsnes9x.a" | cut -f1), $(ls "$OBJDIR"/*.o | wc -l) objects)"

# Report the external symbols the blob's libc/shim/libgcc/libstdc++ must satisfy
# (internal snes9x cross-refs like Memory/CPU/PPU resolve within the archive).
echo "=== external undefined symbols (blob must provide) ==="
arm-none-eabi-nm "$DIR/libsnes9x.a" 2>/dev/null | awk '$1=="U"{print $2}' | sort -u \
  | grep -vE '^(Memory|Settings|CPU|PPU|SA1|IPPU|ICPU|GFX|DMA|Timings|Multi|OpenBus|Registers|RTCData|S9x|Sett)' | head -80
