#!/usr/bin/env python3
"""`omc --restore` under a pty (omp session/exit-diagnostics.ts, AgentSession.restoreInterruptedTurn):
  idle    a finished turn followed by an idle eval snapshot and a signal exit (a session another process wrote) is not
          replayed: no model request, no interrupted-turn abort written (omp 97c79a0673)
  jobs    a process killed while a background bash job ran: --restore replays the cut-off call, then reports the job
          whose outcome is unknown as a failed job, delivered after the retried run (omp 54e5ba00a6, e88d3525d8)
usage: restore_tui.py <omc> <request log> <scratch dir>
Prints "restore tui ok" or the first failing step."""
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
ENTER = "\r"
JOB_PID = os.path.join(work, "job.pid")


def stop():
    global pid
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass
        pid = None


def reap_job():
    """the background job outlives a killed omc: end it by the pid its shell wrote (the shell execs into the sleep)"""
    try:
        with open(JOB_PID) as f:
            os.kill(int(f.read().strip()), signal.SIGKILL)
    except (OSError, ValueError):
        pass


def fail(step, detail=""):
    print(f"restore tui FAIL {step}: {detail!r}")
    stop()
    reap_job()
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


def requests():
    try:
        with open(log) as f:
            return [json.loads(line) for line in f if '"tools"' in line]
    except OSError:
        return []


def send(keys):
    os.write(fd, keys.encode())
    time.sleep(0.3)


def entries(path):
    return [json.loads(l) for l in open(path) if l.strip()]


# idle: user, finished answer, an eval runtime snapshot (an idle context rebuild), a signal exit
stamp = "2026-11-20T10:00:00.000Z"
idle = os.path.join(scratch, "idle.jsonl")
rows = [
    {"type": "session", "version": 3, "id": "01a2108f-65d0-7852-9205-11d17a1ea528", "timestamp": stamp, "cwd": work},
    {"type": "message", "id": "e1", "parentId": None, "timestamp": stamp,
     "message": {"role": "user", "content": [{"type": "text", "text": "say:hi"}], "attribution": "user", "timestamp": 1}},
    {"type": "message", "id": "e2", "parentId": "e1", "timestamp": stamp,
     "message": {"role": "assistant", "content": [{"type": "text", "text": "hi"}], "api": "openai-completions", "provider": "mock",
                 "model": "mock-1", "usage": {"input": 1, "output": 1, "cacheRead": 0, "cacheWrite": 0, "totalTokens": 2,
                 "cost": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0, "total": 0}}, "stopReason": "stop",
                 "timestamp": 2, "completedAt": 3}},
    {"type": "message", "id": "e3", "parentId": "e2", "timestamp": stamp,
     "message": {"role": "custom", "customType": "eval-state-context", "content": "<system-reminder>eval runtime state</system-reminder>",
                 "display": False, "attribution": "agent", "timestamp": 4}},
    {"type": "custom", "id": "e4", "parentId": "e3", "timestamp": stamp, "customType": "session_exit",
     "data": {"reason": "sighup", "kind": "signal", "recordedAt": stamp}},
]
with open(idle, "w") as f:
    f.write("".join(json.dumps(r) + "\n" for r in rows))
n = len(requests())
launch("--restore", idle)
if not wait_for("mock-1", 10):
    fail("idle startup", plain()[-600:])
pump(2.5)
if len(requests()) != n:
    fail("idle replayed", len(requests()) - n)
if any(e.get("type") == "message" and e["message"].get("stopReason") == "aborted" for e in entries(idle)):
    fail("idle abort written", entries(idle)[-1])
stop()

# jobs: a background job starts, then the process dies under the next (foreground) call
for f in glob.glob(os.path.join(agent, "sessions", "*", "*.jsonl")):
    os.remove(f)
launch()
if not wait_for("mock-1", 10):
    fail("jobs startup", plain()[-600:])
script = [["bash", {"command": "echo $$ > job.pid; exec sleep 20", "async": True}],
          ["bash", {"command": "sleep 2; echo replayed-$((40+4))"}]]
send("calls:" + json.dumps(script))
send(ENTER)
if not wait_for("$ sleep 2", 15):
    fail("jobs tool start", plain()[-800:])
time.sleep(0.3)
stop()
sessions = glob.glob(os.path.join(agent, "sessions", "*", "*.jsonl"))
if len(sessions) != 1:
    fail("jobs session", sessions)
n = len(requests())
launch("--restore", sessions[0])
want = ("Previous OMC process exited before background job bg_1 delivered its final result; its outcome is unknown. "
        "Its subprocess may still be running or may have partially completed — verify before rerunning.")
end = time.time() + 30
seen = False
while time.time() < end and not seen:
    pump(0.2)
    seen = any(want in json.dumps(r.get("messages", []), ensure_ascii=False) for r in requests()[n:])
if not seen:
    fail("jobs report", [json.dumps(r.get("messages", [])[-1:])[:400] for r in requests()[n:]])
replay = [r for r in requests()[n:] if "replayed-44" in json.dumps(r.get("messages", []))]
if not replay:
    fail("jobs replay", len(requests()) - n)
first = next(i for i, r in enumerate(requests()[n:]) if want in json.dumps(r.get("messages", []), ensure_ascii=False))
if first < requests()[n:].index(replay[0]):
    fail("jobs report before the replay")
stop()
reap_job()
print("restore tui ok")
