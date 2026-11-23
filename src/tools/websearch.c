/*
 * websearch.c - the web_search tool, port of omp coding-agent web/search/{index,provider}.ts and the
 * credentialed providers under web/search/providers/: anthropic.ts, openai.ts, codex.ts, gemini.ts, xai.ts
 * (hosted search grounding of a chat model) and perplexity.ts (API key), kimi.ts, zai.ts, searxng.ts (search
 * engines). The scraping / plain-API engines (parallel, brave, tavily, exa, duckduckgo) are in web.c.
 *
 * Selection is omp's executeSearch: the `web` model role chain (modelRoles.web, else the catalog defaults
 * web/parallel, web/hosted, web/exa, ...), `web/hosted` expanding to the session model's cheaper webSearchModel
 * swap and the session model itself, xAI candidates ranked by provider priority and moved first for X-only
 * queries. Unavailable candidates are skipped (explicit ones fail), every failure is recorded and the first
 * success is formatted with a fallback note naming the providers that failed before it.
 */
#define _GNU_SOURCE
#include "websearch.h"

#include "ai/google_cca.h"
#include "ai/provider_openai.h"
#include "ai/provider_anthropic.h"
#include "config/auth.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "net/http.h"
#include "prompt/template.h"

#include <ctype.h>
#include <math.h>
#include <strings.h>
#include <time.h>

/* ---------- small helpers ---------- */

static bool str_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static double jnum(const json_t *v) { return json_is_num(v) ? v->u.num.d : 0; }

static bool json_arr_has_str(const json_t *arr, const char *s) {
    json_t *v;
    json_arr_foreach(arr, i, v) if (json_is_str(v) && str_eq(json_sv(v), s)) return true;
    return false;
}

/* ---------- labels, errors, params ---------- */

static const char *const LABELS[][2] = {
    {"parallel", "Parallel"},   {"perplexity", "Perplexity"}, {"gemini", "Gemini"},       {"anthropic", "Anthropic"},
    {"codex", "OpenAI Codex"},  {"openai", "OpenAI API"},     {"xai", "xAI"},             {"openrouter", "OpenRouter"},
    {"zai", "Z.AI"},            {"exa", "Exa"},               {"tinyfish", "TinyFish"},   {"keenable", "Keenable"},
    {"jina", "Jina"},           {"sofya", "Sofya"},
    {"kagi", "Kagi"},           {"tavily", "Tavily"},         {"firecrawl", "Firecrawl"}, {"brave", "Brave"},
    {"deepseek", "DeepSeek"},   {"kimi", "Kimi"},             {"synthetic", "Synthetic"}, {"ollama", "Ollama"},
    {"searxng", "SearXNG"},     {"startpage", "Startpage"},   {"duckduckgo", "DuckDuckGo"}, {"ecosia", "Ecosia"},
    {"google", "Google"},       {"mojeek", "Mojeek"},         {"public", "Public Web"}};

/* getSearchProviderLabel */
static const char *label_of(const char *id) {
    for (size_t i = 0; i < ARRAY_LEN(LABELS); i++)
        if (str_eq(LABELS[i][0], id)) return LABELS[i][1];
    return id;
}

typedef struct {
    char *msg;
    int status;       /* SearchProviderError.status (0 = none) */
    const char *prov; /* SearchProviderError.provider; NULL for a plain Error */
} ws_err_t;

static void fail_typed(ws_err_t *e, const char *prov, int status, char *msg) {
    free(e->msg);
    e->msg = msg;
    e->status = status;
    e->prov = prov;
}
static void fail_plain(ws_err_t *e, char *msg) { fail_typed(e, NULL, 0, msg); }

typedef struct {
    const char *query, *recency, *system_prompt, *session_id;
    double limit, num_results, max_tokens, temperature; /* NAN = unset */
    const ai_model_t *model;
    const char *level; /* thinking selector of the candidate */
    bool explicit_;
    volatile sig_atomic_t *cancel;
    int64_t timeout_ms;
} ws_params_t;

/* numSearchResults ?? limit */
static double want_results(const ws_params_t *p) { return !isnan(p->num_results) ? p->num_results : p->limit; }

/* clampNumResults */
static int clamp_results(double v, int def, int max) {
    if (isnan(v) || v == 0) return def;
    return (int)MIN((double)max, MAX(1.0, v));
}

/* classifyProviderHttpError */
static bool classify_http(ws_err_t *e, const char *prov, int status, const char *body) {
    const char *b = body ? body : "";
    bool credit = str_istr(b, "quota") || str_istr(b, "insufficient");
    for (const char *p = str_istr(b, "credit"); p && !credit; p = str_istr(p + 1, "credit")) {
        const char *q = p + 6;
        if (*q == 's' || *q == 'S') q++;
        while (*q && isspace((unsigned char)*q)) q++;
        if (!strncasecmp(q, "exhausted", 9) || !strncasecmp(q, "exceeded", 8)) credit = true;
    }
    if (credit) fail_typed(e, prov, status, xasprintf("%s: credits exhausted", prov));
    else if (status == 402) fail_typed(e, prov, status, xasprintf("%s: 402 credits exhausted", prov));
    else if (status == 401) fail_typed(e, prov, status, xasprintf("%s: 401 unauthorized", prov));
    else if (status == 403) fail_typed(e, prov, status, xasprintf("%s: 403 forbidden", prov));
    else return false;
    return true;
}

/* one request with the search deadline; OMC_PROVIDER_URL_REWRITE applies (test hook) */
static int ws_http(const char *method, const char *url, const http_headers_t *h, const char *body, const ws_params_t *p,
                   http_resp_t *r) {
    char *u = ai_rewrite_url(url);
    http_req_t req = {.method = method,
                      .url = u,
                      .headers = h,
                      .body = body,
                      .body_len = body ? strlen(body) : 0,
                      .timeout_ms = p->timeout_ms,
                      .cancel = p->cancel};
    int rc = http_request(&req, r);
    free(u);
    return rc;
}

static bool cancelled(const ws_params_t *p) { return p->cancel && *p->cancel; }

/* a transport failure: aborts are reported as such */
static void fail_transport(ws_err_t *e, const ws_params_t *p, const http_resp_t *r) {
    fail_plain(e, xstrdup(cancelled(p) ? "Request was aborted" : (r->error ? r->error : "request failed")));
}

/* ---------- credentials (AuthStorage.keys.source) ---------- */

/* "oauth" | "api_key" | "env" or NULL when the provider has no credential */
static const char *key_kind(const char *provider) {
    if (auth_has_runtime_key(provider)) return "api_key";
    json_t *l = auth_list(), *e;
    const char *kind = NULL;
    json_arr_foreach(l, i, e) {
        if (!str_eq(json_get_str(e, "provider"), provider)) continue;
        kind = str_eq(json_get_str(e, "type"), "oauth") ? "oauth" : "api_key";
    }
    json_free(l);
    if (kind) return kind;
    char *k = ai_env_api_key(provider);
    if (k) kind = "env";
    free(k);
    return kind;
}

static char *env_trimmed(const char *name) {
    const char *v = env_get(name);
    if (!v) return NULL;
    char *t = str_trim(v);
    if (*t) return t;
    free(t);
    return NULL;
}

/* ---------- response builders (SearchResponse JSON) ---------- */

static json_t *resp_new(const char *prov) {
    json_t *r = json_obj();
    json_set_str(r, "provider", prov);
    json_set(r, "sources", json_arr());
    return r;
}

/* dateToAgeSeconds */
static bool age_of(const char *date, int64_t *age) {
    int64_t ms;
    if (!date || !*date || !ws_parse_js_date(date, &ms)) return false;
    int64_t d = (int64_t)time(NULL) * 1000 - ms;
    *age = d >= 0 ? d / 1000 : -((-d + 999) / 1000);
    return true;
}

static json_t *src_new(const char *title, const char *url, const char *snippet, const char *date) {
    json_t *s = json_obj();
    json_set_str(s, "title", title);
    json_set_str(s, "url", url);
    if (snippet) json_set_str(s, "snippet", snippet);
    if (date) json_set_str(s, "publishedDate", date);
    int64_t age;
    if (age_of(date, &age)) json_set_int(s, "ageSeconds", age);
    return s;
}

static void arr_truncate(json_t *arr, size_t n) {
    while (json_len(arr) > n) json_free(json_arr_remove(arr, json_len(arr) - 1));
}

/* reorder keys like omp's SearchResponse object literal so details match */
static json_t *resp_finish(json_t *r) {
    static const char *const order[] = {"provider", "answer",   "sources", "citations", "relatedQuestions",
                                        "searchQueries", "usage", "model", "requestId", "authMode"};
    json_t *o = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(order); i++) {
        json_t *v = json_get(r, order[i]);
        if (v) json_set(o, order[i], json_clone(v));
    }
    json_free(r);
    return o;
}

static void set_str_opt(json_t *o, const char *k, const char *v) {
    if (v && *v) json_set_str(o, k, v);
}

/* SSE `data:` payloads of a buffered body, as parsed JSON (owned array) */
static json_t *sse_events(const char *body) {
    json_t *out = json_arr();
    sb_t data = SB_INIT;
    bool have = false;
    for (const char *p = body ? body : ""; ; ) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (n == 0) {
            if (have) {
                json_t *j = json_parse_str(sb_cstr(&data));
                if (j) json_push(out, j);
            }
            sb_clear(&data);
            have = false;
        } else if (n >= 5 && !strncmp(p, "data:", 5)) {
            size_t k = 5;
            if (k < n && p[k] == ' ') k++;
            if (have) sb_putc(&data, '\n');
            sb_appendn(&data, p + k, n - k);
            have = true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (have) {
        json_t *j = json_parse_str(sb_cstr(&data));
        if (j) json_push(out, j);
    }
    sb_free(&data);
    return out;
}

/* ---------- engines from web.c ---------- */

typedef int (*engine_fn)(const char *, int, const char *, sresults_t *, char **);

static json_t *run_engine(const char *id, engine_fn fn, int def, int max, const ws_params_t *p, ws_err_t *e) {
    sresults_t res = {0};
    char *err = NULL;
    int n = clamp_results(want_results(p), def, max);
    int rc = fn(p->query, n, p->recency, &res, &err);
    if (rc != 0) {
        sres_free(&res);
        if (rc == -2) fail_typed(e, id, 0, xasprintf("%s web search is not configured.", label_of(id)));
        else fail_plain(e, err ? err : xasprintf("%s search failed", label_of(id)));
        if (rc == -2) free(err);
        return NULL;
    }
    free(err);
    json_t *r = resp_new(id), *srcs = json_get(r, "sources");
    for (size_t i = 0; i < res.n; i++)
        json_push(srcs, src_new(res.v[i].title, res.v[i].url, *res.v[i].snippet ? res.v[i].snippet : NULL, res.v[i].date));
    sres_free(&res);
    return r;
}

static json_t *s_parallel(const ws_params_t *p, ws_err_t *e) { return run_engine("parallel", ws_search_parallel, 10, 40, p, e); }
static json_t *s_brave(const ws_params_t *p, ws_err_t *e) { return run_engine("brave", ws_search_brave, 10, 20, p, e); }
static json_t *s_tavily(const ws_params_t *p, ws_err_t *e) { return run_engine("tavily", ws_search_tavily, 5, 20, p, e); }
static json_t *s_exa(const ws_params_t *p, ws_err_t *e) { return run_engine("exa", ws_search_exa, 10, 25, p, e); }
static json_t *s_ddg(const ws_params_t *p, ws_err_t *e) { return run_engine("duckduckgo", ws_search_ddg, 10, 30, p, e); }

static bool always(const ai_model_t *m) { return true; }
static bool has_brave(const ai_model_t *m) { return env_get("BRAVE_API_KEY") != NULL; }
static bool has_tavily(const ai_model_t *m) { return env_get("TAVILY_API_KEY") != NULL; }
static bool has_exa(const ai_model_t *m) { return env_get("EXA_API_KEY") != NULL && settings_bool("exa.enabled", true); }

/* ---------- Anthropic (providers/anthropic.ts) ---------- */

/* parsePageAge: "2 days ago", "3h ago" */
static bool page_age(const char *s, int64_t *out) {
    if (!s || !isdigit((unsigned char)*s)) return false;
    char *e;
    long v = strtol(s, &e, 10);
    while (*e == ' ' || *e == '\t') e++;
    const char *u = e;
    while (isalpha((unsigned char)*e)) e++;
    size_t n = (size_t)(e - u);
    if (!n) return false;
    char unit[16];
    if (n >= sizeof unit) return false;
    for (size_t i = 0; i < n; i++) unit[i] = (char)tolower((unsigned char)u[i]);
    unit[n] = 0;
    static const struct {
        const char *u;
        int64_t s;
    } mul[] = {{"s", 1},    {"sec", 1},     {"second", 1},   {"m", 60},       {"min", 60},     {"minute", 60},
               {"h", 3600}, {"hour", 3600}, {"d", 86400},    {"day", 86400},  {"w", 604800},   {"week", 604800},
               {"mo", 2592000}, {"month", 2592000}, {"y", 31536000}, {"year", 31536000}};
    int64_t m = -1;
    for (size_t i = 0; i < ARRAY_LEN(mul) && m < 0; i++) {
        size_t ul = strlen(mul[i].u);
        if (!strcmp(unit, mul[i].u) || (n == ul + 1 && !strncmp(unit, mul[i].u, ul) && unit[ul] == 's')) m = mul[i].s;
    }
    if (m < 0) return false;
    while (*e == ' ' || *e == '\t') e++;
    if (!strncasecmp(e, "ago", 3)) e += 3;
    if (*e) return false;
    *out = (int64_t)v * m;
    return true;
}

static json_t *s_anthropic(const ws_params_t *p, ws_err_t *e) {
    const ai_model_t *m = p->model;
    char *key = NULL;
    if (str_eq(m->provider, "anthropic")) key = env_trimmed("ANTHROPIC_SEARCH_API_KEY");
    if (!key) key = auth_get_api_key(m->provider);
    if (!key || !*key) {
        free(key);
        fail_plain(e, xasprintf("Anthropic credentials not found for selected provider \"%s\".", m->provider));
        return NULL;
    }
    http_headers_t h = {0};
    char *url = NULL;
    /* the model decides whether to search and answers from its weights when a query reads as stable knowledge; a search
     * backend must always search (omp 88e412881e) */
    char *instr = prompt_load("coding-agent/prompts/system/web-search-anthropic.md");
    char *body = anthropic_search_request(m, key, p->query, p->system_prompt,
                                          isnan(p->max_tokens) ? 0 : (int64_t)p->max_tokens, p->temperature,
                                          p->session_id, instr, &h, &url);
    free(instr);
    http_resp_t r;
    int rc = ws_http("POST", url, &h, body, p, &r);
    free(url);
    free(body);
    free(key);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "anthropic", r.status, r.body))
            fail_typed(e, "anthropic", r.status, xasprintf("Anthropic API error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body);
    http_resp_free(&r);
    if (!j) {
        fail_plain(e, xstrdup("Anthropic API returned invalid JSON"));
        return NULL;
    }
    /* parseResponse */
    json_t *res = resp_new("anthropic"), *srcs = json_get(res, "sources"), *cits = json_arr(), *queries = json_arr();
    strvec_t tool_errors = {0};
    sb_t answer = SB_INIT;
    bool prev_text = false;
    json_t *b;
    json_arr_foreach(json_get_arr(j, "content"), i, b) {
        const char *t = json_get_str(b, "type");
        if (!str_eq(t, "text")) prev_text = false;
        if (str_eq(t, "server_tool_use") && str_eq(json_get_str(b, "name"), "web_search")) {
            const char *q = json_get_str(json_get_obj(b, "input"), "query");
            if (q && *q) json_push_str(queries, q);
        } else if (str_eq(t, "web_search_tool_result") && json_is_obj(json_get(b, "content"))) {
            /* a failed search carries one error object instead of a result list (still an HTTP 200) */
            const char *code = json_get_str(json_get(b, "content"), "error_code");
            if (code && !strvec_contains(&tool_errors, code)) strvec_push(&tool_errors, code);
        } else if (str_eq(t, "web_search_tool_result") && json_is_arr(json_get(b, "content"))) {
            json_t *it;
            json_arr_foreach(json_get(b, "content"), k, it) {
                if (!str_eq(json_get_str(it, "type"), "web_search_result")) continue;
                const char *age = json_get_str(it, "page_age");
                json_t *s = json_obj();
                json_set_str(s, "title", json_get_str(it, "title") ? json_get_str(it, "title") : "");
                json_set_str(s, "url", json_get_str(it, "url") ? json_get_str(it, "url") : "");
                if (age) json_set_str(s, "publishedDate", age);
                int64_t secs;
                if (page_age(age, &secs)) json_set_int(s, "ageSeconds", secs);
                json_push(srcs, s);
            }
        } else if (str_eq(t, "text") && !str_empty(json_get_str(b, "text"))) {
            if (answer.len && !prev_text) sb_append(&answer, "\n\n");
            sb_append(&answer, json_get_str(b, "text"));
            prev_text = true;
            json_t *c;
            json_arr_foreach(json_get_arr(b, "citations"), k, c) {
                json_t *ci = json_obj();
                set_str_opt(ci, "url", json_get_str(c, "url"));
                set_str_opt(ci, "title", json_get_str(c, "title"));
                set_str_opt(ci, "citedText", json_get_str(c, "cited_text"));
                json_push(cits, ci);
            }
        }
    }
    /* a classifier decline (before or mid-output) leaves any text incomplete and Anthropic says to discard it; results it
     * already searched stay usable and the answer says why it is missing */
    const char *model_name = json_get_str(j, "model") ? json_get_str(j, "model") : "";
    char *failure = NULL;
    if (str_eq(json_get_str(j, "stop_reason"), "refusal")) {
        const json_t *sd = json_get_obj(j, "stop_details");
        const char *cat = json_get_str(sd, "category") ? json_get_str(sd, "category") : "uncategorized";
        const char *expl = json_get_str(sd, "explanation");
        char *refusal = expl ? xasprintf("%s declined the query (%s safety classifier): %s", model_name, cat, expl)
                             : xasprintf("%s declined the query (%s safety classifier).", model_name, cat);
        if (json_len(srcs) == 0) failure = refusal;
        else {
            sb_clear(&answer);
            sb_appendf(&answer, "%s The results it found before declining follow.", refusal);
            free(refusal);
            json_free(cits);
            cits = json_arr();
        }
    } else if (json_len(srcs) == 0 && tool_errors.n > 0) {
        char *codes = strvec_join(&tool_errors, ", ");
        failure = xasprintf("Anthropic web search failed: %s.", codes);
        free(codes);
    }
    strvec_free(&tool_errors);
    if (failure) {
        fail_typed(e, "anthropic", 0, failure);
        sb_free(&answer);
        json_free(cits);
        json_free(queries);
        json_free(res);
        json_free(j);
        return NULL;
    }
    if (answer.len) json_set_str(res, "answer", sb_cstr(&answer));
    sb_free(&answer);
    if (json_len(cits)) json_set(res, "citations", cits);
    else json_free(cits);
    if (json_len(queries)) json_set(res, "searchQueries", queries);
    else json_free(queries);
    json_t *u = json_get_obj(j, "usage"), *usage = json_obj();
    if (json_get(u, "input_tokens")) json_set(usage, "inputTokens", json_clone(json_get(u, "input_tokens")));
    if (json_get(u, "output_tokens")) json_set(usage, "outputTokens", json_clone(json_get(u, "output_tokens")));
    json_t *sr = json_get(json_get_obj(u, "server_tool_use"), "web_search_requests");
    if (sr) json_set(usage, "searchRequests", json_clone(sr));
    json_set(res, "usage", usage);
    set_str_opt(res, "model", json_get_str(j, "model"));
    set_str_opt(res, "requestId", json_get_str(j, "id"));
    json_free(j);
    double n = want_results(p);
    if (!isnan(n) && n > 0 && json_len(srcs) > (size_t)n) arr_truncate(srcs, (size_t)n);
    return res;
}

/* AnthropicProvider.isAvailable */
static bool has_anthropic(const ai_model_t *m) {
    return key_kind(m->provider) || (str_eq(m->provider, "anthropic") && env_get("ANTHROPIC_SEARCH_API_KEY"));
}

/* ---------- source/citation dedup shared by the Responses-style providers ---------- */

typedef struct {
    json_t *sources, *citations; /* borrowed arrays */
} srcset_t;

static json_t *find_by_url(json_t *arr, const char *url) {
    json_t *it;
    json_arr_foreach(arr, i, it) if (str_eq(json_get_str(it, "url"), url)) return it;
    return NULL;
}

/* openai.ts addSource: one source + one citation per URL, a real title replaces a URL title, first cited text wins */
static void add_openai_source(srcset_t *s, const char *url_in, const char *title_in, const char *cited_in) {
    if (!url_in) return;
    char *url = str_trim(url_in);
    if (!*url) {
        free(url);
        return;
    }
    char *tt = title_in ? str_trim(title_in) : xstrdup("");
    char *title = *tt ? xstrdup(tt) : xstrdup(url);
    free(tt);
    char *cited = cited_in ? str_trim(cited_in) : NULL;
    if (cited && !*cited) {
        free(cited);
        cited = NULL;
    }
    for (int k = 0; k < 2; k++) {
        json_t *arr = k ? s->citations : s->sources;
        const char *field = k ? "citedText" : "snippet";
        json_t *ex = find_by_url(arr, url);
        if (!ex) {
            json_t *o = json_obj();
            json_set_str(o, "title", title);
            json_set_str(o, "url", url);
            if (cited) json_set_str(o, field, cited);
            json_push(arr, o);
        } else {
            if (str_eq(json_get_str(ex, "title"), url) && strcmp(title, url) != 0) json_set_str(ex, "title", title);
            if (cited && !json_get_str(ex, field)) json_set_str(ex, field, cited);
        }
    }
    free(url);
    free(title);
    free(cited);
}

/* UTF-16 slice [start, end) of a UTF-8 string (JS String.slice) */
static char *js_slice(const char *s, int64_t start, int64_t end) {
    sb_t b = SB_INIT;
    int64_t unit = 0;
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) n = 1;
        int units = cp > 0xFFFF ? 2 : 1;
        if (unit >= start && unit + units <= end) sb_appendn(&b, s + i, (size_t)n);
        unit += units;
        i += (size_t)n;
        if (unit >= end) break;
    }
    return sb_detach(&b);
}

