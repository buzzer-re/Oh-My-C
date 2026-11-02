/*
 * retry.c - transport retry helpers shared by the providers:
 *   ai_extract_retry_hint        pi-utils fetch-retry.ts extractRetryHint
 *   ai_retry_after_from_headers  pi-ai utils/retry-after.ts getRetryAfterMsFromHeaders
 *   ai_provider_retryable_error  pi-ai error/retryable.ts isProviderRetryableError (substring port of its patterns)
 *   ai_replay_safe_stream        pi-ai utils/empty-completion-retry.ts withReplaySafeStreamRetry
 * The fetch loop itself (fetchWithRetry / anthropic-client.ts) is stream_post_url in ai_common.c.
 */
#define _GNU_SOURCE
#include "ai.h"

#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <time.h>

/* ---------- retry hints ---------- */

/* JS Number(): the whole (trimmed) string is a finite number ("" reads as 0) */
static bool js_number(const char *v, double *out) {
    if (!v) return false;
    while (*v == ' ' || *v == '\t') v++;
    if (!*v) {
        *out = 0;
        return true;
    }
    char *end;
    double d = strtod(v, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (*end || end == v || !isfinite(d)) return false;
    *out = d;
    return true;
}

/* Date.parse for the HTTP-date form ("Wed, 21 Oct 2015 07:28:00 GMT"): epoch ms or -1 */
static int64_t http_date_ms(const char *v) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (!v || !strptime(v, "%a, %d %b %Y %H:%M:%S", &tm)) return -1;
    return (int64_t)timegm(&tm) * 1000;
}

int64_t ai_retry_after_from_headers(const http_headers_t *h) {
    if (!h) return -1;
    int64_t best = -1, now = now_ms();
    double d;
    const char *v = http_headers_get(h, "retry-after-ms");
    if (v && *v && js_number(v, &d) && d > 0) best = MAX(best, (int64_t)ceil(d));
    if ((v = http_headers_get(h, "retry-after")) && *v) {
        if (js_number(v, &d)) {
            if (d > 0) best = MAX(best, (int64_t)ceil(d * 1000));
        } else {
            int64_t t = http_date_ms(v);
            if (t > now) best = MAX(best, t - now);
        }
    }
    static const char *const resets[] = {"x-ratelimit-reset-ms", "x-ratelimit-reset"};
    for (int i = 0; i < 2; i++) {
        v = http_headers_get(h, resets[i]);
        if (!v || !*v || !js_number(v, &d) || d <= 0) continue;
        double target;
        if (d > 1e12) target = d;
        else if (d > 1e9) target = d * 1000;
        else {
            best = MAX(best, (int64_t)ceil(i == 0 ? d : d * 1000));
            continue;
        }
        if (target > (double)now) best = MAX(best, (int64_t)ceil(target - (double)now));
    }
    return best;
}

static double unit_ms(const char *u, size_t n, bool days) {
    static const struct {
        const char *name;
        double ms;
    } units[] = {{"ms", 1},         {"s", 1000},       {"sec", 1000},     {"m", 60000},      {"min", 60000},
                 {"mins", 60000},   {"minute", 60000}, {"minutes", 60000}, {"h", 3600000},   {"hr", 3600000},
                 {"hrs", 3600000},  {"hour", 3600000}, {"hours", 3600000}, {"d", 86400000},  {"day", 86400000},
                 {"days", 86400000}};
    for (size_t i = 0; i < ARRAY_LEN(units); i++)
        if (strlen(units[i].name) == n && !strncasecmp(units[i].name, u, n))
            return !days && units[i].ms == 86400000 ? -1 : units[i].ms;
    return -1;
}

