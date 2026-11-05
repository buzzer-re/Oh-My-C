# omc eval kernel: persistent Python namespace driven by the host over a private JSON-lines channel.
# The host spawns this with stdin/stdout pipes; we keep private duplicates of both for the protocol and point fd 0 at
# /dev/null and fd 1 at fd 2 (captured by the host as raw cell output), so nothing user code writes can corrupt framing.
#   host -> kernel  {"type":"exec","id":N,"code":"..."} | {"type":"reply","id":M,"ok":bool,"value"|"error":...}
#   kernel -> host  {"type":"ready"} | {"type":"call","id":M,"name":"read"|"__judge__"|...,"args":{...}}
#                   {"type":"cancel","id":M} | {"type":"result","id":N,"stdout":"...","error":null|"...","images":[...]}
import ast, asyncio, base64, builtins, io, itertools, json, math, os, queue, re, shlex, subprocess, sys, threading, time, traceback
from pathlib import Path

_PIN = os.dup(0)
_POUT = os.dup(1)
_nul = os.open(os.devnull, os.O_RDONLY)
os.dup2(_nul, 0)
os.close(_nul)
os.dup2(2, 1)
try:
    sys.stdin = open(os.devnull)
except Exception:
    pass

_wlock = threading.Lock()
_plock = threading.Lock()
_pending = {}
_ids = itertools.count(1)
_exec_q = queue.Queue()
_closed = [False]
_ns = {"__name__": "__main__", "__builtins__": builtins}
_cell = {"images": None}


def _send(obj):
    data = (json.dumps(obj, allow_nan=False) + "\n").encode("utf-8")
    with _wlock:
        view = memoryview(data)
        while view:
            n = os.write(_POUT, view)
            view = view[n:]


class _Slot:
    __slots__ = ("event", "ok", "value", "error")

    def __init__(self):
        self.event = threading.Event()
        self.ok = False
        self.value = None
        self.error = None


def _reader():
    buf = b""
    while True:
        try:
            chunk = os.read(_PIN, 65536)
        except InterruptedError:
            continue
        except OSError:
            chunk = b""
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                msg = json.loads(line)
            except Exception:
                continue
            t = msg.get("type")
            if t == "exec":
                _exec_q.put(msg)
            elif t == "reply":
                with _plock:
                    slot = _pending.get(msg.get("id"))
                if slot is not None:
                    slot.ok = bool(msg.get("ok"))
                    slot.value = msg.get("value")
                    slot.error = msg.get("error")
                    slot.event.set()
    _closed[0] = True
    with _plock:
        for slot in _pending.values():
            slot.error = "host closed the eval bridge"
            slot.event.set()
    _exec_q.put(None)


def _bridge_call(name, args):
    """Send one request to the host and block (interruptibly) for its reply."""
    if _closed[0]:
        raise RuntimeError("host closed the eval bridge")
    payload = json.dumps(args, allow_nan=False)  # fail early on non-JSON values
    cid = next(_ids)
    slot = _Slot()
    with _plock:
        _pending[cid] = slot
    try:
        _send({"type": "call", "id": cid, "name": name, "args": json.loads(payload)})
        while not slot.event.wait(0.25):
            if _closed[0]:
                raise RuntimeError("host closed the eval bridge")
    except BaseException:
        try:
            _send({"type": "cancel", "id": cid})
        except Exception:
            pass
        raise
    finally:
        with _plock:
            _pending.pop(cid, None)
    if slot.ok:
        return slot.value
    raise RuntimeError(slot.error or f"bridge call {name!r} failed")


# ---------------------------------------------------------------------------
# images
# ---------------------------------------------------------------------------

def _emit_image(image):
    if _cell["images"] is not None and isinstance(image, dict) and image.get("data") and image.get("mimeType"):
        _cell["images"].append({"data": image["data"], "mimeType": image["mimeType"]})
        return True
    return False


def _png(save):
    buf = io.BytesIO()
    save(buf)
    return {"mimeType": "image/png", "data": base64.b64encode(buf.getvalue()).decode("ascii")}


