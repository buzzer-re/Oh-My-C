#!/usr/bin/env python3
"""ACP wire parity: drive `omp acp` and `omc acp` with the same scripted JSON-RPC against the same mock model
and diff the normalized transcripts (every frame the agent sends, in arrival order).

    tests/parity/acp_diff.py [scenario ...] [--only omc|omp] [--keep] [--raw]

Scenarios (default: all): basic, tools, errors, load, bridge, elicit, steer, mcp.
The fake client answers session/request_permission (allow_once), and, when the scenario advertises the
capabilities, fs/read_text_file (from an in-memory "unsaved buffer" map, else disk), fs/write_text_file (disk),
terminal/* (a local subprocess) and elicitation/create (accepts the first option).
Ids (uuids, session ids), timestamps, the work/agent dirs and the product name are normalized.
Env: OMP_BIN (default omp), OMC_BIN (default build/omc), AD_DIR (scratch, default $TMPDIR/omc-acpdiff).
Exit status 1 when a transcript differs (or with --only, never). Dev-time tool: never touches ~/.omp."""
import difflib, json, os, re, select, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
D = os.environ.get("AD_DIR") or os.path.join(os.environ.get("TMPDIR") or "/tmp", "omc-acpdiff")
BINS = {"omp": os.environ.get("OMP_BIN") or "omp", "omc": os.environ.get("OMC_BIN") or os.path.join(ROOT, "build", "omc")}
ENVVAR = {"omp": "PI_CODING_AGENT_DIR", "omc": "OMC_CODING_AGENT_DIR"}


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Client:
    """One agent process; records every frame it sends (and our requests) as transcript lines."""

    def __init__(self, app, work, caps, extra_args=()):
        self.app, self.work, self.caps = app, work, caps
        self.lines, self.next_id, self.buffers, self.terms = [], 100, {}, {}
        self.elicit_answer = None
        self.permission = "allow_once"
        env = dict(os.environ, MOCK_KEY="x", OMC_NO_TITLE="1", PI_NO_TITLE="1", OMP_SKIP_SETUP="1", OMC_SKIP_SETUP="1",
                   HOME=os.path.join(D, app + "-home"))
        env[ENVVAR[app]] = os.path.join(D, app + "a")
        os.makedirs(os.path.join(env["HOME"], ".omp"), exist_ok=True)
        natives = os.path.join(os.path.expanduser("~"), ".omp", "natives")  # omp's extracted native module (read-only)
        link = os.path.join(env["HOME"], ".omp", "natives")
        if os.path.isdir(natives) and not os.path.lexists(link):
            os.symlink(natives, link)
        self.p = subprocess.Popen([BINS[app], "acp", *extra_args], cwd=work, env=env, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=open(os.path.join(D, app + ".stderr"), "a"))
        self.buf = b""

    def send(self, o):
        o = {"jsonrpc": "2.0", **o}
        self.p.stdin.write((json.dumps(o) + "\n").encode())
        self.p.stdin.flush()

    def record(self, tag, o):
        self.lines.append((tag, o))

    def read_frame(self, timeout):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0:
                return None
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r:
                return None
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return json.loads(line)

    def pump(self, until_id=None, timeout=60.0, settle=0.4):
        """Read frames until the response to until_id arrives, then keep reading `settle` seconds of quiet."""
        end = time.time() + timeout
        got = until_id is None
        resp = None
        while True:
            wait = settle if got else max(0.05, end - time.time())
            m = self.read_frame(wait)
            if m is None:
                if got or time.time() >= end:
                    return resp
                continue
            if "method" in m and "id" in m:
                self.record("<", m)
                self.answer(m)
            elif "method" in m:
                self.record("<", m)
            else:
                self.record("<", m)
                if m.get("id") == until_id:
                    got, resp = True, m

    def call(self, method, params, timeout=60.0, settle=0.4):
        self.next_id += 1
        rid = self.next_id
        o = {"id": rid, "method": method, "params": params}
        self.record(">", o)
        self.send(o)
        return self.pump(rid, timeout, settle)

    def notify(self, method, params):
        o = {"method": method, "params": params}
        self.record(">", o)
        self.send(o)

    # ----- client side of agent requests -----
    def answer(self, m):
        method, params, rid = m["method"], m.get("params") or {}, m["id"]
        res = None
        if method == "session/request_permission":
            res = {"outcome": {"outcome": "selected", "optionId": self.permission}}
        elif method == "fs/read_text_file":
            path = params["path"]
            if path in self.buffers:
                text = self.buffers[path]
            else:
                try:
                    text = open(path).read()
                except OSError as e:
                    self.send({"id": rid, "error": {"code": -32002, "message": f"Resource not found: {path}"}})
                    return
            if params.get("line") or params.get("limit"):
                ls = text.split("\n")
                s = max(0, (params.get("line") or 1) - 1)
                text = "\n".join(ls[s:s + params["limit"]] if params.get("limit") else ls[s:])
            res = {"content": text}
        elif method == "fs/write_text_file":
            with open(params["path"], "w") as f:
                f.write(params["content"])
            res = None
        elif method == "terminal/create":
            tid = f"term-{len(self.terms) + 1}"
            env = dict(os.environ)
            for e in params.get("env") or []:
                env[e["name"]] = e["value"]
            pr = subprocess.Popen([params["command"], *(params.get("args") or [])], cwd=params.get("cwd") or self.work,
                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = []
            t = threading.Thread(target=lambda: out.append(pr.stdout.read()), daemon=True)
            t.start()
            self.terms[tid] = (pr, out, t)
            res = {"terminalId": tid}
        elif method.startswith("terminal/"):
            pr, out, t = self.terms[params["terminalId"]]
            if method == "terminal/output":
                if pr.poll() is not None:
                    t.join()
                text = out[0].decode() if out else ""
                res = {"output": text, "truncated": False}
                if pr.poll() is not None:
                    res["exitStatus"] = {"exitCode": pr.returncode, "signal": None}
            elif method == "terminal/wait_for_exit":
                pr.wait()
                t.join()
                res = {"exitCode": pr.returncode, "signal": None}
            elif method == "terminal/kill":
                if pr.poll() is None:
                    pr.kill()
                res = {}
            else:
                res = {}
        elif method == "elicitation/create":
            if self.elicit_answer is not None:
                res = self.elicit_answer(params)
            else:
                props = params["requestedSchema"].get("properties") or {}
                content = {}
                for k, v in props.items():
                    if k.endswith("__other"):
                        continue
                    if v.get("enum"):
                        content[k] = v["enum"][0]
                    elif v.get("oneOf"):
                        content[k] = v["oneOf"][0]["const"]
                    elif v.get("type") == "boolean":
                        content[k] = True
                    elif v.get("type") == "array":
                        content[k] = [v["items"]["anyOf"][0]["const"]]
                    else:
                        content[k] = "typed answer"
                res = {"action": "accept", "content": content}
        else:
            self.send({"id": rid, "error": {"code": -32601, "message": f'"Method not found": {method}'}})
            return
        self.send({"id": rid, "result": res})

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        # drain whatever the agent still writes while shutting down
        self.pump(None, 5, 0.5)
        try:
            self.p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
        for pr, _, _ in self.terms.values():
            if pr.poll() is None:
                pr.kill()


# ---------------- scenarios ----------------

def init(c, caps=None):
    return c.call("initialize", {"protocolVersion": 1, "clientCapabilities": caps or {}}, settle=0.1)


def new(c, mcp=None):
    r = c.call("session/new", {"cwd": c.work, "mcpServers": mcp or []}, settle=0.4)
    return r["result"]["sessionId"] if r and "result" in r else None


def prompt(c, sid, text, **kw):
    return c.call("session/prompt", {"sessionId": sid, "prompt": [{"type": "text", "text": text}]}, **kw)


def sc_basic(c):
    init(c)
    c.call("authenticate", {"methodId": "agent"}, settle=0.05)
    c.call("authenticate", {"methodId": "bogus"}, settle=0.05)
    sid = new(c)
    prompt(c, sid, "say:hello there")
    c.call("session/set_config_option", {"sessionId": sid, "configId": "mode", "value": "plan"})
    c.call("session/set_mode", {"sessionId": sid, "modeId": "default"})
    c.call("session/set_mode", {"sessionId": sid, "modeId": "nope"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "thinking", "value": "high"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "thinking", "value": "bogus"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "model", "value": "mock/mock-1"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "model", "value": "mock/none"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "nope", "value": "x"})
    c.call("session/set_config_option", {"sessionId": sid, "configId": "mode", "value": True})
    c.call("session/list", {"cwd": c.work})
    c.call("session/list", {"cursor": "abc"})
    c.call("session/list", {"cwd": "relative"})
    c.call("_session/steering", {"sessionId": sid, "prompt": [{"type": "text", "text": "steer me"}]})
    c.call("_session/steering", {"sessionId": sid, "prompt": []})
    c.call("_session/steering", {"sessionId": "nope", "prompt": "x"})
    c.call("_session/steering", {"sessionId": sid, "prompt": [{"type": "video"}]})
    c.call("_omp/nothing", {})
    c.call("session/close", {"sessionId": sid})
    c.call("session/close", {"sessionId": sid})
    prompt(c, sid, "say:after close")
    c.call("session/new", {"cwd": "relative/dir", "mcpServers": []})
    c.call("session/prompt", {"sessionId": "missing", "prompt": [{"type": "text", "text": "x"}]})


def sc_tools(c):
    init(c)
    sid = new(c)
    with open(os.path.join(c.work, "a.txt"), "w") as f:
        f.write("alpha\nbeta\n")
    prompt(c, sid, "bash:echo acp-bash")
    prompt(c, sid, "read:a.txt")
    prompt(c, sid, "write:b.txt:hello world")
    prompt(c, sid, "todo:first task,second task")
    prompt(c, sid, "grep:alpha")
    prompt(c, sid, "/dirs")
    prompt(c, sid, "think:pondering things|thought done")


def sc_errors(c):
    init(c)
    sid = new(c)
    prompt(c, sid, "fail:400:content_filter triggered")
    prompt(c, sid, "fail:400:plain failure")
    prompt(c, sid, "streamfail:partial text")


def sc_load(c):
    init(c)
    sid = new(c)
    with open(os.path.join(c.work, "a.txt"), "w") as f:
        f.write("alpha\nbeta\n")
    prompt(c, sid, "bash:echo first")
    prompt(c, sid, "read:a.txt")
    prompt(c, sid, "say:final words")
    c.close()
    c2 = Client(c.app, c.work, c.caps)
    c.lines.append(("#", "restart"))
    c2.lines = c.lines
    init(c2)
    c2.call("session/load", {"sessionId": sid, "cwd": c.work, "mcpServers": []}, settle=0.4)
    c2.call("session/load", {"sessionId": sid, "cwd": "/elsewhere", "mcpServers": []}, settle=0.2)
    c2.call("session/resume", {"sessionId": sid, "cwd": c.work}, settle=0.4)
    r = c2.call("session/fork", {"sessionId": sid, "cwd": c.work, "mcpServers": []}, settle=0.4)
    fid = (r or {}).get("result", {}).get("sessionId")
    if fid:
        prompt(c2, fid, "say:in the fork")
    c2.call("session/load", {"sessionId": "does-not-exist", "cwd": c.work, "mcpServers": []}, settle=0.2)
    c2.call("session/resume", {"sessionId": "does-not-exist", "cwd": c.work}, settle=0.2)
    c2.call("session/fork", {"sessionId": "does-not-exist", "cwd": c.work}, settle=0.2)
    return c2


def sc_bridge(c):
    init(c, {"fs": {"readTextFile": True, "writeTextFile": True}, "terminal": True})
    sid = new(c)
    with open(os.path.join(c.work, "buf.txt"), "w") as f:
        f.write("on disk\n")
    c.buffers[os.path.join(c.work, "buf.txt")] = "unsaved editor buffer\nsecond line\n"
    prompt(c, sid, "read:buf.txt")
    prompt(c, sid, "write:out.txt:bridged write")
    prompt(c, sid, "bash:echo via-terminal")


def sc_elicit(c):
    init(c, {"elicitation": {"form": {}}})
    sid = new(c)
    q = {"questions": [{"id": "color", "question": "Pick a color", "options": [{"label": "Red"}, {"label": "Blue"}]}]}
    prompt(c, sid, "call:ask:" + json.dumps(q))


def sc_steer(c):
    init(c)
    sid = new(c)
    c.next_id += 1
    rid = c.next_id
    o = {"id": rid, "method": "session/prompt", "params": {"sessionId": sid, "prompt": [{"type": "text", "text": "slow:1.5:slow answer"}]}}
    c.record(">", o)
    c.send(o)
    time.sleep(0.5)
    c.call("_session/steering", {"sessionId": sid, "prompt": [{"type": "text", "text": "steered text"}]}, settle=0.05)
    c.pump(rid, 30, 0.5)
    c.next_id += 1
    rid = c.next_id
    o = {"id": rid, "method": "session/prompt", "params": {"sessionId": sid, "prompt": [{"type": "text", "text": "slow:2:to be cancelled"}]}}
    c.record(">", o)
    c.send(o)
    time.sleep(0.5)
    c.notify("session/cancel", {"sessionId": sid})
    c.pump(rid, 30, 0.8)


def sc_mcp(c):
    init(c)
    mock = os.path.join(ROOT, "tests", "e2e", "mock_mcp.py")
    servers = [{"name": "mock", "command": sys.executable, "args": ["-I", mock], "env": []},
               {"name": "broken", "command": "/nonexistent/server", "args": [], "env": []}]
    sid = new(c, servers)
    time.sleep(0.5)
    c.pump(None, 2, 0.5)
    prompt(c, sid, "call:mcp__mock_echo_text:" + json.dumps({"text": "via mcp"}))


SCENARIOS = {"basic": sc_basic, "tools": sc_tools, "errors": sc_errors, "load": sc_load, "bridge": sc_bridge,
             "elicit": sc_elicit, "steer": sc_steer, "mcp": sc_mcp}

# ---------------- normalization ----------------

UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
TS = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z")


def normalize(app, lines, work):
    ids = {}

    def sub_id(m):
        return ids.setdefault(m.group(0), f"<id{len(ids) + 1}>")

    def s(x):
        x = x.replace(os.path.join(D, app + "a"), "<agent>").replace(work, "<cwd>").replace(D, "<D>")
        x = UUID.sub(sub_id, x)
        x = TS.sub("<ts>", x)
        if app == "omp":
            x = re.sub(r"\bomp\b", "omc", x).replace("~/.omp", "~/.omc").replace(".omp/", ".omc/")
        return x

    def walk(o, key=None):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                if k == "version" and isinstance(v, str):
                    v = "<ver>"
                if k == "_meta" and isinstance(v, dict) and "size" in v:  # session file sizes differ by format details
                    v = {**v, "size": "<n>"}
                if k == "availableCommands" and isinstance(v, list) and "--full-commands" not in sys.argv:
                    v = [c.get("name") for c in v]  # the registry is compared by name (--full-commands: everything)
                if k in ("wallTimeMs", "durationMs") and isinstance(v, (int, float)):
                    v = "<ms>"
                out[k] = walk(v, k)
            return out
        if isinstance(o, list):
            return [walk(v, key) for v in o]
        if isinstance(o, str):
            return s(o)
        return o

    out = []
    for tag, o in lines:
        out.append(f"{tag} {json.dumps(walk(o), ensure_ascii=False) if not isinstance(o, str) else o}")
    return out


def setup(app, port):
    a = os.path.join(D, app + "a")
    shutil.rmtree(a, ignore_errors=True)
    os.makedirs(a)
    with open(os.path.join(a, "models.yml"), "w") as f:
        f.write(f"""providers:
  mock:
    baseUrl: http://127.0.0.1:{port}/v1
    apiKey: MOCK_KEY
    api: openai-completions
    models:
      - id: mock-1
        name: Mock One
        contextWindow: 100000
        maxTokens: 4096
""")
    with open(os.path.join(a, "config.yml"), "w") as f:
        f.write("modelRoles:\n  default: mock/mock-1\n")


def run(app, name, port):
    setup(app, port)
    work = os.path.join(D, "work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    c = Client(app, work, {})
    try:
        last = SCENARIOS[name](c) or c
    except Exception as e:  # keep the transcript so far
        c.lines.append(("#", f"harness error: {e!r}"))
        last = c
    last.close()
    if last is not c:
        c.close()
    return normalize(app, last.lines, work)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
        args = [a for a in args if a != only]
    names = args or list(SCENARIOS)
    os.makedirs(D, exist_ok=True)
    port = free_port()
    mock = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tests", "e2e", "mock_openai.py"), str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            env=dict(os.environ, MOCK_LOG=os.path.join(D, "mock.jsonl")))
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), 0.1).close()
            break
        except OSError:
            time.sleep(0.1)
    rc = 0
    try:
        for name in names:
            res = {}
            for app in (["omp", "omc"] if not only else [only]):
                res[app] = run(app, name, port)
                with open(os.path.join(D, f"{name}.{app}"), "w") as f:
                    f.write("\n".join(res[app]) + "\n")
            if only:
                print(f"== {name} ({only})")
                print("\n".join(res[only]))
                continue
            diff = list(difflib.unified_diff(res["omp"], res["omc"], "omp", "omc", lineterm="", n=1))
            if diff:
                rc = 1
                print(f"== {name}: DIFFERS")
                print("\n".join(diff))
            else:
                print(f"== {name}: same ({len(res['omc'])} frames)")
    finally:
        mock.kill()
        mock.wait()
        if "--keep" not in sys.argv:
            for app in ("omp", "omc"):
                shutil.rmtree(os.path.join(D, app + "-home"), ignore_errors=True)
    return 0 if only else rc


if __name__ == "__main__":
    sys.exit(main())
