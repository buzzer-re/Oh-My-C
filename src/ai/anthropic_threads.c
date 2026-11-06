/* anthropic_threads.c - Anthropic message threads; see anthropic_threads.h.
 *
 * Prompt caching inside a thread (measured upstream against the API): the create's system, tools and non-last message
 * breakpoints keep working on every continue; a continue's own breakpoints act on that request only, each looking back
 * about 20 blocks. A continue therefore marks its first appended block, which reaches the previous request's entry,
 * besides its newest block, which writes this turn (continue_breakpoints). */
#define _GNU_SOURCE
#include "ai/anthropic_threads.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

#define THREAD_REUSE_WINDOW_MS 270000 /* the server keeps a thread 5 minutes after its last stored response */
#define MAX_REFUSED_CONTINUES 2
#define MAX_THREAD_CHAINS 16 /* per endpoint and model: side requests mint one-shot ids, each chain pins a conversation */

typedef struct {
    char *message_id;
    json_t *messages;    /* the conversation the server holds: the request's full messages, then the response */
    json_t *fingerprint; /* fixed and create-only fields; any change starts a new thread */
    int64_t stored_at;
} anchor_t;

typedef struct {
    char *session_id;
    anchor_t *anchor; /* a continue that names it removes it: each id continues once */
    int refused;
    bool disabled;
    int refs; /* the store's reference plus each turn using the chain */
} chain_t;

typedef struct {
    char *key; /* base URL \n model id */
    VEC(chain_t *) chains; /* least recently used first */
} store_t;

