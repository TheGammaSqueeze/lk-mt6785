#!/bin/bash
#
# Push a staged LK image to the target over fastboot, or query it. The target must
# already be in bootloader/fastboot mode (the emulator builds have no Android, so
# adb is unavailable on them - fastboot is the interface; SP Flash Tool via the
# preloader is the last-resort fallback).
#
# To enter fastboot: from a working system `adb reboot bootloader`, or from another
# fastboot `fastboot reboot bootloader`, or the device key combo at power-on. On
# this MT6785 the boot mode is chosen by boot_mode_select() before the emu hook, so
# the emu build keeps fastboot reachable.
#
# Usage:
#   fastboot_push.sh                 # just query (getvar all)
#   fastboot_push.sh <image> [part]  # flash <image> to <part> (default part lk_a)
#
set -e
IMG="$1"
PART="${2:-lk_a}"

if ! fastboot devices | grep -q .; then
	echo "No fastboot device found."
	echo "Put the target in bootloader mode (adb/fastboot reboot bootloader, or the"
	echo "power-on key combo) and connect USB, then re-run."
	exit 1
fi

echo "== fastboot device(s) =="
fastboot devices
echo "== getvar =="
fastboot getvar product 2>&1 | head -1 || true
fastboot getvar current-slot 2>&1 | head -1 || true

if [ -z "$IMG" ]; then
	echo "(query only - pass an image path to flash)"
	exit 0
fi
if [ ! -f "$IMG" ]; then
	echo "image not found: $IMG"; exit 1
fi

echo "== flashing $IMG -> partition $PART =="
fastboot flash "$PART" "$IMG"
echo "Done. Run 'fastboot reboot' to boot, or flash more partitions first."
