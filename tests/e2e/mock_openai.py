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
  - "seq:[[tool, args], ...]" one tool call per turn, in order, then the usual "Tool said:" answer
  - "batch:[[tool, args], ...]" all the tool calls in one assistant message, then the usual "Tool said:" answer
  - model "mock-flaky"        every request fails with HTTP 429 (retry fallback chains)
  - a compaction summary (tool-less) or handoff-document request whose messages mention SLOWSUMMARY answers after 4 s
Eval helper side requests (omc eval completion()/judge()):
  - completion (system "You are a helpful assistant." or a forced `respond` tool) -> "completion says: <prompt>", or a
    `respond` call whose fields echo the prompt (strings) / 42 (numbers) / true (booleans)
  - judgment (user text "State:\n...") -> keyword answers: choice = first option label named in the state (else the
    first option), yes/no = YES when the state contains "yes", score = first digit in the state (else 0); a state
    containing SLOWJUDGE answers after 1.5 s, FAILJUDGE answers gibberish (unparseable even on retries)
$MOCK_DELAY (seconds, default 0) is slept between streamed chunks.
$MOCK_SLOT_SECS / $MOCK_THINK_SECS model a single-slot local server (llama.cpp with one slot): requests run one at a
  time, each waiting MOCK_SLOT_SECS (prefill) plus MOCK_THINK_SECS when it would think (no `enable_thinking: false`).
  $MOCK_TIMELOG gets one JSON line per request arrival and per slot start (kind main/title/compress/side, bytes, queue
  time), so tests can pin the order and timing of side requests against the main request.
