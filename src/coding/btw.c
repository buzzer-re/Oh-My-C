/* btw.c - `/btw` side questions, headless: records, history sidecar store, side turn (see btw.h). */
#include "coding/btw.h"

#include "core/fs.h"
#include "prompt/template.h"
#include "tui/ansi.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- records ---------- */

json_t *btw_latest_turn(const json_t *record) {
    json_t *f = json_get_arr(record, "followUps");
    size_t n = json_len(f);
    return n ? json_at(f, n - 1) : (json_t *)record;
}

const char *btw_latest_status(const json_t *record) {
    const char *s = json_get_str(btw_latest_turn(record), "status");
    return s ? s : "running";
}

size_t btw_turn_count(const json_t *record) { return 1 + json_len(json_get_arr(record, "followUps")); }

size_t btw_turns(const json_t *record, const json_t **out, size_t cap) {
    size_t n = 0;
    if (n < cap) out[n++] = record;
    json_t *t;
    json_arr_foreach(json_get_arr(record, "followUps"), i, t) {
        if (n < cap) out[n++] = t;
    }
    return n;
}

const char *btw_topic_label(const json_t *record) {
    const char *t = json_get_str(record, "title");
    if (t) return t;
    t = json_get_str(record, "question");
    return t ? t : "";
}

