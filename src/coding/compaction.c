/* compaction.c - context maintenance (see compaction.h). */
#include "ai/anthropic_threads.h"
#include "coding/compaction.h"

#include "ai/dialect.h"
#include "ai/provider_anthropic.h"
#include "coding/advisor.h"
#include "coding/branchsum.h"
#include "coding/dcr.h"
#include "config/compaction_point.h"
#include "ai/ai_images.h"
#include "config/settings.h"
#include "core/fs.h"
#include "prompt/template.h"
#include "tools/artifacts.h"

#include <ctype.h>
#include <math.h>

#define DEFAULT_RESERVE_TOKENS 16384
#define MAX_SUMMARY_TOKENS 16384
#define RECOVERY_BAND 0.8
#define DEFAULT_SUMMARY_INPUT_WINDOW 200000
#define MIN_SUMMARY_INPUT_TOKENS 16384
#define PLACEHOLDER_TOKEN_ESTIMATE 16
#define MIN_PRUNE_TOKENS 50
#define PRUNE_CACHE_WARM_SUFFIX_TOKENS 8000
#define SUPERSEDE_SUFFIX_TOKEN_LIMIT 8000
#define PRUNE_IDLE_FLUSH_MS (90LL * 60000)

static const char *const DEFAULT_METHODS[] = {"remote", "snapcompact", "handoff", "shake", "soft"};
#define SUPERSEDED_NOTICE "[Superseded by a newer read of this file]"
#define USELESS_NOTICE "[Uneventful result elided]"
#define REMOTE_SUMMARY "Remote compaction preserved provider-native history for this session."

/* ---------- settings ---------- */

static bool is_method(const char *m) {
    for (size_t i = 0; i < ARRAY_LEN(DEFAULT_METHODS); i++)
        if (str_eq(m, DEFAULT_METHODS[i])) return true;
    return false;
}

strvec_t cmp_method_order(void) {
    strvec_t v = {0};
    json_t *o = settings_get("compaction.methodOrder"), *x;
    if (!o) {
        for (size_t i = 0; i < ARRAY_LEN(DEFAULT_METHODS); i++) strvec_push(&v, DEFAULT_METHODS[i]);
        return v;
    }
    json_arr_foreach(json_is_arr(o) ? o : NULL, i, x) if (is_method(json_sv(x)) && !strvec_contains(&v, json_sv(x)))
        strvec_push(&v, json_sv(x));
    return v;
}

/* compact-modes.ts overrides */
static strvec_t mode_methods(const char *mode) {
    strvec_t v = {0};
    if (str_eq(mode, "soft")) strvec_push(&v, "soft");
    else if (str_eq(mode, "remote")) {
        strvec_push(&v, "remote");
        strvec_push(&v, "soft");
    } else if (str_eq(mode, "snapcompact")) strvec_push(&v, "snapcompact");
    else return cmp_method_order();
    return v;
}

static int64_t reserve_tokens(void) { return settings_int("compaction.reserveTokens", DEFAULT_RESERVE_TOKENS); }

/* resolveBudgetReserveTokens */
static int64_t budget_reserve(int64_t window) {
    int64_t prop = MAX(1, (int64_t)floor((double)window * 0.15));
    int64_t r = MAX((int64_t)floor((double)window * 0.15), reserve_tokens());
    return r >= window - prop || r >= window ? prop : r;
}

static int64_t threshold_for(const ai_model_t *m) {
    return m && m->context_window > 0 ? cp_model_threshold(m, m->context_window) : 0;
}

/* ---------- token estimates ---------- */

int64_t cmp_message_tokens(const json_t *msg) {
    if (!msg) return 0;
    const json_t *c = json_get(msg, "content");
    if (json_is_str(c)) return ((int64_t)json_len(c) + 3) / 4;
    size_t chars = 0;
    int64_t images = 0;
    const json_t *b;
    json_arr_foreach(c, i, b) {
        const char *t = json_get_str(b, "type");
        if (str_eq(t, "text") && json_get_str(b, "text")) chars += strlen(json_get_str(b, "text"));
        else if (str_eq(t, "thinking") && json_get_str(b, "thinking")) chars += strlen(json_get_str(b, "thinking"));
        else if (str_eq(t, "toolCall")) {
            char *args = json_dump(json_get(b, "arguments"), 0);
            chars += strlen(args) + (json_get_str(b, "name") ? strlen(json_get_str(b, "name")) : 0);
            free(args);
        } else if (str_eq(t, "image")) images++;
    }
    return ((int64_t)chars + 3) / 4 + images * 1200;
}

static int64_t text_tokens(const char *s) { return s ? ((int64_t)strlen(s) + 3) / 4 : 0; }

static int64_t messages_tokens(const json_t *msgs) {
    int64_t n = 0;
    const json_t *m;
    json_arr_foreach(msgs, i, m) n += cmp_message_tokens(m);
    return n;
}

/* computeNonMessageTokens: system prompt + tool catalog */
static int64_t nonmessage_tokens(app_t *a) {
    pthread_mutex_lock(&a->agent->mu);
    char *sys = json_dump(a->agent->system_prompt, 0);
    int64_t n = text_tokens(sys);
    free(sys);
    for (size_t i = 0; i < a->agent->tools.n; i++) {
        json_t *w = agent_tool_wire(a->agent->tools.v[i]);
        char *d = json_dump(w, 0);
        n += text_tokens(d);
        free(d);
        json_free(w);
    }
    pthread_mutex_unlock(&a->agent->mu);
    return n;
}

/* #estimateStoredContextTokens */
static int64_t stored_context_tokens(app_t *a) {
    pthread_mutex_lock(&a->agent->mu);
    int64_t n = messages_tokens(a->agent->messages);
    pthread_mutex_unlock(&a->agent->mu);
    return n + nonmessage_tokens(a);
}

/* calculateContextTokens / calculatePromptTokens */
static int64_t usage_context_tokens(const json_t *u) {
    if (!u) return 0;
    if (json_get(u, "contextTokens")) return MAX(0, json_get_int(u, "contextTokens", 0));
    int64_t total = json_get_int(u, "totalTokens", 0);
    if (!total)
        total = json_get_int(u, "input", 0) + json_get_int(u, "output", 0) + json_get_int(u, "cacheRead", 0) +
                json_get_int(u, "cacheWrite", 0);
    return MAX(0, total);
}

static int64_t usage_prompt_tokens(const json_t *u) {
    if (json_get(u, "contextTokens")) return MAX(0, json_get_int(u, "contextTokens", 0));
    int64_t p = json_get_int(u, "input", 0) + json_get_int(u, "cacheRead", 0) + json_get_int(u, "cacheWrite", 0);
    return p > 0 ? p : usage_context_tokens(u);
}

/* ---------- entries -> messages ---------- */

static json_t *user_text(const char *text, const char *attribution) {
    json_t *m = msg_user_text(text);
    if (attribution) json_set_str(m, "attribution", attribution);
    return m;
}

/* getMessageFromEntry + convertToLlm (side-request form) */
static json_t *entry_llm(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "message")) {
        const json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "custom") || str_eq(role, "hookMessage")) {
            json_t *d = json_obj();
            json_set_str(d, "role", "developer");
            const json_t *c = json_get(m, "content");
            if (json_is_str(c)) {
                json_t *arr = json_arr();
                json_push(arr, text_block(json_sv(c)));
                json_set(d, "content", arr);
            } else json_set(d, "content", c ? json_clone(c) : json_arr());
            return d;
        }
        return session_message_to_llm(m);
    }
    if (str_eq(t, "custom_message")) {
        json_t *d = json_obj();
        json_set_str(d, "role", "developer");
        const json_t *c = json_get(e, "content");
        if (json_is_str(c)) {
            json_t *arr = json_arr();
            json_push(arr, text_block(json_sv(c)));
            json_set(d, "content", arr);
        } else json_set(d, "content", c ? json_clone(c) : json_arr());
        return d;
    }
    if (str_eq(t, "branch_summary")) {
        json_t *ctx = json_obj();
        json_set_str(ctx, "summary", json_get_str(e, "summary") ? json_get_str(e, "summary") : "");
        char *txt = prompt_render("agent/compaction/prompts/branch-summary-context.md", ctx);
        json_free(ctx);
        json_t *m = user_text(txt ? txt : "", "agent");
        free(txt);
        return m;
    }
    return NULL;
}

static bool has_message(const json_t *e) {
    const char *t = json_get_str(e, "type");
    return str_eq(t, "message") || str_eq(t, "custom_message") || str_eq(t, "branch_summary");
}

static const char *entry_role(const json_t *e) { return json_get_str(json_get(e, "message"), "role"); }

/* isTurnStartEntry */
static bool turn_start(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "branch_summary") || str_eq(t, "custom_message")) return true;
    return str_eq(t, "message") && (str_eq(entry_role(e), "user") || str_eq(entry_role(e), "bashExecution"));
}

/* findValidCutPoints membership */
static bool cut_point(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "branch_summary") || str_eq(t, "custom_message")) return true;
    if (!str_eq(t, "message")) return false;
    static const char *const R[] = {"bashExecution", "hookMessage", "branchSummary", "compactionSummary", "user", "assistant"};
    for (size_t i = 0; i < ARRAY_LEN(R); i++)
        if (str_eq(entry_role(e), R[i])) return true;
    return false;
}

/* ---------- provider-native support ---------- */

static bool responses_api(const char *api) {
    return str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses");
}

/* shouldUseOpenAiRemoteCompaction for the V1 endpoint omc implements (no V2 streaming, no Bedrock OpenAI routes) */
static bool openai_v1(const ai_model_t *m) {
    const json_t *rc = json_get_obj(m->raw, "remoteCompaction");
    if (rc && json_get(rc, "enabled") && !json_get_bool(rc, "enabled", true)) return false;
    const char *api = json_get_str(rc, "api") ? json_get_str(rc, "api") : m->api;
    if (str_eq(m->provider, "openai-codex")) return json_get_str(rc, "endpoint") && *json_get_str(rc, "endpoint");
    if (str_eq(m->provider, "openai")) return str_eq(api, "openai-responses");
    if (!json_get_bool(rc, "enabled", false)) return false;
    return str_eq(api, "openai-responses") || str_eq(api, "openai-codex-responses");
}

/* shouldUseAnthropicNativeCompaction: the Anthropic Messages compaction beta (Bedrock Converse compaction through
 * InvokeModel is not implemented) */
static bool anthropic_native(const ai_model_t *m) { return str_eq(m->api, "anthropic-messages") && anthropic_compaction_supported(m); }

bool cmp_remote_supported(const ai_model_t *m) {
    if (!m || !settings_bool("compaction.remoteEnabled", true)) return false;
    return openai_v1(m) || anthropic_native(m);
}

/* compaction.remoteEndpoint (requestRemoteCompaction's generic summarizer endpoint) is not implemented: the
 * setting does not make `remote` usable */
static bool remote_endpoint_set(void) { return false; }

/* canUseRemoteCompaction */
static bool remote_usable(const ai_model_t *m) { return remote_endpoint_set() || cmp_remote_supported(m); }

/* canUseLiveProviderNativeCompaction */
static bool live_native(const ai_model_t *cand, const ai_model_t *live) {
    if (!str_eq(cand->provider, live->provider)) return false;
    bool same = str_eq(cand->api, "anthropic-messages") || str_eq(cand->api, "bedrock-converse-stream");
    if (same && (!str_eq(cand->api, live->api) || !str_eq(cand->id, live->id))) return false;
    return cmp_remote_supported(cand);
}

/* remotePreserveReusable */
static bool preserve_reusable(const json_t *compaction, const ai_model_t *m) {
    const json_t *oa = json_get_obj(json_get_obj(compaction, "preserveData"), "openaiRemoteCompaction");
    if (!oa || !json_is_arr(json_get(oa, "replacementHistory"))) return true;
    return m && str_eq(json_get_str(oa, "provider"), m->provider) && responses_api(m->api) && cmp_remote_supported(m);
}

/* ---------- preparation (compaction.ts prepareCompaction) ---------- */

typedef struct {
    json_t *path;     /* owned clones root..leaf */
    char *first_kept; /* owned */
    json_t *summarize, *prefix, *recent, *recent_ids;
    char *last_summarized;
    bool split;
    int64_t tokens_before;
    const json_t *prev; /* previous readable compaction (borrowed from path) */
    json_t *ops;
} prep_t;

static void prep_free(prep_t *p) {
    json_free(p->path);
    free(p->first_kept);
    json_free(p->summarize);
    json_free(p->prefix);
    json_free(p->recent);
    json_free(p->recent_ids);
    free(p->last_summarized);
    json_free(p->ops);
    memset(p, 0, sizeof *p);
}

static json_t *new_ops(void) {
    json_t *o = json_obj();
    json_set(o, "read", json_arr());
    json_set(o, "written", json_arr());
    json_set(o, "edited", json_arr());
    return o;
}

static void ops_add(json_t *ops, const char *key, const char *v) {
    json_t *arr = json_get(ops, key), *x;
    json_arr_foreach(arr, i, x) if (str_eq(json_sv(x), v)) return;
    json_push_str(arr, v);
}

