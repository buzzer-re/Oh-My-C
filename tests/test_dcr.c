/* test_dcr.c - date/cwd reminder injector and its session journal (behaviour pinned by omp's date-cwd-reminder.test.ts) */
#include <stdarg.h>
#include "coding/dcr.h"
#include <stdio.h>

static int failures;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                \
        }                                                              \
    } while (0)

static json_t *user(const char *text, int64_t ts) {
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_set_str(m, "content", text);
    json_set_int(m, "timestamp", ts);
    return m;
}

static json_t *assistant(int64_t ts) {
    json_t *m = json_obj();
    json_set_str(m, "role", "assistant");
    json_t *c = json_arr();
    json_t *b = json_obj();
    json_set_str(b, "type", "text");
    json_set_str(b, "text", "reply");
    json_push(c, b);
    json_set(m, "content", c);
    json_set_int(m, "timestamp", ts);
    return m;
}

static json_t *tool_result(int64_t ts) {
    json_t *m = json_obj();
    json_set_str(m, "role", "toolResult");
    json_set_str(m, "toolCallId", "call-1");
    json_set_str(m, "toolName", "read");
    json_set_str(m, "content", "notes");
    json_set_int(m, "timestamp", ts);
    return m;
}

/* owned array from a NULL-terminated list of messages it takes */
static json_t *arr(json_t *first, ...) {
    json_t *a = json_arr();
    va_list ap;
    va_start(ap, first);
    for (json_t *m = first; m; m = va_arg(ap, json_t *)) json_push(a, m);
    va_end(ap);
    return a;
}

/* transforms and frees `msgs`; returns what would go out (owned) */
static json_t *send(dcr_t *d, json_t *msgs, const char *date) {
    json_t *out = dcr_transform(d, msgs, date, "/work");
    if (!out) return msgs;
    json_free(msgs);
    return out;
}

static char *prefixed(const char *date, const char *text) {
    char *r = dcr_render(date, "/work");
    char *t = xasprintf("%s\n\n%s", r, text);
    free(r);
    return t;
}

static bool content_is(const json_t *msgs, size_t i, const char *date, const char *text) {
    char *want = prefixed(date, text);
    bool ok = str_eq(json_get_str(json_at(msgs, i), "content"), want);
    if (!ok) fprintf(stderr, "  got [%s]\n want [%s]\n", json_get_str(json_at(msgs, i), "content"), want);
    free(want);
    return ok;
}

static bool plain_is(const json_t *msgs, size_t i, const char *text) { return str_eq(json_get_str(json_at(msgs, i), "content"), text); }

static bool control_is(const json_t *msgs, size_t i, const char *date) {
    const json_t *m = json_at(msgs, i);
    char *r = dcr_render(date, "/work");
    bool ok = dcr_is_control(m) && str_eq(json_get_str(m, "role"), "developer") && json_get_bool(m, "synthetic", false) &&
              str_eq(json_get_str(m, "content"), r);
    free(r);
    return ok;
}

/* ---------- an in-memory journal ---------- */
typedef struct {
    json_t *placements;
    int loads;
    const char *scope;
} mem_journal_t;

static json_t *mj_load(void *ud) {
    mem_journal_t *j = ud;
    j->loads++;
    return json_clone(j->placements);
}
static void mj_record(void *ud, json_t *p) { json_push(((mem_journal_t *)ud)->placements, p); }
static char *mj_scope(void *ud) {
    mem_journal_t *j = ud;
    return j->scope ? xstrdup(j->scope) : NULL;
}
static dcr_journal_t mj_journal(mem_journal_t *j) { return (dcr_journal_t){.ud = j, .load = mj_load, .record = mj_record, .scope = mj_scope}; }

