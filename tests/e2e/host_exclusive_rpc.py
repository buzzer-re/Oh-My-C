#!/usr/bin/env python3
"""set_host_tools `exclusive` over RPC (tests/e2e/run.sh): shared host tools from one assistant message are all called
before any answer, exclusive ones arrive one at a time and in message order, and a non-boolean value is refused.
usage: host_exclusive_rpc.py <omc>
Prints "host exclusive ok" or the first failing step."""
import json, queue, subprocess, sys, threading, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc", "--no-session"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
calls = queue.Queue()
responses = queue.Queue()
ended = queue.Queue()


def reader():
    for line in p.stdout:
        m = json.loads(line)
        t = m.get("type")
        if t == "host_tool_call":
            calls.put(m)
        elif t == "response":
            responses.put(m)
        elif t == "agent_end":
            ended.put(m)


threading.Thread(target=reader, daemon=True).start()


def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()


def fail(step, detail=""):
    print(f"host exclusive FAIL {step}: {detail!r}")
    p.kill()
    sys.exit(1)


def command(o):
    send(o)
    while True:
        r = responses.get(timeout=20)
        if r.get("id") == o["id"]:
            return r


def answer(call):
    send({"type": "host_tool_result", "id": call["id"], "result": {"content": [{"type": "text", "text": "ok"}]}})


def tool(name, **extra):
    return {"name": name, "description": "Test tool " + name, "parameters": {"type": "object", "properties": {}}, **extra}


def run_batch(pid):
    send({"id": pid, "type": "prompt", "message": "batch:" + json.dumps([["first_tool", {}], ["second_tool", {}], ["third_tool", {}]])})


def drain_end():
    try:
        ended.get(timeout=20)
    except queue.Empty:
        fail("agent_end")


r = command({"id": "bad", "type": "set_host_tools", "tools": [tool("first_tool", exclusive="yes")]})
if r.get("success") or "exclusive must be a boolean" not in str(r.get("error")):
    fail("non-boolean refused", r)

# shared (the default): every call arrives without any answer
r = command({"id": "shared", "type": "set_host_tools", "tools": [tool("first_tool"), tool("second_tool"), tool("third_tool")]})
if not r.get("success"):
    fail("set shared", r)
run_batch("p1")
got = []
try:
    for _ in range(3):
        got.append(calls.get(timeout=20))
except queue.Empty:
    fail("shared calls all arrive unanswered", [c["toolName"] for c in got])
for c in got:
    answer(c)
drain_end()

# exclusive: one at a time, in message order
tools = [tool(n, exclusive=True) for n in ("first_tool", "second_tool", "third_tool")]
r = command({"id": "excl", "type": "set_host_tools", "tools": tools})
if not r.get("success"):
    fail("set exclusive", r)
run_batch("p2")
for name in ("first_tool", "second_tool", "third_tool"):
    try:
        c = calls.get(timeout=20)
    except queue.Empty:
        fail(f"{name} never called")
    if c["toolName"] != name:
        fail("message order", (name, c["toolName"]))
    time.sleep(0.4)
    if not calls.empty():
        fail(f"a call arrived before {name} was answered", calls.get()["toolName"])
    answer(c)
drain_end()
p.kill()
print("host exclusive ok")