static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_'; }
static bool ws_ch(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static const char *skip_ws(const char *p) {
    while (ws_ch(*p)) p++;
    return p;
}
/* [0-9.]+ read like parseFloat; NULL when it holds no number */
static const char *read_num(const char *p, double *out) {
    const char *q = p;
    while (isdigit((unsigned char)*q) || *q == '.') q++;
    if (q == p) return NULL;
    char buf[64];
    size_t n = MIN((size_t)(q - p), sizeof buf - 1);
    memcpy(buf, p, n);
    buf[n] = 0;
    char *e;
    *out = strtod(buf, &e);
    return e == buf ? NULL : q;
}
/* "~? <num> <unit>\b" (the TRY_AGAIN / WILL_RESET_IN grammar): ms or -1 */
static double num_unit_after(const char *p, bool days) {
    p = skip_ws(p);
    if (*p == '~') p = skip_ws(p + 1);
    double v;
    const char *q = read_num(p, &v);
    if (!q || !(v > 0)) return -1;
    q = skip_ws(q);
    const char *u = q;
    while (isalpha((unsigned char)*q)) q++;
    if (q == u || word_ch(*q)) return -1;
    double m = unit_ms(u, (size_t)(q - u), days);
    return m < 0 ? -1 : v * m;
}

/* "YYYY-MM-DD[ T]HH:MM:SS(.f)?(Z|±HH:?MM)?" at p: epoch ms (UTC unless an offset is given) and *has_off */
static int64_t parse_stamp(const char *p, bool *has_off, bool space_only) {
    int Y, Mo, D, h, mi, sec, n = 0;
    char sep;
    *has_off = false;
    if (sscanf(p, "%4d-%2d-%2d%c%2d:%2d:%2d%n", &Y, &Mo, &D, &sep, &h, &mi, &sec, &n) != 7 || n != 19) return -1;
    if (sep != ' ' && (space_only || sep != 'T')) return -1;
    const char *q = p + n;
    if (!space_only && *q == '.' && isdigit((unsigned char)q[1]))
        for (q++; isdigit((unsigned char)*q); q++) {}
    struct tm tm = {.tm_year = Y - 1900, .tm_mon = Mo - 1, .tm_mday = D, .tm_hour = h, .tm_min = mi, .tm_sec = sec};
    int64_t t = (int64_t)timegm(&tm) * 1000;
    if (space_only) return t;
    if (*q == 'Z' || *q == 'z') *has_off = true;
    else if ((*q == '+' || *q == '-') && isdigit((unsigned char)q[1]) && isdigit((unsigned char)q[2])) {
        const char *r = q + 3;
        if (*r == ':') r++;
        if (isdigit((unsigned char)r[0]) && isdigit((unsigned char)r[1])) {
            int64_t off = (int64_t)((q[1] - '0') * 10 + (q[2] - '0')) * 3600000 + (int64_t)((r[0] - '0') * 10 + (r[1] - '0')) * 60000;
            *has_off = true;
            t -= *q == '+' ? off : -off;
        }
    }
    return t;
}

typedef struct {
    double longest, naive;
    bool retry_now;
} hint_acc_t;
static void consider(hint_acc_t *a, double ms) {
    if (ms > 0 && ms > a->longest) a->longest = ms;
}
static void consider_clamped(hint_acc_t *a, double ms) {
    if (ms > 0) consider(a, ms);
    else a->retry_now = true;
}

/* the body-text half of extractRetryHint */
static int64_t body_retry_hint(const char *body, int64_t now) {
    hint_acc_t a = {-1, -1, false};
    /* "reset after 1h2m3s" / "10m15s" / "39s" */
    for (const char *p = str_istr(body, "reset after "); p; p = str_istr(p + 1, "reset after ")) {
        const char *q = p + 12;
        double hrs = 0, mins = 0, secs;
        char *e;
        long v = strtol(q, &e, 10);
        if (e != q && isdigit((unsigned char)*q) && (*e == 'h' || *e == 'H')) {
            hrs = (double)v;
            q = e + 1;
            v = strtol(q, &e, 10);
        }
        if (e != q && isdigit((unsigned char)*q) && (*e == 'm' || *e == 'M')) {
            mins = (double)v;
            q = e + 1;
        }
        const char *r = read_num(q, &secs);
        if (r && (*r == 's' || *r == 'S')) {
            consider(&a, ((hrs * 60 + mins) * 60 + secs) * 1000);
            break;
        }
    }
    /* "(will) reset at <stamp>" and "将在 <stamp> 重置": a naive wall clock (UTC) only as a fallback */
    for (const char *p = str_istr(body, "reset at"); p; p = str_istr(p + 1, "reset at")) {
        if (!ws_ch(p[8])) continue;
        bool off;
        int64_t t = parse_stamp(skip_ws(p + 8), &off, false);
        if (t < 0) continue;
        if (t > now) {
            if (off) consider(&a, (double)(t - now));
            else if ((double)(t - now) > a.naive) a.naive = (double)(t - now);
        }
        break;
    }
    const char *cn = strstr(body, "将在");
    if (cn) {
        bool off;
        const char *q = skip_ws(cn + strlen("将在"));
        int64_t t = parse_stamp(q, &off, true);
        if (t > now && strstr(q, "重置") && (double)(t - now) > a.naive) a.naive = (double)(t - now);
    }
    /* "Resets in 2hr 15min" and "(will) reset(s) in <n> <unit>" */
    bool hrmin = false, resetin = false;
    for (const char *p = str_istr(body, "reset"); p && !(hrmin && resetin); p = str_istr(p + 1, "reset")) {
        const char *q = p + 5;
        if (*q == 's' || *q == 'S') q++;
        if (!ws_ch(*q)) continue;
        q = skip_ws(q);
        if (strncasecmp(q, "in", 2) || !ws_ch(q[2])) continue;
        q += 2;
        if (!hrmin) {
            const char *r = skip_ws(q);
            if (*r == '~') r = skip_ws(r + 1);
            double hv, mv;
            const char *x = isdigit((unsigned char)*r) ? read_num(r, &hv) : NULL;
            if (x && !strncasecmp((x = skip_ws(x)), "hr", 2)) {
                x = skip_ws(x + 2);
                const char *y = isdigit((unsigned char)*x) ? read_num(x, &mv) : NULL;
                if (y && !strncasecmp((y = skip_ws(y)), "min", 3) && !word_ch(y[3]) && hv >= 0 && mv > 0) {
                    consider(&a, hv * 3600000 + mv * 60000);
                    hrmin = true;
                }
            }
        }
        if (!resetin) {
            double ms = num_unit_after(q, true);
            if (ms > 0) {
                consider(&a, ms);
                resetin = true;
            }
        }
    }
    /* "retry-after-ms=98497000" */
    for (const char *p = str_istr(body, "retry-after-ms"); p; p = str_istr(p + 1, "retry-after-ms")) {
        if (p > body && word_ch(p[-1])) continue;
        const char *q = skip_ws(p + 14);
        if (*q != ':' && *q != '=') continue;
        q = skip_ws(q + 1);
        const char *e = q;
        while (isdigit((unsigned char)*e)) e++;
        if (e == q || word_ch(*e)) continue;
        consider_clamped(&a, strtod(q, NULL));
        break;
    }
    /* "Please retry in 250ms" / "\"retryDelay\": \"34.07s\"" / "try again in ~5 min" */
    const char *p = str_istr(body, "please retry in ");
    double v;
    const char *q = p ? read_num(p + 16, &v) : NULL;
    if (q && v > 0) {
        if (!strncasecmp(q, "ms", 2)) consider(&a, v);
        else if (*q == 's' || *q == 'S') consider(&a, v * 1000);
    }
    if ((p = str_istr(body, "\"retryDelay\":"))) {
        q = skip_ws(p + 13);
        if (*q == '"' && (q = read_num(q + 1, &v)) && v > 0) {
            if (!strncasecmp(q, "ms\"", 3)) consider(&a, v);
            else if (!strncasecmp(q, "s\"", 2)) consider(&a, v * 1000);
        }
    }
    for (p = str_istr(body, "try again in"); p; p = str_istr(p + 1, "try again in")) {
        if (!ws_ch(p[12])) continue;
        double ms = num_unit_after(p + 12, false);
        if (ms > 0) {
            consider(&a, ms);
            break;
        }
    }
    /* legacy text: "retry-after: <secs|date>", "x-ratelimit-reset-ms: N", "x-ratelimit-reset: N" */
    for (p = str_istr(body, "retry-after"); p; p = str_istr(p + 1, "retry-after")) {
        q = skip_ws(p + 11);
        if (*q != ':' && *q != '=') continue;
        q = skip_ws(q + 1);
        const char *e = q;
        while (*e && !isspace((unsigned char)*e) && *e != ',' && *e != ';') e++;
        if (e == q) continue;
        char *tok = xstrndup(q, (size_t)(e - q));
        if (js_number(tok, &v)) consider_clamped(&a, v * 1000);
        else {
            int64_t t = http_date_ms(tok);
            if (t >= 0) consider_clamped(&a, (double)(t - now));
        }
        free(tok);
        break;
    }
    static const char *const legacy[] = {"x-ratelimit-reset-ms", "x-ratelimit-reset"};
    for (int i = 0; i < 2; i++) {
        for (p = str_istr(body, legacy[i]); p; p = str_istr(p + 1, legacy[i])) {
            q = skip_ws(p + strlen(legacy[i]));
            if (*q != ':' && *q != '=') continue;
            q = skip_ws(q + 1);
            if (!isdigit((unsigned char)*q)) continue;
            v = strtod(q, NULL);
            if (i == 0) consider_clamped(&a, v > 1e12 ? v - (double)now : v);
            else consider_clamped(&a, v > 1e9 ? v * 1000 - (double)now : v * 1000);
            break;
        }
    }
    if (a.longest > 0) return (int64_t)a.longest;
    if (a.naive > 0) return (int64_t)a.naive;
    return a.retry_now ? 0 : -1;
}

int64_t ai_extract_retry_hint(const http_headers_t *h, const char *body) {
    int64_t now = now_ms();
    if (h) {
        double d;
        const char *v = http_headers_get(h, "retry-after-ms");
        if (v && *v && js_number(v, &d) && d >= 0) return (int64_t)d;
        if ((v = http_headers_get(h, "retry-after")) && *v) {
            if (js_number(v, &d)) return (int64_t)MAX(0.0, d * 1000);
            int64_t t = http_date_ms(v);
            if (t >= 0) return MAX((int64_t)0, t - now);
        }
        if ((v = http_headers_get(h, "x-ratelimit-reset-ms")) && *v && js_number(v, &d) && d > 0) {
            if (d <= 1e9) return (int64_t)d;
            double target = d > 1e12 ? d : d * 1000;
            if (target > (double)now) return (int64_t)(target - (double)now);
        }
        if ((v = http_headers_get(h, "x-ratelimit-reset")) && isdigit((unsigned char)*v)) {
            int64_t delta = strtoll(v, NULL, 10) * 1000 - now;
            if (delta > 0) return delta;
        }
        if ((v = http_headers_get(h, "x-ratelimit-reset-after")) && *v && js_number(v, &d) && d > 0)
            return (int64_t)(d * 1000);
    }
    return body && *body ? body_retry_hint(body, now) : -1;
}

/* ---------- provider error classification ---------- */

static bool any_istr(const char *s, const char *const *pats, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (str_istr(s, pats[i])) return true;
    return false;
}

static int three_digits(const char *p) {
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) || !isdigit((unsigned char)p[2]) ||
        isdigit((unsigned char)p[3]))
        return 0;
    int v = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    return v >= 100 && v <= 599 ? v : 0;
}