struct anth_thread_turn {
    chain_t *chain;
    bool resend_system_tools;
    anchor_t *claimed; /* taken off the chain when first planned */
    bool force_create;
    bool unthreaded;
    bool disable_on_success;
    bool planned;
    bool planned_continue;
    json_t *planned_messages;
    json_t *planned_fingerprint;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static VEC(store_t *) g_stores;

static void anchor_free(anchor_t *a) {
    if (!a) return;
    free(a->message_id);
    json_free(a->messages);
    json_free(a->fingerprint);
    free(a);
}

static void chain_unref(chain_t *c) {
    if (--c->refs > 0) return;
    free(c->session_id);
    anchor_free(c->anchor);
    free(c);
}

/* parseFlag over the variable: empty or unset keeps the default, any other value but a truthy spelling is false */
static bool flag_or(const char *name, bool def) {
    const char *v = env_get(name);
    if (str_empty(v)) return def;
    static const char *const truthy[] = {"1", "TRUE", "true", "YES", "yes", "ON", "on"};
    for (size_t i = 0; i < ARRAY_LEN(truthy); i++)
        if (!strcmp(v, truthy[i])) return true;
    return false;
}

static bool official_url(const char *u) {
    static const char official[] = "https://api.anthropic.com";
    if (str_empty(u)) return true;
    size_t n = strlen(official);
    return strncasecmp(u, official, n) == 0 && (u[n] == 0 || u[n] == '/');
}

bool anth_threads_wanted(const ai_model_t *m, const ai_stream_opts_t *o, const char *base_url) {
    if (o && o->stateful_responses) return o->stateful_responses > 0;
    json_t *flag = model_compat_get(m, "supportsMessageThreads");
    bool policy = json_is_bool(flag) && flag->u.b && official_url(base_url);
    /* withoutDisabledExperimentalBetas: Claude Code's kill switch clears the model's flag */
    json_t *kill = model_compat_get(m, "honorsClaudeCodeBetaKillSwitch");
    if (json_is_bool(kill) && kill->u.b && env_truthy("CLAUDE_CODE_DISABLE_EXPERIMENTAL_BETAS")) policy = false;
    return flag_or("PI_ANTHROPIC_MESSAGE_THREADS", policy);
}

/* anthropicThreadChain under g_mu: created on first use and marked most recently used. Returns a new reference. */
static chain_t *chain_for(const char *base_url, const char *model_id, const char *session_id) {
    char *key = xasprintf("%s\n%s", base_url ? base_url : "", model_id ? model_id : "");
    store_t *st = NULL;
    for (size_t i = 0; i < g_stores.n && !st; i++)
        if (!strcmp(g_stores.v[i]->key, key)) st = g_stores.v[i];
    if (st) {
        free(key);
    } else {
        st = xcalloc(1, sizeof *st);
        st->key = key;
        vec_push(&g_stores, st);
    }
    chain_t *c = NULL;
    for (size_t i = 0; i < st->chains.n; i++) {
        if (strcmp(st->chains.v[i]->session_id, session_id)) continue;
        c = st->chains.v[i];
        memmove(&st->chains.v[i], &st->chains.v[i + 1], (st->chains.n - i - 1) * sizeof st->chains.v[0]);
        st->chains.n--;
        break;
    }
    if (!c) {
        c = xcalloc(1, sizeof *c);
        c->session_id = xstrdup(session_id);
        c->refs = 1;
        if (st->chains.n >= MAX_THREAD_CHAINS) {
            chain_unref(st->chains.v[0]);
            memmove(&st->chains.v[0], &st->chains.v[1], (st->chains.n - 1) * sizeof st->chains.v[0]);
            st->chains.n--;
        }
    }
    vec_push(&st->chains, c);
    c->refs++;
    return c;
}

anth_thread_turn_t *anth_thread_turn_new(const char *base_url, const char *model_id, const char *session_id,
                                         bool resend_system_tools) {
    anth_thread_turn_t *t = xcalloc(1, sizeof *t);
    pthread_mutex_lock(&g_mu);
    t->chain = chain_for(base_url, model_id, session_id);
    pthread_mutex_unlock(&g_mu);
    t->resend_system_tools = resend_system_tools;
    return t;
}

static void planned_clear(anth_thread_turn_t *t) {
    t->planned = false;
    json_free(t->planned_messages);
    json_free(t->planned_fingerprint);
    t->planned_messages = t->planned_fingerprint = NULL;
}

void anth_thread_turn_free(anth_thread_turn_t *t) {
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    chain_unref(t->chain);
    pthread_mutex_unlock(&g_mu);
    anchor_free(t->claimed);
    planned_clear(t);
    free(t);
}

/* isThreadable: no compaction, unsupported context edits, turn-scoped messages or credit redemption */
static bool is_threadable(const json_t *p) {
    if (json_truthy(json_get(p, "compaction")) || json_truthy(json_get(p, "fallback_credit_token"))) return false;
    json_t *e;
    json_arr_foreach(json_get(json_get_obj(p, "context_management"), "edits"), i, e) {
        if (!str_eq(json_get_str(e, "type"), "clear_thinking_20251015")) return false;
    }
    json_t *msg;
    json_arr_foreach(json_get(p, "messages"), j, msg) {
        if (str_eq(json_get_str(msg, "clear_at"), "next_user_message")) return false;
    }
    return true;
}

static int str_cmp_qsort(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* [...new Set(normalizeExtraBetas(header))].sort().join(",") */
static char *betas_key(const char *header) {
    strvec_t parts = str_split(header ? header : "", ",");
    strvec_t set = {0};
    for (size_t i = 0; i < parts.n; i++) {
        char *b = str_trim(parts.v[i]);
        if (*b && !strvec_contains(&set, b)) strvec_push(&set, b);
        free(b);
    }
    strvec_free(&parts);
    if (set.n > 1) qsort(set.v, set.n, sizeof set.v[0], str_cmp_qsort);
    char *r = strvec_join(&set, ",");
    strvec_free(&set);
    return r;
}

static json_t *fingerprint_of(const json_t *p, const char *beta_header) {
    static const char *const keys[] = {"model", "system", "tools", "tool_choice", "thinking", "output_config", "context_management"};
    json_t *f = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        json_t *v = json_get(p, keys[i]);
        if (v) json_set(f, keys[i], json_clone(v));
    }
    json_set_bool(f, "fallbacks", json_len(json_get(p, "fallbacks")) > 0);
    json_set(f, "betas", json_str_own(betas_key(beta_header)));
    return f;
}

static bool has_cache_control(const json_t *block) { return json_truthy(json_get(block, "cache_control")); }

/* deep equality of two content blocks, cache breakpoints aside */
static bool same_block(const json_t *a, const json_t *b) {
    if (!json_is_obj(a) || !json_is_obj(b)) return json_equal(a, b);
    size_t na = 0, nb = 0;
    const char *k;
    json_t *v;
    json_obj_foreach(a, i, k, v) {
        if (!strcmp(k, "cache_control") && has_cache_control(a)) continue;
        na++;
        if (!json_get(b, k) || !json_equal(v, json_get(b, k))) return false;
    }
    json_obj_foreach(b, j, k, v) {
        if (!strcmp(k, "cache_control") && has_cache_control(b)) continue;
        nb++;
    }
    return na == nb;
}

/* soleText: string content, or a single text block that differs from it only by a breakpoint (caching rewrites a
 * string message into that block) */
static const char *sole_text(const json_t *content) {
    if (json_is_str(content)) return json_sv(content);
    if (!json_is_arr(content) || json_len(content) != 1) return NULL;
    const json_t *block = json_at(content, 0);
    if (!str_eq(json_get_str(block, "type"), "text")) return NULL;
    const char *k;
    json_t *v;
    json_obj_foreach(block, i, k, v) {
        if (strcmp(k, "type") && strcmp(k, "text") && strcmp(k, "cache_control")) return NULL;
    }
    return json_get_str(block, "text");
}

static bool same_message(const json_t *a, const json_t *b) {
    if (!str_eq(json_get_str(a, "role"), json_get_str(b, "role"))) return false;
    if (!str_eq(json_get_str(a, "clear_at"), json_get_str(b, "clear_at"))) return false;
    if (!json_equal(json_get(a, "output_config"), json_get(b, "output_config"))) return false;
    const json_t *ca = json_get(a, "content"), *cb = json_get(b, "content");
    if (json_is_str(ca) || json_is_str(cb)) {
        const char *ta = sole_text(ca), *tb = sole_text(cb);
        return ta && tb && !strcmp(ta, tb);
    }
    if (json_len(ca) != json_len(cb)) return false;
    for (size_t i = 0; i < json_len(ca); i++)
        if (!same_block(json_at(ca, i), json_at(cb, i))) return false;
    return true;
}

/* extendsConversation: `messages` appends at least one message to `held`, cache breakpoints aside */
static bool extends_conversation(const json_t *messages, const json_t *held) {
    if (json_len(messages) <= json_len(held)) return false;
    for (size_t i = 0; i < json_len(held); i++)
        if (!same_message(json_at(messages, i), json_at(held, i))) return false;
    return true;
}

static bool block_cacheable(const json_t *b) {
    const char *t = json_get_str(b, "type");
    return str_eq(t, "text") || str_eq(t, "image") || str_eq(t, "tool_use") || str_eq(t, "tool_result");
}

/* continueBreakpoints: keep the newest breakpoint, which writes the turn, add one on the first appended block, which
 * reads up to the previous request, and drop the rest; with the create's system and tools breakpoints that stays
 * within the four a request may count. Messages without a breakpoint (caching off) pass through. */
static void continue_breakpoints(json_t *appended) {
    const json_t *tmpl = NULL;
    long first_m = -1, first_b = -1, newest_m = -1, newest_b = -1;
    for (size_t m = 0; m < json_len(appended); m++) {
        const json_t *content = json_get(json_at(appended, m), "content");
        if (json_is_str(content)) {
            if (first_m < 0) first_m = (long)m, first_b = 0;
            continue;
        }
        for (size_t b = 0; b < json_len(content); b++) {
            const json_t *block = json_at(content, b);
            if (first_m < 0 && block_cacheable(block)) first_m = (long)m, first_b = (long)b;
            if (has_cache_control(block)) newest_m = (long)m, newest_b = (long)b;
            if (block_cacheable(block) && has_cache_control(block)) tmpl = json_get(block, "cache_control");
        }
    }
    if (!tmpl || first_m < 0) return;
    json_t *cc = json_clone(tmpl); /* tmpl lives in a block this loop edits */
    for (size_t m = 0; m < json_len(appended); m++) {
        json_t *msg = json_at(appended, m);
        json_t *content = json_get(msg, "content");
        if ((long)m != first_m && json_is_str(content)) continue;
        if (json_is_str(content)) {
            json_t *arr = json_arr(), *tb = json_obj();
            json_set_str(tb, "type", "text");
            json_set_str(tb, "text", json_sv(content));
            json_push(arr, tb);
            json_set(msg, "content", arr);
            content = arr;
        }
        for (size_t b = 0; b < json_len(content); b++) {
            json_t *block = json_at(content, b);
            bool marked = ((long)m == first_m && (long)b == first_b) || ((long)m == newest_m && (long)b == newest_b);
            if (!marked) {
                if (has_cache_control(block)) json_del(block, "cache_control");
            } else if (block_cacheable(block) && !has_cache_control(block)) {
                json_set(block, "cache_control", json_clone(cc));
            }
        }
    }
    json_free(cc);
}

json_t *anth_thread_plan(anth_thread_turn_t *t, const json_t *params, const char *beta_header, int64_t now,
                         bool *is_continue) {
    if (is_continue) *is_continue = false;
    pthread_mutex_lock(&g_mu);
    if (t->unthreaded || t->chain->disabled || !is_threadable(params)) {
        planned_clear(t);
        pthread_mutex_unlock(&g_mu);
        return NULL;
    }
    json_t *fp = fingerprint_of(params, beta_header);
    const json_t *messages = json_get(params, "messages");
    anchor_t *anchor = t->force_create ? NULL : t->claimed ? t->claimed : t->chain->anchor;
    bool cont = anchor && now - anchor->stored_at < THREAD_REUSE_WINDOW_MS && json_equal(anchor->fingerprint, fp) &&
                extends_conversation(messages, anchor->messages);
    json_t *body = json_clone(params);
    json_t *thread = json_obj();
    if (cont) {
        if (t->chain->anchor == anchor) t->chain->anchor = NULL;
        t->claimed = anchor;
        json_t *appended = json_arr();
        for (size_t i = json_len(anchor->messages); i < json_len(messages); i++) json_push(appended, json_clone(json_at(messages, i)));
        continue_breakpoints(appended);
        json_set(body, "messages", appended);
        json_set_str(thread, "type", "continue");
        json_set_str(thread, "previous_message_id", anchor->message_id);
        if (!t->resend_system_tools) {
            json_del(body, "system");
            json_del(body, "tools");
        }
    } else {
        json_set_str(thread, "type", "create");
    }
    json_set(body, "thread", thread);
    pthread_mutex_unlock(&g_mu);
    planned_clear(t);
    t->planned = true;
    t->planned_continue = cont;
    t->planned_messages = json_clone(messages);
    t->planned_fingerprint = fp;
    if (is_continue) *is_continue = cont;
    return body;
}

/* the error text of thread 400s that carry no error_code:
 * /(?<!diagnostics\.)previous_message_id|\bthread\b.{0,3}(?:param|field|request|type|:)/i */
static bool thread_error_text(const char *msg) {
    static const char pid[] = "previous_message_id", diag[] = "diagnostics.";
    size_t dl = strlen(diag);
    for (const char *p = msg; (p = strcasestr(p, pid)); p++) {
        if ((size_t)(p - msg) < dl || strncasecmp(p - dl, diag, dl)) return true;
    }
    for (const char *p = msg; (p = strcasestr(p, "thread")); p++) {
        bool left = p == msg || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        const char *e = p + 6;
        if (!left || isalnum((unsigned char)*e) || *e == '_') continue;
        for (int gap = 0; gap <= 3 && e[gap]; gap++) {
            const char *q = e + gap;
            if (!strncasecmp(q, "param", 5) || !strncasecmp(q, "field", 5) || !strncasecmp(q, "request", 7) ||
                !strncasecmp(q, "type", 4) || *q == ':')
                return true;
            if (*q == '\n' || *q == '\r') break; /* `.` stops at a line end */
        }
    }
    return false;
}

/* threadErrorCode: error.details.error_code (else error.type) naming a thread error, or a codeless thread 400. Owned. */
static char *thread_error_code(int status, const char *body) {
    if (status != 400 && status != 404) return NULL;
    char *trimmed = str_trim(body ? body : "");
    const char *detail = *trimmed ? trimmed : "status code (no body)";
    json_t *j = json_parse_str(detail);
    const json_t *err = json_get_obj(j, "error");
    const char *code = json_get_str(json_get_obj(err, "details"), "error_code");
    if (!code) code = json_get_str(err, "type");
    char *r = NULL;
    if (code && str_starts(code, "thread_")) {
        r = xstrdup(code);
    } else if (status == 404) {
        if (strstr(detail, "thread_not_found")) r = xstrdup("thread_not_found"); /* a 404 for an unknown model is not one */
    } else {
        char *msg = xasprintf("%d %s", status, detail);
        if (thread_error_text(msg)) r = xstrdup("thread_invalid_request");
        free(msg);
    }
    json_free(j);
    free(trimmed);
    return r;
}

/* g_mu held */
static void disable_chain(anth_thread_turn_t *t) {
    t->chain->disabled = true;
    anchor_free(t->chain->anchor);
    t->chain->anchor = NULL;
    anchor_free(t->claimed);
    t->claimed = NULL;
    t->unthreaded = true;
}

anth_thread_recovery_t anth_thread_recover(anth_thread_turn_t *t, int status, const char *body) {
    if (!t->planned) return ANTH_THREAD_NO_RECOVERY;
    char *code = thread_error_code(status, body);
    if (!code) return ANTH_THREAD_NO_RECOVERY;
    bool was_continue = t->planned_continue;
    planned_clear(t);
    anth_thread_recovery_t r;
    pthread_mutex_lock(&g_mu);
    if (!strcmp(code, "thread_unsupported_request") || !was_continue) {
        /* an organization setting or a feature threads lack; a create refused for a thread reason says the same */
        disable_chain(t);
        r = ANTH_THREAD_UNTHREADED;
    } else {
        /* expired, unstored, already continued, in flight, or the fixed fields drifted: replay in full */
        anchor_free(t->claimed);
        t->claimed = NULL;
        t->chain->refused++;
        if (t->chain->refused >= MAX_REFUSED_CONTINUES) {
            disable_chain(t);
            r = ANTH_THREAD_UNTHREADED;
        } else {
            t->force_create = true;
            r = ANTH_THREAD_REPLAY;
        }
    }
    pthread_mutex_unlock(&g_mu);
    free(code);
    return r;
}

bool anth_thread_drop_after_400(anth_thread_turn_t *t, int status, bool context_overflow) {
    if (!t->planned || status != 400 || context_overflow) return false;
    planned_clear(t);
    t->unthreaded = true;
    t->disable_on_success = true;
    return true;
}

void anth_thread_settle(anth_thread_turn_t *t, const char *message_id, const json_t *message, int64_t now) {
    pthread_mutex_lock(&g_mu);
    if (t->disable_on_success) {
        disable_chain(t);
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (!t->planned) { /* an unthreaded request leaves the server's thread as it was */
        pthread_mutex_unlock(&g_mu);
        return;
    }
    if (t->planned_continue || !t->force_create) t->chain->refused = 0;
    anchor_free(t->chain->anchor);
    t->chain->anchor = NULL;
    if (message_id && *message_id && message) {
        anchor_t *a = xcalloc(1, sizeof *a);
        a->message_id = xstrdup(message_id);
        a->messages = t->planned_messages;
        t->planned_messages = NULL;
        json_push(a->messages, json_clone(message));
        a->fingerprint = t->planned_fingerprint;
        t->planned_fingerprint = NULL;
        a->stored_at = now;
        t->chain->anchor = a;
    }
    pthread_mutex_unlock(&g_mu);
    planned_clear(t);
}

void anth_threads_release_session(const char *session_id) {
    if (!session_id) return;
    pthread_mutex_lock(&g_mu);
    for (size_t s = 0; s < g_stores.n; s++) {
        store_t *st = g_stores.v[s];
        for (size_t i = 0; i < st->chains.n;) {
            if (strcmp(st->chains.v[i]->session_id, session_id)) {
                i++;
                continue;
            }
            chain_unref(st->chains.v[i]);
            memmove(&st->chains.v[i], &st->chains.v[i + 1], (st->chains.n - i - 1) * sizeof st->chains.v[0]);
            st->chains.n--;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

void anth_threads_reset(void) {
    pthread_mutex_lock(&g_mu);
    for (size_t s = 0; s < g_stores.n; s++) {
        store_t *st = g_stores.v[s];
        for (size_t i = 0; i < st->chains.n; i++) chain_unref(st->chains.v[i]);
        st->chains.n = 0;
    }
    pthread_mutex_unlock(&g_mu);
}
