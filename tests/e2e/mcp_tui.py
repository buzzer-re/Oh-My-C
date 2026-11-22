#!/usr/bin/env python3
"""Drive /mcp and /ssh in the interactive TUI under a pty (WP-18): the add wizard end to end on a stdio
server (name from the arguments, transport, command, arguments, connection test, scope, review, save),
/mcp test, /mcp list, wizard cancel with Ctrl+C and /ssh add; the xd:// mount notices (the `xdev: xd://: mounted …` /
`unmounted …` status lines and, with a request log, the model-facing xdev-mount-notice before the next prompt).

usage: mcp_tui.py <omc> <mock_mcp.py> [mock request log]
Runs in a throwaway project dir; OMC_CODING_AGENT_DIR comes from the caller. Prints "mcp tui ok" or what the
screen showed instead."""
import fcntl, json, os, pty, re, select, struct, subprocess, sys, tempfile, termios, time
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off

omc, mock = sys.argv[1], sys.argv[2]
reqlog = sys.argv[3] if len(sys.argv) > 3 else None
proj = tempfile.mkdtemp(prefix="omc-mcptui-", dir=os.environ.get("TMPDIR"))
ANSI = re.compile(r"\x1b\[[0-9;?<>=:]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")

pid, fd = pty.fork()
if pid == 0:
    os.chdir(proj)
    env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", OMC_NO_BROWSER="1")
    for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
        env.pop(k, None)
    os.execvpe(omc, [omc, "--config", NOPRED, "--model", "mock/mock-1", "--no-session"], env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 140, 0, 0))

raw = b""


def text():
    return ANSI.sub("", raw.decode("utf-8", "replace"))


def pump(until, timeout):
    global raw
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return False
            if not chunk:
                return False
            raw += chunk
        if until(text()):
            return True
    return False


def step(keys, want, timeout=15):
    n0 = len(text())
    for k in keys:
        os.write(fd, k.encode())
        time.sleep(0.3)
    if not pump(lambda s: want in s[n0:], timeout):
        print("mcp tui FAIL: waiting for %r after %r; screen tail:\n%s" % (want, keys, text()[-1500:]))
        finish(1)


def finish(code):
    try:
        os.write(fd, b"\x04")
        pump(lambda s: False, 1.0)
    except OSError:
        pass
    try:
        os.kill(pid, 9)
    except OSError:
        pass
    os.waitpid(pid, 0)
    subprocess.run(["rm", "-rf", proj])
    sys.exit(code)


if not pump(lambda s: "╰─" in s, 20):
    print("mcp tui FAIL: no editor:", text()[-600:])
    finish(1)
time.sleep(0.5)
step(["/mcp add demo", "\r"], "How does omp reach it?")
step(["\r"], "The program omp starts for this server.")
step(["python3", "\r"], "Passed to python3, separated by spaces.")
step(["-I " + mock, "\r"], "Where should it live?", 20)
step(["\x1b[B", "\r"], "Save this configuration?")
step(["\r"], "Run /mcp list to see all configured servers.", 25)
cfg = json.load(open(os.path.join(proj, ".omc", "mcp.json")))
if cfg.get("mcpServers", {}).get("demo") != {"type": "stdio", "command": "python3", "args": ["-I", mock]}:
    print("mcp tui FAIL: mcp.json", json.dumps(cfg))
    finish(1)
if 'Added server "demo" to project config' not in text() or "Successfully connected to server" not in text():
    print("mcp tui FAIL: add result:", text()[-800:])
    finish(1)
if "xdev: xd://: mounted mcp__demo_echo_text" not in text():
    print("mcp tui FAIL: no mount notice:", text()[-800:])
    finish(1)
step(["/mcp test demo", "\r"], 'Successfully connected to "demo"', 20)
step(["/mcp list", "\r"], "Project level (")
step(["\x1b"], "")
time.sleep(0.6)
step(["/mcp add x", "\r"], "How does omp reach it?")
step(["\x03"], "Server creation cancelled.")
step(["/ssh add box --host 10.1.2.3 --port 2200", "\r"], 'Added SSH host "box" to project config')
hosts = json.load(open(os.path.join(proj, ".omc", "ssh.json")))
if hosts != {"hosts": {"box": {"host": "10.1.2.3", "port": 2200}}}:
    print("mcp tui FAIL: ssh.json", json.dumps(hosts))
    finish(1)
# the model learns of an unmount of a device its prompt listed with the next prompt
step(["say:first", "\r"], "first")
step(["/mcp disable demo", "\r"], "xdev: xd://: unmounted mcp__demo_echo_text")
step(["say:second", "\r"], "second")
if reqlog:
    time.sleep(0.5)
    reqs = [json.loads(l) for l in open(reqlog)]
    last = [r for r in reqs if r.get("tools") and "say:second" in json.dumps(r["messages"][-1:])]
    want = "<system-notice>\nxd:// device inventory changed.\nUnmounted; writes fail:\n- xd://mcp__demo_echo_text\n</system-notice>"
    msgs = last[-1]["messages"] if last else []
    if len(msgs) < 2 or json.dumps(msgs[-2]) != json.dumps({"role": "user", "content": [{"type": "text", "text": want}]}):
        print("mcp tui FAIL: mount notice request:", json.dumps(msgs[-2:])[:800])
        finish(1)
print("mcp tui ok")
finish(0)
