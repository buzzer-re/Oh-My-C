#!/usr/bin/env python3
"""Dev-only mock of the provider usage / quota endpoints (tests/e2e/usage.sh, tests/tui/compare.sh).
Routes by path suffix, so any prefix works: omc reaches it through OMC_AUTH_URL_REWRITE (real origin -> a prefix
here), omp through models.yml provider baseUrl overrides. Every request is appended to $MOCK_LOG as
{method, path, headers (lower-cased), body}. Reset times are relative to the request time (+N hours and 30 s), so
countdowns render stable. MOCK_USAGE_EMPTY=1 answers every quota endpoint with 500."""
import datetime, json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = os.environ.get("MOCK_LOG")
CONSUMED = []


def iso(hours):
    t = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=hours, seconds=30)
    return t.strftime("%Y-%m-%dT%H:%M:%S.000Z")


def epoch(hours):
    return int(time.time() + hours * 3600 + 30)


def claude_usage():
    return {
        "five_hour": {"utilization": 42.0, "resets_at": iso(2)},
        "seven_day": {"utilization": 77.0, "resets_at": iso(72)},
        "seven_day_opus": None,
        "seven_day_sonnet": None,
        "limits": [{"kind": "weekly_scoped", "percent": 15, "resets_at": iso(72), "scope": {"model": {"display_name": "Fable"}},
                    "is_active": False}],
        "extra_usage": {"is_enabled": True, "monthly_limit": 5000, "used_credits": 1250, "currency": "USD"},
    }


def codex_usage():
    return {
        "plan_type": "plus",
        "rate_limit": {"allowed": True, "limit_reached": False,
                       "primary_window": {"used_percent": 35, "limit_window_seconds": 18000, "reset_after_seconds": 7230},
                       "secondary_window": {"used_percent": 60, "limit_window_seconds": 604800, "reset_at": epoch(96)}},
        "additional_rate_limits": [{"limit_name": "GPT-5.3-Codex-Spark", "metered_feature": "codex_bengalfox",
                                    "rate_limit": {"allowed": True, "limit_reached": False,
                                                   "primary_window": {"used_percent": 5, "limit_window_seconds": 18000,
                                                                      "reset_after_seconds": 18030}}}],
        "credits": {"has_credits": False, "unlimited": False, "balance": "0"},
        "rate_limit_reset_credits": {"available_count": 0 if CONSUMED else 2},
    }


def codex_credits():
    credits = [{"id": "rc_1", "reset_type": "rate_limit", "status": "redeemed" if CONSUMED else "available",
                "granted_at": iso(-240), "expires_at": iso(120), "title": "Saved reset"},
               {"id": "rc_2", "reset_type": "rate_limit", "status": "available", "granted_at": iso(-100), "expires_at": iso(480)}]
    return {"credits": credits, "available_count": 1 if CONSUMED else 2}


ROUTES_GET = {
    "/api/oauth/usage": claude_usage,
    "/api/oauth/profile": lambda: {"account": {"uuid": "acct-claude-1", "email": "claude.user@example.com"}},
    "/backend-api/wham/usage": codex_usage,
    "/backend-api/wham/rate-limit-reset-credits": codex_credits,
    "/backend-api/accounts/verified_access": lambda: {"programs": [{"program": "cyber", "state": "active", "grants": []}]},
    "/copilot_internal/user": lambda: {
        "copilot_plan": "individual", "quota_reset_date": iso(240),
        "quota_snapshots": {
            "premium_interactions": {"entitlement": 300, "remaining": 120, "percent_remaining": 40, "unlimited": False,
                                     "overage_count": 0, "overage_permitted": False, "quota_id": "premium_interactions",
                                     "quota_remaining": 120},
            "chat": {"entitlement": 0, "remaining": 0, "percent_remaining": 100, "unlimited": True, "quota_id": "chat"},
            "completions": {"entitlement": 0, "remaining": 0, "percent_remaining": 100, "unlimited": True, "quota_id": "completions"}}},
    "/user": lambda: {"login": "octocat"},
    "/api/monitor/usage/quota/limit": lambda: {
        "success": True, "code": 200, "msg": "ok",
        "data": {"level": "pro", "limits": [
            {"type": "TOKENS_LIMIT", "unit": 3, "number": 5, "percentage": 12, "nextResetTime": epoch(3) * 1000},
            {"type": "TIME_LIMIT", "unit": 5, "number": 1, "usage": 1000, "currentValue": 250, "remaining": 750, "percentage": 25,
             "nextResetTime": epoch(400) * 1000}]}},
    "/api/monitor/usage/model-usage": lambda: {"success": True, "data": {"totalUsage": 12345}},
    "/v2/quotas": lambda: {
        "rollingFiveHourLimit": {"remaining": 80, "max": 100, "limited": False, "nextTickAt": iso(1), "tickPercent": 0.05},
        "weeklyTokenLimit": {"remainingCredits": "$12.00", "maxCredits": "$20.00", "percentRemaining": 60, "nextRegenAt": iso(30),
                             "nextRegenCredits": "$1.00"}},
}

ROUTES_POST = {
    "/v1internal:loadCodeAssist": lambda b: {"cloudaicompanionProject": "proj-gemini-1",
                                             "currentTier": {"id": "free-tier", "name": "Gemini Code Assist"}},
    "/v1internal:retrieveUserQuota": lambda b: {"buckets": [
        {"modelId": "gemini-2.5-pro", "remainingFraction": 0.8, "resetTime": iso(20)},
        {"modelId": "gemini-2.5-flash", "remainingFraction": 1, "resetTime": iso(20)}]},
}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def log(self, method, body):
        if LOG:
            with open(LOG, "a") as f:
                f.write(json.dumps({"method": method, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()},
                                    "body": body}) + "\n")

    def route(self, table):
        path = self.path.split("?")[0]
        for suffix in sorted(table, key=len, reverse=True):
            if path.endswith(suffix):
                return table[suffix]
        return None

    def do_GET(self):
        self.log("GET", None)
        fn = self.route(ROUTES_GET)
        if not fn:
            return self.reply(404, {"error": "not found: " + self.path})
        if os.environ.get("MOCK_USAGE_EMPTY") == "1":
            return self.reply(500, {"error": "unavailable"})
        return self.reply(200, fn())

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            body = json.loads(raw or b"{}")
        except ValueError:
            body = {}
        self.log("POST", body)
        path = self.path.split("?")[0]
        if path.endswith("/wham/rate-limit-reset-credits/consume"):
            CONSUMED.append(body.get("credit_id"))
            return self.reply(200, {"code": "reset"})
        fn = self.route(ROUTES_POST)
        if not fn:
            return self.reply(404, {"error": "not found: " + self.path})
        if os.environ.get("MOCK_USAGE_EMPTY") == "1":
            return self.reply(500, {"error": "unavailable"})
        return self.reply(200, fn(body))


class S(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass


S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
