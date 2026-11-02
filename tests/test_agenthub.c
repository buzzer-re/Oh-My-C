/* test_agenthub.c - WP-22 subagent visibility: the Agent Hub's formatting (pi-utils formatDuration / formatNumber /
 * formatAge, agent-hub-renderer.ts formatCost / formatMetricColumns), activity rows (agent-activity.ts
 * activityRowsFromProgress + activity/index.ts transcript rows) and the registry's empty-state answers. */
#include "coding/agents.h"
#include "tui/symbols.h"
#include "ui/ui.h"

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

static void eq_fmt(const char *what, int64_t v, const char *want) {
    char *got = ui_hub_format(what, v);
    CHECK(got && !strcmp(got, want), "%s(%lld) = %s, want %s", what, (long long)v, got ? got : "(null)", want);
    free(got);
}

static void test_format(void) {
    eq_fmt("duration", 0, "0ms");
    eq_fmt("duration", 210, "210ms");
    eq_fmt("duration", 1234, "1.2s");
    eq_fmt("duration", 59960, "1m");
    eq_fmt("duration", 60400, "1m");
    eq_fmt("duration", 61000, "1m1s");
    eq_fmt("duration", 3600000 + 120000, "1h2m");
    eq_fmt("duration", 2 * 86400000LL + 3600000, "2d1h");
    eq_fmt("number", 120, "120");
    eq_fmt("number", 1500, "1.5K");
    eq_fmt("number", 2000, "2K");
    eq_fmt("number", 100000, "100K");
    eq_fmt("number", 2500000, "2.5M");
    eq_fmt("age", 1, "just now");
    eq_fmt("age", 61, "1m ago");
    eq_fmt("age", 3 * 86400, "3d ago");
    eq_fmt("age", 15 * 86400, "2w ago");
    eq_fmt("cost", 0, "$0.0000");
    eq_fmt("cost", 1234, "$0.0012");
    eq_fmt("cost", 123400, "$0.123");
    eq_fmt("cost", 2500000, "$2.50");
}

static void test_metric_columns(void) {
    json_t *ref = json_parse_str("{\"id\":\"A\",\"progress\":{\"cost\":0,\"durationMs\":200,\"requests\":0,\"toolCount\":0,\"tokens\":0}}");
    char *row = ui_hub_metric_columns(ref, "just now");
    /* cost 8L, duration 13R, req 8R, tools 9R, tok 8R, age 8R (captured from omp v18.14.2) */
    CHECK(row && !strcmp(row, "$0.0000   200ms active    0 req   0 tools    0 tok just now"), "row: [%s]", row ? row : "");
    free(row);
    json_free(ref);
    ref = json_parse_str("{\"id\":\"A\",\"progress\":{\"cost\":0,\"durationMs\":0,\"requests\":1,\"toolCount\":0,\"tokens\":120}}");
    row = ui_hub_metric_columns(ref, "1m ago");
    CHECK(row && !strcmp(row, "$0.0000              —    1 req   0 tools  120 tok   1m ago"), "row: [%s]", row ? row : "");
    free(row);
    json_free(ref);
    ref = json_parse_str("{\"id\":\"A\"}");
    CHECK(ui_hub_metric_columns(ref, "x") == NULL, "no progress, no metrics");
    json_free(ref);
}

static void test_activity_rows(void) {
    /* live rows: recent tools newest-last, the lifecycle row and the response row (pending status, as omp) */
    json_t *ref = json_parse_str(
        "{\"id\":\"A\",\"progress\":{\"status\":\"completed\",\"task\":\"Complete assignment thoroughly:\\n\\nsay:beta done\","
        "\"recentTools\":[{\"tool\":\"read\",\"args\":\"nofile.txt\",\"endMs\":900}],\"recentOutput\":[\"beta done\"]}}");
    json_t *msgs = json_parse_str(
        "[{\"role\":\"user\",\"content\":\"hi\",\"timestamp\":1},"
        "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"beta done\"},"
        "{\"type\":\"toolCall\",\"id\":\"c1\",\"name\":\"bash\",\"arguments\":{\"command\":\"ls  -la\"}}],\"timestamp\":2},"
        "{\"role\":\"toolResult\",\"toolCallId\":\"c1\",\"toolName\":\"bash\",\"isError\":true,\"content\":[],\"timestamp\":3}]");
    json_t *rows = ui_hub_activity_rows(ref, msgs);
    char *dump = json_dump(rows, 0);
    const char *want =
        "[{\"source\":\"transcript\",\"kind\":\"response\",\"title\":\"Response\",\"summary\":\"beta done\",\"status\":\"success\"},"
        "{\"source\":\"transcript\",\"kind\":\"tool\",\"title\":\"bash\",\"summary\":\"ls -la\",\"status\":\"error\"},"
        "{\"source\":\"live\",\"kind\":\"tool\",\"title\":\"read\",\"summary\":\"nofile.txt\",\"status\":\"success\"},"
        "{\"source\":\"live\",\"kind\":\"lifecycle\",\"title\":\"completed\",\"summary\":\"Complete assignment thoroughly: say:beta done\",\"status\":\"success\"},"
        "{\"source\":\"live\",\"kind\":\"response\",\"title\":\"Response\",\"summary\":\"beta done\",\"status\":\"pending\"}]";
    CHECK(!strcmp(dump, want), "rows:\n%s", dump);
    free(dump);
    json_free(rows);
    json_free(msgs);
    json_free(ref);
}

static int g_changes;
static void on_change(void *ud) {
    (void)ud;
    g_changes++;
}

static void test_registry_empty(void) {
    size_t running = 9, all = 9;
    agents_counts(&running, &all);
    CHECK(running == 0 && all == 0, "counts %zu %zu", running, all);
    json_t *l = agents_list();
    CHECK(json_len(l) == 0, "empty roster");
    json_free(l);
    CHECK(agents_get("Nobody") == NULL, "unknown ref");
    char *err = NULL;
    CHECK(!agents_kill("Nobody", &err) && err &&
              !strcmp(err, "Unknown agent \"Nobody\" — it was never registered or has been released. If a transcript exists, read history://Nobody."),
          "kill error: %s", err ? err : "");
    free(err);
    err = NULL;
    CHECK(!agents_revive("Nobody", &err) && err && strstr(err, "Unknown agent \"Nobody\""), "revive error");
    free(err);
    err = NULL;
    CHECK(!agents_prompt("Nobody", "hi", &err) && err, "prompt error");
    free(err);
    CHECK(agents_pin("Nobody") == NULL, "pin unknown");
    CHECK(agents_messages("Nobody") == NULL, "no transcript");
    agents_listen(on_change, NULL);
    agents_unlisten(on_change, NULL);
    CHECK(g_changes == 0, "no spurious change notifications");
}

int main(void) {
    tui_sym_set_preset("unicode");
    test_format();
    test_metric_columns();
    test_activity_rows();
    test_registry_empty();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
