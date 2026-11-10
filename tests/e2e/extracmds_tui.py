#!/usr/bin/env python3
"""WP-28b commands under a pty (tests/e2e/run.sh): /pause holds a running tool turn and resumes it, /debug's
selector and system-info view, /record writes a redacted .ompcast that `omc play` replays, and the startup
release notes under the welcome banner.
usage: extracmds_tui.py <omc> <scratch dir>
Prints "extracmds tui ok" or the first failing step."""
import fcntl, json, os, pty, re, select, struct, sys, termios, time

omc, scratch = sys.argv[1:3]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
# the startup release notes: a marker two releases behind the bundled changelog
with open(os.path.join(agent, "last-changelog-version"), "w") as f:
    f.write("18.14.0")
src = os.environ["OMC_CODING_AGENT_DIR"]
for name in ("models.yml", "config.yml"):
    p = os.path.join(src, name)
    if os.path.exists(p):
        with open(p) as a, open(os.path.join(agent, name), "w") as b:
            b.write(a.read())

env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
           OMC_CODING_AGENT_DIR=agent, SECRET_TOKEN="hunter2hunter2")
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)
procs = []


def spawn(argv, rows=50, cols=160):
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(argv[0], argv, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    procs.append(pid)
    return fd


fd = spawn([omc, "--model", "mock/mock-1", "--no-session"])
out = bytearray()


def pump(t=0.05, f=None):
    f = fd if f is None else f
    end = time.time() + t
    while True:
        r, _, _ = select.select([f], [], [], max(0, end - time.time()))
        if not r:
            return
        try:
            data = os.read(f, 65536)
        except OSError:
            return
        if not data:
            return
        out.extend(data)


def send(s, pause=0.2):
    os.write(fd, s.encode())
    pump(pause)


def wait_out(needle, timeout=10, since=0):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle.encode() in out[since:] or needle.encode() in screen_text(out[since:]):
            return True
    return False


def screen_text(b):
    """The output's text with escape sequences dropped and whitespace runs collapsed, so a notice the TUI
    word-wraps (a long recording path pushes "replay: omc play" onto two rows) still matches."""
    b = re.sub(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b[@-_]", b"", bytes(b))
    return re.sub(rb"\s+", b" ", b)


def fail(step, detail=""):
    print(f"extracmds tui FAIL {step}: {detail!r}")
    for p in procs:
        try:
            os.kill(p, 9)
        except OSError:
            pass
    sys.exit(1)


if not wait_out("mock-1", 15):
    fail("startup", out[-300:])
if not wait_out("What's New", 2) or not wait_out("Updated to v18.18.6", 1):
    fail("release notes", out[-800:])
with open(os.path.join(agent, "last-changelog-version")) as f:
    if f.read().strip() != "18.18.6":
        fail("marker advanced")
pump(0.8)

# /pause while a tool runs: the next tool waits until resume
mark = len(out)
send('seq:[["bash",{"command":"sleep 2; echo one"}],["bash",{"command":"printf sec%sout ond-"}]]\r', 1.0)
send("/pause\r", 0.5)
if not wait_out("P A U S E D", 5, mark):
    fail("pause screen", out[-500:])
if not wait_out("Paused — ready to suspend", 8, mark):
    fail("pause settled", out[-500:])
time.sleep(1.5)
if b"second-out" in out[mark:]:
    fail("second tool ran while paused")
send("\r", 0.5)
if not wait_out("agents are running again.", 5, mark):
    fail("resume notice", out[-500:])
if not wait_out("second-out", 10, mark):
    fail("held turn resumed", out[-500:])
pump(1.0)

# /debug: selector, then View: system info (fourth row)
mark = len(out)
send("/debug\r", 0.8)
if not wait_out("Debug Tools", 5, mark) or not wait_out("Clear: artifact cache", 2, mark):
    fail("debug selector", out[-500:])
send("\x1b[B", 0.2)
send("\x1b[B", 0.2)
send("\x1b[B", 0.2)
send("\r", 0.8)
if not wait_out("System Information", 5, mark) or not wait_out("App:     omc ", 2, mark):
    fail("system info", out[-800:])

# /record: start, a prompt carrying an env secret and a key, stop
mark = len(out)
send("/record\r", 0.8)
if not wait_out("/record again to stop", 5, mark):
    fail("record start", out[-500:])
send("say:pw hunter2hunter2 key sk-ant-abcdefghijkl\r", 0.5)
if not wait_out("pw hunter2hunter2", 10, mark):
    fail("prompt", out[-500:])
pump(1.0)
mark = len(out)
send("/record\r", 1.0)
if not wait_out("replay: omc play", 5, mark):
    fail("record stop", out[-500:])
recdir = os.path.join(tmp, "omp-recordings")
files = [f for f in os.listdir(recdir) if f.endswith(".ompcast")] if os.path.isdir(recdir) else []
if len(files) != 1:
    fail("recording file", files)
path = os.path.join(recdir, files[0])
text = open(path).read()
lines = text.splitlines()
head = json.loads(lines[0])
if head.get("ompcast") != 1 or head.get("cols") != 160 or head.get("rows") != 50:
    fail("header", head)
events = [json.loads(l) for l in lines[1:]]
if not events or not any(e[1].get("t") == "viewport" for e in events if e[0] == events[0][0]):
    fail("first frame", events[:1])
if "hunter2hunter2" in text or "sk-ant-abcdefghijkl" in text or "••••••" not in text:
    fail("redaction", text[-400:])
os.kill(procs[0], 9)

# omc play: replays the recording (the prompt's redacted text reaches the screen)
mark = len(out)
fd = spawn([omc, "play", path, "-s", "8"])
end = time.time() + 15
while time.time() < end and b"pw \xe2\x80\xa2" not in out[mark:]:
    pump(0.1, fd)
if b"pw \xe2\x80\xa2" not in out[mark:]:
    fail("omc play", out[-500:])
print("extracmds tui ok")