/* AIError.status over a message: "503 ...", "error (503)", "error: 503", "status 503", "HTTP 503" */
static int message_status(const char *msg) {
    int v = three_digits(msg);
    if (v) return v;
    static const char *const pre[] = {"error (", "error: ", "error=", "status ", "status: ", "status=", "http "};
    for (size_t i = 0; i < ARRAY_LEN(pre); i++) {
        const char *p = str_istr(msg, pre[i]);
        if (p && (v = three_digits(p + strlen(pre[i])))) return v;
    }
    return 0;
}

/* account-level usage/quota limits belong to credential rotation, never to the provider backoff */
static bool usage_limit_text(const char *msg, int status) {
    static const char *const pats[] = {
        "usage limit",       "usage_limit",        "limit_reached",         "quota exceeded",    "quota reached",
        "quota_exceeded",    "insufficient_quota", "insufficient quota",    "resource exhausted", "resource_exhausted",
        "exhausted your capacity", "quota will reset", "insufficient balance", "balance exhausted", "out of credits",
        "spending limit",    "spending-limit",     "out of usage",          "access_terminated_error"};
    if (any_istr(msg, pats, ARRAY_LEN(pats))) return true;
    /* a 429/402 with an opaque body */
    return (status == 429 || status == 402) && (str_istr(msg, "status code (no body)") || str_istr(msg, "(empty response)"));
}

