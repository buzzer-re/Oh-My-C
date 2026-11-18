#!/usr/bin/env python3
"""Skill description compression and the session tree's cost cap (omp cost-gate-skill-description.test.ts):
a session resumed at its cap sends no compression request; a compression's spend counts toward the cap, so the next
model request is refused even though the main model is free. Compression stays off the wire until the gate exists.
Usage: skill_cost_rpc.py <omc> <mock request log> <scratch dir> <mock models.yml>
Prints "skill cost ok" or the first failing step."""
import glob, json, os, re, subprocess, sys, threading, time

omc, log, scratch, models = sys.argv[1:5]
agent = os.path.join(scratch, "agent")
work = os.path.join(scratch, "work")
os.makedirs(os.path.join(agent, "skills"), exist_ok=True)
os.makedirs(work, exist_ok=True)
base = re.search(r"baseUrl:\s*(\S+)", open(models).read()).group(1)
with open(os.path.join(agent, "models.yml"), "w") as f:
    f.write(f"""providers:
  mock:
    baseUrl: {base}
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        name: Free
        contextWindow: 100000
        maxTokens: 4096
      - id: mock-paid
        name: Paid
        contextWindow: 100000
        maxTokens: 4096
        cost: {{input: 1000, output: 1000, cacheRead: 0, cacheWrite: 0}}
""")
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("modelRoles:\n  default: mock/mock-1\n  smol: mock/mock-paid\n")
env = dict(os.environ, HOME=scratch, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", TMPDIR=scratch)


def fail(step, detail=""):
    print(f"skill cost FAIL {step}: {detail!r}")
    sys.exit(1)


def requests():
    if not os.path.exists(log):
        return []
    with open(log) as f:
        return [json.loads(line) for line in f if line.strip()]


def skill(name, desc):
    d = os.path.join(agent, "skills", name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "SKILL.md"), "w") as f:
        f.write(f"---\nname: {name}\ndescription: {desc}\n---\nbody\n")


def compressions(reqs, marker):
    return [r for r in reqs if marker in json.dumps(r["messages"]) and "Compress into one routing hint" in json.dumps(r["messages"][-1])]


def rpc(args, prompts, wait_after=2.5):
    p = subprocess.Popen([omc, "--mode", "rpc", *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, env=env, cwd=work)
    frames = []
    lock = threading.Lock()

    def reader():
        for line in p.stdout:
            try:
                with lock:
                    frames.append(json.loads(line))
            except ValueError:
                pass

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    try:
        for i, text in enumerate(prompts):
            time.sleep(wait_after)  # the background compression finishes first
            p.stdin.write(json.dumps({"id": f"p{i}", "type": "prompt", "message": text}) + "\n")
            p.stdin.flush()
            end = time.time() + 15
            while time.time() < end:
                with lock:
                    if any(f.get("type") == "prompt_result" and f.get("id") == f"p{i}" for f in frames):
                        break
                time.sleep(0.05)
        time.sleep(0.3)
    finally:
        p.stdin.close()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    with lock:
        return list(frames)


# metered: the compression's cost reaches the cap, so the free main model's next request is refused
skill("metered-skill", "Metered description, long enough (past the 100-char preview budget) that a routing hint is requested for it by the session")
before = len(requests())
frames = rpc(["--max-cost", "0.000001"], ["say:first-after-compression"])
later = requests()[before:]
if not compressions(later, "Metered description"):
    fail("compression request", [json.dumps(r["messages"])[:200] for r in later])
if any("first-after-compression" in json.dumps(r["messages"]) for r in later):
    fail("request past the cap after a metered compression", json.dumps(later[-1]["messages"])[-300:])
if "Cost cap reached" not in json.dumps(frames):
    fail("no refusal notice", json.dumps(frames)[-400:])

# resumed at the cap: spend recorded by the session stops new background compressions
os.unlink(os.path.join(agent, "skills", "metered-skill", "SKILL.md"))
os.rmdir(os.path.join(agent, "skills", "metered-skill"))
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("modelRoles:\n  default: mock/mock-paid\n  smol: mock/mock-paid\n")
subprocess.run([omc, "-p", "say:paid-turn"], env=env, cwd=work, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
               stderr=subprocess.DEVNULL, timeout=30)
skill("gated-skill", "Gated description that the capped session must not send to a model at all, however long it is")
before = len(requests())
rpc(["--continue", "--max-cost", "0.000001"], [], wait_after=0)
time.sleep(2.5)
later = requests()[before:]
if compressions(later, "Gated description"):
    fail("compression past the cap", json.dumps(later[-1]["messages"])[:300])

# a subagent passes the tree's gate: the parent's first turn reached the cap, so the child's request is not sent
with open(os.path.join(agent, "config.yml"), "w") as f:
    f.write("modelRoles:\n  default: mock/mock-paid\n  smol: mock/mock-1\n")
before = len(requests())
call = "call:task:" + json.dumps({"context": "cap", "tasks": [{"task": "say:subagent-past-the-cap"}]})
frames = rpc(["--max-cost", "0.05", "--no-session"], [call], wait_after=0.2)
time.sleep(1.5)
later = requests()[before:]
if not any("call:task:" in json.dumps(r["messages"]) for r in later):
    fail("parent turn", [json.dumps(r["messages"])[:200] for r in later])
if any("Complete assignment thoroughly" in json.dumps(r["messages"]) for r in later):
    fail("subagent request past the cap", json.dumps(later[-1]["messages"])[-300:])
# executor.ts (omp 3b063cc7af): the cap's refusal is the capped subagent's error, not an empty result
res = [f for f in frames if f.get("type") == "message_end" and f["message"].get("customType") == "async-result"]
if not res or 'status="failed"' not in res[-1]["message"]["content"] or "<error>Cost cap reached: this session spent" not in res[-1]["message"]["content"]:
    fail("subagent cap refusal as its error", json.dumps(res[-1:])[:600])
print("skill cost ok")