/* ---------- no journal ---------- */
static void test_first_reminder(void) {
    dcr_t *d = dcr_new(NULL);
    json_t *out = send(d, arr(user("hello", 1), assistant(2), NULL), "2026-08-14");
    CHECK(content_is(out, 0, "2026-08-14", "hello"));
    CHECK(json_len(out) == 2);
    json_free(out);
    /* a text part goes in front of image parts */
    dcr_t *d2 = dcr_new(NULL);
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_set_int(m, "timestamp", 1);
    json_t *c = json_arr(), *img = json_obj();
    json_set_str(img, "type", "image");
    json_push(c, img);
    json_set(m, "content", c);
    out = send(d2, arr(m, NULL), "2026-08-14");
    const json_t *parts = json_get(json_at(out, 0), "content");
    char *r = dcr_render("2026-08-14", "/work");
    CHECK(json_len(parts) == 2 && str_eq(json_get_str(json_at(parts, 0), "text"), r) && str_eq(json_get_str(json_at(parts, 1), "type"), "image"));
    free(r);
    json_free(out);
    /* nothing to attach it to */
    json_t *none = arr(assistant(1), NULL);
    CHECK(dcr_transform(d2, none, "2026-08-14", "/work") == NULL);
    json_free(none);
    dcr_free(d);
    dcr_free(d2);
}

static void test_changed_moves_to_next_turn(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    json_t *out = send(d, arr(user("first", 1), assistant(2), user("second", 3), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "first"));
    CHECK(content_is(out, 2, "2026-08-15", "second"));
    json_free(out);
    /* replay is byte-identical */
    out = send(d, arr(user("first", 1), assistant(2), user("second", 3), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "first"));
    CHECK(content_is(out, 2, "2026-08-15", "second"));
    json_free(out);
    dcr_free(d);
}

static void test_mid_loop_control(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    json_t *out = send(d, arr(user("first", 1), assistant(2), tool_result(3), NULL), "2026-08-15");
    CHECK(json_len(out) == 4);
    CHECK(control_is(out, 3, "2026-08-15"));
    json_free(out);
    out = send(d, arr(user("first", 1), assistant(2), tool_result(3), assistant(4), tool_result(5), NULL), "2026-08-15");
    CHECK(json_len(out) == 6);
    CHECK(control_is(out, 3, "2026-08-15"));
    json_free(out);
    dcr_free(d);
}

static void test_compaction_keeps_retained(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    json_free(send(d, arr(user("first", 1), assistant(2), user("second", 3), NULL), "2026-08-15"));
    json_t *out = send(d, arr(user("summary", 10), user("second", 3), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-15", "summary"));
    CHECK(content_is(out, 1, "2026-08-15", "second"));
    json_free(out);
    dcr_free(d);
}

static void test_same_ms_twins(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("ok", 1), NULL), "2026-08-14"));
    json_t *out = send(d, arr(user("ok", 1), assistant(2), user("ok", 1), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "ok"));
    CHECK(content_is(out, 2, "2026-08-15", "ok"));
    json_free(out);
    /* a later same-millisecond turn not resent: the first keeps its reminder */
    out = send(d, arr(user("ok", 1), assistant(2), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "ok"));
    json_free(out);
    dcr_free(d);
}

static void test_recreated_first_turn(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("BOOT", 100), user("task", 1), NULL), "2026-08-14"));
    json_t *out = send(d, arr(user("BOOT", 200), user("task", 1), assistant(2), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "BOOT"));
    /* the changed date is appended, never written into the re-created turn */
    CHECK(control_is(out, 3, "2026-08-15"));
    json_free(out);
    dcr_free(d);
}

static void test_other_conversation_between(void) {
    dcr_t *d = dcr_new(NULL);
    json_free(send(d, arr(user("main", 1), NULL), "2026-08-14"));
    json_t *side = send(d, arr(user("side", 50), NULL), "2026-08-15");
    CHECK(content_is(side, 0, "2026-08-15", "side"));
    json_free(side);
    json_t *out = send(d, arr(user("main", 1), assistant(2), NULL), "2026-08-15");
    CHECK(content_is(out, 0, "2026-08-14", "main"));
    json_free(out);
    dcr_free(d);
}

