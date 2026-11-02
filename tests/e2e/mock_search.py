#!/usr/bin/env python3
"""Dev-only mock of every web_search backend omc (and omp) can reach, for tests/e2e/websearch.sh and
tests/parity/search_diff.sh. Real origins map to path prefixes (omc: OMC_PROVIDER_URL_REWRITE; both: models.yml
baseUrl overrides, searxng.endpoint, MOONSHOT_SEARCH_BASE_URL). Every request is appended to $MOCK_LOG as
{method, path, headers (lower-cased), body}.

Routes (by path):
  .../v1/messages                     Anthropic Messages with web_search_20250305 (JSON)
  .../codex/responses                 Codex Responses SSE with a web_search_call
  .../responses                       OpenAI / xAI Responses (JSON) with a web_search_call + url_citation
  ...:streamGenerateContent           Gemini SSE with groundingMetadata (developer API and Cloud Code Assist)
  .../chat/completions                Perplexity chat completions SSE with citations / search_results
  .../coding/v1/search                Kimi Code search
  .../web_search_prime/mcp            Z.AI MCP (initialize, notifications/initialized, tools/call)
  .../search?q=...                    SearXNG JSON;  .../config  SearXNG engine list
  .../mcp (parallel)                  Parallel MCP tools/call
A query containing FAIL<status> makes the backend answer that HTTP status ("FAIL401", "FAIL500")."""
import json, os, re, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

LOG = os.environ.get("MOCK_LOG")