static bool blank(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

char *btw_copy_text(const json_t *record) {
    json_t *f = json_get_arr(record, "followUps");
    for (size_t i = json_len(f); i-- > 0;) {
        const char *a = json_get_str(json_at(f, i), "answer");
        if (!blank(a)) return xstrdup(a);
    }
    const char *a = json_get_str(record, "answer");
    return blank(a) ? NULL : xstrdup(a);
}

static json_t *new_turn(const char *question, const json_t *images, int64_t now) {
    json_t *t = json_obj();
    json_set_str(t, "question", question);
    if (json_len(images)) json_set(t, "images", json_clone(images));
    json_set_str(t, "answer", "");
    json_set_str(t, "status", "running");
    json_set_int(t, "createdAt", now);
    json_set_int(t, "updatedAt", now);
    return t;
}

void btw_begin_turn(const char *question, const char *leaf_id, const json_t *previous, const json_t *images,
                    const char *title, btw_begin_t *out) {
    memset(out, 0, sizeof *out);
    int64_t now = now_ms();
    json_t *turn = new_turn(question, images, now);
    ssize_t last_bad = -1;
    if (previous) {
        out->record = json_clone(previous);
        json_t *f = json_get_arr(out->record, "followUps");
        if (!f) {
            f = json_arr();
            json_set(out->record, "followUps", f);
        }
        out->history = json_arr();
        const json_t *turns[1024];
        size_t n = btw_turns(previous, turns, ARRAY_LEN(turns));
        for (size_t i = 0; i < n; i++) {
            json_push(out->history, json_clone(turns[i]));
            if (!str_eq(json_get_str(turns[i], "status"), "complete")) last_bad = (ssize_t)i;
        }
        json_push(f, turn);
    } else {
        out->record = turn;
        char *id = snowflake_next();
        json_set_str(out->record, "id", id);
        free(id);
        json_set(out->record, "leafId", leaf_id ? json_str(leaf_id) : json_null());
        if (title && *title) json_set_str(out->record, "title", title);
    }
    /* a cancelled/failed transport may still be unwinding: start a fresh lineage after that boundary */
    out->conversation_key = xasprintf("btw:%s:%zd", json_get_str(out->record, "id"), last_bad + 1);
}

void btw_begin_free(btw_begin_t *b) {
    json_free(b->record);
    json_free(b->history);
    free(b->conversation_key);
    memset(b, 0, sizeof *b);
}

void btw_patch_latest(json_t *record, const json_t *patch) {
    json_t *t = btw_latest_turn(record);
    for (size_t i = 0; i < json_len(patch); i++) json_set(t, patch->u.obj.keys[i], json_clone(patch->u.obj.vals[i]));
}

void btw_set_status(json_t *record, const char *status, const char *answer, const char *error) {
    json_t *t = btw_latest_turn(record);
    if (answer) json_set_str(t, "answer", answer);
    json_set_str(t, "status", status);
    json_set_int(t, "updatedAt", now_ms());
    if (error) json_set_str(t, "error", error);
}

json_t *btw_wire_record(const json_t *record) {
    json_t *r = json_clone(record);
    json_del(r, "title");
    return r;
}

/* ---------- side turn ---------- */

static char *render_prompt(const char *question, bool inject) {
    json_t *c = json_obj();
    json_set_str(c, "question", question ? question : "");
    json_set_bool(c, "inject", inject);
    char *q = prompt_render("coding-agent/prompts/system/btw-user.md", c);
    json_free(c);
    return q ? q : xstrdup(question ? question : "");
}

/* ---------- <inject> blocks (overlays/btw-history.ts) ---------- */

#define INJECT_OPEN "<inject>"
#define INJECT_CLOSE "</inject>"

/* the last occurrence of `needle` starting at or before `limit` (NULL: anywhere), NULL when none */
static const char *last_of(const char *s, const char *needle, const char *limit) {
    const char *found = NULL;
    for (const char *p = strstr(s, needle); p && (!limit || p <= limit); p = strstr(p + 1, needle)) found = p;
    return found;
}

bool btw_split_inject(const char *answer, btw_inject_parts_t *out) {
    memset(out, 0, sizeof *out);
    const char *a = answer ? answer : "";
    const char *last_close = last_of(a, INJECT_CLOSE, NULL);
    size_t ol = strlen(INJECT_OPEN), cl = strlen(INJECT_CLOSE);
    /* the last opener that has a close after it, paired with the first close after it: a tag merely mentioned
     * in prose cannot capture the block */
    const char *open = last_close && (size_t)(last_close - a) >= ol ? last_of(a, INJECT_OPEN, last_close - ol) : NULL;
    if (open) {
        const char *close = strstr(open + ol, INJECT_CLOSE);
        out->before = xstrndup(a, (size_t)(open - a));
        out->inject = xstrndup(open + ol, (size_t)(close - open - ol));
        out->after = xstrdup(close + cl);
        out->closed = true;
        return true;
    }
    /* without a closed block the last opener runs to the end; displayed only while the answer still streams */
    const char *streaming = last_of(a, INJECT_OPEN, NULL);
    if (!streaming) return false;
    out->before = xstrndup(a, (size_t)(streaming - a));
    out->inject = xstrdup(streaming + ol);
    out->after = xstrdup("");
    return true;
}

void btw_inject_parts_free(btw_inject_parts_t *p) {
    free(p->before);
    free(p->inject);
    free(p->after);
    memset(p, 0, sizeof *p);
}

char *btw_format_inject_text(const char *inject) {
    char *x = btw_sanitize_display(inject);
    char *t = str_trim(x);
    free(x);
    return t;
}

char *btw_sanitize_display(const char *text) {
    char *plain = tui_strip_ansi(text ? text : "");
    char *u8 = utf8_sanitize(plain, strlen(plain));
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)u8; *p; p++) {
        if (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) { /* C1 controls */
            p++;
            continue;
        }
        if ((*p < 0x20 && *p != '\n' && *p != '\t') || *p == 0x7f) continue;
        sb_putc(&b, (char)*p);
    }
    char *r = tui_replace_tabs(sb_cstr(&b));
    sb_free(&b);
    free(u8);
    free(plain);
    return r;
}

char *btw_inject_of(const char *answer) {
    btw_inject_parts_t p;
    if (!btw_split_inject(answer, &p)) return NULL;
    char *t = p.closed ? btw_format_inject_text(p.inject) : NULL;
    btw_inject_parts_free(&p);
    if (t && !*t) {
        free(t);
        return NULL;
    }
    return t;
}

char *btw_record_inject(const json_t *record) {
    if (!str_eq(btw_latest_status(record), "complete")) return NULL;
    return btw_inject_of(json_get_str(btw_latest_turn(record), "answer"));
}

char *btw_turn_key(const json_t *record, int turn_index) {
    size_t idx = turn_index >= 0 ? (size_t)turn_index : json_len(json_get_arr(record, "followUps"));
    return xasprintf("%s:%zu", json_get_str(record, "id") ? json_get_str(record, "id") : "", idx);
}

