#!/usr/bin/env python3
"""Developer-only helper: decode omp's compact v1 catalog (packages/catalog/src/models.json,
model-catalog-format.ts) into the v0 {provider: {id: model}} layout that slim_catalog.py reads.
Usage: decode_catalog_v1.py <omp>/packages/catalog/src/models.json out.json [models.db]

The optional models.db is the model cache a fresh omp writes to its agent dir on first start
(PI_CODING_AGENT_DIR=<tmp> omp): providers that have no bundled rows (charm-hyper, factory-droid:
discovered online even without credentials) are taken from it, so they list like in omp's hub."""
import json, sqlite3, sys

d = json.load(open(sys.argv[1]))
assert d['version'] == 1, 'unsupported catalog version'
keys, shapes, nodes = d['keys'], d['booleanShapes'], d['nodes']
UNDEF = object()


def dec_obj(obj, node):
    props = node[1]
    for i in range(0, len(props), 2):
        v = dec(props[i + 1])
        if v is not UNDEF:
            obj[keys[props[i]]] = v
    for i, k in enumerate(shapes[node[2]]):
        obj[keys[k]] = ((node[3][i >> 5] >> (i % 32)) & 1) != 0


def dec(v):
    if not isinstance(v, list):
        return v
    if v[0] == -1:
        return UNDEF
    n = nodes[v[0]]
    if n[0] == 1:
        return [x for x in (dec(e) for e in n[1]) if x is not UNDEF]
    o = {}
    dec_obj(o, n)
    return o


bases = {api: dec(v) for api, v in d['compat']}
out = {}
for prov, consts, rows in d['providers']:
    c = dec(consts)
    ms = {}
    for row in rows:
        mid, enc = row[0], row[1]
        m = {'id': mid}
        m.update(c)
        dec_obj(m, nodes[enc[0]])
        if len(row) > 2:
            removed, changes = row[2]
            comp = dict(bases[m['api']])
            for k in removed:
                comp.pop(keys[k], None)
            comp.update(dec(changes))
            m['compat'] = comp
        ms[mid] = m
    out[prov] = ms
if len(sys.argv) > 3:
    db = sqlite3.connect(sys.argv[3])
    for pid, models in db.execute('select provider_id, models from model_cache order by provider_id'):
        prov = pid.split(':')[0]
        if prov in out:
            continue
        ms = json.loads(models)
        out[prov] = {m['id']: m for m in ms if m.get('provider') == prov}
        print('added', prov, len(out[prov]), 'models from the omp model cache')
json.dump(out, open(sys.argv[2], 'w'))
print(len(out), 'providers,', sum(len(v) for v in out.values()), 'models')
