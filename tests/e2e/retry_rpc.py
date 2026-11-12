#!/usr/bin/env python3
"""Manual retry over RPC (omp TurnRecovery.retry, agent-loop endAbortedBeforeModel / unpairedToolCallTail):
an `abort` while a bash call runs ends the run with the aborted boundary turn (empty, "Interrupted by user",
errorId Flag.UserInterrupt); `/retry` then re-executes the failed batch (agent_start names it in
resumedToolCallIds, no model request until its fresh result is in), and a `/retry` after a settled turn reports
"Nothing to retry.".
usage: retry_rpc.py <omc> <request log>
Prints "retry rpc ok" or the first failing step."""
import json, subprocess, sys, threading, time

omc, log = sys.argv[1:3]
p = subprocess.Popen([omc, "--mode", "rpc", "--model", "mock/mock-1", "--no-session"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
events = []
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            events.append(m)


threading.Thread(target=reader, daemon=True).start()


def fail(step, detail=""):
    print(f"retry rpc FAIL {step}: {detail!r}")
    p.kill()
    sys.exit(1)


def send(i, **cmd):
    p.stdin.write(json.dumps({"id": str(i), **cmd}) + "\n")
    p.stdin.flush()


def wait(pred, since, t=10):
    end = time.time() + t
    while time.time() < end:
        with lock:
            for m in events[since:]:
                if pred(m):
                    return m
        time.sleep(0.05)
    return None


def mark():
    with lock:
        return len(events)


def requests():
    try:
        with open(log) as f:
            return sum(1 for line in f if '"tools"' in line)
    except OSError:
        return 0


def ends(m):
    return m.get("type") == "agent_end"


s = mark()
send(1, type="prompt", message="bash:sleep 20")
if not wait(lambda m: m.get("type") == "tool_execution_start", s):
    fail("tool start")
time.sleep(0.3)
send(2, type="abort")
if not wait(ends, s):
    fail("abort end")
with lock:
    boundary = [m["message"] for m in events[s:] if m.get("type") == "message_end" and m["message"].get("role") == "assistant"]
b = boundary[-1] if boundary else {}
if b.get("stopReason") != "aborted" or b.get("content") != [] or b.get("errorMessage") != "Interrupted by user" or \
        b.get("errorId") != 67112960:
    fail("boundary", b)

n = requests()
s = mark()
send(3, type="prompt", message="/retry")
start = wait(lambda m: m.get("type") == "agent_start", s)
if not start or start.get("resumedToolCallIds") != ["call_1"]:
    fail("resumed ids", start)
if not wait(lambda m: m.get("type") == "tool_execution_start" and m.get("toolCallId") == "call_1", s):
    fail("replayed call")
time.sleep(0.5)
if requests() != n:
    fail("replay sent a model request", requests() - n)
if not wait(lambda m: m.get("type") == "command_output" and m.get("text") == "Retrying the last failed turn.", s):
    fail("retry output")
send(4, type="abort")
if not wait(ends, s):
    fail("second abort end")

s = mark()
send(5, type="prompt", message="say:done")
if not wait(ends, s):
    fail("settled turn")
s = mark()
send(6, type="prompt", message="/retry")
if not wait(lambda m: "Nothing to retry." in json.dumps(m), s, 5):
    fail("nothing to retry", events[s:])
p.stdin.close()
p.kill()
print("retry rpc ok")