/* 0 ok, 1 already compacted, 2 nothing to compact */
static int prepare(app_t *a, const ai_model_t *model, prep_t *p) {
    memset(p, 0, sizeof *p);
    if (!a->session || !a->session->leaf_id) return 2;
    p->path = session_path_to(a->session, a->session->leaf_id);
    size_t n = json_len(p->path);
    if (!n) return 2;
    const json_t *last = json_at(p->path, n - 1);
    /* a trailing boundary this preparation re-expands (one the active model cannot reuse) covers nothing yet */
    if (str_eq(json_get_str(last, "type"), "compaction") && !json_get_str(last, "providerReplayThroughEntryId") &&
        (!model || preserve_reusable(last, model)))
        return 1;
    ssize_t prev = -1, reset = -1;
    for (size_t i = n; i-- > 0 && prev < 0;) {
        const json_t *e = json_at(p->path, i);
        if (str_eq(json_get_str(e, "type"), "compaction") && preserve_reusable(e, model)) prev = (ssize_t)i;
    }
    for (size_t i = n; i-- > 0 && reset < 0;)
        if (str_eq(json_get_str(json_at(p->path, i), "type"), "reset_boundary")) reset = (ssize_t)i;
    if (reset > prev) prev = -1;
    p->prev = prev >= 0 ? json_at(p->path, (size_t)prev) : NULL;
    size_t start = (size_t)(MAX(prev, reset) + 1);
    if (p->prev) {
        const char *through = json_get_str(p->prev, "providerReplayThroughEntryId");
        const char *kept = json_get_str(p->prev, "firstKeptEntryId");
        if (json_get_obj(json_get_obj(p->prev, "preserveData"), "openaiRemoteCompaction")) {
            for (size_t i = 0; through && i < (size_t)prev; i++)
                if (str_eq(json_get_str(json_at(p->path, i), "id"), through)) start = (size_t)(MAX((ssize_t)i, reset) + 1);
        } else {
            for (size_t i = (size_t)(reset + 1); kept && i < (size_t)prev; i++)
                if (str_eq(json_get_str(json_at(p->path, i), "id"), kept)) {
                    start = i;
                    break;
                }
            if (kept && !*kept && through)
                for (size_t i = 0; i < (size_t)prev; i++)
                    if (str_eq(json_get_str(json_at(p->path, i), "id"), through)) start = i + 1;
        }
    }
    /* entries that contribute a message, with their LLM form (NULL when dropped) */
    VEC(const json_t *) ents = {0};
    json_t *msgs = json_arr(); /* parallel; json null for dropped messages */
    for (size_t i = start; i < n; i++) {
        const json_t *e = json_at(p->path, i);
        if (!has_message(e)) continue;
        vec_push(&ents, e);
        json_t *m = entry_llm(e);
        json_push(msgs, m ? m : json_null());
    }
    /* tokensBefore: the last surviving assistant usage after the previous boundary */
    const json_t *usage = NULL;
    for (size_t i = n; i-- > (size_t)(MAX(prev, reset) + 1) && !usage;) {
        const json_t *e = json_at(p->path, i);
        const json_t *m = json_get(e, "message");
        if (!str_eq(json_get_str(e, "type"), "message") || !str_eq(json_get_str(m, "role"), "assistant")) continue;
        const char *st = json_get_str(m, "stopReason");
        if (str_eq(st, "aborted") || str_eq(st, "error") || !json_get_obj(m, "usage")) continue;
        usage = json_get_obj(m, "usage");
    }
    p->tokens_before = usage ? usage_context_tokens(usage) : 0;
    int64_t keep = settings_int("compaction.keepRecentTokens", 20000);
    if (model && model->context_window > 0) {
        int64_t w = model->context_window;
        int64_t band = (int64_t)floor((double)threshold_for(model) * RECOVERY_BAND);
        int64_t floor_ = MIN(budget_reserve(w), reserve_tokens());
        int64_t tail = band > floor_ ? band - floor_ : band / 2;
        keep = MIN(keep, MAX(1, tail));
    }
    size_t ne = ents.n;
    int64_t *tok = xcalloc(ne ? ne : 1, sizeof *tok);
    int64_t est = 0;
    for (size_t i = 0; i < ne; i++) est += tok[i] = cmp_message_tokens(json_at(msgs, i));
    if (usage && est > 0) {
        double ratio = (double)usage_prompt_tokens(usage) / (double)est;
        if (isfinite(ratio) && ratio > 1) keep = MAX(1, (int64_t)floor((double)keep / ratio));
    }
    /* findCutPoint */
    VEC(size_t) cps = {0};
    for (size_t i = 0; i < ne; i++)
        if (cut_point(ents.v[i])) vec_push(&cps, i);
    size_t cut = 0;
    if (cps.n) {
        ssize_t ci = (ssize_t)cps.n - 1;
        cut = cps.v[ci];
        int64_t acc = 0;
        for (size_t i = ne; i-- > 0;) {
            acc += tok[i];
            if (ci < 0 || i != cps.v[ci]) continue;
            if (acc > keep) break;
            cut = i;
            ci--;
        }
    }
    ssize_t turn = -1;
    bool is_start = ne && turn_start(ents.v[cut]);
    if (ne && !is_start)
        for (size_t i = cut + 1; i-- > 0 && turn < 0;)
            if (turn_start(ents.v[i])) turn = (ssize_t)i;
    p->split = !is_start && turn >= 0;
    int rc = 0;
    if (!ne || !json_get_str(ents.v[cut], "id")) {
        rc = 2;
        goto out;
    }
    p->first_kept = xstrdup(json_get_str(ents.v[cut], "id"));
    size_t hist_end = p->split ? (size_t)turn : cut;
    p->summarize = json_arr();
    p->prefix = json_arr();
    p->recent = json_arr();
    p->recent_ids = json_arr();
    for (size_t i = 0; i < ne; i++) {
        json_t *m = json_at(msgs, i);
        if (i >= cut) json_push_str(p->recent_ids, json_get_str(ents.v[i], "id"));
        if (json_is_null(m)) continue;
        json_push(i < hist_end ? p->summarize : i < cut ? p->prefix : p->recent, json_clone(m));
    }
    if (hist_end == 0 && !(p->split && (size_t)turn < cut)) {
        rc = 2;
        goto out;
    }
    if (cut > 0) p->last_summarized = xstrdup(json_get_str(ents.v[cut - 1], "id"));
    p->ops = new_ops();
    if (p->prev && !json_get_bool(p->prev, "fromExtension", false)) {
        const json_t *d = json_get(p->prev, "details"), *x;
        json_arr_foreach(json_get_arr(d, "readFiles"), i, x) if (json_is_str(x)) {
            char *s = branchsum_strip_read_selector(json_sv(x));
            ops_add(p->ops, "read", s);
            free(s);
        }
        json_arr_foreach(json_get_arr(d, "modifiedFiles"), i, x) if (json_is_str(x)) ops_add(p->ops, "edited", json_sv(x));
    }
    const json_t *m;
    json_arr_foreach(p->summarize, i, m) branchsum_extract_ops(m, p->ops);
    json_arr_foreach(p->prefix, i, m) branchsum_extract_ops(m, p->ops);
out:
    vec_free(&cps);
    free(tok);
    vec_free(&ents);
    json_free(msgs);
    return rc;
}

/* ---------- requests ---------- */

/* resolveCompactionEffort: off -> omitted, unset/inherit -> high, else the session level, clamped */
static void compaction_effort(app_t *a, const ai_model_t *m, ai_stream_opts_t *o) {
    if (!a->auto_thinking && a->thinking == EFFORT_OFF) {
        o->reasoning = EFFORT_OFF;
        return;
    }
    o->reasoning = model_clamp_effort(m, a->auto_thinking ? EFFORT_HIGH : a->thinking);
}

static char *joined_text(const json_t *msg) {
    sb_t b = SB_INIT;
    bool first = true;
    const json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
        if (!str_eq(json_get_str(blk, "type"), "text")) continue;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        first = false;
    }
    return sb_detach(&b);
}

/* the cancel flag of the running pass: the live turn's, or a background speculation's own */
static __thread volatile sig_atomic_t *tl_cancel;
static volatile sig_atomic_t *cancel_ptr(app_t *a) { return tl_cancel ? tl_cancel : &a->agent->cancel; }
static bool cancelled(app_t *a) { return *cancel_ptr(a) != 0; }

/* one summarization oneshot: owned text, NULL + *err (owned) on failure; *status the HTTP status if known */
static char *oneshot_ex(app_t *a, const ai_model_t *m, const char *system, const char *user, int64_t max_tokens,
                        const char *what, char **err, int *status, bool disable_reasoning) {
    json_t *sys = json_arr();
    json_push_str(sys, system ? system : "");
    json_t *msgs = json_arr();
    json_push(msgs, msg_user_text(user));
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.max_tokens = max_tokens;
    if (disable_reasoning) o.disable_reasoning = true;
    else compaction_effort(a, m, &o);
    o.session_id = a->agent->session_id;
    o.provider_session_state = true;
    o.metadata = (json_t *)app_session_metadata(a, m->provider);
    o.cancel = cancel_ptr(a);
    json_t *r = ai_complete(m, &ctx, &o);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (status) *status = (int)json_get_int(r, "errorStatus", 0);
    if (str_eq(stop, "aborted") || cancelled(a)) *err = xstrdup("Compaction cancelled");
    else if (str_eq(stop, "error") || !r) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xasprintf("%s: %s", what, em && *em ? em : "Unknown error");
    } else text = joined_text(r);
    json_free(r);
    json_free(sys);
    json_free(msgs);
    free(key);
    return text;
}

static char *oneshot(app_t *a, const ai_model_t *m, const char *system, const char *user, int64_t max_tokens,
                     const char *what, char **err, int *status) {
    return oneshot_ex(a, m, system, user, max_tokens, what, err, status, false);
}

static char *load_prompt(const char *name) {
    char *rel = xasprintf("agent/compaction/prompts/%s.md", name);
    char *p = prompt_render(rel, NULL);
    free(rel);
    return p ? p : xstrdup("");
}

static int64_t min_summary_input(const ai_model_t *m) {
    int64_t w = m->context_window > 0 ? m->context_window : DEFAULT_SUMMARY_INPUT_WINDOW;
    return MIN(MIN_SUMMARY_INPUT_TOKENS, MAX(1024, w / 8));
}

static char *clamp_to_budget(const char *text, int64_t budget, int64_t tokens) {
    if (tokens <= budget) return xstrdup(text);
    size_t len = strlen(text);
    size_t keep = MAX((size_t)1024, (size_t)floor((double)len * (double)budget * 0.95 / (double)tokens));
    if (keep >= len) return xstrdup(text);
    while (keep > 0 && ((unsigned char)text[keep] & 0xc0) == 0x80) keep--;
    return xasprintf("%.*s\n\n[... %zu more characters truncated]", (int)keep, text, len - keep);
}

/* summarizeConversationWindow */
static char *summarize_window(app_t *a, const ai_model_t *m, const char *conv, const char *prev, int64_t max_tokens,
                              const char *instructions, const char *override, char **err) {
    char *base = override ? xstrdup(override) : load_prompt(prev ? "compaction-update-summary" : "compaction-summary");
    if (instructions && *instructions) {
        char *b2 = xasprintf("%s\n\nAdditional focus: %s", base, instructions);
        free(base);
        base = b2;
    }
    sb_t p = SB_INIT;
    sb_appendf(&p, "<conversation>\n%s\n</conversation>\n\n", conv);
    if (prev) {
        char *esc = dialect_escape_summary_tags(prev);
        sb_appendf(&p, "<previous-summary>\n%s\n</previous-summary>\n\n", esc);
        free(esc);
    }
    sb_append(&p, base);
    free(base);
    char *sys = load_prompt("summarization-system");
    char *out = oneshot(a, m, sys, sb_cstr(&p), max_tokens, "Summarization failed", err, NULL);
    free(sys);
    sb_free(&p);
    return out;
}

/* generateSummary: windows planned against the summarizer's input budget, the summary folded through them */
static char *generate_summary(app_t *a, const ai_model_t *m, const json_t *msgs, const char *instructions,
                              const char *prev, char **err) {
    int64_t max_tokens = MIN((int64_t)floor(0.8 * (double)reserve_tokens()), MAX_SUMMARY_TOKENS);
    const char *dialect = dialect_for_model(m);
    bool drop = dialect_drops_thinking(m);
    int64_t w = m->context_window > 0 ? m->context_window : DEFAULT_SUMMARY_INPUT_WINDOW;
    int64_t budget = MAX(min_summary_input(m), (int64_t)floor((double)w * 0.8) - max_tokens - MAX_SUMMARY_TOKENS);
    char *whole = dialect_serialize_for_summary(msgs, dialect, drop);
    char *carried = prev ? xstrdup(prev) : NULL;
    if (text_tokens(whole) <= budget) {
        char *s = summarize_window(a, m, whole, carried, max_tokens, instructions, NULL, err);
        free(whole);
        free(carried);
        return s;
    }
    free(whole);
    /* planSummaryWindows */
    size_t n = json_len(msgs), i = 0;
    while (i < n) {
        json_t *win = json_arr();
        int64_t cur = 0;
        for (; i < n; i++) {
            json_t *one = json_arr();
            json_push(one, json_clone(json_at(msgs, i)));
            char *t = dialect_serialize_for_summary(one, dialect, drop);
            int64_t k = text_tokens(t);
            free(t);
            json_free(one);
            if (cur > 0 && cur + k > budget) break;
            json_push(win, json_clone(json_at(msgs, i)));
            cur += k;
        }
        char *text = dialect_serialize_for_summary(win, dialect, drop);
        char *clamped = clamp_to_budget(text, budget, text_tokens(text));
        char *s = summarize_window(a, m, clamped, carried, max_tokens, instructions, NULL, err);
        free(text), free(clamped);
        json_free(win);
        if (!s) {
            free(carried);
            return NULL;
        }
        free(carried);
        carried = s;
    }
    return carried ? carried : xstrdup("");
}

/* generateTurnPrefixSummary */
static char *turn_prefix_summary(app_t *a, const ai_model_t *m, const json_t *msgs, char **err) {
    int64_t max_tokens = MIN((int64_t)floor(0.5 * (double)reserve_tokens()), MAX_SUMMARY_TOKENS);
    char *conv = dialect_serialize_for_summary(msgs, dialect_for_model(m), dialect_drops_thinking(m));
    char *tp = load_prompt("compaction-turn-prefix");
    char *user = xasprintf("<conversation>\n%s\n</conversation>\n\n%s", conv, tp);
    char *sys = load_prompt("summarization-system");
    char *out = oneshot(a, m, sys, user, max_tokens, "Turn prefix summarization failed", err, NULL);
    free(conv), free(tp), free(user), free(sys);
    return out;
}

/* generateShortSummary: NULL on any failure (display only) */
static char *short_summary(app_t *a, const ai_model_t *m, const json_t *recent, const char *history) {
    int64_t max_tokens = MIN(512, (int64_t)floor(0.2 * (double)reserve_tokens()));
    char *conv = dialect_serialize_for_summary(recent, dialect_for_model(m), dialect_drops_thinking(m));
    sb_t p = SB_INIT;
    sb_appendf(&p, "<conversation>\n%s\n</conversation>\n\n", conv);
    free(conv);
    if (history) {
        char *esc = dialect_escape_summary_tags(history);
        sb_appendf(&p, "<previous-summary>\n%s\n</previous-summary>\n\n", esc);
        free(esc);
    }
    char *sp = load_prompt("compaction-short-summary");
    sb_append(&p, sp);
    free(sp);
    char *sys = load_prompt("summarization-system");
    char *err = NULL;
    /* display-only summary under a 512-token cap: reasoning would consume the whole budget (the full summary keeps
     * the session's effort) */
    char *out = oneshot_ex(a, m, sys, sb_cstr(&p), max_tokens, "Short summary failed", &err, NULL, true);
    free(sys);
    sb_free(&p);
    free(err);
    if (out) {
        char *t = str_trim(out);
        bool empty = !*t;
        free(t);
        if (empty) {
            free(out);
            out = NULL;
        }
    }
    return out;
}

static void spec_cancel(app_t *a);

/* ---------- results ---------- */

typedef struct {
    char *summary, *short_summary, *first_kept, *replay_through;
    int64_t tokens_before;
    json_t *details, *preserve;
    bool native_error; /* NativeCompactionError: a provider-native request failed */
    int status;        /* HTTP status of the failure, when known */
} result_t;

static void result_free(result_t *r) {
    free(r->summary);
    free(r->short_summary);
    free(r->first_kept);
    free(r->replay_through);
    json_free(r->details);
    json_free(r->preserve);
    memset(r, 0, sizeof *r);
}

/* the compaction summary message as the live context renders it */
static json_t *summary_message(const char *summary, const char *method) {
    json_t *ctx = json_obj();
    json_set_str(ctx, "summary", summary ? summary : "");
    char *t = prompt_render(str_eq(method, "handoff") ? "agent/compaction/prompts/handoff-summary-context.md"
                                                      : "agent/compaction/prompts/compaction-summary-context.md",
                            ctx);
    json_free(ctx);
    json_t *m = user_text(t ? t : "", "agent");
    free(t);
    return m;
}

/* the live system prompt joined (remoteSystemPrompt) */
static char *live_system_prompt(app_t *a) {
    pthread_mutex_lock(&a->agent->mu);
    strvec_t parts = {0};
    const json_t *x;
    json_arr_foreach(a->agent->system_prompt, i, x) if (json_is_str(x)) strvec_push(&parts, json_sv(x));
    pthread_mutex_unlock(&a->agent->mu);
    char *s = strvec_join(&parts, "\n\n");
    strvec_free(&parts);
    return s;
}

