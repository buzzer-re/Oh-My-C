/* test_tree.c - WP-8: branch summary (entry collection, summarizer prompt, stored summary) and the /tree
 * session tree selector (rows, gutters, filters, search, labels, selection) on a seeded multi-branch session. */
#include "coding/branchsum.h"
#include "tui/theme.h"
#include "ui/overlays.h"
#include "ui/ui.h"

#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                               \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static json_t *user(const char *t) {
    json_t *m = json_obj(), *c = json_arr(), *b = json_obj();
    json_set_str(m, "role", "user");
    json_set_str(b, "type", "text");
    json_set_str(b, "text", t);
    json_push(c, b);
    json_set(m, "content", c);
    return m;
}

static json_t *assistant(const char *t) {
    json_t *m = json_obj(), *c = json_arr(), *b = json_obj();
    json_set_str(m, "role", "assistant");
    json_set_str(b, "type", "text");
    json_set_str(b, "text", t);
    json_push(c, b);
    json_set(m, "content", c);
    json_set_str(m, "stopReason", "stop");
    return m;
}

static json_t *tool_call(const char *id, const char *name, const char *args_json) {
    json_t *m = json_obj(), *c = json_arr(), *b = json_obj();
    json_set_str(m, "role", "assistant");
    json_set_str(b, "type", "toolCall");
    json_set_str(b, "id", id);
    json_set_str(b, "name", name);
    json_set(b, "arguments", json_parse_str(args_json));
    json_push(c, b);
    json_set(m, "content", c);
    json_set_str(m, "stopReason", "toolUse");
    return m;
}

static json_t *tool_result(const char *id, const char *name, const char *text, bool err) {
    json_t *m = json_obj(), *c = json_arr(), *b = json_obj();
    json_set_str(m, "role", "toolResult");
    json_set_str(m, "toolCallId", id);
    json_set_str(m, "toolName", name);
    json_set_str(b, "type", "text");
    json_set_str(b, "text", text);
    json_push(c, b);
    json_set(m, "content", c);
    json_set_bool(m, "isError", err);
    return m;
}

static const char *add(session_t *s, json_t *m) {
    const char *id = session_append_message(s, m);
    json_free(m);
    return id;
}

/* the tests/tui/specs/tree.json session: a fork after the first exchange */
static session_t *seeded(char ids[16][64]) {
    session_t *s = session_in_memory("/tmp/wp8");
    snprintf(ids[0], 64, "%s", session_append_model_change(s, "mock/mock-1", NULL));
    snprintf(ids[1], 64, "%s", add(s, user("Set up the project skeleton")));
    snprintf(ids[2], 64, "%s", add(s, assistant("I'll start by looking at the layout.")));
    snprintf(ids[3], 64, "%s", add(s, tool_call("call_1", "read", "{\"path\":\"README.md\"}")));
    snprintf(ids[4], 64, "%s", add(s, tool_result("call_1", "read", "# Demo\nA demo project.", false)));
    snprintf(ids[5], 64, "%s", add(s, assistant("The README describes a demo project. I will add a src directory next.")));
    snprintf(ids[6], 64, "%s", add(s, user("Now add a build script")));
    snprintf(ids[7], 64, "%s", add(s, tool_call("call_2", "bash", "{\"command\":\"echo build > build.sh\"}")));
    snprintf(ids[8], 64, "%s", add(s, tool_result("call_2", "bash", "", false)));
    snprintf(ids[9], 64, "%s", add(s, assistant("Added build.sh with a placeholder build step.")));
    session_branch(s, ids[5]);
    snprintf(ids[10], 64, "%s", add(s, user("Actually, write a Makefile instead")));
    snprintf(ids[11], 64, "%s", add(s, assistant("Sure, a Makefile is a better fit for this project.")));
    snprintf(ids[12], 64, "%s", session_append_label(s, ids[11], "makefile-path"));
    snprintf(ids[13], 64, "%s", add(s, user("And add a test target")));
    snprintf(ids[14], 64, "%s", add(s, assistant("Added a test target that runs the unit tests.")));
    return s;
}

