/* thinkprobe.c - thinking-capability probes and their cache: ports packages/ai/src/thinking-probe.ts and
 * packages/catalog/src/thinking-capability.ts. omp keeps the verdicts in the `thinking_capability` table of models.db
 * (provider_id, api, endpoint, wire_model_id -> capability JSON); omc has no SQLite and keeps the same rows in
 * <agent dir>/thinking-capability.json ({"rows":[{provider_id, api, endpoint, wire_model_id, capability}]}, the capability
 * column the same JSON text omp stores), written atomically under a flock. */
#include "ai/thinkprobe.h"

#include "ai/compat_detect.h"
#include "core/embed.h"
#include "core/fs.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define PROBE_TIMEOUT_MS 45000
#define PROBE_OUTPUT_TOKENS 1024
#define DAY_MS (24LL * 60 * 60 * 1000)

/* THINKING_EFFORTS */
static const char *const EFFORTS[] = {"minimal", "low", "medium", "high", "xhigh", "max"};
#define N_EFFORTS 6

static int effort_index(const char *s) {
    for (int i = 0; s && i < N_EFFORTS; i++)
        if (!strcmp(s, EFFORTS[i])) return i;
    return -1;
}

static ai_effort_t effort_of(int i) { return (ai_effort_t)(EFFORT_MINIMAL + i); }

static bool is_format(const char *s) {
    static const char *const formats[] = {"openai", "openrouter", "zai", "qwen", "qwen-chat-template", "chat-template",
                                          "responses", "anthropic-adaptive", "anthropic-budget", "google-level", "google-budget",
                                          "none"};
    for (size_t i = 0; s && i < ARRAY_LEN(formats); i++)
        if (!strcmp(s, formats[i])) return true;
    return false;
}

/* TOGGLE_FORMATS */
static bool toggle_format(const char *f) { return str_eq(f, "zai") || str_eq(f, "qwen") || str_eq(f, "qwen-chat-template"); }

bool tp_unverified(const json_t *raw) { return json_get_bool(json_get_obj(raw, "thinking"), "unverified", false); }

/* ---------- formatBatches ---------- */

typedef struct {
    const char *const *f;
    size_t n;
} batch_t;

static const char *const B_OPENAI[] = {"openai"};
static const char *const B_CHAT_ALT[] = {"openrouter", "zai", "qwen", "qwen-chat-template", "chat-template"};
static const char *const B_RESPONSES[] = {"responses"};
static const char *const B_ANTH1[] = {"anthropic-adaptive"};
static const char *const B_ANTH2[] = {"anthropic-budget"};
static const char *const B_GOOGLE1[] = {"google-level"};
static const char *const B_GOOGLE2[] = {"google-budget"};

static size_t format_batches(const char *api, batch_t out[2]) {
#define B(i, a) (out[i].f = a, out[i].n = ARRAY_LEN(a))
    if (str_eq(api, "openai-completions") || (str_eq(api, "openrouter") && str_eq(getenv("PI_OPENROUTER_RESPONSES"), "0"))) {
        B(0, B_OPENAI);
        B(1, B_CHAT_ALT);
        return 2;
    }
    if (str_eq(api, "openrouter") || str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses")) {
        B(0, B_RESPONSES);
        return 1;
    }
    if (str_eq(api, "anthropic-messages")) {
        B(0, B_ANTH1);
        B(1, B_ANTH2);
        return 2;
    }
    if (str_eq(api, "google-generative-ai") || str_eq(api, "google-vertex") || str_eq(api, "google-gemini-cli")) {
        B(0, B_GOOGLE1);
        B(1, B_GOOGLE2);
        return 2;
    }
#undef B
    return 0;
}

/* ---------- capability shape (isCapability) ---------- */

static bool safe_nonneg_int(const json_t *v) {
    return json_is_num(v) && v->is_int && v->u.num.i >= 0 && v->u.num.i <= 9007199254740991LL;
}

static bool attempt_valid(const json_t *a) {
    if (!json_is_obj(a) || !is_format(json_get_str(a, "format"))) return false;
    const json_t *e = json_get(a, "effort");
    if (e && effort_index(json_sv(e)) < 0) return false;
    const json_t *acc = json_get(a, "accepted"), *obs = json_get(a, "observed");
    if (!json_is_bool(acc) || !json_is_bool(obs)) return false;
    return !obs->u.b || acc->u.b;
}

