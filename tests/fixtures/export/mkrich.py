#!/usr/bin/env python3
"""rich export fixture: <out dir> -> rich.jsonl + rich/ sub-sessions"""
import json, os, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
T0 = 1793534400000

def iso(ms):
    import datetime
    return datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (ms % 1000)

def write(path, header, entries, title=None):
    with open(path, "w") as f:
        if title is not None:
            slot = {"type": "title", "v": 1, "title": title, "source": "user", "updatedAt": iso(T0), "pad": ""}
            line = json.dumps(slot, separators=(",", ":"))
            slot["pad"] = " " * (255 - len(line.encode()))
            f.write(json.dumps(slot, separators=(",", ":"), ensure_ascii=False) + "\n")
        f.write(json.dumps(header, ensure_ascii=False) + "\n")
        for e in entries:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")

def chain(specs, prefix):
    entries, parent = [], None
    for i, (ms, body) in enumerate(specs):
        e = {"type": body.pop("type", "message"), "id": "%s%04d" % (prefix, i), "parentId": parent, "timestamp": iso(ms)}
        e.update(body)
        entries.append(e)
        parent = e["id"]
    return entries

long_emoji = "Investigate " + "😀" * 50 + " the   flaky\t\ttests   in park  module " * 6
usage = lambda i, o, c: {"input": i, "output": o, "cacheRead": 5, "cacheWrite": 0, "totalTokens": i + o + 5,
                         "cost": {"input": c / 3, "output": c / 3, "cacheRead": c / 3, "cacheWrite": 0, "total": c}}
main = chain([
    (T0, {"type": "model_change", "model": "mock/mock-1"}),
    (T0 + 10, {"message": {"role": "user", "content": [{"type": "text", "text": long_emoji}], "timestamp": T0 + 10}}),
    (T0 + 900, {"message": {"role": "assistant", "content": [{"type": "text", "text": "  Let me   look\n\nat it  "},
        {"type": "toolCall", "id": "call_1", "name": "bash", "arguments": {"command": "ls -la /tmp", "timeout": 1.5}},
        {"type": "toolCall", "id": "call_2", "name": "task", "arguments": {"tasks": [{"id": "Explore", "assignment": "x" * 300}]}}],
        "provider": "mock", "model": "mock-1", "api": "openai-completions", "usage": usage(1234, 56, 0.000123456789),
        "stopReason": "toolUse", "timestamp": T0 + 100, "duration": 800.5, "ttft": 123.25}}),
    (T0 + 120, {"type": "custom", "customType": "tool_execution_start", "data": {"toolCallId": "call_1", "args": {"command": "ls   -la\n/tmp"}, "startedAt": iso(T0 + 950)}}),
    (T0 + 1500, {"message": {"role": "toolResult", "toolCallId": "call_1", "toolName": "bash", "content": [{"type": "text", "text": "ok"}],
        "isError": False, "timestamp": T0 + 1500, "details": {"async": {"state": "running", "jobId": "job-7"}}}}),
    (T0 + 4000, {"message": {"role": "toolResult", "toolCallId": "call_2", "toolName": "task", "content": [{"type": "text", "text": "done"}],
        "isError": True, "timestamp": T0 + 4000, "details": {"results": [
            {"id": "Explore", "agent": "explore", "task": "Look  around\nthe repo", "durationMs": 2500},
            {"id": "Ghost", "agent": "ghost", "durationMs": 100.5}, {"id": "NoDur"}]}}}),
    (T0 + 5000, {"type": "compaction", "summary": "s", "firstKeptEntryId": "m0003", "tokensBefore": 142500, "tokensAfter": 999}),
    (T0 + 6000, {"message": {"role": "custom", "customType": "async-result", "content": "r", "display": False, "timestamp": T0 + 6000,
        "details": {"jobs": [{"jobId": "job-7"}, {"jobId": "nope"}]}}}),
    (T0 + 6100, {"message": {"role": "user", "content": "second  turn", "timestamp": T0 + 6100, "synthetic": False}}),
    (T0 + 6200, {"message": {"role": "user", "content": "hidden", "timestamp": T0 + 6150, "synthetic": True}}),
    (T0 + 7000, {"message": {"role": "assistant", "content": [{"type": "toolCall", "id": "call_3", "name": "read", "arguments": {"path": "a.txt"}}],
        "provider": "google-antigravity", "model": "gemini-3-pro", "usage": usage(10, 2, 0), "stopReason": "error", "errorMessage": "boom",
        "completedAt": T0 + 7000}}),
    (T0 + 7100, {"type": "mode_change", "mode": "plan"}),
    (T0 + 7200, {"type": "reset_boundary"}),
    (T0 + 8000, {"type": "custom", "customType": "session_exit", "data": {"kind": "sigint", "recordedAt": iso(T0 + 8000),
        "pendingToolCalls": [{"toolCallId": "call_3"}]}}),
], "m")
header = {"type": "session", "version": 3, "id": "0199aaaa-bbbb-7ccc-8ddd-eeeeffff0000", "timestamp": iso(T0), "cwd": "/work",
          "providerPromptCacheKey": "k", "title": "old title"}
write(os.path.join(out, "rich.jsonl"), header, main, title="Rich ✨ export")

sub = os.path.join(out, "rich")
os.makedirs(os.path.join(sub, "Explore"), exist_ok=True)
explore = chain([
    (T0 + 1600, {"message": {"role": "user", "content": "explore", "timestamp": T0 + 1600}}),
    (T0 + 2500, {"message": {"role": "assistant", "content": [{"type": "text", "text": "found it"}], "provider": "mock", "model": "mock-1",
        "usage": usage(100, 20, 0.5), "stopReason": "stop", "timestamp": T0 + 1700, "duration": 800}}),
], "e")
write(os.path.join(sub, "Explore.jsonl"), {"type": "session", "version": 3, "id": "sub1", "timestamp": iso(T0 + 1600), "cwd": "/work"},
      [{"type": "session_init", "id": "i0", "parentId": None, "timestamp": iso(T0 + 1600), "agent": "explore", "resolvedModel": "mock/mock-1"}] + explore)
helper = chain([(T0 + 2000, {"message": {"role": "assistant", "content": [], "model": "mock-1", "timestamp": T0 + 2000, "duration": 100}})], "h")
write(os.path.join(sub, "Explore", "Helper.jsonl"), {"type": "session", "version": 3, "id": "sub2", "timestamp": iso(T0 + 2000), "cwd": "/work"}, helper)
adv = chain([(T0 + 3000, {"message": {"role": "assistant", "content": [{"type": "text", "text": "note"}], "model": "adv-1", "timestamp": T0 + 3000}})], "a")
write(os.path.join(sub, "__advisor.jsonl"), {"type": "session", "version": 3, "id": "adv", "timestamp": iso(T0 + 3000), "cwd": "/work"}, adv)
open(os.path.join(sub, "Broken.jsonl"), "w").write('{"type":"message"}\n')
open(os.path.join(sub, "Explore.jsonl.tombstone"), "w").write("")
open(os.path.join(sub, "Explore.jsonl.1.bak"), "w").write("x")
