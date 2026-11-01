/* evalhost.c - completion handles, typed judgments, bulk judgment batches and the token budget for eval cells
 * (see evalhost.h). */
#define _GNU_SOURCE
#include "coding/evalhost.h"

#include "coding/agents.h"
#include "coding/app.h"
#include "coding/goal.h"
#include "coding/judgment.h"
#include "config/settings.h"
#include "core/fs.h"
#include "tools/jobs.h"

#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#define EVAL_SLOTS 32                         /* EVAL_HANDLE_CONCURRENCY */
#define HANDLE_RETENTION_MS (30 * 60 * 1000)  /* COMPLETION_HANDLE_RETENTION_MS */
#define DRAIN_COALESCE_MS 100
#define PROGRESS_INTERVAL_MS 1000
#define JUDGE_MAX_TOKENS 4096                 /* JUDGMENT_CHAT_MAX_TOKENS */
#define JUDGE_PARSE_RETRIES 2
#define IMAGE_MAX_BYTES (20u * 1024 * 1024)

static const char *STRUCTURED_TOOL = "respond";
static const char *SUBMIT_TOOL = "submit_judgment";

/* ---------- shared helpers ---------- */

static void cv_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mu, int64_t ms) {
    if (ms <= 0) ms = 1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(cv, mu, &ts);
}

/* process-wide ceiling on eval model requests in flight (completion handles, judge, judge_batch items) */
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_slot_cv = PTHREAD_COND_INITIALIZER;
static int g_slots;

static bool cancelled(volatile sig_atomic_t *a, volatile sig_atomic_t *b) { return (a && *a) || (b && *b); }

static bool slot_acquire(volatile sig_atomic_t *c1, volatile sig_atomic_t *c2) {
    pthread_mutex_lock(&g_slot_mu);
    while (g_slots >= EVAL_SLOTS) {
        if (cancelled(c1, c2)) {
            pthread_mutex_unlock(&g_slot_mu);
            return false;
        }
        cv_wait_ms(&g_slot_cv, &g_slot_mu, 50);
    }
    g_slots++;
    pthread_mutex_unlock(&g_slot_mu);
    return true;
}

static void slot_release(void) {
    pthread_mutex_lock(&g_slot_mu);
    g_slots--;
    pthread_cond_signal(&g_slot_cv);
    pthread_mutex_unlock(&g_slot_mu);
}

static char *model_label(const ai_model_t *m) { return xasprintf("%s/%s", m->provider, m->id); }

static const ai_model_t *role_model(const char *role, ai_effort_t *eff, bool *has_eff) {
    char *k = xasprintf("modelRoles.%s", role);
    const char *v = settings_str(k, NULL);
    free(k);
    return v && *v ? app_resolve_model_pattern(v, eff, has_eff) : NULL;
}

/* text of an assistant reply: text blocks joined by one space (+ the forced judgment tool's answer), trimmed */
static char *reply_text(const json_t *msg, const char *tool) {
    sb_t b = SB_INIT;
    json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) {
        const char *type = json_get_str(blk, "type");
        const char *t = NULL;
        if (str_eq(type, "text")) t = json_get_str(blk, "text");
        else if (tool && str_eq(type, "toolCall") && str_eq(json_get_str(blk, "name"), tool))
            t = json_get_str(json_get(blk, "arguments"), "answer");
        if (!t) continue;
        if (b.len) sb_putc(&b, ' ');
        sb_append(&b, t);
    }
    char *r = str_trim(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* one stateless request; returns the owned assistant message */
static json_t *model_request(const ai_model_t *m, const char *key, const char *system, json_t *user_content,
                             json_t *tools, json_t *tool_choice, int64_t max_tokens, bool zero_temp, ai_effort_t effort,
                             bool disable_reasoning, volatile sig_atomic_t *cancel) {
    json_t *sys = json_arr();
    json_push_str(sys, system);
    json_t *msgs = json_arr();
    json_t *u = json_obj();
    json_set_str(u, "role", "user");
    json_set(u, "content", user_content);
    json_set_int(u, "timestamp", now_ms());
    json_push(msgs, u);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs, .tools = tools};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.api_key = key;
    if (max_tokens) o.max_tokens = max_tokens;
    if (zero_temp) o.temperature = 0;
    o.reasoning = effort;
    o.disable_reasoning = disable_reasoning;
    o.tool_choice = tool_choice;
    o.cancel = cancel;
    json_t *r = ai_complete(m, &ctx, &o);
    json_free(sys);
    json_free(msgs);
    return r;
}

static double message_cost(const json_t *msg) { return json_get_num(json_get(json_get(msg, "usage"), "cost"), "total", 0); }

/* ---------- state ---------- */

typedef struct chandle {
    char *id;
    int64_t cell;
    pthread_t th;
    bool joinable;
    volatile sig_atomic_t cancel;
    bool settled;
    int pins;
    int64_t settled_at;
    char *text, *error;
    /* inputs */
    char *prompt, *system;
    json_t *schema;
    const ai_model_t *model;
    ai_effort_t effort;
    struct evalhost *h;
} chandle_t;

typedef struct batch batch_t;

struct evalhost {
    app_t *a;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool closing;
    uint64_t seq;
    VEC(chandle_t *) handles;
    VEC(batch_t *) batches;
};

evalhost_t *evalhost_new(app_t *a) {
    evalhost_t *h = xcalloc(1, sizeof(*h));
    h->a = a;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    return h;
}

/* caller holds mu */
static char *new_id(evalhost_t *h, const char *prefix) {
    return xasprintf("%s-%llx%03llx", prefix, (unsigned long long)now_ms(), (unsigned long long)(++h->seq & 0xfff));
}

/* ---------- completion() ---------- */

static void chandle_free(chandle_t *c) {
    if (c->joinable) pthread_join(c->th, NULL);
    free(c->id);
    free(c->text);
    free(c->error);
    free(c->prompt);
    free(c->system);
    json_free(c->schema);
    free(c);
}

/* first {...} span parsed as JSON (parseJsonPayload) */
static json_t *json_payload(const char *text) {
    char *t = str_trim(text);
    json_t *v = NULL;
    size_t n = strlen(t);
    if (n && t[0] == '{' && t[n - 1] == '}') v = json_parse(t, n, NULL);
    else {
        char *a = strchr(t, '{'), *b = strrchr(t, '}');
        if (a && b && b > a) v = json_parse(a, (size_t)(b - a + 1), NULL);
    }
    free(t);
    return v;
}

