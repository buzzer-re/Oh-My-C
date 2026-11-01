#!/usr/bin/env python3
"""Extract omp's builtin slash-command metadata (name, aliases, icon, description) in registry order.

Usage: build-tools/extract_slash_commands.py <omp checkout> > assets/tui/slash-commands.json

Reads packages/coding-agent/src/slash-commands/builtin-*.ts. Only top-level command objects (one tab
deep inside the exported arrays) are taken; subcommand lists are skipped. Dev-time only.
"""
import json
import os
import re
import sys

ORDER = ["builtin-modes.ts", "builtin-collaboration.ts", "builtin-session.ts", "builtin-lifecycle.ts",
         "builtin-marketplace.ts", "builtin-skills.ts", "builtin-control.ts"]


def str_value(src, key):
    """Value of `\t\tkey: "..."` (2-tab indent) in a command object body, or None."""
    m = re.search(r'^\t\t' + key + r': ("(?:[^"\\]|\\.)*"|`[^`]*`)', src, re.M)
    if not m:
        return None
    v = m.group(1)
    if v.startswith('`'):
        return v[1:-1]
    return json.loads(v)


def main():
    root = os.path.join(sys.argv[1], "packages/coding-agent/src/slash-commands")
    out = []
    for name in ORDER:
        src = open(os.path.join(root, name), encoding="utf-8").read()
        # top-level objects: "\t{\n" ... "\n\t},"
        for m in re.finditer(r'^\t\{\n(.*?)^\t\},?$', src, re.M | re.S):
            body = m.group(1)
            cname = str_value(body, "name")
            if not cname:
                continue
            aliases = re.search(r'^\t\taliases: \[([^\]]*)\]', body, re.M)
            entry = {"name": cname}
            if aliases:
                entry["aliases"] = [json.loads(a.strip()) for a in aliases.group(1).split(",") if a.strip()]
            icon = str_value(body, "icon")
            if icon:
                entry["icon"] = icon
            desc = str_value(body, "description")
            if desc is not None:
                entry["description"] = desc
            out.append(entry)
    json.dump(out, sys.stdout, ensure_ascii=False, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
