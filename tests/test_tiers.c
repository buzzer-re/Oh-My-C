/* test_tiers.c - WP-14 provider knobs: service-tier families and wire gating (pi-ai types.ts), catalog
 * context-window tiers, cache-warmer timing/tier/replay rules, Anthropic slow-mode headers and lane state machine,
 * the per-family tier map's session JSON. */
#include "ai/ai.h"
#include "ai/anthropic_slow.h"
#include "coding/cachewarm.h"
#include "coding/modelctl.h"

#include "ai/provider_openai.h"
#include <stdio.h>
#include <time.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the Chat Completions reasoning fields omc sends for a bundled row (upstream modular / scaleway provider tests) */
static void catalog_reasoning_fields(const char *provider, const char *id, ai_effort_t effort, char **out) {
    *out = NULL;
    const ai_model_t *m = catalog_find(provider, id);
    CHECK(m != NULL);
    if (!m) return;
    json_t *msgs = json_parse_str("[{\"role\":\"user\",\"content\":\"hi\"}]");
    ai_context_t ctx = {.messages = msgs};
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = effort;
    o.disable_reasoning = effort == EFFORT_OFF;
    json_t *p = oaic_build_request(m, &ctx, &o), *f = json_obj();
    if (json_get(p, "reasoning")) json_set(f, "reasoning", json_clone(json_get(p, "reasoning")));
    if (json_get(p, "reasoning_effort")) json_set(f, "reasoning_effort", json_clone(json_get(p, "reasoning_effort")));
    *out = json_dump(f, 0);
    json_free(f);
    json_free(p);
    json_free(msgs);
}

