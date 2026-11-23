/* mcperr.c - MCP tool failure diagnostics (port of omp mcp/errors.ts: MCPTransportError fields, sanitizeDiagnosticText,
 * serializeData, findTraceId, normalizeMCPTransportError, formatMCPToolFailure). */
#include "mcperr.h"

#include "text/regex.h"

#include <ctype.h>
#include <pthread.h>

#define MAX_MESSAGE_CHARS 1000
#define MAX_DATA_CHARS 2000
#define MAX_TRACE_ID_CHARS 128
#define MAX_DATA_STRING_CHARS 256
#define MAX_DATA_DEPTH 5
#define MAX_DATA_ENTRIES 30

void mcp_fail_free(mcp_fail_t *f) {
    if (!f) return;
    free(f->message);
    free(f->code);
    free(f->data);
    free(f->trace_id);
    memset(f, 0, sizeof *f);
}

void mcp_fail_set(mcp_fail_t *f, const char *transport, const char *stage, const char *failure, bool retryable,
                  const char *message, const char *code) {
    if (!f) return;
    mcp_fail_free(f);
    f->set = true;
    f->transport = transport;
    f->stage = stage;
    f->failure = failure;
    f->retryable = retryable;
    f->message = mcp_sanitize_diag(message ? message : "", MAX_MESSAGE_CHARS);
    f->code = code ? xstrdup(code) : NULL;
}

static size_t utf16_len(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* String.prototype.slice(0, n) over UTF-16 units, whole code points */
static char *utf16_slice(const char *s, size_t n) {
    size_t units = 0, i = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (p[i]) {
        size_t len = p[i] < 0x80 ? 1 : p[i] < 0xE0 ? 2 : p[i] < 0xF0 ? 3 : 4;
        size_t u = len == 4 ? 2 : 1;
        if (units + u > n) break;
        units += u;
        for (size_t k = 0; k < len && p[i]; k++) i++;
    }
    return xstrndup(s, i);
}

static char *re_replace(const char *s, const char *pattern, const char *rep) {
    rx_t *rx = rx_compile(pattern, RX_ICASE, NULL);
    if (!rx) return xstrdup(s);
    char *r = rx_replace(rx, s, strlen(s), rep, true);
    rx_free(rx);
    return r;
}

char *mcp_sanitize_diag(const char *value, size_t max_chars) {
    static const char *const P[][2] = {
        {"\\s*For more information, pass `verbose: true` in the second argument to fetch\\(\\)\\.?\\s*$", ""},
        {"\\b(Bearer|Basic)\\s+(?![\\w.~+/-]+\\s*=\\s*\")[^\\s,;]+", "$1 [redacted]"},
        {"([?&](?:access[-_]?token|api[-_]?key|key|token|secret|password)=)[^&#\\s]+", "${1}[redacted]"},
        {"((?:authorization|api[-_]?key|private[-_]?key|access[-_]?key|token|secret|passw(?:or)?d|pwd|credential)\\s*[:=]\\s*)"
         "[^\\s,;}]+",
         "${1}[redacted]"},
    };
    char *cur = xstrdup(value);
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        char *n = re_replace(cur, P[i][0], P[i][1]);
        free(cur);
        cur = n;
    }
    char *sl = utf16_slice(cur, max_chars);
    free(cur);
    /* .trim() */
    char *a = sl;
    while (*a && isspace((unsigned char)*a)) a++;
    size_t n = strlen(a);
    while (n && isspace((unsigned char)a[n - 1])) n--;
    char *r = xstrndup(a, n);
    free(sl);
    return r;
}

static rx_t *g_secret_rx;
static pthread_once_t g_secret_once = PTHREAD_ONCE_INIT;
static void secret_init(void) {
    g_secret_rx = rx_compile("(?:authorization|bearer|cookie|secret|passw(?:or)?d|pwd|token|credential|api[-_]?key|private[-_]?key|"
                             "access[-_]?key|signature)",
                             RX_ICASE, NULL);
}