static char *run_completion(chandle_t *c, char **err) {
    char *key = app_api_key_for(c->model);
    if (!key) {
        char *l = model_label(c->model);
        *err = xasprintf("completion() has no API key for %s. Configure credentials for this provider or choose another "
                         "tier.", l);
        free(l);
        return NULL;
    }
    json_t *tools = NULL, *choice = NULL;
    if (c->schema) {
        tools = json_arr();
        json_t *t = json_obj();
        json_set_str(t, "name", STRUCTURED_TOOL);
        json_set_str(t, "description", "Return your answer by calling this tool with the requested structured fields.");
        json_set(t, "parameters", json_clone(c->schema));
        json_push(tools, t);
        choice = json_obj();
        json_set_str(choice, "type", "tool");
        json_set_str(choice, "name", STRUCTURED_TOOL);
    }
    json_t *content = json_arr();
    json_t *tb = json_obj();
    json_set_str(tb, "type", "text");
    json_set_str(tb, "text", c->prompt);
    json_push(content, tb);
    json_t *r = model_request(c->model, key, c->system ? c->system : "You are a helpful assistant.", content, tools,
                              choice, 0, false, c->effort, false, &c->cancel);
    free(key);
    json_free(tools);
    json_free(choice);
    const char *stop = json_get_str(r, "stopReason");
    char *text = NULL;
    if (str_eq(stop, "aborted") || c->cancel) {
        *err = xstrdup("completion() request aborted.");
    } else if (str_eq(stop, "error")) {
        const char *m = json_get_str(r, "errorMessage");
        *err = xstrdup(m && *m ? m : "completion() request failed.");
    } else if (c->schema) {
        json_t *value = NULL, *blk;
        json_arr_foreach(json_get_arr(r, "content"), i, blk) {
            if (str_eq(json_get_str(blk, "type"), "toolCall") && str_eq(json_get_str(blk, "name"), STRUCTURED_TOOL) && !value)
                value = json_clone(json_get(blk, "arguments"));
        }
        if (!value) {
            char *t = reply_text(r, NULL);
            if (!*t) *err = xstrdup("completion() returned no structured response.");
            else if (!(value = json_payload(t)))
                *err = xstrdup("completion() did not return a structured response matching the schema.");
            free(t);
        }
        if (value) text = json_dump(value, 0);
        json_free(value);
    } else {
        text = reply_text(r, NULL);
        if (!*text) {
            free(text);
            text = NULL;
            *err = xstrdup("completion() returned no text output.");
        }
    }
    json_free(r);
    return text;
}

