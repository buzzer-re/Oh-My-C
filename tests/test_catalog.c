/* test_catalog.c - bundled catalog data (omp v18.14.2: providers, role-default chains, keyless built-ins, model
 * kinds), the model_perf aggregates / cell formatting, and the runtime policy of custom models (compat_detect.c:
 * expected values are omp v18.14.2's get_available_models output for the same models.yml entries). */
#include "ai/ai.h"
#include "ai/compat_detect.h"
#include "core/fs.h"
#include "tui/ansi.h"
#include "tui/symbols.h"
#include "tui/theme.h"
#include "ui/modelbrowser.h"

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

/* compat_resolve_model on a models.yml entry; returns the resolved object (owned) */
static json_t *resolved(const char *provider, const char *base, const char *model_json) {
    json_t *m = json_parse_str(model_json);
    json_set_str(m, "provider", provider);
    json_set_str(m, "baseUrl", base);
    if (!json_get(m, "api")) json_set_str(m, "api", "openai-completions");
    compat_resolve_model(m);
    return m;
}

static void check_json(const json_t *v, const char *want, const char *what) {
    char *got = v ? json_dump(v, 0) : xstrdup("(none)");
    if (strcmp(got, want)) {
        fprintf(stderr, "FAIL %s: got %s want %s\n", what, got, want);
        fails++;
    }
    free(got);
}

