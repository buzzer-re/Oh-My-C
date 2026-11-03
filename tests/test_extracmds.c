/* test_extracmds.c - WP-28 commands: /changelog views over the embedded CHANGELOG (utils/changelog.ts
 * parseChangelogView / selectChangelogEntries / renderChangelogEntries, handleChangelogCommand titles) and the
 * registry entries of /changelog, /setup, /force, /skillful; WP-28b: /pause (the agent pause gate and
 * pause-screen.ts renderPauseScreen layouts). */
#include "coding/changelog.h"
#include "coding/slashcmd.h"
#include "tui/ansi.h"
#include "ui/ui.h"

#include <stdio.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static bool view(const char *args, changelog_kind_t kind, int count) {
    changelog_view_t v;
    char *err = NULL;
    bool ok = changelog_parse_view(args, &v, &err) && v.kind == kind && (kind == CHANGELOG_FULL || v.count == count);
    free(err);
    return ok;
}

static bool view_err(const char *args, const char *want) {
    changelog_view_t v;
    char *err = NULL;
    bool ok = !changelog_parse_view(args, &v, &err) && str_eq(err, want);
    free(err);
    return ok;
}

static void test_views(void) {
    CHECK(view("", CHANGELOG_RECENT, 3));
    CHECK(view("  FULL ", CHANGELOG_FULL, 0));
    CHECK(view("last", CHANGELOG_LAST, 1));
    CHECK(view("Last  4", CHANGELOG_LAST, 4));
    CHECK(view_err("foo", "Usage: /changelog [full|last [N]]"));
    CHECK(view_err("last x", "Usage: /changelog [full|last [N]]"));
    CHECK(view_err("lastly", "Usage: /changelog [full|last [N]]"));
    CHECK(view_err("last 0", "Usage: /changelog [full|last [N]] (N must be a positive integer)"));
}

static void test_entries(void) {
    size_t n = changelog_entry_count();
    CHECK(n > 20);
    changelog_view_t v = {CHANGELOG_LAST, 1};
    size_t shown = 0;
    char *md = changelog_markdown(&v, &shown);
    CHECK(md && shown == 1 && !strncmp(md, "## [18.14.2]", 12)); /* [Unreleased] has no version: skipped */
    CHECK(md && !strstr(md, "## [18.14.1]"));
    free(md);
    v = (changelog_view_t){CHANGELOG_LAST, 2};
    md = changelog_markdown(&v, &shown);
    CHECK(shown == 2 && md && !strncmp(md, "## [18.14.1]", 12)); /* oldest first */
    CHECK(md && strstr(md, "\n\n## [18.14.2]") != NULL);
    free(md);
    v = (changelog_view_t){CHANGELOG_FULL, 0};
    md = changelog_markdown(&v, &shown);
    CHECK(shown == n && md);
    free(md);
    char buf[32];
    CHECK(str_eq(changelog_title(&(changelog_view_t){CHANGELOG_RECENT, 3}, 3, buf, sizeof buf), "Recent Changes"));
    CHECK(str_eq(changelog_title(&(changelog_view_t){CHANGELOG_FULL, 0}, n, buf, sizeof buf), "Full Changelog"));
    CHECK(str_eq(changelog_title(&(changelog_view_t){CHANGELOG_LAST, 1}, 1, buf, sizeof buf), "Last Release"));
    CHECK(str_eq(changelog_title(&(changelog_view_t){CHANGELOG_LAST, 5}, 5, buf, sizeof buf), "Last 5 Releases"));
}

static void test_registry(void) {
    const slash_cmd_t *c = slash_lookup("changelog");
    CHECK(c && c->handle && c->tui && c->allow_args && c->nsubs == 2);
    CHECK(c && str_eq(c->acp_input_hint, "[full|last [N]]"));
    c = slash_lookup("setup");
    CHECK(c && c->tui && !c->handle && slash_lookup("providers") == c); /* TUI only (provider wizard) */
    c = slash_lookup("force");
    CHECK(c && c->handle && c->allow_args && str_eq(c->inline_hint, "<tool-name> [prompt]"));
    c = slash_lookup("skillful");
    CHECK(c && c->handle && c->nsubs == 3 && str_eq(c->acp_input_hint, "[on|off|status]"));
    slash_parsed_t p;
    CHECK(slash_match("/force:read look", &p) == slash_lookup("force"));
    CHECK(str_eq(p.args, "read look"));
    slash_parsed_free(&p);
}

