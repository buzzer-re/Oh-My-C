#!/usr/bin/env python3
"""e2e: background jobs carry the purpose the model gave: proc:// and wait results lead with it, `/jobs` shows it, `/jobs full`
asks the widget role's model for a caption (omp job-widgets/caption.ts) that shows on a later listing, once per job, and a
glanceable command gets none.
Usage: jobs_intent.py <omc> <mock request log> <models.yml> <dir>
Prints "jobs intent ok" or the failing steps."""
import fcntl, json, os, pty, select, shutil, signal, struct, subprocess, sys, termios, time

omc, reqlog, models, root = sys.argv[1:5]
work = os.path.join(root, "work")
agent = os.path.join(root, "agent")
os.makedirs(work, exist_ok=True)
os.makedirs(agent, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
fails = []


def check(name, cond, detail=""):
    if not cond:
        fails.append(name)
        print("FAIL", name, str(detail)[-800:])


env = dict(os.environ, HOME=root, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", OMC_NO_LOCAL_DISCOVERY="1", TERM="xterm-256color",
           OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", OMC_JOB_CAPTION_AFTER_MS="0")
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)


def tool_messages():
    out = []
    for line in open(reqlog):
        r = json.loads(line)
        out = [m for m in r.get("messages", []) if m.get("role") == "tool"] or out
    return ["".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"] for m in out]


# proc:// and wait
script = [["bash", {"i": "Watch the\n  nightly build", "command": "sleep 1; echo built", "async": True}],
          ["read", {"path": "proc://"}],
          ["wait", {}]]
subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "calls:" + json.dumps(script)], cwd=work, env=env, stdin=subprocess.DEVNULL,
               capture_output=True, timeout=60)
msgs = tool_messages()
check("proc listing leads with intent", len(msgs) > 1 and " — Watch the nightly build — sleep 1; echo built" in msgs[1], msgs)
wtext = msgs[2] if len(msgs) > 2 else ""
check("wait intent", "## Completed (1)\n\n### bg_1 [bash] — completed\nIntent: Watch the nightly build\nLabel: sleep 1; echo built\nDelivery: delivered directly to wait.\n```\nbuilt" in wtext, wtext)

# /jobs and /jobs full under a pty
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("modelRoles:\n  widget: mock/mock-1\n")
pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 120, 0, 0))
buf = bytearray()


def pump(seconds):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                buf.extend(os.read(fd, 65536))
            except OSError:
                return


def wait_for(text, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        if text.encode() in buf:
            return True
        pump(0.2)
    return False


def captions():
    n = 0
    for line in open(reqlog):
        r = json.loads(line)
        for m in r.get("messages", []):
            if m.get("role") == "system" and "You explain one background shell command" in json.dumps(m.get("content")):
                n += 1
                last[0] = r
                break
    return n


last = [None]


def send(line):
    os.write(fd, line.encode())
    pump(0.3)
    os.write(fd, b"\r")


try:
    pump(1.5)
    before = captions()
    long_cmd = "for i in 1 2 3; do sleep 30; done"
    call = "call:bash:" + json.dumps({"i": "Wait for the indexer", "command": long_cmd, "async": True})
    send(call)
    check("job started", wait_for("Tool said"), bytes(buf)[-400:])
    call = "call:bash:" + json.dumps({"command": "sleep 30", "async": True})
    del buf[:]
    send(call)
    check("job 2 started", wait_for("Tool said"), bytes(buf)[-400:])
    del buf[:]
    send("/jobs")
    check("/jobs intent", wait_for("Wait for the indexer"), bytes(buf)[-800:])
    check("/jobs no caption yet", b"Waits about" not in buf)
    pump(0.5)
    os.write(fd, b"\x1b")
    pump(0.5)
    del buf[:]
    send("/jobs full")
    pump(2)
    check("/jobs full command", long_cmd.encode() in buf, bytes(buf)[-800:])
    os.write(fd, b"\x1b")
    pump(0.5)
    del buf[:]
    send("/jobs full")
    check("caption on a later listing", wait_for("Waits about 2 min, printing a tick every 3s"), bytes(buf)[-800:])
    check("one caption, none for the glanceable job", captions() == before + 1, captions() - before)
    r = last[0]
    text = json.dumps(r.get("messages")) if r else ""
    check("caption request", "Why it was started: Wait for the indexer" in text and "Killed after: 300s" in text and long_cmd in text, text[-500:])
    check("caption max tokens", r and (r.get("max_tokens") == 120 or r.get("max_completion_tokens") == 120), r and {k: v for k, v in r.items() if "max" in k})
finally:
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
print("jobs intent ok" if not fails else "jobs intent FAILED: " + ", ".join(fails))
sys.exit(1 if fails else 0)
