/* trace.c - offline session trace (port of omp packages/stats/src/trace.ts: scanTranscript,
 * linkChildTracks, buildSessionTraceFromSnapshots, assembleSessionTrace). Produces the SessionTrace JSON
 * that `/export` embeds, with omp's key order; strings follow JS semantics (UTF-16 lengths, \s classes). */
#include "session/trace.h"

#include "ai/ai.h"

#include <math.h>
#include <time.h>

#define LABEL_MAX 80
#define DETAIL_MAX 160

/* ---------- JS string helpers ---------- */

/* JS \s / String.prototype.trim whitespace */
static bool js_space(uint32_t c) {
    return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
           c == 0xFEFF;
}

typedef VEC(uint32_t) cpvec_t;

static void decode(const char *s, cpvec_t *out) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        /* WTF-8 lone surrogate (ED A0..BF xx) */
        if ((unsigned char)s[i] == 0xED && i + 2 < n && ((unsigned char)s[i + 1] & 0xE0) == 0xA0) {
            cp = 0xD000u | (((unsigned)s[i + 1] & 0x3Fu) << 6) | ((unsigned)s[i + 2] & 0x3Fu);
            vec_push(out, cp);
            i += 3;
            continue;
        }
        int k = utf8_decode(s + i, n - i, &cp);
        vec_push(out, cp);
        i += k > 0 ? (size_t)k : 1;
    }
}

static void put_cp(sb_t *b, uint32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) { /* lone surrogate as WTF-8 */
        char t[3] = {(char)0xED, (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
        sb_appendn(b, t, 3);
        return;
    }
    char t[4];
    int k = utf8_encode(cp, t);
    sb_appendn(b, t, (size_t)k);
}

/* headText: whitespace runs collapsed, trimmed, capped at `max` UTF-16 units with a trailing "…" */
static char *head_text(const char *value, size_t max) {
    cpvec_t cps = {0};
    decode(value ? value : "", &cps);
    cpvec_t col = {0};
    bool in_ws = false;
    for (size_t i = 0; i < cps.n; i++) {
        if (js_space(cps.v[i])) {
            if (!in_ws) vec_push(&col, (uint32_t)' ');
            in_ws = true;
        } else {
            vec_push(&col, cps.v[i]);
            in_ws = false;
        }
    }
    size_t a = 0, z = col.n;
    while (a < z && col.v[a] == ' ') a++;
    while (z > a && col.v[z - 1] == ' ') z--;
    size_t units = 0;
    for (size_t i = a; i < z; i++) units += col.v[i] > 0xFFFF ? 2 : 1;
    sb_t b = SB_INIT;
    if (units > max) {
        size_t keep = max - 1, u = 0;
        for (size_t i = a; i < z && u < keep; i++) {
            if (col.v[i] > 0xFFFF) {
                if (u + 2 <= keep) put_cp(&b, col.v[i]);
                else put_cp(&b, 0xD800 + ((col.v[i] - 0x10000) >> 10)); /* slice splits the pair */
                u += 2;
            } else {
                put_cp(&b, col.v[i]);
                u++;
            }
        }
        sb_append(&b, "\xe2\x80\xa6");
    } else {
        for (size_t i = a; i < z; i++) put_cp(&b, col.v[i]);
    }
    vec_free(&cps);
    vec_free(&col);
    return sb_detach(&b);
}

static bool js_blank(const char *s) {
    cpvec_t cps = {0};
    decode(s, &cps);
    bool blank = true;
    for (size_t i = 0; i < cps.n && blank; i++) blank = js_space(cps.v[i]);
    vec_free(&cps);
    return blank;
}

/* contentHead */
static char *content_head(const json_t *content, size_t max) {
    if (json_is_str(content)) return js_blank(json_sv(content)) ? NULL : head_text(json_sv(content), max);
    if (!json_is_arr(content)) return NULL;
    json_t *b;
    json_arr_foreach(content, i, b) {
        if (!json_is_obj(b) || !str_eq(json_get_str(b, "type"), "text")) continue;
        const char *t = json_get_str(b, "text");
        if (t && !js_blank(t)) return head_text(t, max);
    }
    return NULL;
}

/* String(number) */
static char *js_num_str(double d) {
    sb_t b = SB_INIT;
    json_js_num_sb(&b, d);
    return sb_detach(&b);
}

/* compactTokens */
static char *compact_tokens(const json_t *v) {
    if (!json_is_num(v) || !isfinite(v->u.num.d)) return xstrdup("?");
    double x = v->u.num.d;
    if (x >= 1000) {
        char *n = js_num_str(floor(x / 1000 + 0.5));
        char *s = xasprintf("%sk", n);
        free(n);
        return s;
    }
    return js_num_str(x);
}

/* ---------- Date.parse (ISO 8601 forms) ---------- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool digits(const char **p, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if ((*p)[i] < '0' || (*p)[i] > '9') return false;
        v = v * 10 + ((*p)[i] - '0');
    }
    *p += n;
    *out = v;
    return true;
}

bool trace_parse_iso_ms(const char *s, double *out) {
    if (!s) return false;
    const char *p = s;
    int sign = 1, y, mo = 1, d = 1, h = 0, mi = 0, sec = 0, ms = 0;
    if (*p == '+' || *p == '-') {
        sign = *p == '-' ? -1 : 1;
        p++;
        if (!digits(&p, 6, &y)) return false;
    } else if (!digits(&p, 4, &y)) return false;
    y *= sign;
    bool date_only = true;
    if (*p == '-') {
        p++;
        if (!digits(&p, 2, &mo)) return false;
        if (*p == '-') {
            p++;
            if (!digits(&p, 2, &d)) return false;
        }
    }
    bool has_tz = false;
    int tz = 0;
    if (*p == 'T' || *p == 't') {
        date_only = false;
        p++;
        if (!digits(&p, 2, &h) || *p++ != ':' || !digits(&p, 2, &mi)) return false;
        if (*p == ':') {
            p++;
            if (!digits(&p, 2, &sec)) return false;
            if (*p == '.' || *p == ',') {
                p++;
                int nd = 0;
                ms = 0;
                while (*p >= '0' && *p <= '9') {
                    if (nd < 3) ms = ms * 10 + (*p - '0');
                    nd++;
                    p++;
                }
                if (!nd) return false;
                for (; nd < 3; nd++) ms *= 10;
            }
        }
        if (*p == 'Z' || *p == 'z') {
            has_tz = true;
            p++;
        } else if (*p == '+' || *p == '-') {
            int sg = *p == '-' ? -1 : 1, th, tm;
            p++;
            if (!digits(&p, 2, &th)) return false;
            if (*p == ':') p++;
            if (!digits(&p, 2, &tm)) return false;
            has_tz = true;
            tz = sg * (th * 60 + tm);
        }
    }
    if (*p) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 24 || mi > 59 || sec > 59 || (h == 24 && (mi || sec || ms))) return false;
    int64_t days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    double t = (double)days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + sec * 1000.0 + ms;
    if (date_only || has_tz) {
        *out = t - tz * 60000.0;
        return true;
    }
    /* date-time without an offset is local time */
    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    time_t lt = mktime(&tmv);
    if (lt == (time_t)-1) return false;
    *out = (double)lt * 1000.0 + ms;
    return true;
}