static int64_t js_len(const char *s) {
    int64_t units = 0;
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) n = 1;
        units += cp > 0xFFFF ? 2 : 1;
        i += (size_t)n;
    }
    return units;
}

/* ---------- OpenAI API (providers/openai.ts) ---------- */

static char *openai_error_detail(const char *body) {
    json_t *j = body ? json_parse_str(body) : NULL;
    json_t *er = json_get_obj(j, "error");
    sb_t b = SB_INIT;
    static const char *const fields[] = {"type", "code", "param"};
    for (size_t i = 0; er && i < ARRAY_LEN(fields); i++) {
        const char *v = json_get_str(er, fields[i]);
        char *t = v ? str_trim(v) : NULL;
        if (t && *t) sb_appendf(&b, "%s%s=%s", b.len ? ", " : ": ", fields[i], t);
        free(t);
    }
    json_free(j);
    return sb_detach(&b);
}

static json_t *s_openai(const ws_params_t *p, ws_err_t *e) {
    const ai_model_t *m = p->model;
    if (str_eq(key_kind(m->provider), "oauth")) {
        fail_typed(e, "openai", 0,
                   xasprintf("OpenAI API web search requires API-key credentials; OAuth credentials for \"%s\" are not "
                             "supported.",
                             m->provider));
        return NULL;
    }
    int limit = clamp_results(want_results(p), 10, 100);
    json_t *body = json_obj();
    const char *rid = m->raw ? json_get_str(m->raw, "requestModelId") : NULL;
    json_set_str(body, "model", rid ? rid : m->id);
    json_set_str(body, "instructions", p->system_prompt);
    json_set_str(body, "input", p->query);
    json_t *tools = json_arr(), *tool = json_obj();
    json_set_str(tool, "type", "web_search");
    json_push(tools, tool);
    json_set(body, "tools", tools);
    json_t *ntc = model_compat_get(m, "supportsNamedToolChoice");
    if (json_is_bool(ntc) && !ntc->u.b) json_set_str(body, "tool_choice", "required");
    else {
        json_t *tc = json_obj();
        json_set_str(tc, "type", "web_search");
        json_set(body, "tool_choice", tc);
    }
    json_t *inc = json_arr();
    json_push_str(inc, "web_search_call.action.sources");
    json_set(body, "include", inc);
    json_set_bool(body, "store", false);
    const char *rmode = m->raw ? json_get_str(m->raw, "reasoningMode") : NULL;
    if (rmode) {
        json_t *rs = json_obj();
        json_set_str(rs, "mode", rmode);
        json_set(body, "reasoning", rs);
    }
    if (!isnan(p->max_tokens)) json_set_num(body, "max_output_tokens", p->max_tokens);
    char *key = auth_get_api_key(m->provider);
    if (!key || !*key) {
        free(key);
        json_free(body);
        fail_plain(e, xasprintf("OpenAI API credentials not found for selected provider \"%s\".", m->provider));
        return NULL;
    }
    http_headers_t h = {0};
    ai_apply_headers(&h, m, NULL);
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    http_headers_set(&h, "Content-Type", "application/json");
    const char *sess_hdr = model_compat_str(m, "promptCacheSessionHeader");
    if (sess_hdr && p->session_id && *p->session_id) http_headers_set(&h, sess_hdr, p->session_id);
    char *base = xstrdup(m->base_url ? m->base_url : "https://api.openai.com/v1");
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    char *url = xasprintf("%s/responses", base);
    free(base);
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", url, &h, payload, p, &r);
    free(url);
    free(payload);
    free(key);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        const char *base_msg = r.status == 400   ? "OpenAI Responses API rejected the request (400)"
                               : r.status == 401 ? "OpenAI API key was rejected (401)"
                               : r.status == 402 ? "OpenAI API billing limit reached (402)"
                               : r.status == 403 ? "OpenAI API request forbidden (403)"
                               : r.status == 429 ? "OpenAI API rate limit reached (429)"
                                                 : NULL;
        char *detail = openai_error_detail(r.body);
        char *msg = base_msg ? xasprintf("%s%s", base_msg, detail)
                             : xasprintf("OpenAI Responses API request failed (%d)%s", r.status, detail);
        free(detail);
        fail_typed(e, "openai", r.status, msg);
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body);
    http_resp_free(&r);
    if (!j) {
        fail_typed(e, "openai", 502, xstrdup("OpenAI Responses API returned invalid JSON"));
        return NULL;
    }
    if (!json_is_obj(j)) {
        json_free(j);
        fail_typed(e, "openai", 502, xstrdup("OpenAI Responses API returned an invalid response"));
        return NULL;
    }
    /* readResponse */
    json_t *output = json_get_arr(j, "output"), *item;
    size_t calls = 0, search_requests = 0;
    json_arr_foreach(output, i, item) {
        if (str_eq(json_get_str(item, "type"), "web_search_call") && !str_eq(json_get_str(item, "status"), "failed")) {
            calls++;
            if (str_eq(json_get_str(json_get_obj(item, "action"), "type"), "search")) search_requests++;
        }
    }
    if (!calls) {
        json_free(j);
        fail_typed(e, "openai", 502,
                   xstrdup("OpenAI returned a completion without running web search (no web_search_call); refusing to treat "
                           "a non-search answer as a search result"));
        return NULL;
    }
    srcset_t ss = {json_arr(), json_arr()};
    json_t *queries = json_arr(), *consulted = json_arr();
    json_arr_foreach(output, i, item) {
        if (!str_eq(json_get_str(item, "type"), "web_search_call") || str_eq(json_get_str(item, "status"), "failed")) continue;
        json_t *action = json_get_obj(item, "action");
        const char *q = json_get_str(action, "query");
        char *qt = q ? str_trim(q) : NULL;
        if (qt && *qt && !json_arr_has_str(queries, qt)) json_push_str(queries, qt);
        free(qt);
        json_t *s;
        json_arr_foreach(json_get_arr(action, "sources"), k, s) if (json_is_obj(s)) json_push(consulted, json_clone(s));
    }
    sb_t answer = SB_INIT;
    json_arr_foreach(output, i, item) {
        if (!str_eq(json_get_str(item, "type"), "message") || !json_is_arr(json_get(item, "content"))) continue;
        json_t *part;
        json_arr_foreach(json_get(item, "content"), k, part) {
            const char *text = json_get_str(part, "text");
            if (!text) text = "";
            if (str_eq(json_get_str(part, "type"), "output_text") && !str_blank(text)) {
                char *t = str_trim(text);
                if (answer.len) sb_append(&answer, "\n\n");
                sb_append(&answer, t);
                free(t);
            }
            json_t *an;
            json_arr_foreach(json_get_arr(part, "annotations"), x, an) {
                if (!str_eq(json_get_str(an, "type"), "url_citation")) continue;
                json_t *st = json_get(an, "start_index"), *en = json_get(an, "end_index");
                char *cited = NULL;
                if (json_is_num(st) && json_is_num(en)) {
                    int64_t a = json_d2i(st->u.num.d), z = json_d2i(en->u.num.d);
                    if (a >= 0 && z > a) cited = js_slice(text, a, z);
                }
                add_openai_source(&ss, json_get_str(an, "url"), json_get_str(an, "title"), cited);
                free(cited);
            }
        }
        json_t *an;
        json_arr_foreach(json_get_arr(item, "annotations"), x, an) {
            if (str_eq(json_get_str(an, "type"), "url_citation"))
                add_openai_source(&ss, json_get_str(an, "url"), json_get_str(an, "title"), NULL);
        }
    }
    json_t *s;
    json_arr_foreach(consulted, i, s) add_openai_source(&ss, json_get_str(s, "url"), json_get_str(s, "title"), NULL);
    json_free(consulted);
    char *ans = answer.len ? sb_detach(&answer) : NULL;
    sb_free(&answer);
    if (!ans && json_get_str(j, "output_text")) {
        char *t = str_trim(json_get_str(j, "output_text"));
        if (*t) ans = t;
        else free(t);
    }
    if (!ans && json_len(ss.sources) == 0) {
        json_free(ss.sources);
        json_free(ss.citations);
        json_free(queries);
        json_free(j);
        fail_typed(e, "openai", 502, xstrdup("OpenAI web search returned no answer or sources"));
        return NULL;
    }
    arr_truncate(ss.sources, (size_t)limit);
    arr_truncate(ss.citations, (size_t)limit);
    json_t *res = json_obj();
    json_set_str(res, "provider", "openai");
    if (ans) json_set_str(res, "answer", ans);
    free(ans);
    json_set(res, "sources", ss.sources);
    if (json_len(ss.citations)) json_set(res, "citations", ss.citations);
    else json_free(ss.citations);
    if (json_len(queries)) json_set(res, "searchQueries", queries);
    else json_free(queries);
    json_t *ur = json_get_obj(j, "usage"), *usage = json_obj();
    if (json_is_num(json_get(ur, "input_tokens"))) json_set(usage, "inputTokens", json_clone(json_get(ur, "input_tokens")));
    if (json_is_num(json_get(ur, "output_tokens"))) json_set(usage, "outputTokens", json_clone(json_get(ur, "output_tokens")));
    if (json_is_num(json_get(ur, "total_tokens"))) json_set(usage, "totalTokens", json_clone(json_get(ur, "total_tokens")));
    if (search_requests) json_set_int(usage, "searchRequests", (int64_t)search_requests);
    if (json_len(usage)) json_set(res, "usage", usage);
    else json_free(usage);
    json_set_str(res, "model", json_get_str(j, "model") ? json_get_str(j, "model") : m->id);
    set_str_opt(res, "requestId", json_get_str(j, "id"));
    json_set_str(res, "authMode", "api_key");
    json_free(j);
    return res;
}

static bool has_openai(const ai_model_t *m) {
    const char *k = key_kind(m ? m->provider : "openai");
    return k && !str_eq(k, "oauth");
}

/* ---------- OpenAI Codex (providers/codex.ts) ---------- */

#define CODEX_BASE_URL "https://chatgpt.com/backend-api"

/* resolveCodexResponsesUrl */
static char *codex_url(const char *base_in) {
    char *base = xstrdup(base_in && *base_in ? base_in : CODEX_BASE_URL);
    size_t n = strlen(base);
    while (n && base[n - 1] == '/') base[--n] = 0;
    char *r;
    if (str_ends(base, "/codex/responses")) r = xstrdup(base);
    else if (str_ends(base, "/codex")) r = xasprintf("%s/responses", base);
    else r = xasprintf("%s/codex/responses", base);
    free(base);
    return r;
}

/* cleanSourceUrl: drop utm_source=openai */
static char *clean_source_url(const char *u) {
    const char *q = strchr(u, '?');
    if (!q) return xstrdup(u);
    sb_t b = SB_INIT;
    sb_appendn(&b, u, (size_t)(q - u));
    const char *frag = strchr(q, '#');
    size_t qlen = frag ? (size_t)(frag - q - 1) : strlen(q + 1);
    char *qs = xstrndup(q + 1, qlen);
    bool first = true;
    for (char *tok = qs, *next; tok && *tok; tok = next) {
        next = strchr(tok, '&');
        if (next) *next++ = 0;
        if (!strcmp(tok, "utm_source=openai")) continue;
        sb_putc(&b, first ? '?' : '&');
        sb_append(&b, tok);
        first = false;
    }
    free(qs);
    if (frag) sb_append(&b, frag);
    return sb_detach(&b);
}

/* codex.ts addSource */
static void add_codex_source(json_t *sources, const char *title, const char *url, const char *snippet) {
    char *u = clean_source_url(url);
    json_t *ex = find_by_url(sources, u);
    if (!ex) {
        json_t *o = json_obj();
        json_set_str(o, "title", title && str_eq(title, url) ? u : (title ? title : u));
        json_set_str(o, "url", u);
        if (snippet) json_set_str(o, "snippet", snippet);
        json_push(sources, o);
    } else {
        if (str_eq(json_get_str(ex, "title"), json_get_str(ex, "url")) && title && strcmp(title, url) != 0)
            json_set_str(ex, "title", title);
        if (!json_get_str(ex, "snippet") && snippet) json_set_str(ex, "snippet", snippet);
    }
    free(u);
}

/* extractCitationSnippet / xai extractSnippetAround */
static char *snippet_around(const char *text, int64_t start, int64_t end) {
    if (!text || !*text) return NULL;
    int64_t len = js_len(text), a = MAX((int64_t)0, start - 100), z = MIN(len, end + 100);
    char *raw = js_slice(text, a, z);
    /* /\[([^\]]*)\]\([^)]*\)/g -> "$1" */
    sb_t b = SB_INIT;
    for (const char *p = raw; *p;) {
        if (*p == '[') {
            const char *close = strchr(p + 1, ']');
            if (close && close[1] == '(' && strchr(close + 2, ')')) {
                const char *rp = strchr(close + 2, ')');
                sb_appendn(&b, p + 1, (size_t)(close - p - 1));
                p = rp + 1;
                continue;
            }
        }
        sb_putc(&b, *p++);
    }
    free(raw);
    char *t = str_trim(sb_cstr(&b));
    sb_free(&b);
    if (!*t) {
        free(t);
        return NULL;
    }
    if (js_len(t) > 300) {
        char *c = js_slice(t, 0, 297);
        free(t);
        t = xasprintf("%s...", c);
        free(c);
    }
    return t;
}

static bool image_placeholder(const char *text) {
    char *t = str_trim(text);
    char *p = t;
    while (*p && strchr("[(\"'`*_", *p)) p++;
    size_t n = strlen(p);
    while (n && strchr("])\"'`*_.!?", p[n - 1])) n--;
    char *core = xstrndup(p, n);
    char *c = str_trim(core);
    for (char *q = c; *q; q++) *q = (char)tolower((unsigned char)*q);
    static const char *const known[] = {"see attached image", "attached image", "see the attached image", "see image",
                                        "see image above",    "image above",    "see image below",        "image below"};
    bool r = false;
    for (size_t i = 0; i < ARRAY_LEN(known); i++)
        if (!strcmp(c, known[i])) r = true;
    free(c);
    free(core);
    free(t);
    return r;
}

/* normalizeExtractedUrl: trailing prose punctuation and unmatched closers */
static char *normalize_extracted_url(const char *cand) {
    char *u = str_trim(cand);
    size_t n = strlen(u);
    for (;;) {
        if (!n) break;
        char c = u[n - 1];
        int open = 0, close = 0;
        char oc = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
        if (oc)
            for (size_t i = 0; i < n; i++) open += u[i] == oc, close += u[i] == c;
        if (strchr(".,!?;:'\"", c) || (oc && close > open)) {
            u[--n] = 0;
            continue;
        }
        break;
    }
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) {
        free(u);
        return NULL;
    }
    return u;
}

/* extractTextSources: markdown links, then bare URLs */
static void extract_text_sources(const char *text, json_t *sources) {
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        if (text[i] != '[') continue;
        const char *te = strchr(text + i + 1, ']');
        if (!te || te[1] != '(') continue;
        int depth = 0;
        const char *ue = NULL;
        for (const char *q = te + 1; *q && *q != '\n'; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') {
                if (--depth == 0) {
                    ue = q;
                    break;
                }
                if (depth < 0) break;
            }
        }
        if (!ue) continue;
        char *title = xstrndup(text + i + 1, (size_t)(te - text - i - 1));
        char *tt = str_trim(title);
        char *raw = xstrndup(te + 2, (size_t)(ue - te - 2));
        char *url = normalize_extracted_url(raw);
        if (url) add_codex_source(sources, *tt ? tt : url, url, NULL);
        free(url);
        free(raw);
        free(tt);
        free(title);
        i = (size_t)(ue - text);
    }
    for (const char *p = text; (p = strstr(p, "http"));) {
        if (strncmp(p, "http://", 7) && strncmp(p, "https://", 8)) {
            p += 4;
            continue;
        }
        const char *q = p;
        while (*q && !isspace((unsigned char)*q)) q++;
        char *raw = xstrndup(p, (size_t)(q - p));
        char *url = normalize_extracted_url(raw);
        if (url) add_codex_source(sources, url, url, NULL);
        free(url);
        free(raw);
        p = q;
    }
}

