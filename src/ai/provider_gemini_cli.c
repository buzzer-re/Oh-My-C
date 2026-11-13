/*
 * provider_gemini_cli.c - api "google-gemini-cli": Google Cloud Code Assist `v1internal:streamGenerateContent`
 * (providers google-gemini-cli and google-antigravity). Port of packages/ai/src/providers/google-gemini-cli.ts and
 * the google-gemini-cli branch of stream.ts option mapping. Message/tool mapping is shared with provider_google.c.
 *
 * The api key is the structured OAuth credential `{"token","projectId",...}` produced by auth.c.
 */
#include "ai/ai_images.h"
#include "ai/provider_google.h"

#include "ai/google_cca.h"
#include "ai/think_heal.h"
#include "core/embed.h"

#include <math.h>
#include <openssl/sha.h>
#include <pthread.h>

#define MAX_RETRIES 3
#define DEFAULT_FIRST_EVENT_TIMEOUT_MS 300000
#define RATE_LIMIT_BUDGET_MS (5 * 60 * 1000)
#define CLAUDE_THINKING_BETA_HEADER "interleaved-thinking-2025-05-14"
#define MIN_OUTPUT_TOKENS 1024
#define OUTPUT_CAP_WHEN_UNKNOWN 64000
#define MAX_EMPTY_STREAM_RETRIES 2
#define EMPTY_STREAM_BASE_DELAY_MS 500
#define INT63_MASK 0x7fffffffffffffffULL
#define ANTIGRAVITY_RANDOM_BOUND 9000000000000000000ULL

static bool cflag(const ai_model_t *m, const char *key) {
    json_t *v = model_compat_get(m, key);
    return json_is_bool(v) && v->u.b;
}

