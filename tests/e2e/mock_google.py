#!/usr/bin/env python3
"""Dev-only mock Google endpoints for omc end-to-end tests (never real credentials).

  POST /token                                   OAuth2 token endpoint: jwt-bearer grant (service-account ADC) whose
                                                assertion must be an RS256 JWT from sa@mock.iam.gserviceaccount.com
                                                -> access token ya29.mock-sa
  POST /v1/projects/P/locations/L/publishers/google/models/M:streamGenerateContent?alt=sse
                                                Vertex AI (Bearer ya29.mock-sa), Gemini SSE chunks
  POST /v1internal:streamGenerateContent?alt=sse
                                                Cloud Code Assist (Bearer cca-token, project proj-cca), chunks wrapped
                                                in {"response": ...}
Replies: last user text "bash:<cmd>" -> functionCall bash; a functionResponse -> "Tool said: <output>";
anything else -> a thought part plus "plain google answer" (CCA: "plain cca answer").
Requests are appended to $MOCK_LOG (JSONL: path, headers subset, body)."""
import base64, json, os, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")


def b64url_json(seg):
    seg += "=" * (-len(seg) % 4)
    return json.loads(base64.urlsafe_b64decode(seg))


def reply_parts(contents, plain):
    last = contents[-1] if contents else {}
    parts = last.get("parts", [])
    responses = [p["functionResponse"] for p in parts if "functionResponse" in p]
    if responses:
        out = responses[0].get("response", {})
        said = str(out.get("output", out.get("error", ""))).strip()
        return [{"text": "Tool said: "}, {"text": said}], "STOP"
    text = "".join(p.get("text", "") for p in parts if not p.get("text", "").startswith("<system-reminder>"))
    if text.startswith("bash:"):
        return [{"functionCall": {"name": "bash", "args": {"command": text[5:]}}, "thoughtSignature": "c2ln"}], "STOP"
    return [{"text": "hmm", "thought": True, "thoughtSignature": "c2ln"}, {"text": plain}], "STOP"


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, status, body, ctype="application/json"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        path = self.path
        auth = self.headers.get("Authorization", "")
        if LOG:
            try:
                body = json.loads(raw)
            except ValueError:
                body = raw.decode()
            with open(LOG, "a") as f:
                f.write(json.dumps({"path": path, "ua": self.headers.get("User-Agent", ""),
                                    "auth": auth, "body": body}) + "\n")
        if path == "/token":
            form = urllib.parse.parse_qs(raw.decode())
            if form.get("grant_type") != ["urn:ietf:params:oauth:grant-type:jwt-bearer"]:
                return self.send(400, '{"error":"unsupported_grant_type"}')
            segs = form.get("assertion", [""])[0].split(".")
            if len(segs) != 3:
                return self.send(400, '{"error":"invalid_grant"}')
            hdr, claims = b64url_json(segs[0]), b64url_json(segs[1])
            if hdr.get("alg") != "RS256" or claims.get("iss") != "sa@mock.iam.gserviceaccount.com" \
                    or claims.get("exp", 0) - claims.get("iat", 0) != 3600 or not segs[2]:
                return self.send(400, '{"error":"invalid_grant"}')
            return self.send(200, '{"access_token":"ya29.mock-sa","expires_in":3600,"token_type":"Bearer"}')
        req = json.loads(raw)
        if path.startswith("/v1/projects/") and ":streamGenerateContent" in path:
            if auth != "Bearer ya29.mock-sa":
                return self.send(401, '{"error":{"code":401,"message":"Request had invalid authentication credentials.","status":"UNAUTHENTICATED"}}')
            parts, finish = reply_parts(req.get("contents", []), "plain google answer")
            chunks = [{"candidates": [{"content": {"role": "model", "parts": [p]}}]} for p in parts]
            chunks[-1]["candidates"][0]["finishReason"] = finish
            chunks[-1]["usageMetadata"] = {"promptTokenCount": 12, "candidatesTokenCount": 4, "totalTokenCount": 16}
            wrap = lambda c: c
        elif path == "/v1internal:streamGenerateContent?alt=sse":
            if auth != "Bearer cca-token":
                return self.send(401, '{"error":{"code":401,"message":"unauthenticated"}}')
            if req.get("project") != "proj-cca" or "request" not in req:
                return self.send(400, '{"error":{"code":400,"message":"bad envelope"}}')
            parts, finish = reply_parts(req["request"].get("contents", []), "plain cca answer")
            chunks = [{"candidates": [{"content": {"role": "model", "parts": [p]}}]} for p in parts]
            chunks[-1]["candidates"][0]["finishReason"] = finish
            chunks[-1]["usageMetadata"] = {"promptTokenCount": 12, "candidatesTokenCount": 4, "totalTokenCount": 16}
            chunks[0]["responseId"] = "resp-mock"
            wrap = lambda c: {"response": c, "traceId": "trace-mock"}
        else:
            return self.send(404, '{"error":{"code":404,"message":"not found"}}')
        data = b"".join(b"data: " + json.dumps(wrap(c)).encode() + b"\r\n\r\n" for c in chunks)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for i in range(0, len(data), 29):
            c = data[i:i + 29]
            self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
