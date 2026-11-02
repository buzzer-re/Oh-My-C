/* test_usage.c - WP-24 provider usage: the shared formatters (pi-utils formatDuration / formatNumber, chrome
 * formatCoarseDuration, Intl compact), resolveUsedFraction, saved-reset summaries, unreported accounts, the
 * `/usage` text (helpers/usage-report.ts), the classic detail report and the per-turn usage row (strings
 * checked against omp v18.14.2 with tests/tui/compare.sh and tests/e2e/mock_usage.py), and the activity scan
 * behind the heatmap. */
#include "config/usage.h"
#include "core/fs.h"
#include "tui/ansi.h"
#include "tui/symbols.h"
#include "ui/usage.h"

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

static void eqs(char *got, const char *want, const char *what) {
    CHECK(got && !strcmp(got, want), "%s: got [%s] want [%s]", what, got ? got : "(null)", want);
    free(got);
}

static void test_formatters(void) {
    eqs(usage_fmt_duration(0), "0ms", "duration 0");
    eqs(usage_fmt_duration(999), "999ms", "duration ms");
    eqs(usage_fmt_duration(59960), "1m", "duration tenths rounding");
    eqs(usage_fmt_duration(6500), "6.5s", "duration s");
    eqs(usage_fmt_duration(2 * 3600000.0 + 30000), "2h", "duration h");
    eqs(usage_fmt_duration(3 * 86400000.0 + 3 * 3600000.0), "3d3h", "duration d");
    eqs(usage_fmt_duration(16 * 86400000.0 + 16 * 3600000.0 + 5000), "16d16h", "duration 16d16h");
    eqs(usage_fmt_coarse(17300), "17s", "coarse s");
    eqs(usage_fmt_coarse(90 * 60000.0), "2h", "coarse h");
    eqs(usage_fmt_coarse(72 * 3600000.0), "3d", "coarse d");
    eqs(usage_fmt_number(100, -1), "100", "number small");
    eqs(usage_fmt_number(1500, -1), "1.5K", "number 1.5K");
    eqs(usage_fmt_number(25000, -1), "25K", "number 25K");
    eqs(usage_fmt_number(12.345, 2), "12.35", "number frac");
    eqs(usage_fmt_grouped(1234.5, 0), "1,235", "grouped");
    eqs(usage_fmt_grouped(58, 1), "58", "grouped int");
    eqs(usage_fmt_compact(5600), "5.6K", "compact");
    eqs(usage_fmt_compact(12), "12", "compact small");
    eqs(usage_provider_name("google-gemini-cli"), "Google Gemini Cli", "provider name");
    eqs(usage_one_line("a\nb\tc"), "a b c", "one line");
}

static json_t *limit(const char *json) { return json_parse_str(json); }

static void test_fractions(void) {
    double f;
    json_t *l = limit("{\"id\":\"x\",\"label\":\"L\",\"scope\":{\"provider\":\"p\"},\"amount\":{\"used\":25,\"limit\":100,\"unit\":\"requests\"}}");
    CHECK(usage_used_fraction(l, &f) && f == 0.25, "used/limit fraction %g", f);
    json_free(l);
    l = limit("{\"amount\":{\"used\":42,\"unit\":\"percent\"}}");
    CHECK(usage_used_fraction(l, &f) && f == 0.42, "percent fraction %g", f);
    json_free(l);
    l = limit("{\"amount\":{\"remainingFraction\":0.75,\"unit\":\"unknown\"}}");
    CHECK(usage_used_fraction(l, &f) && f == 0.25, "remaining fraction %g", f);
    json_free(l);
    l = limit("{\"amount\":{\"used\":12.5,\"unit\":\"usd\"}}");
    CHECK(!usage_used_fraction(l, &f) && usage_used_only_amount(l), "used-only usd");
    const json_t *arr[1] = {l};
    eqs(usage_absolute_only_amount(arr, 1), "$12.50 used", "absolute only usd");
    json_free(l);
    l = limit("{\"label\":\"Claude 7 Day (Fable)\",\"scope\":{\"tier\":\"fable\"},\"amount\":{\"unit\":\"percent\"}}");
    eqs(usage_limit_title(l), "Claude 7 Day (Fable)", "title keeps a named tier");
    json_free(l);
    l = limit("{\"label\":\"Premium Requests\",\"scope\":{\"tier\":\"individual\"},\"amount\":{\"unit\":\"requests\"}}");
    eqs(usage_limit_title(l), "Premium Requests (individual)", "title adds the tier");
    json_free(l);
}

