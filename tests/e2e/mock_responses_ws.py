#!/usr/bin/env python3
"""Dev-only mock of the standard Responses API over WebSocket, with the HTTP/SSE endpoint behind it, for
tests/e2e/provider_responses_ws.sh and side-by-side runs against omp. Logs every request to $MOCK_LOG as {transport, path,
headers, body}: transport "ws" per response.create frame, "ws-handshake" per upgrade, "http" per POST.

The socket holds the previous response per connection. A frame's previous_response_id must name it, like the real host.
Scenarios, from the first user text: "bash:<cmd>" a bash call, "chain:<n>" n rounds of text plus a call, a tool result
"done", else text. "stale-once" answers the first chained frame with previous_response_not_found, "drop-mid" closes the
socket inside a stream, "reject" answers invalid_request_error before generating, "fail-gen" a response.failed.
MOCK_WS_UPGRADE=<status> refuses every upgrade with that HTTP status."""
import base64, hashlib, json, os, socket, struct, sys, threading

LOG = os.environ.get("MOCK_LOG")
UPGRADE = os.environ.get("MOCK_WS_UPGRADE")
STALED = set()
LOCK = threading.Lock()
NEXT = [0]


def log(entry):
    if LOG:
        with LOCK, open(LOG, "a") as f:
            f.write(json.dumps(entry) + "\n")


def next_id():
    with LOCK:
        NEXT[0] += 1
        return NEXT[0]


def recv_exact(c, n):
    b = b""
    while len(b) < n:
        d = c.recv(n - len(b))
        if not d:
            raise ConnectionError
        b += d
    return b


