#!/usr/bin/env python3
"""Dev-only: drive `<binary> --mode rpc` with one prompt and exit once the session has been idle for a while
(no agent_start within IDLE seconds after an agent_end). Used by request_diff.sh RD_MODE=rpc so session-level
continuations that print mode suppresses (todo reminders) reach the mock model.
  rpc_drive.py <binary> <prompt> [idle-seconds]"""
import json, subprocess, sys, threading, time

binary, prompt = sys.argv[1], sys.argv[2]
IDLE = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
p = subprocess.Popen([binary, "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
state = {"running": False, "last_end": None, "events": []}
lock = threading.Lock()

def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        t = m.get("type")
        with lock:
            if t == "agent_start":
                state["running"] = True
            elif t == "agent_end":
                state["running"] = False
                state["last_end"] = time.time()
            elif t in ("todo_reminder", "notice", "cache_warming_start", "cache_warming_end"):
                state["events"].append(m)

threading.Thread(target=reader, daemon=True).start()
p.stdin.write(json.dumps({"id": "1", "type": "prompt", "message": prompt}) + "\n")
p.stdin.flush()
deadline = time.time() + 120
while time.time() < deadline:
    time.sleep(0.1)
    with lock:
        if not state["running"] and state["last_end"] and time.time() - state["last_end"] > IDLE:
            break
for e in state["events"]:
    print(json.dumps(e))
try:
    p.stdin.close()
    p.wait(timeout=5)
except Exception:
    p.kill()