bool ai_provider_retryable_error(const char *message, int status) {
    const char *msg = message ? message : "";
    if (!status) status = message_status(msg);
    if (usage_limit_text(msg, status)) return false;
    if (status >= 400 && status < 500 && status != 408 && status != 429) /* ALIYUN_MM_GATEWAY_PATTERN */
        return str_istr(msg, "provided url does not appear to be valid") || str_istr(msg, "download multimodal file timed out");
    static const char *const transient[] = {
        /* unexpected socket closes, TLS corruption, TRANSIENT_TRANSPORT_PATTERN, provider extras, stream drops and
         * truncations, event-order envelopes, pi-utils TRANSIENT_MESSAGE_PATTERN, omc's own transport wording */
        "socket connection closed unexpectedly", "socket connection was closed unexpectedly", "socket is closed",
        "bad record mac", "type=server_error", "no capacity", "no_capacity", "no-capacity", "high demand", "peak demand",
        "at capacity", "over capacity", "insufficient capacity", "capacity exceeded", "capacity exhausted",
        "capacity_exceeded", "capacity_exhausted", "peak load", "overloaded", "provider returned error", "rate limit",
        "rate_limit", "rate-limit", "ratelimit", "too many requests", "service unavailable", "service_unavailable",
        "serviceunavailable", "temporarily unavailable", "temporary unavailable", "server error", "server_error",
        "servererror", "internal error", "internal_error", "internalerror", "retry your request", "network error",
        "network_error", "networkerror", "connection error", "connection_error", "connectionerror", "connection refused",
        "connection_refused", "other side closed", "use of closed network connection", "fetch failed",
        "upstream connect", "upstream_connect", "upstream request failed", "reset before headers", "socket hang up",
        "timed out", "time out", "timeout", "terminated", "retry delay", "stream stall", "no error details in response",
        "http2streamreset", "http2refusedstream", "http2enhanceyourcalm", "nghttp2_internal_error",
        "nghttp2_refused_stream", "etimeout", "enotfound", "enetunreach", "ehostunreach", "failedtoopensocket",
        "no internet connection", "couldn't reach", "couldnt reach", "couldn't connect to", "couldnt connect to",
        "tls certificate verification failed", "malformed function call", "malformed_function_call",
        "malformedfunctioncall", "unable to connect", "transport error reading codex response body",
        "received from peer", "1302", "unterminated string", "unexpected end of json input", "unexpected end of data",
        "unexpected eof", "end of file", "eof while parsing", "truncated", "stream disconnected before completion",
        "closed before response.completed", "stream was interrupted", "stream ended before",
        "socket disconnected before secure tls connection", "stream event order", "before message_start",
        "connection reset", "connection closed", "aborted"};
    if (any_istr(msg, transient, ARRAY_LEN(transient))) return true;
    static const char *const codes[] = {"429", "500", "502", "503", "504"};
    for (size_t i = 0; i < ARRAY_LEN(codes); i++)
        for (const char *p = strstr(msg, codes[i]); p; p = strstr(p + 1, codes[i]))
            if ((p == msg || !word_ch(p[-1])) && !word_ch(p[3])) return true;
    return status == 408 || status == 429 || status >= 500;
}

