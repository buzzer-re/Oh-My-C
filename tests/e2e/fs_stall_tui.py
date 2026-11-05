#!/usr/bin/env python3
"""A filesystem read that does not return must not freeze the TUI (tests/e2e/run.sh): the branch shows at startup, then
.git/HEAD becomes a FIFO, which blocks any reader the way a stalled shared filesystem does, and typed keys must still
echo. Then a real branch change reaches the status line once HEAD is a file again.
usage: fs_stall_tui.py <omc> <scratch dir>
Prints "fs stall ok" or the first failing step."""
import fcntl, os, pty, select, signal, struct, subprocess, sys, termios, time

omc, scratch = sys.argv[1:3]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
os.makedirs(work, exist_ok=True)
os.makedirs(agent, exist_ok=True)
subprocess.run(["git", "init", "-q", "-b", "stallmain", work], check=True)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", OMC_CODING_AGENT_DIR=agent)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)
pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 160, 0, 0))
out = bytearray()


def pump(t):
    end = time.time() + t
    while True:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if not r:
            return
        out.extend(os.read(fd, 65536))


def wait_out(needle, timeout, since=0):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.05)
        if needle in out[since:]:
            return True
    return False


def fail(step):
    print(f"fs stall FAIL {step}: {bytes(out[-400:])!r}")
    os.kill(pid, signal.SIGKILL)
    sys.exit(1)


if not wait_out(b"stallmain", 10):
    fail("branch at startup")
head = os.path.join(work, ".git", "HEAD")
os.unlink(head)
os.mkfifo(head)
pump(1.5)  # past the refresh interval: the next frame starts a read that blocks
for key in b"QZX":
    mark = len(out)
    os.write(fd, bytes([key]))
    if not wait_out(bytes([key]), 3, mark):
        fail(f"key {chr(key)} echo while the read is stalled")
# release the blocked reader and move to a real branch
tmp = head + ".new"
with open(tmp, "w") as f:
    f.write("ref: refs/heads/stallnext\n")
writer = os.open(head, os.O_RDWR | os.O_NONBLOCK)
os.write(writer, b"ref: refs/heads/stallmain\n")
os.close(writer)
pump(0.5)
os.replace(tmp, head)
mark = len(out)
os.write(fd, b"W")
pump(1.3)
os.write(fd, b"V")
if not wait_out(b"stallnext", 8, mark):
    fail("branch change")
os.kill(pid, signal.SIGKILL)
print("fs stall ok")
