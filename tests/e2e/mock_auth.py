#!/usr/bin/env python3
"""Mock provider auth endpoints for tests/test_auth.c and the e2e login checks (dev/test only).

omc reaches it through OMC_AUTH_URL_REWRITE (each real origin maps to a path prefix here). Covers RFC 8628
device flows (authorization_pending / slow_down / expiry / denial), GitHub Copilot device login + endpoint
discovery + model policies, Cursor polling, Codex device auth, Kilo, xAI OIDC discovery + userinfo, Muse
key minting, Factory whoami, API-key validation probes and refresh grants. GET /_log returns every request."""
import base64, hashlib, json, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
BASE = f"http://127.0.0.1:{PORT}"
LOG = []
COUNT = {}
LOCK = threading.Lock()


def jwt(payload):
    enc = lambda o: base64.urlsafe_b64encode(json.dumps(o).encode()).rstrip(b"=").decode()
    return f"{enc({'alg': 'none'})}.{enc(payload)}.sig"


def bump(key):
    with LOCK:
        COUNT[key] = COUNT.get(key, 0) + 1
        return COUNT[key]


def device(code_prefix, expires=60):
    return {"device_code": code_prefix + "-dc", "user_code": "ABCD-1234", "verification_uri": BASE + "/verify",
            "verification_uri_complete": BASE + "/verify?user_code=ABCD-1234", "interval": 1, "expires_in": expires}


