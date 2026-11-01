#!/usr/bin/env python3
"""Dev-only stdio MCP server for tests/tui comparisons: one `echo` tool (returns its `text` argument,
or a JSON document when `json` is true)."""
import json, sys

def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()

for line in sys.stdin:
    try:
        msg = json.loads(line)
    except ValueError:
        continue
    mid, method = msg.get("id"), msg.get("method")
    if mid is None:
        continue
    if method == "initialize":
        send({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-03-26"),
              "capabilities": {"tools": {}}, "serverInfo": {"name": "demo", "version": "1.0"}}})
    elif method == "tools/list":
        send({"jsonrpc": "2.0", "id": mid, "result": {"tools": [{"name": "echo", "description": "Echo text back",
              "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "json": {"type": "boolean"}}}}]}})
    elif method == "tools/call":
        args = msg.get("params", {}).get("arguments", {}) or {}
        text = args.get("text", "")
        if args.get("json"):
            text = json.dumps({"echo": text, "items": [1, 2, 3], "nested": {"a": True}})
        send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": text}], "isError": bool(args.get("fail"))}})
    else:
        send({"jsonrpc": "2.0", "id": mid, "result": {}})
