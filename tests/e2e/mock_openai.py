#!/usr/bin/env python3
"""Dev-only mock OpenAI Chat Completions server for omc end-to-end tests.
Scripted behaviour per request (decided from the last message):
  - last message from user  -> stream a tool call (tool chosen by the user text: "bash:<cmd>", "write:<path>:<text>", "read:<path>", "glob:<pat>", "grep:<pat>")
  - last message is a tool result -> stream final text echoing the tool output
Display commands (used by tests/tui/compare.sh to exercise transcript views):
  - "md:<name>"              stream tests/tui/fixtures/<name>.md as the answer (small chunks)
  - "think:<thinking>|<text>" stream reasoning_content deltas, then the text ("\\n" -> newline)
  - "slow:<secs>:<text>"     wait <secs> (loader visible), then stream <text> slowly
  - "fail:<status>:<msg>"    reply with an HTTP error (non-retryable for 400/401/403/404)
  - "streamfail:<text>"      stream <text>, then an in-stream error chunk
Eval helper side requests (omc eval completion()/judge()):
  - completion (system "You are a helpful assistant." or a forced `respond` tool) -> "completion says: <prompt>", or a
    `respond` call whose fields echo the prompt (strings) / 42 (numbers) / true (booleans)
  - judgment (user text "State:\n...") -> keyword answers: choice = first option label named in the state (else the
    first option), yes/no = YES when the state contains "yes", score = first digit in the state (else 0); a state
    containing SLOWJUDGE answers after 1.5 s, FAILJUDGE answers gibberish (unparseable even on retries)
$MOCK_DELAY (seconds, default 0) is slept between streamed chunks.
Memory pipeline side requests (memory.backend=local startup):
  - stage-1 extraction (system "Memory-stage-one extractor") -> strict JSON {rollout_summary, rollout_slug, raw_memory}
  - stage-2 consolidation (system "memory-stage-two consolidator") -> {memory_md, memory_summary, skills:[mock-skill]}
Every request body is appended to $MOCK_LOG (JSONL) for assertions."""
import json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
DELAY = float(os.environ.get("MOCK_DELAY") or 0)
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tui", "fixtures")

def sse(handler, obj):
    handler.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
    handler.wfile.flush()

def judge_reply(system, user):
    import re as _re
    state = user.split("\n\n")[0][len("State:\n"):] if user.startswith("State:\n") else user
    if "SLOWJUDGE" in state: time.sleep(1.5)
    if "FAILJUDGE" in state: return "I cannot decide"
    low = state.lower()
    def answer(section):
        labels = _re.findall(r"^- `([^`]+)`", section, _re.M)
        if "Levels, lowest to highest" in section:
            m = _re.search(r"\b(\d)\b", state)
            return str(min(int(m.group(1)), len(labels) - 1) if m else 0)
        if labels:
            for l in labels:
                if _re.search(r"\b" + _re.escape(l.lower()) + r"\b", low): return l
            return labels[0]
        return "YES" if "yes" in low else "NO"
    parts = _re.split(r"^Question `([^`]+)`: ", system, flags=_re.M)
    if len(parts) > 1:
        return "\n".join(f"{parts[i]}: {answer(parts[i + 1])}" for i in range(1, len(parts), 2))
    return answer(system)