/* classifyCodexSseErrorStatus */
static int codex_err_status(const char *code, const char *message) {
    char *d = xasprintf("%s %s", code, message);
    for (char *q = d; *q; q++) *q = (char)tolower((unsigned char)*q);
    int st = 500;
    if (strstr(d, "rate limit") || strstr(d, "rate-limit") || strstr(d, "ratelimit") || strstr(d, "too many requests") ||
        strstr(d, "quota") || strstr(d, "429"))
        st = 429;
    else if (strstr(d, "unauthorized") || strstr(d, "unauthorised") || strstr(d, "401")) st = 401;
    else if (strstr(d, "forbidden") || strstr(d, "403")) st = 403;
    else if (strstr(d, "timeout") || strstr(d, "timed out")) st = 504;
    free(d);
    return st;
}

static void codex_sse_error(const json_t *ev, const char **code, const char **msg) {
    const json_t *cands[] = {ev, json_get_obj(ev, "error"), json_get_obj(json_get_obj(ev, "response"), "error")};
    *code = "";
    *msg = "";
    for (size_t i = 0; i < ARRAY_LEN(cands); i++) {
        if (!cands[i]) continue;
        const char *c = json_get_str(cands[i], "code"), *m = json_get_str(cands[i], "message");
        if (!**code && c && *c) *code = c;
        if (!**msg && m && *m) *msg = m;
    }
}

static json_t *s_codex(const ws_params_t *p, ws_err_t *e) {
    const ai_model_t *m = p->model;
    char *url = codex_url(m->base_url);
    char *official = codex_url(CODEX_BASE_URL);
    bool custom = strcmp(url, official) != 0;
    free(official);
    const char *kind = key_kind(m->provider);
    if (custom && (str_eq(kind, "oauth") || str_eq(kind, "env"))) {
        fail_typed(e, "codex", 0,
                   xasprintf("Refusing to send official Codex OAuth credentials to custom endpoint %s. Configure an API key "
                             "for provider \"%s\".",
                             m->base_url, m->provider));
        free(url);
        return NULL;
    }
    char *token = auth_get_api_key(m->provider);
    if (!token || !*token) {
        free(token);
        free(url);
        if (custom || str_eq(kind, "env"))
            fail_plain(e, xasprintf("Codex credentials not found for selected provider \"%s\".", m->provider));
        else fail_plain(e, xasprintf("No Codex OAuth credentials found for selected provider \"%s\".", m->provider));
        return NULL;
    }
    http_headers_t h = {0};
    ai_apply_headers(&h, m, NULL);
    http_headers_del(&h, "x-api-key");
    char *auth = xasprintf("Bearer %s", token);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    char *acc = custom ? NULL : oair_codex_account_id(token);
    if (acc) http_headers_set(&h, "chatgpt-account-id", acc);
    else http_headers_del(&h, "chatgpt-account-id");
    free(acc);
    http_headers_set(&h, "originator", "omp");
    http_headers_set(&h, "version", ai_codex_client_version());
    http_headers_set(&h, "User-Agent", OMC_NAME "/" OMC_VERSION);
    http_headers_set(&h, "Accept", "text/event-stream");
    http_headers_set(&h, "Content-Type", "application/json");
    json_t *body = json_obj();
    json_set_str(body, "model", m->id);
    json_set_bool(body, "stream", true);
    json_set_bool(body, "store", false);
    json_t *inc = json_arr();
    json_push_str(inc, "web_search_call.action.sources");
    json_set(body, "include", inc);
    json_set_bool(body, "parallel_tool_calls", true);
    json_t *input = json_arr(), *msg = json_obj(), *content = json_arr(), *it = json_obj();
    json_set_str(msg, "type", "message");
    json_set_str(msg, "role", "user");
    json_set_str(it, "type", "input_text");
    json_set_str(it, "text", p->query);
    json_push(content, it);
    json_set(msg, "content", content);
    json_push(input, msg);
    json_set(body, "input", input);
    json_t *tools = json_arr(), *tool = json_obj();
    json_set_str(tool, "type", "web_search");
    json_set_str(tool, "search_context_size", "high");
    json_push(tools, tool);
    json_set(body, "tools", tools);
    json_t *tc = json_obj();
    json_set_str(tc, "type", "web_search");
    json_set(body, "tool_choice", tc);
    json_set_str(body, "instructions", p->system_prompt);
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", url, &h, payload, p, &r);
    free(url);
    free(payload);
    free(token);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "codex", r.status, r.body))
            fail_typed(e, "codex", r.status, xasprintf("Codex API error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return NULL;
    }
    json_t *events = sse_events(r.body);
    http_resp_free(&r);
    sb_t final = SB_INIT, streamed = SB_INIT;
    json_t *sources = json_arr(), *usage = NULL;
    char *model = xstrdup(m->id), *request_id = xstrdup("");
    bool invoked = false, failed = false;
    json_t *ev;
    json_arr_foreach(events, i, ev) {
        const char *t = json_get_str(ev, "type");
        if (!t || !*t) continue;
        if (str_starts(t, "response.web_search_call")) invoked = true;
        if (str_eq(t, "response.created")) {
            json_t *rs = json_get_obj(ev, "response");
            if (json_get_str(rs, "id")) free(request_id), request_id = xstrdup(json_get_str(rs, "id"));
            if (json_get_str(rs, "model")) free(model), model = xstrdup(json_get_str(rs, "model"));
        } else if (str_eq(t, "response.output_text.delta")) {
            const char *d = json_get_str(ev, "delta");
            if (d) sb_append(&streamed, d);
        } else if (str_eq(t, "response.output_item.done")) {
            json_t *item = json_get_obj(ev, "item");
            const char *ity = json_get_str(item, "type");
            if (str_eq(ity, "web_search_call")) {
                invoked = true;
                json_t *groups[] = {json_get(json_get_obj(item, "action"), "sources"), json_get(item, "sources"),
                                    json_get(item, "results")};
                for (int g = 0; g < 3; g++) {
                    json_t *s;
                    json_arr_foreach(json_is_arr(groups[g]) ? groups[g] : NULL, k, s) {
                        const char *u = json_get_str(s, "url");
                        if (!u) u = json_get_str(s, "source_website_url");
                        if (!u || !*u) continue;
                        const char *ti = json_get_str(s, "title");
                        if (!ti) ti = json_get_str(s, "caption");
                        add_codex_source(sources, ti ? ti : u, u, NULL);
                    }
                }
            }
            if (str_eq(ity, "message")) {
                json_t *part;
                json_arr_foreach(json_get_arr(item, "content"), k, part) {
                    const char *text = json_get_str(part, "text");
                    if (!str_eq(json_get_str(part, "type"), "output_text") || str_empty(text)) continue;
                    if (final.len) sb_append(&final, "\n\n");
                    sb_append(&final, text);
                    json_t *an;
                    json_arr_foreach(json_get_arr(part, "annotations"), x, an) {
                        const char *au = json_get_str(an, "url");
                        if (!str_eq(json_get_str(an, "type"), "url_citation") || !au) continue;
                        json_t *st = json_get(an, "start_index"), *en = json_get(an, "end_index");
                        char *snip = json_is_num(st) && json_is_num(en)
                                         ? snippet_around(text, json_d2i(st->u.num.d), json_d2i(en->u.num.d))
                                         : NULL;
                        const char *at = json_get_str(an, "title");
                        add_codex_source(sources, at ? at : au, au, snip);
                        free(snip);
                    }
                }
            }
            if (str_eq(ity, "reasoning")) {
                json_t *part;
                json_arr_foreach(json_get_arr(item, "summary"), k, part) {
                    const char *text = json_get_str(part, "text");
                    if (!str_eq(json_get_str(part, "type"), "summary_text") || str_empty(text)) continue;
                    if (final.len) sb_append(&final, "\n\n");
                    sb_append(&final, text);
                }
            }
        } else if (str_eq(t, "response.completed") || str_eq(t, "response.done")) {
            json_t *rs = json_get_obj(ev, "response");
            if (rs) {
                if (json_get_str(rs, "model")) free(model), model = xstrdup(json_get_str(rs, "model"));
                if (json_get_str(rs, "id")) free(request_id), request_id = xstrdup(json_get_str(rs, "id"));
                json_t *u = json_get_obj(rs, "usage");
                if (u) {
                    json_free(usage);
                    usage = json_obj();
                    int64_t cached = json_get_int(json_get_obj(u, "input_tokens_details"), "cached_tokens", 0);
                    json_set_int(usage, "inputTokens", json_get_int(u, "input_tokens", 0) - cached);
                    json_set_int(usage, "outputTokens", json_get_int(u, "output_tokens", 0));
                    json_set_int(usage, "totalTokens", json_get_int(u, "total_tokens", 0));
                }
            }
        } else if (str_eq(t, "error") || str_eq(t, "response.failed")) {
            const char *code, *message;
            codex_sse_error(ev, &code, &message);
            char *dm;
            if (str_eq(t, "error")) dm = xasprintf("Codex error (%s): %s", code, *message ? message : "Unknown error");
            else if (*code) dm = xasprintf("Codex request failed (%s): %s", code, *message ? message : "Request failed");
            else dm = xasprintf("Codex request failed: %s", *message ? message : "Request failed");
            fail_typed(e, "codex", codex_err_status(code, message), dm);
            failed = true;
            break;
        }
    }
    json_free(events);
    json_t *res = NULL;
    char *fa = str_trim(sb_cstr(&final)), *sa = str_trim(sb_cstr(&streamed));
    sb_free(&final);
    sb_free(&streamed);
    if (!failed && !invoked) {
        fail_typed(e, "codex", 502,
                   xstrdup("Codex returned a completion without running web search (no web_search_call event); refusing to "
                           "treat a non-search answer as a search result"));
        failed = true;
    }
    bool has_final = *fa && !image_placeholder(fa), has_streamed = *sa && !image_placeholder(sa);
    if (!failed && !has_final && !has_streamed && json_len(sources) == 0) {
        fail_typed(e, "codex", 502, xstrdup("Codex returned image-only response"));
        failed = true;
    }
    if (!failed) {
        const char *answer = has_final ? fa : has_streamed ? sa : "";
        if (json_len(sources) == 0 && *answer) extract_text_sources(answer, sources);
        double n = want_results(p);
        if (!isnan(n) && n > 0 && json_len(sources) > (size_t)n) arr_truncate(sources, (size_t)n);
        res = json_obj();
        json_set_str(res, "provider", "codex");
        if (*answer) json_set_str(res, "answer", answer);
        json_set(res, "sources", sources);
        sources = NULL;
        if (usage) json_set(res, "usage", usage), usage = NULL;
        json_set_str(res, "model", model);
        json_set_str(res, "requestId", request_id);
    }
    json_free(sources);
    json_free(usage);
    free(fa);
    free(sa);
    free(model);
    free(request_id);
    return res;
}

static bool has_codex(const ai_model_t *m) { return key_kind(m ? m->provider : "openai-codex") != NULL; }

/* ---------- Gemini (providers/gemini.ts) ---------- */

static json_t *gemini_tools(void) {
    json_t *tools = json_arr(), *t = json_obj();
    json_set(t, "googleSearch", json_obj());
    json_push(tools, t);
    return tools;
}

static void gemini_generation_config(json_t *req, const ws_params_t *p) {
    if (isnan(p->max_tokens) && isnan(p->temperature)) return;
    json_t *gc = json_obj();
    if (!isnan(p->max_tokens)) json_set_num(gc, "maxOutputTokens", p->max_tokens);
    if (!isnan(p->temperature)) json_set_num(gc, "temperature", p->temperature);
    json_set(req, "generationConfig", gc);
}

/* fetchWithRetry(maxAttempts 4, 1 s * 2^n, 5 min cap) */
static int gemini_post(const char *url, const http_headers_t *h, const char *body, const ws_params_t *p, http_resp_t *r) {
    for (int attempt = 0;; attempt++) {
        int rc = ws_http("POST", url, h, body, p, r);
        bool retry_status = rc == 0 && (r->status >= 500 || r->status == 408 || r->status == 429);
        if (cancelled(p) || attempt >= 3 || (rc == 0 && !retry_status)) return rc;
        int64_t hint = rc == 0 ? ai_extract_retry_hint(&r->headers, r->body) : -1;
        if (hint > 5 * 60 * 1000) return rc;
        int64_t delay = hint >= 0 ? hint : (1000LL << attempt);
        http_resp_free(r);
        int64_t until = mono_ms() + delay;
        while (mono_ms() < until && !cancelled(p)) sleep_ms(MIN((int64_t)50, MAX((int64_t)1, until - mono_ms())));
    }
}