static void test_custom_policy(void) {
    /* the user's local Qwen: loopback, legacy thinking range */
    json_t *m = resolved("splash", "http://127.0.0.1:9/v1",
                         "{\"id\":\"qwen3.6-27b\",\"reasoning\":true,\"input\":[\"text\",\"image\"],"
                         "\"thinking\":{\"mode\":\"effort\",\"minLevel\":\"low\",\"maxLevel\":\"high\"}}");
    json_t *c = json_get(m, "compat");
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"low\",\"medium\",\"high\"]}", "qwen thinking");
    check_json(json_get(m, "identity"), "{\"class\":\"qwen\",\"revision\":\"3.6.0\"}", "qwen identity");
    CHECK(!json_get_bool(c, "supportsStore", true) && str_eq(json_get_str(c, "thinkingFormat"), "qwen"));
    CHECK(str_eq(json_get_str(c, "reasoningDisableMode"), "qwen-enable-thinking-false"));
    CHECK(json_get_bool(c, "qwenPreserveThinking", false) && json_get_bool(c, "replayReasoningContent", false));
    CHECK(json_get_bool(c, "clampOutputToModelMax", false) && str_eq(json_get_str(c, "toolSchemaFlavor"), "grammar"));
    CHECK(json_get_int(c, "streamFirstEventTimeoutMs", -1) == 0 && json_get_int(c, "streamIdleTimeoutMs", 0) == 300000);
    json_free(m);
    /* the same id on a remote host: Qwen dialect, no local-backend knobs */
    m = resolved("remote", "https://api.example.com/v1", "{\"id\":\"qwen3.6-27b\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(str_eq(json_get_str(c, "thinkingFormat"), "qwen") && !json_get_bool(c, "qwenPreserveThinking", true));
    CHECK(!json_get(c, "toolSchemaFlavor") && !json_get(c, "streamIdleTimeoutMs"));
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"]}", "remote qwen thinking");
    json_free(m);
    /* an unclassified reasoning model on the LAN: OpenAI dialect, ladder up to xhigh */
    m = resolved("lan", "http://192.168.1.20:8080/v1", "{\"id\":\"my-model\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(json_get_bool(c, "supportsStore", false) && str_eq(json_get_str(c, "reasoningDisableMode"), "lowest-effort"));
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\",\"xhigh\"]}", "unknown thinking");
    json_free(m);
    /* models.yml compat overrides win; the disable mode follows the overridden dialect */
    m = resolved("splash", "http://localhost:9/v1", "{\"id\":\"qwen3.6-27b\",\"reasoning\":true,\"compat\":{"
                                                  "\"supportsStore\":true,\"thinkingFormat\":\"openai\",\"junk\":1}}");
    c = json_get(m, "compat");
    CHECK(json_get_bool(c, "supportsStore", false) && str_eq(json_get_str(c, "reasoningDisableMode"), "lowest-effort"));
    CHECK(json_get_bool(c, "qwenPreserveThinking", false) && !json_get(c, "junk"));
    json_free(m);
    /* llama.cpp provider rules (providers/llama.cpp.kdl), pre- and post-3.8 Qwen templates */
    m = resolved("llama.cpp", "http://127.0.0.1:8080/v1", "{\"id\":\"qwen3.6-27b\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(!json_get_bool(c, "supportsReasoningEffort", true) && json_get_bool(c, "omitReasoningEffort", false));
    CHECK(!json_get_bool(c, "supportsNamedToolChoice", true));
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"high\"]}", "llama.cpp qwen thinking");
    json_free(m);
    m = resolved("lm-studio", "http://127.0.0.1:1234/v1", "{\"id\":\"qwen/qwen3.8-27b\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(json_get_bool(c, "qwenTemplateReasoningEffort", false) && str_eq(json_get_str(c, "thinkingFormat"), "qwen"));
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"low\",\"medium\",\"xhigh\"],\"requiresEffort\":true}",
               "lm-studio qwen 3.8 thinking");
    json_free(m);
    m = resolved("vllm", "http://10.0.0.5:8000/v1", "{\"id\":\"Qwen/Qwen3.6-27B\",\"reasoning\":true}");
    CHECK(str_eq(json_get_str(json_get(m, "compat"), "reasoningDisableMode"), "qwen-template-false"));
    json_free(m);
    /* lineage contracts: DeepSeek reasoning content, Kimi, gpt-oss ladder, distilled Qwen, non-reasoning models */
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"deepseek-v3.2\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(json_get_bool(c, "requiresReasoningContentForAllAssistantTurns", false) && json_get_bool(c, "stripImageInput", false));
    CHECK(str_eq(json_get_str(c, "streamMarkupHealingPattern"), "dsml") && json_get_bool(c, "disableReasoningOnToolChoice", false));
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"high\",\"max\"]}", "deepseek thinking");
    check_json(json_get(m, "identity"), "{\"class\":\"deepseek\",\"family\":\"v3\",\"revision\":\"3.2.0\"}", "deepseek identity");
    json_free(m);
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"kimi-k2.6\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(str_eq(json_get_str(c, "toolSchemaFlavor"), "moonshot-mfjs") && str_eq(json_get_str(c, "streamMarkupHealingPattern"), "kimi"));
    CHECK(json_get_bool(c, "alwaysSendMaxTokens", false) && str_eq(json_get_str(c, "streamRevision"), "possible"));
    json_free(m);
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"gpt-oss-120b\",\"reasoning\":true}");
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"low\",\"medium\",\"high\"]}", "gpt-oss thinking");
    check_json(json_get(m, "identity"), "{\"class\":\"gpt-oss\",\"family\":\"120b\"}", "gpt-oss identity");
    json_free(m);
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"deepseek-r1-distill-qwen-32b\",\"reasoning\":true}");
    check_json(json_get(m, "identity"), "{\"class\":\"qwen\"}", "distill identity");
    json_free(m);
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"gemma-3-27b-it\",\"thinking\":{\"mode\":\"effort\",\"efforts\":[\"low\"]}}");
    CHECK(!json_get(m, "thinking"));
    check_json(json_get(m, "identity"), "{\"class\":\"gemma\",\"revision\":\"3.0.0\"}", "gemma identity");
    json_free(m);
    /* the rule cascade reaches a custom id: DeepSeek keeps text-only input unless the id names vision (classes/deepseek.kdl) */
    m = resolved("splash", "http://127.0.0.1:9/v1", "{\"id\":\"deepseek-ocr\",\"reasoning\":true}");
    CHECK(!json_get_bool(json_get(m, "compat"), "stripImageInput", true));
    json_free(m);
    /* an Anthropic-wire proxy: signed thinking off the official host, Responses endpoint records */
    m = resolved("ant", "https://claude-proxy.example.com", "{\"id\":\"not-claude\",\"api\":\"anthropic-messages\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(!json_get_bool(c, "officialEndpoint", true) && !json_get_bool(c, "signingEndpoint", true) && json_get_bool(c, "replayUnsignedThinking", false));
    json_free(m);
    m = resolved("resp", "http://127.0.0.1:9/v1", "{\"id\":\"local-resp\",\"api\":\"openai-responses\",\"reasoning\":true}");
    c = json_get(m, "compat");
    CHECK(json_get_bool(c, "clampOutputToModelMax", false) && json_get_int(c, "streamIdleTimeoutMs", 0) == 300000);
    json_free(m);
    /* a custom entry inherits what it leaves out from its bundled reference, explicit values win */
    compat_ref_index_t *rx = compat_ref_index_new();
    json_t *row = json_parse_str("{\"id\":\"vendor/qwen9.9-1b\",\"provider\":\"bundled\",\"contextWindow\":777,\"maxTokens\":55,\"reasoning\":true,"
                                 "\"input\":[\"text\",\"image\"],\"cost\":{\"input\":1,\"output\":2,\"cacheRead\":0,\"cacheWrite\":0},"
                                 "\"compatConfig\":{\"supportsDeveloperRole\":true},\"thinking\":{\"mode\":\"effort\",\"efforts\":[\"low\"]}}");
    compat_ref_index_add(rx, row);
    compat_ref_index_finish(rx);
    CHECK(compat_ref_lookup(rx, "vendor/qwen9.9-1b") == row && compat_ref_lookup(rx, "[Kiro] qwen9.9-1b:cloud") == row);
    CHECK(compat_ref_lookup(rx, "other/QWEN9.9-1B-free") == row && !compat_ref_lookup(rx, "@gw/qwen9.9-1b") && !compat_ref_lookup(rx, "unrelated"));
    m = json_parse_str("{\"id\":\"qwen9.9-1b\",\"provider\":\"splash\",\"api\":\"openai-completions\",\"baseUrl\":\"http://127.0.0.1:9/v1\",\"maxTokens\":9}");
    compat_finalize_custom(m, compat_ref_lookup(rx, "qwen9.9-1b"));
    CHECK(json_get_int(m, "contextWindow", 0) == 777 && json_get_int(m, "maxTokens", 0) == 9 && json_get_bool(m, "reasoning", false));
    CHECK(json_get_num(json_get_obj(m, "cost"), "output", 0) == 2 && json_len(json_get_arr(m, "input")) == 2);
    CHECK(json_get_bool(json_get(m, "compat"), "supportsDeveloperRole", false));
    /* thinking comes along only from a same-provider reference */
    check_json(json_get(m, "thinking"), "{\"mode\":\"effort\",\"efforts\":[\"minimal\",\"low\",\"medium\",\"high\"]}", "reference thinking");
    json_free(m);
    /* no reference: omp's defaults */
    m = json_parse_str("{\"id\":\"plain\",\"provider\":\"splash\",\"api\":\"openai-completions\",\"baseUrl\":\"https://x.example.com/v1\",\"name\":\"Acme: Plain (latest)\"}");
    compat_finalize_custom(m, NULL);
    CHECK(json_get_int(m, "contextWindow", 0) == 128000 && json_get_int(m, "maxTokens", 0) == 16384 && !json_get_bool(m, "reasoning", true));
    CHECK(str_eq(json_get_str(m, "name"), "Plain"));
    json_free(m);
    json_free(row);
    /* loopback / private ranges */
    CHECK(compat_local_base_url("http://localhost:8080/v1") && compat_local_base_url("http://[::1]:8080"));
    CHECK(compat_local_base_url("http://172.20.0.2/v1") && compat_local_base_url("http://box.local./v1"));
    CHECK(!compat_local_base_url("http://172.32.0.2/v1") && !compat_local_base_url("https://api.openai.com/v1"));
    CHECK(!compat_local_base_url("localhost:8080"));
    /* grammar flavor: bare boolean subschemas widen, boolean additionalProperties stays */
    json_t *sch = json_parse_str("{\"type\":\"object\",\"properties\":{\"a\":true,\"b\":{\"type\":\"array\",\"items\":false}},"
                                 "\"additionalProperties\":false}");
    json_t *g = compat_schema_for_grammar(sch);
    check_json(g, "{\"type\":\"object\",\"properties\":{\"a\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"number\"},"
                  "{\"type\":\"boolean\"},{\"type\":\"object\"},{\"type\":\"array\"},{\"type\":\"null\"}]},\"b\":{\"type\":"
                  "\"array\",\"items\":{\"not\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"number\"},{\"type\":\"boolean\"},"
                  "{\"type\":\"object\"},{\"type\":\"array\"},{\"type\":\"null\"}]}}}},\"additionalProperties\":false}",
               "grammar schema");
    json_free(g);
    json_free(sch);
}