static bool to_ms(const json_t *v, double *out) { return json_is_str(v) && trace_parse_iso_ms(json_sv(v), out); }

static bool fin_num(const json_t *v, double *out) {
    if (!json_is_num(v) || !isfinite(v->u.num.d)) return false;
    *out = v->u.num.d;
    return true;
}

/* ---------- spans ---------- */

typedef enum { K_MODEL, K_TOOL, K_BACKGROUND, K_TURN, K_SUBAGENT } span_kind_t;
static const char *const KIND_NAMES[] = {"model", "tool", "background", "turn", "subagent"};

typedef struct {
    char *id;
    span_kind_t kind;
    double start, end;
    char *label;
    char *tool_call_id, *detail, *entry_id, *child_track_id, *model;
    double tokens, cost, ttft;
    bool has_tokens, has_cost, has_ttft, is_error, unterminated;
} span_t;

typedef struct {
    double time;
    const char *kind;
    char *label;
} marker_t;

typedef struct {
    char *child_id, *agent, *task, *result_entry_id;
    double duration_ms, tool_span_end;
    bool has_duration;
} task_result_t;

typedef struct track track_t;
struct track {
    char *id, *parent_id, *label, *agent, *model, *file;
    VEC(span_t) spans;
    VEC(marker_t) markers;
    VEC(task_result_t) task_results;
    double requests, tool_calls, unpriced;
    char *cwd, *title;
    double header_ts, first, last;
    bool has_header_ts, has_first, has_last;
};

static void span_free(span_t *s) {
    free(s->id);
    free(s->label);
    free(s->tool_call_id);
    free(s->detail);
    free(s->entry_id);
    free(s->child_track_id);
    free(s->model);
}

/* stable merge sort */
typedef int (*cmp_fn)(const void *, const void *);
static void msort(void *base, size_t n, size_t sz, cmp_fn cmp) {
    if (n < 2) return;
    char *a = base, *tmp = xmalloc(n * sz);
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = MIN(lo + w, n), hi = MIN(lo + 2 * w, n), i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(a + j * sz, a + i * sz) < 0) memcpy(tmp + k++ * sz, a + j++ * sz, sz);
                else memcpy(tmp + k++ * sz, a + i++ * sz, sz);
            }
            while (i < mid) memcpy(tmp + k++ * sz, a + i++ * sz, sz);
            while (j < hi) memcpy(tmp + k++ * sz, a + j++ * sz, sz);
        }
        memcpy(a, tmp, n * sz);
    }
    free(tmp);
}

static int sgn(double d) { return d < 0 ? -1 : d > 0 ? 1 : 0; }
static int cmp_span(const void *x, const void *y) {
    const span_t *a = x, *b = y;
    double d = a->start - b->start;
    return d != 0 ? sgn(d) : sgn(a->end - b->end);
}
static int cmp_marker(const void *x, const void *y) { return sgn(((const marker_t *)x)->time - ((const marker_t *)y)->time); }

/* ---------- pricing (unpriced-request predicate) ---------- */

/* catalog compat rules.json behavior.pricingPeers (v18.14.2) */
static const struct {
    const char *provider, *peers[3];
    const char *aliases[8][2];
} PRICING_PEERS[] = {
    {"google-antigravity",
     {"google", "google-vertex", "anthropic"},
     {{"gemini-3-pro", "gemini-3-pro-preview"},
      {"gemini-3.1-pro", "gemini-3.1-pro-preview"},
      {"claude-opus-4-5", "claude-opus-4-5@20251101"},
      {"claude-opus-4-6", "claude-opus-4-6@default"},
      {"claude-sonnet-4-5", "claude-sonnet-4-5@20250929"},
      {"claude-sonnet-4-6", "claude-sonnet-4-6@default"},
      {"claude-opus-5-5", "claude-opus-5-5@default"},
      {"claude-sonnet-5-5", "claude-sonnet-5-5@default"}}},
    {"xai-oauth", {"xai"}, {{"grok-4.20-multi-agent-0309", "grok-4.20-multi-agent-beta-latest"}}},
    {"minimax-code", {"minimax"}, {{"MiniMax-M3.1-Flash-Preview", "MiniMax-M3"}}},
    {"minimax-code-cn", {"minimax-cn", "minimax"}, {{"MiniMax-M3.1-Flash-Preview", "MiniMax-M3"}}},
};

