#!/usr/bin/env python3
"""ACP e2e (WP-16): drives `omc acp` as an editor client would, with a fake client that advertises fs /
terminal / form-elicitation capabilities, and prints one `ok <check>` line per verified behaviour (or
`FAIL <check>: <detail>`). Usage: acp_session.py <omc> <mock_mcp.py>"""
import json
import os
import select
import subprocess
import sys
import threading

OMC, MOCK_MCP = sys.argv[1], sys.argv[2]
WORK = os.getcwd()


class Client:
    def __init__(self, caps):
        self.p = subprocess.Popen([OMC, "acp", "-m", "mock/mock-1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL)
        self.buf, self.frames, self.next_id, self.caps = b"", [], 0, caps
        self.buffers, self.terms = {}, {}

    def send(self, o):
        self.p.stdin.write((json.dumps({"jsonrpc": "2.0", **o}) + "\n").encode())
        self.p.stdin.flush()

    def read(self, timeout):
        while b"\n" not in self.buf:
            r, _, _ = select.select([self.p.stdout], [], [], timeout)
            if not r:
                return None
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)

    def call(self, method, params, settle=0.3):
        self.next_id += 1
        rid = self.next_id
        self.send({"id": rid, "method": method, "params": params})
        got, start = None, len(self.frames)
        while True:
            m = self.read(settle if got else 60)
            if m is None:
                break
            self.frames.append(m)
            if "method" in m and "id" in m:
                self.answer(m)
            elif m.get("id") == rid and "method" not in m:
                got = m
        return got, self.frames[start:]

    def answer(self, m):
        method, p, rid = m["method"], m.get("params") or {}, m["id"]
        res = {}
        if method == "session/request_permission":
            res = {"outcome": {"outcome": "selected", "optionId": "allow_once"}}
        elif method == "fs/read_text_file":
            res = {"content": self.buffers.get(p["path"]) or open(p["path"]).read()}
        elif method == "fs/write_text_file":
            open(p["path"], "w").write(p["content"])
        elif method == "terminal/create":
            pr = subprocess.run([p["command"], *p.get("args", [])], cwd=p.get("cwd"), capture_output=True, text=True)
            self.terms["t1"] = pr
            res = {"terminalId": "t1"}
        elif method == "terminal/output":
            res = {"output": self.terms["t1"].stdout, "truncated": False}
        elif method == "terminal/wait_for_exit":
            res = {"exitCode": self.terms["t1"].returncode, "signal": None}
        elif method == "elicitation/create":
            props = p["requestedSchema"]["properties"]
            res = {"action": "accept", "content": {"q0": props["q0"]["oneOf"][0]["const"]}}
        self.send({"id": rid, "result": res})

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=20)


def updates(frames, kind=None):
    us = [f["params"]["update"] for f in frames if f.get("method") == "session/update"]
    return [u for u in us if kind is None or u["sessionUpdate"] == kind]


def check(name, ok, detail=""):
    print(f"ok {name}" if ok else f"FAIL {name}: {detail}")


def texts(frames):
    out = []
    for u in updates(frames):
        for c in u.get("content", []) if isinstance(u.get("content"), list) else []:
            if c.get("type") == "content" and c["content"].get("type") == "text":
                out.append(c["content"]["text"])
        if isinstance(u.get("content"), dict) and u["content"].get("type") == "text":
            out.append(u["content"]["text"])
    return "\n".join(out)


def prompt(c, sid, text):
    return c.call("session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": text}]})


c = Client({})
r, _ = c.call("initialize", {"protocolVersion": 1, "clientCapabilities": {
    "fs": {"readTextFile": True, "writeTextFile": True}, "terminal": True}})
res = r["result"]
check("init", res["agentCapabilities"]["sessionCapabilities"] == {"list": {}, "fork": {}, "resume": {}, "close": {}}
      and res["_meta"] == {"steering": {"supported": True}} and res["agentInfo"]["name"] == "omc", res)
