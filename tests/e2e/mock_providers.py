#!/usr/bin/env python3
"""Dev-only mock for provider request shaping (tests/e2e/providers_transport.sh). omc reaches it through
OMC_PROVIDER_URL_REWRITE, so each real origin maps to a path prefix here and the logged path shows which URL omc
built. Every request is appended to $MOCK_LOG as {method, path, headers (lower-cased), body}.

Protocols (chosen by path suffix):
  .../chat/completions  OpenAI Chat Completions SSE      .../responses  OpenAI Responses SSE
  .../messages          Anthropic Messages SSE           .../v1/systemone, .../decisions, .../ai/run/<model>  native judgments
A user message "bash:<cmd>" gets a bash tool call (chat completions / anthropic), "read:<path>" a read call (anthropic), "call:<tool>:<json args>" any tool
(chat completions), a tool result gets the final text.
.../images/generations, .../images/edits, .../images: a one-image b64_json answer (HTTP 500 when the body mentions
"fail"). An Anthropic request forcing a named tool (tool_choice {type: "tool"}) gets a call to it (think: fixed thoughts).
MOCK_CACHE_TOKENS=<n> reports <n> cache-write tokens on a new Anthropic request and <n> cache-read tokens on a repeated one.
Anthropic "drop:<mode>:<n>:<text>" (the latest user message): the first <n> requests stream <text> and then drop the
connection ("stall": silent for $MOCK_STALL_SECS (60), "close": FIN before the last chunk, "rst": TCP reset); later
requests answer "resumed after drop" (turn recovery checks, tests/e2e/stall.sh).
Anthropic "think:<x>" streams unsigned reasoning before the answer; "refusecot:<x>" is refused (stop_details category
reasoning_extraction, before any output) while an assistant text block replays that reasoning (tests/e2e/refusal.sh).
Paths under /deny answer 403 to Copilot-Integration-Id copilot-chat (identity fallback test)."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
SEEN = set()
REFUSED = set()  # refuse: prompts already refused once
DROPS = {}  # drop command -> requests dropped so far
CARRIED_THOUGHT = "Private chain of thought."  # think: reasoning; refusecot: refuses a request replaying it as text


def latest_user_text(body):
    for m in reversed(body.get("messages") or []):
        if m.get("role") != "user":
            continue
        c = m.get("content")
        t = c if isinstance(c, str) else "".join(x.get("text", "") for x in c or [] if isinstance(x, dict))
        t = t.rsplit("</system-reminder>", 1)[-1].strip()  # omp prepends the date/cwd reminder to the prompt
        if t and not t.startswith("<system"):
            return t
    return ""


def drop_connection(handler, mode):
    import select, socket, struct, time
    sock = handler.connection
    if mode == "stall":
        end = time.time() + float(os.environ.get("MOCK_STALL_SECS") or 60)
        while time.time() < end:
            r, _, _ = select.select([sock], [], [], 0.2)
            try:
                if r and not sock.recv(1, socket.MSG_PEEK):
                    break
            except OSError:
                break
    elif mode == "rst":
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    else:
        sock.shutdown(socket.SHUT_RDWR)
    handler.close_connection = True
    try:
        sock.close()
    except OSError:
        pass


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
        if path.endswith("/decisions") and isinstance(body.get("questions"), list):
            # OpenAI Decisions: questions and answers are arrays
            out = []
            for q in body["questions"]:
                t, name = q.get("type"), q.get("name")
                if t == "predicate":
                    out.append({"type": t, "name": name, "probability": 0.9})
                elif t == "choice":
                    first = q["choices"][0]["value"]
                    out.append({"type": t, "name": name, "choice": first, "probabilities": [{"value": first, "probability": 1}], "confidence": 1})
                else:
                    out.append({"type": t, "name": name, "score": 2, "probabilities": [{"value": 2, "probability": 1}], "confidence": 1})
            return self.reply(200, {"model": body.get("model"), "answers": out, "usage": {"input_tokens": 10, "output_tokens": 2}})
        if path.endswith("/v1/systemone") or path.endswith("/decisions") or "/ai/run/" in path:
            answers = {}
            for qid, q in (body.get("questions") or {}).items():
                t = q.get("type")
                if t == "bool" or t == "noul":
                    answers[qid] = {"type": t, t: 0.9}
                elif t == "choice":
                    first = next(iter(q.get("criteria") or {"a": ""}))
                    answers[qid] = {"type": "choice", "choice": first, "probabilities": {first: 1}, "confidence": 1}
                else:
                    answers[qid] = {"type": "score", "score": 2, "probabilities": {"2": 1}, "confidence": 1}
            res = {"model": body.get("model"), "answers": answers, "usage": {"input_tokens": 10, "output_tokens": 2}}
            return self.reply(200, {"result": res, "success": True, "errors": []} if "/ai/run/" in path else res)
        if path.endswith("/images/generations") or path.endswith("/images/edits") or path.endswith("/images"):
            # image generation (openai-images / openrouter-images): one 4x3 PNG, or an error for prompts asking for it
            if "fail" in json.dumps(body):
                return self.reply(500, {"error": {"message": "image backend down"}})
            png = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGP4z8AARww4OQD1MQv1NXv7ggAAAABJRU5ErkJggg=="
            return self.reply(200, {"created": 1, "data": [{"b64_json": png}], "usage": {"input_tokens": 12, "output_tokens": 34}})
        if path.endswith("/responses/compact"):
            # OpenAI remote compaction: the retained user turns plus an opaque compaction item
            users = [i for i in body.get("input") or [] if i.get("type") == "message" and i.get("role") == "user"]
            out = users[-1:] + [{"type": "compaction", "id": "cmp_1", "encrypted_content": "ENC:%d" % len(body.get("input") or []),
                                 "internal_chat_message_metadata_passthrough": {"turn_id": "turn_0"}}]
            return self.reply(200, {"id": "resp_c", "object": "response.compaction", "output": out,
                                    "usage": {"input_tokens": 1234, "output_tokens": 56, "total_tokens": 1290,
                                              "input_tokens_details": {"cached_tokens": 1000}}})
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
        if path.endswith("/responses") and any(t.get("type") == "image_generation" for t in body.get("tools") or []):
            # hosted image generation over a Responses carrier: a non-streaming image_generation_call
            png = "iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAEElEQVR4nGP4z8AARww4OQD1MQv1NXv7ggAAAABJRU5ErkJggg=="
            tool = body["tools"][0]
            return self.reply(200, {"id": "resp_img", "output": [
                {"type": "image_generation_call", "result": png, "size": tool.get("size", "1024x1024"), "quality": "high"},
                {"type": "message", "content": [{"type": "output_text", "text": "Here is the image."}]}],
                "tools": [{"type": "image_generation", "model": "gpt-image-2-2026"}], "usage": {"input_tokens": 7, "output_tokens": 9}})
        if path.endswith("/responses"):
            self.sse_start()
            last = (body.get("input") or [{}])[-1]
            ltext = next((p.get("text", "") for p in reversed(last.get("content") or []) if isinstance(p, dict)), "") if isinstance(last.get("content"), list) else ""
            if ltext.startswith("bash:"):  # the declared name of the bash tool, as the host would return it
                name = next((t["name"] for t in body.get("tools") or [] if t.get("name", "").endswith("bash")), "bash")
                call = {"type": "function_call", "id": "fc_1", "call_id": "call_1", "name": name, "arguments": json.dumps({"command": ltext[5:]})}
                self.ev({"type": "response.created", "response": {"id": "resp_1"}})
                self.ev({"type": "response.output_item.added", "output_index": 0, "item": dict(call, arguments="")})
                self.ev({"type": "response.output_item.done", "output_index": 0, "item": call})
                self.ev({"type": "response.completed", "response": {"id": "resp_1", "status": "completed", "output": [call],
                                                                      "usage": {"input_tokens": 5, "output_tokens": 3}}})
                return self.end()
            item = {"type": "message", "id": "msg_1", "role": "assistant", "status": "in_progress", "content": []}
            self.ev({"type": "response.created", "response": {"id": "resp_1"}})
            self.ev({"type": "response.output_item.added", "output_index": 0, "item": item})
            self.ev({"type": "response.output_text.delta", "output_index": 0, "content_index": 0, "delta": "plain provider answer"})
            done = dict(item, status="completed", content=[{"type": "output_text", "text": "plain provider answer"}])
            self.ev({"type": "response.output_item.done", "output_index": 0, "item": done})
            self.ev({"type": "response.completed", "response": {"id": "resp_1", "status": "completed", "output": [done],
                                                                  "usage": {"input_tokens": 5, "output_tokens": 3}}})
            return self.end()
        dropu = latest_user_text(body)
        if path.endswith("/messages") and dropu.startswith("drop:"):
            _, mode, n, dtext = (dropu.split(":", 3) + ["", "", ""])[:4]
            if DROPS.get(dropu, 0) < int(n or 1):
                DROPS[dropu] = DROPS.get(dropu, 0) + 1
                self.sse_start()
                self.ev({"type": "message_start", "message": {"id": "m1", "role": "assistant", "content": [],
                                                              "usage": {"input_tokens": 5, "output_tokens": 0}}}, "message_start")
                if dtext:
                    self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}, "content_block_start")
                    self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": dtext}}, "content_block_delta")
                return drop_connection(self, mode)
            if kind == "user":
                text = "resumed after drop"
        if path.endswith("/messages") and body.get("stream") is False and body.get("max_tokens") == 0:
            # a prompt-cache pre-warm: usage only, no content (MOCK_PREWARM_400 refuses it like a gateway would)
            if os.environ.get("MOCK_PREWARM_400"):
                return self.reply(400, {"type": "error", "error": {"type": "invalid_request_error", "message": "max_tokens: must be >= 1"}})
            usage = {"input_tokens": 5, "output_tokens": 0}
            cache = int(os.environ.get("MOCK_CACHE_TOKENS", "0"))
            if cache:
                key = json.dumps([body.get("system"), body.get("tools"), body.get("messages")], sort_keys=True)
                usage["cache_read_input_tokens" if key in SEEN else "cache_creation_input_tokens"] = cache
                SEEN.add(key)
            return self.reply(200, {"id": "msg_pw", "type": "message", "role": "assistant", "content": [], "stop_reason": "max_tokens", "usage": usage})
        if path.endswith("/messages"):
            self.sse_start()
            usage = {"input_tokens": 5, "output_tokens": 0}
            cache = int(os.environ.get("MOCK_CACHE_TOKENS", "0"))
            if cache:  # prompt-cache warming: a replayed prefix reads the entry the first request wrote
                key = json.dumps([body.get("system"), body.get("tools"), body.get("messages")], sort_keys=True)
                usage["cache_read_input_tokens" if key in SEEN else "cache_creation_input_tokens"] = cache
                SEEN.add(key)
            self.ev({"type": "message_start", "message": {"id": "m1", "role": "assistant", "content": [], "usage": usage}}, "message_start")
            forced = body.get("tool_choice") or {}
            if kind == "user" and forced.get("type") == "tool":  # a forced named tool (the think scratchpad)
                args = {"thoughts": "Weighing **one** option.\nThen answering."} if forced.get("name", "").lstrip("_") == "think" else {}
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_f", "name": forced["name"], "input": {}}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": json.dumps(args)}}, "content_block_delta")
                stop = "tool_use"
            elif kind == "user" and latest_user_text(body).startswith("think:"):
                # unsigned reasoning, as a model that is not Claude writes it, then the answer
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": CARRIED_THOUGHT}}, "content_block_delta")
                self.ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
                self.ev({"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "plain provider answer"}}, "content_block_delta")
                self.ev({"type": "content_block_stop", "index": 1}, "content_block_stop")
                self.ev({"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 3}}, "message_delta")
                self.ev({"type": "message_stop"}, "message_stop")
                return self.end()
            elif kind == "user" and latest_user_text(body).startswith("refusecot:") and any(
                    CARRIED_THOUGHT in (b.get("text") or "") for m in body.get("messages") or [] if m.get("role") == "assistant"
                    for b in (m.get("content") if isinstance(m.get("content"), list) else [])):
                # the classifier refuses a request replaying another model's reasoning as text, before any output
                self.ev({"type": "message_delta", "delta": {"stop_reason": "refusal", "stop_details": {"type": "refusal",
                         "category": "reasoning_extraction", "explanation": "declined"}},
                         "usage": {"output_tokens": 0}}, "message_delta")
                self.ev({"type": "message_stop"}, "message_stop")
                return self.end()
            elif kind == "user" and latest_user_text(body).startswith("refuse:") and latest_user_text(body) not in REFUSED:
                # a classifier refusal that stops output already streaming (once per prompt text)
                REFUSED.add(latest_user_text(body))
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "partial before refusal"}}, "content_block_delta")
                self.ev({"type": "content_block_stop", "index": 0}, "content_block_stop")
                self.ev({"type": "message_delta", "delta": {"stop_reason": "refusal", "stop_details": {"type": "refusal",
                         "category": "reasoning_extraction", "explanation": "declined", "fallback_credit_token": "fct-1"}},
                         "usage": {"output_tokens": 3}}, "message_delta")
                self.ev({"type": "message_stop"}, "message_stop")
                return self.end()
            elif kind == "user" and text.startswith(("bash:", "read:")):
                tool, args = ("bash", {"command": text[5:]}) if text.startswith("bash:") else ("read", {"path": text[5:]})
                self.ev({"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_1", "name": tool, "input": {}}}, "content_block_start")
                self.ev({"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": json.dumps(args)}}, "content_block_delta")
                stop = "tool_use"
            else:
                answer = ("Tool said: " + text.strip()) if kind == "tool" else \
                    "resumed after drop" if text == "resumed after drop" else "plain provider answer"
                if "Write a handoff document" in latest_user_text(body):
                    answer = "## Goal\nplain provider answer"  # a handoff document opens with its Goal heading
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
