#!/usr/bin/env python3
"""A `/new` boundary ends plan mode (omp #reconcileModeFromSession -> #clearTransientModeState): an RPC session
started with omc's --plan-mode carries plan-mode-active.md in its system prompt until new_session.
Usage: plan_new_rpc.py <omc>. Prints "plan new ok" or the failed checks."""
import json, subprocess, sys

p = subprocess.Popen([sys.argv[1], "--model", "mock/mock-1", "--mode", "rpc", "--no-session", "--plan-mode"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def call(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()
    for line in p.stdout:
        m = json.loads(line)
        if m.get("type") == "response" and m.get("id") == o["id"]:
            return m


def sysprompt(r):
    s = (r or {}).get("data", {}).get("systemPrompt")
    return "".join(s) if isinstance(s, list) else (s or "")


fails = []
before = sysprompt(call({"id": "s1", "type": "get_state"}))
if "Plan mode active" not in before: fails.append("plan prompt missing before new_session")
call({"id": "n", "type": "new_session"})
after = sysprompt(call({"id": "s2", "type": "get_state"}))
if "Plan mode active" in after: fails.append("plan prompt kept after new_session")
p.stdin.close()
p.wait(timeout=10)
print("plan new ok" if not fails else "\n".join(fails))