static bool str_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++)
        if (!(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v')) return false;
    return true;
}

static const char *identity_class(const ai_model_t *m) { return json_get_str(json_get_obj(m->raw, "identity"), "class"); }

/* ======================================================================
 * credentials
 * ====================================================================== */

void cca_creds_free(cca_creds_t *c) {
    free(c->token);
    free(c->project_id);
    free(c->email);
    free(c->account_id);
    memset(c, 0, sizeof(*c));
}

static int64_t normalize_expiry_ms(const json_t *v) {
    if (!json_is_num(v) || !isfinite(v->u.num.d) || v->u.num.d <= 0) return 0;
    double d = v->u.num.d;
    return (int64_t)(d < 10000000000.0 ? d * 1000 : d);
}

int cca_parse_credentials(const char *api_key, cca_creds_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    *err = NULL;
    json_t *j = api_key ? json_parse_str(api_key) : NULL;
    if (!j) {
        *err = xstrdup("Invalid Google Cloud Code Assist credentials. Use /login to re-authenticate.");
        return -1;
    }
    const char *tok = json_get_str(j, "token");
    const char *pid = json_get_str(j, "projectId");
    if (!pid) pid = json_get_str(j, "project_id");
    if (!tok || !pid) {
        json_free(j);
        *err = xstrdup("Missing token or projectId in Google Cloud credentials. Use /login to re-authenticate.");
        return -1;
    }
    out->token = xstrdup(tok);
    out->project_id = xstrdup(pid);
    const char *e = json_get_str(j, "email"), *a = json_get_str(j, "accountId");
    if (e && *e) out->email = xstrdup(e);
    if (a && *a) out->account_id = xstrdup(a);
    json_t *exp = json_get(j, "expiresAt");
    if (!exp) exp = json_get(j, "expires");
    out->expires_at = normalize_expiry_ms(exp);
    json_free(j);
    return 0;
}

/* ======================================================================
 * provider session state (providerSessionState, keyed by the agent session id)
 * ====================================================================== */

static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;
static hmap_t g_states;
static bool g_states_init;

/* caller holds g_state_mu */
static cca_session_t *session_state(const char *sid) {
    if (!sid || !*sid) return NULL;
    if (!g_states_init) {
        hmap_init(&g_states);
        g_states_init = true;
    }
    cca_session_t *s = hmap_get(&g_states, sid);
    if (!s) {
        s = xcalloc(1, sizeof(*s));
        hmap_put(&g_states, sid, s);
    }
    return s;
}

static void set_str(char **slot, const char *v) {
    free(*slot);
    *slot = v ? xstrdup(v) : NULL;
}

/* ======================================================================
 * request
 * ====================================================================== */

static bool efforts_contain(const ai_model_t *m, ai_effort_t e) {
    json_t *arr = m->thinking ? json_get_arr(m->thinking, "efforts") : NULL, *it;
    json_arr_foreach(arr, i, it) {
        if (str_eq(json_sv(it), effort_name(e))) return true;
    }
    return false;
}

/* defaultSupportedEffort: the effort routed to the default wire id, else the lowest supported one */
static ai_effort_t default_effort(const ai_model_t *m) {
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    const char *def = json_get_str(m->raw, "requestModelId");
    if (routing && def)
        for (int e = EFFORT_MINIMAL; e <= EFFORT_MAX; e++)
            if (efforts_contain(m, (ai_effort_t)e) && str_eq(json_get_str(routing, effort_name((ai_effort_t)e)), def))
                return (ai_effort_t)e;
    for (int e = EFFORT_MINIMAL; e <= EFFORT_MAX; e++)
        if (efforts_contain(m, (ai_effort_t)e)) return (ai_effort_t)e;
    return EFFORT_OFF;
}

/* resolveWireModelId */
static const char *wire_model_id(const ai_model_t *m, ai_effort_t e) {
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    const char *r = routing ? json_get_str(routing, effort_name(e)) : NULL;
    if (r) return r;
    const char *rq = json_get_str(m->raw, "requestModelId");
    return rq ? rq : m->id;
}

typedef struct {
    bool enabled;
    const char *level; /* static */
    bool has_budget;
    int64_t budget;
    bool suppress;
    int64_t max_tokens; /* -1 unset */
    const char *wire;   /* borrowed from the model */
} cca_think_t;

static void resolve_thinking(const ai_model_t *m, const ai_stream_opts_t *o, cca_think_t *t) {
    memset(t, 0, sizeof(*t));
    int64_t base = o->max_tokens > 0 ? o->max_tokens : (m->max_tokens > 0 ? m->max_tokens : -1);
    ai_effort_t e = o->reasoning;
    bool off = e == EFFORT_OFF || o->disable_reasoning;
    bool requires = m->thinking && json_get_bool(m->thinking, "requiresEffort", false);
    bool suppress_off = m->thinking && json_get_bool(m->thinking, "suppressWhenOff", false);
    /* normalizeMandatoryReasoningOptions */
    if (m->reasoning && requires && !suppress_off && off) {
        ai_effort_t d = default_effort(m);
        if (d != EFFORT_OFF) {
            e = d;
            off = false;
        }
    }
    if (!off && m->reasoning) {
        e = google_clamp_effort(m, e);
        const char *mode = m->thinking ? json_get_str(m->thinking, "mode") : NULL;
        if (str_eq(mode, "google-level")) {
            t->max_tokens = google_max_output(base, m, google_thinking_headroom(m, o, e));
            t->wire = wire_model_id(m, e);
            t->enabled = true;
            t->level = google_thinking_level(e, m);
            return;
        }
        int64_t budget = google_effort_budget(m, o, e);
        int64_t maxt = base < 0 ? OUTPUT_CAP_WHEN_UNKNOWN : base + budget;
        if (m->max_tokens > 0) maxt = MIN(maxt, m->max_tokens);
        if (maxt <= budget) budget = MAX((int64_t)0, maxt - MIN_OUTPUT_TOKENS);
        if (budget > 0) {
            t->max_tokens = maxt;
            t->wire = wire_model_id(m, e);
            t->enabled = true;
            t->has_budget = true;
            t->budget = budget;
            return;
        }
    }
    t->max_tokens = base;
    t->wire = wire_model_id(m, EFFORT_OFF);
    if (m->reasoning && suppress_off) {
        /* CCA re-applies the per-id baked server default when thinkingConfig is omitted */
        t->suppress = true;
        const char *mode = m->thinking ? json_get_str(m->thinking, "mode") : NULL;
        if (str_eq(mode, "google-level")) t->level = "MINIMAL";
        else {
            t->has_budget = true;
            t->budget = 0;
        }
    }
}

typedef struct {
    const char *id;
    const char *model_enum;
    int64_t max_output;
} wire_profile_t;

/* offline fallback for rows without discovered profiles (gemini-headers.ts ANTIGRAVITY_MODEL_WIRE_PROFILES) */
static const wire_profile_t FALLBACK_PROFILES[] = {
    {"gemini-3.5-flash-extra-low", "MODEL_PLACEHOLDER_M187", 65536},
    {"gemini-3.5-flash-low", "MODEL_PLACEHOLDER_M20", 65536},
    {"gemini-3-flash-agent", "MODEL_PLACEHOLDER_M132", 65536},
    {"gemini-3.1-pro-low", "MODEL_PLACEHOLDER_M36", 65535},
    {"gemini-pro-agent", "MODEL_PLACEHOLDER_M16", 65535},
    {"claude-sonnet-4-6", NULL, 64000},
    {"claude-opus-4-6-thinking", NULL, 64000},
};

/* getAntigravityModelWireProfile (owned object or NULL) */
static json_t *wire_profile(const ai_model_t *m, const char *wire) {
    json_t *p = json_get_obj(json_get_obj(m->raw, "antigravityWireProfiles"), wire);
    if (p) return json_clone(p);
    for (size_t i = 0; i < ARRAY_LEN(FALLBACK_PROFILES); i++) {
        if (strcmp(FALLBACK_PROFILES[i].id, wire) != 0) continue;
        json_t *o = json_obj();
        if (FALLBACK_PROFILES[i].model_enum) json_set_str(o, "modelEnum", FALLBACK_PROFILES[i].model_enum);
        json_set_int(o, "maxOutputTokens", FALLBACK_PROFILES[i].max_output);
        return o;
    }
    return NULL;
}

/* isReviewedTierVariantWireId, approximated from the row's own effort routing: a family routing efforts to more
 * than one SKU makes each SKU a tier with fixed thinking strength. */
static bool tier_variant(const ai_model_t *m, const char *wire) {
    json_t *routing = m->thinking ? json_get_obj(m->thinking, "effortRouting") : NULL;
    strvec_t tiers = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(routing, i, k, v) {
        if (!strcmp(k, "off") || !json_is_str(v)) continue;
        if (!strvec_contains(&tiers, json_sv(v))) strvec_push(&tiers, json_sv(v));
    }
    bool r = tiers.n > 1 && strvec_contains(&tiers, wire);
    strvec_free(&tiers);
    return r;
}

static char *signed_decimal(uint64_t v) { return xasprintf("-%llu", (unsigned long long)(v & INT63_MASK)); }

static char *random_session_id(void) {
    for (;;) {
        unsigned char b[8];
        rand_bytes(b, sizeof b);
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | b[i];
        v &= INT63_MASK;
        if (v < ANTIGRAVITY_RANDOM_BOUND) return signed_decimal(v);
    }
}

static char *derive_session_id(const ai_context_t *ctx) {
    const char *text = NULL;
    json_t *msg;
    json_arr_foreach(ctx->messages, i, msg) {
        if (!str_eq(json_get_str(msg, "role"), "user")) continue;
        json_t *c = json_get(msg, "content");
        if (json_is_str(c)) text = json_sv(c);
        else if (json_is_arr(c)) {
            json_t *b;
            json_arr_foreach(c, j, b) {
                if (str_eq(json_get_str(b, "type"), "text")) {
                    text = json_get_str(b, "text");
                    break;
                }
            }
        }
        break;
    }
    if (str_blank(text)) return random_session_id();
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)text, strlen(text), d);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[i];
    return signed_decimal(v);
}

