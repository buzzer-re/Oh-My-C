#!/usr/bin/env python3
"""Dev-only stdio MCP server fixture: one tool `echo_text` that returns its input, and two prompts
(`greet` with a `name` argument, `empty` that renders no text) that become `/mockmcp:<prompt>` commands.

With `--full` it also advertises list_changed / subscribe capabilities, serves a resource (`mem://notes`)
and a resource template (`mem://item/{id}`), accepts resources/subscribe, and has a `change_prompts` tool
that adds a `fresh` prompt and sends `notifications/prompts/list_changed` plus
`notifications/resources/updated` for mem://notes (MCP notification tests).

`--startup-delay=<secs>` holds the initialize reply back (a slow server: omp's startup window, MCP tool cache and
deferred tools). $MOCK_MCP_EXTRA_TOOL=<name> adds a tool of that name to tools/list without changing the server
config (so a cached catalog can differ from the live one).
`--modern` speaks MCP 2026-07-28: it answers server/discover (initialize is unknown to it), requires the request _meta,
asks for roots through an input_required round trip (`echo_text` with text "roots"), and acknowledges a
subscriptions/listen stream, on which `echo_text` "notify" announces a new tool `late_tool`.
`--lazy` serves lazy loaders shaped like Kata's `load_*`: `load_extra` adds `extra_tool` and announces it before its
reply, `load_extra_late` replies first and announces it afterwards (no call to the server running)."""
import json, os, sys, time

FULL = "--full" in sys.argv[1:]
BLOBS = "--blobs" in sys.argv[1:]
STARTUP_DELAY = next((float(a.split("=", 1)[1]) for a in sys.argv[1:] if a.startswith("--startup-delay=")), 0.0)
EXTRA_TOOL = os.environ.get("MOCK_MCP_EXTRA_TOOL")
LAZY = "--lazy" in sys.argv[1:]
MODERN = "--modern" in sys.argv[1:]
listen_id = [None]
notified = [False]
initialized_seen = [False]
extra_prompt = [False]
loaded = [False]


def send(obj):
    print(json.dumps(obj), flush=True)


def modern(msg):
    """one 2026-07-28 request; True when handled"""
    mid, method, params = msg.get("id"), msg.get("method"), msg.get("params") or {}
    meta = params.get("_meta") or {}
    if method != "notifications/initialized" and mid is not None and meta.get("io.modelcontextprotocol/protocolVersion") != "2026-07-28":
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32022, "message": "Unsupported protocol version"}})
        return True
    if method == "server/discover":
        res = {"resultType": "complete", "supportedVersions": ["2026-07-28"], "capabilities": {"tools": {"listChanged": True}},
               "instructions": "Modern MCP server instructions.", "ttlMs": 60000, "cacheScope": "public",
               "_meta": {"io.modelcontextprotocol/serverInfo": {"name": "modern-mock", "version": "2"}}}
    elif method == "subscriptions/listen":
        listen_id[0] = mid
        send({"jsonrpc": "2.0", "method": "notifications/subscriptions/acknowledged",
              "params": {"notifications": params.get("notifications"), "_meta": {"io.modelcontextprotocol/subscriptionId": mid}}})
        return True
    elif method == "tools/list":
        tools = [{"name": "echo_text", "description": "Echo text back",
                  "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]
        if notified[0]:
            tools.append({"name": "late_tool", "description": "Announced on the listen stream", "inputSchema": {"type": "object", "properties": {}}})
        res = {"resultType": "complete", "tools": tools, "ttlMs": 60000, "cacheScope": "public"}
    elif method == "tools/call":
        text = (params.get("arguments") or {}).get("text", "")
        if text == "roots" and "inputResponses" not in params:
            res = {"resultType": "input_required", "inputRequests": {"r1": {"method": "roots/list"}}, "requestState": "st-1"}
        elif text == "roots":
            roots = params["inputResponses"]["r1"]["roots"]
            res = {"content": [{"type": "text", "text": "roots %s state=%s initialized=%s" % (roots[0]["uri"].startswith("file:///"), params.get("requestState"), initialized_seen[0])}]}
        elif text == "notify":
            notified[0] = True
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed", "params": {"_meta": {"io.modelcontextprotocol/subscriptionId": listen_id[0]}}})
            res = {"content": [{"type": "text", "text": "notified"}]}
        else:
            res = {"content": [{"type": "text", "text": "modern echo: " + text}]}
    elif method == "notifications/initialized":
        initialized_seen[0] = True
        return True
    else:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "Method not found"}})
        return True
    send({"jsonrpc": "2.0", "id": mid, "result": res})
    return True


