#!/usr/bin/env python3
"""Dev-only mock Amazon Bedrock ConverseStream endpoint for omc end-to-end tests.

Every request is authenticated independently of omc's code: `Authorization: Bearer $MOCK_BEDROCK_BEARER`, or a
SigV4 signature recomputed here (hashlib/hmac) for MOCK_AWS_ACCESS_KEY_ID / MOCK_AWS_SECRET_ACCESS_KEY. A bad
signature gets a 403 like AWS. Responses are application/vnd.amazon.eventstream frames:
  - last user text "bash:<cmd>"  -> a toolUse block calling bash
  - last message is a toolResult  -> text "Tool said: <output>"
  - "throttle"                    -> an in-stream throttlingException
  - anything else                 -> reasoning + text "plain bedrock answer"
Request bodies are appended to $MOCK_LOG (JSONL) with the path."""
import binascii, hashlib, hmac, json, os, struct, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
AK = os.environ.get("MOCK_AWS_ACCESS_KEY_ID", "AKIDMOCK")
SK = os.environ.get("MOCK_AWS_SECRET_ACCESS_KEY", "mock-secret")
BEARER = os.environ.get("MOCK_BEDROCK_BEARER", "")


def frame(headers, payload):
    hb = b""
    for k, v in headers.items():
        kb, vb = k.encode(), v.encode()
        hb += bytes([len(kb)]) + kb + b"\x07" + struct.pack(">H", len(vb)) + vb
    total = 16 + len(hb) + len(payload)
    pre = struct.pack(">II", total, len(hb))
    msg = pre + struct.pack(">I", binascii.crc32(pre)) + hb + payload
    return msg + struct.pack(">I", binascii.crc32(msg))


def event(etype, obj):
    return frame({":event-type": etype, ":content-type": "application/json", ":message-type": "event"},
                 json.dumps(obj).encode())


def verify_sigv4(handler, body):
    auth = handler.headers.get("Authorization", "")
    if BEARER and auth == "Bearer " + BEARER:
        return None
    if not auth.startswith("AWS4-HMAC-SHA256 "):
        return "missing SigV4 authorization"
    parts = dict(p.strip().split("=", 1) for p in auth[len("AWS4-HMAC-SHA256 "):].split(","))
    akid, date, region, service, _ = parts["Credential"].split("/")
    if akid != AK:
        return "The security token included in the request is invalid."
    signed = parts["SignedHeaders"].split(";")
    for need in ("host", "x-amz-date", "x-amz-content-sha256"):
        if need not in signed:
            return "header %s not signed" % need
    if hashlib.sha256(body).hexdigest() != handler.headers.get("x-amz-content-sha256"):
        return "payload hash mismatch"
    raw_path, _, query = handler.path.partition("?")
    cpath = "/".join(urllib.parse.quote(seg, safe="-_.~") for seg in raw_path.split("/"))
    pairs = sorted((urllib.parse.quote(urllib.parse.unquote(k), safe="-_.~"),
                    urllib.parse.quote(urllib.parse.unquote(v), safe="-_.~"))
                   for k, _, v in (p.partition("=") for p in query.split("&") if p))
    cquery = "&".join("%s=%s" % kv for kv in pairs)
    canon_h = "".join("%s:%s\n" % (n, " ".join(handler.headers.get(n, "").split())) for n in signed)
    creq = "\n".join([handler.command, cpath, cquery, canon_h, ";".join(signed), hashlib.sha256(body).hexdigest()])
    amz = handler.headers.get("x-amz-date", "")
    sts = "\n".join(["AWS4-HMAC-SHA256", amz, "/".join([date, region, service, "aws4_request"]),
                     hashlib.sha256(creq.encode()).hexdigest()])
    k = hmac.new(("AWS4" + SK).encode(), date.encode(), hashlib.sha256).digest()
    for m in (region, service, "aws4_request"):
        k = hmac.new(k, m.encode(), hashlib.sha256).digest()
    want = hmac.new(k, sts.encode(), hashlib.sha256).hexdigest()
    if want != parts["Signature"] or service != "bedrock":
        return ("The request signature we calculated does not match the signature you provided. "
                "Check your AWS Secret Access Key and signing method.")
    return None


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"path": self.path, "body": json.loads(body)}) + "\n")
        err = verify_sigv4(self, body)
        if err:
            data = json.dumps({"message": err}).encode()
            self.send_response(403)
            self.send_header("Content-Type", "application/json")
            self.send_header("x-amzn-ErrorType", "UnrecognizedClientException")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        req = json.loads(body)
        msgs = req.get("messages", [])
        last = msgs[-1] if msgs else {}
        blocks = last.get("content", [])
        out = [event("messageStart", {"role": "assistant"})]
        results = [b["toolResult"] for b in blocks if "toolResult" in b]
        text = "".join(b.get("text", "") for b in blocks if "text" in b)
        stop = "end_turn"
        if results:
            said = "".join(c.get("text", "") for c in results[0]["content"]).strip()
            words = ("Tool said: " + said).split(" ")
            for i, w in enumerate(words):
                out.append(event("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"text": (" " if i else "") + w}}))
            out.append(event("contentBlockStop", {"contentBlockIndex": 0}))
        elif text.startswith("bash:"):
            args = json.dumps({"command": text[5:]})
            out.append(event("contentBlockStart", {"contentBlockIndex": 0, "start": {
                "toolUse": {"toolUseId": "tooluse_mock1", "name": "bash"}}}))
            out.append(event("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"toolUse": {"input": args[:7]}}}))
            out.append(event("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"toolUse": {"input": args[7:]}}}))
            out.append(event("contentBlockStop", {"contentBlockIndex": 0}))
            stop = "tool_use"
        elif "throttle" in text:
            out.append(frame({":exception-type": "throttlingException", ":content-type": "application/json",
                              ":message-type": "exception"}, json.dumps({"message": "Rate exceeded"}).encode()))
            stop = None
        else:
            out.append(event("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"reasoningContent": {"text": "hmm"}}}))
            out.append(event("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"reasoningContent": {"signature": "c2ln"}}}))
            out.append(event("contentBlockStop", {"contentBlockIndex": 0}))
            out.append(event("contentBlockDelta", {"contentBlockIndex": 1, "delta": {"text": "plain bedrock"}}))
            out.append(event("contentBlockDelta", {"contentBlockIndex": 1, "delta": {"text": " answer"}}))
            out.append(event("contentBlockStop", {"contentBlockIndex": 1}))
        if stop:
            out.append(event("messageStop", {"stopReason": stop}))
            out.append(event("metadata", {"usage": {"inputTokens": 11, "outputTokens": 7, "totalTokens": 18},
                                          "metrics": {"latencyMs": 1}}))
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.amazon.eventstream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        data = b"".join(out)
        # odd-sized chunks so frames straddle chunk boundaries
        for i in range(0, len(data), 37):
            c = data[i:i + 37]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