/* ---------- journal ---------- */
static void test_journal_resend(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    /* the date rolls over mid tool loop: the continuation carries it as a developer turn */
    json_t *sent = send(d, arr(user("first", 1), assistant(2), tool_result(3), NULL), "2026-08-15");
    CHECK(json_len(sent) == 4);
    /* a resumed session or revived subagent: a new process, days later */
    dcr_t *d2 = dcr_new(&jj);
    json_t *out = send(d2, arr(user("first", 1), assistant(2), tool_result(3), assistant(4), user("next", 5), NULL), "2026-08-17");
    CHECK(content_is(out, 0, "2026-08-14", "first"));
    CHECK(control_is(out, 3, "2026-08-15"));
    CHECK(content_is(out, 5, "2026-08-17", "next"));
    json_free(out);
    json_free(sent);
    dcr_free(d);
    dcr_free(d2);
    json_free(j.placements);
}

static void test_journal_records_once(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    json_free(send(d, arr(user("first", 1), assistant(2), user("second", 3), NULL), "2026-08-15"));
    CHECK(json_len(j.placements) == 2);
    const json_t *p0 = json_at(j.placements, 0), *p1 = json_at(j.placements, 1);
    CHECK(str_eq(json_get_str(p0, "conversation"), "user:1:") && str_eq(json_get_str(p0, "turn"), "user:1:") &&
          str_eq(json_get_str(p0, "placement"), "prefix") && json_get_str(p0, "firstTurn"));
    CHECK(str_eq(json_get_str(p1, "conversation"), "user:1:") && str_eq(json_get_str(p1, "turn"), "user:3:") &&
          str_eq(json_get_str(p1, "placement"), "prefix") && !json_get(p1, "firstTurn"));
    /* wire order of the placement's keys */
    char *dump = json_dump_js(p1);
    CHECK(str_starts(dump, "{\"conversation\":\"user:1:\",\"turn\":\"user:3:\",\"reminder\":"));
    CHECK(str_ends(dump, ",\"placement\":\"prefix\"}"));
    free(dump);
    dcr_free(d);
    json_free(j.placements);
}

static void test_journal_empty_and_interrupted(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    json_t *out = send(d, arr(user("old", 1), assistant(2), NULL), "2026-08-17");
    CHECK(content_is(out, 0, "2026-08-17", "old"));
    json_free(out);
    dcr_free(d);
    json_free(j.placements);

    /* a turn whose request was interrupted before any reply counts as sent */
    j.placements = json_arr();
    d = dcr_new(&jj);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    dcr_free(d);
    d = dcr_new(&jj);
    out = send(d, arr(user("first", 1), NULL), "2026-08-16");
    CHECK(content_is(out, 0, "2026-08-14", "first"));
    CHECK(json_len(out) == 2 && control_is(out, 1, "2026-08-16"));
    json_free(out);
    dcr_free(d);
    json_free(j.placements);
}

static void test_journal_scope_switch(void) {
    /* two forks share their first turn; only the second sent `branch` */
    json_t *one = json_arr(), *two = json_arr();
    char *r14 = dcr_render("2026-08-14", "/work"), *r15 = dcr_render("2026-08-15", "/work");
    json_t *p = json_obj();
    json_set_str(p, "conversation", "user:1:");
    json_set_str(p, "turn", "user:1:");
    json_set_str(p, "reminder", r14);
    json_set_str(p, "placement", "prefix");
    json_push(two, p);
    p = json_obj();
    json_set_str(p, "conversation", "user:1:");
    json_set_str(p, "turn", "user:7:");
    json_set_str(p, "reminder", r15);
    json_set_str(p, "placement", "prefix");
    json_push(two, p);
    free(r14);
    free(r15);
    mem_journal_t j = {.placements = one, .scope = "one"};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    j.placements = two;
    j.scope = "two";
    json_t *out = send(d, arr(user("first", 1), assistant(2), user("branch", 7), assistant(8), user("next", 9), NULL), "2026-08-16");
    CHECK(content_is(out, 2, "2026-08-15", "branch"));
    CHECK(content_is(out, 4, "2026-08-16", "next"));
    json_free(out);
    dcr_free(d);
    json_free(one);
    json_free(two);
}

