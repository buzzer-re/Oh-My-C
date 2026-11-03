/* test_extracmds.c - WP-28 commands: /changelog views over the embedded CHANGELOG (utils/changelog.ts
 * parseChangelogView / selectChangelogEntries / renderChangelogEntries, handleChangelogCommand titles) and the
 * registry entries of /changelog, /setup, /force, /skillful; WP-28b: /pause (the agent pause gate and
 * pause-screen.ts renderPauseScreen layouts), /debug (system info, terminal state, the tar.gz report bundle,
 * env sanitizing, the artifact cache), the startup release notes (resolveStartupChangelogForDisplay,
 * formatStartupChangelogSummary), /record + `omc play` (row normalization, redaction, the paint encoder,
 * recording parse). */
#include "coding/changelog.h"
#include "coding/debugtools.h"
#include "coding/record.h"
#include "core/fs.h"
#include "coding/slashcmd.h"
#include "tui/ansi.h"
#include "ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

char *ui_debug_terminal_state(int cols, int rows, bool sync_output); /* modes/debugctl.c */

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

/* tar header field helpers over a decompressed archive */
static unsigned tar_sum(const unsigned char *h) {
    unsigned s = 0;
    for (int i = 0; i < 512; i++) s += (i >= 148 && i < 156) ? ' ' : h[i];
    return s;
}

