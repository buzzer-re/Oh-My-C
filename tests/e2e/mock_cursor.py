#!/usr/bin/env python3
"""Dev-only mock of Cursor's agent service (Connect protocol over HTTP/1.1) for omc end-to-end tests.

Implements the RunSSE/BidiAppend transport: POST /agent.v1.AgentService/RunSSE carries one framed BidiRequestId
and streams framed AgentServerMessages; every client message arrives as a unary
POST /aiserver.v1.BidiService/BidiAppend (BidiAppendRequest{requestId, appendSeqno, dataBinary}). Messages are
protobuf, hand-encoded here (field numbers from Cursor's agent.proto). Scripted turns, by user text:
  "hi"              -> thinking + text "plain cursor answer" + checkpoint + turnEnded usage
  "bash:<cmd>"      -> requestContext exec, blob set/get round trip, shellArgs exec, then "Tool said: <stdout>"
  "bash24h:<cmd>"   -> the same with Cursor's 24 h hard_timeout on the ShellArgs
  "read:<path>"     -> readArgs exec, then "Read said: <content>"
  "mcp:<tool>:<js>" -> streamed mcpToolCall + mcpArgs exec, then "MCP said: <text>"
  "history"         -> fetches every rootPromptMessagesJson blob over KV and answers "roles: ..."
  "fail"            -> Connect end-stream error (resource_exhausted)
  "flaky"           -> first run drops the stream before turnEnded, the retry answers "recovered"
Requests are logged to $MOCK_LOG (JSONL). Auth: Bearer $MOCK_CURSOR_TOKEN (401 otherwise)."""
import json, os, queue, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
TOKEN = os.environ.get("MOCK_CURSOR_TOKEN", "cursor-mock-token")
LOCK = threading.Lock()
RUNS = {}  # request id -> queue of decoded AgentClientMessage dicts (in seqno order)
PENDING = {}  # request id -> {seqno: bytes}
NEXT = {}
FLAKY = {"count": 0}
ISSUED = set()  # access tokens minted by the login/refresh endpoints
POLLS = {}


def jwt(sub, exp_in):
    import base64, time as _t
    enc = lambda o: base64.urlsafe_b64encode(json.dumps(o).encode()).decode().rstrip("=")
    tok = enc({"alg": "none"}) + "." + enc({"sub": sub, "exp": int(_t.time()) + exp_in}) + ".sig"
    ISSUED.add(tok)
    return tok


def log(obj):
    if LOG:
        with LOCK, open(LOG, "a") as f:
            f.write(json.dumps(obj) + "\n")


# ---------------- protobuf ----------------
def varint(n):
    n &= (1 << 64) - 1
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def field(no, value):
    if isinstance(value, bool):
        return varint(no << 3) + varint(1 if value else 0)
    if isinstance(value, int):
        return varint(no << 3) + varint(value)
    if isinstance(value, str):
        value = value.encode()
    return varint(no << 3 | 2) + varint(len(value)) + value


def msg(*fields):
    return b"".join(field(no, v) for no, v in fields if v is not None)


def parse(data):
    """-> {field_no: [values]} ; LEN values stay bytes, varints are ints"""
    out, i = {}, 0
    while i < len(data):
        tag, i = read_varint(data, i)
        no, wt = tag >> 3, tag & 7
        if wt == 0:
            v, i = read_varint(data, i)
        elif wt == 2:
            n, i = read_varint(data, i)
            v, i = data[i:i + n], i + n
        elif wt == 1:
            v, i = data[i:i + 8], i + 8
        elif wt == 5:
            v, i = data[i:i + 4], i + 4
        else:
            raise ValueError("wire type %d" % wt)
        out.setdefault(no, []).append(v)
    return out


def read_varint(data, i):
    shift = n = 0
    while True:
        b = data[i]
        i += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, i


def get(d, no, default=None):
    v = d.get(no)
    return v[0] if v else default


def frame(payload, flags=0):
    return bytes([flags]) + len(payload).to_bytes(4, "big") + payload


def value_to_json(data):
    d = parse(data)
    if 3 in d:
        return d[3][0].decode()
    if 4 in d:
        return bool(d[4][0])
    if 5 in d:
        out = {}
        for entry in parse(d[5][0]).get(1, []):
            e = parse(entry)
            out[get(e, 1, b"").decode()] = value_to_json(get(e, 2, b""))
        return out
    if 6 in d:
        return [value_to_json(x) for x in parse(d[6][0]).get(1, [])]
    if 2 in d:
        import struct
        return struct.unpack("<d", d[2][0])[0]
    return None


def value(v):
    if isinstance(v, str):
        return field(3, v)
    if isinstance(v, bool):
        return field(4, v)
    if isinstance(v, dict):
        return field(5, b"".join(field(1, field(1, k) + field(2, value(x))) for k, x in v.items()))
    if isinstance(v, (int, float)):
        import struct
        return varint(2 << 3 | 1) + struct.pack("<d", float(v))
    return field(1, 0)


# AgentServerMessage builders
def interaction(no, body):
    return frame(msg((1, msg((no, body)))))


