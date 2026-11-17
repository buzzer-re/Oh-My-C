#!/usr/bin/env python3
"""e2e: read's page budget and the write projection guard (omp read-display-budget / write-read-projection tests):
column-cut lines charged their shown width, the `:raw` byte-cap notice, where a multi-range read stopped, the context
retry, the first-line preview, and `write` refusing a projection whose long lines were cut.
Usage: read_budget.py <omc> <mock request log> <models.yml> <dir>"""
import json, os, re, shutil, subprocess, sys

omc, reqlog, models, root = sys.argv[1:5]
work = os.path.join(root, "work")
os.makedirs(work, exist_ok=True)
fails = []


def call(tool, args, config=""):
    agent = os.path.join(root, "agent")
    shutil.rmtree(agent, ignore_errors=True)
    os.makedirs(agent)
    shutil.copy(models, os.path.join(agent, "models.yml"))
    with open(os.path.join(agent, "config.yml"), "w") as f:
        f.write("read:\n  summarize:\n    enabled: false\n" + config)
    env = dict(os.environ, HOME=root, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", OMC_NO_LOCAL_DISCOVERY="1")
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "call:%s:%s" % (tool, json.dumps(args))], cwd=work, env=env,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    last = None
    for line in open(reqlog):
        r = json.loads(line)
        for m in r.get("messages", []):
            if m.get("role") == "tool":
                last = m
    c = last["content"]
    return c if isinstance(c, str) else "".join(p.get("text", "") for p in c)


def calls(script, config=""):
    """Run `calls:[...]` in one session; returns the last tool message text."""
    agent = os.path.join(root, "agent")
    shutil.rmtree(agent, ignore_errors=True)
    os.makedirs(agent)
    shutil.copy(models, os.path.join(agent, "models.yml"))
    open(os.path.join(agent, "config.yml"), "w").write(config)
    env = dict(os.environ, HOME=root, OMC_CODING_AGENT_DIR=agent, MOCK_KEY="x", OMC_NO_TITLE="1", OMC_NO_LOCAL_DISCOVERY="1")
    subprocess.run([omc, "--model", "mock/mock-1", "--no-session", "-p", "calls:" + json.dumps(script)], cwd=work, env=env,
                   stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    last = None
    for line in open(reqlog):
        for m in json.loads(line).get("messages", []):
            if m.get("role") == "tool":
                last = m
    c = last["content"]
    return c if isinstance(c, str) else "".join(p.get("text", "") for p in c)


def read(target, config=""):
    return call("read", {"path": target}, config)


def rows(text):
    out = {}
    for line in text.split("\n"):
        m = re.match(r"^(\d+):(.*)$", line)
        if m:
            out[int(m.group(1))] = m.group(2)
    return out


def check(name, cond, detail=""):
    if not cond:
        fails.append(name)
        print("FAIL", name, str(detail)[:600])


def put(name, data):
    path = os.path.join(work, name)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode())
    return name


WIDE = "H" * 80000
small_lines = ["r%d" % (i + 1) if i != 150 else WIDE for i in range(300)]
small = put("small.txt", "\n".join(small_lines) + "\n")

t = read(small)
s = rows(t)
check("all rows", len(s) == 300, len(s))
check("wide line cut", s.get(151) == WIDE[:768] + "…")
check("no continuation", "to continue" not in t, t[-200:])
check("raw byte cap named", "Use :raw:151-151 to read line 151 uncut, byte-capped at 50.0KB" in t, t[-200:])

s = rows(read(small + ":151"))
check("page through wide", s.get(150) == "r150" and s.get(151) == WIDE[:768] + "…" and 300 in s, sorted(s)[:5])

t = read(small + ":raw:151-151")
check("raw byte-capped", t.startswith("H" * 40000) and "…" not in t and "partial" in t, t[-120:])

t = read(small, "tools:\n  outputMaxColumns: 0\n")
s = rows(t)
check("cap off stops", 150 in s and 151 not in s and ":151 to continue" in t, t[-200:])

stray = put("stray.bin", b"z" * 8500 + b"\xff" * 30000 + b"\nnext\n")
t = read(stray, "tools:\n  outputMaxColumns: 30000\n")
check("first cut overflow pages on", rows(t).get(1, "").startswith("z" * 8500) and "Showing 0" not in t and "Use :2 to continue" in t, t[-200:])
sole = put("sole.bin", b"z" * 8500 + b"\xff" * 30000 + b"\n")
t = read(sole, "tools:\n  outputMaxColumns: 30000\n")
check("sole overflow no continuation", rows(t).get(1, "").startswith("z" * 8500) and "to continue" not in t, t[-200:])

