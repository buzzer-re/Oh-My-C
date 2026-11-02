#!/usr/bin/env python3
"""Dev-only: drive `<binary> --mode rpc` with JSON commands (one per argument), waiting for each command's response
and for the session to go idle, then print the compaction-related events (auto_compaction_*, notices, responses) one
JSON object per line. Used by tests/e2e/compaction.sh.
  rpc_compact.py <binary> <cmd-json>..."""
import json, subprocess, sys, threading, time

binary = sys.argv[1]
cmds = [json.loads(a) for a in sys.argv[2:]]
p = subprocess.Popen([binary, "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
st = {"running": False, "last": time.time(), "resp": set()}
lock = threading.Lock()
events = []


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            st["last"] = time.time()
            t = m.get("type")
            if t == "agent_start":
                st["running"] = True
            elif t == "agent_end":
                st["running"] = False
            elif t == "response":
                st["resp"].add(m.get("id"))
            if t in ("auto_compaction_start", "auto_compaction_end", "notice", "response"):
                events.append(m)


threading.Thread(target=reader, daemon=True).start()
for i, c in enumerate(cmds):
    c.setdefault("id", str(i))
    p.stdin.write(json.dumps(c) + "\n")
    p.stdin.flush()
    deadline = time.time() + 90
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            if c["id"] in st["resp"] and not st["running"] and time.time() - st["last"] > 0.8:
                break
for e in events:
    print(json.dumps(e))
try:
    p.stdin.close()
    p.wait(timeout=5)
except Exception:
    p.kill()