def _value_image(value):
    """Image for an omp image dict, matplotlib figure, PIL image or _repr_png_/_repr_jpeg_ object."""
    if isinstance(value, dict):
        if value.get("type") == "image" and isinstance(value.get("mimeType"), str) and isinstance(value.get("data"), str):
            return {"mimeType": value["mimeType"], "data": value["data"]}
        return None
    if isinstance(value, (str, bytes, int, float, bool, list, tuple, set, type(None))) or isinstance(value, type):
        return None
    try:
        t = type(value)
        if t.__module__ == "matplotlib.figure" and t.__name__ == "Figure":
            img = _png(lambda b: value.savefig(b, format="png", bbox_inches="tight"))
            try:
                sys.modules["matplotlib.pyplot"].close(value)
            except Exception:
                pass
            return img
        pil = getattr(sys.modules.get("PIL.Image"), "Image", None)
        if isinstance(pil, type) and isinstance(value, pil):
            return _png(lambda b: value.save(b, format="PNG"))
        for attr, mime in (("_repr_png_", "image/png"), ("_repr_jpeg_", "image/jpeg")):
            fn = getattr(value, attr, None)
            if callable(fn):
                data = fn()
                if isinstance(data, tuple):
                    data = data[0] if data else None
                if isinstance(data, (bytes, bytearray)) and data:
                    return {"mimeType": mime, "data": base64.b64encode(bytes(data)).decode("ascii")}
    except Exception:
        return None
    return None


_REAL_PRINT = builtins.print


def _rich_print(*args, sep=" ", end="\n", file=None, flush=False):
    if _cell["images"] is None or (file is not None and file is not sys.stdout):
        return _REAL_PRINT(*args, sep=sep, end=end, file=file, flush=flush)
    texts = []
    for a in args:
        img = _value_image(a)
        if img is not None:
            _emit_image(img)
            texts.append(f"<image {img['mimeType']}>" if isinstance(a, dict) else str(a))
        else:
            texts.append(a)
    _REAL_PRINT(*texts, sep=sep, end=end, file=file, flush=flush)


builtins.print = _rich_print


def _figures():
    plt = sys.modules.get("matplotlib.pyplot")
    if plt is None:
        return
    try:
        for num in list(plt.get_fignums()):
            fig = plt.figure(num)
            _emit_image(_png(lambda b: fig.savefig(b, format="png", bbox_inches="tight")))
        plt.close("all")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# prelude helpers
# ---------------------------------------------------------------------------

_URL_RE = re.compile(r"^([a-z][a-z0-9+.-]*)://(.*)$", re.IGNORECASE)


def _resolve_path(path):
    """local:// maps under the session's local root; other scheme:// paths are not files."""
    if not isinstance(path, str):
        return Path(path)
    m = _URL_RE.match(path)
    if not m:
        return Path(os.path.expanduser(path))
    if m.group(1).lower() != "local":
        raise ValueError(f"Protocol paths are not supported by this helper: {path}")
    root = os.path.abspath(os.environ.get("OMC_EVAL_LOCAL_ROOT") or ".omc-local")
    rel = m.group(2).replace("\\", "/")
    if not rel:
        return Path(root)
    if Path(rel).is_absolute() or ".." in Path(rel).parts:
        raise ValueError(f"Unsafe local:// path (absolute or traversal): {path}")
    return Path(os.path.join(root, rel))


def env(key=None, value=None):
    """Get/set environment variables."""
    if key is None:
        return dict(sorted(os.environ.items()))
    if value is not None:
        os.environ[key] = str(value)
        return str(value)
    return os.environ.get(key)


def read(path, offset=1, limit=None):
    """Read file or read-tool URI contents. offset/limit are 1-indexed lines."""
    m = _URL_RE.match(path) if isinstance(path, str) else None
    if m and m.group(1).lower() != "local":
        if limit is not None and limit <= 0:
            return ""
        sel = path
        if offset > 1 or limit is not None:
            start = max(1, offset)
            sel = f"{path}:{start}-" if limit is None else f"{path}:{start}-{start + limit - 1}"
        return _tool_text(_bridge_call("read", {"path": sel}))
    data = _resolve_path(path).read_text(encoding="utf-8")
    if offset > 1 or limit is not None:
        lines = data.splitlines(keepends=True)
        start = max(0, offset - 1)
        data = "".join(lines[start:start + limit if limit else len(lines)])
    return data


def write(path, content):
    """Write file contents (create parents)."""
    p = _resolve_path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding="utf-8")
    return p


def log(message):
    print(message, file=sys.stderr)


def phase(title):
    print(f"## {title}")


