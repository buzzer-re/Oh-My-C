/* skilldesc.c - compressed skill descriptions (see skilldesc.h). */
#include "skilldesc.h"

#include "coding/app.h"
#include "coding/costgate.h"
#include "config/settings.h"
#include "core/fs.h"
#include "core/json.h"
#include "prompt/template.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <pthread.h>

#define MAX_PREVIEW_CHARS 100
#define MIN_PREVIEW_CUT 40
#define MAX_COMPRESSED_CHARS 160
/* revision of skilldesc_valid's rules, part of every unusable-answer marker: answers an older, stricter revision
 * rejected are asked for again */
#define VALIDATION_REVISION 2
#define MAX_PARALLEL 4

#define PROMPT_PATH "coding-agent/prompts/skills/compress-description.md"

/* ---------- process-wide store ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static json_t *g_store;   /* key -> description */
static strvec_t g_inflight;
static int g_running;

static char *store_path(void) { return omc_agent_path("skill-descriptions.json"); }

static void store_load_locked(void) {
    if (g_store) return;
    char *p = store_path();
    size_t n = 0;
    char *txt = fs_read_limit(p, 8u << 20, &n, NULL);
    free(p);
    g_store = txt ? json_parse(txt, n, NULL) : NULL;
    free(txt);
    if (!json_is_obj(g_store)) {
        json_free(g_store);
        g_store = json_obj();
    }
}

static void store_put(const char *key, const char *value) {
    pthread_mutex_lock(&g_mu);
    store_load_locked();
    json_set_str(g_store, key, value);
    char *dump = json_dump(g_store, 1);
    char *p = store_path();
    char *dir = path_dirname(p);
    fs_mkdir_p(dir, 0700);
    fs_write_atomic(p, dump, strlen(dump), 0600);
    free(dir);
    free(p);
    free(dump);
    pthread_mutex_unlock(&g_mu);
}

