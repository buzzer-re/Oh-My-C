#!/usr/bin/env python3
"""The `.` continue shortcut on a running turn interrupts it and continues (omp AgentSession.resume): type a slow
prompt under a pty, send `.` while it streams, and check that the prompt the model never answered runs again.
usage: continue_tui.py <omc> <req.jsonl>   prints "continue tui ok" or what went wrong"""
import json, os, pty, select, signal, sys, time

omc, reqlog = sys.argv[1:3]
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1")
env.pop("TMUX", None)
start = sum(1 for _ in open(reqlog)) if os.path.exists(reqlog) else 0
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
out = bytearray()


def pump(t):
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            out.extend(data)


def asks():
    if not os.path.exists(reqlog):
        return []
    rs = [json.loads(l) for l in open(reqlog)][start:]
    return [r for r in rs if r.get("tools") and "slow:3:continued" in json.dumps(r.get("messages", [])[-1:])]


try:
    end = time.time() + 15
    while time.time() < end and "╰".encode() not in out:
        pump(0.1)
    pump(0.5)
    os.write(fd, b"slow:3:continued")
    pump(0.3)
    os.write(fd, b"\r")
    end = time.time() + 10
    while time.time() < end and not asks():
        pump(0.1)
    pump(0.5)
    os.write(fd, b".")
    pump(0.2)
    os.write(fd, b"\r")
    end = time.time() + 10
    while time.time() < end and len(asks()) < 2:
        pump(0.2)
    n = len(asks())
    print("continue tui ok" if n >= 2 else "expected the unanswered prompt to run again, got %d request(s)" % n)
finally:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
