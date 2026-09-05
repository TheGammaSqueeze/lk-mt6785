#!/bin/bash
# Build the Genesis-Plus-GX core as a standalone LOADABLE BLOB for boot_b (genesis_core_abi.h).
# Links the GPGX objects (build_core.sh -> libgenesis.a) + the ABI export provider
# (genesis_core_exports.c) + the bump-allocator shim (genesis_shim.c) + a bundled libc
# (genesis_blob_libc.c) + freestanding stubs (genesis_blob_stubs.c) + libm + libgcc into a flat
# binary at a fixed DRAM VMA (genesis_core_blob.ld), with a 20-byte header LK reads to find the
# entry and BSS span. GPGX is C, so NO libstdc++. Output: core_genesis.blob.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CC=arm-none-eabi-gcc
OC=arm-none-eabi-objcopy
OBJDIR="$DIR/obj"
mkdir -p "$OBJDIR"

CORE="$DIR/core"
INC="-I$CORE -I$CORE/z80 -I$CORE/m68k -I$CORE/ntsc -I$CORE/sound -I$CORE/input_hw \
     -I$CORE/cd_hw -I$CORE/cart_hw -I$CORE/cart_hw/svp \
     -I$DIR/libretro -I$DIR/libretro/libretro-common/include"
DEFS="-DLSB_FIRST -DBYTE_ORDER=LITTLE_ENDIAN -D__LIBRETRO__ -DALIGN_LONG -DALIGN_WORD \
      -DMAXROMSIZE=33554432 -DUSE_DYNAMIC_ALLOC \
      -DUSE_16BPP_RENDERING -DFRONTEND_SUPPORTS_RGB565 -DHAVE_YM3438_CORE -DHAVE_OPLL_CORE"
# Match build_core.sh ISA/tune (LTO re-optimizes at link; -fno-ipa-icf dodges the GCC10.3 LTO segfault).
COMMON="-march=armv8-a -mtune=cortex-a76 -mfloat-abi=soft -mthumb-interwork -ffreestanding \
        -fno-short-enums -mno-unaligned-access -fno-strict-aliasing -Os -fno-common \
        -flto -ffat-lto-objects -fno-ipa-icf -ffunction-sections -fdata-sections \
        -std=gnu99 $DEFS $INC"
# libc + stubs are compiled NON-LTO (plain ELF objects). Their memcpy/memset bodies are simple
# byte loops; under -O2 -flto GCC's loop-distribute pass rewrites those loops into a call to
# memcpy, and with the definition itself in the LTO partition that self-call left `memcpy'
# undefined at the flat-blob link. As concrete objects their memcpy/memset are final symbols the
# LTO'd core/exports resolve against. -fno-tree-loop-distribute-patterns/-fno-builtin belt-and-braces.
COMMON_NOLTO="-march=armv8-a -mtune=cortex-a76 -mfloat-abi=soft -mthumb-interwork -ffreestanding \
        -fno-short-enums -mno-unaligned-access -fno-strict-aliasing -Os -fno-common \
        -fno-tree-loop-distribute-patterns -fno-builtin -ffunction-sections -fdata-sections \
        -std=gnu99 $DEFS $INC"

[ -f "$DIR/libgenesis.a" ] || { echo "missing $DIR/libgenesis.a - run emu/genesis/build_core.sh first" >&2; exit 1; }

$CC $COMMON       -Wall -c "$DIR/genesis_core_exports.c" -o "$OBJDIR/genesis_core_exports.o"
$CC $COMMON       -Wall -c "$DIR/genesis_shim.c"         -o "$OBJDIR/genesis_shim.o"
$CC $COMMON_NOLTO -w    -c "$DIR/genesis_blob_libc.c"    -o "$OBJDIR/genesis_blob_libc.o"
$CC $COMMON_NOLTO -w    -c "$DIR/genesis_blob_stubs.c"   -o "$OBJDIR/genesis_blob_stubs.o"

LIBGCC=$($CC $COMMON -print-libgcc-file-name)
LIBM=$($CC $COMMON -print-file-name=libm.a)

$CC $COMMON -O2 -nostdlib -nostartfiles \
	-Wl,-T,"$DIR/genesis_core_blob.ld" -Wl,--gc-sections \
	"$OBJDIR/genesis_core_exports.o" "$OBJDIR/genesis_shim.o" "$OBJDIR/genesis_blob_libc.o" "$OBJDIR/genesis_blob_stubs.o" \
	-Wl,--start-group "$DIR/libgenesis.a" "$LIBM" "$LIBGCC" -Wl,--end-group \
	-o "$OBJDIR/core_genesis.blob.elf"
$OC -O binary "$OBJDIR/core_genesis.blob.elf" "$DIR/core_genesis.blob"

sz=$(stat -c%s "$DIR/core_genesis.blob")
span=$(od -An -tx4 -j16 -N4 "$DIR/core_genesis.blob" | tr -d ' ')
echo "built $DIR/core_genesis.blob : $sz bytes flat (~$((sz/1024)) KiB), DRAM span 0x$span (~$((0x$span/1024/1024)) MiB incl BSS)"
echo "header:"; od -An -tx4 -N20 "$DIR/core_genesis.blob"
