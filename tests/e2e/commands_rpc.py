#!/usr/bin/env python3
"""RPC checks for non-builtin slash commands (WP-9): file commands, prompt templates, `/skill:<name>` and MCP
prompt commands are advertised by get_available_commands and expand before the model sees the prompt.
  commands_rpc.py <omc> <mock request log> <mock_mcp.py>
Runs in a throwaway project dir (cwd) seeded here; OMC_CODING_AGENT_DIR comes from the caller. Prints one line
per check; exits non-zero on the first failure."""
import json, os, subprocess, sys, tempfile, threading, time

omc, log, mock_mcp = sys.argv[1], sys.argv[2], sys.argv[3]
proj = tempfile.mkdtemp(prefix="omc-cmds-", dir=os.environ.get("TMPDIR"))
agent = os.environ["OMC_CODING_AGENT_DIR"]


def put(rel, text):
    path = os.path.join(proj, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


put(".omp/commands/hello.md", "---\ndescription: Say hello\nargument-hint: <name>\n---\nHi $1, all: $ARGUMENTS\n")
put(".omp/prompts/tpl.md", "Template for $1\n")
put(".omp/skills/demo/SKILL.md", "---\nname: demo\ndescription: Demo skill\n---\n# Demo\nDo the demo.\n")
mcp_json = os.path.join(agent, "mcp.json")
with open(mcp_json, "w") as f:
    json.dump({"mcpServers": {"mockmcp": {"command": "python3", "args": ["-I", mock_mcp]}}}, f)

p = subprocess.Popen([omc, "-m", "mock/mock-1", "--mode", "rpc"], cwd=proj, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, text=True)
frames = []
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        with lock:
            frames.append(m)


threading.Thread(target=reader, daemon=True).start()


def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()


def wait(pred, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for f in frames:
                if pred(f):
                    return f
        time.sleep(0.05)
    return None


def cleanup():
    p.kill()
    os.remove(mcp_json)
    subprocess.run(["rm", "-rf", proj])


def fail(msg):
    print("FAIL", msg)
    cleanup()
    sys.exit(1)


def last_user_text():
    with open(log) as f:
        reqs = [json.loads(l) for l in f if l.strip()]
    reqs = [r for r in reqs if r.get("tools")]
    msgs = reqs[-1]["messages"] if reqs else []
    users = [m for m in msgs if m.get("role") == "user"]
    c = users[-1]["content"] if users else ""
    return c if isinstance(c, str) else "\n".join(b.get("text", "") for b in c if b.get("type") == "text")


send({"id": "c", "type": "get_available_commands"})
r = wait(lambda f: f.get("type") == "response" and f.get("id") == "c")
if not r or not r.get("success"):
    fail("get_available_commands")
cmds = {c["name"]: c for c in r["data"]["commands"]}
want = {
    "skill:demo": {"name": "skill:demo", "description": "Demo skill", "input": {"hint": "arguments"}, "source": "skill"},
    "hello": {"name": "hello", "description": "Say hello", "input": {"hint": "<name>"}, "source": "file"},
    "init": {"name": "init", "description": "Generate AGENTS.md for current codebase", "source": "file"},
    "mockmcp:greet": {"name": "mockmcp:greet", "description": "Greet someone politely", "input": {"hint": "arguments"},
                      "source": "mcp_prompt"},
    "mockmcp:empty": {"name": "mockmcp:empty", "description": "MCP prompt from mockmcp", "input": {"hint": "arguments"},
                      "source": "mcp_prompt"},
}
for name, w in want.items():
    if cmds.get(name) != w:
        fail("advertised %s: %s" % (name, json.dumps(cmds.get(name))))
names = [c["name"] for c in r["data"]["commands"]]
if not names.index("skill:demo") < names.index("mockmcp:greet") < names.index("hello") < names.index("init"):
    fail("order " + json.dumps(names))
print("ok available commands")

runs = [
    ("/hello Bob extra", "Hi Bob, all: Bob extra"),
    ("/tpl x", "Template for x"),
    ("/skill:demo go now", '[IMPORTANT: User invoked the "demo" skill; follow its instructions. Full skill below.]\n\n# Demo\nDo the demo.'),
    ("please /skill:demo quickly", "User: please quickly"),
    ("/mockmcp:greet name=Ann", "MCP prompt says hello to Ann\n\nresource part"),
]
for i, (text, expect) in enumerate(runs):
    pid = "p%d" % i
    send({"id": pid, "type": "prompt", "message": text})
    if not wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == pid):
        fail("%s: no prompt_result" % text)
    got = last_user_text()
    if expect not in got:
        fail("%s: model saw %r" % (text, got[:300]))
    print("ok", text)

send({"id": "e", "type": "prompt", "message": "/mockmcp:empty"})
r = wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "e")
if not r or r.get("agentInvoked") is not False:
    fail("/mockmcp:empty: %s" % json.dumps(r))
print("ok empty MCP prompt consumed")
p.stdin.close()
p.wait(timeout=10)
os.remove(mcp_json)
subprocess.run(["rm", "-rf", proj])