def text_of(msg):
    c = msg.get("content")
    if isinstance(c, str): return c
    if isinstance(c, list):  # skip injected <system-reminder> blocks (omp prepends date/cwd)
        return "".join(p.get("text", "") for p in c if isinstance(p, dict) and not p.get("text", "").startswith("<system-reminder>"))
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
        lu = text_of(last) if last.get("role") == "user" else ""
        if lu.startswith("fail:"):
            st, _, m = lu[5:].partition(":")
            b = json.dumps({"error": {"message": m or "mock failure", "type": "invalid_request_error", "code": int(st or 400)}}).encode()
            self.send_response(int(st or 400))
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        chunks = []
        pace = DELAY
        base = {"id": "c1", "object": "chat.completion.chunk", "model": body.get("model")}
        sysc = text_of(msgs[0]) if msgs and msgs[0].get("role") in ("system", "developer") else ""
        advisor = "peer-shadow" in sysc
        tnames = [t.get("function", {}).get("name") for t in body.get("tools") or []]
        lu_all = text_of(last) if last.get("role") == "user" else ""
        side = None
        if last.get("role") == "user" and sysc.startswith("Memory-stage-one extractor"):
            side = ("say", json.dumps({"rollout_summary": "Mock rollout summary.", "rollout_slug": "Mock Slug!", "raw_memory": "- mock raw memory token_abcdefghij123456"}))
        elif last.get("role") == "user" and "memory-stage-two consolidator" in sysc:
            side = ("say", "Here you go:\n" + json.dumps({"memory_md": "# Mock MEMORY", "memory_summary": "MOCK SUMMARY from consolidation", "skills": [
                {"name": "Mock Skill", "content": "Mock skill body", "scripts": [{"path": "run.sh", "content": "echo mock"}, {"path": "../evil", "content": "x"}]}]}))
        elif last.get("role") == "user" and "respond" in tnames:
            params = next(t["function"].get("parameters") or {} for t in body["tools"] if t["function"]["name"] == "respond")
            vals = {"string": lu_all, "number": 42, "integer": 42, "boolean": True}
            side = ("call", "respond", {k: vals.get(v.get("type"), lu_all) for k, v in (params.get("properties") or {}).items()})
        elif last.get("role") == "user" and sysc == "You are a helpful assistant.":
            side = ("say", "completion says: " + lu_all)
        elif last.get("role") == "user" and lu_all.startswith("State:\n"):
            ans = judge_reply(sysc, lu_all)
            side = ("call", "submit_judgment", {"answer": ans}) if "submit_judgment" in tnames else ("say", ans)
        if side and side[0] == "say":
            chunks.append({**base, "choices": [{"index": 0, "delta": {"content": side[1]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        elif side:
            a = json.dumps(side[2])
            chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_s", "type": "function", "function": {"name": side[1], "arguments": a}}]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
        elif advisor and last.get("role") == "tool":
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
            if u.startswith("Compress into one routing hint"):  # skill description compression
                kind, rest = "say", "Use for demo skill test routing"
            elif "<title-request>" in u:  # fork title side request (omp/omc title.generator: fork)
                kind, rest = "say", '<title emoji="🧪" code="PARK">Fix flaky park tests</title>'
            elif "You name coding-agent sessions" in sysc:  # title model fallback
                kind, rest = "say", "<title>Fix flaky park tests</title>"
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
            elif kind in ("md", "think", "slow", "streamfail"):
                name = None
                d = lambda **k: {**base, "choices": [{"index": 0, "delta": k}]}
                if kind == "md":
                    with open(os.path.join(FIXTURES, os.path.basename(rest.strip()) + ".md")) as f: text = f.read()
                elif kind == "think":
                    th, _, text = rest.partition("|")
                    th = th.replace("\\n", "\n")
                    for i in range(0, len(th), 12): chunks.append(d(reasoning_content=th[i:i+12]))
                    text = text.replace("\\n", "\n")
                elif kind == "slow":
                    secs, _, text = rest.partition(":")
                    time.sleep(float(secs or 1)); pace = max(pace, 0.15)
                else:
                    text = rest
                for i in range(0, len(text), 16): chunks.append(d(content=text[i:i+16]))
                if kind == "streamfail":
                    chunks.append({"error": {"message": "mock stream failure", "type": "server_error"}})
                else:
                    chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
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
        for c in chunks:
            w(b"data: " + json.dumps(c).encode() + b"\n\n")
            if pace: self.wfile.flush(); time.sleep(pace)
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