/* tool choice -> FunctionCallingConfig mode / single allowed name */
static const char *tool_choice_mode(const json_t *tc, const char **name) {
    *name = NULL;
    if (json_is_str(tc)) {
        const char *s = json_sv(tc);
        if (!strcmp(s, "required") || !strcmp(s, "any")) return "ANY";
        if (!strcmp(s, "none")) return "NONE";
        return NULL; /* auto */
    }
    if (json_is_obj(tc)) {
        const char *ty = json_get_str(tc, "type"), *n = NULL;
        if (str_eq(ty, "tool")) n = json_get_str(tc, "name");
        else if (str_eq(ty, "function")) {
            json_t *fn = json_get_obj(tc, "function");
            n = fn ? json_get_str(fn, "name") : json_get_str(tc, "name");
        }
        if (n && *n) {
            *name = n;
            return "ANY";
        }
    }
    return NULL;
}

static json_t *function_calling(const char *mode, const char *allowed) {
    json_t *fcc = json_obj();
    json_set_str(fcc, "mode", mode);
    if (allowed) {
        json_t *names = json_arr();
        json_push_str(names, allowed);
        json_set(fcc, "allowedFunctionNames", names);
    }
    json_t *tc = json_obj();
    json_set(tc, "functionCallingConfig", fcc);
    return tc;
}

/* normalizeAntigravityTools: declarations on `parametersJsonSchema` move to CCA-normalized `parameters` */
static void antigravity_tools(json_t *tools) {
    json_t *tool;
    json_arr_foreach(tools, i, tool) {
        json_t *decls = json_get_arr(tool, "functionDeclarations");
        for (size_t d = 0; d < json_len(decls); d++) {
            json_t *decl = json_at(decls, d);
            if (json_get(decl, "parameters")) continue;
            json_t *pjs = json_take(decl, "parametersJsonSchema");
            json_set(decl, "parameters", pjs ? google_normalize_schema_cca(pjs) : json_null());
            json_free(pjs);
        }
    }
}

