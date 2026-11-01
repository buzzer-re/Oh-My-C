#!/usr/bin/env python3
"""Dev-only e2e helper: reqlog.py <log.jsonl> <path-substring> <expr> [body-substring] prints expr evaluated over the last
logged request whose path contains the substring (and whose body contains body-substring) (x = entry, h = headers, b = body)."""
import json, sys
r = [json.loads(l) for l in open(sys.argv[1])]
r = [x for x in r if sys.argv[2] in x["path"] and (len(sys.argv) < 5 or sys.argv[4] in json.dumps(x["body"]))]
x = r[-1] if r else {}
h, b = x.get("headers", {}), x.get("body", {})
v = eval(sys.argv[3])
print(*v) if isinstance(v, tuple) else print(v)
