#!/usr/bin/env python3
"""Dev-only: drive omp's and omc's `--mode rpc` with the same scripted stdin against the same mock model
(throwaway agent dirs, tests/e2e/mock_openai.py) and diff the normalized frame streams.

  tests/parity/rpc_diff.py <script.json> [--shape] [--only omc|omp] [--keep]

The script is a JSON list of steps, run in order:
  {"send": {...}}                    write one command line ({"raw": "..."} sends the text as is)
  {"wait": "response:<id>"}          until the response with that id
  {"wait": "prompt_result:<id>"}     until that prompt_result
  {"wait": "<frame type>"}           until the next frame of that type (counting from the last wait)
  {"sleep": <seconds>}
  {"ui": [<response>, ...]}          queue answers for the next extension_ui_request dialogs (select / confirm /
                                     input / editor / ask), in order; "id" is filled in
  {"setup": "<shell>"}               run in the agent dir before start (first step only; $AGENT = agent dir)
Frames are normalized (timestamps, durations, ids, session paths, the agent dir and cwd) and written to
$RD_DIR/{omp,omc}.frames (one pretty JSON per frame); the unified diff is printed. --shape replaces scalar values
with their type (string values of type/command/method/role/status/... are kept) to compare wire shapes only.
Env: RD_DIR (default $TMPDIR/omc-rpcdiff), OMP_BIN (default omp), OMC_BIN (default build/omc), RD_ARGS (extra CLI
args for both, e.g. "--no-session"), RD_TIMEOUT (per wait, default 30 s), RD_MODE (rpc or rpc-ui), RD_SKIP (frame types left out of the diff,
e.g. "available_commands_update"), RD_DROP (keys removed everywhere before diffing, e.g. "partial,dumpTools").
omp's bundled JS `autoresearch` extension widget frames are always left out (JS extensions are out of scope)."""
import difflib, json, os, queue, random, re, shlex, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if len(sys.argv) > 2 and sys.argv[1] == "--view":  # --view <frames file> [width] [skip,types]: one frame per line
    txt, dec, i = open(sys.argv[2]).read(), json.JSONDecoder(), 0
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    skip = set(sys.argv[4].split(",")) if len(sys.argv) > 4 else set()
    while True:
        while i < len(txt) and txt[i].isspace(): i += 1
        if i >= len(txt): break
        o, i = dec.raw_decode(txt, i)
        if o.get("type") not in skip: print(json.dumps(o, ensure_ascii=False)[:width])
    sys.exit(0)
