#!/bin/bash
# Autonomous flicker probe for the GBA SNES-mini menu. Drives the live menu over the
# USB debug channel and reads the peak render time after each movement. Any peak over
# 16700us (16.7ms) is a frame that missed the 60fps vsync = a visible hitch/flicker.
#
# GUARDS (learned the hard way):
#  - NEVER inject A or Start here: on a focused game card they LAUNCH the game, which
#    tears down the menu (and the metrics). Navigation only: L R U D [ ] and B to back.
#  - Detect a dead USB link ("no link" / empty diag) and STOP instead of hammering a
#    wedged device; print WEDGED so the caller can power-cycle/reboot.
#
#   tools/ayaneo/gba/flicker_probe.sh
S="fastboot -s 0123456789ABCDEF"
BUD=16700
FAIL=0

raw_diag() { timeout 10 $S oem diag 2>&1; }
diag() {
	local o; o=$(raw_diag)
	if echo "$o" | grep -qiE "no link|FAILED|error"; then echo "WEDGED"; return 1; fi
	echo "$o" | grep -o 'peak=[0-9]*us' | head -1 | tr -dc '0-9'
}
key()  { timeout 10 $S oem key:"$1" >/dev/null 2>&1; }
verdict() {
	if [ "$1" = "WEDGED" ] || [ -z "$1" ]; then echo "LINK-DEAD"; FAIL=1; return 1; fi
	if [ "$1" -gt "$BUD" ]; then echo "OVER  ${1}us  <<< FLICKER"; FAIL=1; else echo "ok    ${1}us"; fi
}
measure() { local label="$1"; shift; for k in "$@"; do key "$k"; done; printf "%-22s: " "$label"; local p; p=$(diag); verdict "$p"; }

echo "== flicker probe =="
# known home state
for i in 1 2 3; do key B; done; sleep 3
printf "%-22s: " "idle home"; p=$(diag); verdict "$p" || { echo "== abort (link dead) =="; exit 2; }

measure "carousel scroll R" R R R R R
measure "carousel scroll L" L L L L L
measure "page-jump R (])"   ']' ']'
measure "page-jump L ([)"   '[' '['
measure "up to menubar"     U
measure "menubar L/R"       L R
measure "down to carousel"  D
measure "resume panel (D)"  D
measure "resume nav U/D"    U D
measure "back out (B)"      B

echo "== done (FAIL=$FAIL) =="
exit $FAIL
