#!/usr/bin/env python3
"""Assemble pico_smp_gbahost64.S.src (AArch64) and print it as the .word list that
pico_smp_asm.S carries under pico_smp_gba_land (the LK itself is built as 32 bit code, so the
64 bit host is embedded as data). The two instructions that load the board's mark page address are
printed as the SMP_A64_* macros so one list serves both boards.

  gen_smp_gbahost.py [--write]     needs aarch64-linux-gnu-as / objcopy

With --write the list in pico_smp_asm.S is replaced in place."""
import os, re, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "pico_smp_gbahost64.S.src")
asm = os.path.join(here, "pico_smp_asm.S")
SUBST = {0xd29af807: "SMP_A64_MOVZ_X7", 0xf2a00227: "SMP_A64_MOVK_X7",
         0xd29af814: "SMP_A64_MOVZ_X20", 0xf2a00234: "SMP_A64_MOVK_X20"}
with tempfile.TemporaryDirectory() as d:
    o, b = os.path.join(d, "h.o"), os.path.join(d, "h.bin")
    subprocess.run(["aarch64-linux-gnu-as", "-o", o, src], check=True)
    subprocess.run(["aarch64-linux-gnu-objcopy", "-O", "binary", o, b], check=True)
    data = open(b, "rb").read()
data += b"\0" * (-len(data) % 4)
words = struct.unpack("<%dI" % (len(data) // 4), data)
lines = ["\t.word\t%s" % (SUBST.get(w) or "0x%08x" % w) for w in words]
if "--write" not in sys.argv:
    print("\n".join(lines)); sys.exit(0)
text = open(asm).read()
m = re.search(r"(pico_smp_gba_land:\n)((?:\t\.word\t\S+\n)+)", text)
if not m: sys.exit("pico_smp_gba_land list not found")
open(asm, "w").write(text[:m.start(2)] + "\n".join(lines) + "\n" + text[m.end(2):])
print("%d words (%d bytes) written" % (len(words), len(data)))
