/* test_btw.c - /btw headless pieces: records, conversation keys, the history sidecar store, reply dedupe. */
#include "coding/btw.h"
#include "core/fs.h"

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

static void test_dedupe(void) {
    char *r = app_dedupe_ephemeral_reply("a\nb\nb\nb\nb\nb\nc", 0);
    CHECK(str_eq(r, "a\nb\n[\xe2\x80\xa6" "5\xc3\x97]\nc"));
    free(r);
    r = app_dedupe_ephemeral_reply("x\nx\nx", 0); /* three repeats stay */
    CHECK(str_eq(r, "x\nx\nx"));
    free(r);
    r = app_dedupe_ephemeral_reply("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9", 16);
    CHECK(utf8_valid(r, strlen(r)) && str_ends(r, "[\xe2\x80\xa6truncated]") && strlen(r) <= 16);
    free(r);
}

static void test_records(void) {
    btw_begin_t b;
    btw_begin_turn("first?", "leaf1", NULL, NULL, "/sitrep", &b);
    json_t *r = b.record;
    const char *order[] = {"question", "answer", "status", "createdAt", "updatedAt", "id", "leafId", "title"};
    CHECK(json_len(r) == ARRAY_LEN(order));
    for (size_t i = 0; i < ARRAY_LEN(order) && i < json_len(r); i++) CHECK(str_eq(r->u.obj.keys[i], order[i]));
    const char *id = json_get_str(r, "id");
    CHECK(id && strlen(id) == 16);
    CHECK(!b.history);
    char want[64];
    snprintf(want, sizeof want, "btw:%s:0", id);
    CHECK(str_eq(b.conversation_key, want));
    CHECK(str_eq(btw_topic_label(r), "/sitrep"));
    CHECK(str_eq(btw_latest_status(r), "running"));
    json_t *wire = btw_wire_record(r);
    CHECK(!json_get(wire, "title"));
    json_free(wire);
    /* a cancelled first turn: the follow-up starts a fresh provider lineage */
    btw_set_status(r, "cancelled", NULL, NULL);
    btw_begin_t f;
    btw_begin_turn("again", "leaf1", r, NULL, NULL, &f);
    snprintf(want, sizeof want, "btw:%s:1", id);
    CHECK(str_eq(f.conversation_key, want));
    CHECK(json_len(f.history) == 1);
    CHECK(btw_turn_count(f.record) == 2);
    CHECK(str_eq(json_get_str(btw_latest_turn(f.record), "question"), "again"));
    btw_set_status(f.record, "complete", "the answer", NULL);
    char *copy = btw_copy_text(f.record);
    CHECK(str_eq(copy, "the answer"));
    free(copy);
    btw_begin_free(&f);
    btw_begin_free(&b);
}

static void test_store(void) {
    char *dir = fs_mktempdir("omc-btw-test");
    char *err = NULL;
    btw_store_t *s = btw_store_open(dir, NULL, &err);
    CHECK(s && json_len(btw_store_records(s)) == 0);
    btw_begin_t b;
    btw_begin_turn("q1", NULL, NULL, NULL, "/title", &b);
    CHECK(btw_store_upsert(s, b.record, &err) == 0);
    char *path = xasprintf("%s/btw-history/entry-%s.json", dir, json_get_str(b.record, "id"));
    char *tpath = xasprintf("%s/btw-history/entry-%s.title", dir, json_get_str(b.record, "id"));
    size_t n = 0;
    char *disk = fs_read(path, &n);
    CHECK(disk && disk[n - 1] == '\n' && !strstr(disk, "title") && strstr(disk, "\"leafId\":null"));
    free(disk);
    char *title = fs_read(tpath, &n);
    CHECK(title && str_eq(title, "/title"));
    free(title);
    /* a second store sees the running turn as interrupted, with its title */
    btw_store_t *s2 = btw_store_open(dir, NULL, &err);
    json_t *recs = btw_store_records(s2);
    CHECK(json_len(recs) == 1 && str_eq(json_get_str(json_at(recs, 0), "status"), "interrupted") &&
          str_eq(json_get_str(json_at(recs, 0), "title"), "/title"));
    json_free(recs);
    btw_store_free(s2);
    /* the owner completes it */
    btw_set_status(b.record, "complete", "done", NULL);
    CHECK(btw_store_upsert(s, b.record, &err) == 0);
    /* someone rewrote the file: a conflict, sticky until retry */
    CHECK(fs_write(path, "{}\n", 3) == 0);
    btw_set_status(b.record, "complete", "done again", NULL);
    CHECK(btw_store_upsert(s, b.record, &err) != 0 && err && strstr(err, "conflict"));
    free(err);
    err = NULL;
    CHECK(btw_store_failed(s));
    /* an invalid record fails the open */
    btw_store_t *s3 = btw_store_open(dir, NULL, &err);
    CHECK(!s3 && err && strstr(err, "Invalid BTW history record"));
    free(err);
    btw_store_free(s);
    btw_begin_free(&b);
    /* memory-only store */
    btw_store_t *m = btw_store_open(NULL, NULL, &err);
    btw_begin_turn("mem", NULL, NULL, NULL, NULL, &b);
    CHECK(btw_store_upsert(m, b.record, &err) == 0 && json_len(btw_store_records(m)) == 1);
    btw_begin_free(&b);
    btw_store_free(m);
    fs_rm_rf(dir);
    free(dir);
    free(path);
    free(tpath);
}

int main(void) {
    test_dedupe();
    test_records();
    test_store();
    if (fails) fprintf(stderr, "%d failure(s)\n", fails);
    else printf("test_btw ok\n");
    return fails ? 1 : 0;
}