static void test_branch_summary(void) {
    char ids[16][64];
    session_t *s = seeded(ids);
    char *common = NULL;
    /* leaving the Makefile branch for the build-script branch: old leaf back to the fork point */
    json_t *entries = branchsum_collect(s, s->leaf_id, ids[9], &common);
    CHECK(str_eq(common, ids[5]));
    CHECK(json_len(entries) == 5);
    CHECK(str_eq(json_get_str(json_at(entries, 0), "id"), ids[10]));
    CHECK(str_eq(json_get_str(json_at(entries, 4), "id"), ids[14]));
    json_t *ops = NULL;
    char *prompt = branchsum_prompt(entries, 0, NULL, "xml", &ops);
    /* byte-identical to omp 18.14.2's request for the same navigation (mock model, xml dialect) */
    const char *head = "<conversation>\n\n\nHuman: Actually, write a Makefile instead\n\nAssistant: Sure, a Makefile is a better "
                       "fit for this project.\n\nHuman: And add a test target\n\nAssistant: Added a test target that runs the unit "
                       "tests.\n</conversation>\n\nYou MUST create a structured summary of the conversation branch for context when "
                       "returning.\n\nYou MUST use EXACT format:\n\n## Goal\n\n";
    CHECK(prompt && !strncmp(prompt, head, strlen(head)));
    CHECK(str_ends(prompt, "You MUST preserve exact file paths, function names, error messages."));
    free(prompt);
    prompt = branchsum_prompt(entries, 0, "Focus on the build.", "xml", &(json_t *){NULL});
    CHECK(str_ends(prompt, "</conversation>\n\nFocus on the build."));
    free(prompt);
    json_free(ops);
    json_free(entries);
    free(common);

    /* tool calls / results in both legacy transcript dialects */
    json_t *msgs = json_arr();
    json_push(msgs, user("go"));
    json_push(msgs, tool_call("c1", "read", "{\"path\":\"a.c\",\"limit\":5}"));
    json_push(msgs, tool_result("c1", "read", "int x;", false));
    json_push(msgs, tool_result("c2", "bash", "boom", true));
    char *x = branchsum_serialize(msgs, "xml");
    CHECK(str_eq(x, "\n\nHuman: go\n\nAssistant: <invoke name=\"read\"><parameter name=\"path\">\"a.c\"</parameter><parameter "
                    "name=\"limit\">5</parameter></invoke>\n\nHuman: <tool_response>\nint x;\n</tool_response>\n<tool_response>\nboom\n</tool_response>"));
    free(x);
    x = branchsum_serialize(msgs, "anthropic");
    CHECK(str_eq(x, "\n\nHuman: go\n\nAssistant: <function_calls>\n<invoke name=\"read\"><parameter name=\"path\">\"a.c\"</parameter><parameter "
                    "name=\"limit\">5</parameter></invoke>\n</function_calls>\n\nHuman: <function_results>\n<result>\n<tool_name>read</tool_name>\n"
                    "<stdout>int x;</stdout>\n</result>\n<error>\n<tool_name>bash</tool_name>\n<stderr>boom</stderr>\n</error>\n</function_results>"));
    free(x);
    json_free(msgs);
    CHECK(str_eq(branchsum_dialect(NULL, "claude-sonnet-4-5"), "anthropic"));
    CHECK(str_eq(branchsum_dialect(NULL, "mock-1"), "xml"));

    /* stored summary: preamble + text, then the file list (read / modified, path tree) */
    json_t *fo = json_parse_str("{\"read\":[\"src/a.c\",\"src/b.c\"],\"written\":[\"README.md\"],\"edited\":[\"src/b.c\"]}");
    json_t *rf = NULL, *mf = NULL;
    char *sum = branchsum_finish("## Goal\nship it", fo, &rf, &mf);
    CHECK(str_eq(sum, "User explored another conversation branch, then returned here.\nExploration summary:## Goal\nship it\n\n"
                      "<files>\nREADME.md (Write)\n# src/\na.c (Read)\nb.c (RW)\n</files>"));
    CHECK(json_len(rf) == 1 && str_eq(json_sv(json_at(rf, 0)), "src/a.c"));
    CHECK(json_len(mf) == 2 && str_eq(json_sv(json_at(mf, 0)), "README.md"));
    free(sum);
    json_free(rf), json_free(mf), json_free(fo);
    session_free(s);
}

/* ---------- tree selector ---------- */

static char *last_select;
static bool last_summarize, cancelled;
static char *last_label_id, *last_label;