static void *completion_thread(void *p) {
    chandle_t *c = p;
    char *err = NULL, *text = NULL;
    if (!slot_acquire(&c->cancel, NULL)) err = xstrdup("completion() request aborted.");
    else {
        text = run_completion(c, &err);
        slot_release();
    }
    evalhost_t *h = c->h;
    pthread_mutex_lock(&h->mu);
    c->text = text;
    c->error = err;
    c->settled = true;
    c->settled_at = mono_ms();
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

/* resolve a completion tier: `default` prefers the session's active model; smol/slow inherit the default role */
static const ai_model_t *tier_model(app_t *a, const char *tier, ai_effort_t *effort) {
    *effort = EFFORT_OFF;
    ai_effort_t e = EFFORT_OFF;
    bool has = false;
    const ai_model_t *m = NULL;
    if (str_eq(tier, "default")) {
        m = a->model ? a->model : role_model("default", NULL, NULL);
        return m;
    }
    m = role_model(tier, &e, &has);
    if (!m) m = role_model("default", NULL, NULL);
    if (!m) m = a->model;
    if (!m || !str_eq(tier, "slow")) return m;
    if (has) *effort = e == EFFORT_OFF ? EFFORT_OFF : model_clamp_effort(m, e);
    else if (m->reasoning) *effort = model_clamp_effort(m, EFFORT_HIGH);
    return m;
}

typedef VEC(chandle_t *) chvec_t;

static void evict_handles_locked(evalhost_t *h, chvec_t *dead) {
    int64_t now = mono_ms();
    for (size_t i = 0; i < h->handles.n;) {
        chandle_t *c = h->handles.v[i];
        if (c->settled && !c->pins && now - c->settled_at > HANDLE_RETENTION_MS) {
            vec_push(dead, c);
            memmove(h->handles.v + i, h->handles.v + i + 1, (h->handles.n - i - 1) * sizeof(*h->handles.v));
            h->handles.n--;
        } else {
            i++;
        }
    }
}

static json_t *op_completion(evalhost_t *h, const json_t *args, int64_t cell, char **err) {
    const char *prompt = json_get_str(args, "prompt");
    json_t *tier_j = json_get(args, "model"), *sys_j = json_get(args, "system"), *schema = json_get(args, "schema");
    const char *tier = json_sv(tier_j);
    const char *bad = NULL;
    if (!json_is_obj(args)) bad = "expected an object";
    else if (!prompt || !*prompt) bad = "prompt must be a non-empty string";
    else if (tier_j && !(str_eq(tier, "smol") || str_eq(tier, "default") || str_eq(tier, "slow")))
        bad = "model must be \"smol\", \"default\" or \"slow\"";
    else if (sys_j && !json_is_str(sys_j)) bad = "system must be a string";
    else if (schema && !json_is_obj(schema)) bad = "schema must be an object";
    if (bad) {
        *err = xasprintf("completion() received invalid arguments: %s", bad);
        return NULL;
    }
    if (!tier) tier = "default";
    ai_effort_t eff;
    const ai_model_t *m = tier_model(h->a, tier, &eff);
    if (!m) {
        *err = xasprintf("completion() could not resolve a model for the \"%s\" tier. Configure modelRoles.%s or ensure a "
                         "provider is available.", tier, tier);
        return NULL;
    }
    chandle_t *c = xcalloc(1, sizeof(*c));
    c->h = h;
    c->cell = cell;
    c->prompt = xstrdup(prompt);
    c->system = json_sv(sys_j) ? xstrdup(json_sv(sys_j)) : NULL;
    c->schema = schema ? json_clone(schema) : NULL;
    c->model = m;
    c->effort = eff;
    chvec_t dead = {0};
    pthread_mutex_lock(&h->mu);
    evict_handles_locked(h, &dead);
    c->id = new_id(h, "cmp");
    vec_push(&h->handles, c);
    if (pthread_create(&c->th, NULL, completion_thread, c) == 0) {
        c->joinable = true;
    } else {
        c->settled = true;
        c->settled_at = mono_ms();
        c->error = xstrdup("completion() could not start a worker thread.");
    }
    json_t *res = json_obj();
    json_set_str(res, "id", c->id);
    pthread_mutex_unlock(&h->mu);
    for (size_t i = 0; i < dead.n; i++) chandle_free(dead.v[i]);
    vec_free(&dead);
    return res;
}

/* ---------- handles: wait / status / cancel ---------- */

static chandle_t *find_handle_locked(evalhost_t *h, const char *id) {
    for (size_t i = 0; i < h->handles.n; i++)
        if (str_eq(h->handles.v[i]->id, id)) return h->handles.v[i];
    return NULL;
}

/* parse {kind, id}; agent handles never exist here (eval agent() is not offered) */
static chandle_t *resolve_ref_locked(evalhost_t *h, const json_t *ref, char **err) {
    const char *kind = json_get_str(ref, "kind"), *id = json_get_str(ref, "id");
    if (!json_is_obj(ref)) {
        *err = xstrdup("handle must be an object with kind and id");
        return NULL;
    }
    if ((!str_eq(kind, "agent") && !str_eq(kind, "completion")) || !id || !*id) {
        *err = xstrdup("handle must contain kind agent|completion and a non-empty id");
        return NULL;
    }
    if (str_eq(kind, "agent")) {
        *err = xasprintf("unknown agent handle %s; result expired — read agent://%s", id, id);
        return NULL;
    }
    chandle_t *c = find_handle_locked(h, id);
    if (!c) *err = xasprintf("unknown completion handle %s; result expired", id);
    return c;
}

static json_t *snapshot_locked(const chandle_t *c) {
    json_t *s = json_obj();
    json_set_str(s, "kind", "completion");
    json_set_str(s, "id", c->id);
    if (!c->settled) {
        json_set_str(s, "status", "running");
    } else if (c->error) {
        json_set_str(s, "status", c->cancel ? "cancelled" : "failed");
        json_set_str(s, "error", c->error);
    } else {
        json_set_str(s, "status", "completed");
        json_set_str(s, "text", c->text ? c->text : "");
    }
    return s;
}

static json_t *op_wait(evalhost_t *h, const json_t *args, volatile sig_atomic_t *cancel, char **err) {
    json_t *items = json_get_arr(args, "items");
    if (!items) {
        *err = xstrdup("wait() requires an items array");
        return NULL;
    }
    json_t *tm = json_get(args, "timeoutMs");
    if (tm && (!json_is_num(tm) || !isfinite(tm->u.num.d) || tm->u.num.d < 0)) {
        *err = xstrdup("wait() timeoutMs must be a non-negative finite number");
        return NULL;
    }
    int64_t deadline = tm ? mono_ms() + (int64_t)tm->u.num.d : 0;
    pthread_mutex_lock(&h->mu);
    VEC(chandle_t *) hs = {0};
    json_t *ref;
    json_arr_foreach(items, i, ref) {
        chandle_t *c = resolve_ref_locked(h, ref, err);
        if (!c) break;
        c->pins++;
        vec_push(&hs, c);
    }
    bool aborted = false;
    while (!*err) {
        bool all = true;
        for (size_t i = 0; i < hs.n; i++) all = all && hs.v[i]->settled;
        if (all) break;
        if (cancel && *cancel) {
            aborted = true;
            break;
        }
        if (deadline && mono_ms() >= deadline) break;
        cv_wait_ms(&h->cv, &h->mu, 50);
    }
    json_t *res = NULL;
    if (aborted) {
        /* an aborted wait cancels what it waited on */
        for (size_t i = 0; i < hs.n; i++) hs.v[i]->cancel = 1;
        *err = xstrdup("wait() aborted");
    } else if (!*err) {
        res = json_obj();
        json_t *snaps = json_arr();
        for (size_t i = 0; i < hs.n; i++) json_push(snaps, snapshot_locked(hs.v[i]));
        json_set(res, "items", snaps);
    }
    for (size_t i = 0; i < hs.n; i++) hs.v[i]->pins--;
    pthread_mutex_unlock(&h->mu);
    vec_free(&hs);
    return res;
}

static json_t *op_status_cancel(evalhost_t *h, const json_t *args, bool cancel_it, char **err) {
    if (!json_is_obj(args)) {
        *err = xstrdup("handle request requires an item");
        return NULL;
    }
    pthread_mutex_lock(&h->mu);
    chandle_t *c = resolve_ref_locked(h, json_get(args, "item"), err);
    json_t *res = NULL;
    if (c && cancel_it) {
        res = json_obj();
        bool did = !c->settled;
        if (did) c->cancel = 1;
        json_set_bool(res, "cancelled", did);
    } else if (c) {
        res = snapshot_locked(c);
    }
    pthread_mutex_unlock(&h->mu);
    return res;
}

/* ---------- judgments ---------- */

static const char *sniff_image(const unsigned char *d, size_t n) {
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff) return "image/jpeg";
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "image/gif";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    return NULL;
}

static bool image_mime_ok(const char *m) {
    return str_eq(m, "image/png") || str_eq(m, "image/jpeg") || str_eq(m, "image/gif") || str_eq(m, "image/webp");
}

static json_t *image_block(const char *data, const char *mime, const json_t *detail) {
    json_t *b = json_obj();
    json_set_str(b, "type", "image");
    json_set_str(b, "data", data);
    json_set_str(b, "mimeType", mime);
    if (detail) json_set(b, "detail", json_clone(detail));
    return b;
}

