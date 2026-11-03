#!/usr/bin/env python3
"""Dev-only e2e helper: summarize the first request of a session from mock_openai.py's request log (MOCK_LOG) and
time log (MOCK_TIMELOG).
  first_request_check.py <req.jsonl> <time.jsonl> <workdir> <what>
what:
  size     "<tools> tools, <bytes> B": the first tool-carrying request as compact JSON, with every occurrence of the
           work dir removed (so the number does not depend on where the test runs)
  params   the first request's parameters other than messages / tools, as sorted JSON
  side     the side requests (no tools): "<kind> think=<bool> content=<str|list>" lines, sorted
  order    the other request kinds in arrival order, then the number of skill compressions (which run in the
           background from session start, so they race the first request): e.g. "main title; 2 compress"
  queue    "main queued <= <S>s": the main request's time in the single slot, rounded up to the next 0.5 s"""
import json, math, sys

reqs = [json.loads(l) for l in open(sys.argv[1])]
times = [json.loads(l) for l in open(sys.argv[2])] if sys.argv[2] != "-" else []
work, what = sys.argv[3], sys.argv[4]

def text(m):
    c = m.get("content")
    return c if isinstance(c, str) else "".join(p.get("text", "") for p in c or [] if isinstance(p, dict))

def kind(r):
    last = text(r["messages"][-1]) if r.get("messages") else ""
    if "<title-request>" in last: return "title"
    if last.startswith("Compress into one routing hint"): return "compress"
    return "main" if r.get("tools") else "side"

def thinks(r):
    kw = r.get("chat_template_kwargs") or {}
    return not (r.get("enable_thinking") is False or kw.get("enable_thinking") is False or r.get("reasoning_effort") == "none")

main = next((r for r in reqs if r.get("tools")), None)
if what == "size":
    body = json.dumps(main, ensure_ascii=False, separators=(",", ":"))
    body = body.replace(json.dumps(work)[1:-1], "")
    print(f"{len(main['tools'])} tools, {len(body.encode())} B")
elif what == "params":
    print(json.dumps({k: v for k, v in main.items() if k not in ("messages", "tools")}, sort_keys=True))
elif what == "side":
    lines = [f"{kind(r)} think={thinks(r)} content={type(r['messages'][-1]['content']).__name__}" for r in reqs if not r.get("tools")]
    print("\n".join(sorted(lines)))
elif what == "order":
    kinds = [e["kind"] for e in times if e["event"] == "arrive"]
    print(" ".join(k for k in kinds if k != "compress") + f"; {kinds.count('compress')} compress")
elif what == "queue":
    q = next(e["queued"] for e in times if e["event"] == "start" and e["kind"] == "main")
    print(f"main queued <= {math.ceil(q * 2 + 1e-9) / 2:.1f}s")