json_t *gemini_cli_build_request(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o,
                                 const char *project_id, bool ag, cca_session_t *state) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    cca_think_t th;
    resolve_thinking(m, o, &th);
    bool anthropic = str_eq(identity_class(m), "anthropic");

    json_t *gen = json_obj();
    bool sampling = model_compat_bool(m, "supportsSamplingParams", true);
    if (sampling && !isnan(o->temperature)) json_set_num(gen, "temperature", o->temperature);
    if (th.max_tokens > 0) json_set_int(gen, "maxOutputTokens", th.max_tokens);
    if (sampling && !isnan(o->top_p)) json_set_num(gen, "topP", o->top_p);
    if (m->reasoning && (th.enabled || th.suppress)) {
        json_t *tc = json_obj();
        json_set_bool(tc, "includeThoughts", th.enabled && !o->hide_thinking_summary);
        if (th.level) json_set_str(tc, "thinkingLevel", th.level);
        else if (th.has_budget) json_set_int(tc, "thinkingBudget", th.budget);
        json_set(gen, "thinkingConfig", tc);
    }

    json_t *transformed = ai_transform_messages(ctx->messages, m);
    /* Claude behind Antigravity is Vertex Anthropic: 8000 px and a 5 MB base64 cap per image (compat rules) */
    if (anthropic && str_eq(m->provider, "google-antigravity")) ai_anthropic_prepare_images(transformed, m);
    json_t *contents = google_convert_messages(m, transformed);
    json_free(transformed);
    json_t *req = json_obj();
    json_set(req, "contents", contents);

    json_t *sys = json_arr(), *it;
    json_arr_foreach(ctx->system_prompt, i, it) {
        if (json_is_str(it) && !str_blank(json_sv(it))) {
            json_t *p = json_obj();
            json_set_str(p, "text", json_sv(it));
            json_push(sys, p);
        }
    }
    if (json_len(sys) > 0) {
        json_t *si = json_obj();
        if (ag) json_set_str(si, "role", "user");
        json_set(si, "parts", sys);
        json_set(req, "systemInstruction", si);
    } else {
        json_free(sys);
    }

    if (json_len(ctx->tools) > 0) {
        json_t *tools = google_convert_tools(m, ctx->tools);
        if (ag) antigravity_tools(tools);
        json_set(req, "tools", tools);
        const char *allowed = NULL;
        const char *mode = o->tool_choice ? tool_choice_mode(o->tool_choice, &allowed) : NULL;
        if (mode) {
            json_set(req, "toolConfig", function_calling(mode, allowed));
            /* Antigravity's Gemini routes drop toolConfig: restate a forced choice in the transcript */
            if (ag && !anthropic && !strcmp(mode, "ANY")) {
                size_t n = 0;
                const char *directive = embed_get("prompts/ai/providers/google-antigravity-forced-tool.md", &n);
                json_t *p = json_obj(), *parts = json_arr(), *c = json_obj();
                json_set(p, "text", json_strn(directive ? directive : "", directive ? n : 0));
                json_push(parts, p);
                json_set_str(c, "role", "user");
                json_set(c, "parts", parts);
                json_push(contents, c);
            }
        }
        if (ag && !json_get(req, "toolConfig")) json_set(req, "toolConfig", function_calling("VALIDATED", NULL));
    }
    if (ag && anthropic && cflag(m, "antigravityClaudeToolMode"))
        json_set(req, "toolConfig", function_calling("VALIDATED", NULL));

    const char *wire = th.wire ? th.wire : m->id;
    json_t *env = json_obj();
    if (ag) {
        json_t *profile = wire_profile(m, wire);
        json_t *pmax = json_get(profile, "maxOutputTokens");
        if (json_is_num(pmax)) json_set_int(gen, "maxOutputTokens", (int64_t)pmax->u.num.d);
        json_t *tcfg = json_get_obj(gen, "thinkingConfig");
        if (profile && tcfg && th.enabled && tier_variant(m, wire)) {
            /* a tier SKU fixes its own thinking strength: send it the way the SKU advertises */
            const char *lvl = json_get_str(profile, "thinkingLevel");
            json_t *pb = json_get(profile, "thinkingBudget");
            if (lvl || json_is_num(pb)) {
                json_t *nt = json_obj();
                json_set_bool(nt, "includeThoughts", json_get_bool(tcfg, "includeThoughts", false));
                if (lvl) json_set_str(nt, "thinkingLevel", lvl);
                else json_set_int(nt, "thinkingBudget", (int64_t)pb->u.num.d);
                json_set(gen, "thinkingConfig", nt);
            }
        }
        /* request envelope identity (mirrors the native client) */
        int64_t context_steps = MAX((int64_t)1, (int64_t)json_len(ctx->messages));
        int64_t request_index = 0;
        if (state) {
            if (!state->agent_id) state->agent_id = uuid_v4();
            if (!state->trajectory_id) state->trajectory_id = uuid_v4();
            if (!state->session_id) state->session_id = random_session_id();
            state->step_index = MAX(context_steps, state->step_index + 1);
            request_index = state->request_count++;
        }
        char *agent = state ? xstrdup(state->agent_id) : uuid_v4();
        char *traj = state ? xstrdup(state->trajectory_id) : uuid_v4();
        char *sid = state ? xstrdup(state->session_id) : derive_session_id(ctx);
        int64_t step = state ? state->step_index : context_steps;
        char *request_id = xasprintf("agent/%s/%lld/%s/%lld", agent, (long long)now_ms(), traj, (long long)step);
        json_t *labels = json_obj();
        json_set_str(labels, "cascade_id", agent);
        if (state && state->last_execution_id) json_set_str(labels, "last_execution_id", state->last_execution_id);
        char num[32];
        snprintf(num, sizeof num, "%lld", (long long)(step - 1));
        json_set_str(labels, "last_step_index", num);
        const char *me = json_get_str(profile, "modelEnum");
        if (me) json_set_str(labels, "model_enum", me);
        char *rid = xasprintf("%s-%lld", traj, (long long)request_index);
        json_set_str(labels, "request_id", rid);
        free(rid);
        json_set_str(labels, "root_cascade_id", agent);
        json_set_str(labels, "trajectory_id", traj);
        const char *usage = model_compat_str(m, "antigravityUsageLabel");
        if (!usage) usage = anthropic ? "true" : "false";
        json_set_str(labels, "used_claude", usage);
        json_set_str(labels, "used_claude_conservative", usage);
        json_set_str(labels, "used_non_gemini_model", str_eq(identity_class(m), "gemini") ? "false" : "true");
        json_set(req, "labels", labels);
        if (json_len(gen) > 0) json_set(req, "generationConfig", gen);
        else json_free(gen);
        json_set_str(req, "sessionId", sid);
        json_set_str(env, "project", project_id);
        json_set_str(env, "requestId", request_id);
        json_set(env, "request", req);
        json_set_str(env, "model", wire);
        json_set_str(env, "userAgent", "antigravity");
        free(agent);
        free(traj);
        free(sid);
        free(request_id);
        json_free(profile);
        return env;
    }
    if (json_len(gen) > 0) json_set(req, "generationConfig", gen);
    else json_free(gen);
    json_set_str(env, "project", project_id);
    json_set_str(env, "model", wire);
    json_set(env, "request", req);
    return env;
}

strvec_t gemini_cli_endpoints(const ai_model_t *m, bool ag, const cca_session_t *state) {
    strvec_t eps = {0};
    char *base = m->base_url ? str_trim(m->base_url) : xstrdup("");
    if (!ag) {
        strvec_push(&eps, *base ? base : CCA_GEMINI_CLI_ENDPOINT);
        free(base);
        return eps;
    }
    if (*base) {
        char *clean = xstrdup(base);
        size_t n = strlen(clean);
        while (n && clean[n - 1] == '/') clean[--n] = 0;
        bool known = !strcmp(clean, CCA_ANTIGRAVITY_DAILY_ENDPOINT) || !strcmp(clean, CCA_ANTIGRAVITY_SANDBOX_ENDPOINT);
        free(clean);
        if (!known) {
            strvec_push(&eps, base);
            free(base);
            return eps;
        }
    }
    free(base);
    const char *last = state ? state->last_good_endpoint : NULL;
    if (str_eq(last, CCA_ANTIGRAVITY_SANDBOX_ENDPOINT)) {
        strvec_push(&eps, CCA_ANTIGRAVITY_SANDBOX_ENDPOINT);
        strvec_push(&eps, CCA_ANTIGRAVITY_DAILY_ENDPOINT);
    } else {
        strvec_push(&eps, CCA_ANTIGRAVITY_DAILY_ENDPOINT);
        strvec_push(&eps, CCA_ANTIGRAVITY_SANDBOX_ENDPOINT);
    }
    return eps;
}

