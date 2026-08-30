#!/bin/bash
# Build the render harness for ARM with NEON so the DEVICE blitter path
# (snes_render.c #ifdef __ARM_NEON, the 565->8888 8px/iter upscale + tint) is
# exercised off-device under qemu-arm. The x86 build_host.sh only compiles the
# scalar fallback, so a NEON-specific pixel bug would otherwise slip to the device.
# Run the result with `qemu-arm-static ./host_render_arm ...`.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
CC="${ARM_CC:-arm-linux-gnueabihf-gcc}"
# NEON on; hard-float ABI (the gnueabihf toolchain default) - the ABI only affects
# call conventions, not the integer-NEON pixel math being validated.
# static-linked so qemu-arm-static needs no ARM shared libs / dynamic loader present
"$CC" -O2 -static -mfpu=neon -mfloat-abi=hard -march=armv7-a -I"$DIR" -o "$DIR/host_render_arm" \
    "$DIR/snes_pack.c" "$DIR/snes_scene.c" "$DIR/snes_render.c" \
    "$DIR/snes_menu.c" "$DIR/snes_audio.c" "$DIR/host_render.c" -lm
echo "built $DIR/host_render_arm (ARM+NEON; run under qemu-arm-static)"