/* findAnthropicCompactionCut over LLM messages: the keep-tail boundary moved forward until the summary/tail
 * boundary alternates wire roles and no tool call is separated from its result */
static size_t anthropic_cut(const json_t *msgs, size_t initial) {
    size_t n = json_len(msgs);
    ssize_t *last_result = xcalloc(n ? n : 1, sizeof *last_result);
    for (size_t i = 0; i < n; i++) last_result[i] = -1;
    for (size_t i = 0; i < n; i++) {
        const json_t *m = json_at(msgs, i);
        if (!str_eq(json_get_str(m, "role"), "toolResult")) continue;
        const char *id = json_get_str(m, "toolCallId");
        for (size_t k = 0; k < i && id; k++) {
            const json_t *am = json_at(msgs, k), *b;
            if (!str_eq(json_get_str(am, "role"), "assistant")) continue;
            json_arr_foreach(json_get_arr(am, "content"), x, b) if (str_eq(json_get_str(b, "type"), "toolCall") &&
                                                                    str_eq(json_get_str(b, "id"), id)) last_result[k] = (ssize_t)i;
        }
    }
    ssize_t prot = -1;
    for (size_t i = 0; i < initial && i < n; i++) prot = MAX(prot, last_result[i]);
    size_t out = n;
    for (size_t cut = initial; cut < n; cut++) {
        if (cut > 0) prot = MAX(prot, last_result[cut - 1]);
        if ((ssize_t)cut <= prot) continue;
        const char *r = json_get_str(json_at(msgs, cut), "role");
        if (str_eq(r, "system") || str_eq(r, "developer") || cut == 0) continue;
        bool pa = str_eq(json_get_str(json_at(msgs, cut - 1), "role"), "assistant");
        if (pa != str_eq(r, "assistant")) {
            out = cut;
            break;
        }
    }
    free(last_result);
    return out;
}

/* the Anthropic compaction beta request over the summarized prefix (requestAnthropicNativeCompaction) */
static bool anthropic_compact(app_t *a, const ai_model_t *m, const prep_t *p, const char *instructions, result_t *r,
                              char **err) {
    json_t *all = json_arr();
    const json_t *x;
    json_arr_foreach(p->summarize, i, x) json_push(all, json_clone(x));
    json_arr_foreach(p->prefix, i, x) json_push(all, json_clone(x));
    size_t orig = json_len(all);
    json_arr_foreach(p->recent, i, x) json_push(all, json_clone(x));
    size_t cut = anthropic_cut(all, orig);
    size_t recent_off = cut - orig; /* recent messages also summarized (cut past the original boundary) */
    const char *first_kept = cut == json_len(all) ? "" : cut == orig ? p->first_kept : NULL;
    if (!first_kept) {
        /* entry id of the moved boundary (recent messages and recent ids are parallel only when no recent entry
         * was dropped by convertToLlm; otherwise summarize everything) */
        first_kept = json_len(p->recent_ids) == json_len(p->recent) && recent_off < json_len(p->recent_ids)
                         ? json_sv(json_at(p->recent_ids, recent_off))
                         : "";
        if (!*first_kept) cut = json_len(all);
    }
    for (size_t i = orig; i < cut; i++) branchsum_extract_ops(json_at(all, i), p->ops);
    json_t *msgs = json_arr();
    const char *prev = p->prev ? json_get_str(p->prev, "summary") : NULL;
    if (prev) {
        json_t *sm = summary_message(prev, json_get_str(p->prev, "method"));
        bool oh = false;
        json_t *payload = session_compaction_payload(p->prev, &oh);
        if (payload && !oh && str_eq(json_get_str(payload, "provider"), m->provider)) json_set(sm, "providerPayload", payload);
        else json_free(payload);
        json_push(msgs, sm);
    }
    for (size_t i = 0; i < cut; i++) json_push(msgs, json_clone(json_at(all, i)));
    size_t prefix_len = json_len(msgs);
    for (size_t i = cut; i < json_len(all); i++) json_push(msgs, json_clone(json_at(all, i)));
    json_free(all);
    /* the live system prompt and tools, so the summarized prefix matches what the turns sent */
    pthread_mutex_lock(&a->agent->mu);
    json_t *sys = json_clone(a->agent->system_prompt);
    json_t *tools = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) json_push(tools, agent_tool_wire(a->agent->tools.v[i]));
    pthread_mutex_unlock(&a->agent->mu);
    /* transform the whole history as the live turn does, then cut it. Date/cwd reminder controls are the only messages
     * the transform inserts: count past them, and leave out any at the cut, so the kept turns follow the summarized
     * range directly */
    json_t *tx = a->agent->hooks.transform_context ? a->agent->hooks.transform_context(a->agent->hooks.ud, a->agent, msgs) : NULL;
    const json_t *live = tx ? tx : msgs;
    json_t *sent = json_arr();
    size_t counted = 0;
    for (size_t i = 0; i < json_len(live) && counted < prefix_len; i++) {
        const json_t *lm = json_at(live, i);
        if (!dcr_is_control(lm)) counted++;
        json_push(sent, json_clone(lm));
    }
    json_t *wire = ai_transform_messages(sent, m);
    json_free(sent);
    json_free(tx);
    json_free(msgs);
    /* instructions (anthropic-compaction-instructions.md) */
    json_t *ctx = json_obj();
    char *basep = load_prompt("compaction-summary");
    json_set_str(ctx, "basePrompt", basep);
    free(basep);
    if (instructions && *instructions) json_set_str(ctx, "customInstructions", instructions);
    char *instr = prompt_render("agent/compaction/prompts/anthropic-compaction-instructions.md", ctx);
    json_free(ctx);
    json_t *ac = json_obj();
    json_set_str(ac, "instructions", instr ? instr : "");
    free(instr);
    ai_context_t c = {.system_prompt = sys, .messages = wire, .tools = json_len(tools) ? tools : NULL};
    ai_stream_opts_t o = a->agent->base_opts;
    char *key = app_api_key_for(m);
    o.api_key = key;
    o.max_tokens = MIN((int64_t)floor(0.8 * (double)reserve_tokens()), MAX_SUMMARY_TOKENS);
    compaction_effort(a, m, &o);
    o.disable_reasoning = false;
    o.session_id = a->agent->session_id;
    o.provider_session_state = true;
    o.metadata = (json_t *)app_session_metadata(a, m->provider);
    o.cancel = cancel_ptr(a);
    o.on_payload = NULL;
    o.cursor_exec = NULL;
    o.tool_choice = NULL;
    o.anthropic_compaction = ac;
    json_t *resp = ai_complete(m, &c, &o);
    json_free(ac);
    json_free(sys);
    json_free(tools);
    json_free(wire);
    free(key);
    const char *stop = json_get_str(resp, "stopReason");
    const json_t *payload = json_get_obj(resp, "providerPayload");
    bool ok = false;
    r->status = (int)json_get_int(resp, "errorStatus", 0);
    if (str_eq(stop, "aborted") || cancelled(a)) *err = xstrdup("Compaction cancelled");
    else if (str_eq(stop, "error")) {
        const char *em = json_get_str(resp, "errorMessage");
        *err = xasprintf("Anthropic compaction failed: %s", em ? em : "unknown error");
    } else if (!str_eq(json_get_str(json_get_obj(resp, "stopDetails"), "type"), "compaction") ||
               !str_eq(json_get_str(payload, "type"), "anthropicCompaction") || !json_get_str(payload, "content") ||
               !json_get_str(payload, "signature")) {
        const char *sd = json_get_str(json_get_obj(resp, "stopDetails"), "type");
        *err = str_eq(sd, "compaction") ? xstrdup("Anthropic compaction returned no signed summary")
                                        : xasprintf("Anthropic compaction response carried no compaction block (stop reason: %s)",
                                                    sd ? sd : stop ? stop : "unknown");
    } else {
        ok = true;
        free(r->summary);
        r->summary = xstrdup(json_get_str(payload, "content"));
        free(r->first_kept);
        r->first_kept = xstrdup(first_kept);
        json_t *keep = json_obj();
        json_set_str(keep, "provider", m->provider);
        json_set_str(keep, "content", json_get_str(payload, "content"));
        json_set_str(keep, "signature", json_get_str(payload, "signature"));
        if (json_is_arr(json_get(payload, "toolChanges"))) json_set(keep, "toolChanges", json_clone(json_get(payload, "toolChanges")));
        json_set_bool(keep, "exactTail", true);
        json_set_str(keep, "model", json_get_str(resp, "model") ? json_get_str(resp, "model") : m->id);
        json_set_int(keep, "usedTokens", usage_prompt_tokens(json_get_obj(resp, "usage")));
        if (!r->preserve) r->preserve = json_obj();
        json_set(r->preserve, "anthropicCompaction", keep);
    }
    json_free(resp);
    return ok;
}

/* OpenAI Responses /responses/compact (requestOpenAiRemoteCompaction) over the whole history */
static bool openai_compact(app_t *a, const ai_model_t *m, const prep_t *p, result_t *r, char **err) {
    json_t *msgs = json_arr();
    const json_t *x;
    const json_t *prev_hist = NULL;
    if (p->prev) {
        const json_t *oa = json_get_obj(json_get_obj(p->prev, "preserveData"), "openaiRemoteCompaction");
        if (oa && str_eq(json_get_str(oa, "provider"), m->provider)) prev_hist = json_get_arr(oa, "replacementHistory");
        else if (json_get_str(p->prev, "summary") && settings_bool("compaction.remoteEnabled", true)) {
            /* a local summary has no native payload: encode it as history */
            json_push(msgs, summary_message(json_get_str(p->prev, "summary"), json_get_str(p->prev, "method")));
        }
    }
    json_arr_foreach(p->summarize, i, x) json_push(msgs, json_clone(x));
    json_arr_foreach(p->prefix, i, x) json_push(msgs, json_clone(x));
    json_arr_foreach(p->recent, i, x) json_push(msgs, json_clone(x));
    char *instr = live_system_prompt(a);
    if (!*instr) {
        free(instr);
        instr = load_prompt("summarization-system");
    }
    char *key = app_api_key_for(m);
    json_t *out = ai_openai_remote_compact(m, key, msgs, prev_hist, instr, a->agent->session_id, cancel_ptr(a), err,
                                           &r->status);
    free(key);
    free(instr);
    json_free(msgs);
    if (!out) return false;
    json_t *keep = json_obj();
    json_set_str(keep, "provider", m->provider);
    json_set(keep, "replacementHistory", json_take(out, "replacementHistory"));
    json_set(keep, "compactionItem", json_take(out, "compactionItem"));
    json_free(out);
    if (!r->preserve) r->preserve = json_obj();
    json_set(r->preserve, "openaiRemoteCompaction", keep);
    free(r->summary);
    r->summary = xstrdup(REMOTE_SUMMARY);
    return true;
}

/* compact(): the summary for `prep` on `m` (remote when the method is remote and the model supports it) */
static bool compact_with(app_t *a, const ai_model_t *m, prep_t *p, const char *instructions, bool remote, result_t *r,
                         char **err) {
    memset(r, 0, sizeof *r);
    r->first_kept = xstrdup(p->first_kept);
    r->tokens_before = p->tokens_before;
    bool used_remote = false, native = false;
    const char *prev = p->prev ? json_get_str(p->prev, "summary") : NULL;
    if (remote && settings_bool("compaction.remoteEnabled", true)) {
        if (openai_v1(m)) {
            if (openai_compact(a, m, p, r, err)) used_remote = true;
            else if (cancelled(a)) return false;
            else native = true;
        }
        if (!used_remote && anthropic_native(m)) {
            free(*err);
            *err = NULL;
            if (anthropic_compact(a, m, p, instructions, r, err)) {
                used_remote = true;
                native = false;
            } else if (cancelled(a)) return false;
            else native = true;
        }
        if (!used_remote && native) {
            r->native_error = true;
            return false;
        }
        free(*err);
        *err = NULL;
    }
    char *summary = NULL;
    bool anthropic = r->preserve && json_get(r->preserve, "anthropicCompaction");
    if (anthropic) summary = xstrdup(r->summary);
    else if (used_remote) summary = xstrdup(REMOTE_SUMMARY);
    else if (p->split && json_len(p->prefix)) {
        char *hist = json_len(p->summarize) || prev ? generate_summary(a, m, p->summarize, instructions, prev, err)
                                                    : xstrdup("No prior history.");
        if (!hist) return false;
        char *tp = turn_prefix_summary(a, m, p->prefix, err);
        if (!tp) {
            free(hist);
            return false;
        }
        summary = xasprintf("%s\n\n---\n\n**Turn Context (split turn):**\n\n%s", hist, tp);
        free(hist), free(tp);
    } else if (json_len(p->summarize)) {
        summary = generate_summary(a, m, p->summarize, instructions, prev, err);
        if (!summary) return false;
    } else summary = xstrdup(prev ? prev : "No prior history.");
    free(r->short_summary);
    r->short_summary = used_remote ? xstrdup("Remote compaction") : short_summary(a, m, p->recent, summary);
    if (cancelled(a)) {
        free(summary);
        *err = xstrdup("Compaction cancelled");
        return false;
    }
    json_t *rf = NULL, *mf = NULL;
    char *full = branchsum_upsert_files(summary, p->ops, &rf, &mf);
    if (anthropic) {
        /* the replayed block stays byte-identical; the file lists travel beside it */
        json_t *rf2 = NULL, *mf2 = NULL;
        char *files = branchsum_upsert_files("", p->ops, &rf2, &mf2);
        if (*files) json_set_str(json_get(r->preserve, "anthropicCompaction"), "filesText", files);
        free(files);
        json_free(rf2), json_free(mf2);
    }
    free(summary);
    free(r->summary);
    r->summary = full;
    r->details = json_obj();
    json_set(r->details, "readFiles", rf);
    json_set(r->details, "modifiedFiles", mf);
    return true;
}

static bool auth_failure(const result_t *r, const char *err) {
    return r->status == 401 || r->status == 403 || (err && (strstr(err, "credentials") || strstr(err, "No API key")));
}

/* #compactWithFallbackModel / the auto candidate loop: every candidate with credentials in order */
static bool compact_candidates(app_t *a, prep_t *p, const char *instructions, bool remote, bool any_error_falls_through,
                               result_t *r, char **err) {
    mr_pool_t cands = {0};
    app_compaction_candidates(a, &cands);
    bool ok = false, tried = false;
    char *native_err = NULL;
    const char *native_provider = NULL;
    for (size_t i = 0; i < cands.n && !ok; i++) {
        const ai_model_t *c = cands.v[i];
        if (remote && !remote_endpoint_set() && a->model && !live_native(c, a->model)) continue;
        char *key = app_api_key_for(c);
        bool keyed = key && *key;
        free(key);
        if (!keyed && !catalog_provider_keyless(c->provider)) continue;
        if (native_err && (!str_eq(c->provider, native_provider) || !cmp_remote_supported(c))) break;
        tried = true;
        free(*err);
        *err = NULL;
        result_t tmp;
        if (compact_with(a, c, p, instructions, remote, &tmp, err)) {
            *r = tmp;
            ok = true;
            break;
        }
        if (cancelled(a)) {
            result_free(&tmp);
            break;
        }
        if (tmp.native_error) {
            if (!native_err) {
                native_err = *err ? xstrdup(*err) : xstrdup("Remote compaction failed");
                native_provider = c->provider;
            }
            result_free(&tmp);
            continue;
        }
        bool auth = auth_failure(&tmp, *err);
        result_free(&tmp);
        if (auth || any_error_falls_through) continue;
        break;
    }
    if (!ok && native_err) {
        free(*err);
        *err = native_err;
        native_err = NULL;
    }
    free(native_err);
    if (!ok && !*err) {
        const ai_model_t *m = a->model;
        *err = !tried && m ? xasprintf("Compaction requires usable credentials for %s/%s. Configure %s credentials or assign an "
                                       "authenticated fallback role such as modelRoles.smol.",
                                       m->provider, m->id, m->provider)
                           : xstrdup("Compaction failed: no available model");
    }
    mr_pool_free(&cands);
    return ok;
}

