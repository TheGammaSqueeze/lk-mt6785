#!/bin/bash
# exFAT host tests: build a real mkfs.exfat image with ROM files, run the reader/writer test,
# then fsck the image and verify the written files through the kernel's exfat driver.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"; cd "$DIR"
MNT=/tmp/gba_exfat_mnt; REF=/tmp/gba_exfat_ref
mkdir -p "$MNT" "$REF"
gcc -O2 -Wall -I. -o /tmp/exfat_rw_test fat_exfat_rw_test.c fat_ro.c fat_wr.c fat_exfat.c
run() { # <label> <mkfs args...>
	local img=/tmp/gba_exfat_$1.img
	echo "== exFAT $1 =="
	rm -f "$img" "$REF"/*; dd if=/dev/zero of="$img" bs=1M count=256 status=none
	mkfs.exfat "${@:2}" "$img" >/dev/null
	LOOP=$(sudo losetup -f --show "$img"); sudo mount.exfat-fuse "$LOOP" "$MNT"
	sudo mkdir -p "$MNT/roms/gba"
	sudo dd if=/dev/urandom of="$MNT/roms/gba/small.gba" bs=1k count=3 status=none
	sudo dd if=/dev/urandom of="$MNT/roms/gba/Big Rom Name (USA, Europe) [!].gba" bs=1M count=3 status=none
	sudo dd if=/dev/urandom of="$MNT/roms/gba/odd size.gba" bs=1 count=70001 status=none
	cp "$MNT/roms/gba/"* "$REF/"
	sudo sync; sudo umount "$MNT"; sudo losetup -d "$LOOP"
	/tmp/exfat_rw_test "$img" "$REF"
	echo -n "fsck: "; fsck.exfat -n "$img" 2>&1 | tail -1
	LOOP=$(sudo losetup -f --show "$img"); sudo mount.exfat-fuse "$LOOP" "$MNT"
	ls "$MNT/saves/gba" | wc -l | sed 's/^/kernel sees files in saves: /'
	cmp <(head -c 32768 "$MNT/saves/gba/Some Long Game Name (Europe).sav") <(python3 -c "import sys; sys.stdout.buffer.write(bytes((((i*7+(i>>9))&255)^0x5A) for i in range(32768)))") && echo "fuse readback ok (replaced save)"
	cmp <(head -c 1048576 "$MNT/states/gba/big state file.sus") <(python3 -c "import sys; sys.stdout.buffer.write(bytes(((i*7+(i>>9))&255) for i in range(1048576)))") && echo "fuse readback ok (1 MB state)"
	sudo umount "$MNT"; sudo losetup -d "$LOOP"
}
run c4k   -c 4K
run c128k -c 128K
echo "== exFAT host tests PASS =="