/* loadEvalJudgmentImages: local paths or embedded {type:"image", data, mimeType, detail?}; NULL value => no images */
static json_t *load_images(const json_t *v, const char *cwd, char **err) {
    *err = NULL;
    if (!v) return NULL;
    if (!json_is_arr(v)) {
        *err = xstrdup("invalid judgment image source: images must be an array of paths or image attachments");
        return NULL;
    }
    json_t *out = json_arr(), *src;
    json_arr_foreach(v, i, src) {
        if (json_is_str(src)) {
            const char *p = json_sv(src);
            char *q = json_quote(p);
            if (!*p) {
                *err = xasprintf("invalid judgment image source: images[%zu] path must not be empty", i);
            } else {
                char *abs = p[0] == '/' ? xstrdup(p) : p[0] == '~' ? xasprintf("%s%s", getenv("HOME") ? getenv("HOME") : "", p + 1)
                                                                   : xasprintf("%s/%s", cwd, p);
                struct stat st;
                if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) {
                    *err = xasprintf("invalid judgment image source: images[%zu] path %s could not be loaded: %s", i, q,
                                     strerror(errno ? errno : ENOENT));
                } else if ((size_t)st.st_size > IMAGE_MAX_BYTES) {
                    *err = xasprintf("invalid judgment image source: images[%zu] path %s could not be loaded: file is "
                                     "larger than %u bytes", i, q, IMAGE_MAX_BYTES);
                } else {
                    size_t n = 0;
                    char *data = fs_read(abs, &n);
                    const char *mime = data ? sniff_image((const unsigned char *)data, n) : NULL;
                    if (!data) *err = xasprintf("invalid judgment image source: images[%zu] path %s could not be loaded: %s", i, q,
                                                strerror(errno ? errno : EIO));
                    else if (!mime) *err = xasprintf("invalid judgment image source: images[%zu] path %s is not a supported image", i, q);
                    else {
                        char *b64 = base64_encode(data, n);
                        json_push(out, image_block(b64, mime, NULL));
                        free(b64);
                    }
                    free(data);
                }
                free(abs);
            }
            free(q);
        } else {
            const char *data = json_get_str(src, "data"), *mime = json_get_str(src, "mimeType");
            json_t *detail = json_get(src, "detail");
            const char *ds = json_sv(detail);
            bool ok = json_is_obj(src) && str_eq(json_get_str(src, "type"), "image") && data && *data && image_mime_ok(mime) &&
                      (!detail || str_eq(ds, "auto") || str_eq(ds, "low") || str_eq(ds, "high") || str_eq(ds, "original"));
            size_t dn = 0;
            unsigned char *raw = ok ? base64_decode(data, strlen(data), &dn) : NULL;
            if (!raw || !dn) {
                *err = xasprintf("invalid judgment image source: images[%zu] must be a local path or { type: \"image\", data, "
                                 "mimeType, detail? } with valid base64 data, an image MIME type, and detail auto, low, high, "
                                 "or original", i);
            } else {
                json_push(out, image_block(data, mime, detail));
            }
            free(raw);
        }
        if (*err) break;
    }
    if (*err || !json_len(out)) {
        json_free(out);
        return NULL;
    }
    return out;
}

/* judge role chain: modelRoles.judge, then tiny (-> smol -> default), smol (-> default), default, the session model */
static size_t judge_candidates(app_t *a, const ai_model_t **out, size_t max) {
    const ai_model_t *c[6] = {0};
    c[0] = role_model("judge", NULL, NULL);
    const ai_model_t *dflt = role_model("default", NULL, NULL), *smol = role_model("smol", NULL, NULL);
    const ai_model_t *tiny = role_model("tiny", NULL, NULL);
    c[1] = tiny ? tiny : smol ? smol : dflt;
    c[2] = smol ? smol : dflt;
    c[3] = dflt;
    c[4] = a->model;
    size_t n = 0;
    for (size_t i = 0; i < 5 && n < max; i++) {
        if (!c[i]) continue;
        bool dup = false;
        for (size_t k = 0; k < n; k++) dup = dup || out[k] == c[i];
        if (!dup) out[n++] = c[i];
    }
    return n;
}

typedef struct {
    json_t *answers; /* owned */
    char *model;     /* owned provider/id */
} verdict_t;

/* answer every question about one state through the judge chain; false with *err on failure */
static bool run_judge(app_t *a, const json_t *state, const json_t *questions, const json_t *images,
                      volatile sig_atomic_t *c1, volatile sig_atomic_t *c2, double *cost, verdict_t *v, char **err) {
    *err = NULL;
    const ai_model_t *cands[6];
    size_t n = judge_candidates(a, cands, 6);
    bool want_images = json_len(images) > 0, compatible = false, unsupported = false;
    char *system = NULL, *user = NULL;
    judgment_render_prompt(state, questions, &system, &user);
    char *last_failure = NULL, *last_unavailable = NULL;
    bool multi = json_len(questions) > 1, done = false;
    for (size_t ci = 0; ci < n && !done && !*err; ci++) {
        const ai_model_t *m = cands[ci];
        if (cancelled(c1, c2)) {
            *err = xstrdup("judgment aborted");
            break;
        }
        char *label = model_label(m);
        char *key = app_api_key_for(m);
        if (!key) {
            free(last_unavailable);
            last_unavailable = xasprintf("no API key for %s", label);
            free(label);
            continue;
        }
        if (want_images && !m->input_image) {
            unsupported = true;
            free(last_unavailable);
            last_unavailable = xasprintf("%s does not support image inputs", label);
            free(label);
            free(key);
            continue;
        }
        compatible = true;
        for (int attempt = 0; attempt <= JUDGE_PARSE_RETRIES; attempt++) {
            char *sys = attempt ? judgment_retry_system(system, multi) : xstrdup(system);
            json_t *content;
            if (want_images) {
                content = json_arr();
                json_t *tb = json_obj();
                json_set_str(tb, "type", "text");
                json_set_str(tb, "text", user);
                json_push(content, tb);
                json_t *img;
                json_arr_foreach(images, k, img) json_push(content, json_clone(img));
            } else {
                content = json_str(user);
            }
            json_t *tools = NULL, *choice = NULL;
            if (attempt) {
                tools = json_parse_str("[{\"name\":\"submit_judgment\",\"description\":\"Submit the exact requested answer label,"
                                       " or the requested question-id lines for a batched judgment.\",\"parameters\":{\"type\":"
                                       "\"object\",\"properties\":{\"answer\":{\"type\":\"string\"}},\"required\":[\"answer\"],"
                                       "\"additionalProperties\":false}}]");
                choice = json_obj();
                json_set_str(choice, "type", "tool");
                json_set_str(choice, "name", SUBMIT_TOOL);
            }
            volatile sig_atomic_t *cf = c1 ? c1 : c2;
            json_t *r = model_request(m, key, sys, content, tools, choice, JUDGE_MAX_TOKENS, true, EFFORT_OFF, true, cf);
            free(sys);
            json_free(tools);
            json_free(choice);
            *cost += message_cost(r);
            const char *stop = json_get_str(r, "stopReason");
            if (str_eq(stop, "aborted") || cancelled(c1, c2)) {
                *err = xstrdup("judgment aborted");
                json_free(r);
                break;
            }
            if (str_eq(stop, "error")) {
                const char *em = json_get_str(r, "errorMessage");
                free(last_failure);
                last_failure = xstrdup(em && *em ? em : "unknown error");
                json_free(r);
                break; /* next candidate */
            }
            char *text = reply_text(r, attempt ? SUBMIT_TOOL : NULL);
            json_free(r);
            char *perr = NULL;
            json_t *ans = judgment_parse_reply(questions, text, &perr);
            free(text);
            if (ans) {
                v->answers = ans;
                v->model = xstrdup(label);
                done = true;
                break;
            }
            free(last_failure);
            last_failure = perr;
        }
        free(label);
        free(key);
    }
    if (!done && !*err) {
        if (want_images && !compatible && unsupported) *err = xstrdup("judgment: no compatible judge candidate supports images");
        else if (n == 0) *err = xstrdup(want_images ? "judgment: no compatible judge candidate supports images"
                                                    : "judgment: no judge model available");
        else *err = xasprintf("judgment: every judge candidate failed: %s",
                              last_failure ? last_failure : last_unavailable ? last_unavailable : "unknown error");
    }
    free(last_failure);
    free(last_unavailable);
    free(system);
    free(user);
    return done;
}

static json_t *op_judge(evalhost_t *h, const json_t *args, volatile sig_atomic_t *cancel, char **err) {
    if (!json_is_obj(args)) {
        *err = xstrdup("judge() received invalid arguments: expected { state, questions }");
        return NULL;
    }
    const json_t *state = json_get(args, "state");
    if (!judgment_check_state(state, err)) return NULL;
    json_t *qs = judgment_parse_questions(json_get(args, "questions"), err);
    if (!qs) return NULL;
    json_t *res = NULL;
    if (!slot_acquire(cancel, NULL)) {
        *err = xstrdup("judgment aborted");
    } else {
        json_t *images = load_images(json_get(args, "images"), h->a->cwd, err);
        double cost = 0;
        verdict_t v = {0};
        if (!*err && run_judge(h->a, state, qs, images, cancel, NULL, &cost, &v, err)) {
            res = json_obj();
            json_set(res, "answers", v.answers);
            json_set_str(res, "model", v.model);
            free(v.model);
        }
        json_free(images);
        slot_release();
    }
    json_free(qs);
    return res;
}

/* ---------- judge_batch() ---------- */

struct batch {
    int refs; /* atomic */
    evalhost_t *h;
    app_t *a;
    char *id, *intent;
    size_t total;
    json_t *inputs;    /* [{key, state, images?, imageError?}] */
    json_t *questions; /* normalized */
    int concurrency, retries, min_ok;
    volatile sig_atomic_t cancel;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    json_t *settled; /* [{key, answers|error, model?}] in settle order */
    size_t cursor, next;
    int failed;
    double cost;
    char *model, *error;
    bool running, was_cancelled;
    int64_t started;
    int64_t last_progress;
    bgjob_t *job; /* while the job body runs */
    pthread_t th; /* fallback runner when no job manager */
    bool own_thread;
};

static batch_t *batch_ref(batch_t *b) {
    __atomic_add_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
    return b;
}

static void batch_unref(batch_t *b) {
    if (!b || __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST) != 0) return;
    if (b->own_thread) pthread_join(b->th, NULL);
    free(b->id);
    free(b->intent);
    json_free(b->inputs);
    json_free(b->questions);
    json_free(b->settled);
    free(b->model);
    free(b->error);
    pthread_mutex_destroy(&b->mu);
    pthread_cond_destroy(&b->cv);
    free(b);
}

static void batch_unref_v(void *p) { batch_unref(p); }

/* JavaScript String(key) */
static char *key_text(const json_t *k) { return json_is_str(k) ? xstrdup(json_sv(k)) : json_dump(k, 0); }

static char *fmt_secs(int64_t ms) {
    double s = round((double)ms / 100.0) / 10.0;
    return s == floor(s) ? xasprintf("%.0f", s) : xasprintf("%.1f", s);
}

static char *fmt_cost(double c) { return xasprintf(c < 0.01 ? "$%.4f" : c < 1 ? "$%.3f" : "$%.2f", c); }

/* caller holds b->mu */
static json_t *batch_status_locked(batch_t *b) {
    json_t *s = json_obj();
    json_set_str(s, "id", b->id);
    json_set_str(s, "intent", b->intent);
    json_set_int(s, "total", (int64_t)b->total);
    json_set_int(s, "done", (int64_t)json_len(b->settled));
    json_set_int(s, "failed", b->failed);
    json_set_num(s, "cost", b->cost);
    json_set_bool(s, "running", b->running);
    if (b->model) json_set_str(s, "model", b->model);
    char *e = fmt_secs(now_ms() - b->started);
    json_set(s, "elapsedS", json_num(strtod(e, NULL)));
    free(e);
    if (b->error) json_set_str(s, "error", b->error);
    return s;
}

/* caller holds b->mu */
static char *batch_summary_locked(batch_t *b) {
    sb_t s = SB_INIT;
    sb_appendf(&s, "%s %zu/%zu", b->running ? "judging" : b->was_cancelled ? "cancelled" : "judged", json_len(b->settled),
               b->total);
    if (b->failed) sb_appendf(&s, " · %d failed", b->failed);
    if (b->cost > 0) {
        char *c = fmt_cost(b->cost);
        sb_appendf(&s, " · %s", c);
        free(c);
    }
    if (b->model) sb_appendf(&s, " · %s", b->model);
    char *e = fmt_secs(now_ms() - b->started);
    sb_appendf(&s, " · %ss", e);
    free(e);
    return sb_detach(&s);
}

static void batch_settle(batch_t *b, json_t *item) {
    pthread_mutex_lock(&b->mu);
    if (json_get(item, "error")) b->failed++;
    const char *m = json_get_str(item, "model");
    if (m) {
        free(b->model);
        b->model = xstrdup(m);
    }
    json_push(b->settled, item);
    pthread_cond_broadcast(&b->cv);
    bool report = b->job && now_ms() - b->last_progress >= PROGRESS_INTERVAL_MS;
    char *summary = report ? batch_summary_locked(b) : NULL;
    if (report) b->last_progress = now_ms();
    bgjob_t *job = b->job;
    pthread_mutex_unlock(&b->mu);
    if (summary) job_set_output(job, summary);
    free(summary);
}

static json_t *batch_item_error(const json_t *key, const char *msg) {
    json_t *it = json_obj();
    json_set(it, "key", json_clone(key));
    json_set_str(it, "error", msg);
    return it;
}

static json_t *batch_judge_item(batch_t *b, const json_t *input) {
    const json_t *key = json_get(input, "key");
    if (b->cancel) return batch_item_error(key, "cancelled");
    const char *ie = json_get_str(input, "imageError");
    if (ie) return batch_item_error(key, ie);
    if (!slot_acquire(&b->cancel, NULL)) return batch_item_error(key, "cancelled");
    json_t *res = NULL;
    char *err = NULL;
    json_t *images = load_images(json_get(input, "images"), b->a->cwd, &err);
    if (err) res = batch_item_error(key, err);
    for (int attempt = 0; !res && attempt <= b->retries; attempt++) {
        if (b->cancel) break;
        free(err);
        err = NULL;
        double cost = 0;
        verdict_t v = {0};
        bool ok = run_judge(b->a, json_get(input, "state"), b->questions, images, &b->cancel, NULL, &cost, &v, &err);
        pthread_mutex_lock(&b->mu);
        b->cost += cost;
        pthread_mutex_unlock(&b->mu);
        if (ok) {
            res = json_obj();
            json_set(res, "key", json_clone(key));
            json_set(res, "answers", v.answers);
            json_set_str(res, "model", v.model);
            free(v.model);
        }
    }
    if (!res) res = batch_item_error(key, b->cancel ? "cancelled" : err ? err : "unknown error");
    free(err);
    json_free(images);
    slot_release();
    return res;
}

static void *batch_worker(void *p) {
    batch_t *b = p;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        size_t i = b->next < b->total ? b->next++ : b->total;
        pthread_mutex_unlock(&b->mu);
        if (i >= b->total) break;
        const json_t *input = json_at(b->inputs, i);
        json_t *item = b->cancel ? batch_item_error(json_get(input, "key"), "cancelled") : batch_judge_item(b, input);
        batch_settle(b, item);
    }
    return NULL;
}

/* fan out, wait for every item; returns the summary and sets *failed when the run died wholesale */
static char *batch_run(batch_t *b, volatile sig_atomic_t *job_cancel, bool *failed) {
    size_t nw = (size_t)b->concurrency < b->total ? (size_t)b->concurrency : b->total;
    pthread_t *ths = xcalloc(nw ? nw : 1, sizeof(pthread_t));
    size_t started = 0;
    for (size_t i = 0; i < nw; i++)
        if (pthread_create(&ths[i], NULL, batch_worker, b) == 0) started++;
        else break;
    if (!started) batch_worker(b); /* no threads: judge inline */
    /* propagate job cancellation (proc:// kill, `wait` cancel, session end) into the run */
    pthread_mutex_lock(&b->mu);
    while (json_len(b->settled) < b->total) {
        if (job_cancel && *job_cancel) b->cancel = 1;
        cv_wait_ms(&b->cv, &b->mu, 50);
    }
    pthread_mutex_unlock(&b->mu);
    for (size_t i = 0; i < started; i++) pthread_join(ths[i], NULL);
    free(ths);
    pthread_mutex_lock(&b->mu);
    int ok = (int)json_len(b->settled) - b->failed;
    if (b->cancel) {
        b->was_cancelled = true;
        b->error = xstrdup("judge_batch cancelled");
    } else if (ok < b->min_ok) {
        const char *last = NULL;
        for (size_t i = json_len(b->settled); i > 0 && !last; i--) {
            const char *e = json_get_str(json_at(b->settled, i - 1), "error");
            if (e && !str_eq(e, "cancelled")) last = e;
        }
        b->error = xasprintf("judge_batch: only %d/%zu item(s) judged (min_ok=%d); last error: %s", ok, b->total, b->min_ok,
                             last ? last : "unknown");
    }
    b->running = false;
    char *summary = batch_summary_locked(b);
    char *res = b->error ? xstrdup(b->error) : summary;
    if (b->error) free(summary);
    *failed = b->error != NULL;
    b->job = NULL;
    pthread_cond_broadcast(&b->cv);
    pthread_mutex_unlock(&b->mu);
    return res;
}

static char *batch_job_body(bgjob_t *job, void *ud, bool *failed) {
    batch_t *b = ud;
    pthread_mutex_lock(&b->mu);
    b->job = job;
    pthread_mutex_unlock(&b->mu);
    return batch_run(b, job_cancel_flag(job), failed);
}

static void *batch_thread(void *p) {
    bool failed;
    free(batch_run(p, NULL, &failed));
    return NULL;
}

static char *batch_invalid(const char *detail) { return xasprintf("judge_batch() received invalid arguments: %s", detail); }

static bool batch_key_ok(const json_t *k) {
    return (json_is_str(k) && json_len(k) > 0) || (json_is_num(k) && isfinite(k->u.num.d));
}

/* parseCount: non-negative integer, capped */
static bool parse_count(const json_t *args, const char *name, int def, int max, int *out, char **err) {
    json_t *v = json_get(args, name);
    if (!v) {
        *out = def;
        return true;
    }
    if (!json_is_num(v) || !v->is_int || v->u.num.i < 0) {
        char *d = xasprintf("%s must be a non-negative integer", name);
        *err = batch_invalid(d);
        free(d);
        return false;
    }
    *out = v->u.num.i > max ? max : (int)v->u.num.i;
    return true;
}

static batch_t *find_batch_locked(evalhost_t *h, const char *id) {
    for (size_t i = 0; i < h->batches.n; i++)
        if (str_eq(h->batches.v[i]->id, id)) return h->batches.v[i];
    return NULL;
}

