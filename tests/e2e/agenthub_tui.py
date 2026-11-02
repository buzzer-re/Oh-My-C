#!/usr/bin/env python3
"""Drive the subagent visibility UI under a pty (WP-22): status-line badge, ←← composer hint, the "Subagents · N
running" HUD, the Agent Hub (Alt+A / ←← / /hub: roster, inspector, activity, kill, revive), the transcript viewer
(chat with the agent) and session focus (Enter on a roster row, Esc back to main).

usage: agenthub_tui.py <omc>   (mock model + agent dir from the environment, like tests/e2e/run.sh)
Prints one "<step> ok" line per step, or "<step> FAIL: <screen tail>" and exits 1."""
import json, os, pty, re, select, sys, time

omc = sys.argv[1]
ANSI = re.compile(r"\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")
task = "call:task:" + json.dumps({"context": "ctx", "tasks": [{"task": "slow:6:alpha done"}, {"task": "say:beta done"}]})

pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", COLUMNS="120", LINES="40")
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
        env.pop(k, None)
    os.execvpe(omc, [omc, "--model", "mock/mock-1", task], env)

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
    step("hud", [], "Subagents", "1 running", "running agent", timeout=20)
    step("hub", ["\x1ba"], "Agent Hub", "Roster", "Flat / By parent", "1 running", "1 idle", "Lineage", "Spawned by Main")
    step("activity", ["2"], "1 Agents", "2 Activity", "all agents", "Response", "beta done")
    step("filter", ["f"], "errors")
    step("agents", ["f", "f", "f", "1"], "Roster")
    step("kill", ["x"], "1 aborted")
    time.sleep(1.0)  # the killed turn unwinds before it can be revived
    step("revive", ["r"], "2 idle")
    step("tree", ["t"], "t:flat")
    step("close", ["\x1b"], "")
    time.sleep(0.5)
    step("slash hub", ["/hub", "\r"], "2 Activity", "following", gap=0.4)
    step("viewer", ["k", "\r"], "Agent Hub", "of Main", "send", "top/bottom")
    step("chat", ["say:hello there\r"], "hello there", timeout=20)
    step("viewer close", ["\x1b"], "j/k:select")
    step("hub close", ["\x1b"], "")
    time.sleep(0.7)
    step("left left", ["\x1b[D", "\x1b[D"], "Agent Hub", gap=0.15)
    step("focus", ["\r"], "Complete assignment thoroughly")
    step("unfocus", ["\x1b"], "Background job completed", timeout=20)
finally:
    try:
        os.write(fd, b"\x04")
        pump(lambda s: False, 1.5)
        os.kill(pid, 9)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