/* #projectCompactionContextTokens: the context a pending compaction entry would rebuild on the current branch */
static int64_t project_after(app_t *a, const prep_t *p, const result_t *r, const char *method) {
    UNUSED(p);
    json_t *path = a->session && a->session->leaf_id ? session_path_to(a->session, a->session->leaf_id) : json_arr();
    json_t *e = json_obj();
    json_set_str(e, "type", "compaction");
    json_set_str(e, "id", "projection");
    json_set_str(e, "summary", r->summary ? r->summary : "");
    json_set_str(e, "firstKeptEntryId", r->first_kept ? r->first_kept : "");
    if (method) json_set_str(e, "method", method);
    if (r->replay_through) json_set_str(e, "providerReplayThroughEntryId", r->replay_through);
    if (r->preserve) json_set(e, "preserveData", json_clone(r->preserve));
    json_push(path, e);
    session_context_t sc;
    session_build_context_for(path, &sc);
    int64_t n = 0;
    const json_t *m;
    json_arr_foreach(sc.messages, i, m) {
        const json_t *pl = json_get_obj(m, "providerPayload");
        if (str_eq(json_get_str(pl, "type"), "openaiResponsesHistory")) {
            char *d = json_dump(json_get(pl, "items"), 0);
            n += text_tokens(d);
            free(d);
        } else n += cmp_message_tokens(m);
    }
    session_context_free(&sc);
    json_free(path);
    return n + nonmessage_tokens(a);
}

static int64_t branch_tokens(app_t *a) {
    session_context_t sc; /* as the active model reads it */
    session_build_context_as(a->session, a->model ? a->model->provider : NULL, a->model ? a->model->api : NULL, &sc);
    int64_t n = messages_tokens(sc.messages);
    session_context_free(&sc);
    return n + nonmessage_tokens(a);
}

/* #commitCompactionEntry */
static void commit(app_t *a, const prep_t *p, const result_t *r, const char *method) {
    session_compaction_t c = {.summary = r->summary,
                              .short_summary = r->short_summary,
                              .first_kept_id = r->first_kept,
                              .method = method,
                              .replay_through_id = r->replay_through,
                              .tokens_before = r->tokens_before,
                              .tokens_after = project_after(a, p, r, method),
                              .details = r->details,
                              .preserve_data = r->preserve};
    session_append_compaction_ex(a->session, &c);
    app_reload_context(a);
    a->last_context_tokens = 0;
    a->plan_reference_sent = false;
    advisor_reset(a->advisor);
}

/* ---------- events ---------- */

static const char *action_of(const char *method) {
    return str_eq(method, "remote") ? "remote" : str_eq(method, "snapcompact") ? "snapcompact" : str_eq(method, "handoff") ? "handoff"
                                                                                : str_eq(method, "shake") ? "shake" : "context-full";
}

static const char *reason_name(cmp_reason_t r) {
    return r == CMP_OVERFLOW ? "overflow" : r == CMP_IDLE ? "idle" : r == CMP_INCOMPLETE ? "incomplete" : "threshold";
}

static void emit(app_t *a, const char *type, json_t *ev) {
    json_t *o = json_obj();
    json_set_str(o, "type", type);
    const char *k;
    json_t *v;
    json_obj_foreach(ev, i, k, v) json_set(o, k, json_clone(v));
    json_free(ev);
    char *s = json_dump(o, 0);
    json_free(o);
    app_notice(a, type, "%s", s);
    free(s);
}

static void emit_start(app_t *a, cmp_reason_t reason, const char *action) {
    json_t *e = json_obj();
    json_set_str(e, "reason", reason_name(reason));
    json_set_str(e, "action", action);
    emit(a, "auto_compaction_start", e);
}

/* outcome: "aborted" | "skipped" | "error" | "ok"; result: the compaction result or NULL */
static void emit_end(app_t *a, const char *action, const char *outcome, const char *message, bool will_retry,
                     const result_t *r) {
    json_t *e = json_obj();
    json_set_str(e, "action", action);
    if (r) {
        json_t *res = json_obj();
        json_set_str(res, "summary", r->summary ? r->summary : "");
        if (r->short_summary) json_set_str(res, "shortSummary", r->short_summary);
        json_set_str(res, "firstKeptEntryId", r->first_kept ? r->first_kept : "");
        json_set_int(res, "tokensBefore", r->tokens_before);
        if (r->details) json_set(res, "details", json_clone(r->details));
        json_set(e, "result", res);
    }
    json_set_bool(e, "aborted", str_eq(outcome, "aborted"));
    json_set_bool(e, "willRetry", will_retry);
    if (str_eq(outcome, "skipped")) json_set_bool(e, "skipped", true);
    else if (str_eq(outcome, "error") && strcmp(action, "shake") == 0) json_set_bool(e, "skipped", false);
    if (message) json_set_str(e, "errorMessage", message);
    emit(a, "auto_compaction_end", e);
}

/* ---------- shake ---------- */

typedef struct {
    int64_t protect, min_savings, fence_min;
    bool protect_artifact_reads;
    const char *boundary; /* latest compaction firstKeptEntryId */
} shake_cfg_t;

static const shake_cfg_t DEFAULT_SHAKE = {16000, 4000, 400, true, NULL};
static const shake_cfg_t AGGRESSIVE_SHAKE = {4000, 0, 400, false, NULL};
static const shake_cfg_t RESCUE_SHAKE = {0, 0, 400, true, NULL};

typedef struct {
    bool tool;      /* whole tool result, else a block region */
    json_t *entry;  /* borrowed session entry */
    int block;      /* content index, -1 for string content */
    size_t start, end;
    int64_t tokens;
    char *text;
    const char *label;
} region_t;

typedef VEC(region_t) regions_t;

static void regions_free(regions_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].text);
    vec_free(r);
}

/* branch entries root..leaf (borrowed from the session) */
typedef VEC(json_t *) entvec_t;

static void branch_entries(app_t *a, entvec_t *out) {
    entvec_t rev = {0};
    for (const char *id = a->session->leaf_id; id;) {
        json_t *e = session_get(a->session, id);
        if (!e || rev.n > 1000000) break;
        vec_push(&rev, e);
        id = json_get_str(e, "parentId");
    }
    for (size_t i = rev.n; i-- > 0;) vec_push(out, rev.v[i]);
    vec_free(&rev);
}

static const char *latest_kept_boundary(json_t **ents, size_t n) {
    for (size_t i = n; i-- > 0;)
        if (str_eq(json_get_str(ents[i], "type"), "compaction")) return json_get_str(ents[i], "firstKeptEntryId");
    return NULL;
}

static const json_t *find_call(json_t **ents, size_t n, const char *id) {
    const json_t *found = NULL;
    for (size_t i = 0; i < n && id; i++) {
        const json_t *m = json_get(ents[i], "message"), *b;
        if (!str_eq(json_get_str(ents[i], "type"), "message") || !str_eq(json_get_str(m, "role"), "assistant")) continue;
        json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "toolCall") &&
                                                                str_eq(json_get_str(b, "id"), id)) found = b;
    }
    return found;
}

static const char *read_path(const json_t *result, const json_t *call) {
    if (!str_eq(json_get_str(result, "toolName"), "read") || !str_eq(json_get_str(call, "name"), "read")) return NULL;
    return json_get_str(json_get(call, "arguments"), "path");
}

/* isProtectedToolResult with skill / skill:// read (and artifact:// recovery read) matchers */
static bool protected_result(const json_t *result, const json_t *call, bool artifact_reads) {
    if (str_eq(json_get_str(result, "toolName"), "skill")) return true;
    const char *p = read_path(result, call);
    if (p && str_starts(p, "skill://")) return true;
    if (!artifact_reads) return false;
    if (p && str_starts(p, "artifact://")) return true;
    const json_t *src = json_get_obj(json_get_obj(json_get_obj(result, "details"), "meta"), "source");
    return str_eq(json_get_str(src, "type"), "internal") && json_get_str(src, "value") &&
           str_starts(json_get_str(src, "value"), "artifact://");
}

static bool opening_xml(const char *line, size_t len, char *tag, size_t tagcap) {
    if (len < 3 || line[0] != '<' || line[len - 1] != '>') return false;
    size_t i = 1;
    while (i < len - 1 && (islower((unsigned char)line[i]) || line[i] == '_' || line[i] == '-')) i++;
    if (i == 1) return false;
    size_t tl = i - 1;
    if (i < len - 1) {
        if (!isspace((unsigned char)line[i])) return false;
        for (size_t k = i; k < len - 1; k++)
            if (line[k] == '>') return false;
    }
    if (tl >= tagcap) return false;
    memcpy(tag, line + 1, tl);
    tag[tl] = 0;
    return true;
}

static bool closing_xml(const char *line, size_t len, char *tag, size_t tagcap) {
    if (len < 4 || line[0] != '<' || line[1] != '/' || line[len - 1] != '>') return false;
    size_t tl = len - 3;
    for (size_t k = 2; k < len - 1; k++)
        if (!(islower((unsigned char)line[k]) || line[k] == '_' || line[k] == '-')) return false;
    if (tl >= tagcap) return false;
    memcpy(tag, line + 2, tl);
    tag[tl] = 0;
    return true;
}

static int cmp_range(const void *x, const void *y) {
    const size_t *a = x, *b = y;
    return a[0] < b[0] ? -1 : a[0] > b[0];
}

size_t cmp_block_ranges(const char *text, size_t **out) {
    VEC(size_t) r = {0};
    bool in_fence = false;
    size_t fence_start = 0, xml_start = 0;
    strvec_t stack = {0};
    bool xml_open = false;
    size_t len = strlen(text), ls = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && text[i] != '\n') continue;
        const char *line = text + ls;
        size_t ll = i - ls;
        size_t lead = 0;
        while (lead < ll && isspace((unsigned char)line[lead])) lead++;
        const char *ts = line + lead;
        size_t tl = ll - lead;
        if ((tl >= 3 && !strncmp(ts, "```", 3)) || (tl >= 3 && !strncmp(ts, "~~~", 3))) {
            if (!in_fence) {
                in_fence = true;
                fence_start = ls;
            } else {
                in_fence = false;
                vec_push(&r, fence_start);
                vec_push(&r, i);
            }
            ls = i + 1;
            continue;
        }
        if (!in_fence) {
            char tag[128];
            if (lead == 0 && opening_xml(ts, tl, tag, sizeof tag)) {
                if (!stack.n) {
                    xml_start = ls;
                    xml_open = true;
                }
                strvec_push(&stack, tag);
            } else if (closing_xml(ts, tl, tag, sizeof tag) && stack.n && !strcmp(stack.v[stack.n - 1], tag)) {
                free(stack.v[--stack.n]);
                if (!stack.n && xml_open) {
                    vec_push(&r, xml_start);
                    vec_push(&r, i);
                    xml_open = false;
                }
            }
        }
        ls = i + 1;
    }
    strvec_free(&stack);
    size_t n = r.n / 2;
    qsort(r.v, n, 2 * sizeof(size_t), cmp_range);
    /* mergeRanges: drop ranges overlapping an earlier kept one */
    size_t *kept = xcalloc(n ? 2 * n : 2, sizeof *kept), k = 0;
    size_t last_end = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (any && r.v[2 * i] < last_end) continue;
        kept[2 * k] = r.v[2 * i];
        kept[2 * k + 1] = r.v[2 * i + 1];
        last_end = r.v[2 * i + 1];
        any = true;
        k++;
    }
    vec_free(&r);
    *out = kept;
    return k;
}

static void push_blocks(region_t base, const char *text, const shake_cfg_t *cfg, regions_t *out) {
    size_t *rg = NULL;
    size_t n = cmp_block_ranges(text, &rg);
    for (size_t i = 0; i < n; i++) {
        if (rg[2 * i + 1] <= rg[2 * i]) continue;
        char *slice = xstrndup(text + rg[2 * i], rg[2 * i + 1] - rg[2 * i]);
        int64_t t = text_tokens(slice);
        if (t < cfg->fence_min) {
            free(slice);
            continue;
        }
        region_t r = base;
        r.start = rg[2 * i];
        r.end = rg[2 * i + 1];
        r.tokens = t;
        r.text = slice;
        vec_push(out, r);
    }
    free(rg);
}

static void scan_content(json_t *entry, const json_t *content, const char *label, const shake_cfg_t *cfg, regions_t *out) {
    region_t base = {.entry = entry, .label = label};
    if (json_is_str(content)) {
        base.block = -1;
        push_blocks(base, json_sv(content), cfg, out);
        return;
    }
    const json_t *b;
    json_arr_foreach(content, i, b) if (str_eq(json_get_str(b, "type"), "text") && json_get_str(b, "text")) {
        base.block = (int)i;
        push_blocks(base, json_get_str(b, "text"), cfg, out);
    }
}

static int64_t entry_tokens(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "message")) return cmp_message_tokens(json_get(e, "message"));
    if (str_eq(t, "custom_message")) {
        json_t *m = json_obj();
        json_set(m, "content", json_clone(json_get(e, "content")));
        int64_t n = cmp_message_tokens(m);
        json_free(m);
        return n;
    }
    return 0;
}

/* collectShakeRegions */
static void collect_regions(json_t **ents, size_t n, const shake_cfg_t *cfg, regions_t *out) {
    int64_t *after = xcalloc(n ? n : 1, sizeof *after), acc = 0;
    for (size_t i = n; i-- > 0;) {
        after[i] = acc;
        acc += entry_tokens(ents[i]);
    }
    size_t boundary = 0;
    if (cfg->boundary)
        for (size_t i = 0; i < n; i++)
            if (str_eq(json_get_str(ents[i], "id"), cfg->boundary)) boundary = i;
    for (size_t i = boundary; i < n; i++) {
        json_t *e = ents[i];
        const char *t = json_get_str(e, "type");
        json_t *m = json_get(e, "message");
        bool is_result = str_eq(t, "message") && str_eq(json_get_str(m, "role"), "toolResult");
        bool useless = is_result && json_get_bool(m, "useless", false) && !json_get_bool(m, "isError", false);
        if (!useless && after[i] < cfg->protect) continue;
        if (is_result) {
            if (json_get(m, "prunedAt")) continue;
            if (protected_result(m, find_call(ents, n, json_get_str(m, "toolCallId")), cfg->protect_artifact_reads)) continue;
            sb_t txt = SB_INIT;
            bool any = false;
            const json_t *b;
            json_arr_foreach(json_get_arr(m, "content"), k, b) {
                const char *tx = json_get_str(b, "text");
                if (!str_eq(json_get_str(b, "type"), "text") || !tx || !*tx) continue;
                if (any) sb_putc(&txt, '\n');
                sb_append(&txt, tx);
                any = true;
            }
            if (!any) {
                sb_free(&txt);
                continue;
            }
            region_t r = {.tool = true, .entry = e, .tokens = text_tokens(sb_cstr(&txt)), .label = json_get_str(m, "toolName")};
            r.text = sb_detach(&txt);
            if (!r.label) r.label = "tool";
            vec_push(out, r);
            continue;
        }
        if (str_eq(t, "message")) {
            const char *role = json_get_str(m, "role");
            if (str_eq(role, "assistant")) {
                const json_t *b;
                json_arr_foreach(json_get_arr(m, "content"), k, b) if (str_eq(json_get_str(b, "type"), "text") && json_get_str(b, "text")) {
                    region_t base = {.entry = e, .block = (int)k, .label = "assistant"};
                    push_blocks(base, json_get_str(b, "text"), cfg, out);
                }
            } else if (str_eq(role, "user") || str_eq(role, "developer"))
                scan_content(e, json_get(m, "content"), str_eq(role, "user") ? "user" : "developer", cfg, out);
        } else if (str_eq(t, "custom_message")) {
            scan_content(e, json_get(e, "content"), json_get_str(e, "customType") ? json_get_str(e, "customType") : "custom", cfg, out);
        }
    }
    free(after);
    int64_t savings = 0;
    for (size_t i = 0; i < out->n; i++) savings += MAX(0, out->v[i].tokens - PLACEHOLDER_TOKEN_ESTIMATE);
    if (savings < cfg->min_savings) {
        regions_free(out);
        memset(out, 0, sizeof *out);
    }
}

