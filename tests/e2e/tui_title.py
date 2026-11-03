#!/usr/bin/env python3
"""Automatic session title in the interactive TUI (omp autoTitle = isInteractive): type one prompt under a pty and
wait for the fork title side request; print the title the session got ("title: <name>") or what went wrong.
usage: tui_title.py <omc> <req.jsonl> <prompt>"""
import json, os, pty, select, signal, sys, time

omc, reqlog, prompt = sys.argv[1:4]
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1")
env.pop("OMC_NO_TITLE", None)
env.pop("TMUX", None)
start = sum(1 for _ in open(reqlog)) if os.path.exists(reqlog) else 0
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "--model", "mock/mock-1"], env)
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


def title_request():
    if not os.path.exists(reqlog):
        return None
    rs = [json.loads(l) for l in open(reqlog)][start:]
    return next((r for r in rs if "<title-request>" in json.dumps(r.get("messages", [])[-1:])), None)


try:
    end = time.time() + 15
    while time.time() < end and "╰".encode() not in out:
        pump(0.1)
    pump(0.5)
    os.write(fd, prompt.encode())
    pump(0.3)
    os.write(fd, b"\r")
    end = time.time() + 15
    while time.time() < end and title_request() is None:
        pump(0.2)
    pump(1.0)
    r = title_request()
    print("title request sent" if r else "no title request")
finally:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
