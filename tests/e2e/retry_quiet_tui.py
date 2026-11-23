#!/usr/bin/env python3
"""Quieter auto-retries under a pty (omp 5ad164a69e, event-controller #handleAutoRetryStart): once a retry is
scheduled the failed attempt's error leaves the pinned banner and stays out of the transcript, the retry row names the
reason (`Retrying (1/3) in 1.9s · <reason> (Esc to cancel)`), and a recovered retry leaves only the dim note.
usage: retry_quiet_tui.py <omc> <scratch dir>
Prints "retry quiet ok" or the first failing step."""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

omc, scratch = sys.argv[1:3]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("composer:\n  predictions: false\nretry:\n  baseDelayMs: 2000\n")
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
           OMC_CODING_AGENT_DIR=agent, HOME=scratch)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "TERM_PROGRAM"):
    env.pop(k, None)
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b\][^\x07]*\x07")
out = bytearray()
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))


def pump(t):
    end = time.time() + t
    while True:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if not r:
            return
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        out.extend(data)


def plain(mark=0):
    return ANSI.sub(b"", bytes(out[mark:])).decode("utf-8", "replace")


def wait_out(needle, timeout=15, mark=0):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle in plain(mark):
            return True
    return False


def fail(step, detail=""):
    print(f"retry quiet FAIL {step}: {detail!r}")
    if os.environ.get("RQ_DUMP"):
        open(os.environ["RQ_DUMP"], "wb").write(bytes(out))
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    sys.exit(1)


pump(1.5)
os.write(fd, b"failfor:1:503:try again later|slow:0:recovered fine")
pump(0.3)
os.write(fd, b"\r")
if not wait_out("Retrying (1/", 15):
    fail("retry row", [l for l in plain().split("\r") if "rror" in l or "etry" in l or "failonce" in l][-6:])
mark = len(out)
pump(0.6)
if not re.search(r"Retrying \(1/\d+\) in [0-9.]+s · .*try again later", plain()):
    fail("retry reason", plain()[-800:])
if not wait_out("recovered fine", 20, mark):
    fail("recovered", plain(mark)[-800:])
pre = plain(mark)
pre = pre[:pre.index("recovered fine")]
if "Error:" in pre:
    fail("inline error during the retry", pre[-800:])
if not wait_out("retried", 5, mark):
    fail("retried note", plain(mark)[-800:])
os.kill(pid, signal.SIGKILL)
os.waitpid(pid, 0)
print("retry quiet ok")
