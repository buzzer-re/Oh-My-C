#!/usr/bin/env python3
"""Dev-only mock of the Kiro services for omc end-to-end tests (fake credentials only). One port serves every host,
told apart by the path prefix the test's URL rewrites add:

  /rt-<region>/      runtime: GenerateAssistantResponse as application/vnd.amazon.eventstream frames
  /mgmt-<region>/    management: ListAvailableModels (paged), ListAvailableProfiles, GetUsageLimits (AWS JSON 1.0)
  /oidc-<region>/    AWS SSO-OIDC: client/register, device_authorization, token (device grant + refresh)
  /social/           Kiro social auth: oauth/device/authorization, oauth/device/poll, refreshToken

Bearers: "kiro-access" (OAuth, any region), "kiro-access-2" (after a refresh), "ksk_good" (an API key accepted only in
us-east-1, needs `tokentype: API_KEY`). Runtime replies by the last user text:
  "bash:<cmd>"   -> a streamed toolUseEvent calling bash        last message a toolResult -> "Tool said: <output>"
  "think"        -> reasoning + text                            "capacity" -> one INSUFFICIENT_MODEL_CAPACITY throttle
  "toolong"      -> HTTP 400 "Input is too long."               "filtered" -> a CONTENT_FILTERED refusal
  anything else  -> "plain kiro answer" with token usage, context usage and a metering event
Every request is appended to $MOCK_LOG (JSONL: path, headers, body)."""
import binascii, json, os, struct, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
STATE = {"capacity": 0, "polls": 0, "social_polls": 0}
PROFILE_A = "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMA"
PROFILE_B = "arn:aws:codewhisperer:us-east-1:123456789012:profile/TEAMB"


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


def model(mid, name, schema=None, image=False):
    m = {"modelId": mid, "modelName": name, "tokenLimits": {"maxInputTokens": 200000, "maxOutputTokens": 32000},
         "supportedInputTypes": ["TEXT", "IMAGE"] if image else ["TEXT"], "rateUnit": "Credit", "rateMultiplier": 1.0}
    if schema:
        m["additionalModelRequestFieldsSchema"] = schema
    return m