static json_t *batch_create(evalhost_t *h, const json_t *args, char **err) {
    if (json_get(args, "images")) {
        *err = batch_invalid("images must be provided on each item");
        return NULL;
    }
    json_t *items = json_get(args, "items");
    if (!json_is_arr(items)) {
        *err = batch_invalid("items must be an array of { key, state }");
        return NULL;
    }
    if (!json_len(items)) {
        *err = batch_invalid("items must not be empty");
        return NULL;
    }
    json_t *inputs = json_arr(), *it;
    json_arr_foreach(items, i, it) {
        json_t *key = json_get(it, "key");
        if (!json_is_obj(it) || !batch_key_ok(key)) {
            char *d = xasprintf("item %zu needs a string or number key", i);
            *err = batch_invalid(d);
            free(d);
            break;
        }
        for (size_t k = 0; k < json_len(inputs) && !*err; k++) {
            if (json_equal(json_get(json_at(inputs, k), "key"), key)) {
                char *q = json_dump(key, 0);
                char *d = xasprintf("duplicate item key %s", q);
                *err = batch_invalid(d);
                free(d);
                free(q);
            }
        }
        if (*err) break;
        if (!judgment_check_state(json_get(it, "state"), err)) break;
        json_t *in = json_obj();
        json_set(in, "key", json_clone(key));
        json_set(in, "state", json_clone(json_get(it, "state")));
        json_t *imgs = json_get(it, "images");
        if (imgs && json_is_arr(imgs)) json_set(in, "images", json_clone(imgs));
        else if (imgs) {
            char *kt = key_text(key);
            char *q = json_quote(kt);
            json_set(in, "imageError", json_str_own(xasprintf("images for input %s must be an array of paths or image "
                                                              "attachments", q)));
            free(q);
            free(kt);
        }
        json_push(inputs, in);
    }
    json_t *qs = *err ? NULL : judgment_parse_questions(json_get(args, "questions"), err);
    int conc = 0, retries = 0, min_ok = 0;
    if (!*err) parse_count(args, "concurrency", EVAL_SLOTS, EVAL_SLOTS, &conc, err);
    if (!*err) parse_count(args, "retries", 1, 1000, &retries, err);
    if (!*err) parse_count(args, "minOk", 1, (int)json_len(inputs), &min_ok, err);
    json_t *intent_j = json_get(args, "intent");
    char *intent_trim = json_sv(intent_j) ? str_trim(json_sv(intent_j)) : NULL;
    if (!*err && intent_j && (!intent_trim || !*intent_trim)) *err = batch_invalid("intent must be a non-empty string");
    free(intent_trim);
    if (*err) {
        json_free(inputs);
        json_free(qs);
        return NULL;
    }
    batch_t *b = xcalloc(1, sizeof(*b));
    b->refs = 1; /* registry */
    b->h = h;
    b->a = h->a;
    b->intent = xstrdup(json_sv(intent_j) ? json_sv(intent_j) : "Judging");
    b->total = json_len(inputs);
    b->inputs = inputs;
    b->questions = qs;
    b->concurrency = conc > 0 ? conc : EVAL_SLOTS;
    b->retries = retries;
    b->min_ok = min_ok;
    b->settled = json_arr();
    b->running = true;
    b->started = now_ms();
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    pthread_mutex_lock(&h->mu);
    b->id = new_id(h, "jdgb");
    vec_push(&h->batches, b);
    pthread_mutex_unlock(&h->mu);
    jobs_t *jobs = h->a->env ? h->a->env->jobs : NULL;
    bool started = false;
    if (jobs) {
        /* the batch is an async job under its own id: completion auto-delivers its summary */
        char *jid = jobs_start(jobs, "eval", b->id, b->intent, NULL, false, batch_job_body, batch_ref(b), batch_unref_v);
        started = jid != NULL;
        free(jid);
    }
    if (!started) {
        /* no job manager: run on a thread owned by the batch (the last unref joins it) */
        if (pthread_create(&b->th, NULL, batch_thread, b) == 0) b->own_thread = true;
        else batch_thread(b);
    }
    pthread_mutex_lock(&b->mu);
    json_t *st = batch_status_locked(b);
    pthread_mutex_unlock(&b->mu);
    return st;
}

static json_t *batch_drain(batch_t *b, const json_t *args, volatile sig_atomic_t *cancel, char **err) {
    json_t *tm = json_get(args, "timeoutMs");
    if (tm && (!json_is_num(tm) || !isfinite(tm->u.num.d) || tm->u.num.d < 0)) {
        *err = batch_invalid("timeoutMs must be a non-negative finite number");
        return NULL;
    }
    int64_t deadline = tm ? mono_ms() + (int64_t)tm->u.num.d : INT64_MAX;
    pthread_mutex_lock(&b->mu);
    if (b->cursor == json_len(b->settled) && b->running) {
        while (b->cursor == json_len(b->settled) && b->running && mono_ms() < deadline && !(cancel && *cancel))
            cv_wait_ms(&b->cv, &b->mu, deadline == INT64_MAX ? 100 : MIN(100, deadline - mono_ms()));
        /* hold briefly for near-simultaneous settles */
        int64_t linger_end = MIN(mono_ms() + DRAIN_COALESCE_MS, deadline);
        while (b->running && b->cursor < json_len(b->settled) && json_len(b->settled) < b->total && mono_ms() < linger_end &&
               !(cancel && *cancel))
            cv_wait_ms(&b->cv, &b->mu, linger_end - mono_ms());
    }
    json_t *res = NULL;
    if (cancel && *cancel) {
        *err = xstrdup("judge_batch drain aborted");
    } else if (b->cursor < json_len(b->settled)) {
        json_t *items = json_arr();
        for (size_t i = b->cursor; i < json_len(b->settled); i++) json_push(items, json_clone(json_at(b->settled, i)));
        b->cursor = json_len(b->settled);
        res = json_obj();
        json_set(res, "items", items);
    } else if (!b->running && b->error) {
        *err = xstrdup(b->error);
    } else {
        res = json_obj();
        json_set(res, "items", json_arr());
    }
    pthread_mutex_unlock(&b->mu);
    return res;
}

static bool batch_cancel(evalhost_t *h, batch_t *b) {
    pthread_mutex_lock(&b->mu);
    bool did = b->running && !b->cancel;
    if (did) b->cancel = 1;
    pthread_mutex_unlock(&b->mu);
    if (did && h->a->env && h->a->env->jobs) jobs_cancel(h->a->env->jobs, b->id);
    return did;
}

