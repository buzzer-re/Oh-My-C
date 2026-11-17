#!/usr/bin/env python3
"""WP-28 commands over RPC: /changelog, /skillful and /force advertised and dispatched as builtins
(command_output), /setup kept TUI-only; /force <tool> <prompt> sends the prompt with the named
tool_choice, and the next request carries "none" (setForcedToolChoice's [forced, "none"] sequence).
Usage: extracmds_rpc.py <omc> <mock request log>"""
import json, subprocess, sys, threading, time

omc, reqlog = sys.argv[1], sys.argv[2]
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


def wait(pred, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for f in frames:
                if pred(f):
                    return f
        time.sleep(0.05)
    return None


def fail(msg):
    print("FAIL", msg)
    p.kill()
    sys.exit(1)


def requests():
    out = []
    try:
        for line in open(reqlog):
            r = json.loads(line)
            out.append(r.get("body", r))
    except OSError:
        pass
    return out


send({"id": "c", "type": "get_available_commands"})
r = wait(lambda f: f.get("type") == "response" and f.get("id") == "c")
if not r or not r.get("success"):
    fail("get_available_commands")
cmds = {c["name"]: c for c in r["data"]["commands"]}
for name in ("changelog", "skillful", "force"):
    if cmds.get(name, {}).get("source") != "builtin":
        fail("missing builtin " + name)
if cmds["changelog"].get("input", {}).get("hint") != "[full|last [N]]":
    fail("changelog hint " + json.dumps(cmds["changelog"]))
if cmds["force"].get("aliases") != ["force:"]:
    fail("force aliases " + json.dumps(cmds["force"]))
for name in ("setup", "providers", "ratchet", "omfg", "debug", "pause", "record"):
    if name in cmds:
        fail("advertised " + name)
print("ok advertised")

checks = [
    ("/changelog last 1", "## [18.22.0]"),
    ("/changelog foo", "Usage: /changelog [full|last [N]]"),
    ("/skillful status", "Skill listing: on (session override; default from the skillful setting)."),
    ("/skillful off", "Skill listing disabled for this session."),
    ("/skillful", "Skill listing enabled for this session."),
    ("/skillful nope", "Usage: /skillful [on|off|status]"),
    ("/force", "Usage: /force:<tool-name> [prompt]"),
    ("/force nope", 'Tool "nope" is not currently active.'),
]
for i, (text, want) in enumerate(checks):
    pid = "p%d" % i
    n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    if not r or not r.get("success"):
        fail("%s: response %s" % (text, json.dumps(r)))
    out = wait(lambda f: f.get("type") == "command_output" and want in f.get("text", "") and frames.index(f) >= n0)
    if not out:
        fail("%s: no command_output containing %r (got %s)" % (
            text, want, [f.get("text") for f in frames[n0:] if f.get("type") == "command_output"]))
    print("ok", text)

# /force <tool> <prompt>: the prompt runs with the forced choice, the next request gets "none"
n0 = len(frames)
before = len(requests())
send({"id": "f", "type": "prompt", "message": "/force:read hello there"})
if not wait(lambda f: f.get("type") == "command_output" and f.get("text") == "Next turn forced to use read."
            and frames.index(f) >= n0):
    fail("/force status")
if not wait(lambda f: f.get("type") == "agent_end" and frames.index(f) >= n0, 20):
    fail("/force prompt did not run")
reqs = requests()[before:]
main = [b for b in reqs if any("hello there" in json.dumps(m) for m in b.get("messages", []))]
if not main or main[0].get("tool_choice") != {"type": "function", "function": {"name": "read"}}:
    fail("forced tool_choice " + json.dumps([b.get("tool_choice") for b in reqs]))
print("ok forced request")
n1 = len(frames)
before = len(requests())
send({"id": "g", "type": "prompt", "message": "again"})
if not wait(lambda f: f.get("type") == "agent_end" and frames.index(f) >= n1, 20):
    fail("second prompt")
reqs = requests()[before:]
if not reqs or reqs[0].get("tool_choice") != "none":
    fail("none tool_choice " + json.dumps([b.get("tool_choice") for b in reqs]))
print("ok none after force")
p.stdin.close()
p.wait(timeout=10)
print("extracmds rpc ok")