static void check_cell(const char *provider, const char *id, bool full, const char *want) {
    char *c = model_perf_cell(catalog_find(provider, id), full);
    if (strcmp(c, want)) {
        fprintf(stderr, "FAIL perf cell %s/%s: got '%s' want '%s'\n", provider, id, c, want);
        fails++;
    }
    free(c);
}

/* omp d174e191..b918fe76: the Standard Credits rate in force, from dated `credit-rates` cards */
static void test_credit_rates(void) {
    const ai_model_t *sol = catalog_find("factory-droid", "gpt-5.6-sol");
    CHECK(sol != NULL);
    if (!sol) return;
    ai_credit_rate_t cr;
    int64_t aug21 = 1787270400000LL, nov5 = 1793836800000LL, nov22 = 1795305600000LL; /* 2026, UTC */
    CHECK(ai_factory_droid_credit_rate(sol, nov5, &cr));
    CHECK(cr.rate == 1.6 && cr.has_next && cr.next_rate == 2 && cr.next_at == nov22);
    /* passing the boundary: the standard rate, nothing further */
    CHECK(ai_factory_droid_credit_rate(sol, nov22, &cr));
    CHECK(cr.rate == 2 && !cr.has_next);
    /* before any card: the base rate; the promotion is the next change */
    CHECK(ai_factory_droid_credit_rate(sol, aug21, &cr));
    CHECK(cr.rate == 2 && cr.has_next && cr.next_rate == 1.6);
    /* a model without dated cards keeps its base rate */
    const ai_model_t *opus = catalog_find("factory-droid", "claude-opus-5-5");
    CHECK(opus && ai_factory_droid_credit_rate(opus, nov5, &cr) && cr.rate == 1.6 && !cr.has_next);
    /* no credit rate at all */
    const ai_model_t *other = catalog_find("anthropic", "claude-opus-5-5");
    CHECK(other && !ai_factory_droid_credit_rate(other, nov5, &cr));
    /* the row shows the badge beside the dollar reference; a credit-charged model is never `free` */
    tui_theme_set(tui_theme_builtin("titanium", TUI_COLOR_TRUECOLOR));
    tui_sym_set_preset("unicode");
    ui_model_browser_t *b = ui_model_browser_new();
    ui_model_browser_add(b, opus);
    ui_model_browser_done_adding(b);
    strvec_t rows = {0};
    ui_model_browser_render(b, 140, &rows);
    bool badge = false;
    for (size_t i = 0; i < rows.n; i++) {
        char *p = tui_strip_ansi(rows.v[i]);
        badge |= strstr(p, "$4/20 1.6\xc3\x97") != NULL;
        free(p);
    }
    CHECK(badge);
    CHECK(!ui_model_is_free(opus));
    strvec_free(&rows);
    ui_model_browser_free(b);
}

