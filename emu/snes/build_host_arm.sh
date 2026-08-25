#!/bin/bash
# Build the host render harness for ARM+NEON so the NEON blit can be validated
# under qemu-arm-static against the web reference (the x86 host build can't run NEON).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
arm-linux-gnueabihf-gcc -O3 -mfpu=neon -mcpu=cortex-a55 -ffast-math -I"$DIR" \
    -o "$DIR/host_render_arm" \
    "$DIR/snes_pack.c" "$DIR/snes_scene.c" "$DIR/snes_render.c" \
    "$DIR/snes_menu.c" "$DIR/snes_audio.c" "$DIR/host_render.c" -lm -static
echo "built $DIR/host_render_arm (run via qemu-arm-static)"
