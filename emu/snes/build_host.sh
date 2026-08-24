#!/bin/bash
# Build the host render harness (validates the portable engine against the web app).
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
gcc -O2 -I"$DIR" -o "$DIR/host_render" \
    "$DIR/snes_pack.c" "$DIR/snes_scene.c" "$DIR/snes_render.c" \
    "$DIR/snes_menu.c" "$DIR/snes_audio.c" "$DIR/host_render.c" -lm
echo "built $DIR/host_render"
