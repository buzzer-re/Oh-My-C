#!/usr/bin/env python3
"""Scripted RPC client for e2e tests: sends commands, answers host_tool_call frames, prints responses."""
import json, subprocess, sys, threading, time

omc = sys.argv[1]
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "rpc"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
pending = {}
lock = threading.Lock()

def send(o):
    with lock:
        p.stdin.write(json.dumps(o) + "\n")
        p.stdin.flush()

def reader():
    for line in p.stdout:
        m = json.loads(line)
        t = m.get("type")
        if t == "host_tool_call":
            print("host_tool_call", m["toolName"], json.dumps(m["arguments"]), flush=True)
            send({"type": "host_tool_result", "id": m["id"], "result": {"content": [{"type": "text", "text": "host says " + m["arguments"].get("x", "")}]}})
        elif t == "response":
            pending[m.get("id")] = m
            print("response", m["command"], m["success"], json.dumps(m.get("data"))[:300], flush=True)
        elif t == "prompt_result":
            pending["prompt:" + str(m.get("id"))] = m
        elif t == "message_end" and m["message"]["role"] == "assistant":
            c = m["message"]["content"]
            print("assistant", " ".join(b.get("text", "") for b in c if b.get("type") == "text"), flush=True)

threading.Thread(target=reader, daemon=True).start()

def call(o, wait_prompt=False):
    send(o)
    key = ("prompt:" if wait_prompt else "") + o["id"]
    for _ in range(300):
        if key in pending: return pending[key]
        time.sleep(0.05)

call({"id": "h", "type": "set_host_tools", "tools": [{"name": "host_echo", "description": "Echo via host", "parameters": {"type": "object", "properties": {"x": {"type": "string"}}}}]})
call({"id": "p1", "type": "prompt", "message": 'call:host_echo:{"x":"hi"}'}, wait_prompt=True)
call({"id": "p2", "type": "prompt", "message": "second"}, wait_prompt=True)
call({"id": "lt", "type": "get_last_assistant_text"})
call({"id": "bm", "type": "get_branch_messages"})
r = call({"id": "t", "type": "get_tree"})
tree = r["data"]["tree"]
def count(n): return 1 + sum(count(c) for c in n["children"])
print("tree nodes", sum(count(n) for n in tree), "leaf", r["data"]["leafId"] is not None)
bm = pending["bm"]["data"]["messages"]
call({"id": "nav", "type": "navigate_tree", "entryId": bm[-1]["entryId"]})
call({"id": "pg", "type": "get_messages_page", "limit": 2})
call({"id": "td", "type": "set_todos", "phases": [{"name": "P", "tasks": [{"content": "a", "status": "pending"}]}]})
call({"id": "tl", "type": "get_available_thinking_levels"})
call({"id": "ef", "type": "set_event_filter", "events": ["agent_end"]})
call({"id": "ac", "type": "get_available_commands"})
p.stdin.close()
p.wait(timeout=10)