/* ======================================================================
 * SSE (CloudCodeAssistResponseChunk)
 * ====================================================================== */

static json_t *ccontent(cca_sse_t *s) { return json_get(s->b.msg, "content"); }
static json_t *cur_block(cca_sse_t *s) { return s->b.cur_index >= 0 ? json_at(ccontent(s), (size_t)s->b.cur_index) : NULL; }

void cca_sse_init(cca_sse_t *s, const ai_model_t *m, const json_t *tools, ai_event_cb cb, void *ud) {
    memset(s, 0, sizeof(*s));
    aib_init(&s->b, m, cb, ud);
    s->model = m;
    s->stop_reason = "stop";
    s->flash_leak = cflag(m, "flashStreamLeakWorkaround");
    json_t *t;
    json_arr_foreach(tools, i, t) {
        const char *n = json_get_str(t, "name");
        if (n) strvec_push(&s->tool_names, n);
    }
    think_heal_t *h = xmalloc(sizeof(*h));
    think_heal_init(h);
    s->heal = h;
}

void cca_sse_free(cca_sse_t *s) {
    free(s->error);
    free(s->response_id);
    free(s->tbuf_sig);
    sb_free(&s->tbuf);
    strvec_free(&s->tool_names);
    if (s->heal) {
        think_heal_free(s->heal);
        free(s->heal);
    }
    aib_free(&s->b);
    memset(s, 0, sizeof(*s));
}

static void retain_sig(cca_sse_t *s, const char *sig) {
    json_t *blk = cur_block(s);
    if (!blk || !sig || !*sig) return;
    const char *ty = json_get_str(blk, "type");
    if (str_eq(ty, "thinking")) json_set_str(blk, "thinkingSignature", sig);
    else if (str_eq(ty, "text")) json_set_str(blk, "textSignature", sig);
}

static void thinking_delta(cca_sse_t *s, const char *t, size_t n) {
    aib_thinking_delta(&s->b, t, n);
    json_t *blk = cur_block(s);
    if (blk && !json_get(blk, "summary")) json_set_bool(blk, "summary", true); /* Gemini thought summaries */
}

typedef struct {
    cca_sse_t *s;
    const char *sig;
} heal_ud_t;

static void heal_emit(void *ud, int kind, const char *t, size_t n) {
    heal_ud_t *h = ud;
    if (kind == THINK_HEAL_THINKING) {
        thinking_delta(h->s, t, n);
    } else {
        aib_text_delta(&h->s->b, t, n);
        retain_sig(h->s, h->sig);
    }
}

static void feed_visible(cca_sse_t *s, const char *t, size_t n, const char *sig) {
    heal_ud_t h = {s, sig};
    think_heal_feed(s->heal, t, n, heal_emit, &h);
}

static void flush_visible(cca_sse_t *s, const char *sig) {
    heal_ud_t h = {s, sig};
    think_heal_flush(s->heal, heal_emit, &h);
}

/* ---- planning-leak buffering (flash models leaking `{"thought": ...}` JSON into text) ---- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    return p;
}

static bool planning_leak_prefix(const char *text) {
    const char *t = skip_ws(text);
    if (*t != '{') return false;
    size_t tl = strlen(t);
    const char *ab = skip_ws(t + 1);
    if (!*ab) return tl <= 100;
    if (*ab != '"') return false;
    const char *q = strchr(ab + 1, '"');
    if (!q) return str_starts("thought", ab + 1) && tl <= 100;
    if ((size_t)(q - ab - 1) != 7 || strncmp(ab + 1, "thought", 7) != 0) return false;
    const char *ak = skip_ws(q + 1);
    if (!*ak) return tl <= 100;
    return *ak == ':';
}

/* brace-balanced leading object; quote-aware unless ignore_quotes. Returns end offset (exclusive) or 0. */
static size_t leading_object_end(const char *text, bool ignore_quotes, size_t *start_out) {
    const char *t = skip_ws(text);
    *start_out = (size_t)(t - text);
    if (*t != '{') return 0;
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; t[i]; i++) {
        char c = t[i];
        if (!ignore_quotes) {
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
                continue;
            }
            if (c == '"') {
                in_str = true;
                continue;
            }
        }
        if (c == '{') depth++;
        else if (c == '}') {
            if (--depth == 0) return *start_out + i + 1;
        }
    }
    return 0;
}

static bool has_quoted_key(const char *s, const char *key) {
    char *q = xasprintf("\"%s\"", key);
    bool r = strstr(s, q) != NULL;
    free(q);
    return r;
}

static bool leak_signature_text(cca_sse_t *s, const char *t) {
    if (has_quoted_key(t, "thought")) return true;
    for (size_t i = 0; i < s->tool_names.n; i++)
        if (has_quoted_key(t, s->tool_names.v[i])) return true;
    return has_quoted_key(t, "_i") || has_quoted_key(t, "paths") || has_quoted_key(t, "command") ||
           (has_quoted_key(t, "path") && has_quoted_key(t, "content"));
}

static bool leak_object(cca_sse_t *s, const json_t *o) {
    if (!json_is_obj(o)) return false;
    if (json_is_str(json_get(o, "thought"))) return true;
    const char *call = json_get_str(o, "call");
    if (call && strvec_contains(&s->tool_names, call)) return true;
    return json_get(o, "_i") || json_get(o, "paths") || json_get(o, "command") ||
           (json_get(o, "path") && json_get(o, "content"));
}

enum { PB_INCOMPLETE, PB_PLAIN, PB_LEAK };