$MOCK_FAIL_RETRY_AFTER_MS (default "0"): `retry-after-ms` header on retryable "fail:" replies (408/429/5xx) so the
  transport's own retries (6 x 500 ms*2^n per attempt, like omp) don't slow the suite; "none" omits it (real backoff).
  A header of 0 is a retry-now hint: it never reaches the error message (omp's retry-after-ms= suffix needs > 0).
Memory pipeline side requests (memory.backend=local startup):
  - stage-1 extraction (system "Memory-stage-one extractor") -> strict JSON {rollout_summary, rollout_slug, raw_memory}
  - stage-2 consolidation (system "memory-stage-two consolidator") -> {memory_md, memory_summary, skills:[mock-skill]}
Subagent turns ("Complete assignment thoroughly:\n\n<assignment>") run <assignment> as the script (e.g. slow:3:done).
Idle recap side turns ("<recap>…") answer a fixed one-line recap.
/agents architect requests ("Custom agent request:…" containing AGENTSPEC) answer a docs-writer agent spec as JSON.
Mid-stream connection drops (turn recovery), driven by the latest user message:
  - "drop:<mode>:<n>:<text>"   the first <n> requests stream <text> (may be empty) and then drop the connection;
    later requests answer "resumed after drop" (or "Tool said: ..." after a tool result)
  - "dropcall:<mode>:<n>:<cmd>" the same, but a complete bash tool call (<cmd>) streams before the drop
  (counted per mock process; $MOCK_DROPS_FROM_HISTORY counts the failed turns kept in the request instead, so two
  clients can share the mock)
  <mode>: "stall" (connection stays open, silent, for $MOCK_STALL_SECS, default 60), "close" (FIN mid-body, no
  terminating chunk) or "rst" (TCP reset mid-body).
/btw side questions ("<btw>…Question:\n<q>\n</btw>"): <q> may be md:/think:/slow:/streamfail:/fail:, else a fixed
  markdown answer naming <q>.
Every request body is appended to $MOCK_LOG (JSONL) for assertions."""
import json, os, re, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
DELAY = float(os.environ.get("MOCK_DELAY") or 0)
FAIL_RETRY_AFTER = os.environ.get("MOCK_FAIL_RETRY_AFTER_MS", "0")
STALL_SECS = float(os.environ.get("MOCK_STALL_SECS") or 60)
SLOT_SECS = float(os.environ.get("MOCK_SLOT_SECS") or 0)
THINK_SECS = float(os.environ.get("MOCK_THINK_SECS") or 0)
TIMELOG = os.environ.get("MOCK_TIMELOG")
SLOT = threading.Lock()
DROPS = {}  # drop command text -> requests dropped so far
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

def thinking_on(body):
    """Whether a Qwen-style local server would think: the template thinks unless `enable_thinking: false` arrives
    (top level or chat_template_kwargs) or the request asks for no reasoning effort."""
    kw = body.get("chat_template_kwargs") or {}
    if body.get("enable_thinking") is False or kw.get("enable_thinking") is False: return False
    return body.get("reasoning_effort") not in ("none",)

def req_kind(body):
    """main (tools + system prompt), title, compress (skill description), or side."""
    msgs = body.get("messages") or []
    last = text_of(msgs[-1]) if msgs else ""
    if "<title-request>" in last: return "title"
    if last.startswith("Compress into one routing hint"): return "compress"
    return "main" if body.get("tools") else "side"

def text_of(msg):
    c = msg.get("content")
    if isinstance(c, str): return c
    if isinstance(c, list):  # skip injected <system-reminder> blocks (omp prepends date/cwd)
        t = "".join(p.get("text", "") for p in c if isinstance(p, dict) and not p.get("text", "").startswith("<system-reminder>"))
        # `attach:<prompt>` after CLI @file attachments: drop the leading <file> blocks so <prompt> is the command
        m = re.match(r'^(?:<file name="[^"]*">.*?</file>\n)+attach:', t, flags=re.S)
        return t[m.end():] if m else t
    return ""

STEER_NUDGES = ("STOP: In NEXT reply", "Continue task now", "Before task complete, verify:")


def seq_next(msgs):
    """`seq:[[tool, args], ...]` user prompt: the call after the tool results so far (None when done)."""
    ui = max((i for i, m in enumerate(msgs) if m.get("role") == "user" and text_of(m).startswith("seq:")), default=None)
    if ui is None: return None
    try: calls = json.loads(text_of(msgs[ui])[4:])
    except ValueError: return None
    done = sum(1 for m in msgs[ui:] if m.get("role") == "tool")
    return calls[done] if done < len(calls) else None

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        body = json.loads(raw)
        if LOG:
            with open(LOG, "a") as f: f.write(json.dumps(body) + "\n")
        if not (SLOT_SECS or THINK_SECS or TIMELOG): return self.reply(body)
        thinks = thinking_on(body)
        arrived = time.time()
        if TIMELOG:
            with open(TIMELOG, "a") as f:
                f.write(json.dumps({"t": arrived, "event": "arrive", "kind": req_kind(body), "bytes": len(raw), "thinks": thinks}) + "\n")
        if not (SLOT_SECS or THINK_SECS): return self.reply(body)
        with SLOT:  # one llama.cpp-style slot: requests run one at a time, prefill + (optional) thinking first
            start = time.time()
            time.sleep(SLOT_SECS + (THINK_SECS if thinks else 0))
            if TIMELOG:
                with open(TIMELOG, "a") as f:
                    f.write(json.dumps({"t": time.time(), "event": "start", "kind": req_kind(body), "queued": start - arrived}) + "\n")
            self.reply(body)

    def reply(self, body):
        msgs = body.get("messages", [])
        # hidden steering nudges (prewalk plan/continue/checklist) trail the turn: answer what precedes them
        while len(msgs) > 1 and msgs[-1].get("role") in ("user", "system", "developer") and \
                text_of(msgs[-1]).lstrip().startswith(STEER_NUDGES):
            msgs = msgs[:-1]
        last = msgs[-1] if msgs else {}
        lu = text_of(last) if last.get("role") == "user" else ""
        if lu.startswith("<btw>") and "\nQuestion:\n" in lu:  # /btw side question: "fail:" applies to its question
            lu = lu.split("\nQuestion:\n", 1)[1].rsplit("\n</btw>", 1)[0]
        if body.get("model") == "mock-flaky":  # always rate limited (retry fallback chains)
            lu = "fail:429:Rate limit exceeded for mock-flaky"
        if lu.startswith("fail:"):
            st, _, m = lu[5:].partition(":")
            b = json.dumps({"error": {"message": m or "mock failure", "type": "invalid_request_error", "code": int(st or 400)}}).encode()
            code = int(st or 400)
            self.send_response(code)
            if (code >= 500 or code in (408, 429)) and FAIL_RETRY_AFTER != "none":
                self.send_header("retry-after-ms", FAIL_RETRY_AFTER)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        dropu = next((text_of(m) for m in reversed(msgs) if m.get("role") == "user" and
                      not text_of(m).startswith("<system")), "")
        if dropu.startswith(("drop:", "dropcall:")) and body.get("tools"):
            kind, mode, n, rest = (dropu.split(":", 3) + ["", "", ""])[:4]
            done = DROPS.get(dropu, 0)
            if os.environ.get("MOCK_DROPS_FROM_HISTORY"):  # two clients share the mock (tests/tui/compare.sh)
                ui = max(i for i, m in enumerate(msgs) if m.get("role") == "user" and text_of(m) == dropu)
                done = sum(1 for m in msgs[ui:] if m.get("role") == "assistant")
            if done < int(n or 1):
                DROPS[dropu] = DROPS.get(dropu, 0) + 1
                return self.drop_stream(body, kind, mode, rest)
            if last.get("role") != "tool":
                lu = "say:resumed after drop"
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
        if "SLOWSUMMARY" in json.dumps(msgs[1:]) and (not tnames or "Write a handoff document" in lu):
            time.sleep(4)  # compaction summary / handoff document request over a SLOWSUMMARY transcript
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
        # `calls:[[tool, args], ...]` scripts sequential tool calls: each request issues the next call not yet
        # answered after the scripted user message, then the turn ends with a plain answer
        ui = max((i for i, m in enumerate(msgs) if m.get("role") == "user" and text_of(m).startswith("calls:")), default=-1)
        if not side and ui >= 0 and not advisor and (last.get("role") == "tool" or ui == len(msgs) - 1):
            script = json.loads(text_of(msgs[ui])[6:])
            done = sum(1 for m in msgs[ui + 1:] if m.get("role") == "tool")
            side = ("call", script[done][0], script[done][1], f"call_{done + 1}") if done < len(script) else ("say", "script done")
        if not side and last.get("role") == "user" and text_of(last).startswith("batch:"):
            for k, (tname, targs) in enumerate(json.loads(text_of(last)[6:])):
                chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": k, "id": f"call_{k + 1}", "type": "function", "function": {"name": tname, "arguments": json.dumps(targs)}}]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
        elif side and side[0] == "say":
            chunks.append({**base, "choices": [{"index": 0, "delta": {"content": side[1]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        elif side:
            a = json.dumps(side[2])
            chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": side[3] if len(side) > 3 else "call_s", "type": "function", "function": {"name": side[1], "arguments": a}}]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
        elif advisor and last.get("role") == "tool":
            chunks.append({**base, "choices": [{"index": 0, "delta": {"content": "noted"}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        elif last.get("role") == "tool" and seq_next(msgs):
            tname, targs = seq_next(msgs)
            a = json.dumps(targs)
            n = sum(1 for m in msgs if m.get("role") == "tool")
            chunks.append({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": f"call_{n + 1}", "type": "function", "function": {"name": tname, "arguments": a}}]}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}]})
        elif last.get("role") == "tool":
            out = text_of(last)
            words = ("Tool said: " + out.strip()).split(" ")
            for i, w in enumerate(words):
                chunks.append({**base, "choices": [{"index": 0, "delta": {"content": (" " if i else "") + w}}]})
            chunks.append({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
        else:
            u = lu if lu.startswith("say:resumed after drop") else text_of(last)
            if u.startswith("Complete assignment thoroughly:\n\n"):  # subagent turn: the assignment is the script
                u = u[len("Complete assignment thoroughly:\n\n"):].strip()
            btw = u.startswith("<btw>") and "\nQuestion:\n" in u
            if btw:  # /btw side question: the question picks the display command, else a fixed markdown answer
                u = u.split("\nQuestion:\n", 1)[1].rsplit("\n</btw>", 1)[0]
                if u.partition(":")[0] not in ("md", "think", "slow", "streamfail"):
                    u = "say:**Side answer** for `" + u + "`:\n- first point\n- second point"
            if u.startswith("title:") or "Generate" in (body.get("messages", [{}])[0].get("content") or "")[:0]:
                pass
            kind, _, rest = u.partition(":")
            if "<system-interrupt" in u:
                kind, rest = "say", "complied with rule"
            if u.startswith("Compress into one routing hint"):  # skill description compression
                kind, rest = "say", "Use for demo skill test routing"
            elif "<title-request>" in u:  # fork title side request (omp/omc title.generator: fork)
                kind, rest = "say", '<title emoji="🧪" code="PARK">Fix flaky park tests</title>'
            elif u.startswith("Custom agent request:") and "AGENTSPEC" in u:  # /agents architect (agent-creation-user.md)
                kind, rest = "say", json.dumps({"identifier": "docs-writer", "whenToUse": "Use this agent when docs need writing.",
                                                "systemPrompt": "You write docs.\nKeep them short."})
            elif u.startswith("<recap>"):  # idle recap side turn (recap-user.md)
                kind, rest = "say", "Recap: goal is fixing the flaky park tests; next, rerun the suite."
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
            elif kind == "seq":
                name, args = json.loads(rest)[0]
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

    def drop_stream(self, body, kind, mode, text):
        """Stream part of a turn (text, or a complete bash call), then drop the connection per <mode>."""
        import socket, struct
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        base = {"id": "c1", "object": "chat.completion.chunk", "model": body.get("model")}
        def w(obj):
            b = b"data: " + json.dumps(obj).encode() + b"\n\n"
            self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
            self.wfile.flush()
        w({**base, "choices": [{"index": 0, "delta": {"role": "assistant", "content": ""}}]})
        if kind == "dropcall":
            a = json.dumps({"command": text})
            w({**base, "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "call_drop", "type": "function", "function": {"name": "bash", "arguments": a}}]}}]})
        else:
            for i in range(0, len(text), 16):
                w({**base, "choices": [{"index": 0, "delta": {"content": text[i:i + 16]}}]})
        time.sleep(0.2)
        sock = self.connection
        if mode == "stall":
            end = time.time() + STALL_SECS
            while time.time() < end:
                time.sleep(0.2)
                try:  # stop once the client gave up (its FIN/RST makes the socket readable)
                    import select
                    r, _, _ = select.select([sock], [], [], 0)
                    if r and not sock.recv(1, socket.MSG_PEEK): break
                except OSError:
                    break
        elif mode == "rst":
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        else:
            sock.shutdown(socket.SHUT_RDWR)
        self.close_connection = True
        try: sock.close()
        except OSError: pass

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
    class S(ThreadingHTTPServer):
        daemon_threads = True
        def handle_error(self, request, client_address):
            pass  # clients closing pooled keep-alive connections is expected
    S(("127.0.0.1", port), H).serve_forever()
