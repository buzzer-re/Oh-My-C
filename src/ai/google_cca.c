/* google_cca.c - Cloud Code Assist client identity + project provisioning; see google_cca.h. */
#include "ai/google_cca.h"

#include <pthread.h>
#include <sys/utsname.h>

#define DEFAULT_GEMINI_CLI_VERSION "0.46.0"
#define DEFAULT_ANTIGRAVITY_VERSION "2.26.0"
#define ANTIGRAVITY_CL "1005016104"
#define ANTIGRAVITY_MANIFEST_URL "https://antigravity-hub-auto-updater-974169037036.us-central1.run.app/manifest/latest-arm64-mac.yml"
#define MANIFEST_TIMEOUT_MS 5000
#define MANIFEST_RETRY_MS (10 * 60000)
#define OAUTH_REQUEST_TIMEOUT_MS 30000
#define GEMINI_POLL_INTERVAL_MS 5000
#define GEMINI_POLL_MAX_ATTEMPTS 24
#define AG_ONBOARD_TIMEOUT_MS 30000
#define AG_ONBOARD_POLL_MS 1000
#define TIER_FREE "free-tier"
#define TIER_LEGACY "legacy-tier"
#define TIER_STANDARD "standard-tier"
#define GCA_DOCS "See https://goo.gle/gemini-cli-auth-docs#workspace-gca"

/* ---------- client identity ---------- */

static const char *host_platform(void) {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "linux";
#endif
}

static const char *host_arch(void) {
    static char arch[80];
    if (!arch[0]) {
        struct utsname u;
        const char *m = uname(&u) == 0 ? u.machine : "x86_64";
        if (!strcmp(m, "x86_64") || !strcmp(m, "amd64")) m = "x64";
        else if (!strcmp(m, "aarch64")) m = "arm64";
        else if (!strncmp(m, "i686", 4) || !strncmp(m, "i386", 4)) m = "ia32";
        snprintf(arch, sizeof arch, "%s", m);
    }
    return arch;
}

char *cca_gemini_cli_user_agent(const char *model_id) {
    const char *v = env_get("PI_AI_GEMINI_CLI_VERSION");
    return xasprintf("GeminiCLI/%s/%s (%s; %s; terminal)", v ? v : DEFAULT_GEMINI_CLI_VERSION,
                     model_id && *model_id ? model_id : "gemini-3.1-pro-preview", host_platform(), host_arch());
}

void cca_gemini_cli_headers(http_headers_t *h, const char *model_id) {
    char *ua = cca_gemini_cli_user_agent(model_id);
    http_headers_set(h, "User-Agent", ua);
    free(ua);
    http_headers_set(h, "Client-Metadata", "ideType=IDE_UNSPECIFIED,platform=PLATFORM_UNSPECIFIED,pluginType=GEMINI");
}

static pthread_mutex_t g_ver_mu = PTHREAD_MUTEX_INITIALIZER;
static char *g_discovered_version;
static int64_t g_version_failed_at;

char *cca_parse_manifest_version(const char *yaml) {
    if (!yaml) return NULL;
    strvec_t lines = str_split_lines(yaml, strlen(yaml));
    char *out = NULL;
    for (size_t i = 0; i < lines.n; i++) {
        const char *p = lines.v[i];
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "version", 7) != 0) continue;
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char val[64];
        size_t n = 0;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q && n < sizeof val - 1) val[n++] = *p++;
            if (*p != q) continue;
            p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '#' && n < sizeof val - 1) val[n++] = *p++;
        }
        val[n] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') continue; /* not a well-formed `version:` line */
        char *t = str_trim(val);
        int a, b, c, used = 0;
        if (sscanf(t, "%d.%d.%d%n", &a, &b, &c, &used) == 3 && (size_t)used == strlen(t) && t[0] >= '0' && t[0] <= '9' &&
            !strchr(t, '+') && !strchr(t, '-'))
            out = t;
        else
            free(t);
        break; /* first `version:` line decides */
    }
    strvec_free(&lines);
    return out;
}

void cca_antigravity_ensure_version(void) {
    if (env_get("PI_AI_ANTIGRAVITY_VERSION")) return;
    pthread_mutex_lock(&g_ver_mu);
    bool skip = g_discovered_version || (g_version_failed_at && mono_ms() - g_version_failed_at < MANIFEST_RETRY_MS);
    if (!skip) {
        http_headers_t h = {0};
        http_headers_set(&h, "Cache-Control", "no-cache");
        http_headers_set(&h, "User-Agent", "electron-builder");
        http_resp_t r;
        if (http_get(ANTIGRAVITY_MANIFEST_URL, &h, MANIFEST_TIMEOUT_MS, &r) == 0 && r.status >= 200 && r.status < 300)
            g_discovered_version = cca_parse_manifest_version(r.body);
        http_resp_free(&r);
        http_headers_free(&h);
        if (!g_discovered_version) g_version_failed_at = mono_ms();
    }
    pthread_mutex_unlock(&g_ver_mu);
}