static void test_resets(void) {
    char *exp = iso8601_from_ms(now_ms() + 5 * 86400000LL);
    char *js = xasprintf("{\"availableCount\":2,\"credits\":[{\"grantedAt\":\"2026-10-01T00:00:00.000Z\",\"expiresAt\":\"%s\","
                         "\"status\":\"available\"}]}",
                         exp);
    json_t *rc = json_parse_str(js);
    usage_reset_summary_t s;
    CHECK(usage_reset_summary(rc, now_ms(), &s), "summary");
    CHECK(s.banked == 2 && s.redeemable == 2 && s.soonest_expiry && !strcmp(s.soonest_expiry, exp) && !s.unavailable_reason,
          "summary fields %lld %lld %s %s", (long long)s.banked, (long long)s.redeemable, s.soonest_expiry, s.unavailable_reason);
    usage_reset_summary_free(&s);
    json_free(rc);
    rc = json_parse_str("{\"availableCount\":1,\"redeemableCount\":0,\"eligible\":false,\"credits\":[]}");
    CHECK(usage_reset_summary(rc, now_ms(), &s) && s.unavailable_reason && !strcmp(s.unavailable_reason, "not eligible"), "ineligible: %s",
          s.unavailable_reason);
    usage_reset_summary_free(&s);
    json_free(rc);
    /* reset-usage.ts rows and messages */
    json_t *st = json_parse_str("[{\"provider\":\"openai-codex\",\"credentialId\":3,\"email\":\"a@b.c\",\"active\":false,"
                                "\"availableCount\":2,\"credits\":[{\"id\":\"rc_1\",\"status\":\"available\"}]}]");
    json_t *acc = usage_reset_accounts(st);
    eqs(usage_reset_account_line(json_at(acc, 0)), "- a@b.c [Codex \xc2\xb7 openai-codex/3]: 2 saved, 2 usable now", "account line");
    char *err = NULL;
    CHECK(usage_resolve_reset_target(acc, "codex/3", "/usage reset", &err) == json_at(acc, 0) && !err, "resolve codex/3");
    CHECK(!usage_resolve_reset_target(acc, "codex/9", "/usage reset", &err) && err &&
              !strcmp(err, "No stored account matches \"codex/9\". List choices with `/usage reset`."),
          "unknown account: %s", err);
    free(err);
    err = NULL;
    CHECK(!usage_resolve_reset_target(acc, "gemini/1", "/usage reset", &err) && err &&
              !strcmp(err, "Unknown reset provider \"gemini\". Use anthropic or openai-codex."),
          "unknown provider: %s", err);
    free(err);
    json_t *o = json_parse_str("{\"ok\":true,\"code\":\"reset\",\"provider\":\"openai-codex\"}");
    eqs(usage_describe_redeem(o, "a@b.c"), "Reset applied for a@b.c (Codex) \xe2\x80\x94 your rate-limit window has been refreshed.", "redeem ok");
    json_free(o);
    o = json_parse_str("{\"ok\":true,\"code\":\"reset\",\"provider\":\"anthropic\",\"cleared\":[\"anthropic:5h\"]}");
    eqs(usage_describe_redeem(o, "x"), "Reset applied for x (Claude) \xe2\x80\x94 Claude's 5h session limit has been refreshed; weekly limits are unchanged.",
        "redeem claude 5h");
    json_free(o);
    json_free(acc);
    json_free(st);
    free(js);
    free(exp);
}

static void test_unreported(void) {
    json_t *reports = json_parse_str("[{\"provider\":\"anthropic\",\"fetchedAt\":1,\"limits\":[],\"metadata\":{\"email\":\"a@x\"}}]");
    json_t *accounts = json_parse_str("[{\"provider\":\"anthropic\",\"type\":\"oauth\",\"email\":\"a@x\"},"
                                      "{\"provider\":\"anthropic\",\"type\":\"oauth\",\"email\":\"b@x\"},"
                                      "{\"provider\":\"zai\",\"type\":\"api_key\"}]");
    json_t *u = usage_unreported_accounts(reports, accounts);
    CHECK(json_len(u) == 2 && str_eq(json_get_str(json_at(u, 0), "email"), "b@x") && str_eq(json_get_str(json_at(u, 1), "provider"), "zai"),
          "unreported: %zu", json_len(u));
    eqs(usage_account_label(json_at(u, 1)), "API key", "api key label");
    json_free(u);
    json_free(accounts);
    json_free(reports);
}

static const char *REPORTS =
    "[{\"provider\":\"zai\",\"fetchedAt\":%lld,\"limits\":[{\"id\":\"zai:tokens:5h\",\"label\":\"ZAI 5 Hours Token Quota\","
    "\"scope\":{\"provider\":\"zai\",\"windowId\":\"5h\",\"shared\":true},\"window\":{\"id\":\"5h\",\"label\":\"5 Hours\","
    "\"durationMs\":18000000,\"resetsAt\":%lld},\"amount\":{\"usedFraction\":0.12,\"remainingFraction\":0.88,\"unit\":\"tokens\"},"
    "\"status\":\"ok\"}],\"metadata\":{\"planType\":\"pro\"}}]";

