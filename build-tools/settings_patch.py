#!/usr/bin/env python3
"""Dev-time only: update assets/config/settings-registry.json and assets/tui/settings-ui.json between two upstream
tags by reading the `register({...})` blocks as text. It never runs upstream code (extract_settings.sh does, through
the Bun inside omp).

Usage: settings_patch.py <omp git checkout> <old tag or commit> <new tag or commit> [--apply]

Prints every setting added, removed or changed, and the values it could not resolve from source (a default computed
at runtime, say): fix those by hand after --apply. `order` is omp config list's position; new keys take the order of
the setting registered just before them."""
import json
import re
import subprocess
import sys

UP, OLD, NEW = sys.argv[1:4]
APPLY = "--apply" in sys.argv
REG = "assets/config/settings-registry.json"
UI = "assets/tui/settings-ui.json"


def git(*args):
    return subprocess.run(["git", "-C", UP, *args], check=True, capture_output=True, text=True).stdout


def skip_string(s, i):
    q = s[i]
    i += 1
    while i < len(s):
        c = s[i]
        if c == "\\":
            i += 2
            continue
        if c == q:
            return i + 1
        if q == "`" and c == "$" and s[i + 1 : i + 2] == "{":
            i = match_bracket(s, i + 1) + 1
            continue
        i += 1
    return i


def match_bracket(s, i):
    """Index of the bracket closing the one at s[i]."""
    depth = 0
    while i < len(s):
        c = s[i]
        if c in "\"'`":
            i = skip_string(s, i)
            continue
        if s.startswith("//", i):
            i = s.find("\n", i)
            i = len(s) if i < 0 else i
            continue
        if s.startswith("/*", i):
            i = s.find("*/", i)
            i = len(s) if i < 0 else i + 2
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return len(s) - 1


def props(body):
    """Top-level `key: value` pairs of an object literal's body, as raw text."""
    out = {}
    i = 0
    while i < len(body):
        g = re.compile(r"\s*(?://[^\n]*\n\s*)*(?:/\*.*?\*/\s*)*get\s+([A-Za-z_]\w*)\s*\(\s*\)\s*\{", re.S).match(body, i)
        if g:  # a getter (`get description() { ... }`): its body as raw text, flagged so a change is reported, not guessed
            j = match_bracket(body, g.end() - 1) + 1
            out[g.group(1)] = "<getter>" + body[g.end() : j - 1].strip()
            i = j + 1 if j < len(body) and body[j] == "," else j
            continue
        m = re.compile(r"\s*(?://[^\n]*\n\s*)*(?:/\*.*?\*/\s*)*([A-Za-z_]\w*)\s*(:|\(|,)", re.S).match(body, i)
        if not m:
            break
        key = m.group(1)
        j = m.end()
        if m.group(2) == "(":  # a method: skip its parameters and body
            j = match_bracket(body, j - 1) + 1
            while j < len(body) and body[j] in " \n\t":
                j += 1
            if body[j : j + 1] == "{":
                j = match_bracket(body, j) + 1
            out[key] = "<function>"
        elif m.group(2) == ",":
            out[key] = key
            i = j
            continue
        else:
            start = j
            while j < len(body):
                c = body[j]
                if c in "\"'`":
                    j = skip_string(body, j)
                    continue
                if c in "([{":
                    j = match_bracket(body, j) + 1
                    continue
                if c == ",":
                    break
                if body.startswith("//", j):
                    k = body.find("\n", j)
                    j = len(body) if k < 0 else k
                    continue
                j += 1
            out[key] = body[start:j].strip()
        i = j + 1 if j < len(body) and body[j] == "," else j
    return out


def literal(raw):
    """A JSON value for a source literal, or None when it isn't one (the second result says which)."""
    raw = raw.strip()
    raw = re.sub(r"\s+as\s+const$", "", raw)
    if raw in ("true", "false"):
        return raw == "true", True
    if raw == "undefined":
        return None, False
    if raw == "null":
        return None, True
    if re.fullmatch(r"-?\d[\d_]*(\.\d+)?", raw):
        v = raw.replace("_", "")
        return (float(v) if "." in v else int(v)), True
    m = re.fullmatch(r"\"((?:[^\"\\]|\\.)*)\"|'((?:[^'\\]|\\.)*)'|`((?:[^`\\$]|\\.)*)`", raw, re.S)
    if m:
        t = next(g for g in m.groups() if g is not None)
        return t.encode().decode("unicode_escape").encode("latin-1", "ignore").decode("utf8", "ignore") if "\\" in t else t, True
    # an array or object of plain literals
    if raw[:1] in "[{":
        j = js_json(raw)
        if j is None:
            return None, False
        try:
            return json.loads(j), True
        except json.JSONDecodeError:
            return None, False
    return None, False