/* omp e66cb977c7: Claude Fable / Mythos 5.2, discovered-row repairs, Mythos 5.1+ prefix binding, slow-role default */
static void test_fable_52(void) {
    const ai_model_t *f = catalog_find("anthropic", "claude-fable-5-2"), *my = catalog_find("anthropic", "claude-mythos-5-2");
    CHECK(f && my);
    /* /v1/models reports a not-yet-catalogued Fable/Mythos row text-only, non-reasoning and unpriced */
    const char *ids[] = {"claude-fable-5-2", "claude-mythos-5-2"};
    for (size_t i = 0; i < 2; i++) {
        char *js = xasprintf("{\"id\":\"%s\",\"api\":\"anthropic-messages\",\"reasoning\":false,\"input\":[\"text\"],"
                             "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0}}",
                             ids[i]);
        json_t *m = resolved("anthropic", "https://api.anthropic.com", js);
        free(js);
        CHECK(json_get_bool(m, "reasoning", false));
        check_json(json_get(m, "input"), "[\"text\",\"image\"]", ids[i]);
        const json_t *c = json_get_obj(m, "cost");
        CHECK(json_get_num(c, "input", 0) == 8 && json_get_num(c, "output", 0) == 40);
        CHECK(json_get_num(c, "cacheRead", 0) == 0.2 && json_get_num(c, "cacheWrite", 0) == 10);
        CHECK(json_get_int(m, "contextWindow", 0) == 1000000 && json_get_int(m, "maxTokens", 0) == 128000);
        CHECK(json_get_bool(json_get_obj(m, "thinking"), "prefixBinding", false));
        json_free(m);
    }
    /* Mythos 5.1 binds the thinking prefix on every account */
    json_t *m = resolved("anthropic", "https://api.anthropic.com",
                         "{\"id\":\"claude-mythos-5-1\",\"api\":\"anthropic-messages\",\"reasoning\":true}");
    CHECK(json_get_bool(json_get_obj(m, "thinking"), "prefixBinding", false));
    json_free(m);
    /* the slow role prefers Fable 5.2 right after the Codex Sol rows */
    json_t *slow = json_get_arr(catalog_role_defaults(), "slow");
    CHECK(str_eq(json_sv(json_at(slow, 2)), "anthropic/claude-fable-5-2"));
}

