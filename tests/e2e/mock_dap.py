#!/usr/bin/env python3
"""Dev-only mock DAP adapter: simulates a program with lines 1..10, `x` grows with each line."""
import json, sys
seq = 0
line = 1
bps = set()
inp = sys.stdin.buffer
out = sys.stdout.buffer

def send(obj):
    global seq
    seq += 1
    obj["seq"] = seq
    b = json.dumps(obj).encode()
    out.write(b"Content-Length: %d\r\n\r\n" % len(b) + b)
    out.flush()

def respond(req, body=None, ok=True, msg=None):
    r = {"type": "response", "request_seq": req["seq"], "command": req["command"], "success": ok}
    if body is not None: r["body"] = body
    if msg: r["message"] = msg
    send(r)

def event(name, body=None):
    send({"type": "event", "event": name, "body": body or {}})

def read():
    hdr = b""
    while not hdr.endswith(b"\r\n\r\n"):
        c = inp.read(1)
        if not c: return None
        hdr += c
    n = int(hdr.split(b":")[1])
    return json.loads(inp.read(n))

prog = "prog.c"
while True:
    req = read()
    if req is None: break
    c, a = req["command"], req.get("arguments", {})
    if c == "initialize":
        respond(req, {"supportsConfigurationDoneRequest": True})
        event("initialized")
    elif c == "launch":
        prog = a.get("program", prog)
        respond(req)
    elif c == "setBreakpoints":
        bps = {b["line"] for b in a.get("breakpoints", [])}
        respond(req, {"breakpoints": [{"id": i + 1, "verified": True, "line": b["line"]} for i, b in enumerate(a.get("breakpoints", []))]})
    elif c == "configurationDone":
        respond(req)
        event("stopped", {"reason": "entry", "threadId": 1})
    elif c in ("continue", "next", "stepIn", "stepOut"):
        respond(req, {"allThreadsContinued": True})
        if c == "continue":
            nxt = [b for b in sorted(bps) if b > line]
            if nxt:
                line = nxt[0]
                event("stopped", {"reason": "breakpoint", "threadId": 1})
            else:
                event("output", {"category": "stdout", "output": "program done\n"})
                event("exited", {"exitCode": 0}); event("terminated")
        else:
            line += 1
            event("stopped", {"reason": "step", "threadId": 1})
    elif c == "stackTrace":
        respond(req, {"stackFrames": [{"id": 1000, "name": "main", "line": line, "column": 1, "source": {"path": prog}}]})
    elif c == "threads":
        respond(req, {"threads": [{"id": 1, "name": "main"}]})
    elif c == "scopes":
        respond(req, {"scopes": [{"name": "Locals", "variablesReference": 7}]})
    elif c == "variables":
        respond(req, {"variables": [{"name": "x", "value": str(line * 10), "type": "int", "variablesReference": 0}]})
    elif c == "evaluate":
        expr = a.get("expression")
        respond(req, {"result": str(line * 10) if expr == "x" else "?", "type": "int", "variablesReference": 0})
    elif c == "disconnect":
        respond(req); break
    else:
        respond(req, ok=False, msg="unsupported")
