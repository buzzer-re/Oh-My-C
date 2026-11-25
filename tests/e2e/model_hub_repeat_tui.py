#!/usr/bin/env python3
"""Models hub: a repeated Enter on a just-assigned model row keeps its role (omp bb6d78c023, 29c9405e86). After a
role chip assigns, the row's chip reads as assigned, so Enter Enter there would press it again and clear the role; the
hub leaves the committed role alone until the cursor moves, the strip selection moves or the view changes.

usage: model_hub_repeat_tui.py <omc> <scratch dir>
Prints one "<step> ok" line per step, or "<step> FAIL: ..." and exits 1."""
import fcntl, os, pty, re, select, struct, sys, termios, time

omc, scratch = sys.argv[1], sys.argv[2]
ANSI = re.compile(r"\x1b\[[0-9;?<>=:]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][\s\S]*?\x1b\\|\x1b.")
agent, work = os.path.join(scratch, "agent"), os.path.join(scratch, "work")
os.makedirs(agent, exist_ok=True)
os.makedirs(work, exist_ok=True)
open(os.path.join(agent, "models.yml"), "w").write("""providers:
  mock:
    baseUrl: http://127.0.0.1:9/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        name: Mock One
        reasoning: false
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-2
        name: Mock Two
        reasoning: false
        contextWindow: 100000
        maxTokens: 4096
""")
CONFIG = os.path.join(agent, "config.yml")
open(CONFIG, "w").write("setupVersion: 3\nmodelRoles:\n  default: mock/mock-1\nstartup:\n  checkUpdate: false\n")


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", detail[-1500:], flush=True)
        sys.exit(1)
    print(name, "ok", flush=True)


def default_role():
    m = re.search(r"^  default: (\S+)$", open(CONFIG).read(), re.M)
    return m.group(1) if m else None


pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", COLUMNS="120",
               LINES="40", OMC_CODING_AGENT_DIR=agent, HOME=scratch)
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM", "KITTY_WINDOW_ID"):
        env.pop(k, None)
    os.chdir(work)
    os.execvpe(omc, [omc], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
raw = b""


def pump(secs):
    global raw
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 1 << 20)
            except OSError:
                return
            if not chunk:
                return
            raw += chunk


def press(keys, secs=0.8):
    """the text drawn after `keys`"""
    start = len(raw)
    os.write(fd, keys.encode())
    pump(secs)
    return ANSI.sub("", raw[start:].decode("utf-8", "replace"))


try:
    pump(4)
    press("\x1bm", 1.5)
    out = press("mock-")
    check("search", "mock-1" in out and "mock-2" in out, out)
    press("\x1b[B")
    out = press("\r")
    check("role strip", "mock-2 →" in out.replace("\n", " ") or "mock-2" in out, out)
    press("\r", 1.2)
    check("assigned", default_role() == "mock/mock-2", open(CONFIG).read())
    out = press("\r")
    check("chip reads assigned", "● default" in out, out)
    press("\r", 1.2)
    check("repeat keeps role", default_role() == "mock/mock-2", open(CONFIG).read())
    # moving within the strip re-arms the toggle: right then left, Enter unassigns
    press("\r")
    press("\x1b[C")
    press("\x1b[D")
    press("\r", 1.2)
    check("strip move re-arms", default_role() is None, open(CONFIG).read())
    # assign again, then move the cursor away and back: Enter Enter unassigns
    press("\r")
    press("\r", 1.2)
    check("assigned again", default_role() == "mock/mock-2", open(CONFIG).read())
    press("\x1b[B")
    press("\x1b[A")
    press("\r")
    press("\r", 1.2)
    check("cursor move re-arms", default_role() is None, open(CONFIG).read())
finally:
    if os.environ.get("MP_DUMP"):
        open(os.environ["MP_DUMP"], "wb").write(raw)
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass
