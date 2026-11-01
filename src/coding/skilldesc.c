/* skilldesc.c - compressed skill descriptions (see skilldesc.h). */
#include "skilldesc.h"

#include "coding/app.h"
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
#define MAX_COMPRESSED_WORDS 12
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

char *skilldesc_valid(const char *text) {
    char *line = str_trim(text ? text : "");
    int words = 0;
    bool in = false;
    for (const char *p = line; *p; p++) {
        if (isspace((unsigned char)*p)) in = false;
        else if (!in) {
            in = true;
            words++;
        }
    }
    if (!*line || strpbrk(line, "\r\n") || utf8_strlen(line) > MAX_COMPRESSED_CHARS || words > MAX_COMPRESSED_WORDS) {
        free(line);
        return NULL;
    }
    return line;
}

/* ---------- background compression ---------- */

static const ai_model_t *pick_model(void) {
    const char *roles[] = {"modelRoles.smol", "modelRoles.default", "modelRoles.tiny"};
    for (size_t i = 0; i < ARRAY_LEN(roles); i++) {
        const char *r = settings_str(roles[i], NULL);
        const ai_model_t *m = r ? app_resolve_model_pattern(r, NULL, NULL) : NULL;
        if (m) return m;
    }
    return app_default_model();
}

typedef struct {
    char *key, *name, *description;
} job_t;

static void *compress_thread(void *p) {
    job_t *j = p;
    pthread_mutex_lock(&g_mu);
    while (g_running >= MAX_PARALLEL) pthread_cond_wait(&g_cv, &g_mu);
    g_running++;
    pthread_mutex_unlock(&g_mu);
    const ai_model_t *m = pick_model();
    if (m) {
        char *unusable = xasprintf("%s:%s/%s", j->key, m->provider, m->id);
        char *seen = store_get(unusable);
        if (!seen) {
            json_t *c = json_obj();
            json_set_str(c, "name", j->name);
            json_set_str(c, "description", j->description);
            char *req = prompt_render(PROMPT_PATH, c);
            json_free(c);
            json_t *msgs = json_arr();
            json_push(msgs, msg_user_text(req ? req : ""));
            free(req);
            ai_context_t ctx = {.messages = msgs};
            ai_stream_opts_t o;
            ai_stream_opts_init(&o);
            char *key = app_api_key_for(m);
            o.api_key = key;
            o.max_tokens = 1024;
            o.disable_reasoning = true;
            o.temperature = 0;
            json_t *r = key ? ai_complete(m, &ctx, &o) : NULL;
            const char *stop = json_get_str(r, "stopReason");
            if (str_eq(stop, "stop")) {
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
};

skilldesc_t *skilldesc_new(void) {
    skilldesc_t *c = xcalloc(1, sizeof(*c));
    pthread_mutex_init(&c->mu, NULL);
    c->snapshot = json_obj();
    return c;
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
    pthread_mutex_destroy(&c->mu);
    free(c);
}

char *skilldesc_render(void *catalog, const char *name, const char *description) {
    skilldesc_t *c = catalog;
    if (!c || !name || !description) return NULL;
    char *key = key_for(name, description);
    pthread_mutex_lock(&c->mu);
    const char *snap = json_get_str(c->snapshot, key);
    char *r = snap ? xstrdup(snap) : NULL;
    pthread_mutex_unlock(&c->mu);
    if (!r) {
        r = settings_bool("skills.compressDescriptions", true) ? store_get(key) : NULL;
        if (!r) {
            r = skilldesc_preview(description);
            if (settings_bool("skills.compressDescriptions", true)) schedule(key, name, description);
        }
        pthread_mutex_lock(&c->mu);
        json_set_str(c->snapshot, key, r);
        pthread_mutex_unlock(&c->mu);
    }
    free(key);
    return r;
}
