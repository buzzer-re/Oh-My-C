#!/usr/bin/env python3
"""The quick model picker (Alt+P) refreshes a stale catalog in the background (omp c9e8ae3134): once the last online
discovery is older than the TTL (OMC_DISCOVERY_TTL_MS=0 here), opening the picker re-runs discovery, and the open
picker lists a model a local server loaded after the session started.

usage: picker_refresh_tui.py <omc> <mock_local.py> <scratch dir> <port>
Prints one "<step> ok" line per step, or "<step> FAIL: ..." and exits 1."""
import fcntl, os, pty, re, select, signal, struct, subprocess, sys, termios, time

omc, mock, scratch, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
ANSI = re.compile(r"\x1b\[[0-9;?<>=:]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][\s\S]*?\x1b\\|\x1b.")
agent, work, extra = os.path.join(scratch, "agent"), os.path.join(scratch, "work"), os.path.join(scratch, "extra-model")
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
""")
open(os.path.join(agent, "config.yml"), "w").write("setupVersion: 3\nmodelRoles:\n  default: mock/mock-1\nstartup:\n  checkUpdate: false\n")
srv = subprocess.Popen([sys.executable, "-I", mock, port], env=dict(os.environ, MOCK_LOCAL_LLAMA_EXTRA=extra),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", detail[-1500:], flush=True)
        finish(1)
    print(name, "ok", flush=True)


pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", COLUMNS="120",
               LINES="40", OMC_CODING_AGENT_DIR=agent, HOME=scratch, OMC_DISCOVERY_TTL_MS="0",
               LLAMA_CPP_BASE_URL="http://127.0.0.1:%s/llama" % port, OLLAMA_HOST="127.0.0.1:9",
               LM_STUDIO_BASE_URL="http://127.0.0.1:9/v1")
    env.pop("OMC_NO_LOCAL_DISCOVERY", None)
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
    srv.kill()
    srv.wait()
    sys.exit(code)


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
    start = len(raw)
    os.write(fd, keys.encode())
    pump(secs)
    return ANSI.sub("", raw[start:].decode("utf-8", "replace"))


pump(4)
out = press("\x1bp", 2.0)
check("picker", "gemma-local.gguf" in out and "newly-loaded.gguf" not in out, out)
press("\x1b", 1.0)
open(extra, "w").write("newly-loaded.gguf\n")
start = len(raw)
press("\x1bp", 0.5)
end = time.time() + 8
while time.time() < end and "newly-loaded.gguf" not in ANSI.sub("", raw[start:].decode("utf-8", "replace")):
    pump(0.2)
out = ANSI.sub("", raw[start:].decode("utf-8", "replace"))
check("refreshed", "newly-loaded.gguf" in out, out)
finish(0)
