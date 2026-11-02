#!/usr/bin/env python3
"""Dev-only stdio MCP server fixture: one tool `echo_text` that returns its input, and two prompts
(`greet` with a `name` argument, `empty` that renders no text) that become `/mockmcp:<prompt>` commands."""
import json, sys
for line in sys.stdin:
    msg = json.loads(line)
    mid, method = msg.get("id"), msg.get("method")
    if mid is None:
        continue
    if method == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}, "prompts": {}},
               "serverInfo": {"name": "mock", "version": "1"}, "instructions": "Mock MCP server instructions."}
    elif method == "prompts/list":
        res = {"prompts": [{"name": "greet", "description": "Greet someone politely",
                            "arguments": [{"name": "name", "required": True}]},
                           {"name": "empty"}]}
    elif method == "prompts/get":
        p = msg["params"]
        if p["name"] == "greet":
            who = (p.get("arguments") or {}).get("name", "nobody")
            res = {"messages": [{"role": "user", "content": {"type": "text", "text": "MCP prompt says hello to " + who}},
                                {"role": "user", "content": {"type": "resource",
                                                             "resource": {"uri": "mem://x", "text": "resource part"}}}]}
        else:
            res = {"messages": []}
    elif method == "tools/list":
        res = {"tools": [{"name": "echo_text", "description": "Echo text back",
                          "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]}
    elif method == "tools/call":
        res = {"content": [{"type": "text", "text": "mcp echo: " + msg["params"]["arguments"].get("text", "")}]}
    else:
        print(json.dumps({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "nope"}}), flush=True)
        continue
    print(json.dumps({"jsonrpc": "2.0", "id": mid, "result": res}), flush=True)
