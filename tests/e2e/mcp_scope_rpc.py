#!/usr/bin/env python3
"""MCP tools a session's call loads stay scoped to that session (omp mcp-session-tool-views.test.ts): a subagent calling
a lazy loader changes neither the main session's tools nor its system prompt; the main session's own call to the server
brings the tool in; a change the server announces while no call runs reaches every session.
Usage: mcp_scope_rpc.py <omc> <mock request log> <mock_mcp.py> <scratch dir>
Prints "mcp scope ok" or the first failing step."""
import json, os, subprocess, sys, threading, time

omc, log, mock, scratch = sys.argv[1:5]
work = os.path.join(scratch, "work")
os.makedirs(work, exist_ok=True)
EXTRA = "mcp__lazy_extra_tool"


def fail(step, detail=""):
    print(f"mcp scope FAIL {step}: {detail!r}")
    sys.exit(1)


with open(os.path.join(work, ".mcp.json"), "w") as f:
    json.dump({"mcpServers": {"lazy": {"command": "python3", "args": ["-I", mock, "--full", "--lazy"]}}}, f)
env = dict(os.environ, HOME=scratch, OMC_NO_TITLE="1", TMPDIR=scratch)


def requests():
    if not os.path.exists(log):
        return []
    with open(log) as f:
        return [json.loads(line) for line in f if line.strip()]


def mains(reqs):
    return [r for r in reqs if r.get("tools")]


class Session:
    def __init__(self):
        self.p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc", "--no-session"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=env, cwd=work)
        self.frames = []
        self.lock = threading.Lock()
        threading.Thread(target=self.read, daemon=True).start()
        self.n = 0

    def read(self):
        for line in self.p.stdout:
            try:
                with self.lock:
                    self.frames.append(json.loads(line))
            except ValueError:
                pass

    def prompt(self, text, settled=False):
        self.n += 1
        rid = f"p{self.n}"
        self.p.stdin.write(json.dumps({"id": rid, "type": "prompt", "message": text}) + "\n")
        self.p.stdin.flush()
        end = time.time() + 30
        while time.time() < end:
            with self.lock:
                if any(f.get("type") == "prompt_result" and f.get("id") == rid for f in self.frames):
                    return
            time.sleep(0.05)
        fail("prompt timed out", text)

    def close(self):
        self.p.stdin.close()
        try:
            self.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def has_extra(r):
    return EXTRA in json.dumps(r.get("tools")) or EXTRA in json.dumps(r["messages"])


s = Session()
time.sleep(1.0)  # the server connects and its tools mount
# the subagent's loader call: its own next request sees the tool, the main session's do not
before = len(requests())
s.prompt("call:task:" + json.dumps({"context": "scope", "tasks": [{"task": "call:mcp__lazy_load_extra:{}"}]}))
end = time.time() + 20
while time.time() < end and "Background job" not in json.dumps(requests()[-1:]):
    time.sleep(0.1)
time.sleep(1.0)
later = requests()[before:]
children = [r for r in mains(later) if "Complete assignment thoroughly" in json.dumps(r["messages"])]
parents = [r for r in mains(later) if "Complete assignment thoroughly" not in json.dumps(r["messages"])]
if len(children) < 2:
    fail("subagent requests", len(children))
if not has_extra(children[-1]):
    fail("the subagent's next request lacks the tool it loaded", json.dumps(children[-1]["messages"])[-300:])
if not parents or any(has_extra(r) for r in parents):
    fail("the subagent's load reached the main session", [has_extra(r) for r in parents])
before = len(requests())
s.prompt("say:main-after-subagent-load")
later = mains(requests()[before:])
if not later or has_extra(later[-1]):
    fail("main request after the subagent's load", len(later))
if later[-1]["messages"][0] != parents[0]["messages"][0]:
    fail("main system prompt changed", "")
# the main session's own call to the server brings the tool in for its next request
before = len(requests())
s.prompt("call:mcp__lazy_load_extra:{}")
later = mains(requests()[before:])
if len(later) < 2 or has_extra(later[0]) or not has_extra(later[-1]):
    fail("main session's own load", [has_extra(r) for r in later])
s.close()

# a change announced while no call runs reaches every session
s = Session()
time.sleep(1.0)
before = len(requests())
s.prompt("call:mcp__lazy_load_extra_late:{}")
time.sleep(1.5)
s.prompt("call:task:" + json.dumps({"context": "late", "tasks": [{"task": "say:after-late-load"}]}))
end = time.time() + 15
children = []
while time.time() < end and not children:
    children = [r for r in mains(requests()[before:]) if "Complete assignment thoroughly" in json.dumps(r["messages"])]
    time.sleep(0.1)
if not children or not has_extra(children[-1]):
    fail("a change announced outside a call", len(children))
s.close()
print("mcp scope ok")