GOOD = "good-key"
MISTRAL = {"name": "ok", "polls": 0, "challenge": None}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, obj=None, raw=None):
        body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        if obj is not None:
            self.send_header("Content-Type", "application/json")
        for k, v in getattr(self, "extra", []):
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def record(self, body):
        u = urllib.parse.urlparse(self.path)
        form = {}
        if "x-www-form-urlencoded" in self.headers.get("Content-Type", ""):
            form = dict(urllib.parse.parse_qsl(body.decode()))
        js = None
        if "json" in self.headers.get("Content-Type", "") and body:
            try:
                js = json.loads(body)
            except ValueError:
                js = None
        entry = {"method": self.command, "path": u.path, "query": dict(urllib.parse.parse_qsl(u.query)),
                 "headers": {k.lower(): v for k, v in self.headers.items()}, "form": form, "json": js}
        with LOCK:
            LOG.append(entry)
        return u.path, entry

    def bearer(self):
        a = self.headers.get("Authorization", "")
        return a[7:] if a.startswith("Bearer ") else None

    def mistral(self, path, j, q):
        """Mistral Vibe browser sign-in (registry/oauth/mistral-vibe.ts); /mistral/_scenario?name=... picks the polls."""
        global MISTRAL
        console = "https://console.mistral.ai"
        if path == "/mistral/_scenario":
            MISTRAL = {"name": q.get("name", "ok"), "polls": 0, "challenge": None}
            return self.send(200, {})
        name = MISTRAL["name"]
        if path == "/mistral/api/vibe/sign-in":
            MISTRAL["challenge"] = j.get("code_challenge")
            if name == "start-500":
                return self.send(500, {})
            poll = "https://evil.example.com/api/poll" if name == "bad-url" else console + "/api/vibe/sign-in/p%201/status"
            exp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() + (0 if name == "expires-now" else 600)))
            return self.send(200, {"process_id": "p 1", "sign_in_url": console + "/vibe/approve?p=1", "poll_url": poll, "expires_at": exp})
        if path == "/mistral/api/vibe/sign-in/p 1/status" or path == "/mistral/api/vibe/sign-in/p%201/status":
            MISTRAL["polls"] += 1
            n = MISTRAL["polls"]
            done = {"status": "completed", "exchange_token": "xt-1"}
            if name == "expired":
                return self.send(410)
            if name == "denied":
                return self.send(200, {"status": "denied"})
            if name == "hung-up":
                return self.send(200, {"status": "error", "message": "console said no"})
            if name == "unknown":
                return self.send(200, {"status": "sideways"})
            if name == "no-token":
                return self.send(200, {"status": "completed"})
            if name == "flaky":
                return self.send(200, done) if n >= 3 else self.send(503, {}) if n == 1 else self.send(200, [])
            if name == "down":
                return self.send(503, {})
            if name == "throttled":
                if n == 1:
                    self.extra = [("Retry-After", "1")]
                    return self.send(429, {})
                return self.send(200, done)
            return self.send(200, done) if n >= 2 else self.send(200, {"status": "pending"})
        if path == "/mistral/api/vibe/sign-in/p%201/exchange" or path == "/mistral/api/vibe/sign-in/p 1/exchange":
            MISTRAL["exchanged"] = True
            if name == "workspace":
                return self.send(403, {"error_code": 9004})
            dig = base64.urlsafe_b64encode(hashlib.sha256((j.get("code_verifier") or "").encode()).digest()).rstrip(b"=").decode()
            if dig != MISTRAL["challenge"] or j.get("exchange_token") != "xt-1":
                return self.send(400, {"detail": "verifier"})
            return self.send(200, {"api_key": "vibe-key"})
        return self.send(404, {})

    def do_GET(self):
        self.handle_req(b"")

    def do_POST(self):
        self.handle_req(self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)))

    def handle_req(self, body):
        path, e = self.record(body)
        f, j, q = e["form"], e["json"] or {}, e["query"]
        if path == "/_log":
            return self.send(200, LOG)
        if path.startswith("/mistral/"):
            return self.mistral(path, j, q)

        # ---- Kimi device flow (KIMI_CODE_OAUTH_HOST=BASE/kimi, /kimi-expire, /kimi-deny) ----
        for pfx in ("/kimi-expire", "/kimi-deny", "/kimi"):
            if path == pfx + "/api/oauth/device_authorization":
                return self.send(200, device(pfx[1:], expires=1 if pfx == "/kimi-expire" else 60))
            if path == pfx + "/api/oauth/token":
                if f.get("grant_type") == "refresh_token":
                    if f.get("refresh_token") != "kimi-r1":
                        return self.send(400, {"error": "invalid_grant"})
                    return self.send(200, {"access_token": jwt({"user_id": "kimi-user", "n": 2}), "refresh_token": "kimi-r2", "expires_in": 3600})
                if f.get("device_code") != pfx[1:] + "-dc":
                    return self.send(400, {"error": "invalid_grant"})
                if pfx == "/kimi-expire":
                    return self.send(400, {"error": "authorization_pending"})
                if pfx == "/kimi-deny":
                    return self.send(400, {"error": "access_denied"})
                n = bump("kimi-poll")
                if n == 1:
                    return self.send(400, {"error": "authorization_pending"})
                if n == 2:
                    return self.send(400, {"error": "slow_down"})
                return self.send(200, {"access_token": jwt({"user_id": "kimi-user", "n": 1}), "refresh_token": "kimi-r1", "expires_in": 3600})

        # ---- GitHub Copilot ----
        for pfx, cid in (("/gh", "Ov23li8tweQw6odWQebz"), ("/ghe", "Ov23ctDVkRmgkPke0Mmm")):
            if path == pfx + "/login/device/code":
                if f.get("client_id") != cid or f.get("scope") != "read:user":
                    return self.send(400, {"error": "bad_client"})
                return self.send(200, {"device_code": "gh-dc", "user_code": "WXYZ-0001", "verification_uri": "https://github.com/login/device",
                                       "interval": 1, "expires_in": 60})
            if path == pfx + "/login/oauth/access_token":
                n = bump("gh-poll" + pfx)
                if n == 1:
                    return self.send(200, {"error": "authorization_pending"})
                if n == 2:
                    return self.send(200, {"error": "slow_down", "interval": 1})
                return self.send(200, {"access_token": "gho_test" if pfx == "/gh" else "gho_ent", "token_type": "bearer"})
        if path == "/ghapi/copilot_internal/user":
            if self.headers.get("Authorization") != "token gho_test":
                return self.send(401, {"message": "Bad credentials"})
            return self.send(200, {"endpoints": {"api": "https://api.individual.githubcopilot.com/"}})
        if path.startswith("/capi/models/") and path.endswith("/policy"):
            bump("policy")
            return self.send(200, {})

        # ---- xAI OAuth (OIDC discovery + device + userinfo) ----
        if path == "/xauth/.well-known/openid-configuration":
            return self.send(200, {"issuer": "https://auth.x.ai", "token_endpoint": "https://auth.x.ai/oauth2/token"})
        if path == "/xauth/oauth2/device/code":
            return self.send(200, device("xai"))
        if path == "/xauth/oauth2/token":
            if f.get("grant_type") == "refresh_token":
                return self.send(200, {"access_token": jwt({"sub": "xai-sub", "n": 2}), "refresh_token": "xr2", "expires_in": 3600})
            if bump("xai-poll") == 1:
                return self.send(400, {"error": "authorization_pending"})
            return self.send(200, {"access_token": jwt({"sub": "xai-sub", "n": 1}), "refresh_token": "xr1", "expires_in": 3600})
        if path == "/xauth/oauth2/userinfo":
            return self.send(200, {"email": "me@x.ai", "sub": "xai-sub"})

        # ---- Cursor ----
        if path == "/cursor/auth/poll":
            if bump("cursor-poll") == 1:
                return self.send(404, {"error": "not found"})
            return self.send(200, {"accessToken": jwt({"sub": "github|42", "exp": int(time.time()) + 3600}), "refreshToken": "cur-r1"})
        if path == "/cursor/aiserver.v1.DashboardService/GetMe":
            return self.send(200, {"authId": "github|42", "email": "cur@example.com"})
        if path == "/cursor/oauth/token":
            if j.get("refresh_token") != "cur-r1" or j.get("client_id") != "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB":
                return self.send(401, {"error": "bad refresh"})
            return self.send(200, {"access_token": jwt({"sub": "github|42", "exp": int(time.time()) + 7200, "n": 2})})

        # ---- OpenAI Codex device flow + token endpoint ----
        if path == "/oai/api/accounts/deviceauth/usercode":
            return self.send(200, {"device_auth_id": "da1", "user_code": "CODX-0001", "interval": "1"})
        if path == "/oai/api/accounts/deviceauth/token":
            if bump("codex-poll") == 1:
                return self.send(403, {"error": "pending"})
            return self.send(200, {"authorization_code": "ac1", "code_verifier": "cv1"})
        if path == "/oai/oauth/token":
            claims = {"https://api.openai.com/auth": {"chatgpt_account_id": "acct-1", "chatgpt_plan_type": "Pro"},
                      "https://api.openai.com/profile": {"email": "Me@Example.com"}}
            if f.get("grant_type") == "authorization_code":
                if f.get("code") != "ac1" or f.get("code_verifier") != "cv1" or f.get("redirect_uri") != "https://auth.openai.com/deviceauth/callback":
                    return self.send(400, {"error": "invalid_grant", "error_description": "bad code"})
                return self.send(200, {"access_token": jwt(claims), "refresh_token": "or1", "expires_in": 3600})
            # codex-rs AuthAPI refresh diagnostics (omp 6425ef0e8d): omc only refreshes a token nearing expiry
            if f.get("grant_type") == "refresh_token" and f.get("refresh_token") == "or1" and f.get("refresh_reason") == "proactive_expiry":
                return self.send(200, {"access_token": jwt(dict(claims, n=2)), "refresh_token": "or2", "expires_in": 3600})
            return self.send(400, {"error": "invalid_grant"})

        # ---- Kilo ----
        if path == "/kilo/api/device-auth/codes":
            return self.send(200, {"code": "KILO1", "verificationUrl": "https://app.kilo.ai/device", "expiresIn": 60})
        if path == "/kilo/api/device-auth/codes/KILO1":
            if bump("kilo-poll") == 1:
                return self.send(202, {"status": "pending"})
            return self.send(200, {"status": "approved", "token": "kilo-tok"})

        # ---- Muse Code (Meta device flow + key mint) ----
        if path == "/meta/oidc/device/authorization/":
            return self.send(200, device("meta"))
        if path == "/meta/oidc/device/token/":
            return self.send(200, {"access_token": "meta-at", "refresh_token": "meta-rt", "expires_in": 3600})
        if path == "/metaapi/muse-code/key":
            if self.bearer() != "meta-at" or j.get("onboard") is not True:
                return self.send(401, {"error": "unauthorized"})
            return self.send(200, {"api_key": "muse-key", "user_email": "Muse@Example.com", "user_id": "u1", "is_subs_active": True})

        # ---- Factory (WorkOS device flow + whoami) ----
        if path == "/workos/user_management/authorize/device":
            return self.send(200, device("workos"))
        if path == "/workos/user_management/authenticate":
            tok = jwt({"sub": "user_1", "email": "f@x.com", "external_org_id": "org_f", "exp": int(time.time()) + 3600})
            return self.send(200, {"access_token": tok, "refresh_token": "fr1", "user": {"id": "user_1", "email": "f@x.com"}})
        if path == "/factory/api/cli/whoami":
            if self.headers.get("X-Factory-Org-Id") != "org_f":
                return self.send(403, {"error": "no org"})
            return self.send(200, {"orgId": "org_f", "region": "global"})

        # ---- API-key validation probes ----
        if path in ("/xaiapi/v1/models", "/tp/compatible-mode/v1/models", "/qwen/compatible-mode/v1/models"):
            if self.bearer() in (GOOD, "sk-tp-good"):
                return self.send(200, {"data": []})
            return self.send(401, {"error": {"message": "Incorrect API key provided", "code": "invalid_api_key"}})
        if path == "/zaiapi/api/coding/paas/v4/chat/completions":
            if j.get("model") != "glm-5.2" or j.get("max_tokens") != 1 or j.get("messages") != [{"role": "user", "content": "ping"}]:
                return self.send(400, {"error": {"message": "bad probe"}})
            return self.send(200 if self.bearer() == GOOD else 401, {"error": {"message": "invalid key"}} if self.bearer() != GOOD else {"choices": []})
        if path == "/umans/v1/messages":
            ok = self.headers.get("x-api-key") == GOOD and self.headers.get("anthropic-version") == "2023-06-01"
            return self.send(200 if ok else 401, {"type": "message"} if ok else {"type": "error", "error": {"type": "authentication_error"}})
        if path == "/cmdcode/alpha/whoami":
            k = self.bearer()
            return self.send(500 if k == "flaky" else 403 if k == "forbidden" else 401 if k != GOOD else 200, {"error": "x"})
        if path == "/coreweave/v1/models":
            return self.send(200 if self.headers.get("OpenAI-Project") == "team/proj" and self.bearer() == GOOD else 401, {})
        return self.send(404, {"error": "not found", "path": path})


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