/* true when the provider has pricing peers and none of them prices the model; -1 when no peer rule */
static int peer_unpriced(const char *provider, const char *model) {
    for (size_t i = 0; i < ARRAY_LEN(PRICING_PEERS); i++) {
        if (strcmp(PRICING_PEERS[i].provider, provider)) continue;
        const char *peer_id = model;
        for (size_t k = 0; k < 8 && PRICING_PEERS[i].aliases[k][0]; k++)
            if (!strcmp(PRICING_PEERS[i].aliases[k][0], model)) peer_id = PRICING_PEERS[i].aliases[k][1];
        for (size_t k = 0; k < 3 && PRICING_PEERS[i].peers[k]; k++) {
            const ai_model_t *m = catalog_find(PRICING_PEERS[i].peers[k], peer_id);
            if (m && (m->cost_input != 0 || m->cost_output != 0 || m->cost_cache_read != 0 || m->cost_cache_write != 0)) return 0;
        }
        return 1;
    }
    return -1;
}

/* resolveUsageTotal */
static double usage_total(const json_t *u) {
    double t;
    if (fin_num(json_get(u, "totalTokens"), &t)) return t;
    if (!json_is_obj(u)) return 0;
    double s = 0, x;
    static const char *const K[] = {"input", "output", "cacheRead", "cacheWrite"};
    for (size_t i = 0; i < ARRAY_LEN(K); i++)
        if (fin_num(json_get(u, K[i]), &x)) s += x;
    json_t *o = json_get(u, "orchestration");
    if (json_is_obj(o)) {
        static const char *const O[] = {"input", "output", "cacheRead"};
        for (size_t i = 0; i < ARRAY_LEN(O); i++)
            if (fin_num(json_get(o, O[i]), &x)) s += x;
    }
    return s;
}

/* ---------- scanTranscript ---------- */

typedef struct {
    char *call_id, *name, *args_preview, *assistant_entry_id;
    double model_end;
} pending_tool_t;

typedef struct {
    char *job_id, *label, *entry_id;
    double start, end;
    bool has_end;
} bg_job_t;

/* closeBackgroundJob: end the open job `jid` at `at`, never before it opened; a later job may reuse the id */
static void close_bg_job(hmap_t *active, const char *jid, double at) {
    bg_job_t *job = hmap_get(active, jid);
    if (!job) return;
    job->end = MAX(job->start, at);
    job->has_end = true;
    hmap_del(active, jid);
}

typedef struct {
    double time;
    char *label, *entry_id;
} turn_t;
static int cmp_turn(const void *x, const void *y) { return sgn(((const turn_t *)x)->time - ((const turn_t *)y)->time); }

static char *dupn(const char *s) { return s ? xstrdup(s) : NULL; }

/* activeChain: the parentId chain walked back from the last entry with an id */
static json_t *active_chain(const json_t *entries) {
    hmap_t by_id;
    hmap_init(&by_id);
    const json_t *last = NULL, *e;
    json_arr_foreach(entries, i, e) {
        const char *id = json_get_str(e, "id");
        if (id && *id) {
            hmap_put(&by_id, id, (void *)e);
            last = e;
        }
    }
    json_t *chain = json_arr();
    if (!last) {
        hmap_free(&by_id, NULL);
        return chain;
    }
    hmap_t keep;
    hmap_init(&keep);
    for (const json_t *cur = last; cur;) {
        const char *id = json_get_str(cur, "id");
        if (!id || hmap_get(&keep, id)) break;
        hmap_put(&keep, id, (void *)1);
        const char *pid = json_get_str(cur, "parentId");
        cur = pid ? hmap_get(&by_id, pid) : NULL;
    }
    json_arr_foreach(entries, i, e) {
        const char *id = json_get_str(e, "id");
        if (id && hmap_get(&keep, id)) json_push(chain, json_clone(e));
    }
    hmap_free(&keep, NULL);
    hmap_free(&by_id, NULL);
    return chain;
}

static char *tool_start_detail(const json_t *data) {
    const char *cand = NULL;
    json_t *args = json_get(data, "args");
    if (json_is_obj(args) || json_is_arr(args)) {
        if (json_get_str(args, "command")) cand = json_get_str(args, "command");
        else if (json_get_str(args, "path")) cand = json_get_str(args, "path");
    }
    if (!cand) cand = json_sv(json_get(data, "intent")); /* candidate ??= data.intent */
    if (cand && !js_blank(cand)) return head_text(cand, DETAIL_MAX);
    return NULL;
}

static char *args_preview(const json_t *args) {
    if (!args || args->type == JSON_NULL) return NULL;
    char *s = json_dump_js(args);
    char *h = head_text(s, DETAIL_MAX);
    free(s);
    return h;
}

static void push_marker(track_t *t, double time, const char *kind, char *label) {
    marker_t m = {time, kind, label};
    vec_push(&t->markers, m);
}

