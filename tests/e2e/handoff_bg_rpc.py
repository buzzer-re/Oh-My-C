#!/usr/bin/env python3
"""RPC checks for the background /handoff (omp 9ed87e0c21, compact(focus, {mode: "handoff"})): a `handoff` sent
while a tool runs neither aborts the turn nor waits for it; the summary lands at the next turn boundary, the turn
goes on over the compacted context, and an abort cancels a pending handoff.

  handoff_bg_rpc.py <omc> <agent dir> <mock request log>

Prints one line per check, then "handoff bg ok"; exits non-zero on the first failure."""
import json, os, shutil, subprocess, sys, threading, time

omc, agent, reqlog = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(agent, exist_ok=True)
shutil.copy(os.path.join(os.path.dirname(agent), "agent", "models.yml"), os.path.join(agent, "models.yml"))
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("compaction:\n  keepRecentTokens: 1\n  asyncEnabled: false\ncomposer:\n  predictions: false\n")
env = dict(os.environ, OMC_CODING_AGENT_DIR=agent)
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True, env=env)
frames = []
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            frames.append(m)


threading.Thread(target=reader, daemon=True).start()


def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()


def wait(pred, timeout=20, start=0):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for i, f in enumerate(frames[start:]):
                if pred(f):
                    return start + i, f
        time.sleep(0.05)
    return None, None


def fail(msg):
    print("FAIL", msg)
    p.kill()
    sys.exit(1)


def requests():
    try:
        with open(reqlog) as f:
            return [json.loads(l) for l in f if l.strip()]
    except OSError:
        return []


send({"id": "p1", "type": "prompt", "message": "say:first answer"})
if wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "p1")[1] is None:
    fail("first prompt did not finish")
n0 = len(frames)
send({"id": "p2", "type": "prompt", "message": "bash:sleep 2; echo slept"})
if wait(lambda f: f.get("type") == "tool_execution_start", start=n0)[1] is None:
    fail("the tool did not start")
send({"id": "h1", "type": "handoff"})
hi, hr = wait(lambda f: f.get("type") == "response" and f.get("id") == "h1", start=n0)
if not hr or not hr.get("success"):
    fail("handoff response %s" % hr)
ti, _ = wait(lambda f: f.get("type") == "tool_execution_end", start=n0)
if ti is None or ti > hi:
    fail("handoff applied before the tool turn ended")
print("ok handoff during a tool turn")
_, pr = wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "p2", start=n0)
with lock:
    ends = [f for f in frames[n0:] if f.get("type") == "agent_end"]
    starts = [f for f in frames[n0:] if f.get("type") == "manual_compaction_start"]
    mends = [f for f in frames[n0:] if f.get("type") == "manual_compaction_end"]
if not pr or any(f.get("aborted") for f in ends):
    fail("the turn was interrupted: %s" % pr)
if not starts or starts[0].get("mode") != "handoff" or not mends or "result" not in mends[0]:
    fail("manual compaction events %s %s" % (starts, mends))
print("ok turn kept")
last = requests()[-1]
body = json.dumps(last.get("messages"))
if "plain answer" not in body or "slept" not in body:
    fail("the next request missed the summary or the tool result: " + body[:600])
print("ok next request over the summary")
send({"id": "st", "type": "get_state"})
_, st = wait(lambda f: f.get("type") == "response" and f.get("id") == "st")
sf = ((st or {}).get("data") or {}).get("sessionFile")
ents = [json.loads(l) for l in open(sf) if l.strip()] if sf and os.path.exists(sf) else []
comp = [e for e in ents if e.get("type") == "compaction"]
if not comp or comp[-1].get("method") != "handoff":
    fail("no handoff compaction entry")
print("ok handoff compaction entry")

# an abort cancels a pending handoff (abortCompaction)
n1 = len(frames)
send({"id": "p3", "type": "prompt", "message": "say:SLOWSUMMARY notes"})
if wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "p3", start=n1)[1] is None:
    fail("third prompt did not finish")
send({"id": "h2", "type": "handoff"})
time.sleep(0.8)
send({"id": "h3", "type": "handoff"})
_, busy = wait(lambda f: f.get("type") == "response" and f.get("id") == "h3", start=n1)
if not busy or busy.get("success") or busy.get("error") != "Compaction already in progress":
    fail("second handoff %s" % busy)
send({"id": "ab", "type": "abort"})
_, h2 = wait(lambda f: f.get("type") == "response" and f.get("id") == "h2", start=n1)
if not h2 or h2.get("success") or h2.get("error") != "Compaction cancelled":
    fail("cancelled handoff %s" % h2)
print("ok abort cancels the handoff")
p.stdin.close()
p.wait(timeout=10)
print("handoff bg ok")
