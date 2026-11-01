#!/usr/bin/env python3
"""Dev-only mock of Devin's Cascade API (Connect protocol over HTTP/1.1) for omc e2e tests.

Endpoints:
  POST /exa.auth_pb.AuthService/GetUserJwt              unary application/proto -> GetUserJwtResponse
  POST /exa.api_server_pb.ApiServerService/AssignModel  unary -> AssignModelResponse (router models)
  POST /exa.api_server_pb.ApiServerService/GetChatMessage  Connect server stream of GetChatMessageResponse
The chat answer is scripted from the last prompt: TOOL result -> "Tool said: <result>"; "bash:<cmd>" -> thinking
then a bash tool call (arguments streamed cumulatively over two frames); "fail" -> Connect end-stream error;
anything else -> "plain devin answer". Metadata.apiKey must equal $MOCK_DEVIN_KEY (default devin-test-key), else 401.
Every request is appended to $MOCK_LOG (JSONL) as {path, headers, decoded summary}."""
import gzip, json, os, re, struct, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
KEY = os.environ.get("MOCK_DEVIN_KEY", "devin-test-key")

# ---- minimal protobuf codec ----
def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1
        r |= (c & 0x7F) << s; s += 7
        if c < 0x80: return r, i

def fields(b):
    i, out = 0, []
    while i < len(b):
        tag, i = varint(b, i)
        no, wt = tag >> 3, tag & 7
        if wt == 0: v, i = varint(b, i)
        elif wt == 1: v = struct.unpack("<d", b[i:i + 8])[0]; i += 8
        elif wt == 2:
            n, i = varint(b, i); v = b[i:i + n]; i += n
        elif wt == 5: v = b[i:i + 4]; i += 4
        else: raise ValueError("wire type %d" % wt)
        out.append((no, wt, v))
    return out

def get(fs, no, default=None):
    for n, _, v in fs:
        if n == no: return v
    return default

def getall(fs, no): return [v for n, _, v in fs if n == no]
def s(v): return v.decode() if isinstance(v, (bytes, bytearray)) else (v or "")

def enc_varint(n):
    out = bytearray()
    while True:
        c = n & 0x7F; n >>= 7
        if n: out.append(c | 0x80)
        else: out.append(c); return bytes(out)

def f_len(no, data):
    if isinstance(data, str): data = data.encode()
    return enc_varint(no << 3 | 2) + enc_varint(len(data)) + data

def f_int(no, n): return enc_varint(no << 3) + enc_varint(n)

def metadata(b):
    fs = fields(b)
    return {"ideName": s(get(fs, 1)), "ideVersion": s(get(fs, 7)), "ideType": s(get(fs, 28)), "extensionName": s(get(fs, 12)),
            "extensionVersion": s(get(fs, 2)), "apiKey": s(get(fs, 3)), "locale": s(get(fs, 4)), "os": s(get(fs, 5)),
            "userJwt": s(get(fs, 21))}

def prompt(b):
    fs = fields(b)
    return {"messageId": s(get(fs, 1)), "source": get(fs, 2, 0), "prompt": s(get(fs, 3)), "toolCallId": s(get(fs, 7)),
            "toolResultIsError": bool(get(fs, 9, 0)), "thinking": s(get(fs, 11)),
            "toolCalls": [{"id": s(get(t, 1)), "name": s(get(t, 2)), "argumentsJson": s(get(t, 3))}
                          for t in map(fields, getall(fs, 6))],
            "images": [s(get(fields(im), 2)) for im in getall(fs, 10)]}

def chat_request(b):
    fs = fields(b)
    cfg = fields(get(fs, 8, b""))
    tc = fields(get(fs, 12, b""))
    return {"metadata": metadata(get(fs, 1, b"")), "systemPrompt": s(get(fs, 2)), "prompts": [prompt(p) for p in getall(fs, 3)],
            "chatModelUid": s(get(fs, 21)), "requestType": get(fs, 7, 0), "plannerMode": get(fs, 20, 0),
            "cascadeId": s(get(fs, 16)), "executionId": s(get(fs, 22)), "modelAssignmentJwt": s(get(fs, 26)),
            "disableParallelToolCalls": bool(get(fs, 11, 0)), "toolChoice": s(get(tc, 1)),
            "systemPromptCache": get(fields(get(fs, 13, b"")), 1, 0),
            "tools": [{"name": s(get(t, 1)), "schema": s(get(t, 3))} for t in map(fields, getall(fs, 10))],
            "config": {"numCompletions": get(cfg, 1, 0), "maxTokens": get(cfg, 2, 0), "maxNewlines": get(cfg, 3, 0),
                       "temperature": get(cfg, 5, 0), "topK": get(cfg, 7, 0), "topP": get(cfg, 8, 0)}}

