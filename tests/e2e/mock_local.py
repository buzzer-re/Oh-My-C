#!/usr/bin/env python3
"""Mock local model servers for implicit discovery tests (src/ai/localdisc.c), one port, three personalities:
  /ollama/api/tags, POST /ollama/api/show        Ollama
  /llama/models, /llama/props                     llama.cpp (native root)
  /lms/v1/models, /lms/api/v0/models              LM Studio
Usage: mock_local.py <port>
MOCK_LOCAL_REQUIRE_KEY=<key>: the LM Studio routes answer 401 unless sent `Authorization: Bearer <key>`."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OLLAMA_TAGS = {"models": [{"name": "qwen3:8b", "model": "qwen3:8b"}, {"name": "llava:7b", "model": "llava:7b"}]}
OLLAMA_SHOW = {
    "qwen3:8b": {"capabilities": ["completion", "tools", "thinking"], "model_info": {"qwen3.context_length": 40960}},
    "llava:7b": {"capabilities": ["completion", "vision"], "parameters": "num_ctx 8192\nstop x"},
}
LLAMA_MODELS = {"data": [{"id": "gemma-local.gguf", "meta": {"n_ctx_train": 131072}, "status": {"args": ["--ctx-size", "16384"]}}]}
LLAMA_PROPS = {"default_generation_settings": {"n_ctx": 32768, "params": {"n_predict": 2048}}, "modalities": {"vision": False}}
LMS_MODELS = {"data": [{"id": "qwen2.5-vl-7b"}, {"id": "text-embedding-nomic"}]}
LMS_V0 = {"data": [{"id": "qwen2.5-vl-7b", "type": "vlm", "state": "loaded", "loaded_context_length": 12000, "max_context_length": 32768},
                   {"id": "text-embedding-nomic", "type": "embeddings", "max_context_length": 2048}]}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        routes = {"/ollama/api/tags": OLLAMA_TAGS, "/llama/models": LLAMA_MODELS, "/llama/props": LLAMA_PROPS,
                  "/lms/v1/models": LMS_MODELS, "/lms/api/v0/models": LMS_V0}
        need = os.environ.get("MOCK_LOCAL_REQUIRE_KEY")
        if need and self.path.startswith("/lms/") and self.headers.get("Authorization") != "Bearer " + need:
            self.send({"error": "unauthorized"}, 401)
        elif self.path in routes:
            self.send(routes[self.path])
        else:
            self.send({"error": "not found"}, 404)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if self.path == "/ollama/api/show" and body.get("model") in OLLAMA_SHOW:
            self.send(OLLAMA_SHOW[body["model"]])
        else:
            self.send({"error": "not found"}, 404)


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
