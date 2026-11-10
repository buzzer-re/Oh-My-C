#!/usr/bin/env python3
"""RPC wire-format e2e checks (omp modes/rpc parity): ready / negotiate_protocol, v2 rpc_chunk framing and the v1
transport-limit error, messageId stamping and the "delta" projection, session events (model_changed, queue_update,
available_commands_update), response shapes (set_model, get_entries since, get_messages_page cursor, get_state,
session changes, login errors), async `bash` + `abort_bash`, the ask tool and tool approvals over
extension_ui_request / extension_ui_response (`--mode rpc-ui`). Prints "rpc wire ok" or the failed checks.
  rpc_wire.py <omc binary>"""
import base64, json, subprocess, sys, threading, time

B = sys.argv[1]
fails = []


class Rpc:
    def __init__(self, *args):
        self.p = subprocess.Popen([B, "--model", "mock/mock-1", "--no-session", *args], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.frames, self.lock, self.ui = [], threading.Lock(), []
        self.raw = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            self.raw.append(len(line))
            m = json.loads(line)
            if m.get("type") == "extension_ui_request" and m.get("method") in ("select", "editor", "input", "ask") and self.ui:
                a = dict(self.ui.pop(0))
                a.update(type="extension_ui_response", id=m["id"])
                self.send(a)
            with self.lock:
                self.frames.append(m)

    def send(self, o):
        self.p.stdin.write((json.dumps(o) + "\n").encode())
        self.p.stdin.flush()

    def wait(self, pred, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for f in self.frames:
                    if pred(f): return f
            time.sleep(0.02)
        return None

    def call(self, o, timeout=20):
        self.send(o)
        return self.wait(lambda f: f.get("type") == "response" and f.get("id") == o["id"], timeout)

    def prompt(self, pid, msg):
        self.send({"id": pid, "type": "prompt", "message": msg})
        return self.wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == pid, 30)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=15)
        except Exception:
            self.p.kill()


def check(name, cond, info=""):
    if not cond: fails.append(f"{name}: {str(info)[:400]}")


r = Rpc("--mode", "rpc-ui")
ready = r.wait(lambda f: f.get("type") == "ready")
check("ready", ready == {"type": "ready", "protocolVersion": 1, "supportedProtocolVersions": [1, 2],
                         "maxFrameBytes": 1048576, "maxReassembledFrameBytes": 67108864,
                         "supportsBashExcludeFromContext": True}, ready)
x = r.call({"id": "bx", "type": "bash", "command": "echo x", "excludeFromContext": None})
check("bash exclude type", x and x.get("error") == "excludeFromContext must be a boolean", x)
check("startup commands", r.wait(lambda f: f.get("type") == "available_commands_update"), "no available_commands_update")
x = r.call({"id": "n1", "type": "negotiate_protocol", "protocolVersion": 1})
check("negotiate v1", x and x.get("error") == "Unsupported RPC protocol version: 1", x)
x = r.call({"id": "u", "type": "bogus"})
check("unknown", x and x.get("error") == "Unknown command: bogus", x)

# message lifecycle: one messageId per message, partial snapshots, turn_start before the input message
res = r.prompt("p1", "say:hello world")
with r.lock:
    fr = list(r.frames)
ts = next(i for i, f in enumerate(fr) if f.get("type") == "turn_start")
us = next(i for i, f in enumerate(fr) if f.get("type") == "message_start" and f["message"]["role"] == "user")
check("turn_start first", ts < us, (ts, us))
am = [f for f in fr if f.get("type", "").startswith("message_") and f["message"].get("role") == "assistant"]
ids = {f.get("messageId") for f in am}
check("messageId", len(ids) == 1 and all(i and i.startswith("msg-") for i in ids), ids)
up = [f for f in am if f["type"] == "message_update"]
check("partial", up and all("partial" in f["assistantMessageEvent"] for f in up), up[:1])
check("prompt_result", res and res.get("status") == "completed" and res.get("agentInvoked") is True, res)

# delta projection
x = r.call({"id": "ef", "type": "set_event_filter", "events": None, "messageUpdates": "delta"})
check("filter", x and x["data"] == {"events": None, "messageUpdates": "delta"}, x)
n0 = len(r.frames)
r.prompt("p2", "say:delta run")
with r.lock:
    up = [f for f in r.frames[n0:] if f.get("type") == "message_update"]
