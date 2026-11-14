#!/usr/bin/env python3
"""e2e: what one bash tool command does to its session ends with it (omp 18.17): `exec` redirections, an EXIT trap, late output
of a background job, a syntax error. State the session keeps still persists. Silent success says so. RPC `bash` results report
middle elision and column truncation.
Usage: shell_scope.py <omc> <request log> <models.yml> <scratch dir>"""
import json, os, shutil, subprocess, sys, threading

omc, reqlog, models, scratch = sys.argv[1:5]
agent, work, home = (os.path.join(scratch, d) for d in ("agent", "work", "home"))
for d in (agent, work, home): os.makedirs(d, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
env = dict(os.environ, OMC_CODING_AGENT_DIR=agent, HOME=home)
fails = 0


def check(name, ok, got=""):
    global fails
    if not ok:
        fails += 1
        print(f"FAIL {name}: {got!r}")


cmds = ["exec >out.txt; echo hidden", "echo visible",
        "exec 2>/dev/null; ls /nonexistent-zz", "ls /nonexistent-zz 2>&1 | wc -l",
        "trap 'echo FIRED' EXIT; echo set", "echo later",
        "(sleep 0.5; echo late; touch latemark) & echo started", "sleep 1; echo after; ls latemark",
        "echo \"abc", "if", "foo", "echo a\nbar",
        "export KEEP=yes; f() { echo fn; }; alias al='echo aliased'; cd /", "echo $KEEP; f; al; pwd",
        "true", "false", "echo x > /dev/stdout; echo y > /dev/stderr; echo $0"]
def bash_parse_error(cmd):
    """the installed bash's own parse error for `bash -c cmd` (bash 5.1 and 5.3 word these differently; omc passes
    real bash's text through), as the tool result shows it"""
    err = subprocess.run(["bash", "-c", cmd], capture_output=True, text=True).stderr
    return err + "\n\nCommand exited with code 2"


open(reqlog, "w").close()
subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "seq:" + json.dumps([["bash", {"command": c}] for c in cmds])],
               cwd=work, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=28)
msgs = []
for line in open(reqlog):
    m = [x for x in json.loads(line).get("messages", []) if x.get("role") == "tool"]
    if len(m) >= len(msgs): msgs = m
res = ["".join(p.get("text", "") for p in m["content"]) if not isinstance(m["content"], str) else m["content"] for m in msgs]
want = ["(no output, exit code 0)", "visible\n",
        "(no output)\n\nCommand exited with code 2", "1\n",
        "set\nFIRED\n", "later\n",
        "started\n", None,
        bash_parse_error("echo \"abc"),
        bash_parse_error("if"),
        "bash: line 1: foo: command not found\n\n\nCommand exited with code 127",
        "a\nbash: line 2: bar: command not found\n\n\nCommand exited with code 127",
        "(no output, exit code 0)", "yes\nfn\naliased\n" + os.path.realpath(work) + "\n",
        "(no output, exit code 0)", "(no output)\n\nCommand exited with code 1", "x\ny\nbash\n"]
check("all ran", len(res) == len(want), res)
for i, (c, r, w) in enumerate(zip(cmds, res, want)):
    if w is not None: check(c, r == w, r)
if len(res) > 7:
    check("late output dropped, job alive", res[7].startswith("after\nlatemark\n") and "late\n" not in res[7], res[7])
check("exec file", open(os.path.join(work, "out.txt")).read() == "hidden\n")

# RPC bash: BashResult carries what was elided and cut
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--no-session", "--mode", "rpc"], cwd=work, env=env, text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p.stdin.write('{"id":"b1","type":"bash","command":"seq 1 30000"}\n{"id":"b2","type":"bash","command":"python3 -c \\"print(\'x\'*2000); print(\'y\')\\""}\n')
p.stdin.flush()
timer = threading.Timer(20, p.kill)
timer.start()
data = {}
for line in p.stdout:
    f = json.loads(line) if line.startswith("{") else {}
    if f.get("type") == "response" and f.get("command") == "bash":
        data[f["id"]] = f.get("data", {})
        if len(data) == 2: break
p.stdin.close()
p.wait(timeout=15)
timer.cancel()
b1, b2 = data.get("b1", {}), data.get("b2", {})
check("rpc elided", b1.get("elidedLines") == 20564 and b1.get("elidedBytes", 0) > 0 and "columnMax" not in b1, {k: v for k, v in b1.items() if k != "output"})
check("rpc column", b2.get("columnMax") == 768 and b2.get("columnTruncatedLines") == 1 and b2.get("columnDroppedBytes", 0) > 0 and
      b2.get("columnTruncatedRange") == {"first": 1, "last": 1} and "elidedBytes" not in b2, {k: v for k, v in b2.items() if k != "output"})
print("shell scope ok" if not fails else "shell scope failed")
sys.exit(1 if fails else 0)