static track_t *scan_transcript(const json_t *entries, const char *track_id, const char *parent_track_id, const char *file,
                                const json_t *header) {
    track_t *t = xcalloc(1, sizeof *t);
    t->id = xstrdup(track_id);
    t->parent_id = dupn(parent_track_id);
    t->label = xstrdup("");
    t->file = xstrdup(file);
    json_t *chain = active_chain(entries);
    VEC(pending_tool_t) pending = {0};
    hmap_t starts, results, active_bg;
    hmap_init(&starts);
    hmap_init(&results);
    hmap_init(&active_bg);
    typedef struct {
        double start;
        char *detail;
    } start_fact_t;
    typedef struct {
        double end;
        bool is_error;
        char *entry_id, *tool_name;
    } result_fact_t;
    VEC(bg_job_t *) bg = {0};
    VEC(turn_t) turns = {0};
    bool has_exit = false;
    double exit_at = 0;
    hmap_t exit_ids;
    hmap_init(&exit_ids);

    const char *cwd = json_get_str(header, "cwd");
    const char *header_title = json_get_str(header, "title");
    double hts;
    t->has_header_ts = to_ms(json_get(header, "timestamp"), &hts);
    t->header_ts = hts;
    const char *slot_title = NULL, *init_agent = NULL, *init_model = NULL, *first_model = NULL;
    double last_chain_ts = 0;

    json_t *raw;
    json_arr_foreach(entries, i, raw) {
        const char *type = json_get_str(raw, "type");
        if (str_eq(type, "title") && json_get_str(raw, "title")) slot_title = json_get_str(raw, "title");
        else if (str_eq(type, "session")) {
            if (json_get_str(raw, "cwd")) cwd = json_get_str(raw, "cwd");
            if (json_get_str(raw, "title")) header_title = json_get_str(raw, "title");
            t->has_header_ts = to_ms(json_get(raw, "timestamp"), &hts);
            t->header_ts = hts;
        } else if (str_eq(type, "session_init")) {
            if (json_get_str(raw, "agent")) init_agent = json_get_str(raw, "agent");
            if (json_get_str(raw, "resolvedModel")) init_model = json_get_str(raw, "resolvedModel");
        }
    }

    json_t *entry;
    json_arr_foreach(chain, ci, entry) {
        double env;
        bool has_env = to_ms(json_get(entry, "timestamp"), &env);
        if (has_env && env > last_chain_ts) last_chain_ts = env;
        const char *type = json_get_str(entry, "type");
        const char *eid = json_get_str(entry, "id");

        if (str_eq(type, "custom")) {
            json_t *data = json_get(entry, "data");
            const char *ct = json_get_str(entry, "customType");
            if (str_eq(ct, "tool_execution_start")) {
                const char *call = json_get_str(data, "toolCallId");
                if (call) {
                    double st;
                    bool ok = json_get_str(data, "startedAt") && to_ms(json_get(data, "startedAt"), &st);
                    if (!ok && has_env) {
                        st = env;
                        ok = true;
                    }
                    if (ok) {
                        start_fact_t *f = hmap_get(&starts, call);
                        if (f) free(f->detail);
                        else {
                            f = xcalloc(1, sizeof *f);
                            hmap_put(&starts, call, f);
                        }
                        f->start = st;
                        f->detail = tool_start_detail(data);
                    }
                }
            } else if (str_eq(ct, "session_exit")) {
                double rec;
                bool ok = json_get_str(data, "recordedAt") && to_ms(json_get(data, "recordedAt"), &rec);
                if (!ok && has_env) {
                    rec = env;
                    ok = true;
                }
                hmap_t ids;
                hmap_init(&ids);
                json_t *pc;
                json_arr_foreach(json_get_arr(data, "pendingToolCalls"), k, pc) {
                    if (json_get_str(pc, "toolCallId")) hmap_put(&ids, json_get_str(pc, "toolCallId"), (void *)1);
                }
                if (ok) {
                    has_exit = true;
                    exit_at = rec;
                    hmap_free(&exit_ids, NULL);
                    exit_ids = ids;
                    push_marker(t, rec, "session_exit", xstrdup(json_get_str(data, "kind") ? json_get_str(data, "kind") : "exit"));
                } else hmap_free(&ids, NULL);
            }
            continue;
        }
        if (str_eq(type, "custom_message")) {
            /* async-result deliveries persist as custom_message entries carrying details.jobs[].jobId */
            json_t *jobs = json_get(json_get(entry, "details"), "jobs");
            if (str_eq(json_get_str(entry, "customType"), "async-result") && has_env && json_is_arr(jobs)) {
                json_t *job;
                json_arr_foreach(jobs, k, job) {
                    const char *jid = json_get_str(job, "jobId");
                    if (jid) close_bg_job(&active_bg, jid, env);
                }
            }
            continue;
        }
        if (str_eq(type, "compaction") && has_env) {
            char *a = compact_tokens(json_get(entry, "tokensBefore")), *b = compact_tokens(json_get(entry, "tokensAfter"));
            push_marker(t, env, "compaction", xasprintf("compaction %s\xe2\x86\x92%s", a, b));
            free(a);
            free(b);
            continue;
        }
        if (str_eq(type, "model_change") && has_env) {
            json_t *m = json_get(entry, "model");
            char *l = json_is_str(m) ? xstrdup(json_sv(m)) : m && m->type != JSON_NULL ? json_dump_js(m) : xstrdup("model change");
            push_marker(t, env, "model_change", l);
            continue;
        }
        if (str_eq(type, "mode_change") && has_env) {
            json_t *m = json_get(entry, "mode");
            char *l = json_is_str(m) ? xstrdup(json_sv(m)) : m && m->type != JSON_NULL ? json_dump_js(m) : xstrdup("mode change");
            push_marker(t, env, "mode_change", l);
            continue;
        }
        if (str_eq(type, "reset_boundary") && has_env) {
            push_marker(t, env, "reset", xstrdup("reset"));
            continue;
        }
        json_t *msg = json_get(entry, "message");
        if (!str_eq(type, "message") || !json_is_obj(msg)) continue;
        const char *role = json_get_str(msg, "role");

        if (str_eq(role, "assistant")) {
            t->requests++;
            json_t *usage = json_get(msg, "usage");
            double total = usage_total(usage);
            json_t *cost_obj = json_get(usage, "cost");
            double cost_total = 0;
            bool has_cost = fin_num(json_get(cost_obj, "total"), &cost_total);
            if (total > 0 && (!has_cost || cost_total == 0)) {
                const char *prov = json_get_str(msg, "provider") ? json_get_str(msg, "provider") : "";
                const char *model = json_get_str(msg, "model") ? json_get_str(msg, "model") : "";
                int pu = *prov && *model ? peer_unpriced(prov, model) : -1;
                if (pu == 1) t->unpriced++;
                /* the no-cost + no-timestamp + time-based catalog branch needs scheduled pricing omc's
                 * catalog does not carry */
            }
            double start = 0, end = 0, x;
            bool has_start = fin_num(json_get(msg, "timestamp"), &start), has_end = false;
            if (fin_num(json_get(msg, "completedAt"), &x)) {
                end = x;
                has_end = true;
            } else if (has_start && json_is_num(json_get(msg, "duration"))) {
                end = start + json_get(msg, "duration")->u.num.d;
                has_end = true;
            }
            if (!has_start) {
                if (!has_end) {
                    if (!has_env) continue;
                    end = env;
                }
                double dur = json_is_num(json_get(msg, "duration")) ? json_get(msg, "duration")->u.num.d : 0;
                start = end - dur;
                has_end = true;
            }
            if (!has_end) end = has_env ? env : start;
            end = fmax(start, end);
            if (json_get_str(msg, "model") && !first_model) first_model = json_get_str(msg, "model");
            span_t sp = {0};
            sp.id = xasprintf("%s:%s", track_id, eid ? eid : "undefined");
            sp.kind = K_MODEL;
            sp.start = start;
            sp.end = end;
            json_t *mv = json_get(msg, "model");
            sp.label = head_text(json_is_str(mv) ? json_sv(mv) : "model", LABEL_MAX);
            sp.detail = content_head(json_get(msg, "content"), DETAIL_MAX);
            if (eid && *eid) sp.entry_id = xstrdup(eid);
            if (json_get_str(msg, "model")) sp.model = xstrdup(json_get_str(msg, "model"));
            if (total > 0) {
                sp.tokens = total;
                sp.has_tokens = true;
            }
            if (has_cost) {
                sp.cost = cost_total;
                sp.has_cost = true;
            }
            if (json_is_num(json_get(msg, "ttft"))) {
                sp.ttft = json_get(msg, "ttft")->u.num.d;
                sp.has_ttft = true;
            }
            const char *em = json_get_str(msg, "errorMessage");
            if (str_eq(json_get_str(msg, "stopReason"), "error") || (em && *em)) sp.is_error = true;
            vec_push(&t->spans, sp);
            json_t *blk;
            json_arr_foreach(json_get_arr(msg, "content"), k, blk) {
                if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
                if (!json_get_str(blk, "id") || !json_get_str(blk, "name")) continue;
                pending_tool_t pt = {xstrdup(json_get_str(blk, "id")), xstrdup(json_get_str(blk, "name")),
                                     args_preview(json_get(blk, "arguments")), xstrdup(eid ? eid : ""), end};
                vec_push(&pending, pt);
            }
            continue;
        }
        if (str_eq(role, "toolResult")) {
            const char *call = json_get_str(msg, "toolCallId");
            if (!call) continue;
            double end;
            if (!fin_num(json_get(msg, "timestamp"), &end)) {
                if (!has_env) continue;
                end = env;
            }
            result_fact_t *r = hmap_get(&results, call);
            if (r) {
                free(r->entry_id);
                free(r->tool_name);
            } else {
                r = xcalloc(1, sizeof *r);
                hmap_put(&results, call, r);
            }
            r->end = end;
            r->is_error = json_get_bool(msg, "isError", false);
            r->entry_id = dupn(eid);
            r->tool_name = xstrdup(json_get_str(msg, "toolName") ? json_get_str(msg, "toolName") : "tool");
            json_t *details = json_get(msg, "details");
            json_t *async = json_get(details, "async");
            if (json_is_obj(async) && str_eq(json_get_str(async, "state"), "running") && json_get_str(async, "jobId")) {
                bg_job_t *j = xcalloc(1, sizeof *j);
                j->job_id = xstrdup(json_get_str(async, "jobId"));
                j->start = end;
                char *l = xasprintf("%s job", json_get_str(msg, "toolName") ? json_get_str(msg, "toolName") : "tool");
                j->label = head_text(l, LABEL_MAX);
                free(l);
                j->entry_id = dupn(eid);
                vec_push(&bg, j);
                hmap_put(&active_bg, j->job_id, j);
            }
            /* a `wait` (details.jobs) or proc:// cancel (details.proc.jobs) that reports a settled job replaces its
             * async-result delivery, so the job closes when it settled, or at this result */
            json_t *proc = json_get(details, "proc");
            json_t *reported = str_eq(json_get_str(details, "op"), "wait") ? json_get(details, "jobs")
                               : (json_is_obj(proc) && str_eq(json_get_str(proc, "op"), "cancel")) ? json_get(proc, "jobs")
                                                                                                 : NULL;
            if (json_is_arr(reported)) {
                json_t *job;
                json_arr_foreach(reported, k, job) {
                    const char *jid = json_get_str(job, "id"), *st = json_get_str(job, "status");
                    if (!jid || !(str_eq(st, "completed") || str_eq(st, "failed") || str_eq(st, "cancelled"))) continue;
                    double settled;
                    double at = fin_num(json_get(job, "settledAtMs"), &settled) ? MIN(settled, end) : end;
                    close_bg_job(&active_bg, jid, at);
                }
            }
            if (str_eq(json_get_str(msg, "toolName"), "task") && json_is_arr(json_get(details, "results"))) {
                json_t *res;
                json_arr_foreach(json_get(details, "results"), k, res) {
                    if (!json_get_str(res, "id")) continue;
                    task_result_t tr = {0};
                    tr.child_id = xstrdup(json_get_str(res, "id"));
                    tr.agent = dupn(json_get_str(res, "agent"));
                    tr.task = dupn(json_get_str(res, "task"));
                    if (json_is_num(json_get(res, "durationMs"))) {
                        tr.duration_ms = json_get(res, "durationMs")->u.num.d;
                        tr.has_duration = true;
                    }
                    tr.result_entry_id = xstrdup(eid ? eid : "");
                    tr.tool_span_end = end;
                    vec_push(&t->task_results, tr);
                }
            }
            continue;
        }
        if (str_eq(role, "user") && !(json_get(msg, "synthetic") && json_get(msg, "synthetic")->type == JSON_BOOL &&
                                      json_get(msg, "synthetic")->u.b)) {
            double time;
            if (!fin_num(json_get(msg, "timestamp"), &time)) {
                if (!has_env) continue;
                time = env;
            }
            char *l = content_head(json_get(msg, "content"), LABEL_MAX);
            turn_t tn = {time, l ? l : xstrdup("user"), dupn(eid)};
            vec_push(&turns, tn);
        }
    }

    /* tool spans */
    for (size_t i = 0; i < pending.n; i++) {
        pending_tool_t *p = &pending.v[i];
        start_fact_t *mk = hmap_get(&starts, p->call_id);
        result_fact_t *r = hmap_get(&results, p->call_id);
        double start = mk ? mk->start : p->model_end, end;
        bool unterminated = false;
        if (r) end = r->end;
        else if (has_exit && hmap_get(&exit_ids, p->call_id)) {
            end = exit_at;
            unterminated = true;
        } else {
            end = last_chain_ts != 0 ? last_chain_ts : start;
            unterminated = true;
        }
        span_t sp = {0};
        sp.id = xasprintf("%s:%s:%s", track_id, p->assistant_entry_id, p->call_id);
        sp.kind = K_TOOL;
        sp.start = start;
        sp.end = fmax(start, end);
        sp.label = head_text(p->name, LABEL_MAX);
        sp.tool_call_id = xstrdup(p->call_id);
        const char *detail = mk && mk->detail ? mk->detail : p->args_preview;
        if (detail && *detail) sp.detail = xstrdup(detail);
        const char *entry_id = r && r->entry_id ? r->entry_id : p->assistant_entry_id;
        if (entry_id && *entry_id) sp.entry_id = xstrdup(entry_id);
        if (r && r->is_error) sp.is_error = true;
        sp.unterminated = unterminated;
        vec_push(&t->spans, sp);
    }
    /* background spans */
    for (size_t i = 0; i < bg.n; i++) {
        bg_job_t *j = bg.v[i];
        double end = j->has_end ? j->end : (last_chain_ts != 0 ? last_chain_ts : j->start);
        span_t sp = {0};
        sp.id = xasprintf("%s:bg:%zu:%s", track_id, i, j->job_id);
        sp.kind = K_BACKGROUND;
        sp.start = j->start;
        sp.end = fmax(j->start, end);
        sp.label = xstrdup(j->label);
        if (j->entry_id && *j->entry_id) sp.entry_id = xstrdup(j->entry_id);
        sp.unterminated = !j->has_end;
        vec_push(&t->spans, sp);
    }
    /* turn spans */
    msort(turns.v, turns.n, sizeof *turns.v, cmp_turn);
    size_t nspans = t->spans.n;
    for (size_t i = 0; i < turns.n; i++) {
        turn_t *tn = &turns.v[i];
        double next = i + 1 < turns.n ? turns.v[i + 1].time : INFINITY, end = tn->time;
        for (size_t k = 0; k < t->spans.n; k++) {
            span_t *s = &t->spans.v[k];
            if (s->start >= tn->time && s->start < next && s->end > end) end = s->end;
        }
        (void)nspans;
        span_t sp = {0};
        sp.id = tn->entry_id ? xasprintf("%s:%s", track_id, tn->entry_id) : xasprintf("%s:turn%zu", track_id, i);
        sp.kind = K_TURN;
        sp.start = tn->time;
        sp.end = end;
        sp.label = xstrdup(tn->label);
        if (tn->entry_id && *tn->entry_id) sp.entry_id = xstrdup(tn->entry_id);
        vec_push(&t->spans, sp);
    }
    msort(t->spans.v, t->spans.n, sizeof *t->spans.v, cmp_span);
    msort(t->markers.v, t->markers.n, sizeof *t->markers.v, cmp_marker);
    for (size_t k = 0; k < t->spans.n; k++) {
        span_t *s = &t->spans.v[k];
        if (!t->has_first || s->start < t->first) t->first = s->start, t->has_first = true;
        if (!t->has_last || s->end > t->last) t->last = s->end, t->has_last = true;
    }
    t->agent = dupn(init_agent);
    t->model = dupn(init_model ? init_model : first_model);
    t->tool_calls = (double)pending.n;
    t->cwd = dupn(cwd);
    t->title = dupn(slot_title ? slot_title : header_title);

    for (size_t i = 0; i < pending.n; i++) {
        free(pending.v[i].call_id);
        free(pending.v[i].name);
        free(pending.v[i].args_preview);
        free(pending.v[i].assistant_entry_id);
    }
    vec_free(&pending);
    hmap_foreach(&starts, it) {
        start_fact_t *f = it->val;
        free(f->detail);
        free(f);
    }
    hmap_foreach(&results, it) {
        result_fact_t *r = it->val;
        free(r->entry_id);
        free(r->tool_name);
        free(r);
    }
    hmap_free(&starts, NULL);
    hmap_free(&results, NULL);
    hmap_free(&active_bg, NULL);
    hmap_free(&exit_ids, NULL);
    for (size_t i = 0; i < bg.n; i++) {
        free(bg.v[i]->job_id);
        free(bg.v[i]->label);
        free(bg.v[i]->entry_id);
        free(bg.v[i]);
    }
    vec_free(&bg);
    for (size_t i = 0; i < turns.n; i++) {
        free(turns.v[i].label);
        free(turns.v[i].entry_id);
    }
    vec_free(&turns);
    json_free(chain);
    return t;
}

