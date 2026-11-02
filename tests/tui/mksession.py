#!/usr/bin/env python3
"""Dev-only: write an omp v3 session JSONL from a compact spec, so tests/tui/compare.sh `replay` can
resume the same scripted transcript in omp and omc (tool cards rendered from arbitrary args/details
without running the tools or a model).

  mksession.py <spec.json> <cwd> <out.jsonl>

spec: a JSON list of entries, in order:
  {"user": "text"}                                   user message
  {"say": "text"}                                    assistant text
  {"tool": "name", "args": {...}, "text": "result text", "details": {...}, "isError": false,
   "images": [{"data": b64, "mimeType": "image/png"}]}  one tool call + its result
  {"tools": [ {tool entry}, ... ]}                   several calls in one assistant message
  {"parent": "00001003"}                             continue from that entry (a new branch); entry ids
                                                     are "%08x" % (0x1000 + n) for the n-th entry (the
                                                     model_change root is 00001001)
  {"label": "name", "target": "00001003"}            a label entry (target defaults to the last entry)
  {"entry": {...}}                                   any raw entry (type and fields as written)
"""
import json, sys, uuid

def main():
    spec = json.load(open(sys.argv[1]))
    cwd, out = sys.argv[2], sys.argv[3]
    ts = "2026-11-01T12:00:00.000Z"
    ms = 1793534400000
    lines = []
    sid = str(uuid.uuid4())
    lines.append({"type": "session", "version": 3, "id": sid, "timestamp": ts, "cwd": cwd})
    parent = None
    n = 0
    def add(entry):
        nonlocal parent, n
        n += 1
        eid = "%08x" % (0x1000 + n)
        entry.update({"id": eid, "parentId": parent, "timestamp": ts})
        parent = eid
        lines.append(entry)
    add({"type": "model_change", "model": "mock/mock-1"})
    usage = {"input": 100, "output": 20, "cacheRead": 0, "cacheWrite": 0, "totalTokens": 120,
             "cost": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0, "total": 0}}
    def assistant(content, stop):
        return {"role": "assistant", "content": content, "api": "openai-completions", "provider": "mock",
                "model": "mock-1", "usage": usage, "stopReason": stop, "timestamp": ms}
    k = 0
    for e in spec:
        if "parent" in e:
            parent = e["parent"]
        elif "label" in e:
            add({"type": "label", "targetId": e.get("target", parent), "label": e["label"]})
        elif "entry" in e:
            add(dict(e["entry"]))
        elif "user" in e:
            add({"type": "message", "message": {"role": "user", "content": [{"type": "text", "text": e["user"]}], "timestamp": ms}})
        elif "say" in e:
            add({"type": "message", "message": assistant([{"type": "text", "text": e["say"]}], "stop")})
        else:
            calls = e["tools"] if "tools" in e else [e]
            blocks = []
            for c in calls:
                k += 1
                c["_id"] = "call_%d" % k
                blocks.append({"type": "toolCall", "id": c["_id"], "name": c["tool"], "arguments": c.get("args", {})})
            add({"type": "message", "message": assistant(blocks, "toolUse")})
            for c in calls:
                content = [{"type": "text", "text": c.get("text", "")}]
                for im in c.get("images", []):
                    content.append({"type": "image", "data": im["data"], "mimeType": im["mimeType"]})
                msg = {"role": "toolResult", "toolCallId": c["_id"], "toolName": c["tool"], "content": content,
                       "isError": bool(c.get("isError")), "timestamp": ms}
                if "details" in c:
                    msg["details"] = c["details"]
                add({"type": "message", "message": msg})
    with open(out, "w") as f:
        for l in lines:
            f.write(json.dumps(l) + "\n")

main()