/* parseGeminiSearchStream */
static json_t *gemini_parse(const char *body, const char *fallback_model) {
    json_t *res = resp_new("gemini"), *srcs = json_get(res, "sources"), *cits = json_arr(), *queries = json_arr();
    sb_t answer = SB_INIT;
    json_t *usage = NULL;
    char *model = xstrdup(fallback_model);
    for (const char *p = body ? body : ""; *p;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n > 5 && !strncmp(p, "data:", 5)) {
            char *js = xstrndup(p + 5, n - 5);
            char *t = str_trim(js);
            json_t *chunk = *t ? json_parse_str(t) : NULL;
            free(t);
            free(js);
            if (chunk) {
                json_t *rd = json_get_obj(chunk, "response") ? json_get_obj(chunk, "response") : chunk;
                json_t *cand = json_at(json_get_arr(rd, "candidates"), 0), *part;
                json_arr_foreach(json_get_arr(json_get_obj(cand, "content"), "parts"), i, part) {
                    const char *tx = json_get_str(part, "text");
                    if (tx && *tx) sb_append(&answer, tx);
                }
                json_t *gm = json_get_obj(cand, "groundingMetadata");
                json_t *chunks = json_get_arr(gm, "groundingChunks"), *gc;
                json_arr_foreach(chunks, i, gc) {
                    json_t *web = json_get_obj(gc, "web");
                    const char *uri = json_get_str(web, "uri");
                    if (!uri || !*uri || find_by_url(srcs, uri)) continue;
                    const char *ti = json_get_str(web, "title");
                    json_t *s = json_obj();
                    json_set_str(s, "title", ti ? ti : uri);
                    json_set_str(s, "url", uri);
                    json_push(srcs, s);
                }
                json_t *sup;
                json_arr_foreach(chunks ? json_get_arr(gm, "groundingSupports") : NULL, i, sup) {
                    const char *cited = json_get_str(json_get_obj(sup, "segment"), "text");
                    json_t *idx;
                    json_arr_foreach(json_get_arr(sup, "groundingChunkIndices"), k, idx) {
                        json_t *web = json_get_obj(json_at(chunks, (size_t)jnum(idx)), "web");
                        const char *uri = json_get_str(web, "uri");
                        if (!uri || !*uri) continue;
                        json_t *c = json_obj();
                        json_set_str(c, "url", uri);
                        json_set_str(c, "title", json_get_str(web, "title") ? json_get_str(web, "title") : uri);
                        if (cited) json_set_str(c, "citedText", cited);
                        json_push(cits, c);
                    }
                }
                json_t *q;
                json_arr_foreach(json_get_arr(gm, "webSearchQueries"), i, q) if (json_is_str(q) && !json_arr_has_str(queries, json_sv(q)))
                    json_push_str(queries, json_sv(q));
                json_t *um = json_get_obj(rd, "usageMetadata");
                if (um) {
                    json_free(usage);
                    usage = json_obj();
                    json_set_int(usage, "inputTokens", json_get_int(um, "promptTokenCount", 0));
                    json_set_int(usage, "outputTokens", json_get_int(um, "candidatesTokenCount", 0));
                    json_set_int(usage, "totalTokens", json_get_int(um, "totalTokenCount", 0));
                }
                if (json_get_str(rd, "modelVersion")) free(model), model = xstrdup(json_get_str(rd, "modelVersion"));
                json_free(chunk);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (answer.len) json_set_str(res, "answer", sb_cstr(&answer));
    sb_free(&answer);
    if (json_len(cits)) json_set(res, "citations", cits);
    else json_free(cits);
    if (json_len(queries)) json_set(res, "searchQueries", queries);
    else json_free(queries);
    if (usage) json_set(res, "usage", usage);
    json_set_str(res, "model", model);
    free(model);
    return res;
}

static bool grounding_redirect(const char *url) {
    return url && (str_starts(url, "https://vertexaisearch.cloud.google.com/") || str_starts(url, "http://vertexaisearch.cloud.google.com/")) &&
           strstr(url, "/grounding-api-redirect");
}

/* resolveGroundingRedirect: HEAD without following, Location (http/https) or the proxy URL */
static char *resolve_redirect(const char *url, const ws_params_t *p) {
    ws_params_t q = *p;
    q.timeout_ms = MIN(p->timeout_ms, (int64_t)5000);
    http_resp_t r;
    int rc = ws_http("HEAD", url, NULL, NULL, &q, &r);
    char *out = xstrdup(url);
    const char *loc = rc == 0 ? http_headers_get(&r.headers, "location") : NULL;
    if (loc && (str_starts(loc, "https://") || str_starts(loc, "http://"))) {
        free(out);
        out = xstrdup(loc);
    }
    http_resp_free(&r);
    return out;
}

/* finalizeGeminiSearchResult */
static json_t *gemini_finalize(json_t *res, const ws_params_t *p, ws_err_t *e) {
    json_t *srcs = json_get(res, "sources"), *cits = json_get(res, "citations");
    if (!json_get_str(res, "answer") && json_len(srcs) == 0) {
        json_free(res);
        fail_typed(e, "gemini", 502, xstrdup("Gemini API returned an empty grounded response"));
        return NULL;
    }
    json_t *map = json_obj();
    for (int k = 0; k < 2; k++) {
        json_t *it;
        json_arr_foreach(k ? cits : srcs, i, it) {
            const char *u = json_get_str(it, "url");
            if (!grounding_redirect(u)) continue;
            if (!json_get(map, u)) json_set(map, u, json_str_own(resolve_redirect(u, p)));
            json_set_str(it, "url", json_get_str(map, u));
        }
    }
    json_free(map);
    json_t *dedup = json_arr(), *it;
    json_arr_foreach(srcs, i, it) if (!find_by_url(dedup, json_get_str(it, "url"))) json_push(dedup, json_clone(it));
    json_set(res, "sources", dedup);
    return res;
}

static json_t *s_gemini(const ws_params_t *p, ws_err_t *e) {
    const ai_model_t *m = p->model;
    const char *model_id = m->id; /* resolveWireModelId: effort-routed wire ids are not split out of omc's catalog */
    http_resp_t r;
    int rc;
    char *secret = NULL;
    const char *kind_label;
    if (str_eq(m->api, "google-gemini-cli")) {
        kind_label = "Gemini Cloud Code API error";
        char *cred = auth_get_api_key(m->provider);
        json_t *cj = cred ? json_parse_str(cred) : NULL;
        free(cred);
        const char *token = json_get_str(cj, "token"), *project = json_get_str(cj, "projectId");
        if (!token || !*token || !project || !*project) {
            json_free(cj);
            fail_plain(e, xasprintf("No Gemini OAuth credentials found for selected provider \"%s\".", m->provider));
            return NULL;
        }
        bool antigravity = str_eq(m->provider, "google-antigravity");
        secret = xstrdup(token);
        json_t *body = json_obj(), *req = json_obj(), *contents = json_arr(), *c = json_obj(), *parts = json_arr(), *pt = json_obj();
        json_set_str(body, "project", project);
        json_set_str(body, "model", model_id);
        json_set_str(c, "role", "user");
        json_set_str(pt, "text", p->query);
        json_push(parts, pt);
        json_set(c, "parts", parts);
        json_push(contents, c);
        json_set(req, "contents", contents);
        json_set(req, "tools", gemini_tools());
        if (p->system_prompt && *p->system_prompt) {
            json_t *si = json_obj(), *sp = json_arr(), *st = json_obj();
            if (antigravity) json_set_str(si, "role", "user");
            json_set_str(st, "text", p->system_prompt);
            json_push(sp, st);
            json_set(si, "parts", sp);
            json_set(req, "systemInstruction", si);
        }
        gemini_generation_config(req, p);
        json_set(body, "request", req);
        if (antigravity) {
            json_set_str(body, "userAgent", "antigravity");
            char *u = uuid_v4();
            json_set(body, "requestId", json_str_own(xasprintf("agent-%s", u)));
            free(u);
        } else {
            json_set_str(body, "userAgent", OMC_NAME "/" OMC_VERSION);
            char *rnd = rand_hex(5);
            json_set(body, "requestId", json_str_own(xasprintf("omp-%lld-%s", (long long)now_ms(), rnd)));
            free(rnd);
        }
        json_free(cj);
        http_headers_t h = {0};
        ai_apply_headers(&h, m, NULL);
        char *auth = xasprintf("Bearer %s", secret);
        http_headers_set(&h, "Authorization", auth);
        free(auth);
        http_headers_set(&h, "Content-Type", "application/json");
        if (antigravity) {
            cca_antigravity_headers(&h);
        } else {
            http_headers_set(&h, "Accept", "text/event-stream");
        }
        char *base = xstrdup(m->base_url ? m->base_url : "");
        size_t bl = strlen(base);
        while (bl && base[bl - 1] == '/') base[--bl] = 0;
        char *url = xasprintf("%s/v1internal:streamGenerateContent?alt=sse", base);
        free(base);
        char *payload = json_dump(body, 0);
        json_free(body);
        rc = gemini_post(url, &h, payload, p, &r);
        free(url);
        free(payload);
        http_headers_free(&h);
    } else if (str_eq(m->api, "google-generative-ai")) {
        kind_label = "Gemini Developer API error";
        char *base = xstrdup(m->base_url ? m->base_url : "");
        char *bt = str_trim(base);
        free(base);
        size_t bl = strlen(bt);
        while (bl && bt[bl - 1] == '/') bt[--bl] = 0;
        if (!str_starts(bt, "https://") && !str_starts(bt, "http://")) {
            fail_typed(e, "gemini", 400, xasprintf("Gemini model base URL must be a valid absolute URL: %s", m->base_url ? m->base_url : ""));
            free(bt);
            return NULL;
        }
        bool cf = strstr(bt, "://gateway.ai.cloudflare.com") != NULL;
        char *stored = auth_get_api_key(m->provider);
        if (!stored || !*stored) {
            free(stored);
            free(bt);
            fail_plain(e, xasprintf("No Gemini credentials found for selected provider \"%s\".", m->provider));
            return NULL;
        }
        if (cf) {
            char *tok = NULL, *acc = NULL, *gw = NULL;
            if (ai_cloudflare_credential(stored, &tok, &acc, &gw)) secret = tok, tok = NULL;
            free(tok);
            free(acc);
            free(gw);
            free(stored);
        } else secret = stored;
        if (!secret || !*secret) {
            free(secret);
            free(bt);
            fail_typed(e, "gemini", 401, xstrdup("Selected Gemini credential is empty"));
            return NULL;
        }
        json_t *body = json_obj(), *contents = json_arr(), *c = json_obj(), *parts = json_arr(), *pt = json_obj();
        json_set_str(c, "role", "user");
        json_set_str(pt, "text", p->query);
        json_push(parts, pt);
        json_set(c, "parts", parts);
        json_push(contents, c);
        json_set(body, "contents", contents);
        json_set(body, "tools", gemini_tools());
        if (p->system_prompt && *p->system_prompt) {
            json_t *si = json_obj(), *sp = json_arr(), *st = json_obj();
            json_set_str(st, "text", p->system_prompt);
            json_push(sp, st);
            json_set(si, "parts", sp);
            json_set(body, "systemInstruction", si);
        }
        gemini_generation_config(body, p);
        http_headers_t h = {0};
        ai_apply_headers(&h, m, NULL);
        if (cf) {
            char *a = xasprintf("Bearer %s", secret);
            http_headers_set(&h, "cf-aig-authorization", a);
            free(a);
        } else http_headers_set(&h, "x-goog-api-key", secret);
        http_headers_set(&h, "Content-Type", "application/json");
        http_headers_set(&h, "Accept", "text/event-stream");
        char *url = xasprintf("%s/models/%s:streamGenerateContent?alt=sse", bt, model_id);
        free(bt);
        char *payload = json_dump(body, 0);
        json_free(body);
        rc = gemini_post(url, &h, payload, p, &r);
        free(url);
        free(payload);
        http_headers_free(&h);
    } else {
        fail_typed(e, "gemini", 400,
                   xasprintf("Selected model %s/%s does not use a Gemini grounding transport", m->provider, m->id));
        return NULL;
    }
    if (rc != 0) {
        free(secret);
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        char *text = r.body && *secret ? str_replace(r.body, secret, "[redacted]") : xstrdup(r.body ? r.body : "");
        if (!classify_http(e, "gemini", r.status, text))
            fail_typed(e, "gemini", r.status, xasprintf("%s (%d): %s", kind_label, r.status, text));
        free(text);
        free(secret);
        http_resp_free(&r);
        return NULL;
    }
    free(secret);
    json_t *res = gemini_finalize(gemini_parse(r.body, model_id), p, e);
    http_resp_free(&r);
    if (!res) return NULL;
    double n = want_results(p);
    if (!isnan(n) && n > 0 && json_len(json_get(res, "sources")) > (size_t)n) arr_truncate(json_get(res, "sources"), (size_t)n);
    return res;
}

static bool has_gemini(const ai_model_t *m) {
    if (str_eq(m->api, "google-gemini-cli")) return str_eq(key_kind(m->provider), "oauth");
    if (str_eq(m->api, "google-generative-ai")) return key_kind(m->provider) != NULL;
    return false;
}

/* ---------- xAI (providers/xai.ts) ---------- */

static char *recency_start(const char *recency) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (str_eq(recency, "day")) tm.tm_mday -= 1;
    else if (str_eq(recency, "week")) tm.tm_mday -= 7;
    else if (str_eq(recency, "month")) tm.tm_mon -= 1;
    else tm.tm_year -= 1;
    time_t u = timegm(&tm);
    gmtime_r(&u, &tm);
    char buf[16];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return xstrdup(buf);
}

static void xai_add(json_t *srcs, json_t *cits, json_t *seen, const char *url, const char *title, const char *cited) {
    char *u = str_trim(url);
    if (!*u || json_get(seen, u)) {
        free(u);
        return;
    }
    json_set_bool(seen, u, true);
    char *tt = title ? str_trim(title) : xstrdup("");
    char *ct = cited ? str_trim(cited) : xstrdup("");
    json_t *s = json_obj(), *c = json_obj();
    json_set_str(s, "title", *tt ? tt : u);
    json_set_str(s, "url", u);
    if (*ct) json_set_str(s, "snippet", ct);
    json_set_str(c, "title", *tt ? tt : u);
    json_set_str(c, "url", u);
    if (*ct) json_set_str(c, "citedText", ct);
    json_push(srcs, s);
    json_push(cits, c);
    free(tt);
    free(ct);
    free(u);
}

static bool all_digits(const char *s) {
    char *t = str_trim(s);
    bool r = *t != 0;
    for (char *q = t; *q; q++)
        if (!isdigit((unsigned char)*q)) r = false;
    free(t);
    return r;
}

static void xai_annotations(const json_t *anns, json_t *srcs, json_t *cits, json_t *seen, const char *text) {
    json_t *a;
    json_arr_foreach(json_is_arr(anns) ? anns : NULL, i, a) {
        const char *u = json_get_str(a, "url");
        if (!str_eq(json_get_str(a, "type"), "url_citation") || !u) continue;
        const char *title = json_get_str(a, "title");
        if (title && all_digits(title)) title = NULL;
        const char *cited = json_get_str(a, "cited_text");
        if (!cited) cited = json_get_str(a, "text");
        char *snip = NULL;
        json_t *st = json_get(a, "start_index"), *en = json_get(a, "end_index");
        if (!cited && json_is_num(st) && json_is_num(en)) snip = snippet_around(text, json_d2i(st->u.num.d), json_d2i(en->u.num.d));
        xai_add(srcs, cits, seen, u, title, cited ? cited : snip);
        free(snip);
    }
}

/* parseXAIAnswer */
static char *xai_answer(const json_t *resp) {
    typedef struct {
        sb_t text;
        int ntexts;
        bool cites;
        int phase; /* 0 none, 1 commentary, 2 final_answer */
    } entry_t;
    VEC(entry_t) msgs = {0};
    json_t *item;
    json_arr_foreach(json_get_arr(resp, "output"), i, item) {
        if (!json_is_obj(item)) continue;
        const char *ty = json_get_str(item, "type");
        if (ty && !str_eq(ty, "message")) continue;
        json_t *content = json_get_arr(item, "content");
        if (!content && !ty) continue;
        const char *ph = json_get_str(item, "phase");
        entry_t en = {SB_INIT, 0, false, str_eq(ph, "commentary") ? 1 : str_eq(ph, "final_answer") ? 2 : 0};
        json_t *part;
        json_arr_foreach(content, k, part) {
            if (!json_is_obj(part)) continue;
            const char *t = json_get_str(part, "output_text");
            if (!t) t = json_get_str(part, "text");
            char *tt = t ? str_trim(t) : NULL;
            if (tt && *tt) {
                if (en.ntexts++) sb_putc(&en.text, '\n');
                sb_append(&en.text, tt);
            }
            free(tt);
            json_t *a;
            json_arr_foreach(json_get_arr(part, "annotations"), x, a) if (str_eq(json_get_str(a, "type"), "url_citation") &&
                                                                          !str_blank(json_get_str(a, "url"))) en.cites = true;
        }
        json_t *a;
        json_arr_foreach(json_get_arr(item, "annotations"), x, a) if (str_eq(json_get_str(a, "type"), "url_citation") &&
                                                                      !str_blank(json_get_str(a, "url"))) en.cites = true;
        vec_push(&msgs, en);
    }
    bool final_content = false, any_final = false;
    for (size_t i = 0; i < msgs.n; i++) {
        if (msgs.v[i].phase == 2) any_final = true;
        if (msgs.v[i].phase == 2 && msgs.v[i].ntexts) final_content = true;
    }
    char *out = NULL;
    bool done = false;
    if (!final_content) {
        if (any_final) done = true;
        else if (!msgs.n) {
            const char *ot = json_get_str(resp, "output_text");
            char *t = ot ? str_trim(ot) : NULL;
            if (t && *t) out = t;
            else free(t);
            done = true;
        } else if (msgs.v[msgs.n - 1].ntexts == 0 && msgs.v[msgs.n - 1].phase != 1) done = true;
    }
    if (!done) {
        sb_t b = SB_INIT;
        int parts = 0;
        for (size_t i = 0; i < msgs.n; i++) {
            entry_t *en = &msgs.v[i];
            bool keep = final_content ? en->phase == 2
                                      : en->phase == 0 && (i == msgs.n - 1 || en->cites || js_len(sb_cstr(&en->text)) >= 300);
            if (!keep || !en->ntexts) continue;
            if (parts++) sb_putc(&b, '\n');
            sb_append(&b, sb_cstr(&en->text));
        }
        char *t = str_trim(sb_cstr(&b));
        sb_free(&b);
        if (*t) out = t;
        else free(t);
    }
    for (size_t i = 0; i < msgs.n; i++) sb_free(&msgs.v[i].text);
    vec_free(&msgs);
    return out;
}

#define XAI_DEFAULT_BASE_URL "https://api.x.ai/v1"

static json_t *s_xai(const ws_params_t *p, ws_err_t *e) {
    const ai_model_t *m = p->model;
    if (!str_eq(m->provider, "xai") && !str_eq(m->provider, "xai-oauth")) {
        fail_typed(e, "xai", 400, xasprintf("Selected model %s/%s is not an xAI model", m->provider, m->id));
        return NULL;
    }
    char *base = xstrdup(m->base_url ? m->base_url : XAI_DEFAULT_BASE_URL);
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    const char *kind = key_kind(m->provider);
    bool official_oauth = str_eq(m->provider, "xai-oauth") && (str_eq(kind, "oauth") || str_eq(kind, "env"));
    if (strcmp(base, XAI_DEFAULT_BASE_URL) != 0 && official_oauth) {
        fail_typed(e, "xai", 0,
                   xasprintf("Refusing to send official xAI OAuth credentials to custom endpoint %s. Configure an API key for "
                             "provider \"xai-oauth\".",
                             m->base_url));
        free(base);
        return NULL;
    }
    char *key = auth_get_api_key(m->provider);
    if (!key || !*key) {
        free(key);
        free(base);
        fail_plain(e, xasprintf("xAI credentials not found for selected provider \"%s\".", m->provider));
        return NULL;
    }
    int cap = clamp_results(want_results(p), 10, 30);
    json_t *body = json_obj(), *input = json_arr(), *sm = json_obj(), *um = json_obj();
    json_set_str(body, "model", m->id);
    json_set_str(sm, "role", "system");
    json_set_str(sm, "content", p->system_prompt);
    json_set_str(um, "role", "user");
    json_set_str(um, "content", p->query);
    json_push(input, sm);
    json_push(input, um);
    json_set(body, "input", input);
    /* searchTools for an unscoped query: web search plus X search */
    json_t *tools = json_arr(), *ws = json_obj(), *xs = json_obj();
    json_set_str(ws, "type", "web_search");
    json_push(tools, ws);
    json_set_str(xs, "type", "x_search");
    if (p->recency) json_set(xs, "from_date", json_str_own(recency_start(p->recency)));
    json_push(tools, xs);
    json_set(body, "tools", tools);
    json_set_str(body, "tool_choice", "required");
    json_set_int(body, "max_turns", 2); /* XAI_SEARCH_MAX_TURNS: one tool round plus a follow-up when a call misfires */
    json_t *rs = json_obj();
    json_set_str(rs, "effort", "low");
    json_set(body, "reasoning", rs);
    if (!isnan(p->max_tokens)) json_set_num(body, "max_output_tokens", p->max_tokens);
    if (!isnan(p->temperature)) json_set_num(body, "temperature", p->temperature);
    http_headers_t h = {0};
    ai_apply_headers(&h, m, NULL);
    http_headers_set(&h, "Content-Type", "application/json");
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    char *url = xasprintf("%s/responses", base);
    free(base);
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", url, &h, payload, p, &r);
    free(url);
    free(payload);
    free(key);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "xai", r.status, r.body))
            fail_typed(e, "xai", r.status, xasprintf("xAI Responses API error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body);
    if (!j) {
        fail_typed(e, "xai", r.status, xstrdup("xAI Responses API returned invalid JSON: JSON Parse error: Unexpected identifier"));
        http_resp_free(&r);
        return NULL;
    }
    http_resp_free(&r);
    /* parseResponse */
    json_t *srcs = json_arr(), *cits = json_arr(), *seen = json_obj(), *item;
    xai_annotations(json_get(j, "annotations"), srcs, cits, seen, NULL);
    json_arr_foreach(json_get_arr(j, "output"), i, item) {
        if (!json_is_obj(item)) continue;
        xai_annotations(json_get(item, "annotations"), srcs, cits, seen, NULL);
        json_t *part;
        json_arr_foreach(json_get_arr(item, "content"), k, part) {
            if (!json_is_obj(part)) continue;
            const char *t = json_get_str(part, "output_text");
            if (!t) t = json_get_str(part, "text");
            xai_annotations(json_get(part, "annotations"), srcs, cits, seen, t);
        }
    }
    json_arr_foreach(json_get_arr(j, "output"), i, item) {
        if (!str_eq(json_get_str(item, "type"), "web_search_call")) continue;
        json_t *groups[] = {json_get(json_get_obj(item, "action"), "sources"), json_get(item, "sources"), json_get(item, "results")};
        for (int g = 0; g < 3; g++) {
            json_t *s;
            json_arr_foreach(json_is_arr(groups[g]) ? groups[g] : NULL, k, s) {
                const char *u = json_get_str(s, "url");
                if (!u) u = json_get_str(s, "source_website_url");
                if (!u) continue;
                const char *ti = json_get_str(s, "title");
                xai_add(srcs, cits, seen, u, ti ? ti : json_get_str(s, "caption"), NULL);
            }
        }
    }
    json_t *c;
    json_arr_foreach(json_get_arr(j, "citations"), i, c) if (json_is_str(c)) xai_add(srcs, cits, seen, json_sv(c), NULL, NULL);
    json_free(seen);
    arr_truncate(srcs, (size_t)cap);
    arr_truncate(cits, (size_t)cap);
    char *answer = xai_answer(j);
    if (!answer && json_len(srcs) == 0) {
        json_free(srcs);
        json_free(cits);
        json_free(j);
        fail_typed(e, "xai", 502, xstrdup("xAI web_search returned no answer or sources"));
        return NULL;
    }
    json_t *res = json_obj();
    json_set_str(res, "provider", "xai");
    if (answer) json_set_str(res, "answer", answer);
    free(answer);
    json_set(res, "sources", srcs);
    if (json_len(cits)) json_set(res, "citations", cits);
    else json_free(cits);
    json_t *u = json_get_obj(j, "usage");
    if (u) {
        json_t *usage = json_obj();
        static const char *const f[][3] = {{"input_tokens", "inputTokens", "inputTokens"},
                                           {"output_tokens", "outputTokens", "outputTokens"},
                                           {"total_tokens", "totalTokens", "totalTokens"}};
        for (int k = 0; k < 3; k++) {
            json_t *v = json_get(u, f[k][0]);
            if (!v) v = json_get(u, f[k][1]);
            if (json_is_num(v)) json_set(usage, f[k][2], json_clone(v));
        }
        if (json_len(usage)) json_set(res, "usage", usage);
        else json_free(usage);
    }
    set_str_opt(res, "model", json_get_str(j, "model"));
    set_str_opt(res, "requestId", json_get_str(j, "id"));
    json_set_str(res, "authMode", official_oauth ? "oauth" : "api_key");
    json_free(j);
    return res;
}

static bool has_xai(const ai_model_t *m) { return key_kind(m ? m->provider : "xai") != NULL; }

/* ---------- Perplexity (providers/perplexity.ts, API-key transport) ---------- */

static json_t *s_perplexity(const ws_params_t *p, ws_err_t *e) {
    char *key = str_eq(key_kind("perplexity"), "oauth") ? NULL : auth_get_api_key("perplexity");
    if (!key || !*key) {
        free(key);
        fail_typed(e, "perplexity", 401, xstrdup("No authentication method available."));
        return NULL;
    }
    char *model = env_trimmed("PI_PERPLEXITY_API_MODEL");
    json_t *body = json_obj(), *msgs = json_arr();
    json_set_str(body, "model", model ? model : "sonar-pro");
    free(model);
    if (p->system_prompt && *p->system_prompt) {
        json_t *sm = json_obj();
        json_set_str(sm, "role", "system");
        json_set_str(sm, "content", p->system_prompt);
        json_push(msgs, sm);
    }
    json_t *um = json_obj();
    json_set_str(um, "role", "user");
    json_set_str(um, "content", p->query);
    json_push(msgs, um);
    json_set(body, "messages", msgs);
    json_set_bool(body, "stream", true);
    json_t *so = json_obj();
    json_set_bool(so, "include_usage", true);
    json_set(body, "stream_options", so);
    json_set_num(body, "temperature", isnan(p->temperature) ? 0.2 : p->temperature);
    json_set_num(body, "max_tokens", isnan(p->max_tokens) ? 8192 : p->max_tokens);
    json_set_str(body, "search_mode", "web");
    json_set_num(body, "num_search_results", isnan(p->num_results) ? 20 : p->num_results);
    json_t *wso = json_obj();
    json_set_str(wso, "search_type", "pro");
    json_set_str(wso, "search_context_size", "high");
    json_set(body, "web_search_options", wso);
    json_set_bool(body, "enable_search_classifier", true);
    json_set_str(body, "reasoning_effort", "medium");
    json_set_str(body, "language_preference", "en");
    json_set_bool(body, "return_related_questions", true);
    if (p->recency) json_set_str(body, "search_recency_filter", p->recency);
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "Accept", "text/event-stream");
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", "https://api.perplexity.ai/chat/completions", &h, payload, p, &r);
    free(payload);
    free(key);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        char *detail = ai_extract_error(r.status, r.body);
        if (!classify_http(e, "perplexity", r.status, detail))
            fail_typed(e, "perplexity", r.status, xasprintf("Perplexity API error (%d): %s", r.status, detail));
        free(detail);
        http_resp_free(&r);
        return NULL;
    }
    json_t *events = sse_events(r.body), *ev;
    http_resp_free(&r);
    sb_t answer = SB_INIT;
    const char *id = NULL, *model_out = NULL;
    json_t *citations = NULL, *results = NULL, *related = NULL, *usage = NULL;
    json_arr_foreach(events, i, ev) {
        if (json_get_str(ev, "id") && *json_get_str(ev, "id")) id = json_get_str(ev, "id");
        if (json_get_str(ev, "model") && *json_get_str(ev, "model")) model_out = json_get_str(ev, "model");
        if (json_is_arr(json_get(ev, "citations"))) citations = json_get(ev, "citations");
        if (json_is_arr(json_get(ev, "search_results"))) results = json_get(ev, "search_results");
        if (json_is_arr(json_get(ev, "related_questions"))) related = json_get(ev, "related_questions");
        if (json_is_obj(json_get(ev, "usage"))) usage = json_get(ev, "usage");
        const char *d = json_get_str(json_get_obj(json_at(json_get_arr(ev, "choices"), 0), "delta"), "content");
        if (d) sb_append(&answer, d);
    }
    json_t *res = resp_new("perplexity"), *srcs = json_get(res, "sources"), *cits = json_arr(), *c;
    size_t ncites = 0;
    json_arr_foreach(citations, i, c) if (json_is_str(c) && *json_sv(c)) ncites++;
    if (ncites) {
        json_arr_foreach(citations, i, c) {
            if (!json_is_str(c) || !*json_sv(c)) continue;
            const char *u = json_sv(c);
            json_t *sr = NULL, *x;
            json_arr_foreach(results, k, x) if (!sr && str_eq(json_get_str(x, "url"), u)) sr = x;
            const char *title = json_get_str(sr, "title") ? json_get_str(sr, "title") : u;
            json_push(srcs, src_new(title, u, json_get_str(sr, "snippet"), json_get_str(sr, "date")));
            json_t *ci = json_obj();
            json_set_str(ci, "url", u);
            json_set_str(ci, "title", title);
            json_push(cits, ci);
        }
    } else {
        json_t *x;
        json_arr_foreach(results, k, x) {
            const char *u = json_get_str(x, "url");
            if (!u || !*u) continue;
            json_push(srcs, src_new(json_get_str(x, "title") ? json_get_str(x, "title") : u, u, json_get_str(x, "snippet"),
                                    json_get_str(x, "date")));
        }
    }
    if (answer.len) json_set_str(res, "answer", sb_cstr(&answer));
    sb_free(&answer);
    if (json_len(cits)) json_set(res, "citations", cits);
    else json_free(cits);
    json_t *rq = json_arr(), *q;
    json_arr_foreach(related, i, q) if (json_is_str(q) && !str_blank(json_sv(q))) json_push_str(rq, json_sv(q));
    if (json_len(rq)) json_set(res, "relatedQuestions", rq);
    else json_free(rq);
    int64_t in = json_get_int(usage, "prompt_tokens", 0), out = json_get_int(usage, "completion_tokens", 0);
    int64_t tot = json_get_int(usage, "total_tokens", in + out);
    if (in || out || tot) {
        json_t *uo = json_obj();
        json_set_int(uo, "inputTokens", in);
        json_set_int(uo, "outputTokens", out);
        json_set_int(uo, "totalTokens", tot);
        json_set(res, "usage", uo);
    }
    set_str_opt(res, "model", model_out ? model_out : "sonar-pro");
    set_str_opt(res, "requestId", id);
    json_set_str(res, "authMode", "api_key");
    json_free(events);
    if (!isnan(p->limit) && p->limit > 0 && json_len(srcs) > (size_t)p->limit) arr_truncate(srcs, (size_t)p->limit);
    return res;
}

