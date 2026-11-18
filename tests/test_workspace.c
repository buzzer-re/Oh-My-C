/* test_workspace.c - WP-21 workspace and session lifecycle: multi-root workspace directories in the session header
 * (session-workspace.ts / SessionManager), restartArgv (cli/flag-tables.ts), the session-worktree helpers
 * (session-worktree.ts) and the /move overlay (move-overlay.ts + move-directory-source.ts). */
#include "cli/args.h"
#include "core/fs.h"
#include "session/session.h"
#include "session/worktree.h"
#include "tui/ansi.h"
#include "ui/ui.h"

#include <unistd.h>

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

static void test_workspace_dirs(void) {
    session_t *s = session_in_memory("/srv/proj/app");
    char *r = NULL, *err = NULL;
    CHECK(session_add_workspace_dir(s, "../lib", &r, &err) == 1 && str_eq(r, "/srv/proj/lib"), "add relative: %s", r);
    free(r);
    CHECK(session_add_workspace_dir(s, "/srv/proj/lib/", &r, &err) == 0 && str_eq(r, "/srv/proj/lib"), "already present");
    free(r);
    CHECK(session_add_workspace_dir(s, ".", &r, &err) == -1 &&
              str_eq(err, "The current working directory is already the primary workspace root."),
          "cwd refused: %s", err);
    free(err);
    err = NULL;
    CHECK(session_add_workspace_dir(s, "/opt/other", &r, &err) == 1, "add absolute");
    free(r);
    strvec_t d = session_additional_dirs(s);
    CHECK(d.n == 2 && str_eq(d.v[0], "/srv/proj/lib") && str_eq(d.v[1], "/opt/other"), "order kept");
    strvec_free(&d);
    CHECK(session_remove_workspace_dir(s, "../lib", &r) == 1 && str_eq(r, "/srv/proj/lib"), "remove");
    free(r);
    CHECK(session_remove_workspace_dir(s, "../lib", &r) == 0, "remove twice is a no-op");
    free(r);
    CHECK(session_remove_workspace_dir(s, "/opt/other", &r) == 1, "remove last");
    free(r);
    CHECK(!json_get(s->header, "additionalDirectories"), "header key dropped when empty");
    /* setAdditionalDirectories: normalized, deduped, the cwd dropped */
    strvec_t in = {0};
    strvec_push(&in, "../x");
    strvec_push(&in, "/srv/proj/x");
    strvec_push(&in, ".");
    strvec_push(&in, "~/y");
    session_set_additional_dirs(s, &in);
    d = session_additional_dirs(s);
    char *hy = path_join(home_dir(), "y");
    CHECK(d.n == 2 && str_eq(d.v[0], "/srv/proj/x") && str_eq(d.v[1], hy), "set: %zu", d.n);
    free(hy);
    strvec_free(&d);
    strvec_free(&in);
    /* moveTo re-filters the roots: the new cwd may have been one of them */
    CHECK(session_move(s, "/srv/proj/x", &err) == 0, "move: %s", err);
    CHECK(str_eq(session_cwd(s), "/srv/proj/x"), "moved cwd %s", session_cwd(s));
    d = session_additional_dirs(s);
    CHECK(d.n == 1 && !str_eq(d.v[0], "/srv/proj/x"), "the new cwd is no extra root");
    strvec_free(&d);
    session_free(s);
}

static void test_restart_argv(void) {
    char *argv[] = {"omc", "--model", "mock/m", "-c", "--resume=abc", "hello", "--goal", "g", "--add-dir", "d",
                    "--session", "s1", "--thinking=high", "--", "--model", "x"};
    launch_remember_argv((int)ARRAY_LEN(argv), argv);
    strvec_t k = launch_restart_argv("/s/f.jsonl");
    const char *want[] = {"--model", "mock/m", "--add-dir", "d", "--thinking=high", "--restore", "/s/f.jsonl"};
    bool ok = k.n == ARRAY_LEN(want);
    for (size_t i = 0; ok && i < k.n; i++) ok = str_eq(k.v[i], want[i]);
    sb_t got = SB_INIT;
    for (size_t i = 0; i < k.n; i++) sb_appendf(&got, "%s ", k.v[i]);
    CHECK(ok, "restart argv: %s", sb_cstr(&got));
    sb_free(&got);
    strvec_free(&k);
    k = launch_restart_argv(NULL);
    CHECK(k.n == 5, "no session on disk: no --resume (%zu)", k.n);
    strvec_free(&k);
}

/* a bare --restore keeps the session --session already named (omp 59110aa258) */
static void test_restore_flag(void) {
    const char *cases[][4] = {{"omc", "--session", "sid", "--restore"}, {"omc", "--restore", "--session", "sid"}};
    for (size_t i = 0; i < 2; i++) {
        launch_args_t a;
        CHECK(launch_parse_args(4, (char **)cases[i], &a) == 0, "parse %zu", i);
        CHECK(a.restore && str_eq(a.resume, "sid"), "restore %zu: %s", i, a.resume ? a.resume : "(null)");
        launch_args_free(&a);
    }
    char *b[] = {"omc", "--session", "a", "--restore", "b"};
    launch_args_t a;
    CHECK(launch_parse_args(5, b, &a) == 0 && str_eq(a.resume, "b"), "--restore <id> wins");
    launch_args_free(&a);
}