static void test_journal_renumbered(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    json_free(send(d, arr(user("ok", 1), NULL), "2026-08-14"));
    /* two same-millisecond turns: the second is user:1:#1 and takes the new date */
    json_free(send(d, arr(user("ok", 1), assistant(2), user("ok", 1), NULL), "2026-08-15"));
    /* the compaction drops the first, so the survivor is user:1: now */
    json_t *compacted = send(d, arr(user("summary", 3), user("ok", 1), NULL), "2026-08-15");
    CHECK(content_is(compacted, 1, "2026-08-15", "ok"));
    dcr_free(d);
    d = dcr_new(&jj);
    json_t *out = send(d, arr(user("summary", 3), user("ok", 1), assistant(4), user("next", 5), NULL), "2026-08-16");
    CHECK(json_equal(json_at(out, 0), json_at(compacted, 0)));
    CHECK(json_equal(json_at(out, 1), json_at(compacted, 1)));
    json_free(out);
    json_free(compacted);
    dcr_free(d);
    json_free(j.placements);
}

static void test_journal_recreated_load_once(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    int64_t stamp = 1000;
    json_free(send(d, arr(user("BOOT", stamp++), user("task", 1), NULL), "2026-08-14"));
    json_t *sent = NULL;
    for (int r = 0; r < 5; r++) {
        json_t *msgs = arr(user("BOOT", stamp++), user("task", 1), NULL);
        for (int k = 0; k <= r; k++) json_push(msgs, assistant(2 + k));
        json_free(sent);
        sent = send(d, msgs, "2026-08-14");
    }
    CHECK(j.loads == 1);
    dcr_free(d);
    d = dcr_new(&jj);
    json_t *msgs = arr(user("BOOT", stamp++), user("task", 1), NULL);
    for (int k = 0; k < 5; k++) json_push(msgs, assistant(2 + k));
    json_push(msgs, assistant(20));
    json_push(msgs, user("next", 50));
    json_t *out = send(d, msgs, "2026-08-16");
    CHECK(content_is(out, 0, "2026-08-14", "BOOT"));
    CHECK(content_is(out, json_len(out) - 1, "2026-08-16", "next"));
    json_free(out);
    json_free(sent);
    dcr_free(d);
    json_free(j.placements);
}

/* ---------- side thread: no session access, placements wait for the writer ---------- */
static void test_side_thread_defers(void) {
    mem_journal_t j = {.placements = json_arr()};
    dcr_journal_t jj = mj_journal(&j);
    dcr_t *d = dcr_new(&jj);
    dcr_set_side_thread(true);
    json_free(send(d, arr(user("side", 1), NULL), "2026-08-14"));
    dcr_set_side_thread(false);
    CHECK(json_len(j.placements) == 0 && j.loads == 0);
    json_free(send(d, arr(user("main", 5), NULL), "2026-08-14"));
    CHECK(json_len(j.placements) == 2);
    dcr_free(d);
    json_free(j.placements);
}

/* ---------- the session journal ---------- */
static const char *append(session_t *s, json_t *m) {
    const char *id = session_append_message(s, m);
    json_free(m);
    return id;
}

static void test_session_tree_move(void) {
    session_t *s = session_in_memory("/work");
    session_t *sp = s;
    dcr_journal_t j1 = dcr_session_journal(&sp);
    dcr_t *earlier = dcr_new(&j1);
    append(s, user("first", 1));
    json_free(send(earlier, arr(user("first", 1), NULL), "2026-08-14"));
    char *fork_point = xstrdup(append(s, assistant(2)));
    append(s, user("side b", 7));
    json_t *sent_b = send(earlier, arr(user("first", 1), assistant(2), user("side b", 7), NULL), "2026-08-15");
    char *leaf_b = xstrdup(append(s, assistant(8)));
    session_branch(s, fork_point);
    append(s, user("side a", 9));

    /* reopened later: the new process serves branch A, then the tree moves to branch B */
    dcr_journal_t j2 = dcr_session_journal(&sp);
    dcr_t *resumed = dcr_new(&j2);
    json_free(send(resumed, arr(user("first", 1), assistant(2), user("side a", 9), NULL), "2026-08-16"));
    session_branch(s, leaf_b);
    append(s, user("next", 10));
    json_t *moved = send(resumed, arr(user("first", 1), assistant(2), user("side b", 7), assistant(8), user("next", 10), NULL), "2026-08-16");
    CHECK(json_equal(json_at(moved, 0), json_at(sent_b, 0)));
    CHECK(json_equal(json_at(moved, 2), json_at(sent_b, 2)));
    CHECK(content_is(moved, 4, "2026-08-16", "next"));
    json_free(moved);
    json_free(sent_b);
    free(fork_point);
    free(leaf_b);
    dcr_free(earlier);
    dcr_free(resumed);
    session_free(s);
}

