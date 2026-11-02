/* test_export.c - `/export` HTML (session/export_html.c, session/trace.c) against omp v18.14.2 output.
 *
 * tests/fixtures/export/rich.jsonl (+ rich/ sub-sessions: nested subagent, advisor, tombstone, gzip, a
 * header-less transcript, a .bak; built by mkrich.py) exercises the trace port: usage/cost doubles,
 * ttft, tool_execution_start, background jobs, task results, compaction/model/mode/reset/exit markers,
 * the title slot and an emoji-heavy label truncated mid surrogate pair. rich.omp.html is
 * `omp --export rich.jsonl rich.omp.html`; themes.omp.css is the <style> body of omp's
 * `/export --themes` with the default theme.dark=titanium / theme.light=light. */
#include "core/fs.h"
#include "session/export_html.h"
#include "session/trace.h"

#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void first_diff(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    fprintf(stderr, "  first difference at byte %zu:\n  want: %.120s\n  got:  %.120s\n", i, a + (i > 40 ? i - 40 : 0),
            b + (i > 40 ? i - 40 : 0));
}

int main(void) {
    /* Date.parse forms the trace relies on */
    double ms = 0;
    CHECK(trace_parse_iso_ms("2026-11-01T12:00:00.000Z", &ms) && ms == 1793534400000.0, "iso Z %f", ms);
    CHECK(trace_parse_iso_ms("2026-11-01T14:00:00.1234+02:00", &ms) && ms == 1793534400123.0, "iso offset %f", ms);
    CHECK(trace_parse_iso_ms("2026-11-01", &ms) && ms == 1793491200000.0, "date only %f", ms);
    CHECK(!trace_parse_iso_ms("yesterday", &ms), "garbage parsed");

    /* default output name */
    char *n = export_default_name("/x/2026-11-01T12-00-00-000Z_abc.jsonl");
    CHECK(!strcmp(n, "omc-session-2026-11-01T12-00-00-000Z_abc.html"), "default name %s", n);
    free(n);

    /* exportFromFile: byte-identical to omp */
    size_t len;
    char *want = fs_read("tests/fixtures/export/rich.omp.html", &len);
    CHECK(want, "missing fixture");
    char *out = fs_mktemp("omc-export-", ".html"), *written = NULL, *err = NULL;
    int rc = export_html_from_file("tests/fixtures/export/rich.jsonl", out, &written, &err);
    CHECK(rc == 0, "export failed: %s", err ? err : "");
    char *got = rc == 0 ? fs_read(out, &len) : NULL;
    if (want && got) {
        CHECK(!strcmp(want, got), "export differs from omp");
        if (strcmp(want, got)) first_diff(want, got);
    }
    unlink(out);
    free(out);
    free(written);
    free(got);
    free(want);
    free(err);
    err = NULL;
    rc = export_html_from_file("tests/fixtures/export/missing.jsonl", NULL, &written, &err);
    CHECK(rc != 0 && err && !strcmp(err, "File not found: tests/fixtures/export/missing.jsonl"), "missing file: %s", err ? err : "");
    free(err);

    /* --themes: generateThemeStyles("theme", {dark: titanium, light: light}) */
    want = fs_read("tests/fixtures/export/themes.omp.css", &len);
    err = NULL;
    char *css = export_theme_styles("titanium", "light", &err);
    CHECK(css && want && !strcmp(css, want), "theme styles differ (%s)", err ? err : "");
    if (css && want && strcmp(css, want)) first_diff(want, css);
    free(css);
    free(want);
    free(err);
    err = NULL;
    css = export_theme_styles("no-such-theme", "light", &err);
    CHECK(!css && err && !strcmp(err, "Theme not found: no-such-theme"), "unknown theme: %s", err ? err : "");
    free(err);

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
