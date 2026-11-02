#!/usr/bin/env python3
"""Minimal ACP client for e2e tests: spawns `omc --mode acp`, runs one prompt, prints observed updates."""
import json
import os
import subprocess
import sys

omc, prompt = sys.argv[1], sys.argv[2]
decision = sys.argv[3] if len(sys.argv) > 3 else "allow_once"
extra = sys.argv[4:]  # more omc flags (e.g. --yolo)
p = subprocess.Popen([omc, "--model", "mock/mock-1", "--mode", "acp", *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)


def send(o):
    o["jsonrpc"] = "2.0"
    p.stdin.write(json.dumps(o) + "\n")
    p.stdin.flush()


def wait(req_id):
    while True:
        line = p.stdout.readline()
        if not line:
            sys.exit("EOF")
        m = json.loads(line)
        if m.get("method") == "session/update":
            u = m["params"]["update"]
            kind = u["sessionUpdate"]
            if kind in ("agent_message_chunk", "agent_thought_chunk", "user_message_chunk"):
                print(kind, u["content"]["text"])
            elif kind == "tool_call":
                print("tool_call", u["kind"], u["title"], u["status"])
            elif kind == "tool_call_update":
                texts = [c["content"]["text"] for c in u.get("content", []) if c.get("type") == "content" and c["content"].get("type") == "text"]
                print("tool_call_update", u["status"], (texts[-1] if texts else "").strip()[:200])
            else:
                print(kind)
        elif m.get("method") == "session/request_permission":
            print("permission", m["params"]["toolCall"]["title"])
            send({"id": m["id"], "result": {"outcome": {"outcome": "selected", "optionId": decision}}})
        elif m.get("id") == req_id:
            return m


r = wait(send({"id": 1, "method": "initialize", "params": {"protocolVersion": 1, "clientCapabilities": {}}}) or 1)
print("init", json.dumps(r["result"]["agentCapabilities"]["promptCapabilities"]))
send({"id": 2, "method": "session/new", "params": {"cwd": os.getcwd(), "mcpServers": []}})
r = wait(2)
sid = r["result"]["sessionId"]
print("session", r["result"]["modes"]["currentModeId"])
send({"id": 3, "method": "session/prompt", "params": {"sessionId": sid, "prompt": [{"type": "text", "text": prompt}]}})
r = wait(3)
print("stop", r.get("result", {}).get("stopReason"), r.get("error"))
send({"id": 4, "method": "session/list", "params": {}})
r = wait(4)
print("listed", any(s["sessionId"] == sid for s in r["result"]["sessions"]))
p.stdin.close()
p.wait(timeout=10)
