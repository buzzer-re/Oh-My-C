/*
 * provider_cursor.c - "cursor-agent" API: Cursor's agent service (port of omp packages/ai/src/providers/cursor.ts).
 *
 * Wire: Connect protocol over HTTP/1.1, the official RunSSE/BidiAppend path omp falls back to when HTTP/2 is
 * unavailable. A run is one streaming POST to /agent.v1.AgentService/RunSSE whose body is a single framed
 * BidiRequestId; the response streams framed AgentServerMessages. Every client message (the run request
 * itself, exec results, blob replies, heartbeats) goes out as a unary BidiAppend POST carrying the request
 * id and a sequence number. Messages are protobuf (pb.c + cursor_proto.c).
 *
 * Cursor's server drives the agent loop: it streams text/thinking, announces tool calls, and asks the client
 * to run tools through exec frames. Those run as local tools through the host bridge (opts->cursor_exec);
 * each such call gets a toolCall block in the message and a paired toolResult handed to the bridge.
 *
 * Threads: a reader thread drains RunSSE (answering blob and permission queries inline), a writer thread
 * sends appends in order plus a heartbeat every 5 s, and the calling thread processes interaction updates
 * and exec frames, so events and tool runs stay on the caller's thread.
 */
#define _GNU_SOURCE
#include "ai.h"
#include "cursor_proto.h"
#include "core/fs.h"
#include "tools/diff.h"

#include <errno.h>
#include <math.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define CURSOR_DEFAULT_BASE_URL "https://api2.cursor.sh"
#define CURSOR_CLIENT_VERSION "cli-2026.09.02-c22c1a3"
#define CURSOR_RUN_SSE_PATH "/agent.v1.AgentService/RunSSE"
#define CURSOR_BIDI_APPEND_PATH "/aiserver.v1.BidiService/BidiAppend"
#define CURSOR_MAX_STREAM_RETRIES 5
#define CURSOR_RETRY_BASE_DELAY_MS 500
#define CURSOR_CONVERSATION_CACHE_MAX 128
#define CURSOR_HEARTBEAT_MS 5000
#define CURSOR_MAX_FRAME (256u << 20)
#define MAX_TOOL_CALL_ID_LENGTH 64
#define EDIT_MATERIALIZATION_MAX_BYTES (4 * 1024 * 1024)
#define NOT_IMPLEMENTED "Not implemented by this client"
#define NOT_IMPLEMENTED_SUFFIX "not implemented by this client"


/* ======================================================================================================
 * small helpers
 * ==================================================================================================== */

static json_t *jobj1(const char *k, json_t *v) {
    json_t *o = json_obj();
    json_set(o, k, v);
    return o;
}

static json_t *jbytes(const void *p, size_t n) { return json_strn(p ? p : "", n); }

static const char *jstr(const json_t *o, const char *k) {
    const char *s = json_get_str(o, k);
    return s ? s : "";
}

static size_t jstrlen(const json_t *o, const char *k) {
    json_t *v = json_get(o, k);
    return json_is_str(v) ? v->u.str.len : 0;
}

static bool has(const json_t *o, const char *k) { return json_get(o, k) != NULL; }

static void sha256(const void *data, size_t len, unsigned char out[32]) {
    SHA256((const unsigned char *)(data ? data : ""), len, out);
}

static char *sha256_hex(const void *data, size_t len) {
    unsigned char md[32];
    sha256(data, len, md);
    return hex_encode(md, 32);
}

/* omp deterministicUuid: sha256(seed) hex shaped as a UUID */
static char *deterministic_uuid(const char *seed) {
    char *h = sha256_hex(seed, strlen(seed));
    char *r = xasprintf("%.8s-%.4s-%.4s-%.4s-%.12s", h, h + 8, h + 12, h + 16, h + 20);
    free(h);
    return r;
}

/* omp normalizeToolCallId: [^a-zA-Z0-9_-] -> _, max 64 */
static char *normalize_tool_call_id(const char *id) {
    sb_t b = SB_INIT;
    for (const char *p = id ? id : ""; *p && b.len < MAX_TOOL_CALL_ID_LENGTH; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        sb_putc(&b, ok ? c : '_');
    }
    return sb_detach(&b);
}

/* omp sanitizeText: strip ANSI escapes and C0/C1 controls except \t and \n */
static char *sanitize_text(const char *s, size_t n) {
    char *u = utf8_sanitize(s, n);
    sb_t b = SB_INIT;
    size_t len = strlen(u);
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)u[i];
        if (c == 0x1b) { /* ESC sequence: CSI / OSC / two-byte */
            i++;
            if (i < len && u[i] == '[') {
                i++;
                while (i < len && !((unsigned char)u[i] >= 0x40 && (unsigned char)u[i] <= 0x7e)) i++;
                if (i < len) i++;
            } else if (i < len && u[i] == ']') {
                i++;
                while (i < len && u[i] != 0x07 && !(u[i] == 0x1b && i + 1 < len && u[i + 1] == '\\')) i++;
                if (i < len) i += u[i] == 0x07 ? 1 : 2;
            } else if (i < len) {
                i++;
            }
            continue;
        }
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7f) {
            i++;
            continue;
        }
        if (c == 0xc2 && i + 1 < len && (unsigned char)u[i + 1] >= 0x80 && (unsigned char)u[i + 1] <= 0x9f) {
            i += 2; /* C1 control */
            continue;
        }
        sb_putc(&b, (char)c);
        i++;
    }
    free(u);
    return sb_detach(&b);
}

/* tool result content -> text (images as "[mime image]") */
static char *tool_result_text(const json_t *tr) {
    sb_t b = SB_INIT;
    json_t *c = json_get(tr, "content"), *it;
    bool first = true;
    json_arr_foreach(c, i, it) {
        if (!first) sb_putc(&b, '\n');
        first = false;
        if (str_eq(json_get_str(it, "type"), "image")) sb_appendf(&b, "[%s image]", jstr(it, "mimeType"));
        else sb_append(&b, jstr(it, "text"));
    }
    return sb_detach(&b);
}

static bool tr_is_error(const json_t *tr) { return json_get_bool(tr, "isError", false); }

