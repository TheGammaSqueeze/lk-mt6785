#!/bin/bash
#
# Build + run the GBA-SD FAT WRITE host tests against real mkfs.fat images and
# fsck them, on FAT16 and FAT32 (small + large clusters). Validates the correctness
# critical write engine (fat_wr write/LFN/replace + gba_sd_save round-trip) off
# device. The write tests create AND verify their own content, so they do not depend
# on pre-existing files (unlike fat_ro_test.c, which is run separately against a
# fixed image with known content).
#
# Needs: gcc, mkfs.fat, fsck.fat, sudo (loop-mount to make dirs + verify). No mtools.
#
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
MNT=/tmp/gba_sd_mnt
mkdir -p "$MNT" 2>/dev/null || true

echo "== building =="
gcc -O2 -I. -o /tmp/wr_test   fat_wr_test.c      fat_ro.c fat_wr.c
gcc -O2 -I. -o /tmp/save_test gba_sd_save_test.c fat_ro.c fat_wr.c gba_sd_save.c
gcc -O2 -I. -o /tmp/dirext_test fat_dirext_test.c fat_ro.c fat_wr.c

mkimg() { # <img> <mkfs-args...>  - fresh image with the SD dir layout (no roms needed)
	local img="$1"; shift
	rm -f "$img"; dd if=/dev/zero of="$img" bs=1M count=96 status=none
	mkfs.fat "$@" "$img" >/dev/null 2>&1
	sudo mount -o loop "$img" "$MNT"
	sudo mkdir -p "$MNT/saves/gba" "$MNT/states/gba"
	sudo sync; sudo umount "$MNT"
}

run() { # <label> <mkfs-args...>
	local img=/tmp/gba_sd_$2.img
	echo "== $1 =="
	mkimg "$img" "${@:2}"
	cp "$img" "$img.a"; /tmp/wr_test   "$img.a"
	cp "$img" "$img.b"; /tmp/save_test "$img.b"
	sudo mount -o loop "$img.b" "$MNT"; sudo mkdir -p "$MNT/saves/gba" 2>/dev/null||true; sudo umount "$MNT"; /tmp/dirext_test "$img.b"
	echo -n "fsck: "; fsck.fat -n "$img.b" 2>&1 | tail -1
}

run "FAT32 512B" -F 32
run "FAT32 32KB" -F 32 -s 64
run "FAT16 2KB"  -F 16 -s 4

echo "== all FAT-write host tests PASS =="
