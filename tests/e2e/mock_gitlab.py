#!/usr/bin/env python3
"""Dev-only mock of GitLab Duo for omc end-to-end tests (fake credentials only: GITLAB_TOKEN=glpat-test).

omc reaches it through OMC_PROVIDER_URL_REWRITE: https://gitlab.com -> /gl, https://cloud.gitlab.com -> /cgl.
  gitlab-duo (non-agentic): POST /gl/api/v4/ai/third_party_agents/direct_access -> AI-gateway token + headers;
    the Cloud Connector proxy speaks Anthropic Messages (/cgl/ai/v1/proxy/anthropic/v1/messages), OpenAI Chat
    Completions and Responses (/cgl/ai/v1/proxy/openai/v1/...). A user turn "bash:<cmd>" answers with a bash tool
    call; a tool result is echoed back as "Tool said: <output>"; anything else is "plain duo answer".
  gitlab-duo-agent (Duo Workflow): groups / settings PUT / project discovery / direct_access / workflow create /
    GraphQL models / stop PATCH, and the workflow WebSocket (/gl/api/v4/ai/duo_workflows/ws, RFC 6455 handshake and
    framing implemented here). The start request's goal decides the script:
      "bash:<cmd>"  agent reasoning, a runMCPTool action, then (after the actionResponse) "Tool said: <response>"
      "pause"       one snapshot with text, a tool boundary and more text (exercises pause_turn + replay)
      "reject"      close 4403
      otherwise     two incremental checkpoints "plain workflow" + " answer", FINISHED
Every request (and every WebSocket frame received) is appended to $MOCK_LOG as JSON lines; GET /_log returns them."""
import base64, hashlib, json, os, socket, struct, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
LOG = os.environ.get("MOCK_LOG")
TOKEN = "glpat-test"
LOCK = threading.Lock()
STATE = {"workflow": 100, "direct": 0}


def log(entry):
    if LOG:
        with LOCK, open(LOG, "a") as f:
            f.write(json.dumps(entry) + "\n")