static char *store_get(const char *key) {
    pthread_mutex_lock(&g_mu);
    store_load_locked();
    const char *v = json_get_str(g_store, key);
    char *r = v ? xstrdup(v) : NULL;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* sha256(prompt template \0 name \0 description) as hex, like upstream's keyFor */
static char *key_for(const char *name, const char *description) {
    char *tpl = prompt_load(PROMPT_PATH);
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned char md[32];
    unsigned int mdl = 0;
    EVP_DigestInit_ex(c, EVP_sha256(), NULL);
    EVP_DigestUpdate(c, tpl ? tpl : "", tpl ? strlen(tpl) : 0);
    EVP_DigestUpdate(c, "\0", 1);
    EVP_DigestUpdate(c, name, strlen(name));
    EVP_DigestUpdate(c, "\0", 1);
    EVP_DigestUpdate(c, description, strlen(description));
    EVP_DigestFinal_ex(c, md, &mdl);
    EVP_MD_CTX_free(c);
    free(tpl);
    char *hex = xmalloc(65);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", md[i]);
    return hex;
}

/* byte offset of the n-th code point / code points within the first len bytes */
static size_t utf8_offset(const char *s, size_t n) {
    size_t i = 0, cps = 0;
    while (s[i] && cps < n) {
        i++;
        while (((unsigned char)s[i] & 0xC0) == 0x80) i++;
        cps++;
    }
    return i;
}

static size_t utf8_strlen_n(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len && s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

char *skilldesc_preview(const char *description) {
    /* collapse whitespace */
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = description ? description : ""; *p; p++) {
        if (isspace((unsigned char)*p)) sp = b.len > 0;
        else {
            if (sp) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    char *text = sb_detach(&b);
    size_t chars = utf8_strlen(text);
    if (chars <= MAX_PREVIEW_CHARS) return text;
    /* boundary = first MAX_PREVIEW_CHARS-1 characters */
    size_t off = utf8_offset(text, MAX_PREVIEW_CHARS - 1);
    char *boundary = xstrndup(text, off);
    free(text);
    /* first sentence ending in .!? followed by space/end, if long enough */
    for (char *p = boundary; *p; p++) {
        if (strchr(".!?", *p) && (p[1] == ' ' || !p[1])) {
            size_t len = (size_t)(p - boundary) + 1;
            if (utf8_strlen_n(boundary, len) >= MIN_PREVIEW_CUT) {
                boundary[len] = 0;
                return boundary;
            }
            break;
        }
    }
    char *space = strrchr(boundary, ' ');
    if (space && utf8_strlen_n(boundary, (size_t)(space - boundary)) >= MIN_PREVIEW_CUT) {
        while (space > boundary && space[-1] == ' ') space--;
        *space = 0;
    }
    char *r = xasprintf("%s…", boundary);
    free(boundary);
    return r;
}

/* the prompt's 12-word target stays advisory: small models overshoot it by a few words while staying well inside the
 * character budget, which is what bounds the prompt */
char *skilldesc_valid(const char *text) {
    char *line = str_trim(text ? text : "");
    if (!*line || strpbrk(line, "\r\n") || utf8_strlen(line) > MAX_COMPRESSED_CHARS) {
        free(line);
        return NULL;
    }
    return line;
}

/* ---------- background compression ---------- */

/* skill-descriptions.ts select(): the smol role chain's first candidate over the available chat
 * models (configured smol, the inherited default, then the built-in chain), else a configured tiny;
 * never the session's foreground model */
static const ai_model_t *pick_model(void) {
    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    mr_chain_t chain;
    mr_role_chain("smol", &avail, &chain);
    const ai_model_t *m = chain.n ? chain.v[0].model : NULL;
    mr_chain_free(&chain);
    if (!m) m = mr_role_selection("tiny", &avail, NULL);
    mr_pool_free(&avail);
    return m;
}

typedef struct {
    char *key, *name, *description;
} job_t;

/* refuseAttempt: a refusal inside a provider's retry loop aborts the request, so no retry layer treats it as a
 * transient network error. The payload hook runs once per provider attempt, right before it is dispatched. */
static void refuse_attempt(void *ud, const json_t *payload) {
    UNUSED(payload);
    volatile sig_atomic_t *refused = ud;
    if (!costgate_admits()) *refused = 1;
}

static void *compress_thread(void *p) {
    job_t *j = p;
    pthread_mutex_lock(&g_mu);
    while (g_running >= MAX_PARALLEL) pthread_cond_wait(&g_cv, &g_mu);
    g_running++;
    pthread_mutex_unlock(&g_mu);
    const ai_model_t *m = pick_model();
    if (m) {
        char *unusable = xasprintf("%s:v%d:%s/%s", j->key, VALIDATION_REVISION, m->provider, m->id);
        char *seen = store_get(unusable);
        if (!seen && !costgate_admits()) {
            LOGW("skill description compression failed for %s: the session reached its cost cap", j->name);
        } else if (!seen) {
            json_t *c = json_obj();
            json_set_str(c, "name", j->name);
            json_set_str(c, "description", j->description);
            char *req = prompt_render(PROMPT_PATH, c);
            json_free(c);
            json_t *msgs = json_arr();
            /* omp: messages: [{role: "user", content: request}] - a plain string, not text blocks */
            json_t *um = json_obj();
            json_set_str(um, "role", "user");
            json_set(um, "content", json_str_own(req ? req : xstrdup("")));
            json_set_int(um, "timestamp", now_ms());
            json_push(msgs, um);
            ai_context_t ctx = {.messages = msgs};
            ai_stream_opts_t o;
            ai_stream_opts_init(&o);
            char *key = app_api_key_for(m);
            o.api_key = key;
            o.max_tokens = 1024;
            o.disable_reasoning = true;
            o.temperature = 0;
            char *side_sid = uuid_v4(); /* a side request is its own conversation on the wire */
            o.session_id = side_sid;
            volatile sig_atomic_t refused = 0;
            o.cancel = &refused;
            o.on_payload = refuse_attempt;
            o.payload_ud = (void *)&refused;
            json_t *r = key ? ai_complete(m, &ctx, &o) : NULL;
            free(side_sid);
            costgate_record(json_get_num(json_get(json_get(r, "usage"), "cost"), "total", 0));
            const char *stop = json_get_str(r, "stopReason");
            if (refused) {
                LOGW("skill description compression failed for %s: the session reached its cost cap", j->name);
            } else if (str_eq(stop, "stop")) {
                char *txt = ai_message_text(r);
                char *v = skilldesc_valid(txt);
                if (v) store_put(j->key, v);
                else {
                    char *pv = skilldesc_preview(j->description);
                    store_put(unusable, pv);
                    free(pv);
                    LOGW("skill description compression unusable for %s; keeping the preview for this model", j->name);
                }
                free(v);
                free(txt);
            } else if (str_eq(stop, "length")) {
                char *pv = skilldesc_preview(j->description);
                store_put(unusable, pv);
                free(pv);
            } else if (r) {
                LOGW("skill description compression failed for %s: %s", j->name, json_get_str(r, "errorMessage"));
            }
            json_free(r);
            json_free(msgs);
            free(key);
        }
        free(seen);
        free(unusable);
    }
    pthread_mutex_lock(&g_mu);
    g_running--;
    for (size_t i = 0; i < g_inflight.n; i++)
        if (!strcmp(g_inflight.v[i], j->key)) {
            free(g_inflight.v[i]);
            g_inflight.v[i] = g_inflight.v[--g_inflight.n];
            break;
        }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    free(j->key);
    free(j->name);
    free(j->description);
    free(j);
    return NULL;
}

static void schedule(const char *key, const char *name, const char *description) {
    pthread_mutex_lock(&g_mu);
    bool busy = strvec_contains(&g_inflight, key);
    if (!busy) strvec_push(&g_inflight, key);
    pthread_mutex_unlock(&g_mu);
    if (busy) return;
    job_t *j = xcalloc(1, sizeof(*j));
    j->key = xstrdup(key);
    j->name = xstrdup(name);
    j->description = xstrdup(description);
    pthread_t th;
    if (pthread_create(&th, NULL, compress_thread, j) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&g_mu);
        for (size_t i = 0; i < g_inflight.n; i++)
            if (!strcmp(g_inflight.v[i], key)) {
                free(g_inflight.v[i]);
                g_inflight.v[i] = g_inflight.v[--g_inflight.n];
                break;
            }
        pthread_mutex_unlock(&g_mu);
        free(j->key);
        free(j->name);
        free(j->description);
        free(j);
    }
}

/* ---------- per-session catalog ---------- */

struct skilldesc {
    pthread_mutex_t mu;
    json_t *snapshot; /* key -> description shown in this session */
    json_t *deferred; /* misses rendered before skilldesc_start_compression: key -> {name, description}; NULL once it ran */
};

skilldesc_t *skilldesc_new(void) {
    skilldesc_t *c = xcalloc(1, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    c->snapshot = json_obj();
    c->deferred = json_obj();
    return c;
}

void skilldesc_start_compression(skilldesc_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mu);
    json_t *deferred = c->deferred;
    c->deferred = NULL;
    pthread_mutex_unlock(&c->mu);
    const char *k;
    json_t *v;
    json_obj_foreach(deferred, i, k, v) schedule(k, json_get_str(v, "name"), json_get_str(v, "description"));
    json_free(deferred);
}

void skilldesc_free(skilldesc_t *c) {
    if (!c) return;
    /* give in-flight compressions a short grace period to land in the cache */
    int64_t end = now_ms() + 4000;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        int n = (int)g_inflight.n;
        pthread_mutex_unlock(&g_mu);
        if (!n || now_ms() >= end) break;
        struct timespec ts = {0, 50 * 1000000L};
        nanosleep(&ts, NULL);
    }
    json_free(c->snapshot);
    json_free(c->deferred);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

char *skilldesc_render(void *catalog, const char *name, const char *description) {
    skilldesc_t *c = catalog;
    if (!c || !name || !description) return NULL;
    /* omp d36f905ad4, dfb02f0644, 18e38771a8: an authored description already within the preview budget needs no
     * compression and is used verbatim (whitespace collapsed), so its exclusions survive */
    {
        size_t chars = 0;
        bool sp = false, any = false;
        for (const char *p = description; *p; p++) {
            if (isspace((unsigned char)*p)) {
                sp = any;
                continue;
            }
            if (sp) chars++;
            sp = false;
            any = true;
            if (((unsigned char)*p & 0xC0) != 0x80) chars++;
        }
        if (chars <= MAX_PREVIEW_CHARS) return skilldesc_preview(description);
    }
    char *key = key_for(name, description);
    pthread_mutex_lock(&c->mu);
    const char *snap = json_get_str(c->snapshot, key);
    char *r = snap ? xstrdup(snap) : NULL;
    pthread_mutex_unlock(&c->mu);
    if (!r) {
        bool miss = false;
        r = settings_bool("skills.compressDescriptions", true) ? store_get(key) : NULL;
        if (!r) {
            r = skilldesc_preview(description);
            miss = settings_bool("skills.compressDescriptions", true);
        }
        pthread_mutex_lock(&c->mu);
        json_set_str(c->snapshot, key, r);
        if (miss && c->deferred) {
            json_t *d = json_obj();
            json_set_str(d, "name", name);
            json_set_str(d, "description", description);
            json_set(c->deferred, key, d);
            miss = false;
        }
        pthread_mutex_unlock(&c->mu);
        if (miss) schedule(key, name, description);
    }
    free(key);
    return r;
}
