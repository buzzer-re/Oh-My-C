#!/usr/bin/env python3
"""The `.`/`c` continue shortcut and prompts rejected before dispatch under a pty (tests/e2e/run.sh), omp
AgentSession.resume + #restoreDroppedPrompt: an empty conversation reports nothing to resume and sends nothing; after a
reply `.` sends the hidden manual-continue directive without a user bubble; a prompt with no credential returns to the
editor.
usage: resume_tui.py <omc> <request log> <scratch dir>
Prints "resume tui ok" or the first failing step."""
import fcntl, json, os, pty, re, select, signal, struct, sys, termios, time

omc, log, scratch = sys.argv[1:4]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
           OMC_CODING_AGENT_DIR=agent, HOME=scratch)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "ANTHROPIC_API_KEY", "ANTHROPIC_OAUTH_TOKEN"):
    env.pop(k, None)
pid = fd = None
out = bytearray()
ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(\x07|\x1b\\)|\x1b[@-Z\\-_]")


def stop():
    global pid
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass
        pid = None


def fail(step, detail=""):
    print(f"resume tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch(model):
    global pid, fd, out
    stop()
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc, "--model", model, "--no-session"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))


def pump(t):
    end = time.time() + t
    while True:
        left = end - time.time()
        if left <= 0:
            return
        r, _, _ = select.select([fd], [], [], left)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            out.extend(data)
            # the terminal capability probes get an answer, as a real terminal gives
            if b"\x1b[c" in data:
                os.write(fd, b"\x1b[?62c")


def plain():
    return ANSI.sub(b"", bytes(out)).decode("utf-8", "replace")


def wait_for(text, t=10):
    end = time.time() + t
    while time.time() < end:
        pump(0.1)
        if text in plain():
            return True
    return False


def submit(text):
    os.write(fd, text.encode())
    pump(0.2)
    os.write(fd, b"\r")


def requests():
    if not os.path.exists(log):
        return []
    with open(log) as f:
        return [json.loads(line) for line in f if line.strip()]


launch("mock/mock-1")
pump(1.0)
before = len(requests())
submit(".")
if not wait_for("Nothing to resume"):
    fail("dot on empty conversation", plain()[-400:])
out.clear()
submit("c")
if not wait_for("Nothing to resume"):
    fail("c on empty conversation", plain()[-400:])
if len(requests()) != before:
    fail("empty conversation sent a request", len(requests()) - before)

submit("say:hello-resume")
if not wait_for("hello-resume"):
    fail("first reply", plain()[-400:])
pump(1.0)
count = len(requests())
out.clear()
submit(".")
end = time.time() + 10
while time.time() < end and len(requests()) == count:
    pump(0.1)
if len(requests()) == count:
    fail("dot after a reply sent nothing", plain()[-400:])
sent = json.dumps(requests()[-1]["messages"][-1])
if "MUST resume most recent intent" not in sent:
    fail("manual-continue directive", sent[:400])
pump(0.5)

# no credential: the prompt is refused before any request and returns to the editor
launch("anthropic/claude-sonnet-4-5")
pump(1.0)
count = len(requests())
out.clear()
submit("keep-this-prompt")
if not wait_for("No API key found for anthropic."):
    fail("missing credential error", plain()[-600:])
pump(0.5)
out.clear()
os.write(fd, b"\x0c")  # a redraw shows what the editor holds
pump(0.5)
os.write(fd, b"!")
if not wait_for("keep-this-prompt!", 5):
    fail("prompt back in the editor", plain()[-600:])
if len(requests()) != count:
    fail("missing credential sent a request", len(requests()) - count)
stop()
print("resume tui ok")