static void test_worktree_helpers(void) {
    char *b = session_wt_default_branch();
    CHECK(strlen(b) == strlen("wt/20261102-153000") && str_starts(b, "wt/") && b[11] == '-', "default branch %s", b);
    free(b);
    session_wt_t w = {.path = "/w/wt-1", .branch = "feat"};
    char *s = session_wt_summary(&w, false);
    CHECK(str_eq(s, "Moved to worktree /w/wt-1 on branch feat (checked out, started from clean HEAD)."), "%s", s);
    free(s);
    w.kept_changes = true;
    s = session_wt_summary(&w, false);
    CHECK(str_eq(s, "Moved to worktree /w/wt-1 on branch feat (checked out, uncommitted changes carried over)."), "%s", s);
    free(s);
    s = session_wt_summary(&w, true);
    CHECK(str_eq(s, "Moved to worktree /w/wt-1 on branch feat (checked out, uncommitted changes moved, source checkout cleaned)."),
          "%s", s);
    free(s);
    session_wt_state_t clean = {0}, dirty = {.dirty = true, .moved = true};
    CHECK(str_eq(session_wt_decide_exit("keep", &dirty), "keep"), "keep");
    CHECK(str_eq(session_wt_decide_exit("remove", &clean), "remove"), "remove clean");
    CHECK(str_eq(session_wt_decide_exit("remove", &dirty), "prompt"), "remove dirty prompts");
    CHECK(str_eq(session_wt_decide_exit("ask", &clean), "prompt"), "ask prompts");
    char *m = session_wt_exit_message(&w, &dirty);
    CHECK(str_eq(m, "/w/wt-1 (feat): has uncommitted changes; has new commits (branch is kept)"), "%s", m);
    free(m);
    char *h = wt_hash_path("/srv/repo");
    CHECK(strlen(h) == 7, "hashPath is 7 hex digits: %s", h);
    free(h);
    char *e = NULL;
    session_wt_t out;
    CHECK(session_wt_create("/", "bad name", true, &out, &e) == -1 && e, "a non-repository refuses");
    free(e);
}

static void dummy_done(void *ud, const char *dir) {
    char **slot = ud;
    *slot = dir ? xstrdup(dir) : xstrdup("<cancel>");
}

static void test_move_overlay(void) {
    const char *tmp = getenv("TMPDIR");
    char *root = xasprintf("%s/omc-move-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(root) != NULL, "mkdtemp");
    const char *dirs[] = {"beta", "Alpha", ".hidden", "gamma"};
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) {
        char *p = path_join(root, dirs[i]);
        fs_mkdir_p(p, 0755);
        free(p);
    }
    char *f = path_join(root, "file.txt");
    fs_write(f, "x", 1);
    free(f);
    char *picked = NULL;
    tui_component_t *c = ui_move_overlay_new(root, dummy_done, &picked);
    strvec_t out = {0};
    c->vt->render(c, 100, &out);
    sb_t plain = SB_INIT;
    for (size_t i = 0; i < out.n; i++) {
        char *p = tui_strip_ansi(out.v[i]);
        sb_appendf(&plain, "%s\n", p);
        free(p);
    }
    const char *t = sb_cstr(&plain);
    CHECK(strstr(t, " Move to directory ") && strstr(t, "\xe2\x96\xb6 Alpha/") && strstr(t, "  beta/") && strstr(t, "  gamma/"),
          "listing:\n%s", t);
    CHECK(!strstr(t, ".hidden") && !strstr(t, "file.txt"), "hidden dirs and files are not listed:\n%s", t);
    CHECK(strstr(t, "Alpha/") < strstr(t, "beta/") && strstr(t, "beta/") < strstr(t, "gamma/"), "localeCompare order:\n%s", t);
    CHECK(strstr(t, "Type to filter \xc2\xb7 ") && strstr(t, " navigate \xc2\xb7 ") && strstr(t, " accept \xc2\xb7 ") &&
              strstr(t, " confirm \xc2\xb7 ") && strstr(t, " cancel"),
          "hint:\n%s", t);
    strvec_free(&out);
    sb_free(&plain);
    /* typing filters (case-insensitive substring), Enter confirms the highlighted directory */
    tui_key_t k = {.kind = TUI_KEY_PASTE, .text = "GAM", .text_len = 3};
    c->vt->handle_key(c, &k);
    c->vt->render(c, 60, &out);
    bool only = false;
    for (size_t i = 0; i < out.n; i++) {
        char *p = tui_strip_ansi(out.v[i]);
        if (strstr(p, "gamma/")) only = true;
        CHECK(!strstr(p, "beta/"), "filtered row: %s", p);
        free(p);
    }
    CHECK(only, "gamma listed after typing");
    strvec_free(&out);
    tui_key_t enter = {.kind = TUI_KEY_ENTER};
    c->vt->handle_key(c, &enter);
    char *want = path_join(root, "gamma");
    CHECK(picked && str_eq(picked, want), "picked %s", picked ? picked : "(null)");
    free(want);
    free(picked);
    c->vt->destroy(c);
    fs_rm_rf(root);
    free(root);
}

int main(void) {
    test_workspace_dirs();
    test_restart_argv();
    test_restore_flag();
    test_worktree_helpers();
    test_move_overlay();
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