static char *placeholder(const region_t *r, size_t index, const char *artifact) {
    if (artifact)
        return xasprintf("[shaken ~%lld tokens \xe2\x80\x94 recover: artifact://%s (region %zu)]", (long long)r->tokens, artifact, index + 1);
    return xasprintf("[shaken ~%lld tokens]", (long long)r->tokens);
}

static int cmp_region_start(const void *x, const void *y) {
    const region_t *const *a = x, *const *b = y;
    long sa = (*a)->tool ? -1 : (long)(*a)->start, sb = (*b)->tool ? -1 : (long)(*b)->start;
    return sb < sa ? -1 : sb > sa;
}

/* shake("elide"): returns -1 when a required artifact could not be saved */
static int shake_elide(app_t *a, const shake_cfg_t *cfg_in, bool require_artifact, bool tool_results_only, app_shake_result_t *res) {
    entvec_t ents = {0};
    branch_entries(a, &ents);
    shake_cfg_t cfg = *cfg_in;
    cfg.boundary = latest_kept_boundary(ents.v, ents.n);
    regions_t regions = {0};
    collect_regions(ents.v, ents.n, &cfg, &regions);
    if (tool_results_only) {
        size_t k = 0;
        for (size_t i = 0; i < regions.n; i++) {
            if (regions.v[i].tool) regions.v[k++] = regions.v[i];
            else free(regions.v[i].text);
        }
        regions.n = k;
    }
    if (!regions.n) {
        regions_free(&regions);
        vec_free(&ents);
        return 0;
    }
    /* artifact offload */
    sb_t art = SB_INIT;
    for (size_t i = 0; i < regions.n; i++)
        sb_appendf(&art, "%s### region %zu (%s, ~%lld tok)\n\n%s\n", i ? "\n" : "", i + 1, regions.v[i].label,
                   (long long)regions.v[i].tokens, regions.v[i].text);
    char *artifact = a->env && a->env->artifacts ? artifacts_save(a->env->artifacts, "shake", sb_cstr(&art), art.len) : NULL;
    sb_free(&art);
    if (require_artifact && !artifact) {
        regions_free(&regions);
        vec_free(&ents);
        return -1;
    }
    VEC(region_t *) order = {0};
    for (size_t i = 0; i < regions.n; i++) vec_push(&order, &regions.v[i]);
    char **ph = xcalloc(regions.n, sizeof *ph);
    int64_t orig = 0, repl = 0;
    for (size_t i = 0; i < regions.n; i++) {
        ph[i] = placeholder(&regions.v[i], i, artifact);
        orig += regions.v[i].tokens;
        repl += text_tokens(ph[i]);
        if (regions.v[i].tool) res->tool_results_dropped++;
        else res->blocks_dropped++;
    }
    qsort(order.v, order.n, sizeof *order.v, cmp_region_start);
    for (size_t k = 0; k < order.n; k++) {
        region_t *r = order.v[k];
        const char *p = ph[r - regions.v];
        json_t *m = json_get(r->entry, "message");
        if (r->tool) {
            json_t *kept = json_arr(), *b;
            bool replaced = false;
            json_arr_foreach(json_get_arr(m, "content"), x, b) {
                if (!str_eq(json_get_str(b, "type"), "text")) json_push(kept, json_clone(b));
                else if (!replaced && json_get_str(b, "text") && *json_get_str(b, "text")) {
                    json_push(kept, text_block(p));
                    replaced = true;
                }
            }
            json_set(m, "content", kept);
            json_set_int(m, "prunedAt", now_ms());
            continue;
        }
        json_t *owner = str_eq(json_get_str(r->entry, "type"), "message") ? m : r->entry;
        json_t *c = json_get(owner, "content");
        json_t *slot = r->block < 0 ? c : json_at(c, (size_t)r->block);
        const char *text = r->block < 0 ? json_sv(c) : json_get_str(slot, "text");
        if (!text || r->end > strlen(text)) continue;
        char *nt = xasprintf("%.*s%s%s", (int)r->start, text, p, text + r->end);
        if (r->block < 0) json_set_str(owner, "content", nt);
        else json_set_str(slot, "text", nt);
        free(nt);
        if (owner == m && str_eq(json_get_str(m, "role"), "assistant"))
            json_set_int(m, "elidedOutputTokens", json_get_int(m, "elidedOutputTokens", 0) + MAX(0, r->tokens - text_tokens(p)));
    }
    for (size_t i = 0; i < regions.n; i++) free(ph[i]);
    free(ph);
    vec_free(&order);
    res->tokens_freed = MAX(0, orig - repl);
    free(artifact);
    regions_free(&regions);
    vec_free(&ents);
    session_rewrite(a->session);
    app_reload_context(a);
    advisor_reset(a->advisor);
    return 1;
}

void app_shake(app_t *a, const char *mode, app_shake_result_t *r) {
    memset(r, 0, sizeof *r);
    r->mode = str_eq(mode, "images") ? "images" : str_eq(mode, "thinking") ? "thinking" : "elide";
    if (!a->session) return;
    if (!strcmp(r->mode, "elide")) {
        shake_elide(a, &AGGRESSIVE_SHAKE, false, false, r);
        return;
    }
    entvec_t ents = {0};
    branch_entries(a, &ents);
    bool images = !strcmp(r->mode, "images");
    for (size_t i = 0; i < ents.n; i++) {
        json_t *e = ents.v[i];
        const char *type = json_get_str(e, "type");
        json_t *owner = str_eq(type, "message") ? json_get(e, "message") : str_eq(type, "custom_message") && images ? e : NULL;
        if (!owner || !json_is_arr(json_get(owner, "content"))) continue;
        if (!images && !str_eq(json_get_str(owner, "role"), "assistant")) continue;
        int64_t before = cmp_message_tokens(owner);
        json_t *b, *kept = json_arr();
        int dropped = 0;
        json_arr_foreach(json_get_arr(owner, "content"), k, b) {
            const char *bt = json_get_str(b, "type");
            bool drop = images ? str_eq(bt, "image") : (str_eq(bt, "thinking") || str_eq(bt, "redactedThinking"));
            if (drop) dropped++;
            else json_push(kept, json_clone(b));
        }
        if (!dropped) {
            json_free(kept);
            continue;
        }
        if (images && !json_len(kept) && owner == e) json_push(kept, text_block("[image removed]"));
        json_set(owner, "content", kept);
        r->tokens_freed += MAX(0, before - cmp_message_tokens(owner));
        if (images) r->images_dropped += dropped;
        else r->thinking_dropped += dropped;
    }
    vec_free(&ents);
    if (r->images_dropped + r->thinking_dropped == 0) {
        r->tokens_freed = 0;
        return;
    }
    session_rewrite(a->session);
    app_reload_context(a);
    advisor_reset(a->advisor);
}

/* shake-types.ts formatShakeSummary */
char *app_shake_summary(const app_shake_result_t *r) {
    if (!strcmp(r->mode, "images")) {
        int n = r->images_dropped;
        return n == 0 ? xstrdup("No images found in this session.") : xasprintf("Dropped %d image%s from this session.", n, n == 1 ? "" : "s");
    }
    if (!strcmp(r->mode, "thinking")) {
        int n = r->thinking_dropped;
        if (n == 0) return xstrdup("No thinking blocks found in this session.");
        char *freed = r->tokens_freed > 0 ? xasprintf(" (~%lld tokens freed)", (long long)r->tokens_freed) : xstrdup("");
        char *s = xasprintf("Dropped %d thinking block%s from this session%s.", n, n == 1 ? "" : "s", freed);
        free(freed);
        return s;
    }
    sb_t b = SB_INIT;
    if (r->tool_results_dropped > 0) sb_appendf(&b, "%d tool result%s", r->tool_results_dropped, r->tool_results_dropped == 1 ? "" : "s");
    if (r->blocks_dropped > 0) sb_appendf(&b, "%s%d block%s", b.len ? " + " : "", r->blocks_dropped, r->blocks_dropped == 1 ? "" : "s");
    if (!b.len) {
        sb_free(&b);
        return xstrdup("Nothing to shake.");
    }
    char *s = xasprintf("Shook %s (~%lld tokens freed).", sb_cstr(&b), (long long)r->tokens_freed);
    sb_free(&b);
    return s;
}

/* ---------- handoff ---------- */

typedef struct {
    json_t *sys, *tools, *wire;
    char *sid, *main_sid;
} handoff_req_t;

static void handoff_req_free(handoff_req_t *h) {
    if (h->sid) anth_threads_release_session(h->sid);
    json_free(h->sys);
    json_free(h->tools);
    json_free(h->wire);
    free(h->sid);
    free(h->main_sid);
    memset(h, 0, sizeof *h);
}

/* the handoff request built through the live turn's pipeline (context hooks run on the calling thread) */
static bool handoff_build(app_t *a, const char *focus, handoff_req_t *h, char **err) {
    memset(h, 0, sizeof *h);
    const ai_model_t *model = a->model;
    if (!model) {
        *err = xstrdup("No model selected for handoff");
        return false;
    }
    char *key = app_api_key_for(model);
    bool keyed = key && *key;
    free(key);
    if (!keyed && !catalog_provider_keyless(model->provider)) {
        *err = xasprintf("No API key for %s", model->provider);
        return false;
    }
    json_t *ctx = json_obj();
    if (focus && *focus) json_set_str(ctx, "additionalFocus", focus);
    char *prompt = prompt_render("agent/compaction/prompts/handoff-document.md", ctx);
    json_free(ctx);
    pthread_mutex_lock(&a->agent->mu);
    json_t *msgs = json_clone(a->agent->messages);
    h->sys = json_clone(a->agent->system_prompt);
    h->tools = json_arr();
    for (size_t i = 0; i < a->agent->tools.n; i++) json_push(h->tools, agent_tool_wire(a->agent->tools.v[i]));
    h->main_sid = a->agent->session_id ? xstrdup(a->agent->session_id) : NULL;
    pthread_mutex_unlock(&a->agent->mu);
    json_push(msgs, user_text(prompt ? prompt : "", "agent"));
    free(prompt);
    json_t *tx = a->agent->hooks.transform_context ? a->agent->hooks.transform_context(a->agent->hooks.ud, a->agent, msgs) : NULL;
    json_t *snap = tx ? tx : msgs;
    ai_prepare_context_images(snap, model, h->sys, h->tools);
    h->wire = ai_transform_messages(snap, model);
    json_free(tx);
    json_free(msgs);
    char *rid = short_id();
    h->sid = xasprintf("%s:side:%s", h->main_sid ? h->main_sid : "", rid);
    free(rid);
    return true;
}

/* SessionHandoff.generateDocument's request: owned text, NULL + *err on failure; NULL without *err for an empty
 * auto-triggered document */
static char *handoff_exec(app_t *a, handoff_req_t *h, bool auto_triggered, char **err) {
    const ai_model_t *model = a->model;
    ai_context_t c = {.system_prompt = h->sys, .messages = h->wire, .tools = json_len(h->tools) ? h->tools : NULL};
    ai_stream_opts_t o = a->agent->base_opts;
    char *key = app_api_key_for(model);
    o.api_key = key;
    app_fill_request_opts(a, &o);
    o.session_id = h->sid;
    o.provider_session_state = true;
    o.metadata = (json_t *)app_session_metadata(a, model->provider);
    o.prompt_cache_key = h->main_sid;
    compaction_effort(a, model, &o);
    o.disable_reasoning = false;
    o.cancel = cancel_ptr(a);
    o.on_payload = NULL;
    o.cursor_exec = NULL;
    json_t *none = json_str("none"), *autoc = json_str("auto");
    o.tool_choice = none;
    json_t *r = ai_complete(model, &c, &o);
    const char *em = json_get_str(r, "errorMessage");
    if (str_eq(json_get_str(r, "stopReason"), "error") && json_get_int(r, "errorStatus", 0) == 400 && em &&
        str_istr(em, "tool_choice") && str_istr(em, "auto") && str_istr(em, "supported")) {
        json_free(r);
        o.tool_choice = autoc;
        r = ai_complete(model, &c, &o);
    }
    json_free(none), json_free(autoc);
    char *text = NULL;
    const char *stop = json_get_str(r, "stopReason");
    if (str_eq(stop, "aborted") || cancelled(a)) *err = xstrdup("Handoff cancelled");
    else if (str_eq(stop, "error") || !r) {
        em = json_get_str(r, "errorMessage");
        *err = xasprintf("Handoff generation failed: %s", em && *em ? em : "Unknown error");
    } else {
        text = joined_text(r);
        char *t = str_trim(text);
        bool empty = !*t;
        free(t);
        if (empty) {
            free(text);
            text = NULL;
            if (!auto_triggered) *err = xstrdup("Handoff generation produced no content");
        }
    }
    json_free(r);
    free(key);
    if (text && auto_triggered && settings_bool("compaction.handoffSaveToDisk", false) && a->env && a->env->artifacts &&
        artifacts_dir(a->env->artifacts)) {
        /* handoff-<ISO timestamp with : and . as -> .md in the session artifacts dir */
        char *iso = iso8601_now();
        for (char *q = iso; *q; q++)
            if (*q == ':' || *q == '.') *q = '-';
        char *path = xasprintf("%s/handoff-%s.md", artifacts_dir(a->env->artifacts), iso);
        char *body = xasprintf("%s\n", text);
        if (fs_write(path, body, strlen(body)) != 0) LOGW("Failed to save handoff document to disk: %s", path);
        free(body), free(path), free(iso);
    }
    return text;
}

static char *handoff_document(app_t *a, const char *focus, bool auto_triggered, char **err) {
    handoff_req_t h;
    if (!handoff_build(a, focus, &h, err)) return NULL;
    char *doc = handoff_exec(a, &h, auto_triggered, err);
    handoff_req_free(&h);
    return doc;
}

/* handoffSummaryFromDocument */
static void handoff_result(const prep_t *p, const char *doc, result_t *r) {
    memset(r, 0, sizeof *r);
    json_t *rf = NULL, *mf = NULL;
    r->summary = branchsum_upsert_files(doc, p->ops, &rf, &mf);
    r->details = json_obj();
    json_set(r->details, "readFiles", rf);
    json_set(r->details, "modifiedFiles", mf);
    r->first_kept = xstrdup(p->first_kept);
    r->tokens_before = p->tokens_before;
}

