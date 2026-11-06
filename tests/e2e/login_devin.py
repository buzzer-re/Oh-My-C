#!/usr/bin/env python3
"""`omc login devin-cli` and `omc login devin-windsurf` against a throwaway HOME and a local exchange endpoint: both file a
`devin` API key (store-as). usage: login_devin.py <omc> <scratch-dir>. Prints one "<step> ok" line per step, or "<step>
FAIL: <why>" and exits 1. Fake credentials only."""
import gzip, json, os, re, subprocess, sys, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

omc, scratch = sys.argv[1:3]
SEEN = []


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def field(no, text):
    data = text.encode()
    return varint(no << 3 | 2) + varint(len(data)) + data


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        SEEN.append((self.path, {k.lower(): v for k, v in self.headers.items()}, body))
        code = body[2:2 + body[1]].decode("latin1") if len(body) > 2 else ""
        status, reply = 200, field(1, "gsk-ws-seat") + field(2, "Seat") + field(3, "https://server.codeium.com")
        if code == "code-gz":
            reply = gzip.compress(field(1, "gsk-ws-zipped"))
        elif code == "code-empty":
            reply = field(2, "Seat")
        elif code == "code-bad":
            status, reply = 403, b"denied"
        self.send_response(status)
        self.send_header("Content-Type", "application/proto")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]


def fresh(name):
    home = os.path.join(scratch, name)
    os.makedirs(os.path.join(home, "agent"), exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k != "XDG_DATA_HOME"}
    env.update(HOME=home, OMC_CODING_AGENT_DIR=os.path.join(home, "agent"), OMC_NO_BROWSER="1",
               OMC_AUTH_URL_REWRITE="https://server.codeium.com=http://127.0.0.1:%d" % port)
    return home, env


def fail(step, why):
    print("%s FAIL: %s" % (step, why))
    sys.exit(1)


def stored(home):
    with open(os.path.join(home, "agent", "auth.json")) as f:
        return json.load(f)


def cli(step, home, env, want):
    r = subprocess.run([omc, "login", "devin-cli"], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
    out = (r.stdout + r.stderr).replace(home, "HOME")
    if want not in out:
        fail(step, out)
    print(step, "ok")


def windsurf(code):
    home, env = fresh("ws-" + code)
    p = subprocess.Popen([omc, "login", "devin-windsurf"], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    timer = threading.Timer(30, p.kill)
    timer.start()
    url, lines = None, []
    for line in p.stdout:
        lines.append(line)
        if line.startswith("https://windsurf.com/editor/signin?"):
            url = line.strip()
            break
    if not url:
        fail("windsurf url", "".join(lines))
    q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(url).query))
    for k, v in (("response_type", "code"), ("code_challenge_method", "S256"), ("prompt", "login"), ("redirect_parameters_type", "query")):
        if q.get(k) != v:
            fail("windsurf params", url)
    if "client_id" in q or not re.match(r"http://127\.0\.0\.1:\d+/callback$", q["redirect_uri"]):
        fail("windsurf params", url)
    time.sleep(0.2)
    urllib.request.urlopen("%s?code=%s&state=%s" % (q["redirect_uri"], code, q["state"]), timeout=10).read()
    rest = p.stdout.read()
    p.wait()
    timer.cancel()
    return home, "".join(lines) + rest


# ---- devin-cli
home, env = fresh("cli")
cli("cli missing", home, env, "No Devin CLI credentials found (probed: HOME/.local/share/devin/credentials.toml, HOME/Library/Application Support/devin/credentials.toml). Run `devin auth login`, then retry.")
creds = os.path.join(home, ".local", "share", "devin")
os.makedirs(creds)
creds = os.path.join(creds, "credentials.toml")
open(creds, "w").write('windsurf_api_key = "sk-ws-1"\napi_server_url = "https://evil.example.com"\n')
cli("cli bad host", home, env, 'point at unsupported API server "https://evil.example.com" (supported: server.codeium.com, server.enterprise.windsurf.com).')
open(creds, "w").write('api_server_url = "https://server.codeium.com"\n[x]\nwindsurf_api_key = "no"\n')
cli("cli no key", home, env, "have no windsurf_api_key. Run `devin auth login`, then retry.")
open(creds, "w").write("windsurf_api_key = = bad\n")
cli("cli bad toml", home, env, "are not valid TOML: TOML Parse error: Expected a value but found '='")
open(creds, "w").write('# seat\nwindsurf_api_key = " sk-ws-1 "\napi_server_url = "https://server.enterprise.windsurf.com:443/x"\n[other]\nk = [1,\n 2]\n')
cli("cli ok", home, env, "Logged in to Devin (adopt CLI credentials)")
if stored(home).get("devin", {}).get("key") != "sk-ws-1" or "devin-cli" in stored(home):
    fail("cli store-as", json.dumps(stored(home)))
print("cli store-as ok")

# ---- devin-windsurf
home, out = windsurf("code-1")
if "Logged in to Devin (Windsurf Enterprise)" not in out:
    fail("windsurf login", out)
if "Sign in with your Windsurf (Enterprise) account in your browser." not in out:
    fail("windsurf instructions", out)
path, headers, body = SEEN[-1]
if path != "/exa.seat_management_pb.SeatManagementService/ExchangePKCEAuthorizationCode":
    fail("windsurf exchange", path)
if headers.get("content-type") != "application/proto" or headers.get("connect-protocol-version") != "1" or headers.get("accept") != "*/*":
    fail("windsurf headers", str(headers))
if body[:2] != b"\x0a\x06" or body[2:8] != b"code-1" or body[8] != 0x12 or body[9] != 0x80 or body[10] != 1 or len(body) != 11 + 128:
    fail("windsurf request", repr(body[:16]))
if stored(home).get("devin", {}).get("key") != "sk-ws-seat":
    fail("windsurf store-as", json.dumps(stored(home)))
print("windsurf login ok")
home, out = windsurf("code-gz")
if stored(home).get("devin", {}).get("key") != "sk-ws-zipped":
    fail("windsurf gzip", out)
print("windsurf gzip ok")
home, out = windsurf("code-empty")
if "Windsurf code exchange returned no API key" not in out:
    fail("windsurf no key", out)
print("windsurf no key ok")
home, out = windsurf("code-bad")
if "Windsurf code exchange failed: 403 denied" not in out:
    fail("windsurf denied", out)
print("windsurf denied ok")
srv.shutdown()
