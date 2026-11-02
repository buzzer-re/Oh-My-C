#!/usr/bin/env python3
"""RPC checks for /mcp and /ssh (WP-18, text-mode handlers ported from helpers/mcp.ts and helpers/ssh.ts):
list / add / enable / disable / remove on mcp.json, test / resources / prompts over one-off connections, the
TUI-only verbs, ssh.json add / list / remove, and the MCP prompt catalog refreshing on
notifications/prompts/list_changed (get_available_commands picks up the new prompt).
  mcp_rpc.py <omc> <mock_mcp.py>
Runs in a throwaway project dir; OMC_CODING_AGENT_DIR comes from the caller (its mcp.json is written and
removed here). Prints one line per check; exits non-zero on the first failure."""
import json, os, subprocess, sys, tempfile, threading, time

omc, mock_mcp = sys.argv[1], sys.argv[2]
proj = tempfile.mkdtemp(prefix="omc-mcprpc-", dir=os.environ.get("TMPDIR"))
agent = os.environ["OMC_CODING_AGENT_DIR"]
mcp_json = os.path.join(agent, "mcp.json")
with open(mcp_json, "w") as f:
    json.dump({"mcpServers": {"mockmcp": {"command": "python3", "args": ["-I", mock_mcp, "--full"]}}}, f)

p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], cwd=proj, stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
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


def wait(pred, timeout=20):
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
    p.wait()
    if os.path.exists(mcp_json):
        os.remove(mcp_json)
    subprocess.run(["rm", "-rf", proj])


def fail(msg):
    print("FAIL", msg)
    cleanup()
    sys.exit(1)


seq = [0]


def slash(text, want):
    seq[0] += 1
    pid = "s%d" % seq[0]
    with lock:
        n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    if not r or not r.get("success") or (r.get("data") or {}).get("agentInvoked") is not False:
        fail("%s: response %s" % (text, json.dumps(r)))
    out = wait(lambda f: f.get("type") == "command_output" and want in f.get("text", "") and frames.index(f) >= n0)
    if not out:
        with lock:
            got = [f.get("text") for f in frames[n0:] if f.get("type") == "command_output"]
        fail("%s: no command_output containing %r (got %s)" % (text, want, got))
    print("ok", text)


slash("/mcp", "MCP server management (ACP mode)")
slash("/mcp list", "mockmcp | stdio | enabled | python3 [user]")
slash("/mcp add web --url example.com/x?key=secret --transport sse", 'Added MCP server "web" (project).')
cfg = json.load(open(os.path.join(proj, ".omc", "mcp.json")))
keys = list(cfg.keys())
if keys[0] != "$schema" or cfg["mcpServers"]["web"] != {"type": "sse", "url": "https://example.com/x?key=secret"}:
    fail("project mcp.json: " + json.dumps(cfg))
print("ok project mcp.json written")
slash("/mcp list", "web | sse | enabled | https://example.com/x [project]")
slash("/mcp add web --url http://a", 'Failed to add server: Server "web" already exists in')
slash("/mcp add stdio -- echo hi there", 'Added MCP server "stdio" (project).')
slash("/mcp disable web", 'Server "web" disabled (project config).')
slash("/mcp list", "web | sse | disabled |")
slash("/mcp enable web", 'Server "web" enabled (project config).')
slash("/mcp disable ghost", 'Server "ghost" disabled.')
slash("/mcp enable nope", 'Server "nope" not found in user or project config.')
slash("/mcp remove web", 'Removed server "web" from project config.')
slash("/mcp rm stdio --scope project", 'Removed server "stdio" from project config.')
slash("/mcp remove web", 'Failed to remove MCP server: Server "web" not found in')
slash("/mcp add bad name -- x", "Unknown option: name")
slash("/mcp add only", "Provide --url or -- <command...> for non-interactive add.")
slash("/mcp test mockmcp", 'Server "mockmcp" connected (2 tools).\n  - echo_text\n  - change_prompts')
slash("/mcp test nope", 'Server "nope" not found. Run /mcp list to see configured servers.')
slash("/mcp resources", "mockmcp/mem://notes")
slash("/mcp prompts", "mockmcp/greet — Greet someone politely\nmockmcp/empty")
slash("/mcp notifications", "MCP notifications require the TUI client (live MCPManager).")
slash("/mcp reconnect mockmcp", "/mcp reconnect requires OAuth or browser flows only available in the TUI client.")
slash("/mcp reauth mockmcp", "stdio servers manage their own credentials, so OMP has no OAuth to reauthorize.")
slash("/mcp bogus", "Unknown /mcp subcommand: bogus. Use /mcp help for available subcommands.")

slash("/ssh", "SSH host management (ACP mode)")
slash("/ssh list", "No SSH hosts configured.")
slash("/ssh add box --host 10.0.0.5 --user me --port 2222", 'Added SSH host "box" (project).')
hosts = json.load(open(os.path.join(proj, ".omc", "ssh.json")))
if hosts != {"hosts": {"box": {"host": "10.0.0.5", "username": "me", "port": 2222}}}:
    fail("ssh.json: " + json.dumps(hosts))
print("ok ssh.json written")
slash("/ssh list", "box | 10.0.0.5 | me | 2222 [project]")
slash("/ssh add box --host x", 'Failed to add SSH host: Host "box" already exists in')
slash("/ssh add x --port 99999 --host h", "Invalid --port value. Must be an integer between 1 and 65535.")
slash("/ssh remove box", 'Removed SSH host "box" from project config.')
slash("/ssh nope", "Unknown /ssh subcommand: nope. Use /ssh help for available subcommands.")

# notifications/prompts/list_changed refreshes the prompt catalog behind the advertised commands
send({"id": "t", "type": "prompt", "message": 'call:mcp__mockmcp_change_prompts:{}'})
if not wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "t", 30):
    fail("change_prompts turn did not finish")
found = False
for attempt in range(40):
    send({"id": "c%d" % attempt, "type": "get_available_commands"})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == "c%d" % attempt)
    if r and any(c.get("name") == "mockmcp:fresh" for c in r["data"]["commands"]):
        found = True
        break
    time.sleep(0.1)
if not found:
    fail("mockmcp:fresh not advertised after prompts/list_changed")
print("ok prompts/list_changed refreshes MCP prompt commands")

p.stdin.close()
p.wait(timeout=10)
os.remove(mcp_json)
subprocess.run(["rm", "-rf", proj])
print("ok mcp rpc done")
