#!/usr/bin/env python3
"""Audit which of omp's settings omc honours (WP-26, docs/gaps.md §3).

For every setting in assets/config/settings-registry.json (omp's registry; the ones in
assets/tui/settings-ui.json are the 402 that /settings shows) this lists the omc source lines that
read it and classifies it:

  wired    read by omc code outside the settings plumbing (or marked `wired` in the annotations
           when it is read indirectly, e.g. through its parent object)
  partial  read, but only part of omp's behaviour is implemented (annotated with what differs)
  inert    shown/saved but nothing reads it (annotated with why)
  oos      ⛔ out of scope: needs the JS runtime, a hosted service, a browser or on-device ML

A "read" is a C string literal equal to the key anywhere in src/**/*.{c,h} except the generic
settings plumbing (PLUMBING below), which handles every key alike. Classifications that cannot be
derived from the source live in build-tools/settings_audit.tsv (key or `prefix.*`, status, note).
tests/test_settings_audit.c enforces the same rules in `make test`.

Usage: build-tools/settings_audit.py [--table] [--ui-only] [--status S] [--counts]
  default: per-status counts (all registry keys, and the /settings subset), then the
  non-wired keys grouped by status with their notes.
  --table: one row per key: key, shown in /settings, status, readers, note.
"""
import fnmatch
import glob
import json
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUMBING = {"src/config/settings.c", "src/ui/settings.c", "src/tools/cfg_url.c", "src/cli/sub_config.c"}
STATUSES = ("wired", "partial", "inert", "oos")
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def load_annotations(path):
    ann = []  # (pattern, status, note) in file order; first match wins
    with open(path, encoding="utf-8") as f:
        for ln, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2 or parts[1] not in STATUSES + ("omc-only",):
                sys.exit(f"{path}:{ln}: bad line: {line!r}")
            ann.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    return ann


def annotation_for(key, ann):
    for pat, st, note in ann:
        if st == "omc-only":
            continue
        if pat == key or (pat.endswith(".*") and fnmatch.fnmatchcase(key, pat)):
            return st, note
    return None, ""


def scan_readers(keys):
    readers = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(ROOT, "src/**/*.[ch]"), recursive=True)):
        rel = os.path.relpath(path, ROOT)
        if rel in PLUMBING:
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for ln, line in enumerate(f, 1):
                for m in LIT.finditer(line):
                    if m.group(1) in keys:
                        readers[m.group(1)].append(f"{rel}:{ln}")
    return readers


def audit():
    reg = json.load(open(os.path.join(ROOT, "assets/config/settings-registry.json")))
    ui = {s["path"] for s in json.load(open(os.path.join(ROOT, "assets/tui/settings-ui.json")))["settings"]}
    ann = load_annotations(os.path.join(ROOT, "build-tools/settings_audit.tsv"))
    keys = [e["id"] for e in reg]
    readers = scan_readers(set(keys))
    rows = []
    for k in keys:
        st, note = annotation_for(k, ann)
        read = bool(readers.get(k))
        if st is None:
            st = "wired" if read else "unclassified"
        rows.append({"key": k, "ui": k in ui, "status": st, "readers": readers.get(k, []), "note": note})
    return rows


def main(argv):
    rows = audit()
    if "--ui-only" in argv:
        rows = [r for r in rows if r["ui"]]
    if "--status" in argv:
        want = argv[argv.index("--status") + 1]
        rows = [r for r in rows if r["status"] == want]
    if "--table" in argv:
        print("| Setting | /settings | Status | omc readers | Note |")
        print("|---|---|---|---|---|")
        for r in rows:
            rd = ", ".join(r["readers"][:3]) + (" …" if len(r["readers"]) > 3 else "")
            print(f"| `{r['key']}` | {'yes' if r['ui'] else ''} | {r['status']} | {rd} | {r['note']} |")
        return 0
    order = STATUSES + ("unclassified",)
    for label, sub in (("registry", rows), ("/settings", [r for r in rows if r["ui"]])):
        c = {s: sum(1 for r in sub if r["status"] == s) for s in order}
        print(f"{label}: {len(sub)} keys: " + ", ".join(f"{s} {c[s]}" for s in order if c[s] or s != "unclassified"))
    if "--counts" in argv:
        return 0
    for s in ("unclassified", "partial", "inert", "oos"):
        sub = [r for r in rows if r["status"] == s]
        if not sub:
            continue
        print(f"\n## {s} ({len(sub)})")
        for r in sub:
            print(f"  {r['key']}{'' if r['ui'] else ' (not in /settings)'}" + (f"  -- {r['note']}" if r["note"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