static void test_catalog_new_providers(void) {
    static const struct {
        const char *provider, *id;
        ai_effort_t effort;
        const char *want;
    } cases[] = {
        {"modular", "MiniMaxAI/MiniMax-M3.1-Flash", EFFORT_HIGH, "{\"reasoning\":{\"effort\":\"high\"}}"},
        {"modular", "minimax/minimax-m3", EFFORT_HIGH, "{\"reasoning\":{\"enabled\":true}}"},
        {"modular", "minimax/minimax-m3", EFFORT_OFF, "{\"reasoning\":{\"enabled\":false}}"},
        {"modular", "google/gemma-4-31b-it", EFFORT_HIGH, "{\"reasoning\":{\"enabled\":true}}"},
        {"modular", "google/gemma-4-31b-it", EFFORT_OFF, "{\"reasoning\":{\"enabled\":false}}"},
        {"scaleway", "glm-5.3", EFFORT_HIGH, "{\"reasoning_effort\":\"high\"}"},
        {"scaleway", "glm-5.3", EFFORT_OFF, "{\"reasoning_effort\":\"none\"}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *got = NULL;
        catalog_reasoning_fields(cases[i].provider, cases[i].id, cases[i].effort, &got);
        if (got && strcmp(got, cases[i].want))
            fprintf(stderr, "  %s/%s effort %d: got %s, want %s\n", cases[i].provider, cases[i].id, (int)cases[i].effort, got, cases[i].want);
        CHECK(str_eq(got, cases[i].want));
        free(got);
    }
    CHECK(str_eq(json_get_str(catalog_provider_info("modular"), "defaultModel"), "google/gemma-4-31b-it"));
    CHECK(str_eq(json_get_str(catalog_provider_info("scaleway"), "defaultModel"), "glm-5.3"));
    setenv("SCW_API_KEY", "legacy-key", 1); /* the legacy env name still resolves */
    char *key = ai_env_api_key("scaleway");
    CHECK(str_eq(key, "legacy-key"));
    free(key);
    unsetenv("SCW_API_KEY");
}

static void test_families(void) {
    const ai_model_t *gpt = catalog_find("openai", "gpt-5.2"), *opus = catalog_find("anthropic", "claude-opus-4-6");
    const ai_model_t *gem = catalog_find("google", "gemini-2.5-pro");
    CHECK(gpt && opus && gem);
    if (!gpt || !opus || !gem) return;
    CHECK(str_eq(ai_service_tier_family(gpt), "openai"));
    CHECK(str_eq(ai_service_tier_family(opus), "anthropic"));
    CHECK(str_eq(ai_service_tier_family(gem), "google"));
    CHECK(ai_should_send_service_tier("priority", gpt) && ai_should_send_service_tier("flex", gpt));
    CHECK(ai_should_send_service_tier("ultrafast", gpt));
    CHECK(!ai_should_send_service_tier("auto", gpt) && !ai_should_send_service_tier(NULL, gpt));
    CHECK(!ai_should_send_service_tier("priority", opus)); /* realized as speed: fast instead */
    CHECK(ai_realizes_priority_tier("priority", opus) && !ai_realizes_priority_tier("flex", opus));
    CHECK(ai_should_send_service_tier("flex", gem) && !ai_should_send_service_tier("scale", gem));
    CHECK(ai_service_tier_for_family("anthropic", "priority") && !ai_service_tier_for_family("anthropic", "flex"));
    CHECK(!ai_service_tier_for_family("openai", "none"));
    ai_service_tiers_t t = {.openai = "priority", .google = "flex"};
    CHECK(str_eq(ai_resolve_model_service_tier(&t, gpt), "priority"));
    CHECK(ai_resolve_model_service_tier(&t, opus) == NULL);
    CHECK(str_eq(ai_resolve_model_service_tier(&t, gem), "flex"));
    CHECK(ai_fireworks_fast_model_id("glm-5.2-fast") && !ai_fireworks_fast_model_id("glm-5.2"));
    /* Ultrafast: first-party OpenAI as-is, a relay on the OpenAI-family APIs only when it advertises it, a bare
     * provider string for the provider alone */
    json_t *relay_raw = json_parse_str("{\"serviceTiers\":[\"priority\",\"ultrafast\"]}");
    json_t *plain_raw = json_parse_str("{\"serviceTiers\":[\"priority\"]}");
    ai_model_t relay = {.id = "gpt-6-sol", .provider = "my-relay", .api = "openai-responses", .raw = relay_raw};
    CHECK(ai_should_send_service_tier("ultrafast", &relay));
    CHECK(str_eq(ai_service_tier_family(&relay), "openai"));
    ai_model_t relay_comp = {.id = "gpt-6-sol", .provider = "my-relay", .api = "openai-completions", .raw = relay_raw};
    CHECK(ai_should_send_service_tier("ultrafast", &relay_comp));
    ai_model_t silent = {.id = "gpt-6-sol", .provider = "my-relay", .api = "openai-responses", .raw = plain_raw};
    CHECK(!ai_should_send_service_tier("ultrafast", &silent));
    CHECK(ai_should_send_service_tier("priority", &silent));
    ai_model_t other = {.id = "llama-4", .provider = "my-relay", .api = "openai-responses", .raw = relay_raw};
    CHECK(!ai_should_send_service_tier("ultrafast", &other)); /* not OpenAI-family */
    ai_model_t codex = {.id = "gpt-6-sol", .provider = "openai-codex", .api = "openai-codex-responses", .raw = plain_raw};
    CHECK(!ai_should_send_service_tier("ultrafast", &codex));
    CHECK(ai_should_send_service_tier_for("ultrafast", "openai-codex", NULL));
    CHECK(ai_should_send_service_tier_for("ultrafast", "openai", NULL));
    CHECK(!ai_should_send_service_tier_for("ultrafast", "my-relay", NULL));
    json_free(relay_raw);
    json_free(plain_raw);
    for (size_t i = 0; i < catalog_count(); i++) { /* Copilot proxies OpenAI models but rejects service_tier */
        const ai_model_t *m = catalog_at(i);
        if (str_eq(m->provider, "github-copilot") && str_starts(m->id, "gpt-")) {
            CHECK(ai_service_tier_family(m) == NULL);
            break;
        }
    }
}

static void test_context_windows(void) {
    const ai_model_t *m = catalog_find("openai", "gpt-5.6");
    CHECK(m);
    if (!m) return;
    int64_t std = 0, ext = 0;
    CHECK(catalog_context_tiers(m, &std, &ext) && std == 272000 && ext == 1050000);
    catalog_apply_context_policy(false, NULL);
    CHECK(m->context_window == 272000);
    catalog_apply_context_policy(true, NULL);
    CHECK(m->context_window == 1050000);
    catalog_apply_context_policy(false, NULL);
    const ai_model_t *opus = catalog_find("anthropic", "claude-opus-4-6");
    CHECK(opus && !catalog_context_tiers(opus, NULL, NULL));
}

static void test_cachewarm_rules(void) {
    CHECK(cachewarm_delay_ms(300000) == 270000);
    CHECK(cachewarm_delay_ms(3600000) == 3240000);
    CHECK(cachewarm_delay_ms(12000) == 2000);
    CHECK(cachewarm_delay_ms(10000) == -1);
    const ai_model_t *opus = catalog_find("anthropic", "claude-opus-4-6");
    if (!opus) return;
    CHECK(str_eq(cachewarm_tier(opus, NULL, true), "long")); /* OAuth seats write 1h entries */
    CHECK(str_eq(cachewarm_tier(opus, NULL, false), "short"));
    CHECK(str_eq(cachewarm_tier(opus, "long", false), "long"));
    CHECK(cachewarm_tier(opus, "none", false) == NULL);
    ai_stream_opts_t o;
    ai_stream_opts_init(&o);
    o.reasoning = EFFORT_HIGH;
    CHECK(cachewarm_replayable(opus, &o)); /* adaptive thinking ignores the output cap */
    for (size_t i = 0; i < catalog_count(); i++) { /* a budget-thinking Claude cannot replay under max_tokens 1 */
        const ai_model_t *m = catalog_at(i);
        if (str_eq(m->provider, "anthropic") && m->reasoning && str_eq(json_get_str(m->thinking, "mode"), "budget")) {
            CHECK(!cachewarm_replayable(m, &o));
            o.reasoning = EFFORT_OFF;
            CHECK(cachewarm_replayable(m, &o));
            break;
        }
    }
}

static char g_notice[512];
static int g_notices;

static void on_notice(void *ud, const char *level, const char *message) {
    UNUSED(ud);
    snprintf(g_notice, sizeof g_notice, "%s: %s", level, message);
    g_notices++;
}

static bool g_low;
static bool low(void *ud) {
    UNUSED(ud);
    return g_low;
}

static void test_slow_mode(void) {
    anthropic_slow_reset_all();
    int64_t reset = (int64_t)time(NULL) + 3600;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)reset);
    http_headers_t h = {0};
    http_headers_set(&h, "anthropic-ratelimit-unified-slow-offer", "treatment");
    http_headers_set(&h, "anthropic-ratelimit-unified-reset", buf);
    http_headers_set(&h, "anthropic-ratelimit-unified-slow-retry-after", "30");
    anthropic_slow_signal_t sig;
    CHECK(anthropic_slow_parse(&h, &sig));
    CHECK(str_eq(sig.offer, "treatment") && (int64_t)sig.unified_reset_at == reset && sig.retry_after_ms == 30000);
    http_headers_free(&h);
    http_headers_t none = {0};
    http_headers_set(&none, "content-type", "text/event-stream");
    CHECK(!anthropic_slow_parse(&none, &sig));
    http_headers_free(&none);

    anthropic_slow_hooks_t hk = {low, on_notice, NULL, NULL};
    char *lane = anthropic_slow_lane_key("sk-ant-oat-test");
    CHECK(str_starts(lane, "key:"));
    bool cap = false;
    /* /slow off: the offer is recorded but not taken */
    g_low = false;
    http_headers_set(&h, "anthropic-ratelimit-unified-slow-offer", "treatment");
    http_headers_set(&h, "anthropic-ratelimit-unified-reset", buf);
    anthropic_slow_parse(&h, &sig);
    http_headers_free(&h);
    CHECK(anthropic_slow_on_failure(&hk, lane, 429, false, &sig, false, 0, 0, &cap) == -1);
    CHECK(anthropic_slow_active_resets_at(lane) == -1);
    /* /slow on accepts the recorded offer right away */
    g_low = true;
    anthropic_slow_accept(lane);
    CHECK(anthropic_slow_active_resets_at(lane) == reset && anthropic_slow_is_active(&hk, lane));
    char *label = anthropic_slow_label(lane, true);
    CHECK(label && str_starts(label, "low priority until "));
    free(label);
    /* a slot_busy 429 on a slow request: a server-paced capacity wait (20 s ±30%), first wait announced */
    anthropic_slow_signal_t busy = {.status = "slot_busy", .retry_after_ms = -1, .max_wait_ms = -1, .budget_utilization = -1,
                                    .budget_reset_at = -1, .unified_reset_at = -1, .five_hour_reset_at = -1, .weekly_reset_at = -1};
    g_notices = 0;
    int64_t d = anthropic_slow_on_failure(&hk, lane, 429, false, &busy, true, 0, 0, &cap);
    CHECK(cap && d >= 14000 && d <= 26000);
    CHECK(g_notices == 1 && strstr(g_notice, "Working at low priority"));
    /* waited past the max-wait budget: the lane pauses */
    d = anthropic_slow_on_failure(&hk, lane, 429, false, &busy, true, 1200000, 3, &cap);
    CHECK(d == -1 && anthropic_slow_active_resets_at(lane) == -1);
    CHECK(strstr(g_notice, "Low priority paused for 10 minutes"));
    /* /slow off stops an active window and keeps it resumable */
    anthropic_slow_reset_all();
    http_headers_set(&h, "anthropic-ratelimit-unified-slow-offer", "treatment");
    http_headers_set(&h, "anthropic-ratelimit-unified-reset", buf);
    anthropic_slow_parse(&h, &sig);
    http_headers_free(&h);
    g_notices = 0;
    CHECK(anthropic_slow_on_failure(&hk, lane, 429, false, &sig, false, 0, 0, &cap) == 0); /* auto-accept */
    CHECK(strstr(g_notice, "Claude session limit reached") && strstr(g_notice, "Run /slow off to stop."));
    CHECK(anthropic_slow_stop(lane, "user") && anthropic_slow_active_resets_at(lane) == -1);
    anthropic_slow_accept(lane);
    CHECK(anthropic_slow_active_resets_at(lane) == reset);
    free(lane);
    anthropic_slow_reset_all();
}

