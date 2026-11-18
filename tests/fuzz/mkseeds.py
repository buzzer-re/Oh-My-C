#!/usr/bin/env python3
"""tests/fuzz/mkseeds.py - (re)generate the small seed corpora in tests/fuzz/seeds/<target>/ from fixtures and
hand-written samples. Dev-time only; run from the repo root: python3 -I tests/fuzz/mkseeds.py"""
import glob, io, json, os, struct, zlib, gzip, hashlib, base64, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "tests", "fuzz", "seeds")
SEP = b"\n\x01\n"

def put(target, name, data):
    d = os.path.join(OUT, target)
    os.makedirs(d, exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)

# ---- json
J = [
    '{"a":1,"b":[true,false,null],"c":{"d":"e\\u00e9\\ud83d\\ude00"},"n":-1.5e300,"i":9223372036854775807}',
    '[1,2.5,-0,1e-400,"x\\n\\t\\"",{}]',
    '{"role":"assistant","content":[{"type":"text","text":"hi"},{"type":"toolCall","id":"c1","name":"bash","arguments":{"command":"ls"}}]}',
    '{"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":42}}',
    '{"x":[[[[[[[[[[1]]]]]]]]]],"y":"\\u0000"}',
]
for i, s in enumerate(J):
    put("json", "j%d" % i, s)

# ---- yaml
Y = [
    "providers:\n  mock:\n    baseUrl: http://127.0.0.1:1/v1\n    api: openai-completions\n    models:\n      - id: m1\n        name: \"Mock\"\n        contextWindow: 128000\n",
    "---\nname: skill\ndescription: >\n  folded\n  text\ntags: [a, b, {c: d}]\n---\nbody\n",
    "a: &x {k: v}\nb: *x\nc: |\n  lit\n  block\nd: !!str 12\ne: ~\nf: 0x1F\ng: 1e3\nh: 'q''uote'\n",
    "- a\n- - b\n  - c\n-\n  k: v\n...\n---\nsecond: doc\n",
]
for i, s in enumerate(Y):
    put("yaml", "y%d" % i, s)

# ---- regex: <flags><pattern> SEP <subject>
R = [
    (0, r"(?P<w>\w+)\s+(\d{2,4})", "hello  2024 world 12"),
    (1, r"^foo.*bar$", "FOO x BAR\nfoobar"),
    (2, r"[[:alpha:]&&[^aeiou]]+|\p{Greek}+", "xyzαβγ"),
    (0, r"(a|ab)(c|bcd)(d*)", "abcd"),
    (0, r"(?<=x)y(?!z)\1?", "xyxyz"),
    (0x80, r"foo(bar", "foo(bar"),
    (0, r"a{2,5}?b|\bc\B", "aaab abc"),
    (4, r"(?x) a b # comment\n c", "abc"),
]
for i, (f, p, s) in enumerate(R):
    put("regex", "r%d" % i, bytes([f]) + p.encode() + SEP + s.encode())

# ---- glob: <flags><pattern lines> SEP <path>
G = [(0, "**/*.c\n!src/vendor/**\n# comment\nbuild/", "src/core/json.c"),
     (1, "*.{ts,tsx}\n[!a-c]?.md\n/root.txt\n\\#lit", "a/b/x.tsx"),
     (0, "foo/**/bar/*.js\n**", "foo/a/b/bar/c.js")]
for i, (f, p, s) in enumerate(G):
    put("glob", "g%d" % i, bytes([f]) + p.encode() + SEP + s.encode())

# ---- sse: <selector><chunk> stream ; selector%5: 0 anthropic, 1 chat, 2 responses, 3 google, 4 ollama
def sse(events):
    out = ""
    for ev, data in events:
        if ev:
            out += "event: %s\n" % ev
        out += "data: %s\n\n" % json.dumps(data)
    return out
anth = sse([
    ("message_start", {"type": "message_start", "message": {"id": "m", "usage": {"input_tokens": 10, "cache_read_input_tokens": 2}}}),
    ("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": "hmm"}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "signature_delta", "signature": "c2ln"}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 0}),
    ("content_block_start", {"type": "content_block_start", "index": 1, "content_block": {"type": "text", "text": ""}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 1, "delta": {"type": "text_delta", "text": "Hello"}}),
    ("content_block_start", {"type": "content_block_start", "index": 2, "content_block": {"type": "tool_use", "id": "t1", "name": "bash", "input": {}}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 2, "delta": {"type": "input_json_delta", "partial_json": "{\"command\":\"l"}}),
    ("content_block_delta", {"type": "content_block_delta", "index": 2, "delta": {"type": "input_json_delta", "partial_json": "s\"}"}}),
    ("content_block_stop", {"type": "content_block_stop", "index": 2}),
    ("message_delta", {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 5}}),
    ("message_stop", {"type": "message_stop"}),
])
put("sse", "anthropic", bytes([0x10, 7]) + anth.encode())
put("sse", "anthropic_err", bytes([0x10, 63]) + sse([("error", {"type": "error", "error": {"type": "overloaded_error", "message": "Overloaded"}})]).encode())
chat = sse([
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"role": "assistant", "reasoning_content": "think"}}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"content": "Hi"}}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "id": "x", "type": "function", "function": {"name": "bash", "arguments": "{\"a\":"}}]}}]}),
    (None, {"id": "c1", "choices": [{"index": 0, "delta": {"tool_calls": [{"index": 0, "function": {"arguments": "1}"}}]}, "finish_reason": "tool_calls"}]}),
    (None, {"id": "c1", "choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 3, "prompt_tokens_details": {"cached_tokens": 1}}}),
]) + "data: [DONE]\n\n"
put("sse", "chat", bytes([0x11, 5]) + chat.encode())
resp = sse([
    ("response.created", {"type": "response.created", "response": {"id": "r1"}}),
    ("response.output_item.added", {"type": "response.output_item.added", "output_index": 0, "item": {"type": "reasoning", "id": "rs1", "summary": []}}),
    ("response.reasoning_summary_text.delta", {"type": "response.reasoning_summary_text.delta", "item_id": "rs1", "delta": "plan"}),
    ("response.output_item.done", {"type": "response.output_item.done", "output_index": 0, "item": {"type": "reasoning", "id": "rs1", "summary": [{"type": "summary_text", "text": "plan"}], "encrypted_content": "e"}}),
    ("response.output_item.added", {"type": "response.output_item.added", "output_index": 1, "item": {"type": "message", "id": "m1", "role": "assistant", "content": []}}),
    ("response.output_text.delta", {"type": "response.output_text.delta", "item_id": "m1", "output_index": 1, "delta": "Hey"}),
    ("response.output_item.added", {"type": "response.output_item.added", "output_index": 2, "item": {"type": "function_call", "id": "fc1", "call_id": "call1", "name": "bash", "arguments": ""}}),
    ("response.function_call_arguments.delta", {"type": "response.function_call_arguments.delta", "item_id": "fc1", "delta": "{\"command\":\"ls\"}"}),
    ("response.output_item.done", {"type": "response.output_item.done", "output_index": 2, "item": {"type": "function_call", "id": "fc1", "call_id": "call1", "name": "bash", "arguments": "{\"command\":\"ls\"}"}}),
    ("response.completed", {"type": "response.completed", "response": {"id": "r1", "status": "completed", "usage": {"input_tokens": 5, "output_tokens": 7, "input_tokens_details": {"cached_tokens": 0}}}}),
])
put("sse", "responses", bytes([0x12, 9]) + resp.encode())
goog = sse([
    (None, {"candidates": [{"content": {"parts": [{"text": "think", "thought": True, "thoughtSignature": "c2ln"}], "role": "model"}}]}),
    (None, {"candidates": [{"content": {"parts": [{"text": "Hello"}, {"functionCall": {"name": "bash", "args": {"command": "ls"}}}]}, "finishReason": "STOP"}],
            "usageMetadata": {"promptTokenCount": 3, "candidatesTokenCount": 4, "thoughtsTokenCount": 1}}),
])
put("sse", "google", bytes([0x13, 11]) + goog.encode())
oll = "\n".join(json.dumps(x) for x in [
    {"model": "m", "message": {"role": "assistant", "content": "", "thinking": "hm"}, "done": False},
    {"model": "m", "message": {"role": "assistant", "content": "Hi", "tool_calls": [{"function": {"name": "bash", "arguments": {"command": "ls"}}}]}, "done": False},
    {"model": "m", "message": {"role": "assistant", "content": ""}, "done": True, "done_reason": "stop", "prompt_eval_count": 3, "eval_count": 2},
]) + "\n"
put("sse", "ollama", bytes([0x14, 3]) + oll.encode())

# ---- bedrock event stream
def es_frame(headers, payload):
    hb = b""
    for k, v in headers:
        kb, vb = k.encode(), v.encode()
        hb += bytes([len(kb)]) + kb + bytes([7]) + struct.pack(">H", len(vb)) + vb
    total = 12 + len(hb) + len(payload) + 4
    prelude = struct.pack(">II", total, len(hb))
    pc = struct.pack(">I", zlib.crc32(prelude) & 0xffffffff)
    body = prelude + pc + hb + payload
    return body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)
def ev(t, p):
    return es_frame([(":event-type", t), (":content-type", "application/json"), (":message-type", "event")], json.dumps(p).encode())
bed = (ev("messageStart", {"role": "assistant"}) +
       ev("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"reasoningContent": {"text": "hm"}}}) +
       ev("contentBlockDelta", {"contentBlockIndex": 0, "delta": {"reasoningContent": {"signature": "s"}}}) +
       ev("contentBlockStop", {"contentBlockIndex": 0}) +
       ev("contentBlockDelta", {"contentBlockIndex": 1, "delta": {"text": "Hi"}}) +
       ev("contentBlockStart", {"contentBlockIndex": 2, "start": {"toolUse": {"toolUseId": "t", "name": "bash"}}}) +
       ev("contentBlockDelta", {"contentBlockIndex": 2, "delta": {"toolUse": {"input": "{\"command\":\"ls\"}"}}}) +
       ev("contentBlockStop", {"contentBlockIndex": 2}) +
       ev("messageStop", {"stopReason": "tool_use"}) +
       ev("metadata", {"usage": {"inputTokens": 3, "outputTokens": 4}, "metrics": {"latencyMs": 1}}))
put("bedrock", "stream", bytes([5]) + bed)
put("bedrock", "exception", bytes([33]) + es_frame([(":exception-type", "throttlingException"), (":content-type", "application/json"), (":message-type", "exception")], b'{"message":"slow down"}'))
put("bedrock", "one", bytes([0x80]) + ev("messageStart", {"role": "assistant"}))

# ---- http: <selector> raw response; selector & 0x80 => websocket frames
def chunked(b, n=5):
    out = b""
    for i in range(0, len(b), n):
        c = b[i:i + n]
        out += b"%x\r\n" % len(c) + c + b"\r\n"
    return out + b"0\r\nX-Trailer: 1\r\n\r\n"
body = anth.encode()
put("http", "cl", b"\x00HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n{\"ok\": true}\n")
put("http", "chunked_sse", b"\x10HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked(body, 37))
gz = gzip.compress(b"hello gzip world " * 20)
put("http", "gzip", b"\x00HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: %d\r\n\r\n" % len(gz) + gz)
put("http", "continue", b"\x00HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 429 Too Many Requests\r\nRetry-After: 2\r\nConnection: close\r\n\r\n{\"error\":{\"message\":\"rate\"}}")
put("http", "close", b"\x00HTTP/1.0 500 Internal\r\n\r\nboom")
def wsf(op, data, fin=True):
    b0 = (0x80 if fin else 0) | op
    n = len(data)
    if n < 126:
        h = bytes([b0, n])
    elif n < 65536:
        h = bytes([b0, 126]) + struct.pack(">H", n)
    else:
        h = bytes([b0, 127]) + struct.pack(">Q", n)
    return h + data
put("http", "ws", b"\x80" + wsf(1, b'{"type":"response.created"}') + wsf(9, b"p") + wsf(1, b"frag", False) + wsf(0, b"ment") + wsf(2, b"\x00\x01" * 100) + wsf(8, struct.pack(">H", 1000) + b"bye"))

# ---- codec: <selector> a SEP b
C = [(0, b"aGVsbG8gd29ybGQ=", b""), (0, b"aGVs\nbG8-_w", b""), (1, b"https://user:p%40ss@[::1]:8080/a/b?c=d#frag", b"../x?y"),
     (1, b"http://example.com/%E2%82%AC%zz", b"//other/path"), (2, b"/a/./b/../../c//d/", b"../e"), (3, b"\xe2\x82\xac\xff\xc0\xaf\xf0\x9f\x98\x80", b""),
     (4, b"a,b,,c\r\nd\n", b","), (5, b"Content-Type", b"text/plain")]
for i, (s, a, b) in enumerate(C):
    put("codec", "c%d" % i, bytes([s]) + a + SEP + b)

# ---- markdown: <width><flags> md
for p in sorted(glob.glob(os.path.join(ROOT, "tests", "tui", "fixtures", "*.md"))):
    data = open(p, "rb").read()[:6000]
    put("markdown", os.path.basename(p), bytes([80, 0x01]) + data)
put("markdown", "html", bytes([40, 0x81]) + b"<p>para &amp; <b>bold</b><br>x</p>\n<ol><li>one<li>two</ol>\n<!-- c -->\n<blockquote>q</blockquote>\n| a | b |\n|:-|-:|\n| `x` | [l](http://u) |\n")

# ---- ansi: <width><start> text
put("ansi", "a0", bytes([20, 3]) + b"\x1b[1;31mred\x1b[0m plain \x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\ wide:\xe4\xb8\xad\xe6\x96\x87 emoji:\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd tab\there")
put("ansi", "a1", bytes([5, 0]) + b"\x1b[38;2;1;2;3mabc\x1b[48;5;200mdef\x1b[4:3mghi\x1b[58:2::1:2:3m\x1b[?25l\x1bPdcs\x1b\\")

# ---- htmlform: html SEP answer
put("htmlform", "f0", b'<form><h2>Pick</h2><fieldset><legend>Color</legend><label><input type="radio" name="c" value="r" checked> Red</label>'
    b'<label><input type="radio" name="c" value="g"> Green</label></fieldset><label>Age <input type="number" name="age" min="1" max="120" required></label>'
    b'<select name="s" multiple><option value="a">A<option value="b" selected>B</select><textarea name="t" rows=3 minlength=2 pattern="[a-z]+"></textarea>'
    b'<input type="hidden" name="h" value="1"><table><tr><th>x<td>y</table><pre>code</pre><img src="data:image/png;base64,AA" alt="pic"></form>' + SEP + b"42")

# ---- img: real images via Pillow when available
try:
    from PIL import Image
    im = Image.new("RGBA", (7, 5), (10, 20, 30, 128))
    for x in range(7):
        im.putpixel((x, x % 5), (255, x * 30, 0, 255))
    for fmt, kw, name in [("PNG", {}, "png"), ("GIF", {}, "gif"), ("WEBP", {"lossless": True}, "webp_ll"), ("WEBP", {"quality": 50}, "webp")]:
        b = io.BytesIO()
        im.save(b, fmt, **kw)
        put("img", name, b.getvalue())
    b = io.BytesIO()
    im.convert("RGB").save(b, "JPEG", quality=60)
    put("img", "jpeg", b.getvalue())
    b = io.BytesIO()
    im.convert("RGB").save(b, "JPEG", quality=60, progressive=True, subsampling=2)
    put("img", "jpeg_prog", b.getvalue())
    b = io.BytesIO()
    im.convert("P").save(b, "PNG", optimize=True)
    put("img", "png_pal", b.getvalue())
    b = io.BytesIO()
    im.convert("L").save(b, "PNG")
    put("img", "png_gray", b.getvalue())
except ImportError:
    print("note: no Pillow, img seeds skipped", file=sys.stderr)

# ---- edit: <mode> file SEP args ; mode%5: 0 replace, 1 patch, 2 apply_patch, 3 sloppy, 4 hashline
modes = {"replace": 0, "patch": 1, "apply_patch": 2, "sloppy": 3}
n = 0
for mode, m in modes.items():
    for p in sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "pi-edit", mode, "*.json"))):
        try:
            cases = json.load(open(p))["cases"]
        except Exception:
            continue
        for c in cases[:6]:
            files = c.get("files") or {}
            name, content = (next(iter(files.items())) if files else ("a.txt", ""))
            args = json.dumps(c.get("args", {})).replace(json.dumps(name)[1:-1], "a.txt")
            put("edit", "%s_%d" % (mode, n), bytes([m]) + content.encode() + SEP + args.encode())
            n += 1
put("edit", "hashline_0", bytes([4]) + b"alpha\nbeta\ngamma\n" + SEP + b"[a.txt#@TAG@]\nPUT 2.=2:\n+BETA\nPUT >3:\n+delta")
put("edit", "hashline_1", bytes([4]) + b"int f(void) {\n    return 1;\n}\n" + SEP + b"[a.txt#@TAG@]\nPUT 1*:\n+int f(void) { return 2; }\nCUT 2.=2 @r1\nPUT >1 @r1\nREM 3")

# ---- session: jsonl files (capped)
for p in [os.path.join(ROOT, "tests", "fixtures", "dump", "session.jsonl"), os.path.join(ROOT, "tests", "fixtures", "export", "rich.jsonl")]:
    data = open(p, "rb").read()
    put("session", os.path.basename(os.path.dirname(p)) + "_" + os.path.basename(p), data[:60000])

# ---- rpcframe: <chunk> frames
def lspf(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b
put("rpcframe", "lsp", bytes([7]) + lspf({"jsonrpc": "2.0", "id": 1, "result": {"capabilities": {}}}) + b"noise\r\n\r\n" +
    b"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n" + lspf({"jsonrpc": "2.0", "method": "window/logMessage", "params": {"message": "x"}}))
put("rpcframe", "bad", bytes([3]) + b"Content-Length: 12abc\r\n\r\n{}")

# ---- approval: <tool selector> args JSON or command
A = [(0, b'{"command":"cd /tmp && rm -rf ./x; echo $(whoami) | tee >(cat) `id`"}'), (0, b"git push --force origin main && sudo dd if=/dev/zero of=/dev/sda"),
     (1, b'{"input":"[a.txt#ABCD]\\nPUT 1.=1:\\n+x"}'), (0x21, b'{"input":"*** Begin Patch\\n*** Update File: ../x\\n@@\\n-a\\n+b\\n*** End Patch"}'),
     (2, b'{"path":"xd://dev/file","content":"x"}'), (4, b'{"pattern":"foo","path":"/etc/**/*.conf"}'), (8, b'{"url":"https://example.com/a"}')]
for i, (s, a) in enumerate(A):
    put("approval", "a%d" % i, bytes([s]) + a)

# ---- pb: <selector> bytes
def varint(v):
    out = b""
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])
def ld(no, b):
    return varint(no << 3 | 2) + varint(len(b)) + b
