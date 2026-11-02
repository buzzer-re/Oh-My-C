#!/usr/bin/env python3
"""Workspace and session lifecycle over RPC (WP-21): --add-dir, /dirs, /add-dir, /remove-dir (the system prompt's
<workspace-roots> section and the extra root's context files), /pin, /fresh, /move, /wt (worktree under
$OMC_WORKTREE_DIR with the uncommitted changes carried over). Usage: workspace_rpc.py <omc> <request log>.
Runs in throwaway directories under $TMPDIR; prints one line per check, exits non-zero on the first failure."""
import json, os, shutil, subprocess, sys, tempfile, threading, time

omc, reqlog = sys.argv[1], sys.argv[2]
tmp = os.path.realpath(tempfile.mkdtemp(prefix="omc-wp21-"))
work, extra, other, wtdir = (os.path.join(tmp, d) for d in ("work", "extra", "other", "wt"))
for d in (work, extra, other):
    os.makedirs(d)
with open(os.path.join(extra, "AGENTS.md"), "w") as f:
    f.write("Extra root rules: EXTRA-ROOT-MARK\n")
git_env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
for cmd in (["git", "init", "-q", "-b", "main"], ["git", "add", "-A"], ["git", "commit", "-q", "--allow-empty", "-m", "init"]):
    subprocess.run(cmd, cwd=work, env=git_env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
with open(os.path.join(work, "dirty.txt"), "w") as f:
    f.write("uncommitted\n")

env = dict(git_env, OMC_WORKTREE_DIR=wtdir)
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc", "--add-dir", "../extra"], cwd=work, env=env,
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
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


def fail(msg):
    print("FAIL", msg)
    p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1)


seq = [0]


def command(text, want):
    seq[0] += 1
    pid = "w%d" % seq[0]
    with lock:
        n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    if not r or not r.get("success"):
        fail("%s: response %s" % (text, json.dumps(r)))
    out = None
    end = time.time() + 15
    while out is None and time.time() < end:  # frames after n0 (identical texts may repeat earlier)
        with lock:
            out = next((f for f in frames[n0:] if f.get("type") == "command_output"), None)
        time.sleep(0.05)
    got = out.get("text", "") if out else None
    if got is None or want not in got:
        fail("%s: command_output %r does not contain %r" % (text, got, want))
    print("ok", text)
    time.sleep(0.2)  # the RPC prompt thread settles before the next prompt
    return got


def state():
    seq[0] += 1
    pid = "s%d" % seq[0]
    send({"id": pid, "type": "get_state"})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    return (r or {}).get("data") or {}


def run_turn(text):
    seq[0] += 1
    pid = "t%d" % seq[0]
    send({"id": pid, "type": "prompt", "message": text})
    if not wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == pid, 30) and \
            not wait(lambda f: f.get("type") == "agent_end", 30):
        fail("turn did not finish: " + text)
    time.sleep(0.3)


def last_system_prompt():
    with open(reqlog) as f:
        reqs = [json.loads(l) for l in f if l.strip()]
    reqs = [r for r in reqs if r.get("tools")]
    msgs = reqs[-1]["messages"] if reqs else []
    return "\n".join(m["content"] if isinstance(m["content"], str) else "".join(c.get("text", "") for c in m["content"])
                     for m in msgs if m.get("role") in ("system", "developer"))


command("/dirs", "Workspace directories:\n  %s (working directory)\n  %s" % (work, extra))
run_turn("bash:echo roots")
sp = last_system_prompt()
if "<workspace-roots>" not in sp or ("- " + extra) not in sp:
    fail("system prompt lacks <workspace-roots> with the --add-dir root")
if "EXTRA-ROOT-MARK" not in sp:
    fail("system prompt lacks the extra root's AGENTS.md")
print("ok system prompt workspace roots + extra context file")

command("/add-dir ../other", "Added %s.\nWorkspace directories:\n  %s (working directory)\n  %s\n  %s" % (other, work, extra, other))
command("/add-dir ../other", "Already in the workspace: %s" % other)
command("/add-dir ../missing", "Directory does not exist: %s" % os.path.join(tmp, "missing"))
command("/add-dir", "Usage: /add-dir <path>\nWorkspace directories:")
command("/remove-dir .", "Cannot remove the working directory; use /move to change it.")
command("/remove-dir ../other", "Removed %s.\nWorkspace directories:\n  %s (working directory)\n  %s" % (other, work, extra))
command("/remove-dir ../other", "Not a workspace directory: %s" % other)

st = state()
sf = st.get("sessionFile")
if not sf or not os.path.exists(sf):
    fail("session file not on disk: %r" % sf)
with open(sf) as f:
    header = [json.loads(l) for l in f if l.strip() and '"type":"session"' in l.replace(" ", "")][0]
if header.get("additionalDirectories") != [extra]:
    fail("session header additionalDirectories %r" % header.get("additionalDirectories"))
print("ok session header additionalDirectories")

command("/pin", "Session pinned to the top of the resume list.")
pins = json.load(open(os.path.join(os.environ["OMC_CODING_AGENT_DIR"], "session-pins.json")))
if st.get("sessionId") not in pins:
    fail("session-pins.json %r lacks %s" % (pins, st.get("sessionId")))
command("/pin", "Session unpinned.")
command("/pin nosuchsession", 'Session "nosuchsession" not found.')

command("/fresh", "Fresh provider session started (1 provider state pruned).")
command("/fresh", "Fresh provider session started (0 provider states pruned).")

command("/move ../missing", "Directory does not exist: %s" % os.path.join(tmp, "missing"))
command("/move ../other", "Moved to %s." % other)
command("/dirs", "Workspace directories:\n  %s (working directory)\n  %s" % (other, extra))
moved = state().get("sessionFile") or ""
if not os.path.exists(moved) or os.path.exists(sf) or os.path.dirname(moved) == os.path.dirname(sf):
    fail("session file did not move to the new bucket: %r (was %r)" % (moved, sf))
print("ok session file moved")

command("/move ../work", "Moved to %s." % work)
out = command("/wt wp21-branch", "on branch wp21-branch (checked out, uncommitted changes carried over).")
wt = out.split("Moved to worktree ", 1)[1].split(" on branch", 1)[0]
if not wt.startswith(os.path.realpath(wtdir)) or not os.path.isfile(os.path.join(wt, "dirty.txt")):
    fail("worktree %r lacks the carried-over change" % wt)
command("/dirs", "  %s (working directory)" % wt)
command("/wt wp21-branch", "Worktree creation failed: Branch 'wp21-branch' already exists; pick another name.")
print("ok worktree")

p.stdin.close()
try:
    p.wait(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()
subprocess.run(["git", "worktree", "remove", "--force", wt], cwd=work, env=git_env, stdout=subprocess.DEVNULL,
               stderr=subprocess.DEVNULL)
shutil.rmtree(tmp, ignore_errors=True)
print("workspace rpc ok")