static bool secret_key(const char *k) {
    pthread_once(&g_secret_once, secret_init);
    return g_secret_rx && rx_matches(g_secret_rx, k);
}

static json_t *sanitize_data(const json_t *v, int depth) {
    if (depth > MAX_DATA_DEPTH) return json_str("[truncated]");
    if (json_is_str(v)) return json_str_own(mcp_sanitize_diag(json_sv(v), MAX_DATA_STRING_CHARS));
    if (!v || json_is_null(v) || json_is_num(v) || json_is_bool(v)) return v ? json_clone(v) : json_null();
    if (json_is_arr(v)) {
        json_t *a = json_arr();
        for (size_t i = 0; i < json_len(v) && i < MAX_DATA_ENTRIES; i++) json_push(a, sanitize_data(json_at(v, i), depth + 1));
        return a;
    }
    json_t *o = json_obj();
    const char *k;
    json_t *item;
    size_t count = 0;
    json_obj_foreach(v, i, k, item) {
        if (count++ == MAX_DATA_ENTRIES) break;
        json_set(o, k, secret_key(k) ? json_str("[redacted]") : sanitize_data(item, depth + 1));
    }
    return o;
}

char *mcp_serialize_data(const json_t *data) {
    if (!data) return NULL;
    json_t *san = sanitize_data(data, 0);
    char *s = json_dump_js(san);
    json_free(san);
    size_t len = utf16_len(s);
    if (len <= MAX_DATA_CHARS) return s;
    long preview = MAX_DATA_CHARS - 64;
    for (int guard = 0; guard < 64 && preview > 0; guard++) {
        char *pv = utf16_slice(s, (size_t)preview);
        json_t *b = json_obj();
        json_set_bool(b, "truncated", true);
        json_set_str(b, "preview", pv);
        free(pv);
        char *bs = json_dump_js(b);
        json_free(b);
        size_t bl = utf16_len(bs);
        if (bl <= MAX_DATA_CHARS) {
            free(s);
            return bs;
        }
        preview -= (long)(bl - MAX_DATA_CHARS);
        free(bs);
    }
    free(s);
    return NULL;
}


static char *safe_trace_id(const char *v) {
    if (!v) return NULL;
    while (*v && isspace((unsigned char)*v)) v++;
    size_t n = strlen(v);
    while (n && isspace((unsigned char)v[n - 1])) n--;
    if (!n || n > MAX_TRACE_ID_CHARS) return NULL;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)v[i]) && !strchr("._:/-", v[i])) return NULL;
    return xstrndup(v, n);
}

static bool trace_key(const char *k) {
    char *l = str_lower(k);
    bool r = !strcmp(l, "traceparent");
    static const char *const base[] = {"trace", "request", "correlation"};
    for (size_t i = 0; i < 3 && !r; i++) {
        size_t bl = strlen(base[i]);
        if (strncmp(l, base[i], bl)) continue;
        const char *p = l + bl;
        if (*p == '-' || *p == '_') p++;
        r = !strcmp(p, "id");
    }
    free(l);
    return r;
}

char *mcp_find_trace_id(const json_t *v, int depth) {
    if (depth > MAX_DATA_DEPTH || !json_is_obj(v)) return NULL;
    const char *k;
    json_t *item;
    json_obj_foreach(v, i, k, item) {
        if (trace_key(k)) {
            char *t = safe_trace_id(json_sv(item));
            if (t) return t;
        }
        char *nested = mcp_find_trace_id(item, depth + 1);
        if (nested) return nested;
    }
    return NULL;
}