static bool has_perplexity(const ai_model_t *m) {
    const char *k = key_kind("perplexity");
    return k && !str_eq(k, "oauth");
}

/* ---------- Kimi Code (providers/kimi.ts) ---------- */

static char *kimi_key(void) {
    char *k = env_trimmed("MOONSHOT_SEARCH_API_KEY");
    if (!k) k = env_trimmed("KIMI_SEARCH_API_KEY");
    if (!k && key_kind("kimi-code")) k = auth_get_api_key("kimi-code");
    return k;
}

static json_t *s_kimi(const ws_params_t *p, ws_err_t *e) {
    char *key = kimi_key();
    if (!key || !*key) {
        free(key);
        fail_plain(e, xstrdup("Kimi search credentials not found. Kimi web search uses the Kimi Code service (api.kimi.com); set "
                              "MOONSHOT_SEARCH_API_KEY / KIMI_SEARCH_API_KEY to a Kimi Code Console key, or login with 'omp "
                              "/login kimi-code'. A Moonshot Open Platform key (MOONSHOT_API_KEY) is not accepted here."));
        return NULL;
    }
    int limit = clamp_results(want_results(p), 10, 20);
    char *url = env_trimmed("MOONSHOT_SEARCH_BASE_URL");
    if (!url) url = env_trimmed("KIMI_SEARCH_BASE_URL");
    if (!url) url = xstrdup("https://api.kimi.com/coding/v1/search");
    json_t *body = json_obj();
    json_set_str(body, "text_query", p->query);
    json_set_int(body, "limit", limit);
    json_set_bool(body, "enable_page_crawling", false);
    json_set_int(body, "timeout_seconds", 30);
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    http_headers_set(&h, "Content-Type", "application/json");
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", url, &h, payload, p, &r);
    free(url);
    free(payload);
    free(key);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "kimi", r.status, r.body))
            fail_typed(e, "kimi", r.status, xasprintf("Kimi search API error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body), *it;
    const char *rid = http_headers_get(&r.headers, "x-request-id");
    if (!rid) rid = http_headers_get(&r.headers, "x-msh-request-id");
    json_t *res = resp_new("kimi"), *srcs = json_get(res, "sources");
    json_arr_foreach(json_get_arr(j, "search_results"), i, it) {
        const char *u = json_get_str(it, "url");
        if (!u || !*u) continue;
        char *date = json_get_str(it, "date") ? str_trim(json_get_str(it, "date")) : NULL;
        char *snip = json_get_str(it, "snippet") ? str_trim(json_get_str(it, "snippet")) : NULL;
        if (snip && !*snip) free(snip), snip = NULL;
        if (!snip && json_get_str(it, "content")) {
            snip = str_trim(json_get_str(it, "content"));
            if (!*snip) free(snip), snip = NULL;
        }
        char *title = json_get_str(it, "title") ? str_trim(json_get_str(it, "title")) : NULL;
        json_t *s = src_new(title && *title ? title : u, u, snip, date && *date ? date : NULL);
        char *site = json_get_str(it, "site_name") ? str_trim(json_get_str(it, "site_name")) : NULL;
        if (site && *site) json_set_str(s, "author", site);
        json_push(srcs, s);
        free(date);
        free(snip);
        free(title);
        free(site);
    }
    arr_truncate(srcs, (size_t)limit);
    if (rid) json_set_str(res, "requestId", rid);
    json_free(j);
    http_resp_free(&r);
    return res;
}

static bool has_kimi(const ai_model_t *m) {
    char *a = env_trimmed("MOONSHOT_SEARCH_API_KEY"), *b = env_trimmed("KIMI_SEARCH_API_KEY");
    bool r = a || b || key_kind("kimi-code");
    free(a);
    free(b);
    return r;
}

/* ---------- Z.AI (providers/zai.ts, web_search_prime over MCP) ---------- */

/* postZaiMcp: parsed JSON-RPC payload (owned; NULL when !expect), *session updated; false on failure (e set) */
static bool zai_post(const char *key, const char *method, json_t *params, char **session, bool expect, const ws_params_t *p,
                     json_t **out, ws_err_t *e) {
    http_headers_t h = {0};
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    http_headers_set(&h, "Content-Type", "application/json");
    http_headers_set(&h, "Accept", "application/json, text/event-stream");
    if (*session) http_headers_set(&h, "Mcp-Session-Id", *session);
    json_t *body = json_obj();
    json_set_str(body, "jsonrpc", "2.0");
    json_set_str(body, "method", method);
    json_set(body, "params", params);
    if (expect) json_set(body, "id", json_str_own(uuid_v4()));
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", "https://api.z.ai/api/mcp/web_search_prime/mcp", &h, payload, p, &r);
    free(payload);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return false;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "zai", r.status, r.body))
            fail_typed(e, "zai", r.status, xasprintf("Z.AI MCP error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return false;
    }
    const char *sid = http_headers_get(&r.headers, "Mcp-Session-Id");
    if (sid) {
        free(*session);
        *session = xstrdup(sid);
    }
    *out = NULL;
    if (expect) {
        /* parseZaiMcpResponse: the last data: message, else the whole body */
        json_t *last = NULL;
        for (const char *q = r.body ? r.body : ""; *q;) {
            const char *nl = strchr(q, '\n');
            size_t n = nl ? (size_t)(nl - q) : strlen(q);
            char *line = xstrndup(q, n), *t = str_trim(line);
            if (str_starts(t, "data:")) {
                char *d = str_trim(t + 5);
                json_t *j = *d ? json_parse_str(d) : NULL;
                if (j) json_free(last), last = j;
                free(d);
            }
            free(t);
            free(line);
            if (!nl) break;
            q = nl + 1;
        }
        if (!last) last = json_parse_str(r.body ? r.body : "");
        if (!last) {
            fail_typed(e, "zai", 500, xstrdup("Failed to parse Z.AI MCP response"));
            http_resp_free(&r);
            return false;
        }
        *out = last;
    }
    http_resp_free(&r);
    return true;
}

/* readJsonRpcPayload: false (e set) on an error payload */
static bool zai_check(const json_t *j, ws_err_t *e) {
    const char *dm = json_get_str(j, "msg");
    if (!dm) dm = json_get_str(j, "message");
    if (!dm) dm = json_get_str(j, "error_message");
    json_t *succ = json_get(j, "success");
    if (json_is_bool(succ) && !succ->u.b && dm && *dm) {
        json_t *code = json_get(j, "code");
        if (json_is_num(code) && jnum(code) != 0)
            fail_typed(e, "zai", (int)jnum(code), xasprintf("Z.AI API error (%lld): %s", (long long)jnum(code), dm));
        else fail_typed(e, "zai", 0, xasprintf("Z.AI API error: %s", dm));
        return false;
    }
    if (!json_is_obj(j)) {
        fail_typed(e, "zai", 500, xstrdup("Failed to parse Z.AI MCP response"));
        return false;
    }
    json_t *er = json_get_obj(j, "error");
    if (er) {
        json_t *code = json_get(er, "code");
        const char *m = json_get_str(er, "message");
        int st = json_is_num(code) ? (int)jnum(code) : 400;
        if (json_is_num(code) && jnum(code) != 0)
            fail_typed(e, "zai", st, xasprintf("Z.AI MCP error (%lld): %s", (long long)jnum(code), m ? m : "Unknown error"));
        else fail_typed(e, "zai", st, xasprintf("Z.AI MCP error: %s", m ? m : "Unknown error"));
        return false;
    }
    return true;
}

static json_t *zai_tool(const char *key, json_t *args, const ws_params_t *p, ws_err_t *e) {
    char *session = NULL;
    json_t *init = json_obj(), *ci = json_obj(), *out = NULL;
    json_set_str(init, "protocolVersion", "2025-03-26");
    json_set(init, "capabilities", json_obj());
    json_set_str(ci, "name", "omp-coding-agent");
    json_set_str(ci, "version", "1.0.0");
    json_set(init, "clientInfo", ci);
    if (!zai_post(key, "initialize", init, &session, true, p, &out, e) || !zai_check(out, e)) {
        json_free(out);
        json_free(args);
        free(session);
        return NULL;
    }
    json_free(out);
    out = NULL;
    if (!zai_post(key, "notifications/initialized", json_obj(), &session, false, p, &out, e)) {
        json_free(args);
        free(session);
        return NULL;
    }
    json_t *call = json_obj();
    json_set_str(call, "name", "web_search_prime");
    json_set(call, "arguments", args);
    bool ok = zai_post(key, "tools/call", call, &session, true, p, &out, e);
    free(session);
    if (!ok || !zai_check(out, e)) {
        json_free(out);
        return NULL;
    }
    json_t *result = json_get_obj(out, "result");
    if (result && json_get_bool(result, "isError", false)) {
        sb_t b = SB_INIT;
        json_t *c;
        json_arr_foreach(json_get_arr(result, "content"), i, c) {
            const char *t = json_get_str(c, "text");
            char *tt = t ? str_trim(t) : NULL;
            if (tt && *tt) {
                if (b.len) sb_putc(&b, '\n');
                sb_append(&b, tt);
            }
            free(tt);
        }
        char *text = str_trim(sb_cstr(&b));
        sb_free(&b);
        int st = 400;
        const char *me = str_istr(text, "MCP error");
        if (me) {
            const char *q = me + 9;
            while (*q == ' ') q++;
            if (*q == '-' || isdigit((unsigned char)*q)) st = abs(atoi(q));
        }
        fail_typed(e, "zai", st, *text ? text : (free(text), xstrdup("Z.AI MCP tool call failed")));
        json_free(out);
        return NULL;
    }
    json_t *r = json_get(out, "result") ? json_clone(json_get(out, "result")) : json_clone(out);
    json_free(out);
    return r;
}

static json_t *zai_results(const json_t *v) {
    if (json_is_arr(v)) return (json_t *)v;
    if (!json_is_obj(v)) return NULL;
    if (json_is_arr(json_get(v, "search_result"))) return json_get(v, "search_result");
    if (json_is_arr(json_get(v, "results"))) return json_get(v, "results");
    return NULL;
}

static json_t *s_zai(const ws_params_t *p, ws_err_t *e) {
    char *key = auth_get_api_key("zai");
    if (!key || !*key) {
        free(key);
        fail_plain(e, xstrdup("Z.AI credentials not found. Set ZAI_API_KEY or login with 'omp /login zai'."));
        return NULL;
    }
    double want = want_results(p);
    int count = isnan(want) || want == 0 ? 10 : (int)want;
    json_t *raw = NULL;
    for (int a = 0; a < 3 && !raw; a++) {
        json_t *args = json_obj();
        json_set_str(args, a == 0 ? "query" : "search_query", p->query);
        if (a == 2) json_set_str(args, "search_engine", "search-prime");
        json_set_int(args, "count", count);
        ws_err_t e2 = {0};
        raw = zai_tool(key, args, p, &e2);
        if (raw) {
            free(e2.msg);
            break;
        }
        bool arg_err = e2.prov && (e2.status == 400 || str_istr(e2.msg, "invalid") || str_istr(e2.msg, "argument") ||
                                   str_istr(e2.msg, "search_query") || str_istr(e2.msg, "query"));
        if (a == 2 || !arg_err || cancelled(p)) {
            fail_typed(e, e2.prov, e2.status, e2.msg);
            free(key);
            return NULL;
        }
        free(e2.msg);
    }
    free(key);
    /* parseSearchPayload */
    json_t *cands = json_arr(), *texts = json_arr();
    json_push(cands, json_clone(raw));
    if (json_is_obj(raw)) {
        static const char *const keys[] = {"structuredContent", "data", "result"};
        for (size_t k = 0; k < 3; k++)
            if (json_get(raw, keys[k]) && !json_is_null(json_get(raw, keys[k])) && !(json_is_bool(json_get(raw, keys[k])) && !json_get(raw, keys[k])->u.b))
                json_push(cands, json_clone(json_get(raw, keys[k])));
        json_t *part;
        json_arr_foreach(json_get_arr(raw, "content"), i, part) {
            const char *t = json_get_str(part, "text");
            char *tt = t ? str_trim(t) : NULL;
            if (!tt || !*tt) {
                free(tt);
                continue;
            }
            json_t *pj = json_parse_str(tt);
            if (pj && json_is_str(pj)) {
                json_t *inner = json_parse_str(json_sv(pj));
                if (inner) json_free(pj), pj = inner;
            }
            if (pj) {
                json_push(cands, pj);
                if (!zai_results(pj) || json_len(zai_results(pj)) == 0) json_push_str(texts, tt);
            } else json_push_str(texts, tt);
            free(tt);
        }
    }
    json_t *results = NULL, *owner = NULL, *c;
    json_arr_foreach(cands, i, c) {
        json_t *rs = zai_results(c);
        if (rs && json_len(rs)) {
            results = rs;
            owner = c;
            break;
        }
    }
    json_t *res = resp_new("zai"), *srcs = json_get(res, "sources"), *it;
    if (json_len(texts)) {
        sb_t b = SB_INIT;
        json_t *t;
        json_arr_foreach(texts, i, t) {
            if (b.len) sb_append(&b, "\n\n");
            sb_append(&b, json_sv(t));
        }
        json_set_str(res, "answer", sb_cstr(&b));
        sb_free(&b);
    }
    json_arr_foreach(results, i, it) {
        const char *u = json_get_str(it, "link");
        if (str_blank(u)) u = json_get_str(it, "url");
        if (str_blank(u)) continue;
        char *uu = str_trim(u);
        const char *pd = json_get_str(it, "publish_date");
        if (str_blank(pd)) pd = json_get_str(it, "publishedDate");
        char *date = !str_blank(pd) ? str_trim(pd) : NULL;
        const char *ti = json_get_str(it, "title");
        char *title = !str_blank(ti) ? str_trim(ti) : xstrdup(uu);
        const char *ct = json_get_str(it, "content");
        char *snip = !str_blank(ct) ? str_trim(ct) : NULL;
        json_t *s = src_new(title, uu, snip, date);
        const char *media = json_get_str(it, "media");
        if (!str_blank(media)) json_set(s, "author", json_str_own(str_trim(media)));
        json_push(srcs, s);
        free(uu);
        free(date);
        free(title);
        free(snip);
    }
    if (owner && json_is_obj(owner)) {
        const char *rid = json_get_str(owner, "request_id");
        if (!rid) rid = json_get_str(owner, "requestId");
        if (!rid) rid = json_get_str(owner, "id");
        set_str_opt(res, "requestId", rid);
    }
    if (!isnan(want) && want > 0 && json_len(srcs) > (size_t)want) arr_truncate(srcs, (size_t)want);
    json_free(cands);
    json_free(texts);
    json_free(raw);
    return res;
}

