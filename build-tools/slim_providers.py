#!/usr/bin/env python3
"""Developer-only helper: build omc's assets/providers.json.z from omp's compiled catalog rules
(packages/catalog/src/compat/rules.json). Not used by the build (the .z asset is committed).
  providers     rules.providers without the seed model rows (those are in models.json.z), plus
                "keyless": true for omp's keyless providers (model-registry #keylessProviders:
                local, web and the implicit local servers ollama / llama.cpp / lm-studio / apple)
  auth          rules.auth.providers (login descriptors)
  roleDefaults  rules.behavior.roleDefaults (built-in role selector chains, behavior.kdl role-defaults)
Usage: slim_providers.py rules.json out.z"""
import json, sys, zlib

r = json.load(open(sys.argv[1]))
KEYLESS = ['local', 'web', 'ollama', 'llama.cpp', 'lm-studio', 'apple']
provs = {}
for pid, v in r['providers'].items():
    v = {k: x for k, x in v.items() if k != 'seed'}
    provs[pid] = v
for pid in KEYLESS:
    provs.setdefault(pid, {'id': pid})['keyless'] = True
out = {'providers': provs, 'auth': r['auth']['providers'], 'roleDefaults': r['behavior']['roleDefaults']}
s = json.dumps(out, separators=(',', ':'))
open(sys.argv[2], 'wb').write(zlib.compress(s.encode(), 9))
print(len(provs), 'providers,', len(out['auth']), 'login descriptors,', len(out['roleDefaults']), 'role chains')