int app_handoff(app_t *a, const char *focus, char **err) {
    *err = NULL;
    if (a->compacting) {
        *err = xstrdup("Compaction already in progress");
        return -1;
    }
    if (!a->model) {
        *err = xstrdup("No model selected for handoff");
        return -1;
    }
    size_t messages = 0;
    entvec_t ents = {0};
    branch_entries(a, &ents);
    for (size_t i = 0; i < ents.n; i++) messages += str_eq(json_get_str(ents.v[i], "type"), "message");
    vec_free(&ents);
    if (messages < 2) {
        *err = xstrdup("Nothing to hand off (no messages yet)");
        return -1;
    }
    spec_cancel(a);
    a->compacting = true;
    if (!agent_is_running(a->agent)) a->agent->cancel = 0;
    prep_t p;
    int rc = -1;
    if (prepare(a, a->model, &p) != 0) {
        *err = xstrdup("Nothing to hand off (already compacted)");
        goto out;
    }
    char *doc = handoff_document(a, focus, false, err);
    if (!doc) goto out;
    result_t r;
    handoff_result(&p, doc, &r);
    free(doc);
    commit(a, &p, &r, "handoff");
    result_free(&r);
    app_notice(a, "info", "Context compacted");
    rc = 0;
out:
    prep_free(&p);
    a->compacting = false;
    return rc;
}

/* ---------- manual ---------- */

/* SessionMaintenance.compact: manual /compact over the (mode-overridden) method order */
static int manual(app_t *a, const char *mode, const char *instructions, char **err) {
    const ai_model_t *model = a->model;
    if (!model) {
        *err = xstrdup("No model selected");
        return -1;
    }
    strvec_t methods = mode_methods(mode);
    bool explicit_snap = str_eq(mode, "snapcompact");
    int rc = -1;
    for (size_t idx = 0; idx < methods.n; idx++) {
        const char *m = methods.v[idx];
        if (str_eq(m, "remote")) {
            if (!remote_usable(model)) continue;
        } else if (str_eq(m, "snapcompact")) {
            /* omc has no snapcompact renderer: an explicit request reports it once, naming the /compact mode of the next
             * manual method (remote or soft); the ladder passes over it */
            if (!explicit_snap) continue;
            const char *fallback = "soft";
            strvec_t order = cmp_method_order();
            for (size_t k = 0; k < order.n; k++) {
                if (str_eq(order.v[k], "soft") || (str_eq(order.v[k], "remote") && remote_usable(model))) {
                    fallback = str_eq(order.v[k], "remote") ? "remote" : "soft";
                    break;
                }
            }
            strvec_free(&order);
            if (!model->input_image)
                *err = xasprintf("snapcompact needs a vision-capable model (%s is text-only). Run /compact %s instead.", model->id, fallback);
            else *err = xasprintf("snapcompact is not implemented in omc. Run /compact %s instead.", fallback);
            break;
        } else if (!str_eq(m, "soft")) continue;
        bool remote = str_eq(m, "remote");
        if (remote && !remote_endpoint_set()) {
            mr_pool_t cands = {0};
            app_compaction_candidates(a, &cands);
            bool any = false;
            for (size_t i = 0; i < cands.n && !any; i++) any = live_native(cands.v[i], model);
            mr_pool_free(&cands);
            if (!any) {
                app_notice(a, "warning", "remote compaction is unavailable for %s; trying the next preferred method", model->id);
                continue;
            }
        }
        prep_t p;
        int pr = prepare(a, model, &p);
        if (pr) {
            *err = xstrdup(pr == 1 ? "Already compacted" : "Nothing to compact (session too small)");
            prep_free(&p);
            break;
        }
        app_notice(a, "info", "Compacting context\xe2\x80\xa6");
        result_t r;
        free(*err);
        *err = NULL;
        if (compact_candidates(a, &p, instructions, remote, false, &r, err)) {
            commit(a, &p, &r, m);
            result_free(&r);
            prep_free(&p);
            app_notice(a, "info", "Context compacted");
            free(*err);
            *err = NULL;
            rc = 0;
            break;
        }
        prep_free(&p);
        if (cancelled(a)) {
            free(*err);
            *err = xstrdup("Compaction cancelled");
            break;
        }
        if (idx + 1 < methods.n)
            app_notice(a, "warning", "%s compaction failed (%s); trying the next preferred method", m, *err ? *err : "unknown error");
    }
    if (rc != 0 && !*err) *err = xstrdup("No configured compaction method can run manually.");
    strvec_free(&methods);
    return rc;
}

int app_compact_mode(app_t *a, const char *mode, const char *instructions, char **err) {
    *err = NULL;
    if (a->compacting) {
        *err = xstrdup("compaction already running");
        return -1;
    }
    spec_cancel(a); /* a manual pass supersedes background speculation */
    a->compacting = true;
    if (!agent_is_running(a->agent)) a->agent->cancel = 0; /* a stale Esc must not cancel this pass */
    int rc = manual(a, mode, instructions, err);
    a->compacting = false;
    return rc;
}

int app_compact(app_t *a, const char *instructions, char **err) { return app_compact_mode(a, NULL, instructions, err); }

/* ---------- dead-end rescue ---------- */

static char *thousands(int64_t v) {
    char raw[32];
    snprintf(raw, sizeof raw, "%lld", (long long)(v < 0 ? -v : v));
    sb_t b = SB_INIT;
    if (v < 0) sb_putc(&b, '-');
    size_t n = strlen(raw);
    for (size_t i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, raw[i]);
    }
    return sb_detach(&b);
}

enum { PROGRESS_PREPARE, PROGRESS_HEADROOM, PROGRESS_RETRY_FIT };

/* #compactionCreatedHeadroom / #compactionCreatedRetryFit */
static bool has_progress(app_t *a, int kind) {
    const ai_model_t *m = a->model;
    if (!m || m->context_window <= 0) return true;
    if (kind == PROGRESS_PREPARE) {
        prep_t p;
        bool ok = prepare(a, m, &p) == 0;
        prep_free(&p);
        return ok;
    }
    int64_t stored = stored_context_tokens(a);
    if (kind == PROGRESS_HEADROOM) return stored <= (int64_t)floor((double)threshold_for(m) * RECOVERY_BAND);
    int64_t w = m->context_window;
    int64_t budget = MAX(0, MIN(w - 1, w - budget_reserve(w)));
    return stored <= budget;
}

/* #rescueCompactionDeadEnd: elide inside the kept tail, then drop images */
static bool rescue_dead_end(app_t *a, bool skip_elide, int kind, int *flags) {
    if (cancelled(a)) return false;
    int elided = 0;
    int64_t elided_tokens = 0;
    bool artifact = false;
    if (!skip_elide) {
        app_shake_result_t r;
        memset(&r, 0, sizeof r);
        r.mode = "elide";
        if (shake_elide(a, &RESCUE_SHAKE, false, false, &r) > 0) {
            elided = r.tool_results_dropped + r.blocks_dropped;
            elided_tokens = r.tokens_freed;
            artifact = a->env && a->env->artifacts && artifacts_dir(a->env->artifacts);
            *flags |= CMP_REWRITTEN;
        }
        if (elided > 0 && has_progress(a, kind)) {
            char *t = thousands(elided_tokens);
            app_notice(a, "info", "Compaction dead-end recovery: elided %d heavy block%s (~%s tokens) to %s so maintenance could make progress.",
                       elided, elided == 1 ? "" : "s", t, artifact ? "an artifact" : "placeholders");
            free(t);
            return true;
        }
    }
    if (cancelled(a)) return false;
    app_shake_result_t ir;
    app_shake(a, "images", &ir);
    if (ir.images_dropped > 0) *flags |= CMP_REWRITTEN;
    if (ir.images_dropped > 0 && has_progress(a, kind)) {
        char *part = NULL;
        if (elided > 0) {
            char *t = thousands(elided_tokens);
            part = xasprintf("elided %d heavy block%s (~%s tokens) to %s and ", elided, elided == 1 ? "" : "s", t,
                             artifact ? "an artifact" : "placeholders");
            free(t);
        }
        app_notice(a, "info", "Compaction dead-end recovery: %sdropped %d attached image%s so maintenance could make progress.",
                   part ? part : "", ir.images_dropped, ir.images_dropped == 1 ? "" : "s");
        free(part);
        return true;
    }
    return false;
}

/* #compactionDeadEndWarning (owned) */
static char *dead_end_warning(app_t *a, const char *remedies) {
    const ai_model_t *m = a->model;
    if (m && m->context_window > 0 && nonmessage_tokens(a) > (int64_t)floor((double)threshold_for(m) * RECOVERY_BAND))
        return xstrdup("Compaction cannot create headroom \xe2\x80\x94 pausing automatic maintenance. The system prompt and always-sent "
                       "tool schemas alone exceed the recovery band; reduce fixed context or adjust compaction.modelThresholds.");
    return xasprintf("Compaction freed too little context to make progress \xe2\x80\x94 pausing automatic maintenance to avoid a "
                     "compaction loop. The most recent turn alone is too large to reduce further; %s or switch to a larger-context model.",
                     remedies);
}

/* stamp the dead-end warning on the branch's latest compaction entry (the divider badge) */
static void stamp_warning(app_t *a, const char *warning) {
    entvec_t ents = {0};
    branch_entries(a, &ents);
    for (size_t i = ents.n; i-- > 0;)
        if (str_eq(json_get_str(ents.v[i], "type"), "compaction")) {
            json_set_str(ents.v[i], "warning", warning);
            session_rewrite(a->session);
            break;
        }
    vec_free(&ents);
}

/* ---------- async (speculative) compaction ---------- */

#define SPECULATION_LEAD_MIN_TOKENS 8192
#define SPECULATION_LEAD_MAX_TOKENS 32000

struct cmp_spec {
    app_t *a;
    pthread_t th;
    volatile sig_atomic_t cancel;
    volatile int done; /* the thread finished (armed or failed) */
    bool armed, native_failed, joined;
    char *method; /* remote | handoff | soft */
    char *snapshot_leaf;
    int64_t ctx_at_start;
    prep_t prep;
    handoff_req_t hreq;
    result_t result;
};

/* resolveSpeculationLeadTokens */
static int64_t spec_lead(int64_t threshold) {
    return MIN(SPECULATION_LEAD_MAX_TOKENS, MAX(SPECULATION_LEAD_MIN_TOKENS, (int64_t)floor((double)threshold * 0.125)));
}

static bool async_enabled(void) {
    return settings_bool("compaction.enabled", true) && settings_bool("compaction.asyncEnabled", true);
}

/* resolveSpeculationMethod: the first available configured method when it is an LLM one; local methods
 * (shake, snapcompact) are instant and are not speculated. Anthropic-native compaction replays the live
 * turn's transformed prefix, which a background thread cannot rebuild, so it is not speculated in omc. */
static const char *spec_method(app_t *a, bool skip_remote) {
    const ai_model_t *m = a->model;
    strvec_t methods = cmp_method_order();
    const char *out = NULL;
    for (size_t i = 0; i < methods.n; i++) {
        const char *c = methods.v[i];
        if (str_eq(c, "remote")) {
            if (skip_remote || !remote_usable(m)) continue;
            out = (remote_endpoint_set() || openai_v1(m)) ? "remote" : NULL;
            break;
        }
        if (str_eq(c, "snapcompact")) {
            if (m && m->input_image) break; /* local: nothing to speculate */
            continue;
        }
        if (str_eq(c, "handoff") || str_eq(c, "soft")) out = str_eq(c, "handoff") ? "handoff" : "soft";
        break;
    }
    strvec_free(&methods);
    return out;
}