static void test_session_reload_other_writer(void) {
    session_t *s = session_in_memory("/work");
    session_t *sp = s;
    dcr_journal_t j1 = dcr_session_journal(&sp), j2 = dcr_session_journal(&sp);
    dcr_t *here = dcr_new(&j1);
    append(s, user("first", 1));
    json_free(send(here, arr(user("first", 1), NULL), "2026-08-14"));
    append(s, assistant(2));
    /* another process continues the same session the next day; this one then reloads it */
    dcr_t *elsewhere = dcr_new(&j2);
    append(s, user("second", 5));
    json_t *sent = send(elsewhere, arr(user("first", 1), assistant(2), user("second", 5), NULL), "2026-08-15");
    append(s, assistant(6));
    append(s, user("third", 9));
    json_t *out = send(here, arr(user("first", 1), assistant(2), user("second", 5), assistant(6), user("third", 9), NULL), "2026-08-15");
    CHECK(json_equal(json_at(out, 0), json_at(sent, 0)));
    CHECK(json_equal(json_at(out, 2), json_at(sent, 2)));
    CHECK(plain_is(out, 4, "third"));
    json_free(out);
    json_free(sent);
    dcr_free(here);
    dcr_free(elsewhere);
    session_free(s);
}

static void test_session_clear(void) {
    session_t *s = session_in_memory("/work");
    session_t *sp = s;
    dcr_journal_t j = dcr_session_journal(&sp);
    dcr_t *d = dcr_new(&j);
    /* a context hook's static bootstrap message opens both histories */
    append(s, user("BOOT", 1));
    json_free(send(d, arr(user("BOOT", 1), NULL), "2026-08-14"));
    append(s, assistant(2));
    session_append_reset_boundary(s);
    json_t *out = send(d, arr(user("BOOT", 50), NULL), "2026-08-16");
    CHECK(content_is(out, 0, "2026-08-16", "BOOT"));
    json_free(out);
    dcr_free(d);
    session_free(s);
}

/* the entries hold the custom type and the data upstream writes, and a rebuilt context never shows them */
static void test_session_entries(void) {
    session_t *s = session_in_memory("/work");
    session_t *sp = s;
    dcr_journal_t j = dcr_session_journal(&sp);
    dcr_t *d = dcr_new(&j);
    append(s, user("first", 1));
    json_free(send(d, arr(user("first", 1), NULL), "2026-08-14"));
    const json_t *e = json_at(s->entries, json_len(s->entries) - 1);
    CHECK(str_eq(json_get_str(e, "type"), "custom") && str_eq(json_get_str(e, "customType"), "date-cwd-reminder"));
    CHECK(str_eq(json_get_str(json_get(e, "data"), "conversation"), "user:1:"));
    session_context_t c;
    session_build_context(s, &c);
    CHECK(json_len(c.messages) == 1);
    session_context_free(&c);
    dcr_free(d);
    session_free(s);
}

int main(void) {
    test_first_reminder();
    test_changed_moves_to_next_turn();
    test_mid_loop_control();
    test_compaction_keeps_retained();
    test_same_ms_twins();
    test_recreated_first_turn();
    test_other_conversation_between();
    test_journal_resend();
    test_journal_records_once();
    test_journal_empty_and_interrupted();
    test_journal_scope_switch();
    test_journal_renumbered();
    test_journal_recreated_load_once();
    test_side_thread_defers();
    test_session_tree_move();
    test_session_reload_other_writer();
    test_session_clear();
    test_session_entries();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_dcr ok\n");
    return 0;
}