static void on_select(void *ud, const char *id, bool summarize) {
    UNUSED(ud);
    free(last_select);
    last_select = xstrdup(id);
    last_summarize = summarize;
}
static void on_cancel(void *ud) {
    UNUSED(ud);
    cancelled = true;
}
static void on_label(void *ud, const char *id, const char *label) {
    UNUSED(ud);
    free(last_label_id), free(last_label);
    last_label_id = xstrdup(id);
    last_label = label ? xstrdup(label) : NULL;
}

static void plain(tui_component_t *c, int width, strvec_t *out) {
    strvec_t raw = {0};
    tui_component_render(c, width, &raw);
    for (size_t i = 0; i < raw.n; i++) {
        char *p = tui_strip_ansi(raw.v[i]);
        size_t n = strlen(p);
        while (n && p[n - 1] == ' ') p[--n] = 0;
        strvec_push_own(out, p);
    }
    strvec_free(&raw);
}

static void key(tui_component_t *c, const char *bytes) {
    tui_key_t keys[64];
    size_t n = tui_keys_decode_all(bytes, strlen(bytes), keys, 64);
    for (size_t i = 0; i < n; i++) {
        tui_component_handle_key(c, &keys[i]);
        tui_key_free(&keys[i]);
    }
}

static void test_tree_selector(void) {
    char ids[16][64];
    session_t *s = seeded(ids);
    ui_tree_cb_t cb = {on_select, on_cancel, on_label, NULL};
    tui_component_t *c = ui_tree_selector_new(s->entries, ids[14], 40, "default", &cb);
    strvec_t out = {0};
    plain(c, 120, &out);
    /* omp 18.14.2 at 120x40 (tests/tui/specs/tree.json) */
    static const char *const WANT[] = {
        "╭─ Session Tree ───────────────────────────────────────────────────────────────────────────────────────────────────────╮",
        "│                                                                                                                      │",
        "│ ⏎: switch. Alt+↑/Alt+↓: previous/next turn. PgUp/PgDn (←/→): page. Home/End: first/last item. ⇧⏎: summarize & switc… │",
        "│ Search:                                                                                                              │",
        "├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤",
        "│                                                                                                                      │",
        "│   • user: Set up the project skeleton                                                                                │",
        "│   • assistant: I'll start by looking at the layout.                                                                  │",
        "│   • [read: README.md]                                                                                                │",
        "│   • assistant: The README describes a demo project. I will add a src directory next.                                 │",
        "│   ├─ • user: Actually, write a Makefile instead                                                                      │",
        "│   │  • [makefile-path] assistant: Sure, a Makefile is a better fit for this project.                                 │",
        "│   │  • user: And add a test target                                                                                   │",
        "│ › │  • assistant: Added a test target that runs the unit tests.                                                      │",
        "│   └─ user: Now add a build script                                                                                    │",
        "│      [bash: echo build > build.sh]                                                                                   │",
        "│      assistant: Added build.sh with a placeholder build step.                                                        │",
        "│                                                                                                                      │",
        "╰──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯",
    };
    CHECK(out.n == ARRAY_LEN(WANT));
    for (size_t i = 0; i < out.n && i < ARRAY_LEN(WANT); i++) {
        if (!str_eq(out.v[i], WANT[i])) fprintf(stderr, "row %zu:\n got  %s\n want %s\n", i, out.v[i], WANT[i]);
        CHECK(str_eq(out.v[i], WANT[i]));
    }
    strvec_free(&out);
    /* user-only filter: the selection walks up to the user request */
    key(c, "\x1b[111;5u"); /* ctrl+o (kitty) -> no-tools */
    key(c, "\x1b[111;5u"); /* -> user-only */
    plain(c, 120, &out);
    CHECK(strstr(out.v[out.n - 3], "[user]"));
    bool sel_user = false;
    for (size_t i = 0; i < out.n; i++) sel_user |= strstr(out.v[i], "› │  • user: And add a test target") != NULL;
    CHECK(sel_user);
    strvec_free(&out);
    key(c, "\x1b" "d"); /* alt+d -> default */
    /* search narrows; Esc clears the search before closing */
    key(c, "build");
    plain(c, 120, &out);
    CHECK(strstr(out.v[3], "Search: build"));
    size_t rows = 0;
    for (size_t i = 6; i + 2 < out.n; i++) rows += out.v[i][0] && strstr(out.v[i], "build") != NULL;
    CHECK(rows == 2); /* the bash result has no searchable text (its call args are not indexed) */
    if (rows != 2 || getenv("TREE_DEBUG"))
        for (size_t i = 0; i < out.n; i++) fprintf(stderr, "%s\n", out.v[i]);
    strvec_free(&out);
    key(c, "\x1b");
    CHECK(!cancelled);
    plain(c, 120, &out);
    CHECK(str_eq(out.v[3], "│ Search:                                                                                                              │"));
    strvec_free(&out);
    /* Shift+L edits the selected entry's label */
    key(c, "\x1b[H\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B\x1b[B"); /* home, 6 x down: "user: And add a test target" */
    key(c, "L");
    plain(c, 120, &out);
    CHECK(strstr(out.v[6], "Label (empty to remove):"));
    strvec_free(&out);
    key(c, "tests\r");
    CHECK(str_eq(last_label_id, ids[13]) && str_eq(last_label, "tests"));
    plain(c, 120, &out);
    bool labeled = false;
    for (size_t i = 0; i < out.n; i++) labeled |= strstr(out.v[i], "• [tests] user: And add a test target") != NULL;
    CHECK(labeled);
    strvec_free(&out);
    /* Enter switches, Shift+Enter summarizes & switches */
    key(c, "\r");
    CHECK(str_eq(last_select, ids[13]) && !last_summarize);
    key(c, "\x1b[13;2u");
    CHECK(str_eq(last_select, ids[13]) && last_summarize);
    key(c, "\x1b");
    CHECK(cancelled);
    tui_component_destroy(c);

    /* everything hidden by the filter: the empty state explains why */
    c = ui_tree_selector_new(s->entries, ids[14], 40, "labeled-only", &cb);
    tui_component_destroy(c);
    session_t *e = session_in_memory("/tmp/wp8");
    session_append_model_change(e, "mock/mock-1", NULL);
    c = ui_tree_selector_new(e->entries, e->leaf_id, 40, "default", &cb);
    plain(c, 120, &out);
    CHECK(strstr(out.v[6], "1 entries hidden by the current filter [default]"));
    CHECK(strstr(out.v[7], "Press Alt+A to show all, Alt+D for default"));
    CHECK(strstr(out.v[8], "(0/1)"));
    strvec_free(&out);
    tui_component_destroy(c);
    session_free(e);
    session_free(s);
    free(last_select), free(last_label_id), free(last_label);
}

