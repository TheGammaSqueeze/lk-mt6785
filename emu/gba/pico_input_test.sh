#!/bin/bash
# pico_input_test.sh - pico_input.c on the PC (pico_input_test.c), with the address and undefined
# behaviour checkers
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -DPICO_INPUT_TEST -I"$DIR" \
	-o "$TMP/t" "$DIR/pico_input_test.c" "$DIR/pico_input.c"
mkdir -p "$TMP/card"
"$TMP/t" "$TMP/card"
