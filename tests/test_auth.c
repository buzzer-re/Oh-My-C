/* test_auth.c - provider login flows against tests/e2e/mock_auth.py (no real credentials, no network):
 * RFC 8628 device flows (pending / slow_down / expiry / denial, headers hook, OIDC-discovered token endpoint,
 * userinfo, JWT claims, after-exchange hooks), the custom hooks (GitHub Copilot incl. enterprise domains,
 * Cursor, Codex device, Kilo, Alibaba Token Plan, Cloudflare), api-key validation (accepted, rejected, optional,
 * trustForbidden, headers hook) and refresh of stored OAuth credentials. Runs from the repository root. */
#include "ai/ai.h"
#include "config/auth.h"
#include "core/fs.h"
#include "net/http.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* scripted controller: answers prompts in order, records what the flow showed */
typedef struct {
    const char *answers[8];
    size_t next;
    strvec_t prompts, progress;
    char *url, *instructions;
} script_t;

static void s_auth(void *ud, const char *url, const char *launch, const char *instr) {
    script_t *s = ud;
    free(s->url);
    free(s->instructions);
    s->url = xstrdup(url);
    s->instructions = instr ? xstrdup(instr) : NULL;
}

static void s_progress(void *ud, const char *msg) { strvec_push(&((script_t *)ud)->progress, msg); }

static char *s_prompt(void *ud, const auth_prompt_t *p) {
    script_t *s = ud;
    strvec_push(&s->prompts, p->message);
    const char *a = s->next < ARRAY_LEN(s->answers) ? s->answers[s->next++] : NULL;
    return a ? xstrdup(a) : NULL;
}

static void s_free(script_t *s) {
    strvec_free(&s->prompts);
    strvec_free(&s->progress);
    free(s->url);
    free(s->instructions);
}

static int login(const char *provider, script_t *s, char **err) {
    auth_login_hooks_t h = {0};
    h.on_auth = s_auth;
    h.on_progress = s_progress;
    h.prompt = s_prompt;
    h.ud = s;
    *err = NULL;
    return auth_login_hooked(provider, &h, err);
}

static json_t *stored(const char *provider) {
    char *p = omc_agent_path("auth.json");
    size_t n;
    char *s = fs_read(p, &n);
    json_t *j = s ? json_parse(s, n, NULL) : NULL;
    free(s);
    free(p);
    json_t *e = json_clone(json_get(j, provider));
    json_free(j);
    return e;
}

static bool has_progress(const script_t *s, const char *msg) {
    for (size_t i = 0; i < s->progress.n; i++)
        if (strstr(s->progress.v[i], msg)) return true;
    return false;
}

/* rewrite every stored credential's expiry so the next auth_get_api_key refreshes it */
static void expire(const char *provider) {
    char *p = omc_agent_path("auth.json");
    size_t n;
    char *s = fs_read(p, &n);
    json_t *j = json_parse(s, n, NULL);
    json_set_int(json_get(j, provider), "expires", 1000);
    char *out = json_dump(j, 2);
    fs_write_atomic(p, out, strlen(out), 0600);
    free(out);
    free(s);
    free(p);
    json_free(j);
    auth_reload();
}

static json_t *jwt_claims(const char *tok) {
    const char *a = tok ? strchr(tok, '.') : NULL, *b = a ? strchr(a + 1, '.') : NULL;
    if (!b) return NULL;
    size_t n;
    unsigned char *d = base64_decode(a + 1, (size_t)(b - a - 1), &n);
    json_t *j = d ? json_parse((const char *)d, n, NULL) : NULL;
    free(d);
    return j;
}

static json_t *mock_log(int port) {
    char *url = xasprintf("http://127.0.0.1:%d/_log", port);
    http_resp_t r;
    json_t *j = NULL;
    if (http_get(url, NULL, 5000, &r) == 0) j = json_parse_str(r.body ? r.body : "");
    http_resp_free(&r);
    free(url);
    return j;
}

static const json_t *last_req(const json_t *log, const char *path) {
    const json_t *hit = NULL, *e;
    json_arr_foreach(log, i, e) if (str_eq(json_get_str(e, "path"), path)) hit = e;
    return hit;
}