char *cca_antigravity_version(void) {
    const char *v = env_get("PI_AI_ANTIGRAVITY_VERSION");
    if (v) return xstrdup(v);
    pthread_mutex_lock(&g_ver_mu);
    char *r = xstrdup(g_discovered_version ? g_discovered_version : DEFAULT_ANTIGRAVITY_VERSION);
    pthread_mutex_unlock(&g_ver_mu);
    return r;
}

char *cca_antigravity_user_agent(void) {
    char *ver = cca_antigravity_version();
    const char *cl = env_get("PI_AI_ANTIGRAVITY_CL");
    /* native builds: macOS ships arm64 only, Linux/Windows x64 only */
#if defined(__APPLE__)
    const char *os_def = "darwin", *arch_def = "arm64";
#else
    const char *os_def = "linux", *arch_def = "amd64";
#endif
    const char *os = env_get("PI_AI_ANTIGRAVITY_OS"), *arch = env_get("PI_AI_ANTIGRAVITY_ARCH");
    char *ua = xasprintf("antigravity/hub/%s (aidev_client; os_type=%s; arch=%s; cl=%s)", ver, os ? os : os_def,
                         arch ? arch : arch_def, cl ? cl : ANTIGRAVITY_CL);
    free(ver);
    return ua;
}

void cca_antigravity_headers(http_headers_t *h) {
    char *ua = cca_antigravity_user_agent();
    http_headers_set(h, "User-Agent", ua);
    free(ua);
    http_headers_set(h, "Accept-Encoding", "gzip");
}

/* ---------- validation-required errors ---------- */

char *cca_extract_validation_url(const char *body) {
    if (!body || !strstr(body, "VALIDATION_REQUIRED")) return NULL;
    const char *start = strchr(body, '{');
    if (!start) return NULL;
    json_t *j = json_parse_str(start);
    char *url = NULL;
    json_t *d;
    json_arr_foreach(json_get_arr(json_get_obj(j, "error"), "details"), i, d) {
        const char *u = json_get_str(json_get_obj(d, "metadata"), "validation_url");
        if (str_eq(json_get_str(d, "reason"), "VALIDATION_REQUIRED") && u) {
            url = xstrdup(u);
            break;
        }
    }
    json_free(j);
    return url;
}

char *cca_validation_message(const char *url, const char *next_action, const char *email) {
    return email && *email ? xasprintf("Account verification required for %s. Visit %s to continue, then %s.", email, url, next_action)
                           : xasprintf("Account verification required. Visit %s to continue, then %s.", url, next_action);
}

/* ---------- control-plane requests ---------- */