int btw_run_turn(app_t *a, const char *question, const json_t *images, const json_t *history, bool inject,
                 const char *conversation_key, const json_t *streaming, void (*on_delta)(void *ud, const char *delta),
                 void *ud, volatile sig_atomic_t *cancel, char **reply, json_t **assistant, char **err) {
    *reply = NULL;
    *assistant = NULL;
    *err = NULL;
    const ai_model_t *model = a->model;
    if (!model) {
        *err = xstrdup("No active model available for /btw.");
        return -1;
    }
    json_t *hist = json_arr(), *turn;
    json_arr_foreach(history, i, turn) {
        json_t *content = json_arr();
        char *p = render_prompt(json_get_str(turn, "question"), inject);
        json_push(content, text_block(p));
        free(p);
        json_t *img;
        json_arr_foreach(json_get_arr(turn, "images"), k, img) json_push(content, json_clone(img));
        json_t *um = json_obj();
        json_set_str(um, "role", "user");
        json_set(um, "content", content);
        json_set_str(um, "attribution", "agent");
        json_set_int(um, "timestamp", json_get_int(turn, "createdAt", 0));
        json_push(hist, um);
        const char *ans = json_get_str(turn, "answer");
        if (!ans || !*ans) continue;
        /* saved history holds visible text only: context, not new billed turns */
        json_t *am = json_obj();
        json_set_str(am, "role", "assistant");
        json_t *ac = json_arr();
        json_push(ac, text_block(ans));
        json_set(am, "content", ac);
        json_set_str(am, "api", model->api ? model->api : "");
        json_set_str(am, "provider", model->provider ? model->provider : "");
        json_set_str(am, "model", model->id ? model->id : "");
        json_t *u = json_parse_str("{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"totalTokens\":0,"
                                   "\"cost\":{\"input\":0,\"output\":0,\"cacheRead\":0,\"cacheWrite\":0,\"total\":0}}");
        json_set(am, "usage", u);
        json_set_str(am, "stopReason", "stop");
        json_set_int(am, "timestamp", json_get_int(turn, "updatedAt", 0));
        json_push(hist, am);
    }
    char *prompt = render_prompt(question, inject);
    app_ephemeral_t e = {.prompt_text = prompt,
                         .prompt_images = images,
                         .history = hist,
                         .conversation_key = conversation_key,
                         .streaming = streaming,
                         .dedupe = true,
                         .reply_max_bytes = 0, /* read in full and saved: no 4 KiB cap */
                         .on_text_delta = on_delta,
                         .ud = ud,
                         .cancel = cancel};
    *assistant = app_ephemeral_turn(a, &e, reply, err);
    free(prompt);
    json_free(hist);
    return *assistant ? 0 : -1;
}

/* ---------- history store ---------- */

typedef struct {
    char *id;
    json_t *record;   /* snapshot (recovered view: running -> interrupted) */
    uint64_t rev;     /* hash of the bytes on disk */
    bool has_rev;
    int lease;        /* flock fd while this store owns the topic, -1 otherwise */
} entry_t;

struct btw_store {
    pthread_mutex_t mu;
    char *dir; /* NULL: memory only */
    VEC(entry_t) entries;
    char *write_error;
};

static bool valid_id(const char *id) {
    if (!id || !isalnum((unsigned char)id[0])) return false;
    size_t n = strlen(id);
    if (n > 128) return false;
    for (size_t i = 1; i < n; i++)
        if (!isalnum((unsigned char)id[i]) && id[i] != '_' && id[i] != '-') return false;
    return true;
}

static bool valid_status(const char *s) {
    return str_eq(s, "running") || str_eq(s, "complete") || str_eq(s, "cancelled") || str_eq(s, "error") ||
           str_eq(s, "interrupted");
}

static const char *const TURN_KEYS[] = {"question", "images", "answer", "status", "createdAt", "updatedAt", "error"};

static bool known_key(const char *k, bool record) {
    for (size_t i = 0; i < ARRAY_LEN(TURN_KEYS); i++)
        if (str_eq(k, TURN_KEYS[i])) return true;
    return record && (str_eq(k, "id") || str_eq(k, "leafId") || str_eq(k, "followUps"));
}

