#!/usr/bin/env python3
"""Extract omp's builtin slash-command registry metadata in registry order.

Usage: build-tools/extract_slash_commands.py <omp checkout> > assets/tui/slash-commands.json

Reads packages/coding-agent/src/slash-commands/builtin-*.ts (plus compact-modes.ts and the thinking
level table for the two computed subcommand lists). Only top-level command objects (one tab deep inside
the exported arrays) are taken. Per command: name, aliases, icon, description (a `get description()`
template keeps `{key:<id>}` placeholders for formatKeyHint), acpDescription, inlineHint, acpInputHint,
allowArgs, subcommandOptional, subcommands [{name, description, usage}], and whether the spec has a
text-mode `handle` (ACP/RPC) and a `handleTui`. Dev-time only.
"""
import json
import os
import re
import sys

ORDER = ["builtin-modes.ts", "builtin-collaboration.ts", "builtin-session.ts", "builtin-lifecycle.ts",
         "builtin-marketplace.ts", "builtin-skills.ts", "builtin-control.ts"]


def js_string(v):
    if v.startswith('`'):
        return v[1:-1].replace('\\`', '`')
    if v.startswith("'"):
        return json.loads('"' + v[1:-1].replace('"', '\\"').replace("\\'", "'") + '"')
    return json.loads(v)


STR = r'("(?:[^"\\]|\\.)*"|`(?:[^`\\]|\\.)*`|\'(?:[^\'\\]|\\.)*\')'


def str_value(src, key, indent=2):
    """Value of `<indent tabs>key: "..."` in a command object body, or None."""
    m = re.search(r'^' + '\t' * indent + key + r': ' + STR, src, re.M)
    return js_string(m.group(1)) if m else None


def getter_description(body):
    m = re.search(r'^\t\tget description\(\) \{\n\t\t\treturn (`(?:[^`\\]|\\.)*`);', body, re.M)
    if not m:
        return None
    t = m.group(1)[1:-1].replace('\\`', '`')
    return re.sub(r'\$\{formatKeyHint\("([^"]+)"\)\}', r'{key:\1}', t)


def parse_subcommands(body):
    m = re.search(r'^\t\tsubcommands: \[\n(.*?)^\t\t\],?$', body, re.M | re.S)
    if not m:
        m1 = re.search(r'^\t\tsubcommands: \[(\{.*\})\],?$', body, re.M)
        if not m1:
            return None
        items = [m1.group(1)]
    else:
        # each item: "\t\t\t{ ... }," on one line or "\t\t\t{\n ... \n\t\t\t},"
        items = re.findall(r'^\t\t\t\{(.*?)\},?$', m.group(1), re.M | re.S)
    out = []
    for it in items:
        sub = {}
        for key in ("name", "description", "usage"):
            mm = re.search(key + r': ' + STR, it)
            if mm:
                sub[key] = js_string(mm.group(1))
        if "name" in sub:
            out.append(sub)
    return out


def compact_modes(root):
    src = open(os.path.join(root, "../session/compact-modes.ts"), encoding="utf-8").read()
    out = []
    for m in re.finditer(r'\t\{\n\t\tname: "([^"]+)",\n\t\tdescription: ' + STR + r',\n(.*?)\n\t\},', src, re.S):
        sub = {"name": m.group(1), "description": js_string(m.group(2))}
        if "rejectsFocus: true" not in m.group(3):
            sub["usage"] = "[focus]"
        out.append(sub)
    return out


def thinking_levels(checkout):
    src = open(os.path.join(checkout, "packages/tui/src/thinking.ts"), encoding="utf-8").read()
    desc = {}
    for m in re.finditer(r'label: "([^"]+)",\s*description: "([^"]+)"', src):
        desc[m.group(1)] = m.group(2)
    m = re.search(r'description: "(Auto-detect per prompt)"', src)
    if m:
        desc["auto"] = m.group(1)
    levels = ["off", "minimal", "low", "medium", "high", "xhigh", "max", "auto"]
    return [{"name": l, "description": desc.get(l, "")} for l in levels]


def main():
    checkout = sys.argv[1]
    root = os.path.join(checkout, "packages/coding-agent/src/slash-commands")
    out = []
    for name in ORDER:
        src = open(os.path.join(root, name), encoding="utf-8").read()
        # top-level objects: "\t{\n" ... "\n\t},"
        for m in re.finditer(r'^\t\{\n(.*?)^\t\},?$', src, re.M | re.S):
            body = m.group(1)
            cname = str_value(body, "name")
            if not cname:
                continue
            entry = {"name": cname}
            aliases = re.search(r'^\t\taliases: \[([^\]]*)\]', body, re.M)
            if aliases:
                entry["aliases"] = [json.loads(a.strip()) for a in aliases.group(1).split(",") if a.strip()]
            icon = str_value(body, "icon")
            if icon:
                entry["icon"] = icon
            desc = str_value(body, "description")
            if desc is None:
                desc = getter_description(body)
            if desc is not None:
                entry["description"] = desc
            for key in ("acpDescription", "inlineHint", "acpInputHint"):
                v = str_value(body, key)
                if v is not None:
                    entry[key] = v
            if re.search(r'^\t\tallowArgs: true', body, re.M):
                entry["allowArgs"] = True
            if re.search(r'^\t\tsubcommandOptional: true', body, re.M):
                entry["subcommandOptional"] = True
            if re.search(r'^\t\tsubcommands: COMPACT_MODES', body, re.M):
                entry["subcommands"] = compact_modes(root)
            elif re.search(r'^\t\tsubcommands: CLI_THINKING_LEVELS', body, re.M):
                entry["subcommands"] = thinking_levels(checkout)
            else:
                subs = parse_subcommands(body)
                if subs:
                    entry["subcommands"] = subs
            if re.search(r'^\t\thandle(:|\()', body, re.M):
                entry["handle"] = True
            if re.search(r'^\t\thandleTui(:|\()', body, re.M):
                entry["handleTui"] = True
            out.append(entry)
    json.dump(out, sys.stdout, ensure_ascii=False, indent=1)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
