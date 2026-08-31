#!/bin/bash
# Capture the carousel MOTION off the device and assemble it into per-frame PNGs +
# an animated GIF, so movement flicker/judder is actually visible (a single settled
# screenshot cannot show motion). Uses the on-device motion-capture ring:
#   oem capmotion   grabs 6 consecutive presented frames during an injected scroll
#   oem getframe:i  dumps frame i as RGB565 hex
#
#   tools/ayaneo/gba/fastboot_motion.sh [static]
# Pass "static" to capture 6 steady frames (no scroll) to inspect idle frame-to-frame.
S="fastboot -s 0123456789ABCDEF"
OUT=/mnt/c/pairmini
HERE="$(cd "$(dirname "$0")" && pwd)"
MODE=""
[ "$1" = "static" ] && MODE=":s"

echo ">> capmotion$MODE"
info=$(timeout 20 $S oem capmotion$MODE 2>&1 | tr -d '\r' | grep -oE 'cap [0-9]+' | head -1)
n=$(echo "$info" | grep -oE '[0-9]+')
[ -z "$n" ] && { echo "!! capture failed (device link?)"; exit 1; }
echo ">> captured $n frames; pulling..."

rm -f "$OUT"/motion_*.png /tmp/mframe_*.txt
for i in $(seq 0 $((n - 1))); do
	timeout 40 $S oem getframe:$i 2>&1 | tr -d '\r' | grep -oE '[0-9]{3}[:+][0-9a-f]+' > /tmp/mframe_$i.txt
	python3 "$HERE/fastboot_menu_shot.py" "$OUT/motion_$(printf %02d $i).png" < /tmp/mframe_$i.txt >/dev/null 2>&1
done

if command -v convert >/dev/null 2>&1; then
	convert -delay 16 -loop 0 "$OUT"/motion_*.png "$OUT/motion.gif" && echo ">> wrote $OUT/motion.gif"
else
	echo ">> frames in $OUT/motion_*.png (install imagemagick for a gif)"
fi
