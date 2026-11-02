/* test_extracmds.c - WP-28 commands: /changelog views over the embedded CHANGELOG (utils/changelog.ts
 * parseChangelogView / selectChangelogEntries / renderChangelogEntries, handleChangelogCommand titles) and the
 * registry entries of /changelog, /setup, /force, /skillful. */
#include "coding/changelog.h"
#include "coding/slashcmd.h"

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

int main(void) {
    test_views();
    test_entries();
    test_registry();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