static const char *check_turn(const json_t *t, bool record) {
    if (!json_is_obj(t)) return "must be an object";
    for (size_t i = 0; i < json_len(t); i++)
        if (!known_key(t->u.obj.keys[i], record)) return "has an unknown key";
    if (!json_is_str(json_get(t, "question"))) return "question must be a string";
    if (!json_is_str(json_get(t, "answer"))) return "answer must be a string";
    if (!valid_status(json_get_str(t, "status"))) return "status is invalid";
    for (int k = 0; k < 2; k++) {
        json_t *v = json_get(t, k ? "updatedAt" : "createdAt");
        if (!json_is_num(v) || v->u.num.d < 0 || v->u.num.d > 8.64e15) return "timestamps must be valid";
    }
    json_t *e = json_get(t, "error");
    if (e && !json_is_str(e)) return "error must be a string";
    json_t *imgs = json_get(t, "images"), *img;
    if (imgs && !json_is_arr(imgs)) return "images must be an array";
    json_arr_foreach(imgs, i, img) {
        if (!str_eq(json_get_str(img, "type"), "image") || !json_get_str(img, "data") || !json_get_str(img, "mimeType"))
            return "images are invalid";
    }
    return NULL;
}

/* parseRecord: the record and its follow-ups in omp's schema; NULL + *err on a mismatch */
static json_t *parse_record(const char *bytes, size_t len, char **err) {
    char *perr = NULL;
    json_t *r = json_parse(bytes, len, &perr);
    if (!r) {
        *err = xasprintf("%s", perr ? perr : "invalid JSON");
        free(perr);
        return NULL;
    }
    const char *why = check_turn(r, true);
    json_t *lf = json_get(r, "leafId");
    if (!why && !(json_is_str(json_get(r, "id")) && json_len(json_get(r, "id")) > 0)) why = "id must be a non-empty string";
    if (!why && !(lf && (json_is_str(lf) || json_is_null(lf)))) why = "leafId must be a string or null";
    json_t *f = json_get(r, "followUps"), *t;
    if (!why && f && !json_is_arr(f)) why = "followUps must be an array";
    json_arr_foreach(f, i, t) {
        if (!why) why = check_turn(t, false);
    }
    if (why) {
        *err = xasprintf("Invalid BTW history record: %s", why);
        json_free(r);
        return NULL;
    }
    if (!valid_id(json_get_str(r, "id"))) {
        *err = xstrdup("Invalid BTW history record id");
        json_free(r);
        return NULL;
    }
    return r;
}

static void recover_running(json_t *r) {
    if (str_eq(json_get_str(r, "status"), "running")) json_set_str(r, "status", "interrupted");
    json_t *t;
    json_arr_foreach(json_get_arr(r, "followUps"), i, t) {
        if (str_eq(json_get_str(t, "status"), "running")) json_set_str(t, "status", "interrupted");
    }
}

static entry_t *find_entry(btw_store_t *s, const char *id) {
    for (size_t i = 0; i < s->entries.n; i++)
        if (str_eq(s->entries.v[i].id, id)) return &s->entries.v[i];
    return NULL;
}

static entry_t *add_entry(btw_store_t *s, const char *id) {
    entry_t e = {.id = xstrdup(id), .lease = -1};
    vec_push(&s->entries, e);
    return &s->entries.v[s->entries.n - 1];
}

