#!/usr/bin/env python3
"""e2e driver for the eval kernel helpers: runs cells in one RPC session (one kernel) against the mock model and
checks each tool result.  usage: eval_client.py <omc> ; prints "ok <name>" / "FAIL <name>: ..." lines."""
import json, subprocess, sys, threading, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     text=True)
results, done = [], {}
lock = threading.Lock()


def reader():
    for line in p.stdout:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("type") == "message_end" and m["message"].get("role") == "toolResult":
            msg = m["message"]
            txt = "".join(b.get("text", "") for b in msg.get("content", []) if b.get("type") == "text")
            with lock:
                results.append((msg.get("isError"), txt))
        elif m.get("type") == "prompt_result":
            with lock:
                done[m.get("id")] = True


threading.Thread(target=reader, daemon=True).start()
fails = 0
n = [0]


def cell(name, code, want, timeout=None, reset=False, error=False, absent=None):
    global fails
    n[0] += 1
    pid = f"p{n[0]}"
    args = {"language": "py", "code": code}
    if timeout is not None:
        args["timeout"] = timeout
    if reset:
        args["reset"] = True
    with lock:
        before = len(results)
    p.stdin.write(json.dumps({"id": pid, "type": "prompt", "message": "call:eval:" + json.dumps(args)}) + "\n")
    p.stdin.flush()
    for _ in range(600):
        with lock:
            if pid in done and len(results) > before:
                break
        time.sleep(0.05)
    with lock:
        got = results[before] if len(results) > before else (None, "<no result>")
    ok = all(w in got[1] for w in (want if isinstance(want, list) else [want])) and bool(got[0]) == error and (not absent or absent not in got[1])
    print(("   ok  " if ok else "   FAIL ") + "eval " + name + ("" if ok else f": got {got!r}"), flush=True)
    if not ok:
        fails += 1


Q = {"verdict": {"type": "choice", "instructions": "Which fruit?", "criteria": {"apple": None, "pear": "a pear"}},
     "nice": {"type": "bool", "instructions": "Is it nice?"},
     "level": {"type": "score", "instructions": "How ripe?", "criteria": ["green", "ok", "ripe"]}}
cell("basic", "6*7", "42")
cell("raw fd output", "import os\nos.system('echo from-the-fd')\nprint('py-out')", ["py-out", "from-the-fd"])
cell("tool bridge", "print(await tool.bash({'command': 'echo bridged'}))", "bridged")
cell("completion+wait", "h = completion('hi there')\nh2 = completion('two', model='smol')\nprint(wait([h, h2]))",
     "['completion says: hi there', 'completion says: two']")
cell("completion schema",
     "h = completion('q', schema={'type':'object','properties':{'a':{'type':'string'},'n':{'type':'number'}}})\nh.wait()",
     "{'a': 'q', 'n': 42}")
cell("completion status", "h = completion('x')\nh.wait()\nprint(h.status, h.done())", "completed True")
cell("completion attempts", "h = completion('x')\nh.wait()\na = h.attempts\nprint(len(a), a[0]['outcome'], a[0]['model'], 'error' in a[0])",
     "1 succeeded mock/mock-1 False")
cell("wait bad handle", "wait(['x'])", "TypeError", error=True)
cell("judge", "r = await judge('a ripe pear, yes 2', " + repr(Q) + ")\n"
     "print(r['verdict']['choice'], r['nice']['bool'], r['level']['score'])", "pear 1 2")
cell("judge single", "r = await judge({'text': 'nothing'}, {'b': {'type': 'bool', 'instructions': 'ok?'}})\nr['b']",
     "{'type': 'bool', 'bool': 0}")
cell("judge invalid", "await judge('x', {'b': {'type': 'nope', 'instructions': 'i'}})",
     'question "b" type must be "choice", "bool", or "score"', error=True)
cell("judge parse failure", "await judge('FAILJUDGE', {'b': {'type': 'bool', 'instructions': 'ok?'}})",
     "judgment: every judge candidate failed", error=True)
cell("judge_batch drain",
     "b = judge_batch(['yes 1', 'no', 'FAILJUDGE'], {'b': {'type': 'bool', 'instructions': 'ok?'}}, intent='Grading')\n"
     "got = []\nwhile True:\n    items = await b.drain(5)\n    if not items: break\n    got += items\n"
     "print(sorted(k for k, it in got if it.ok), b.results()['0'], list(b.failed()), b.status()['done'])",
     ["[0, 1]", "'bool': 1", "['2']", " 3"])
cell("judge_batch iter",
     "b = judge_batch({'x': 'apple', 'y': 'pear'}, " + repr({"v": Q["verdict"]}) + ")\n"
     "out = {}\nasync for k, it in b.drain_iter(10):\n    out[k] = it.answers['v']['choice']\nprint(sorted(out.items()))",
     "[('x', 'apple'), ('y', 'pear')]")
cell("judge_batch start", "B = judge_batch(['SLOWJUDGE'] * 40, {'b': {'type': 'bool', 'instructions': 'ok?'}}, "
     "concurrency=2)\nwrite('bid.txt', B.id)\nprint(B.id.startswith('jdgb-'), B.total)", "True 40")
cell("judge_batch attach after reset", "b2 = judge_batch.attach(read('bid.txt'))\nprint(b2.status()['running'], b2.cancel())",
     "True True", reset=True, absent="Kernel restarted")
cell("judge_batch cancelled", "import asyncio\nfor _ in range(50):\n    if not b2.status()['running']: break\n"
     "    await asyncio.sleep(0.2)\ntry:\n    while await b2.drain(1): pass\nexcept Exception as e:\n    print('raised', e)\n"
     "print(b2.status()['running'])", ["raised judge_batch cancelled", "False"])
cell("judge_batch min_ok", "b = judge_batch(['FAILJUDGE'], {'b': {'type': 'bool', 'instructions': 'ok?'}})\n"
     "try:\n    while await b.drain(10): pass\nexcept Exception as e:\n    print('raised', e)", "only 0/1 item(s) judged")
cell("judge_batch unknown", "judge_batch.attach('jdgb-nope')", 'unknown judge_batch "jdgb-nope"', error=True)
cell("budget", "print(budget.total, budget.spent() >= 0, budget.remaining())", "None True inf")
cell("timeout keeps state", "KEEP = 5\nimport time\ntime.sleep(10)", "eval cell timed out after 1s", timeout=1, error=True)
cell("state kept", "KEEP", "5")
cell("helper wait pauses timeout", "b = judge_batch(['SLOWJUDGE', 'SLOWJUDGE'], {'b': {'type': 'bool', 'instructions': 'ok?'}}, "
     "concurrency=1)\nn = 0\nwhile True:\n    items = await b.drain()\n    if not items: break\n    n += len(items)\nn",
     "2", timeout=2)
cell("kernel dies", "import os\nos._exit(7)", "Python kernel exited", error=True)
cell("restart notice on the first cell", "print('again')",
     ["Kernel restarted: the previous Python kernel is dead; state is unavailable. Re-run needed setup: variables, imports and definitions from earlier cells are gone.\nagain"])
cell("once per replacement", "print('quiet')", "quiet", absent="Kernel restarted")
cell("dies between cells", "import os, threading\nthreading.Timer(0.2, lambda: os._exit(9)).start()\nprint('bye')", "bye")
time.sleep(0.8)
cell("replaced on the next call", "print('back')", ["Kernel restarted", "back"])
cell("xd docs", "print(await tool.read({'path': 'xd://eval/judge'}))", "judge_batch(states, questions")
p.stdin.close()
p.wait(timeout=20)
sys.exit(1 if fails else 0)