static bool has_zai(const ai_model_t *m) { return key_kind("zai") != NULL; }

/* ---------- Keenable (providers/keenable.ts) ---------- */

/* The part of parseSearchQuery (web/search/query.ts) Keenable reads: terms, `site:`, date bounds. Other constraint values come
 * back as keywords; omc has no central post-filter, so they filter nothing (see docs/tools.md). */
typedef struct {
    strvec_t terms, sites, in_title, in_url, in_text, filetypes;
    strvec_t neg_terms, excluded_sites; /* `-term` / `-"phrase"` (rendered) and `-site:` hosts, for engines with negation */
    char *before, *after;
    bool has_directives;
} kq_t;

static void kq_free(kq_t *q) {
    strvec_free(&q->neg_terms);
    strvec_free(&q->excluded_sites);
    strvec_free(&q->terms);
    strvec_free(&q->sites);
    strvec_free(&q->in_title);
    strvec_free(&q->in_url);
    strvec_free(&q->in_text);
    strvec_free(&q->filetypes);
    free(q->before);
    free(q->after);
}

/* a `"`, U+201C or U+201D at p: its byte length, else 0 */
static size_t kq_quote(const char *p) {
    if (*p == '"') return 1;
    return !strncmp(p, "\xE2\x80\x9C", 3) || !strncmp(p, "\xE2\x80\x9D", 3) ? 3 : 0;
}

/* parseDateValue: YYYY[-MM[-DD]] (also / and .) or MM/DD/YYYY (day first when the first field exceeds 12); owned ISO or NULL */
static char *kq_date(const char *v) {
    int a = 0, b = 0, c = 0, n = 0, la = 0;
    if (sscanf(v, "%d%n", &a, &n) != 1 || !n) return NULL;
    la = n;
    const char *p = v + n;
    int y, m = 1, d = 1;
    bool has_b = false, has_c = false;
    if (*p && strchr("-/.", *p) && isdigit((unsigned char)p[1])) {
        b = (int)strtol(p + 1, (char **)&p, 10);
        has_b = true;
        if (*p && strchr("-/.", *p) && isdigit((unsigned char)p[1])) {
            c = (int)strtol(p + 1, (char **)&p, 10);
            has_c = true;
        }
    }
    if (*p) return NULL;
    if (la == 4) {
        y = a;
        if (has_b) m = b;
        if (has_c) d = c;
    } else if (la <= 2 && has_c && c >= 1000 && c <= 9999) {
        y = c;
        m = a;
        d = b;
        if (m > 12 && d <= 12) {
            int t = m;
            m = d;
            d = t;
        }
    } else return NULL;
    if (y < 1000 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31) return NULL;
    return xasprintf("%d-%02d-%02d", y, m, d);
}

static char *kq_site(const char *value) { /* normalizeSite */
    char *s = str_trim(value);
    for (char *x = s; *x; x++) *x = (char)tolower((unsigned char)*x);
    const char *h = s;
    const char *sep = strstr(s, "://");
    if (sep && sep > s && isalpha((unsigned char)*s)) {
        bool scheme = true;
        for (const char *x = s; x < sep; x++) scheme &= isalnum((unsigned char)*x) || strchr("+.-", *x) != NULL;
        if (scheme) h = sep + 3;
    }
    if (!strncmp(h, "*.", 2)) h += 2;
    char *r = xstrdup(h);
    size_t n = strlen(r);
    while (n && (r[n - 1] == '/' || r[n - 1] == '.')) r[--n] = 0;
    free(s);
    return r;
}

/* `name:value` directive names: 1 site, 2 inurl, 3 intitle, 4 intext, 5 filetype, 6 before, 7 after, 8 lang; 0 none */
static int kq_field(const char *name, size_t n) {
    static const struct {
        const char *name;
        int field;
    } F[] = {{"site", 1},    {"domain", 1},   {"host", 1},     {"inurl", 2},  {"url", 2},   {"intitle", 3},
             {"title", 3},   {"intext", 4},   {"inbody", 4},   {"inanchor", 4}, {"filetype", 5}, {"ext", 5},
             {"before", 6},  {"until", 6},    {"after", 7},    {"since", 7},  {"lang", 8},  {"language", 8}};
    for (size_t i = 0; i < ARRAY_LEN(F); i++)
        if (strlen(F[i].name) == n && !strncasecmp(F[i].name, name, n)) return F[i].field;
    return 0;
}

static int kq_all_mode(const char *name, size_t n) { /* ALL_MODES: inTitle, inUrl, inText */
    if (n == 10 && !strncasecmp(name, "allintitle", n)) return 3;
    if (n == 8 && !strncasecmp(name, "allinurl", n)) return 2;
    if (n == 9 && !strncasecmp(name, "allintext", n)) return 4;
    return 0;
}

/* DIRECTIVE_PATTERN: [+-]?name:value. Returns the name's start and length and the value, or false */
static bool kq_directive(const char *t, const char **name, size_t *nl, const char **value, bool *minus) {
    const char *p = t;
    *minus = *p == '-';
    if (*p == '+' || *p == '-') p++;
    if (!isalpha((unsigned char)*p)) return false;
    const char *s = p;
    while (isalpha((unsigned char)*p) || *p == '-') p++;
    if (*p != ':') return false;
    *name = s;
    *nl = (size_t)(p - s);
    *value = p + 1;
    return true;
}

static void kq_constraint(kq_t *q, int field, const char *value, bool negated) {
    q->has_directives = true;
    char *v = str_trim(value);
    if (*v && negated && field == 1) {
        char *site = kq_site(v);
        if (*site) strvec_push(&q->excluded_sites, site);
        free(site);
    }
    if (*v && !negated) {
        if (field == 1) {
            char *site = kq_site(v);
            if (*site) strvec_push(&q->sites, site);
            free(site);
        } else if (field == 5) {
            const char *e = v;
            while (*e == '.') e++;
            char *low = xstrdup(e);
            for (char *x = low; *x; x++) *x = (char)tolower((unsigned char)*x);
            if (*low) strvec_push(&q->filetypes, low);
            free(low);
        } else {
            strvec_push(field == 2 ? &q->in_url : field == 3 ? &q->in_title : &q->in_text, v);
        }
    }
    free(v);
}

static void kq_parse(const char *raw, kq_t *q) {
    memset(q, 0, sizeof *q);
    strvec_t tok = {0};
    VEC(char) quoted = {0};
    for (const char *p = raw; *p;) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        size_t ql = kq_quote(p);
        if (ql) {
            const char *s = p + ql, *e = s;
            while (*e && !kq_quote(e)) e++;
            char *inner = xstrndup(s, (size_t)(e - s)), *t = str_trim(inner);
            free(inner);
            if (*t) {
                strvec_push(&tok, t);
                vec_push(&quoted, 1);
            }
            free(t);
            p = *e ? e + kq_quote(e) : e;
            continue;
        }
        sb_t b = SB_INIT;
        bool qv = false;
        while (*p && !isspace((unsigned char)*p)) {
            ql = kq_quote(p);
            if (ql && b.len && b.s[b.len - 1] == ':') { /* intitle:"budget tips" */
                const char *e = p + ql;
                while (*e && !kq_quote(e)) e++;
                sb_appendn(&b, p + ql, (size_t)(e - (p + ql)));
                qv = true;
                p = *e ? e + kq_quote(e) : e;
                continue;
            }
            if (ql) break;
            sb_putc(&b, *p++);
        }
        if (b.len) {
            const char *t = sb_cstr(&b);
            if (qv) {
                strvec_push(&tok, t);
                vec_push(&quoted, 2);
            } else { /* splitParens */
                while (*t == '(') {
                    strvec_push(&tok, "(");
                    vec_push(&quoted, 0);
                    t++;
                }
                size_t n = strlen(t), trailing = 0;
                while (n && t[n - 1] == ')') {
                    int depth = 0;
                    for (size_t i = 0; i + 1 < n; i++) depth += t[i] == '(' ? 1 : t[i] == ')' ? -1 : 0;
                    if (depth > 0) break;
                    n--;
                    trailing++;
                }
                if (n) {
                    char *w = xstrndup(t, n);
                    strvec_push(&tok, w);
                    vec_push(&quoted, 0);
                    free(w);
                }
                for (size_t k = 0; k < trailing; k++) {
                    strvec_push(&tok, ")");
                    vec_push(&quoted, 0);
                }
            }
        }
        sb_free(&b);
    }
    bool negate_next = false;
    int all_mode = 0;
    for (size_t i = 0; i < tok.n; i++) {
        const char *t = tok.v[i];
        bool phrase = quoted.v[i] == 1;
        if (!phrase) {
            if (!strcmp(t, "(") || !strcmp(t, ")")) continue;
            if (!strcmp(t, "OR") || !strcmp(t, "|") || !strcmp(t, "||") || !strcmp(t, "AND") || !strcmp(t, "&&")) {
                q->has_directives = true;
                continue;
            }
            if (!strcmp(t, "NOT") || !strcmp(t, "!")) {
                negate_next = true;
                q->has_directives = true;
                continue;
            }
            if (!strcmp(t, "-") || !strcmp(t, "+")) {
                if (t[0] == '-' && i + 1 < tok.n && quoted.v[i + 1] == 1) negate_next = true;
                continue;
            }
            const char *name, *value;
            size_t nl;
            bool minus;
            if (kq_directive(t, &name, &nl, &value, &minus)) {
                int am = kq_all_mode(name, nl), field = kq_field(name, nl);
                if (am) {
                    all_mode = am;
                    q->has_directives = true;
                    char *inl = str_trim(value);
                    if (*inl) kq_constraint(q, am, inl, minus);
                    free(inl);
                    continue;
                }
                if (field) {
                    char *v = str_trim(value);
                    if (!*v && i + 1 < tok.n) { /* `site: example.com` */
                        const char *next = tok.v[i + 1], *n2, *v2;
                        size_t l2;
                        bool m2;
                        bool reserved = !strcmp(next, "(") || !strcmp(next, ")") || !strcmp(next, "OR") || !strcmp(next, "AND") ||
                                        !strcmp(next, "NOT") || !strcmp(next, "|") || !strcmp(next, "||") || !strcmp(next, "&&") ||
                                        !strcmp(next, "!") ||
                                        (kq_directive(next, &n2, &l2, &v2, &m2) && (kq_field(n2, l2) || kq_all_mode(n2, l2)));
                        if (quoted.v[i + 1] == 1 || !reserved) {
                            free(v);
                            v = str_trim(next);
                            i++;
                        }
                    }
                    if (!*v) {
                        q->has_directives = true;
                        free(v);
                        continue;
                    }
                    bool negated = minus || negate_next;
                    negate_next = false;
                    if (field == 6 || field == 7) {
                        char *iso = kq_date(v);
                        if (iso) {
                            char **slot = field == 6 ? &q->before : &q->after;
                            free(*slot);
                            *slot = iso;
                            q->has_directives = true;
                        } else {
                            strvec_push(&q->terms, t); /* `before:someday` is a plain term */
                        }
                    } else if (field == 8) {
                        q->has_directives = true;
                    } else {
                        kq_constraint(q, field, v, negated);
                    }
                    free(v);
                    continue;
                }
            }
        }
        /* plain term, optional +/- prefix */
        const char *text = t;
        bool as_phrase = phrase, negated = false;
        if (!phrase && t[0] == '-' && t[1]) {
            negate_next = true;
            q->has_directives = true;
            while (*text == '-') text++;
            if (!*text) continue;
            const char *name, *value;
            size_t nl;
            bool minus;
            int f = kq_directive(text, &name, &nl, &value, &minus) ? kq_field(name, nl) : 0;
            if (f && f < 6) {
                negate_next = false;
                kq_constraint(q, f, value, true);
                continue;
            }
        } else if (!phrase && t[0] == '+' && t[1]) {
            text = t + 1;
            as_phrase = true;
            q->has_directives = true;
        }
        negated = negate_next;
        negate_next = false;
        if (all_mode) {
            kq_constraint(q, all_mode, text, negated);
            continue;
        }
        if (negated) { /* renderTerm: dropped without negation syntax; kept for engines that take it */
            char *w = as_phrase ? xasprintf("-\"%s\"", text) : xasprintf("-%s", text);
            strvec_push(&q->neg_terms, w);
            free(w);
            continue;
        }
        if (as_phrase) {
            char *w = xasprintf("\"%s\"", text);
            strvec_push(&q->terms, w);
            free(w);
        } else strvec_push(&q->terms, text);
    }
    strvec_free(&tok);
    vec_free(&quoted);
}

/* formatQuery({...parsed, raw: ""}, {phrases: true}): the terms, or the constraint values as plain keywords. Owned, "" when none. */
static char *kq_format(const kq_t *q) {
    strvec_t parts = {0};
    if (q->terms.n) for (size_t i = 0; i < q->terms.n; i++) strvec_push(&parts, q->terms.v[i]);
    else {
        const strvec_t *const L[] = {&q->sites, &q->in_title, &q->in_url, &q->in_text, &q->filetypes};
        for (size_t k = 0; k < ARRAY_LEN(L); k++)
            for (size_t i = 0; i < L[k]->n; i++) strvec_push(&parts, L[k]->v[i]);
    }
    char *joined = strvec_join(&parts, " ");
    strvec_free(&parts);
    char *t = str_trim(joined);
    free(joined);
    return t;
}

/* new URL(value).toString() for http(s) values, owned; NULL for anything else */
static char *ws_normalize_http_url(const char *v) {
    if (!v) return NULL;
    size_t sl = !strncasecmp(v, "https://", 8) ? 8 : !strncasecmp(v, "http://", 7) ? 7 : 0;
    if (!sl || !v[sl] || strpbrk(v, " \t\r\n")) return NULL;
    sb_t b = SB_INIT;
    size_t hl = strcspn(v + sl, "/?#");
    if (!hl) {
        sb_free(&b);
        return NULL;
    }
    for (size_t i = 0; i < sl + hl; i++) sb_putc(&b, (char)tolower((unsigned char)v[i]));
    if (v[sl + hl] != '/') sb_putc(&b, '/');
    sb_append(&b, v + sl + hl);
    return sb_detach(&b);
}

static json_t *s_keenable(const ws_params_t *p, ws_err_t *e) {
    int max = clamp_results(want_results(p), 10, 50);
    kq_t q;
    kq_parse(p->query, &q);
    char *query = q.has_directives ? kq_format(&q) : str_trim(p->query);
    if (!*query) {
        free(query);
        kq_free(&q);
        fail_typed(e, "keenable", 400, xstrdup("Keenable needs search terms; date-only queries are not supported"));
        return NULL;
    }
    json_t *body = json_obj();
    json_set_str(body, "query", query);
    json_set_int(body, "max_results", max);
    free(query);
    strvec_t hosts = {0}; /* siteHosts: bare, deduplicated */
    for (size_t i = 0; i < q.sites.n; i++) {
        char *h = xstrndup(q.sites.v[i], strcspn(q.sites.v[i], "/"));
        if (*h && !strvec_contains(&hosts, h)) strvec_push(&hosts, h);
        free(h);
    }
    if (hosts.n == 1) json_set_str(body, "site", hosts.v[0]);
    strvec_free(&hosts);
    /* Keenable accepts relative deltas on its date filters */
    const char *delta = str_eq(p->recency, "day") ? "1d" : str_eq(p->recency, "week") ? "7d" : str_eq(p->recency, "month") ? "1mo" : str_eq(p->recency, "year") ? "1y" : NULL;
    if (q.after) json_set_str(body, "published_after", q.after);
    else if (delta) json_set_str(body, "published_after", delta);
    if (q.before) json_set_str(body, "published_before", q.before);
    kq_free(&q);
    bool configured = key_kind("keenable") != NULL;
    char *key = configured ? auth_get_api_key("keenable") : NULL;
    /* a configured credential that fails to resolve must not silently downgrade to anonymous search */
    if (configured && (!key || !*key)) {
        free(key);
        json_free(body);
        fail_typed(e, "keenable", 0, xstrdup("Keenable credentials could not be resolved. Check your configured API key or credential helper."));
        return NULL;
    }
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    http_headers_set(&h, "Content-Type", "application/json");
    if (key) http_headers_set(&h, "X-API-Key", key);
    else http_headers_set(&h, "X-Keenable-Title", "oh-my-pi"); /* the keyless endpoint rejects requests without it */
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", key ? "https://api.keenable.ai/v1/search" : "https://api.keenable.ai/v1/search/public", &h, payload, p, &r);
    free(payload);
    http_headers_free(&h);
    bool keyed = key != NULL;
    free(key);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        char *txt = xstrndup(r.body ? r.body : "", MIN(r.body_len, (size_t)(8 * 1024)));
        if (!classify_http(e, "keenable", r.status, txt))
            fail_typed(e, "keenable", r.status, xasprintf("Keenable search API error (%d): %s", r.status, txt));
        free(txt);
        http_resp_free(&r);
        return NULL;
    }
    json_t *data = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    if (!data) {
        fail_typed(e, "keenable", 500, xstrdup("Keenable search API returned invalid JSON"));
        return NULL;
    }
    const json_t *results = json_get(data, "results");
    if (!json_is_arr(results)) {
        json_free(data);
        fail_typed(e, "keenable", 500, xstrdup("Keenable search API returned an unexpected response shape"));
        return NULL;
    }
    json_t *resp = resp_new("keenable"), *srcs = json_get(resp, "sources");
    strvec_t seen = {0};
    const json_t *x;
    json_arr_foreach(results, i, x) {
        if (!json_is_obj(x)) continue;
        char *url = ws_normalize_http_url(json_get_str(x, "url"));
        if (!url || strvec_contains(&seen, url)) {
            free(url);
            continue;
        }
        strvec_push(&seen, url);
        char *title = json_get_str(x, "title") ? ws_collapse_spaces(json_get_str(x, "title")) : xstrdup("");
        char *snip = json_get_str(x, "snippet") ? ws_collapse_spaces(json_get_str(x, "snippet")) : xstrdup("");
        const char *date = json_get_str(x, "published_at");
        if ((int)json_len(srcs) < max) json_push(srcs, src_new(*title ? title : url, url, *snip ? snip : NULL, date && *date ? date : NULL));
        free(snip);
        free(title);
        free(url);
    }
    strvec_free(&seen);
    json_free(data);
    json_set_str(resp, "authMode", keyed ? "api_key" : "anonymous");
    return resp;
}

static bool has_keenable(const ai_model_t *m) { return key_kind("keenable") != NULL; }

/* ---------- Sofya (providers/sofya.ts) ---------- */

#define SOFYA_MAX_DOMAIN_FILTERS 10 /* Sofya rejects more than 10 hosts in either domain list with a 422 */

