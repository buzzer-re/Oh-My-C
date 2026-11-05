#!/usr/bin/env python3
"""e2e: `lsp request` with payload "null" sends JSON null; `lsp reload` asks rust-analyzer to reload its workspace while the
crate set is unchanged and restarts it when a new Cargo.toml appeared (reloadWorkspace only refreshes the projects found at
initialize). Uses the fake server tests/e2e/bin-lsp/rust-analyzer.
Usage: lsp_reload.py <omc> <mock request log> <models.yml> <dir>
Prints "lsp reload ok" or the failing steps."""
import json, os, shutil, subprocess, sys, time

omc, reqlog, models, root = sys.argv[1:5]
here = os.path.dirname(os.path.abspath(__file__))
work = os.path.join(root, "work")
agent = os.path.join(root, "agent")
log = os.path.join(root, "lsp.log")
os.makedirs(os.path.join(work, "src"), exist_ok=True)
os.makedirs(agent, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
with open(os.path.join(work, "Cargo.toml"), "w") as f:
    f.write('[package]\nname = "a"\nversion = "0.1.0"\n')
with open(os.path.join(work, "src", "lib.rs"), "w") as f:
    f.write("pub fn a() {}\n")
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("lsp:\n  enabled: true\n")
env = dict(os.environ, HOME=root, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", OMC_NO_LOCAL_DISCOVERY="1", FAKE_LSP_LOG=log,
           PATH=os.path.join(here, "bin-lsp") + os.pathsep + os.environ["PATH"])
fails = []


def check(name, cond, detail=""):
    if not cond:
        fails.append(name)
        print("FAIL", name, str(detail)[-900:])


def run(script):
    open(log, "w").close()
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "calls:" + json.dumps(script)], cwd=work, env=env,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=90)
    msgs = []
    for line in open(reqlog):
        msgs = [m for m in json.loads(line).get("messages", []) if m.get("role") == "tool"] or msgs
    texts = ["".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"] for m in msgs]
    return texts, [json.loads(line) for line in open(log)]


lsp = "xd://lsp"


def call(args):
    return ["write", {"path": lsp, "content": json.dumps(args)}]


texts, events = run([call({"action": "request", "file": "src/lib.rs", "query": "rust-analyzer/x", "payload": "null"})])
ev = [e for e in events if e.get("method") == "rust-analyzer/x"]
check("payload null sent", ev and ev[0]["hasParams"] and ev[0]["params"] is None, events)

reload_call = call({"action": "reload", "file": "src/lib.rs"})
new_crate = ["bash", {"command": "mkdir -p sub && printf '[package]\\nname=\"s\"\\nversion=\"0.1.0\"\\n' > sub/Cargo.toml"}]
texts, events = run([reload_call])
check("same crates reload", any("Reloaded rust-analyzer" in t for t in texts), texts)
check("reloadWorkspace sent", any(e.get("method") == "rust-analyzer/reloadWorkspace" for e in events), events)
texts, events = run([reload_call, new_crate, reload_call])
check("new crate restarts", texts and "Restarted rust-analyzer" in texts[-1] and "Reloaded rust-analyzer" in texts[0], texts)
check("no reloadWorkspace on restart", sum(e.get("method") == "rust-analyzer/reloadWorkspace" for e in events) == 1, events)
print("lsp reload ok" if not fails else "lsp reload FAILED: " + ", ".join(fails))
sys.exit(1 if fails else 0)