void mcp_fail_json_rpc(mcp_fail_t *f, const char *transport, const json_t *error) {
    if (!f) return;
    const json_t *code = json_get(error, "code");
    char *cs = NULL;
    if (json_is_num(code)) {
        sb_t b = SB_INIT;
        json_js_num_sb(&b, code->is_int ? (double)code->u.num.i : code->u.num.d);
        cs = sb_detach(&b);
    } else {
        cs = json_is_str(code) ? xstrdup(json_sv(code)) : xstrdup("undefined");
    }
    const char *msg = json_get_str(error, "message");
    char *m = xasprintf("MCP error %s: %s", cs, msg ? msg : "undefined");
    mcp_fail_set(f, transport, "protocol", "json_rpc", false, m, json_get(error, "code") ? cs : NULL);
    free(m);
    free(cs);
    const json_t *data = json_get(error, "data");
    if (data) {
        f->data = mcp_serialize_data(data);
        f->trace_id = mcp_find_trace_id(data, 0);
    }
}

static bool has_word(const char *msg, const char *const *words) {
    for (size_t i = 0; words[i]; i++) {
        const char *p = msg;
        size_t wl = strlen(words[i]);
        while ((p = str_istr(p, words[i]))) {
            bool l = p == msg || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
            bool r = !(isalnum((unsigned char)p[wl]) || p[wl] == '_');
            if (l && r) return true;
            p++;
        }
    }
    return false;
}

void mcp_fail_normalize(mcp_fail_t *f, const char *transport, const char *stage, const char *message, const char *code) {
    if (!f) return;
    char *m = mcp_sanitize_diag(message ? message : "", MAX_MESSAGE_CHARS);
    char *uc = code ? str_lower(code) : NULL;
    for (char *p = uc; p && *p; p++) *p = (char)toupper((unsigned char)*p);
    static const char *const CONNECT[] = {"ECONNREFUSED", "ConnectionRefused", "ENETUNREACH", "EHOSTUNREACH", "ENOTFOUND",
                                          "EAI_AGAIN", "FailedToOpenSocket", NULL};
    static const char *const RESET[] = {"ECONNRESET", NULL};
    static const char *const EOFW[] = {"EPIPE", "eof", "transport closed", "socket closed", NULL};
    const char *failure = "unknown";
    bool retryable = false;
    char *codev = code ? xstrdup(code) : NULL;
    bool cc = uc && (!strcmp(uc, "ECONNREFUSED") || !strcmp(uc, "CONNECTIONREFUSED") || !strcmp(uc, "ENETUNREACH") ||
                     !strcmp(uc, "EHOSTUNREACH") || !strcmp(uc, "ENOTFOUND") || !strcmp(uc, "EAI_AGAIN") ||
                     !strcmp(uc, "FAILEDTOOPENSOCKET"));
    int st = 0;
    if (cc || has_word(m, CONNECT)) {
        failure = "connect";
        retryable = true;
    } else if ((uc && !strcmp(uc, "ECONNRESET")) || has_word(m, RESET)) {
        failure = "reset";
        retryable = true;
    } else if ((uc && !strcmp(uc, "EPIPE")) || has_word(m, EOFW)) {
        failure = "eof";
        retryable = true;
    } else if (str_istr(m, "timeout") || str_istr(m, "timed out")) {
        failure = "timeout";
    } else if (!strncasecmp(m, "HTTP ", 5) && sscanf(m + 5, "%3d:", &st) == 1 && st >= 100 && st <= 999 && m[8] == ':') {
        free(codev);
        codev = xasprintf("%d", st);
        failure = "http_status";
        retryable = st == 404 || st == 502 || st == 503;
    } else if (str_istr(m, "not connected") || str_istr(m, "connection failed") || str_istr(m, "fetch failed")) {
        failure = "connect";
        retryable = true;
    } else if ((str_istr(m, "invalid") || str_istr(m, "unexpected end")) && str_istr(m, "json")) {
        failure = "malformed_response";
    } else if (str_istr(m, "malformed")) {
        failure = "malformed_response";
    }
    const char *stg = !strcmp(failure, "connect") ? "connect" : !strcmp(failure, "malformed_response") ? "decode" : stage;
    mcp_fail_set(f, transport, stg, failure, retryable, *m ? m : "Unknown MCP transport failure", codev);
    free(codev);
    free(uc);
    free(m);
}

