#!/usr/bin/env python3
"""Developer-only helper: convert upstream omp packages/catalog/src/compat/{rules.json,axes.ts} into omc's compact
assets/compat_rules.json.z (the model taxonomy and rule cascade src/ai/compat_detect.c evaluates for custom and
discovered models). Not used by the build (the .z asset is committed).
  slim_compat_rules.py <omp>/packages/catalog/src/compat assets/compat_rules.json.z"""
import json, os, re, sys, zlib

src, out = sys.argv[1], sys.argv[2]
rules = json.load(open(os.path.join(src, "rules.json")))
axes = open(os.path.join(src, "axes.ts")).read()

# wire axis -> compat records it applies to (axes.ts getKeyRecords); a helper array is spelled OAI there
records = {}
for m in re.finditer(r'wire\(\s*"(\w+)",\s*(OAI|\[[^\]]*\])', axes):
    raw = m.group(2)
    names = re.findall(r'"([a-z-]+)"', raw)
    if raw == "OAI" or "...OAI" in raw:
        names = ["openai", "openai-responses"] + names
    records[m.group(1)] = sorted(set(records.get(m.group(1), []) + names))
missing = sorted({k for r in rules["cascade"]["rules"] for k in r.get("wire", {})} - set(records))
if missing:
    sys.exit(f"wire axes without records (extend the axes.ts parser): {missing}")

cascade = []
for r in rules["cascade"]["rules"]:
    r = {k: v for k, v in r.items() if k != "source"}
    cascade.append(r)
classes = []
for c in rules["taxonomy"]["classes"]:
    c = dict(c)
    c["overrides"] = [{k: v for k, v in o.items() if k not in ("rationale", "provenance", "id")} for o in c["overrides"]]
    classes.append(c)
collapse = rules["taxonomy"]["collapse"]
slim = {
    "cascade": cascade,
    "classes": classes,
    "collapse": {k: collapse[k] for k in ("suffixes", "pairTokens", "lanes", "routingVariants", "effortFamilies", "variantFamilies", "providerAliases") if k in collapse},
    "discovery": {k: rules["taxonomy"]["discovery"][k] for k in ("billingVariantSuffixes", "trailingMarkers", "referenceOnlyTrailingMarkers")},
    "referenceIsolatedProviders": rules["behavior"]["referenceIsolatedProviders"],
    "wireRecords": records,
}
s = json.dumps(slim, separators=(",", ":"), ensure_ascii=False)
open(out, "wb").write(zlib.compress(s.encode(), 9))
print(f"{len(s)} B json, {os.path.getsize(out)} B compressed; {len(cascade)} rules, {len(classes)} classes, {len(records)} wire axes")