static void test_report_text(void) {
    int64_t now = 1800000000000LL;
    char *js = xasprintf(REPORTS, (long long)now - 17000, (long long)now + 3 * 3600000LL + 30000);
    json_t *reports = json_parse_str(js);
    char *t = usage_report_text(reports, now, NULL, NULL, NULL);
    const char *want = "```\nUsage (17s ago)\n\nZai\n- ZAI 5 Hours Token Quota\n  account 1: 12.00 tokens used (88.0% left)\n"
                       "  [\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91"
                       "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91"
                       "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91] 12%\n  resets in 3h\n```";
    CHECK(!strcmp(t, want), "report text:\n%s", t);
    free(t);
    /* classic detail report (plain text of the themed lines) */
    char *d = ui_usage_detail(reports, (double)now, 116, NULL, NULL, NULL, NULL);
    char *plain = tui_strip_ansi(d);
    CHECK(strstr(plain, "Usage (17.0s ago)\n\nZai\n") && strstr(plain, "ZAI 5 Hours Token Quota\n") && strstr(plain, "88% free") &&
              strstr(plain, "(3h)") && strstr(plain, "  resets in 3h"),
          "detail:\n%s", plain);
    free(plain);
    free(d);
    json_free(reports);
    free(js);
    json_t *st = json_parse_str("{\"input\":10,\"output\":5,\"cacheRead\":0,\"cacheWrite\":0,\"totalTokens\":15,\"premiumRequests\":0,\"cost\":0.001}");
    eqs(usage_session_text(st),
        "Usage\nInput tokens: 10\nOutput tokens: 5\nCache read tokens: 0\nCache write tokens: 0\nTotal tokens: 15\nPremium requests: 0\nCost: $0.001000",
        "session tallies");
    json_free(st);
}

static void test_usage_row(void) {
    json_t *u = json_parse_str("{\"input\":90,\"output\":20,\"cacheRead\":0,\"cacheWrite\":10}");
    char *r = ui_usage_row(u, 0, 40, 0, 22);
    char *want = xasprintf("\xce\x94 22ms  %s 100  %s 20  %s 0.0s", tui_sym("icon.input"), tui_sym("icon.output"), tui_sym("icon.time"));
    CHECK(!strcmp(r, want), "row: [%s] want [%s]", r, want);
    free(r);
    free(want);
    json_free(u);
    u = json_parse_str("{\"input\":1500,\"output\":300,\"cacheRead\":12000,\"cacheWrite\":0}");
    r = ui_usage_row(u, 2000, 0, 0, 0);
    want = xasprintf("%s 1.5K  %s 300  %s 12K  %s 150.0/s", tui_sym("icon.input"), tui_sym("icon.output"), tui_sym("icon.cache"),
                     tui_sym("icon.throughput"));
    CHECK(!strcmp(r, want), "row throughput: [%s] want [%s]", r, want);
    free(r);
    free(want);
    json_free(u);
}

static void test_activity(void) {
    char *dir = fs_mktempdir("omc-usage-test");
    char *sub = path_join(dir, "--work--");
    fs_mkdir_p(sub, 0700);
    char *file = path_join(sub, "s.jsonl");
    int64_t ts = now_ms();
    char *lines = xasprintf("{\"type\":\"session\",\"id\":\"h\"}\n"
                            "{\"type\":\"message\",\"id\":\"m1\",\"message\":{\"role\":\"user\",\"timestamp\":%lld}}\n"
                            "{\"type\":\"message\",\"id\":\"m2\",\"message\":{\"role\":\"assistant\",\"timestamp\":%lld,\"usage\":{\"cost\":{\"total\":0.5}}}}\n"
                            "{\"type\":\"message\",\"id\":\"m3\",\"message\":{\"role\":\"assistant\",\"timestamp\":%lld,\"usage\":{\"cost\":{\"total\":0.25}}}}\n",
                            (long long)ts, (long long)ts, (long long)ts);
    fs_write(file, lines, strlen(lines));
    /* a fork copying the same entries counts them once */
    char *fork = path_join(sub, "f.jsonl");
    fs_write(fork, lines, strlen(lines));
    json_t *pts = NULL;
    ui_usage_load_activity(dir, &pts);
    CHECK(json_len(pts) == 1 && json_get_num(json_at(pts, 0), "requests", 0) == 2 && json_get_num(json_at(pts, 0), "cost", 0) == 0.75,
          "activity: %s", json_dump(pts, 0));
    json_free(pts);
    fs_rm_rf(dir);
    free(lines), free(file), free(fork), free(sub), free(dir);
}

int main(void) {
    test_formatters();
    test_fractions();
    test_resets();
    test_unreported();
    test_report_text();
    test_usage_row();
    test_activity();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
