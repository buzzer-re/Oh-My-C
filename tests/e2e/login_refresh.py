#!/usr/bin/env python3
"""A provider login refreshes that provider's models in the running session (selector-controller.ts: refreshProvider
after login, omp #5780). omc starts while no LM Studio server answers, then a server that requires the API key comes
up; /login must make its models appear in /model without restarting.

usage: login_refresh.py <omc> <mock_local.py> <agent-dir> <port>
Prints one "<step> ok" line per step, or "<step> FAIL: <screen tail>" and exits 1."""
import json, os, pty, re, select, signal, subprocess, sys, time

omc, mock, agent, port = sys.argv[1:5]
KEY = "lms-secret-key"
ANSI = re.compile(r"\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")
pid, fd = pty.fork()
if pid == 0:
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", COLUMNS="140", LINES="45",
               OMC_CODING_AGENT_DIR=agent, LM_STUDIO_BASE_URL=f"http://127.0.0.1:{port}/lms/v1",
               OLLAMA_BASE_URL="http://127.0.0.1:9/none", LLAMA_CPP_BASE_URL="http://127.0.0.1:9/none")
    env.pop("OMC_NO_LOCAL_DISCOVERY", None)
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
        env.pop(k, None)
    os.execvpe(omc, [omc, "--model", "mock/mock-1"], env)
import fcntl, struct, termios
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 45, 140, 0, 0))
raw = b""
mark = 0


def text():
    return ANSI.sub("", raw[mark:].decode("utf-8", "replace"))


def pump(until, timeout):
    global raw
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                c = os.read(fd, 65536)
            except OSError:
                return False
            if not c:
                return False
            raw += c
        if until(text()):
            return True
    return False


def step(name, keys, *expect, timeout=10, gap=0.05):
    global mark
    mark = len(raw)
    for k in keys:
        os.write(fd, k.encode())
        if gap:
            time.sleep(gap)
    if not pump(lambda s: all(e in s for e in expect), timeout):
        print(name, "FAIL:", text()[-1200:])
        sys.exit(1)
    print(name, "ok")


srv = None
try:
    step("started", [], "mock-1")
    srv = subprocess.Popen([sys.executable, "-I", mock, port], env=dict(os.environ, MOCK_LOCAL_REQUIRE_KEY=KEY))
    time.sleep(0.5)
    step("login selector", list("/login") + ["\r"], "Select provider to login", gap=0.03)
    step("login prompt", list("LM Studio") + ["\r"], "LM Studio API key", gap=0.03)
    step("logged in", list(KEY) + ["\r"], "Logged in to", gap=0.01)
    time.sleep(0.3)
    step("model hub lists it", list("/model") + ["\r"], "lm-studio", gap=0.03)
    state = json.load(open(os.path.join(agent, "local-models.json")))
    ids = [m["id"] for m in state.get("lm-studio", {}).get("models", [])]
    if "qwen2.5-vl-7b" not in ids:
        print("discovery FAIL:", ids)
        sys.exit(1)
    print("discovery ok")
    print("login refresh ok")
finally:
    try:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
    except OSError:
        pass
    if srv:
        srv.kill()