static void *spec_main(void *arg) {
    struct cmp_spec *sp = arg;
    tl_cancel = &sp->cancel;
    dcr_set_side_thread(true); /* the turn that writes the session may still be running */
    char *err = NULL;
    bool ok;
    if (str_eq(sp->method, "handoff")) {
        char *doc = handoff_exec(sp->a, &sp->hreq, true, &err);
        ok = doc != NULL;
        if (ok) handoff_result(&sp->prep, doc, &sp->result);
        free(doc);
    } else {
        ok = compact_candidates(sp->a, &sp->prep, NULL, str_eq(sp->method, "remote"), false, &sp->result, &err);
        if (!ok && sp->result.native_error) sp->native_failed = true;
        if (!ok) result_free(&sp->result);
    }
    free(err);
    sp->armed = ok && !sp->cancel;
    __atomic_store_n(&sp->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void spec_free(struct cmp_spec *sp) {
    if (!sp) return;
    sp->cancel = 1;
    if (!sp->joined) pthread_join(sp->th, NULL);
    prep_free(&sp->prep);
    handoff_req_free(&sp->hreq);
    result_free(&sp->result);
    free(sp->method);
    free(sp->snapshot_leaf);
    free(sp);
}

/* cancelSpeculation */
static void spec_cancel(app_t *a) {
    spec_free(a->cmp_spec);
    a->cmp_spec = NULL;
}

void cmp_free(app_t *a) { spec_cancel(a); }

static bool spec_done(const struct cmp_spec *sp) { return __atomic_load_n(&sp->done, __ATOMIC_ACQUIRE) != 0; }

/* #startSpeculationRun: the branch snapshot is prepared here; only the model requests run in the background */
static void spec_start(app_t *a, int64_t ctx, const char *method) {
    struct cmp_spec *sp = xcalloc(1, sizeof *sp);
    sp->a = a;
    if (prepare(a, a->model, &sp->prep) != 0 || !a->session->leaf_id) {
        prep_free(&sp->prep);
        free(sp);
        return;
    }
    if (str_eq(method, "handoff")) {
        char *focus = prompt_render("agent/compaction/prompts/auto-handoff-threshold-focus.md", NULL), *err = NULL;
        bool ok = handoff_build(a, focus, &sp->hreq, &err);
        free(focus);
        free(err);
        if (!ok) {
            prep_free(&sp->prep);
            free(sp);
            return;
        }
    }
    sp->method = xstrdup(method);
    sp->snapshot_leaf = xstrdup(a->session->leaf_id);
    sp->ctx_at_start = ctx;
    if (pthread_create(&sp->th, NULL, spec_main, sp) != 0) {
        prep_free(&sp->prep);
        handoff_req_free(&sp->hreq);
        free(sp->method);
        free(sp->snapshot_leaf);
        free(sp);
        return;
    }
    a->cmp_spec = sp;
}

/* index of an entry id on the current branch (entries borrowed), -1 when absent */
static ssize_t branch_index(json_t **ents, size_t n, const char *id) {
    for (size_t i = 0; id && i < n; i++)
        if (str_eq(json_get_str(ents[i], "id"), id)) return (ssize_t)i;
    return -1;
}

/* #armedSpeculationValid */
static bool spec_valid(app_t *a, struct cmp_spec *sp, int64_t trigger) {
    const ai_model_t *m = a->model;
    if (!m || !sp->armed) return false;
    const json_t *oa = json_get_obj(sp->result.preserve, "openaiRemoteCompaction");
    if (oa && !(str_eq(json_get_str(oa, "provider"), m->provider) && responses_api(m->api) && cmp_remote_supported(m))) return false;
    entvec_t ents = {0};
    branch_entries(a, &ents);
    ssize_t leaf = branch_index(ents.v, ents.n, sp->snapshot_leaf);
    bool ok = leaf >= 0;
    for (size_t i = (size_t)(leaf + 1); ok && i < ents.n; i++) {
        const char *t = json_get_str(ents.v[i], "type");
        if (str_eq(t, "compaction") || str_eq(t, "reset_boundary")) ok = false;
    }
    bool grew = ok && (size_t)leaf + 1 < ents.n;
    bool native_anthropic = json_get(sp->result.preserve, "anthropicCompaction") != NULL;
    if (ok && grew && branch_index(ents.v, ents.n, sp->result.first_kept) < 0 &&
        !(native_anthropic && str_eq(sp->result.first_kept, "")))
        ok = false;
    vec_free(&ents);
    if (!ok) return false;
    free(sp->result.replay_through);
    sp->result.replay_through = (oa || (native_anthropic && grew)) ? xstrdup(sp->snapshot_leaf) : NULL;
    int64_t projected = project_after(a, NULL, &sp->result, sp->method);
    int64_t now = branch_tokens(a);
    int64_t baseline = MAX(MAX(now, sp->ctx_at_start), MAX(sp->result.tokens_before, trigger));
    if (projected >= baseline) return false;
    if (grew && !native_anthropic) {
        int64_t band = (int64_t)floor((double)threshold_for(m) * RECOVERY_BAND);
        if (projected > band) return false;
        int64_t stored = stored_context_tokens(a);
        if (stored > 0 && projected >= stored) return false;
        if (trigger > 0 && projected >= trigger) return false;
    }
    return true;
}

/* #claimArmedSpeculation: an in-flight run is aborted; an armed result is returned when still valid (owned) */
static struct cmp_spec *spec_claim(app_t *a, int64_t trigger) {
    struct cmp_spec *sp = a->cmp_spec;
    if (!sp) return NULL;
    a->cmp_spec = NULL;
    if (!spec_done(sp) || !sp->armed || !settings_bool("compaction.asyncEnabled", true) || !spec_valid(a, sp, trigger)) {
        spec_free(sp);
        return NULL;
    }
    pthread_join(sp->th, NULL);
    sp->joined = true;
    return sp;
}

/* maybeStartSpeculativeCompaction: inside [threshold - lead, threshold) a background run prepares the summary */
static void spec_maybe_start(app_t *a, int64_t ctx) {
    const ai_model_t *m = a->model;
    if (!m || m->context_window <= 0 || !async_enabled() || a->compacting || !a->session) return;
    int64_t th = threshold_for(m);
    if (ctx >= th || th - ctx > spec_lead(th)) return;
    struct cmp_spec *sp = a->cmp_spec;
    bool skip_remote = sp && spec_done(sp) && sp->native_failed;
    if (sp) {
        if (!spec_done(sp)) return; /* one run at a time */
        if (sp->armed) {
            /* refresh on growth: a fresh cut reclaims more once the kept tail grew past the keep budget */
            int64_t growth = ctx - sp->ctx_at_start;
            int64_t budget = MAX(settings_int("compaction.keepRecentTokens", 20000), SPECULATION_LEAD_MIN_TOKENS);
            if ((growth <= budget || json_get(sp->result.preserve, "anthropicCompaction")) && spec_valid(a, sp, 0)) return;
        }
        spec_cancel(a);
    }
    const char *method = spec_method(a, skip_remote);
    if (method) spec_start(a, ctx, method);
}

/* deferThresholdCompactionToSpeculation: within the grace band above the threshold a live run absorbs the
 * pass; true while deferral is in effect */
static bool spec_defer(app_t *a, int64_t ctx) {
    const ai_model_t *m = a->model;
    if (!m || m->context_window <= 0 || !async_enabled() || a->compacting || !a->session) return false;
    struct cmp_spec *sp = a->cmp_spec;
    bool skip_remote = sp && spec_done(sp) && sp->native_failed;
    const char *method = spec_method(a, skip_remote);
    if (!method) return false;
    int64_t th = threshold_for(m);
    int64_t grace = MIN(th + spec_lead(th), m->context_window - SPECULATION_LEAD_MIN_TOKENS);
    if (ctx >= grace) return false;
    if (sp) {
        if (!spec_done(sp)) return true; /* still summarizing in the background */
        if (sp->armed) {
            if (!spec_valid(a, sp, 0)) {
                spec_cancel(a);
                return false;
            }
            return false; /* ready: the real pass splices it in now */
        }
        spec_cancel(a); /* failed: a fresh run */
    }
    spec_start(a, ctx, method);
    return a->cmp_spec != NULL;
}

/* ---------- automatic ---------- */

static bool method_usable(const char *m, cmp_reason_t reason, const ai_model_t *model) {
    if (str_eq(m, "remote")) return remote_usable(model);
    if (str_eq(m, "snapcompact")) return false; /* not implemented in omc */
    if (str_eq(m, "handoff")) return reason != CMP_OVERFLOW;
    return true;
}

static bool later_usable(const strvec_t *methods, size_t from, cmp_reason_t reason, const ai_model_t *model, bool skip_shake) {
    for (size_t i = from; i < methods->n; i++)
        if (!(skip_shake && str_eq(methods->v[i], "shake")) && method_usable(methods->v[i], reason, model)) return true;
    return false;
}

/* drop a trailing failed assistant before a retry (error, or a truncated incomplete turn) */
static void drop_failed_tail(app_t *a, cmp_reason_t reason) {
    pthread_mutex_lock(&a->agent->mu);
    size_t n = json_len(a->agent->messages);
    json_t *last = n ? json_at(a->agent->messages, n - 1) : NULL;
    const char *st = json_get_str(last, "stopReason");
    if (str_eq(json_get_str(last, "role"), "assistant") && (str_eq(st, "error") || (reason == CMP_INCOMPLETE && str_eq(st, "length"))))
        json_free(json_arr_remove(a->agent->messages, n - 1));
    pthread_mutex_unlock(&a->agent->mu);
}

/* #runAutoShake: 0 done, 1 fall back to the next method */
static int auto_shake(app_t *a, cmp_reason_t reason, bool will_retry, int64_t trigger, int *flags) {
    emit_start(a, reason, "shake");
    app_shake_result_t r;
    memset(&r, 0, sizeof r);
    r.mode = "elide";
    shake_elide(a, &DEFAULT_SHAKE, false, false, &r);
    if (cancelled(a)) {
        emit_end(a, "shake", "aborted", NULL, false, NULL);
        return 0;
    }
    bool reclaimed = r.tool_results_dropped + r.blocks_dropped > 0;
    bool still_over = false;
    const ai_model_t *m = a->model;
    if (m && m->context_window > 0) {
        int64_t th = threshold_for(m);
        if (trigger > 0) still_over = MAX(0, trigger - r.tokens_freed) > (int64_t)floor((double)th * RECOVERY_BAND);
        else still_over = stored_context_tokens(a) > th;
    }
    if (reason != CMP_IDLE && ((reason == CMP_OVERFLOW && !reclaimed) || still_over)) {
        char *msg = reclaimed ? xasprintf("Auto-shake reclaimed ~%lld tokens but context is still above the threshold; trying the "
                                          "next preferred compaction method.",
                                          (long long)r.tokens_freed)
                              : xstrdup("Auto-shake found nothing eligible to drop; trying the next preferred compaction method.");
        json_t *e = json_obj();
        json_set_str(e, "action", "shake");
        json_set_bool(e, "aborted", false);
        json_set_bool(e, "willRetry", false);
        json_set_bool(e, "skipped", !reclaimed);
        json_set_str(e, "errorMessage", msg);
        emit(a, "auto_compaction_end", e);
        free(msg);
        if (reclaimed) *flags |= CMP_REWRITTEN;
        return 1;
    }
    json_t *e = json_obj();
    json_set_str(e, "action", "shake");
    json_set_bool(e, "aborted", false);
    json_set_bool(e, "willRetry", will_retry);
    json_set_bool(e, "skipped", !reclaimed);
    emit(a, "auto_compaction_end", e);
    if (will_retry) drop_failed_tail(a, reason);
    if (reclaimed) *flags |= CMP_REWRITTEN | CMP_COMMITTED;
    else if (will_retry) *flags |= CMP_COMMITTED;
    return 0;
}

static const char *context_prefix(cmp_reason_t reason) {
    return reason == CMP_OVERFLOW ? "Context overflow recovery failed: " : reason == CMP_INCOMPLETE ? "Incomplete response recovery failed: "
                                                                                                    : "Auto-compaction failed: ";
}

/* #thresholdFixedContextDeadEnd: when the system prompt and tool schemas alone exceed the recovery band, no threshold
 * pass can land under it, and re-running one every turn loops (#18589). Warns once per session. */
static bool threshold_fixed_context_dead_end(app_t *a) {
    const ai_model_t *m = a->model;
    if (!m || m->context_window <= 0 ||
        nonmessage_tokens(a) <= (int64_t)floor((double)threshold_for(m) * RECOVERY_BAND)) {
        free(a->fixed_dead_end_session);
        a->fixed_dead_end_session = NULL;
        return false;
    }
    const char *sid = a->session ? session_id(a->session) : NULL;
    if (!sid || !str_eq(a->fixed_dead_end_session, sid)) {
        app_notice(a, "warning", "%s",
                   "Compaction cannot create headroom \xe2\x80\x94 pausing automatic maintenance. The system prompt and "
                   "always-sent tool schemas alone exceed the recovery band; reduce fixed context or adjust "
                   "compaction.modelThresholds.");
        free(a->fixed_dead_end_session);
        a->fixed_dead_end_session = sid ? xstrdup(sid) : NULL;
    }
    return true;
}

int cmp_auto(app_t *a, cmp_reason_t reason, bool will_retry, int64_t trigger) {
    if (reason != CMP_IDLE && !settings_bool("compaction.enabled", true)) return CMP_NONE;
    if (a->compacting || !a->session) return CMP_NONE;
    /* every threshold caller and strategy goes through here, so the fixed-context gate lives here once */
    if (reason == CMP_THRESHOLD && threshold_fixed_context_dead_end(a)) return CMP_BLOCKED;
    strvec_t methods = cmp_method_order();
    if (!methods.n) {
        strvec_free(&methods);
        return CMP_NONE;
    }
    a->compacting = true;
    if (!agent_is_running(a->agent)) a->agent->cancel = 0;
    int flags = CMP_NONE;
    bool from_shake = false, picked_any = false;
    for (size_t idx = 0; idx < methods.n; idx++) {
        const char *m = methods.v[idx];
        if (!method_usable(m, reason, a->model)) continue;
        picked_any = true;
        /* an armed background speculation supplies this pass's summary (snapcompact is local and never waits) */
        struct cmp_spec *armed = str_eq(m, "snapcompact") ? NULL : spec_claim(a, trigger);
        if (armed) {
            const char *action = str_eq(armed->method, "remote") ? "remote" : str_eq(armed->method, "handoff") ? "handoff" : "context-full";
            emit_start(a, reason, action);
            result_t *r = &armed->result;
            r->tokens_before = MAX(MAX(r->tokens_before, trigger > 0 ? trigger : 0), branch_tokens(a));
            commit(a, &armed->prep, r, armed->method);
            flags |= CMP_REWRITTEN;
            bool progress = true;
            if (will_retry) {
                drop_failed_tail(a, reason);
                progress = has_progress(a, PROGRESS_RETRY_FIT) || rescue_dead_end(a, from_shake, PROGRESS_RETRY_FIT, &flags);
            } else if (reason != CMP_IDLE) {
                progress = has_progress(a, PROGRESS_HEADROOM) || rescue_dead_end(a, from_shake, PROGRESS_HEADROOM, &flags);
            }
            char *warning = progress ? NULL : dead_end_warning(a, "clear large tool output");
            if (warning) stamp_warning(a, warning);
            emit_end(a, action, "ok", NULL, will_retry, r);
            if (warning) app_notice(a, "warning", "%s", warning);
            free(warning);
            spec_free(armed);
            flags |= progress ? CMP_COMMITTED : CMP_BLOCKED;
            break;
        }
        if (str_eq(m, "shake")) {
            if (auto_shake(a, reason, will_retry, trigger, &flags) == 1) {
                from_shake = true;
                continue;
            }
            break;
        }
        const char *action = action_of(m);
        emit_start(a, reason, action);
        if (!a->model) {
            emit_end(a, action, "skipped", NULL, false, NULL);
            break;
        }
        prep_t p;
        int pr = prepare(a, a->model, &p);
        if (pr && reason != CMP_IDLE && rescue_dead_end(a, from_shake, PROGRESS_PREPARE, &flags)) {
            /* dead-end rescue: an oversized kept tail was reduced so the cut can move */
            prep_free(&p);
            pr = prepare(a, a->model, &p);
        }
        if (pr) {
            prep_free(&p);
            emit_end(a, action, "skipped", NULL, false, NULL);
            if (reason != CMP_IDLE) {
                char *w = dead_end_warning(a, "shrink it (e.g. clear large tool output)");
                app_notice(a, "warning", "%s", w);
                free(w);
                flags |= CMP_BLOCKED;
            }
            break;
        }
        result_t r;
        memset(&r, 0, sizeof r);
        char *err = NULL;
        bool ok;
        if (str_eq(m, "handoff")) {
            char *auto_focus = prompt_render("agent/compaction/prompts/auto-handoff-threshold-focus.md", NULL);
            char *doc = handoff_document(a, auto_focus, true, &err);
            free(auto_focus);
            if (cancelled(a)) {
                free(doc);
                free(err);
                prep_free(&p);
                emit_end(a, action, "aborted", NULL, false, NULL);
                break;
            }
            if (!doc && !err) {
                prep_free(&p);
                emit_end(a, action, "error", "Auto-handoff returned no document; trying the next preferred compaction method.", false, NULL);
                continue;
            }
            ok = doc != NULL;
            if (ok) handoff_result(&p, doc, &r);
            free(doc);
        } else {
            ok = compact_candidates(a, &p, NULL, str_eq(m, "remote"), true, &r, &err);
        }
        if (!ok) {
            prep_free(&p);
            if (cancelled(a)) {
                free(err);
                emit_end(a, action, "aborted", NULL, false, NULL);
                break;
            }
            char *msg = xasprintf("%s%s", context_prefix(reason), err ? err : "compaction failed");
            free(err);
            if (idx + 1 < methods.n) {
                char *m2 = xasprintf("%s; trying the next preferred compaction method.", msg);
                emit_end(a, action, "error", m2, false, NULL);
                free(m2);
                free(msg);
                continue;
            }
            emit_end(a, action, "error", msg, false, NULL);
            free(msg);
            break;
        }
        free(err);
        /* #compactionReducesContext */
        int64_t before_branch = branch_tokens(a);
        int64_t effective = MAX(r.tokens_before, trigger > 0 ? trigger : 0);
        int64_t after = project_after(a, &p, &r, m);
        int64_t baseline = MAX(effective, before_branch);
        if (after >= baseline) {
            bool later = later_usable(&methods, idx + 1, reason, a->model, false);
            char *msg = later ? xasprintf("%s compaction would not reduce context; trying the next preferred compaction method.", action)
                              : xasprintf("%s compaction would not reduce context.", action);
            app_notice(a, "warning", "%s", msg);
            json_t *e = json_obj();
            json_set_str(e, "action", action);
            json_set_bool(e, "aborted", false);
            json_set_bool(e, "willRetry", false);
            json_set_bool(e, "skipped", !later);
            json_set_str(e, "errorMessage", msg);
            emit(a, "auto_compaction_end", e);
            free(msg);
            result_free(&r);
            prep_free(&p);
            if (later) continue;
            if (reason == CMP_THRESHOLD) flags |= CMP_BLOCKED;
            break;
        }
        if (after < MAX(effective, before_branch)) r.tokens_before = MAX(effective, before_branch);
        commit(a, &p, &r, m);
        flags |= CMP_REWRITTEN;
        /* post-maintenance progress guard: a retry only has to fit, a threshold pass needs the recovery band */
        bool progress = true;
        if (will_retry) {
            drop_failed_tail(a, reason);
            progress = has_progress(a, PROGRESS_RETRY_FIT) || rescue_dead_end(a, from_shake, PROGRESS_RETRY_FIT, &flags);
        } else if (reason != CMP_IDLE) {
            progress = has_progress(a, PROGRESS_HEADROOM) || rescue_dead_end(a, from_shake, PROGRESS_HEADROOM, &flags);
        }
        char *warning = progress ? NULL : dead_end_warning(a, "clear large tool output");
        if (warning) stamp_warning(a, warning);
        emit_end(a, action, "ok", NULL, will_retry, &r);
        if (warning) app_notice(a, "warning", "%s", warning);
        free(warning);
        result_free(&r);
        prep_free(&p);
        if (progress) flags |= CMP_COMMITTED;
        else flags |= CMP_BLOCKED;
        break;
    }
    if (!picked_any && reason == CMP_THRESHOLD && from_shake) flags |= CMP_BLOCKED;
    strvec_free(&methods);
    a->compacting = false;
    return flags;
}

/* ---------- pruning (compaction/pruning.ts) ---------- */

/* splitReadSelector's selector part: everything after the stripped path, or NULL */
char *cmp_read_supersede_key(const char *tool, const json_t *args) {
    if (!str_eq(tool, "read")) return NULL;
    const char *path = json_get_str(args, "path");
    if (!path || !*path || strstr(path, "://")) return NULL;
    char *base = branchsum_strip_read_selector(path);
    size_t bl = strlen(base);
    if (bl == strlen(path)) return base;
    const char *sel = path + bl + 1;
    bool alt = false;
    char *selc = xstrdup(sel);
    for (char *tok = strtok(selc, ":"); tok; tok = strtok(NULL, ":"))
        if (!strcasecmp(tok, "raw") || !strcasecmp(tok, "conflicts")) alt = true;
    free(selc);
    char *k = xasprintf("%s%c%s", base, alt ? '\x02' : '\x01', sel);
    free(base);
    return k;
}

/* isCompleteReadResult */
static bool complete_read(const json_t *msg) {
    const json_t *d = json_get_obj(msg, "details");
    if (!d || json_truthy(json_get(d, "summary")) || json_get_bool(json_get_obj(d, "truncation"), "truncated", false)) return false;
    const json_t *meta = json_get_obj(d, "meta");
    if (json_truthy(json_get(meta, "truncation")) || json_get_bool(json_get_obj(meta, "limits"), "columnTruncated", false)) return false;
    if (json_get(d, "totalLines")) return true;
    const json_t *b;
    json_arr_foreach(json_get_arr(msg, "content"), i, b) if (str_eq(json_get_str(b, "type"), "image")) return true;
    return false;
}

typedef struct {
    json_t *msg;
    size_t index;
    int64_t tokens;
    const char *notice;
} cand_t;

typedef VEC(cand_t) cands_t;

typedef struct {
    char *key;
    bool success, complete;
} newer_t;
typedef VEC(newer_t) newers_t;

static newer_t *newer_get(newers_t *v, const char *key) {
    for (size_t i = 0; i < v->n; i++)
        if (!strcmp(v->v[i].key, key)) return &v->v[i];
    return NULL;
}

static json_t *result_msg(json_t *e) {
    json_t *m = json_get(e, "message");
    return str_eq(json_get_str(e, "type"), "message") && str_eq(json_get_str(m, "role"), "toolResult") ? m : NULL;
}

/* collectSupersededResults (message order) */
static void collect_superseded(json_t **ents, size_t n, size_t start, cands_t *out) {
    newers_t newer = {0};
    cands_t rev = {0};
    for (size_t i = n; i-- > start;) {
        json_t *m = result_msg(ents[i]);
        if (!m || json_get(m, "prunedAt")) continue;
        const json_t *call = find_call(ents, n, json_get_str(m, "toolCallId"));
        if (!call || protected_result(m, call, false)) continue;
        char *key = cmp_read_supersede_key(json_get_str(call, "name"), json_get(call, "arguments"));
        if (!key) continue;
        char *sep = strchr(key, '\x01');
        char *parent_key = sep ? xstrndup(key, (size_t)(sep - key)) : NULL;
        newer_t *same = newer_get(&newer, key);
        newer_t *parent = parent_key ? newer_get(&newer, parent_key) : NULL;
        bool err = json_get_bool(m, "isError", false);
        bool superseded = err ? (same != NULL || (parent && parent->success)) : ((same && same->success) || (parent && parent->complete));
        if (!same) {
            newer_t nn = {.key = xstrdup(key)};
            vec_push(&newer, nn);
            same = &newer.v[newer.n - 1];
        }
        if (!err && !same->success) {
            same->success = true;
            same->complete = complete_read(m);
        }
        free(key);
        free(parent_key);
        if (!superseded) continue;
        cand_t c = {.msg = m, .index = i, .tokens = cmp_message_tokens(m), .notice = SUPERSEDED_NOTICE};
        vec_push(&rev, c);
    }
    for (size_t i = rev.n; i-- > 0;) vec_push(out, rev.v[i]);
    vec_free(&rev);
    for (size_t i = 0; i < newer.n; i++) free(newer.v[i].key);
    vec_free(&newer);
}

static int64_t pruned_savings(int64_t tokens, const char *notice) { return MAX(0, tokens - (int64_t)((strlen(notice) + 3) / 4)); }

static bool cand_has(const cands_t *c, const json_t *m) {
    for (size_t i = 0; i < c->n; i++)
        if (c->v[i].msg == m) return true;
    return false;
}

static void collect_useless(json_t **ents, size_t n, size_t start, const cands_t *exclude, cands_t *out) {
    for (size_t i = start; i < n; i++) {
        json_t *m = result_msg(ents[i]);
        if (!m || !json_get_bool(m, "useless", false) || json_get(m, "prunedAt") || json_get_bool(m, "isError", false)) continue;
        if (cand_has(exclude, m)) continue;
        if (protected_result(m, find_call(ents, n, json_get_str(m, "toolCallId")), false)) continue;
        int64_t t = cmp_message_tokens(m);
        if (pruned_savings(t, USELESS_NOTICE) <= 0) continue;
        cand_t c = {.msg = m, .index = i, .tokens = t, .notice = USELESS_NOTICE};
        vec_push(out, c);
    }
}

static void blank(json_t *m, const char *notice, int64_t at) {
    json_t *c = json_arr();
    json_push(c, text_block(notice));
    json_set(m, "content", c);
    json_set_int(m, "prunedAt", at);
}

static int cmp_cand(const void *x, const void *y) {
    const cand_t *a = x, *b = y;
    return a->index < b->index ? -1 : a->index > b->index;
}

static size_t boundary_index(json_t **ents, size_t n) {
    const char *b = latest_kept_boundary(ents, n);
    for (size_t i = n; b && i-- > 0;)
        if (str_eq(json_get_str(ents[i], "id"), b)) return i;
    return 0;
}

/* pruneSupersededToolResults: cache-aware (small suffix, or an idle cold cache) */
static int64_t prune_stale(app_t *a) {
    bool supersede = settings_bool("compaction.supersedeReads", true), useless = settings_bool("compaction.dropUseless", true);
    if (!supersede && !useless) return 0;
    entvec_t ents = {0};
    branch_entries(a, &ents);
    size_t start = boundary_index(ents.v, ents.n);
    cands_t c = {0};
    if (supersede) collect_superseded(ents.v, ents.n, start, &c);
    if (useless) {
        cands_t u = {0};
        collect_useless(ents.v, ents.n, start, &c, &u);
        for (size_t i = 0; i < u.n; i++) vec_push(&c, u.v[i]);
        vec_free(&u);
        if (c.n > 1) qsort(c.v, c.n, sizeof *c.v, cmp_cand);
    }
    int64_t saved = 0;
    if (c.n) {
        int64_t last_ts = -1;
        for (size_t i = ents.n; i-- > 0 && last_ts < 0;)
            if (str_eq(json_get_str(ents.v[i], "type"), "message")) last_ts = json_get_int(json_get(ents.v[i], "message"), "timestamp", 0);
        bool idle = last_ts > 0 && now_ms() - last_ts >= PRUNE_IDLE_FLUSH_MS;
        int64_t limit = (a->model && a->model->thinking && json_get_bool(a->model->thinking, "prefixBinding", false)) ? 0
                                                                                                                     : SUPERSEDE_SUFFIX_TOKEN_LIMIT;
        cands_t take = {0};
        if (idle) {
            for (size_t i = 0; i < c.n; i++) vec_push(&take, c.v[i]);
        } else {
            int64_t suffix = 0;
            ssize_t next = (ssize_t)c.n - 1;
            for (size_t i = ents.n; i-- > 0 && next >= 0 && suffix <= limit;) {
                while (next >= 0 && c.v[next].index == i) vec_push(&take, c.v[next--]);
                if (str_eq(json_get_str(ents.v[i], "type"), "message")) suffix += cmp_message_tokens(json_get(ents.v[i], "message"));
                if (i == 0) break;
            }
        }
        int64_t at = now_ms();
        for (size_t i = 0; i < take.n; i++) {
            saved += pruned_savings(take.v[i].tokens, take.v[i].notice);
            blank(take.v[i].msg, take.v[i].notice, at);
        }
        if (take.n) {
            session_rewrite(a->session);
            app_reload_context(a);
        }
        vec_free(&take);
    }
    vec_free(&c);
    vec_free(&ents);
    return saved;
}

/* pruneToolOutputs (DEFAULT_PRUNE_CONFIG: protect 40k, minimum savings 20k) */
static int64_t prune_outputs(app_t *a) {
    entvec_t ents = {0};
    branch_entries(a, &ents);
    size_t start = boundary_index(ents.v, ents.n);
    cands_t none = {0}, use = {0}; /* supersedeKey is not passed on this path */
    bool drop_useless = settings_bool("compaction.dropUseless", true);
    if (drop_useless) collect_useless(ents.v, ents.n, start, &none, &use);
    int64_t warm = (a->model && a->model->thinking && json_get_bool(a->model->thinking, "prefixBinding", false)) ? 0
                                                                                                                 : PRUNE_CACHE_WARM_SUFFIX_TOKENS;
    cands_t take = {0};
    int64_t acc = 0, suffix = 0, saved = 0;
    for (size_t i = ents.n; i-- > start;) {
        int64_t after = suffix;
        json_t *m = result_msg(ents.v[i]);
        if (!m) {
            if (str_eq(json_get_str(ents.v[i], "type"), "message")) suffix += cmp_message_tokens(json_get(ents.v[i], "message"));
            continue;
        }
        int64_t t = cmp_message_tokens(m);
        suffix += t;
        if (after > warm) break;
        if (json_get(m, "prunedAt")) {
            acc += t;
            continue;
        }
        bool useless = cand_has(&use, m);
        if (!useless && (acc < 40000 || t < MIN_PRUNE_TOKENS ||
                         protected_result(m, find_call(ents.v, ents.n, json_get_str(m, "toolCallId")), false))) {
            acc += t;
            continue;
        }
        cand_t c = {.msg = m, .index = i, .tokens = t, .notice = useless ? USELESS_NOTICE : NULL};
        vec_push(&take, c);
        acc += t;
    }
    for (size_t i = 0; i < take.n; i++) {
        char *nt = take.v[i].notice ? NULL : xasprintf("[Output truncated - %lld tokens]", (long long)take.v[i].tokens);
        saved += pruned_savings(take.v[i].tokens, take.v[i].notice ? take.v[i].notice : nt);
        free(nt);
    }
    if (!take.n || saved < 20000) saved = 0;
    else {
        int64_t at = now_ms();
        for (size_t i = 0; i < take.n; i++) {
            char *nt = take.v[i].notice ? xstrdup(take.v[i].notice) : xasprintf("[Output truncated - %lld tokens]", (long long)take.v[i].tokens);
            blank(take.v[i].msg, nt, at);
            free(nt);
        }
        session_rewrite(a->session);
        app_reload_context(a);
    }
    vec_free(&take);
    vec_free(&use);
    vec_free(&ents);
    return saved;
}

/* ---------- triggers ---------- */

static const json_t *last_assistant_msg(app_t *a) {
    const json_t *m = NULL, *x;
    json_arr_foreach(a->agent->messages, i, x) if (str_eq(json_get_str(x, "role"), "assistant")) m = x;
    return m;
}

void cmp_after_turn(app_t *a) {
    if (!a->session || !a->model) return;
    prune_stale(a);
    if (!settings_bool("compaction.enabled", true)) return;
    strvec_t methods = cmp_method_order();
    size_t nm = methods.n;
    strvec_free(&methods);
    if (!nm) return;
    pthread_mutex_lock(&a->agent->mu);
    const json_t *la = last_assistant_msg(a);
    bool errored = la && str_eq(json_get_str(la, "stopReason"), "error");
    pthread_mutex_unlock(&a->agent->mu);
    if (errored) return;
    prune_outputs(a);
    int64_t window = a->model->context_window;
    if (window <= 0) return;
    int64_t th = threshold_for(a->model);
    int64_t ctx = MAX(a->last_context_tokens, stored_context_tokens(a));
    if (th <= 0) return;
    if (ctx <= th) {
        spec_maybe_start(a, ctx);
        return;
    }
    if (spec_defer(a, ctx)) return; /* a background speculation absorbs the blocking summarization */
    cmp_auto(a, CMP_THRESHOLD, false, ctx);
}

void cmp_before_prompt(app_t *a, const json_t *pending) {
    if (!a->session || !a->model || a->compacting || a->model->context_window <= 0) return;
    if (!settings_bool("compaction.enabled", true)) return;
    strvec_t methods = cmp_method_order();
    size_t nm = methods.n;
    strvec_free(&methods);
    if (!nm) return;
    int64_t th = threshold_for(a->model);
    int64_t ctx = stored_context_tokens(a) + messages_tokens(pending);
    if (th <= 0) return;
    if (ctx <= th) {
        spec_maybe_start(a, ctx);
        return;
    }
    if (spec_defer(a, ctx)) return;
    cmp_auto(a, CMP_THRESHOLD, false, ctx);
}

void cmp_mid_turn(app_t *a, const json_t *assistant) {
    if (!a->session || !a->model || a->compacting || cancelled(a)) return;
    if (!settings_bool("compaction.enabled", true) || !settings_bool("compaction.midTurnEnabled", true)) return;
    strvec_t methods = cmp_method_order();
    size_t nm = methods.n;
    strvec_free(&methods);
    if (!nm || !assistant) return;
    const char *st = json_get_str(assistant, "stopReason");
    if (str_eq(st, "aborted") || str_eq(st, "error")) return;
    if (a->model->context_window <= 0) return;
    int64_t billed = usage_context_tokens(json_get_obj(assistant, "usage"));
    int64_t ctx = MAX(billed, stored_context_tokens(a));
    int64_t th = threshold_for(a->model);
    if (th <= 0) return;
    if (ctx <= th) {
        spec_maybe_start(a, ctx);
        return;
    }
    if (spec_defer(a, ctx)) return;
    cmp_auto(a, CMP_THRESHOLD, false, ctx);
}
