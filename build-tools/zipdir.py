#!/usr/bin/env python3
"""zipdir.py OUT.zip BASE DIR: zip BASE/DIR into OUT.zip with entries under DIR/ (the Windows release archive of
`make WINDOWS=1 dist`). Dev-time tool: the stdlib only; sorted entries, each with its file's modification time."""
import os
import sys
import time
import zipfile


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    out, base, top = sys.argv[1:]
    root = os.path.join(base, top)
    files = []
    for d, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            files.append(os.path.join(d, n))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            arc = os.path.relpath(f, base).replace(os.sep, "/")
            # zip stores local time from 1980 on; Expand-Archive gives the files these dates
            stamp = time.localtime(max(os.path.getmtime(f), 315532800 + 86400))
            info = zipfile.ZipInfo(arc, date_time=stamp[:6])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if f.endswith((".exe", ".dll")) else 0o644) << 16
            with open(f, "rb") as fh:
                z.writestr(info, fh.read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
