#!/usr/bin/env python3
"""OAuth-protected streamable-HTTP MCP server for e2e tests (dev/test only).
RFC 9728 resource metadata, RFC 8414 AS metadata, RFC 7591 registration, auth code + PKCE (S256 verified),
refresh tokens. /authorize redirects straight back with a code (the test plays the browser with curl -L)."""
import base64, hashlib, json, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
BASE = f"http://127.0.0.1:{PORT}"
codes = {}      # code -> (challenge, redirect, resource)
tokens = {"tok-0": False}
seq = [0]

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, obj=None, headers=None, raw=None):
        body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(code)
        for k, v in (headers or {}).items(): self.send_header(k, v)
        if obj is not None: self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        if u.path.startswith("/.well-known/oauth-protected-resource"):
            return self.send(200, {"resource": BASE + "/mcp", "authorization_servers": [BASE], "scopes_supported": ["mcp.read"]})
        if u.path == "/.well-known/oauth-authorization-server":
            return self.send(200, {"issuer": BASE, "authorization_endpoint": BASE + "/authorize", "token_endpoint": BASE + "/token",
                                   "registration_endpoint": BASE + "/register", "code_challenge_methods_supported": ["S256"]})
        if u.path == "/authorize":
            assert q.get("client_id") == "cid-1" and q.get("code_challenge_method") == "S256" and q.get("resource") == BASE + "/mcp"
            seq[0] += 1; code = f"code-{seq[0]}"
            codes[code] = (q["code_challenge"], q["redirect_uri"], q["resource"])
            loc = q["redirect_uri"] + "?" + urllib.parse.urlencode({"code": code, "state": q["state"]})
            return self.send(302, None, {"Location": loc})
        return self.send(404, {"error": "not found"})
    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if u.path == "/register":
            reg = json.loads(data); assert reg["redirect_uris"][0].startswith("http://127.0.0.1:")
            return self.send(201, {"client_id": "cid-1"})
        if u.path == "/token":
            f = dict(urllib.parse.parse_qsl(data.decode()))
            if f.get("grant_type") == "authorization_code":
                ch, red, res = codes.pop(f.get("code"), (None, None, None))
                ok = ch and base64.urlsafe_b64encode(hashlib.sha256(f["code_verifier"].encode()).digest()).rstrip(b"=").decode() == ch
                if not ok or f.get("redirect_uri") != red or f.get("resource") != res: return self.send(400, {"error": "invalid_grant"})
                tokens["tok-1"] = True
                return self.send(200, {"access_token": "tok-1", "refresh_token": "ref-1", "expires_in": 3600, "token_type": "Bearer"})
            if f.get("grant_type") == "refresh_token" and f.get("refresh_token") == "ref-1":
                tokens["tok-2"] = True
                return self.send(200, {"access_token": "tok-2", "expires_in": 3600, "token_type": "Bearer"})
            return self.send(400, {"error": "unsupported_grant_type"})
        if u.path == "/mcp":
            auth = self.headers.get("Authorization", "")
            if not (auth.startswith("Bearer ") and tokens.get(auth[7:])):
                return self.send(401, {"error": "unauthorized"}, {"WWW-Authenticate":
                    f'Bearer resource_metadata="{BASE}/.well-known/oauth-protected-resource/mcp"'})
            msg = json.loads(data)
            if "id" not in msg: return self.send(202, None, raw=b"")
            m = msg["method"]
            if m == "initialize":
                res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "oauth-mock", "version": "1"}}
            elif m == "tools/list":
                res = {"tools": [{"name": "whoami", "description": "who am I", "inputSchema": {"type": "object", "properties": {}}}]}
            elif m == "tools/call":
                res = {"content": [{"type": "text", "text": "authorized as " + auth[7:]}]}
            else:
                res = {}
            return self.send(200, {"jsonrpc": "2.0", "id": msg["id"], "result": res})
        return self.send(404, {"error": "not found"})

ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