check("delta message", up and all(f["message"] == {"role": "assistant"} for f in up), up[:1])
check("delta partial", all("partial" not in f["assistantMessageEvent"] for f in up), up[:1])
st = [f for f in up if f["assistantMessageEvent"]["type"] == "text_start"]
check("delta block", st and st[0]["assistantMessageEvent"].get("block") == {"type": "text", "text": ""}, st[:1])
r.call({"id": "ef2", "type": "set_event_filter", "events": None})
x = r.call({"id": "ef3", "type": "set_event_filter", "events": "x"})
check("filter invalid", x and x.get("error") == "events must be null or an array of non-empty event type strings", x)

# model
x = r.call({"id": "m", "type": "set_model", "provider": "mock", "modelId": "mock-2"})
check("set_model", x and x["data"].get("id") == "mock-2" and x["data"].get("provider") == "mock", x)
check("model_changed", r.wait(lambda f: f.get("type") == "model_changed", 2), "no model_changed")
x = r.call({"id": "m2", "type": "set_model", "provider": "mock", "modelId": "nope"})
check("set_model missing", x and x.get("error") == "Model not found: mock/nope", x)

# entries / paging / state
x = r.call({"id": "e1", "type": "get_entries"})
ents = x["data"]["entries"] if x else []
check("entries leafId", x and "leafId" in x["data"], x)
x = r.call({"id": "e2", "type": "get_entries", "since": ents[0]["id"] if ents else "?"})
check("entries since", x and len(x["data"]["entries"]) == len(ents) - 1, x)
x = r.call({"id": "e3", "type": "get_entries", "since": "nope"})
check("entries unknown", x and x.get("code") == "unknown_since" and x.get("error") == "Unknown entries cursor: nope", x)
x = r.call({"id": "g1", "type": "get_messages_page", "limit": 1})
d = x["data"] if x else {}
check("page 1", d.get("totalMessages") == 4 and len(d.get("messages", [])) == 1 and d.get("nextCursor"), d)
x = r.call({"id": "g2", "type": "get_messages_page", "cursor": d.get("nextCursor", ""), "limit": 256})
d2 = x["data"] if x else {}
check("page 2", len(d2.get("messages", [])) == 3 and "nextCursor" not in d2, d2)
x = r.call({"id": "g3", "type": "get_messages_page", "cursor": "%%"})
check("page bad cursor", x and x.get("error") == "Invalid RPC message cursor", x)
x = r.call({"id": "s", "type": "get_state"})
s = x["data"] if x else {}
for k in ("queuedMessages", "isSettled", "contextUsage", "autoCompactionEnabled", "dumpTools", "systemPrompt", "todoPhases"):
    check("state " + k, k in s, list(s))
x = r.call({"id": "ss", "type": "get_session_stats"})
check("stats", x and set(x["data"].get("tokens", {})) == {"input", "output", "reasoning", "cacheRead", "cacheWrite", "total"}, x)
x = r.call({"id": "sn", "type": "set_session_name", "name": " "})
check("session name", x and x.get("error") == "Session name cannot be empty", x)

# async bash: abort_bash overtakes a running command
t0 = time.time()
r.send({"id": "b", "type": "bash", "command": "sleep 10; echo late"})
time.sleep(0.5)
a = r.call({"id": "ab", "type": "abort_bash"}, 5)
x = r.wait(lambda f: f.get("type") == "response" and f.get("id") == "b", 8)
check("abort_bash", a and x and x["data"].get("cancelled") is True and time.time() - t0 < 6, x)

# queue_update while a run streams
r.send({"id": "q", "type": "prompt", "message": "slow:1.5:x"})
r.wait(lambda f: f.get("type") == "agent_start" and True, 5)
time.sleep(0.3)
r.call({"id": "fu", "type": "follow_up", "message": "later"})
qu = r.wait(lambda f: f.get("type") == "queue_update" and f.get("followUp") == ["later"], 5)
check("queue_update", qu and qu.get("liveSteered") == 0, qu)
r.wait(lambda f: f.get("type") == "prompt_result" and f.get("id") == "q", 30)
r.wait(lambda f: f.get("type") == "queue_update" and f.get("followUp") == [], 10)