static json_t *make_tool_result(const char *id, const char *name, const char *text, bool is_error) {
    json_t *m = json_obj();
    json_set_str(m, "role", "toolResult");
    json_set_str(m, "toolCallId", id);
    json_set_str(m, "toolName", name);
    json_t *c = json_arr();
    json_t *t = json_obj();
    json_set_str(t, "type", "text");
    json_set_str(t, "text", text);
    json_push(c, t);
    json_set(m, "content", c);
    json_set_bool(m, "isError", is_error);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

static size_t count_lines(const char *s) {
    if (!s || !*s) return 0;
    size_t n = 1;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

/* omp piReadPath and friends (cursor-pi-args.ts) */
static bool chunk_is_raw(const char *s, size_t n) { return n == 3 && strncasecmp(s, "raw", 3) == 0; }

static bool path_has_raw(const char *p) {
    const char *s = p;
    for (;;) {
        const char *c = strchr(s, ':');
        size_t n = c ? (size_t)(c - s) : strlen(s);
        if (chunk_is_raw(s, n)) return true;
        if (!c) return false;
        s = c + 1;
    }
}

/* ^L?(\d+)(?:(\.\.|[-+])L?(\d+)?)?$ list joined by ',' */
static bool is_range_list(const char *s, size_t n) {
    if (!n) return false;
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *c = memchr(p, ',', (size_t)(end - p));
        const char *ce = c ? c : end;
        const char *q = p;
        if (q < ce && (*q == 'L' || *q == 'l')) q++;
        if (q >= ce || *q < '0' || *q > '9') return false;
        long start = 0;
        while (q < ce && *q >= '0' && *q <= '9') start = start * 10 + (*q++ - '0');
        if (start < 1) return false;
        if (q < ce) {
            char sep = 0;
            if (ce - q >= 2 && q[0] == '.' && q[1] == '.') {
                sep = '.';
                q += 2;
            } else if (*q == '-' || *q == '+') {
                sep = *q++;
            } else {
                return false;
            }
            if (q < ce && (*q == 'L' || *q == 'l')) q++;
            bool have_end = q < ce;
            long e = 0;
            while (q < ce && *q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0');
            if (q != ce) return false;
            if (sep == '+') {
                if (!have_end || e < 1) return false;
            } else if (have_end && e < start) {
                return false;
            }
        }
        p = c ? c + 1 : end;
    }
    return true;
}

static bool read_path_has_range(const char *p) {
    const char *last = strrchr(p, ':');
    const char *l = last ? last + 1 : p;
    if (is_range_list(l, strlen(l))) return true;
    if (!last || !chunk_is_raw(l, strlen(l))) return false;
    const char *prev_end = last;
    const char *prev = prev_end;
    while (prev > p && prev[-1] != ':') prev--;
    if (prev == p && prev_end == p) return false;
    return is_range_list(prev, (size_t)(prev_end - prev));
}

/* NULL when limit <= 0 (no selector expresses "zero lines") */
static int64_t jfloor(const json_t *v) {
    if (!json_is_num(v)) return 0;
    return v->is_int ? v->u.num.i : (int64_t)floor(v->u.num.d);
}

static char *pi_read_path(const char *path, const json_t *offset, const json_t *limit) {
    if (!json_is_num(offset)) offset = NULL;
    if (!json_is_num(limit)) limit = NULL;
    if (limit && jfloor(limit) <= 0) return NULL;
    bool has_start = offset != NULL, has_count = limit != NULL;
    if (!has_start && !has_count) return xstrdup(path);
    char *base = path_has_raw(path) ? xstrdup(path) : xasprintf("%s:raw", path);
    char *r;
    int64_t start = has_start ? MAX(1, jfloor(offset)) : 0;
    int64_t count = has_count ? jfloor(limit) : 0;
    if (!has_start) r = xasprintf("%s:1+%lld", base, (long long)count);
    else if (!has_count) r = xasprintf("%s:%lld-", base, (long long)start);
    else r = xasprintf("%s:%lld+%lld", base, (long long)start, (long long)count);
    free(base);
    return r;
}

static char *cursor_raw_read_path(const char *path) {
    if (path_has_raw(path)) return xstrdup(path);
    if (read_path_has_range(path)) {
        const char *last = strrchr(path, ':');
        const char *l = last ? last + 1 : path;
        bool bare_line = true;
        const char *q = l;
        if (*q == 'L' || *q == 'l') q++;
        if (!*q) bare_line = false;
        for (; *q; q++)
            if (*q < '0' || *q > '9') bare_line = false;
        char *head = last ? xstrndup(path, (size_t)(last - path)) : xstrdup("");
        char *r = xasprintf("%s:raw:%s%s", head, l, bare_line ? "-" : "");
        free(head);
        return r;
    }
    return xasprintf("%s:raw", path);
}

static char *cursor_exec_read_path(const char *path, const json_t *offset, const json_t *limit) {
    char *ranged = pi_read_path(path, offset, limit);
    if (!ranged) return NULL;
    char *r = cursor_raw_read_path(ranged);
    free(ranged);
    return r;
}

static char *pi_read_display_path(const char *path, const json_t *offset, const json_t *limit) {
    char *c = pi_read_path(path, offset, limit);
    if (c) return c;
    int64_t start = json_is_num(offset) ? MAX(1, jfloor(offset)) : 1;
    return xasprintf("%s:raw:%lld+0", path, (long long)start);
}

static bool path_is_abs(const char *p) { return p && p[0] == '/'; }

/* path.join(base, pattern) unless pattern is absolute or base is empty/"." */
static char *pi_join_path(const char *base, const char *pattern) {
    if (path_is_abs(pattern)) return xstrdup(pattern);
    if (!base || !*base || strcmp(base, ".") == 0) return xstrdup(pattern ? pattern : "");
    size_t bl = strlen(base);
    while (bl > 1 && base[bl - 1] == '/') bl--;
    return xasprintf("%.*s/%s", (int)bl, base, pattern ? pattern : "");
}

static char *regex_escape_literal(const char *s) {
    sb_t b = SB_INIT;
    for (; *s; s++) {
        if (strchr(".*+?^${}()|[]\\", *s)) sb_putc(&b, '\\');
        sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

/* shellDeadlineSeconds: hard timeout wins, else a wait budget above the 300 s default; -1 = unset */
static int64_t ms_to_seconds(int64_t ms) {
    if (ms <= 0) return -1;
    int64_t s = (ms + 500) / 1000;
    return s < 1 ? 1 : s;
}

/* both budgets stop at the bash tool's 3600 s ceiling: Cursor's native Shell sends a 24 h hard_timeout on every
 * call, which the tool would otherwise clamp with a notice on every result */
#define BASH_MAX_DEADLINE_SECONDS 3600

static int64_t shell_deadline_seconds(const json_t *args) {
    json_t *hard = json_get(args, "hardTimeout");
    if (hard) {
        int64_t h = ms_to_seconds(json_get_int(args, "hardTimeout", 0));
        if (h > 0) return MIN(h, BASH_MAX_DEADLINE_SECONDS);
    }
    int64_t w = ms_to_seconds(json_get_int(args, "timeout", 0));
    return w > 300 ? MIN(w, BASH_MAX_DEADLINE_SECONDS) : -1;
}

/* ======================================================================================================
 * conversation cache: checkpoint + blob store per conversation id (bounded LRU)
 * ==================================================================================================== */

typedef struct {
    char *data;
    size_t len;
} blob_t;

typedef struct conv {
    char *id;
    json_t *state;  /* ConversationStateStructure (decoded) or NULL */
    hmap_t blobs;   /* hex(blob id) -> blob_t* */
    uint64_t used;
    int refs;
    bool evicted;
} conv_t;

static pthread_mutex_t g_conv_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(conv_t *) g_convs;
static uint64_t g_conv_clock;
/* #8345: base conversation id -> rotated wire id */
static hmap_t g_rotated;
static hmap_t g_rotated_ok;    /* rotated ids that completed a turn */
static hmap_t g_rotated_fresh; /* rotated ids not yet used */
static bool g_maps_init;

static void blob_free(void *p) {
    blob_t *b = p;
    if (!b) return;
    free(b->data);
    free(b);
}

static void conv_destroy(conv_t *c) {
    free(c->id);
    json_free(c->state);
    hmap_free(&c->blobs, blob_free);
    free(c);
}

static void maps_init_locked(void) {
    if (g_maps_init) return;
    g_maps_init = true;
    hmap_init(&g_rotated);
    hmap_init(&g_rotated_ok);
    hmap_init(&g_rotated_fresh);
}

/* returns a referenced entry (created when missing); *existed tells whether it was cached */
static conv_t *conv_acquire(const char *id, bool *existed) {
    pthread_mutex_lock(&g_conv_mu);
    conv_t *c = NULL;
    for (size_t i = 0; i < g_convs.n; i++)
        if (strcmp(g_convs.v[i]->id, id) == 0) c = g_convs.v[i];
    *existed = c != NULL;
    if (!c) {
        c = xcalloc(1, sizeof(*c));
        c->id = xstrdup(id);
        hmap_init(&c->blobs);
        vec_push(&g_convs, c);
        while (g_convs.n > CURSOR_CONVERSATION_CACHE_MAX) {
            size_t lru = 0;
            for (size_t i = 1; i < g_convs.n; i++)
                if (g_convs.v[i]->used < g_convs.v[lru]->used) lru = i;
            conv_t *old = g_convs.v[lru];
            g_convs.v[lru] = g_convs.v[--g_convs.n];
            old->evicted = true;
            if (!old->refs) conv_destroy(old);
        }
    }
    c->used = ++g_conv_clock;
    c->refs++;
    pthread_mutex_unlock(&g_conv_mu);
    return c;
}

static void conv_release(conv_t *c, bool drop) {
    pthread_mutex_lock(&g_conv_mu);
    if (drop && !c->evicted) {
        for (size_t i = 0; i < g_convs.n; i++) {
            if (g_convs.v[i] == c) {
                g_convs.v[i] = g_convs.v[--g_convs.n];
                break;
            }
        }
        c->evicted = true;
    }
    if (--c->refs == 0 && c->evicted) conv_destroy(c);
    pthread_mutex_unlock(&g_conv_mu);
}

static void conv_drop_id(const char *id) {
    pthread_mutex_lock(&g_conv_mu);
    for (size_t i = 0; i < g_convs.n; i++) {
        conv_t *c = g_convs.v[i];
        if (strcmp(c->id, id) == 0) {
            g_convs.v[i] = g_convs.v[--g_convs.n];
            c->evicted = true;
            if (!c->refs) conv_destroy(c);
            break;
        }
    }
    pthread_mutex_unlock(&g_conv_mu);
}

/* store a blob under sha256(data); returns the 32-byte id as a JSON bytes string */
static json_t *store_blob(conv_t *c, const void *data, size_t len) {
    unsigned char id[32];
    sha256(data, len, id);
    char *key = hex_encode(id, 32);
    pthread_mutex_lock(&g_conv_mu);
    if (!hmap_get(&c->blobs, key)) {
        blob_t *b = xmalloc(sizeof(*b));
        b->data = xmalloc(len ? len : 1);
        if (len) memcpy(b->data, data, len);
        b->len = len;
        hmap_put(&c->blobs, key, b);
    }
    pthread_mutex_unlock(&g_conv_mu);
    free(key);
    return jbytes(id, 32);
}

static void set_blob(conv_t *c, const json_t *id, const json_t *data) {
    if (!json_is_str(id)) return;
    char *key = hex_encode(id->u.str.s, id->u.str.len);
    blob_t *b = xmalloc(sizeof(*b));
    size_t len = json_is_str(data) ? data->u.str.len : 0;
    b->data = xmalloc(len ? len : 1);
    if (len) memcpy(b->data, data->u.str.s, len);
    b->len = len;
    pthread_mutex_lock(&g_conv_mu);
    blob_free(hmap_put(&c->blobs, key, b));
    pthread_mutex_unlock(&g_conv_mu);
    free(key);
}

/* owned copy of a blob as a JSON bytes string, NULL when unknown */
static json_t *get_blob(conv_t *c, const json_t *id) {
    if (!json_is_str(id)) return NULL;
    char *key = hex_encode(id->u.str.s, id->u.str.len);
    pthread_mutex_lock(&g_conv_mu);
    blob_t *b = hmap_get(&c->blobs, key);
    json_t *r = b ? jbytes(b->data, b->len) : NULL;
    pthread_mutex_unlock(&g_conv_mu);
    free(key);
    return r;
}

static void conv_set_state(conv_t *c, const json_t *state) {
    pthread_mutex_lock(&g_conv_mu);
    json_free(c->state);
    c->state = state ? json_clone(state) : NULL;
    pthread_mutex_unlock(&g_conv_mu);
}

static json_t *conv_get_state(conv_t *c) {
    pthread_mutex_lock(&g_conv_mu);
    json_t *s = c->state ? json_clone(c->state) : NULL;
    pthread_mutex_unlock(&g_conv_mu);
    return s;
}

/* ======================================================================================================
 * model wire resolution (resolveWireModelId, resolveCursorMaxMode, resolveCursorWireModel)
 * ==================================================================================================== */

static const char *const EFFORT_ORDER[] = {"minimal", "low", "medium", "high", "xhigh", "max"};

typedef struct {
    const char *suffix;
    const char *effort; /* NULL: thinking variant */
    const char *except_bare_prefix;
} suffix_rule_t;

static const suffix_rule_t SUFFIX_RULES[] = {
    {"-thinking", NULL, NULL}, {"-extra-high", "xhigh", NULL}, {"-none", "off", NULL},   {"-minimal", "minimal", NULL},
    {"-medium", "medium", NULL}, {"-xhigh", "xhigh", NULL},     {"-high", "high", NULL}, {"-low", "low", NULL},
    {"-max", "max", "qwen"},
};

static const char *bare_of(const char *id) {
    const char *s = strrchr(id, '/');
    return s ? s + 1 : id;
}

static bool ends_with_ci(const char *s, size_t n, const char *suf) {
    size_t k = strlen(suf);
    return n >= k && strncasecmp(s + n - k, suf, k) == 0;
}

/* collapseVariantId("cursor", id): logical id + effort ("" when none). Returns owned logical id. */
static char *collapse_variant(const char *id, const char **effort) {
    *effort = NULL;
    size_t n = strlen(id);
    char *lower = str_lower(id);
    const char *bare = bare_of(lower);
    const suffix_rule_t *win = NULL;
    for (size_t i = 0; i < ARRAY_LEN(SUFFIX_RULES); i++) {
        const suffix_rule_t *r = &SUFFIX_RULES[i];
        if (!ends_with_ci(lower, n, r->suffix)) continue;
        if (r->except_bare_prefix && str_starts(bare, r->except_bare_prefix)) continue;
        if (!win || strlen(r->suffix) > strlen(win->suffix)) win = r;
    }
    if (win) {
        *effort = win->effort;
        free(lower);
        return xstrndup(id, n - strlen(win->suffix));
    }
    /* cursor "-fast" lane wrapping an effort tier */
    if (ends_with_ci(lower, n, "-fast")) {
        size_t tn = n - 5;
        const char *tbare = bare_of(lower);
        const suffix_rule_t *er = NULL;
        for (size_t i = 0; i < ARRAY_LEN(SUFFIX_RULES); i++) {
            const suffix_rule_t *r = &SUFFIX_RULES[i];
            if (!r->effort || !ends_with_ci(lower, tn, r->suffix)) continue;
            if (r->except_bare_prefix && str_starts(tbare, r->except_bare_prefix)) continue;
            if (!er || strlen(r->suffix) > strlen(er->suffix)) er = r;
        }
        if (er && tn > strlen(er->suffix) && lower[tn - strlen(er->suffix) - 1] != '/') {
            *effort = er->effort;
            char *r = xasprintf("%.*s%s", (int)(tn - strlen(er->suffix)), id, id + tn);
            free(lower);
            return r;
        }
    }
    free(lower);
    return xstrdup(id);
}

static bool is_max_mode_wire_id(const char *id) {
    const char *e;
    char *l = collapse_variant(id, &e);
    free(l);
    return e && (strcmp(e, "xhigh") == 0 || strcmp(e, "max") == 0);
}

static const char *model_str(const ai_model_t *m, const char *k) { return json_get_str(m->raw, k); }

static int model_bool(const ai_model_t *m, const char *k) { /* -1 unset */
    json_t *v = json_get(m->raw, k);
    return json_is_bool(v) ? v->u.b : -1;
}

static bool resolve_max_mode(const ai_model_t *m, const char *wire) {
    json_t *routes = json_get_obj(m->raw, "cursorMaxModeRoutes");
    json_t *d = routes ? json_get(routes, wire) : NULL;
    if (json_is_bool(d)) return d->u.b;
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    int own = model_bool(m, "cursorMaxMode");
    if (!routing || strcmp(wire, m->id) == 0) return own >= 0 ? own : is_max_mode_wire_id(wire);
    const char *off = json_get_str(routing, "off");
    bool routes_own = str_eq(off, m->id);
    bool inferred = off && is_max_mode_wire_id(off);
    for (size_t i = 0; i < ARRAY_LEN(EFFORT_ORDER); i++) {
        const char *t = json_get_str(routing, EFFORT_ORDER[i]);
        if (str_eq(t, m->id)) routes_own = true;
        if (t && is_max_mode_wire_id(t)) inferred = true;
    }
    if (routes_own) return own >= 0 ? own : is_max_mode_wire_id(wire);
    if (own == 1 && !inferred) return true;
    return is_max_mode_wire_id(wire);
}

static const char *model_class(const ai_model_t *m) {
    json_t *id = json_get_obj(m->raw, "identity");
    return id ? json_get_str(id, "class") : NULL;
}

static const char *model_family(const ai_model_t *m) {
    json_t *id = json_get_obj(m->raw, "identity");
    return id ? json_get_str(id, "family") : NULL;
}

/* wire id after thinking-effort routing (resolveWireModelId) */
static char *resolve_wire_model_id(const ai_model_t *m, const ai_stream_opts_t *o) {
    const char *effort = "off";
    if (o->reasoning != EFFORT_OFF && !o->disable_reasoning && m->reasoning) effort = effort_name(o->reasoning);
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    const char *t = routing ? json_get_str(routing, effort) : NULL;
    if (t) return xstrdup(t);
    const char *rid = model_str(m, "requestModelId");
    return xstrdup(rid ? rid : m->id);
}

typedef struct {
    char *model_id;
    char *details_id;
    json_t *parameters; /* array of {id, value} */
    bool max_mode;
} wire_model_t;

static void wire_model_free(wire_model_t *w) {
    free(w->model_id);
    free(w->details_id);
    json_free(w->parameters);
}

static json_t *param(const char *id, const char *value) {
    json_t *p = json_obj();
    json_set_str(p, "id", id);
    json_set_str(p, "value", value);
    return p;
}

static void resolve_wire_model(const ai_model_t *m, const char *wire, bool discovered, wire_model_t *w) {
    memset(w, 0, sizeof(*w));
    w->max_mode = resolve_max_mode(m, wire);
    w->parameters = json_arr();
    json_t *routes = json_get_obj(m->raw, "cursorModelRoutes");
    json_t *route = routes ? json_get_obj(routes, wire) : NULL;
    if (route) {
        w->model_id = xstrdup(jstr(route, "modelId"));
        w->details_id = xstrdup(wire);
        json_t *ps = json_get_arr(route, "parameters"), *p;
        json_arr_foreach(ps, i, p) json_push(w->parameters, param(jstr(p, "id"), jstr(p, "value")));
        json_t *mm = json_get(route, "maxMode");
        if (json_is_bool(mm)) w->max_mode = mm->u.b;
        return;
    }
    if (discovered) {
        w->model_id = xstrdup(wire);
        w->details_id = xstrdup(wire);
        json_t *ps = json_get_arr(m->raw, "cursorModelParameters"), *p;
        json_arr_foreach(ps, i, p) json_push(w->parameters, param(jstr(p, "id"), jstr(p, "value")));
        return;
    }
    const char *effort;
    char *base = collapse_variant(wire, &effort);
    if (effort && *base && str_eq(model_class(m), "openai")) {
        if (strcmp(effort, "off") == 0) {
            w->model_id = base;
            w->details_id = xstrdup(wire);
            return;
        }
        for (size_t i = 0; i < ARRAY_LEN(EFFORT_ORDER); i++) {
            if (strcmp(effort, EFFORT_ORDER[i]) == 0) {
                w->model_id = base;
                w->details_id = xstrdup(wire);
                json_push(w->parameters, param("reasoning", effort));
                return;
            }
        }
    }
    free(base);
    w->model_id = xstrdup(wire);
    w->details_id = xstrdup(wire);
    /* catalog cursor-model-parameter: bare composer-2.5 pins the Standard tier */
    if (strcmp(wire, "composer-2.5") == 0) json_push(w->parameters, param("fast", "false"));
}

/* ======================================================================================================
 * request building
 * ==================================================================================================== */

static json_t *norm_system_prompts(const json_t *sys) {
    json_t *out = json_arr(), *s;
    json_arr_foreach(sys, i, s) {
        if (!json_is_str(s)) continue;
        char *t = str_trim(json_sv(s));
        if (*t) json_push(out, json_clone(s));
        free(t);
    }
    return out;
}

static json_t *store_json_blob(conv_t *c, const json_t *v) {
    char *s = json_dump(v, 0);
    json_t *id = store_blob(c, s, strlen(s));
    free(s);
    return id;
}

static json_t *store_msg_blob(conv_t *c, const char *msg, const json_t *val) {
    size_t len;
    char *bytes = cursor_encode(msg, val, &len);
    json_t *id = store_blob(c, bytes, len);
    free(bytes);
    return id;
}

static bool is_user_role(const json_t *m) {
    const char *r = json_get_str(m, "role");
    return str_eq(r, "user") || str_eq(r, "developer");
}

/* text items joined by "\n" (untrimmed) */
static char *content_text(const json_t *content) {
    if (json_is_str(content)) return xstrdup(json_sv(content));
    sb_t b = SB_INIT;
    json_t *it;
    bool first = true;
    json_arr_foreach(content, i, it) {
        if (!str_eq(json_get_str(it, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        first = false;
        sb_append(&b, jstr(it, "text"));
    }
    return sb_detach(&b);
}

static bool content_has_images(const json_t *content) {
    json_t *it;
    json_arr_foreach(content, i, it) if (str_eq(json_get_str(it, "type"), "image")) return true;
    return false;
}

/* UserMessage {text, messageId, selectedContext?} */
static json_t *cursor_user_message(const json_t *content, const char *text, const char *message_id) {
    json_t *um = json_obj();
    json_set_str(um, "text", text);
    char *mid = message_id ? xstrdup(message_id) : uuid_v4();
    json_set_str(um, "messageId", mid);
    free(mid);
    if (json_is_arr(content) && content_has_images(content)) {
        json_t *imgs = json_arr(), *it;
        json_arr_foreach(content, i, it) {
            if (!str_eq(json_get_str(it, "type"), "image")) continue;
            json_t *si = json_obj();
            char *u = uuid_v4();
            json_set_str(si, "uuid", u);
            free(u);
            json_set_str(si, "mimeType", jstr(it, "mimeType"));
            size_t dl = 0;
            const char *b64 = jstr(it, "data");
            unsigned char *data = base64_decode(b64, strlen(b64), &dl);
            json_set(si, "data", jbytes(data, data ? dl : 0));
            free(data);
            json_push(imgs, si);
        }
        json_set(um, "selectedContext", jobj1("selectedImages", imgs));
    }
    return um;
}

/* Vercel-AI-SDK-shaped user content for rootPromptMessagesJson */
static json_t *root_prompt_user_content(const json_t *content) {
    json_t *parts = json_arr();
    if (json_is_str(content)) {
        char *t = str_trim(json_sv(content));
        if (*t) {
            json_t *p = json_obj();
            json_set_str(p, "type", "text");
            json_set_str(p, "text", t);
            json_push(parts, p);
        }
        free(t);
        return parts;
    }
    json_t *it;
    json_arr_foreach(content, i, it) {
        if (str_eq(json_get_str(it, "type"), "text")) {
            char *t = str_trim(jstr(it, "text"));
            if (*t) {
                json_t *p = json_obj();
                json_set_str(p, "type", "text");
                json_set_str(p, "text", t);
                json_push(parts, p);
            }
            free(t);
        } else if (str_eq(json_get_str(it, "type"), "image")) {
            json_t *p = json_obj();
            json_set_str(p, "type", "image");
            json_set_str(p, "image", "");
            char *uri = xasprintf("data:%s;base64,%s", jstr(it, "mimeType"), jstr(it, "data"));
            json_set_str(p, "image", uri);
            free(uri);
            json_set_str(p, "mediaType", jstr(it, "mimeType"));
            json_push(parts, p);
        }
    }
    return parts;
}

static char *user_content_key(const json_t *content) {
    if (json_is_str(content)) return str_trim(json_sv(content));
    sb_t b = SB_INIT;
    json_t *it;
    json_arr_foreach(content, i, it) {
        const char *type = jstr(it, "type");
        sb_append(&b, type);
        if (strcmp(type, "text") == 0) sb_append(&b, jstr(it, "text"));
        else {
            sb_append(&b, jstr(it, "mimeType"));
            sb_append(&b, jstr(it, "data"));
        }
    }
    char *h = sha256_hex(b.s ? b.s : "", b.len);
    sb_free(&b);
    return h;
}

/* JSON value normalization for MCP args (drops non-finite numbers -> null) */
static json_t *normalize_mcp_arguments(const json_t *args) {
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(args, i, k, v) json_set(out, k, json_clone(v));
    return out;
}

static bool is_k3(const ai_model_t *m) { return str_eq(model_family(m), "k3"); }

static bool can_replay_thinking(const json_t *msg, const ai_model_t *target) {
    return is_k3(target) && str_eq(json_get_str(msg, "api"), "cursor-agent") &&
           str_eq(json_get_str(msg, "provider"), "cursor") && str_eq(json_get_str(msg, "model"), target->id);
}

typedef struct {
    json_t *content; /* array */
    strvec_t call_ids;
} astep_t;
typedef VEC(astep_t) astep_vec_t;

/* buildCursorAssistantSteps: split one assistant message into model rounds */
static void assistant_steps(const json_t *msg, const ai_model_t *target, astep_vec_t *steps) {
    bool replay = can_replay_thinking(msg, target);
    strvec_t issued = {0};
    astep_t st = {json_arr(), {0}};
    json_t *it;
    json_arr_foreach(json_get(msg, "content"), i, it) {
        const char *type = jstr(it, "type");
        bool is_call = strcmp(type, "toolCall") == 0;
        if (is_call) {
            if (strvec_contains(&issued, jstr(it, "id"))) continue;
            strvec_push(&issued, jstr(it, "id"));
        }
        if (!is_call && st.call_ids.n) {
            vec_push(steps, st);
            st.content = json_arr();
            memset(&st.call_ids, 0, sizeof st.call_ids);
        }
        if (strcmp(type, "text") == 0) {
            if (jstrlen(it, "text")) {
                json_t *p = json_obj();
                json_set_str(p, "type", "text");
                json_set(p, "text", json_clone(json_get(it, "text")));
                json_push(st.content, p);
            }
        } else if (strcmp(type, "thinking") == 0) {
            if (replay && jstrlen(it, "thinking")) {
                json_t *p = json_obj();
                json_set_str(p, "type", "reasoning");
                json_set(p, "text", json_clone(json_get(it, "thinking")));
                json_set(p, "providerOptions", jobj1("cursor", jobj1("modelName", json_str(jstr(msg, "model")))));
                if (jstrlen(it, "thinkingSignature")) json_set_str(p, "signature", jstr(it, "thinkingSignature"));
                json_push(st.content, p);
            }
        } else if (is_call) {
            json_t *p = json_obj();
            json_set_str(p, "type", "tool-call");
            char *nid = normalize_tool_call_id(jstr(it, "id"));
            json_set_str(p, "toolCallId", nid);
            free(nid);
            json_set_str(p, "toolName", jstr(it, "name"));
            json_t *args = json_get_obj(it, "arguments");
            json_set(p, "args", args ? normalize_mcp_arguments(args) : json_obj());
            json_push(st.content, p);
            strvec_push(&st.call_ids, jstr(it, "id"));
        }
    }
    vec_push(steps, st);
    strvec_free(&issued);
}

static char *orphan_result_text(const json_t *tr) {
    char *t = tool_result_text(tr);
    char *r = xasprintf("%s\n%s", tr_is_error(tr) ? "[Tool Error]" : "[Tool Result]", *t ? t : "(empty result)");
    free(t);
    return r;
}

static json_t *find_result(const json_t *messages, size_t end, const char *id) {
    json_t *found = NULL;
    for (size_t i = 0; i < end; i++) {
        json_t *m = json_at(messages, i);
        if (str_eq(json_get_str(m, "role"), "toolResult") && str_eq(json_get_str(m, "toolCallId"), id)) found = m;
    }
    return found;
}

static bool call_issued(const json_t *messages, size_t end, const char *id) {
    for (size_t i = 0; i < end; i++) {
        json_t *m = json_at(messages, i), *it;
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        json_arr_foreach(json_get(m, "content"), k, it) {
            if (str_eq(json_get_str(it, "type"), "toolCall") && str_eq(json_get_str(it, "id"), id)) return true;
        }
    }
    return false;
}

static void push_json_blob(conv_t *c, json_t *entries, json_t *obj /* owned */) {
    json_push(entries, store_json_blob(c, obj));
    json_free(obj);
}

static void push_tool_result_blob(conv_t *c, json_t *entries, const json_t *tr, strvec_t *emitted) {
    char *nid = normalize_tool_call_id(jstr(tr, "toolCallId"));
    json_t *item = json_obj();
    json_set_str(item, "type", "tool-result");
    json_set_str(item, "toolName", jstr(tr, "toolName"));
    json_set_str(item, "toolCallId", nid);
    json_set(item, "result", json_str_own(tool_result_text(tr)));
    if (tr_is_error(tr)) json_set_bool(item, "isError", true);
    json_t *m = json_obj();
    json_set_str(m, "role", "tool");
    json_set_str(m, "id", nid);
    json_t *c2 = json_arr();
    json_push(c2, item);
    json_set(m, "content", c2);
    push_json_blob(c, entries, m);
    strvec_push(emitted, jstr(tr, "toolCallId"));
    free(nid);
}

/* buildRootPromptMessagesJson */
static json_t *build_root_prompt(conv_t *c, const json_t *messages, const json_t *system_ids, long active_idx,
                                 const ai_model_t *target) {
    size_t end = active_idx >= 0 ? (size_t)active_idx : json_len(messages);
    json_t *entries = json_clone(system_ids);
    strvec_t emitted = {0};
    for (size_t i = 0; i < end; i++) {
        json_t *m = json_at(messages, i);
        const char *role = jstr(m, "role");
        if (is_user_role(m)) {
            json_t *content = root_prompt_user_content(json_get(m, "content"));
            if (!json_len(content)) {
                json_free(content);
                continue;
            }
            json_t *o = json_obj();
            json_set_str(o, "role", "user");
            json_set(o, "content", content);
            push_json_blob(c, entries, o);
        } else if (strcmp(role, "assistant") == 0) {
            astep_vec_t steps = {0};
            assistant_steps(m, target, &steps);
            /* drop empty steps */
            size_t kept = 0;
            for (size_t s = 0; s < steps.n; s++) {
                if (json_len(steps.v[s].content)) steps.v[kept++] = steps.v[s];
                else {
                    json_free(steps.v[s].content);
                    strvec_free(&steps.v[s].call_ids);
                }
            }
            steps.n = kept;
            for (size_t s = 0; s < steps.n; s++) {
                json_t *o = json_obj();
                json_set_str(o, "role", "assistant");
                json_set(o, "content", steps.v[s].content);
                steps.v[s].content = NULL;
                push_json_blob(c, entries, o);
                if (s + 1 < steps.n) {
                    for (size_t j = i + 1; j < end; j++) {
                        json_t *later = json_at(messages, j);
                        if (!str_eq(json_get_str(later, "role"), "toolResult")) continue;
                        const char *id = jstr(later, "toolCallId");
                        if (!strvec_contains(&steps.v[s].call_ids, id) || strvec_contains(&emitted, id)) continue;
                        json_t *r = find_result(messages, end, id);
                        if (r) push_tool_result_blob(c, entries, r, &emitted);
                    }
                }
                strvec_free(&steps.v[s].call_ids);
            }
            vec_free(&steps);
        } else if (strcmp(role, "toolResult") == 0) {
            const char *id = jstr(m, "toolCallId");
            if (strvec_contains(&emitted, id)) continue;
            if (!call_issued(messages, end, id)) {
                json_t *o = json_obj(), *ca = json_arr(), *t = json_obj();
                json_set_str(o, "role", "assistant");
                json_set_str(t, "type", "text");
                json_set(t, "text", json_str_own(orphan_result_text(m)));
                json_push(ca, t);
                json_set(o, "content", ca);
                push_json_blob(c, entries, o);
                continue;
            }
            const json_t *res = m;
            for (size_t j = i + 1; j < end && str_eq(json_get_str(json_at(messages, j), "role"), "toolResult"); j++) {
                json_t *later = json_at(messages, j);
                if (str_eq(json_get_str(later, "toolCallId"), id)) res = later;
            }
            push_tool_result_blob(c, entries, res, &emitted);
        }
    }
    strvec_free(&emitted);
    return entries;
}

static json_t *mcp_tool_result(const json_t *tr) {
    if (tr_is_error(tr)) {
        char *t = tool_result_text(tr);
        json_t *r = jobj1("error", jobj1("error", json_str_own(t)));
        return r;
    }
    json_t *content = json_arr(), *it;
    json_arr_foreach(json_get(tr, "content"), i, it) {
        if (str_eq(json_get_str(it, "type"), "text")) {
            json_push(content, jobj1("text", jobj1("text", json_str(jstr(it, "text")))));
        } else if (str_eq(json_get_str(it, "type"), "image")) {
            size_t dl = 0;
            const char *b64 = jstr(it, "data");
            unsigned char *d = base64_decode(b64, strlen(b64), &dl);
            json_t *img = json_obj();
            json_set(img, "data", jbytes(d, d ? dl : 0));
            json_set_str(img, "mimeType", jstr(it, "mimeType"));
            free(d);
            json_push(content, jobj1("image", img));
        }
    }
    return jobj1("success", jobj1("content", content));
}

static json_t *encode_mcp_args(const json_t *args) {
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(args, i, k, v) {
        sb_t b = SB_INIT;
        pb_value_encode(v, &b);
        json_set(out, k, jbytes(b.s, b.len));
        sb_free(&b);
    }
    return out;
}

static json_t *tool_call_step(const json_t *call, const json_t *result) {
    char *nid = normalize_tool_call_id(jstr(call, "id"));
    json_t *margs = json_obj();
    json_set_str(margs, "name", jstr(call, "name"));
    json_t *a = json_get_obj(call, "arguments");
    json_set(margs, "args", encode_mcp_args(a));
    json_set_str(margs, "toolCallId", nid);
    json_set_str(margs, "providerIdentifier", "pi-agent");
    json_set_str(margs, "toolName", jstr(call, "name"));
    json_t *mcp = jobj1("args", margs);
    if (result) json_set(mcp, "result", mcp_tool_result(result));
    json_t *tc = json_obj();
    json_set_str(tc, "toolCallId", nid);
    json_set(tc, "mcpToolCall", mcp);
    free(nid);
    return jobj1("toolCall", tc);
}

/* buildConversationTurns */
static json_t *build_turns(conv_t *c, const json_t *messages, long active_idx, const ai_model_t *target) {
    json_t *turns = json_arr();
    size_t n = json_len(messages), end = active_idx >= 0 ? (size_t)active_idx : n;
    size_t i = 0;
    while (i < n) {
        json_t *m = json_at(messages, i);
        if (!is_user_role(m)) {
            i++;
            continue;
        }
        if ((long)i == active_idx) break;
        char *raw = content_text(json_get(m, "content"));
        char *text = str_trim(raw);
        free(raw);
        if (!*text && !content_has_images(json_get(m, "content"))) {
            free(text);
            i++;
            continue;
        }
        char *key = user_content_key(json_get(m, "content"));
        char *seed = xasprintf("u:%zu:%s", json_len(turns), key);
        char *mid = deterministic_uuid(seed);
        json_t *um = cursor_user_message(json_get(m, "content"), text, mid);
        free(key);
        free(seed);
        free(mid);
        free(text);
        json_t *um_id = store_msg_blob(c, "UserMessage", um);
        json_free(um);
        json_t *steps = json_arr();
        i++;
        while (i < n && !is_user_role(json_at(messages, i))) {
            json_t *sm = json_at(messages, i);
            const char *role = jstr(sm, "role");
            if (strcmp(role, "assistant") == 0) {
                strvec_t issued = {0};
                json_t *it;
                json_arr_foreach(json_get(sm, "content"), k, it) {
                    const char *type = jstr(it, "type");
                    json_t *step = NULL;
                    if (strcmp(type, "text") == 0) {
                        if (!jstrlen(it, "text")) continue;
                        step = jobj1("assistantMessage", jobj1("text", json_clone(json_get(it, "text"))));
                    } else if (strcmp(type, "thinking") == 0) {
                        if (!jstrlen(it, "thinking") || !can_replay_thinking(sm, target)) continue;
                        step = jobj1("thinkingMessage", jobj1("text", json_clone(json_get(it, "thinking"))));
                    } else if (strcmp(type, "toolCall") == 0) {
                        if (strvec_contains(&issued, jstr(it, "id"))) continue;
                        strvec_push(&issued, jstr(it, "id"));
                        step = tool_call_step(it, find_result(messages, end, jstr(it, "id")));
                    } else {
                        continue;
                    }
                    json_push(steps, store_msg_blob(c, "ConversationStep", step));
                    json_free(step);
                }
                strvec_free(&issued);
            } else if (strcmp(role, "toolResult") == 0 && !call_issued(messages, end, jstr(sm, "toolCallId"))) {
                json_t *step = jobj1("assistantMessage", jobj1("text", json_str_own(orphan_result_text(sm))));
                json_push(steps, store_msg_blob(c, "ConversationStep", step));
                json_free(step);
            }
            i++;
        }
        json_t *agent_turn = json_obj();
        json_set(agent_turn, "userMessage", um_id);
        json_set(agent_turn, "steps", steps);
        json_t *turn = jobj1("agentConversationTurn", agent_turn);
        json_push(turns, store_msg_blob(c, "ConversationTurnStructure", turn));
        json_free(turn);
    }
    return turns;
}

/* K3 history must come from the same model (assertCursorKimiK3HistoryReplayable). Owned error or NULL. */
static char *check_k3_history(const json_t *messages, long active_idx, const ai_model_t *target) {
    if (!is_k3(target)) return NULL;
    size_t end = active_idx >= 0 ? (size_t)active_idx : json_len(messages);
    for (size_t i = 0; i < end; i++) {
        json_t *m = json_at(messages, i);
        if (!str_eq(json_get_str(m, "role"), "assistant")) continue;
        bool same = str_eq(json_get_str(m, "api"), "cursor-agent") && str_eq(json_get_str(m, "provider"), "cursor") &&
                    str_eq(json_get_str(m, "model"), target->id);
        if (!same)
            return xasprintf("Cursor %s cannot continue history from a different model (%s/%s); start a new session.",
                             target->id, jstr(m, "provider"), jstr(m, "model"));
    }
    return NULL;
}

/* McpToolDefinition list for requestContext (buildMcpToolDefinitions) */
static bool native_tool(const char *n) {
    static const char *const names[] = {"bash", "read", "write", "delete", "ls", "grep", "todo"};
    for (size_t i = 0; i < ARRAY_LEN(names); i++)
        if (strcmp(n, names[i]) == 0) return true;
    return false;
}

static json_t *build_mcp_tool_defs(const json_t *tools) {
    json_t *out = json_arr();
    if (!json_len(tools)) return out;
    json_t *adv = json_arr(), *t, *bash_tool = NULL;
    json_arr_foreach(tools, i, t) {
        const char *n = jstr(t, "name");
        if (strcmp(n, "bash") == 0) bash_tool = t;
        if (!native_tool(n)) json_push(adv, json_clone(t));
    }
    if (!json_len(adv)) {
        json_free(adv);
        return out;
    }
    /* staged previews are finalized only by `resolve` / `reject`, commands run through `bash` while it is on:
     * re-include the built-in bash (dropped as native) whenever pi-agent tools are advertised */
    if (bash_tool) json_push(adv, json_clone(bash_tool));
    json_arr_foreach(adv, i, t) {
        json_t *def = json_obj();
        json_set_str(def, "name", jstr(t, "name"));
        json_set_str(def, "description", jstr(t, "description"));
        json_set_str(def, "providerIdentifier", "pi-agent");
        json_set_str(def, "toolName", jstr(t, "name"));
        json_t *schema = json_get_obj(t, "parameters");
        json_t *fallback = NULL;
        if (!schema) {
            fallback = json_obj();
            json_set_str(fallback, "type", "object");
            json_set(fallback, "properties", json_obj());
            json_set(fallback, "required", json_arr());
        }
        sb_t b = SB_INIT;
        pb_value_encode(schema ? schema : fallback, &b);
        json_set(def, "inputSchema", jbytes(b.s, b.len));
        sb_free(&b);
        json_free(fallback);
        json_push(out, def);
    }
    json_free(adv);
    return out;
}

static json_t *build_rules(const json_t *sys) {
    json_t *prompts = norm_system_prompts(sys), *p;
    json_t *rules = json_arr();
    json_arr_foreach(prompts, i, p) {
        json_t *r = json_obj();
        char *path = xasprintf("/omp/system-prompt/%zu.mdc", i);
        json_set_str(r, "fullPath", path);
        free(path);
        json_set(r, "content", json_clone(p));
        json_set(r, "type", jobj1("global", json_obj()));
        json_set_int(r, "source", 2); /* CursorRuleSource.USER */
        json_push(rules, r);
    }
    json_free(prompts);
    return rules;
}

typedef struct {
    json_t *client_msg; /* AgentClientMessage {runRequest} */
    json_t *state;      /* conversation state sent (borrowed from client_msg) */
    char *fallback_wire; /* discovered-id retry candidate */
    char *err;
} built_request_t;

static void built_free(built_request_t *b) {
    json_free(b->client_msg);
    free(b->fallback_wire);
    free(b->err);
    memset(b, 0, sizeof(*b));
}

static void build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, conv_t *c,
                          const char *conversation_id, const json_t *resume_state, bool discovered,
                          built_request_t *out) {
    memset(out, 0, sizeof(*out));
    const json_t *messages = ctx->messages;
    json_t *sys = norm_system_prompts(ctx->system_prompt);
    json_t *system_ids = json_arr(), *sp;
    if (!json_len(sys)) {
        json_t *g = json_obj();
        json_set_str(g, "role", "system");
        json_set_str(g, "content", "You are a helpful assistant.");
        json_push(system_ids, store_json_blob(c, g));
        json_free(g);
    }
    json_arr_foreach(sys, i, sp) {
        json_t *g = json_obj();
        json_set_str(g, "role", "system");
        json_set(g, "content", json_clone(sp));
        json_push(system_ids, store_json_blob(c, g));
        json_free(g);
    }

    size_t n = json_len(messages);
    json_t *last = n ? json_at(messages, n - 1) : NULL;
    bool resume = resume_state != NULL;
    json_t *active = !resume && last && is_user_role(last) ? last : NULL;
    long hist_end = active ? (long)n - 1 : -1;

    json_t *action;
    char *utext = NULL;
    bool uimages = false;
    if (active) {
        json_t *content = json_get(active, "content");
        if (json_is_str(content)) utext = str_trim(json_sv(content));
        else {
            utext = content_text(content);
            uimages = content_has_images(content);
        }
    }
    char *utrim = utext ? str_trim(utext) : NULL;
    if (active && (*utrim || uimages)) {
        json_t *um = cursor_user_message(json_get(active, "content"), utext, NULL);
        action = jobj1("userMessageAction", jobj1("userMessage", um));
    } else {
        action = jobj1("resumeAction", json_obj());
    }
    free(utext);
    free(utrim);

    char *wire = resolve_wire_model_id(m, o);
    wire_model_t w;
    resolve_wire_model(m, wire, discovered, &w);
    json_t *details = json_obj();
    json_set_str(details, "modelId", w.details_id);
    json_set_str(details, "displayModelId", m->id);
    json_set_str(details, "displayName", m->name ? m->name : m->id);
    if (w.max_mode) json_set_bool(details, "maxMode", true);
    json_t *requested = json_obj();
    json_set_str(requested, "modelId", w.model_id);
    json_set_bool(requested, "maxMode", w.max_mode);
    json_set(requested, "parameters", json_clone(w.parameters));
    if (!discovered && !json_get_obj(m->raw, "cursorModelRoutes") && json_len(w.parameters) &&
        strcmp(w.model_id, wire) != 0)
        out->fallback_wire = xstrdup(wire);

    json_t *state;
    if (resume) {
        state = json_clone(resume_state);
    } else {
        out->err = check_k3_history(messages, hist_end, m);
        if (out->err) {
            json_free(sys);
            json_free(system_ids);
            json_free(action);
            json_free(details);
            json_free(requested);
            wire_model_free(&w);
            free(wire);
            return;
        }
        json_t *turns = build_turns(c, messages, hist_end, m);
        json_t *root = build_root_prompt(c, messages, system_ids, hist_end, m);
        if (json_len(root) > json_len(system_ids) && json_len(sys)) {
            json_t *prompts = norm_system_prompts(ctx->system_prompt);
            strvec_t parts = {0};
            json_t *p;
            json_arr_foreach(prompts, i, p) strvec_push(&parts, json_sv(p));
            char *joined = strvec_join(&parts, "\n\n");
            char *rules = xasprintf("<rules>\n%s\n</rules>", joined);
            json_t *re = json_obj();
            json_set_str(re, "role", "user");
            json_set_str(re, "content", rules);
            json_arr_insert(root, json_len(system_ids), store_json_blob(c, re));
            json_free(re);
            free(rules);
            free(joined);
            strvec_free(&parts);
            json_free(prompts);
        }
        json_t *cached = conv_get_state(c);
        bool matching = false;
        if (cached) {
            json_t *head = json_get_arr(cached, "rootPromptMessagesJson");
            if (json_len(head) >= json_len(system_ids)) {
                matching = true;
                for (size_t i = 0; i < json_len(system_ids); i++) {
                    json_t *a = json_at(head, i), *b = json_at(system_ids, i);
                    if (!json_is_str(a) || a->u.str.len != b->u.str.len ||
                        memcmp(a->u.str.s, b->u.str.s, b->u.str.len) != 0)
                        matching = false;
                }
            }
        }
        state = matching ? cached : json_obj();
        if (!matching) json_free(cached);
        json_set(state, "rootPromptMessagesJson", root);
        json_set(state, "turns", turns);
    }
    conv_set_state(c, state);

    json_t *run = json_obj();
    json_set(run, "conversationState", state);
    json_set(run, "action", action);
    json_set(run, "modelDetails", details);
    json_set(run, "requestedModel", requested);
    json_set_str(run, "conversationId", conversation_id);
    out->client_msg = jobj1("runRequest", run);
    out->state = state;
    json_free(sys);
    json_free(system_ids);
    wire_model_free(&w);
    free(wire);
}

/* ======================================================================================================
 * transport: RunSSE reader + BidiAppend writer
 * ==================================================================================================== */

typedef struct {
    char *data;
    size_t len;
} chunk_t;

typedef struct run {
    /* config */
    char *run_url, *append_url;
    http_headers_t headers, append_headers;
    char *request_id;
    conv_t *conv;
    /* sync */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    volatile sig_atomic_t stop; /* aborts in-flight HTTP */
    /* inbound */
    VEC(json_t *) inq;
    size_t inq_head;
    bool reader_done;
    int http_status;
    char *transport_error;
    json_t *end_error; /* Connect end-stream error object */
    bool ended;        /* end-stream frame seen: stop sending */
    /* outbound */
    VEC(chunk_t) outq;
    size_t outq_head;
    uint64_t seqno;
    bool writer_stop;
    char *append_error;
    json_t *append_error_json;
    int append_status;
    pthread_t reader, writer;
    bool reader_started, writer_started;
    connect_decoder_t dec;
} run_t;

static void run_push_out_locked(run_t *r, json_t *client_msg) {
    if (r->ended || r->writer_stop) return;
    size_t len;
    char *bytes = cursor_encode("AgentClientMessage", client_msg, &len);
    chunk_t c = {bytes, len};
    vec_push(&r->outq, c);
    pthread_cond_broadcast(&r->cv);
}

/* send one AgentClientMessage (borrowed) */
static void run_send(run_t *r, const json_t *client_msg) {
    pthread_mutex_lock(&r->mu);
    run_push_out_locked(r, (json_t *)client_msg);
    pthread_mutex_unlock(&r->mu);
}

static void handle_kv(run_t *r, const json_t *kv) {
    json_t *resp = json_obj();
    json_set_int(resp, "id", json_get_int(kv, "id", 0));
    json_t *a;
    if ((a = json_get_obj(kv, "getBlobArgs"))) {
        json_t *data = get_blob(r->conv, json_get(a, "blobId"));
        json_t *res = json_obj();
        if (data) json_set(res, "blobData", data);
        json_set(resp, "getBlobResult", res);
    } else if ((a = json_get_obj(kv, "setBlobArgs"))) {
        set_blob(r->conv, json_get(a, "blobId"), json_get(a, "blobData"));
        json_set(resp, "setBlobResult", json_obj());
    } else {
        json_free(resp);
        return;
    }
    json_t *msg = jobj1("kvClientMessage", resp);
    run_send(r, msg);
    json_free(msg);
}

/* raw unknown-field response: field `no` = Approved{} (bytes 0a 00 inside, length 2) */
static void handle_interaction_query(run_t *r, const json_t *q) {
    int64_t id = json_get_int(q, "id", 0);
    json_t *resp = json_obj();
    json_set_int(resp, "id", id);
    static const char *const approve[][2] = {{"webSearchRequestQuery", "webSearchRequestResponse"},
                                             {"exaSearchRequestQuery", "exaSearchRequestResponse"},
                                             {"exaFetchRequestQuery", "exaFetchRequestResponse"},
                                             {"webFetchRequestQuery", "webFetchRequestResponse"}};
    bool answered = false;
    for (size_t i = 0; i < ARRAY_LEN(approve); i++) {
        if (has(q, approve[i][0])) {
            json_set(resp, approve[i][1], jobj1("approved", json_obj()));
            answered = true;
        }
    }
    if (!answered && has(q, "askQuestionInteractionQuery")) {
        json_t *rej = jobj1("reason", json_str("Interactive questions are " NOT_IMPLEMENTED_SUFFIX));
        json_set(resp, "askQuestionInteractionResponse", jobj1("result", jobj1("rejected", rej)));
        answered = true;
    } else if (!answered && has(q, "switchModeRequestQuery")) {
        json_t *rej = jobj1("reason", json_str("Mode switches are " NOT_IMPLEMENTED_SUFFIX));
        json_set(resp, "switchModeRequestResponse", jobj1("rejected", rej));
        answered = true;
    } else if (!answered && has(q, "createPlanRequestQuery")) {
        json_t *e = jobj1("error", json_str("Plan files are " NOT_IMPLEMENTED_SUFFIX));
        json_set(resp, "createPlanRequestResponse", jobj1("result", jobj1("error", e)));
        answered = true;
    } else if (!answered && !has(q, "setupVmEnvironmentArgs")) {
        /* a query this schema does not name: approve the first unknown LEN field >= 2 */
        json_t *unk = json_get_arr(q, "$unknown"), *u;
        json_arr_foreach(unk, i, u) {
            int64_t no = json_get_int(u, "no", 0);
            if (json_get_int(u, "wt", -1) == 2 && no >= 2) {
                json_t *uf = json_arr(), *f = json_obj();
                json_set_int(f, "no", no);
                json_set_int(f, "wt", 2);
                json_set(f, "data", jbytes("\x02\x0a\x00", 3));
                json_push(uf, f);
                json_set(resp, "$unknown", uf);
                answered = true;
                break;
            }
        }
    }
    if (answered) {
        json_t *msg = jobj1("interactionResponse", resp);
        run_send(r, msg);
        json_free(msg);
    } else {
        json_free(resp);
    }
}

static int reader_on_headers(void *ud, int status, const http_headers_t *h) {
    run_t *r = ud;
    pthread_mutex_lock(&r->mu);
    r->http_status = status;
    pthread_mutex_unlock(&r->mu);
    return status >= 200 && status < 300 ? 0 : 1;
}

static int reader_on_data(void *ud, const char *data, size_t len) {
    run_t *r = ud;
    if (r->stop) return 1;
    connect_decoder_feed(&r->dec, data, len);
    int flags;
    const char *payload;
    size_t plen;
    int rc;
    while ((rc = connect_decoder_next(&r->dec, &flags, &payload, &plen)) == 1) {
        if (flags & CONNECT_FLAG_END_STREAM) {
            json_t *j = json_parse(payload, plen, NULL);
            json_t *err = json_is_obj(j) ? json_get_obj(j, "error") : NULL;
            pthread_mutex_lock(&r->mu);
            if (!j) {
                if (!r->transport_error) r->transport_error = xstrdup("Failed to parse Connect end stream");
                r->stop = 1;
            } else if (err) {
                if (!r->end_error) r->end_error = json_clone(err);
                r->stop = 1;
            }
            r->ended = true;
            pthread_cond_broadcast(&r->cv);
            pthread_mutex_unlock(&r->mu);
            json_free(j);
            if (r->stop) return 1;
            continue;
        }
        char *err = NULL;
        json_t *msg = cursor_decode("AgentServerMessage", payload, plen, &err);
        if (!msg) {
            LOGD("cursor: parseServerMessage: %s", err ? err : "?");
            free(err);
            continue;
        }
        json_t *v;
        if ((v = json_get_obj(msg, "kvServerMessage"))) handle_kv(r, v);
        else if ((v = json_get_obj(msg, "interactionQuery"))) handle_interaction_query(r, v);
        pthread_mutex_lock(&r->mu);
        vec_push(&r->inq, msg);
        pthread_cond_broadcast(&r->cv);
        pthread_mutex_unlock(&r->mu);
    }
    if (rc < 0) {
        pthread_mutex_lock(&r->mu);
        if (!r->transport_error) r->transport_error = xstrdup("Cursor frame exceeds the maximum size");
        r->stop = 1;
        pthread_mutex_unlock(&r->mu);
        return 1;
    }
    return 0;
}

static void *reader_main(void *p) {
    run_t *r = p;
    sb_t body = SB_INIT;
    json_t *rid = json_obj();
    json_set_str(rid, "requestId", r->request_id);
    size_t len;
    char *bytes = cursor_encode("BidiRequestId", rid, &len);
    connect_frame(&body, 0, bytes, len);
    free(bytes);
    json_free(rid);
    http_req_t req = {.method = "POST",
                      .url = r->run_url,
                      .headers = &r->headers,
                      .body = body.s,
                      .body_len = body.len,
                      .idle_timeout_ms = 300000,
                      .on_headers = reader_on_headers,
                      .on_data = reader_on_data,
                      .ud = r,
                      .cancel = &r->stop};
    http_resp_t resp;
    int rc = http_request(&req, &resp);
    pthread_mutex_lock(&r->mu);
    if (rc != 0 || resp.error) {
        if (!r->stop && !r->transport_error)
            r->transport_error = xstrdup(resp.error ? resp.error : "Cursor RunSSE connection failed");
    }
    r->reader_done = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    http_resp_free(&resp);
    sb_free(&body);
    return NULL;
}

static void writer_fail(run_t *r, char *msg, json_t *err_json, int status) {
    pthread_mutex_lock(&r->mu);
    if (!r->append_error) {
        r->append_error = msg;
        r->append_error_json = err_json;
        r->append_status = status;
        msg = NULL;
        err_json = NULL;
    }
    r->stop = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    free(msg);
    json_free(err_json);
}

static void *writer_main(void *p) {
    run_t *r = p;
    int64_t next_hb = mono_ms() + CURSOR_HEARTBEAT_MS;
    pthread_mutex_lock(&r->mu);
    for (;;) {
        while (!r->writer_stop && r->outq_head == r->outq.n) {
            int64_t now = mono_ms();
            if (now >= next_hb) {
                next_hb = now + CURSOR_HEARTBEAT_MS;
                if (!r->ended && !r->stop) {
                    json_t *hb = jobj1("clientHeartbeat", json_obj());
                    run_push_out_locked(r, hb);
                    json_free(hb);
                    continue;
                }
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t wait = next_hb - now;
            if (wait > 1000) wait = 1000;
            ts.tv_sec += wait / 1000;
            ts.tv_nsec += (wait % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&r->cv, &r->mu, &ts);
        }
        if (r->writer_stop || r->stop) break;
        chunk_t c = r->outq.v[r->outq_head++];
        uint64_t seq = r->seqno++;
        pthread_mutex_unlock(&r->mu);

        json_t *req = json_obj();
        json_set(req, "requestId", jobj1("requestId", json_str(r->request_id)));
        json_set_int(req, "appendSeqno", (int64_t)seq);
        json_set(req, "dataBinary", jbytes(c.data, c.len));
        size_t blen;
        char *body = cursor_encode("BidiAppendRequest", req, &blen);
        json_free(req);
        int64_t timeout = 60000 + (int64_t)((c.len + 128 * 1024 - 1) / (128 * 1024)) * 1000;
        http_req_t hr = {.method = "POST",
                         .url = r->append_url,
                         .headers = &r->append_headers,
                         .body = body,
                         .body_len = blen,
                         .timeout_ms = timeout,
                         .cancel = &r->stop};
        http_resp_t resp;
        int rc = http_request(&hr, &resp);
        free(body);
        free(c.data);
        if (rc != 0 || resp.error) {
            if (!r->stop) writer_fail(r, xstrdup(resp.error ? resp.error : "Cursor BidiAppend failed"), NULL, 0);
            http_resp_free(&resp);
            pthread_mutex_lock(&r->mu);
            break;
        }
        if (resp.status < 200 || resp.status >= 300) {
            json_t *ej = resp.body ? json_parse(resp.body, resp.body_len, NULL) : NULL;
            if (json_is_obj(ej)) writer_fail(r, NULL, ej, resp.status);
            else {
                json_free(ej);
                writer_fail(r, xasprintf("Cursor BidiAppend failed with HTTP %d", resp.status), NULL, resp.status);
            }
            http_resp_free(&resp);
            pthread_mutex_lock(&r->mu);
            break;
        }
        http_resp_free(&resp);
        pthread_mutex_lock(&r->mu);
    }
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static char *join_url(const char *base, const char *path) {
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') n--;
    return xasprintf("%.*s%s", (int)n, base, path);
}

static const char *const RESERVED_HEADERS[] = {"content-type", "connect-protocol-version", "te", "authorization",
                                               "x-ghost-mode", "x-cursor-client-version", "x-cursor-client-type",
                                               "x-request-id", "host", "content-length", "connection", "keep-alive",
                                               "proxy-connection", "transfer-encoding", "upgrade", "http2-settings"};

static bool reserved_header(const char *name) {
    if (name[0] == ':') return true;
    for (size_t i = 0; i < ARRAY_LEN(RESERVED_HEADERS); i++)
        if (strcasecmp(name, RESERVED_HEADERS[i]) == 0) return true;
    return false;
}

static void add_caller_headers(http_headers_t *h, const json_t *src) {
    const char *k;
    json_t *v;
    json_obj_foreach(src, i, k, v) {
        if (!json_is_str(v) || reserved_header(k)) continue;
        char *lk = str_lower(k);
        http_headers_set(h, lk, json_sv(v));
        free(lk);
    }
}

static run_t *run_start(const ai_model_t *m, const ai_stream_opts_t *o, conv_t *conv, const char *request_id,
                        const char *original_request_id, const json_t *client_msg) {
    run_t *r = xcalloc(1, sizeof(*r));
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    connect_decoder_init(&r->dec, CURSOR_MAX_FRAME);
    const char *base = m->base_url && *m->base_url ? m->base_url : CURSOR_DEFAULT_BASE_URL;
    r->run_url = join_url(base, CURSOR_RUN_SSE_PATH);
    r->append_url = join_url(base, CURSOR_BIDI_APPEND_PATH);
    r->request_id = xstrdup(request_id);
    r->conv = conv;
    for (int pass = 0; pass < 2; pass++) {
        http_headers_t *h = pass ? &r->append_headers : &r->headers;
        if (json_is_obj(m->headers)) add_caller_headers(h, m->headers);
        if (json_is_obj(o->headers)) add_caller_headers(h, o->headers);
        http_headers_set(h, "content-type", pass ? "application/proto" : "application/connect+proto");
        char *auth = xasprintf("Bearer %s", o->api_key);
        http_headers_set(h, "authorization", auth);
        free(auth);
        http_headers_set(h, "x-cursor-client-type", "cli");
        http_headers_set(h, "x-cursor-client-version", CURSOR_CLIENT_VERSION);
        http_headers_set(h, "x-ghost-mode", "true");
        http_headers_set(h, "connect-protocol-version", "1");
        http_headers_set(h, "x-request-id", request_id);
        if (original_request_id) http_headers_set(h, "x-original-request-id", original_request_id);
        http_headers_set(h, "x-cursor-streaming", "true");
    }
    /* the run request is the first append (seqno 0) */
    pthread_mutex_lock(&r->mu);
    run_push_out_locked(r, (json_t *)client_msg);
    pthread_mutex_unlock(&r->mu);
    r->reader_started = pthread_create(&r->reader, NULL, reader_main, r) == 0;
    r->writer_started = pthread_create(&r->writer, NULL, writer_main, r) == 0;
    if (!r->reader_started || !r->writer_started) {
        pthread_mutex_lock(&r->mu);
        if (!r->transport_error) r->transport_error = xstrdup("Cursor transport: cannot start threads");
        r->stop = 1;
        r->reader_done = !r->reader_started ? true : r->reader_done;
        pthread_mutex_unlock(&r->mu);
    }
    return r;
}

static void run_close(run_t *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->stop = 1;
    r->writer_stop = true;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    if (r->writer_started) pthread_join(r->writer, NULL);
    if (r->reader_started) pthread_join(r->reader, NULL);
    for (size_t i = r->inq_head; i < r->inq.n; i++) json_free(r->inq.v[i]);
    vec_free(&r->inq);
    for (size_t i = r->outq_head; i < r->outq.n; i++) free(r->outq.v[i].data);
    vec_free(&r->outq);
    http_headers_free(&r->headers);
    http_headers_free(&r->append_headers);
    connect_decoder_free(&r->dec);
    free(r->run_url);
    free(r->append_url);
    free(r->request_id);
    free(r->transport_error);
    free(r->append_error);
    json_free(r->append_error_json);
    json_free(r->end_error);
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->cv);
    free(r);
}

/* ======================================================================================================
 * stream state: the assistant message and its blocks
 * ==================================================================================================== */

typedef enum { BK_MCP = 1, BK_TODO, BK_EXEC, BK_EDIT, BK_SCM, BK_FETCH } bkind_t;

typedef struct {
    int index;
    bkind_t kind;
    char *envelope;
    sb_t partial;
    bool has_partial;
    bool resolved;
    bool open;
} tblock_t;

typedef struct {
    const ai_model_t *model;
    const ai_stream_opts_t *opts;
    ai_event_cb cb;
    void *ud;
    json_t *msg;
    int text_idx, think_idx;
    sb_t text, think;
    VEC(tblock_t *) tb;
    tblock_t *current;
    strvec_t resolved_mcp, edit_owned, paired_edit;
    bool saw_token_delta;
    int64_t t_start, t_first;
    char *cwd;
    json_t *req_tools; /* McpToolDefinition[] */
    json_t *req_rules; /* CursorRule[] */
    run_t *run;
    /* turn accounting */
    int64_t progress_version;
    bool saw_progress;
    bool saw_turn_ended;
    bool step_completed_latest;
    json_t *latest_checkpoint;
    int64_t latest_checkpoint_version;
    bool latest_checkpoint_terminal;
    /* CursorExecOrder: exec frames run one at a time in arrival order on this thread, so a call already waits for
     * the calls before it. A native StrReplace/Write spans two frames (its read, then its write once the server
     * applied the edit) and holds its place until the write lands or the server completes it: frames arriving
     * meanwhile wait here, in order, and are dropped when the run ends first. */
    char *exec_span;
    json_t *exec_waiting;
    bool quiet_frame; /* the frame being run materializes a native edit: its tool runs emit no events */
} cs_t;

static json_t *content(cs_t *s) { return json_get(s->msg, "content"); }
static json_t *block_at(cs_t *s, int idx) { return json_at(content(s), (size_t)idx); }

static void ev(cs_t *s, ai_event_type_t t, int idx, const char *d, size_t n, const json_t *blk, const char *reason) {
    if (!s->cb) return;
    ai_event_t e = {.type = t, .content_index = idx, .delta = d, .delta_len = n, .block = blk, .partial = s->msg,
                    .reason = reason};
    s->cb(s->ud, &e);
}

static void first_token(cs_t *s) {
    if (!s->t_first) s->t_first = mono_ms();
}

static void end_text(cs_t *s) {
    if (s->text_idx < 0) return;
    int idx = s->text_idx;
    s->text_idx = -1;
    ev(s, AI_EV_TEXT_END, idx, NULL, 0, block_at(s, idx), NULL);
}

static void end_thinking(cs_t *s) {
    if (s->think_idx < 0) return;
    int idx = s->think_idx;
    s->think_idx = -1;
    ev(s, AI_EV_THINKING_END, idx, NULL, 0, block_at(s, idx), NULL);
}

static void text_delta(cs_t *s, const json_t *t) {
    first_token(s);
    const char *d = json_is_str(t) ? t->u.str.s : "";
    size_t n = json_is_str(t) ? t->u.str.len : 0;
    if (s->text_idx < 0) {
        json_t *b = json_obj();
        json_set_str(b, "type", "text");
        json_set_str(b, "text", "");
        json_push(content(s), b);
        s->text_idx = (int)json_len(content(s)) - 1;
        sb_clear(&s->text);
        ev(s, AI_EV_TEXT_START, s->text_idx, NULL, 0, NULL, NULL);
    }
    sb_appendn(&s->text, d, n);
    json_set(block_at(s, s->text_idx), "text", json_strn(s->text.s ? s->text.s : "", s->text.len));
    ev(s, AI_EV_TEXT_DELTA, s->text_idx, d, n, NULL, NULL);
}

static void thinking_delta(cs_t *s, const json_t *t) {
    first_token(s);
    const char *d = json_is_str(t) ? t->u.str.s : "";
    size_t n = json_is_str(t) ? t->u.str.len : 0;
    if (s->think_idx < 0) {
        json_t *b = json_obj();
        json_set_str(b, "type", "thinking");
        json_set_str(b, "thinking", "");
        json_push(content(s), b);
        s->think_idx = (int)json_len(content(s)) - 1;
        sb_clear(&s->think);
        ev(s, AI_EV_THINKING_START, s->think_idx, NULL, 0, NULL, NULL);
    }
    sb_appendn(&s->think, d, n);
    json_set(block_at(s, s->think_idx), "thinking", json_strn(s->think.s ? s->think.s : "", s->think.len));
    ev(s, AI_EV_THINKING_DELTA, s->think_idx, d, n, NULL, NULL);
}

static json_t *find_call_block(cs_t *s, const char *id) {
    json_t *b;
    json_arr_foreach(content(s), i, b) {
        if (str_eq(json_get_str(b, "type"), "toolCall") && str_eq(json_get_str(b, "id"), id)) return b;
    }
    return NULL;
}

static tblock_t *tblock_for_index(cs_t *s, int idx) {
    for (size_t i = 0; i < s->tb.n; i++)
        if (s->tb.v[i]->index == idx) return s->tb.v[i];
    return NULL;
}

/* push a toolCall block (args owned); returns its tracking record */
static tblock_t *push_call(cs_t *s, const char *id, const char *name, json_t *args, bkind_t kind, const char *envelope,
                           bool resolved) {
    json_t *b = json_obj();
    json_set_str(b, "type", "toolCall");
    json_set_str(b, "id", id);
    json_set_str(b, "name", name);
    json_set(b, "arguments", json_is_obj(args) ? args : json_obj());
    if (!json_is_obj(args)) json_free(args);
    json_push(content(s), b);
    tblock_t *t = xcalloc(1, sizeof(*t));
    t->index = (int)json_len(content(s)) - 1;
    t->kind = kind;
    t->envelope = envelope && *envelope ? xstrdup(envelope) : NULL;
    t->resolved = resolved;
    vec_push(&s->tb, t);
    return t;
}

static void retain_call(cs_t *s, tblock_t *t) {
    t->open = true;
    s->current = t;
}

static tblock_t *resolve_streamed(cs_t *s, const char *envelope) {
    if (!envelope || !*envelope) return s->current;
    for (size_t i = 0; i < s->tb.n; i++) {
        tblock_t *t = s->tb.v[i];
        if (t->open && t->envelope && strcmp(t->envelope, envelope) == 0) return t;
    }
    return s->current && !s->current->envelope ? s->current : NULL;
}

static void release_call(cs_t *s, tblock_t *t) {
    t->open = false;
    if (s->current == t) s->current = NULL;
}

static void pair_result(cs_t *s, json_t *tr) {
    const ai_cursor_exec_t *x = s->opts->cursor_exec;
    if (x && x->result) x->result(x->ud, tr);
    else json_free(tr);
}

static void pair_text(cs_t *s, const char *id, const char *name, const char *text, bool is_error) {
    pair_result(s, make_tool_result(id, name, text, is_error));
}

/* synthesizeCursorExecToolCall: completed block for an exec-channel tool; args owned */
static void synthesize_call(cs_t *s, const char *id, const char *name, json_t *args, bool executed) {
    if (find_call_block(s, id)) {
        json_free(args);
        return;
    }
    end_text(s);
    end_thinking(s);
    tblock_t *t = push_call(s, id, name, args, BK_EXEC, NULL, executed);
    ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
    ev(s, AI_EV_TOOLCALL_END, t->index, NULL, 0, block_at(s, t->index), NULL);
}

/* ---------- MCP arg decoding ---------- */

static json_t *parse_tool_args_json(const char *text, size_t n) {
    char *t = xstrndup(text, n);
    char *trim = str_trim(t);
    json_t *r = NULL;
    if (*trim) r = json_parse(trim, strlen(trim), NULL);
    if (!r && *trim) r = json_parse_partial(trim, strlen(trim));
    free(trim);
    if (!r) r = json_str(t);
    free(t);
    return r;
}

static json_t *decode_mcp_arg_value(const json_t *bytes) {
    if (!json_is_str(bytes)) return json_null();
    json_t *v = pb_value_decode(bytes->u.str.s, bytes->u.str.len);
    if (v) {
        if (json_is_str(v)) {
            const char *p = json_sv(v);
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (*p == '{' || *p == '[' || *p == '"') {
                json_t *r = parse_tool_args_json(v->u.str.s, v->u.str.len);
                json_free(v);
                return r;
            }
        }
        return v;
    }
    return parse_tool_args_json(bytes->u.str.s, bytes->u.str.len);
}

static json_t *decode_mcp_args_map(const json_t *map) {
    if (!json_is_obj(map)) return NULL;
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(map, i, k, v) json_set(out, k, decode_mcp_arg_value(v));
    return out;
}

/* mergeCursorMcpToolCallArgs: completion wins except where it downgrades a structured value to a string */
static json_t *merge_mcp_args(const json_t *streamed, const json_t *completion) {
    json_t *merged = json_is_obj(streamed) ? json_clone(streamed) : json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(completion, i, k, v) {
        json_t *sv = json_get(merged, k);
        if (json_is_str(v) && (json_is_obj(sv) || json_is_arr(sv))) continue;
        json_set(merged, k, json_clone(v));
    }
    return merged;
}

/* ---------- todo ---------- */

static const char *todo_status(int64_t st) {
    switch (st) {
    case 2: return "in_progress";
    case 3: return "completed";
    case 4: return "abandoned";
    default: return "pending";
    }
}

static json_t *map_todos(const json_t *todos) {
    json_t *out = json_arr(), *t;
    json_arr_foreach(todos, i, t) {
        json_t *o = json_obj();
        json_set_str(o, "content", jstr(t, "content"));
        json_set_str(o, "status", todo_status(json_get_int(t, "status", 0)));
        json_push(out, o);
    }
    return out;
}

/* extractTodoSnapshot: owned {todos, merged} or NULL when not a usable full snapshot */
static json_t *todo_snapshot(const json_t *tc) {
    json_t *update = json_get_obj(tc, "updateTodosToolCall"), *read = json_get_obj(tc, "readTodosToolCall");
    if (read) {
        json_t *ra = json_get_obj(read, "args");
        if (json_len(json_get(ra, "statusFilter")) || json_len(json_get(ra, "idFilter"))) return NULL;
    }
    json_t *call = update ? update : read;
    if (!call) return NULL;
    json_t *res = json_get_obj(json_get_obj(call, "result"), "success");
    if (!res) return NULL;
    json_t *todos = json_get_arr(res, "todos");
    size_t n = json_len(todos);
    if (json_get_int(res, "totalCount", 0) != (int64_t)n) return NULL;
    if (read && !update && n == 0) return NULL;
    json_t *mapped = map_todos(todos), *t;
    strvec_t seen = {0};
    bool bad = false;
    json_arr_foreach(mapped, i, t) {
        const char *c = jstr(t, "content");
        if (!*c || strvec_contains(&seen, c)) bad = true;
        strvec_push(&seen, c);
    }
    strvec_free(&seen);
    strvec_t finished = {0};
    json_arr_foreach(todos, i, t) {
        const char *st = todo_status(json_get_int(t, "status", 0));
        if (*jstr(t, "id") && (strcmp(st, "completed") == 0 || strcmp(st, "abandoned") == 0))
            strvec_push(&finished, jstr(t, "id"));
    }
    json_arr_foreach(todos, i, t) {
        json_t *d;
        json_arr_foreach(json_get(t, "dependencies"), k, d) if (!strvec_contains(&finished, json_sv(d) ? json_sv(d) : "")) bad = true;
    }
    strvec_free(&finished);
    if (bad) {
        json_free(mapped);
        return NULL;
    }
    json_t *snap = json_obj();
    json_set(snap, "todos", mapped);
    json_set_bool(snap, "merged", json_get_bool(res, "wasMerge", false));
    return snap;
}

static char *todo_error(const json_t *tc) {
    json_t *update = json_get_obj(tc, "updateTodosToolCall"), *read = json_get_obj(tc, "readTodosToolCall");
    json_t *call = update ? update : read;
    json_t *e = json_get_obj(json_get_obj(call, "result"), "error");
    if (!e) return NULL;
    const char *t = jstr(e, "error");
    return xstrdup(*t ? t : "Todo operation failed");
}

static json_t *todo_display_args(const json_t *tc) {
    json_t *a = json_get_obj(json_get_obj(tc, "updateTodosToolCall"), "args");
    json_t *o = json_obj();
    json_set(o, "todos", a && json_get_arr(a, "todos") ? map_todos(json_get_arr(a, "todos")) : json_arr());
    if (a && json_get_bool(a, "merge", false)) json_set_bool(o, "merge", true);
    return o;
}

static json_t *todo_tool_result(const char *id, const json_t *snap, const char *error) {
    char *text;
    if (error) text = xstrdup(error);
    else if (snap) {
        json_t *todos = json_get(snap, "todos"), *t;
        size_t done = 0;
        json_arr_foreach(todos, i, t) if (str_eq(json_get_str(t, "status"), "completed")) done++;
        text = json_len(todos) ? xasprintf("%zu/%zu tasks completed", done, json_len(todos)) : xstrdup("No todos");
    } else {
        text = xstrdup("Todo snapshot not mirrored");
    }
    json_t *r = make_tool_result(id, "todo", text, error != NULL);
    free(text);
    return r;
}

/* ---------- edit (native StrReplace) ---------- */

static void exec_leave(cs_t *s, const char *id);
static void edit_end(cs_t *s, const json_t *tr);

static bool edit_owned(cs_t *s, const char *id) {
    if (strvec_contains(&s->edit_owned, id)) return true;
    json_t *b = find_call_block(s, id);
    return b && str_eq(json_get_str(b, "name"), "edit");
}

static tblock_t *open_or_update_edit(cs_t *s, const json_t *tc, const char *envelope) {
    json_t *edit = json_get_obj(tc, "editToolCall");
    if (!edit) return NULL;
    if (envelope && *envelope && !strvec_contains(&s->edit_owned, envelope)) strvec_push(&s->edit_owned, envelope);
    const char *tcid = jstr(tc, "toolCallId");
    if (*tcid && !strvec_contains(&s->edit_owned, tcid)) strvec_push(&s->edit_owned, tcid);
    const char *id = *tcid ? tcid : envelope;
    if (!id || !*id) return NULL;
    json_t *args = json_get_obj(edit, "args");
    json_t *next = json_obj();
    if (args && has(args, "path")) json_set_str(next, "path", jstr(args, "path"));
    if (args && has(args, "streamContent")) json_set_str(next, "stream_content", jstr(args, "streamContent"));
    json_t *existing = find_call_block(s, id);
    if (existing) {
        json_t *a = json_get_obj(existing, "arguments");
        json_merge(a, next);
        json_free(next);
        int idx = -1;
        for (size_t i = 0; i < json_len(content(s)); i++)
            if (json_at(content(s), i) == existing) idx = (int)i;
        return tblock_for_index(s, idx);
    }
    end_text(s);
    end_thinking(s);
    tblock_t *t = push_call(s, id, "edit", next, BK_EDIT, envelope, true);
    retain_call(s, t);
    ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
    return t;
}

static void describe_edit_result(const json_t *tc, char **text, bool *is_error) {
    json_t *res = json_get_obj(json_get_obj(tc, "editToolCall"), "result");
    json_t *v;
    *is_error = true;
    if ((v = json_get_obj(res, "success"))) {
        *is_error = false;
        const char *m = jstr(v, "message");
        *text = xstrdup(*m ? m : *jstr(v, "path") ? jstr(v, "path") : "Edited");
    } else if ((v = json_get_obj(res, "error"))) {
        const char *m = jstr(v, "modelVisibleError");
        *text = xstrdup(*m ? m : *jstr(v, "error") ? jstr(v, "error") : "Edit failed");
    } else if ((v = json_get_obj(res, "rejected"))) {
        *text = xstrdup(*jstr(v, "reason") ? jstr(v, "reason") : "Edit rejected");
    } else if ((v = json_get_obj(res, "fileNotFound"))) {
        *text = *jstr(v, "path") ? xasprintf("File not found: %s", jstr(v, "path")) : xstrdup("File not found");
    } else if ((v = json_get_obj(res, "readPermissionDenied"))) {
        *text = *jstr(v, "path") ? xasprintf("Read permission denied: %s", jstr(v, "path"))
                                 : xstrdup("Read permission denied");
    } else if ((v = json_get_obj(res, "writePermissionDenied"))) {
        *text = *jstr(v, "error")  ? xstrdup(jstr(v, "error"))
                : *jstr(v, "path") ? xasprintf("Write permission denied: %s", jstr(v, "path"))
                                   : xstrdup("Write permission denied");
    } else {
        *text = xstrdup("Edit reported no result");
    }
}

/* ======================================================================================================
 * interaction updates
 * ==================================================================================================== */

static bool exec_owned_case(const json_t *tc) {
    static const char *const cases[] = {"piReadToolCall", "piBashToolCall", "piEditToolCall", "piWriteToolCall",
                                        "piGrepToolCall", "piFindToolCall", "piLsToolCall", "listMcpResourcesToolCall",
                                        "readMcpResourceToolCall"};
    for (size_t i = 0; i < ARRAY_LEN(cases); i++)
        if (has(tc, cases[i])) return true;
    return false;
}

static void tool_call_started(cs_t *s, const json_t *u) {
    const char *envelope = jstr(u, "callId");
    json_t *tc = json_get_obj(u, "toolCall");
    json_t *scm = tc ? json_get_obj(tc, "connectScmToolCall") : NULL;
    end_text(s);
    end_thinking(s);
    if (scm) {
        json_t *a = json_get_obj(scm, "args");
        json_t *repo = json_get_obj(json_get_obj(a, "github"), "repository");
        const char *id = *jstr(a, "toolCallId") ? jstr(a, "toolCallId") : envelope;
        char *gen = *id ? NULL : uuid_v4();
        json_t *args = json_obj();
        if (repo) {
            json_set_str(args, "owner", jstr(repo, "owner"));
            json_set_str(args, "repo", jstr(repo, "repo"));
        }
        tblock_t *t = push_call(s, gen ? gen : id, "connect_scm", args, BK_SCM, envelope, true);
        free(gen);
        retain_call(s, t);
        ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
        return;
    }
    if (!tc || exec_owned_case(tc)) return;
    json_t *mcp = json_get_obj(tc, "mcpToolCall");
    if (mcp) {
        json_t *args = json_get_obj(mcp, "args");
        const char *aid = jstr(args, "toolCallId");
        char *gen = *aid ? NULL : uuid_v4();
        const char *id = gen ? gen : aid;
        for (size_t i = 0; i < s->resolved_mcp.n; i++) {
            if (strcmp(s->resolved_mcp.v[i], id) == 0) {
                free(s->resolved_mcp.v[i]);
                s->resolved_mcp.v[i] = s->resolved_mcp.v[--s->resolved_mcp.n];
                break;
            }
        }
        if (find_call_block(s, id)) {
            free(gen);
            return;
        }
        const char *name = *jstr(args, "toolName") ? jstr(args, "toolName") : jstr(args, "name");
        json_t *decoded = decode_mcp_args_map(json_get(args, "args"));
        tblock_t *t = push_call(s, id, name, decoded ? decoded : json_obj(), BK_MCP, envelope, false);
        free(gen);
        retain_call(s, t);
        ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
        return;
    }
    if (has(tc, "updateTodosToolCall") || has(tc, "readTodosToolCall")) {
        char *gen = *envelope ? NULL : uuid_v4();
        tblock_t *t = push_call(s, gen ? gen : envelope, "todo", todo_display_args(tc), BK_TODO, envelope, true);
        free(gen);
        retain_call(s, t);
        ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
        return;
    }
    json_t *fetch = json_get_obj(tc, "fetchToolCall");
    if (!fetch) fetch = json_get_obj(tc, "webFetchToolCall");
    if (fetch) {
        json_t *a = json_get_obj(fetch, "args");
        const char *url = jstr(a, "url");
        const char *id = *jstr(a, "toolCallId") ? jstr(a, "toolCallId") : envelope;
        char *gen = *id ? NULL : uuid_v4();
        json_t *args = json_obj();
        if (*url) json_set_str(args, "url", url);
        tblock_t *t = push_call(s, gen ? gen : id, "web_fetch", args, BK_FETCH, envelope, true);
        free(gen);
        retain_call(s, t);
        ev(s, AI_EV_TOOLCALL_START, t->index, NULL, 0, NULL, NULL);
        return;
    }
    open_or_update_edit(s, tc, envelope);
}

static void tool_call_delta(cs_t *s, const json_t *u, bool partial) {
    const char *envelope = jstr(u, "callId");
    if (partial && json_get_obj(u, "toolCall")) open_or_update_edit(s, json_get_obj(u, "toolCall"), envelope);
    json_t *ed = json_get_obj(json_get_obj(u, "toolCallDelta"), "editToolCallDelta");
    const char *edit_delta = ed ? json_get_str(ed, "streamContentDelta") : NULL;
    if (edit_delta && *edit_delta) {
        tblock_t *t = resolve_streamed(s, envelope);
        if (t && t->kind == BK_EDIT) {
            json_t *a = json_get_obj(block_at(s, t->index), "arguments");
            char *cur = xasprintf("%s%s", json_get_str(a, "stream_content") ? json_get_str(a, "stream_content") : "",
                                  edit_delta);
            json_set(a, "stream_content", json_str_own(cur));
            ev(s, AI_EV_TOOLCALL_DELTA, t->index, edit_delta, strlen(edit_delta), NULL, NULL);
            return;
        }
    }
    tblock_t *t = resolve_streamed(s, envelope);
    if (!t || t->kind != BK_MCP) return;
    json_t *snap = json_get(u, "argsTextDelta");
    const char *sn = json_is_str(snap) ? snap->u.str.s : "";
    size_t sl = json_is_str(snap) ? snap->u.str.len : 0;
    const char *chunk = sn;
    size_t cl = sl;
    if (sl >= t->partial.len && (t->partial.len == 0 || memcmp(sn, t->partial.s, t->partial.len) == 0)) {
        chunk = sn + t->partial.len;
        cl = sl - t->partial.len;
    }
    if (!cl) return;
    sb_appendn(&t->partial, chunk, cl);
    t->has_partial = true;
    json_t *parsed = json_parse_partial(t->partial.s, t->partial.len);
    if (json_is_obj(parsed)) json_set(block_at(s, t->index), "arguments", parsed);
    else json_free(parsed);
    ev(s, AI_EV_TOOLCALL_DELTA, t->index, chunk, cl, NULL, NULL);
}

static void tool_call_completed(cs_t *s, const json_t *u) {
    tblock_t *t = resolve_streamed(s, jstr(u, "callId"));
    if (!t) return;
    json_t *tc = json_get_obj(u, "toolCall");
    json_t *blk = block_at(s, t->index);
    const char *id = jstr(blk, "id");
    if (t->kind == BK_MCP) {
        json_t *streamed = NULL;
        bool parse_error = false;
        if (t->has_partial && t->partial.len) {
            streamed = json_parse(t->partial.s, t->partial.len, NULL);
            if (!json_is_obj(streamed)) {
                json_free(streamed);
                streamed = NULL;
                parse_error = true;
            }
        } else {
            streamed = json_clone(json_get(blk, "arguments"));
        }
        json_t *mcp = tc ? json_get_obj(tc, "mcpToolCall") : NULL;
        json_t *decoded = decode_mcp_args_map(json_get(json_get_obj(mcp, "args"), "args"));
        if (!parse_error) {
            json_set(blk, "arguments", merge_mcp_args(streamed, decoded));
        } else if (decoded && json_len(decoded)) {
            json_set(blk, "arguments", json_clone(decoded));
        } else {
            json_t *pp = json_parse_partial(t->partial.s, t->partial.len);
            json_set(blk, "arguments", json_is_obj(pp) ? pp : json_obj());
            if (!json_is_obj(pp)) json_free(pp);
        }
        json_free(streamed);
        json_free(decoded);
    } else if (t->kind == BK_SCM) {
        json_t *scm = tc ? json_get_obj(tc, "connectScmToolCall") : NULL;
        json_t *repo = json_get_obj(json_get_obj(json_get_obj(scm, "args"), "github"), "repository");
        if (repo) {
            json_t *a = json_obj();
            json_set_str(a, "owner", jstr(repo, "owner"));
            json_set_str(a, "repo", jstr(repo, "repo"));
            json_set(blk, "arguments", a);
        }
        json_t *res = json_get_obj(scm, "result"), *v;
        if (json_get_obj(res, "success")) pair_text(s, id, "connect_scm", "SCM connected", false);
        else if ((v = json_get_obj(res, "error")))
            pair_text(s, id, "connect_scm", *jstr(v, "error") ? jstr(v, "error") : "SCM connection failed", true);
        else if ((v = json_get_obj(res, "rejected")))
            pair_text(s, id, "connect_scm", *jstr(v, "reason") ? jstr(v, "reason") : "SCM connection rejected", true);
        else pair_text(s, id, "connect_scm", "SCM connection reported no result", true);
    } else if (t->kind == BK_FETCH) {
        json_t *f = tc ? json_get_obj(tc, "fetchToolCall") : NULL;
        if (!f && tc) f = json_get_obj(tc, "webFetchToolCall");
        const char *url = jstr(json_get_obj(f, "args"), "url");
        if (*url) json_set(blk, "arguments", jobj1("url", json_str(url)));
        json_t *res = json_get_obj(f, "result"), *v;
        if ((v = json_get_obj(res, "success"))) {
            const char *c = jstr(v, "content");
            pair_text(s, id, "web_fetch", *c ? c : *jstr(v, "url") ? jstr(v, "url") : "Fetched", false);
        } else if ((v = json_get_obj(res, "error"))) {
            pair_text(s, id, "web_fetch", *jstr(v, "error") ? jstr(v, "error") : "Fetch failed", true);
        } else {
            pair_text(s, id, "web_fetch", "Fetch completed", false);
        }
    } else if (t->kind == BK_TODO) {
        json_t *snap = tc ? todo_snapshot(tc) : NULL;
        char *err = tc ? todo_error(tc) : NULL;
        if (snap) {
            json_t *a = json_obj();
            json_set(a, "todos", json_clone(json_get(snap, "todos")));
            json_set(a, "merged", json_clone(json_get(snap, "merged")));
            json_set(blk, "arguments", a);
        }
        pair_result(s, todo_tool_result(id, snap, err));
        json_free(snap);
        free(err);
    } else if (t->kind == BK_EDIT) {
        exec_leave(s, id);
        json_t *edit = tc ? json_get_obj(tc, "editToolCall") : NULL;
        json_t *ea = json_get_obj(edit, "args");
        if (ea) {
            json_t *a = json_get_obj(blk, "arguments");
            if (has(ea, "path")) json_set_str(a, "path", jstr(ea, "path"));
            if (has(ea, "streamContent")) json_set_str(a, "stream_content", jstr(ea, "streamContent"));
        }
        if (!strvec_contains(&s->paired_edit, id)) {
            char *text;
            bool is_error;
            describe_edit_result(tc, &text, &is_error);
            json_t *r = make_tool_result(id, "edit", text, is_error);
            edit_end(s, r); /* editSettled: a failed match or a refusal ends the live card too */
            pair_result(s, r);
            free(text);
        }
    }
    sb_clear(&t->partial);
    t->has_partial = false;
    ev(s, AI_EV_TOOLCALL_END, t->index, NULL, 0, blk, NULL);
    release_call(s, t);
}

static void apply_turn_ended_usage(cs_t *s, const json_t *te) {
    json_t *u = json_get(s->msg, "usage");
    int64_t in = json_get_int(te, "inputTokens", 0), out = json_get_int(te, "outputTokens", 0);
    int64_t cr = json_get_int(te, "cacheReadTokens", 0), cw = json_get_int(te, "cacheWriteTokens", 0);
    int64_t rs = json_get_int(te, "reasoningTokens", 0);
    if (in <= 0 && out <= 0 && cr <= 0 && cw <= 0) return;
    if (in > 0) json_set_int(u, "input", MAX(in - cr - cw, 0));
    if (out > 0) json_set_int(u, "output", out);
    if (cr > 0) json_set_int(u, "cacheRead", cr);
    if (cw > 0) json_set_int(u, "cacheWrite", cw);
    if (rs > 0) json_set_int(u, "reasoningTokens", rs);
    json_set_int(u, "totalTokens", json_get_int(u, "input", 0) + json_get_int(u, "output", 0) +
                                       json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0));
}

static void process_interaction(cs_t *s, const json_t *u) {
    json_t *v;
    if ((v = json_get_obj(u, "textDelta"))) {
        text_delta(s, json_get(v, "text"));
    } else if ((v = json_get_obj(u, "thinkingDelta"))) {
        thinking_delta(s, json_get(v, "text"));
    } else if (has(u, "thinkingCompleted")) {
        end_thinking(s);
    } else if ((v = json_get_obj(u, "toolCallStarted"))) {
        tool_call_started(s, v);
    } else if ((v = json_get_obj(u, "toolCallDelta"))) {
        tool_call_delta(s, v, false);
    } else if ((v = json_get_obj(u, "partialToolCall"))) {
        tool_call_delta(s, v, true);
    } else if ((v = json_get_obj(u, "toolCallCompleted"))) {
        tool_call_completed(s, v);
    } else if ((v = json_get_obj(u, "turnEnded"))) {
        json_set_str(s->msg, "stopReason", "stop");
        apply_turn_ended_usage(s, v);
    } else if ((v = json_get_obj(u, "tokenDelta"))) {
        s->saw_token_delta = true;
        json_t *us = json_get(s->msg, "usage");
        json_set_int(us, "output", json_get_int(us, "output", 0) + json_get_int(v, "tokens", 0));
        json_set_int(us, "totalTokens", json_get_int(us, "input", 0) + json_get_int(us, "output", 0));
    }
}

/* close every tool block still open (flushOpenToolCalls) */
static void flush_open_calls(cs_t *s) {
    for (size_t i = 0; i < s->tb.n; i++) {
        tblock_t *t = s->tb.v[i];
        if (!t->open) continue;
        json_t *blk = block_at(s, t->index);
        if (t->has_partial) {
            json_t *p = json_parse(t->partial.s ? t->partial.s : "", t->partial.len, NULL);
            if (!json_is_obj(p)) {
                json_free(p);
                p = json_parse_partial(t->partial.s ? t->partial.s : "", t->partial.len);
            }
            json_set(blk, "arguments", json_is_obj(p) ? p : json_obj());
            if (!json_is_obj(p)) json_free(p);
            sb_clear(&t->partial);
            t->has_partial = false;
        }
        if (t->kind == BK_SCM || t->kind == BK_TODO || t->kind == BK_EDIT || t->kind == BK_FETCH) {
            if (!(t->kind == BK_EDIT && strvec_contains(&s->paired_edit, jstr(blk, "id")))) {
                json_t *r = make_tool_result(jstr(blk, "id"), jstr(blk, "name"),
                                             "The connection to Cursor closed before this call completed.", true);
                /* the edit's materializing read surfaced no call, so nothing else ends its live card */
                if (t->kind == BK_EDIT) edit_end(s, r);
                pair_result(s, r);
            }
        }
        ev(s, AI_EV_TOOLCALL_END, t->index, NULL, 0, blk, NULL);
        t->open = false;
    }
    s->current = NULL;
}

/* ======================================================================================================
 * exec frames
 * ==================================================================================================== */

static void send_exec(cs_t *s, const json_t *exec, const char *result_case, json_t *value /* owned */) {
    json_t *em = json_obj();
    json_set_int(em, "id", json_get_int(exec, "id", 0));
    json_set_str(em, "execId", jstr(exec, "execId"));
    json_set(em, result_case, value);
    json_t *msg = jobj1("execClientMessage", em);
    s->progress_version++;
    run_send(s->run, msg);
    json_free(msg);
}

static void send_stream_close(cs_t *s, const json_t *exec) {
    json_t *c = jobj1("streamClose", jobj1("id", json_int(json_get_int(exec, "id", 0))));
    json_t *msg = jobj1("execClientControlMessage", c);
    s->progress_version++;
    run_send(s->run, msg);
    json_free(msg);
}

static void send_throw(cs_t *s, const json_t *exec, const char *error, const char *code) {
    json_t *t = json_obj();
    json_set_int(t, "id", json_get_int(exec, "id", 0));
    json_set_str(t, "error", error);
    if (code) json_set_str(t, "errorCode", code);
    json_t *msg = jobj1("execClientControlMessage", jobj1("throw", t));
    s->progress_version++;
    run_send(s->run, msg);
    json_free(msg);
    send_stream_close(s, exec);
}

static json_t *oneof(const char *c, json_t *v) { return jobj1(c, v); }

/* run the host tool: NULL when the host has no bridge (omp: no handler) */
static json_t *run_host(cs_t *s, const char *id, const char *tool, json_t *args /* owned */) {
    const ai_cursor_exec_t *x = s->opts->cursor_exec;
    json_t *r = NULL;
    if (x && x->run) {
        r = s->quiet_frame && x->run_quiet ? x->run_quiet(x->ud, id, tool, args) : x->run(x->ud, id, tool, args);
        if (!r) r = make_tool_result(id, tool, "Tool returned no result", true);
    }
    json_free(args);
    return r;
}

typedef json_t *(*from_tr_fn)(const json_t *tr, const json_t *ctx);
typedef json_t *(*from_reason_fn)(const char *reason, const json_t *ctx);

/* resolveExecHandler: returns the exec result (owned); *tr_out receives the tool result when asked */
static json_t *resolve_exec(cs_t *s, const char *id, const char *tool, json_t *args, bool pair, from_tr_fn from,
                           from_reason_fn rejected, const json_t *ctx, json_t **tr_out) {
    if (tr_out) *tr_out = NULL;
    const ai_cursor_exec_t *x = s->opts->cursor_exec;
    if (!x || !x->run) {
        json_free(args);
        if (pair) pair_text(s, id, tool, "Tool not available", true);
        return rejected("Tool not available", ctx);
    }
    json_t *tr = run_host(s, id, tool, args);
    json_t *res = from(tr, ctx);
    if (pair) pair_result(s, json_clone(tr));
    if (tr_out) *tr_out = tr;
    else json_free(tr);
    return res;
}

/* ---- result builders ---- */

static json_t *details_of(const json_t *tr) { return json_get_obj(tr, "details"); }

static bool tr_truncated(const json_t *tr) {
    json_t *t = json_get_obj(details_of(tr), "truncation");
    return t && json_get_bool(t, "truncated", false);
}

static int64_t read_total_lines(const json_t *tr, bool *ok) {
    json_t *d = details_of(tr);
    json_t *v = json_get(d, "totalLines");
    *ok = true;
    if (json_is_num(v)) return json_get_int(d, "totalLines", 0);
    json_t *t = json_get_obj(json_get_obj(d, "meta"), "truncation");
    v = json_get(t, "totalLines");
    if (json_is_num(v)) return json_get_int(t, "totalLines", 0);
    *ok = false;
    return 0;
}

static int64_t read_file_size(const json_t *tr, bool *ok) {
    json_t *v = json_get(details_of(tr), "fileSize");
    *ok = json_is_num(v) && v->is_int && v->u.num.i >= 0;
    return *ok ? v->u.num.i : 0;
}

static json_t *read_error(const char *path, const char *err) {
    json_t *e = json_obj();
    json_set_str(e, "path", path);
    json_set_str(e, "error", err);
    return oneof("error", e);
}

static json_t *read_rejected(const char *reason, const json_t *ctx) {
    json_t *e = json_obj();
    json_set_str(e, "path", jstr(ctx, "path"));
    json_set_str(e, "reason", reason);
    return oneof("rejected", e);
}


static bool regex_path_not_found(const char *t) {
    /* ^Path '.*' not found$ */
    size_t n = strlen(t);
    return str_starts(t, "Path '") && n >= 17 && strcmp(t + n - 11, "' not found") == 0 && !strchr(t, '\n');
}

static json_t *read_from_tr(const json_t *tr, const json_t *ctx) {
    const char *path = jstr(ctx, "path");
    char *text = tool_result_text(tr);
    json_t *r;
    if (tr_is_error(tr)) {
        if (regex_path_not_found(text)) r = oneof("fileNotFound", jobj1("path", json_str(path)));
        else r = read_error(path, *text ? text : "Read failed");
        free(text);
        return r;
    }
    bool range = json_get_bool(ctx, "rangeApplied", false), ok;
    int64_t total = read_total_lines(tr, &ok);
    if (!ok) total = range ? 0 : (int64_t)count_lines(text);
    int64_t size = read_file_size(tr, &ok);
    if (!ok) size = (int64_t)strlen(text);
    json_t *sv = json_obj();
    json_set_str(sv, "path", path);
    json_set_int(sv, "totalLines", total);
    json_set_int(sv, "fileSize", size);
    json_set_bool(sv, "truncated", tr_truncated(tr));
    json_set_bool(sv, "rangeApplied", range);
    json_set(sv, "content", json_str_own(text));
    return oneof("success", sv);
}

static json_t *ls_from_tr(const json_t *tr, const json_t *ctx) {
    const char *path = jstr(ctx, "path");
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *e = json_obj();
        json_set_str(e, "path", path);
        json_set_str(e, "error", *text ? text : "Ls failed");
        free(text);
        return oneof("error", e);
    }
    const char *root = *path ? path : ".";
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') rl--;
    json_t *dirs = json_arr(), *files = json_arr();
    strvec_t lines = str_split_lines(text, strlen(text));
    for (size_t i = 0; i < lines.n; i++) {
        char *e = str_trim(lines.v[i]);
        if (!*e || e[0] == '[') {
            free(e);
            continue;
        }
        char *cut = strstr(e, " (");
        if (cut) *cut = 0;
        size_t n = strlen(e);
        if (n && e[n - 1] == '/') {
            e[n - 1] = 0;
            json_t *d = json_obj();
            char *abs = xasprintf("%.*s/%s", (int)rl, root, e);
            json_set_str(d, "absPath", abs);
            free(abs);
            json_set(d, "childrenDirs", json_arr());
            json_set(d, "childrenFiles", json_arr());
            json_set_bool(d, "childrenWereProcessed", false);
            json_set(d, "fullSubtreeExtensionCounts", json_obj());
            json_set_int(d, "numFiles", 0);
            json_push(dirs, d);
        } else {
            json_push(files, jobj1("name", json_str(e)));
        }
        free(e);
    }
    strvec_free(&lines);
    free(text);
    json_t *rootn = json_obj();
    json_set_str(rootn, "absPath", root);
    json_set(rootn, "childrenDirs", dirs);
    size_t nf = json_len(files);
    json_set(rootn, "childrenFiles", files);
    json_set_bool(rootn, "childrenWereProcessed", true);
    json_set(rootn, "fullSubtreeExtensionCounts", json_obj());
    json_set_int(rootn, "numFiles", (int64_t)nf);
    return oneof("success", jobj1("directoryTreeRoot", rootn));
}

static json_t *path_reason(const char *key, const char *reason, const json_t *ctx) {
    json_t *e = json_obj();
    json_set_str(e, "path", jstr(ctx, "path"));
    json_set_str(e, "reason", reason);
    return oneof(key, e);
}

static json_t *path_error(const char *error, const json_t *ctx) {
    json_t *e = json_obj();
    json_set_str(e, "path", jstr(ctx, "path"));
    json_set_str(e, "error", error);
    return oneof("error", e);
}

static json_t *ls_rejected(const char *r, const json_t *ctx) { return path_reason("rejected", r, ctx); }
static json_t *write_rejected(const char *r, const json_t *ctx) { return path_reason("rejected", r, ctx); }
static json_t *delete_rejected(const char *r, const json_t *ctx) { return path_reason("rejected", r, ctx); }
static json_t *diag_rejected(const char *r, const json_t *ctx) { return path_reason("rejected", r, ctx); }

static json_t *write_from_tr(const json_t *tr, const json_t *ctx) {
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *r = path_error(*text ? text : "Write failed", ctx);
        free(text);
        return r;
    }
    free(text);
    const char *ft = jstr(ctx, "fileText");
    json_t *fb = json_get(ctx, "fileBytes");
    json_t *sv = json_obj();
    json_set_str(sv, "path", jstr(ctx, "path"));
    json_set_int(sv, "linesCreated", (int64_t)count_lines(ft));
    json_set_int(sv, "fileSize", json_is_str(fb) && fb->u.str.len ? (int64_t)fb->u.str.len : (int64_t)strlen(ft));
    if (json_get_bool(ctx, "returnFileContentAfterWrite", false)) json_set_str(sv, "fileContentAfterWrite", ft);
    return oneof("success", sv);
}

static json_t *delete_from_tr(const json_t *tr, const json_t *ctx) {
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *r = path_error(*text ? text : "Delete failed", ctx);
        free(text);
        return r;
    }
    free(text);
    bool ok;
    int64_t size = read_file_size(tr, &ok);
    json_t *sv = json_obj();
    json_set_str(sv, "path", jstr(ctx, "path"));
    json_set_str(sv, "deletedFile", jstr(ctx, "path"));
    json_set_int(sv, "fileSize", size);
    json_set_str(sv, "prevContent", "");
    return oneof("success", sv);
}

static json_t *shell_failure(const json_t *ctx, const char *error, int64_t code) {
    json_t *f = json_obj();
    json_set_str(f, "command", jstr(ctx, "command"));
    json_set_str(f, "workingDirectory", jstr(ctx, "workingDirectory"));
    json_set_int(f, "exitCode", code);
    json_set_str(f, "signal", "");
    json_set_str(f, "stdout", "");
    json_set_str(f, "stderr", error);
    json_set_int(f, "executionTime", 0);
    json_set_bool(f, "aborted", false);
    return oneof("failure", f);
}


static json_t *shell_rejected(const char *reason, const json_t *ctx) {
    json_t *f = json_obj();
    json_set_str(f, "command", jstr(ctx, "command"));
    json_set_str(f, "workingDirectory", jstr(ctx, "workingDirectory"));
    json_set_str(f, "reason", reason);
    json_set_bool(f, "isReadonly", false);
    return oneof("rejected", f);
}

static json_t *shell_from_tr(const json_t *tr, const json_t *ctx) {
    char *out = tool_result_text(tr);
    json_t *r;
    if (tr_is_error(tr)) {
        json_t *code = json_get(details_of(tr), "exitCode");
        int64_t c = json_is_num(code) && code->is_int ? code->u.num.i : 1;
        r = shell_failure(ctx, *out ? out : "Shell failed", c);
    } else {
        json_t *sv = json_obj();
        json_set_str(sv, "command", jstr(ctx, "command"));
        json_set_str(sv, "workingDirectory", jstr(ctx, "workingDirectory"));
        json_set_int(sv, "exitCode", 0);
        json_set_str(sv, "signal", "");
        json_set_str(sv, "stdout", out);
        json_set_str(sv, "stderr", "");
        json_set_int(sv, "executionTime", 0);
        r = oneof("success", sv);
    }
    free(out);
    return r;
}

static void sanitize_shell(json_t *res) {
    json_t *v = json_get_obj(res, "success");
    if (!v) v = json_get_obj(res, "failure");
    if (!v) return;
    for (int i = 0; i < 2; i++) {
        const char *k = i ? "stderr" : "stdout";
        json_t *s = json_get(v, k);
        if (json_is_str(s) && s->u.str.len) json_set(v, k, json_str_own(sanitize_text(s->u.str.s, s->u.str.len)));
    }
}

static json_t *grep_error(const char *e, const json_t *ctx) { return oneof("error", jobj1("error", json_str(e))); }

static bool is_hex_tag(const char *s, size_t n) { /* #[0-9a-f]{4,}$ */
    if (n < 5 || s[0] != '#') return false;
    for (size_t i = 1; i < n; i++)
        if (!strchr("0123456789abcdefABCDEF", s[i])) return false;
    return true;
}

static json_t *grep_from_tr(const json_t *tr, const json_t *ctx) {
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *r = grep_error(*text ? text : "Grep failed", ctx);
        free(text);
        return r;
    }
    const char *mode = *jstr(ctx, "outputMode") ? jstr(ctx, "outputMode") : "content";
    json_t *d = details_of(tr);
    bool client_trunc = json_is_bool(json_get(d, "truncated")) && json_get_bool(d, "truncated", false);
    json_t *files = json_arr(), *f;
    json_arr_foreach(json_get_arr(d, "files"), i, f) if (json_is_str(f)) json_push(files, json_clone(f));
    json_t *offset = json_get(ctx, "offset");
    json_t *uni;
    if (strcmp(mode, "files_with_matches") == 0) {
        json_t *fr = json_obj();
        size_t nf = json_len(files);
        json_set(fr, "files", files);
        json_set_int(fr, "totalFiles", (int64_t)nf);
        json_set_bool(fr, "clientTruncated", client_trunc);
        json_set_bool(fr, "ripgrepTruncated", false);
        if (offset) json_set(fr, "offsetApplied", json_clone(offset));
        uni = oneof("files", fr);
    } else if (strcmp(mode, "count") == 0) {
        json_free(files);
        json_t *counts = json_arr(), *fm;
        int64_t total = 0;
        json_arr_foreach(json_get_arr(d, "fileMatches"), i, fm) {
            if (!json_is_str(json_get(fm, "path")) || !json_is_num(json_get(fm, "count"))) continue;
            json_t *c = json_obj();
            json_set_str(c, "file", jstr(fm, "path"));
            json_set_int(c, "count", json_get_int(fm, "count", 0));
            total += json_get_int(fm, "count", 0);
            json_push(counts, c);
        }
        json_t *cr = json_obj();
        size_t nc = json_len(counts);
        json_set(cr, "counts", counts);
        json_set_int(cr, "totalFiles", (int64_t)nc);
        json_set_int(cr, "totalMatches", total);
        json_set_bool(cr, "clientTruncated", client_trunc);
        json_set_bool(cr, "ripgrepTruncated", false);
        if (offset) json_set(cr, "offsetApplied", json_clone(offset));
        uni = oneof("count", cr);
    } else {
        /* parse the grep tool's text: "# dir/" / "## file" headers, "*12:match" / " 13:context" rows */
        json_t *by_file = json_obj(); /* file -> [matches] in first-seen order */
        int64_t matched = 0;
        char *dirs[64] = {0};
        char *cur = json_len(files) == 1 ? xstrdup(json_sv(json_at(files, 0))) : NULL;
        strvec_t lines = str_split(text, "\n");
        for (size_t li = 0; li < lines.n; li++) {
            const char *line = lines.v[li];
            size_t hashes = 0;
            while (line[hashes] == '#') hashes++;
            if (hashes && line[hashes] == ' ' && line[hashes + 1]) {
                const char *name = line + hashes + 1;
                for (size_t lv = hashes; lv < 64; lv++) {
                    free(dirs[lv]);
                    dirs[lv] = NULL;
                }
                const char *parent = hashes >= 1 && hashes - 1 < 64 ? dirs[hashes - 1] : NULL;
                char *cand = parent ? xasprintf("%s/%s", parent, name) : xstrdup(name);
                size_t cl = strlen(cand);
                free(cur);
                cur = NULL;
                if (cl && cand[cl - 1] == '/') {
                    cand[cl - 1] = 0;
                    if (hashes < 64) dirs[hashes] = cand;
                    else free(cand);
                } else {
                    bool listed = false;
                    json_arr_foreach(files, k, f) if (str_eq(json_sv(f), cand)) listed = true;
                    if (!listed) {
                        char *h = strrchr(cand, '#');
                        if (h && is_hex_tag(h, strlen(h))) *h = 0;
                    }
                    cur = cand;
                }
                continue;
            }
            size_t ll = strlen(line);
            if (ll > 2 && line[0] == '[' && line[ll - 1] == ']') {
                const char *h = NULL;
                for (const char *p = line + ll - 2; p > line; p--)
                    if (*p == '#') {
                        h = p;
                        break;
                    }
                if (h && is_hex_tag(h, (size_t)(line + ll - 1 - h)) && h > line + 1) {
                    free(cur);
                    cur = json_len(files) ? xstrdup(json_sv(json_at(files, 0))) : xstrndup(line + 1, (size_t)(h - line - 1));
                    continue;
                }
            }
            if ((line[0] == '*' || line[0] == ' ') && line[1] >= '0' && line[1] <= '9' && cur) {
                const char *p = line + 1;
                int64_t num = 0;
                while (*p >= '0' && *p <= '9') num = num * 10 + (*p++ - '0');
                if (*p != ':' && *p != '|') continue;
                bool ctxline = line[0] == ' ';
                json_t *list = json_get_arr(by_file, cur);
                if (!list) {
                    list = json_arr();
                    json_set(by_file, cur, list);
                }
                json_t *mm = json_obj();
                json_set_int(mm, "lineNumber", num);
                json_set_str(mm, "content", p + 1);
                json_set_bool(mm, "contentTruncated", false);
                json_set_bool(mm, "isContextLine", ctxline);
                json_push(list, mm);
                if (!ctxline) matched++;
            }
        }
        strvec_free(&lines);
        for (int lv = 0; lv < 64; lv++) free(dirs[lv]);
        free(cur);
        json_free(files);
        json_t *matches = json_arr();
        const char *k;
        json_t *v;
        int64_t total_lines = 0;
        json_obj_foreach(by_file, i, k, v) {
            json_t *fmo = json_obj();
            json_set_str(fmo, "file", k);
            total_lines += (int64_t)json_len(v);
            json_set(fmo, "matches", json_clone(v));
            json_push(matches, fmo);
        }
        json_free(by_file);
        json_t *cr = json_obj();
        json_set(cr, "matches", matches);
        json_set_int(cr, "totalLines", total_lines);
        json_set_int(cr, "totalMatchedLines", matched);
        json_set_bool(cr, "clientTruncated", client_trunc);
        json_set_bool(cr, "ripgrepTruncated", false);
        if (offset) json_set(cr, "offsetApplied", json_clone(offset));
        uni = oneof("content", cr);
    }
    free(text);
    json_t *sv = json_obj();
    json_set_str(sv, "pattern", jstr(ctx, "pattern"));
    json_set_str(sv, "path", jstr(ctx, "path"));
    json_set_str(sv, "outputMode", mode);
    json_set(sv, "workspaceResults", jobj1(*jstr(ctx, "path") ? jstr(ctx, "path") : ".", uni));
    return oneof("success", sv);
}

static json_t *diag_from_tr(const json_t *tr, const json_t *ctx) {
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *r = oneof("error", jobj1("error", json_str(*text ? text : "Diagnostics failed")));
        free(text);
        return r;
    }
    free(text);
    json_t *sv = json_obj();
    json_set_str(sv, "path", jstr(ctx, "path"));
    json_set(sv, "diagnostics", json_arr());
    json_set_int(sv, "totalDiagnostics", 0);
    return oneof("success", sv);
}

static json_t *mcp_error(const char *e) { return oneof("error", jobj1("error", json_str(e))); }

static json_t *mcp_from_tr(const json_t *tr, const json_t *ctx) {
    if (tr_is_error(tr)) {
        char *t = tool_result_text(tr);
        json_t *r = mcp_error(*t ? t : "MCP tool failed");
        free(t);
        return r;
    }
    json_t *res = mcp_tool_result(tr); /* {success:{content}} */
    json_set_bool(json_get_obj(res, "success"), "isError", false);
    return res;
}

static json_t *mcp_not_found(const char *reason, const json_t *ctx) {
    json_t *nf = json_obj();
    json_set_str(nf, "name", jstr(ctx, "toolName"));
    json_set(nf, "availableTools", json_arr());
    return oneof("toolNotFound", nf);
}

static json_t *mcp_error_cb(const char *e, const json_t *ctx) { return mcp_error(e); }

/* ---- pi (modern CLI) result builders ---- */

static json_t *pi_truncation(const json_t *tr) {
    json_t *d = details_of(tr);
    json_t *t = json_get(d, "truncation");
    if (!t) t = json_get(json_get_obj(d, "meta"), "truncation");
    if (!json_is_obj(t)) return NULL;
    json_t *flag = json_get(t, "truncated");
    if (flag && !(json_is_bool(flag) && flag->u.b)) return NULL;
    json_t *o = json_obj();
    json_set_bool(o, "truncated", true);
    json_set_str(o, "truncatedBy", jstr(t, "truncatedBy"));
    json_set_int(o, "totalLines", json_get_int(t, "totalLines", 0));
    json_set_int(o, "outputLines", json_get_int(t, "outputLines", 0));
    json_set_int(o, "outputBytes", json_get_int(t, "outputBytes", 0));
    json_set_bool(o, "firstLineExceedsLimit", json_get_bool(t, "firstLineExceedsLimit", false));
    json_set_bool(o, "lastLinePartial", json_get_bool(t, "lastLinePartial", false));
    return o;
}

static int64_t positive_count(const json_t *v) {
    if (!json_is_num(v)) return -1;
    double d = v->is_int ? (double)v->u.num.i : v->u.num.d;
    return d > 0 ? (int64_t)d : -1;
}

static int64_t result_limit_reached(const json_t *tr) {
    json_t *d = details_of(tr);
    int64_t flat = positive_count(json_get(d, "resultLimitReached"));
    if (flat > 0) return flat;
    json_t *lim = json_get_obj(json_get_obj(json_get_obj(d, "meta"), "limits"), "resultLimit");
    return positive_count(json_get(lim, "reached"));
}

static json_t *pi_error(const char *e, const json_t *ctx) { return oneof("error", jobj1("error", json_str(e))); }
static json_t *pi_rejected(const char *r, const json_t *ctx) { return oneof("rejected", jobj1("reason", json_str(r))); }

static json_t *pi_success_output(const json_t *tr, const char *fail_text, json_t **out_success) {
    char *text = tool_result_text(tr);
    if (tr_is_error(tr)) {
        json_t *r = pi_error(*text ? text : fail_text, NULL);
        free(text);
        *out_success = NULL;
        return r;
    }
    json_t *sv = json_obj();
    json_set(sv, "output", json_str_own(text));
    *out_success = sv;
    return oneof("success", sv);
}

static json_t *pi_read_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    json_t *r = pi_success_output(tr, "Read failed", &sv);
    if (sv) {
        json_t *t = pi_truncation(tr);
        if (t) json_set(sv, "truncation", t);
    }
    return r;
}

