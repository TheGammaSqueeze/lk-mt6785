#!/usr/bin/env python3
"""Zip a folder with sorted entries and fixed timestamps, so the same files give the same zip.

    det_zip.py <folder> <out.zip>      (the folder's name is the top level inside the zip)
"""
import os, sys, zipfile


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1].rstrip("/"), sys.argv[2]
    top = os.path.basename(src)
    paths = []
    for d, dirs, files in os.walk(src):
        dirs.sort()
        rel = os.path.relpath(d, src)
        base = top if rel == "." else top + "/" + rel.replace(os.sep, "/")
        paths.append((base + "/", None))
        paths += [(base + "/" + f, os.path.join(d, f)) for f in sorted(files)]
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, path in sorted(paths):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            if path is None:
                info.external_attr = 0o40755 << 16 | 0x10
                z.writestr(info, b"")
            else:
                info.external_attr = (0o755 if os.access(path, os.X_OK) else 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(path, "rb") as f:
                    z.writestr(info, f.read())


if __name__ == "__main__":
    main()