def js_json(raw):
    """JSON text for a JS array/object literal of plain values: keys quoted and trailing commas dropped only outside
    strings (a description holding `word:` stays intact), '...' and `...` strings re-encoded; None for a template with
    interpolation."""
    out = []
    i = 0
    while i < len(raw):
        if raw[i] in "\"'`":
            j = skip_string(raw, i)
            tok = raw[i:j]
            if tok[0] == "`" and "${" in tok:
                return None
            text, _ = literal(tok)
            out.append(json.dumps(text, ensure_ascii=False))
            i = j
            continue
        j = i
        while j < len(raw) and raw[j] not in "\"'`":
            j += 1
        code = re.sub(r"([{,]\s*)([A-Za-z_]\w*)\s*:", r'\1"\2":', raw[i:j])
        out.append(re.sub(r",\s*([}\]])", r"\1", code))
        i = j
    return "".join(out)


def settings_at(tag):
    out = {}
    files = git("grep", "-lE", r"\bregister\(\{", tag, "--", "packages/coding-agent/src", "packages/*/src").splitlines()
    for f in sorted({x.split(":", 1)[1] for x in files}):
        if "/test/" in f:
            continue
        src = git("show", f"{tag}:{f}")
        for m in re.finditer(r"\bregister\(\{", src):
            start = m.end() - 1
            end = match_bracket(src, start)
            p = props(src[start + 1 : end])
            sid, ok = literal(p.get("id", ""))
            if not ok or not isinstance(sid, str):
                continue
            ui = props(p["ui"].strip()[1:-1]) if p.get("ui", "").startswith("{") else {}
            out[sid] = {"file": f, "pos": m.start(), "p": p, "ui": ui, "src": src}
    return out


def constant(src, raw):
    """`DEFAULT_X` or `A.b` declared in the same file as a plain literal."""
    m = re.search(r"\b(?:const|export const)\s+" + re.escape(raw) + r"\b[^=]*=\s*([^;\n]+)", src)
    return literal(m.group(1)) if m else (None, False)


def field(entry, raw, unresolved, name):
    v, ok = literal(raw)
    if not ok:
        v, ok = constant(entry["src"], raw)
    if not ok and raw != "undefined":
        unresolved.append(f"{name}: {raw[:70]}")
    return v, ok


def strtext(raw):
    if raw is None:
        return None
    parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"|`((?:[^`\\]|\\.)*)`|'((?:[^'\\]|\\.)*)'", raw, re.S)
    return "".join(a or b or c for a, b, c in parts).replace('\\"', '"').replace("\\n", "\n").replace("\\'", "'") or None


def describe(sid, e):
    unresolved = []
    p, ui = e["p"], e["ui"]
    r = {"id": sid, "type": literal(p.get("type", '""'))[0], "tab": strtext(ui.get("tab")) or "internal"}
    if "default" in p:
        d, ok = field(e, p["default"], unresolved, "default")
        if ok:
            r["default"] = d
    if "values" in p:
        v, ok = field(e, p["values"], unresolved, "values")
        if ok:
            r["values"] = v
    raw = ui.get("description")
    if raw and raw.startswith("<getter>"):  # computed at runtime upstream (key hints etc.): write it by hand
        unresolved.append("description: computed by a getter; update its text by hand")
        raw = None
    desc = strtext(raw)
    if desc:
        r["description"] = desc
    if "env" in p:
        r["env"], _ = literal(p["env"])
    return r, unresolved