static json_t *pi_bash_from(const json_t *tr, const json_t *ctx) {
    char *text = tool_result_text(tr);
    json_t *t = pi_truncation(tr);
    json_t *v = json_obj();
    json_t *r;
    if (tr_is_error(tr)) {
        json_set_str(v, "error", *text ? text : "Command failed");
        if (t) json_set(v, "truncation", t);
        r = oneof("error", v);
    } else {
        json_set_str(v, "output", text);
        if (t) json_set(v, "truncation", t);
        r = oneof("success", v);
    }
    free(text);
    return r;
}

static json_t *pi_edit_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    json_t *r = pi_success_output(tr, "Edit failed", &sv);
    if (sv) {
        json_t *d = details_of(tr);
        json_set_str(sv, "diff", json_get_str(d, "diff") ? json_get_str(d, "diff") : "");
        json_set_str(sv, "patch", json_get_str(d, "patch") ? json_get_str(d, "patch") : "");
        int64_t fcl = positive_count(json_get(d, "firstChangedLine"));
        if (fcl > 0) json_set_int(sv, "firstChangedLine", fcl);
    }
    return r;
}

static json_t *pi_write_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    return pi_success_output(tr, "Write failed", &sv);
}

static json_t *pi_grep_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    json_t *r = pi_success_output(tr, "Grep failed", &sv);
    if (sv) {
        json_t *d = details_of(tr);
        int64_t mlr = positive_count(json_get(d, "perFileLimitReached"));
        json_t *t = pi_truncation(tr);
        if (!t && mlr <= 0 && json_get_bool(d, "truncated", false)) {
            const char *out = jstr(sv, "output");
            t = json_obj();
            json_set_bool(t, "truncated", true);
            json_set_str(t, "truncatedBy", "matches");
            json_set_int(t, "totalLines", 0);
            json_set_int(t, "outputLines", (int64_t)count_lines(out));
            json_set_int(t, "outputBytes", (int64_t)strlen(out));
            json_set_bool(t, "firstLineExceedsLimit", false);
            json_set_bool(t, "lastLinePartial", false);
        }
        if (t) json_set(sv, "truncation", t);
        if (mlr > 0) json_set_int(sv, "matchLimitReached", mlr);
        json_set_bool(sv, "linesTruncated", json_get_bool(d, "linesTruncated", false));
    }
    return r;
}