static char *history_dir(const char *artifacts_dir, const char *scope) {
    char *root = path_join(artifacts_dir, "btw-history");
    if (!scope) return root;
    bool plain = *scope != 0;
    for (const char *p = scope; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') plain = false;
    char *seg = plain ? xstrdup(scope) : hex_encode(scope, strlen(scope)); /* never escape the root */
    char *d = path_join3(root, "sessions", seg);
    free(seg);
    free(root);
    return d;
}

btw_store_t *btw_store_open(const char *artifacts_dir, const char *scope, char **err) {
    *err = NULL;
    btw_store_t *s = xcalloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    if (!artifacts_dir) return s;
    s->dir = history_dir(artifacts_dir, scope);
    dirlist_t dl = {0};
    if (fs_list_dir(s->dir, &dl) != 0) {
        if (errno == ENOENT || !fs_exists(s->dir)) return s;
        *err = xasprintf("Failed to read BTW history %s: %s", s->dir, strerror(errno));
        btw_store_free(s);
        return NULL;
    }
    for (size_t i = 0; i < dl.n; i++) {
        const char *name = dl.v[i].name;
        if (!str_ends(name, ".json")) continue;
        char *path = path_join(s->dir, name);
        struct stat st;
        if (lstat(path, &st) != 0) {
            free(path);
            continue;
        }
        size_t len = 0;
        char *bytes = S_ISREG(st.st_mode) ? fs_read(path, &len) : NULL;
        char *perr = NULL;
        json_t *r = bytes ? parse_record(bytes, len, &perr) : NULL;
        if (!r) {
            *err = xasprintf("Failed to read BTW history %s: %s", path, perr ? perr : "Expected a regular file");
            free(perr);
            free(bytes);
            free(path);
            dirlist_free(&dl);
            btw_store_free(s);
            return NULL;
        }
        char *want = xasprintf("entry-%s.json", json_get_str(r, "id"));
        if (!str_eq(want, name)) {
            *err = xasprintf("Failed to read BTW history %s: Record id does not match its filename", path);
            free(want);
            json_free(r);
            free(bytes);
            free(path);
            dirlist_free(&dl);
            btw_store_free(s);
            return NULL;
        }
        free(want);
        entry_t *e = add_entry(s, json_get_str(r, "id"));
        e->rev = hash_xx64(bytes, len, 0);
        e->has_rev = true;
        free(bytes);
        free(path);
        /* a missing or unreadable title only loses the heading */
        char *tpath = xasprintf("%s/entry-%s.title", s->dir, e->id);
        size_t tl = 0;
        char *title = fs_is_file(tpath) ? fs_read(tpath, &tl) : NULL;
        free(tpath);
        recover_running(r);
        if (title) json_set_str(r, "title", title);
        free(title);
        e->record = r;
    }
    dirlist_free(&dl);
    return s;
}

static void release_lease(entry_t *e) {
    if (e->lease < 0) return;
    flock(e->lease, LOCK_UN);
    close(e->lease);
    e->lease = -1;
}

void btw_store_free(btw_store_t *s) {
    if (!s) return;
    for (size_t i = 0; i < s->entries.n; i++) {
        release_lease(&s->entries.v[i]);
        free(s->entries.v[i].id);
        json_free(s->entries.v[i].record);
    }
    vec_free(&s->entries);
    free(s->dir);
    free(s->write_error);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

const char *btw_store_dir(const btw_store_t *s) { return s->dir; }

static int cmp_records(const void *pa, const void *pb) {
    const json_t *a = *(const json_t *const *)pa, *b = *(const json_t *const *)pb;
    int64_t ca = json_get_int(a, "createdAt", 0), cb = json_get_int(b, "createdAt", 0);
    if (ca != cb) return cb > ca ? 1 : -1;
    return strcmp(json_get_str(a, "id"), json_get_str(b, "id"));
}

json_t *btw_store_records(btw_store_t *s) {
    pthread_mutex_lock(&s->mu);
    size_t n = 0;
    const json_t **v = xcalloc(s->entries.n + 1, sizeof *v);
    for (size_t i = 0; i < s->entries.n; i++)
        if (s->entries.v[i].record) v[n++] = s->entries.v[i].record;
    qsort(v, n, sizeof *v, cmp_records);
    json_t *out = json_arr();
    for (size_t i = 0; i < n; i++) json_push(out, json_clone(v[i]));
    free(v);
    pthread_mutex_unlock(&s->mu);
    return out;
}

json_t *btw_store_find(btw_store_t *s, const char *id) {
    pthread_mutex_lock(&s->mu);
    entry_t *e = find_entry(s, id);
    json_t *r = e && e->record ? json_clone(e->record) : NULL;
    pthread_mutex_unlock(&s->mu);
    return r;
}

bool btw_store_failed(const btw_store_t *s) { return s->write_error != NULL; }

/* acquireFileLock: exclusive OS lock on `<file>.lock`, a few tries */
static int acquire_lease(const char *file, char **err) {
    char *lp = xasprintf("%s.lock", file);
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lp);
    if (fd >= 0) {
        for (int attempt = 0; attempt < 20; attempt++) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
            sleep_ms(50);
        }
        close(fd);
    }
    *err = xasprintf("Failed to acquire lock for %s after 20 attempts", file);
    return -1;
}

