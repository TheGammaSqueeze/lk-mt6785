#!/usr/bin/env python3
"""make_roots.py - the trusted roots for the Pico's HTTPS (pico_tls): the PC's root bundle
(Mozilla's list, as Linux ships it) turned into DER certificates one after another, the form
pico_pki reads. Copy the result to the card as /bios/certs/roots.der.

usage: make_roots.py [bundle.pem] [out.der]
"""
import base64
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/certs/ca-certificates.crt"
dst = sys.argv[2] if len(sys.argv) > 2 else "roots.der"

out = bytearray()
count = 0
inside = False
lines = []
for line in open(src, encoding="ascii", errors="ignore"):
    line = line.strip()
    if line == "-----BEGIN CERTIFICATE-----":
        inside, lines = True, []
    elif line == "-----END CERTIFICATE-----" and inside:
        out += base64.b64decode("".join(lines))
        count += 1
        inside = False
    elif inside:
        lines.append(line)
open(dst, "wb").write(out)
print(f"{count} roots, {len(out)} bytes -> {dst}")