static json_t *pi_find_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    json_t *r = pi_success_output(tr, "Find failed", &sv);
    if (sv) {
        json_t *t = pi_truncation(tr);
        if (t) json_set(sv, "truncation", t);
        int64_t l = result_limit_reached(tr);
        if (l > 0) json_set_int(sv, "resultLimitReached", l);
    }
    return r;
}

static json_t *pi_ls_from(const json_t *tr, const json_t *ctx) {
    json_t *sv;
    json_t *r = pi_success_output(tr, "Ls failed", &sv);
    if (sv) {
        json_t *t = pi_truncation(tr);
        if (t) json_set(sv, "truncation", t);
        int64_t l = result_limit_reached(tr);
        if (l > 0) json_set_int(sv, "entryLimitReached", l);
    }
    return r;
}

/* ---- shell stream ---- */

static void send_shell_event(cs_t *s, const json_t *exec, const char *c, json_t *v) {
    send_exec(s, exec, "shellStream", jobj1(c, v));
}

static void shell_stream_exit(cs_t *s, const json_t *exec, const json_t *res) {
    json_t *v;
    if ((v = json_get_obj(res, "success")) || (v = json_get_obj(res, "failure"))) {
        bool failure = has(res, "failure");
        for (int i = 0; i < 2; i++) {
            const char *k = i ? "stderr" : "stdout";
            json_t *o = json_get(v, k);
            if (json_is_str(o) && o->u.str.len)
                send_shell_event(s, exec, k, jobj1("data", json_str_own(sanitize_text(o->u.str.s, o->u.str.len))));
        }
        json_t *ex = json_obj();
        json_set_int(ex, "code", (uint32_t)json_get_int(v, "exitCode", 0));
        json_set_str(ex, "cwd", jstr(v, "workingDirectory"));
        json_set_bool(ex, "aborted", failure ? json_get_bool(v, "aborted", false) : false);
        if (failure && has(v, "abortReason")) json_set_int(ex, "abortReason", json_get_int(v, "abortReason", 0));
        send_shell_event(s, exec, "exit", ex);
    } else if ((v = json_get_obj(res, "rejected"))) {
        send_shell_event(s, exec, "rejected", json_clone(v));
        json_t *ex = json_obj();
        json_set_int(ex, "code", 1);
        json_set_str(ex, "cwd", jstr(v, "workingDirectory"));
        json_set_bool(ex, "aborted", false);
        send_shell_event(s, exec, "exit", ex);
    }
}

