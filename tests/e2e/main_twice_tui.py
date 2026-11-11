#!/usr/bin/env python3
"""omc_main twice in one process under a pty (tests/e2e/run.sh): both TUI runs start, run a tool, and show the ask
dialog, which a stale "UI closed" flag from the first run used to deny in the second.
usage: main_twice_tui.py <main_twice binary> <scratch dir>
Prints "main twice ok" or the first failing step."""
import fcntl, json, os, pty, select, signal, struct, sys, termios, time

host, scratch = sys.argv[1:3]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
os.makedirs(work, exist_ok=True)
os.makedirs(agent, exist_ok=True)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", OMC_CODING_AGENT_DIR=agent)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)
pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    # --motion changes a setting at startup: a listener left over from the first run must not fire into dead memory
    os.execve(host, [host, "--model", "mock/mock-1", "--no-session", "--motion", "reduced"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 160, 0, 0))
out = bytearray()


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


def wait_out(needle, timeout=15, since=0):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.05)
        if needle.encode() in out[since:]:
            return True
    return False


def fail(step):
    print(f"main twice FAIL {step}: {bytes(out[-500:])!r}")
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    sys.exit(1)


def command(text):
    os.write(fd, text.encode())
    pump(0.3)
    os.write(fd, b"\r")


ASK = "call:ask:" + json.dumps({"html": '<form><p>Pick one</p><label><input type="radio" name="q1" value="a"> Aa</label>'
                                       '<label><input type="radio" name="q1" value="b"> Bb</label></form>'})

next_mark = 0
for run in (1, 2):
    mark = next_mark
    if not wait_out("Tip: ", 15, mark):  # the welcome banner: a new TUI really started
        fail(f"run {run} start")
    pump(0.8)
    command("bash:echo run" + str(run) + "-tool")
    if not wait_out(f"run{run}-tool", 15, mark):
        fail(f"run {run} tool")
    pump(0.5)
    mark_ask = len(out)
    command(ASK)
    if not wait_out("Pick one", 10, mark_ask):
        fail(f"run {run} ask dialog")
    pump(0.3)
    os.write(fd, b"\r")  # pick Aa (focus moves to Submit)
    pump(0.3)
    os.write(fd, b"\r")  # Submit
    pump(1.0)
    next_mark = len(out)
    command("/exit")

if not wait_out("main twice rc 0 0", 15):
    fail("both runs exit 0")
_, status = os.waitpid(pid, 0)
if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
    fail(f"host status {status}")
print("main twice ok")
