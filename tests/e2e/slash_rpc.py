#!/usr/bin/env python3
"""RPC slash-command checks (WP-4): the shared registry behind get_available_commands and builtin
dispatch of `prompt` text (command_output frames, agentInvoked:false). Prints one line per check;
exits non-zero on the first failure."""
import json, subprocess, sys, threading, time

omc = sys.argv[1]
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


send({"id": "c", "type": "get_available_commands"})
r = wait(lambda f: f.get("type") == "response" and f.get("id") == "c")
if not r or not r.get("success"):
    fail("get_available_commands")
cmds = {c["name"]: c for c in r["data"]["commands"]}
for name in ("model", "effort", "advisor", "compact", "shake", "handoff", "retry", "rename", "dirs", "memory"):
    if name not in cmds:
        fail("missing builtin " + name)
for name in ("settings", "quit", "help", "thinking"):
    if name in cmds:
        fail("advertised TUI-only/unknown command " + name)
if any(c.get("source") not in ("builtin", "skill", "file", "mcp_prompt") for c in cmds.values()):
    fail("unknown command source advertised")
if cmds.get("init", {}).get("source") != "file":  # the bundled /init file command
    fail("bundled /init not advertised")
if cmds["model"].get("aliases") != ["models"]:
    fail("model aliases")
if cmds["compact"]["input"]["hint"] != "[soft|handoff] [focus]":
    fail("compact hint " + json.dumps(cmds["compact"].get("input")))
if [s["name"] for s in cmds["shake"].get("subcommands", [])] != ["elide", "images", "thinking"]:
    fail("shake subcommands")
print("ok available commands")

checks = [
    ("/dirs", "Workspace directories:\n  "),
    ("/model", "Current model: mock/mock-1"),
    # mock-1 has no `reasoning`: a provisional ladder (omp 3f8862bf9e), adjustable with auto
    ("/effort bogus", "Unknown thinking level: bogus. Available: off, auto, minimal, low, medium, high, xhigh, max"),
    ("/advisor status", "Advisor is disabled."),
    ("/advisor bogus", "Usage: /advisor [on|off|status|dump [raw]|configure]"),
    ("/shake bogus", 'Unknown /shake mode "bogus". Use elide, images, or thinking.'),
    ("/shake", "Nothing to shake."),
    ("/compact snapcompact now", "/compact snapcompact does not take focus instructions"),
    ("/retry", "Nothing to retry."),
    ("/rename Hello there", "Session renamed to Hello there."),
]
for i, (text, want) in enumerate(checks):
    pid = "p%d" % i
    n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    if not r or not r.get("success") or (r.get("data") or {}).get("agentInvoked") is not False:
        fail("%s: response %s" % (text, json.dumps(r)))
    out = wait(lambda f: f.get("type") == "command_output" and want in f.get("text", "") and frames.index(f) >= n0)
    if not out:
        fail("%s: no command_output containing %r (got %s)" % (
            text, want, [f.get("text") for f in frames[n0:] if f.get("type") == "command_output"]))
    print("ok", text)

# an unknown slash word still reaches the model
send({"id": "u", "type": "prompt", "message": "/notacommand hi"})
if not wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "u", 15):
    fail("/notacommand did not run as a prompt")
print("ok unknown slash goes to the model")
p.stdin.close()
p.wait(timeout=10)
