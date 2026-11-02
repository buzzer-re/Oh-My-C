#!/usr/bin/env python3
"""RPC checks for WP-19 model switching: /switch (session-only model, never persisted), /modelpreset
(save/list/switch/delete in config.yml) and a prewalk handoff (plan nudge, todo gate, switch at the first
write, `prewalk` model_change, checklist steer, the next request on the target model).

  model_switch_rpc.py <omc> <agent dir> <mock request log>

Prints one line per check; exits non-zero on the first failure."""
import json, os, subprocess, sys, threading, time

omc, agent, reqlog = sys.argv[1], sys.argv[2], sys.argv[3]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True)
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


def wait(pred, timeout=15, start=0):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for f in frames[start:]:
                if pred(f):
                    return f
        time.sleep(0.05)
    return None


def fail(msg):
    print("FAIL", msg)
    p.kill()
    sys.exit(1)


def command(text, want, n=[0]):
    n[0] += 1
    pid = "c%d" % n[0]
    n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    if not wait(lambda f: f.get("type") == "response" and f.get("id") == pid, start=n0):
        fail("%s: no response" % text)
    out = wait(lambda f: f.get("type") == "command_output" and want in f.get("text", ""), start=n0)
    if not out:
        with lock:
            got = [f.get("text") for f in frames[n0:] if f.get("type") == "command_output"]
        fail("%s: no command_output containing %r (got %s)" % (text, want, got))
    print("ok", text)


def config():
    try:
        return open(os.path.join(agent, "config.yml")).read()
    except OSError:
        return ""


# /switch: session-only
command("/switch", "Current model: mock/mock-1")
command("/switch zzqq", "Unknown model: zzqq")
before = config()
command("/switch mock/mock-2", "Session-only model: mock/mock-2.")
if config() != before:
    fail("/switch persisted settings")
send({"id": "st", "type": "get_state"})
st = wait(lambda f: f.get("type") == "response" and f.get("id") == "st")
if ((st or {}).get("data") or {}).get("model", {}).get("id") != "mock-2":
    fail("/switch did not change the session model: %s" % json.dumps(st))
print("ok /switch state")
command("/switch mock/mock-1", "Session-only model: mock/mock-1.")

# /modelpreset
command("/modelpreset", "No model presets saved. Use /modelpreset save <name> to create one.")
command("/modelpreset save 9bad", 'Invalid preset name "9bad": use a letter, then letters, digits, - or _')
command("/modelpreset save fast", 'Saved model preset "fast"')
if "modelPresets:\n  fast:\n    modelRoles:" not in config():
    fail("preset not written: " + config())
command("/modelpreset list", "Model presets: fast")
command("/modelpreset switch fast", 'Switched to preset "fast" (mock/mock-1')
command("/modelpreset switch nope", "Preset not found: nope")
command("/modelpreset delete fast", 'Deleted model preset "fast"')
if "modelPresets" in config():
    fail("empty modelPresets kept: " + config())
command("/modelpreset bogus", "Usage: /modelpreset [list | save <name> | switch <name> | delete <name>]")

# prewalk
command("/prewalk a b", "Usage: /prewalk [into] | restart [from] [into] | off")
command("/prewalk off", "Prewalk already off for this session; keeping the active model.")
command("/prewalk zzqq", 'Model "zzqq" not found')
command("/prewalk mock/mock-2", "Prewalk on: switching to mock/mock-2 at the next edit/write (todo-gated).")
open(reqlog, "a").close()
mark = os.path.getsize(reqlog)
n0 = len(frames)
send({"id": "run", "type": "prompt", "message":
      'seq:[["todo",{"op":"init","items":["a","b"]}],["write",{"path":"pw.txt","content":"hi"}]]'})
if not wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "run", 30, n0):
    fail("prewalk run did not finish")
with open(reqlog) as f:
    f.seek(mark)
    reqs = [json.loads(l) for l in f if l.strip()]
models = [r.get("model") for r in reqs if r.get("messages")]
if not models or models[0] != "mock-1" or models[-1] != "mock-2":
    fail("prewalk handoff: request models %s" % models)
body = json.dumps(reqs[0])
if "STOP: In NEXT reply, before further exploration, write complete plan." not in body:
    fail("plan nudge not sent with the first request")
if "Before task complete, verify:" not in json.dumps(reqs[-1]):
    fail("checklist not steered after the handoff")
print("ok prewalk handoff", models)
send({"id": "st2", "type": "get_state"})
st = wait(lambda f: f.get("type") == "response" and f.get("id") == "st2")
if ((st or {}).get("data") or {}).get("model", {}).get("id") != "mock-2":
    fail("prewalk did not switch the session model")
sf = ((st or {}).get("data") or {}).get("sessionFile")
if sf and os.path.exists(sf):
    ents = [json.loads(l) for l in open(sf) if l.strip()]
    if not any(e.get("type") == "model_change" and e.get("role") == "prewalk" for e in ents):
        fail("no prewalk model_change entry")
    if any(e.get("customType") == "prewalk-plan" for e in ents):
        fail("plan nudge persisted")
    print("ok prewalk session entries")
command("/prewalk off", "Prewalk already off for this session; keeping the active model.")
p.stdin.close()
p.wait(timeout=10)
