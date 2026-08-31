#!/bin/bash
# Reliably flash lk_a over fastboot on device 0123456789ABCDEF.
#
# Two gotchas this handles:
#  1) The eMMC write state goes stale: a `flash` fails with "flash write failure"
#     unless a FRESH `reboot bootloader` session precedes it.
#  2) The fastboot client can HANG forever on `reboot`/`reboot bootloader` waiting
#     for an ack the emulator-USB gadget does not send. That hung client also holds
#     the USB so nothing else can enumerate. So every reboot is timeout-wrapped and
#     followed by `pkill -9 fastboot` (the reboot still takes effect on the device).
#
#   tools/ayaneo/gba/flash_lk.sh [image]
set +e
S="fastboot -s 0123456789ABCDEF"
IMG="${1:-/work/svoboda_lk/out/lk_a_gba_sd_signed.img}"

wait_fb() { for i in $(seq 1 45); do timeout 4 $S devices 2>&1 | grep -qi fastboot && return 0; sleep 3; done; return 1; }

echo ">> waiting for device on USB (fastboot or booting into the menu)..."
wait_fb || { echo "!! no device"; exit 1; }

echo ">> fresh bootloader session (clears stale eMMC write state)..."
timeout 12 $S reboot bootloader >/dev/null 2>&1
pkill -9 fastboot 2>/dev/null
sleep 3
wait_fb || { echo "!! device did not return to fastboot"; exit 1; }

echo ">> flashing lk_a <- $IMG"
timeout 45 $S flash lk_a "$IMG" 2>&1 | tail -2

echo ">> reboot into the menu..."
timeout 12 $S reboot >/dev/null 2>&1
pkill -9 fastboot 2>/dev/null
echo ">> done (device booting; USB debug channel comes up in the menu)"