for line in sys.stdin:
    msg = json.loads(line)
    if MODERN and modern(msg):
        continue
    mid, method = msg.get("id"), msg.get("method")
    if mid is None:
        continue
    if method == "initialize":
        if STARTUP_DELAY:
            time.sleep(STARTUP_DELAY)
        caps = {"tools": {}, "prompts": {}}
        if FULL:
            caps = {"tools": {"listChanged": True}, "prompts": {"listChanged": True},
                    "resources": {"subscribe": True, "listChanged": True}}
        res = {"protocolVersion": "2025-06-18", "capabilities": caps,
               "serverInfo": {"name": "mock", "version": "1"}, "instructions": "Mock MCP server instructions."}
    elif method == "prompts/list":
        prompts = [{"name": "greet", "description": "Greet someone politely",
                    "arguments": [{"name": "name", "required": True, "description": "Who to greet"}]},
                   {"name": "empty"}]
        if extra_prompt[0]:
            prompts.append({"name": "fresh", "description": "Added at runtime"})
        res = {"prompts": prompts}
    elif method == "prompts/get":
        p = msg["params"]
        if p["name"] == "greet":
            who = (p.get("arguments") or {}).get("name", "nobody")
            res = {"messages": [{"role": "user", "content": {"type": "text", "text": "MCP prompt says hello to " + who}},
                                {"role": "user", "content": {"type": "resource",
                                                             "resource": {"uri": "mem://x", "text": "resource part"}}}]}
        elif p["name"] == "fresh":
            res = {"messages": [{"role": "user", "content": {"type": "text", "text": "fresh prompt text"}}]}
        else:
            res = {"messages": []}
    elif method == "tools/list":
        tools = [{"name": "echo_text", "description": "Echo text back",
                  "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}}]
        if FULL:
            tools.append({"name": "change_prompts", "description": "Add a prompt and notify",
                          "inputSchema": {"type": "object", "properties": {}}})
        if EXTRA_TOOL:
            tools.append({"name": EXTRA_TOOL, "description": "Extra tool",
                          "inputSchema": {"type": "object", "properties": {}}})
        if LAZY:
            tools += [{"name": n, "description": "Load extra tools", "inputSchema": {"type": "object", "properties": {}}}
                      for n in ("load_extra", "load_extra_late")]
            if loaded[0]:
                tools.append({"name": "extra_tool", "description": "A lazily loaded tool",
                              "inputSchema": {"type": "object", "properties": {}}})
        res = {"tools": tools}
    elif method == "tools/call":
        name = msg["params"].get("name")
        if LAZY and name in ("load_extra", "load_extra_late"):
            first = not loaded[0]
            loaded[0] = True
            if first and name == "load_extra":
                send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            send({"jsonrpc": "2.0", "id": msg.get("id"), "result": {"content": [{"type": "text", "text": "loaded"}]}})
            if first and name == "load_extra_late":
                time.sleep(0.2)
                send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
            continue
        if name == "change_prompts":
            extra_prompt[0] = True
            send({"jsonrpc": "2.0", "method": "notifications/prompts/list_changed"})
            send({"jsonrpc": "2.0", "method": "notifications/resources/updated", "params": {"uri": "mem://notes"}})
            res = {"content": [{"type": "text", "text": "prompts changed"}]}
        elif msg["params"]["arguments"].get("text") == "__meta__":  # what the call carried in _meta (a progress token)
            res = {"content": [{"type": "text", "text": "meta " + json.dumps(msg["params"].get("_meta"))}]}
        elif msg["params"]["arguments"].get("text") == "__slow__":  # outlasts a 1 s server timeout
            time.sleep(2.5)
            res = {"content": [{"type": "text", "text": "too late"}]}
        elif msg["params"]["arguments"].get("text") == "__expired__":
            # an expired session refuses the first call (marker file in TMPDIR); the re-initialized one answers
            mark = os.path.join(os.environ.get("TMPDIR", "/tmp"), "mock-mcp-expired")
            if not os.path.exists(mark):
                open(mark, "w").close()
                send({"jsonrpc": "2.0", "id": msg.get("id"), "error": {"code": -32001, "message": "Unknown or expired session"}})
                continue
            res = {"content": [{"type": "text", "text": "recovered after re-initialize"}]}
        elif msg["params"]["arguments"].get("text") == "__crash__":
            sys.exit(3)  # the server dies mid-call, without answering
        elif msg["params"]["arguments"].get("text") == "__rpcerror__":  # a JSON-RPC error with secret-bearing data
            send({"jsonrpc": "2.0", "id": msg.get("id"), "error": {"code": -32000, "message": "boom Bearer abc.def",
                  "data": {"requestId": "req-42", "apiKey": "s3cret", "detail": "x?token=zzz&q=1", "n": [1, 2]}}})
            continue
        elif msg["params"]["arguments"].get("text", "").startswith("struct:"):
            kind = msg["params"]["arguments"]["text"][7:]
            if kind == "list":  # the MCP SDK's {result: value} wrapper around a list return, one text block per item
                res = {"content": [{"type": "text", "text": "a"}, {"type": "text", "text": "b"}], "structuredContent": {"result": ["a", "b"]}}
            elif kind == "obj":
                res = {"content": [{"type": "text", "text": "x"}], "structuredContent": {"k": 1, "s": "v"}}
            elif kind == "err":
                res = {"content": [{"type": "text", "text": "bad"}, {"type": "resource", "resource": {"uri": "mem://r", "text": "rt"}}], "isError": True}
            else:
                res = {"content": [{"type": "text", "text": '{"k": 1}'}], "structuredContent": {"k": 1}}
        else:
            res = {"content": [{"type": "text", "text": "mcp echo: " + msg["params"]["arguments"].get("text", "")}]}
    elif FULL and method == "resources/list":
        res = {"resources": [{"uri": "mem://notes", "name": "notes", "description": "Mock notes", "mimeType": "text/plain"}]}
        if BLOBS:  # byte resources (read as a lone blob part)
            res["resources"] += [{"uri": "mem://blob/logo.png", "name": "logo", "mimeType": "image/png"},
                                 {"uri": "mem://blob/data", "name": "data", "mimeType": "application/octet-stream"}]
    elif FULL and method == "resources/templates/list":
        res = {"resourceTemplates": [{"uriTemplate": "mem://item/{id}", "name": "item", "description": "One item"}]}
    elif FULL and method == "resources/read":
        uri = msg["params"]["uri"]
        if uri == "mem://blob/logo.png":  # a lone binary part: the resource's bytes (a 1x1 PNG)
            png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
            res = {"contents": [{"uri": uri, "mimeType": "image/png", "blob": png}]}
        elif uri == "mem://blob/data":
            res = {"contents": [{"uri": uri, "mimeType": "application/octet-stream", "blob": "AAECAwQF"}]}
        else:
            res = {"contents": [{"uri": uri, "mimeType": "text/plain", "text": "contents of " + uri}]}
    elif FULL and method in ("resources/subscribe", "resources/unsubscribe"):
        res = {}
    else:
        send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "nope"}})
        continue
    send({"jsonrpc": "2.0", "id": mid, "result": res})
