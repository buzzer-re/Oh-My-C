#!/usr/bin/env python3
"""Drive WP-22b TUI behaviour under a pty: the working row while the run only waits on background jobs ("Waiting
for background job …"), Esc ending that wait ("Stopped waiting · …"), and the /agents hub (list, detail block,
agent strip, model strip, enable/disable written to config.yml).

usage: agents_tui.py <omc> <agent dir>   (mock model + agent dir from the environment, like tests/e2e/run.sh)
Prints one "<step> ok" line per step, or "<step> FAIL: <screen tail>" and exits 1."""
import json, os, pty, re, select, sys, time

omc, agent_dir = sys.argv[1], sys.argv[2]
ANSI = re.compile(r"\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")
task = lambda t: "call:task:" + json.dumps({"context": "ctx", "tasks": [{"task": t}]})

pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", COLUMNS="120", LINES="40")
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
        env.pop(k, None)
    os.execvpe(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)

import fcntl, struct, termios
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
raw = b""
mark = 0


def text_since(m):
    return ANSI.sub("", raw[m:].decode("utf-8", "replace"))


def pump(until, timeout):
    global raw
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return False
            if not chunk:
                return False
            raw += chunk
        if until(text_since(mark)):
            return True
    return False


def step(name, keys, *expect, timeout=15, gap=0.0):
    global mark
    mark = len(raw)
    for k in keys:
        os.write(fd, k.encode())
        if gap: time.sleep(gap)
    ok = pump(lambda s: all(e in s for e in expect), timeout)
    if not ok:
        print(name, "FAIL:", text_since(mark)[-1500:])
        sys.exit(1)
    print(name, "ok")


try:
    pump(lambda s: "Mock One" in s, 10)
    step("waiting", [task("slow:3:child done"), "\r"], "Waiting for background job", gap=0.05)
    step("delivered", [], "Background job completed", timeout=20)
    time.sleep(1.0)
    step("waiting again", [task("slow:4:second"), "\r"], "Waiting for background job", gap=0.05)
    step("stop waiting", ["\x1b"], "Stopped waiting", "1 background job still running (/jobs)")
    step("folded", [], "Background job completed", timeout=20)
    time.sleep(0.5)
    step("agents", ["/agents", "\r"], "All agents", "Bundled", "reviewer", "embedded:reviewer.md", "configure", gap=0.3)
    step("strip", ["\r"], "reviewer", "disable", "model: auto")
    step("model strip", ["\r"], "pick model", "pattern")
    step("back", ["\x1b", "\x1b"], "enable/disable", gap=0.3)
    step("toggle", [" "], "reviewer disabled")
    time.sleep(0.5)
    cfg = open(os.path.join(agent_dir, "config.yml")).read()
    print("config", "disabledAgents" in cfg and "reviewer" in cfg)
    step("enable", [" "], "reviewer enabled")
    step("close", ["\x1b"], "")
finally:
    try:
        os.kill(pid, 9)
    except OSError:
        pass
