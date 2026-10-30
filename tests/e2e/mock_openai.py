#!/usr/bin/env python3
"""Dev-only mock OpenAI Chat Completions server for omc end-to-end tests.
Scripted behaviour per request (decided from the last message):
  - last message from user  -> stream a tool call (tool chosen by the user text: "bash:<cmd>", "write:<path>:<text>", "read:<path>", "glob:<pat>", "grep:<pat>")
  - last message is a tool result -> stream final text echoing the tool output
Every request body is appended to $MOCK_LOG (JSONL) for assertions."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")

def sse(handler, obj):
    handler.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
    handler.wfile.flush()

def text_of(msg):
    c = msg.get("content")
    if isinstance(c, str): return c
    if isinstance(c, list): return "".join(p.get("text", "") for p in c if isinstance(p, dict))
    return ""

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
        if LOG:
            with open(LOG, "a") as f: f.write(json.dumps(body) + "\n")
        msgs = body.get("messages", [])
        last = msgs[-1] if msgs else {}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        chunks = []
        base = {"id": "c1", "object": "chat.completion.chunk", "model": body.get("model")}
        sysc = text_of(msgs[0]) if msgs and msgs[0].get("role") in ("system", "developer") else ""
        advisor = "peer-shadow" in sysc
        if advisor and last.get("role") == "tool":
            chunks.append({**base, "choices": [{"index": 0, "delta": {"content": "noted"}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        elif last.get("role") == "tool":
            out = text_of(last)
            words = ("Tool said: " + out.strip()).split(" ")
            for i, w in enumerate(words):
                chunks.append({**base, "choices": [{"index": 0, "delta": {"content": (" " if i else "") + w}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        else:
            u = text_of(last)
            if u.startswith("title:") or "Generate" in (body.get("messages", [{}])[0].get("content") or "")[:0]:
                pass
            kind, _, rest = u.partition(":")
            if "<system-interrupt" in u:
                kind, rest = "say", "complied with rule"
            if advisor:
                kind, rest = ("call", 'advise:{"note":"Check the empty-input edge case","severity":"concern"}') if "ADVISE_ME" in u else ("say", "silent")
            elif "<advisory" in u:
                kind, rest = "say", "acknowledged advisory"
            if kind == "bash": name, args = "bash", {"command": rest}
            elif kind == "write":
                p, _, t = rest.partition(":"); name, args = "write", {"path": p, "content": t}
            elif kind == "read": name, args = "read", {"path": rest}
            elif kind == "glob": name, args = "glob", {"path": rest}
            elif kind == "grep": name, args = "grep", {"pattern": rest}
            elif kind == "call":
                tname, _, a = rest.partition(":")
                name, args = tname, json.loads(a or "{}")
            elif kind == "say":
                for w in rest.split(" "):
                    chunks.append({**base, "choices": [{"index": 0, "delta": {"content": w + " "}}]})
                chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
                name = None
            elif kind == "edit": name, args = "edit", {"input": rest.replace("\\n", "\n")}
            elif kind == "todo": name, args = "todo", {"op": "init", "items": rest.split(",")}
            else:
                chunks.append({**base, "choices": [{"index": 0, "delta": {"content": "plain answer"}}]})
                chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
                name = None
            if name:
                a = json.dumps(args)
                chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_1", "type": "function", "function": {"name": name, "arguments": ""}}]}}]})
                for i in range(0, len(a), 7):
                    chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "function": {"arguments": a[i:i+7]}}]}}]})
                chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
        chunks.append({**base, "choices": [], "usage": {"prompt_tokens": 100, "completion_tokens": 20, "total_tokens": 120}})
        def w(b):
            self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
        for c in chunks: w(b"data: " + json.dumps(c).encode() + b"\n\n")
        w(b"data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    class S(ThreadingHTTPServer):
        daemon_threads = True
        def handle_error(self, request, client_address):
            pass  # clients closing pooled keep-alive connections is expected
    S(("127.0.0.1", port), H).serve_forever()
