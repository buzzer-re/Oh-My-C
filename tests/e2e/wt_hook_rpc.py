#!/usr/bin/env python3
"""/wt runs the new worktree's post-checkout hook like `git worktree add` (omp 911a80022a): once, after the session
moved in, with git's arguments (null old HEAD, the base commit, 1), then rescans skills so the next request lists a
skill the hook created; a failing hook keeps the worktree and comes back as `Warning: Worktree created, but
post-checkout hook exited with N: <last line>`. Usage: wt_hook_rpc.py <omc> <request log>.
Runs in throwaway directories under $TMPDIR; prints one line per check, exits non-zero on the first failure."""
import json, os, shutil, stat, subprocess, sys, tempfile, threading, time

omc, reqlog = sys.argv[1], sys.argv[2]
tmp = os.path.realpath(tempfile.mkdtemp(prefix="omc-wthook-"))
work, wtdir, log = os.path.join(tmp, "work"), os.path.join(tmp, "wt"), os.path.join(tmp, "hook.log")
os.makedirs(work)
git_env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
for cmd in (["git", "init", "-q", "-b", "main"], ["git", "commit", "-q", "--allow-empty", "-m", "init"]):
    subprocess.run(cmd, cwd=work, env=git_env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=work, env=git_env, capture_output=True, text=True).stdout.strip()
hook = os.path.join(work, ".git", "hooks", "post-checkout")


def write_hook(body):
    os.makedirs(os.path.dirname(hook), exist_ok=True)
    with open(hook, "w") as f:
        f.write("#!/bin/sh\necho \"$1 $2 $3 $(pwd -P)\" >> '%s'\n%s" % (log, body))
    os.chmod(hook, os.stat(hook).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


write_hook("mkdir -p .omp/skills/hooked && printf -- '---\\nname: hooked\\ndescription: Made by the hook\\n---\\nbody\\n'"
           " > .omp/skills/hooked/SKILL.md\necho hook says hi\n")

env = dict(git_env, OMC_WORKTREE_DIR=wtdir)
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], cwd=work, env=env, stdin=subprocess.PIPE,
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


def fail(msg):
    print("FAIL", msg)
    p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(1)


def texts_after(n0):
    with lock:
        return "\n".join(f.get("text", "") for f in frames[n0:] if f.get("type") == "command_output")


seq = [0]


def command(text, want):
    seq[0] += 1
    pid = "h%d" % seq[0]
    with lock:
        n0 = len(frames)
    send({"id": pid, "type": "prompt", "message": text})
    r = wait(lambda f: f.get("type") == "response" and f.get("id") == pid)
    if not r or not r.get("success"):
        fail("%s: response %s" % (text, json.dumps(r)))
    end = time.time() + 20
    while time.time() < end and want not in texts_after(n0):
        time.sleep(0.05)
    out = texts_after(n0)
    if want not in out:
        fail("%s: wanted %r in %r" % (text, want, out))
    time.sleep(0.2)  # the RPC prompt thread settles before the next prompt
    return out


out = command("/wt hook-ok", "on branch hook-ok")
if "Warning:" in out:
    fail("unexpected warning: %r" % out)
wt = out.split("Moved to worktree ", 1)[1].split(" on branch", 1)[0]
runs = open(log).read().splitlines() if os.path.exists(log) else []
if runs != ["%s %s 1 %s" % ("0" * len(head), head, wt)]:
    fail("hook runs %r (wanted one run in %s)" % (runs, wt))
print("ok hook ran once with git's arguments")

# the rescan put the skill the hook made into the system prompt of the next request
seq[0] += 1
send({"id": "h%d" % seq[0], "type": "prompt", "message": "say:after the hook"})
if not wait(lambda f: f.get("type") == "agent_end", 20):
    fail("no agent_end after the prompt")
reqs = [json.loads(l) for l in open(reqlog) if l.strip()]
sysmsg = next((m for m in reqs[-1].get("messages", []) if m.get("role") == "system"), {})
sys_text = sysmsg.get("content") if isinstance(sysmsg.get("content"), str) else json.dumps(sysmsg.get("content"))
if "hooked" not in (sys_text or ""):
    fail("the hook's skill is not in the system prompt")
print("ok skills rescanned")

write_hook("echo first\necho 'boom: bad thing' >&2\nexit 3\n")
out = command("/wt hook-fail", "Warning: Worktree created, but post-checkout hook exited with 3: boom: bad thing")
wt2 = out.split("Moved to worktree ", 1)[1].split(" on branch", 1)[0]
if not os.path.isdir(wt2):
    fail("the worktree of a failed hook is gone")
print("ok failing hook warns and keeps the worktree")

p.stdin.close()
try:
    p.wait(timeout=10)
except subprocess.TimeoutExpired:
    p.kill()
for w in (wt, wt2):
    subprocess.run(["git", "worktree", "remove", "--force", w], cwd=work, env=git_env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
shutil.rmtree(tmp, ignore_errors=True)
print("wt hook rpc ok")
