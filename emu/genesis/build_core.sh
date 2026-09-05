#!/bin/bash
# Build the Genesis-Plus-GX libretro core (Sega MD/SMS/GG/SG-1000) into a static
# archive for the loadable boot_b blob. Mirrors emu/snes9x/build_core.sh: freestanding
# arm, soft-float, LTO, hot/cold -O2/-Os split to fit the boot_b slot. The ROM is fed as
# a buffer via retro_load_game (no file I/O). Sega CD image loading (CHD/mp3/vorbis) is
# NOT built; the cd_hw HARDWARE compiles but is never activated (no CD BIOS/game loads).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CC=arm-none-eabi-gcc
AR=arm-none-eabi-ar

CORE="$DIR/core"
INC="-I$CORE -I$CORE/z80 -I$CORE/m68k -I$CORE/ntsc -I$CORE/sound -I$CORE/input_hw \
     -I$CORE/cd_hw -I$CORE/cart_hw -I$CORE/cart_hw/svp \
     -I$DIR/libretro -I$DIR/libretro/libretro-common/include"

# ARM defines from GPGX Makefile.libretro (Switch/ARM target), minus CD image deps.
# NO HAVE_CHD / HAVE_ZLIB (raw ROM buffers from SD, no zip/chd). RGB565 output.
DEFS="-DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -D__LIBRETRO__ -DALIGN_LONG -DALIGN_WORD \
      -DM68K_OVERCLOCK_SHIFT=20 -DZ80_OVERCLOCK_SHIFT=20 -DMAXROMSIZE=33554432 \
      -DUSE_DYNAMIC_ALLOC \
      -DUSE_16BPP_RENDERING -DFRONTEND_SUPPORTS_RGB565 -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE"

COMMON="-march=armv8-a -mtune=cortex-a76 -mfloat-abi=soft -mthumb-interwork -ffreestanding \
        -fno-short-enums -mno-unaligned-access -fno-strict-aliasing -fno-common \
        -flto -ffat-lto-objects -fno-ipa-icf \
        -ffunction-sections -fdata-sections -std=gnu99 $DEFS $INC"
OPT_HOT="-O3"
OPT_COLD="-O2"

# HOT files: run every frame (68000, Z80, VDP render/ctrl, sound) -> -O3; everything else -O2.
# NOTE: the `rel` passed here is the find(1) path WITH the leading "core/" (e.g. core/m68k/m68kcpu),
# so HOT_SET entries must match after stripping that prefix - opt_for strips it. The old code compared
# the "core/..."-prefixed rel against un-prefixed HOT_SET entries, so NOTHING matched and the ENTIRE
# core built at -Os (a ~2-3x emulation slowdown). Both the prefix strip and the -Os->-O2 cold floor
# below are the fix. Extra hot loops added (vdp_sms, cart mappers seldom hot but cheap to include).
HOT_SET=" m68k/m68kcpu z80/z80 vdp_render vdp_ctrl vdp_sms system sound/sound sound/ym2612 sound/sn76489 sound/ym2413 sound/blip_buf sound/psg mem68k memz80 membnk io_ctrl input_hw/input "
opt_for() { r="${1#core/}"; case "$HOT_SET" in *" $r "*) echo "$OPT_HOT";; *) echo "$OPT_COLD";; esac; }

OBJDIR="$DIR/obj"
rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

# All core .c recursively EXCEPT minimp3 (CD MP3 audio, SIMD/float - not built) and any
# nested libchdr (removed). Plus the libretro entry (libretro.c).
SRCS="$(cd "$DIR" && find core -name '*.c' | grep -v '/minimp3/' | grep -v '/libchdr/' | grep -v 'yx5200'; echo libretro/libretro.c)"

n=0
for f in $SRCS; do
	rel="${f%.c}"
	o="$OBJDIR/$(echo "$rel" | tr '/' '_').o"
	$CC $COMMON $(opt_for "$rel") -c "$DIR/$f" -o "$o"
	n=$((n+1))
done

rm -f "$DIR/libgenesis.a"
$AR rcs "$DIR/libgenesis.a" "$OBJDIR"/*.o
echo "built $DIR/libgenesis.a ($(du -h "$DIR/libgenesis.a" | cut -f1), $n objects)"