/* consumePlanningBuffer: *visible receives the text to show (owned) unless incomplete */
static int consume_planning(cca_sse_t *s, const char *text, bool final, char **visible) {
    *visible = NULL;
    if (!planning_leak_prefix(text)) {
        *visible = xstrdup(text);
        return PB_PLAIN;
    }
    size_t start, end = leading_object_end(text, false, &start);
    if (!end) end = leading_object_end(text, true, &start);
    if (!end) {
        if (!final) return PB_INCOMPLETE;
        char *tr = str_trim(text);
        bool leak = leak_signature_text(s, tr);
        free(tr);
        *visible = xstrdup(leak ? "" : text);
        return leak ? PB_LEAK : PB_PLAIN;
    }
    char *json_text = xstrndup(text + start, end - start);
    json_t *parsed = json_parse_str(json_text);
    bool leak;
    if (!parsed) {
        leak = leak_signature_text(s, json_text);
    } else {
        leak = leak_object(s, parsed);
        json_free(parsed);
    }
    free(json_text);
    *visible = xstrdup(leak ? text + end : text);
    return leak ? PB_LEAK : PB_PLAIN;
}

static void flush_planning_buffer(cca_sse_t *s, bool final) {
    char *vis = NULL;
    int r = consume_planning(s, sb_cstr(&s->tbuf), final, &vis);
    if (r == PB_INCOMPLETE) return;
    if (r == PB_LEAK) s->stripped_leak = true;
    char *sig = s->tbuf_sig;
    s->tbuf_sig = NULL;
    s->buffering = false;
    sb_clear(&s->tbuf);
    feed_visible(s, vis, strlen(vis), sig);
    free(vis);
    free(sig);
}

static void push_tool_call(cca_sse_t *s, const json_t *part) {
    json_t *fc = json_get_obj(part, "functionCall");
    const char *name = json_get_str(fc, "name");
    const char *id = json_get_str(fc, "id");
    bool need_new = !id || !*id;
    if (!need_new) {
        json_t *b;
        json_arr_foreach(ccontent(s), i, b) {
            if (str_eq(json_get_str(b, "type"), "toolCall") && str_eq(json_get_str(b, "id"), id)) need_new = true;
        }
    }
    char *gen = need_new ? google_next_tool_call_id(name) : NULL;
    aib_toolcall_start(&s->b, gen ? gen : id, name ? name : "");
    free(gen);
    json_t *args = json_get_obj(fc, "args");
    char *aj = args ? json_dump(args, 0) : xstrdup("{}");
    aib_toolcall_delta(&s->b, aj, strlen(aj));
    free(aj);
    const char *sig = json_get_str(part, "thoughtSignature");
    if (sig && *sig) aib_toolcall_set_field(&s->b, "thoughtSignature", sig);
    aib_end_block(&s->b);
}

static bool has_tool_call(cca_sse_t *s) {
    json_t *b;
    json_arr_foreach(ccontent(s), i, b) {
        if (str_eq(json_get_str(b, "type"), "toolCall")) return true;
    }
    return false;
}

int cca_sse_event(void *ud, const char *event, const char *data, size_t len) {
    cca_sse_t *s = ud;
    json_t *chunk = json_parse(data, len, NULL);
    if (!json_is_obj(chunk)) {
        json_free(chunk);
        return 0;
    }
    json_t *err = json_get_obj(chunk, "error");
    if (err) {
        const char *msg = json_get_str(err, "message"), *status = json_get_str(err, "status");
        char *detail = msg && status ? xasprintf("%s (%s)", msg, status) : xstrdup(msg ? msg : status ? status : "unknown error");
        json_t *details = json_get_arr(err, "details");
        char *residue = NULL;
        if (json_len(details) > 0) {
            json_t *r = json_obj(), *e = json_obj();
            if (status) json_set_str(e, "status", status);
            json_set(e, "details", json_clone(details));
            json_set(r, "error", e);
            char *rj = json_dump(r, 0);
            json_free(r);
            residue = xasprintf(" %s", rj);
            free(rj);
        }
        free(s->error);
        s->error = xasprintf("Cloud Code Assist stream error: %s%s", detail, residue ? residue : "");
        free(detail);
        free(residue);
        json_free(chunk);
        return 2;
    }
    json_t *resp = json_get_obj(chunk, "response");
    if (!resp) {
        json_free(chunk);
        return 0;
    }
    const char *rid = json_get_str(resp, "responseId");
    if (rid) set_str(&s->response_id, rid);
    json_t *cands = json_get_arr(resp, "candidates");
    json_t *pf = json_get_obj(resp, "promptFeedback");
    if (json_len(cands) == 0 && pf && json_get_str(pf, "blockReason")) {
        const char *detail = json_get_str(pf, "blockReasonMessage");
        free(s->error);
        s->error = xasprintf("Request blocked by Google (%s)%s%s", json_get_str(pf, "blockReason"), detail ? ": " : "",
                             detail ? detail : "");
        json_free(chunk);
        return 2;
    }
    json_t *cand = json_at(cands, 0);
    json_t *part;
    json_arr_foreach(json_get_arr(json_get_obj(cand, "content"), "parts"), i, part) {
        json_t *tv = json_get(part, "text");
        const char *sig = json_get_str(part, "thoughtSignature");
        bool has_fc = json_get_obj(part, "functionCall") != NULL;
        if (json_is_str(tv) && tv->u.str.len > 0) {
            const char *text = tv->u.str.s;
            size_t tl = tv->u.str.len;
            if (json_get_bool(part, "thought", false)) {
                flush_visible(s, NULL);
                thinking_delta(s, text, tl);
                retain_sig(s, sig);
            } else {
                if (s->buffering) {
                    sb_appendn(&s->tbuf, text, tl);
                    if (sig && *sig) set_str(&s->tbuf_sig, sig);
                } else if (s->flash_leak && *skip_ws(text) == '{') {
                    s->buffering = true;
                    sb_clear(&s->tbuf);
                    sb_appendn(&s->tbuf, text, tl);
                    set_str(&s->tbuf_sig, sig && *sig ? sig : NULL);
                } else {
                    feed_visible(s, text, tl, sig);
                }
                if (s->buffering) flush_planning_buffer(s, false);
            }
        } else if (json_is_str(tv) && sig && *sig && !has_fc) {
            retain_sig(s, sig);
        }
        if (has_fc) {
            flush_visible(s, NULL);
            aib_end_block(&s->b);
            s->buffering = false;
            sb_clear(&s->tbuf);
            push_tool_call(s, part);
        }
    }
    const char *fr = json_get_str(cand, "finishReason");
    if (fr) {
        s->saw_finish = true;
        const char *mapped = google_map_finish_reason(fr);
        if ((!strcmp(mapped, "stop") || !strcmp(mapped, "length")) && has_tool_call(s)) {
            s->stop_reason = "toolUse";
        } else {
            s->stop_reason = mapped;
            if (!strcmp(mapped, "error")) {
                free(s->error);
                s->error = xasprintf("Generation failed with finish reason: %s", fr);
            }
        }
    }
    json_t *um = json_get_obj(resp, "usageMetadata");
    if (um) google_apply_usage(&s->b, um);
    json_free(chunk);
    return 0;
}

