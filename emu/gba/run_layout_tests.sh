#!/bin/bash
# Partition layout tests for fat_mount: MBR, GPT (primary and backup), protective MBR, a
# damaged table (raw probe), and bare volumes, each with FAT32 or exFAT inside. Uses a small
# mount-only host program.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"; cd "$DIR"
cat > /tmp/mount_probe.c <<'C'
#include <stdio.h>
#include <string.h>
#include "fat_ro.h"
static FILE *g;
static unsigned rd(void *c, uint32_t l, uint32_t n, void *b) { (void)c; fseek(g, (long)l * 512, SEEK_SET); return (unsigned)fread(b, 512, n, g); }
int main(int argc, char **argv)
{
	fat_vol v; fat_dir d; fat_dirent e; int n = 0, rc;
	g = fopen(argv[1], "rb"); if (!g) return 2;
	rc = fat_mount(&v, rd, 0);
	if (rc != 0) { printf("mount rc=%d\n", rc); return 1; }
	if (fat_opendir(&v, "/roms/gba", &d) != 0) { printf("no /roms/gba\n"); return 1; }
	while (fat_readdir(&d, &e)) n++;
	printf("%s at lba %u: %d entries in /roms/gba\n", v.is_exfat ? "exFAT" : (v.is_fat32 ? "FAT32" : "FAT16"), v.part_lba, n);
	return n == 2 ? 0 : 1;
}
C
gcc -O2 -I. -o /tmp/mount_probe /tmp/mount_probe.c fat_ro.c fat_wr.c fat_exfat.c 2>/dev/null
MNT=/tmp/gba_layout_mnt; mkdir -p "$MNT"
fill() { # <dev> <fs>
	if [ "$2" = exfat ]; then sudo mkfs.exfat "$1" >/dev/null; sudo mount.exfat-fuse "$1" "$MNT"; else sudo mkfs.fat -F 32 "$1" >/dev/null; sudo mount "$1" "$MNT"; fi
	sudo mkdir -p "$MNT/roms/gba"; sudo dd if=/dev/urandom of="$MNT/roms/gba/a.gba" bs=1k count=2 status=none; sudo dd if=/dev/urandom of="$MNT/roms/gba/b.gba" bs=1k count=5 status=none
	sudo sync; sudo umount "$MNT"
}
mk() { # <label> <table: gpt|msdos|none> <fs> <extra: corrupt_mbr|corrupt_gpt_primary|none>
	local img=/tmp/gba_layout_$1.img
	rm -f "$img"; dd if=/dev/zero of="$img" bs=1M count=128 status=none
	if [ "$2" = none ]; then
		LOOP=$(sudo losetup -f --show "$img"); fill "$LOOP" "$3"; sudo losetup -d "$LOOP"
	else
		parted -s "$img" mklabel "$2" mkpart primary 1MiB 100% >/dev/null
		LOOP=$(sudo losetup -f --show -P "$img"); sleep 0.5
		fill "${LOOP}p1" "$3"; sudo losetup -d "$LOOP"
	fi
	case "$4" in
		corrupt_mbr) dd if=/dev/urandom of="$img" bs=1 count=64 seek=$((0x1BE)) conv=notrunc status=none ;;   # garbage partition table, signature kept
		corrupt_gpt_primary) dd if=/dev/urandom of="$img" bs=512 count=1 seek=1 conv=notrunc status=none ;;    # primary header destroyed: backup must serve
		kill_both) dd if=/dev/zero of="$img" bs=512 count=34 conv=notrunc status=none ;;                        # MBR + GPT gone: raw probe at 2048
	esac
	echo -n "$1: "; /tmp/mount_probe "$img"
}
mk mbr_fat32         msdos fat32 none
mk mbr_exfat         msdos exfat none
mk gpt_fat32         gpt   fat32 none
mk gpt_exfat         gpt   exfat none
mk gpt_exfat_backup  gpt   exfat corrupt_gpt_primary
mk mbr_fat32_corrupt msdos fat32 corrupt_mbr
mk gpt_exfat_notable gpt   exfat kill_both
mk bare_fat32        none  fat32 none
mk bare_exfat        none  exfat none
echo "== layout tests PASS =="
