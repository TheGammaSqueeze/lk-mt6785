#!/bin/bash
# Autonomous flicker probe for the GBA SNES-mini menu. Drives the live menu over the
# USB debug channel and reads the peak render time after each movement. Any peak over
# 16700us (16.7ms) is a frame that missed the 60fps vsync = a visible hitch/flicker.
#
# The peak metric (g_dbg_peak_us) holds the max over ~2s then decays to current, so we
# idle ~2.5s before a test to clear stale peaks, do the movement burst, then read peak
# immediately so it reflects THAT movement.
#
#   tools/ayaneo/gba/flicker_probe.sh
S="fastboot -s 0123456789ABCDEF"
BUD=16700

diag() { timeout 12 $S oem diag 2>&1 | grep -o 'peak=[0-9]*us' | head -1 | tr -dc '0-9'; }
key()  { timeout 12 $S oem key:$1 >/dev/null 2>&1; }
idle() { sleep "${1:-2.7}"; }
verdict() { if [ "$1" -gt "$BUD" ]; then echo "OVER($1us)"; else echo "ok($1us)"; fi; }

echo "== flicker probe =="
# get to a known home state: a few cancels
for i in 1 2 3; do key B; done
idle 3

echo -n "idle home          : "; p=$(diag); verdict "$p"
# carousel scroll right/left bursts
for i in 1 2 3 4 5 6; do key R; done; echo -n "carousel scroll R  : "; p=$(diag); verdict "$p"
for i in 1 2 3 4 5 6; do key L; done; echo -n "carousel scroll L  : "; p=$(diag); verdict "$p"
# shoulder page-jumps
key ']'; key ']'; echo -n "page-jump R (])    : "; p=$(diag); verdict "$p"
key '['; key '['; echo -n "page-jump L ([)    : "; p=$(diag); verdict "$p"
# up into the menubar, down back
idle; key U; echo -n "up to menubar      : "; p=$(diag); verdict "$p"
key L; key R; echo -n "menubar L/R        : "; p=$(diag); verdict "$p"
key D; echo -n "down to carousel   : "; p=$(diag); verdict "$p"
# start = options/suspend submenu enter/exit
idle; key T; echo -n "submenu enter (T)  : "; p=$(diag); verdict "$p"
key D; key U; echo -n "submenu nav U/D    : "; p=$(diag); verdict "$p"
key B; echo -n "submenu exit (B)   : "; p=$(diag); verdict "$p"
# select = sort
idle; key S; echo -n "select/sort (S)    : "; p=$(diag); verdict "$p"
echo "== done =="
