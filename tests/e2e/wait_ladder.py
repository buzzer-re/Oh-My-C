#!/usr/bin/env python3
"""e2e: `wait` (tools/wait.ts): a wait with nothing new returns after the ladder's window with a nudge from the second
rung on, jobs finishing moments apart share one result, a progress note neither ends a wait nor interrupts bash while a
direct message ends it, and a subagent cannot broadcast.
Usage: wait_ladder.py <omc> <request log> <models.yml> <scratch dir> ladder|lone|peers"""
import json, os, shutil, subprocess, sys, time

omc, reqlog, models, scratch, part = sys.argv[1:6]
agent, work, home = (os.path.join(scratch, d) for d in ("agent", "work", "home"))
for d in (agent, work, home): os.makedirs(d, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
env = dict(os.environ, OMC_CODING_AGENT_DIR=agent, HOME=home)
fails = 0


def check(name, ok, got=""):
    global fails
    if not ok:
        fails += 1
        print(f"FAIL {name}: {got!r}")


def run(script):
    open(reqlog, "w").close()
    t0 = time.time()
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "seq:" + json.dumps(script)], cwd=work, env=env,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=28)
    return time.time() - t0


def requests():
    """The session's own requests: a subagent's script travels as its assignment."""
    def own(r):
        for m in r.get("messages", []):
            c = m.get("content")
            blocks = [c] if isinstance(c, str) else [p.get("text", "") for p in c or [] if isinstance(p, dict)]
            if m.get("role") == "user" and any(b.startswith("seq:") for b in blocks): return True
        return False
    return [r for r in map(json.loads, open(reqlog)) if own(r)]


def tool_results(rs):
    """Tool result texts by call order of the longest request (a later run may have elided uneventful results)."""
    best = []
    for r in rs:
        msgs = [m for m in r.get("messages", []) if m.get("role") == "tool"]
        if len(msgs) >= len(best): best = msgs
    return ["".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"] for m in best]


if part == "ladder":
    # the second wait shares its turn with another call, so it climbs one rung (omp 502f43cabb)
    run([["bash", {"command": "sleep 24", "async": True}], ["wait", {}], [["wait", {}], ["bash", {"command": "true"}]]])
    res = []
    for r in requests():  # each wait as first sent, before a later pruning pass could elide it
        msgs = [m for m in r.get("messages", []) if m.get("role") == "tool"]
        if msgs and len(msgs) > len(res):
            res.extend("".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"]
                       for m in msgs[len(res):])
    # an elapsed wait names the window it spent and what to do once nothing else is left (omp 545b727cdd)
    check("first wait", len(res) >= 3 and res[1] == "## Still Running (1)\n\n- `bg_1` [bash] — sleep 24\n\n"
          "No job finished within 5.0s. Nothing else left? Call `wait` right away with no other tool call: it then blocks up to 4m.", res)
    want = ("## Still Running (1)\n\n- `bg_1` [bash] — sleep 24\n\n"
            "No job finished within 10.0s. Nothing else left? Call `wait` right away with no other tool call: it then blocks up to 4m.\n"
            "<system-notice>\nNothing new after 15.0s of back-to-back waits.\nWaiting is not progress:")
    check("second wait nudges", len(res) >= 3 and res[2].startswith(want), res)
elif part == "lone":
    # a back-to-back wait alone in its turn takes the top rung at once: it lasts until the job settles
    t = run([["bash", {"command": "sleep 14", "async": True}], ["wait", {}], ["wait", {}]])
    res = tool_results(requests())
    check("lone wait top rung", len(res) >= 3 and "## Completed (1)" in res[2] and t > 12, (t, res))
else:
    # jobs finishing 1 s apart share one result
    run([["bash", {"command": "sleep 0.5; echo a", "async": True}], ["bash", {"command": "sleep 1.5; echo b", "async": True}], ["wait", {}]])
    res = tool_results(requests())
    check("settle window", len(res) >= 3 and "## Completed (2)" in res[2] and "Still Running" not in res[2], res)
    # a subagent cannot broadcast
    run([["task", {"context": "c", "tasks": [{"task": 'call:write:{"path":"agent://all","content":"hi"}'}]}], ["wait", {}]])
    res = tool_results(requests())
    check("subagent broadcast refused", len(res) >= 2 and "Only the main agent can broadcast. Message the one agent that can answer via `agent://<id>`." in res[1], res)
    # a progress note does not end the wait (the job result does) and arrives as an aside after it
    kid = [["bash", {"command": "sleep 0.3"}], ["write", {"path": "agent://Main?progress", "content": "halfway there"}], ["bash", {"command": "sleep 1.5"}]]
    run([["task", {"context": "c", "tasks": [{"task": "seq:" + json.dumps(kid)}]}], ["wait", {}]])
    rs = requests()
    res = tool_results(rs)
    check("progress keeps the wait", len(res) >= 2 and "## Completed (1)" in res[1], res)
    texts = json.dumps([r.get("messages") for r in rs])
    check("progress aside", "(progress; its reply follows later)" in texts and "Active interruptible wait stopped" not in texts, texts[-600:])
    # a direct message ends the wait and is returned by it
    kid = [["bash", {"command": "sleep 0.3"}], ["write", {"path": "agent://Main", "content": "direct hello"}], ["bash", {"command": "sleep 3"}]]
    run([["task", {"context": "c", "tasks": [{"task": "seq:" + json.dumps(kid)}]}], ["wait", {}]])
    res = tool_results(requests())
    check("direct ends the wait", len(res) >= 2 and res[1].startswith("[m") and res[1].endswith(": direct hello") and "Completed" not in res[1], res)
print("wait ok" if not fails else "wait failed")
sys.exit(1 if fails else 0)
