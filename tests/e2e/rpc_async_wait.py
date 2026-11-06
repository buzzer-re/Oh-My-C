#!/usr/bin/env python3
"""e2e (WP-22b): the parent waits on its background subagents like omp. A run that ends while jobs can still wake
it reports agent_end {isTerminal: false, awaitingAsyncWork: true} and prompt_result {sessionSettled: false}; the
results arrive as an omp-shaped async-result custom message, and only the wake run's terminal agent_end closes
the stretch with session_settled (no prompt_result of its own). A subagent that spawned its own child answers
only after that child's result reached it. The task result carries the Tip/Coordinate advisories, the child's
output lands in `<artifacts>/<id>.md`, and subagent requests carry no todo tool and no personality block.
Usage: rpc_async_wait.py <omc> <mock request log>"""
import json, os, subprocess, sys, threading, time

omc, reqlog = sys.argv[1], sys.argv[2]
start_req = sum(1 for _ in open(reqlog)) if os.path.exists(reqlog) else 0
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
lock = threading.Lock()
frames = []

def reader():
    for line in p.stdout:
        with lock:
            frames.append(json.loads(line))

threading.Thread(target=reader, daemon=True).start()

def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()

def wait(pred, what, timeout=25):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for f in frames:
                if pred(f): return f
        time.sleep(0.05)
    raise SystemExit("timeout " + what)

send({"id": "q1", "type": "set_subagent_subscription", "level": "progress"})
inner = "call:task:" + json.dumps({"context": "inner", "tasks": [{"task": "slow:1:grand done"}]})
outer = "call:task:" + json.dumps({"context": "ctx", "tasks": [{"task": inner}, {"task": "say:sibling done"}]})
send({"id": "p1", "type": "prompt", "message": outer})
te = wait(lambda f: f.get("type") == "tool_execution_end" and f.get("toolName") == "task", "task end")
text = te["result"]["content"][0]["text"]
print("advisories", "Tip: this call spawned 2 generic `task` workers" in text, "2 siblings run together" in text)
end1 = wait(lambda f: f.get("type") == "agent_end", "first agent_end")
print("first end", end1.get("isTerminal"), end1.get("awaitingAsyncWork"))
pr = wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "p1", "prompt_result")
print("prompt_result", pr["sessionSettled"])
settled = wait(lambda f: f.get("type") == "session_settled", "session_settled", timeout=40)
with lock:
    snap = list(frames)
customs = [f["message"] for f in snap if f.get("type") == "message_start" and f["message"].get("customType") == "async-result"]
c = customs[0] if customs else {}
print("delivery", len(customs) >= 1, c.get("role"), c.get("display"), c.get("attribution"),
      c.get("details", {}).get("meta", {}).get("source", {}).get("value"), c.get("details", {}).get("jobs", [{}])[0].get("status"))
ends = [f for f in snap if f.get("type") == "agent_end"]
print("last end", ends[-1].get("isTerminal"), "awaitingAsyncWork" not in ends[-1])
print("wake prompt_results", sum(1 for f in snap if f.get("type") == "prompt_result" and "id" not in f))
# the nested child answered after its own child's result: its delivered output is the post-delivery answer
outer_child = [m for m in customs if "plain answer" in m.get("content", "")]
print("nested waited", bool(outer_child))
done = [f["payload"] for f in snap if f.get("type") == "subagent_lifecycle" and f["payload"]["status"] == "completed"]
mds = [d["sessionFile"][:-len(".jsonl")] + ".md" for d in done if d.get("sessionFile")]
print("output md", len(mds) >= 2 and all(os.path.exists(m) for m in mds), any(open(m).read().strip() == "sibling done" for m in mds if os.path.exists(m)))
reqs = [json.loads(l) for l in open(reqlog).read().splitlines()[start_req:]]
def user_text(r):
    us = [m for m in r.get("messages", []) if m.get("role") == "user"]
    c = us[0]["content"] if us else ""
    return c if isinstance(c, str) else "".join(x.get("text", "") for x in c if isinstance(x, dict))
sub = [r for r in reqs if "Complete assignment thoroughly" in user_text(r)]
tools = [t["function"]["name"] for t in sub[0].get("tools", [])] if sub else []
sysc = "".join(m["content"] if isinstance(m["content"], str) else "" for m in sub[0]["messages"] if m["role"] in ("system", "developer")) if sub else ""
print("subagent request", bool(sub), "todo" not in tools, "# Personality" not in sysc, "task" in tools)
p.stdin.close()
p.wait(timeout=10)
