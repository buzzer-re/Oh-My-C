#!/usr/bin/env python3
"""Plan mode remembers the pre-plan model across a resume (omp 05aca3f69c, 9a2daffb9b): entering plan mode with a
`plan` role journals the model and thinking level it replaced (`prePlanModelIdentity` / `prePlanThinkingLevel` on the
plan mode_change); a session resumed in plan mode restores that model when planning ends. The pre-plan model here is a
session-only `/switch` pick, so the resume (which restores the default-role model) does not land on it by itself.

usage: plan_resume_tui.py <omc> <agent dir>   (the agent dir holds a models.yml with the mock provider)
Prints one "<step> ok" line per step, or "<step> FAIL: ..." and exits 1."""
import glob, json, os, pty, re, select, signal, sys, time

omc, agent = sys.argv[1], sys.argv[2]
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off
ANSI = re.compile(r"\x1b\[[0-9;?<>=:]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][\s\S]*?\x1b\\|\x1b.")
scratch = os.path.dirname(os.path.abspath(agent))
work = os.path.join(scratch, "planres-work")
os.makedirs(work, exist_ok=True)
open(os.path.join(agent, "config.yml"), "w").write(
    "setupVersion: 3\nmodelRoles:\n  default: mock/mock-1\n  plan: mock/mock-think\nstartup:\n  checkUpdate: false\n")


def check(name, ok, detail=""):
    if not ok:
        print(name, "FAIL:", str(detail)[-1500:], flush=True)
        sys.exit(1)
    print(name, "ok", flush=True)


def entries():
    files = sorted(glob.glob(os.path.join(agent, "sessions", "**", "*.jsonl"), recursive=True), key=os.path.getmtime)
    if not files:
        return []
    return [json.loads(l) for l in open(files[-1]) if l.strip()]


class App:
    def __init__(self, *args):
        env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1",
                   OMC_CODING_AGENT_DIR=agent, HOME=scratch, COLUMNS="120", LINES="40")
        for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM", "KITTY_WINDOW_ID"):
            env.pop(k, None)
        self.raw = b""
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(work)
            os.execve(omc, [omc, "--config", NOPRED, *args], env)

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    chunk = os.read(self.fd, 1 << 20)
                except OSError:
                    return
                if not chunk:
                    return
                self.raw += chunk

    def text(self, start=0):
        return ANSI.sub("", self.raw[start:].decode("utf-8", "replace"))

    def wait_for(self, needle, secs=10, start=0):
        end = time.time() + secs
        while time.time() < end and needle not in self.text(start):
            self.pump(0.1)
        return needle in self.text(start)

    def type(self, s):
        os.write(self.fd, s.encode())
        self.pump(0.3)
        os.write(self.fd, b"\r")

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except OSError:
            pass


app = App()
try:
    check("composer", app.wait_for("╰", 15), app.text())
    app.pump(0.5)
    mark = len(app.raw)
    app.type("/switch mock/mock-2")
    check("switch", app.wait_for("Session-only model: mock/mock-2", 10, mark), app.text(mark))
    mark = len(app.raw)
    app.type("/plan")
    check("plan on", app.wait_for("Plan mode enabled", 10, mark), app.text(mark))
    mark = len(app.raw)
    app.type("say:planned hello")
    check("turn", app.wait_for("planned hello", 15, mark), app.text(mark))
    app.pump(1.0)
finally:
    app.kill()
modes = [e for e in entries() if e.get("type") == "mode_change" and e.get("mode") == "plan"]
data = modes[-1].get("data", {}) if modes else {}
check("journal", data.get("prePlanModelIdentity") == "mock/mock-2" and "prePlanThinkingLevel" in data, data)
models = [e.get("model") for e in entries() if e.get("type") == "model_change"]
check("plan model", models and models[-1] == "mock/mock-think", models)

app = App("-c")
try:
    check("resumed in plan", app.wait_for("Plan mode enabled", 15), app.text())
    app.pump(0.8)
    mark = len(app.raw)
    app.type("/plan")
    check("plan off", app.wait_for("Plan mode paused", 10, mark), app.text(mark))
    app.pump(0.8)
finally:
    app.kill()
models = [(e.get("model"), e.get("role")) for e in entries() if e.get("type") == "model_change"]
check("restored", models and models[-1] == ("mock/mock-2", "temporary"), models)
