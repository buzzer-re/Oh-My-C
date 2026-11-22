/* codex_version.c - runtime-resolved OpenAI Codex client version (port of omp catalog/src/wire/codex-version.ts,
 * omp f54745ac23 / 88fc862ac4).
 *
 * The ChatGPT backend gates `/models` and `/responses` on the client version without rejecting old clients, so a
 * stale pin silently hides new SKUs. The version tracks the `latest` npm dist-tag of `@openai/codex`:
 *  - the first read in a process loads `<agent dir>/codex-client-version.json` ({version, checkedAt}) and keeps that
 *    value for the rest of the process;
 *  - when the record is missing or older than 6 h, a detached background fetch (5 s timeout) rewrites it, so the next
 *    process starts with the new version;
 *  - the result never drops below the pinned CODEX_CLIENT_VERSION_PIN.
 * `PI_AI_CODEX_CLIENT_VERSION` pins the version and disables both the record and the refresh.
 *
 * omc has no XDG directory layout, so the record always lives in the agent dir (omp's non-XDG location). The npm
 * request honors OMC_PROVIDER_URL_REWRITE (test hook). */
#include "ai/provider_openai.h"

#include "core/fs.h"
#include "net/http.h"

#include <pthread.h>

#define NPM_LATEST_URL "https://registry.npmjs.org/@openai/codex/latest"
#define REFRESH_INTERVAL_MS (6LL * 60 * 60 * 1000)
#define FETCH_TIMEOUT_MS 5000

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_resolved;
static bool g_refresh_started;

/* /^(\d+)\.(\d+)\.(\d+)$/ */
static bool parse_semver(const char *s, long v[3]) {
    if (!s) return false;
    const char *p = s;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return false;
        long n = 0;
        while (*p >= '0' && *p <= '9') {
            if (n > 100000000) return false;
            n = n * 10 + (*p++ - '0');
        }
        v[i] = n;
        if (i < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    return *p == 0;
}

bool ai_codex_semver_valid(const char *s) {
    long v[3];
    return parse_semver(s, v);
}

int ai_codex_semver_cmp(const char *a, const char *b) {
    long pa[3], pb[3];
    if (!parse_semver(a, pa) || !parse_semver(b, pb)) return 0;
    for (int i = 0; i < 3; i++)
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    return 0;
}

static const char *newer_of(const char *candidate) {
    if (!candidate || !ai_codex_semver_valid(candidate)) return CODEX_CLIENT_VERSION_PIN;
    return ai_codex_semver_cmp(candidate, CODEX_CLIENT_VERSION_PIN) > 0 ? candidate : CODEX_CLIENT_VERSION_PIN;
}

static char *record_path(void) { return omc_agent_path("codex-client-version.json"); }

static void *refresh_main(void *arg) {
    (void)arg;
    char *url = ai_rewrite_url(NPM_LATEST_URL);
    http_headers_t h = {0};
    http_headers_set(&h, "Accept", "application/json");
    http_resp_t r;
    int rc = http_get(url, &h, FETCH_TIMEOUT_MS, &r);
    http_headers_free(&h);
    free(url);
    if (rc != 0 || r.status < 200 || r.status >= 300) {
        if (rc != 0) LOGD("codex client version refresh failed: %s", r.error ? r.error : "network error");
        http_resp_free(&r);
        return NULL;
    }
    json_t *j = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    const char *version = json_get_str(j, "version");
    if (version && ai_codex_semver_valid(version)) {
        json_t *rec = json_obj();
        json_set_str(rec, "version", version);
        json_set_int(rec, "checkedAt", now_ms());
        char *s = json_dump(rec, 0);
        char *path = record_path();
        if (fs_write_atomic(path, s, strlen(s), 0644) != 0) LOGD("codex client version record not written: %s", path);
        free(path);
        free(s);
        json_free(rec);
    }
    json_free(j);
    return NULL;
}

const char *ai_codex_client_version(void) {
    const char *override = env_get("PI_AI_CODEX_CLIENT_VERSION");
    if (override && *override) return override;
    pthread_mutex_lock(&g_mu);
    if (!g_resolved) {
        char *path = record_path();
        char *raw = fs_read(path, NULL);
        free(path);
        json_t *rec = raw ? json_parse_str(raw) : NULL;
        free(raw);
        const char *version = json_get_str(rec, "version");
        json_t *checked = json_get(rec, "checkedAt");
        bool valid = json_is_obj(rec) && version && json_is_num(checked);
        g_resolved = xstrdup(newer_of(valid ? version : NULL));
        int64_t checked_at = valid ? json_d2i(json_get_num(rec, "checkedAt", 0)) : 0;
        if (!g_refresh_started && (!valid || now_ms() - checked_at > REFRESH_INTERVAL_MS)) {
            g_refresh_started = true;
            pthread_t t;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&t, &a, refresh_main, NULL) != 0) LOGD("codex client version refresh not started");
            pthread_attr_destroy(&a);
        }
        json_free(rec);
    }
    const char *v = g_resolved;
    pthread_mutex_unlock(&g_mu);
    return v;
}

void ai_codex_client_version_reset_for_tests(void) {
    pthread_mutex_lock(&g_mu);
    free(g_resolved);
    g_resolved = NULL;
    g_refresh_started = false;
    pthread_mutex_unlock(&g_mu);
}