def _apply_query(data, query):
    query = (query or "").strip()
    if query.startswith("."):
        query = query[1:]
    if not query:
        return data
    tokens, cur, i = [], "", 0
    while i < len(query):
        ch = query[i]
        if ch == ".":
            if cur:
                tokens.append(("key", cur))
                cur = ""
        elif ch == "[":
            if cur:
                tokens.append(("key", cur))
                cur = ""
            j = query.find("]", i)
            j = len(query) if j < 0 else j
            inner = query[i + 1:j]
            tokens.append(("key", inner[1:-1]) if inner.startswith('"') and inner.endswith('"') else ("index", int(inner)))
            i = j
        else:
            cur += ch
        i += 1
    if cur:
        tokens.append(("key", cur))
    for kind, v in tokens:
        if kind == "index":
            if not isinstance(data, list) or v >= len(data):
                return None
            data = data[v]
        else:
            if not isinstance(data, dict) or v not in data:
                return None
            data = data[v]
    return data


def output(*ids, format="raw", query=None, offset=None, limit=None):
    """Read task/agent output by ID (agent://<id>)."""
    if not ids:
        raise ValueError("At least one output ID is required")
    if query and (offset is not None or limit is not None):
        raise ValueError("query cannot be combined with offset/limit")
    results = []
    for oid in ids:
        raw = _bridge_call("__output__", {"id": str(oid)})["content"]
        lines = raw.splitlines()
        content, rng = raw, None
        if query:
            try:
                value = json.loads(raw)
            except json.JSONDecodeError as e:
                raise ValueError(f"Output {oid} is not valid JSON: {e}")
            r = _apply_query(value, query)
            content = json.dumps(r, indent=2) if r is not None else "null"
        elif offset is not None or limit is not None:
            start = max(1, offset or 1)
            if start > len(lines):
                raise ValueError(f"Offset {start} is beyond end of output ({len(lines)} lines) for {oid}")
            end = min(len(lines), start + (limit if limit is not None else len(lines) - start + 1) - 1)
            content = "\n".join(lines[start - 1:end])
            rng = {"start_line": start, "end_line": end, "total_lines": len(lines)}
        if format == "stripped":
            content = re.sub(r"\x1b\[[0-9;]*m", "", content)
        if format == "json":
            d = {"id": oid, "line_count": len(lines) if not query else len(content.splitlines()),
                 "char_count": len(raw) if not query else len(content), "content": content}
            if rng:
                d["range"] = rng
            if query:
                d["query"] = query
            results.append(d)
        else:
            results.append({"id": oid, "content": content})
    if len(ids) == 1:
        return results[0] if format == "json" else results[0]["content"]
    return results


def _tool_text(value):
    if isinstance(value, dict):
        for img in value.get("images") or []:
            _emit_image(img)
        return value.get("text", "")
    return value


class _ToolCallable:
    __slots__ = ("_name",)

    def __init__(self, name):
        self._name = name

    def __repr__(self):
        return f"<tool.{self._name}>"

    async def __call__(self, args=None, /, **kwargs):
        if args is None:
            merged = {}
        elif isinstance(args, dict):
            merged = dict(args)
        else:
            raise TypeError(f"tool.{self._name}(...) expects a dict of arguments (got {type(args).__name__})")
        merged.update(kwargs)
        return _tool_text(await asyncio.to_thread(_bridge_call, self._name, merged))


class _ToolProxy:
    __slots__ = ()

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return _ToolCallable(name)

    def __getitem__(self, name):
        return _ToolCallable(name)

    def __repr__(self):
        return "<tool proxy>"


tool = _ToolProxy()

_UNSET = object()


class CompletionHandle:
    """Background one-shot completion handle returned by ``completion()``."""

    __slots__ = ("id", "_schema", "_result")
    kind = "completion"

    def __init__(self, id, schema=None):
        self.id = id
        self._schema = schema
        self._result = _UNSET

    @property
    def status(self):
        snap = _bridge_call("__status__", {"item": {"kind": self.kind, "id": self.id}})
        return snap.get("status") if isinstance(snap, dict) else "failed"

    @property
    def attempts(self):
        snap = _bridge_call("__status__", {"item": {"kind": self.kind, "id": self.id}})
        return snap.get("attempts", []) if isinstance(snap, dict) else []

    def done(self):
        return self.status != "running"

    def wait(self, timeout=None):
        if self._result is not _UNSET:
            return self._result
        return wait([self], timeout=timeout)[0]

    def cancel(self):
        r = _bridge_call("__cancel__", {"item": {"kind": self.kind, "id": self.id}})
        return bool(r.get("cancelled")) if isinstance(r, dict) else False

    def __await__(self):
        return asyncio.to_thread(self.wait).__await__()

    def __repr__(self):
        return f"<completion {self.id}>"


