#!/bin/bash
# Build the GammaOS latency test ROM: out/latency_test.gba (copy it to the card's /roms/gba/).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
CC=${CC:-arm-none-eabi-gcc}
OBJCOPY=${OBJCOPY:-arm-none-eabi-objcopy}
$CC -mcpu=arm7tdmi -marm -O2 -ffreestanding -nostdlib -nostartfiles -Wall -T rom.ld \
	-o out/latency_test.elf crt0.s main.c
$OBJCOPY -O binary out/latency_test.elf out/latency_test.gba
python3 - out/latency_test.gba <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
if len(d) < 0x8000:
    d += b'\xff' * (0x8000 - len(d))          # a 32 KiB cartridge
d[0xA0:0xAC] = b'LATENCYTEST\x00'               # title
d[0xAC:0xB0] = b'ZLAT'                          # game code (no real game uses it)
d[0xB0:0xB2] = b'00'                            # maker
d[0xB2] = 0x96                                  # fixed value
d[0xB3] = 0                                     # unit code
d[0xB4] = 0                                     # device type
d[0xB5:0xBC] = bytes(7)
d[0xBC] = 0                                     # version
d[0xBD] = (-(sum(d[0xA0:0xBD]) + 0x19)) & 0xFF  # header checksum
open(p, 'wb').write(d)
PY
echo "out/latency_test.gba"
