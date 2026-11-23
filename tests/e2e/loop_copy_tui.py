#!/usr/bin/env python3
"""Loop mode (WP-23) and the /copy picker, /copy targets and /open (WP-20) under a pty, checked through the
requests the mock model receives, the OSC 52 clipboard writes and a fake `xdg-open` (tests/e2e/run.sh).
usage: loop_copy_tui.py <omc> <req.jsonl> <scratch dir> loop|copy
Prints "loop copy ok" or the first failing step."""
import base64, fcntl, json, os, pty, re, select, struct, sys, termios, time
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off

omc, reqlog, scratch, part = sys.argv[1:5]
bindir = os.path.join(scratch, "bin")
work = os.path.join(scratch, "work")
openlog = os.path.join(scratch, "open.log")
os.makedirs(bindir, exist_ok=True)
os.makedirs(work, exist_ok=True)
with open(os.path.join(bindir, "xdg-open"), "w") as f:
    f.write("#!/bin/sh\nprintf '%s\\n' \"$1\" >> \"$FAKE_OPEN_LOG\"\n")
os.chmod(os.path.join(bindir, "xdg-open"), 0o755)
os.chdir(work)

env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", FAKE_OPEN_LOG=openlog,
           PATH=bindir + ":" + os.environ["PATH"])
