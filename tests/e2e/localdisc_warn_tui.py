#!/usr/bin/env python3
"""Implicit local-provider discovery warning under a pty (tests/e2e/run.sh): with LM_STUDIO_BASE_URL naming a closed
port, omp's config warning (model-registry.ts #warnProviderDiscoveryFailure) with Bun's ECONNREFUSED text heads the
header when startup found a model (omp's background discovery moves it there), and is a startup notice after the
welcome when startup had to await discovery for want of a model (main.ts drainConfigWarnings); without the env var the
default endpoint stays optional and silent.
usage: localdisc_warn_tui.py <omc> <scratch dir> <closed port>
Prints "localdisc warn tui ok" or the first failing step."""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

omc, scratch, port = sys.argv[1], sys.argv[2], sys.argv[3]
work = os.path.join(scratch, "work")
agent = os.path.join(scratch, "agent")
tmp = os.path.join(scratch, "tmp")
for d in (work, agent, tmp):
    os.makedirs(d, exist_ok=True)
os.chdir(work)
with open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "models.yml")) as a, open(os.path.join(agent, "models.yml"), "w") as b:
    b.write(a.read())

base = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1", TMPDIR=tmp,
            OMC_CODING_AGENT_DIR=agent)
for k in ("TMUX", "SSH_CONNECTION", "SSH_TTY", "SSH_CLIENT", "OMC_NO_LOCAL_DISCOVERY", "OLLAMA_BASE_URL", "OLLAMA_HOST",
          "LLAMA_CPP_BASE_URL", "LM_STUDIO_BASE_URL"):
    base.pop(k, None)
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
    print(f"localdisc warn tui FAIL {step}: {detail!r}")
    stop()
    sys.exit(1)


def launch(env, model=True):
    global pid, fd, out
    stop()
    out = bytearray()
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(omc, [omc] + (["--model", "mock/mock-1"] if model else []) + ["--no-session"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 160, 0, 0))


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


def screen_text():
    """the output without escape sequences, whitespace runs collapsed (the warning wraps over two rows)"""
    b = re.sub(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b[@-_]", b"", bytes(out))
    return re.sub(rb"\s+", b" ", b).decode("utf-8", "replace")


def wait_text(needle, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        pump(0.1)
        if needle in screen_text():
            return True
    return False


url = f"http://127.0.0.1:{port}/v1"
want = (f"Warning: Built-in lm-studio provider: model discovery at {url} (from LM_STUDIO_BASE_URL) failed. Fix or unset "
        f"LM_STUDIO_BASE_URL, or add \"lm-studio\" to disabledProviders. Error: ECONNREFUSED: Unable to connect. Is the "
        f"computer able to access the url?")
launch(dict(base, LM_STUDIO_BASE_URL=url))
if not wait_text(want):
    fail("warning", screen_text()[-900:])
# startup found a model: the header warning comes before the welcome banner's tip
txt = screen_text()
if txt.find("Built-in lm-studio") > txt.find("Tip: "):
    fail("warning above the banner", txt[-900:])

# no model (an agent dir without models.yml): a startup notice after the tip
bare = os.path.join(scratch, "bare-agent")
os.makedirs(bare, exist_ok=True)
launch(dict(base, LM_STUDIO_BASE_URL=url, OMC_CODING_AGENT_DIR=bare), model=False)
if not wait_text(want):
    fail("warning without a model", screen_text()[-900:])
txt = screen_text()
if txt.find("Built-in lm-studio") < txt.find("Tip: "):
    fail("warning below the banner without a model", txt[-900:])

# no env var: the default endpoint is optional, nothing is said
launch(base)
if not wait_text("Tip: "):
    fail("banner", screen_text()[-600:])
pump(2.0)
if "Built-in" in screen_text():
    fail("silent without the env var", screen_text()[-600:])
stop()
print("localdisc warn tui ok")
