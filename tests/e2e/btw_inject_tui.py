#!/usr/bin/env python3
"""/btw steering inject and Ctrl+R over an ask prompt under a pty (tests/e2e/run.sh): with btw.inject on, the side
question carries the instruction, the answer shows its block, Alt+I sends it once, verbatim and unexpanded; with it off
there is no instruction, block or send. Ctrl+R in the ask dialog's custom-answer prompt picks into that prompt, not the
hidden composer.
usage: btw_inject_tui.py <omc> <req.jsonl> <scratch dir>
Prints "btw inject tui ok" or the first failing step."""
import fcntl, json, os, pty, select, signal, struct, sys, termios, time

omc, reqlog, scratch = sys.argv[1:4]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
config = os.path.join(agent, "config.yml")

env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
           OMC_CODING_AGENT_DIR=agent)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "TERM_PROGRAM"):
    env.pop(k, None)
pid = fd = None
out = bytearray()


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
    print(f"btw inject tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch(inject):
    global pid, fd, out
    stop()
    with open(config, "w") as f:
        f.write("btw:\n  inject: %s\n" % ("true" if inject else "false"))
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 160, 0, 0))
    pump(1.0)


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


def wait_out(needle, timeout=15, mark=0):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle.encode() in out[mark:]:
            return True
    return False


def command(text):
    os.write(fd, text.encode())
    pump(0.3)
    os.write(fd, b"\r")


def requests():
    res = []
    try:
        with open(reqlog) as f:
            for line in f:
                res.append(json.loads(line))
    except OSError:
        pass
    return res


def user_texts(req):
    texts = []
    for m in req.get("messages", []):
        if m.get("role") != "user":
            continue
        c = m.get("content")
        texts.append(c if isinstance(c, str) else "".join(p.get("text", "") for p in c if isinstance(p, dict)))
    return texts


def sent_count(text):
    """requests whose last user message ends in `text` (the first turn carries a system-reminder prefix)"""
    return sum(1 for r in requests() if user_texts(r) and user_texts(r)[-1].endswith(text))


def wait_sent(text, n=1, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if sent_count(text) >= n:
            return True
    return False


# ---- on: instruction, block, one verbatim send
launch(True)
start = len(requests())
command("/btw slow:0:Try this <inject>say:INJ-OK</inject>")
if not wait_out("Inject into main session"):
    fail("block label", out[-800:])
if not wait_out("to inject"):
    fail("footer hint", out[-800:])
side = [r for r in requests()[start:] if any("<btw>" in t for t in user_texts(r))]
if not side or "Steering: only when the question asks" not in user_texts(side[-1])[-1]:
    fail("instruction", side[-1:] or "no side request")
mark = len(out)
os.write(fd, b"\x1bi")
if not wait_sent("say:INJ-OK"):
    fail("send", out[-800:])
if not wait_out("Injected into main session", 10, mark):
    fail("sent label", out[mark:][-800:])
if not wait_out("INJ-OK", 10, mark):
    fail("reply", out[mark:][-800:])
os.write(fd, b"\x1bi")  # once per answer
pump(1.5)
if sent_count("say:INJ-OK") != 1:
    fail("sent twice", sent_count("say:INJ-OK"))

# model-written text never runs as a command
mark = len(out)
command("/btw slow:0:<inject>/session</inject>")
if not wait_out("to inject", 15, mark):
    fail("second block", out[mark:][-800:])
os.write(fd, b"\x1bi")
if not wait_sent("/session"):
    fail("slash text stays literal", out[mark:][-800:])

# an unclosed block in a finished answer stays plain text and cannot be sent
mark = len(out)
command("/btw slow:0:Note <inject>never closed")
if not wait_out("to close", 15, mark):
    fail("unclosed", out[mark:][-800:])
pump(0.5)
if b"to inject" in out[mark:]:
    fail("unclosed block key", out[mark:][-800:])
os.write(fd, b"\x1bi")
pump(1.0)
if any(user_texts(r) and user_texts(r)[-1].endswith("never closed") for r in requests()):
    fail("unclosed send")

# ---- off: no instruction, no block, no send
launch(False)
start = len(requests())
mark = len(out)
command("/btw slow:0:Off <inject>say:OFF-SENT</inject>")
if not wait_out("to close", 15, mark):
    fail("off answer", out[mark:][-800:])
pump(0.5)
side = [r for r in requests()[start:] if any("<btw>" in t for t in user_texts(r))]
if not side or "Steering:" in user_texts(side[-1])[-1]:
    fail("off instruction", side[-1:] or "no side request")
if b"Inject into main session" in out[mark:] or b"to inject" in out[mark:]:
    fail("off block", out[mark:][-800:])
os.write(fd, b"\x1bi")
pump(1.0)
if sent_count("say:OFF-SENT"):
    fail("off send")
os.write(fd, b"\x1b")  # close the panel
pump(0.3)

# ---- Ctrl+R over the ask dialog's custom-answer prompt: the pick lands in the prompt
mark = len(out)
command("say:HISTORY-ENTRY")
if not wait_out("HISTORY-ENTRY", 15, mark):
    fail("history entry", out[mark:][-800:])
pump(0.5)
mark = len(out)
command('call:ask:{"questions":[{"id":"q1","question":"Pick one","options":[{"label":"Aa"},{"label":"Bb"}]}]}')
if not wait_out("Pick one", 15, mark):
    fail("ask", out[mark:][-800:])
pump(0.8)
os.write(fd, b"\x1b[B\x1b[B")  # down to Other
pump(0.3)
os.write(fd, b"\r")
if not wait_out("> ", 5):
    fail("ask prompt", out[mark:][-800:])
pump(0.5)
mark = len(out)
os.write(fd, b"\x12")
if not wait_out("History", 5, mark):
    fail("history over the prompt", out[mark:][-800:])
pump(0.3)
os.write(fd, b"HISTORY")  # recency would pick the ask prompt itself
pump(0.8)
os.write(fd, b"\r")
pump(0.8)
# the prompt holds the pick: submitting it answers the ask
os.write(fd, b"\r")
end = time.time() + 15
answered = False
while time.time() < end and not answered:
    pump(0.2)
    for r in requests():
        for m in r.get("messages", []):
            if m.get("role") == "tool" and "say:HISTORY-ENTRY" in json.dumps(m.get("content")):
                answered = True
if not answered:
    fail("ask answer", out[-800:])

stop()
print("btw inject tui ok")