static int count_req(const json_t *log, const char *prefix) {
    int n = 0;
    json_t *e;
    json_arr_foreach(log, i, e) if (str_starts(json_get_str(e, "path"), prefix)) n++;
    return n;
}

int main(void) {
    char tmpl[] = "/tmp/omc-auth-test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return 1;
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("OMC_NO_BROWSER", "1", 1);
    setenv("OMC_AUTH_POLL_SCALE_MS", "20", 1);
    srand((unsigned)getpid());
    /* random port; retry on another one when the mock can't bind (other test suites may hold it) */
    int port = 0;
    pid_t mock = -1;
    char *base = NULL;
    for (int attempt = 0; attempt < 8; attempt++) {
        port = 30000 + rand() % 20000;
        char ports[16];
        snprintf(ports, sizeof ports, "%d", port);
        mock = fork();
        if (mock == 0) {
            execlp("python3", "python3", "-I", "tests/e2e/mock_auth.py", ports, (char *)NULL);
            _exit(127);
        }
        free(base);
        base = xasprintf("http://127.0.0.1:%d", port);
        bool ready = false;
        for (int i = 0; i < 100 && !ready; i++) {
            int status;
            if (waitpid(mock, &status, WNOHANG) == mock) break; /* mock exited (port taken) */
            http_resp_t r;
            char *u = xasprintf("%s/_log", base);
            int rc = http_get(u, NULL, 1000, &r);
            int st = r.status;
            http_resp_free(&r);
            free(u);
            if (rc == 0 && st == 200) ready = true;
            else usleep(100000);
        }
        if (ready) break;
        kill(mock, SIGTERM);
        waitpid(mock, NULL, 0);
        mock = -1;
    }
    char *rw = xasprintf("https://github.com=%1$s/gh,https://api.github.com=%1$s/ghapi,https://api.individual.githubcopilot.com=%1$s/capi,"
                         "https://ghe.example.com=%1$s/ghe,https://auth.x.ai=%1$s/xauth,https://api.x.ai=%1$s/xaiapi,"
                         "https://api2.cursor.sh=%1$s/cursor,https://auth.openai.com=%1$s/oai,https://api.kilo.ai=%1$s/kilo,"
                         "https://auth.meta.com=%1$s/meta,https://api.meta.ai=%1$s/metaapi,https://api.workos.com=%1$s/workos,"
                         "https://api.factory.ai=%1$s/factory,https://api.z.ai=%1$s/zaiapi,https://api.code.umans.ai=%1$s/umans,"
                         "https://api.commandcode.ai=%1$s/cmdcode,https://api.inference.wandb.ai=%1$s/coreweave,"
                         "https://token-plan.ap-southeast-1.maas.aliyuncs.com=%1$s/tp,https://console.mistral.ai=%1$s/mistral",
                         base);
    setenv("OMC_AUTH_URL_REWRITE", rw, 1);
    auth_init();
    char *err = NULL;

    /* ---- device code: Kimi (headers hook, pending -> slow_down -> token, JWT claim accountId) ---- */
    {
        char *host = xasprintf("%s/kimi", base);
        setenv("KIMI_CODE_OAUTH_HOST", host, 1);
        script_t s = {0};
        int rc = login("kimi-code", &s, &err);
        CHECK(rc == 0, "kimi login: %s", err ? err : "?");
        CHECK(s.url && strstr(s.url, "/verify?user_code=ABCD-1234"), "kimi verification url: %s", s.url ? s.url : "(null)");
        CHECK(str_eq(s.instructions, "Enter code: ABCD-1234"), "kimi instructions: %s", s.instructions ? s.instructions : "(null)");
        CHECK(has_progress(&s, "Requesting device authorization...") && has_progress(&s, "Waiting for device authorization..."),
              "kimi progress");
        json_t *c = stored("kimi-code");
        CHECK(str_eq(json_get_str(c, "refresh"), "kimi-r1"), "kimi refresh token");
        CHECK(str_eq(json_get_str(c, "accountId"), "kimi-user"), "kimi accountId from JWT claim");
        int64_t exp = json_get_int(c, "expires", 0);
        CHECK(exp > now_ms() + 3000000 && exp < now_ms() + 3400000, "kimi expiry (3600 s - 300 s skew): %lld", (long long)exp);
        json_free(c);
        json_t *log = mock_log(port);
        const json_t *d = last_req(log, "/kimi/api/oauth/device_authorization");
        CHECK(str_eq(json_get_str(json_get(d, "headers"), "x-msh-platform"), "kimi_cli"), "kimi fingerprint header");
        CHECK(json_get_str(json_get(d, "headers"), "x-msh-device-id") && strlen(json_get_str(json_get(d, "headers"), "x-msh-device-id")) == 32,
              "kimi device id");
        CHECK(str_eq(json_get_str(json_get(d, "form"), "client_id"), "17e5f671-d194-4dfb-9706-5516cb48c098"), "kimi client id");
        CHECK(count_req(log, "/kimi/api/oauth/token") == 3, "kimi polled 3 times: %d", count_req(log, "/kimi/api/oauth/token"));
        const json_t *t = last_req(log, "/kimi/api/oauth/token");
        CHECK(str_eq(json_get_str(json_get(t, "form"), "grant_type"), "urn:ietf:params:oauth:grant-type:device_code"), "device grant");
        json_free(log);
        free(err);
        s_free(&s);
        /* refresh: request rule with the kimi headers hook */
        expire("kimi-code");
        char *k = auth_get_api_key("kimi-code");
        json_t *p = jwt_claims(k);
        CHECK(json_get_int(p, "n", 0) == 2, "kimi refreshed access token: %s", k ? k : "(null)");
        json_free(p);
        free(k);
        c = stored("kimi-code");
        CHECK(str_eq(json_get_str(c, "refresh"), "kimi-r2") && json_get_int(c, "expires", 0) > now_ms(), "kimi refresh stored");
        json_free(c);
        free(host);

        /* expiry and denial */
        host = xasprintf("%s/kimi-expire", base);
        setenv("KIMI_CODE_OAUTH_HOST", host, 1);
        script_t s2 = {0};
        err = NULL;
        rc = login("kimi-code", &s2, &err);
        CHECK(rc != 0 && str_eq(err, "Device flow timed out"), "kimi expiry: %s", err ? err : "(none)");
        free(err);
        s_free(&s2);
        free(host);
        host = xasprintf("%s/kimi-deny", base);
        setenv("KIMI_CODE_OAUTH_HOST", host, 1);
        script_t s3 = {0};
        rc = login("kimi-code", &s3, &err);
        CHECK(rc != 0 && str_eq(err, "kimi-code device authorization was denied"), "kimi denied: %s", err ? err : "(none)");
        free(err);
        s_free(&s3);
        free(host);
        unsetenv("KIMI_CODE_OAUTH_HOST");
    }

    /* ---- device code with an OIDC-discovered token endpoint + userinfo (xAI) ---- */
    {
        script_t s = {0};
        int rc = login("xai-oauth", &s, &err);
        CHECK(rc == 0, "xai login: %s", err ? err : "?");
        json_t *c = stored("xai-oauth");
        CHECK(str_eq(json_get_str(c, "email"), "me@x.ai") && str_eq(json_get_str(c, "accountId"), "xai-sub"), "xai identity");
        json_free(c);
        json_t *log = mock_log(port);
        const json_t *d = last_req(log, "/xauth/oauth2/device/code");
        CHECK(str_eq(json_get_str(json_get(d, "form"), "scope"), "openid profile email offline_access grok-cli:access api:access"), "xai scope");
        json_free(log);
        free(err);
        s_free(&s);
        expire("xai-oauth");
        char *k = auth_get_api_key("xai-oauth");
        c = stored("xai-oauth");
        CHECK(str_eq(json_get_str(c, "refresh"), "xr2") && str_eq(json_get_str(c, "email"), "me@x.ai"), "xai refresh keeps identity");
        json_free(c);
        free(k);
    }

    /* ---- Mistral Vibe browser sign-in: PKCE-bound exchange, poll classification, the key stored as a plain API key ---- */
    {
        char *u = xasprintf("%s/mistral/_scenario?name=ok", base);
        http_resp_t hr;
        http_get(u, NULL, 5000, &hr);
        http_resp_free(&hr);
        free(u);
        script_t s = {0};
        int rc = login("mistral", &s, &err);
        CHECK(rc == 0, "mistral login: %s", err ? err : "?");
        CHECK(str_eq(s.url, "https://console.mistral.ai/vibe/approve?p=1"), "mistral sign-in url: %s", s.url ? s.url : "?");
        CHECK(str_eq(s.instructions, "Sign in with your Mistral account and approve Mistral Vibe access, then return here."),
              "mistral instructions");
        CHECK(strvec_contains(&s.progress, "Waiting for Mistral sign-in to complete...") &&
                  strvec_contains(&s.progress, "Exchanging sign-in for a Mistral API key..."),
              "mistral progress");
        json_t *c = stored("mistral");
        CHECK(str_eq(json_get_str(c, "type"), "api_key") && str_eq(json_get_str(c, "key"), "vibe-key"), "mistral key stored as an API key");
        json_free(c);
        free(err);
        err = NULL;
        s_free(&s);
        auth_logout("mistral");

        static const struct {
            const char *scenario, *want;
        } fail_cases[] = {
            {"expired", "Mistral sign-in expired; run /login again"},
            {"denied", "Mistral sign-in was denied"},
            {"hung-up", "console said no"},
            {"unknown", "Mistral sign-in returned unknown state \"sideways\""},
            {"no-token", "Mistral sign-in completed without an exchange token"},
            {"down", "Mistral sign-in status unavailable: HTTP 503"},
            {"workspace", "Your Mistral workspace can't be used for Mistral Vibe; run /login again and pick another workspace in the browser, or ask your administrator"},
            {"bad-url", "Failed to start Mistral browser sign-in: unexpected URL https://evil.example.com/api/poll"},
            {"start-500", "Failed to start Mistral browser sign-in: HTTP 500"},
            {"expires-now", "Device flow timed out"},
        };
        for (size_t i = 0; i < ARRAY_LEN(fail_cases); i++) {
            u = xasprintf("%s/mistral/_scenario?name=%s", base, fail_cases[i].scenario);
            http_get(u, NULL, 5000, &hr);
            http_resp_free(&hr);
            free(u);
            script_t f = {0};
            rc = login("mistral", &f, &err);
            CHECK(rc != 0 && str_eq(err, fail_cases[i].want), "mistral %s: %s", fail_cases[i].scenario, err ? err : "(no error)");
            json_t *none = stored("mistral");
            CHECK(!none, "mistral %s stored nothing", fail_cases[i].scenario);
            json_free(none);
            free(err);
            err = NULL;
            s_free(&f);
        }

        /* two transient poll failures ride out; a 429 stretches only that wait */
        static const char *const ok_cases[] = {"flaky", "throttled"};
        for (size_t i = 0; i < ARRAY_LEN(ok_cases); i++) {
            u = xasprintf("%s/mistral/_scenario?name=%s", base, ok_cases[i]);
            http_get(u, NULL, 5000, &hr);
            http_resp_free(&hr);
            free(u);
            script_t f = {0};
            rc = login("mistral", &f, &err);
            CHECK(rc == 0, "mistral %s: %s", ok_cases[i], err ? err : "?");
            free(err);
            err = NULL;
            s_free(&f);
            auth_logout("mistral");
        }

        /* already cancelled: the console is never contacted */
        volatile int abort_now = 1;
        script_t f = {0};
        auth_login_hooks_t h = {0};
        h.abort = &abort_now;
        h.ud = &f;
        h.on_auth = s_auth;
        h.on_progress = s_progress;
        h.prompt = s_prompt;
        json_t *lg = mock_log(port);
        int before = count_req(lg, "/mistral/api/");
        json_free(lg);
        rc = auth_login_hooked("mistral", &h, &err);
        CHECK(rc != 0 && str_eq(err, "Login cancelled"), "mistral cancelled: %s", err ? err : "?");
        lg = mock_log(port);
        CHECK(count_req(lg, "/mistral/api/") == before, "mistral cancelled before any request");
        json_free(lg);
        free(err);
        err = NULL;
        s_free(&f);
    }

    /* ---- after-exchange hooks: Muse key mint, Factory whoami ---- */
    {
        script_t s = {0};
        int rc = login("muse-code", &s, &err);
        CHECK(rc == 0, "muse login: %s", err ? err : "?");
        json_t *c = stored("muse-code");
        json_t *bundle = json_parse_str(json_get_str(c, "access") ? json_get_str(c, "access") : "");
        CHECK(str_eq(json_get_str(bundle, "apiKey"), "muse-key") && str_eq(json_get_str(bundle, "oauthAccessToken"), "meta-at"), "muse bundle");
        CHECK(str_eq(json_get_str(c, "email"), "muse@example.com") && str_eq(json_get_str(c, "accountId"), "u1"), "muse identity");
        CHECK(json_get_int(c, "expires", 0) > 8000000000000000LL, "muse never expires");
        json_free(bundle);
        json_free(c);
        free(err);
        s_free(&s);

        script_t f = {0};
        rc = login("factory-droid", &f, &err);
        CHECK(rc == 0, "factory login: %s", err ? err : "?");
        c = stored("factory-droid");
        CHECK(str_eq(json_get_str(c, "orgId"), "org_f") && str_eq(json_get_str(c, "region"), "global") &&
                  str_eq(json_get_str(c, "inferenceRegion"), "global") && str_eq(json_get_str(c, "email"), "f@x.com"),
              "factory identity");
        json_free(c);
        free(err);
        s_free(&f);
    }

    /* ---- GitHub Copilot: domain prompt, device flow, endpoint discovery, model policies, refresh hook ---- */
    {
        script_t s = {.answers = {""}};
        int rc = login("github-copilot", &s, &err);
        CHECK(rc == 0, "copilot login: %s", err ? err : "?");
        CHECK(s.prompts.n == 1 && str_eq(s.prompts.v[0], "GitHub Enterprise URL/domain (blank for github.com)"), "copilot prompt");
        CHECK(str_eq(s.url, "https://github.com/login/device") && str_eq(s.instructions, "Enter code: WXYZ-0001"), "copilot auth info");
        CHECK(has_progress(&s, "Enabling models..."), "copilot enabling models");
        json_t *c = stored("github-copilot");
        CHECK(str_eq(json_get_str(c, "access"), "gho_test") && str_eq(json_get_str(c, "refresh"), "gho_test"), "copilot token");
        CHECK(str_eq(json_get_str(c, "apiEndpoint"), "https://api.individual.githubcopilot.com"), "copilot api endpoint");
        CHECK(!json_get(c, "enterpriseUrl"), "copilot public has no enterprise url");
        json_free(c);
        json_t *log = mock_log(port);
        CHECK(count_req(log, "/gh/login/oauth/access_token") == 3, "copilot polls");
        CHECK(count_req(log, "/capi/models/") > 0, "copilot model policies posted");
        const json_t *pol = last_req(log, "/capi/models/claude-sonnet-4.5/policy");
        if (!pol) {
            json_t *e;
            json_arr_foreach(log, i, e) if (str_starts(json_get_str(e, "path"), "/capi/models/")) pol = e;
        }
        CHECK(str_eq(json_get_str(json_get(pol, "headers"), "openai-intent"), "chat-policy") &&
                  str_eq(json_get_str(json_get(pol, "headers"), "copilot-integration-id"), "copilot-chat"),
              "copilot policy headers");
        json_free(log);
        free(err);
        s_free(&s);
        /* structured api key carries the endpoint */
        char *k = auth_get_api_key("github-copilot");
        json_t *kj = json_parse_str(k ? k : "");
        CHECK(str_eq(json_get_str(kj, "token"), "gho_test") && str_eq(json_get_str(kj, "apiEndpoint"), "https://api.individual.githubcopilot.com"),
              "copilot structured key: %s", k ? k : "(null)");
        json_free(kj);
        free(k);
        expire("github-copilot");
        k = auth_get_api_key("github-copilot");
        c = stored("github-copilot");
        CHECK(json_get_int(c, "expires", 0) > now_ms() + 300LL * 24 * 3600 * 1000 &&
                  str_eq(json_get_str(c, "apiEndpoint"), "https://api.individual.githubcopilot.com"),
              "copilot refresh hook");
        json_free(c);
        free(k);
        /* enterprise domain: Copilot CLI client id, enterpriseUrl stored */
        script_t e = {.answers = {"https://GHE.example.com/"}};
        rc = login("github-copilot", &e, &err);
        CHECK(rc == 0, "copilot enterprise login: %s", err ? err : "?");
        c = stored("github-copilot");
        CHECK(str_eq(json_get_str(c, "enterpriseUrl"), "ghe.example.com") && str_eq(json_get_str(c, "access"), "gho_ent"), "copilot enterprise");
        json_free(c);
        free(err);
        s_free(&e);
        script_t bad = {.answers = {"not a host!"}};
        rc = login("github-copilot", &bad, &err);
        CHECK(rc != 0 && str_eq(err, "Invalid GitHub Enterprise URL/domain"), "copilot invalid domain: %s", err ? err : "(none)");
        free(err);
        s_free(&bad);
    }

    /* ---- Cursor (poll + email), Codex device (stored as openai-codex), Kilo ---- */
    {
        script_t s = {0};
        int rc = login("cursor", &s, &err);
        CHECK(rc == 0, "cursor login: %s", err ? err : "?");
        CHECK(s.url && str_starts(s.url, "https://cursor.com/loginDeepControl?challenge=") && strstr(s.url, "&mode=login&redirectTarget=cli"),
              "cursor url");
        json_t *c = stored("cursor");
        CHECK(str_eq(json_get_str(c, "email"), "cur@example.com") && str_eq(json_get_str(c, "refresh"), "cur-r1"), "cursor stored");
        json_free(c);
        free(err);
        s_free(&s);
        expire("cursor");
        char *k = auth_get_api_key("cursor");
        c = stored("cursor");
        CHECK(str_eq(json_get_str(c, "refresh"), "cur-r1") && json_get_int(c, "expires", 0) > now_ms() + 3600000,
              "cursor refresh keeps refresh token");
        json_free(c);
        free(k);

        script_t x = {0};
        rc = login("openai-codex-device", &x, &err);
        CHECK(rc == 0, "codex device login: %s", err ? err : "?");
        CHECK(str_eq(x.url, "https://auth.openai.com/codex/device") && str_eq(x.instructions, "Enter code: CODX-0001"), "codex device auth");
        c = stored("openai-codex");
        CHECK(c && str_eq(json_get_str(c, "accountId"), "acct-1") && str_eq(json_get_str(c, "email"), "me@example.com") &&
                  str_eq(json_get_str(c, "orgName"), "pro"),
              "codex identity stored under openai-codex");
        json_free(c);
        c = stored("openai-codex-device");
        CHECK(!c, "nothing stored under the login alias");
        json_free(c);
        free(err);
        s_free(&x);
        expire("openai-codex");
        k = auth_get_api_key("openai-codex");
        c = stored("openai-codex");
        CHECK(str_eq(json_get_str(c, "refresh"), "or2") && str_eq(json_get_str(c, "orgName"), "pro"), "codex refresh + profile hook");
        json_free(c);
        free(k);

        script_t kl = {0};
        rc = login("kilo", &kl, &err);
        CHECK(rc == 0 && str_eq(kl.instructions, "Enter code: KILO1"), "kilo login: %s", err ? err : "?");
        c = stored("kilo");
        CHECK(str_eq(json_get_str(c, "access"), "kilo-tok"), "kilo token");
        json_free(c);
        free(err);
        s_free(&kl);
    }

    /* ---- api-key logins and validation ---- */
    {
        script_t s = {.answers = {"good-key"}};
        int rc = login("xai", &s, &err);
        CHECK(rc == 0, "xai api key: %s", err ? err : "?");
        CHECK(str_eq(s.url, "https://console.x.ai/team/default/api-keys") && has_progress(&s, "Validating API key..."), "xai api-key flow");
        json_t *c = stored("xai");
        CHECK(str_eq(json_get_str(c, "type"), "api_key") && str_eq(json_get_str(c, "key"), "good-key"), "xai key stored");
        json_free(c);
        free(err);
        s_free(&s);
        auth_logout("xai");
        script_t b = {.answers = {"wrong-key"}};
        rc = login("xai", &b, &err);
        CHECK(rc != 0 && err && str_starts(err, "xAI API key validation failed (401): ") && strstr(err, "Incorrect API key"),
              "xai rejected: %s", err ? err : "(none)");
        c = stored("xai");
        CHECK(!c, "rejected key not stored");
        json_free(c);
        free(err);
        s_free(&b);

        script_t z = {.answers = {"good-key"}};
        rc = login("zai", &z, &err);
        CHECK(rc == 0, "zai chat-completions probe: %s", err ? err : "?");
        free(err);
        s_free(&z);
        script_t um = {.answers = {"good-key"}};
        rc = login("umans", &um, &err);
        CHECK(rc == 0, "umans anthropic probe: %s", err ? err : "?");
        free(err);
        s_free(&um);
        script_t um2 = {.answers = {"bad"}};
        rc = login("umans", &um2, &err);
        CHECK(rc != 0 && err && strstr(err, "API key validation failed (401)"), "umans rejected: %s", err ? err : "(none)");
        free(err);
        s_free(&um2);

        script_t o = {.answers = {"flaky"}};
        rc = login("commandcode", &o, &err);
        CHECK(rc == 0 && has_progress(&o, "Validating API key (optional)...") &&
                  has_progress(&o, "Skipping Command Code validation endpoint; continuing with provided API key."),
              "optional probe 500 skipped: %s", err ? err : "?");
        free(err);
        s_free(&o);
        script_t fb = {.answers = {"forbidden"}};
        rc = login("commandcode", &fb, &err);
        CHECK(rc == 0, "trustForbidden 403 accepted: %s", err ? err : "?");
        free(err);
        s_free(&fb);
        script_t un = {.answers = {"nope"}};
        rc = login("commandcode", &un, &err);
        CHECK(rc != 0, "optional probe 401 rejects");
        free(err);
        s_free(&un);

        script_t emp = {.answers = {"  "}};
        rc = login("xai", &emp, &err);
        CHECK(rc != 0 && str_eq(err, "API key is required"), "empty key: %s", err ? err : "(none)");
        free(err);
        s_free(&emp);
        script_t ol = {.answers = {""}};
        rc = login("ollama", &ol, &err);
        c = stored("ollama");
        CHECK(rc == 0 && !c, "ollama empty fallback stores nothing: %s", err ? err : "");
        json_free(c);
        free(err);
        s_free(&ol);

        unsetenv("COREWEAVE_PROJECT");
        unsetenv("WANDB_INFERENCE_PROJECT");
        unsetenv("WANDB_PROJECT");
        script_t cw = {.answers = {"good-key"}};
        rc = login("coreweave", &cw, &err);
        CHECK(rc != 0 && err && strstr(err, "requires OpenAI-Project"), "coreweave missing project: %s", err ? err : "(none)");
        free(err);
        s_free(&cw);
        setenv("COREWEAVE_PROJECT", "team/proj", 1);
        script_t cw2 = {.answers = {"good-key"}};
        rc = login("coreweave", &cw2, &err);
        CHECK(rc == 0, "coreweave headers hook: %s", err ? err : "?");
        free(err);
        s_free(&cw2);
    }

    /* ---- multi-prompt custom logins ---- */
    {
        script_t s = {.answers = {"", "good-key", "Cookie: a=1; b=2"}};
        int rc = login("alibaba-token-plan", &s, &err);
        CHECK(rc == 0 && s.prompts.n == 3, "token plan login: %s", err ? err : "?");
        json_t *c = stored("alibaba-token-plan");
        json_t *k = json_parse_str(json_get_str(c, "key") ? json_get_str(c, "key") : "");
        CHECK(str_eq(json_get_str(k, "token"), "good-key") && str_eq(json_get_str(k, "cookie"), "a=1; b=2") && !json_get(k, "baseUrl"),
              "token plan credential: %s", json_get_str(c, "key") ? json_get_str(c, "key") : "(null)");
        json_free(k);
        json_free(c);
        free(err);
        s_free(&s);
        script_t bc = {.answers = {"1", "good-key", "justavalue"}};
        rc = login("alibaba-token-plan", &bc, &err);
        CHECK(rc != 0 && err && strstr(err, "Invalid QwenCloud Cookie header"), "token plan bad cookie: %s", err ? err : "(none)");
        free(err);
        s_free(&bc);

        script_t cf = {.answers = {" tok ", "acct", "gw"}};
        rc = login("cloudflare-ai-gateway", &cf, &err);
        c = stored("cloudflare-ai-gateway");
        CHECK(rc == 0 && str_eq(json_get_str(c, "key"), "{\"token\":\"tok\",\"accountId\":\"acct\",\"gatewayId\":\"gw\"}"), "cloudflare gateway");
        json_free(c);
        free(err);
        s_free(&cf);
        script_t cw = {.answers = {"tok", ""}};
        rc = login("cloudflare-workers-ai", &cw, &err);
        CHECK(rc != 0 && str_eq(err, "Cloudflare account ID is required"), "cloudflare account required: %s", err ? err : "(none)");
        free(err);
        s_free(&cw);

        script_t cancel = {.answers = {NULL}};
        rc = login("cloudflare-workers-ai", &cancel, &err);
        CHECK(rc != 0 && str_eq(err, "Login cancelled"), "cancelled prompt: %s", err ? err : "(none)");
        free(err);
        s_free(&cancel);

        script_t sf = {.answers = {"http://evil.example.com"}};
        rc = login("snowflake", &sf, &err);
        CHECK(rc != 0 && str_eq(err, "Snowflake account URL must use https"), "snowflake https only: %s", err ? err : "(none)");
        free(err);
        s_free(&sf);
    }

    /* ---- a provider paused by its auth rules (omp 8895c0fa32): Google Antigravity ---- */
    {
        const char *why = "Google may restrict accounts that use Antigravity outside its own apps, so this provider is paused for now.";
        CHECK(str_eq(catalog_provider_pause_reason("google-antigravity"), why), "antigravity pause reason");
        CHECK(!catalog_provider_pause_reason("google-gemini-cli"), "gemini-cli not paused");
        CHECK(auth_set_api_key("google-antigravity", "stored-token") == 0, "store a paused login");
        CHECK(!auth_has_credentials("google-antigravity") && !auth_get_api_key("google-antigravity") &&
                  !auth_get_credential("google-antigravity"),
              "paused provider resolves no key");
        json_t *c = stored("google-antigravity");
        CHECK(c != NULL, "the stored login stays on disk");
        json_free(c);
        script_t sp = {0};
        err = NULL;
        int rc = login("google-antigravity", &sp, &err);
        CHECK(rc != 0 && err && !strcmp(err, "google-antigravity is turned off: Google may restrict accounts that use Antigravity outside its own "
                                              "apps, so this provider is paused for now."),
              "paused login refused: %s", err ? err : "(none)");
        free(err);
        s_free(&sp);
        json_t *d;
        bool listed = false;
        json_arr_foreach(auth_oauth_providers(), i, d) if (auth_login_listed(d) && str_eq(json_get_str(d, "id"), "google-antigravity")) listed = true;
        CHECK(!listed, "paused provider is no login choice");
        auth_logout("google-antigravity");
    }

    if (mock > 0) {
        kill(mock, SIGTERM);
        waitpid(mock, NULL, 0);
    }
    char *cmd = xasprintf("rm -rf '%s'", dir);
    if (system(cmd) != 0) {}
    free(cmd);
    free(rw);
    free(base);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
