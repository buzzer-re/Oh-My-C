#!/usr/bin/env python3
"""Dev-only: drive `<binary> --mode rpc` through the WP-14 commands (set_fast_mode, set_slow_mode, set_cache_warming
and the /fast, /slow, /extended-context slash commands as prompts) and print one normalized line per response or
command output, so omp and omc can be diffed; tests/e2e/run.sh compares omc with tests/e2e/tiers_rpc.expected
(recorded from omp v18.14.2 on openai/gpt-5.2).
  tiers_rpc.py <binary>"""
import json, subprocess, sys, threading, time, queue

binary = sys.argv[1]
p = subprocess.Popen([binary, "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
q = queue.Queue()


def reader():
    for line in p.stdout:
        try:
            q.put(json.loads(line))
        except ValueError:
            pass


threading.Thread(target=reader, daemon=True).start()


def send(cmd, timeout=20):
    p.stdin.write(json.dumps(cmd) + "\n")
    p.stdin.flush()
    deadline = time.time() + timeout
    out = []
    while time.time() < deadline:
        try:
            m = q.get(timeout=0.2)
        except queue.Empty:
            continue
        if m.get("type") == "response" and m.get("id") == cmd["id"]:
            out.append(m)
            if cmd["type"] != "prompt":
                return out
            deadline = min(deadline, time.time() + 1.5)  # slash output follows the acknowledgement
        elif cmd["type"] == "prompt" and m.get("type") != "response":
            out.append(m)
            if m.get("type") == "agent_end":
                return out
    return out


def text(msg):
    c = msg.get("content")
    if isinstance(c, str):
        return c
    return "".join(b.get("text", "") for b in c or [] if b.get("type") == "text")


n = 0
for cmd in [
    {"type": "set_fast_mode", "enabled": True},
    {"type": "set_fast_mode", "enabled": False},
    {"type": "set_slow_mode", "enabled": "yes"},
    {"type": "set_slow_mode", "enabled": True},
    {"type": "set_slow_mode", "enabled": False},
    {"type": "set_cache_warming", "mode": "bogus"},
    {"type": "set_cache_warming", "mode": "streaming"},
]:
    n += 1
    cmd["id"] = str(n)
    for r in send(cmd):
        print(json.dumps({k: r.get(k) for k in ("command", "success", "data", "error")}, sort_keys=True), flush=True)
for text_cmd in ["/fast status", "/fast on", "/fast", "/fast bogus", "/slow status", "/slow on", "/slow", "/extended-context status",
                 "/extended-context on", "/extended-context off"]:
    n += 1
    for m in send({"id": str(n), "type": "prompt", "message": text_cmd}):
        t = m.get("type")
        if t == "message_end":
            msg = m.get("message") or {}
            print(f"{text_cmd} -> [{msg.get('role')}] {text(msg)}", flush=True)
        elif t in ("notice", "command_output"):
            print(f"{text_cmd} -> {t}: {m.get('message') or m.get('text') or m.get('output')}", flush=True)
        elif t == "response" and not m.get("success"):  # (omp writes the output before the acknowledgement)
            print(f"{text_cmd} -> failed: {m.get('error')}", flush=True)
try:
    p.stdin.close()
    p.wait(timeout=5)
except Exception:
    p.kill()