static json_t *op_judge_batch(evalhost_t *h, const json_t *args, volatile sig_atomic_t *cancel, char **err) {
    const char *op = json_get_str(args, "op");
    if (!json_is_obj(args) || !op) {
        *err = batch_invalid("expected { op, … }");
        return NULL;
    }
    if (str_eq(op, "create")) return batch_create(h, args, err);
    static const char *const OPS[] = {"attach", "status", "drain", "results", "failed", "cancel", "close", NULL};
    bool known = false;
    for (int i = 0; OPS[i]; i++) known = known || str_eq(op, OPS[i]);
    if (!known) {
        char *d = xasprintf("unknown op \"%s\"", op);
        *err = batch_invalid(d);
        free(d);
        return NULL;
    }
    const char *id = json_get_str(args, "id");
    if (!id || !*id) {
        *err = batch_invalid("op requires a batch id");
        return NULL;
    }
    pthread_mutex_lock(&h->mu);
    batch_t *b = find_batch_locked(h, id);
    if (b) batch_ref(b);
    pthread_mutex_unlock(&h->mu);
    if (!b) {
        *err = xasprintf("unknown judge_batch \"%s\"", id);
        return NULL;
    }
    json_t *res = NULL;
    if (str_eq(op, "attach") || str_eq(op, "status")) {
        pthread_mutex_lock(&b->mu);
        res = batch_status_locked(b);
        pthread_mutex_unlock(&b->mu);
    } else if (str_eq(op, "drain")) {
        res = batch_drain(b, args, cancel, err);
    } else if (str_eq(op, "results") || str_eq(op, "failed")) {
        bool want_ok = str_eq(op, "results");
        json_t *m = json_obj(), *it;
        pthread_mutex_lock(&b->mu);
        json_arr_foreach(b->settled, i, it) {
            json_t *v = json_get(it, want_ok ? "answers" : "error");
            if (!v) continue;
            char *k = key_text(json_get(it, "key"));
            json_set(m, k, json_clone(v));
            free(k);
        }
        pthread_mutex_unlock(&b->mu);
        res = json_obj();
        json_set(res, op, m);
    } else if (str_eq(op, "cancel")) {
        res = json_obj();
        json_set_bool(res, "cancelled", batch_cancel(h, b));
    } else { /* close */
        batch_cancel(h, b);
        pthread_mutex_lock(&h->mu);
        bool removed = false;
        for (size_t i = 0; i < h->batches.n; i++) {
            if (h->batches.v[i] != b) continue;
            memmove(h->batches.v + i, h->batches.v + i + 1, (h->batches.n - i - 1) * sizeof(*h->batches.v));
            h->batches.n--;
            removed = true;
            break;
        }
        pthread_mutex_unlock(&h->mu);
        if (removed) batch_unref(b); /* registry reference */
        res = json_obj();
        json_set_bool(res, "closed", true);
    }
    batch_unref(b);
    return res;
}

/* ---------- budget / output / interrupt ---------- */

static json_t *op_budget(evalhost_t *h) {
    app_t *a = h->a;
    json_t *r = json_obj();
    int64_t out = __atomic_load_n(&a->output_tokens, __ATOMIC_SEQ_CST);
    int64_t turn_spent = out - a->turn_output_baseline;
    if (turn_spent < 0) turn_spent = 0;
    const goal_t *g = app_goal(a);
    if (a->turn_budget_total > 0) {
        json_set_int(r, "total", a->turn_budget_total);
        json_set_int(r, "spent", turn_spent);
        json_set_bool(r, "hard", a->turn_budget_hard);
    } else if (g && goal_enabled(g)) {
        int64_t budget = goal_budget(g);
        if (budget > 0) json_set_int(r, "total", budget);
        else json_set(r, "total", json_null());
        json_set_int(r, "spent", goal_used(g));
        json_set_bool(r, "hard", budget > 0);
    } else {
        json_set(r, "total", json_null());
        json_set_int(r, "spent", turn_spent);
        json_set_bool(r, "hard", false);
    }
    return r;
}

static json_t *op_output(const json_t *args, char **err) {
    const char *id = json_get_str(args, "id");
    if (!id || !*id) {
        *err = xstrdup("output() requires an id");
        return NULL;
    }
    char *e = NULL;
    char *txt = agents_read_output(id, &e);
    if (!txt) {
        *err = e ? e : xasprintf("Output not found: %s", id);
        return NULL;
    }
    free(e);
    json_t *r = json_obj();
    json_set(r, "content", json_str_own(txt));
    return r;
}

static json_t *op_interrupt(evalhost_t *h, const json_t *args) {
    int64_t cell = json_get_int(args, "cell", -1);
    pthread_mutex_lock(&h->mu);
    for (size_t i = 0; i < h->handles.n; i++)
        if (h->handles.v[i]->cell == cell && !h->handles.v[i]->settled) h->handles.v[i]->cancel = 1;
    pthread_mutex_unlock(&h->mu);
    return json_obj();
}

json_t *evalhost_call(evalhost_t *h, const char *name, const json_t *args, int64_t cell, volatile sig_atomic_t *cancel,
                      char **err) {
    *err = NULL;
    pthread_mutex_lock(&h->mu);
    bool closing = h->closing;
    pthread_mutex_unlock(&h->mu);
    if (closing) {
        *err = xstrdup("eval host is shutting down");
        return NULL;
    }
    if (str_eq(name, "__completion__")) return op_completion(h, args, cell, err);
    if (str_eq(name, "__wait__")) return op_wait(h, args, cancel, err);
    if (str_eq(name, "__status__")) return op_status_cancel(h, args, false, err);
    if (str_eq(name, "__cancel__")) return op_status_cancel(h, args, true, err);
    if (str_eq(name, "__judge__")) return op_judge(h, args, cancel, err);
    if (str_eq(name, "__judge_batch__")) return op_judge_batch(h, args, cancel, err);
    if (str_eq(name, "__budget__")) return op_budget(h);
    if (str_eq(name, "__output__")) return op_output(args, err);
    if (str_eq(name, "__interrupt__")) return op_interrupt(h, args);
    *err = xasprintf("unknown eval bridge call %s", name);
    return NULL;
}

void evalhost_shutdown(evalhost_t *h) {
    if (!h) return;
    pthread_mutex_lock(&h->mu);
    h->closing = true;
    for (size_t i = 0; i < h->handles.n; i++) h->handles.v[i]->cancel = 1;
    for (size_t i = 0; i < h->batches.n; i++) h->batches.v[i]->cancel = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

void evalhost_free(evalhost_t *h) {
    if (!h) return;
    evalhost_shutdown(h);
    for (size_t i = 0; i < h->handles.n; i++) chandle_free(h->handles.v[i]);
    vec_free(&h->handles);
    for (size_t i = 0; i < h->batches.n; i++) batch_unref(h->batches.v[i]);
    vec_free(&h->batches);
    pthread_mutex_destroy(&h->mu);
    pthread_cond_destroy(&h->cv);
    free(h);
}
