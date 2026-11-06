#!/usr/bin/env python3
"""e2e: `edit.mode: none` / PI_EDIT_VARIANT=none remove the edit tool and keep bash (omp 75b4c84f); the sloppy edit mode reads
`*** End` / `*** End Patch` closing a Replace or Insert body as a close, never as content (omp ae923ccd);
skills.manualSkills hides skills from the model's catalog (omp a5e905cd).
Usage: edit_none.py <omc> <request log> <models.yml> <scratch dir>"""
import json, os, shutil, subprocess, sys

omc, reqlog, models, scratch = sys.argv[1:5]
agent, work, home = (os.path.join(scratch, d) for d in ("agent", "work", "home"))
for d in (agent, work, home): os.makedirs(d, exist_ok=True)
shutil.copy(models, os.path.join(agent, "models.yml"))
env = dict(os.environ, OMC_CODING_AGENT_DIR=agent, HOME=home)
env.pop("PI_EDIT_VARIANT", None)
fails = 0


def check(name, ok, got=""):
    global fails
    if not ok:
        fails += 1
        print(f"FAIL {name}: {got!r}")


def config(text):
    with open(os.path.join(agent, "config.yml"), "w") as f: f.write(text)


def run(prompt, extra=None):
    open(reqlog, "w").close()
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", prompt], cwd=work, env=dict(env, **(extra or {})),
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=25)
    rs = [r for r in map(json.loads, open(reqlog)) if r.get("tools")]
    return rs


def tools(rs): return [t["function"]["name"] for t in rs[0]["tools"]] if rs else []


config("edit:\n  mode: none\nbash:\n  enabled: false\n")
names = tools(run("say:hi"))
check("setting removes edit, keeps bash", "edit" not in names and "bash" in names, names)
config("bash:\n  enabled: false\n")
names = tools(run("say:hi"))
check("bash.enabled still works", "edit" in names and "bash" not in names, names)
names = tools(run("say:hi", {"PI_EDIT_VARIANT": "none"}))
check("env removes edit, keeps bash", "edit" not in names and "bash" in names, names)
config("edit:\n  mode: none\n")
open(os.path.join(work, "n.txt"), "w").write("plain\n")
rs = run('seq:[["read",{"path":"n.txt"}]]')
msg = [m for m in rs[-1]["messages"] if m.get("role") == "tool"]
text = "".join(p.get("text", "") for p in msg[0]["content"]) if msg and not isinstance(msg[0]["content"], str) else (msg[0]["content"] if msg else "")
check("no hashline anchors", text.startswith("1:plain") or text.startswith("plain"), text)

config("edit:\n  mode: sloppy\n")
for name, before, patch, want in [
    ("sentinels close bodies", "one\ntwo\nthree\nfour\n",
     "*** Edit File: a.txt\n*** Find\none\n*** Replace\nONE\n*** End Patch\n*** Find\ntwo\n*** Replace\nTWO\n*** End\n"
     "*** Find\nthree\n*** Insert After\n3.5\n*** End\n*** Find\nfour\n*** Replace\nFOUR\n*** End\n", "ONE\nTWO\nthree\n3.5\nFOUR\n"),
    ("literal in Find", "a\n*** End\nb\n", "*** Edit File: a.txt\n*** Find\na\n*** End\n*** Replace\na\n", "a\nb\n"),
]:
    path = os.path.join(work, "a.txt")
    open(path, "w").write(before)
    rs = run("seq:" + json.dumps([["edit", {"input": patch}]]))
    check(name, open(path).read() == want, open(path).read())
# skills.manualSkills: out of the model's catalog, by glob
for n in ("third-party-a", "pdf", "keep"):
    os.makedirs(os.path.join(agent, "skills", n), exist_ok=True)
    with open(os.path.join(agent, "skills", n, "SKILL.md"), "w") as f: f.write(f"---\nname: {n}\ndescription: Demo skill {n}\n---\nBody\n")
config("skills:\n  manualSkills:\n    - third-party-*\n    - pdf\n")
rs = run("say:hi")
sysmsg = json.dumps(rs[0]["messages"][0]) if rs else ""
check("manual skills hidden", "Demo skill keep" in sysmsg and "Demo skill pdf" not in sysmsg and "Demo skill third-party-a" not in sysmsg, sysmsg[-300:])
print("edit none ok" if not fails else "edit none failed")
sys.exit(1 if fails else 0)