bool tp_capability_valid(const json_t *cap) {
    if (!json_is_obj(cap) || !safe_nonneg_int(json_get(cap, "checkedAt"))) return false;
    const char *format = json_get_str(cap, "format");
    if (!is_format(format) || !json_is_bool(json_get(cap, "observed"))) return false;
    const json_t *efforts = json_get(cap, "efforts"), *e;
    if (!json_is_arr(efforts)) return false;
    int prev = -1;
    json_arr_foreach(efforts, i, e) {
        int k = effort_index(json_sv(e));
        if (k <= prev) return false; /* unknown, repeated or out of THINKING_EFFORTS order */
        prev = k;
    }
    if (str_eq(format, "none") ? json_len(efforts) != 0 : json_len(efforts) == 0) return false;
    const json_t *attempts = json_get(cap, "attempts"), *a;
    if (!json_is_arr(attempts)) return false;
    json_arr_foreach(attempts, j, a) if (!attempt_valid(a)) return false;
    return true;
}

/* ---------- cache key ---------- */

/* new URL(baseUrl) with username, password, search and hash cleared, toString(), trailing slashes dropped */
char *tp_endpoint(const char *base_url) {
    if (!base_url) return NULL;
    const char *colon = strchr(base_url, ':');
    if (!colon || colon == base_url) return NULL;
    for (const char *p = base_url; p < colon; p++)
        if (!(((*p | 32) >= 'a' && (*p | 32) <= 'z') || (p > base_url && ((*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.'))))
            return NULL;
    for (const char *p = base_url; *p; p++)
        if ((unsigned char)*p <= ' ') return NULL;
    char *scheme_raw = xstrndup(base_url, (size_t)(colon - base_url)), *scheme = str_lower(scheme_raw);
    free(scheme_raw);
    bool special = str_eq(scheme, "http") || str_eq(scheme, "https") || str_eq(scheme, "ws") || str_eq(scheme, "wss") ||
                   str_eq(scheme, "ftp") || str_eq(scheme, "file");
    const char *rest = colon + 1;
    sb_t out = SB_INIT;
    sb_append(&out, scheme);
    sb_putc(&out, ':');
    const char *end = rest + strcspn(rest, "?#");
    if (str_starts(rest, "//") || special) {
        while (*rest == '/' || (special && *rest == '\\')) rest++;
        const char *auth_end = rest + strcspn(rest, "/\\?#");
        if (auth_end > end) auth_end = end;
        const char *host = rest;
        for (const char *p = rest; p < auth_end; p++)
            if (*p == '@') host = p + 1; /* userinfo dropped */
        if (host == auth_end && special && !str_eq(scheme, "file")) {
            free(scheme);
            sb_free(&out);
            return NULL; /* special URLs need a host */
        }
        const char *port = NULL;
        for (const char *p = auth_end; p > host; p--)
            if (p[-1] == ':') {
                port = p;
                break;
            } else if (p[-1] == ']') break;
        const char *host_end = port ? port - 1 : auth_end;
        sb_append(&out, "//");
        for (const char *p = host; p < host_end; p++) sb_putc(&out, special && *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p);
        if (port) {
            for (const char *p = port; p < auth_end; p++)
                if (*p < '0' || *p > '9') {
                    free(scheme);
                    sb_free(&out);
                    return NULL;
                }
            long n = port < auth_end ? strtol(port, NULL, 10) : -1;
            bool dflt = n < 0 || (str_eq(scheme, "http") && n == 80) || (str_eq(scheme, "https") && n == 443) ||
                        (str_eq(scheme, "ws") && n == 80) || (str_eq(scheme, "wss") && n == 443) || (str_eq(scheme, "ftp") && n == 21);
            if (n > 65535) {
                free(scheme);
                sb_free(&out);
                return NULL;
            }
            if (!dflt) sb_appendf(&out, ":%ld", n);
        }
        rest = auth_end;
        if (special && rest == end) sb_putc(&out, '/');
    }
    for (const char *p = rest; p < end; p++) sb_putc(&out, special && *p == '\\' ? '/' : *p);
    free(scheme);
    char *s = sb_detach(&out);
    size_t n = strlen(s);
    while (n && s[n - 1] == '/') s[--n] = 0;
    return s;
}

typedef struct {
    const char *provider, *api, *wire;
    char *endpoint;
} tp_key_t;

static bool key_of(const json_t *raw, tp_key_t *k) {
    k->provider = json_get_str(raw, "provider");
    k->api = json_get_str(raw, "api");
    k->wire = json_get_str(raw, "requestModelId");
    if (!k->wire) k->wire = json_get_str(raw, "id");
    k->endpoint = tp_endpoint(json_get_str(raw, "baseUrl"));
    if (!k->provider || !k->api || !k->wire || !k->endpoint) {
        free(k->endpoint);
        return false;
    }
    return true;
}

static bool row_matches(const json_t *row, const tp_key_t *k) {
    return str_eq(json_get_str(row, "provider_id"), k->provider) && str_eq(json_get_str(row, "api"), k->api) &&
           str_eq(json_get_str(row, "endpoint"), k->endpoint) && str_eq(json_get_str(row, "wire_model_id"), k->wire);
}

/* ---------- cache file ---------- */

static char *cache_path(void) { return omc_agent_path("thinking-capability.json"); }

static json_t *cache_load(const char *path) {
    char *s = fs_read_limit(path, 16u << 20, NULL, NULL);
    json_t *j = s ? json_parse_str(s) : NULL;
    free(s);
    if (!json_is_obj(j) || !json_is_arr(json_get(j, "rows"))) {
        json_free(j);
        j = json_obj();
        json_set(j, "rows", json_arr());
    }
    return j;
}

json_t *tp_read(const json_t *raw) {
    tp_key_t k;
    if (!key_of(raw, &k)) return NULL; /* an unresolved runtime endpoint cannot share learned metadata */
    char *path = cache_path();
    json_t *cap = NULL;
    if (fs_is_file(path)) {
        json_t *j = cache_load(path), *row;
        json_arr_foreach(json_get_arr(j, "rows"), i, row) {
            if (!row_matches(row, &k)) continue;
            const char *text = json_get_str(row, "capability");
            json_t *v = text ? json_parse_str(text) : NULL;
            if (tp_capability_valid(v)) {
                int64_t age = now_ms() - json_get_int(v, "checkedAt", 0);
                int64_t ttl = str_eq(json_get_str(v, "format"), "none") ? DAY_MS : 30 * DAY_MS;
                if (age >= 0 && age < ttl) cap = v, v = NULL;
            }
            json_free(v);
            break;
        }
        json_free(j);
    }
    free(path);
    free(k.endpoint);
    return cap;
}

void tp_write(const json_t *raw, const json_t *cap) {
    tp_key_t k;
    if (!tp_capability_valid(cap) || !key_of(raw, &k)) return;
    char *path = cache_path(), *lp = xasprintf("%s.lock", path), *dir = path_dirname(path);
    fs_mkdir_p(dir, 0700);
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) flock(fd, LOCK_EX);
    json_t *j = cache_load(path), *rows = json_get_arr(j, "rows"), *row, *hit = NULL;
    json_arr_foreach(rows, i, row) if (!hit && row_matches(row, &k)) hit = row;
    if (!hit) {
        hit = json_obj();
        json_set_str(hit, "provider_id", k.provider);
        json_set_str(hit, "api", k.api);
        json_set_str(hit, "endpoint", k.endpoint);
        json_set_str(hit, "wire_model_id", k.wire);
        json_push(rows, hit);
    }
    json_set(hit, "capability", json_str_own(json_dump_js(cap))); /* JSON.stringify(capability) */
    char *text = json_dump(j, 0);
    if (fs_write_atomic(path, text, strlen(text), 0644) != 0) LOGW("thinking capability cache %s: %s", path, strerror(errno));
    free(text);
    json_free(j);
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
    free(lp);
    free(dir);
    free(path);
    free(k.endpoint);
}

/* ---------- applyThinkingCapability ---------- */

static json_t *identity_map(const json_t *efforts) {
    json_t *m = json_obj(), *e;
    json_arr_foreach(efforts, i, e) json_set_str(m, json_sv(e), json_sv(e));
    return m;
}

/* openAIOverrides (owned or NULL) */
static json_t *openai_overrides(const json_t *cap) {
    const char *format = json_get_str(cap, "format");
    json_t *c = json_obj();
    if (str_eq(format, "none")) {
        json_set_bool(c, "supportsReasoningParams", false);
        json_set_bool(c, "supportsReasoningEffort", false);
        json_set_bool(c, "omitReasoningEffort", true);
        json_set_str(c, "reasoningDisableMode", "omit");
        json_set_bool(c, "trustExplicitThinkingOnly", true);
        return c;
    }
    json_t *map = identity_map(json_get_arr(cap, "efforts"));
    if (str_eq(format, "responses")) {
        json_set_bool(c, "supportsReasoningParams", true);
        json_set_bool(c, "supportsReasoningEffort", true);
        json_set_bool(c, "omitReasoningEffort", false);
        json_set(c, "reasoningEffortMap", map);
        return c;
    }
    if (!str_eq(format, "openai") && !str_eq(format, "openrouter") && !toggle_format(format) && !str_eq(format, "chat-template")) {
        json_free(map);
        json_free(c);
        return NULL;
    }
    bool effort_accepted = false;
    const json_t *a;
    json_arr_foreach(json_get_arr(cap, "attempts"), i, a) {
        if (str_eq(json_get_str(a, "format"), format) && json_get_bool(a, "accepted", false) && json_get_str(a, "effort"))
            effort_accepted = true;
    }
    bool toggle_only = toggle_format(format) && !effort_accepted;
    const char *disable = str_eq(format, "openrouter")           ? "openrouter-enabled-false"
                          : str_eq(format, "zai")                ? "zai-thinking-disabled"
                          : str_eq(format, "qwen")               ? "qwen-enable-thinking-false"
                          : str_eq(format, "qwen-chat-template") ? "qwen-template-false"
                          : str_eq(format, "chat-template")      ? "chat-template-thinking-false"
                                                                 : "omit";
    json_set_str(c, "thinkingFormat", format);
    json_set_str(c, "reasoningDisableMode", disable);
    json_set_bool(c, "supportsReasoningParams", true);
    json_set_bool(c, "supportsReasoningEffort", !toggle_only);
    json_set_bool(c, "omitReasoningEffort", toggle_only);
    json_set(c, "reasoningEffortMap", map);
    if (str_eq(format, "qwen") || str_eq(format, "qwen-chat-template")) json_set_bool(c, "qwenTemplateReasoningEffort", effort_accepted);
    if (str_eq(format, "zai")) json_set_bool(c, "zaiReasoningEffortDialect", effort_accepted);
    return c;
}

/* learnedThinking (owned or NULL) */
static json_t *learned_thinking(const json_t *cap) {
    const char *format = json_get_str(cap, "format");
    if (str_eq(format, "none")) return NULL;
    const char *mode = str_eq(format, "anthropic-adaptive")                                  ? "anthropic-adaptive"
                       : str_eq(format, "google-level")                                      ? "google-level"
                       : str_eq(format, "anthropic-budget") || str_eq(format, "google-budget") ? "budget"
                                                                                               : "effort";
    json_t *t = json_obj();
    json_set_str(t, "mode", mode);
    json_set(t, "efforts", json_clone(json_get(cap, "efforts")));
    json_set(t, "effortMap", identity_map(json_get_arr(cap, "efforts")));
    if (str_eq(format, "google-budget")) { /* GOOGLE_THINKING_BUDGETS */
        static const int64_t budgets[N_EFFORTS] = {1024, 4096, 8192, 16384, 24575, 32768};
        json_t *b = json_obj();
        for (int i = 0; i < N_EFFORTS; i++) json_set_int(b, EFFORTS[i], budgets[i]);
        json_set(t, "effortBudgets", b);
    }
    return t;
}

json_t *tp_apply_raw(const json_t *raw, const json_t *cap) {
    if (!tp_unverified(raw) || !tp_capability_valid(cap)) return NULL;
    const char *api = json_get_str(raw, "api"), *format = json_get_str(cap, "format");
    bool openai = str_eq(api, "openai-completions") || str_eq(api, "openrouter") || str_eq(api, "openai-responses") ||
                  str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses");
    json_t *over = NULL;
    if (openai) over = openai_overrides(cap);
    else if (str_eq(api, "anthropic-messages") && str_eq(format, "anthropic-adaptive")) {
        over = json_obj();
        json_set_bool(over, "disableAdaptiveThinking", false);
        json_set_bool(over, "supportsOutputEffort", true);
    } else if (str_eq(api, "anthropic-messages") && str_eq(format, "none")) {
        over = json_obj();
        json_set_str(over, "disabledThinking", "omit");
        json_set_bool(over, "supportsOutputEffort", false);
    }
    /* toModelSpec: the authored compat; the provisional ladder and its derived reasoning are not authored input */
    json_t *o = json_clone(raw);
    json_t *compat = json_get_obj(o, "compatConfig") ? json_clone(json_get(o, "compatConfig")) : NULL;
    json_del(o, "compat");
    json_del(o, "compatConfig");
    json_del(o, "thinking");
    if (over) {
        if (!compat) compat = json_obj();
        const char *k;
        json_t *v;
        json_obj_foreach(over, i, k, v) json_set(compat, k, json_clone(v)); /* Object.assign: shallow */
        json_free(over);
    }
    if (compat) json_set(o, "compat", compat);
    json_set_bool(o, "reasoning", !str_eq(format, "none") || json_get_bool(cap, "observed", false));
    json_t *thinking = learned_thinking(cap);
    if (thinking) json_set(o, "thinking", json_clone(thinking));
    /* resolved limits, prices, kind and prompt cache stay as the row already has them */
    static const char *const keep[] = {"cost", "contextWindow", "maxTokens", "kind", "promptCache"};
    json_t *kept[ARRAY_LEN(keep)];
    for (size_t i = 0; i < ARRAY_LEN(keep); i++) kept[i] = json_get(raw, keep[i]) ? json_clone(json_get(raw, keep[i])) : NULL;
    compat_resolve_model(o);
    for (size_t i = 0; i < ARRAY_LEN(keep); i++)
        if (kept[i]) json_set(o, keep[i], kept[i]);
    /* learned controls are exact, not inputs for further rule-owned ladder expansion */
    if (thinking) json_set(o, "thinking", thinking);
    else json_del(o, "thinking");
    return o;
}

json_t *tp_learned_row(const json_t *raw) {
    if (!tp_unverified(raw)) return NULL;
    json_t *cap = tp_read(raw);
    json_t *o = cap ? tp_apply_raw(raw, cap) : NULL;
    json_free(cap);
    return o;
}

/* hypothesis(format) */
static json_t *hypothesis(const char *format) {
    json_t *c = json_obj();
    json_set_int(c, "checkedAt", now_ms());
    json_set_str(c, "format", format);
    json_t *e = json_arr();
    if (toggle_format(format)) json_push_str(e, "high");
    else
        for (int i = 0; i < N_EFFORTS; i++) json_push_str(e, EFFORTS[i]);
    json_set(c, "efforts", e);
    json_set_bool(c, "observed", false);
    json_set(c, "attempts", json_arr());
    return c;
}

/* clampThinkingLevelForModel */
static ai_effort_t clamp_for(const ai_model_t *m, ai_effort_t requested) {
    if (!m->reasoning || requested == EFFORT_OFF) return EFFORT_OFF;
    const json_t *levels = json_get_arr(m->thinking, "efforts"), *e;
    int req = (int)requested - EFFORT_MINIMAL;
    json_arr_foreach(levels, i, e) if (effort_index(json_sv(e)) == req) return requested;
    const json_t *map = json_get_obj(m->thinking, "effortMap");
    const char *alias = json_get_str(map, EFFORTS[req]);
    int ai = effort_index(alias);
    if (ai >= 0) {
        bool listed = false;
        json_arr_foreach(levels, j, e) if (effort_index(json_sv(e)) == ai) listed = true;
        const char *self = json_get_str(map, alias);
        if (listed && (!self || !strcmp(self, alias))) return effort_of(ai);
    }
    int clamped = -1, first = -1;
    json_arr_foreach(levels, k, e) {
        int x = effort_index(json_sv(e));
        if (first < 0) first = x;
        if (x > req) break;
        clamped = x;
    }
    if (clamped < 0) clamped = first;
    return clamped < 0 ? EFFORT_OFF : effort_of(clamped);
}

/* ---------- probes ---------- */

typedef struct {
    const char *format;
    int effort; /* EFFORTS index, -1 for none (toggle formats and the control) */
    ai_model_t *model;
    ai_stream_opts_t o;
    volatile sig_atomic_t cancel;
    int payloads;
    int verdict; /* -1 inconclusive, 0 rejected, 1 accepted */
    bool observed, done;
    pthread_mutex_t *mu;
    pthread_cond_t *cv;
} attempt_t;

typedef struct flight {
    char *key;
    json_t *raw;       /* the unverified row (owned) */
    ai_model_t *model; /* over raw */
    char *api_key;
    json_t *headers, *max_in_flight;
    ai_effort_t requested;
    volatile sig_atomic_t cancel; /* every waiter left */
    int refs, waiters;
    bool done;
    json_t *cap; /* the verdict, NULL when inconclusive or aborted */
    pthread_cond_t cv;
    struct flight *next;
} flight_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static flight_t *g_flights;

static void on_probe_payload(void *ud, const json_t *payload) {
    attempt_t *a = ud;
    (void)payload;
    /* validation retries would turn a rejected tier into a false positive: a second dispatch aborts the probe */
    if (++a->payloads > 1) a->cancel = 1;
}

static void *attempt_main(void *arg) {
    attempt_t *a = arg;
    size_t plen = 0;
    const char *prompt = embed_get("prompts/ai/prompts/thinking-probe.md", &plen);
    json_t *msgs = json_arr(), *u = json_obj();
    json_set_str(u, "role", "user");
    json_set(u, "content", json_strn(prompt ? prompt : "", prompt ? plen : 0));
    json_set_int(u, "timestamp", now_ms());
    json_push(msgs, u);
    ai_context_t ctx = {.messages = msgs};
    json_t *r = ai_stream_dispatch(a->model, &ctx, &a->o, NULL, NULL);
    int64_t status = json_get_int(r, "errorStatus", 0);
    const char *stop = json_get_str(r, "stopReason");
    if (status == 400 || status == 422) {
        a->verdict = 0;
    } else if (str_eq(stop, "error") || str_eq(stop, "aborted")) {
        a->verdict = -1;
    } else {
        a->verdict = 1;
        a->observed = json_get_int(json_get_obj(r, "usage"), "reasoningTokens", 0) > 0;
        const json_t *b;
        json_arr_foreach(json_get_arr(r, "content"), i, b) {
            if (!str_eq(json_get_str(b, "type"), "thinking")) continue;
            const char *th = json_get_str(b, "thinking"), *sig = json_get_str(b, "thinkingSignature");
            if ((th && *th) || (sig && *sig)) a->observed = true;
        }
    }
    json_free(r);
    json_free(msgs);
    pthread_mutex_lock(a->mu);
    a->done = true;
    pthread_cond_broadcast(a->cv);
    pthread_mutex_unlock(a->mu);
    return NULL;
}

/* the candidate model of one attempt: the format's hypothesis applied, or the neutral view for the control */
static ai_model_t *candidate(const flight_t *f, const char *format) {
    json_t *raw;
    if (str_eq(format, "none")) { /* withoutUnverifiedThinking */
        raw = json_clone(f->raw);
        json_del(raw, "thinking");
        json_set_bool(raw, "reasoning", false);
        return catalog_model_variant(f->model, raw, false);
    }
    json_t *h = hypothesis(format);
    raw = tp_apply_raw(f->raw, h);
    json_free(h);
    return raw ? catalog_model_variant(f->model, raw, true) : NULL;
}

static void attempt_opts(const flight_t *f, attempt_t *a) {
    ai_stream_opts_init(&a->o);
    a->o.api_key = f->api_key;
    a->o.headers = f->headers;
    a->o.max_in_flight = f->max_in_flight;
    a->o.reasoning = str_eq(a->format, "none") ? EFFORT_OFF : a->effort >= 0 ? effort_of(a->effort) : EFFORT_HIGH;
    a->o.max_tokens = PROBE_OUTPUT_TOKENS;
    a->o.max_http_attempts = 1;
    a->o.single_http_attempt = true;
    a->o.max_retry_delay_ms = -1; /* maxRetryDelayMs: 0 -> no wait is ever taken */
    a->o.first_event_timeout_ms = PROBE_TIMEOUT_MS;
    a->o.idle_timeout_ms = PROBE_TIMEOUT_MS;
    a->o.cache_retention = "none";
    a->o.stateful_responses = -1;
    a->o.prefer_websockets = -1;
    a->o.accept_empty_response = true;
    a->o.cancel = &a->cancel;
    a->o.on_payload = on_probe_payload;
    a->o.payload_ud = a;
}

/* run one batch's attempts in parallel; false when the flight was aborted */
static bool run_batch(flight_t *f, attempt_t *at, size_t n) {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    pthread_t *th = xcalloc(n, sizeof *th);
    bool *started = xcalloc(n, sizeof *started);
    for (size_t i = 0; i < n; i++) {
        at[i].mu = &mu;
        at[i].cv = &cv;
        at[i].verdict = -1;
        if (!at[i].model) { /* nothing to send in this format: inconclusive */
            at[i].done = true;
            continue;
        }
        attempt_opts(f, &at[i]);
        started[i] = pthread_create(&th[i], NULL, attempt_main, &at[i]) == 0;
        if (!started[i]) attempt_main(&at[i]);
    }
    int64_t deadline = mono_ms() + PROBE_TIMEOUT_MS;
    pthread_mutex_lock(&mu);
    for (;;) {
        size_t done = 0;
        for (size_t i = 0; i < n; i++) done += at[i].done;
        if (done == n) break;
        bool stop = f->cancel || mono_ms() >= deadline;
        for (size_t i = 0; stop && i < n; i++) at[i].cancel = 1; /* AbortSignal.timeout / the flight's controller */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&cv, &mu, &ts);
    }
    pthread_mutex_unlock(&mu);
    for (size_t i = 0; i < n; i++)
        if (started[i]) pthread_join(th[i], NULL);
    free(th);
    free(started);
    return !f->cancel;
}

static json_t *attempt_json(const attempt_t *a) {
    json_t *o = json_obj();
    json_set_str(o, "format", a->format);
    if (a->effort >= 0) json_set_str(o, "effort", EFFORTS[a->effort]);
    json_set_bool(o, "accepted", a->verdict == 1);
    json_set_bool(o, "observed", a->verdict == 1 && a->observed);
    return o;
}

/* probe(): the capability, or NULL when inconclusive or aborted */
static json_t *probe(flight_t *f) {
    batch_t batches[2];
    size_t nb = format_batches(f->model->api, batches);
    int req = f->requested == EFFORT_OFF ? 3 : (int)f->requested - EFFORT_MINIMAL;
    int order[N_EFFORTS], no = 0;
    order[no++] = req;
    for (int i = 0; i < N_EFFORTS; i++)
        if (i != req) order[no++] = i;
    json_t *attempts = json_arr();
    int baseline = -1; /* the reasoning-off control's verdict, shared by the first batch */
    bool baseline_observed = false;
    json_t *cap = NULL;
    for (size_t bi = 0; bi < nb && !cap; bi++) {
        size_t n = 0;
        for (size_t k = 0; k < batches[bi].n; k++) n += toggle_format(batches[bi].f[k]) ? 1 : N_EFFORTS;
        if (bi == 0) n++;
        attempt_t *at = xcalloc(n, sizeof *at);
        size_t j = 0;
        for (size_t k = 0; k < batches[bi].n; k++) {
            const char *fmt = batches[bi].f[k];
            ai_model_t *shared = candidate(f, fmt);
            if (toggle_format(fmt)) {
                at[j].format = fmt;
                at[j].effort = -1;
                at[j++].model = shared;
                continue;
            }
            for (int e = 0; e < N_EFFORTS; e++) {
                at[j].format = fmt;
                at[j].effort = order[e];
                at[j++].model = e == 0 ? shared : shared ? catalog_model_variant(shared, json_clone(shared->raw), false) : NULL;
            }
        }
        if (bi == 0) {
            at[j].format = "none";
            at[j].effort = -1;
            at[j++].model = candidate(f, "none");
        }
        bool ok = run_batch(f, at, n);
        bool inconclusive = false;
        for (size_t i = 0; i < n; i++) {
            if (at[i].verdict < 0) inconclusive = true;
            catalog_model_free(at[i].model);
        }
        /* a quota, auth, transport or timeout failure is not evidence against a tier: never save a truncated ladder */
        if (!ok || inconclusive) {
            free(at);
            json_free(attempts);
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            json_push(attempts, attempt_json(&at[i]));
            if (str_eq(at[i].format, "none")) baseline = at[i].verdict, baseline_observed = at[i].observed;
        }
        free(at);
        for (size_t k = 0; k < batches[bi].n && !cap; k++) {
            const char *fmt = batches[bi].f[k];
            bool accepted[N_EFFORTS] = {0}, any = false, observed = false;
            const json_t *a;
            json_arr_foreach(attempts, x, a) {
                if (!str_eq(json_get_str(a, "format"), fmt) || !json_get_bool(a, "accepted", false)) continue;
                any = true;
                if (json_get_bool(a, "observed", false)) observed = true;
                int e = effort_index(json_get_str(a, "effort"));
                if (e >= 0) accepted[e] = true;
            }
            if (!any) continue;
            cap = json_obj();
            json_set_int(cap, "checkedAt", now_ms());
            json_set_str(cap, "format", fmt);
            json_t *ef = json_arr();
            if (toggle_format(fmt)) json_push_str(ef, "high");
            else
                for (int e = 0; e < N_EFFORTS; e++)
                    if (accepted[e]) json_push_str(ef, EFFORTS[e]);
            json_set(cap, "efforts", ef);
            json_set_bool(cap, "observed", observed);
            json_set(cap, "attempts", json_clone(attempts));
        }
    }
    if (!cap && baseline == 1) {
        cap = json_obj();
        json_set_int(cap, "checkedAt", now_ms());
        json_set_str(cap, "format", "none");
        json_set(cap, "efforts", json_arr());
        json_set_bool(cap, "observed", baseline_observed);
        json_set(cap, "attempts", json_clone(attempts));
    }
    json_free(attempts);
    return cap;
}

static void flight_release(flight_t *f) { /* g_mu held */
    if (--f->refs > 0) return;
    pthread_cond_destroy(&f->cv);
    catalog_model_free(f->model);
    json_free(f->cap);
    json_free(f->headers);
    json_free(f->max_in_flight);
    free(f->api_key);
    free(f->key);
    free(f);
}

static void *flight_main(void *arg) {
    flight_t *f = arg;
    json_t *cap = probe(f);
    if (cap && !f->cancel) {
        tp_write(f->raw, cap);
        char *ef = json_dump(json_get(cap, "efforts"), 0);
        LOGD("Learned thinking capability: provider=%s model=%s format=%s efforts=%s observed=%s", f->model->provider, f->model->id,
             json_get_str(cap, "format"), ef, json_get_bool(cap, "observed", false) ? "true" : "false");
        free(ef);
    } else if (f->cancel) {
        json_free(cap);
        cap = NULL;
    }
    pthread_mutex_lock(&g_mu);
    for (flight_t **p = &g_flights; *p; p = &(*p)->next)
        if (*p == f) {
            *p = f->next;
            break;
        }
    f->cap = cap;
    f->done = true;
    pthread_cond_broadcast(&f->cv);
    flight_release(f);
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

/* learn(): the cached or freshly probed capability (owned or NULL); *aborted when the caller cancelled first */
static json_t *learn(const ai_model_t *m, const ai_stream_opts_t *o, bool *aborted) {
    *aborted = false;
    json_t *cached = tp_read(m->raw);
    if (cached) return cached;
    const char *wire = json_get_str(m->raw, "requestModelId");
    char *key = xasprintf("%s\x1f%s\x1f%s\x1f%s", m->provider, m->api, m->base_url ? m->base_url : "", wire ? wire : m->id);
    pthread_mutex_lock(&g_mu);
    flight_t *f = g_flights;
    while (f && strcmp(f->key, key)) f = f->next;
    if (!f) {
        f = xcalloc(1, sizeof *f);
        f->key = key;
        key = NULL;
        f->raw = json_clone(m->raw);
        f->model = catalog_model_variant(m, f->raw, false);
        f->api_key = o->api_key ? xstrdup(o->api_key) : NULL;
        f->headers = o->headers ? json_clone(o->headers) : NULL;
        f->max_in_flight = o->max_in_flight ? json_clone(o->max_in_flight) : NULL;
        f->requested = o->reasoning;
        pthread_cond_init(&f->cv, NULL);
        f->refs = 1; /* the worker */
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, flight_main, f) != 0) {
            pthread_attr_destroy(&attr);
            f->refs = 0;
            flight_release(f);
            pthread_mutex_unlock(&g_mu);
            free(key);
            return NULL;
        }
        pthread_attr_destroy(&attr);
        f->next = g_flights;
        g_flights = f;
    }
    free(key);
    f->refs++;
    f->waiters++;
    while (!f->done) {
        if (o->cancel && *o->cancel) { /* untilAborted: this caller leaves; the last one aborts the probes */
            *aborted = true;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&f->cv, &g_mu, &ts);
    }
    json_t *cap = !*aborted && f->cap ? json_clone(f->cap) : NULL;
    if (--f->waiters == 0 && !f->done) f->cancel = 1;
    flight_release(f);
    pthread_mutex_unlock(&g_mu);
    return cap;
}

json_t *tp_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb, void *ud) {
    batch_t batches[2];
    size_t nb = format_batches(m->api, batches);
    if (!nb || o->reasoning == EFFORT_OFF || o->disable_reasoning || o->force_reasoning_off || !tp_unverified(m->raw))
        return ai_stream_dispatch(m, ctx, o, cb, ud);
    bool aborted;
    json_t *cap = learn(m, o, &aborted);
    if (aborted || (o->cancel && *o->cancel)) {
        json_free(cap);
        json_t *msg = ai_error_message(m, "aborted", "Request was aborted");
        if (cb) {
            ai_event_t ev = {.type = AI_EV_ERROR, .content_index = -1, .partial = msg, .reason = "aborted"};
            cb(ud, &ev);
        }
        return msg;
    }
    /* inconclusive probes leave the capability unknown: still honor the requested level rather than turning it off */
    json_t *use = cap ? cap : hypothesis(batches[0].f[0]);
    json_t *raw = tp_apply_raw(m->raw, use);
    json_free(use);
    ai_model_t *learned = raw ? catalog_model_variant(m, raw, true) : NULL;
    const ai_model_t *run = learned ? learned : m;
    ai_stream_opts_t lo = *o;
    lo.reasoning = clamp_for(run, o->reasoning);
    json_t *r = ai_stream_dispatch(run, ctx, &lo, cb, ud);
    catalog_model_free(learned);
    return r;
}
