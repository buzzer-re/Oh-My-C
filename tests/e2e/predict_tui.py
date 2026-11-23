#!/usr/bin/env python3
"""Composer predictions under a pty (tests/e2e/run.sh, omp composer-prediction-controller.ts): after a completed turn
one side request asks for the next message; its reply shows as ghost text in the empty composer, Tab inserts it
without sending, Enter then sends it; NO_PREDICTION shows nothing; with composer.predictionAlternatives a second Tab
lists the alternatives; a /compact keeps the ghost; with composer.predictions off no side request is made.
usage: predict_tui.py <omc> <req.jsonl> <scratch dir>
Prints "predict tui ok" or the first failing step."""
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
    print(f"predict tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch(cfg):
    global pid, fd, out
    stop()
    with open(config, "w") as f:
        f.write(cfg)
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
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


def last_user(req):
    for m in reversed(req.get("messages", [])):
        if m.get("role") == "user":
            c = m.get("content")
            return c if isinstance(c, str) else "".join(p.get("text", "") for p in c if isinstance(p, dict))
    return ""


def predictions(start):
    return [r for r in requests()[start:] if last_user(r).startswith("<composer-prediction>")]


def wait_requests(cond, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if cond():
            return True
    return False


# ---- one prediction: ghost text, Tab inserts it, Enter sends it
launch("")
start = len(requests())
mark = len(out)
command("say:first reply predict=Run~the~tests")
if not wait_out("first reply", 15, mark):
    fail("reply", out[mark:][-800:])
if not wait_requests(lambda: len(predictions(start)) == 1):
    fail("prediction request", [last_user(r)[:60] for r in requests()[start:]])
p = predictions(start)[0]
if "Reply with that message only" not in last_user(p) or "NO_PREDICTION" not in last_user(p):
    fail("prompt", last_user(p)[:300])
if p.get("max_tokens", p.get("max_completion_tokens")) != 1024:
    fail("max tokens", {k: v for k, v in p.items() if "max" in k})
if not wait_out("Run the tests", 10, mark):
    fail("ghost", [(m.get("role"), str(m.get("content"))[:80]) for m in p.get("messages", [])])
pump(0.5)
start = len(requests())
os.write(fd, b"\t")
pump(0.5)
if len(requests()) != start:
    fail("tab sent", last_user(requests()[-1]))
os.write(fd, b"\r")
if not wait_requests(lambda: any(last_user(r).endswith("Run the tests") for r in requests()[start:])):
    fail("insert then send", [last_user(r)[-60:] for r in requests()[start:]])

# ---- NO_PREDICTION: nothing shown, typing keeps working
launch("")
start = len(requests())
mark = len(out)
command("say:second reply predict=NO_PREDICTION")
if not wait_requests(lambda: len(predictions(start)) == 1):
    fail("no-prediction request")
pump(1.0)
if b"NO_PREDICTION" in out[mark:].replace(b"predict=NO_PREDICTION", b""):
    fail("skip reply shown", out[mark:][-800:])

# ---- alternatives: the prompt asks for four, Tab inserts the first, Tab again lists them, Down+Tab swaps
launch("composer:\n  predictionAlternatives: true\n")
start = len(requests())
mark = len(out)
command("say:third reply predict=1.~Alpha~plan|-~Beta~plan|alpha~plan!")
if not wait_requests(lambda: len(predictions(start)) == 1):
    fail("alternatives request")
if "up to 4 different messages" not in last_user(predictions(start)[0]):
    fail("alternatives prompt", last_user(predictions(start)[0])[:300])
if not wait_out("Alpha plan", 10, mark):
    fail("alternatives ghost", out[mark:][-800:])
pump(0.3)
os.write(fd, b"\t")
if not wait_out("1 more", 5, mark):
    fail("alternatives hint", out[mark:][-800:])
mark = len(out)
os.write(fd, b"\t")
if not wait_out("Beta plan", 5, mark):
    fail("alternatives list", out[mark:][-800:])
os.write(fd, b"\x1b[B")
pump(0.3)
os.write(fd, b"\t")
pump(0.3)
start = len(requests())
os.write(fd, b"\r")
if not wait_requests(lambda: any(last_user(r).endswith("Beta plan") for r in requests()[start:])):
    fail("alternative sent", [last_user(r)[-60:] for r in requests()[start:]])

# ---- a /compact keeps the prediction: its anchor (the last message) survives in the kept tail, so the ghost comes
# back once the command leaves the composer (omp cancels only on a new session or a focus switch)
launch("compaction:\n  keepRecentTokens: 1\n")
command("say:fifth reply")
if not wait_out("fifth reply", 15, len(out)):
    fail("compact first reply", out[-800:])
mark = len(out)
command("say:sixth reply predict=Keep~going")
if not wait_out("Keep going", 15, mark):
    fail("compact ghost", out[mark:][-800:])
pump(0.5)
mark = len(out)
command("/compact")
if not wait_out("compacted", 20, mark):
    fail("compact ran", out[mark:][-800:])
if not wait_out("Keep going", 5, mark):
    fail("ghost after compact", out[mark:][-800:])

# ---- off: no side request
launch("composer:\n  predictions: false\n")
start = len(requests())
mark = len(out)
command("say:fourth reply")
if not wait_out("fourth reply", 15, mark):
    fail("off reply", out[mark:][-800:])
pump(1.5)
if predictions(start):
    fail("off request")
stop()
print("predict tui ok")