static void test_tier_json(void) {
    ai_service_tiers_t t = {.openai = "priority", .google = "flex"}, u;
    json_t *j = modelctl_tiers_json(&t);
    char *s = json_dump(j, 0);
    CHECK(str_eq(s, "{\"openai\":\"priority\",\"google\":\"flex\"}"));
    free(s);
    modelctl_tiers_from_json(j, &u);
    CHECK(str_eq(u.openai, "priority") && str_eq(u.google, "flex") && !u.anthropic);
    json_free(j);
    ai_service_tiers_t e = {0};
    j = modelctl_tiers_json(&e);
    CHECK(json_is_null(j));
    json_free(j);
    json_t *bad = json_parse_str("{\"anthropic\":\"flex\",\"openai\":\"ultrafast\",\"x\":\"priority\"}");
    modelctl_tiers_from_json(bad, &u); /* coerceServiceTierByFamily drops tiers a family cannot take */
    CHECK(!u.anthropic && str_eq(u.openai, "ultrafast") && !u.google);
    json_free(bad);
    modelctl_tiers_for_all("priority", &u);
    CHECK(str_eq(u.openai, "priority") && str_eq(u.anthropic, "priority") && str_eq(u.google, "priority"));
    modelctl_tiers_for_all("scale", &u);
    CHECK(str_eq(u.openai, "scale") && !u.anthropic && !u.google);
    modelctl_tiers_for_setting("inherit", &t, &u);
    CHECK(str_eq(u.openai, "priority") && str_eq(u.google, "flex"));
    modelctl_tiers_for_setting("none", &t, &u);
    CHECK(modelctl_tiers_empty(&u));
}

int main(void) {
    catalog_init();
    test_families();
    test_catalog_new_providers();
    test_context_windows();
    test_cachewarm_rules();
    test_slow_mode();
    test_tier_json();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
