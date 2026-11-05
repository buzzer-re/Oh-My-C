#!/usr/bin/env python3
"""/quiet under a pty (tests/e2e/run.sh): with a broken and a working MCP server and a bad retry.fallbackChains, the
startup warnings show by default, `/quiet` persists startup.quietWarnings, the next launch keeps the welcome banner
without the warnings while `/mcp list` still names the broken server, and `/quiet off` brings them back.
usage: quiet_tui.py <omc> <mock_mcp.py> <scratch dir>
Prints "quiet tui ok" or the first failing step."""
import fcntl, json, os, pty, select, signal, struct, sys, termios, time

omc, mock_mcp, scratch = sys.argv[1:4]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())
config = os.path.join(agent, "config.yml")
with open(config, "w") as f:
    f.write('retry:\n  fallbackChains:\n    "mock/*":\n      - "other/*"\n')
with open(os.path.join(agent, "mcp.json"), "w") as f:
    json.dump({"mcpServers": {"goodsrv": {"command": sys.executable, "args": ["-I", mock_mcp]},
                              "brokensrv": {"command": "/nonexistent/omc-broken-mcp"}}}, f)

env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
           OMC_CODING_AGENT_DIR=agent, OMP_MCP_STARTUP_TIMEOUT_MS="0")
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT"):
    env.pop(k, None)
pid = fd = None
out = bytearray()


def stop():
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
        except OSError:
            pass


def fail(step, detail=""):
    print(f"quiet tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch():
    global pid, fd, out
    stop()
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc, "--model", "mock/mock-1", "--no-session"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))


def pump(t):
    end = time.time() + t
    while True:
        r, _, _ = select.select([fd], [], [], max(0, end - time.time()))
        if not r:
            return
        try:
            data = os.read(fd, 65536)
        except OSError:
            return
        if not data:
            return
        out.extend(data)


def wait_out(needle, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle.encode() in out:
            return True
    return False


def command(text):
    os.write(fd, text.encode())
    pump(0.3)
    os.write(fd, b"\r")


def setting():
    with open(config) as f:
        return f.read()


# default: the warnings show
launch()
if not wait_out("MCP finished with failures"):
    fail("default mcp warning", out[-800:])
if b"brokensrv" not in out or b"Warning: " not in out:
    fail("default warnings", out[-800:])

# the menu shows its icon: a muted bell in the unicode preset
os.write(fd, b"/qui")
if not wait_out("\U0001F515", 5):
    fail("unicode icon", out[-500:])
os.write(fd, b"\x7f" * 4)
pump(0.3)

# /quiet persists it
command("/quiet status")
if not wait_out("Quiet startup is off."):
    fail("status off", out[-500:])
command("/quiet")
if not wait_out("Quiet startup on: warnings stay hidden the next time omc opens."):
    fail("toggle on", out[-500:])
if "quietWarnings: true" not in setting():
    fail("persisted on", setting())

# next launch: banner yes, warnings no, and the failure is still one command away
launch()
if not wait_out("Tip: ", 15):
    fail("banner kept", out[-800:])
pump(3.0)
for needle in (b"MCP finished with failures", b"failed to connect", b"Connected to MCP", b"Warning: ", b"brokensrv"):
    if needle in out:
        fail(f"still shown: {needle.decode()}", out[-800:])
command("/mcp list")
if not wait_out("brokensrv"):
    fail("/mcp list", out[-800:])
command("/quiet nonsense")
if not wait_out("Usage: /quiet [on|off|status]"):
    fail("usage", out[-500:])
command("/quiet off")
if not wait_out("Quiet startup off: warnings show again the next time omc opens."):
    fail("toggle off", out[-500:])
if "quietWarnings: false" not in setting():
    fail("persisted off", setting())

# back to normal
with open(config, "a") as f:
    f.write("\nsymbolPreset: nerd\n")  # omc's rewrite may leave no trailing newline
launch()
if not wait_out("MCP finished with failures"):
    fail("warning back", out[-800:])

# the same icon from the nerd font
os.write(fd, b"/qui")
if not wait_out("", 5):
    fail("nerd icon", out[-500:])
stop()
print("quiet tui ok")
