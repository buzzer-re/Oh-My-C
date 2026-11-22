#!/usr/bin/env python3
"""The ask tool's HTML form in the interactive TUI under a pty (omp html-form/dialog.ts).

usage: ask_form_tui.py <omc> <req.jsonl> <scratch dir>
Scenarios, each a fresh `omc --model mock/mock-1 --no-session call:ask:{html}`:
  submit   ↓ Enter picks Blue, End/Backspace edits the prefilled number, Tab reaches Submit, Enter submits
  invalid  Submit with a required field empty shows "1 to fix" and the field's error, then typing fixes it
  other    typing into a radio group's Other answer replaces the pick
  timeout  ask.timeout 2: the countdown title, then the defaults are submitted as a timeout
  cancel   Esc cancels the form and ends the run
Prints "ask form tui ok" or the first failing step."""
import json, os, pty, re, select, sys, time
NOPRED = os.path.join(os.path.dirname(os.path.abspath(__file__)), "no-predictions.yml")  # composer.predictions off

omc, reqlog, scratch = sys.argv[1:4]
ANSI = re.compile(r"\x1b\[[0-9;?<>=]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[PX^_][^\x1b]*\x1b\\|\x1b.")
FORM = ('<form><p>Paint it</p><fieldset><legend>Pick a color</legend>'
        '<label><input type="radio" name="color" value="red" checked> Red</label>'
        '<label><input type="radio" name="color" value="blue"> Blue</label></fieldset>'
        '<label>Coats <input type="number" name="n" value="2" min="1" required></label></form>')
REQUIRED = '<form><label>Your name <input name="who" required placeholder="first last"></label></form>'


def tool_results():
    res = []
    try:
        with open(reqlog) as f:
            for line in f:
                for m in json.loads(line).get("messages", []):
                    if m.get("role") == "tool":
                        c = m.get("content")
                        res.append(c if isinstance(c, str) else json.dumps(c))
    except OSError:
        pass
    return res


def run(name, html, keys, expect_tool=None, expect_screen=None, agent=None, before=None):
    n0 = len(tool_results())
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", OMC_SKIP_SETUP="1", PI_NO_DESKTOP_NOTIFY="1")
        for k in ("TMUX", "STY", "ZELLIJ", "TERM_PROGRAM"):
            env.pop(k, None)
        if agent:
            env["OMC_CODING_AGENT_DIR"] = agent
        os.execvpe(omc, [omc, "--config", NOPRED, "--model", "mock/mock-1", "--no-session", "call:ask:" + json.dumps({"html": html})], env)
    raw = b""

    def pump(until, timeout):
        nonlocal raw
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
            if until(ANSI.sub("", raw.decode("utf-8", "replace"))):
                return True
        return False

    def fail(step):
        print(f"{name}: {step}:", ANSI.sub("", raw.decode("utf-8", "replace"))[-1200:])
        try:
            os.kill(pid, 9)
        except OSError:
            pass
        sys.exit(1)

    try:
        if not pump(lambda s: "Submit" in s, 20):
            fail("no form")
        time.sleep(0.5)
        if before and not pump(lambda s: before in s, 10):
            fail("before " + before)
        for k in keys:
            if isinstance(k, tuple):  # (wait-for-text,)
                if not pump(lambda s, t=k[0]: t in s, 10):
                    fail("waiting for " + k[0])
                continue
            os.write(fd, k.encode())
            time.sleep(0.25)
        if expect_tool is not None:
            end = time.time() + 20
            got = None
            while time.time() < end and got is None:
                pump(lambda s: False, 0.2)
                new = tool_results()[n0:]
                if new:
                    got = new[0]
            if got is None or expect_tool not in got:
                fail(f"tool result {got!r}, want {expect_tool!r}")
        if expect_screen is not None and not pump(lambda s: expect_screen in s, 15):
            fail("screen " + expect_screen)
    finally:
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


DOWN, ENTER, TAB, END, BS, ESC = "\x1b[B", "\r", "\t", "\x05", "\x7f", "\x1b"
run("submit", FORM, [DOWN, ENTER, END, BS, "5", TAB, ENTER],
    expect_tool='User answers:\n{\n  "color": "blue",\n  "n": 5\n}')
run("invalid", REQUIRED, [TAB, ENTER, ("1 to fix",), ("Required",), "Ada", TAB, ENTER],
    expect_tool='User answers:\n{\n  "who": "Ada"\n}')
run("other", FORM, [DOWN, DOWN, "teal", TAB, TAB, ENTER],
    expect_tool='User answers:\n{\n  "color": "teal",\n  "n": 2\n}')
agent = os.environ.get("OMC_CODING_AGENT_DIR", "")
tagent = os.path.join(scratch, "agent-timeout")
os.makedirs(tagent, exist_ok=True)
with open(os.path.join(agent, "models.yml")) as src, open(os.path.join(tagent, "models.yml"), "w") as dst:
    dst.write(src.read())
with open(os.path.join(tagent, "config.yml"), "w") as f:
    f.write("ask:\n  timeout: 2\n")
run("timeout", FORM, [("Ask (2s)",)], agent=tagent,
    expect_tool="No answer before the timeout; submitted the form's current values:\n{\n  \"color\": \"red\",\n  \"n\": 2\n}")
run("cancel", FORM, [ESC], expect_screen="Ask tool was cancelled by the user")
print("ask form tui ok")