ADAPTIVE = {"properties": {"thinking": {"properties": {"type": {"enum": ["adaptive", "disabled"]}}},
                           "output_config": {"properties": {"effort": {"enum": ["low", "medium", "high"], "default": "high"}}},
                           "max_tokens": {"maximum": 64000}}}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, obj, ctype="application/json"):
        body = json.dumps(obj).encode() if not isinstance(obj, bytes) else obj
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("x-amzn-RequestId", "req-mock-1")
        self.end_headers()
        self.wfile.write(body)

    def bearer(self):
        a = self.headers.get("authorization", "")
        return a[7:] if a.startswith("Bearer ") else ""

    def authorized(self, region):
        tok = self.bearer()
        if tok.startswith("ksk_"):
            return tok == "ksk_good" and region == "us-east-1" and self.headers.get("tokentype") == "API_KEY"
        return tok in ("kiro-access", "kiro-access-2")

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = None
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()},
                                    "body": body}) + "\n")
        parts = self.path.strip("/").split("/")
        head = parts[0]
        if head.startswith("rt-"):
            return self.runtime(head[3:], body)
        if head.startswith("mgmt-"):
            return self.management(head[5:], body)
        if head.startswith("oidc-"):
            return self.oidc("/".join(parts[1:]), body)
        if head == "social":
            return self.social("/".join(parts[1:]), body)
        self.reply(404, {"message": "unknown path"})

    # ---------- management ----------
    def management(self, region, body):
        target = self.headers.get("x-amz-target", "")
        if not self.authorized(region):
            return self.reply(403, {"__type": "AccessDeniedException", "message": "denied"})
        if target.endswith(".ListAvailableModels"):
            if body.get("origin") != "KIRO_CLI":
                return self.reply(400, {"message": "origin"})
            if body.get("nextToken") == "p2":
                return self.reply(200, {"models": [model("auto", "Auto"), model("claude-x", "dup")]})
            return self.reply(200, {"models": [model("claude-x", "Claude X", ADAPTIVE, image=True)], "nextToken": "p2"})
        if target.endswith(".ListAvailableProfiles"):
            if region != "us-east-1":
                return self.reply(200, {"profiles": []})
            return self.reply(200, {"profiles": [{"arn": PROFILE_A, "profileName": "Team A"},
                                                 {"arn": PROFILE_B, "profileName": "Team B"}, {"arn": "bad"}]})
        if target.endswith(".GetUsageLimits"):
            return self.reply(200, {
                "subscriptionInfo": {"subscriptionTitle": "KIRO PRO"}, "userInfo": {"email": "someone@example.com"},
                "overageConfiguration": {"overageStatus": "DISABLED"},
                "usageBreakdownList": [{"resourceType": "CREDIT", "displayNamePlural": "Credits",
                                        "currentUsageWithPrecision": 63.8, "usageLimitWithPrecision": 638,
                                        "nextDateReset": 4102444800}]})
        self.reply(400, {"message": "unknown target " + target})

    # ---------- runtime ----------
    def runtime(self, region, body):
        if not self.authorized(region):
            return self.reply(403, {"__type": "com.amazon#AccessDeniedException", "message": "denied"})
        cur = body["conversationState"]["currentMessage"]["userInputMessage"]
        text = cur.get("content", "")
        results = (cur.get("userInputMessageContext") or {}).get("toolResults") or []
        # the first turn carries the system prompt, joined in front of the user text
        whole = text.strip()
        last = next((w for w in ("toolong", "capacity", "filtered", "think") if whole.endswith(w)), "")
        if not last and "bash:" in whole:
            last = whole[whole.rfind("bash:"):]
        if last == "toolong":
            return self.reply(400, {"__type": "ValidationException", "message": "Input is too long."})
        if last == "capacity" and STATE["capacity"] == 0:
            STATE["capacity"] += 1
            exc = frame({":exception-type": "ThrottlingException", ":content-type": "application/json",
                         ":message-type": "exception"},
                        json.dumps({"message": "busy", "reason": "INSUFFICIENT_MODEL_CAPACITY"}).encode())
            return self.reply(200, exc, "application/vnd.amazon.eventstream")
        out = [event("initial-response", {"conversationId": body["conversationState"]["conversationId"]})]
        if results:
            out.append(event("assistantResponseEvent", {"content": "Tool said: " + results[0]["content"][0]["text"].strip()}))
        elif last.startswith("bash:"):
            args = json.dumps({"command": last[5:]})
            out.append(event("toolUseEvent", {"toolUseId": "tooluse_k1", "name": "bash", "input": args[:10]}))
            out.append(event("toolUseEvent", {"toolUseId": "tooluse_k1", "name": "bash", "input": args[10:]}))
            out.append(event("toolUseEvent", {"toolUseId": "tooluse_k1", "name": "bash", "stop": True}))
        elif last == "filtered":
            out.append(event("metadataEvent", {"stopReason": "CONTENT_FILTERED",
                                               "stopDetails": {"refusal": {"category": "cyber", "explanation": "not allowed"}}}))
        elif last == "think":
            out.append(event("reasoningContentEvent", {"text": "pondering"}))
            out.append(event("reasoningContentEvent", {"text": "", "signature": "sig-1"}))
            out.append(event("assistantResponseEvent", {"content": "thought kiro answer"}))
        elif last == "capacity":
            out.append(event("assistantResponseEvent", {"content": "kiro answer after capacity retry"}))
        else:
            out.append(event("assistantResponseEvent", {"content": "plain "}))
            out.append(event("assistantResponseEvent", {"content": "kiro answer"}))
        out.append(event("contextUsageEvent", {"contextUsagePercentage": 1.5}))
        out.append(event("meteringEvent", {"unit": "credit", "usage": 0.1}))
        out.append(event("metadataEvent", {"requestId": "req-meta-1", "tokenUsage": {
            "uncachedInputTokens": 120, "outputTokens": 7, "cacheReadInputTokens": 30, "cacheWriteInputTokens": 5}}))
        self.reply(200, b"".join(out), "application/vnd.amazon.eventstream")

    # ---------- AWS SSO-OIDC ----------
    def oidc(self, path, body):
        if path == "client/register":
            return self.reply(200, {"clientId": "cid-1", "clientSecret": "csec-1"})
        if path == "device_authorization":
            return self.reply(200, {"deviceCode": "dev-1", "userCode": "KIRO-1234", "interval": 1, "expiresIn": 60,
                                    "verificationUriComplete": "https://device.sso.example/?user_code=KIRO-1234"})
        if path == "token":
            if body.get("grantType") == "refresh_token":
                if body.get("refreshToken") == "dead":
                    return self.reply(400, {"error": "invalid_grant"})
                return self.reply(200, {"accessToken": "kiro-access-2", "expiresIn": 3600})
            STATE["polls"] += 1
            if STATE["polls"] % 2 == 1:
                return self.reply(400, {"error": "authorization_pending"})
            return self.reply(200, {"accessToken": "kiro-access", "refreshToken": "kiro-refresh", "expiresIn": 3600})
        self.reply(404, {})

    # ---------- Kiro social auth ----------
    def social(self, path, body):
        if path == "oauth/device/authorization":
            return self.reply(200, {"deviceCode": "sdev", "userCode": "SOC-42", "intervalInMilliseconds": 1000,
                                    "expiresInMilliseconds": 60000,
                                    "verificationUriComplete": "https://app.kiro.dev/device?code=SOC-42"})
        if path == "oauth/device/poll":
            STATE["social_polls"] += 1
            if STATE["social_polls"] % 2 == 1:
                return self.reply(200, {"status": "authorization_pending"})
            return self.reply(200, {"accessToken": "kiro-access", "refreshToken": "soc-refresh",
                                    "profileArn": PROFILE_B})
        if path == "refreshToken":
            return self.reply(200, {"accessToken": "kiro-access-2", "profileArn": PROFILE_A})
        self.reply(404, {})


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