/* siteHosts: bare, deduplicated hosts of site values, at most `cap`; NULL when none */
static json_t *sofya_hosts(const strvec_t *sites, size_t cap) {
    json_t *arr = json_arr();
    strvec_t seen = {0};
    for (size_t i = 0; i < sites->n && seen.n < cap; i++) {
        char *h = xstrndup(sites->v[i], strcspn(sites->v[i], "/"));
        if (*h && !strvec_contains(&seen, h)) {
            strvec_push(&seen, h);
            json_push_str(arr, h);
        }
        free(h);
    }
    strvec_free(&seen);
    if (!json_len(arr)) {
        json_free(arr);
        return NULL;
    }
    return arr;
}

/* POST https://sofya.co/v1/search (bearer SOFYA_API_KEY or a stored `sofya` credential): keywords, quoted phrases
 * and `-` exclusions; site:/-site: hosts as include/exclude_domains; after:/before: as a closed freshness range
 * (`before:` exclusive, a missing side spans to the epoch or today), else the recency window */
static json_t *s_sofya(const ws_params_t *p, ws_err_t *e) {
    int max = clamp_results(want_results(p), 10, 20);
    char *key = auth_get_api_key("sofya");
    if (!key || !*key) {
        free(key);
        fail_typed(e, "sofya", 0, xstrdup("Sofya credentials not found. Set SOFYA_API_KEY or configure an API key for provider \"sofya\"."));
        return NULL;
    }
    kq_t q;
    kq_parse(p->query, &q);
    char *kw = q.has_directives || q.neg_terms.n ? kq_format(&q) : str_trim(p->query);
    sb_t qb = SB_INIT;
    sb_append(&qb, kw);
    if (q.has_directives || q.neg_terms.n)
        for (size_t i = 0; i < q.neg_terms.n; i++) sb_appendf(&qb, "%s%s", qb.len ? " " : "", q.neg_terms.v[i]);
    free(kw);
    json_t *body = json_obj();
    json_set(body, "query", json_str_own(sb_detach(&qb)));
    json_set_str(body, "search_depth", "snippets"); /* 1 credit; `basic` page text would be truncated anyway */
    json_set_int(body, "max_results", max);
    json_t *inc = sofya_hosts(&q.sites, SOFYA_MAX_DOMAIN_FILTERS), *exc = sofya_hosts(&q.excluded_sites, SOFYA_MAX_DOMAIN_FILTERS);
    if (inc) json_set(body, "include_domains", inc);
    if (exc) json_set(body, "exclude_domains", exc);
    if (q.after || q.before) {
        char end[16];
        time_t t = time(NULL);
        if (q.before) {
            struct tm tm = {0};
            if (sscanf(q.before, "%4d-%2d-%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                t = timegm(&tm) - 86400;
            }
        }
        strftime(end, sizeof end, "%Y-%m-%d", gmtime(&t));
        json_set(body, "freshness", json_str_own(xasprintf("%s:%s", q.after ? q.after : "1970-01-01", end)));
    } else if (p->recency && *p->recency) {
        json_set_str(body, "freshness", p->recency);
    }
    kq_free(&q);
    http_headers_t h = {0};
    http_headers_set(&h, "Content-Type", "application/json");
    char *auth = xasprintf("Bearer %s", key);
    http_headers_set(&h, "Authorization", auth);
    free(auth);
    free(key);
    char *payload = json_dump(body, 0);
    json_free(body);
    http_resp_t r;
    int rc = ws_http("POST", "https://sofya.co/v1/search", &h, payload, p, &r);
    free(payload);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        char *txt = xstrndup(r.body ? r.body : "", MIN(r.body_len, (size_t)(8 * 1024)));
        if (!classify_http(e, "sofya", r.status, txt))
            fail_typed(e, "sofya", r.status, xasprintf("Sofya search API error (%d): %s", r.status, txt));
        free(txt);
        http_resp_free(&r);
        return NULL;
    }
    json_t *data = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    if (!data) {
        fail_typed(e, "sofya", 500, xstrdup("Sofya search API returned invalid JSON"));
        return NULL;
    }
    const json_t *results = json_get(data, "results");
    if (!json_is_arr(results)) {
        json_free(data);
        fail_typed(e, "sofya", 500, xstrdup("Sofya search API returned an unexpected response shape"));
        return NULL;
    }
    json_t *resp = resp_new("sofya"), *srcs = json_get(resp, "sources");
    strvec_t seen = {0};
    const json_t *x;
    json_arr_foreach(results, i, x) {
        if (!json_is_obj(x) || (int)json_len(srcs) >= max) continue;
        char *url = ws_normalize_http_url(json_get_str(x, "url"));
        if (!url || strvec_contains(&seen, url)) {
            free(url);
            continue;
        }
        strvec_push(&seen, url);
        char *title = json_get_str(x, "title") ? ws_collapse_spaces(json_get_str(x, "title")) : xstrdup("");
        const char *sraw = json_get_str(x, "description") && *json_get_str(x, "description") ? json_get_str(x, "description")
                                                                                           : json_get_str(x, "content");
        char *snip = sraw ? ws_collapse_spaces(sraw) : xstrdup("");
        const char *date = json_get_str(x, "published_date");
        json_push(srcs, src_new(*title ? title : url, url, *snip ? snip : NULL, date && *date ? date : NULL));
        free(snip);
        free(title);
        free(url);
    }
    strvec_free(&seen);
    /* a query Sofya rewrote is reported, so a dropped site: or quote is visible */
    const char *exec = json_get_str(data, "relaxed_query") && *json_get_str(data, "relaxed_query") ? json_get_str(data, "relaxed_query")
                                                                                                 : json_get_str(data, "altered_query");
    if (exec && *exec) {
        json_t *qs = json_arr();
        json_push(qs, json_str_own(ws_collapse_spaces(exec)));
        json_set(resp, "searchQueries", qs);
    }
    json_free(data);
    json_set_str(resp, "authMode", "api_key");
    return resp;
}

static bool has_sofya(const ai_model_t *m) { return key_kind("sofya") != NULL; }

/* ---------- SearXNG (providers/searxng.ts) ---------- */

/* SearXNG connection value: setting, else its environment variable */
static char *searxng_setting(const char *key, const char *env) {
    const char *v = settings_str(key, NULL);
    if (v && *v) return xstrdup(v);
    return env ? env_trimmed(env) : NULL;
}

/* URLSearchParams (application/x-www-form-urlencoded) value encoding */
static void form_encode(sb_t *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("*-._", *p)) sb_putc(b, (char)*p);
        else if (*p == ' ') sb_putc(b, '+');
        else sb_appendf(b, "%%%02X", *p);
    }
}

static bool has_ctl(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return true;
        if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) return true;
    }
    return false;
}

static char *searxng_answer_text(const json_t *a) {
    if (json_is_str(a)) {
        char *t = str_trim(json_sv(a));
        if (*t) return t;
        free(t);
        return NULL;
    }
    if (!json_is_obj(a)) return NULL;
    if (json_is_str(json_get(a, "answer"))) {
        char *t = str_trim(json_get_str(a, "answer"));
        if (*t) return t;
        free(t);
        return NULL;
    }
    if (json_is_arr(json_get(a, "translations"))) {
        sb_t b = SB_INIT;
        int n = 0;
        json_t *it;
        json_arr_foreach(json_get(a, "translations"), i, it) {
            const char *t = json_get_str(it, "text");
            if (str_blank(t)) continue;
            char *tt = str_trim(t);
            if (n++) sb_putc(&b, '\n');
            sb_append(&b, tt);
            free(tt);
            if (n == 3) break;
        }
        if (n) return sb_detach(&b);
        sb_free(&b);
    }
    json_t *cur = json_get_obj(a, "current");
    if (cur) {
        const char *sum = json_get_str(cur, "summary");
        if (!str_blank(sum)) return str_trim(sum);
        strvec_t parts = {0};
        const char *loc = json_get_str(json_get_obj(cur, "location"), "name");
        if (!str_blank(loc)) strvec_push(&parts, loc);
        json_t *temp = json_get_obj(cur, "temperature"), *val = json_get(temp, "val");
        if (json_is_str(val) || json_is_num(val)) {
            char *v = json_is_str(val) ? xstrdup(json_sv(val)) : json_dump(val, 0);
            const char *unit = json_get_str(temp, "unit");
            char *tv = xasprintf("%s%s", v, unit ? unit : "");
            if (!str_blank(tv)) strvec_push(&parts, tv);
            free(tv);
            free(v);
        }
        const char *cond = json_get_str(cur, "condition");
        if (!str_blank(cond)) strvec_push(&parts, cond);
        char *r = parts.n ? strvec_join(&parts, ": ") : NULL;
        strvec_free(&parts);
        return r;
    }
    return NULL;
}

static json_t *s_searxng(const ws_params_t *p, ws_err_t *e) {
    int n = clamp_results(want_results(p), 10, 20);
    char *endpoint = searxng_setting("searxng.endpoint", "SEARXNG_ENDPOINT");
    if (!endpoint) {
        fail_plain(e, xstrdup("SearXNG endpoint not configured. Set searxng.endpoint in settings or SEARXNG_ENDPOINT in "
                              "environment."));
        return NULL;
    }
    char *user = searxng_setting("searxng.basicUsername", "SEARXNG_BASIC_USERNAME");
    char *pass = searxng_setting("searxng.basicPassword", "SEARXNG_BASIC_PASSWORD");
    char *auth = NULL;
    const char *msg = NULL;
    if (user || pass) {
        if (!user || !pass)
            msg = "SearXNG Basic auth requires both searxng.basicUsername and searxng.basicPassword, or "
                  "SEARXNG_BASIC_USERNAME and SEARXNG_BASIC_PASSWORD.";
        else if (strchr(user, ':')) msg = "SearXNG Basic auth username cannot contain ':' because RFC 7617 uses it as the separator.";
        else if (has_ctl(user) || has_ctl(pass)) msg = "SearXNG Basic auth credentials must not contain RFC 7617 control characters.";
        else {
            char *raw = xasprintf("%s:%s", user, pass);
            char *b64 = base64_encode(raw, strlen(raw));
            auth = xasprintf("Basic %s", b64);
            free(b64);
            free(raw);
        }
    } else {
        char *tok = searxng_setting("searxng.token", "SEARXNG_TOKEN");
        if (tok) auth = xasprintf("Bearer %s", tok);
        free(tok);
    }
    free(user);
    free(pass);
    json_t *ss = settings_get("searxng.safesearch");
    if (!msg && ss && !json_is_null(ss) && !(json_is_num(ss) && (jnum(ss) == 0 || jnum(ss) == 1 || jnum(ss) == 2)))
        msg = "searxng.safesearch must be 0 (off), 1 (moderate), or 2 (strict).";
    if (msg) {
        fail_plain(e, xstrdup(msg));
        free(auth);
        free(endpoint);
        return NULL;
    }
    size_t el = strlen(endpoint);
    while (el && endpoint[el - 1] == '/') endpoint[--el] = 0;
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    if (auth) http_headers_set(&h, "Authorization", auth);
    free(auth);
    /* engines: shortcuts resolve through the instance's /config */
    const char *engines_cfg = settings_str("searxng.engines", NULL);
    char *engines = NULL;
    if (engines_cfg && *engines_cfg) {
        strvec_t parts = {0};
        char *dup = xstrdup(engines_cfg);
        for (char *tok = dup, *next; tok; tok = next) {
            next = strchr(tok, ',');
            if (next) *next++ = 0;
            char *t = str_trim(tok);
            if (*t) strvec_push(&parts, t);
            free(t);
        }
        free(dup);
        if (parts.n) {
            char *cfg_url = xasprintf("%s/config", endpoint);
            http_resp_t cr;
            json_t *cfg = NULL;
            if (ws_http("GET", cfg_url, &h, NULL, p, &cr) == 0 && cr.status >= 200 && cr.status < 300) cfg = json_parse_str(cr.body);
            http_resp_free(&cr);
            free(cfg_url);
            for (size_t i = 0; i < parts.n; i++) {
                json_t *eng;
                json_arr_foreach(json_get_arr(cfg, "engines"), k, eng) {
                    const char *nm = json_get_str(eng, "name"), *sc = json_get_str(eng, "shortcut");
                    if (!nm || !*nm) continue;
                    if (!strcasecmp(nm, parts.v[i]) || (sc && !strcasecmp(sc, parts.v[i]))) {
                        free(parts.v[i]);
                        parts.v[i] = xstrdup(nm);
                        break;
                    }
                }
            }
            json_free(cfg);
            engines = strvec_join(&parts, ",");
        }
        strvec_free(&parts);
    }
    /* stripExternalBangs */
    sb_t q = SB_INIT;
    {
        char *dup = xstrdup(p->query);
        bool first = true;
        for (char *tok = strtok(dup, " \t\n\r"); tok; tok = strtok(NULL, " \t\n\r")) {
            if (!strncmp(tok, "!!", 2)) continue;
            if (!first) sb_putc(&q, ' ');
            sb_append(&q, tok);
            first = false;
        }
        free(dup);
    }
    sb_t url = SB_INIT;
    sb_appendf(&url, "%s/search?q=", endpoint);
    form_encode(&url, sb_cstr(&q));
    sb_free(&q);
    sb_append(&url, "&format=json");
    if (!isnan(want_results(p)) && want_results(p) != 0) sb_append(&url, "&pageno=1");
    if (p->recency) sb_appendf(&url, "&time_range=%s", str_eq(p->recency, "day") ? "day" : str_eq(p->recency, "year") ? "year" : "month");
    const char *cats = settings_str("searxng.categories", NULL);
    if (cats && *cats) {
        sb_append(&url, "&categories=");
        form_encode(&url, cats);
    }
    if (engines) {
        sb_append(&url, "&engines=");
        form_encode(&url, engines);
    }
    if (json_is_num(ss)) sb_appendf(&url, "&safesearch=%d", (int)jnum(ss));
    const char *lang = settings_str("searxng.language", NULL);
    if (lang && *lang) {
        sb_append(&url, "&language=");
        form_encode(&url, lang);
    }
    free(engines);
    free(endpoint);
    http_resp_t r;
    int rc = ws_http("GET", sb_cstr(&url), &h, NULL, p, &r);
    sb_free(&url);
    http_headers_free(&h);
    if (rc != 0) {
        fail_transport(e, p, &r);
        http_resp_free(&r);
        return NULL;
    }
    if (r.status < 200 || r.status >= 300) {
        if (!classify_http(e, "searxng", r.status, r.body))
            fail_typed(e, "searxng", r.status, xasprintf("SearXNG API error (%d): %s", r.status, r.body ? r.body : ""));
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body), *it;
    http_resp_free(&r);
    json_t *srcs = json_arr();
    json_arr_foreach(json_get_arr(j, "results"), i, it) {
        const char *u = json_get_str(it, "url");
        if (!u || !*u) continue;
        const char *pd = json_get_str(it, "publishedDate");
        if (!pd) pd = json_get_str(it, "published_date");
        const char *ct = json_get_str(it, "content");
        if (!ct) ct = json_get_str(it, "snippet");
        char *snip = ct ? str_trim(ct) : NULL;
        if (snip && !*snip) free(snip), snip = NULL;
        json_push(srcs, src_new(json_get_str(it, "title") ? json_get_str(it, "title") : u, u, snip, pd));
        free(snip);
    }
    arr_truncate(srcs, (size_t)n);
    json_t *unresp = json_get_arr(j, "unresponsive_engines");
    if (json_len(srcs) == 0 && json_len(unresp)) {
        sb_t b = SB_INIT;
        json_t *pair;
        json_arr_foreach(unresp, i, pair) {
            if (b.len) sb_append(&b, "; ");
            const char *en = json_is_str(json_at(pair, 0)) ? json_sv(json_at(pair, 0)) : "undefined";
            const char *rs = json_is_str(json_at(pair, 1)) ? json_sv(json_at(pair, 1)) : "undefined";
            sb_appendf(&b, "%s: %s", en, rs);
        }
        fail_typed(e, "searxng", 503, xasprintf("SearXNG returned no usable results; upstream engines failed: %s", sb_cstr(&b)));
        sb_free(&b);
        json_free(srcs);
        json_free(j);
        return NULL;
    }
    json_t *res = json_obj();
    json_set_str(res, "provider", "searxng");
    sb_t ans = SB_INIT;
    int na = 0;
    json_t *a;
    json_arr_foreach(json_get_arr(j, "answers"), i, a) {
        char *t = searxng_answer_text(a);
        if (!t) continue;
        if (na++) sb_append(&ans, "\n\n");
        sb_append(&ans, t);
        free(t);
        if (na == 3) break;
    }
    if (na) json_set_str(res, "answer", sb_cstr(&ans));
    sb_free(&ans);
    json_set(res, "sources", srcs);
    json_t *sug = json_get_arr(j, "suggestions");
    if (json_len(sug)) json_set(res, "relatedQuestions", json_clone(sug));
    json_free(j);
    return res;
}

static bool has_searxng(const ai_model_t *m) {
    char *e = searxng_setting("searxng.endpoint", "SEARXNG_ENDPOINT");
    bool r = e != NULL;
    free(e);
    return r;
}

/* ---------- registry (provider.ts) ---------- */

typedef json_t *(*search_fn)(const ws_params_t *, ws_err_t *);
typedef bool (*avail_fn)(const ai_model_t *);
typedef struct {
    const char *id;
    search_fn search;
    avail_fn avail;          /* role-chain admission */
    avail_fn explicit_avail; /* explicit selection (NULL = avail) */
} provider_t;

static const provider_t ENGINES[] = {
    {"parallel", s_parallel, always, NULL},     {"brave", s_brave, has_brave, NULL},
    {"tavily", s_tavily, has_tavily, NULL},     {"exa", s_exa, has_exa, NULL},
    {"duckduckgo", s_ddg, always, always},      {"perplexity", s_perplexity, has_perplexity, NULL},
    {"kimi", s_kimi, has_kimi, NULL},           {"zai", s_zai, has_zai, NULL},
    {"searxng", s_searxng, has_searxng, NULL},  {"keenable", s_keenable, has_keenable, always},
    {"sofya", s_sofya, has_sofya, NULL},
};
static const provider_t GROUNDED[] = {
    {"anthropic", s_anthropic, has_anthropic, NULL}, {"openai", s_openai, has_openai, NULL},
    {"codex", s_codex, has_codex, NULL},             {"gemini", s_gemini, has_gemini, NULL},
    {"xai", s_xai, has_xai, NULL},
};
/* SearchProvider.label (codex reads "OpenAI" there, "OpenAI Codex" in the renderer labels) */
static const char *provider_label(const char *id) { return str_eq(id, "codex") ? "OpenAI" : label_of(id); }

static const provider_t *find_provider(const provider_t *tab, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++)
        if (str_eq(tab[i].id, id)) return &tab[i];
    return NULL;
}

/* ---------- executeSearch (web/search/index.ts) ---------- */

typedef struct {
    char *id, *label; /* provider meta */
    ws_err_t err;
} failure_t;

static char *format_failure(const failure_t *f);

/* failureDetails: reason flattened and capped, the way the fallback note quotes it */
static json_t *failure_details(const failure_t *v, size_t n) {
    json_t *arr = json_arr();
    for (size_t k = 0; k < n; k++) {
        char *reason = format_failure(&v[k]);
        char *col = ws_collapse_spaces(reason);
        size_t cl = strlen(col);
        if (cl && col[cl - 1] == '.') col[cl - 1] = 0;
        char *tr = ws_truncate(col, 240);
        json_t *o = json_obj();
        json_set_str(o, "provider", v[k].id);
        json_set_str(o, "label", v[k].label);
        json_set_str(o, "reason", tr);
        if (v[k].err.prov && v[k].err.status) json_set_int(o, "status", v[k].err.status);
        json_push(arr, o);
        free(tr);
        free(col);
        free(reason);
    }
    return arr;
}

