#!/usr/bin/env python3
"""ACP forwards automatic maintenance as `_omp/compaction/start` / `_omp/compaction/end` extension notifications.
usage: acp_compaction.py <omc> <prompt 1> <prompt 2>; prints each notification's method and params keys."""
import json, os, select, subprocess, sys, time

omc, prompts = sys.argv[1], sys.argv[2:]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
seen = []
next_id = 0


def call(method, params):
    global next_id
    next_id += 1
    rid = next_id
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}) + "\n")
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line:
            sys.exit("EOF")
        m = json.loads(line)
        if str(m.get("method", "")).startswith("_omp/compaction/"):
            seen.append(m)
            continue
        if m.get("id") == rid and "method" not in m:
            return m.get("result")


call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
sid = call("session/new", {"cwd": os.getcwd(), "mcpServers": []})["sessionId"]
for text in prompts:
    call("session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": text}]})
end = time.time() + 5
while time.time() < end and not any(m["method"].endswith("/end") for m in seen):
    r, _, _ = select.select([p.stdout], [], [], 0.2)
    if r:
        line = p.stdout.readline()
        if not line:
            break
        m = json.loads(line)
        if str(m.get("method", "")).startswith("_omp/compaction/"):
            seen.append(m)
p.kill()
for m in seen:
    a = m["params"]
    print(m["method"], "id" not in m, a.get("sessionId") == sid, sorted(a.keys()), a.get("reason"), a.get("action"), a.get("aborted"), a.get("willRetry"))