/* ---------- tree + assembly ---------- */

typedef struct {
    const char *stem;
    track_t *track;
} child_t;

static void link_child_tracks(track_t *scan, child_t *kids, size_t nk) {
    bool *linked = xcalloc(nk ? nk : 1, sizeof *linked);
    for (size_t i = 0; i < scan->task_results.n; i++) {
        task_result_t *r = &scan->task_results.v[i];
        track_t *child = NULL;
        for (size_t k = 0; k < nk; k++)
            if (!strcmp(kids[k].stem, r->child_id)) {
                child = kids[k].track;
                linked[k] = true;
            }
        double start, end;
        if (child && child->has_first && child->has_last) {
            start = child->first;
            end = child->last;
        } else if (r->has_duration) {
            start = r->tool_span_end - r->duration_ms;
            end = r->tool_span_end;
        } else continue;
        span_t sp = {0};
        sp.id = xasprintf("%s:%s:%s", scan->id, r->result_entry_id, r->child_id);
        sp.kind = K_SUBAGENT;
        sp.start = start;
        sp.end = fmax(start, end);
        sp.label = head_text(r->agent ? r->agent : r->child_id, LABEL_MAX);
        if (r->task) sp.detail = head_text(r->task, DETAIL_MAX);
        if (*r->result_entry_id) sp.entry_id = xstrdup(r->result_entry_id);
        if (child) {
            sp.child_track_id = xstrdup(child->id);
            if (child->model) sp.model = xstrdup(child->model);
        }
        vec_push(&scan->spans, sp);
    }
    for (size_t k = 0; k < nk; k++) {
        if (linked[k]) continue;
        track_t *child = kids[k].track;
        if (!child->has_first || !child->has_last) continue;
        span_t sp = {0};
        sp.id = xasprintf("%s:sub:%s", scan->id, kids[k].stem);
        sp.kind = K_SUBAGENT;
        sp.start = child->first;
        sp.end = fmax(child->first, child->last);
        sp.label = head_text(child->agent ? child->agent : child->label, LABEL_MAX);
        sp.child_track_id = xstrdup(child->id);
        if (child->model) sp.model = xstrdup(child->model);
        vec_push(&scan->spans, sp);
    }
    free(linked);
    msort(scan->spans.v, scan->spans.n, sizeof *scan->spans.v, cmp_span);
}

