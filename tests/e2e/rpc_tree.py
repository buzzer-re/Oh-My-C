#!/usr/bin/env python3
"""e2e (WP-8): RPC navigate_tree with summarize writes omp's branch_summary entry and sends omp's summarizer
request. Usage: rpc_tree.py <omc> <mock request log>"""
import json, subprocess, sys, threading, time

omc, reqlog = sys.argv[1], sys.argv[2]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
pending = {}

def send(o):
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()

def reader():
    for line in p.stdout:
        m = json.loads(line)
        if m.get("type") == "response":
            pending[m.get("id")] = m
        elif m.get("type") == "prompt_result":
            pending["prompt:" + str(m.get("id"))] = m

threading.Thread(target=reader, daemon=True).start()

def call(o, wait_prompt=False):
    send(o)
    key = ("prompt:" if wait_prompt else "") + o["id"]
    for _ in range(400):
        if key in pending: return pending[key]
        time.sleep(0.05)
    raise SystemExit("timeout " + key)

call({"id": "p1", "type": "prompt", "message": "alpha"}, wait_prompt=True)
call({"id": "p2", "type": "prompt", "message": "beta"}, wait_prompt=True)
bm = call({"id": "bm", "type": "get_branch_messages"})["data"]["messages"]
beta = [m for m in bm if m["text"] == "beta"][0]["entryId"]
nav = call({"id": "nav", "type": "navigate_tree", "entryId": beta, "summarize": True})
print("nav", nav["success"], json.dumps(nav.get("data")))
tree = call({"id": "t", "type": "get_tree"})["data"]

def walk(nodes):
    for n in nodes:
        yield n
        yield from walk(n.get("children", []))

for n in walk(tree["tree"]):
    e = n.get("entry", n)
    if e.get("type") == "branch_summary":
        print("summary leaf", tree["leafId"] == e["id"], "fromId parent", e["fromId"] == e["parentId"])
        print("summary", json.dumps(e["summary"]))
        print("details", json.dumps(e.get("details")), "fromExtension", e.get("fromExtension"))
reqs = [json.loads(l) for l in open(reqlog)]
summ = [r for r in reqs if r.get("max_completion_tokens") == 2048 and "<conversation>" in json.dumps(r)]
if summ:
    text = summ[-1]["messages"][-1]["content"]
    text = text if isinstance(text, str) else text[0]["text"]
    print("request", json.dumps(text[:120]))
p.stdin.close()
p.wait(timeout=10)