static void test_debug(void) {
    const slash_cmd_t *c = slash_lookup("debug");
    CHECK(c && c->tui && !c->handle && !c->allow_args && str_eq(c->icon, "bug") && str_eq(c->description, "Open debug tools selector"));
    /* system info: omp's rows minus the Bun/Node versions */
    json_t *info = debug_system_info("/work");
    char *t = debug_format_system_info(info);
    CHECK(!strncmp(t, "System Information\n\xe2\x94\x81", 22));
    CHECK(strstr(t, "\nApp:     omc ") && strstr(t, "\nCWD:     /work\n") && !strstr(t, "Bun:") && !strstr(t, "Node:"));
    CHECK(str_eq(json_get_str(info, "cwd"), "/work") && json_get_num(json_get(info, "memory"), "total", 0) > 0);
    free(t);
    json_free(info);
    /* sanitizeEnv */
    setenv("WP28B_API_THING", "v1", 1);
    setenv("WP28B_PLAIN", "v2", 1);
    json_t *env = debug_sanitized_env();
    CHECK(str_eq(json_get_str(env, "WP28B_API_THING"), "[REDACTED]") && str_eq(json_get_str(env, "WP28B_PLAIN"), "v2"));
    json_free(env);
    json_t *mem = debug_memory_stats();
    CHECK(json_get_num(json_get(mem, "process"), "rss", 0) > 0);
    json_free(mem);
    /* terminal state rows */
    setenv("TERM", "xterm-256color", 1);
    unsetenv("TMUX");
    char *ts = ui_debug_terminal_state(120, 40, true);
    CHECK(!strncmp(ts, "Terminal State\n", 15) && strstr(ts, "\nGeometry:     120x40 cells \xc2\xb7 cell 9x18px\n"));
    CHECK(strstr(ts, "  Sync output:  yes (DEC 2026)\n") && strstr(ts, "  TERM:                 xterm-256color\n"));
    free(ts);
    /* tar.gz: parent directories once, 0644/0755, mtime 0, a PAX record for a path no ustar split fits */
    char dir[] = "/tmp/omc-wp28b-XXXXXX";
    const char *tmp = getenv("TMPDIR");
    char *base = tmp ? xasprintf("%s/omc-wp28b-XXXXXX", tmp) : xstrdup(dir);
    CHECK(mkdtemp(base) != NULL);
    char *out = path_join(base, "r.tar.gz");
    sb_t longname = SB_INIT;
    for (int i = 0; i < 120; i++) sb_putc(&longname, 'x');
    const char *names[] = {"a.json", "art/sub/f.txt", sb_cstr(&longname)};
    const char *data[] = {"{}", "hello", "z"};
    size_t lens[] = {2, 5, 1};
    char *err = NULL;
    CHECK(debug_write_tar_gz(out, names, data, lens, 3, &err) == 0);
    gzFile gz = gzopen(out, "rb");
    unsigned char buf[16384];
    int n = gz ? gzread(gz, buf, sizeof buf) : 0;
    if (gz) gzclose(gz);
    /* a.json, art/, art/sub/, art/sub/f.txt, PaxHeaders + PaxFile, 2 end blocks */
    CHECK(n == 512 * (2 + 1 + 1 + 2 + 2 + 2 + 2) );
    if (n >= 512 * 6) {
        CHECK(!strcmp((char *)buf, "a.json") && buf[156] == '0' && !memcmp(buf + 100, "0000644", 7) && !memcmp(buf + 136, "00000000000", 11));
        CHECK(!strcmp((char *)buf + 1024, "art/") && buf[1024 + 156] == '5' && !memcmp(buf + 1024 + 100, "0000755", 7));
        CHECK(!strcmp((char *)buf + 1536, "art/sub/") && !strcmp((char *)buf + 2048, "art/sub/f.txt"));
        CHECK(!memcmp(buf + 257, "ustar\0" "00", 8) && strtoul((char *)buf + 148, NULL, 8) == tar_sum(buf));
        CHECK(!strcmp((char *)buf + 3072, "PaxHeaders/00000004") && buf[3072 + 156] == 'x');
        CHECK(!strncmp((char *)buf + 3584, "130 path=xxx", 12)); /* "<len> path=<120 x>\n": len counts itself */
        CHECK(!strcmp((char *)buf + 4096, "PaxFile/00000004"));
    }
    sb_free(&longname);
    /* report bundle: members in omp's order, the session's artifact subtree under artifacts/ */
    char *sess = path_join(base, "s.jsonl"), *adir = path_join(base, "s"), *af = path_join(adir, "1.md");
    fs_write(sess, "{}\n", 3);
    fs_mkdir_p(adir, 0755);
    fs_write(af, "x", 1);
    json_t *cfg = json_obj();
    json_set_str(cfg, "model", "m");
    strvec_t files = {0};
    char *rep = debug_report_bundle(base, sess, "/w", cfg, NULL, &files, &err);
    CHECK(rep && files.n == 5 && str_eq(files.v[0], "system.json") && str_eq(files.v[1], "env.json") &&
          str_eq(files.v[2], "config.json") && str_eq(files.v[3], "session.jsonl") && str_eq(files.v[4], "artifacts/1.md"));
    CHECK(rep && strstr(rep, "/omp-report-") && strstr(rep, "Z.tar.gz"));
    /* the artifact cache: subdirectories of the sessions dir */
    debug_cache_stats_t st;
    debug_artifact_cache_stats(base, &st);
    CHECK(st.count == 1 && st.total_size == 1 && st.oldest_ms > 0);
    CHECK(debug_clear_artifact_cache(base, 30) == 0); /* fresh: nothing older than 30 days */
    CHECK(debug_clear_artifact_cache(base, -1) == 1);
    free(rep), free(sess), free(adir), free(af), free(out), free(err);
    strvec_free(&files);
    json_free(cfg);
    fs_rm_rf(base);
    free(base);
}

static char *marker_read(const char *p) {
    size_t n = 0;
    return fs_read(p, &n);
}