def _handle_value(handle, snap):
    status = snap.get("status") if isinstance(snap, dict) else "failed"
    if status == "running":
        raise TimeoutError(f"{handle.kind} handle {handle.id} is still running")
    if status in ("failed", "cancelled"):
        msg = snap.get("error") if isinstance(snap, dict) else None
        raise RuntimeError(msg or f"{handle.kind} handle {handle.id} failed")
    if "data" in snap:
        value = snap["data"]
    else:
        text = snap.get("text", "")
        value = json.loads(text) if handle._schema is not None else text
    handle._result = value
    return value


def wait(handles, timeout=None, *, raise_errors=True):
    """Wait for completion handles in input order."""
    items = [handles] if isinstance(handles, CompletionHandle) else list(handles)
    for h in items:
        if not isinstance(h, CompletionHandle):
            raise TypeError("wait() expects agent or completion handles")
    results = [None] * len(items)
    pending, idx = [], []
    for i, h in enumerate(items):
        if h._result is _UNSET:
            pending.append({"kind": h.kind, "id": h.id})
            idx.append(i)
        else:
            results[i] = h._result
    if pending:
        args = {"items": pending}
        if timeout is not None:
            args["timeoutMs"] = max(0, float(timeout) * 1000)
        resp = _bridge_call("__wait__", args)
        snaps = resp.get("items", []) if isinstance(resp, dict) else []
        if len(snaps) != len(pending):
            raise RuntimeError("wait() returned an incomplete handle result")
        for i, snap in zip(idx, snaps):
            try:
                results[i] = _handle_value(items[i], snap)
            except (RuntimeError, ValueError) as e:
                results[i] = e if isinstance(e, RuntimeError) else RuntimeError(str(e))
    if raise_errors:
        for r in results:
            if isinstance(r, RuntimeError):
                raise r
    return results


def completion(prompt, *, model="default", system=None, schema=None):
    """Start a stateless completion and return its handle."""
    args = {"prompt": prompt, "model": model}
    if system is not None:
        args["system"] = system
    if schema is not None:
        args["schema"] = schema
    r = _bridge_call("__completion__", args)
    if not isinstance(r, dict) or not isinstance(r.get("id"), str):
        raise RuntimeError("completion() did not return a handle")
    return CompletionHandle(r["id"], schema)


def _check_questions(questions):
    if not isinstance(questions, dict):
        raise TypeError("judge(state, questions) expects questions as a dict keyed by id")


async def judge(state, questions, *, images=None):
    """Answer typed questions over one ``state``; returns ``{id: answer}`` (choice/bool/score)."""
    _check_questions(questions)
    args = {"state": state, "questions": questions}
    if images is not None:
        args["images"] = images
    r = await asyncio.to_thread(_bridge_call, "__judge__", args)
    if not isinstance(r, dict) or not isinstance(r.get("answers"), dict):
        raise RuntimeError("judge() did not return answers")
    return r["answers"]


class JudgmentItem:
    """One settled ``judge_batch`` item: ``answers`` on success, else ``error``."""

    __slots__ = ("key", "answers", "error", "model")

    def __init__(self, item):
        self.key = item.get("key")
        self.answers = item.get("answers")
        self.error = item.get("error")
        self.model = item.get("model")

    @property
    def ok(self):
        return self.error is None

    def __repr__(self):
        if self.error is not None:
            return f"<judgment {self.key!r} error={self.error!r}>"
        return f"<judgment {self.key!r} {self.answers!r}>"


