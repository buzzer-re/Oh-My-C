/* test_model_resolver.c - omp role resolution (config/model-resolver.ts semantics), retry fallback chains and
 * online tiny candidates over a temp agent dir with a custom `mock` provider. */
#include "config/model_resolver.h"
#include "config/auth.h"
#include "config/settings.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool is(const ai_model_t *m, const char *key) {
    char *k = mr_model_key(m);
    bool ok = k && !strcmp(k, key);
    if (!ok) fprintf(stderr, "  got %s want %s\n", k ? k : "(none)", key);
    free(k);
    return ok;
}

static void set(const char *path, const char *json) { settings_set_runtime(path, json_parse_str(json)); }

int main(void) {
    char tmpl[] = "/tmp/omc-test-resolver.XXXXXX";
    char *dir = mkdtemp(tmpl);
    /* no ambient credentials: only the mock provider is available */
    for (char **e = environ; *e;) {
        const char *eq = strchr(*e, '=');
        size_t n = eq ? (size_t)(eq - *e) : strlen(*e);
        if ((n > 8 && !strncmp(*e + n - 8, "_API_KEY", 8)) || (n > 6 && !strncmp(*e + n - 6, "_TOKEN", 6))) {
            char *k = xstrndup(*e, n);
            unsetenv(k);
            free(k);
            e = environ;
        } else
            e++;
    }
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("HOME", dir, 1);
    setenv("OMC_NO_LOCAL_DISCOVERY", "1", 1);
    setenv("MOCK_KEY", "x", 1);
    char *my = xasprintf("%s/models.yml", dir);
    const char *yml = "providers:\n  mock:\n    baseUrl: http://127.0.0.1:9/v1\n    apiKey: MOCK_KEY\n    api: openai-completions\n"
                      "    models:\n      - id: mock-1\n        reasoning: true\n      - id: mock-2\n        reasoning: true\n"
                      "      - id: mock-flaky\n      - id: mock-3-20250101\n      - id: mock-3-20260101\n"
                      "      - id: MiMo-V2.6-Pro\n      - id: mimo-v2.6-pro\n";
    fs_write_atomic(my, yml, strlen(yml), 0644);
    free(my);
    settings_init(dir, NULL);
    auth_init();

    mr_pool_t avail = {0};
    mr_pool_available(&avail, "chat");
    CHECK(avail.n >= 5);
    bool only_mock = true;
    for (size_t i = 0; i < avail.n; i++) only_mock = only_mock && !strcmp(avail.v[i]->provider, "mock");
    CHECK(only_mock);

    /* matching engine */
    CHECK(is(mr_match("mock/mock-1", &avail, true), "mock/mock-1"));
    CHECK(is(mr_match("MOCK-2", &avail, true), "mock/mock-2"));
    CHECK(is(mr_match("flaky", &avail, false), "mock/mock-flaky"));
    CHECK(mr_match("flaky", &avail, true) == NULL);
    CHECK(is(mr_match("mock-3", &avail, false), "mock/mock-3-20260101")); /* newest dated id */
    CHECK(mr_match("mock/flaky", &avail, false) == NULL);                  /* known provider: exact only */
    /* ids differing only by case each resolve their exact selector; a spelling matching neither stays ambiguous */
    CHECK(is(mr_match("mock/MiMo-V2.6-Pro", &avail, true), "mock/MiMo-V2.6-Pro"));
    CHECK(is(mr_match("mock/mimo-v2.6-pro", &avail, true), "mock/mimo-v2.6-pro"));
    CHECK(mr_match("mock/MIMO-V2.6-PRO", &avail, true) == NULL);
    mr_result_t r;
    CHECK(mr_parse_pattern("mock-2:high", &avail, &r) && is(r.model, "mock/mock-2") && str_eq(r.level, "high"));
    mr_result_clear(&r);
    CHECK(mr_parse_pattern("mock-2:bogus", &avail, &r) && is(r.model, "mock/mock-2") && r.warning);
    mr_result_clear(&r);

    /* level selectors */
    char *base;
    CHECK(mr_split_level("a/b:max", -1, false, &base) == NULL && !strcmp(base, "a/b:max"));
    free(base);
    CHECK(str_eq(mr_split_level("a/b:max", -1, true, &base), "max") && !strcmp(base, "a/b"));
    free(base);

    /* no roles configured: no default from configuration, but pickDefaultAvailableModel still picks one */
    CHECK(mr_default_model(NULL) != NULL);
    CHECK(mr_resolve_role("smol", NULL) == NULL); /* the built-in smol chain names no mock model */

    /* role aliases and inheritance */
    set("modelRoles", "{\"default\":\"mock/mock-1\"}");
    CHECK(is(mr_default_model(NULL), "mock/mock-1"));
    CHECK(is(mr_resolve_role("smol", NULL), "mock/mock-1"));  /* smol inherits a configured default */
    CHECK(is(mr_resolve_role("slow", NULL), "mock/mock-1"));
    CHECK(is(mr_resolve_role("tiny", NULL), "mock/mock-1"));  /* tiny -> smol -> default */
    CHECK(mr_resolve_role("advisor", NULL) == NULL);          /* advisor never inherits the primary */
    CHECK(mr_resolve_role("plan", NULL) == NULL);             /* no default chain for plan */
    strvec_t pats = mr_configured_patterns("@judge");
    CHECK(pats.n >= 3 && !strcmp(pats.v[0], "typesafe/jev-1.14.0") && strvec_contains(&pats, "mock/mock-1"));
    strvec_free(&pats);
    const char *lv = NULL;
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"smol\":\"@default:low\",\"slow\":\"pi/smol\",\"commit\":\"*\"}");
    CHECK(is(mr_resolve_role("smol", &lv), "mock/mock-1") && str_eq(lv, "low"));
    CHECK(is(mr_resolve_role("slow", &lv), "mock/mock-1") && str_eq(lv, "low"));
    CHECK(is(mr_resolve_role("commit", NULL), "mock/mock-1"));
    char *ex = mr_explicit_role("@smol:high");
    CHECK(ex && !strcmp(ex, "smol"));
    free(ex);
    CHECK(mr_explicit_role("mock/mock-1") == NULL);
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"advisor\":\"nope-model,mock-2\",\"slow\":\"mock/mock-2\"}");
    CHECK(is(mr_resolve_role("advisor", NULL), "mock/mock-2"));
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"slow\":\"mock/mock-2\"}");
    CHECK(is(mr_resolve_role("advisor", NULL), "mock/mock-2")); /* configured slow */

    /* role chains */
    mr_chain_t ch;
    mr_role_chain("smol", NULL, &ch);
    CHECK(ch.n == 1 && is(ch.v[0].model, "mock/mock-1") && !ch.v[0].explicit_);
    mr_chain_free(&ch);
    set("retry.fallbackChains", "{\"smol\":[\"mock/mock-flaky\",\"mock/mock-2\"]}");
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"smol\":\"anthropic/claude-haiku-5-5,mock/mock-1\"}");
    mr_role_chain("smol", NULL, &ch);
    CHECK(ch.n == 3 && is(ch.v[0].model, "mock/mock-1") && ch.v[0].explicit_ && is(ch.v[1].model, "mock/mock-flaky") &&
          is(ch.v[2].model, "mock/mock-2"));
    CHECK(ch.n && ch.v[0].replaces && !strcmp(ch.v[0].replaces, "anthropic/claude-haiku-5-5"));
    mr_chain_free(&ch);

    /* retry fallback chain keys and candidates */
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"smol\":\"mock/mock-2\"}");
    set("retry.fallbackChains", "{\"default\":[\"mock/mock-2\",\"mock/mock-flaky\"],\"mock/mock-flaky\":[\"mock/mock-1\"]}");
    const ai_model_t *m1 = catalog_find("mock", "mock-1"), *mf = catalog_find("mock", "mock-flaky");
    char *key = mr_fallback_chain_key("mock/mock-1:high", m1, NULL);
    CHECK(key && !strcmp(key, "default"));
    mr_selector_t *c = NULL;
    size_t n = mr_fallback_candidates(key, "mock/mock-1:high", m1, false, &c);
    CHECK(n == 2 && !strcmp(c[0].raw, "mock/mock-2") && !strcmp(c[1].raw, "mock/mock-flaky"));
    for (size_t i = 0; i < n; i++) mr_selector_free(&c[i]);
    free(c);
    free(key);
    key = mr_fallback_chain_key("mock/mock-flaky", mf, NULL); /* model keys beat roles */
    CHECK(key && !strcmp(key, "mock/mock-flaky"));
    free(key);
    key = mr_fallback_chain_key("mock/mock-2", catalog_find("mock", "mock-2"), NULL); /* smol gets the default chain */
    CHECK(key && !strcmp(key, "smol"));
    free(key);
    /* a chat session on the model assigned to a kind role does not adopt that role's chain */
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"web\":\"mock/mock-2\"}");
    set("retry.fallbackChains", "{\"default\":[\"mock/mock-flaky\"],\"web\":[\"web/exa\"]}");
    key = mr_fallback_chain_key("mock/mock-2", catalog_find("mock", "mock-2"), NULL);
    CHECK(key && !strcmp(key, "default"));
    free(key);
    key = mr_fallback_chain_key("mock/mock-2", catalog_find("mock", "mock-2"), "web"); /* the explicit hint still applies */
    CHECK(key && !strcmp(key, "web"));
    free(key);
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"smol\":\"mock/mock-2\"}");
    set("retry.fallbackChains", "{\"mock/*\":[\"other/*\"]}");
    key = mr_fallback_chain_key("mock/mock-2", NULL, NULL);
    CHECK(key && !strcmp(key, "mock/*"));
    n = mr_fallback_candidates(key, "mock/mock-2", NULL, false, &c);
    CHECK(n == 1 && !strcmp(c[0].raw, "other/mock-2"));
    for (size_t i = 0; i < n; i++) mr_selector_free(&c[i]);
    free(c);
    free(key);
    strvec_t warn = {0};
    CHECK(mr_fallback_validate(&warn) == 1);
    strvec_free(&warn);

    /* cooldowns and admission */
    mr_suppress_selector("mock/mock-2", now_ms() + 60000);
    CHECK(mr_selector_suppressed("mock/mock-2") && !mr_selector_suppressed("mock/mock-1"));
    CHECK(mr_fallback_admits(false) && mr_fallback_admits(true));
    set("retry.fallbackOn", "\"usage-limit\"");
    CHECK(!mr_fallback_admits(false) && mr_fallback_admits(true));
    CHECK(str_eq(mr_rate_limit_reason("429 Too Many Requests"), "RATE_LIMIT_EXCEEDED"));
    CHECK(str_eq(mr_rate_limit_reason("model overloaded"), "MODEL_CAPACITY_EXHAUSTED"));
    CHECK(mr_retry_backoff_ms(500, 1) <= 500 && mr_retry_backoff_ms(500, 10) <= 8000 && mr_retry_backoff_ms(500, 10) >= 6000);
    CHECK(mr_retry_backoff_ms(10, 1) <= 10 && mr_retry_backoff_ms(10, 1) >= 7.5 && mr_retry_backoff_ms(10, 2) >= 15);
    /* reason windows (turn-recovery.ts): a plain 503 keeps the exponential base (was a 45-75 s capacity wait) */
    CHECK(mr_error_status("503 try again later") == 503 && mr_error_status("try again") == 0);
    CHECK(str_eq(mr_rate_limit_reason("503 try again later"), "MODEL_CAPACITY_EXHAUSTED"));
    CHECK(mr_reason_window_ms("503 try again later", 503, false, false) == 0);
    CHECK(mr_reason_window_ms("529 model overloaded", 529, false, false) == 0);
    CHECK(mr_reason_window_ms("429 model overloaded", 429, false, false) >= 45000);
    CHECK(mr_reason_window_ms("503 MODEL_CAPACITY_EXHAUSTED", 503, false, false) >= 45000);
    CHECK(mr_reason_window_ms("503 Resource exhausted", 503, false, false) >= 45000);
    CHECK(mr_reason_window_ms("429 Too Many Requests", 429, false, false) == 30000);
    CHECK(mr_reason_window_ms("429 Too Many Requests", 429, false, true) == 0); /* retry-after is authoritative */
    CHECK(mr_reason_window_ms("too many concurrent requests", 0, false, false) == 5000);
    CHECK(mr_reason_window_ms("429 usage limit reached", 429, true, false) == 0);

    /* online tiny candidates: role primaries, their chains (default chain applied), then the current model */
    set("modelRoles", "{\"default\":\"mock/mock-1\",\"tiny\":\"mock/mock-flaky\"}");
    set("retry.fallbackChains", "{\"default\":[\"mock/mock-2\"]}");
    static const char *const roles[] = {"tiny", "commit", "smol"};
    mr_pool_t out = {0};
    mr_online_tiny_candidates(roles, 3, m1, &out);
    CHECK(out.n == 3 && is(out.v[0], "mock/mock-flaky") && is(out.v[1], "mock/mock-2") && is(out.v[2], "mock/mock-1"));
    mr_pool_free(&out);
    set("retry.modelFallback", "false");
    mr_online_tiny_candidates(roles, 3, m1, &out);
    CHECK(out.n == 1 && is(out.v[0], "mock/mock-flaky"));
    mr_pool_free(&out);

    /* MRU usage breaks ties and persists */
    mr_usage_record("mock/mock-2");
    mr_usage_record("mock/mock-1");
    strvec_t u = mr_usage_order();
    CHECK(u.n == 2 && !strcmp(u.v[0], "mock/mock-1"));
    strvec_free(&u);

    /* omp f462f71eb4..ef855c96bc: disabledProviders from any layer drops the native project items (its config.yml
     * modelRoles stay); a project that disables itself keeps that switch */
    char *proj = xasprintf("%s/proj", dir), *pdir = xasprintf("%s/proj/.omc", dir), *pcfg = xasprintf("%s/proj/.omc/config.yml", dir);
    fs_mkdir_p(pdir, 0755);
    const char *pyml = "fetch:\n  enabled: false\nmodelRoles:\n  smol: mock/mock-2\n";
    fs_write_atomic(pcfg, pyml, strlen(pyml), 0644);
    settings_init(proj, NULL);
    CHECK(!settings_bool("fetch.enabled", true) && !strcmp(settings_provenance("fetch.enabled"), "project"));
    char *gcfg = xasprintf("%s/config.yml", dir);
    fs_write_atomic(gcfg, "disabledProviders:\n  - native\n", 30, 0644);
    settings_init(proj, NULL);
    CHECK(settings_bool("fetch.enabled", true) && !strcmp(settings_provenance("fetch.enabled"), "default"));
    CHECK(str_eq(json_get_str(json_get(settings_project(), "modelRoles"), "smol"), "mock/mock-2"));
    unlink(gcfg);
    pyml = "fetch:\n  enabled: false\ndisabledProviders: [native]\n";
    fs_write_atomic(pcfg, pyml, strlen(pyml), 0644);
    settings_init(proj, NULL);
    CHECK(settings_bool("fetch.enabled", true) && !strcmp(settings_provenance("fetch.enabled"), "default"));
    json_t *dp = settings_get("disabledProviders");
    CHECK(json_len(dp) == 1 && str_eq(json_sv(json_at(dp, 0)), "native"));
    free(proj), free(pdir), free(pcfg), free(gcfg);

    { /* a dotted revision spelling adopts the catalog id (omp 8ddc3e51ce, e8ff8d8f25) */
        const ai_model_t *dashed = NULL;
        for (size_t i = 0; i < catalog_count() && !dashed; i++) {
            const ai_model_t *m = catalog_at(i);
            if (m && !strcmp(m->provider, "anthropic") && strstr(m->id, "-4-5") && !strchr(m->id, '.')) dashed = m;
        }
        CHECK(dashed != NULL);
        if (dashed) {
            char *dotted = xstrdup(dashed->id), *p = strstr(dotted, "-4-5");
            p[2] = '.';
            char *sel = xasprintf("anthropic/%s", dotted);
            mr_selector_t s;
            CHECK(mr_parse_selector(sel, &s) && !strcmp(s.id, dashed->id) && !strcmp(s.raw, sel));
            mr_selector_free(&s);
            free(sel);
            free(dotted);
        }
    }
    mr_pool_free(&avail);
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("model resolver ok\n");
    return fails ? 1 : 0;
}
