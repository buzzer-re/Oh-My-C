#!/usr/bin/env python3
"""/settings rows checked side by side against omp 19.1.1: a string setting without choices opens a text field
(settings-defs.ts entryToSettingDef; the generated table types some of them `submenu`), and the Todo Backend row offers
omp's runtime choice list without extensions, `Native` alone (omp b0d3834d78).

usage: settings_rows_tui.py <omc> <scratch dir>
Prints one "<step> ok" line per step, or "<step> FAIL: ..." and exits 1."""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

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
        contextWindow: 100000
        maxTokens: 4096
""")
open(os.path.join(agent, "config.yml"), "w").write("setupVersion: 3\nmodelRoles:\n  default: mock/mock-1\nstartup:\n  checkUpdate: false\n")

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


def finish(code):
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    sys.exit(code)


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", detail[-1500:], flush=True)
        finish(1)
    print(name, "ok", flush=True)


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


def press(keys, secs=0.6):
    start = len(raw)
    os.write(fd, keys.encode())
    pump(secs)
    return ANSI.sub("", raw[start:].decode("utf-8", "replace"))


pump(4)
press("/settings", 0.4)
press("\r", 1.5)
press("ovnfs binary", 1.0)
out = press("\r", 1.0)
check("text field", "Clear field to unset" in out, out)
press("\x1b", 0.8)
press("\x7f" * 12, 0.8)
press("todo backend", 1.0)
for _ in range(10):
    out = press("\x1b[A", 0.3)
    if "❯ Todo Backend" in out:
        break
check("todo backend row", "❯ Todo Backend" in out, out)
out = press("\r", 1.0)
check("native choice", "Native" in out and "Keep the todo list in the session only" in out, out)
finish(0)