class JudgmentBatch:
    """Host-owned bulk judgment run. Pull settled items with ``await drain()`` across as many cells as needed."""

    __slots__ = ("id", "total", "intent")

    def __init__(self, id, total, intent):
        self.id = id
        self.total = total
        self.intent = intent

    def _call(self, op, **args):
        return _bridge_call("__judge_batch__", {"op": op, "id": self.id, **args})

    def status(self):
        return self._call("status")

    async def drain(self, timeout=None):
        args = {}
        if timeout is not None:
            args["timeoutMs"] = max(0, float(timeout) * 1000)
        r = await asyncio.to_thread(self._call, "drain", **args)
        items = r.get("items", []) if isinstance(r, dict) else []
        return [(it.get("key"), JudgmentItem(it)) for it in items]

    async def drain_iter(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + float(timeout)
        while True:
            remaining = None if deadline is None else max(0.0, deadline - time.monotonic())
            items = await self.drain(remaining)
            if not items:
                return
            for entry in items:
                yield entry

    def results(self):
        r = self._call("results")
        return r.get("results", {}) if isinstance(r, dict) else {}

    def failed(self):
        r = self._call("failed")
        return r.get("failed", {}) if isinstance(r, dict) else {}

    def cancel(self):
        r = self._call("cancel")
        return bool(r.get("cancelled")) if isinstance(r, dict) else False

    def close(self):
        self._call("close")
        return None

    def __repr__(self):
        return f"<judge_batch {self.id} total={self.total}>"


def _judge_batch_from(r):
    if not isinstance(r, dict) or not isinstance(r.get("id"), str):
        raise RuntimeError("judge_batch() did not return a batch")
    return JudgmentBatch(r["id"], int(r.get("total") or 0), r.get("intent") or "Judging")


def judge_batch(states, questions, *, images=None, concurrency=None, retries=None, min_ok=None, intent=None):
    """Judge every state with the same ``questions`` on the host; returns a ``JudgmentBatch``."""
    _check_questions(questions)
    if intent is not None and (not isinstance(intent, str) or not intent.strip()):
        raise TypeError("judge_batch() intent must be a non-empty string")
    if isinstance(states, dict):
        items = [{"key": k, "state": s} for k, s in states.items()]
    elif isinstance(states, (list, tuple)):
        items = [{"key": i, "state": s} for i, s in enumerate(states)]
    else:
        raise TypeError("judge_batch(states, questions) expects states as a dict or list")
    if images is not None:
        if not isinstance(images, dict):
            raise TypeError("judge_batch() images must be a dict keyed by input key")
        keys = {it["key"] for it in items}
        for k in images:
            if k not in keys:
                raise ValueError(f"images contains unknown input key {json.dumps(k)}")
        for it in items:
            if it["key"] in images:
                it["images"] = images[it["key"]]
    args = {"op": "create", "items": items, "questions": questions}
    if concurrency is not None:
        args["concurrency"] = int(concurrency)
    if retries is not None:
        args["retries"] = int(retries)
    if min_ok is not None:
        args["minOk"] = int(min_ok)
    if intent is not None:
        args["intent"] = intent
    return _judge_batch_from(_bridge_call("__judge_batch__", args))


def _attach_judge_batch(id):
    """Re-create a ``JudgmentBatch`` ref by id (after a kernel reset)."""
    return _judge_batch_from(_bridge_call("__judge_batch__", {"op": "attach", "id": str(id)}))


judge_batch.attach = _attach_judge_batch


class _Budget:
    """Live view of the turn (+Nk) or Goal Mode token budget."""

    def _snap(self):
        return _bridge_call("__budget__", {}) or {}

    @property
    def total(self):
        return self._snap().get("total")

    @property
    def hard(self):
        return bool(self._snap().get("hard"))

    def spent(self):
        return int(self._snap().get("spent") or 0)

    def remaining(self):
        s = self._snap()
        total = s.get("total")
        if total is None:
            return math.inf
        return max(0, total - int(s.get("spent") or 0))

    def __repr__(self):
        try:
            s = self._snap()
            return f"<budget total={s.get('total')} spent={s.get('spent')}>"
        except Exception:
            return "<budget unavailable>"


budget = _Budget()

_ns.update({
    "tool": tool, "read": read, "write": write, "env": env, "log": log, "phase": phase, "output": output,
    "wait": wait, "completion": completion, "CompletionHandle": CompletionHandle, "judge": judge,
    "judge_batch": judge_batch, "JudgmentBatch": JudgmentBatch, "JudgmentItem": JudgmentItem, "budget": budget,
})

# ---------------------------------------------------------------------------
# magics + execution
# ---------------------------------------------------------------------------

_MAGIC_PIP = re.compile(r"^\s*%pip\s+(.*)$")
_MAGIC_LOAD = re.compile(r"^\s*%load\s+(.+?)\s*$")
_MAGIC_TIME = re.compile(r"^(\s*)%time\s+(.+)$")
_BANG = re.compile(r"^(\s*)!(.+)$")


def _load_path(arg):
    """%load argument: a quoted path ("my setup.py") or a bare one."""
    try:
        parts = shlex.split(arg)
    except ValueError:
        return arg
    return parts[0] if len(parts) == 1 else arg


def __omc_load(p):
    path = _resolve_path(p)
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), _ns)