args =[a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
only = None
for i, a in enumerate(sys.argv):
    if a == "--only" and i + 1 < len(sys.argv):
        only = sys.argv[i + 1]
        if only in args: args.remove(only)
if not args:
    sys.exit(__doc__)
SCRIPT = json.load(open(args[0]))
SHAPE = "--shape" in flags
D = os.environ.get("RD_DIR") or os.path.join(os.environ.get("TMPDIR") or "/tmp", "omc-rpcdiff")
TIMEOUT = float(os.environ.get("RD_TIMEOUT") or 30)

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p

def setup_agent(d, port):
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    with open(os.path.join(d, "models.yml"), "w") as f:
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
      - id: mock-2
        name: Mock Two
        reasoning: true
        contextWindow: 200000
        maxTokens: 8192
""")
    with open(os.path.join(d, "config.yml"), "w") as f:
        f.write("modelRoles:\n  default: mock/mock-1\n")
    for st in SCRIPT:
        if "setup" in st:
            subprocess.run(st["setup"], shell=True, cwd=d, env={**os.environ, "AGENT": d}, check=False)

def run(name, binary, envvar):
    port = free_port()
    agent = os.path.join(D, name + "-agent")
    work = os.path.join(D, name + "-work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    setup_agent(agent, port)
    log = os.path.join(D, name + ".mock.jsonl")
    if os.path.exists(log): os.remove(log)
    mock = subprocess.Popen([sys.executable, "-I", os.path.join(ROOT, "tests/e2e/mock_openai.py"), str(port)],
                            env={**os.environ, "MOCK_LOG": log}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", port), 0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    env = {**os.environ, envvar: agent, "MOCK_KEY": "x", "OMC_NO_TITLE": "1", "PI_NO_TITLE": "1",
           "OMC_SKIP_SETUP": "1", "OMP_SKIP_SETUP": "1", "HOME": os.path.join(D, name + "-home")}
    os.makedirs(env["HOME"], exist_ok=True)
    extra = shlex.split(os.environ.get("RD_ARGS", ""))
    p = subprocess.Popen([binary, "--mode", os.environ.get("RD_MODE", "rpc"), *extra], cwd=work, env=env, stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=open(os.path.join(D, name + ".stderr"), "w"))
    frames, q = [], queue.Queue()
    ui_answers = []

    def reader():
        for line in p.stdout:
            raw = line.decode("utf-8", "replace").rstrip("\n")
            try:
                m = json.loads(raw)
            except ValueError:
                m = {"type": "<unparsed>", "line": raw[:500]}
            frames.append(m)
            q.put(m)
        q.put(None)
    threading.Thread(target=reader, daemon=True).start()

    def send(o):
        line = o["raw"] if isinstance(o, dict) and "raw" in o else json.dumps(o)
        try:
            p.stdin.write((line + "\n").encode())
            p.stdin.flush()
        except BrokenPipeError:
            pass

    seen = 0
    def wait(spec, limit=None):
        nonlocal seen
        deadline = time.time() + (TIMEOUT if limit is None else limit)
        while time.time() < deadline:
            while seen < len(frames):
                m = frames[seen]
                seen += 1
                if m.get("type") == "extension_ui_request" and m.get("method") in ("select", "confirm", "input", "editor", "ask") and ui_answers:
                    a = dict(ui_answers.pop(0))
                    a.update({"type": "extension_ui_response", "id": m["id"]})
                    send(a)
                kind, _, val = spec.partition(":")
                if val and m.get("type") == kind and m.get("id") == val: return True
                if not val and m.get("type") == spec: return True
            time.sleep(0.02)
        if limit is None: frames.append({"type": "<timeout>", "waiting": spec})
        return False

    for st in SCRIPT:
        if "send" in st: send(st["send"])
        elif "wait" in st: wait(st["wait"])
        elif "sleep" in st: wait("\0", st["sleep"])  # keeps answering dialogs while sleeping
        elif "ui" in st: ui_answers.extend(st["ui"])
    try:
        p.stdin.close()
    except BrokenPipeError:
        pass
    try:
        p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    time.sleep(0.2)
    mock.kill()
    mock.wait()
    return frames, {agent: "<AGENT>", work: "<CWD>", os.path.realpath(work): "<CWD>", env["HOME"]: "<HOME>"}

TIME_KEYS = {"timestamp", "createdAt", "updatedAt", "durationMs", "startedAt", "lastUpdate", "elapsedMs", "ttftMs",
             "tokensPerSecond", "modifiedAt", "mtime", "lastActivity", "ttft", "latencyMs", "duration"}
KEEP_STR = {"type", "command", "method", "role", "status", "stopReason", "level", "source", "api", "provider",
            "customType", "reason", "action", "queue", "mode", "thinkingLevel", "steeringMode", "followUpMode",
            "interruptMode", "notifyType", "toolName", "name", "error", "errorMessage", "code", "op"}
ID_RX = re.compile(r"^(?:[0-9a-f]{8}|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|\d{15,20}|[0-9a-z]{20,32})$")

def normalizer(paths):
    ids = {}
    def path_sub(s):
        for k in sorted(paths, key=len, reverse=True):
            s = s.replace(k, paths[k])
        s = re.sub(r"\d{4}-\d{2}-\d{2}T\d{2}[-:]\d{2}[-:]\d{2}[-.:]\d{3}Z", "<TIME>", s)
        return s
    def norm(v, key=None):
        if isinstance(v, dict):
            return {k: norm(x, k) for k, x in v.items()}
        if isinstance(v, list):
            return [norm(x, key) for x in v]
        if key in TIME_KEYS and isinstance(v, (int, float, str)) and v is not None:
            return "<T>"
        if isinstance(v, str):
            if ID_RX.match(v) and key not in ("text", "delta", "content"):
                if v not in ids: ids[v] = f"<ID{len(ids) + 1}>"
                return ids[v]
            v = path_sub(v)
            for k, x in list(ids.items()):
                if len(k) >= 8 and k in v: v = v.replace(k, x)
            if SHAPE and key not in KEEP_STR: return "<str>" if v else ""
            return v
        if SHAPE and isinstance(v, bool): return "<bool>"
        if SHAPE and isinstance(v, (int, float)): return "<num>"
        return v
    return norm

SKIP = set(filter(None, os.environ.get("RD_SKIP", "").split(",")))
DROP = set(filter(None, os.environ.get("RD_DROP", "").split(",")))

def drop_keys(v):
    if isinstance(v, dict): return {k: drop_keys(x) for k, x in v.items() if k not in DROP}
    if isinstance(v, list): return [drop_keys(x) for x in v]
    return v

def dump(name, frames, paths):
    norm = normalizer(paths)
    out = []
    for m in frames:
        if m.get("type") in SKIP or (m.get("method") == "setWidget" and m.get("widgetKey") == "autoresearch"): continue
        m = drop_keys(m)
        out.append(json.dumps(norm(m), indent=1, ensure_ascii=False, sort_keys=SHAPE))
    with open(os.path.join(D, name + ".frames"), "w") as f:
        f.write("\n".join(out) + "\n")
    return "\n".join(out).splitlines()

os.makedirs(D, exist_ok=True)
res = {}
for name, binary, envvar in (("omp", os.environ.get("OMP_BIN", "omp"), "PI_CODING_AGENT_DIR"),
                             ("omc", os.environ.get("OMC_BIN", os.path.join(ROOT, "build/omc")), "OMC_CODING_AGENT_DIR")):
    if only and only != name: continue
    frames, paths = run(name, binary, envvar)
    res[name] = dump(name, frames, paths)
    print(f"{name}: {len(frames)} frames: " + " ".join(f.get("type", "?") + (":" + f["command"] if f.get("type") == "response" else "") for f in frames))
if len(res) == 2:
    diff = list(difflib.unified_diff(res["omp"], res["omc"], "omp", "omc", lineterm="", n=2))
    print("\n".join(diff) if diff else "frames identical")
    if "--keep" not in flags:
        for n in ("omp", "omc"):
            for suf in ("-agent", "-work", "-home"):
                shutil.rmtree(os.path.join(D, n + suf), ignore_errors=True)
    sys.exit(1 if diff else 0)