msg = ld(1, ld(1, ld(1, b"hello")))
put("pb", "msg", bytes([0]) + msg)
put("pb", "connect", bytes([0x85]) + bytes([0]) + struct.pack(">I", len(msg)) + msg + bytes([2]) + struct.pack(">I", 37) + b'{"error":{"code":"internal","message":"x"}}'[:37])
put("pb", "value", bytes([0x40]) + ld(5, ld(1, ld(1, b"k") + ld(2, varint(3 << 3 | 2)[:0] + b"\x1a\x01v"))))
# ---- tarx: <selector> tar.gz (even) or tar (odd): `omc update` release archives in each tar dialect
import tarfile
def tarball(fmt, names, gz):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as t:
        for name, body in names:
            info = tarfile.TarInfo(name)
            info.mtime = 1700000000
            if body is None:
                info.type = tarfile.DIRTYPE
                t.addfile(info)
            else:
                info.size = len(body)
                info.mode = 0o755
                t.addfile(info, io.BytesIO(body))
    raw = buf.getvalue()
    return gzip.compress(raw, mtime=0) if gz else raw
REL = [("omc-9.9.9-linux-arm64", None), ("omc-9.9.9-linux-arm64/omc", b"#!/bin/sh\necho omc/9.9.9\n"),
       ("omc-9.9.9-linux-arm64/README.md", b"readme\n")]
put("tarx", "release-gz", bytes([0]) + tarball(tarfile.USTAR_FORMAT, REL, True))
put("tarx", "release-tar", bytes([1]) + tarball(tarfile.USTAR_FORMAT, REL, False))
put("tarx", "gnu-longname", bytes([1]) + tarball(tarfile.GNU_FORMAT, [("d/" * 60 + "omc", b"x" * 700)], False))
put("tarx", "pax", bytes([1 | 4]) + tarball(tarfile.PAX_FORMAT, [("omc", b"bin"), ("\u00e9" * 120, b"y")], False))
print("seeds written to", OUT)
