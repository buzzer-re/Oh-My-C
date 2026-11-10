#!/usr/bin/env python3
"""Dev-only: drive `<binary> --mode rpc [args...]` through prompts one after another (each waits for the previous run's
agent_end), then exit. Prints every `notice` and `auto_retry_*` event as JSON, one per line.
  rpc_seq.py <binary> [--arg <omc arg>]... <prompt>...
Every wait is bounded (30 s per prompt); stdin of the child is a pipe, closed at the end."""
import json, subprocess, sys, threading, time

argv = sys.argv[1:]
binary = argv.pop(0)
extra = []
while len(argv) >= 2 and argv[0] == "--arg":
    extra.append(argv[1])
    argv = argv[2:]
prompts = argv
p = subprocess.Popen([binary, "--mode", "rpc", *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True)
ends = []
events = []
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            if m.get("type") == "agent_end":
                ends.append(time.time())
            elif m.get("type") in ("notice",) or str(m.get("type", "")).startswith("auto_retry"):
                events.append(m)


threading.Thread(target=reader, daemon=True).start()
for i, text in enumerate(prompts):
    p.stdin.write(json.dumps({"id": str(i + 1), "type": "prompt", "message": text}) + "\n")
    p.stdin.flush()
    deadline = time.time() + 30
    while time.time() < deadline:
        time.sleep(0.05)
        with lock:
            if len(ends) > i:
                break
    time.sleep(0.2)
with lock:
    for e in events:
        print(json.dumps(e))
try:
    p.stdin.close()
    p.wait(timeout=5)
except Exception:
    p.kill()
