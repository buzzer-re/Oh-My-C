#!/usr/bin/env python3
"""Drive the interactive approval dialog under a pty (WP-10).

usage: approval_tui.py <omc> <prompt> <key> <expect> [extra omc args...]
Starts `omc --model mock/mock-1 --no-session <extra args> <prompt>`, waits for the "Allow tool:" dialog, sends <key>
(escapes decoded, e.g. '\\x11' for Ctrl+Q, '\\x1b' for Esc) once the dialog armed, then waits for
<expect> in the screen text. Prints "approval ok" plus the run-status kind it saw, or what it got instead."""
import os
import pty
import re
import select
import sys
import time

omc, prompt, key, expect = sys.argv[1], sys.argv[2], sys.argv[3].encode().decode("unicode_escape"), sys.argv[4]
extra = sys.argv[5:]
ANSI = re.compile(r"\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")

pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1")
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
        env.pop(k, None)
    os.execvpe(omc, [omc, "--model", "mock/mock-1", "--no-session", *extra, prompt], env)

raw = b""


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
        if until(ANSI.sub("", raw.decode("utf-8", "replace"))):
            return True
    return False


try:
    if not pump(lambda s: "Allow tool:" in s, 20):
        print("no dialog:", ANSI.sub("", raw.decode("utf-8", "replace"))[-600:])
        sys.exit(1)
    time.sleep(0.8)  # Enter and the approve chord arm 500 ms after mount
    os.write(fd, key.encode())
    ok = pump(lambda s: expect in s.split("Allow tool:")[-1], 20)
    text = ANSI.sub("", raw.decode("utf-8", "replace"))
    kinds = sorted(set(re.findall(r"kind=(\w+)", raw.decode("utf-8", "replace"))))
    print(("approval ok" if ok else "approval FAIL: " + text[-800:]) + " kinds=" + ",".join(kinds))
finally:
    try:
        os.write(fd, b"\x04")
        pump(lambda s: False, 1.5)
    except OSError:
        pass
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    os.waitpid(pid, 0)
