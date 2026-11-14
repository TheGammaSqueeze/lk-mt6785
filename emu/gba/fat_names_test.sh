#!/bin/bash
#
# Names outside ASCII on the card (fat_names_test.c), on FAT32, FAT16 and exFAT images whose files
# the host's own drivers wrote (vfat with UTF-8 names; exFAT through exfat-fuse). After the test
# writes to each image: fsck, the host lists the written names back the same, and for exFAT an
# independent check of every entry set's checksum and name hash against the volume's up-case table.
#
# Needs: gcc, zlib, mkfs.fat, fsck.fat, mkfs.exfat, fsck.exfat, mount.exfat-fuse, 7z, python3, sudo.
#
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"
W=$(mktemp -d); LO=
cleanup() { sudo umount "$W/mnt" 2>/dev/null || true; [ -n "$LO" ] && sudo losetup -d "$LO" 2>/dev/null || true; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/inc/lib" "$W/inc/kernel" "$W/mnt" "$W/files"
# the LK's headers the play time code takes, for one thread on the host
echo '#include <zlib.h>' > "$W/inc/lib/zlib.h"
echo 'unsigned long current_time(void);' > "$W/inc/platform.h"
printf 'static inline void enter_critical_section(void) {}\nstatic inline void exit_critical_section(void) {}\n' > "$W/inc/kernel/thread.h"
printf 'typedef int mutex_t;\nstatic inline void mutex_init(mutex_t *m) { (void)m; }\nstatic inline void mutex_acquire(mutex_t *m) { (void)m; }\nstatic inline void mutex_release(mutex_t *m) { (void)m; }\n' > "$W/inc/kernel/mutex.h"
gcc -O1 -Wall -Wno-format-truncation -I. -I"$W/inc" -o "$W/names_test" fat_names_test.c fat_ro.c fat_wr.c \
	fat_exfat.c sd_fat.c rom_archive.c rom_7z.c rom_lzma.c pico_fold.c gba_sd_save.c pico_stats.c -lz

# the files, made once: ROMs, a zip marked UTF-8, a zip in code page 437, a 7z
python3 - "$W/files" <<'PY'
import os, sys, zipfile, struct
d = sys.argv[1]
def rom(tag): return (tag.encode() * 64)[:4096]
names = ["Pokémon Émeraude.gba", "Pokémon2.gba", "ポケモン.gba",
         "Smile \U0001F600.gba", "Zelda.gba", "apple.gba"]
os.makedirs(d + "/gba", exist_ok=True)
for n in names: open(os.path.join(d, "gba", n), "wb").write(rom(n))
with zipfile.ZipFile(d + "/gba/Multi.zip", "w") as z:
    z.writestr("Café One.gba", rom("c1")); z.writestr("Zeta.gba", rom("ze"))
# one name in code page 437 (no UTF-8 flag): written under a stand-in, then the byte put in
with zipfile.ZipFile(d + "/gba/Old437.zip", "w") as z:
    z.writestr("CafX Two.gba", rom("c2")); z.writestr("Old.gba", rom("ol")); z.writestr("readme.txt", b"x")
b = bytearray(open(d + "/gba/Old437.zip", "rb").read())
assert b.count(b"CafX Two.gba") == 2
b = bytearray(bytes(b).replace(b"CafX Two.gba", b"Caf\x82 Two.gba"))
open(d + "/gba/Old437.zip", "wb").write(b)
os.makedirs(d + "/7z", exist_ok=True)
open(d + "/7z/Sept é.gba", "wb").write(rom("s7")); open(d + "/7z/Huit.gba", "wb").write(rom("h8"))
PY
(cd "$W/files/7z" && 7z a -bd -mx=1 ../gba/Seven.7z "Sept é.gba" Huit.gba >/dev/null)
rm -rf "$W/files/7z"

# put the files on an image through the host's driver
fill() { # <mountpoint>
	sudo mkdir -p "$1/roms/gba" "$1/roms/gb" "$1/Jeux é/gba" "$1/gammaos" "$1/roms/gba/boxart" "$1/saves/gba" "$1/states/gba"
	sudo cp "$W/files/gba/"* "$1/roms/gba/"
	# two Game Boy games whose names have the same underscore form, "___ ___"
	echo cn | sudo tee "$1/roms/gb/宝可梦 测试版.gb" >/dev/null
	echo ko | sudo tee "$1/roms/gb/포켓몬 테스트.gb" >/dev/null
	# what a version before 1.1 left: files under the underscore form of the names
	printf oldgb_ | sudo tee "$1/saves/gba/___ ___.sav" >/dev/null
	sudo mkdir -p "$1/roms/gb/boxart"; printf oldgbart | sudo tee "$1/roms/gb/boxart/___ ___.ART" >/dev/null
	printf oldpk4 | sudo tee "$1/saves/gba/____.sav" >/dev/null
	printf oldsav | sudo tee "$1/saves/gba/Pok_mon _meraude.sav" >/dev/null
	printf oldst3 | sudo tee "$1/states/gba/Pok_mon _meraude.st3" >/dev/null
	printf oldart | sudo tee "$1/roms/gba/boxart/Pok_mon _meraude.ART" >/dev/null
	printf '2 4242 1700000000 Pok_mon _meraude\n0 99 1700000000 ___ ___\n' | sudo tee "$1/gammaos/playtime.txt" >/dev/null
	echo x | sudo tee "$1/Jeux é/gba/x.gba" >/dev/null
	# a name longer in UTF-8 (304 bytes) than the host's own file systems allow: on the card only
	sudo python3 -c 'import sys; open(sys.argv[1] + "/roms/gba/" + "\u3042" * 100 + ".gba", "wb").write(b"long" * 1024)' "$1"
	printf 'gba /Jeux é/gba\n' | sudo tee "$1/gammaos/folders.txt" >/dev/null
}

# an independent check of an exFAT image: every entry set's checksum and name hash (the up-case
# table read from the volume), printing the names it finds
exfat_check() {
python3 - "$1" <<'PY'
import struct, sys
img = open(sys.argv[1], "rb").read()
fat_off, fat_len, heap_off, ccount, root = struct.unpack_from("<IIIII", img, 0x50)
bps = 1 << img[0x6C]; spc = 1 << img[0x6D]; cb = bps * spc
def clus(n): return img[heap_off * bps + (n - 2) * cb: heap_off * bps + (n - 1) * cb]
def chain(first, nofat, length):
    out, c = b"", first
    n = (length + cb - 1) // cb if length else 10 ** 9
    for _ in range(min(n, 4096)):
        if c < 2 or c >= ccount + 2: break
        out += clus(c)
        c = c + 1 if nofat else struct.unpack_from("<I", img, fat_off * bps + c * 4)[0]
    return out
up = {}
def load_upcase(data):
    i, code = 0, 0
    while i + 2 <= len(data):
        v = struct.unpack_from("<H", data, i)[0]; i += 2
        if v == 0xFFFF:
            code += struct.unpack_from("<H", data, i)[0]; i += 2; continue
        up[code] = v; code += 1
bad, names = 0, []
def walk(data, path):
    global bad
    i = 0
    while i + 32 <= len(data):
        t = data[i]
        if t == 0: break
        if t == 0x82: load_upcase(chain(struct.unpack_from("<I", data, i + 20)[0], 0, struct.unpack_from("<Q", data, i + 24)[0]))
        if t != 0x85: i += 32; continue
        secs = data[i + 1]; s = data[i: i + 32 * (secs + 1)]
        ck = 0
        for k, byte in enumerate(s):
            if k in (2, 3): continue
            ck = ((ck >> 1) | ((ck & 1) << 15)) + byte & 0xFFFF
        if ck != struct.unpack_from("<H", s, 2)[0]: bad += 1; print("bad set checksum at", path)
        nlen = s[35]; units = []
        for k in range(2, secs + 1):
            if s[32 * k] == 0xC1: units += struct.unpack_from("<15H", s, 32 * k + 2)
        units = units[:nlen]
        h = 0
        for u in units:
            u = up.get(u, u)
            for byte in (u & 0xFF, u >> 8): h = ((h >> 1) | ((h & 1) << 15)) + byte & 0xFFFF
        name = struct.pack("<%dH" % len(units), *units).decode("utf-16-le")
        if h != struct.unpack_from("<H", s, 36)[0]: bad += 1; print("bad name hash:", path + "/" + name)
        names.append(path + "/" + name)
        if struct.unpack_from("<H", s, 4)[0] & 0x10:
            walk(chain(struct.unpack_from("<I", s, 52)[0], s[33] & 2, struct.unpack_from("<Q", s, 56)[0]), path + "/" + name)
        i += 32 * (secs + 1)
walk(chain(root, 0, 0), "")
for want in ["/saves/gba/Écran ü.sav", "/saves/gba/x\U0001F600.sav", "/saves/Jeux é/sub/in.sav",
             "/saves/gba/Pokémon Émeraude.sav", "/saves/gba/宝可梦 测试版.sav", "/saves/gba/포켓몬 테스트.sav"]:
    if want not in names: bad += 1; print("missing", want)
print("exFAT sets checked: %d names, %d bad" % (len(names), bad))
sys.exit(1 if bad else 0)
PY
}

run() { # <label> <fs> <mkfs args...>
	local label=$1 fs=$2 img="$W/img"; shift 2
	echo "== $label =="
	rm -f "$img"; dd if=/dev/zero of="$img" bs=1M count=64 status=none
	if [ "$fs" = exfat ]; then
		mkfs.exfat "$@" "$img" >/dev/null
		LO=$(sudo losetup -f --show "$img")
		sudo mount.exfat-fuse "$LO" "$W/mnt" >/dev/null; fill "$W/mnt"; sudo umount "$W/mnt"; sudo losetup -d "$LO"; LO=
	else
		mkfs.fat "$@" "$img" >/dev/null
		sudo mount -o loop,utf8 "$img" "$W/mnt"; fill "$W/mnt"; sudo umount "$W/mnt"
	fi
	"$W/names_test" "$img" "$fs"
	if [ "$fs" = exfat ]; then
		echo -n "fsck: "; fsck.exfat -n "$img" 2>&1 | tail -1
		exfat_check "$img"
		LO=$(sudo losetup -f --show "$img")
		sudo mount.exfat-fuse "$LO" "$W/mnt" >/dev/null
	else
		echo -n "fsck: "; fsck.fat -n "$img" 2>&1 | tail -1
		sudo mount -o loop,utf8,ro "$img" "$W/mnt"
	fi
	echo "the host lists: $(ls "$W/mnt/saves/gba" | tr '\n' '|')"
	ls "$W/mnt/saves/gba" | grep -qx "Écran ü.sav" && ls "$W/mnt/saves/gba" | grep -qx "x😀.sav" && \
		! ls "$W/mnt/saves/gba" | grep -q "テスト" || { echo "FAIL the host does not list the names written"; exit 1; }
	sudo umount "$W/mnt"
	if [ -n "$LO" ]; then sudo losetup -d "$LO"; LO=; fi
}

run "FAT32" fat -F 32
run "FAT16" fat -F 16 -s 4
run "exFAT" exfat
echo "== names outside ASCII: all PASS =="