/* omp 503d43fbb5: a models.yml `compat.disabledThinking` on an anthropic-messages model is kept */
static void test_anthropic_disabled_thinking(void) {
    json_t *m = resolved("claude", "https://api.anthropic.com",
                         "{\"id\":\"claude-haiku-5-5\",\"api\":\"anthropic-messages\",\"reasoning\":true,"
                         "\"compat\":{\"disabledThinking\":\"disabled\"}}");
    CHECK(str_eq(json_get_str(json_get(m, "compat"), "disabledThinking"), "disabled"));
    json_free(m);
    /* only the Haiku 5.5 route rule assigns one: other Claude models keep the API's Off handling */
    m = resolved("anthropic", "https://api.anthropic.com", "{\"id\":\"claude-opus-5\",\"api\":\"anthropic-messages\",\"reasoning\":true}");
    CHECK(!json_get(json_get(m, "compat"), "disabledThinking"));
    json_free(m);
    m = resolved("anthropic", "https://api.anthropic.com", "{\"id\":\"claude-haiku-5-5\",\"api\":\"anthropic-messages\",\"reasoning\":true}");
    CHECK(str_eq(json_get_str(json_get(m, "compat"), "disabledThinking"), "disabled"));
    json_free(m);
}

/* catalog axes resolve at runtime for bundled rows */
static void test_catalog_policy(void) {
    const json_t *max = compat_catalog_policy("muse-code", "openai-responses", "muse-spark-1.3", "toolCallNameMaxLength");
    CHECK(json_is_num(max) && (max->is_int ? max->u.num.i : (int64_t)max->u.num.d) == 64);
    const json_t *ws = compat_catalog_policy("openai", "openai-responses", "gpt-5.5", "supportsWebsockets");
    CHECK(json_is_bool(ws) && ws->u.b);
    CHECK(compat_catalog_policy("openai", "openai-completions", "gpt-5.5", "supportsWebsockets") == NULL);
    CHECK(compat_catalog_policy("openai-codex", "openai-codex-responses", "gpt-5.5", "supportsWebsockets") == NULL);
    CHECK(catalog_find("factory-droid", "muse-spark-1.3") && catalog_find("factory-droid", "gemini-4-argon"));
}

int main(void) {
    char tmpl[] = "/tmp/omc-test-catalog.XXXXXX";
    char *dir = mkdtemp(tmpl);
    setenv("OMC_CODING_AGENT_DIR", dir, 1);
    setenv("OMC_NO_LOCAL_DISCOVERY", "1", 1);

    test_credit_rates();
    test_catalog_policy();
    test_fable_52();
    test_anthropic_disabled_thinking();

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

    test_custom_policy();

    fs_rm_rf(dir);
    if (fails) return 1;
    printf("catalog: ok\n");
    return 0;
}
