#!/usr/bin/env python3
"""
Push ROMs to the device's microSD over fastboot, using the LK `oem sd-put:` path
(emu/gba/sd_fastboot.c). This is the reliable way to get ROMs onto the card without
a physical reader: the emulator LK has no adb/UMS gadget, and the SD is not a
fastboot partition, but `fastboot stage <file>` + `fastboot oem sd-put:<abspath>`
writes an arbitrary file to the FAT card (parent dirs are auto-created).

The device must be in the bootloader with the SD free (the running ROM menu holds
the card, so sd-put fails then - reboot to the bootloader first). Files are copied
to a local temp before staging because fastboot cannot read /mnt/c (9p) paths.

Usage:
  push_roms.py <console> <src...> [--serial S] [--extract] [--max-name N]
               [--allow-long] [--dry-run]

  <console>  gb|gbc|gba|snes|genesis|sms|gg|sg
  <src...>   one or more ROM files and/or directories (dirs are scanned, non-recursive)
             .zip/.7z archives are extracted only with --extract (needs 7z)

Examples:
  push_roms.py genesis "/mnt/c/roms/genesis" --extract --serial 0123456789ABCDEF
  push_roms.py snes "/mnt/c/roms/snes/Super Mario World (E).smc" --serial 0123456789ABCDEF

Safety (learned the hard way, see push_boxart.py): a very long filename writes many
LFN directory entries and can wedge the USB link mid-FAT-write and corrupt the SD
directory. Names longer than --max-name (default 30) are SKIPPED unless --allow-long
is given. Pushes run one file at a time; each is verified by the "sd-put: wrote N"
reply matching the file size.
"""
import sys, os, re, shutil, argparse, subprocess, tempfile

# console -> (dest dir on the SD, accepted raw extensions) - must match
# gba_sd_list_roms() scan folders/extensions in emu/gba/sd_fat.c.
CONSOLES = {
    "gb":      ("/roms/gb",      (".gb",)),
    "gbc":     ("/roms/gbc",     (".gbc",)),
    "gba":     ("/roms/gba",     (".gba",)),
    "snes":    ("/roms/snes",    (".sfc", ".smc")),
    "genesis": ("/roms/genesis", (".md", ".gen", ".bin", ".smd")),
    "sms":     ("/roms/sms",     (".sms",)),
    "gg":      ("/roms/gg",      (".gg",)),
    "sg":      ("/roms/sg",      (".sg",)),
}


def fb(serial, *args, capture=False):
    cmd = ["fastboot"] + (["-s", serial] if serial else []) + list(args)
    return subprocess.run(cmd, capture_output=True, text=True) if capture \
        else subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def gather(srcs, exts, do_extract, workdir):
    """Yield (local_path_readable_by_fastboot, dest_basename) for every ROM."""
    out = []
    files = []
    for s in srcs:
        if os.path.isdir(s):
            for n in sorted(os.listdir(s)):
                p = os.path.join(s, n)
                if os.path.isfile(p):
                    files.append(p)
        elif os.path.isfile(s):
            files.append(s)
        else:
            print("skip (not found):", s)
    for p in files:
        low = p.lower()
        if low.endswith(exts):
            # copy into workdir so fastboot can read it (not /mnt/c)
            dest = os.path.basename(p)
            local = os.path.join(workdir, "stage_%d_%s" % (len(out), re.sub(r"[^A-Za-z0-9._-]", "_", dest)))
            shutil.copyfile(p, local)
            out.append((local, dest))
        elif do_extract and (low.endswith(".zip") or low.endswith(".7z")):
            before = set(os.listdir(workdir))
            r = subprocess.run(["7z", "e", "-y", "-o" + workdir, p],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if r.returncode != 0:
                print("extract FAILED:", p); continue
            for n in sorted(set(os.listdir(workdir)) - before):
                if n.lower().endswith(exts):
                    out.append((os.path.join(workdir, n), n))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("console", choices=sorted(CONSOLES))
    ap.add_argument("src", nargs="+", help="ROM files and/or directories")
    ap.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL", "0123456789ABCDEF"))
    ap.add_argument("--extract", action="store_true", help="also extract .zip/.7z (needs 7z)")
    ap.add_argument("--max-name", type=int, default=30, help="skip dest names longer than this (LFN wedge risk)")
    ap.add_argument("--allow-long", action="store_true", help="push long names anyway (risky)")
    ap.add_argument("--dry-run", action="store_true", help="list what would be pushed, do not touch the device")
    a = ap.parse_args()

    destdir, exts = CONSOLES[a.console]

    if not a.dry_run:
        q = fb(a.serial, "devices", capture=True)
        if a.serial not in (q.stdout or ""):
            print("device %s not in fastboot. Reboot to the bootloader (SD must be free) and retry." % a.serial)
            print("(the running ROM menu holds the SD, so sd-put fails then.)")
            sys.exit(1)

    work = tempfile.mkdtemp(prefix="pushroms_")
    try:
        items = gather(a.src, exts, a.extract, work)
        if not items:
            print("no matching ROMs found for console '%s' (exts: %s)" % (a.console, " ".join(exts)))
            sys.exit(1)

        ok = fail = skipped = 0
        for local, dest in items:
            sz = os.path.getsize(local)
            if len(dest) > a.max_name and not a.allow_long:
                print("SKIP  %-34s (%d chars > %d, LFN wedge risk; --allow-long to force, or shorten)"
                      % (dest, len(dest), a.max_name))
                skipped += 1
                continue
            target = "%s/%s" % (destdir, dest)
            if a.dry_run:
                print("would push  %-34s -> %s  (%d bytes)" % (dest, target, sz))
                ok += 1
                continue
            fb(a.serial, "stage", local)
            r = fb(a.serial, "oem", "sd-put:%s" % target, capture=True)
            blob = (r.stdout or "") + (r.stderr or "")
            m = re.search(r"wrote (\d+)", blob)
            if m and int(m.group(1)) == sz:
                print("OK    %-34s -> %s  (%d bytes)" % (dest, target, sz))
                ok += 1
            else:
                last = blob.strip().splitlines()[-1] if blob.strip() else "no reply"
                print("FAIL  %-34s : %s" % (dest, last))
                fail += 1
        print("\n%d pushed, %d skipped, %d failed." % (ok, skipped, fail))
        if not a.dry_run and ok:
            print("Reboot the device (fastboot -s %s reboot) to rescan the ROM lists." % a.serial)
        sys.exit(1 if fail else 0)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