def text(t):
    return interaction(1, msg((1, t)))


def thinking(t):
    return interaction(4, msg((1, t)))


def exec_msg(eid, exec_id, no, body):
    return frame(msg((2, msg((1, eid), (15, exec_id), (no, body)))))


def kv(kid, no, body):
    return frame(msg((4, msg((1, kid), (no, body)))))


def turn_ended(inp, out):
    return interaction(14, msg((1, inp), (2, out)))


def checkpoint(used):
    return frame(msg((3, msg((5, msg((1, used), (2, 200000)))))))


END = frame(b"{}", 2)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def body(self):
        n = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(n)

    def reply_json(self, status, obj):
        b = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        # login poll: 404 until the second poll of a uuid, then the token pair
        if self.path.startswith("/auth/poll?"):
            from urllib.parse import parse_qs, urlparse
            q = parse_qs(urlparse(self.path).query)
            uuid = q.get("uuid", [""])[0]
            log({"poll": uuid, "verifier": len(q.get("verifier", [""])[0])})
            with LOCK:
                POLLS[uuid] = POLLS.get(uuid, 0) + 1
                n = POLLS[uuid]
            if n < 2:
                return self.reply_json(404, {})
            return self.reply_json(200, {"accessToken": jwt("github|42", 3600), "refreshToken": "refresh-1"})
        self.reply_json(404, {})

    def do_POST(self):
        data = self.body()
        if self.path == "/oauth/token":
            req = json.loads(data or b"{}")
            log({"refresh": req})
            if req.get("refresh_token") != "refresh-1" or req.get("client_id") != "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB":
                return self.reply_json(401, {"error": "invalid_grant"})
            return self.reply_json(200, {"access_token": jwt("github|42", 3600)})
        auth = self.headers.get("authorization") or ""
        if self.path == "/aiserver.v1.DashboardService/GetMe":
            if auth[7:] not in ISSUED:
                return self.reply_json(401, {})
            return self.reply_json(200, {"authId": "github|42", "email": "dev@example.com"})
        if auth != "Bearer " + TOKEN and auth[7:] not in ISSUED:
            if self.path.endswith("/BidiAppend"):
                time.sleep(0.5)  # let the RunSSE 401 settle the run first
            self.send_response(401)
            self.send_header("Content-Type", "application/json")
            b = json.dumps({"code": "unauthenticated", "message": "bad token"}).encode()
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        if self.path == "/aiserver.v1.BidiService/BidiAppend":
            return self.append(data)
        if self.path == "/agent.v1.AgentService/RunSSE":
            return self.run(data)
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def append(self, data):
        if self.headers.get("content-type") != "application/proto":
            self.send_response(415)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        d = parse(data)
        rid = get(parse(get(d, 2, b"")), 1, b"").decode()
        seq = get(d, 3, 0)
        payload = get(d, 4, b"")
        with LOCK:
            q = RUNS.setdefault(rid, queue.Queue())
            PENDING.setdefault(rid, {})[seq] = payload
            nxt = NEXT.get(rid, 0)
            while nxt in PENDING[rid]:
                q.put(parse(PENDING[rid].pop(nxt)))
                nxt += 1
            NEXT[rid] = nxt
        self.send_response(200)
        self.send_header("Content-Type", "application/proto")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def chunk(self, b):
        self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
        self.wfile.flush()

    def run(self, data):
        rid = get(parse(data[5:]), 1, b"").decode()
        with LOCK:
            q = RUNS.setdefault(rid, queue.Queue())
        self.send_response(200)
        self.send_header("Content-Type", "application/connect+proto")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        heartbeats = [0]

        def recv(want):
            """next client message carrying field `want` (heartbeats counted, others logged)"""
            while True:
                m = q.get(timeout=20)
                if 7 in m:
                    heartbeats[0] += 1
                    continue
                if want in m:
                    return m
                log({"unexpected": list(m.keys())})

        first = recv(1)  # runRequest
        run = parse(get(first, 1))
        action = parse(get(run, 2, b""))
        user = parse(get(parse(get(action, 1, b"")), 1, b"")) if 1 in action else {}
        user_text = get(user, 1, b"").decode()
        details = parse(get(run, 3, b""))
        requested = parse(get(run, 9, b""))
        state = parse(get(run, 1, b""))
        params = [{"id": get(parse(p), 1, b"").decode(), "value": get(parse(p), 2, b"").decode()}
                  for p in requested.get(3, [])]
        log({"path": self.path, "requestId": rid, "user": user_text, "resume": 2 in action,
             "conversationId": get(run, 5, b"").decode(),
             "modelDetails": get(details, 1, b"").decode(), "displayModelId": get(details, 3, b"").decode(),
             "requestedModel": get(requested, 1, b"").decode(), "maxMode": bool(get(requested, 2, 0)),
             "parameters": params, "rootPrompt": len(state.get(1, [])), "turns": len(state.get(8, [])),
             "clientVersion": self.headers.get("x-cursor-client-version"),
             "clientType": self.headers.get("x-cursor-client-type"),
             "streaming": self.headers.get("x-cursor-streaming"),
             "originalRequestId": self.headers.get("x-original-request-id"),
             "contentType": self.headers.get("content-type")})
        try:
            self.script(user_text.rsplit("\n", 1)[-1], state, recv, rid)
        except queue.Empty:
            log({"timeout": rid})
        log({"heartbeats": heartbeats[0], "requestId": rid})
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def script(self, user_text, state, recv, rid):
        out = self.chunk
        if user_text.startswith("bash:") or user_text.startswith("bash24h:"):
            # bash24h: Cursor's native Shell 24 h hard_timeout (ShellArgs field 14)
            hard = 86400000 if user_text.startswith("bash24h:") else None
            cmd = user_text.split(":", 1)[1]
            out(exec_msg(1, "ctx1", 10, b""))  # requestContextArgs
            ctx = parse(get(parse(get(recv(2), 2)), 10))
            rc = parse(get(parse(get(ctx, 1)), 1))
            tools = [get(parse(t), 1, b"").decode() for t in rc.get(7, [])]
            rules = len(rc.get(2, []))
            log({"requestContext": {"tools": tools, "rules": rules}})
            out(kv(5, 3, msg((1, b"\x01\x02blob"), (2, b"blob-data"))))  # setBlobArgs
            recv(3)
            out(kv(6, 2, msg((1, b"\x01\x02blob"))))  # getBlobArgs
            got = get(parse(get(parse(get(recv(3), 3)), 2, b"")), 1, b"")
            out(interaction(2, msg((1, "env1"), (2, msg((1, msg((1, msg((1, cmd))))))))))  # toolCallStarted shell
            out(exec_msg(2, "sh1", 2, msg((1, cmd), (4, "call_shell_1"), (14, hard))))
            res = parse(get(parse(get(recv(2), 2)), 2))  # shellResult
            ok = parse(get(res, 1, b"")) if 1 in res else parse(get(res, 2, b""))
            stdout = get(ok, 5, b"").decode().strip()
            out(interaction(3, msg((1, "env1"))))
            out(text("Tool said: " + stdout + ("" if got == b"blob-data" else " (KV MISMATCH)")))
            out(interaction(17, msg((1, 1))))
            out(checkpoint(1234))
            out(turn_ended(500, 40))
            out(END)
            return
        if user_text.startswith("read:"):
            out(exec_msg(1, "rd1", 7, msg((1, user_text[5:]), (2, "call_read_1"))))
            res = parse(get(parse(get(recv(2), 2)), 7))
            if 1 in res:
                content = get(parse(get(res, 1)), 2, b"").decode()
            else:
                content = "ERR " + repr(res)
            out(text("Read said: " + content.strip()))
            out(turn_ended(10, 5))
            out(END)
            return
        if user_text.startswith("mcp:"):
            _, tool, args = user_text.split(":", 2)
            argmap = b"".join(field(2, field(1, k) + field(2, value(v))) for k, v in json.loads(args).items())
            margs = msg((1, tool), (3, "call_mcp_1"), (4, "pi-agent"), (5, tool)) + argmap
            out(interaction(2, msg((1, "envm"), (2, msg((15, msg((1, margs))))))))
            out(exec_msg(3, "mcp1", 11, margs))
            res = parse(get(parse(get(recv(2), 2)), 11))
            if 1 in res:
                items = parse(get(res, 1)).get(1, [])
                said = " ".join(get(parse(get(parse(i), 1, b"")), 1, b"").decode() for i in items)
            else:
                said = "ERR " + repr(res)
            out(interaction(3, msg((1, "envm"), (2, msg((15, msg((1, margs))))))))
            out(text("MCP said: " + said.strip()))
            out(turn_ended(10, 5))
            out(END)
            return
        if user_text == "history":
            roles = []
            for i, blob_id in enumerate(state.get(1, [])):
                out(kv(100 + i, 2, msg((1, blob_id))))
                data = get(parse(get(parse(get(recv(3), 3)), 2, b"")), 1, b"")
                try:
                    roles.append(json.loads(data)["role"])
                except Exception:
                    roles.append("?")
            out(text("roles: " + ",".join(roles)))
            out(turn_ended(10, 5))
            out(END)
            return
        if user_text == "fail":
            out(frame(json.dumps({"error": {"code": "resource_exhausted", "message": "Error"}}).encode(), 2))
            return
        if user_text in ("flaky", ""):
            with LOCK:
                FLAKY["count"] += 1
                n = FLAKY["count"]
            if n == 1:
                return  # drop: no turnEnded, no end frame
            out(text("recovered"))
            out(turn_ended(10, 5))
            out(END)
            return
        out(thinking("pondering"))
        out(interaction(5, b""))
        out(text("plain cursor "))
        out(text("answer"))
        out(interaction(8, msg((1, 3))))
        out(checkpoint(4321))
        out(turn_ended(1200, 30))
        out(END)


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], ConnectionError):  # clients drop appends/streams on close
            super().handle_error(request, client_address)


if __name__ == "__main__":
    Server(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