def query_of(path, body):
    if isinstance(body, dict):
        for k in ("text_query", "query", "input"):
            v = body.get(k)
            if isinstance(v, str):
                return v
        msgs = body.get("messages") or body.get("input") or (body.get("request") or {}).get("contents") or body.get("contents") or []
        if isinstance(msgs, list):
            for m in reversed(msgs):
                if not isinstance(m, dict) or m.get("role") not in ("user", None):
                    continue
                c = m.get("content")
                if isinstance(c, str):
                    return c
                for part in (c or []) + (m.get("parts") or []):
                    if isinstance(part, dict) and isinstance(part.get("text"), str):
                        return part["text"]
        args = (body.get("params") or {}).get("arguments") or {}
        if args:
            return args.get("query") or args.get("search_query") or json.dumps(args.get("search_queries"))
    q = parse_qs(urlparse(path).query).get("q")
    return q[0] if q else ""


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, status, body, ctype="application/json", headers=None):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def sse(self, events):
        self.send(200, "".join("data: " + json.dumps(e) + "\n\n" for e in events), "text/event-stream")

    def handle_any(self, method):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0)) if method == "POST" else b""
        try:
            body = json.loads(raw) if raw else None
        except Exception:
            body = raw.decode(errors="replace")
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"method": method, "path": self.path,
                                    "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body}) + "\n")
        path = urlparse(self.path).path
        q = query_of(self.path, body) or ""
        m = re.search(r"FAIL(\d{3})", q)
        if m and not path.endswith("/config"):
            return self.send(int(m.group(1)), json.dumps({"error": {"message": "mock failure " + m.group(1), "type": "mock_error"}}))
        if method == "HEAD":
            return self.send(302, "", headers={"Location": "https://example.com/resolved"})
        if path.endswith("/v1/messages"):
            return self.send(200, json.dumps({
                "id": "msg_mock", "model": body.get("model"), "type": "message", "role": "assistant",
                "content": [
                    {"type": "server_tool_use", "id": "srv_1", "name": "web_search", "input": {"query": q}},
                    {"type": "web_search_tool_result", "tool_use_id": "srv_1", "content": [
                        {"type": "web_search_result", "title": "Anthropic Result", "url": "https://example.com/anthropic", "page_age": "2 days ago"}]},
                    {"type": "text", "text": "Anthropic answer about " + q,
                     "citations": [{"type": "web_search_result_location", "url": "https://example.com/anthropic",
                                    "title": "Anthropic Result", "cited_text": "cited anthropic text"}]}],
                "usage": {"input_tokens": 11, "output_tokens": 22, "server_tool_use": {"web_search_requests": 1}}}))
        if path.endswith("/codex/responses"):
            return self.sse([
                {"type": "response.created", "response": {"id": "resp_codex", "model": body.get("model")}},
                {"type": "response.web_search_call.completed"},
                {"type": "response.output_item.done", "item": {"type": "web_search_call", "action": {"sources": [
                    {"url": "https://example.com/codex?utm_source=openai", "title": "Codex Source"}]}}},
                {"type": "response.output_text.delta", "delta": "Codex answer about " + q},
                {"type": "response.output_item.done", "item": {"type": "message", "content": [
                    {"type": "output_text", "text": "Codex answer about " + q, "annotations": [
                        {"type": "url_citation", "url": "https://example.com/codex", "title": "Codex Source", "start_index": 0, "end_index": 5}]}]}},
                {"type": "response.completed", "response": {"id": "resp_codex", "model": body.get("model"),
                                                            "usage": {"input_tokens": 10, "output_tokens": 5, "total_tokens": 15}}}])
        if path.endswith("/responses"):
            text = "Responses answer about " + q
            return self.send(200, json.dumps({
                "id": "resp_mock", "model": body.get("model"),
                "output": [
                    {"type": "web_search_call", "status": "completed", "action": {"type": "search", "query": q,
                     "sources": [{"url": "https://example.com/consulted", "title": "Consulted"}]}},
                    {"type": "message", "content": [{"type": "output_text", "text": text, "annotations": [
                        {"type": "url_citation", "url": "https://example.com/responses", "title": "Responses Source",
                         "start_index": 0, "end_index": 9}]}]}],
                "usage": {"input_tokens": 7, "output_tokens": 8, "total_tokens": 15}}))
        if ":streamGenerateContent" in path:
            chunk = {"candidates": [{"content": {"role": "model", "parts": [{"text": "Gemini answer about " + q}]},
                                     "groundingMetadata": {
                                         "groundingChunks": [{"web": {"uri": "https://example.com/gemini", "title": "Gemini Source"}}],
                                         "groundingSupports": [{"segment": {"text": "Gemini answer"}, "groundingChunkIndices": [0]}],
                                         "webSearchQueries": [q]}}],
                     "usageMetadata": {"promptTokenCount": 3, "candidatesTokenCount": 4, "totalTokenCount": 7},
                     "modelVersion": "gemini-mock"}
            return self.sse([{"response": chunk}] if "v1internal" in path else [chunk])
        if path.endswith("/chat/completions"):
            return self.sse([
                {"id": "pplx_1", "model": body.get("model"), "choices": [{"delta": {"content": "Perplexity answer about " + q}}],
                 "citations": ["https://example.com/perplexity"],
                 "search_results": [{"title": "Perplexity Source", "url": "https://example.com/perplexity", "snippet": "pplx snippet", "date": "2026-01-01"}],
                 "related_questions": ["What next?"]},
                {"id": "pplx_1", "choices": [{"delta": {}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 5, "completion_tokens": 6, "total_tokens": 11}}])
        if path.endswith("/coding/v1/search"):
            return self.send(200, json.dumps({"search_results": [
                {"site_name": "Kimi Site", "title": "Kimi Source", "url": "https://example.com/kimi", "snippet": "kimi snippet", "date": "2026-01-02"}]}),
                headers={"x-request-id": "kimi-req"})
        if path.endswith("/web_search_prime/mcp"):
            method_name = body.get("method")
            if method_name == "notifications/initialized":
                return self.send(202, "")
            if method_name == "initialize":
                return self.send(200, "data: " + json.dumps({"jsonrpc": "2.0", "id": body.get("id"), "result": {"protocolVersion": "2025-03-26"}}) + "\n\n",
                                 "text/event-stream", {"Mcp-Session-Id": "zai-session"})
            payload = json.dumps([{"title": "Z.AI Source", "link": "https://example.com/zai", "content": "zai snippet", "media": "ZaiMedia", "publish_date": "2026-01-03"}])
            return self.send(200, "data: " + json.dumps({"jsonrpc": "2.0", "id": body.get("id"), "result": {"content": [{"type": "text", "text": payload}]}}) + "\n\n",
                             "text/event-stream")
        if path.endswith("/config"):
            return self.send(200, json.dumps({"engines": [{"name": "duckduckgo", "shortcut": "ddg"}, {"name": "brave", "shortcut": "br"}]}))
        if path.endswith("/search"):
            return self.send(200, json.dumps({"query": q, "results": [
                {"title": "SearXNG Source", "url": "https://example.com/searxng", "content": "searx snippet", "publishedDate": "2026-01-04T00:00:00"}],
                "answers": ["searx answer"], "suggestions": ["searx suggestion"]}))
        if path.endswith("/mcp"):
            payload = {"results": [{"url": "https://example.com/parallel", "title": "Parallel Source", "excerpts": ["parallel excerpt"]}]}
            return self.send(200, json.dumps({"jsonrpc": "2.0", "id": body.get("id"), "result": {"structuredContent": payload}}))
        self.send(404, json.dumps({"error": {"message": "no route " + path}}))

    def do_POST(self):
        self.handle_any("POST")

    def do_GET(self):
        self.handle_any("GET")

    def do_HEAD(self):
        self.handle_any("HEAD")


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