/* row `i` of a rendered pause screen without ANSI, trailing blanks trimmed (owned) */
static char *prow(const strvec_t *v, size_t i) {
    char *t = tui_strip_ansi(v->v[i]);
    char *r = str_trim(t);
    free(t);
    return r;
}

static bool prow_is(const strvec_t *v, size_t i, const char *want) {
    char *r = prow(v, i);
    bool ok = str_eq(r, want);
    if (!ok) fprintf(stderr, "  row %zu: [%s] want [%s]\n", i, r, want);
    free(r);
    return ok;
}

static void test_pause(void) {
    const slash_cmd_t *c = slash_lookup("pause");
    CHECK(c && c->tui && !c->handle && !c->allow_args && str_eq(c->icon, "pause"));
    CHECK(c && str_eq(c->description, "Freeze all agents (main, subagents, advisor) until resumed"));
    /* the gate: engage once, the second engage is a no-op; resume reports the hold */
    CHECK(!agent_pause_gate_paused() && agent_pause_gate_paused_at() == 0);
    CHECK(agent_pause_gate_pause());
    CHECK(!agent_pause_gate_pause());
    CHECK(agent_pause_gate_paused() && agent_pause_gate_paused_at() > 0);
    CHECK(agent_pause_gate_resume() >= 0);
    CHECK(agent_pause_gate_resume() == -1);
    CHECK(agent_unsettled_loops() == 0);
    /* full scene, 100x30, no outstanding work: 16 content rows centered (7 above) */
    strvec_t v = {0};
    ui_pause_render(100, 30, 7000, NULL, 0, 0, &v);
    CHECK(v.n == 30);
    CHECK(prow_is(&v, 6, "") && prow_is(&v, 7, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88    \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"));
    CHECK(prow_is(&v, 15, "P A U S E D"));
    CHECK(prow_is(&v, 19, "Paused \xe2\x80\x94 ready to suspend"));
    CHECK(prow_is(&v, 20, "paused for 0:07"));
    strvec_free(&v);
    /* outstanding work + a session name; clock past an hour */
    ui_pause_render(100, 30, 3723000, "my\n  session", 2, 1, &v);
    CHECK(prow_is(&v, 5, "my session") && prow_is(&v, 15, "P A U S E D"));
    CHECK(prow_is(&v, 19, "Pausing \xe2\x80\x94 waiting for work to settle"));
    CHECK(prow_is(&v, 20, "Unsettled turns: 2") && prow_is(&v, 21, "Unfinished jobs: 1"));
    CHECK(prow_is(&v, 22, "paused for 1:02:03"));
    strvec_free(&v);
    /* compact card below 64 columns */
    ui_pause_render(40, 12, 0, NULL, 0, 0, &v);
    CHECK(v.n == 12 && prow_is(&v, 3, "\xe2\x96\x8c\xe2\x96\x8c P A U S E D") && prow_is(&v, 6, "Paused \xe2\x80\x94 ready to suspend"));
    strvec_free(&v);
    /* one row: the live state and the way out */
    ui_pause_render(40, 1, 0, NULL, 1, 0, &v);
    CHECK(v.n == 1 && prow_is(&v, 0, "T:1 J:0 \xc2\xb7 \xe2\x8e\x8b to resume"));
    strvec_free(&v);
    ui_pause_render(40, 2, 0, NULL, 1, 3, &v);
    CHECK(prow_is(&v, 0, "Pausing \xc2\xb7 T:1 J:3") && prow_is(&v, 1, "\xe2\x8e\x8b to resume"));
    strvec_free(&v);
    char *d = ui_coarse_duration(52400);
    CHECK(str_eq(d, "52s"));
    free(d);
    d = ui_coarse_duration(90000);
    CHECK(str_eq(d, "2m"));
    free(d);
}

int main(void) {
    test_views();
    test_pause();
    test_entries();
    test_registry();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