huge = "T" * (4 * 1024 * 1024 + 200000)
ranges = put("ranges.bin", b"z" * 8500 + b"\xff" * 30000 + ("\ntwo\nthree\nfour\n%s\n" % huge).encode())
t = read(ranges + ":1-2,4-4", "tools:\n  outputMaxColumns: 30000\n")
check("range stop rows", sorted(rows(t)) == [1, 4], sorted(rows(t)))
check("range stop notice", "[Range 1-2 stopped before line 2 at the 50.0KB read budget. Use :2-2 to continue]" in t, t[-300:])

five = put("five.txt", "short\n" + "f" * 5000 + "\n")
check("whole wording", "Use :raw:2-2 to read line 2 whole" in read(five))

records = [json.dumps({"id": i + 1, "body": "x" * 5000}) for i in range(35)]
jsonl = put("records.jsonl", "\n".join(records) + "\n")
s = rows(read(jsonl))
check("5 KB records", len(s) == 35 and s.get(35) == records[34][:768] + "…", len(s))

ctx = put("context.txt", WIDE + "\n" + "y" * 300 + "\nr3\n")
s = rows(read(ctx + ":2-2", "tools:\n  artifactSpillThreshold: 1\n"))
check("context retry", s.get(2) == "y" * 300, sorted(s))

# repeated identical output: the hint suggests a selector only where one is accepted
put("repeat.txt", "a\nb\n")
t = calls([["read", {"path": "repeat.txt"}]] * 3)
check("repeat hint", "[You have received this identical output 3 times. Re-reading 'repeat.txt' will not change it \u2014 use a narrower selector (path:A-B), or proceed with the edit.]" in t, t)
t = calls([["read", {"path": "xd://debug"}]] * 3)
# xd:// docs take line selectors since omp c51e68b454, so their hint suggests one too
check("repeat hint xd://", "identical output 3 times" in t and "use a narrower selector (path:A-B), or proceed with the edit.]" in t, t[-300:])
t = calls([["read", {"path": "repeat.txt"}]] * 2)
check("no hint on second read", "identical output" not in t, t)

# write: read-projection guard
orig = "\n".join("line %d" % (i + 1) for i in range(60)) + "\n"
p = put("sample.txt", orig)
for notice in ["[Showing lines 1-20 of 60. Use :21 to continue]", "[Showing lines 1-20 of 60 (50.0KB limit). Use :21 to continue]",
               "[40 more lines in notebook. Use :21 to continue]", "[More lines in file (120.0KB total; not scanned to EOF). Use :21 to continue]"]:
    partial = "\n".join(orig.split("\n")[:20]) + "\n" + notice + "\n"
    r = call("write", {"path": p, "content": partial})
    check("refuse " + notice, "incomplete read projection" in r and "truncation notice" in r, r)
    check("untouched", open(os.path.join(work, p)).read() == orig)

wide_src = "short\n" + "x" * (60 * 1024) + "\n"
p = put("wide.txt", wide_src)
proj = read(p)
check("wide projection notice", "Some lines truncated to 768 chars" in proj, proj[-200:])
r = call("write", {"path": p, "content": proj})
check("refuse cut projection", "incomplete read projection" in r and "column-cap notice" in r, r)
check("cut untouched", open(os.path.join(work, p)).read() == wide_src)

edge_src = "x" * 769
p = put("edge.txt", edge_src)
proj = read(p + ":1-1", "edit:\n  mode: replace\n")
check("edge notice", "Some lines truncated to 768 chars" in proj, proj[-200:])
r = call("write", {"path": p, "content": proj}, "edit:\n  mode: replace\n")
check("refuse same-length cut", "incomplete read projection" in r, r)

num_src = "x" * 769 + "\nb\nc\nd\ne\n"
p = put("numbered.txt", num_src)
proj = read(p + ":1-1", "edit:\n  mode: replace\nreadLineNumbers: true\n")
check("numbered clause", re.search(r"\[Showing lines .*\. Some lines truncated to 768 chars", proj) is not None, proj[-200:])
r = call("write", {"path": p, "content": proj}, "edit:\n  mode: replace\nreadLineNumbers: true\n")
check("refuse numbered clause", "incomplete read projection" in r, r)

p = put("notes.txt", "header\nbody line\n")
content = "header\n[Some lines truncated to explain the format\n"
r = call("write", {"path": p, "content": content})
check("unbracketed writes", open(os.path.join(work, p)).read() == content, r)
content = "header\n[Some lines truncated to explain the format]\n"
p = put("format.txt", content)
r = call("write", {"path": p, "content": content})
check("bracketed prose writes", "Successfully wrote" in r, r)
p = put("frac.txt", "x" * 2000 + "\n")
r = call("write", {"path": p, "content": "abc\n[Some lines truncated to 768.5 chars. Use :raw:1-1 to read line 1 whole]\n"})
check("fractional cap refused", "incomplete read projection" in r, r)
r = call("write", {"path": p, "content": "abc\n[Some lines truncated to 768 cats]\n"})
check("wrong unit writes", "Successfully wrote" in r, r)
print("read budget ok" if not fails else "read budget FAILED: " + ", ".join(fails))
sys.exit(1 if fails else 0)