_ns["__omc_load"] = __omc_load


def _rewrite(code):
    out = []
    for line in code.split("\n"):
        m = _MAGIC_PIP.match(line)
        if m:
            out.append(f"__import__('subprocess').run([__import__('sys').executable, '-m', 'pip'] + {m.group(1).split()!r}, check=False)")
            continue
        m = _MAGIC_LOAD.match(line)
        if m:
            out.append(f"__omc_load({_load_path(m.group(1))!r})")
            continue
        m = _MAGIC_TIME.match(line)
        if m:
            ind, stmt = m.group(1), m.group(2)
            out.append(f"{ind}__omc_t0 = __import__('time').perf_counter()")
            out.append(f"{ind}exec(compile({stmt!r}, '<time>', 'exec'))")
            out.append(f"{ind}print('Wall time: %.2f ms' % ((__import__('time').perf_counter() - __omc_t0) * 1000))")
            continue
        m = _BANG.match(line)
        if m:
            out.append(f"{m.group(1)}print(__import__('subprocess').run({m.group(2)!r}, shell=True, capture_output=True, text=True).stdout, end='')")
            continue
        out.append(line)
    return "\n".join(out)


_loop = asyncio.new_event_loop()
asyncio.set_event_loop(_loop)


def _await(value):
    if asyncio.iscoroutine(value):
        return _loop.run_until_complete(value)
    return value


def _cancel_pending():
    try:
        tasks = [t for t in asyncio.all_tasks(_loop) if not t.done()]
        for t in tasks:
            t.cancel()
        if tasks:
            _loop.run_until_complete(asyncio.gather(*tasks, return_exceptions=True))
    except BaseException:
        pass


def _run(code):
    code = _rewrite(code)
    tree = ast.parse(code, "<cell>", "exec")
    last_expr = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        last_expr = ast.Expression(tree.body.pop().value)
    flags = ast.PyCF_ALLOW_TOP_LEVEL_AWAIT
    _await(eval(compile(tree, "<cell>", "exec", flags=flags), _ns))
    if last_expr is not None:
        val = _await(eval(compile(last_expr, "<cell>", "eval", flags=flags), _ns))
        if val is not None:
            img = _value_image(val)
            if img is not None:
                _emit_image(img)
                print(f"<image {img['mimeType']}>" if isinstance(val, dict) else repr(val))
            else:
                print(repr(val))


def _do_cell(msg):
    buf = io.StringIO()
    old_out, old_err = sys.stdout, sys.stderr
    sys.stdout = sys.stderr = buf
    _cell["images"] = []
    err = None
    try:
        try:
            _run(msg.get("code", ""))
        except SystemExit as e:
            err = f"SystemExit: {e.code}"
        except BaseException:
            et, ev, tb = sys.exc_info()
            frames = [f for f in traceback.extract_tb(tb) if f.filename == "<cell>"]
            err = "".join(traceback.format_list(frames)) + "".join(traceback.format_exception_only(et, ev))
            if et is KeyboardInterrupt:
                _cancel_pending()
        _figures()
    finally:
        sys.stdout, sys.stderr = old_out, old_err
        images = _cell["images"] or []
        _cell["images"] = None
    return {"type": "result", "id": msg.get("id"), "stdout": buf.getvalue(), "error": err, "images": images}


def main():
    threading.Thread(target=_reader, daemon=True).start()
    _send({"type": "ready", "python": sys.version.split()[0]})
    while True:
        try:
            msg = _exec_q.get()
        except KeyboardInterrupt:
            continue
        if msg is None:
            return
        try:
            res = _do_cell(msg)
        except KeyboardInterrupt:
            res = {"type": "result", "id": msg.get("id"), "stdout": "", "error": "KeyboardInterrupt", "images": []}
        while True:
            try:
                _send(res)
                break
            except KeyboardInterrupt:
                continue
            except Exception:
                return


if __name__ == "__main__":
    main()