typedef VEC(track_t *) trackvec_t;

static int cmp_key(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static track_t *visit(const json_t *header, const json_t *entries, const char *key, const char *parent_id, const json_t *subs,
                      char **keys, size_t nkeys, trackvec_t *tracks) {
    track_t *scan = scan_transcript(entries, key, parent_id, key, header);
    vec_push(tracks, scan);
    VEC(child_t) kids = {0};
    for (size_t i = 0; i < nkeys; i++) {
        const json_t *snap = json_get(subs, keys[i]);
        const char *parent = json_get_str(snap, "parent");
        if (parent && !json_get(subs, parent)) parent = NULL;
        bool mine = parent_id == NULL ? parent == NULL : (parent && !strcmp(parent, key));
        if (!mine) continue;
        track_t *child = visit(json_get(snap, "header"), json_get(snap, "entries"), keys[i], key, subs, keys, nkeys, tracks);
        const char *slash = strrchr(keys[i], '/');
        const char *stem = slash ? slash + 1 : keys[i];
        free(child->label);
        if (!strcmp(stem, "__advisor") || str_starts(stem, "__advisor.")) child->label = xstrdup("Advisor");
        else {
            char *l = xasprintf("%s %s", child->agent ? child->agent : "agent", stem);
            child->label = head_text(l, LABEL_MAX);
            free(l);
        }
        child_t c = {stem, child};
        /* Map.set: a later child with the same stem replaces the earlier one in place */
        bool replaced = false;
        for (size_t k = 0; k < kids.n; k++)
            if (!strcmp(kids.v[k].stem, stem)) {
                kids.v[k].track = child;
                replaced = true;
            }
        if (!replaced) vec_push(&kids, c);
    }
    link_child_tracks(scan, kids.v, kids.n);
    vec_free(&kids);
    return scan;
}

static void put_num(json_t *o, const char *k, double d) {
    if (d == floor(d) && fabs(d) < 9007199254740992.0) json_set(o, k, json_int((int64_t)d));
    else json_set_num(o, k, d);
}

static void put_str_or_null(json_t *o, const char *k, const char *v) { json_set(o, k, v ? json_str(v) : json_null()); }

static json_t *span_json(const span_t *s) {
    json_t *o = json_obj();
    json_set_str(o, "id", s->id);
    json_set_str(o, "kind", KIND_NAMES[s->kind]);
    put_num(o, "start", s->start);
    put_num(o, "end", s->end);
    json_set_str(o, "label", s->label);
    if (s->tool_call_id) json_set_str(o, "toolCallId", s->tool_call_id);
    if (s->detail) json_set_str(o, "detail", s->detail);
    if (s->entry_id) json_set_str(o, "entryId", s->entry_id);
    if (s->child_track_id) json_set_str(o, "childTrackId", s->child_track_id);
    if (s->model) json_set_str(o, "model", s->model);
    if (s->has_tokens) put_num(o, "tokens", s->tokens);
    if (s->has_cost) put_num(o, "cost", s->cost);
    if (s->has_ttft) put_num(o, "ttft", s->ttft);
    if (s->is_error) json_set_bool(o, "isError", true);
    if (s->unterminated) json_set_bool(o, "unterminated", true);
    return o;
}

static void track_free(track_t *t) {
    for (size_t i = 0; i < t->spans.n; i++) span_free(&t->spans.v[i]);
    vec_free(&t->spans);
    for (size_t i = 0; i < t->markers.n; i++) free(t->markers.v[i].label);
    vec_free(&t->markers);
    for (size_t i = 0; i < t->task_results.n; i++) {
        task_result_t *r = &t->task_results.v[i];
        free(r->child_id);
        free(r->agent);
        free(r->task);
        free(r->result_entry_id);
    }
    vec_free(&t->task_results);
    free(t->id);
    free(t->parent_id);
    free(t->label);
    free(t->agent);
    free(t->model);
    free(t->file);
    free(t->cwd);
    free(t->title);
    free(t);
}

typedef struct {
    double s, e;
} iv_t;
static int cmp_iv(const void *x, const void *y) { return sgn(((const iv_t *)x)->s - ((const iv_t *)y)->s); }

typedef struct {
    char *tool;
    double calls, errors, total_ms, max_ms;
} tstat_t;
static int cmp_tstat(const void *x, const void *y) { return sgn(((const tstat_t *)y)->total_ms - ((const tstat_t *)x)->total_ms); }

json_t *session_trace_build(const json_t *root_header, const json_t *root_entries, const json_t *sub_sessions) {
    VEC(char *) keys = {0};
    if (json_is_obj(sub_sessions))
        for (size_t i = 0; i < sub_sessions->u.obj.n; i++) {
            bool dup = false;
            for (size_t k = 0; k < keys.n && !dup; k++) dup = !strcmp(keys.v[k], sub_sessions->u.obj.keys[i]);
            if (!dup) vec_push(&keys, sub_sessions->u.obj.keys[i]);
        }
    if (keys.n) qsort(keys.v, keys.n, sizeof *keys.v, cmp_key);
    trackvec_t tracks = {0};
    track_t *root = visit(root_header, root_entries, "main", NULL, sub_sessions, keys.v, keys.n, &tracks);
    vec_free(&keys);

    /* assembleSessionTrace */
    free(root->label);
    root->label = xstrdup("Main");
    double mtime = root->has_last ? root->last : root->has_header_ts ? root->header_ts : 0;
    bool has_started = false, has_ended = false;
    double started = 0, ended = 0, tool_calls = 0, requests = 0, total_tokens = 0, cost_total = 0, unpriced = 0;
    VEC(iv_t) iv = {0};
    VEC(tstat_t) stats = {0};
    for (size_t ti = 0; ti < tracks.n; ti++) {
        track_t *t = tracks.v[ti];
        requests += t->requests;
        tool_calls += t->tool_calls;
        unpriced += t->unpriced;
        for (size_t k = 0; k < t->spans.n; k++) {
            span_t *s = &t->spans.v[k];
            iv_t v = {s->start, s->end};
            vec_push(&iv, v);
            if (!has_started || s->start < started) started = s->start, has_started = true;
            if (!has_ended || s->end > ended) ended = s->end, has_ended = true;
            if (s->kind == K_MODEL) {
                if (s->has_tokens && s->tokens != 0) total_tokens += s->tokens;
                if (s->has_cost && s->cost != 0) cost_total += s->cost;
            } else if (s->kind == K_TOOL) {
                double dur = s->end - s->start;
                tstat_t *agg = NULL;
                for (size_t q = 0; q < stats.n && !agg; q++)
                    if (!strcmp(stats.v[q].tool, s->label)) agg = &stats.v[q];
                if (agg) {
                    agg->calls++;
                    if (s->is_error) agg->errors++;
                    agg->total_ms += dur;
                    if (dur > agg->max_ms) agg->max_ms = dur;
                } else {
                    tstat_t ts = {xstrdup(s->label), 1, s->is_error ? 1 : 0, dur, dur};
                    vec_push(&stats, ts);
                }
            }
        }
    }
    double model_ms = 0, tool_ms = 0, turns = 0;
    for (size_t k = 0; k < root->spans.n; k++) {
        span_t *s = &root->spans.v[k];
        if (s->kind == K_MODEL) model_ms += s->end - s->start;
        else if (s->kind == K_TOOL) tool_ms += s->end - s->start;
        else if (s->kind == K_TURN) turns++;
    }
    double start = has_started ? started : root->has_header_ts ? root->header_ts : mtime;
    double end = has_ended ? ended : start;
    double wall = end - start;
    double covered = 0;
    if (iv.n) {
        msort(iv.v, iv.n, sizeof *iv.v, cmp_iv);
        double cs = iv.v[0].s, ce = iv.v[0].e;
        for (size_t i = 1; i < iv.n; i++) {
            if (iv.v[i].s > ce) {
                covered += ce - cs;
                cs = iv.v[i].s;
                ce = iv.v[i].e;
            } else if (iv.v[i].e > ce) ce = iv.v[i].e;
        }
        covered += ce - cs;
    }
    vec_free(&iv);
    msort(stats.v, stats.n, sizeof *stats.v, cmp_tstat);

    json_t *o = json_obj();
    json_set_str(o, "file", root->file);
    put_str_or_null(o, "title", root->title);
    put_str_or_null(o, "cwd", root->cwd);
    put_num(o, "startedAt", start);
    put_num(o, "endedAt", end);
    put_num(o, "mtimeMs", mtime);
    json_set_str(o, "etag", "snapshot");
    json_t *ta = json_arr();
    for (size_t ti = 0; ti < tracks.n; ti++) {
        track_t *t = tracks.v[ti];
        json_t *tj = json_obj();
        json_set_str(tj, "id", t->id);
        put_str_or_null(tj, "parentId", t->parent_id);
        json_set_str(tj, "label", t->label);
        put_str_or_null(tj, "agent", t->agent);
        put_str_or_null(tj, "model", t->model);
        json_set_str(tj, "file", t->file);
        json_t *sa = json_arr();
        for (size_t k = 0; k < t->spans.n; k++) json_push(sa, span_json(&t->spans.v[k]));
        json_set(tj, "spans", sa);
        json_t *ma = json_arr();
        for (size_t k = 0; k < t->markers.n; k++) {
            json_t *m = json_obj();
            put_num(m, "time", t->markers.v[k].time);
            json_set_str(m, "kind", t->markers.v[k].kind);
            json_set_str(m, "label", t->markers.v[k].label);
            json_push(ma, m);
        }
        json_set(tj, "markers", ma);
        json_push(ta, tj);
    }
    json_set(o, "tracks", ta);
    json_t *sum = json_obj();
    put_num(sum, "wallMs", wall);
    put_num(sum, "modelMs", model_ms);
    put_num(sum, "toolMs", tool_ms);
    put_num(sum, "idleMs", fmax(0, wall - covered));
    put_num(sum, "turns", turns);
    put_num(sum, "requests", requests);
    put_num(sum, "toolCalls", tool_calls);
    put_num(sum, "subagents", (double)tracks.n - 1);
    put_num(sum, "totalTokens", total_tokens);
    put_num(sum, "costTotal", cost_total);
    put_num(sum, "unpricedRequests", unpriced);
    json_t *sa = json_arr();
    for (size_t i = 0; i < stats.n; i++) {
        json_t *s = json_obj();
        json_set_str(s, "tool", stats.v[i].tool);
        put_num(s, "calls", stats.v[i].calls);
        put_num(s, "errors", stats.v[i].errors);
        put_num(s, "totalMs", stats.v[i].total_ms);
        put_num(s, "maxMs", stats.v[i].max_ms);
        json_push(sa, s);
        free(stats.v[i].tool);
    }
    vec_free(&stats);
    json_set(sum, "toolStats", sa);
    json_set(o, "summary", sum);
    for (size_t ti = 0; ti < tracks.n; ti++) track_free(tracks.v[ti]);
    vec_free(&tracks);
    return o;
}