static void test_startup_changelog(void) {
    CHECK(str_eq(changelog_current_version(), "18.14.2"));
    const char *tmp = getenv("TMPDIR");
    char *base = xasprintf("%s/omc-wp28b-cl-XXXXXX", tmp ? tmp : "/tmp");
    CHECK(mkdtemp(base) != NULL);
    char *marker = path_join(base, "last-changelog-version");
    changelog_startup_t s;
    /* no marker: record the current version, show nothing */
    CHECK(!changelog_startup_resolve("summary", marker, &s));
    char *m = marker_read(marker);
    CHECK(str_eq(m, "18.14.2"));
    free(m);
    /* steady state */
    CHECK(!changelog_startup_resolve("summary", marker, &s));
    /* three newest of six unseen releases; the marker advances */
    fs_write(marker, "18.13.0\n", 8);
    CHECK(changelog_startup_resolve("summary", marker, &s));
    CHECK(s.selected == 3 && s.total_unseen == 6 && str_eq(s.latest, "18.14.2") && s.change_count == 99);
    CHECK(s.markdown && !strncmp(s.markdown, "## [18.14.2]", 12) && !s.truncated);
    char *sum = changelog_startup_summary(&s);
    CHECK(str_eq(sum, "Updated to v18.14.2 \xc2\xb7 99 changes across 3 releases\n3 breaking changes \xc2\xb7 17 added \xc2\xb7 "
                      "22 changed \xc2\xb7 57 fixed \xc2\xb7 +3 earlier releases \xc2\xb7 Use /changelog full for history."));
    free(sum);
    changelog_startup_free(&s);
    m = marker_read(marker);
    CHECK(str_eq(m, "18.14.2"));
    free(m);
    /* one release: "in 1 release", details hint */
    fs_write(marker, "18.14.1", 7);
    CHECK(changelog_startup_resolve("expanded", marker, &s) && s.selected == 1 && s.total_unseen == 1);
    sum = changelog_startup_summary(&s);
    CHECK(strstr(sum, " in 1 release\n") && strstr(sum, "\xc2\xb7 Use /changelog for details."));
    free(sum);
    changelog_startup_free(&s);
    /* hidden: an upgrade advances the marker, a downgrade keeps it */
    fs_write(marker, "18.0.0", 6);
    CHECK(!changelog_startup_resolve("hidden", marker, &s));
    m = marker_read(marker);
    CHECK(str_eq(m, "18.14.2"));
    free(m);
    fs_write(marker, "99.0.0", 6);
    CHECK(!changelog_startup_resolve("hidden", marker, &s));
    m = marker_read(marker);
    CHECK(str_eq(m, "99.0.0"));
    free(m);
    /* an unparsable marker is replaced */
    fs_write(marker, "v18", 3);
    CHECK(!changelog_startup_resolve("summary", marker, &s));
    m = marker_read(marker);
    CHECK(str_eq(m, "18.14.2"));
    free(m);
    changelog_startup_t empty = {0};
    sum = changelog_startup_summary(&empty);
    CHECK(str_eq(sum, "Updated omp. Use /changelog for recent changes."));
    free(sum);
    fs_rm_rf(base);
    free(marker);
    free(base);
}

static bool norm_is(const char *row, const char *want) {
    char *n = record_normalize_row(row);
    bool ok = str_eq(n, want);
    if (!ok) fprintf(stderr, "  normalize: [%s]\n", n);
    free(n);
    return ok;
}

