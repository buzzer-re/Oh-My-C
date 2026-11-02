#!/usr/bin/env python3
"""Editor I/O (WP-6) under a pty: Ctrl+G external editor, Ctrl+R history search, large-paste chip and menu,
raw paste, `#` prompt actions / `#N` GitHub refs and fuzzy `@` mentions, checked through the requests the
mock model receives (tests/e2e/run.sh).
usage: editor_io.py <omc> <req.jsonl> <fake_editor.sh> <scratch dir>
Prints "editor io ok" or the first failing step."""
import json, os, pty, select, sys, time

omc, reqlog, fake_editor, scratch = sys.argv[1:5]
edlog = os.path.join(scratch, "editor.log")
bindir = os.path.join(scratch, "bin")
os.makedirs(bindir, exist_ok=True)
clip = "\n".join(f"clip row {i}" for i in range(1, 13))
with open(os.path.join(bindir, "xclip"), "w") as f:  # clipboard text only (no image targets)
    f.write("#!/bin/sh\ncase \"$*\" in *TARGETS*) printf 'UTF8_STRING\\nTEXT\\n';; *-o*) printf '%s' \"$OMC_FAKE_CLIP\";; esac\n")
os.chmod(os.path.join(bindir, "xclip"), 0o755)
with open("zeta_module.txt", "w") as f:
    f.write("z\n")

env = dict(os.environ, TERM="xterm-256color", EDITOR=fake_editor, FAKE_EDITOR_LOG=edlog, OMC_SKIP_SETUP="1",
           PI_NO_DESKTOP_NOTIFY="1", DISPLAY=":99", OMC_FAKE_CLIP=clip, PATH=bindir + ":" + os.environ["PATH"])
for k in ("VISUAL", "WAYLAND_DISPLAY", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "TMUX"):
    env.pop(k, None)
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "-m", "mock/mock-1", "--no-session"], env)
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


def wait_out(needle, timeout=10):
    start = len(out)
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle.encode() in out[start - 4096 if start > 4096 else 0:]:
            return True
    return False


def requests():
    try:
        return [json.loads(l) for l in open(reqlog)]
    except FileNotFoundError:
        return []


def text(m):
    c = m.get("content")
    return c if isinstance(c, str) else "".join(b.get("text", "") for b in c or [] if isinstance(b, dict))


def wait_request(n_before, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        rs = [r for r in requests()[n_before:] if r.get("tools")]
        if rs:
            pump(0.6)  # let the turn finish
            users = [m for m in rs[-1]["messages"] if m.get("role") == "user"]
            return text(users[-1]) if users else ""
    return None


def fail(step, detail=""):
    print(f"editor io FAIL {step}: {detail!r}")
    if os.environ.get("EIO_DEBUG"):
        sys.stderr.write(out[-3000:].decode("utf-8", "replace"))
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    sys.exit(1)


def submit(step, expect):
    n = len(requests())
    send("\r")
    got = wait_request(n)
    if got is None or expect not in got:
        fail(step, got)
    return got


if not wait_out("mock-1", 15):
    fail("startup", out[-300:])
pump(1.0)

# Ctrl+G: the draft goes to $EDITOR as a .omp.md file, the edited text comes back (trailing newline trimmed)
send("draft one")
send("\x07", 1.5)
log = open(edlog).read() if os.path.exists(edlog) else ""
if "ext=.omp.md\ndraft one\n--" not in log:
    fail("ctrl+g editor input", log)
submit("ctrl+g result", "draft one\nedited by fake editor")

# Ctrl+R: search the persisted history, Enter puts the prompt back in the composer
send("\x12", 0.6)
if not wait_out("History", 3):
    fail("ctrl+r overlay", out[-400:])
send("edited fake", 0.6)
send("\r", 0.4)
got = submit("ctrl+r select", "draft one\nedited by fake editor")
if got != "draft one\nedited by fake editor":
    fail("ctrl+r exact", got)

# a marker-sized paste below paste.largeMenuThreshold becomes a 📄 chip; it expands on submit
block = "\n".join(f"pasted line {i}" for i in range(1, 16))
send("\x1b[200~" + block + "\x1b[201~", 0.6)
if not wait_out("+15 lines", 3):
    fail("paste chip card", out[-400:])
send(" summarize")
submit("paste chip expansion", block + "  summarize")  # insertAtom adds `${label} `

# at the threshold the large-paste menu opens; "Attach as local file" saves local://paste-N.md
big = "\n".join(f"row {i}" for i in range(1, 121))
send("\x1b[200~" + big + "\x1b[201~", 0.6)
if not wait_out("Pasted 120 lines", 3):
    fail("paste menu", out[-400:])
send("\x1b[B", 0.2)
send("\r", 0.6)
if not wait_out("Saved 120 pasted lines to local://paste-1.md", 3):
    fail("paste menu local file", out[-400:])
submit("paste menu reference", "local://paste-1.md")

# Ctrl+Shift+V pastes the clipboard text raw: no chip, every row inline
send("\x1b[118;6u", 1.0)
submit("raw paste", clip)

# `#123` offers pr:// and issue:// (PR first); Enter applies the reference without submitting
send("look at #123", 0.6)
send("\r", 0.3)
submit("github ref", "look at pr://123")

# `#` prompt actions: "#cop" + Enter runs "Copy current line" and leaves the text without the token
send("keep this #cop", 0.6)
send("\r", 0.4)
if not wait_out("Copied line: keep this", 3):
    fail("prompt action", out[-400:])
submit("prompt action text", "keep this")

# `@` fuzzy project-wide mention: `@zetamod` finds zeta_module.txt, Enter inserts it with a trailing space
send("read @zetamod", 1.0)
send("\r", 0.4)
submit("at mention", "read @zeta_module.txt")

send("\x04", 0.5)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass
print("editor io ok")
