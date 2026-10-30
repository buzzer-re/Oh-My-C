#!/usr/bin/env python3
"""Dev-only stdio MCP server fixture: one tool `echo_text` that returns its input."""
import json, sys
for line in sys.stdin:
    msg = json.loads(line)
    mid, method = msg.get("id"), msg.get("method")
    if mid is None:
        continue
    if method == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "mock", "version": "1"},
               "instructions": "Mock MCP server instructions."}
    elif method == "tools/list":
        res = {"tools": [{"name": "echo_text", "description": "Echo text back",
                          "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]}
    elif method == "tools/call":
        res = {"content": [{"type": "text", "text": "mcp echo: " + msg["params"]["arguments"].get("text", "")}]}
    else:
        print(json.dumps({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "nope"}}), flush=True)
        continue
    print(json.dumps({"jsonrpc": "2.0", "id": mid, "result": res}), flush=True)
