#!/usr/bin/env python3
"""e2e (WP-22): RPC subagent frames and commands (rpc-subagents.ts): set_subagent_subscription, subagent_lifecycle /
subagent_progress / subagent_event frames, get_subagents, get_subagent_messages (persisted `<artifacts>/<id>.jsonl`
transcript), steer_subagent and cancel_subagent. Usage: rpc_subagents.py <omc>"""
import json, subprocess, sys, threading, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
lock = threading.Lock()
frames, pending = [], {}

def reader():
    for line in p.stdout:
        m = json.loads(line)
        with lock:
            frames.append(m)
            if m.get("type") == "response":
                pending[m.get("id")] = m
            elif m.get("type") == "prompt_result":
                pending["prompt:" + str(m.get("id"))] = m

threading.Thread(target=reader, daemon=True).start()

def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()

def call(o, key=None):
    send(o)
    key = key or o["id"]
    for _ in range(600):
        with lock:
            if key in pending: return pending.pop(key)
        time.sleep(0.05)
    raise SystemExit("timeout " + key)

def wait_frame(pred, what):
    for _ in range(600):
        with lock:
            for i, f in enumerate(frames):
                if pred(f):
                    del frames[i]
                    return f
        time.sleep(0.05)
    raise SystemExit("timeout " + what)

def lifecycle(status):
    return lambda f: f.get("type") == "subagent_lifecycle" and f["payload"]["status"] == status

print("level", call({"id": "l1", "type": "set_subagent_subscription", "level": "events"})["data"])
task = lambda t: "call:task:" + json.dumps({"context": "ctx", "tasks": [{"task": t}]})

# a slow child: listed while it runs, steerable, its transcript readable afterwards
send({"id": "p1", "type": "prompt", "message": task("slow:2:first answer")})
st = wait_frame(lifecycle("started"), "started")["payload"]
aid = st["id"]
print("started", st["agent"], st["detached"], st["index"], st["sessionFile"].endswith("/" + aid + ".jsonl"))
subs = call({"id": "g1", "type": "get_subagents"})["data"]["subagents"]
print("listed", len(subs), subs[0]["id"] == aid, subs[0]["status"], subs[0]["agent"])
steer = call({"id": "s1", "type": "steer_subagent", "subagentId": aid, "message": "say:steered reply"})
print("steer", steer["success"])
prog = wait_frame(lambda f: f.get("type") == "subagent_progress", "progress")["payload"]
print("progress", prog["progress"]["id"] == aid, prog["task"].startswith("Complete assignment thoroughly:"), prog["assignment"])
ev = wait_frame(lambda f: f.get("type") == "subagent_event", "event")["payload"]
print("event", ev["id"] == aid, bool(ev["event"]["type"]))
done = wait_frame(lifecycle("completed"), "completed")["payload"]
print("completed", done["id"] == aid)
for _ in range(200):
    with lock:
        if "prompt:p1" in pending: break
    time.sleep(0.05)
subs = call({"id": "g2", "type": "get_subagents"})["data"]["subagents"]
print("after", len(subs))
tr = call({"id": "m1", "type": "get_subagent_messages", "subagentId": aid})["data"]
roles = [m["role"] for m in tr["messages"]]
texts = json.dumps(tr["messages"])
print("transcript", tr["fromByte"], tr["nextByte"] > 0, tr["reset"], roles[0], roles.count("assistant") >= 1,
      any(e.get("type") == "session" for e in tr["entries"]), "steered reply" in texts)
tail = call({"id": "m2", "type": "get_subagent_messages", "sessionFile": tr["sessionFile"], "fromByte": tr["nextByte"]})["data"]
print("tail", tail["fromByte"] == tr["nextByte"], len(tail["messages"]))
over = call({"id": "m3", "type": "get_subagent_messages", "subagentId": aid, "fromByte": 10 ** 9})["data"]
print("reset", over["reset"], over["fromByte"])

# a child that never answers in time: cancel_subagent kills it, its lifecycle ends aborted
send({"id": "p2", "type": "prompt", "message": task("slow:30:never")})
aid2 = wait_frame(lifecycle("started"), "started 2")["payload"]["id"]
time.sleep(0.3)
print("cancel", call({"id": "c1", "type": "cancel_subagent", "subagentId": aid2})["data"])
print("aborted", wait_frame(lifecycle("aborted"), "aborted")["payload"]["id"] == aid2)
print("cancel again", call({"id": "c2", "type": "cancel_subagent", "subagentId": aid2})["data"])
print("level off", call({"id": "l2", "type": "set_subagent_subscription", "level": "off"})["data"])
p.stdin.close()
p.wait(timeout=20)