def text_of(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        out = []
        for p in content:
            if isinstance(p, dict):
                if p.get("type") in ("text", "input_text", "output_text") and not p.get("text", "").startswith("<system-reminder>"):
                    out.append(p.get("text", ""))
                elif p.get("type") == "tool_result":
                    out.append(text_of(p.get("content")))
        return "".join(out)
    return ""


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_json(self, code, obj, headers=()):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        for k, v in headers:
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw) if raw else None
        except Exception:
            return raw.decode(errors="replace")

    def record(self, body):
        log({"method": self.command, "path": self.path, "auth": self.headers.get("Authorization", ""),
             "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body})

    def authed(self, want):
        return self.headers.get("Authorization") == "Bearer " + want

    def do_GET(self):
        if self.path == "/_log":
            data = open(LOG).read() if LOG and os.path.exists(LOG) else ""
            b = data.encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        self.record(None)
        p = self.path.split("?")[0]
        if p == "/gl/api/v4/ai/duo_workflows/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
            return self.websocket()
        if not self.authed(TOKEN):
            return self.send_json(401, {"message": "401 Unauthorized"})
        if p == "/gl/api/v4/groups":
            return self.send_json(200, [{"id": 41, "full_path": "other-group"},
                                        {"id": 42, "full_path": "duo-group", "duo_features_enabled": True}])
        if p == "/gl/api/v4/groups/42/projects":
            return self.send_json(200, [{"id": 7, "path_with_namespace": "duo-group/proj"}])
        return self.send_json(404, {"message": "404 Not Found", "path": p})

    def do_PUT(self):
        self.record(self.body())
        return self.send_json(200, {"id": 42})

    def do_PATCH(self):
        self.record(self.body())
        return self.send_json(200, {"status": "stopped"})

    def do_POST(self):
        body = self.body()
        self.record(body)
        p = self.path.split("?")[0]
        if p == "/gl/api/v4/ai/third_party_agents/direct_access":
            if not self.authed(TOKEN):
                return self.send_json(401, {"message": "401 Unauthorized"})
            STATE["direct"] += 1
            return self.send_json(200, {"token": "duo-direct-tok", "headers": {"x-gitlab-realm": "saas", "x-gitlab-instance-id": "inst-1"}})
        if p == "/gl/api/v4/ai/duo_workflows/direct_access":
            if not self.authed(TOKEN):
                return self.send_json(401, {"message": "401 Unauthorized"})
            return self.send_json(200, {"gitlab_rails": {"token": "wf-tok", "base_url": "https://gitlab.com"}})
        if p == "/gl/api/v4/ai/duo_workflows/workflows":
            with LOCK:
                STATE["workflow"] += 1
                wid = STATE["workflow"]
            return self.send_json(201, {"id": wid})
        if p == "/gl/api/graphql":
            return self.send_json(200, {"data": {"aiChatAvailableModels": {
                "defaultModel": {"name": "Claude Sonnet 4.6 - Vertex", "ref": "claude_sonnet_4_6_vertex"},
                "selectableModels": [], "pinnedModel": None}}})
        if p.startswith("/cgl/"):
            if not self.authed("duo-direct-tok") or self.headers.get("x-gitlab-realm") != "saas":
                return self.send_json(401, {"error": {"message": "proxy auth failed"}})
            if p.endswith("/v1/messages"):
                return self.anthropic(body)
            if p.endswith("/chat/completions"):
                return self.chat(body)
            if p.endswith("/responses"):
                return self.oai_responses(body)
        return self.send_json(404, {"message": "not found", "path": p})

    # ---------- proxy protocols ----------
    def sse_start(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def chunk(self, s):
        b = s.encode()
        self.wfile.write(b"%x\r\n%s\r\n" % (len(b), b))
        self.wfile.flush()

    def sse_end(self):
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def anthropic(self, body):
        msgs = body.get("messages", [])
        last = msgs[-1] if msgs else {}
        content = last.get("content")
        tool_res = [c for c in content if isinstance(c, dict) and c.get("type") == "tool_result"] if isinstance(content, list) else []
        user = text_of(content)
        self.sse_start()
        ev = lambda name, obj: self.chunk("event: %s\ndata: %s\n\n" % (name, json.dumps(obj)))
        ev("message_start", {"type": "message_start", "message": {"id": "msg_1", "type": "message", "role": "assistant", "model": body.get("model"),
                                                                  "content": [], "usage": {"input_tokens": 5, "output_tokens": 0}}})
        if not tool_res and "bash:" in user:
            cmd = user[user.index("bash:") + 5:].strip()
            ev("content_block_start", {"type": "content_block_start", "index": 0,
                                       "content_block": {"type": "tool_use", "id": "toolu_1", "name": "bash", "input": {}}})
            ev("content_block_delta", {"type": "content_block_delta", "index": 0,
                                       "delta": {"type": "input_json_delta", "partial_json": json.dumps({"command": cmd})}})
            ev("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "tool_use"
        else:
            txt = "Tool said: " + text_of(tool_res[0].get("content")).strip() if tool_res else "plain duo answer"
            ev("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})
            ev("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": txt}})
            ev("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "end_turn"
        ev("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 3}})
        ev("message_stop", {"type": "message_stop"})
        self.sse_end()

    def chat(self, body):
        msgs = body.get("messages", [])
        last = msgs[-1] if msgs else {}
        self.sse_start()
        d = lambda obj: self.chunk("data: %s\n\n" % json.dumps(obj))
        base = {"id": "c1", "object": "chat.completion.chunk", "model": body.get("model")}
        if last.get("role") == "user" and "bash:" in text_of(last.get("content")):
            u = text_of(last.get("content"))
            cmd = u[u.index("bash:") + 5:].strip()
            d(dict(base, choices=[{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                                                       "function": {"name": "bash", "arguments": json.dumps({"command": cmd})}}]}}]))
            d(dict(base, choices=[{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]))
        else:
            txt = "Tool said: " + text_of(last.get("content")).strip() if last.get("role") == "tool" else "plain duo answer"
            d(dict(base, choices=[{"index": 0, "delta": {"content": txt}}]))
            d(dict(base, choices=[{"index": 0, "delta": {}, "finish_reason": "stop"}]))
        self.chunk("data: [DONE]\n\n")
        self.sse_end()

    def oai_responses(self, body):
        self.sse_start()
        ev = lambda obj: self.chunk("event: %s\ndata: %s\n\n" % (obj["type"], json.dumps(obj)))
        item = {"type": "message", "id": "msg_1", "role": "assistant", "status": "completed",
                "content": [{"type": "output_text", "text": "plain duo answer", "annotations": []}]}
        ev({"type": "response.created", "response": {"id": "resp_1", "status": "in_progress", "output": []}})
        ev({"type": "response.output_item.added", "output_index": 0, "item": dict(item, content=[], status="in_progress")})
        ev({"type": "response.content_part.added", "output_index": 0, "item_id": "msg_1", "content_index": 0,
            "part": {"type": "output_text", "text": "", "annotations": []}})
        ev({"type": "response.output_text.delta", "output_index": 0, "item_id": "msg_1", "content_index": 0, "delta": "plain duo answer"})
        ev({"type": "response.output_item.done", "output_index": 0, "item": item})
        ev({"type": "response.completed", "response": {"id": "resp_1", "status": "completed", "output": [item],
                                                       "usage": {"input_tokens": 5, "output_tokens": 3, "total_tokens": 8}}})
        self.sse_end()

    # ---------- WebSocket ----------
    def websocket(self):
        if self.headers.get("authorization") != "Bearer wf-tok":
            return self.send_json(401, {"message": "bad workflow token"})
        key = self.headers.get("Sec-WebSocket-Key", "")
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        self.wfile.flush()
        self.close_connection = True
        try:
            self.ws_session()
        except (ConnectionError, OSError, ValueError):
            pass

    def ws_recv(self):
        msg, op0 = b"", None
        while True:
            h = self.rfile.read(2)
            if len(h) < 2:
                raise ConnectionError("eof")
            fin, op = h[0] & 0x80, h[0] & 0x0F
            masked, n = h[1] & 0x80, h[1] & 0x7F
            if not masked:
                raise ValueError("client frame not masked")
            if n == 126:
                n = struct.unpack(">H", self.rfile.read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self.rfile.read(8))[0]
            mk = self.rfile.read(4)
            data = bytes(b ^ mk[i % 4] for i, b in enumerate(self.rfile.read(n)))
            if op == 8:
                return 8, data
            if op in (9, 10):
                continue
            if op != 0:
                op0 = op
            msg += data
            if fin:
                return op0, msg

    def ws_send(self, obj, op=1, raw=None):
        data = raw if raw is not None else json.dumps(obj).encode()
        if len(data) < 126:
            hdr = struct.pack("BB", 0x80 | op, len(data))
        elif len(data) < 65536:
            hdr = struct.pack(">BBH", 0x80 | op, 126, len(data))
        else:
            hdr = struct.pack(">BBQ", 0x80 | op, 127, len(data))
        # exercise fragmentation for larger text frames: split into a text frame + continuation
        if op == 1 and len(data) > 64:
            a, b = data[:32], data[32:]
            self.wfile.write(struct.pack("BB", 0x01, len(a)) + a)
            self.wfile.write(struct.pack("BB", 0x89, 2) + b"hi")  # interleaved ping (client must pong)
            hb = struct.pack("BB", 0x80, len(b)) if len(b) < 126 else (struct.pack(">BBH", 0x80, 126, len(b)) if len(b) < 65536 else struct.pack(">BBQ", 0x80, 127, len(b)))
            self.wfile.write(hb + b)
        else:
            self.wfile.write(hdr + data)
        self.wfile.flush()

    def checkpoint(self, log_entries, status="RUNNING"):
        cp = json.dumps({"channel_values": {"ui_chat_log": log_entries}})
        return {"newCheckpoint": {"status": status, "checkpoint": cp,
                                  "agent_context_usage": {"Chat Agent": {"total_tokens": 1234, "max_tokens": 1000000}}}}

    def ws_session(self):
        op, data = self.ws_recv()
        start = json.loads(data)
        log({"ws": "start", "query": self.path, "frame": start})
        sr = start.get("startRequest", {})
        goal = sr.get("goal", "")
        req = {"message_type": "request", "content": "q"}
        if "reject" in goal:
            self.ws_send(None, op=8, raw=struct.pack(">H", 4403) + b"nope")
            self.ws_recv()
            return
        if "pause" in goal:
            self.ws_send(self.checkpoint([req, {"message_type": "agent", "message_id": "a1", "content": "part one"},
                                          {"message_type": "tool", "content": "t"},
                                          {"message_type": "agent", "message_id": "a2", "content": "part two"}], "FINISHED"))
            self.ws_recv()
            return
        if "bash:" in goal and "<ran:result>" not in goal:
            cmd = goal[goal.rindex("bash:") + 5:].split("<|im_end|>")[0].strip()
            reasoning = {"message_type": "agent", "message_sub_type": "reasoning", "message_id": "r1", "content": "Running the command"}
            self.ws_send(self.checkpoint([req, reasoning]))
            self.ws_send({"requestID": "req-1", "runMCPTool": {"name": "bash", "args": json.dumps({"command": cmd, "i": "Run it"})}})
            op, data = self.ws_recv()
            resp = json.loads(data)
            log({"ws": "response", "frame": resp})
            out = resp.get("actionResponse", {}).get("plainTextResponse", {})
            txt = (out.get("response") or out.get("error") or "").strip()
            self.ws_send(self.checkpoint([req, reasoning, {"message_type": "tool", "content": "bash"},
                                          {"message_type": "agent", "message_id": "a9", "content": "Tool said: " + txt}], "FINISHED"))
            self.ws_recv()
            return
        a = {"message_type": "agent", "message_id": "p1", "content": "plain workflow"}
        self.ws_send(self.checkpoint([req, a]))
        self.ws_send(self.checkpoint([req, dict(a, content="plain workflow answer")], "FINISHED"))
        self.ws_recv()


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