void cca_sse_end(cca_sse_t *s) {
    if (s->buffering && s->tbuf.len > 0) flush_planning_buffer(s, true);
    char *sig = s->tbuf_sig;
    s->tbuf_sig = NULL;
    s->buffering = false;
    sb_clear(&s->tbuf);
    flush_visible(s, sig);
    free(sig);
    aib_end_block(&s->b);
}

bool cca_sse_meaningful(cca_sse_t *s) {
    json_t *b;
    json_arr_foreach(ccontent(s), i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "toolCall")) return true;
        if (str_eq(t, "text") && !str_blank(json_get_str(b, "text"))) return true;
    }
    return false;
}

bool cca_sse_thought_only(cca_sse_t *s) {
    json_t *b;
    json_arr_foreach(ccontent(s), i, b) {
        if (!str_eq(json_get_str(b, "type"), "thinking")) continue;
        const char *sig = json_get_str(b, "thinkingSignature");
        if (!str_blank(json_get_str(b, "thinking")) || (sig && *sig)) return true;
    }
    return false;
}

/* ======================================================================
 * entry point
 * ====================================================================== */

static bool transient_status(int s) { return s == 408 || s == 429 || s == 500 || s == 502 || s == 503 || s == 504; }

static json_t *fail(const ai_model_t *m, ai_event_cb cb, void *ud, const char *msg) {
    ai_builder_t b;
    aib_init(&b, m, cb, ud);
    json_t *r = aib_finish(&b, "error", msg);
    aib_free(&b);
    return r;
}

static bool wait_cancellable(const ai_stream_opts_t *o, int64_t ms) {
    int64_t until = mono_ms() + ms;
    while (mono_ms() < until) {
        if (o->cancel && *o->cancel) return false;
        sleep_ms(20);
    }
    return true;
}

/* fresh message for a retry; "start" was already emitted when started */
static void reset_sse(cca_sse_t *s, const ai_model_t *m, const ai_context_t *ctx, ai_event_cb cb, void *ud) {
    bool started = s->b.started;
    cca_sse_free(s);
    cca_sse_init(s, m, ctx->tools, cb, ud);
    s->b.started = started;
}