static void test_record(void) {
    const slash_cmd_t *c = slash_lookup("record");
    CHECK(c && c->tui && !c->handle && str_eq(c->description, "Start or stop recording this screen to a replayable file (omc play)"));
    /* normalizeStreamRow: SGR + OSC 8 survive; other CSI, controls, cursor moves drop; images become [image] */
    CHECK(norm_is("\x1b[1mhi\x1b[0m\x1b[2K\x07x", "\x1b[1mhi\x1b[0mx"));
    CHECK(norm_is("\x1b]8;;http://a\x07link\x1b]8;;\x07", "\x1b]8;;http://a\x07link\x1b]8;;\x07"));
    CHECK(norm_is("a\x1b]66;s=2;BIG\x07" "b", "aBIGb"));
    CHECK(norm_is("x\x1b_Ga=T,f=100;AAAA\x1b\\y", "[image]"));
    CHECK(norm_is("\x1bPq#0;2;0;0;0\x1b\\", "[image]"));
    CHECK(norm_is("pre \xf4\x8e\xbb\xae post", "[image]"));
    CHECK(norm_is("tab\there\xc2\x85!\x1b(0", "tab\there!"));
    /* redaction: builtin shapes, prefixes, assignments, connection URLs, plain secrets; styling drops */
    const char *plain[] = {"hunter2hunter2"};
    record_redactor_t *r = record_redactor_new(plain, 1);
    char *x = record_redact_row(r, "\x1b[1mok\x1b[0m");
    CHECK(str_eq(x, "\x1b[1mok\x1b[0m"));
    free(x);
    x = record_redact_row(r, "\x1b[1mkey sk-ant-abcdef1234\x1b[0m end");
    CHECK(str_eq(x, "key \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 end"));
    free(x);
    x = record_redact_row(r, "export API_TOKEN=\"abcd1234\" and pw hunter2hunter2!");
    CHECK(x && strstr(x, "API_TOKEN=\"\xe2\x80\xa2") && !strstr(x, "abcd1234") && !strstr(x, "hunter2hunter2"));
    free(x);
    x = record_redact_row(r, "db postgres://bob:s3cretpw@host/db Bearer abc.def");
    CHECK(x && !strstr(x, "s3cretpw") && !strstr(x, "abc.def") && strstr(x, "postgres://bob:"));
    free(x);
    /* encoder: a first full viewport, then a patch of the changed row, history + a resize */
    record_encoder_t *e = record_encoder_new(80, 3, r);
    const char *v1[] = {"a", "b", "c"}, *v2[] = {"a", "B", "c"}, *hist[] = {"old"};
    record_paint_t p1 = {NULL, 0, v1, 3, false, false, 80, 3};
    record_encoder_push(e, &p1);
    json_t *f = record_encoder_drain(e);
    char *s = json_dump(f, 0);
    CHECK(str_eq(s, "[{\"t\":\"viewport\",\"rows\":[\"a\",\"b\",\"c\"]}]"));
    free(s), json_free(f);
    record_paint_t p2 = {hist, 1, v2, 3, false, false, 80, 3};
    record_encoder_push(e, &p2);
    f = record_encoder_drain(e);
    s = json_dump(f, 0);
    CHECK(str_eq(s, "[{\"t\":\"history\",\"rows\":[\"old\"]},{\"t\":\"patch\",\"ops\":[[1,\"B\"]],\"rows\":3}]"));
    free(s), json_free(f);
    record_paint_t p3 = {NULL, 0, v2, 3, true, false, 90, 3};
    record_encoder_push(e, &p3);
    f = record_encoder_drain(e);
    s = json_dump(f, 0);
    CHECK(str_eq(s, "[{\"t\":\"reset\"},{\"t\":\"resize\",\"cols\":90,\"rows\":3},{\"t\":\"viewport\",\"rows\":[\"a\",\"B\",\"c\"]}]"));
    free(s), json_free(f);
    f = record_encoder_drain(e); /* nothing pending */
    CHECK(json_len(f) == 0);
    json_free(f);
    record_encoder_free(e);
    record_redactor_free(r);
    /* parseRecording */
    const char *ok = "{\"ompcast\":1,\"cols\":80,\"rows\":24,\"title\":\"t\",\"createdAt\":\"x\"}\n"
                     "[0,{\"t\":\"viewport\",\"rows\":[\"a\"]}]\n[5,{\"t\":\"patch\",\"ops\":[[0,\"b\"]],\"rows\":1}]\n[9,{\"t\":\"vie";
    char *err = NULL;
    json_t *rec = record_parse(ok, strlen(ok), &err);
    CHECK(rec && json_len(json_get(rec, "events")) == 2 && !err); /* the torn last line is ignored */
    json_free(rec);
    const char *bad = "{\"ompcast\":1,\"cols\":80,\"rows\":24,\"title\":\"t\",\"createdAt\":\"x\"}\n[0,{\"t\":\"hello\"}]\n";
    rec = record_parse(bad, strlen(bad), &err);
    CHECK(!rec && str_eq(err, "malformed recording event on line 2"));
    free(err);
    rec = record_parse("{}\n", 3, &err);
    CHECK(!rec && str_eq(err, "not an omp recording (missing ompcast header)"));
    free(err);
    const char *v2h = "{\"ompcast\":2,\"cols\":80,\"rows\":24,\"title\":\"t\",\"createdAt\":\"x\"}\n";
    rec = record_parse(v2h, strlen(v2h), &err);
    CHECK(!rec && str_eq(err, "unsupported recording version 2"));
    free(err);
    char *path = record_new_path("0123456789abcdef");
    CHECK(path && strstr(path, "/omp-recordings/") && str_ends(path, "-01234567.ompcast"));
    free(path);
}

int main(void) {
    test_views();
    test_record();
    test_startup_changelog();
    test_pause();
    test_debug();
    test_entries();
    test_registry();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
