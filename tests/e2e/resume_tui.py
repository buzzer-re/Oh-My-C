#!/usr/bin/env python3
"""The `.`/`c` continue shortcut and prompts rejected before dispatch under a pty (tests/e2e/run.sh), omp
AgentSession.resume + #restoreDroppedPrompt: an empty conversation reports nothing to resume and sends nothing; after a
reply `.` sends the hidden manual-continue directive without a user bubble; a prompt with no credential returns to the
editor.
usage: resume_tui.py <omc> <request log> <scratch dir>
Prints "resume tui ok" or the first failing step."""
import fcntl, glob, json, os, pty, re, select, signal, struct, subprocess, sys, termios, time

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



def resumes_with_directive(step):
    count = len(requests())
    out.clear()
    submit(".")
    end = time.time() + 10
    while time.time() < end and len(requests()) == count:
        pump(0.1)
    if len(requests()) == count:
        fail(step + " sent nothing", plain()[-400:])
    sent = json.dumps(requests()[-1]["messages"][-1])
    if "MUST resume most recent intent" not in sent:
        fail(step + " directive", sent[:400])
    pump(0.5)


# /handoff: the prompt survives only in the summary, and `.` still resumes it with the directive
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("compaction:\n  keepRecentTokens: 1\n")
launch("mock/mock-1")
pump(1.0)
submit("say:before-handoff")
if not wait_for("before-handoff"):
    fail("reply before the handoff", plain()[-400:])
pump(1.0)
count = len(requests())
submit("/handoff")
end = time.time() + 15
while time.time() < end and len(requests()) == count:
    pump(0.1)
if len(requests()) == count:
    fail("handoff sent nothing", plain()[-400:])
pump(1.5)
resumes_with_directive("dot after a handoff")
os.unlink(os.path.join(agent, "config.yml"))

# a compaction that kept nothing: the summary alone is resumable, not a prompt that runs again as is
subprocess.run([omc, "--model", "mock/mock-1", "-p", "say:only-in-summary"], env=env, stdin=subprocess.DEVNULL,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
files = sorted(glob.glob(os.path.join(agent, "sessions", "**", "*.jsonl"), recursive=True), key=os.path.getmtime)
if not files:
    fail("no session file for the compaction")
with open(files[-1]) as f:
    entries = [json.loads(line) for line in f if line.strip()]
with open(files[-1], "a") as f:
    f.write(json.dumps({"type": "compaction", "id": "c0mpact0", "parentId": entries[-1].get("id"),
                        "timestamp": "2026-11-06T00:00:00.000Z", "summary": "the summarized request",
                        "tokensBefore": 10}) + "\n")
stop()
out = bytearray()
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "--model", "mock/mock-1", "--continue"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
pump(1.5)
resumes_with_directive("dot on a summary alone")
if "the summarized request" not in json.dumps(requests()[-1]["messages"]):
    fail("summary in the resumed request", json.dumps(requests()[-1]["messages"])[:400])
if "only-in-summary" in json.dumps(requests()[-1]["messages"]):
    fail("compacted prompt in the resumed request", json.dumps(requests()[-1]["messages"])[:400])

# an empty Enter interrupts the turn and forces the queue into the continuation: the steer, then the follow-up
launch("mock/mock-1")
pump(1.0)
submit("slow:20:forced-run")
end = time.time() + 10
count = len(requests())
while time.time() < end and "forced-run" not in json.dumps(requests()[-1:]):
    pump(0.1)
pump(0.5)
count = len(requests())
os.write(fd, b"say:forced-follow-up")
pump(0.2)
os.write(fd, b"\x11")  # app.message.followUp
pump(0.3)
submit("say:forced-steer")
pump(0.5)
started = time.time()
os.write(fd, b"\r")
end = time.time() + 15
while time.time() < end and len(requests()) < count + 2:
    pump(0.1)
if len(requests()) < count + 2:
    fail("forced delivery sent nothing", plain()[-600:])
if time.time() - started > 10:
    fail("forced delivery waited for the turn", time.time() - started)
first, second = [json.dumps(r["messages"]) for r in requests()[count:count + 2]]
if "forced-steer" not in first or "forced-follow-up" in first:
    fail("steer leads the continuation", first[-300:])
if "forced-follow-up" not in second:
    fail("follow-up joins the continuation", second[-300:])
pump(0.5)

# the idle recap waits for background jobs: a recap written over a running job would be stale when its result arrives
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("recap:\n  idleSeconds: 1\n")
launch("mock/mock-1")
pump(1.0)
count = len(requests())
submit("call:task:" + json.dumps({"context": "recap", "tasks": [{"task": "slow:5:recap-job-done"}]}))
is_recap = lambda r: "<recap>" in json.dumps(r["messages"][-1])
end = time.time() + 30
while time.time() < end and not any(is_recap(r) for r in requests()[count:]):
    pump(0.2)
later = requests()[count:]
first = next((i for i, r in enumerate(later) if is_recap(r)), None)
if first is None:
    fail("no recap after the job settled", plain()[-600:])
if "recap-job-done" not in json.dumps(later[first]["messages"]) or "async-result" in json.dumps(later[first]["messages"][-1]):
    fail("recap before the job's result", json.dumps(later[first]["messages"])[-600:])
os.unlink(os.path.join(agent, "config.yml"))

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

# the picker opens on titled sessions and the full scan brings in the untitled one and the prompts
def run_print(text):
    subprocess.run([omc, "--model", "mock/mock-1", "-p", text], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)


run_print("say:picker-named-prompt")
files = sorted(glob.glob(os.path.join(agent, "sessions", "**", "*.jsonl"), recursive=True), key=os.path.getmtime)
if not files:
    fail("no session file")
with open(files[-1]) as f:
    body = f.read()
with open(files[-1], "w") as f:
    f.write('{"type":"title","title":"Picker Titled Row"}\n' + body)
time.sleep(0.05)
run_print("say:picker-untitled-prompt")
stop()
out = bytearray()
pid, fd = pty.fork()
if pid == 0:
    os.execve(omc, [omc, "--model", "mock/mock-1", "--resume"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
if not wait_for("Picker Titled Row"):
    fail("titled row", plain()[-600:])
if not wait_for("say:picker-untitled-prompt"):
    fail("untitled row after the full scan", plain()[-600:])
if not wait_for("say:picker-named-prompt"):
    fail("titled row's prompt after the full scan", plain()[-600:])
stop()
print("resume tui ok")