# ask over select / editor dialogs
r.ui += [{"value": "B (Recommended)"}]
r.prompt("a1", 'call:ask:{"questions":[{"id":"q1","question":"Pick one","options":[{"label":"A"},{"label":"B","description":"bee"}],"recommended":1}]}')
sel = r.wait(lambda f: f.get("type") == "extension_ui_request" and f.get("method") == "select" and f.get("title") == "Pick one")
check("ask select", sel and sel["options"] == ["A", "B (Recommended)", "Other (type your own)"] and
      sel.get("optionDetails") == [{}, {"description": "bee"}, {}], sel)
te = r.wait(lambda f: f.get("type") == "tool_execution_end" and f.get("toolName") == "ask")
check("ask answer", te and te["result"]["content"][0]["text"] == "User selected: B", te)
x = r.call({"id": "ad", "type": "set_ask_dialog", "enabled": True})
check("set_ask_dialog", x and x["data"] == {"enabled": True}, x)
r.ui += [{"answers": [{"id": "q", "selectedOptions": ["A"]}]}]
n0 = len(r.frames)
r.prompt("a2", 'call:ask:{"questions":[{"id":"q","question":"Q","options":[{"label":"A"}]}]}')
with r.lock:
    ask = next((f for f in r.frames[n0:] if f.get("method") == "ask"), None)
    te = next((f for f in r.frames[n0:] if f.get("type") == "tool_execution_end"), None)
check("ask dialog", ask and ask["questions"] == [{"id": "q", "question": "Q", "options": [{"label": "A"}]}], ask)
check("ask dialog answer", te and te["result"]["content"][0]["text"] == "User selected: A", te)

# session change shapes
n0 = len(r.frames)
x = r.call({"id": "ns", "type": "new_session"})
check("new_session", x and x["data"] == {"cancelled": False}, x)
x = r.call({"id": "lg", "type": "login", "providerId": "nope"})
check("login unknown", x and x.get("error") == "Unknown OAuth provider: nope", x)

# transport limits: v1 replaces an oversized response with an error, v2 chunks it
big = [{"name": "p", "tasks": [{"content": "x" * 1200000, "status": "pending"}]}]
x = r.call({"id": "big1", "type": "set_todos", "phases": big})
check("v1 limit", x and x.get("error") == "RPC response exceeded the transport limit", x)
x = r.call({"id": "n2", "type": "negotiate_protocol", "protocolVersion": 2})
check("negotiate v2", x and x["data"] == {"protocolVersion": 2}, x)
r.send({"id": "big2", "type": "set_todos", "phases": big})
chunks = []
end = time.time() + 10
while time.time() < end:
    with r.lock:
        chunks = [f for f in r.frames if f.get("type") == "rpc_chunk"]
    if chunks and len(chunks) == chunks[0]["count"]: break
    time.sleep(0.05)
ok = bool(chunks) and len(chunks) == chunks[0]["count"]
if ok:
    data = b"".join(base64.b64decode(c["data"]) for c in sorted(chunks, key=lambda c: c["index"]))
    whole = json.loads(data)
    ok = whole.get("id") == "big2" and whole.get("success") is True and len(data) == chunks[0]["byteLength"]
check("v2 chunks", ok and max(r.raw) <= 1048576, [{k: v for k, v in c.items() if k != "data"} for c in chunks])
r.close()

# tool approval over a select dialog (approvalMode always-ask)
r = Rpc("--mode", "rpc", "--approval-mode", "always-ask")
r.ui += [{"value": "Deny"}]
r.prompt("ap", "bash:echo approved")
sel = r.wait(lambda f: f.get("type") == "extension_ui_request" and f.get("method") == "select")
check("approval select", sel and sel["title"] == "Allow tool: bash\nCommand: echo approved" and
      sel["options"] == ["Approve", "Approve bash for this session", "Deny"], sel)
te = r.wait(lambda f: f.get("type") == "tool_execution_end")
check("approval deny", te and te["isError"] and te["result"]["content"][0]["text"] == "Tool call denied by user: bash", te)
r.close()

print("rpc wire ok" if not fails else "\n".join(fails))