/* ---- exec dispatcher ---- */

static json_t *with_path(const char *path) { return jobj1("path", json_str(path)); }

/* the edit card's tool_execution_end (CursorExecHandlers editSettled / materializeEdit) */
static void edit_end(cs_t *s, const json_t *tr) {
    const ai_cursor_exec_t *x = s->opts->cursor_exec;
    if (x && x->edit_end) x->edit_end(x->ud, tr);
}

/* a BOM-stripped, LF-normalized copy (normalizeToLF(stripBom(text).text)). Owned. */
static char *edit_norm(const char *t) {
    if (!strncmp(t, "\xef\xbb\xbf", 3)) t += 3;
    sb_t b = SB_INIT;
    for (const char *p = t; *p; p++) {
        if (*p == '\r') {
            sb_putc(&b, '\n');
            if (p[1] == '\n') p++;
        } else {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

/* materializeEdit: the write reports as `edit` with the diff against the file as it was (`write` restores the
 * file's CRLF and BOM, so both sides compare normalized, as `edit` does) */
static void materialized_edit_details(json_t *tr, const char *before, const char *after, const char *abs) {
    char *b = edit_norm(before), *a = edit_norm(after);
    int64_t first = 0;
    char *diff = diff_numbered_unified(b, a, -1, &first);
    json_t *d = json_is_obj(json_get(tr, "details")) ? json_clone(json_get(tr, "details")) : json_obj();
    json_set(d, "diff", json_str_own(diff));
    if (first) json_set_int(d, "firstChangedLine", first);
    else json_del(d, "firstChangedLine");
    json_set_str(d, "path", abs);
    json_set(tr, "details", d);
    free(b);
    free(a);
}

static void handle_exec(cs_t *s, const json_t *exec) {
    json_t *a;
    if (has(exec, "requestContextArgs")) {
        json_t *rc = json_obj();
        json_set(rc, "rules", json_clone(s->req_rules));
        json_set(rc, "repositoryInfo", json_arr());
        json_set(rc, "tools", json_clone(s->req_tools));
        json_set(rc, "gitRepos", json_arr());
        json_set(rc, "projectLayouts", json_arr());
        json_set(rc, "mcpInstructions", json_arr());
        json_set(rc, "fileContents", json_obj());
        json_set(rc, "customSubagents", json_arr());
        send_exec(s, exec, "requestContextResult", oneof("success", jobj1("requestContext", rc)));
        return;
    }
    if ((a = json_get_obj(exec, "readArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        const char *path = jstr(a, "path");
        json_t *off = json_get(a, "offset"), *lim = json_get(a, "limit");
        bool owned = edit_owned(s, id);
        s->quiet_frame = owned; /* the edit's read runs without a Read card of its own */
        bool neg = off && json_get_int(a, "offset", 0) < 0;
        char *composed = neg ? cursor_raw_read_path(path) : cursor_exec_read_path(path, off, lim);
        bool range = off || lim || read_path_has_range(path);
        if (!owned) {
            json_t *args;
            if (neg) {
                args = with_path(composed);
                json_set(args, "offset", json_clone(off));
                if (lim) json_set(args, "limit", json_clone(lim));
            } else {
                args = jobj1("path", json_str_own(pi_read_display_path(path, off, lim)));
            }
            synthesize_call(s, id, "read", args, true);
        }
        json_t *ctx = with_path(path);
        json_set_bool(ctx, "rangeApplied", range);
        json_t *res, *tr = NULL;
        if (!composed) {
            /* limit 0: zero lines, no tool run */
            json_t *empty = make_tool_result(id, "read", "", false);
            res = read_from_tr(empty, ctx);
            if (!owned) pair_result(s, empty);
            else json_free(empty);
        } else {
            json_t *eargs = with_path(composed);
            if (neg) {
                json_set(eargs, "offset", json_clone(off));
                if (lim) json_set(eargs, "limit", json_clone(lim));
            }
            res = resolve_exec(s, id, "read", eargs, !owned, read_from_tr, read_rejected, ctx, &tr);
        }
        /* StrReplace writes the returned bytes back: a truncated raw read must be materialized whole */
        if (owned && !off && !lim && !read_path_has_range(path) && tr && !tr_is_error(tr) && tr_truncated(tr)) {
            char *abs = path_is_abs(path) ? xstrdup(path) : xasprintf("%s/%s", s->cwd, path);
            FILE *fp = fopen(abs, "rb");
            json_free(res);
            if (!fp) {
                res = read_error(path, "Unable to read the complete file for StrReplace");
            } else {
                sb_t b = SB_INIT;
                char buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, fp)) > 0 && b.len <= EDIT_MATERIALIZATION_MAX_BYTES)
                    sb_appendn(&b, buf, n);
                fclose(fp);
                if (b.len > EDIT_MATERIALIZATION_MAX_BYTES) {
                    char *e = xasprintf("File exceeds %d bytes; StrReplace cannot load it whole. Use a line-range "
                                        "read and a targeted edit instead.",
                                        EDIT_MATERIALIZATION_MAX_BYTES);
                    res = read_error(path, e);
                    free(e);
                } else {
                    json_t *full = make_tool_result(id, "read", b.s ? b.s : "", false);
                    json_set(full, "details", jobj1("fileSize", json_int((int64_t)b.len)));
                    json_t *ctx2 = with_path(path);
                    res = read_from_tr(full, ctx2);
                    json_free(ctx2);
                    json_free(full);
                }
                sb_free(&b);
            }
            free(abs);
        }
        json_free(tr);
        json_free(ctx);
        free(composed);
        free(gen);
        s->quiet_frame = false;
        send_exec(s, exec, "readResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "lsArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        synthesize_call(s, id, "read", with_path(jstr(a, "path")), true);
        json_t *ctx = with_path(jstr(a, "path"));
        json_t *res = resolve_exec(s, id, "read", with_path(jstr(a, "path")), true, ls_from_tr, ls_rejected, ctx, NULL);
        json_free(ctx);
        free(gen);
        send_exec(s, exec, "lsResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "grepArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        const char *pattern = jstr(a, "pattern"), *glob = jstr(a, "glob");
        char *pt = str_trim(pattern);
        bool empty = !*pt;
        free(pt);
        if (empty) {
            char *e = *glob ? xasprintf("grep pattern is required (received an empty pattern). To list files matching "
                                        "\"%s\", pass a non-empty regex (e.g. \".\") and set path to that glob, or "
                                        "use the ls/read tool instead.",
                                        glob)
                            : xstrdup("grep pattern is required (received an empty pattern).");
            send_exec(s, exec, "grepResult", grep_error(e, NULL));
            free(e);
            free(gen);
            return;
        }
        const char *p = *jstr(a, "path") ? jstr(a, "path") : ".";
        char *search = *glob ? xasprintf("%s/%s", p, glob) : xstrdup(p);
        json_t *targs = json_obj();
        json_set_str(targs, "pattern", pattern);
        json_set_str(targs, "path", search);
        if (json_get_bool(a, "caseInsensitive", false)) json_set_bool(targs, "case", false);
        int64_t off = json_get_int(a, "offset", 0);
        if (off > 0) json_set_int(targs, "skip", off);
        synthesize_call(s, id, "grep", json_clone(targs), true);
        json_t *res = resolve_exec(s, id, "grep", targs, true, grep_from_tr, grep_error, a, NULL);
        free(search);
        free(gen);
        send_exec(s, exec, "grepResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "writeArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        bool owned = edit_owned(s, id);
        json_t *fb = json_get(a, "fileBytes");
        const char *ctext = has(a, "fileText") || !json_is_str(fb) ? jstr(a, "fileText") : NULL;
        char *content_s = ctext ? xstrdup(ctext) : xstrndup(fb->u.str.s, fb->u.str.len);
        json_t *targs = with_path(jstr(a, "path"));
        json_set_str(targs, "content", content_s);
        if (!owned) synthesize_call(s, id, "write", json_clone(targs), true);
        /* materializeEdit: the file as it was, for the edit card's diff (a missing file diffs from empty) */
        char *before = NULL, *abs = NULL;
        if (owned) {
            const char *wp = jstr(a, "path");
            abs = path_is_abs(wp) ? xstrdup(wp) : xasprintf("%s/%s", s->cwd ? s->cwd : ".", wp);
            size_t n = 0;
            before = fs_read(abs, &n);
            if (!before && errno == ENOENT) before = xstrdup("");
        }
        json_t *tr = NULL;
        s->quiet_frame = owned;
        json_t *res = resolve_exec(s, id, "write", targs, false, write_from_tr, write_rejected, a, &tr);
        s->quiet_frame = false;
        if (tr) {
            if (owned) {
                json_set_str(tr, "toolName", "edit");
                if (!json_get_bool(tr, "isError", false) && before) materialized_edit_details(tr, before, content_s, abs);
                edit_end(s, tr);
            }
            pair_result(s, tr);
        } else {
            pair_text(s, id, owned ? "edit" : "write", "Tool not available", true);
        }
        if (owned) strvec_push(&s->paired_edit, id);
        free(before);
        free(abs);
        free(content_s);
        free(gen);
        send_exec(s, exec, "writeResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "deleteArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        synthesize_call(s, id, "delete", with_path(jstr(a, "path")), true);
        json_t *res = resolve_exec(s, id, "delete", with_path(jstr(a, "path")), true, delete_from_tr, delete_rejected,
                                   a, NULL);
        free(gen);
        send_exec(s, exec, "deleteResult", res);
        return;
    }
    bool shell = (a = json_get_obj(exec, "shellArgs")) != NULL;
    bool stream = !shell && (a = json_get_obj(exec, "shellStreamArgs")) != NULL;
    bool mini = !shell && !stream && (a = json_get_obj(exec, "miniSweAgentBashArgs")) != NULL;
    if (shell || stream || mini) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        json_t *ctx = json_obj();
        json_set_str(ctx, "command", jstr(a, "command"));
        json_set_str(ctx, "workingDirectory", *jstr(a, "workingDirectory") ? jstr(a, "workingDirectory") : s->cwd);
        json_t *targs = json_obj();
        json_set_str(targs, "command", jstr(a, "command"));
        if (*jstr(a, "workingDirectory")) json_set_str(targs, "cwd", jstr(a, "workingDirectory"));
        int64_t dl = shell_deadline_seconds(a);
        if (dl > 0) json_set_int(targs, "timeout", dl);
        synthesize_call(s, id, "bash", json_clone(targs), true);
        if (stream) send_shell_event(s, exec, "start", json_obj());
        json_t *res = resolve_exec(s, id, "bash", targs, true, shell_from_tr, shell_rejected, ctx, NULL);
        sanitize_shell(res);
        if (stream) {
            shell_stream_exit(s, exec, res);
            send_exec(s, exec, "shellResult", res);
            send_stream_close(s, exec);
        } else {
            send_exec(s, exec, mini ? "miniSweAgentBashResult" : "shellResult", res);
        }
        json_free(ctx);
        free(gen);
        return;
    }
    if ((a = json_get_obj(exec, "backgroundShellSpawnArgs"))) {
        json_t *rj = json_obj();
        json_set_str(rj, "command", jstr(a, "command"));
        json_set_str(rj, "workingDirectory", jstr(a, "workingDirectory"));
        json_set_str(rj, "reason", "Not implemented");
        json_set_bool(rj, "isReadonly", false);
        send_exec(s, exec, "backgroundShellSpawnResult", oneof("rejected", rj));
        return;
    }
    if (has(exec, "writeShellStdinArgs")) {
        send_exec(s, exec, "writeShellStdinResult", oneof("error", jobj1("error", json_str("Not implemented"))));
        return;
    }
    if ((a = json_get_obj(exec, "fetchArgs"))) {
        json_t *e = json_obj();
        json_set_str(e, "url", jstr(a, "url"));
        json_set_str(e, "error", "Not implemented");
        send_exec(s, exec, "fetchResult", oneof("error", e));
        return;
    }
    if ((a = json_get_obj(exec, "diagnosticsArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        json_t *targs = json_obj();
        json_set_str(targs, "action", "diagnostics");
        json_set_str(targs, "file", jstr(a, "path"));
        synthesize_call(s, id, "lsp", json_clone(targs), true);
        json_t *res = resolve_exec(s, id, "lsp", targs, true, diag_from_tr, diag_rejected, a, NULL);
        free(gen);
        send_exec(s, exec, "diagnosticsResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "mcpArgs"))) {
        const char *name = jstr(a, "name");
        const char *tool = *jstr(a, "toolName") ? jstr(a, "toolName") : name;
        const char *id = jstr(a, "toolCallId");
        json_t *margs = decode_mcp_args_map(json_get(a, "args"));
        if (!margs) margs = json_obj();
        if (json_get_bool(a, "smartModeApprovalOnly", false)) {
            /* approval probe: this client keeps no approval policy to consult */
            char *r = xasprintf("Tool \"%s\" is not approved to run without asking.", tool);
            send_exec(s, exec, "mcpResult", oneof("rejected", jobj1("reason", json_str_own(r))));
            json_free(margs);
            return;
        }
        bool handler = s->opts->cursor_exec && s->opts->cursor_exec->run;
        if (handler) {
            json_t *existing = find_call_block(s, id);
            if (existing) {
                for (size_t i = 0; i < s->tb.n; i++)
                    if (block_at(s, s->tb.v[i]->index) == existing) s->tb.v[i]->resolved = true;
            } else {
                synthesize_call(s, id, tool, json_clone(margs), true);
                strvec_push(&s->resolved_mcp, id);
            }
        }
        json_t *ctx = jobj1("toolName", json_str(tool));
        json_t *res;
        if (!handler) {
            res = mcp_not_found("Tool not available", ctx);
            json_free(margs);
        } else {
            res = resolve_exec(s, id, tool, margs, true, mcp_from_tr, mcp_error_cb, ctx, NULL);
        }
        json_free(ctx);
        send_exec(s, exec, "mcpResult", res);
        return;
    }
    if (has(exec, "listMcpResourcesExecArgs")) {
        send_exec(s, exec, "listMcpResourcesExecResult", oneof("success", jobj1("resources", json_arr())));
        return;
    }
    if ((a = json_get_obj(exec, "readMcpResourceExecArgs"))) {
        send_exec(s, exec, "readMcpResourceExecResult", oneof("notFound", jobj1("uri", json_str(jstr(a, "uri")))));
        return;
    }
    if (has(exec, "recordScreenArgs")) {
        send_exec(s, exec, "recordScreenResult", oneof("failure", jobj1("error", json_str(NOT_IMPLEMENTED))));
        return;
    }
    if (has(exec, "computerUseArgs")) {
        send_exec(s, exec, "computerUseResult", oneof("error", jobj1("error", json_str(NOT_IMPLEMENTED))));
        return;
    }
    if ((a = json_get_obj(exec, "piReadArgs"))) {
        char *id = uuid_v4();
        json_t *off = json_get(a, "offset"), *lim = json_get(a, "limit");
        synthesize_call(s, id, "read", jobj1("path", json_str_own(pi_read_display_path(jstr(a, "path"), off, lim))),
                        true);
        char *composed = pi_read_path(jstr(a, "path"), off, lim);
        json_t *res;
        if (!composed) {
            json_t *empty = make_tool_result(id, "read", "", false);
            res = pi_read_from(empty, NULL);
            pair_result(s, empty);
        } else {
            char *raw = cursor_raw_read_path(composed);
            res = resolve_exec(s, id, "read", jobj1("path", json_str_own(raw)), true, pi_read_from, pi_error, NULL,
                               NULL);
        }
        free(composed);
        free(id);
        send_exec(s, exec, "piReadResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piBashArgs"))) {
        char *id = uuid_v4();
        json_t *targs = jobj1("command", json_str(jstr(a, "command")));
        json_t *to = json_get(a, "timeout");
        if (json_is_num(to) && (to->is_int ? to->u.num.i >= 0 : to->u.num.d >= 0)) json_set(targs, "timeout", json_clone(to));
        synthesize_call(s, id, "bash", json_clone(targs), true);
        json_t *res = resolve_exec(s, id, "bash", targs, true, pi_bash_from, pi_error, NULL, NULL);
        free(id);
        send_exec(s, exec, "piBashResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piEditArgs"))) {
        char *id = uuid_v4();
        json_t *edits = json_get_arr(a, "edits");
        json_t *first = json_at(edits, 0);
        json_t *disp = with_path(jstr(a, "path"));
        json_set_str(disp, "old_string", jstr(first, "oldText"));
        json_set_str(disp, "new_string", jstr(first, "newText"));
        synthesize_call(s, id, "edit", disp, true);
        json_t *targs = with_path(jstr(a, "path"));
        if (json_len(edits) == 1) {
            json_set_str(targs, "old_string", jstr(first, "oldText"));
            json_set_str(targs, "new_string", jstr(first, "newText"));
        } else {
            json_t *list = json_arr(), *e;
            json_arr_foreach(edits, i, e) {
                json_t *o = json_obj();
                json_set_str(o, "old_string", jstr(e, "oldText"));
                json_set_str(o, "new_string", jstr(e, "newText"));
                json_push(list, o);
            }
            json_set(targs, "edits", list);
        }
        json_t *res = resolve_exec(s, id, "edit", targs, true, pi_edit_from, pi_rejected, NULL, NULL);
        free(id);
        send_exec(s, exec, "piEditResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piWriteArgs"))) {
        char *id = uuid_v4();
        json_t *targs = with_path(jstr(a, "path"));
        json_set_str(targs, "content", jstr(a, "content"));
        synthesize_call(s, id, "write", json_clone(targs), true);
        json_t *res = resolve_exec(s, id, "write", targs, true, pi_write_from, pi_rejected, NULL, NULL);
        free(id);
        send_exec(s, exec, "piWriteResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piGrepArgs"))) {
        char *id = uuid_v4();
        json_t *targs = json_obj();
        const char *pattern = jstr(a, "pattern");
        if (json_get_bool(a, "literal", false)) json_set(targs, "pattern", json_str_own(regex_escape_literal(pattern)));
        else json_set_str(targs, "pattern", pattern);
        const char *glob = jstr(a, "glob");
        json_set(targs, "path", json_str_own(*glob ? pi_join_path(jstr(a, "path"), glob)
                                                   : xstrdup(*jstr(a, "path") ? jstr(a, "path") : ".")));
        if (json_get_bool(a, "ignoreCase", false)) json_set_bool(targs, "case", false);
        json_t *disp = json_clone(targs);
        if (has(a, "context")) json_set_int(disp, "context", json_get_int(a, "context", 0));
        if (has(a, "limit")) json_set_int(disp, "limit", MAX(1, json_get_int(a, "limit", 1)));
        synthesize_call(s, id, "grep", disp, true);
        json_t *res = resolve_exec(s, id, "grep", targs, true, pi_grep_from, pi_error, NULL, NULL);
        free(id);
        send_exec(s, exec, "piGrepResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piFindArgs"))) {
        char *id = uuid_v4();
        json_t *targs = json_obj();
        json_set(targs, "path", json_str_own(pi_join_path(jstr(a, "path"), jstr(a, "pattern"))));
        if (has(a, "limit")) json_set_int(targs, "limit", MAX(1, json_get_int(a, "limit", 1)));
        synthesize_call(s, id, "glob", json_clone(targs), true);
        json_t *res = resolve_exec(s, id, "glob", targs, true, pi_find_from, pi_error, NULL, NULL);
        free(id);
        send_exec(s, exec, "piFindResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "piLsArgs"))) {
        char *id = uuid_v4();
        const char *p = *jstr(a, "path") ? jstr(a, "path") : ".";
        synthesize_call(s, id, "read", with_path(p), true);
        json_t *res = resolve_exec(s, id, "read", with_path(p), true, pi_ls_from, pi_error, NULL, NULL);
        free(id);
        send_exec(s, exec, "piLsResult", res);
        return;
    }
    if ((a = json_get_obj(exec, "redactedReadArgs"))) {
        send_exec(s, exec, "redactedReadResult",
                  read_error(jstr(a, "path"), "Secret redaction is not implemented by this client"));
        return;
    }
    if ((a = json_get_obj(exec, "mcpStateExecArgs"))) {
        json_t *wanted = json_get_arr(a, "serverIdentifiers");
        json_t *servers = json_arr();
        /* all request-context tools share the "pi-agent" provider identifier */
        bool want = !json_len(wanted);
        json_t *w;
        json_arr_foreach(wanted, i, w) if (str_eq(json_sv(w), "pi-agent")) want = true;
        if (json_len(s->req_tools) && want) {
            json_t *srv = json_obj();
            json_set_str(srv, "serverName", "pi-agent");
            json_set_str(srv, "serverIdentifier", "pi-agent");
            json_set(srv, "tools", json_clone(s->req_tools));
            json_set_str(srv, "status", "connected");
            json_push(servers, srv);
        }
        send_exec(s, exec, "mcpStateExecResult", oneof("success", jobj1("servers", servers)));
        return;
    }
    if ((a = json_get_obj(exec, "executeHookArgs"))) {
        static const char *const neutral[] = {"preCompact",         "subagentStart",      "subagentStop",
                                              "preToolUse",         "postToolUse",        "postToolUseFailure",
                                              "beforeSubmitPrompt", "afterAgentResponse", "afterAgentThought",
                                              "stop"};
        json_t *req = json_get_obj(a, "request");
        for (size_t i = 0; i < ARRAY_LEN(neutral); i++) {
            if (has(req, neutral[i])) {
                send_exec(s, exec, "executeHookResult", jobj1("response", jobj1(neutral[i], json_obj())));
                return;
            }
        }
        const char *unset = "unset";
        const char *k;
        json_t *v;
        json_obj_foreach(req, i, k, v) if (k[0] != '$') unset = k;
        char *e = xasprintf("Unsupported hook request: %s", unset);
        send_throw(s, exec, e, "unknown_hook_request");
        free(e);
        return;
    }
    if (has(exec, "subagentArgs")) {
        send_exec(s, exec, "subagentResult",
                  oneof("error", jobj1("error", json_str("Subagents are " NOT_IMPLEMENTED_SUFFIX))));
        return;
    }
    if ((a = json_get_obj(exec, "subagentAwaitArgs"))) {
        send_exec(s, exec, "subagentAwaitResult", oneof("notFound", jobj1("agentId", json_str(jstr(a, "agentId")))));
        return;
    }
    if (has(exec, "forceBackgroundShellArgs")) {
        send_exec(s, exec, "forceBackgroundShellResult", jobj1("status", json_int(2))); /* NOT_FOUND */
        return;
    }
    if (has(exec, "forceBackgroundSubagentArgs")) {
        send_exec(s, exec, "forceBackgroundSubagentResult", jobj1("status", json_int(2)));
        return;
    }
    if (has(exec, "smartModeClassifierArgs")) {
        send_exec(s, exec, "smartModeClassifierResult",
                  oneof("error", jobj1("error", json_str("Smart-mode classification is " NOT_IMPLEMENTED_SUFFIX))));
        return;
    }
    if ((a = json_get_obj(exec, "canvasDiagnosticsArgs"))) {
        json_t *e = with_path(jstr(a, "path"));
        json_set_str(e, "error", "Canvas diagnostics are " NOT_IMPLEMENTED_SUFFIX);
        send_exec(s, exec, "canvasDiagnosticsResult", oneof("error", e));
        return;
    }
    static const char *const prechecks[][2] = {{"shellAllowlistPrecheckArgs", "shellAllowlistPrecheckResult"},
                                               {"mcpAllowlistPrecheckArgs", "mcpAllowlistPrecheckResult"},
                                               {"webFetchAllowlistPrecheckArgs", "webFetchAllowlistPrecheckResult"}};
    for (size_t i = 0; i < ARRAY_LEN(prechecks); i++) {
        if (has(exec, prechecks[i][0])) {
            send_exec(s, exec, prechecks[i][1], jobj1("allowlisted", json_bool(false)));
            return;
        }
    }
    if ((a = json_get_obj(exec, "conversationSearchArgs"))) {
        char *gen = *jstr(a, "toolCallId") ? NULL : uuid_v4();
        const char *id = gen ? gen : jstr(a, "toolCallId");
        const char *err = "Conversation search is " NOT_IMPLEMENTED_SUFFIX;
        json_t *args = jobj1("query", json_str(jstr(a, "query")));
        if (has(a, "limit")) json_set_int(args, "limit", json_get_int(a, "limit", 0));
        synthesize_call(s, id, "search_conversations", args, true);
        pair_text(s, id, "search_conversations", err, true);
        send_exec(s, exec, "conversationSearchResult", oneof("error", jobj1("error", json_str(err))));
        free(gen);
        return;
    }
    if (has(exec, "agentStoreConflictArgs")) {
        send_exec(s, exec, "agentStoreConflictResult",
                  oneof("error", jobj1("error", json_str("Agent store conflicts are " NOT_IMPLEMENTED_SUFFIX))));
        return;
    }
    if (has(exec, "gitDiffRequest")) {
        send_throw(s, exec, "Git diff is " NOT_IMPLEMENTED_SUFFIX, "exec_variant_unsupported");
        return;
    }
    /* a frame this schema does not model */
    bool known = false;
    const char *k;
    json_t *v;
    json_obj_foreach(exec, i, k, v) {
        if (strcmp(k, "id") && strcmp(k, "execId") && strcmp(k, "spanContext") && strcmp(k, "$unknown") &&
            strcmp(k, "acceptHookAdditionalContexts")) {
            known = true;
            char *e = xasprintf("No handler for exec message of type %s", k);
            send_throw(s, exec, e, "exec_variant_unsupported");
            free(e);
            break;
        }
    }
    if (!known) send_throw(s, exec, "Unknown exec message variant", "unknown_exec_variant");
}

/* ======================================================================================================
 * errors
 * ==================================================================================================== */

static const struct {
    int code;
    const char *name;
    int status;
} CURSOR_ERRORS[] = {
    {0, "UNSPECIFIED", 0},
    {1, "BAD_API_KEY", 401},
    {2, "NOT_LOGGED_IN", 401},
    {3, "INVALID_AUTH_ID", 401},
    {4, "NOT_HIGH_ENOUGH_PERMISSIONS", 403},
    {5, "BAD_MODEL_NAME", 404},
    {6, "USER_NOT_FOUND", 404},
    {7, "FREE_USER_RATE_LIMIT_EXCEEDED", 429},
    {8, "PRO_USER_RATE_LIMIT_EXCEEDED", 429},
    {9, "FREE_USER_USAGE_LIMIT", 429},
    {10, "PRO_USER_USAGE_LIMIT", 429},
    {11, "AUTH_TOKEN_NOT_FOUND", 401},
    {12, "AUTH_TOKEN_EXPIRED", 401},
    {13, "OPENAI", 0},
    {14, "OPENAI_RATE_LIMIT_EXCEEDED", 429},
    {18, "AGENT_REQUIRES_LOGIN", 401},
    {20, "MAX_TOKENS", 413},
    {21, "USER_ABORTED_REQUEST", 0},
    {22, "GENERIC_RATE_LIMIT_EXCEEDED", 429},
    {23, "PRO_USER_ONLY", 403},
    {25, "TIMEOUT", 0},
    {28, "GPT_4_VISION_PREVIEW_RATE_LIMIT", 429},
    {29, "CUSTOM_MESSAGE", 0},
    {30, "OUTDATED_CLIENT", 0},
    {31, "CLAUDE_IMAGE_TOO_LARGE", 0},
    {33, "FILE_NOT_FOUND", 404},
    {34, "API_KEY_RATE_LIMIT", 429},
    {35, "DEBOUNCED", 429},
    {36, "BAD_REQUEST", 0},
    {37, "REPOSITORY_SERVICE_REPOSITORY_IS_NOT_INITIALIZED", 0},
    {38, "UNAUTHORIZED", 401},
    {39, "NOT_FOUND", 404},
    {40, "DEPRECATED", 0},
    {41, "RESOURCE_EXHAUSTED", 429},
    {42, "BAD_USER_API_KEY", 401},
    {43, "CONVERSATION_TOO_LONG", 413},
    {44, "USAGE_PRICING_REQUIRED", 429},
    {45, "USAGE_PRICING_REQUIRED_CHANGEABLE", 429},
    {46, "GITHUB_NO_USER_CREDENTIALS", 403},
    {47, "GITHUB_USER_NO_ACCESS", 403},
    {48, "GITHUB_APP_NO_ACCESS", 403},
    {49, "GITHUB_MULTIPLE_OWNERS", 403},
    {50, "RATE_LIMITED", 429},
    {51, "RATE_LIMITED_CHANGEABLE", 429},
    {52, "CUSTOM", 0},
    {53, "HOOKS_BLOCKED", 0},
    {54, "SUSPICIOUS_USAGE_BLOCKED", 403},
    {55, "EXTENSION_HOST_TIMEOUT", 0},
    {56, "NETWORK_ERROR", 0},
    {57, "PROVIDER_ERROR", 0},
    {58, "MODEL_BLOCKED", 403},
    {59, "INTERNAL", 0},
    {60, "MAX_MODE_REQUIRED", 0},
    {61, "MODEL_NO_LONGER_SUPPORTED", 404},
    {62, "PRICING_WARNING", 0},
    {63, "SLOW_POOL", 0},
    {64, "UNSUPPORTED_REGION", 403},
    {65, "ACCOUNT_CLOSED", 401},
};

static bool retryable_code(int code) {
    return code == 13 || code == 25 || code == 55 || code == 56 || code == 57 || code == 59 || code == 63;
}

typedef struct {
    char *message;      /* errorMessage shown to the user */
    char *classify;     /* classification text (Connect prefix) */
    char *code_name;    /* structured Cursor error name */
    int status;
    bool retryable;
    bool incomplete;
    bool aborted;
} cerr_t;

static void cerr_free(cerr_t *e) {
    free(e->message);
    free(e->classify);
    free(e->code_name);
    memset(e, 0, sizeof(*e));
}

static bool status_retryable(int st) { return st == 408 || st == 429 || st >= 500; }

static void classify_connect_error(const json_t *error, cerr_t *out) {
    memset(out, 0, sizeof(*out));
    json_t *d;
    json_arr_foreach(json_get_arr(error, "details"), i, d) {
        const char *type = json_get_str(d, "type");
        if (!str_eq(type, "aiserver.v1.ErrorDetails") && !str_eq(type, "type.googleapis.com/aiserver.v1.ErrorDetails"))
            continue;
        const char *value = json_get_str(d, "value");
        if (!value || !*value) continue;
        size_t bl = 0;
        unsigned char *bytes = base64_decode(value, strlen(value), &bl);
        if (!bytes) continue;
        char *err = NULL;
        json_t *ed = cursor_decode("ErrorDetails", bytes, bl, &err);
        free(bytes);
        free(err);
        if (!ed) continue;
        int code = (int)json_get_int(ed, "error", 0);
        const char *name = NULL;
        int status = 0;
        for (size_t k = 0; k < ARRAY_LEN(CURSOR_ERRORS); k++) {
            if (CURSOR_ERRORS[k].code == code) {
                name = CURSOR_ERRORS[k].name;
                status = CURSOR_ERRORS[k].status;
            }
        }
        char *gen = name ? NULL : xasprintf("ERROR_%d", code);
        json_t *det = json_get_obj(ed, "details");
        char *title = str_trim(jstr(det, "title")), *body = str_trim(jstr(det, "detail"));
        char *msg = *title && *body ? xasprintf("%s: %s", title, body) : xstrdup(*title ? title : *body ? body : name ? name : gen);
        out->code_name = xstrdup(name ? name : gen);
        out->message = xasprintf("Cursor %s: %s", out->code_name, msg);
        out->classify = xstrdup(out->message);
        json_t *rf = det ? json_get(det, "isRetryable") : NULL;
        if (code == 21) {
            out->aborted = true;
        } else if (status) {
            out->status = status;
            out->retryable = status_retryable(status) && !str_istr(out->code_name, "USAGE");
        } else if ((json_is_bool(rf) && rf->u.b) || (!json_is_bool(rf) && retryable_code(code))) {
            out->status = 503;
            out->retryable = true;
        } else {
            out->status = 400;
        }
        free(title);
        free(body);
        free(msg);
        free(gen);
        json_free(ed);
        return;
    }
    const char *code = json_get_str(error, "code");
    const char *message = json_get_str(error, "message");
    out->classify = xasprintf("Connect error %s: %s", code && *code ? code : "unknown",
                              message && *message ? message : "Unknown error");
    out->message = connect_format_end_stream_error(error);
    out->retryable = connect_has_retryable_cursor_detail(json_get(error, "details")) ||
                     str_eq(code, "unavailable") || str_eq(code, "deadline_exceeded");
}

static bool err_model_not_found(const cerr_t *e) {
    if (str_eq(e->code_name, "BAD_MODEL_NAME")) return true;
    const char *m = e->classify ? e->classify : e->message;
    return m && (strncasecmp(m, "Connect error not_found:", 24) == 0 || strncasecmp(m, "gRPC error 5:", 13) == 0);
}

static bool err_conversation_rejected(const cerr_t *e) {
    if (str_eq(e->code_name, "PROVIDER_ERROR")) return true;
    const char *m = e->message ? e->message : "";
    char *l = str_lower(m);
    bool r = false;
    for (char *p = strstr(l, "resource"); p; p = strstr(p + 1, "resource")) {
        const char *q = p + 8;
        if (*q && strncmp(q + (*q == '_' || *q == ' ' || *q == '-' ? 1 : 0), "exhausted", 9) == 0) r = true;
        if (strncmp(q, "exhausted", 9) == 0) r = true;
    }
    free(l);
    return r;
}

/* ======================================================================================================
 * one attempt
 * ==================================================================================================== */

typedef enum { ATTEMPT_OK, ATTEMPT_ERROR, ATTEMPT_ABORTED } attempt_rc_t;

/* execFrameCallId: the frame's tool call id, or the frame itself when it carries none. Owned. */
static char *exec_frame_call_id(const json_t *exec) {
    for (size_t i = 0; i < exec->u.obj.n; i++) {
        const char *k = exec->u.obj.keys[i];
        const json_t *v = exec->u.obj.vals[i];
        if (!json_is_obj(v) || !str_ends(k, "Args")) continue;
        const char *tc = json_get_str(v, "toolCallId");
        if (tc && *tc) return xstrdup(tc);
        break;
    }
    char *id = json_get(exec, "id") ? json_dump(json_get(exec, "id"), 0) : xstrdup("");
    const char *eid = json_get_str(exec, "execId");
    char *r = xasprintf("exec:%s:%s", id, eid ? eid : "");
    free(id);
    return r;
}

static void exec_run_frame(cs_t *s, const json_t *exec) {
    if (has(exec, "requestContextArgs")) {
        handle_exec(s, exec);
        return;
    }
    char *id = exec_frame_call_id(exec);
    bool edit_read = has(exec, "readArgs") && edit_owned(s, id);
    handle_exec(s, exec);
    /* an edit keeps its place past its read: its write follows, or the server completes it */
    if (edit_read && !s->exec_span) s->exec_span = id;
    else free(id);
}

/* CursorExecOrder.leave: the call gives up its place; frames waiting on it run in order until one holds a place */
static void exec_leave(cs_t *s, const char *id) {
    if (!s->exec_span || !str_eq(s->exec_span, id)) return;
    free(s->exec_span);
    s->exec_span = NULL;
    while (!s->exec_span && json_len(s->exec_waiting)) {
        json_t *f = json_arr_remove(s->exec_waiting, 0);
        exec_run_frame(s, f);
        json_free(f);
    }
}

static void handle_exec_ordered(cs_t *s, const json_t *exec) {
    if (has(exec, "requestContextArgs")) {
        handle_exec(s, exec);
        return;
    }
    char *id = exec_frame_call_id(exec);
    if (s->exec_span && !str_eq(s->exec_span, id)) {
        if (!s->exec_waiting) s->exec_waiting = json_arr();
        json_push(s->exec_waiting, json_clone(exec));
        free(id);
        return;
    }
    bool held = s->exec_span != NULL; /* the edit's write frame proceeds at once */
    exec_run_frame(s, exec);
    if (held) exec_leave(s, id);
    free(id);
}

static void handle_server_message(cs_t *s, const json_t *msg) {
    json_t *iu = json_get_obj(msg, "interactionUpdate");
    bool heartbeat = iu && has(iu, "heartbeat");
    if (!heartbeat) {
        s->saw_progress = true;
        s->progress_version++;
    }
    if (iu && has(iu, "stepCompleted")) s->step_completed_latest = true;
    else if (!heartbeat && !has(msg, "conversationCheckpointUpdate") && !has(msg, "kvServerMessage"))
        s->step_completed_latest = false;
    json_t *v;
    if (iu) {
        process_interaction(s, iu);
        if (has(iu, "turnEnded")) s->saw_turn_ended = true;
    } else if ((v = json_get_obj(msg, "execServerMessage"))) {
        handle_exec_ordered(s, v);
    } else if ((v = json_get_obj(msg, "conversationCheckpointUpdate"))) {
        json_free(s->latest_checkpoint);
        s->latest_checkpoint = json_clone(v);
        s->latest_checkpoint_version = s->progress_version;
        s->latest_checkpoint_terminal = s->saw_turn_ended;
        conv_set_state(s->run->conv, v);
        int64_t used = json_get_int(json_get_obj(v, "tokenDetails"), "usedTokens", 0);
        if (used > 0) json_set_int(json_get(s->msg, "usage"), "contextTokens", used);
    }
    /* kvServerMessage / interactionQuery were answered by the reader thread */
}

static bool open_calls_left(cs_t *s) {
    for (size_t i = 0; i < s->tb.n; i++)
        if (s->tb.v[i]->open) return true;
    return false;
}

static attempt_rc_t run_attempt(cs_t *s, run_t *r, cerr_t *err) {
    memset(err, 0, sizeof(*err));
    const ai_stream_opts_t *o = s->opts;
    for (;;) {
        pthread_mutex_lock(&r->mu);
        while (r->inq_head == r->inq.n && !r->reader_done && !(o->cancel && *o->cancel)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&r->cv, &r->mu, &ts);
        }
        if (o->cancel && *o->cancel) {
            r->stop = 1;
            pthread_cond_broadcast(&r->cv);
            pthread_mutex_unlock(&r->mu);
            return ATTEMPT_ABORTED;
        }
        if (r->inq_head < r->inq.n) {
            json_t *m = r->inq.v[r->inq_head];
            r->inq.v[r->inq_head++] = NULL;
            pthread_mutex_unlock(&r->mu);
            handle_server_message(s, m);
            json_free(m);
            continue;
        }
        /* reader done and queue drained */
        pthread_mutex_unlock(&r->mu);
        break;
    }
    pthread_mutex_lock(&r->mu);
    bool terminal_ok = s->saw_turn_ended && s->latest_checkpoint && s->latest_checkpoint_terminal;
    attempt_rc_t rc = ATTEMPT_OK;
    if (r->append_error || r->append_error_json) {
        if (r->append_error_json) classify_connect_error(r->append_error_json, err);
        else {
            err->message = xstrdup(r->append_error);
            err->status = r->append_status;
            err->retryable = r->append_status == 0 || status_retryable(r->append_status);
        }
        rc = ATTEMPT_ERROR;
    } else if (r->end_error) {
        if (!terminal_ok) {
            classify_connect_error(r->end_error, err);
            rc = ATTEMPT_ERROR;
        }
    } else if (r->http_status && (r->http_status < 200 || r->http_status >= 300)) {
        err->message = xasprintf("Cursor RunSSE failed with HTTP %d", r->http_status);
        err->status = r->http_status;
        err->retryable = status_retryable(r->http_status);
        rc = ATTEMPT_ERROR;
    } else if (r->transport_error) {
        if (!terminal_ok) {
            err->message = xstrdup(r->transport_error);
            err->retryable = true;
            rc = ATTEMPT_ERROR;
        }
    } else if (!s->saw_turn_ended) {
        json_t *c = content(s);
        json_t *lastb = json_len(c) ? json_at(c, json_len(c) - 1) : NULL;
        if (!(s->step_completed_latest && str_eq(json_get_str(lastb, "type"), "text") && !open_calls_left(s))) {
            err->message = xstrdup("Cursor stream ended before turnEnded");
            err->incomplete = true;
            err->retryable = true;
            rc = ATTEMPT_ERROR;
        }
    }
    pthread_mutex_unlock(&r->mu);
    if (err->aborted) rc = ATTEMPT_ABORTED;
    return rc;
}

/* ======================================================================================================
 * entry point
 * ==================================================================================================== */

static char *conversation_base_id(const ai_stream_opts_t *o, bool *ephemeral) {
    *ephemeral = !o->session_id || !*o->session_id;
    return *ephemeral ? uuid_v4() : xstrdup(o->session_id);
}

static json_t *finish(cs_t *s, const char *stop, const char *errmsg) {
    end_text(s);
    end_thinking(s);
    flush_open_calls(s);
    json_set_str(s->msg, "stopReason", stop);
    if (errmsg) json_set_str(s->msg, "errorMessage", errmsg);
    ai_usage_compute_cost(s->model, json_get(s->msg, "usage"));
    int64_t end = mono_ms();
    json_set_int(s->msg, "duration", end - s->t_start);
    if (s->t_first) json_set_int(s->msg, "ttft", s->t_first - s->t_start);
    bool err = strcmp(stop, "error") == 0 || strcmp(stop, "aborted") == 0;
    ev(s, err ? AI_EV_ERROR : AI_EV_DONE, -1, NULL, 0, NULL, stop);
    json_t *m = s->msg;
    s->msg = NULL;
    return m;
}

static void cs_free(cs_t *s) {
    json_free(s->msg);
    sb_free(&s->text);
    sb_free(&s->think);
    for (size_t i = 0; i < s->tb.n; i++) {
        free(s->tb.v[i]->envelope);
        sb_free(&s->tb.v[i]->partial);
        free(s->tb.v[i]);
    }
    vec_free(&s->tb);
    strvec_free(&s->resolved_mcp);
    strvec_free(&s->edit_owned);
    strvec_free(&s->paired_edit);
    free(s->cwd);
    json_free(s->req_tools);
    json_free(s->req_rules);
    json_free(s->latest_checkpoint);
    free(s->exec_span);
    json_free(s->exec_waiting);
}

/* withPromptLast: hosts append context after the prompt (developer messages), and Cursor sends the last message as
 * the run's user message, so trailing developer context moves ahead of the user prompt. Owned array or NULL (as is). */
static json_t *with_prompt_last(const json_t *messages) {
    size_t n = json_len(messages), i = n;
    while (i > 0 && str_eq(json_get_str(json_at(messages, i - 1), "role"), "developer")) i--;
    if (i == n || i == 0 || !str_eq(json_get_str(json_at(messages, i - 1), "role"), "user")) return NULL;
    json_t *out = json_arr();
    for (size_t k = 0; k < n; k++)
        if (k != i - 1) json_push(out, json_clone(json_at(messages, k)));
    json_push(out, json_clone(json_at(messages, i - 1)));
    return out;
}

static json_t *cursor_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                             void *ud);

json_t *provider_cursor_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                               void *ud) {
    json_t *reordered = with_prompt_last(ctx->messages);
    if (!reordered) return cursor_stream(m, ctx, o, cb, ud);
    ai_context_t turn = *ctx;
    turn.messages = reordered;
    json_t *r = cursor_stream(m, &turn, o, cb, ud);
    json_free(reordered);
    return r;
}

static json_t *cursor_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                             void *ud) {
    cs_t s = {0};
    s.model = m;
    s.opts = o;
    s.cb = cb;
    s.ud = ud;
    s.text_idx = s.think_idx = -1;
    s.t_start = mono_ms();
    s.msg = ai_error_message(m, "stop", NULL);
    json_set_str(s.msg, "api", "cursor-agent");
    char cwdbuf[4096];
    s.cwd = xstrdup(getcwd(cwdbuf, sizeof cwdbuf) ? cwdbuf : ".");

    if (!o->api_key || !*o->api_key) {
        ev(&s, AI_EV_START, -1, NULL, 0, NULL, NULL);
        json_t *r = finish(&s, "error", "Cursor API key (access token) is required");
        cs_free(&s);
        return r;
    }

    bool ephemeral;
    char *base_id = conversation_base_id(o, &ephemeral);
    pthread_mutex_lock(&g_conv_mu);
    maps_init_locked();
    const char *rot = hmap_get(&g_rotated, base_id);
    char *conversation_id = xstrdup(rot ? rot : base_id);
    bool rotated_fresh = hmap_has(&g_rotated_fresh, conversation_id);
    pthread_mutex_unlock(&g_conv_mu);

    bool known = false;
    conv_t *conv = conv_acquire(conversation_id, &known);
    if (rotated_fresh) conv_set_state(conv, NULL);
    s.req_tools = build_mcp_tool_defs(ctx->tools);
    s.req_rules = build_rules(ctx->system_prompt);

    ev(&s, AI_EV_START, -1, NULL, 0, NULL, NULL);

    char *original_request_id = NULL;
    json_t *resume_state = NULL; /* checkpoint the next attempt resumes */
    uint64_t resume_hash = 0;
    int no_progress_resumes = 0;
    bool discovered = false;
    json_t *result = NULL;
    cerr_t last = {0};

    for (int attempt = 0;; attempt++) {
        built_request_t br;
        build_request(m, ctx, o, conv, conversation_id, resume_state, discovered, &br);
        if (br.err) {
            result = finish(&s, "error", br.err);
            built_free(&br);
            break;
        }
        if (o->on_payload) o->on_payload(o->payload_ud, br.client_msg);
        char *request_id = uuid_v4();
        if (!original_request_id) original_request_id = xstrdup(request_id);
        run_t *r = run_start(m, o, conv, request_id, attempt ? original_request_id : NULL, br.client_msg);
        s.run = r;
        s.saw_turn_ended = false;
        s.step_completed_latest = false;
        cerr_t err;
        attempt_rc_t rc = run_attempt(&s, r, &err);
        run_close(r);
        s.run = NULL;
        free(request_id);

        if (rc == ATTEMPT_OK) {
            if (strcmp(conversation_id, base_id) != 0) {
                pthread_mutex_lock(&g_conv_mu);
                hmap_put(&g_rotated_ok, conversation_id, (void *)1);
                hmap_del(&g_rotated_fresh, conversation_id);
                pthread_mutex_unlock(&g_conv_mu);
            }
            cerr_free(&err);
            built_free(&br);
            result = finish(&s, "stop", NULL);
            break;
        }
        if (rc == ATTEMPT_ABORTED) {
            result = finish(&s, "aborted", err.message ? err.message : "Request was aborted");
            cerr_free(&err);
            built_free(&br);
            break;
        }
        bool cancelled = o->cancel && *o->cancel;
        /* discovered-id retry: the normalized effort pair was refused before any output */
        if (!discovered && !s.saw_progress && !cancelled && br.fallback_wire && err_model_not_found(&err)) {
            discovered = true;
            cerr_free(&err);
            built_free(&br);
            attempt--;
            continue;
        }
        bool fresh = s.latest_checkpoint && s.latest_checkpoint_version == s.progress_version &&
                     !s.latest_checkpoint_terminal;
        bool replay_safe = fresh || !s.saw_progress;
        json_t *retry_cp = fresh ? s.latest_checkpoint : resume_state;
        uint64_t h = 0;
        if (retry_cp) {
            size_t len;
            char *bytes = cursor_encode("ConversationStateStructure", retry_cp, &len);
            h = hash_fnv1a(bytes, len);
            free(bytes);
        }
        int npr = retry_cp && resume_state && h == resume_hash ? no_progress_resumes + 1 : 0;
        if (replay_safe && !cancelled && attempt < CURSOR_MAX_STREAM_RETRIES && npr <= 2 && err.retryable) {
            int64_t delay = (int64_t)CURSOR_RETRY_BASE_DELAY_MS << attempt;
            int64_t cap = o->max_retry_delay_ms ? o->max_retry_delay_ms : 60000;
            if (cap > 0 && delay > cap) delay = cap;
            LOGW("cursor stream retrying (attempt %d): %s; in %lldms", attempt + 1, err.message ? err.message : "?",
                 (long long)delay);
            json_t *next = retry_cp ? json_clone(retry_cp) : NULL;
            json_free(resume_state);
            resume_state = next;
            resume_hash = h;
            no_progress_resumes = npr;
            int64_t until = mono_ms() + delay;
            while (mono_ms() < until && !(o->cancel && *o->cancel)) sleep_ms(20);
            cerr_free(&err);
            built_free(&br);
            if (o->cancel && *o->cancel) {
                result = finish(&s, "aborted", "Request was aborted");
                break;
            }
            continue;
        }
        /* #8345/#15830: a poisoned conversation rotates its wire id for the next request */
        if (!s.saw_token_delta && err_conversation_rejected(&err)) {
            pthread_mutex_lock(&g_conv_mu);
            const char *cur = hmap_get(&g_rotated, base_id);
            bool can = !cur || hmap_has(&g_rotated_ok, cur);
            if (can) {
                char *nid = uuid_v4();
                if (cur) hmap_del(&g_rotated_ok, cur);
                free(hmap_put(&g_rotated, base_id, xstrdup(nid)));
                hmap_put(&g_rotated_fresh, nid, (void *)1);
                LOGD("cursor conversation rotated %s -> %s", conversation_id, nid);
                free(nid);
            }
            pthread_mutex_unlock(&g_conv_mu);
            if (can) conv_drop_id(conversation_id);
        }
        last = err;
        built_free(&br);
        result = finish(&s, "error", last.message ? last.message : "Cursor request failed");
        break;
    }
    cerr_free(&last);
    json_free(resume_state);
    free(original_request_id);
    conv_release(conv, ephemeral);
    if (ephemeral) {
        pthread_mutex_lock(&g_conv_mu);
        char *rv = hmap_del(&g_rotated, base_id);
        if (rv) {
            hmap_del(&g_rotated_ok, rv);
            hmap_del(&g_rotated_fresh, rv);
        }
        pthread_mutex_unlock(&g_conv_mu);
        if (rv) conv_drop_id(rv);
        free(rv);
    }
    free(conversation_id);
    free(base_id);
    cs_free(&s);
    return result;
}
