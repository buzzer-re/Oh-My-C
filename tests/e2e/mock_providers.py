#!/usr/bin/env python3
"""Dev-only mock for provider request shaping (tests/e2e/providers_transport.sh). omc reaches it through
OMC_PROVIDER_URL_REWRITE, so each real origin maps to a path prefix here and the logged path shows which URL omc
built. Every request is appended to $MOCK_LOG as {method, path, headers (lower-cased), body}.

Protocols (chosen by path suffix):
  .../chat/completions  OpenAI Chat Completions SSE      .../responses  OpenAI Responses SSE
  .../messages          Anthropic Messages SSE           .../v1/systemone, .../ai/run/<model>  native judgments
A user message "bash:<cmd>" gets a bash tool call (chat completions / anthropic), "read:<path>" a read call (anthropic), "call:<tool>:<json args>" any tool
(chat completions), a tool result gets the final text.
Paths under /deny answer 403 to Copilot-Integration-Id copilot-chat (identity fallback test)."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")


def text_of(msg):
    c = msg.get("content")
    if isinstance(c, str):
        return c
    out = ""
    for p in c or []:
        if isinstance(p, dict) and p.get("type") in ("text", "input_text") and not p.get("text", "").startswith("<system-reminder>"):
            out += p.get("text", "")
    return out


def last_turn(body):
    msgs = body.get("messages") or body.get("input") or []
    last = msgs[-1] if msgs else {}
    role = last.get("role")
    if role == "tool":
        return "tool", text_of(last)
    if role == "user" and isinstance(last.get("content"), list) and any(
            isinstance(p, dict) and p.get("type") == "tool_result" for p in last["content"]):
        res = [p for p in last["content"] if p.get("type") == "tool_result"][-1]
        c = res.get("content")
        return "tool", c if isinstance(c, str) else "".join(x.get("text", "") for x in c or [])
    return "user", text_of(last)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def sse_start(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

    def chunk(self, data):
        self.wfile.write(b"%x\r\n%s\r\n" % (len(data), data))
        self.wfile.flush()

    def ev(self, obj, event=None):
        s = (f"event: {event}\n" if event else "") + "data: " + json.dumps(obj) + "\n\n"
        self.chunk(s.encode())

    def end(self):
        self.chunk(b"")

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {}
        path = self.path
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"method": "POST", "path": path, "headers": {k.lower(): v for k, v in self.headers.items()},
                                    "body": body}) + "\n")
        path = path.split("?")[0]  # routing ignores query strings (omp's Anthropic client adds ?beta=true)
        if path.startswith("/deny") and self.headers.get("Copilot-Integration-Id") == "copilot-chat":
            return self.reply(403, {"error": {"message": "integration not allowed", "code": "forbidden"}})
        if path.endswith("/v1/systemone") or "/ai/run/" in path:
            answers = {}
            for qid, q in (body.get("questions") or {}).items():
                t = q.get("type")
                if t == "bool":
                    answers[qid] = {"type": "bool", "bool": 0.9}
                elif t == "choice":
                    first = next(iter(q.get("criteria") or {"a": ""}))
                    answers[qid] = {"type": "choice", "choice": first, "probabilities": {first: 1}, "confidence": 1}
                else:
                    answers[qid] = {"type": "score", "score": 2, "probabilities": {"2": 1}, "confidence": 1}
            res = {"model": body.get("model"), "answers": answers, "usage": {"input_tokens": 10, "output_tokens": 2}}
            return self.reply(200, {"result": res, "success": True, "errors": []} if "/ai/run/" in path else res)
        if path.endswith("/responses/compact"):
            # OpenAI remote compaction: the retained user turns plus an opaque compaction item
            users = [i for i in body.get("input") or [] if i.get("type") == "message" and i.get("role") == "user"]
            out = users[-1:] + [{"type": "compaction", "id": "cmp_1", "encrypted_content": "ENC:%d" % len(body.get("input") or [])}]
            return self.reply(200, {"id": "resp_c", "object": "response.compaction", "output": out,
                                    "usage": {"input_tokens": 1234, "output_tokens": 56}})
        kind, text = last_turn(body)
        if path.endswith("/messages") and body.get("compaction") is not None:
            # Anthropic on-demand compaction: a signed compaction block, stop_reason "compaction"
            self.sse_start()
            self.ev({"type": "message_start", "message": {"id": "m1", "role": "assistant", "model": body.get("model"), "content": [],
                                                          "usage": {"input_tokens": 900, "output_tokens": 0}}}, "message_start")
            self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "compaction", "content": "Native summary of the session.",
                                                                                  "signature": "SIG123"}}, "content_block_start")
            self.ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
            self.ev({"type": "message_delta", "delta": {"stop_reason": "compaction"}, "usage": {"output_tokens": 40}}, "message_delta")
            self.ev({"type": "message_stop"}, "message_stop")
            return self.end()
        if path.endswith("/chat/completions"):
            self.sse_start()
            if kind == "user" and text.startswith(("bash:", "call:")):
                if text.startswith("call:"):
                    name, _, args = text[5:].partition(":")
                else:
                    name, args = "bash", json.dumps({"command": text[5:]})
                tc = {"index": 0, "id": "call_1", "type": "function", "function": {"name": name, "arguments": args}}
                self.ev({"choices": [{"index": 0, "delta": {"tool_calls": [tc]}}]})
                self.ev({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
            else:
                answer = ("Tool said: " + text.strip()) if kind == "tool" else "plain provider answer"
                self.ev({"choices": [{"index": 0, "delta": {"content": answer}}]})
                self.ev({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": {"prompt_tokens": 5, "completion_tokens": 3}})
            self.chunk(b"data: [DONE]\n\n")
            return self.end()
        if path.endswith("/responses"):
            self.sse_start()
            item = {"type": "message", "id": "msg_1", "role": "assistant", "status": "in_progress", "content": []}
            self.ev({"type": "response.created", "response": {"id": "resp_1"}})
            self.ev({"type": "response.output_item.added", "output_index": 0, "item": item})
            self.ev({"type": "response.output_text.delta", "output_index": 0, "content_index": 0, "delta": "plain provider answer"})
            done = dict(item, status="completed", content=[{"type": "output_text", "text": "plain provider answer"}])
            self.ev({"type": "response.output_item.done", "output_index": 0, "item": done})
            self.ev({"type": "response.completed", "response": {"id": "resp_1", "status": "completed", "output": [done],
                                                                  "usage": {"input_tokens": 5, "output_tokens": 3}}})
            return self.end()
        if path.endswith("/messages"):
            self.sse_start()
            self.ev({"type": "message_start", "message": {"id": "m1", "role": "assistant", "content": [], "usage": {"input_tokens": 5, "output_tokens": 0}}}, "message_start")
            if kind == "user" and text.startswith(("bash:", "read:")):
                tool, args = ("bash", {"command": text[5:]}) if text.startswith("bash:") else ("read", {"path": text[5:]})
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_1", "name": tool, "input": {}}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": json.dumps(args)}}, "content_block_delta")
                stop = "tool_use"
            else:
                answer = ("Tool said: " + text.strip()) if kind == "tool" else "plain provider answer"
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": answer}}, "content_block_delta")
                stop = "end_turn"
            self.ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
            self.ev({"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 3}}, "message_delta")
            self.ev({"type": "message_stop"}, "message_stop")
            return self.end()
        return self.reply(404, {"error": {"message": "not found: " + path}})


class S(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # clients closing kept-alive connections


S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
