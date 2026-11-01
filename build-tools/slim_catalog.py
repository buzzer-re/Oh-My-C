#!/usr/bin/env python3
"""Developer-only helper: convert upstream omp packages/catalog/src/models.json (v0 layout)
into omc's compact assets/models.json.z. Not used by the build (the .z asset is committed).
omp >= 18.14 ships the compact v1 catalog: decode it first with decode_catalog_v1.py."""
import json, sys, collections, zlib
d = json.load(open(sys.argv[1]))
allm = [m for p in d.values() for m in p.values()]
byapi = collections.defaultdict(list)
for m in allm:
    byapi[m['api']].append(m)
defaults = {}
for api, ms in byapi.items():
    keys = None
    for m in ms:
        ks = set(m.get('compat', {}).keys()) if 'compat' in m else set()
        keys = ks if keys is None else keys & ks
    dd = {}
    for k in sorted(keys or []):
        c = collections.Counter(json.dumps(m['compat'][k], sort_keys=True) for m in ms)
        dd[k] = json.loads(c.most_common(1)[0][0])
    defaults[api] = dd
out = {}
for p, ms in d.items():
    out[p] = {}
    for mid, m in ms.items():
        m = dict(m)
        if 'compat' in m:
            m['compat'] = {k: v for k, v in m['compat'].items() if k not in defaults[m['api']] or defaults[m['api']][k] != v}
            if not m['compat']:
                m['compat'] = {}
        out[p][mid] = m
s = json.dumps({"defaults": defaults, "providers": out}, separators=(',', ':'))
open(sys.argv[2], 'wb').write(zlib.compress(s.encode(), 9))
print(len(s), 'disabledThinking in anthropic defaults:', 'disabledThinking' in defaults.get('anthropic-messages', {}))
