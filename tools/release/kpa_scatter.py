#!/usr/bin/env python3
"""Write an SP Flash Tool scatter for the Konkr Pocket Advance that downloads only the LK.

    kpa_scatter.py <stock scatter.txt|.xml> <out scatter> <lk image file name> [partition ...]

From the stock BW03 scatter every partition is unticked with no file except the given LK
partitions (default lk_a and lk_b), which get the LK file and are ticked. The preloader keeps its
file name, unticked: SP Flash Tool reads its memory setup to connect, it is not written. The text
scatter (for SP Flash Tool v5) also has its config version lowered from V2.2.0, which v5 refuses,
to V2.1.0; the .xml scatter (for SP Flash Tool V6) keeps the stock version.
"""
import re, sys


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    src, dst, lk = sys.argv[1], sys.argv[2], sys.argv[3]
    parts = set(sys.argv[4:] or ["lk_a", "lk_b"])
    raw = open(src, newline="").read()
    eol = "\r\n" if "\r\n" in raw else "\n"   # written back with the stock file's line ends
    text = raw.replace("\r\n", "\n")
    out, name, seen = [], None, set()
    xml = src.lower().endswith(".xml")
    for line in text.split("\n"):
        if xml:   # <partition_name>lk_a</partition_name> and the like, one per line
            m = re.match(r"^(\s*)<(partition_name|file_name|is_download)>(.*)</\2>\s*$", line)
            if m:
                ind, key, val = m.groups()
                if key == "partition_name":
                    name = val.strip()
                elif key == "file_name":
                    if name in parts:
                        val = lk
                        seen.add(name)
                    elif name != "preloader":
                        val = "NONE"
                elif key == "is_download":
                    val = "true" if name in parts else "false"
                line = "%s<%s>%s</%s>" % (ind, key, val, key)
            out.append(line)
            continue
        m = re.match(r"^(\s*(?:- )?)(config_version|partition_name|file_name|is_download): (.*)$", line)
        if m:
            ind, key, val = m.groups()
            if key == "config_version":
                line = ind + "config_version: V2.1.0"
            elif key == "partition_name":
                name = val.strip()
            elif key == "file_name":
                if name in parts:
                    line, _ = ind + "file_name: " + lk, seen.add(name)
                elif name != "preloader":
                    line = ind + "file_name: NONE"
            elif key == "is_download":
                line = ind + "is_download: " + ("true" if name in parts else "false")
        out.append(line)
    missing = parts - seen
    if missing:
        sys.exit("partitions not in the scatter: " + ", ".join(sorted(missing)))
    with open(dst, "w", newline="") as f:
        f.write(eol.join(out))


if __name__ == "__main__":
    main()
