#!/usr/bin/env python3
"""Plan-mode e2e over RPC: write a plan, then propose it with `write xd://propose`.
Usage: plan_rpc.py <omc> [extra omc args...]. Prints assistant texts, tool results and notices."""
import json, subprocess, sys, threading, time

PLAN = "# Retry plan\n\n## Steps\n\n1. Add retry.\n"
p = subprocess.Popen([sys.argv[1], "--model", "mock/mock-1", "--mode", "rpc", *sys.argv[2:]], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, text=True)
done = {}

def reader():
    for line in p.stdout:
        m = json.loads(line)
        t = m.get("type")
        if t == "prompt_result":
            done[m.get("id")] = True
        elif t == "message_end" and m["message"]["role"] == "assistant":
            c = m["message"]["content"]
            print("assistant", " ".join(b.get("text", "") for b in c if b.get("type") == "text"), flush=True)
        elif t == "tool_execution_end":
            r = m.get("result") or {}
            txt = " ".join(b.get("text", "") for b in r.get("content", []) if b.get("type") == "text")
            print("tool", m.get("toolName"), json.dumps(r.get("details"), sort_keys=True), txt, flush=True)
        elif t == "notice":
            print("notice", m.get("level"), m.get("message") or m.get("text"), flush=True)

threading.Thread(target=reader, daemon=True).start()

def prompt(i, msg):
    p.stdin.write(json.dumps({"id": i, "type": "prompt", "message": msg}) + "\n")
    p.stdin.flush()
    for _ in range(400):
        if i in done: return
        time.sleep(0.05)

prompt("1", "call:write:" + json.dumps({"path": "local://retry-plan.md", "content": PLAN}))
prompt("2", 'call:write:{"path":"xd://propose","content":"retry"}')
p.stdin.close()
p.wait(timeout=10)