/* EXPIRED_SESSION: /(?:unknown|expired|invalid) session|session (?:not found|expired|is invalid)/i */
static bool expired_session(const char *m) {
    if (!m) return false;
    static const char *const pre[] = {"unknown session", "expired session", "invalid session", "session not found",
                                      "session expired", "session is invalid"};
    for (size_t i = 0; i < ARRAY_LEN(pre); i++)
        if (str_istr(m, pre[i])) return true;
    return false;
}

bool mcp_fail_reconnectable(const mcp_fail_t *f) {
    if (!f || !f->set) return false;
    if (!strcmp(f->failure, "connect") || !strcmp(f->failure, "reset") || !strcmp(f->failure, "eof") || !strcmp(f->failure, "closed"))
        return f->retryable;
    if (!strcmp(f->failure, "http_status") && f->code && (!strcmp(f->code, "404") || !strcmp(f->code, "502") || !strcmp(f->code, "503")))
        return true;
    /* an expired HTTP session (-32001 Unknown or expired session) never ran the call: re-initialize and retry once */
    return (!strcmp(f->failure, "http_status") || !strcmp(f->failure, "json_rpc")) && expired_session(f->message);
}

static const char *next_step(const mcp_fail_t *f) {
    const char *fl = f->failure;
    if (!f->retryable && (!strcmp(fl, "connect") || !strcmp(fl, "eof") || !strcmp(fl, "reset") || !strcmp(fl, "closed")))
        return "The connection was lost after the request was sent, so it may already have run; check its effect before retrying.";
    if (!strcmp(fl, "connect")) return "Check that the MCP server is running and reachable, then retry.";
    if (!strcmp(fl, "timeout"))
        return "Raise this server's `timeout` (ms) in its MCP config or set OMP_MCP_TIMEOUT_MS; the request outcome is unknown.";
    if (!strcmp(fl, "eof") || !strcmp(fl, "reset"))
        return !strcmp(f->transport, "stdio") ? "Inspect the MCP subprocess logs, restart the server, then retry."
                                             : "Check the MCP server logs and availability, then retry.";
    if (!strcmp(fl, "malformed_response")) return "Inspect the MCP server logs for an invalid JSON-RPC response.";
    if (!strcmp(fl, "json_rpc")) return "Address the server-reported MCP error before retrying.";
    if (!strcmp(fl, "http_status"))
        return f->retryable ? "Check the MCP server status, then retry." : "Check the MCP endpoint and authentication configuration.";
    if (!strcmp(fl, "closed")) return "Reconnect the MCP server, then retry.";
    return "Inspect the MCP server logs and transport configuration.";
}

char *mcp_format_tool_failure(const mcp_fail_t *f, const char *server, const char *tool) {
    char *sv = mcp_sanitize_diag(server ? server : "", MAX_MESSAGE_CHARS);
    char *tv = mcp_sanitize_diag(tool ? tool : "", MAX_MESSAGE_CHARS);
    sb_t b = SB_INIT;
    sb_appendf(&b, "MCP failure\nserver: %s\ntool: %s\ntransport: %s\nstage: %s\nfailure: %s\nretryable: %s\nmessage: %s", sv, tv,
               f->transport, f->stage, f->failure, f->retryable ? "yes" : "no", f->message ? f->message : "");
    if (f->code) sb_appendf(&b, "\ncode: %s", f->code);
    if (f->trace_id) sb_appendf(&b, "\ntrace_id: %s", f->trace_id);
    if (f->data) sb_appendf(&b, "\ndata: %s", f->data);
    sb_appendf(&b, "\nnext: %s", next_step(f));
    free(sv);
    free(tv);
    return sb_detach(&b);
}