/* formatSearchProviderFailure */
static char *format_failure(const failure_t *f) {
    const ws_err_t *e = &f->err;
    if (e->prov) {
        if (str_eq(e->prov, "anthropic") && e->status == 404)
            return xstrdup("Anthropic web search returned 404 (model or endpoint not found).");
        if ((e->status == 401 || e->status == 403) && !str_eq(e->prov, "zai"))
            return xasprintf("%s authorization failed (%d). Check API key or base URL.", label_of(e->prov), e->status);
    }
    return xstrdup(e->msg ? e->msg : "");
}

static bool is_hosted(const ai_model_t *m) { return str_eq(m->provider, "web") && str_eq(m->id, "hosted"); }

typedef struct {
    const ai_model_t *model;
    bool explicit_;
    const char *level;
} cand_t;
typedef VEC(cand_t) cands_t;

/* resolveConfiguredModelTarget(target, current, pool) */
static const ai_model_t *configured_target(const char *target, const ai_model_t *cur, const mr_pool_t *pool) {
    if (!target || !*target) return NULL;
    const char *slash = strchr(target, '/');
    for (size_t i = 0; slash && i < pool->n; i++) {
        const ai_model_t *m = pool->v[i];
        size_t pl = (size_t)(slash - target);
        if (strlen(m->provider) == pl && !strncmp(m->provider, target, pl) && str_eq(m->id, slash + 1)) return m;
    }
    for (size_t i = 0; i < pool->n; i++)
        if (str_eq(pool->v[i]->provider, cur->provider) && str_eq(pool->v[i]->id, target)) return pool->v[i];
    return NULL;
}

/* buildModelProviderPriorityRank for the xAI pair: modelProviderOrder, then the built-in xai-oauth > xai */
static int xai_rank(const ai_model_t *m) {
    strvec_t order = mr_pattern_list(settings_str("modelProviderOrder", ""));
    json_t *arr = settings_get("modelProviderOrder");
    if (json_is_arr(arr)) {
        strvec_free(&order);
        json_t *v;
        json_arr_foreach(arr, i, v) if (json_is_str(v)) strvec_push(&order, json_sv(v));
    }
    int r = -1;
    for (size_t i = 0; i < order.n && r < 0; i++)
        if (!strcasecmp(order.v[i], m->provider)) r = (int)i;
    strvec_free(&order);
    if (r >= 0) return r;
    return 1000 + (str_eq(m->provider, "xai-oauth") ? 0 : str_eq(m->provider, "xai") ? 1 : 2);
}

/* rankXAIProviders: xAI-grounded entries reordered among their own slots */
static void rank_xai(cands_t *c) {
    for (size_t i = 0; i < c->n; i++) {
        if (!str_eq(model_web_search(c->v[i].model), "xai")) continue;
        for (size_t j = i + 1; j < c->n; j++) {
            if (!str_eq(model_web_search(c->v[j].model), "xai")) continue;
            if (xai_rank(c->v[j].model) < xai_rank(c->v[i].model)) {
                cand_t t = c->v[i];
                c->v[i] = c->v[j];
                c->v[j] = t;
            }
        }
    }
}

static bool x_host(const char *host, size_t n) {
    static const char *const hosts[] = {"x.com", "twitter.com", "www.x.com", "www.twitter.com", "mobile.x.com",
                                        "mobile.twitter.com"};
    for (size_t i = 0; i < ARRAY_LEN(hosts); i++)
        if (strlen(hosts[i]) == n && !strncasecmp(hosts[i], host, n)) return true;
    return false;
}

/* targetsX over the plain query text: `from:<handle>` terms, or every site: on an X host */
static bool targets_x(const char *query) {
    char *dup = xstrdup(query);
    bool from = false, any_site = false, all_x = true;
    for (char *tok = strtok(dup, " \t\n\r"); tok; tok = strtok(NULL, " \t\n\r")) {
        if (!strncasecmp(tok, "from:", 5)) {
            const char *h = tok + 5;
            if (*h == '@') h++;
            size_t n = strlen(h);
            bool ok = n >= 1 && n <= 15;
            for (size_t i = 0; i < n; i++)
                if (!isalnum((unsigned char)h[i]) && h[i] != '_') ok = false;
            if (ok) from = true;
        } else if (!strncasecmp(tok, "site:", 5) && tok[5]) {
            any_site = true;
            const char *h = tok + 5, *sl = strchr(h, '/');
            if (!x_host(h, sl ? (size_t)(sl - h) : strlen(h))) all_x = false;
        }
    }
    free(dup);
    return from || (any_site && all_x);
}

/* formatForLLM */
static char *format_for_llm(const json_t *r, const strvec_t *notes) {
    strvec_t parts = {0};
    for (size_t i = 0; i < notes->n; i++) strvec_push_own(&parts, xasprintf("Note: %s", notes->v[i]));
    json_t *srcs = json_get(r, "sources");
    size_t ns = json_len(srcs);
    const char *answer = json_get_str(r, "answer");
    if (answer && *answer) {
        strvec_push(&parts, answer);
        if (ns) {
            strvec_push(&parts, "\n## Sources");
            strvec_push_own(&parts, xasprintf("%zu source%s", ns, ns == 1 ? "" : "s"));
        }
    }
    json_t *s;
    json_arr_foreach(srcs, i, s) {
        json_t *ag = json_get(s, "ageSeconds");
        char *age = json_is_num(ag) ? ws_format_age((int64_t)jnum(ag)) : xstrdup("");
        const char *shown = *age ? age : json_get_str(s, "publishedDate");
        char *line = shown && *shown ? xasprintf("[%zu] %s (%s)\n    %s", i + 1, json_get_str(s, "title"), shown, json_get_str(s, "url"))
                                     : xasprintf("[%zu] %s\n    %s", i + 1, json_get_str(s, "title"), json_get_str(s, "url"));
        strvec_push_own(&parts, line);
        free(age);
        const char *sn = json_get_str(s, "snippet");
        if (sn && *sn) {
            char *t = ws_truncate(sn, 240);
            strvec_push_own(&parts, xasprintf("    %s", t));
            free(t);
        }
    }
    json_t *cits = json_get(r, "citations");
    if (json_len(cits)) {
        strvec_push(&parts, "\n## Citations");
        size_t nc = json_len(cits);
        strvec_push_own(&parts, xasprintf("%zu citation%s", nc, nc == 1 ? "" : "s"));
        json_t *c;
        json_arr_foreach(cits, i, c) {
            const char *t = json_get_str(c, "title");
            const char *u = json_get_str(c, "url");
            strvec_push_own(&parts, xasprintf("[%zu] %s\n    %s", i + 1, t && *t ? t : u, u ? u : ""));
            const char *ct = json_get_str(c, "citedText");
            if (ct && *ct) {
                char *tt = ws_truncate(ct, 240);
                strvec_push_own(&parts, xasprintf("    %s", tt));
                free(tt);
            }
        }
    }
    json_t *rq = json_get(r, "relatedQuestions");
    if (json_len(rq)) {
        strvec_push(&parts, "\n## Related");
        size_t nq = json_len(rq);
        strvec_push_own(&parts, xasprintf("%zu question%s", nq, nq == 1 ? "" : "s"));
        json_t *q;
        json_arr_foreach(rq, i, q) if (json_is_str(q)) strvec_push_own(&parts, xasprintf("- %s", json_sv(q)));
    }
    json_t *sq = json_get(r, "searchQueries");
    if (json_len(sq)) {
        strvec_push_own(&parts, xasprintf("Search queries: %zu", json_len(sq)));
        json_t *q;
        json_arr_foreach(sq, i, q) {
            if (i >= 3) break;
            char *t = ws_truncate(json_is_str(q) ? json_sv(q) : "", 120);
            strvec_push_own(&parts, xasprintf("- %s", t));
            free(t);
        }
    }
    char *out = strvec_join(&parts, "\n");
    strvec_free(&parts);
    return out;
}

/* hasRenderableSearchContent */
static bool renderable(const json_t *r) {
    if (!str_blank(json_get_str(r, "answer"))) return true;
    if (json_len(json_get(r, "sources")) || json_len(json_get(r, "citations"))) return true;
    json_t *x;
    json_arr_foreach(json_get(r, "relatedQuestions"), i, x) if (json_is_str(x) && !str_blank(json_sv(x))) return true;
    json_arr_foreach(json_get(r, "searchQueries"), i, x) if (json_is_str(x) && !str_blank(json_sv(x))) return true;
    return false;
}

static double arg_num(const json_t *args, const char *k) {
    json_t *v = json_get(args, k);
    return json_is_num(v) ? jnum(v) : NAN;
}

char *ws_execute(const json_t *args, const char *model_sel, const ai_model_t *session_model, const char *session_id,
                 volatile sig_atomic_t *cancel, json_t **details, bool *is_error) {
    *is_error = false;
    mr_pool_t pool = {0};
    mr_pool_role(&pool, "web");
    cands_t cands = {0};
    if (model_sel) {
        mr_result_t rr = {0};
        if (mr_resolve_value(model_sel, &pool, &rr) && rr.model) vec_push(&cands, ((cand_t){rr.model, true, rr.level}));
        mr_result_clear(&rr);
    } else {
        mr_chain_t chain = {0};
        mr_role_chain("web", &pool, &chain);
        for (size_t i = 0; i < chain.n; i++) vec_push(&cands, ((cand_t){chain.v[i].model, chain.v[i].explicit_, chain.v[i].level}));
        mr_chain_free(&chain);
    }
    /* expandHostedCandidate */
    cands_t exp = {0};
    for (size_t i = 0; i < cands.n; i++) {
        cand_t c = cands.v[i];
        if (!is_hosted(c.model) || !session_model) {
            vec_push(&exp, c);
            continue;
        }
        const ai_model_t *swap = configured_target(model_web_search_model(session_model), session_model, &pool);
        if (swap == session_model || (swap && str_eq(swap->provider, session_model->provider) && str_eq(swap->id, session_model->id)))
            swap = NULL;
        size_t before = exp.n;
        if (swap && model_web_search(swap)) vec_push(&exp, ((cand_t){swap, c.explicit_, c.level}));
        if (model_web_search(session_model)) vec_push(&exp, ((cand_t){session_model, c.explicit_, c.level}));
        if (exp.n == before) vec_push(&exp, c);
    }
    vec_free(&cands);
    rank_xai(&exp);
    const char *query = json_get_str(args, "query");
    if (!query) query = "";
    /* preferXAI: only xAI reaches X posts */
    if (!model_sel && targets_x(query)) {
        cands_t x = {0};
        for (size_t i = 0; i < exp.n; i++)
            if (str_eq(model_web_search(exp.v[i].model), "xai")) vec_push(&x, exp.v[i]);
        if (x.n == 0) {
            mr_pool_t grounded = {0};
            for (size_t i = 0; i < pool.n; i++)
                if (str_eq(model_web_search(pool.v[i]), "xai")) mr_pool_push(&grounded, pool.v[i]);
            const ai_model_t *fb = mr_pick_default_available(&grounded);
            if (fb) {
                const ai_model_t *runner = configured_target(model_web_search_model(fb), fb, &grounded);
                vec_push(&x, ((cand_t){runner ? runner : fb, false, NULL}));
            }
            mr_pool_free(&grounded);
        }
        for (size_t i = 0; i < exp.n; i++)
            if (!str_eq(model_web_search(exp.v[i].model), "xai")) vec_push(&x, exp.v[i]);
        vec_free(&exp);
        exp = x;
    }
    char *sys = prompt_load("coding-agent/prompts/system/web-search.md"); /* raw text import, like omp */
    int64_t secs = settings_int("providers.webSearchTimeoutSeconds", 60);
    ws_params_t p = {.query = query,
                     .recency = json_get_str(args, "recency"),
                     .system_prompt = sys,
                     .session_id = session_id,
                     .limit = arg_num(args, "limit"),
                     .num_results = arg_num(args, "num_search_results"),
                     .max_tokens = arg_num(args, "max_tokens"),
                     .temperature = arg_num(args, "temperature"),
                     .cancel = cancel,
                     .timeout_ms = (secs > 0 ? MIN(secs, (int64_t)300) : 60) * 1000};
    VEC(failure_t) fails = {0};
    int available = 0;
    char *text = NULL;
    const char *failed_provider = "none";
    for (size_t i = 0; i < exp.n && !text; i++) {
        const ai_model_t *m = exp.v[i].model;
        failure_t f = {xstrdup(m->id), xstrdup(m->name ? m->name : m->id), {0}};
        const provider_t *prov = NULL;
        if (is_hosted(m)) {
            if (!exp.v[i].explicit_) {
                free(f.id);
                free(f.label);
                continue;
            }
            fail_plain(&f.err, xstrdup("The session model has no web search grounding."));
        } else if (str_eq(mr_model_kind(m), "search")) {
            prov = find_provider(ENGINES, ARRAY_LEN(ENGINES), m->id);
            if (!prov && !exp.v[i].explicit_) { /* engines omc does not ship are never admitted by the auto chain */
                free(f.id);
                free(f.label);
                continue;
            }
            if (!prov) fail_plain(&f.err, xasprintf("Web search engine %s/%s is not supported by omc.", m->provider, m->id));
        } else if (model_web_search(m)) {
            prov = find_provider(GROUNDED, ARRAY_LEN(GROUNDED), model_web_search(m));
            if (!prov && !exp.v[i].explicit_) {
                free(f.id);
                free(f.label);
                continue;
            }
            if (!prov)
                fail_plain(&f.err, xasprintf("%s web search grounding is not supported by omc.", label_of(model_web_search(m))));
        } else {
            fail_plain(&f.err, xasprintf("Model %s/%s does not support web search", m->provider, m->id));
        }
        if (prov) {
            free(f.id);
            free(f.label);
            f.id = xstrdup(prov->id);
            f.label = xstrdup(provider_label(prov->id));
            bool ok = exp.v[i].explicit_ && prov->explicit_avail ? prov->explicit_avail(m) : prov->avail(m);
            if (!ok && !exp.v[i].explicit_) {
                free(f.id);
                free(f.label);
                continue;
            }
            if (!ok) {
                fail_typed(&f.err, prov->id, 0,
                           xasprintf("%s web search is unavailable. Configure its credentials or select the automatic provider "
                                     "chain.",
                                     provider_label(prov->id)));
            } else {
                available++;
                p.model = m;
                p.level = exp.v[i].level;
                p.explicit_ = exp.v[i].explicit_;
                json_t *r = prov->search(&p, &f.err);
                if (r && !str_eq(mr_model_kind(m), "search") && json_len(json_get(r, "sources")) == 0 &&
                    json_len(json_get(r, "citations")) == 0) {
                    fail_typed(&f.err, prov->id, 204,
                               xasprintf("%s returned no sources; %s/%s may not support hosted web search.",
                                         provider_label(prov->id), m->provider, m->id));
                    json_free(r);
                    r = NULL;
                }
                if (r && !renderable(r)) {
                    fail_typed(&f.err, prov->id, 204, xasprintf("%s returned no renderable search content.", provider_label(prov->id)));
                    json_free(r);
                    r = NULL;
                }
                if (r) {
                    r = resp_finish(r);
                    strvec_t notes = {0};
                    json_t *failed = fails.n ? failure_details(fails.v, fails.n) : NULL;
                    if (failed) {
                        sb_t b = SB_INIT;
                        sb_appendf(&b, "answered by %s after ", provider_label(prov->id));
                        const json_t *fx;
                        json_arr_foreach(failed, k, fx)
                            sb_appendf(&b, "%s%s failed (%s)", k ? " and " : "", json_get_str(fx, "label"), json_get_str(fx, "reason"));
                        strvec_push_own(&notes, sb_detach(&b));
                    }
                    text = format_for_llm(r, &notes);
                    strvec_free(&notes);
                    *details = json_obj();
                    json_set(*details, "response", r);
                    if (failed) json_set(*details, "failures", failed);
                    free(f.id);
                    free(f.label);
                    break;
                }
            }
        }
        if (cancel && *cancel) {
            free(f.err.msg);
            free(f.id);
            free(f.label);
            text = xstrdup("Error: Request was aborted");
            *details = json_obj();
            json_t *resp = json_obj();
            json_set_str(resp, "provider", "none");
            json_set(resp, "sources", json_arr());
            json_set(*details, "response", resp);
            json_set_str(*details, "error", "Request was aborted");
            *is_error = true;
            break;
        }
        failed_provider = prov ? prov->id : "none";
        vec_push(&fails, f);
    }
    if (!text) {
        char *msg;
        if (available == 0 && fails.n == 0)
            msg = model_sel ? xasprintf("No web search model matches selector \"%s\".", model_sel)
                            : xstrdup("No web search model configured.");
        else if (fails.n > 1) {
            sb_t b = SB_INIT;
            sb_append(&b, "All web search providers failed: ");
            for (size_t k = 0; k < fails.n; k++) {
                char *fm = format_failure(&fails.v[k]);
                sb_appendf(&b, "%s%s: %s", k ? "; " : "", fails.v[k].id, fm);
                free(fm);
            }
            msg = sb_detach(&b);
        } else msg = format_failure(&fails.v[0]);
        text = xasprintf("Error: %s", msg);
        *details = json_obj();
        json_t *resp = json_obj();
        json_set_str(resp, "provider", available == 0 && fails.n == 0 ? "none" : failed_provider);
        json_set(resp, "sources", json_arr());
        json_set(*details, "response", resp);
        json_set_str(*details, "error", msg);
        if (!(available == 0 && fails.n == 0)) json_set(*details, "failures", failure_details(fails.v, fails.n));
        free(msg);
    }
    for (size_t k = 0; k < fails.n; k++) {
        free(fails.v[k].id);
        free(fails.v[k].label);
        free(fails.v[k].err.msg);
    }
    vec_free(&fails);
    vec_free(&exp);
    mr_pool_free(&pool);
    free(sys);
    return text;
}

/* ---------- the tool ---------- */

static int web_search_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    char *sid = NULL;
    const ai_model_t *sm = env && env->active_model ? env->active_model(env, &sid) : NULL;
    json_t *details = NULL;
    bool is_error = false;
    char *text = ws_execute(args, NULL, sm, sid, ctx->cancel, &details, &is_error);
    tool_result_text(out, text);
    out->details = details;
    out->is_error = is_error;
    free(text);
    free(sid);
    return 0;
}

agent_tool_t *tool_web_search_new(tool_env_t *env) {
    /* plain description: the X operators (from:, site:x.com handles) are not mapped onto x_search filters */
    json_t *ctx = json_obj();
    char *desc = tool_prompt("web-search", ctx);
    json_free(ctx);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"recency\":{\"enum\":"
                           "[\"day\",\"week\",\"month\",\"year\"],\"type\":\"string\"},\"limit\":{\"type\":\"number\"},"
                           "\"max_tokens\":{\"type\":\"number\"},\"temperature\":{\"type\":\"number\"},"
                           "\"num_search_results\":{\"type\":\"number\"}},\"required\":[\"query\"],"
                           "\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("web_search", "Web Search", desc, s, web_search_execute);
    free(desc);
    t->ud = env;
    return t;
}