/* the resume picker's first pass: titled sessions from the file heads only */
static void test_picker_first_pass(void) {
    char *dir = xasprintf("%s/omc-picker-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    fs_mkdir_p(dir, 0777);
    char *a = path_join(dir, "a.jsonl"), *b = path_join(dir, "b.jsonl"), *c = path_join(dir, "c.jsonl");
    const char *titled = "{\"type\":\"title\",\"title\":\"Named\"}\n{\"type\":\"session\",\"id\":\"s1\",\"cwd\":\"/w\"}\n"
                         "{\"type\":\"message\",\"message\":{\"role\":\"user\",\"content\":\"hi\"}}\n";
    const char *untitled = "{\"type\":\"session\",\"id\":\"s2\",\"cwd\":\"/w\"}\n";
    fs_write(a, titled, strlen(titled));
    fs_write(b, untitled, strlen(untitled));
    fs_write(c, "not a session\n", 14);
    session_list_t l;
    CHECK(session_list_titled(dir, &l) == 0 && l.n == 1);
    if (l.n == 1) {
        CHECK(str_eq(l.v[0].title, "Named") && str_eq(l.v[0].id, "s1") && !l.v[0].first_message && l.v[0].message_count == 0);
        session_list_remove(&l, 0);
        CHECK(l.n == 0);
    }
    session_list_free(&l);
    CHECK(session_dir_has_sessions(dir));
    session_list(dir, &l);
    CHECK(l.n == 2);
    session_list_free(&l);
    unlink(a);
    unlink(b);
    CHECK(!session_dir_has_sessions(dir));
    unlink(c);
    rmdir(dir);
    free(a), free(b), free(c), free(dir);
}

/* The UI thread reads the main session (a transcript rebuild after a compaction, the status line's title, /btw)
 * while the prompt worker appends to it: by_id rehashes, leaf_id is replaced and freed, entries grow. Those reads
 * go through the locked accessors; without the lock this races (TSan) and under ASan the reader walks a freed
 * hash table (the strcmp crash). */
typedef struct {
    session_t *s;
    volatile int done;
} race_t;

static void *race_writer(void *p) {
    race_t *r = p;
    for (int i = 0; i < 4000; i++) {
        json_t *m = user("x");
        session_append_message(r->s, m);
        json_free(m);
        if (i % 500 == 0) {
            char t[32];
            snprintf(t, sizeof t, "title %d", i);
            session_set_title(r->s, t, "auto");
        }
    }
    r->done = 1;
    return NULL;
}

static void test_session_concurrent_reads(void) {
    race_t r = {session_in_memory("/tmp"), 0};
    pthread_t th;
    CHECK(pthread_create(&th, NULL, race_writer, &r) == 0);
    size_t last = 0;
    bool monotonic = true;
    int reads = 0;
    while (!r.done || reads < 3) {
        json_t *path = session_current_path(r.s);
        char *leaf = session_leaf_dup(r.s), *title = session_title_dup(r.s);
        size_t n = json_len(path);
        if (n < last) monotonic = false;
        last = n;
        if (n && leaf && !str_eq(json_get_str(json_at(path, n - 1), "id"), leaf) && json_len(path) == n) {
            /* the leaf may move between the two calls; it must at least name an entry of the session */
            if (!session_get(r.s, leaf)) monotonic = false;
        }
        free(leaf), free(title);
        json_free(path);
        reads++;
    }
    pthread_join(th, NULL);
    CHECK(monotonic);
    json_t *path = session_current_path(r.s);
    CHECK(json_len(path) == 4000);
    json_free(path);
    char *title = session_title_dup(r.s);
    CHECK(str_eq(title, "title 3500"));
    free(title);
    session_free(r.s);
}

/* a failed append (disk full, read-only file) is reported once and the next entry rewrites the whole file, so the file
 * never ends up with a hole in its parent chain (omp #rewriteRequired / onPersistenceError) */
static void test_session_write_failure(void) {
    if (geteuid() == 0) return; /* root writes through file modes */
    char tmpl[] = "/tmp/omc-tree-XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL);
    if (!dir) return;
    session_t *s = session_create("/tmp", dir);
    json_t *u = json_parse_str("{\"role\":\"user\",\"content\":\"one\",\"timestamp\":1}");
    json_t *a = json_parse_str("{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"two\"}],\"timestamp\":2}");
    session_append_message(s, u);
    session_append_message(s, a); /* materializes the file */
    CHECK(s->on_disk && session_take_persist_error(s) == NULL);
    CHECK(chmod(s->file, 0444) == 0 && chmod(dir, 0555) == 0); /* no append, no rewrite (temp file + rename) */
    session_append_message(s, u); /* EACCES */
    char *e = session_take_persist_error(s);
    CHECK(e && strstr(e, "EACCES: permission denied, open '") == e);
    free(e);
    session_append_message(s, a); /* still failing: logged, not reported again */
    CHECK(session_take_persist_error(s) == NULL);
    char *still = session_persist_retry(s);
    CHECK(still && strstr(still, "EACCES"));
    free(still);
    CHECK(chmod(dir, 0755) == 0);
    session_append_message(s, u); /* the store accepts writes again: the full rewrite restores all five entries */
    CHECK(session_take_persist_error(s) == NULL && !s->rewrite_required);
    CHECK(session_persist_retry(s) == NULL);
    char *err = NULL;
    session_t *r = session_open(s->file, &err);
    CHECK(r && json_len(r->entries) == json_len(s->entries) && json_len(r->entries) == 5);
    json_t *path = r ? session_current_path(r) : NULL;
    CHECK(json_len(path) == 5); /* the parent chain is whole */
    json_free(path);
    session_free(r);
    free(err);
    unlink(s->file);
    rmdir(dir);
    session_free(s);
    json_free(u);
    json_free(a);
}

int main(void) {
    test_session_write_failure();
    test_session_concurrent_reads();
    test_picker_first_pass();
    char *err = NULL;
    tui_theme_t *th = tui_theme_load("titanium", TUI_COLOR_TRUECOLOR, &err);
    if (th) tui_theme_set(th);
    free(err);
    test_branch_summary();
    test_tree_selector();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
