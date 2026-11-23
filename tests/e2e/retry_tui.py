#!/usr/bin/env python3
"""Manual retry under a pty (omp InputController.handleRetry, TurnRecovery.retry, InteractiveMode.syncRetryHintRow):
  ask     Esc on an ask form aborts the turn: the idle row shows "F5 to retry"; F5 re-runs the ask call itself (no
          model request), and the answered form goes on to the model
  resume  a session whose last turn died on a tool call (Esc on the form, then exit) shows the row again after
          `-c`, and F5 replays the call there too
  nothing F5 after a turn that settled reports "Nothing to retry" and sends nothing
  restore a process killed during a bash call: `--restore <session>` closes the cut-off turn and replays the call
          (AgentSession.restoreInterruptedTurn -> retry), so the model gets its real output, not "outcome unknown"
  reuse   a streamed read whose call id repeats a prior turn's bash call (the mock numbers ids per turn) keeps that
          bash card: only a call that starts without a streamed preview (a replay) evicts the prior turn's card
          (EventController #priorTurnToolComponents)
usage: retry_tui.py <omc> <request log> <scratch dir>
Prints "retry tui ok" or the first failing step."""
import fcntl, glob, json, os, pty, re, select, signal, struct, sys, termios, time
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off

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
FORM = '<form><label><input type="radio" name="a" value="x"> x</label></form>'
F5, ESC, ENTER, TAB = "\x1b[15~", "\x1b", "\r", "\t"


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
    print(f"retry tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch(*args):
    global pid, fd, out
    stop()
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc, "--config", NOPRED, "--model", "mock/mock-1", *args], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))


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
            if b"\x1b[c" in data:
                os.write(fd, b"\x1b[?62c")


def plain():
    return ANSI.sub(b"", bytes(out)).decode("utf-8", "replace")


def wait_for(text, t=8, since=0):
    end = time.time() + t
    while time.time() < end:
        pump(0.1)
        if text in plain()[since:]:
            return True
    return False


def last_tool_results():
    try:
        with open(log) as f:
            lines = [json.loads(line) for line in f if '"tools"' in line]
    except OSError:
        return []
    msgs = lines[-1].get("messages", []) if lines else []
    return [m.get("content") if isinstance(m.get("content"), str) else json.dumps(m.get("content"))
            for m in msgs if m.get("role") == "tool"]


def requests():
    try:
        with open(log) as f:
            return sum(1 for line in f if '"tools"' in line)
    except OSError:
        return 0


def send(keys):
    os.write(fd, keys.encode())
    time.sleep(0.3)


def prompt_ask():
    if not wait_for("mock-1", 10):
        fail("startup", plain()[-600:])
    send("call:ask:" + json.dumps({"html": FORM}))
    send(ENTER)
    if not wait_for("Submit"):
        fail("form", plain()[-600:])
    time.sleep(0.3)
    send(ESC)
    if not wait_for("Ask tool was cancelled by the user"):
        fail("cancel", plain()[-600:])


def replay_and_answer(step):
    n = requests()
    mark = len(plain())
    send(F5)
    if not wait_for("Submit", since=mark):
        fail(step + " replay", plain()[-600:])
    if requests() != n:
        fail(step + " replay sent a model request", requests() - n)
    send(ENTER)
    send(TAB)
    send(ENTER)
    if not wait_for("Tool said: User answers:", 10, since=mark):
        fail(step + " answer", plain()[-800:])


# ask: Esc on the form, the hint, F5 replays the call
launch()
prompt_ask()
if not wait_for("F5 to retry"):
    fail("ask hint", plain()[-600:])
replay_and_answer("ask")
mark = len(plain())
send(F5)
if not wait_for("Nothing to retry", since=mark):
    fail("nothing", plain()[-600:])

# resume: the row comes back after -c on a session whose last turn died on the ask call
launch()
prompt_ask()
if not wait_for("F5 to retry"):
    fail("resume hint before exit", plain()[-600:])
stop()
launch("-c")
if not wait_for("F5 to retry", 10):
    fail("resume hint", plain()[-800:])
replay_and_answer("resume")
stop()

# restore: kill the process while the bash call runs, then --restore that session
for f in glob.glob(os.path.join(agent, "sessions", "*", "*.jsonl")):
    os.remove(f)
launch()
if not wait_for("mock-1", 10):
    fail("restore startup", plain()[-600:])
send("bash:sleep 1; echo replayed-$((40+2))")
send(ENTER)
if not wait_for("$ sleep 1"):
    fail("restore tool start", plain()[-600:])
time.sleep(0.3)
stop()
sessions = glob.glob(os.path.join(agent, "sessions", "*", "*.jsonl"))
if len(sessions) != 1:
    fail("restore session", sessions)
n = requests()
launch("--restore", sessions[0])
if not wait_for("Tool said:", 15):
    fail("restore answer", plain()[-800:])
if requests() != n + 1:
    fail("restore requests", requests() - n)
res = last_tool_results()
if not res or "replayed-42" not in res[-1]:
    fail("restore replayed output", res)
stop()

# reuse: a later turn's streamed read reusing call_1 leaves the earlier bash card in place
launch()
if not wait_for("mock-1", 10):
    fail("reuse startup", plain()[-600:])
with open("reuse.txt", "w") as f:
    f.write("reuse\n")
send("bash:echo kept-card")
send(ENTER)
if not wait_for("Tool said: kept-card"):
    fail("reuse bash", plain()[-600:])
send("read:reuse.txt")
send(ENTER)
if not wait_for("Read reuse.txt"):
    fail("reuse read", plain()[-600:])
pump(1)
mark = len(plain())
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 110, 0, 0))
os.kill(pid, signal.SIGWINCH)
pump(1.5)
if "$ echo kept-card" not in plain()[mark:]:
    fail("reuse kept the bash card", plain()[mark:][-1500:])
stop()
print("retry tui ok")