def frame(payload, flags=0): return bytes([flags]) + struct.pack(">I", len(payload)) + payload

def resp(message_id="", text=None, thinking=None, sig=None, tool=None, stop=0, usage=None, actual=None):
    b = b""
    if message_id: b += f_len(1, message_id)
    if text: b += f_len(3, text)
    if stop: b += f_int(5, stop)
    if tool: b += f_len(6, b"".join(f_len(n, v) for n, v in tool if v))
    if usage: b += f_len(7, f_int(2, usage[0]) + f_int(3, usage[1]) + f_int(5, usage[2]))
    if thinking: b += f_len(9, thinking)
    if sig: b += f_len(10, sig)
    if actual: b += f_len(23, actual)
    return b

def visible(text):
    return re.sub(r"<system-reminder>.*?</system-reminder>", "", text, flags=re.S).strip()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def log(self, entry):
        if LOG:
            with open(LOG, "a") as f: f.write(json.dumps(entry) + "\n")

    def reply(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        hdrs = {k.lower(): v for k, v in self.headers.items()}
        entry = {"path": self.path, "headers": hdrs}
        if self.path.endswith("/GetUserJwt"):
            md = metadata(get(fields(body), 1, b""))
            entry["metadata"] = md
            self.log(entry)
            if md["apiKey"] != KEY:
                return self.reply(401, json.dumps({"error": {"message": "invalid api key"}}).encode(), "application/json")
            return self.reply(200, f_len(1, "jwt-devin-1"), "application/proto")
        if self.path.endswith("/AssignModel"):
            fs = fields(body)
            entry.update(metadata=metadata(get(fs, 1, b"")), router=s(get(fs, 2)), cascadeId=s(get(fs, 3)))
            self.log(entry)
            return self.reply(200, f_len(1, f_len(1, "assign-jwt") + f_len(2, "MODEL_ASSIGNED_1")), "application/proto")
        if not self.path.endswith("/GetChatMessage"):
            return self.reply(404, b"{}", "application/json")
        flags, n = body[0], struct.unpack(">I", body[1:5])[0]
        payload = body[5:5 + n]
        if flags & 1: payload = gzip.decompress(payload)
        req = chat_request(payload)
        entry["request"] = req
        entry["compressed"] = bool(flags & 1)
        self.log(entry)
        if req["metadata"]["userJwt"] != "jwt-devin-1":
            return self.reply(401, json.dumps({"error": {"message": "bad user jwt"}}).encode(), "application/json")
        last = req["prompts"][-1] if req["prompts"] else {}
        out = []
        if last.get("source") == 4:
            out.append(frame(resp("msg-2", text="Tool said: " + last["prompt"].strip(), usage=(20, 5, 0))))
        else:
            u = visible(last.get("prompt", ""))
            if u == "fail":
                trailer = {"error": {"code": "resource_exhausted", "message": "quota exceeded",
                                     "details": [{"type": "exa.QuotaInfo", "debug": {"reason": "mock"}}]}}
                out.append(frame(json.dumps(trailer).encode(), 2))
                return self.stream(out)
            if u.startswith("bash:"):
                args = json.dumps({"command": u[5:]})
                out.append(frame(resp("msg-1", thinking="Let me run it.", sig="sig-1")))
                half = len(args) // 2
                out.append(frame(resp(tool=[(1, "call.1|item"), (2, "bash"), (3, args[:half])])))
                out.append(frame(resp(tool=[(1, ""), (2, ""), (3, args)])))  # cumulative, active-id continuation
                out.append(frame(resp(stop=10, usage=(12, 7, 3))))
            else:
                out.append(frame(resp("msg-1", text="plain devin ", actual="MODEL_ACTUAL")))
                out.append(frame(gzip.compress(resp(text="answer", usage=(10, 4, 2))), 1))
        out.append(frame(b"{}", 2))
        self.stream(out)

    def stream(self, frames):
        self.send_response(200)
        self.send_header("Content-Type", "application/connect+proto")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for f in frames:
            self.wfile.write(b"%x\r\n%s\r\n" % (len(f), f))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
