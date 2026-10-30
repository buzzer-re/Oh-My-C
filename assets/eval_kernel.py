# omc eval kernel: persistent Python namespace driven over JSON lines on stdin/stdout.
# Protocol (one JSON object per line):
#   host -> kernel: {"type":"exec","id":N,"code":"..."} | {"type":"tool_result","id":M,"result":{...}}
#   kernel -> host: {"type":"tool_call","id":M,"name":"read","args":{...}}
#                   {"type":"result","id":N,"stdout":"...","error":null|"...","images":[{"data":b64,"mimeType":...}]}
import ast, asyncio, base64, builtins, io, json, os, re, subprocess, sys, traceback, types

_proto_out = sys.stdout
_proto_in = sys.stdin
_ns = {"__name__": "__main__", "__builtins__": builtins}
_call_id = [0]


def _send(obj):
    _proto_out.write(json.dumps(obj) + "\n")
    _proto_out.flush()


def _tool_call(name, args):
    _call_id[0] += 1
    cid = _call_id[0]
    _send({"type": "tool_call", "id": cid, "name": name, "args": args or {}})
    while True:
        line = _proto_in.readline()
        if not line:
            raise RuntimeError("host closed")
        msg = json.loads(line)
        if msg.get("type") == "tool_result" and msg.get("id") == cid:
            r = msg.get("result") or {}
            if r.get("isError"):
                raise RuntimeError(r.get("text") or "tool error")
            return r.get("text", "")


class _Tools:
    def __getattr__(self, name):
        async def call(args=None, **kw):
            merged = dict(args or {})
            merged.update(kw)
            return _tool_call(name, merged)
        return call


def _read(path, offset=None, limit=None):
    with open(os.path.expanduser(path), encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    start = (offset or 1) - 1
    end = start + limit if limit else None
    return "\n".join(lines[start:end])


def _write(path, content):
    p = os.path.expanduser(path)
    d = os.path.dirname(p)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)
    return p


def _env(key=None, value=None):
    if key is None:
        return dict(os.environ)
    if value is not None:
        os.environ[key] = str(value)
    return os.environ.get(key)


def _log(message):
    print(message, file=sys.stderr)


def _phase(title):
    print(f"## {title}")


_ns.update({"tool": _Tools(), "read": _read, "write": _write, "env": _env, "log": _log, "phase": _phase})

_MAGIC_PIP = re.compile(r"^\s*%pip\s+(.*)$")
_MAGIC_LOAD = re.compile(r"^\s*%load\s+(\S+)\s*$")
_BANG = re.compile(r"^(\s*)!(.+)$")


def _rewrite(code):
    out = []
    for line in code.split("\n"):
        m = _MAGIC_PIP.match(line)
        if m:
            out.append(f"__import__('subprocess').run([__import__('sys').executable, '-m', 'pip'] + {m.group(1).split()!r}, check=False)")
            continue
        m = _MAGIC_LOAD.match(line)
        if m:
            out.append(f"exec(open({m.group(1)!r}).read())")
            continue
        m = _BANG.match(line)
        if m:
            out.append(f"{m.group(1)}print(__import__('subprocess').run({m.group(2)!r}, shell=True, capture_output=True, text=True).stdout, end='')")
            continue
        out.append(line)
    return "\n".join(out)


def _figures():
    imgs = []
    plt = sys.modules.get("matplotlib.pyplot")
    if plt is None:
        return imgs
    try:
        for num in plt.get_fignums():
            fig = plt.figure(num)
            buf = io.BytesIO()
            fig.savefig(buf, format="png", bbox_inches="tight")
            imgs.append({"data": base64.b64encode(buf.getvalue()).decode(), "mimeType": "image/png"})
        plt.close("all")
    except Exception:
        pass
    return imgs


def _run(code):
    code = _rewrite(code)
    tree = ast.parse(code, "<cell>", "exec")
    last_expr = None
    if tree.body and isinstance(tree.body[-1], ast.Expr):
        last_expr = ast.Expression(tree.body.pop().value)
    flags = ast.PyCF_ALLOW_TOP_LEVEL_AWAIT
    body = compile(tree, "<cell>", "exec", flags=flags)
    res = eval(body, _ns)
    if asyncio.iscoroutine(res):
        asyncio.get_event_loop().run_until_complete(res)
    if last_expr is not None:
        val = eval(compile(last_expr, "<cell>", "eval", flags=flags), _ns)
        if asyncio.iscoroutine(val):
            val = asyncio.get_event_loop().run_until_complete(val)
        if val is not None:
            print(repr(val))


def main():
    try:
        asyncio.set_event_loop(asyncio.new_event_loop())
    except Exception:
        pass
    _send({"type": "ready", "python": sys.version.split()[0]})
    while True:
        line = _proto_in.readline()
        if not line:
            return
        try:
            msg = json.loads(line)
        except Exception:
            continue
        if msg.get("type") != "exec":
            continue
        buf = io.StringIO()
        old_out, old_err = sys.stdout, sys.stderr
        sys.stdout = sys.stderr = buf
        err = None
        try:
            _run(msg.get("code", ""))
        except SystemExit as e:
            err = f"SystemExit: {e.code}"
        except BaseException:
            et, ev, tb = sys.exc_info()
            frames = [f for f in traceback.extract_tb(tb) if f.filename == "<cell>"]
            err = "".join(traceback.format_list(frames)) + "".join(traceback.format_exception_only(et, ev))
        finally:
            sys.stdout, sys.stderr = old_out, old_err
        _send({"type": "result", "id": msg.get("id"), "stdout": buf.getvalue(), "error": err, "images": _figures()})


if __name__ == "__main__":
    main()