for k in ("OMC_NO_BROWSER", "TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)
lc_pid, fd = pty.fork()
if lc_pid == 0:
    os.execve(omc, [omc, "--config", NOPRED, "--model", "mock/mock-1", "--no-session"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))  # wide: notices stay on one row
out = bytearray()


def pump(t=0.05):
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


def send(s, pause=0.15):
    os.write(fd, s.encode() if isinstance(s, str) else s)
    pump(pause)


def wait_out(needle, timeout=10, since=None):
    start = len(out) if since is None else since
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle.encode() in out[start:]:
            return True
    return False


def requests():
    try:
        return [json.loads(l) for l in open(reqlog)]
    except FileNotFoundError:
        return []


def last_user(r):
    users = [m for m in r.get("messages", []) if m.get("role") == "user"]
    if not users:
        return ""
    c = users[-1].get("content")
    return c if isinstance(c, str) else "".join(b.get("text", "") for b in c or [] if isinstance(b, dict))


def prompts_since(n, text):
    return sum(1 for r in requests()[n:] if r.get("tools") and last_user(r).rstrip().endswith(text))


def clipboard_since(start):
    found = re.findall(rb"\x1b\]52;c;([A-Za-z0-9+/=]*)\x07", bytes(out[start:]))
    return base64.b64decode(found[-1]).decode() if found else None


def fail(step, detail=""):
    print(f"loop copy FAIL {step}: {detail!r}")
    if os.environ.get("LOOPCOPY_DEBUG"):
        sys.stderr.write(out[-3000:].decode("utf-8", "replace"))
    try:
        os.kill(lc_pid, 9)
    except OSError:
        pass
    sys.exit(1)


if not wait_out("mock-1", 15, 0):
    fail("startup", out[-300:])
pump(1.0)

def loop_part():
    # /loop 2 <prompt>: the inline prompt runs, then re-submits twice (800 ms apart) and the budget ends the loop
    n = len(requests())
    mark = len(out)
    send("/loop 2 say:loopy\r")
    if not wait_out("Limited to 2 iterations. 2 of 2 iterations remaining. Repeating it after each turn.", 5, mark):
        fail("loop enabled notice", out[-500:])
    if not wait_out("Loop limit reached. Loop mode disabled.", 20, mark):
        fail("loop limit", out[-500:])
    if prompts_since(n, "say:loopy") != 3:
        fail("loop iterations", [last_user(r)[:80] for r in requests()[n:] if r.get("tools")])

    # --until '<cmd>': re-evaluated before every re-submit; exit 0 ends the loop
    n = len(requests())
    mark = len(out)
    send("/loop --until 'test -f stop.flag' say:gated\r")
    if not wait_out("Continuing until `test -f stop.flag` succeeds.", 5, mark):
        fail("until notice", out[-500:])
    end = time.time() + 15
    while time.time() < end and prompts_since(n, "say:gated") < 2:
        pump(0.1)
    if prompts_since(n, "say:gated") < 2:
        fail("until iterations", prompts_since(n, "say:gated"))
    open("stop.flag", "w").close()
    if not wait_out("Loop condition `test -f stop.flag` is now satisfied. Loop mode disabled.", 15, mark):
        fail("until satisfied", out[-500:])

    # a broken condition (exit > 1) stops the loop with its output
    mark = len(out)
    send("/loop --while 'echo broken-cond; exit 3' say:w\r")
    if not wait_out("Loop condition `echo broken-cond; exit 3` failed (exit 3): broken-cond. Loop mode disabled.", 15, mark):
        fail("broken condition", out[-500:])

    # Esc suspends the loop mid-turn (the turn aborts, loop mode stays on, paused); /loop again disables it
    mark = len(out)
    send("/loop\r", 0.5)
    send("slow:3:zz\r", 1.0)
    send("\x1b", 1.0)
    if not wait_out("Loop paused", 5, mark):
        fail("esc pause", out[-500:])
    n = len(requests())
    pump(2.0)
    if prompts_since(n, "slow:3:zz"):
        fail("paused resubmit", prompts_since(n, "slow:3:zz"))
    mark = len(out)
    send("/loop\r")
    if not wait_out("Loop mode disabled.", 5, mark):
        fail("loop disable", out[-500:])
    send("/loop 0\r")
    if not wait_out("Loop count must be a positive integer.", 5, mark):
        fail("loop usage error", out[-500:])

def copy_part():
    mark = len(out)
    # /copy code|cmd|link copy the latest target through OSC 52
    mark = len(out)
    send("md:copy\r")
    if not wait_out("plain-link", 10, mark):
        fail("copy fixture", out[-500:])
    pump(0.8)
    mark = len(out)
    send("/copy code\r", 0.8)
    if clipboard_since(mark) != "make -B -j3\n./build/omc --version" or not wait_out("Copied code block to clipboard", 3, mark):
        fail("copy code", clipboard_since(mark))
    mark = len(out)
    send("/copy link\r", 0.8)
    if clipboard_since(mark) != "https://example.org/plain-link":
        fail("copy link", clipboard_since(mark))
    mark = len(out)
    send("/copy cmd\r", 0.8)
    if not wait_out("No command to copy.", 3, mark):
        fail("copy cmd none", out[-300:])
    # /copy local creates and copies the session's local:// directory (omp 7cc6ba48cc)
    mark = len(out)
    send("/copy local\r", 0.8)
    got = clipboard_since(mark)
    if not got or not os.path.isdir(got) or not wait_out("to clipboard", 3, mark):
        fail("copy local", got)

    # the picker: the last turn is outlined; Enter copies it, Right descends into its blocks, `o` opens a link
    mark = len(out)
    send("/copy\r", 0.8)
    if not wait_out("pick what to put on the clipboard", 3, mark) or not wait_out("4 blocks", 3, mark):
        fail("picker", out[-500:])
    send("\r", 0.8)
    got = clipboard_since(mark)
    if not got or not got.startswith("Run the build") or not wait_out("Copied assistant message to clipboard", 3, mark):
        fail("picker whole turn", got)
    mark = len(out)
    send("/copy\r", 0.8)
    send("\x1b[C", 0.4)
    if not wait_out("1/4 · sh code · 2 lines", 3, mark):
        fail("picker blocks", out[-500:])
    send("\x1b[B", 0.2)
    send("\x1b[B", 0.3)
    send("\r", 0.8)
    if clipboard_since(mark) != "https://example.com/omc/docs" or not wait_out("Copied link · omc docs to clipboard", 3, mark):
        fail("picker link block", clipboard_since(mark))
    mark = len(out)
    send("/copy\r", 0.8)
    send("\x1b[C", 0.3)
    send("\x1b[B", 0.2)
    send("\x1b[B", 0.2)
    send("\x1b[B", 0.2)
    send("o", 1.0)
    if not wait_out("Opening link: https://example.org/plain-link", 3, mark):
        fail("picker open", out[-500:])

    # /open: the latest link of the conversation
    mark = len(out)
    send("/open\r", 1.0)
    if not wait_out("Opening https://example.org/plain-link", 3, mark):
        fail("open", out[-500:])
    pump(0.5)
    opened = open(openlog).read().split() if os.path.exists(openlog) else []
    if opened != ["https://example.org/plain-link", "https://example.org/plain-link"]:
        fail("opener calls", opened)

if part == "loop":
    loop_part()
else:
    copy_part()

send("\x04", 0.5)
try:
    os.waitpid(lc_pid, 0)
except ChildProcessError:
    pass
print(part + " ok")