static const char *status_text(int s) {
    switch (s) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

typedef struct {
    http_headers_t h;
    const char *provider;
    cca_progress_fn progress;
    void *ud;
} cca_ctx_t;

static void progress(cca_ctx_t *c, const char *msg) {
    if (c->progress) c->progress(c->ud, msg);
}

/* Returns 0 with status and body (owned) when a response arrived, -1 with err on transport failure. */
static int cca_request(cca_ctx_t *c, const char *method, const char *url, const char *body, int64_t timeout_ms,
                       int *status, char **resp_body, char **err) {
    http_req_t req = {.method = method, .url = url, .headers = &c->h, .body = body, .body_len = body ? strlen(body) : 0,
                      .timeout_ms = timeout_ms > 0 ? timeout_ms : OAUTH_REQUEST_TIMEOUT_MS};
    http_resp_t r;
    int rc = http_request(&req, &r);
    if (rc != 0) {
        bool timed_out = r.error && str_istr(r.error, "timed out");
        *err = timed_out ? xasprintf("Timed out after %lldms waiting for %s", (long long)req.timeout_ms, url)
                         : xasprintf("%s request to %s failed: %s", c->provider, url, r.error ? r.error : "network error");
        http_resp_free(&r);
        return -1;
    }
    *status = r.status;
    *resp_body = xstrdup(r.body ? r.body : "");
    http_resp_free(&r);
    return 0;
}

static char *dump_obj(json_t *j) {
    char *s = json_dump(j, 0);
    json_free(j);
    return s;
}

static json_t *gemini_metadata(const char *duet) {
    json_t *m = json_obj();
    json_set_str(m, "ideType", "IDE_UNSPECIFIED");
    json_set_str(m, "platform", "PLATFORM_UNSPECIFIED");
    json_set_str(m, "pluginType", "GEMINI");
    if (duet) json_set_str(m, "duetProject", duet);
    return m;
}

static bool vpc_sc_affected(const char *body) {
    json_t *j = json_parse_str(body ? body : "");
    bool hit = false;
    json_t *d;
    json_arr_foreach(json_get_arr(json_get_obj(j, "error"), "details"), i, d) {
        if (str_eq(json_get_str(d, "reason"), "SECURITY_POLICY_VIOLATED")) hit = true;
    }
    json_free(j);
    return hit;
}

static int gemini_discover(cca_ctx_t *c, const char *endpoint, char **project, char **err) {
    const char *envp = env_get("GOOGLE_CLOUD_PROJECT");
    if (!envp) envp = env_get("GOOGLE_CLOUD_PROJECT_ID");
    progress(c, "Checking for existing Cloud Code Assist project...");
    json_t *lb = json_obj();
    if (envp) json_set_str(lb, "cloudaicompanionProject", envp);
    json_set(lb, "metadata", gemini_metadata(envp));
    char *body = dump_obj(lb);
    char *url = xasprintf("%s/v1internal:loadCodeAssist", endpoint);
    int status = 0;
    char *resp = NULL;
    int rc = cca_request(c, "POST", url, body, 0, &status, &resp, err);
    free(url);
    free(body);
    if (rc) return -1;
    json_t *data = NULL;
    if (status < 200 || status >= 300) {
        if (vpc_sc_affected(resp)) {
            data = json_parse_str("{\"currentTier\":{\"id\":\"" TIER_STANDARD "\"}}");
        } else {
            *err = xasprintf("loadCodeAssist failed: %d %s: %s", status, status_text(status), resp);
            free(resp);
            return -1;
        }
    } else {
        data = json_parse_str(resp);
        if (!json_is_obj(data)) {
            json_free(data);
            data = json_obj();
        }
    }
    free(resp);
    if (json_get(data, "currentTier") && !json_is_null(json_get(data, "currentTier"))) {
        const char *p = json_get_str(data, "cloudaicompanionProject");
        if (p && *p) *project = xstrdup(p);
        else if (envp) *project = xstrdup(envp);
        else *err = xstrdup("This account requires setting the GOOGLE_CLOUD_PROJECT or GOOGLE_CLOUD_PROJECT_ID environment "
                            "variable. " GCA_DOCS);
        json_free(data);
        return *project ? 0 : -1;
    }
    const char *tier_id = TIER_LEGACY;
    json_t *t;
    json_t *allowed = json_get_arr(data, "allowedTiers");
    json_arr_foreach(allowed, i, t) {
        if (json_get_bool(t, "isDefault", false)) {
            tier_id = json_get_str(t, "id");
            break;
        }
    }
    if (!tier_id) tier_id = TIER_FREE;
    char *tier = xstrdup(tier_id);
    json_free(data);
    if (strcmp(tier, TIER_FREE) != 0 && !envp) {
        free(tier);
        *err = xstrdup("This account requires setting the GOOGLE_CLOUD_PROJECT or GOOGLE_CLOUD_PROJECT_ID environment variable. " GCA_DOCS);
        return -1;
    }
    progress(c, "Provisioning Cloud Code Assist project (this may take a moment)...");
    json_t *ob = json_obj();
    json_set_str(ob, "tierId", tier);
    bool paid = strcmp(tier, TIER_FREE) != 0 && envp;
    json_set(ob, "metadata", gemini_metadata(paid ? envp : NULL));
    if (paid) {
        /* cloudaicompanionProject goes after metadata, as upstream assigns it later */
        json_set_str(ob, "cloudaicompanionProject", envp);
    }
    free(tier);
    body = dump_obj(ob);
    url = xasprintf("%s/v1internal:onboardUser", endpoint);
    rc = cca_request(c, "POST", url, body, 0, &status, &resp, err);
    free(url);
    free(body);
    if (rc) return -1;
    if (status < 200 || status >= 300) {
        *err = xasprintf("onboardUser failed: %d %s: %s", status, status_text(status), resp);
        free(resp);
        return -1;
    }
    json_t *lro = json_parse_str(resp);
    free(resp);
    if (!json_get_bool(lro, "done", false) && json_get_str(lro, "name")) {
        char *name = xstrdup(json_get_str(lro, "name"));
        json_free(lro);
        lro = NULL;
        for (int attempt = 0; attempt < GEMINI_POLL_MAX_ATTEMPTS; attempt++) {
            if (attempt > 0) {
                char *m = xasprintf("Waiting for project provisioning (attempt %d/%d)...", attempt + 1, GEMINI_POLL_MAX_ATTEMPTS);
                progress(c, m);
                free(m);
                sleep_ms(GEMINI_POLL_INTERVAL_MS);
            }
            url = xasprintf("%s/v1internal/%s", endpoint, name);
            rc = cca_request(c, "GET", url, NULL, 0, &status, &resp, err);
            free(url);
            if (rc) {
                free(name);
                return -1;
            }
            if (status < 200 || status >= 300) {
                *err = xasprintf("Failed to poll operation: %d %s", status, status_text(status));
                free(resp);
                free(name);
                return -1;
            }
            json_t *d = json_parse_str(resp);
            free(resp);
            if (json_get_bool(d, "done", false)) {
                lro = d;
                break;
            }
            json_free(d);
        }
        free(name);
        if (!lro) {
            *err = xasprintf("Project provisioning did not complete after %d attempts", GEMINI_POLL_MAX_ATTEMPTS);
            return -1;
        }
    }
    const char *pid = json_get_str(json_get_obj(json_get_obj(lro, "response"), "cloudaicompanionProject"), "id");
    if (pid && *pid) *project = xstrdup(pid);
    else if (envp) *project = xstrdup(envp);
    json_free(lro);
    if (*project) return 0;
    *err = xstrdup("Could not discover or provision a Google Cloud project. Try setting the GOOGLE_CLOUD_PROJECT or "
                   "GOOGLE_CLOUD_PROJECT_ID environment variable. " GCA_DOCS);
    return -1;
}

/* ---- antigravity ---- */

/* POST/GET returning parsed JSON on HTTP 200, else NULL + *err */
static json_t *ag_call(cca_ctx_t *c, const char *label, const char *method, const char *url, const char *body,
                       int64_t timeout_ms, char **err) {
    int status = 0;
    char *resp = NULL;
    if (cca_request(c, method, url, body, timeout_ms, &status, &resp, err) != 0) {
        char *wrapped = xasprintf("Could not discover an Antigravity project. %s", *err);
        free(*err);
        *err = wrapped;
        return NULL;
    }
    if (status != 200) {
        *err = xasprintf("%s failed: %d %s: %s", label, status, status_text(status), resp);
        free(resp);
        return NULL;
    }
    json_t *j = json_parse_str(resp);
    free(resp);
    if (!json_is_obj(j)) {
        json_free(j);
        *err = xasprintf("failed to unmarshal %s response", label);
        return NULL;
    }
    return j;
}

static bool has_msg_field(const json_t *j, const char *k) {
    json_t *v = json_get(j, k);
    return v && !json_is_null(v);
}

static json_t *ag_load(cca_ctx_t *c, const char *endpoint, char **err) {
    char *url = xasprintf("%s/v1internal:loadCodeAssist", endpoint);
    json_t *b = json_obj(), *md = json_obj();
    json_set_str(md, "ideType", "ANTIGRAVITY");
    json_set(b, "metadata", md);
    char *body = dump_obj(b);
    json_t *payload = ag_call(c, "loadCodeAssist", "POST", url, body, 0, err);
    free(body);
    const char *pid = payload ? json_get_str(payload, "cloudaicompanionProject") : NULL;
    if (payload && !has_msg_field(payload, "paidTier") && pid && *pid) {
        b = json_obj();
        json_set_str(b, "cloudaicompanionProject", pid);
        md = json_obj();
        json_set_str(md, "ideType", "ANTIGRAVITY");
        json_set(b, "metadata", md);
        body = dump_obj(b);
        json_t *again = ag_call(c, "loadCodeAssist", "POST", url, body, 0, err);
        free(body);
        json_free(payload);
        payload = again;
    }
    free(url);
    return payload;
}

static int ag_free_tier_check(const json_t *p, char **err) {
    json_t *t;
    json_arr_foreach(json_get_arr(p, "allowedTiers"), i, t) {
        if (str_eq(json_get_str(t, "id"), TIER_FREE)) return 0;
    }
    json_arr_foreach(json_get_arr(p, "ineligibleTiers"), i, t) {
        if (!str_eq(json_get_str(t, "tierId"), TIER_FREE)) continue;
        const char *reason = json_get_str(t, "reasonMessage");
        if (!reason || !*reason) return 0;
        const char *vu = json_get_str(t, "validationUrl");
        *err = vu && *vu ? xasprintf("%s\n%s", reason, vu) : xstrdup(reason);
        return -1;
    }
    return 0;
}

static int ag_onboard(cca_ctx_t *c, const char *endpoint, char **err) {
    int64_t deadline = mono_ms() + AG_ONBOARD_TIMEOUT_MS;
    char *url = xasprintf("%s/v1internal:onboardUser", endpoint);
    json_t *b = json_obj(), *md = json_obj();
    json_set_str(b, "tierId", TIER_FREE);
    json_set_str(md, "ideType", "ANTIGRAVITY");
    json_set(b, "metadata", md);
    char *body = dump_obj(b);
    json_t *op = ag_call(c, "onboardUser", "POST", url, body, deadline - mono_ms(), err);
    free(body);
    free(url);
    for (;;) {
        if (!op) return -1;
        if (json_get_bool(op, "done", false)) {
            json_t *e = json_get(op, "error");
            int rc = 0;
            if (e && !json_is_null(e)) {
                const char *m = json_get_str(e, "message");
                json_t *code = json_get(e, "code");
                char *desc;
                if (m) desc = json_is_num(code) ? xasprintf("%lld: %s", (long long)code->u.num.d, m) : xstrdup(m);
                else desc = json_dump(e, 0);
                *err = xasprintf("OnboardUser operation failed: %s", desc);
                free(desc);
                rc = -1;
            } else if (!has_msg_field(op, "response")) {
                *err = xstrdup("failed to unmarshal OnboardUserResponse");
                rc = -1;
            }
            json_free(op);
            return rc;
        }
        int64_t left = deadline - mono_ms();
        if (left <= 0) {
            json_free(op);
            *err = xasprintf("onboardUser timed out after %dms", AG_ONBOARD_TIMEOUT_MS);
            return -1;
        }
        sleep_ms(MIN((int64_t)AG_ONBOARD_POLL_MS, left));
        const char *name = json_get_str(op, "name");
        if (!name || !*name) {
            json_free(op);
            *err = xstrdup("onboardUser returned an operation without a name");
            return -1;
        }
        left = deadline - mono_ms();
        if (left <= 0) {
            json_free(op);
            *err = xasprintf("onboardUser timed out after %dms", AG_ONBOARD_TIMEOUT_MS);
            return -1;
        }
        url = xasprintf("%s/v1internal/%s", endpoint, name);
        json_free(op);
        op = ag_call(c, "onboardUser operation", "GET", url, NULL, left, err);
        free(url);
    }
}

static int antigravity_discover(cca_ctx_t *c, const char *endpoint, char **project, char **err) {
    progress(c, "Checking Cloud Code Assist account status...");
    json_t *initial = ag_load(c, endpoint, err);
    if (!initial) return -1;
    if (ag_free_tier_check(initial, err) != 0) {
        json_free(initial);
        return -1;
    }
    bool need_onboard = !has_msg_field(initial, "currentTier");
    json_free(initial);
    if (need_onboard) {
        progress(c, "Provisioning the Antigravity free tier...");
        if (ag_onboard(c, endpoint, err) != 0) return -1;
    }
    progress(c, "Refreshing Cloud Code Assist project...");
    json_t *refreshed = ag_load(c, endpoint, err);
    if (!refreshed) return -1;
    const char *pid = json_get_str(refreshed, "cloudaicompanionProject");
    if (pid && *pid) *project = xstrdup(pid);
    json_free(refreshed);
    if (*project) return 0;
    *err = xstrdup("loadCodeAssist did not return a cloudaicompanionProject");
    return -1;
}

int cca_discover_project(const char *provider, const char *access_token, const char *endpoint, cca_progress_fn progress_fn,
                         void *ud, char **project_out, char **err) {
    *project_out = NULL;
    *err = NULL;
    bool ag = str_eq(provider, "google-antigravity");
    cca_ctx_t c = {.provider = provider, .progress = progress_fn, .ud = ud};
    char *bearer = xasprintf("Bearer %s", access_token ? access_token : "");
    http_headers_set(&c.h, "Authorization", bearer);
    free(bearer);
    http_headers_set(&c.h, "Content-Type", "application/json");
    if (ag) cca_antigravity_headers(&c.h);
    else cca_gemini_cli_headers(&c.h, NULL);
    char *base = xstrdup(endpoint ? endpoint : ag ? CCA_ANTIGRAVITY_DAILY_ENDPOINT : CCA_GEMINI_CLI_ENDPOINT);
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') base[--bl] = 0;
    int rc = ag ? antigravity_discover(&c, base, project_out, err) : gemini_discover(&c, base, project_out, err);
    free(base);
    http_headers_free(&c.h);
    return rc;
}