def read_frame(c):
    h = recv_exact(c, 2)
    op, masked, ln = h[0] & 15, h[1] & 128, h[1] & 127
    if ln == 126:
        ln = struct.unpack(">H", recv_exact(c, 2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", recv_exact(c, 8))[0]
    mask = recv_exact(c, 4) if masked else b"\0\0\0\0"
    data = bytearray(recv_exact(c, ln))
    for i in range(len(data)):
        data[i] ^= mask[i & 3]
    return op, bytes(data)


def send_frame(c, op, data):
    h = bytes([0x80 | op])
    n = len(data)
    if n < 126:
        h += bytes([n])
    elif n < 65536:
        h += bytes([126]) + struct.pack(">H", n)
    else:
        h += bytes([127]) + struct.pack(">Q", n)
    c.sendall(h + data)


def texts(item):
    c = item.get("content")
    if isinstance(c, str):
        return [c]
    return [p.get("text", "") for p in c or [] if isinstance(p, dict) and p.get("type") in ("input_text", "output_text")]


def first_prompt(items):
    for it in items:
        if it.get("role") == "user":
            t = texts(it)
            return t[-1] if t else ""
    return ""


def events(items, prompt):
    """The reply's events for the full conversation so far."""
    rid = "resp_%d" % next_id()
    rounds = sum(1 for it in items if it.get("type") == "function_call")
    last = items[-1] if items else {}
    out = []
    if prompt.startswith("chain:") and rounds < int(prompt[6:].split()[0]):
        out.append({"type": "message", "id": "msg_%d" % next_id(), "role": "assistant", "status": "completed",
                    "content": [{"type": "output_text", "text": "step %d" % rounds, "annotations": [], "logprobs": []}]})
        out.append({"type": "function_call", "id": "fc_%d" % next_id(), "call_id": "call_%d" % next_id(), "name": "bash", "status": "completed",
                    "arguments": json.dumps({"command": "echo %d" % rounds})})
    elif last.get("type") == "function_call_output":
        out.append({"type": "message", "id": "msg_%d" % next_id(), "role": "assistant", "status": "completed",
                    "content": [{"type": "output_text", "text": "done", "annotations": [], "logprobs": []}]})
    elif prompt.startswith("bash:"):
        out.append({"type": "function_call", "id": "fc_%d" % next_id(), "call_id": "call_%d" % next_id(), "name": "bash", "status": "completed",
                    "arguments": json.dumps({"command": prompt[5:].split(" stale")[0]})})
    else:
        out.append({"type": "message", "id": "msg_%d" % next_id(), "role": "assistant", "status": "completed",
                    "content": [{"type": "output_text", "text": "plain answer", "annotations": [], "logprobs": []}]})
    evs = [{"type": "response.created", "response": {"id": rid, "status": "in_progress"}}]
    for i, it in enumerate(out):
        added = dict(it, status="in_progress")
        if it["type"] == "message":
            added["content"] = []
        else:
            added["arguments"] = ""
        evs.append({"type": "response.output_item.added", "output_index": i, "item": added})
        if it["type"] == "message":
            evs.append({"type": "response.output_text.delta", "output_index": i, "content_index": 0, "item_id": it["id"], "delta": it["content"][0]["text"]})
        evs.append({"type": "response.output_item.done", "output_index": i, "item": it})
    evs.append({"type": "response.completed", "response": {"id": rid, "status": "completed", "output": out,
                                                            "usage": {"input_tokens": 5, "output_tokens": 3, "total_tokens": 8}}})
    return rid, out, evs


def serve_ws(c, path, head):
    held_id, held = None, []
    while True:
        op, data = read_frame(c)
        if op == 8:
            send_frame(c, 8, data[:2])
            return
        if op == 9:
            send_frame(c, 10, data)
            continue
        if op != 1:
            continue
        body = json.loads(data)
        log({"transport": "ws", "path": path, "headers": head, "body": body})
        input_items = body.get("input") or []
        prev = body.get("previous_response_id")
        full = input_items
        if prev:
            prompt = first_prompt(held)
            if "stale-once" in prompt and prompt not in STALED:
                STALED.add(prompt)
                send_frame(c, 1, json.dumps({"type": "error", "error": {"type": "invalid_request_error", "code": "previous_response_not_found",
                                                                         "message": "Previous response not found"}, "status": 400}).encode())
                continue
            if prev != held_id:
                send_frame(c, 1, json.dumps({"type": "error", "error": {"type": "invalid_request_error", "code": "previous_response_not_found",
                                                                         "message": "unknown id " + str(prev)}, "status": 400}).encode())
                continue
            full = held + input_items
        prompt = first_prompt(full)
        if "reject" in prompt:
            send_frame(c, 1, json.dumps({"type": "error", "error": {"type": "invalid_request_error", "code": "bad_param", "param": "x",
                                                                     "message": "request rejected"}, "status": 400}).encode())
            continue
        if "fail-gen" in prompt:
            send_frame(c, 1, json.dumps({"type": "response.created", "response": {"id": "resp_f"}}).encode())
            send_frame(c, 1, json.dumps({"type": "response.failed", "response": {"id": "resp_f", "status": "failed", "error": {"code": "server_error", "message": "boom"}}}).encode())
            continue
        rid, out, evs = events(full, prompt)
        for i, e in enumerate(evs):
            if "drop-mid" in prompt and i == 2:
                c.close()
                return
            send_frame(c, 1, json.dumps(e).encode())
        held_id, held = rid, full + out


def serve_http(c, method, path, head, rest):
    n = int(head.get("content-length", 0))
    while len(rest) < n:
        d = c.recv(65536)
        if not d:
            return
        rest += d
    body = json.loads(rest[:n] or b"{}")
    log({"transport": "http", "path": path, "headers": head, "body": body})
    items = body.get("input") or []
    _, _, evs = events(items, first_prompt(items))
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n")
    for e in evs:
        c.sendall(("data: %s\n\n" % json.dumps(e)).encode())
    c.close()


def handle(c):
    try:
        raw = b""
        while b"\r\n\r\n" not in raw:
            d = c.recv(65536)
            if not d:
                return
            raw += d
        hdr, _, rest = raw.partition(b"\r\n\r\n")
        lines = hdr.decode("latin1").split("\r\n")
        method, path, _ = lines[0].split(" ", 2)
        head = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
        if head.get("upgrade", "").lower() == "websocket":
            log({"transport": "ws-handshake", "path": path, "headers": head, "body": {}})
            if UPGRADE:
                c.sendall(("HTTP/1.1 %s Nope\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" % UPGRADE).encode())
                return
            accept = base64.b64encode(hashlib.sha1((head["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
            serve_ws(c, path, head)
        else:
            serve_http(c, method, path, head, rest)
    except (ConnectionError, OSError):
        pass
    finally:
        try:
            c.close()
        except OSError:
            pass


if __name__ == "__main__":
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", int(sys.argv[1])))
    s.listen(64)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()