/* ---------- replay-safe stream retries ---------- */

typedef struct {
    ai_event_t ev;
    char *delta;
    json_t *block, *partial;
    char *reason;
} held_ev_t;

typedef struct {
    ai_event_cb cb;
    void *ud;
    bool committed;
    VEC(held_ev_t) held; /* pre-output events of the running attempt */
    held_ev_t terminal;
    bool has_terminal;
} replay_t;

static void held_clear(held_ev_t *h) {
    free(h->delta);
    json_free(h->block);
    json_free(h->partial);
    free(h->reason);
    memset(h, 0, sizeof *h);
}

static held_ev_t held_copy(const ai_event_t *ev) {
    held_ev_t h = {.ev = *ev};
    if (ev->delta) {
        h.delta = xstrndup(ev->delta, ev->delta_len);
        h.ev.delta = h.delta;
    }
    if (ev->block) h.ev.block = h.block = json_clone(ev->block);
    if (ev->partial) h.ev.partial = h.partial = json_clone(ev->partial);
    if (ev->reason) h.ev.reason = h.reason = xstrdup(ev->reason);
    return h;
}

static void replay_drop(replay_t *r) {
    for (size_t i = 0; i < r->held.n; i++) held_clear(&r->held.v[i]);
    r->held.n = 0;
    if (r->has_terminal) held_clear(&r->terminal);
    r->has_terminal = false;
}

