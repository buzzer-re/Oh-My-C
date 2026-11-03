/* test_catalog.c - bundled catalog data (omp v18.14.2: providers, role-default chains, keyless built-ins, model
 * kinds) and the model_perf aggregates / cell formatting. */
#include "ai/ai.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static void check_cell(const char *provider, const char *id, bool full, const char *want) {
    char *c = model_perf_cell(catalog_find(provider, id), full);
    if (strcmp(c, want)) {
        fprintf(stderr, "FAIL perf cell %s/%s: got '%s' want '%s'\n", provider, id, c, want);
        fails++;
    }
    free(c);
}

int main(void) {
    char tmpl[] = "/tmp/omc-test-catalog.XXXXXX";
    char *dir = mkdtemp(tmpl);
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("OMC_NO_LOCAL_DISCOVERY", "1", 1);

    /* providers present in omp's catalog but absent from its bundled rows */
    CHECK(catalog_find("charm-hyper", "glm-5.3") || catalog_find("charm-hyper", "deepseek-v4-flash-0731"));
    bool droid = false;
    for (size_t i = 0; i < catalog_count(); i++)
        if (str_eq(catalog_at(i)->provider, "factory-droid")) droid = true;
    CHECK(droid);

    /* built-in local / web pseudo-providers: keyless, with omp's kinds */
    CHECK(catalog_provider_keyless("local") && catalog_provider_keyless("web") && catalog_provider_keyless("ollama"));
    CHECK(!catalog_provider_keyless("anthropic"));
    const ai_model_t *kokoro = catalog_find("local", "kokoro"), *par = catalog_find("local", "parakeet-tdt-0.6b-v3");
    CHECK(kokoro && str_eq(catalog_model_kind(kokoro), "tts") && catalog_model_available(kokoro));
    CHECK(par && str_eq(catalog_model_kind(par), "stt"));
    CHECK(str_eq(catalog_model_kind(catalog_find("local", "lfm2.5-230m")), "tiny"));
    CHECK(str_eq(catalog_model_kind(catalog_find("web", "duckduckgo")), "search"));
    CHECK(str_eq(catalog_model_kind(catalog_find("anthropic", "claude-opus-4-8")), "chat"));
    CHECK(!json_get_bool(catalog_provider_info("apple"), "automaticDefault", true));

    /* role-default chains (behavior.kdl role-defaults) */
    json_t *rd = catalog_role_defaults();
    CHECK(str_eq(json_sv(json_at(json_get_arr(rd, "smol"), 0)), "claude-haiku-5-5"));
    CHECK(str_eq(json_sv(json_at(json_get_arr(rd, "web"), 0)), "web/parallel"));
    CHECK(str_eq(json_sv(json_at(json_get_arr(rd, "judge"), 2)), "@tiny"));
    CHECK(json_get_arr(rd, "slow") && json_get_arr(rd, "image") && json_get_arr(rd, "speech") &&
          json_get_arr(rd, "dictation"));

    /* model_perf: tps over total duration, ttft only when 0 < ttft < duration */
    double tps, ttft, n;
    CHECK(!model_perf_get("anthropic/claude-opus-4-8", &tps, &ttft, &n));
    check_cell("anthropic", "claude-opus-4-8", true, "");
    model_perf_record("anthropic/claude-opus-4-8", 500, 4000, 800);
    model_perf_record("anthropic/claude-opus-4-8", 100, 1000, 5000); /* bogus ttft ignored */
    CHECK(model_perf_get("anthropic/claude-opus-4-8", &tps, &ttft, &n));
    CHECK(n == 2 && tps == 120.0 && ttft == 800.0);
    check_cell("anthropic", "claude-opus-4-8", true, "0.8s 120t/s");
    check_cell("anthropic", "claude-opus-4-8", false, "120t/s");
    model_perf_record("openai/gpt-5.5@priority", 90, 10000, 12000); /* only the tier row: fastest tier shown */
    check_cell("openai", "gpt-5.5", true, "9.0t/s priority");
    json_t *msg = json_parse_str("{\"role\":\"assistant\",\"provider\":\"xai\",\"model\":\"grok-4.5\",\"stopReason\":\"stop\","
                                 "\"usage\":{\"output\":42},\"duration\":2000,\"ttft\":12000}");
    model_perf_record_message(msg);
    json_set_str(msg, "stopReason", "error");
    model_perf_record_message(msg); /* errored turns measure nothing */
    json_free(msg);
    CHECK(model_perf_get("xai/grok-4.5", &tps, &ttft, &n) && n == 1 && tps == 21.0 && ttft < 0);
    check_cell("xai", "grok-4.5", true, "21t/s");
    /* decay: past 256 samples every new one halves the sums first */
    for (int i = 0; i < 257; i++) model_perf_record("k/decay", 10, 1000, 0);
    CHECK(model_perf_get("k/decay", &tps, &ttft, &n) && n == 129 && tps == 10.0);
    /* persisted for other sessions */
    char *path = omc_agent_path("model-perf.json");
    CHECK(fs_is_file(path));
    free(path);

    /* models.yml edited mid-session (ModelRegistry.refreshIfStale): a new provider appears, legacy
     * minLevel/maxLevel thinking normalizes to efforts, removed models retire, unchanged config is a no-op */
    char *yml = omc_agent_path("models.yml");
    const char *v1 = "providers:\n  splash:\n    baseUrl: http://127.0.0.1:8000/v1\n    api: openai-completions\n"
                     "    apiKey: K1\n    models:\n      - id: org/Model-gguf:PQ2_0\n        reasoning: true\n"
                     "        thinking:\n          minLevel: low\n          maxLevel: xhigh\n          mode: effort\n"
                     "      - id: other\n";
    CHECK(!catalog_find("splash", "org/Model-gguf:PQ2_0"));
    fs_write(yml, v1, strlen(v1));
    CHECK(catalog_refresh_if_stale());
    const ai_model_t *sm = catalog_find("splash", "org/Model-gguf:PQ2_0");
    CHECK(sm && str_eq(sm->base_url, "http://127.0.0.1:8000/v1"));
    CHECK(sm && !model_supports_effort(sm, EFFORT_MINIMAL) && model_supports_effort(sm, EFFORT_LOW) &&
          model_supports_effort(sm, EFFORT_XHIGH) && !model_supports_effort(sm, EFFORT_MAX));
    CHECK(catalog_resolve("splash/org/Model-gguf:PQ2_0") == sm);
    CHECK(str_eq(json_get_str(catalog_provider_info("splash"), "configApiKey"), "K1"));
    CHECK(!catalog_refresh_if_stale()); /* unchanged */
    const char *v2 = "providers:\n  splash:\n    baseUrl: http://127.0.0.1:8001/v1\n    api: openai-completions\n"
                     "    apiKey: K2\n    models:\n      - id: org/Model-gguf:PQ2_0\n";
    fs_write(yml, v2, strlen(v2));
    struct timespec ts[2] = {{0, UTIME_OMIT}, {time(NULL) + 5, 0}}; /* force a different mtime */
    utimensat(AT_FDCWD, yml, ts, 0);
    CHECK(catalog_refresh_if_stale());
    CHECK(catalog_find("splash", "org/Model-gguf:PQ2_0") == sm && str_eq(sm->base_url, "http://127.0.0.1:8001/v1"));
    CHECK(!catalog_find("splash", "other")); /* removed from models.yml */
    CHECK(str_eq(json_get_str(catalog_provider_info("splash"), "configApiKey"), "K2"));
    free(yml);

    fs_rm_rf(dir);
    if (fails) return 1;
    printf("catalog: ok\n");
    return 0;
}
