#!/usr/bin/env python3
"""Dev-only mock of Anthropic message threads (`message-threads-2026-08-12`) for tests/e2e/provider_threads.sh and
side-by-side runs against omp. It stores each threaded conversation like the API does and rebuilds the full
conversation of a continue, so a test can check that the client's history and the server's agree.

POST .../v1/messages logs {path, headers, body, kind, full} to $MOCK_LOG: kind is "plain", "create" or "continue", and
full is the rebuilt message list. A continue must name an unused stored id, carry appended messages only and omit
`system` and `tools` (unless the request is a Bearer one).

The last user text picks the reply: "bash:<cmd>" a bash tool call ("many:<n>" n parallel calls), a tool result "done",
anything else plain text. Scenarios in the latest user text:
  refuse-continue      answers the first continue with 404 thread_not_found
  refuse-twice         every continue gets 404 thread_expired
  unsupported          any thread request gets 400 thread_unsupported_request
  plain400             any thread request gets a 400 without a thread code
  paused               stop_reason pause_turn
  chain:<n>            (the first prompt) n rounds of text plus a bash call
  thinky               each reply opens with a signed thinking block"""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
HELD = {}  # message id -> conversation stored under it
USED = set()
NEXT = [0]
REFUSED = set()


def strip_cc(v):
    if isinstance(v, dict):
        return {k: strip_cc(x) for k, x in v.items() if k != "cache_control"}
    if isinstance(v, list):
        return [strip_cc(x) for x in v]
    return v


def user_text(msgs):
    for m in reversed(msgs or []):
        if m.get("role") != "user":
            continue
        c = m.get("content")
        if isinstance(c, str):
            return c
        texts = [b.get("text", "") for b in c if b.get("type") == "text"]
        return texts[-1] if texts else ""
    return ""


def first_prompt(msgs):
    for m in msgs or []:
        if m.get("role") == "user":
            c = m.get("content")
            if isinstance(c, str):
                return c
            texts = [b.get("text", "") for b in c if b.get("type") == "text"]
            return texts[-1] if texts else ""
    return ""


class Server(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # a client killed mid-stream is expected


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def error(self, status, message, code=None):
        err = {"type": "not_found_error" if status == 404 else "invalid_request_error", "message": message}
        if code:
            err["details"] = {"error_code": code}
        b = json.dumps({"type": "error", "error": err}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        th = body.get("thread")
        kind = th.get("type") if th else "plain"
        msgs = body.get("messages") or []
        full = msgs
        problem = None
        if th and "message-threads-2026-08-12" not in (self.headers.get("anthropic-beta") or ""):
            problem = "thread without its beta"
        if kind == "continue":
            pid = th.get("previous_message_id")
            full = HELD.get(pid, []) + msgs
            bearer = (self.headers.get("authorization") or "").lower().startswith("bearer")
            if pid not in HELD:
                problem = "unknown previous_message_id"
            elif pid in USED:
                problem = "previous_message_id already continued"
            elif not bearer and ("system" in body or "tools" in body):
                problem = "continue resends system or tools"
            USED.add(pid)
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()},
                                    "body": body, "kind": kind, "full": strip_cc(full), "problem": problem}) + "\n")
        prompt = first_prompt(full)
        if th and "unsupported" in prompt:
            return self.error(400, "thread: unsupported for this organization", "thread_unsupported_request")
        if th and "plain400" in prompt:
            return self.error(400, "messages: something the client cannot explain")
        if kind == "continue" and "refuse-twice" in prompt:
            return self.error(404, "thread expired", "thread_expired")
        if kind == "continue" and "refuse-continue" in prompt and prompt not in REFUSED:
            REFUSED.add(prompt)
            return self.error(404, "thread not found", "thread_not_found")
        if problem:
            return self.error(400, problem, "thread_invalid_continue")
        self.reply(body, full, kind, prompt)

    def reply(self, body, full, kind, prompt):
        NEXT[0] += 1
        mid = "msg_%d" % NEXT[0]
        last = full[-1] if full else {}
        results = isinstance(last.get("content"), list) and any(b.get("type") == "tool_result" for b in last["content"])
        text = user_text(full)
        blocks = []
        stop = "end_turn"
        rounds = sum(1 for m in full if m.get("role") == "assistant")
        if "thinky" in prompt:
            blocks.append({"type": "thinking", "thinking": "let me think %d" % NEXT[0], "signature": "sig%d" % NEXT[0]})
        if text.startswith("chain:") and rounds < int(text[6:].split()[0]) or (results and prompt.startswith("chain:") and rounds < int(prompt[6:].split()[0])):
            blocks.append({"type": "text", "text": "step %d" % rounds})
            blocks.append({"type": "tool_use", "id": "toolu_%d" % NEXT[0], "name": "bash", "input": {"command": "echo %d" % rounds}})
            stop = "tool_use"
        elif results:
            blocks.append({"type": "text", "text": "done"})
        elif text.startswith("bash:"):
            blocks.append({"type": "tool_use", "id": "toolu_%d" % NEXT[0], "name": "bash", "input": {"command": text[5:]}})
            stop = "tool_use"
        elif text.startswith("many:"):
            for i in range(int(text[5:].split()[0])):
                blocks.append({"type": "tool_use", "id": "toolu_%d_%d" % (NEXT[0], i), "name": "bash", "input": {"command": "echo %d" % i}})
            stop = "tool_use"
        else:
            blocks.append({"type": "text", "text": "plain answer"})
        if "paused" in prompt:
            stop = "pause_turn"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        cached = sum(1 for _ in full)
        ev = [{"type": "message_start", "message": {"id": mid, "type": "message", "role": "assistant", "model": body.get("model"),
               "content": [], "usage": {"input_tokens": 10, "output_tokens": 0, "cache_read_input_tokens": cached}}}]
        for i, b in enumerate(blocks):
            if b["type"] == "thinking":
                ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "thinking", "thinking": "", "signature": ""}})
                ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "thinking_delta", "thinking": b["thinking"]}})
                ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "signature_delta", "signature": b["signature"]}})
            elif b["type"] == "text":
                ev.append({"type": "content_block_start", "index": i, "content_block": {"type": "text", "text": ""}})
                ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "text_delta", "text": b["text"]}})
            else:
                ev.append({"type": "content_block_start", "index": i, "content_block": dict(b, input={})})
                ev.append({"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": json.dumps(b["input"])}})
            ev.append({"type": "content_block_stop", "index": i})
        ev.append({"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 5}})
        ev.append({"type": "message_stop"})
        for e in ev:
            frame = ("event: %s\ndata: %s\n\n" % (e["type"], json.dumps(e))).encode()
            self.wfile.write(b"%x\r\n%s\r\n" % (len(frame), frame))
        self.wfile.write(b"0\r\n\r\n")
        if body.get("thread"):
            HELD[mid] = full + [{"role": "assistant", "content": blocks}]


if __name__ == "__main__":
    Server(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