static void replay_flush(replay_t *r) {
    for (size_t i = 0; i < r->held.n; i++) {
        r->cb(r->ud, &r->held.v[i].ev);
        held_clear(&r->held.v[i]);
    }
    r->held.n = 0;
}

/* isMeaningfulCompletionEvent */
static bool meaningful(const ai_event_t *ev) {
    switch (ev->type) {
    case AI_EV_TEXT_DELTA:
    case AI_EV_THINKING_DELTA:
    case AI_EV_TOOLCALL_DELTA: return ev->delta_len > 0;
    case AI_EV_TEXT_END: return !str_empty(json_get_str(ev->block, "text"));
    case AI_EV_THINKING_END: return !str_empty(json_get_str(ev->block, "thinking"));
    default: return false;
    }
}

static void replay_cb(void *ud, const ai_event_t *ev) {
    replay_t *r = ud;
    if (ev->type == AI_EV_DONE || ev->type == AI_EV_ERROR) {
        if (r->has_terminal) held_clear(&r->terminal);
        r->terminal = held_copy(ev);
        r->has_terminal = true;
        return;
    }
    if (!r->committed && !meaningful(ev)) {
        vec_push(&r->held, held_copy(ev));
        return;
    }
    r->committed = true;
    replay_flush(r);
    r->cb(r->ud, ev);
}

/* hasVisibleAssistantContent */
static bool visible_content(const json_t *msg) {
    json_t *b;
    json_arr_foreach(json_get(msg, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "image") || str_eq(t, "toolCall")) return true;
        if (str_eq(t, "text")) {
            for (const char *s = json_get_str(b, "text"); s && *s; s++)
                if (!isspace((unsigned char)*s)) return true;
        }
    }
    return false;
}

static bool wait_ms_cancellable(const ai_stream_opts_t *o, int64_t ms) {
    int64_t until = mono_ms() + ms;
    while (mono_ms() < until) {
        if (o && o->cancel && *o->cancel) return false;
        sleep_ms(MIN((int64_t)50, MAX((int64_t)1, until - mono_ms())));
    }
    return !(o && o->cancel && *o->cancel);
}

#define REPLAY_BASE_DELAY_MS 500 /* EMPTY_COMPLETION_BASE_DELAY_MS */
#define MAX_EMPTY_COMPLETION_RETRIES 2

json_t *ai_replay_safe_stream(ai_attempt_fn once, void *self, const ai_stream_opts_t *o, ai_event_cb cb, void *ud,
                              bool retry_empty, int max_error_retries) {
    replay_t r = {.cb = cb, .ud = ud};
    int empty_retries = 0, error_retries = 0;
    for (;;) {
        r.committed = o && o->accept_empty_response;
        json_t *msg = once(self, replay_cb, &r);
        const char *stop = json_get_str(msg, "stopReason");
        const char *errmsg = json_get_str(msg, "errorMessage");
        const char *sd = json_get_str(json_get(msg, "stopDetails"), "type");
        bool cancelled = o && o->cancel && *o->cancel;
        int64_t delay = -1;
        if (retry_empty && !(o && o->accept_empty_response) && !r.committed && str_eq(stop, "stop") &&
            !str_eq(sd, "pause_turn") && !str_eq(sd, "compaction") && !errmsg &&
            json_get_int(json_get(msg, "usage"), "output", 0) <= 1 && !visible_content(msg) &&
            empty_retries < MAX_EMPTY_COMPLETION_RETRIES) {
            delay = (int64_t)REPLAY_BASE_DELAY_MS << empty_retries;
            empty_retries++;
        } else if (max_error_retries > 0 && !r.committed && str_eq(stop, "error") && errmsg &&
                   error_retries < max_error_retries &&
                   ai_provider_retryable_error(errmsg, (int)json_get_int(msg, "errorStatus", 0))) {
            delay = (int64_t)REPLAY_BASE_DELAY_MS << error_retries;
            error_retries++;
        }
        if (delay >= 0 && !cancelled) {
            if (wait_ms_cancellable(o, delay)) {
                replay_drop(&r);
                json_free(msg);
                continue;
            }
        }
        replay_flush(&r);
        if (r.has_terminal) {
            cb(ud, &r.terminal.ev);
            held_clear(&r.terminal);
            r.has_terminal = false;
        }
        free(r.held.v);
        return msg;
    }
}