def ui_row(sid, r, e):
    """The settings panel row omp's settings-defs.ts entryToSettingDef makes of a registration, or None for one the
    panel does not show (a number or array without choices)."""
    ui = e["ui"]
    if not ui:
        return None
    schema = r["type"]
    options = None  # None: no `options`; "runtime": host-supplied choices; else the list
    if "options" in ui:
        o, ok = literal(ui["options"])
        options = "runtime" if o == "runtime" else (o if ok else [])
    credential = literal(e["p"].get("credential", "false"))[0] is True
    secret = False
    if schema == "boolean":
        kind = "boolean"
    elif schema == "enum":
        kind = "enum" if options is None else "submenu"
    elif schema == "number":
        if options is None or options == "runtime":
            return None
        kind = "submenu"
    elif schema == "string":
        if options is None:
            kind, secret = "text", credential
        else:
            kind = "submenu"
    elif schema == "array":
        if literal(ui.get("editor", '""'))[0] == "json":
            kind, secret, options = "text", credential, None
        elif options is None or options == "runtime":
            return None
        else:
            kind = "multiselect"
    elif schema == "record":
        kind, options = ("providerLimits" if sid == "providers.maxInFlightRequests" else "text"), None
    else:
        return None
    row = {"path": sid, "schemaType": schema, "type": kind}
    if "default" in r:
        row["default"] = r["default"]
    row.update({"tab": r["tab"], "label": strtext(ui.get("label")), "description": r.get("description")})
    if strtext(ui.get("group")):
        row["group"] = strtext(ui.get("group"))
    if kind == "enum" and "values" in r:
        row["values"] = r["values"]
    if options is not None and kind != "text":
        row["options"] = options
    if kind == "multiselect" and literal(ui.get("ordered", "false"))[0] is True:
        row["ordered"] = True
    if secret:
        row["secret"] = True
    return row


old, new = settings_at(OLD), settings_at(NEW)
reg = json.load(open(REG))
tab = json.load(open(UI))
by_id = {x["id"]: x for x in reg}
ui_by = {x["path"]: x for x in tab["settings"]}
print(f"parsed {len(old)} settings at {OLD}, {len(new)} at {NEW}; registry has {len(reg)}")
print("in the registry but not found in source:", sorted(set(by_id) - set(new) - set(old))[:12])

added = [s for s in new if s not in old]
removed = [s for s in old if s not in new]
changed = []
for s in new:
    if s in old:
        keys = ("type", "default", "values", "env")
        a, b = old[s]["p"], new[s]["p"]
        ua, ub = old[s]["ui"], new[s]["ui"]
        diff = [k for k in keys if a.get(k) != b.get(k)] + [f"ui.{k}" for k in sorted(set(ua) | set(ub)) if ua.get(k) != ub.get(k)]
        if diff:
            changed.append((s, diff))
print("ADDED", added)
print("REMOVED", removed)
for s, d in changed:
    print("CHANGED", s, d)

problems = []
for s in added:
    r, un = describe(s, new[s])
    problems += [f"{s} {u}" for u in un]
    keys = list(new)
    prev = next((k for k in reversed(keys[: keys.index(s)]) if k in by_id), None)
    r["order"] = by_id[prev]["order"] if prev else -1
    by_id[s] = r
    reg.append(r)
    row = ui_row(s, r, new[s])
    if row:
        tab["settings"].append(row)
        tab["settings"].sort(key=lambda x: by_id.get(x.get("path"), {}).get("order", 1 << 30))  # the panel lists rows in registration order
        ui_by[s] = row
for s in removed:
    reg[:] = [x for x in reg if x["id"] != s]
    tab["settings"][:] = [x for x in tab["settings"] if x["path"] != s]
for s, d in changed:
    r, un = describe(s, new[s])
    problems += [f"{s} {u}" for u in un]
    cur = by_id.get(s)
    if cur:
        for k in ("default", "values", "description", "type", "env"):
            if k in r:
                cur[k] = r[k]
    row = ui_by.get(s)
    fresh = ui_row(s, r, new[s])
    if row and fresh is None and new[s]["ui"]:  # the panel no longer shows it (a number or array that lost its choices)
        tab["settings"][:] = [x for x in tab["settings"] if x["path"] != s]
    elif row and fresh:
        if fresh["type"] != row.get("type"):  # a new kind of row: keys of the old kind go
            for k in ("options", "values", "secret", "ordered"):
                if k not in fresh:
                    row.pop(k, None)
        for k, v in fresh.items():
            if k in ("options",) and not v:
                continue
            row[k] = v
print("UNRESOLVED (fix by hand):")
for p in problems:
    print(" ", p)
if APPLY:
    json.dump(reg, open(REG, "w", encoding="utf8"), ensure_ascii=False, separators=(",", ":"))
    open(REG, "a").write("\n")
    json.dump(tab, open(UI, "w", encoding="utf8"), ensure_ascii=False, separators=(",", ":"))
    open(UI, "a").write("\n")
    print("applied")
