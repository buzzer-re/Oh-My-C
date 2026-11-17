#!/usr/bin/env python3
"""Model picker (Alt+P) provider scopes and thinking slider under a pty (omp 807c6d37f5 / 94a10c5b22): the footer
names Tab provider and ←/→ thinking, ←/→ step the highlighted model's thinking level (shown after the status hint),
Tab / Shift+Tab step the list through all models and each provider's, and a pick applies the slider's level.
Expected text is omp 18.22.0's, compared side by side in tmux with tests/tui/compare.sh.

usage: model_picker_tui.py <omc> <scratch dir>
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
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-r
        name: Mock Reason
        reasoning: true
        contextWindow: 100000
        maxTokens: 4096
  other:
    baseUrl: http://127.0.0.1:9/v1
    apiKey: OTHER_KEY
    api: openai-completions
    models:
      - id: other-1
        name: Other One
        contextWindow: 100000
        maxTokens: 4096
""")
open(os.path.join(agent, "config.yml"), "w").write("setupVersion: 3\nmodelRoles:\n  default: mock/mock-1\nstartup:\n  checkUpdate: false\n")


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", detail[-1500:], flush=True)
        sys.exit(1)
    print(name, "ok", flush=True)


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
    out = press("\x1bp", 1.5)
    check("footer", "⇥ provider · ←/→ thinking · type to search" in out, out)
    check("no thinking", "role models stay unchanged · no thinking" in out, out)
    out = press("\x1b[B")
    check("slider", "role models stay unchanged · thinking High" in out, out)
    out = press("\x1b[D")
    check("step left", "thinking Med" in out, out)
    out = press("\t")
    check("provider scope", "\u2502" in out and "other/other-1" not in out, out)
    out = press("\t")
    check("next provider", "other/other-1" in out, out)
    out = press("\t")
    check("all again", "mock/mock-1" in out, out)
    press("\x1b[Z")
    out = press("\x1b[Z")
    check("shift tab", "mock/mock-1" in out and "other/other-1" not in out, out)
    press("\x1b[B")
    out = press("\r", 1.2)
    check("pick applies level", "Session-only model: mock/mock-r" in out, out)
    out = press("\x1bp", 1.5)
    check("picked level kept", "mock/mock-r" in out and "thinking Med" in out, out)
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
