/* test_catalog.c - bundled catalog data (omp v18.14.2: providers, role-default chains, keyless built-ins, model
 * kinds) and the model_perf aggregates / cell formatting. */
#include "ai/ai.h"
#include "core/fs.h"

#include <stdio.h>
#include <stdlib.h>

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

    fs_rm_rf(dir);
    if (fails) return 1;
    printf("catalog: ok\n");
    return 0;
}
