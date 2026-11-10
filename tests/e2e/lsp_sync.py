#!/usr/bin/env python3
"""e2e (omp 452b2e0d, bdf8c984): a query that can draw on other open documents first syncs those edited outside omc; an unreadable
open document is closed for a read-only query and fails an action that writes edits; documents unused for the idle window close before the next
open. Uses the fake server tests/e2e/bin-lsp/rust-analyzer (FAKE_LSP_LOG).
Usage: lsp_sync.py <omc> <mock request log> <models.yml> <dir>"""
import json, os, shutil, subprocess, sys

omc, reqlog, models, root = sys.argv[1:5]
here = os.path.dirname(os.path.abspath(__file__))
work, agent, log = os.path.join(root, "work"), os.path.join(root, "agent"), os.path.join(root, "lsp.log")
os.makedirs(os.path.join(work, "src"), exist_ok=True)
os.makedirs(agent, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
with open(os.path.join(work, "Cargo.toml"), "w") as f: f.write('[package]\nname = "a"\nversion = "0.1.0"\n')
for n in "abc":
    with open(os.path.join(work, "src", n + ".rs"), "w") as f: f.write(f"pub fn {n}() {{}}\n")
with open(os.path.join(agent, "config.yml"), "w") as f: f.write("lsp:\n  enabled: true\n")
env = dict(os.environ, HOME=root, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", OMC_NO_LOCAL_DISCOVERY="1",
           FAKE_LSP_LOG=log, PATH=os.path.join(here, "bin-lsp") + os.pathsep + os.environ["PATH"])
fails = 0


def check(name, ok, got=""):
    global fails
    if not ok:
        fails += 1
        print(f"FAIL {name}: {str(got)[-600:]!r}")


def lsp(**args): return ["write", {"path": "xd://lsp", "content": json.dumps(args)}]


def run(script, extra=None):
    open(reqlog, "w").close()
    if os.path.exists(log): os.unlink(log)
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "seq:" + json.dumps(script)], cwd=work, env=dict(env, **(extra or {})),
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=28)
    msgs = []
    for line in open(reqlog):
        m = [x for x in json.loads(line).get("messages", []) if x.get("role") == "tool"]
        if len(m) >= len(msgs): msgs = m
    res = ["".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"] for m in msgs]
    return res, [json.loads(l) for l in open(log)] if os.path.exists(log) else []


def methods(rows, name, file):
    return [r for r in rows if r.get("method") == name and json.dumps(r.get("params")).find(file) >= 0]


# edited outside omc: a query about b.rs pushes a.rs's disk text first, and a further sweep sends nothing new
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), lsp(action="code_actions", file="src/b.rs", line=1),
                 ["bash", {"command": "echo 'pub fn a2() {}' > src/a.rs"}], lsp(action="code_actions", file="src/b.rs", line=1)])
ch = methods(rows, "textDocument/didChange", "a.rs")
check("sync outside edit", len(ch) == 1 and "a2" in json.dumps(ch[0]["params"]), rows)
names = [r.get("method") for r in rows]
check("sync before the request", names.index("textDocument/didChange") < len(names) - 1 - names[::-1].index("textDocument/codeAction"), names)
with open(os.path.join(work, "src", "a.rs"), "w") as f: f.write("pub fn a() {}\n")

# deleted and unreadable documents: a read-only query closes them and answers
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), ["bash", {"command": "rm src/a.rs; mkdir src/a.rs"}],
                 lsp(action="code_actions", file="src/b.rs", line=1)])
check("unreadable closed for a query", len(methods(rows, "textDocument/didClose", "a.rs")) == 1 and "EISDIR" not in res[-1], res)
os.rmdir(os.path.join(work, "src", "a.rs"))
with open(os.path.join(work, "src", "a.rs"), "w") as f: f.write("pub fn a() {}\n")
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), ["bash", {"command": "rm src/a.rs; mkdir src/a.rs"}],
                 lsp(action="code_actions", file="src/b.rs", line=1, apply=True, query="fix")])
check("unreadable fails an applied code action", "EISDIR" in res[-1] and not methods(rows, "textDocument/codeAction", "b.rs"), res)
os.rmdir(os.path.join(work, "src", "a.rs"))
with open(os.path.join(work, "src", "a.rs"), "w") as f: f.write("pub fn a() {}\n")

# idle window: the next open closes what no operation used, and a reopen goes on with the next version
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), ["bash", {"command": "sleep 0.3"}],
                 lsp(action="code_actions", file="src/b.rs", line=1), ["bash", {"command": "sleep 0.3"}], lsp(action="code_actions", file="src/a.rs", line=1)],
                {"OMC_LSP_FILE_IDLE_MS": "200"})
check("idle closed", len(methods(rows, "textDocument/didClose", "a.rs")) == 1, rows)
op = methods(rows, "textDocument/didOpen", "a.rs")
check("reopen versions", [o["params"]["textDocument"]["version"] for o in op] == [1, 2], op)
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), lsp(action="code_actions", file="src/b.rs", line=1)])
check("not idle by default", not methods(rows, "textDocument/didClose", "a.rs"), rows)

# code actions: the unfiltered set plus the source kinds a server lists only on request, merged without repeats (omp
# da8463bb58, f27d11cfc6); a document symbol's line is its identifier's (omp 55041b7e07)
res, rows = run([lsp(action="code_actions", file="src/a.rs", line=1), lsp(action="symbols", file="src/a.rs")], {"FAKE_LSP_ACTIONS": "1"})
check("source actions listed", "0: [quickfix] Fix" in res[0] and "1: [source.organizeImports] Organize Imports" in res[0]
      and "2:" not in res[0], res)
check("symbol identifier line", "a @ line 3" in res[1], res)
print("lsp sync ok" if not fails else "lsp sync failed")
sys.exit(1 if fails else 0)