static int do_upsert(btw_store_t *s, const json_t *record, bool retry, char **err) {
    *err = NULL;
    if (s->write_error && !retry) {
        *err = xstrdup(s->write_error);
        return -1;
    }
    /* parse what is written exactly as a reader would (title stays in its sidecar) */
    json_t *fields = btw_wire_record(record);
    char *content = json_dump(fields, 0);
    json_free(fields);
    char *perr = NULL;
    json_t *saved = parse_record(content, strlen(content), &perr);
    if (!saved) {
        *err = perr;
        free(content);
        return -1;
    }
    json_free(saved);
    const char *id = json_get_str(record, "id");
    const char *title = json_get_str(record, "title");
    bool running = str_eq(btw_latest_status(record), "running");
    entry_t *e = find_entry(s, id);
    if (!s->dir) {
        if (!e) e = add_entry(s, id);
        json_free(e->record);
        e->record = json_clone(record);
        free(content);
        return 0;
    }
    if (retry) {
        free(s->write_error);
        s->write_error = NULL;
    }
    int rc = -1;
    char *path = xasprintf("%s/entry-%s.json", s->dir, id);
    sb_t body = SB_INIT;
    sb_append(&body, content);
    sb_putc(&body, '\n');
    if (fs_mkdir_p(s->dir, 0700) != 0 || chmod(s->dir, 0700) != 0) {
        *err = xasprintf("%s: %s", s->dir, strerror(errno));
        goto out;
    }
    if (!e) e = add_entry(s, id);
    if (e->lease < 0) {
        e->lease = acquire_lease(path, err);
        if (e->lease < 0) goto out;
    }
    /* compare under the topic lease: a missing or rewritten record is a conflict, not a blind insert */
    size_t len = 0;
    char *disk = fs_read(path, &len);
    bool same = disk ? e->has_rev && hash_xx64(disk, len, 0) == e->rev : !e->has_rev;
    free(disk);
    if (!same) {
        *err = xasprintf("BTW history conflict for %s; reopen history before retrying", id);
        goto out;
    }
    if (!e->has_rev) {
        /* the title never changes: written once, before the record is visible */
        char *tp = xasprintf("%s/entry-%s.title", s->dir, id);
        int trc = title ? fs_write_atomic(tp, title, strlen(title), 0600) : (unlink(tp) == 0 || errno == ENOENT ? 0 : -errno);
        free(tp);
        if (trc != 0) {
            *err = xstrdup(strerror(-trc));
            goto out;
        }
    }
    int wrc = fs_write_atomic(path, body.s, body.len, 0600);
    if (wrc != 0) {
        *err = xstrdup(strerror(-wrc));
        goto out;
    }
    e->rev = hash_xx64(body.s, body.len, 0);
    e->has_rev = true;
    json_free(e->record);
    e->record = json_clone(record);
    if (!running) release_lease(e);
    rc = 0;
out:
    if (rc != 0) {
        /* sticky until an explicit retry; release every topic this store owns */
        if (!s->write_error) s->write_error = xstrdup(*err ? *err : "write failed");
        for (size_t i = 0; i < s->entries.n; i++) release_lease(&s->entries.v[i]);
    }
    sb_free(&body);
    free(path);
    free(content);
    return rc;
}

int btw_store_upsert(btw_store_t *s, const json_t *record, char **err) {
    pthread_mutex_lock(&s->mu);
    int rc = do_upsert(s, record, false, err);
    pthread_mutex_unlock(&s->mu);
    return rc;
}

int btw_store_retry(btw_store_t *s, const json_t *record, char **err) {
    pthread_mutex_lock(&s->mu);
    int rc = do_upsert(s, record, true, err);
    pthread_mutex_unlock(&s->mu);
    return rc;
}
