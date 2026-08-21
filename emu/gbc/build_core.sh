#!/bin/bash
# Build the gambatte GBC core + runtime shim into a static archive that the LK
# link step pulls in. Compiled with arm-none-eabi-g++ (which ships libstdc++
# headers), ABI-matched to LK's 32-bit armv7-a soft-float build.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CORE="$DIR/libgambatte"
CXX=arm-none-eabi-g++
AR=arm-none-eabi-ar

INC="-I$CORE/src -I$CORE/include -I$DIR/common -I$DIR/common/resample \
     -I$CORE/libretro -I$CORE/libretro-common/include"
FLAGS="-march=armv7-a -mfloat-abi=soft -mthumb-interwork -ffreestanding \
       -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
       -Os -fno-common -D__LIBRETRO__ -DVIDEO_RGB565 $INC"

OBJDIR="$DIR/obj"
rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

SRCS="bootloader cpu gambatte initstate interrupter interruptrequester \
      gambatte-memory sound statesaver tima video video_libretro \
      mem/cartridge mem/cartridge_libretro mem/huc3 mem/memptrs mem/rtc \
      sound/channel1 sound/channel2 sound/channel3 sound/channel4 \
      sound/duty_unit sound/envelope_unit sound/length_counter \
      video/ly_counter video/lyc_irq video/next_m0_time video/ppu \
      video/sprite_mapper"

for f in $SRCS; do
	o="$OBJDIR/$(echo $f | tr '/' '_').o"
	$CXX $FLAGS -c "$CORE/src/$f.cpp" -o "$o"
done
# runtime shim + C bridge
$CXX $FLAGS -c "$DIR/gbc_shim.cpp" -o "$OBJDIR/gbc_shim.o"
$CXX $FLAGS -c "$DIR/gbc_wrap.cpp" -o "$OBJDIR/gbc_wrap.o"

rm -f "$DIR/libgbc.a"
$AR rcs "$DIR/libgbc.a" "$OBJDIR"/*.o
echo "built $DIR/libgbc.a ($(du -h "$DIR/libgbc.a" | cut -f1))"

# report undefined symbols the LK link must satisfy (excluding our own)
echo "=== undefined symbols ==="
arm-none-eabi-nm "$DIR/libgbc.a" 2>/dev/null | awk '$1=="U"{print $2}' | sort -u
