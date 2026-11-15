#!/usr/bin/env python3
"""Dev-only e2e: a steer queued while a tool batch runs never skips the already-emitted calls (omp agent-loop.ts runTool,
#10439). Drives `<omc> --mode rpc --no-session` against the mock model from tests/e2e/run.sh:
  immediate: batch [bash "sleep 2", write] + a steer at 0.5 s -> bash backgrounds itself early (the cooperative
             steering signal), the write still runs, then the steer opens the next turn
  wait:      the same with interruptMode "wait" -> bash runs to the end (no cooperative signal), the write runs
Also checks omp's event order: each toolResult message right after its tool_execution_end, and the steer emitted
after the next turn_start. Prints one summary line per scenario.
  steer_batch_rpc.py <omc>"""
import json, os, shutil, subprocess, sys, tempfile, threading, time

B = sys.argv[1]


def scenario(mode):
    d = tempfile.mkdtemp(prefix="omc-steerbatch-")
    path = os.path.join(d, "w.txt")
    secs = 2 if mode == "immediate" else 1
    batch = [["bash", {"command": f"sleep {secs}; echo slow-done"}], ["write", {"path": path, "content": "W\n"}]]
    p = subprocess.Popen([B, "--mode", "rpc", "--no-session"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    frames, done = [], threading.Event()

    def reader():
        for line in p.stdout:
            try:
                m = json.loads(line)
            except ValueError:
                continue
            frames.append(m)
            if m.get("type") == "agent_end":
                done.set()
        done.set()

    threading.Thread(target=reader, daemon=True).start()

    def send(o):
        p.stdin.write(json.dumps(o) + "\n")
        p.stdin.flush()

    if mode == "wait":
        send({"id": "m", "type": "set_interrupt_mode", "mode": "wait"})
    send({"id": "1", "type": "prompt", "message": "batch:" + json.dumps(batch)})
    time.sleep(0.5)
    send({"id": "s1", "type": "steer", "message": "say:handled"})
    done.wait(30)
    try:
        p.stdin.close()
    except BrokenPipeError:
        pass
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()

    ends = {f.get("toolName"): f for f in frames if f.get("type") == "tool_execution_end"}
    text = lambda f: "".join(c.get("text", "") for c in ((f or {}).get("result") or {}).get("content") or [])
    bash, write = text(ends.get("bash")), text(ends.get("write"))
    skipped = sum("Skipped due to" in text(f) for f in ends.values())
    bash_state = "backgrounded" if "Backgrounded early to handle an incoming message" in bash else \
        "finished" if "slow-done" in bash else "?" + bash[:80]
    write_state = "ok" if "Successfully wrote" in write else "?" + write[:80]
    # each toolResult message follows its tool_execution_end; the steer follows the next turn_start
    order = "ok"
    kinds = [(f.get("type"), (f.get("message") or {}).get("role")) for f in frames]
    for i, f in enumerate(frames):
        if f.get("type") == "tool_execution_end" and kinds[i + 1:i + 2] != [("message_start", "toolResult")]:
            order = "result-late"
        m = f.get("message") or {}
        if f.get("type") == "message_start" and m.get("steering") and (i == 0 or kinds[i - 1][0] != "turn_start"):
            order = "steer-before-turn"
    exists = os.path.exists(path)
    shutil.rmtree(d, ignore_errors=True)
    print(f"{mode}: bash={bash_state} write={write_state} file={'yes' if exists else 'no'} skipped={skipped} order={order}")


scenario("immediate")
scenario("wait")