json_t *provider_gemini_cli_stream(const ai_model_t *m, const ai_context_t *ctx, const ai_stream_opts_t *o, ai_event_cb cb,
                                   void *ud) {
    ai_stream_opts_t defo;
    if (!o) {
        ai_stream_opts_init(&defo);
        o = &defo;
    }
    bool ag = str_eq(m->provider, "google-antigravity");
    if (!o->api_key || !*o->api_key)
        return fail(m, cb, ud, "Google Cloud Code Assist requires OAuth authentication. Use /login to authenticate.");
    cca_creds_t creds;
    char *perr = NULL;
    if (cca_parse_credentials(o->api_key, &creds, &perr) != 0) {
        json_t *r = fail(m, cb, ud, perr);
        free(perr);
        return r;
    }
    if (creds.expires_at && now_ms() >= creds.expires_at) {
        cca_creds_free(&creds);
        return fail(m, cb, ud, "OAuth token expired before request — please retry; AuthStorage will refresh on the next attempt.");
    }

    pthread_mutex_lock(&g_state_mu);
    cca_session_t *state = ag ? session_state(o->session_id) : NULL;
    strvec_t endpoints = gemini_cli_endpoints(m, ag, state);
    if (state && endpoints.n == 1) set_str(&state->last_good_endpoint, NULL);
    json_t *params = gemini_cli_build_request(m, ctx, o, creds.project_id, ag, state);
    pthread_mutex_unlock(&g_state_mu);
    if (o->on_payload) o->on_payload(o->payload_ud, params);
    char *body = json_dump(params, 0);
    json_free(params);
    /* the backend gates newer models on the client version */
    if (ag) cca_antigravity_ensure_version();

    http_headers_t h = {0};
    char *bearer = xasprintf("Bearer %s", creds.token);
    http_headers_set(&h, "Authorization", bearer);
    free(bearer);
    http_headers_set(&h, "Content-Type", "application/json");
    if (ag) {
        cca_antigravity_headers(&h);
        http_headers_set(&h, "Accept", "*/*"); /* the native Go client sends no specific Accept */
    } else {
        http_headers_set(&h, "Accept", "text/event-stream");
        cca_gemini_cli_headers(&h, m->id);
    }
    if (cflag(m, "claudeThinkingBetaHeader") && str_eq(identity_class(m), "anthropic") && m->reasoning)
        http_headers_set(&h, "anthropic-beta", CLAUDE_THINKING_BETA_HEADER);
    ai_apply_headers(&h, NULL, o);

    json_t *fet = model_compat_get(m, "streamFirstEventTimeoutMs");
    int64_t first_event_ms = json_is_num(fet) && fet->u.num.d > 0 ? (int64_t)fet->u.num.d : DEFAULT_FIRST_EVENT_TIMEOUT_MS;
    /* streamFirstEventTimeoutMs request option (providers.streamFirstEventTimeoutSeconds); 0 disables */
    if (o->first_event_timeout_ms > 0) first_event_ms = o->first_event_timeout_ms;
    else if (o->first_event_timeout_ms == 0) first_event_ms = 24LL * 3600 * 1000;

    cca_sse_t s;
    cca_sse_init(&s, m, ctx->tools, cb, ud);
    json_t *result = NULL;
    for (size_t ei = 0; ei < endpoints.n && !result; ei++) {
        const char *endpoint = endpoints.v[ei];
        bool last = ei + 1 == endpoints.n;
        char *url = xasprintf("%s/v1internal:streamGenerateContent?alt=sse", endpoint);
        bool failover = false, received = false;
        char *error = NULL;
        for (int attempt = 0; attempt <= MAX_EMPTY_STREAM_RETRIES; attempt++) {
            if (attempt > 0 && !wait_cancellable(o, (int64_t)EMPTY_STREAM_BASE_DELAY_MS << (attempt - 1))) {
                result = aib_finish(&s.b, "aborted", "Request was aborted");
                break;
            }
            ai_stream_opts_t ro = *o;
            ro.max_retries = attempt == 0 && last ? MAX_RETRIES : 0;
            ro.retry_policy = AI_RETRY_GEMINI_CLI;
            ro.max_retry_delay_ms = o->max_retry_delay_ms ? o->max_retry_delay_ms : RATE_LIMIT_BUDGET_MS;
            ai_post_ex_t ex = {.idle_timeout_ms = first_event_ms};
            int status = 0;
            char *err = NULL;
            int rc = ai_sse_post_ex(url, &h, body, &ro, cca_sse_event, &s, &status, &err, &ex);
            if (rc == -2) {
                free(err);
                free(ex.err_body);
                result = aib_finish(&s.b, "aborted", "Request was aborted");
                break;
            }
            if (rc == -1) {
                if (status >= 400) {
                    if (transient_status(status) && !last && attempt == 0) {
                        failover = true;
                    } else {
                        char *vu = cca_extract_validation_url(ex.err_body);
                        char *detail = vu ? cca_validation_message(vu, "retry your request", creds.email)
                                          : xstrdup(ex.err_body ? ex.err_body : err ? err : "");
                        char *text = xasprintf("Cloud Code Assist API error (%d): %s", status, detail);
                        error = ai_rewrap_http_error(text, err);
                        free(text);
                        free(detail);
                        free(vu);
                    }
                } else if (!last && !s.b.started) {
                    failover = true;
                } else {
                    error = xstrdup(err ? err : "request failed");
                }
                free(err);
                free(ex.err_body);
                break;
            }
            free(err);
            free(ex.err_body);
            if (rc == 2) {
                if (!last && !s.b.started) failover = true;
                else error = xstrdup(s.error ? s.error : "Cloud Code Assist stream error");
                break;
            }
            cca_sse_end(&s);
            bool meaningful = cca_sse_meaningful(&s);
            if (strcmp(s.stop_reason, "stop") != 0 || meaningful) {
                received = meaningful;
                break;
            }
            /* a thought-only STOP is a complete response: replaying burns another reasoning pass */
            if (cca_sse_thought_only(&s)) break;
            if (attempt < MAX_EMPTY_STREAM_RETRIES) reset_sse(&s, m, ctx, cb, ud);
        }
        free(url);
        if (result) break;
        if (!failover && !error) {
            if (!strcmp(s.stop_reason, "error") || !strcmp(s.stop_reason, "aborted")) {
                error = xstrdup(s.error ? s.error : "An unknown error occurred");
            } else if (!received) {
                error = xstrdup(cca_sse_thought_only(&s) ? "Cloud Code Assist API returned a thought-only response without final output"
                                                          : "Cloud Code Assist API returned an empty response");
                if (!last && !s.b.started) failover = true;
            } else if (!s.saw_finish) {
                error = xstrdup("Cloud Code Assist stream ended without a finish reason (connection dropped or response truncated)");
                if (!last && !s.b.started) failover = true;
            }
        }
        if (failover) {
            free(error);
            reset_sse(&s, m, ctx, cb, ud);
            continue;
        }
        if (error) {
            result = aib_finish(&s.b, "error", error);
            free(error);
            break;
        }
        pthread_mutex_lock(&g_state_mu);
        if (state) {
            set_str(&state->last_good_endpoint, endpoint);
            set_str(&state->last_execution_id, s.response_id);
        }
        pthread_mutex_unlock(&g_state_mu);
        result = aib_finish(&s.b, s.stop_reason, NULL);
    }
    if (!result) result = aib_finish(&s.b, "error", "Cloud Code Assist request failed on every endpoint");
    cca_sse_free(&s);
    strvec_free(&endpoints);
    free(body);
    http_headers_free(&h);
    cca_creds_free(&creds);
    return result;
}