r, fr = c.call("session/new", {"cwd": WORK, "mcpServers": []})
sid = r["result"]["sessionId"]
check("configOptions", [o["id"] for o in r["result"]["configOptions"]] == ["mode", "model", "thinking"], r["result"])
kinds = [u["sessionUpdate"] for u in updates(fr)]
check("bootstrap", kinds[:2] == ["available_commands_update", "session_info_update"] and fr[0].get("id") == r["id"], kinds)
open("buf.txt", "w").write("on disk\n")
c.buffers[os.path.join(WORK, "buf.txt")] = "unsaved editor buffer\n"
r, fr = prompt(c, sid, "read:buf.txt")
check("fs read", any(f.get("method") == "fs/read_text_file" for f in fr) and "unsaved editor buffer" in texts(fr), texts(fr))
r, fr = prompt(c, sid, "write:out.txt:bridged write")
check("fs write", any(f.get("method") == "fs/write_text_file" for f in fr) and open("out.txt").read() == "bridged write")
r, fr = prompt(c, sid, "bash:echo via-terminal")
terms = [c2 for u in updates(fr, "tool_call_update") for c2 in u.get("content", []) if c2.get("type") == "terminal"]
check("terminal", any(f.get("method") == "terminal/create" for f in fr) and terms and "via-terminal" in texts(fr), texts(fr))
uu = updates(fr, "usage_update")
check("usage_update", uu and uu[-1]["size"] == 100000 and uu[-1]["used"] > 0 and "usage" not in uu[-1], uu)
check("turn usage", r["result"].get("usage", {}).get("totalTokens") == 240, r["result"])
r, _ = c.call("_session/steering", {"sessionId": sid, "prompt": [{"type": "text", "text": "x"}]})
check("steering idle", r["result"] == {"outcome": "promptRequired", "queue": "steering"}, r)
r, fr = c.call("session/set_config_option", {"sessionId": sid, "configId": "mode", "value": "plan"})
check("set_config_option", [u["sessionUpdate"] for u in updates(fr)] == ["current_mode_update", "config_option_update"]
      and r["result"]["configOptions"][0]["currentValue"] == "plan", r)
c.call("session/set_mode", {"sessionId": sid, "modeId": "default"})
r, fr = prompt(c, sid, "fail:400:content_filter blocked")
check("refusal", r["result"]["stopReason"] == "refusal" and "content_filter blocked" in texts(fr), r)
r, _ = c.call("session/fork", {"sessionId": sid, "cwd": WORK, "mcpServers": []})
fid = r["result"]["sessionId"]
r2, _ = c.call("session/close", {"sessionId": fid})
r3, _ = prompt(c, fid, "say:closed")
check("fork close", fid != sid and r2["result"] == {} and r3["error"]["code"] == -32002, (r, r2, r3))
c.close()

c = Client({})
c.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
r, fr = c.call("session/load", {"sessionId": sid, "cwd": WORK, "mcpServers": []})
kinds = [u["sessionUpdate"] for u in updates(fr)]
check("load replay", "user_message_chunk" in kinds and "tool_call" in kinds and "tool_call_update" in kinds
      and "configOptions" in r["result"], kinds)
r, fr = c.call("session/resume", {"sessionId": sid, "cwd": WORK})
check("resume", "modes" in r["result"] and "user_message_chunk" not in [u["sessionUpdate"] for u in updates(fr)], r)
c.close()

c = Client({})
c.call("initialize", {"protocolVersion": 1, "clientCapabilities": {"elicitation": {"form": {}}}})
r, _ = c.call("session/new", {"cwd": WORK, "mcpServers": [
    {"name": "mock", "command": sys.executable, "args": ["-I", MOCK_MCP], "env": []}]})
sid = r["result"]["sessionId"]
q = {"questions": [{"id": "color", "question": "Pick a color", "options": [{"label": "Red"}, {"label": "Blue"}]}]}
r, fr = prompt(c, sid, "call:ask:" + json.dumps(q))
check("elicitation", any(f.get("method") == "elicitation/create" for f in fr) and "User selected: Red" in texts(fr), texts(fr))
r, fr = prompt(c, sid, "call:mcp__mock_echo_text:" + json.dumps({"text": "via mcp"}))
check("mcp servers", "mcp echo: via mcp" in texts(fr), texts(fr))
c.close()
