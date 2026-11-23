#define _GNU_SOURCE
#include <pthread.h>
#include "session.h"
#include "session/blobs.h"
#include "session/trace.h"
#include "tools/outsink.h"

#include "core/fs.h"
#include "prompt/template.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- lock ---------- */

/* The tree (entries, by_id, leaf_id), header and title are written by one thread at a time (the session's prompt
 * worker, or the UI thread while it is idle), but the UI thread also reads them while the worker runs (a transcript
 * rebuild after a compaction, the status line's title, /btw). Writers and the shared read paths hold `mu`
 * (recursive, so a locked API may call another); see session.h. */
static pthread_mutex_t *mu_new(void) {
    pthread_mutex_t *m = xmalloc(sizeof *m);
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &at);
    pthread_mutexattr_destroy(&at);
    return m;
}

void session_lock(const session_t *s) {
    if (s && s->mu) pthread_mutex_lock(s->mu);
}

void session_unlock(const session_t *s) {
    if (s && s->mu) pthread_mutex_unlock(s->mu);
}

/* ---------- paths ---------- */

static char *encode_rel(const char *prefix, const char *rel) {
    char *enc = xstrdup(rel);
    for (char *p = enc; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
    char *r;
    if (!*enc) r = xstrdup(prefix);
    else if (str_ends(prefix, "-")) r = xasprintf("%s%s", prefix, enc);
    else r = xasprintf("%s-%s", prefix, enc);
    free(enc);
    return r;
}

static bool rel_within(const char *root, const char *path, const char **rel) {
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/') n--;
    if (strncmp(path, root, n) != 0) return false;
    if (path[n] == 0) {
        *rel = "";
        return true;
    }
    if (path[n] != '/') return false;
    *rel = path + n + 1;
    return true;
}

char *session_dir_for_cwd(const char *cwd) {
    char *canon = path_real(cwd);
    if (!canon) canon = path_abs(cwd, NULL);
    const char *tmp = env_get("TMPDIR");
    char *tmp_canon = path_real(tmp ? tmp : "/tmp");
    char *home_canon = path_real(home_dir());
    const char *rel;
    char *name;
    if (tmp_canon && rel_within(tmp_canon, canon, &rel)) {
        name = encode_rel("-tmp", rel);
    } else if (home_canon && rel_within(home_canon, canon, &rel)) {
        name = encode_rel("-", rel);
    } else {
        const char *a = canon[0] == '/' ? canon + 1 : canon;
        char *e = xstrdup(a);
        for (char *p = e; *p; p++)
            if (*p == '/' || *p == '\\' || *p == ':') *p = '-';
        name = xasprintf("--%s--", e);
        free(e);
    }
    char *base = omc_agent_path("sessions");
    char *r = path_join(base, name);
    free(base);
    free(name);
    free(canon);
    free(tmp_canon);
    free(home_canon);
    return r;
}

static char *file_safe_ts(const char *iso) {
    char *t = xstrdup(iso);
    for (char *p = t; *p; p++)
        if (*p == ':' || *p == '.') *p = '-';
    return t;
}

/* ---------- core ---------- */

static session_t *session_new_base(const char *cwd) {
    session_t *s = xcalloc(1, sizeof(*s));
    s->mu = mu_new();
    s->entries = json_arr();
    hmap_init(&s->by_id);
    s->header = json_obj();
    json_set_str(s->header, "type", "session");
    json_set_int(s->header, "version", SESSION_VERSION);
    char *id = uuid_v7();
    json_set_str(s->header, "id", id);
    free(id);
    char *ts = iso8601_now();
    json_set_str(s->header, "timestamp", ts);
    free(ts);
    char *c = cwd ? path_abs(cwd, NULL) : get_cwd();
    json_set_str(s->header, "cwd", c);
    free(c);
    return s;
}

session_t *session_in_memory(const char *cwd) { return session_new_base(cwd); }

session_t *session_create(const char *cwd, const char *session_dir) {
    session_t *s = session_new_base(cwd);
    s->persist = true;
    s->dir = session_dir ? xstrdup(session_dir) : session_dir_for_cwd(session_cwd(s));
    char *fts = file_safe_ts(json_get_str(s->header, "timestamp"));
    s->file = xasprintf("%s/%s_%s.jsonl", s->dir, fts, session_id(s));
    free(fts);
    return s;
}

session_t *session_create_file(const char *cwd, const char *file) {
    session_t *s = session_new_base(cwd);
    s->persist = true;
    s->dir = path_dirname(file);
    s->file = xstrdup(file);
    return s;
}

void session_free(session_t *s) {
    if (!s) return;
    json_free(s->header);
    json_free(s->entries);
    hmap_free(&s->by_id, NULL);
    free(s->leaf_id);
    free(s->file);
    free(s->dir);
    free(s->title);
    free(s->title_source);
    free(s->persist_error);
    free(s->last_persist_error);
    if (s->mu) {
        pthread_mutex_destroy(s->mu);
        free(s->mu);
    }
    free(s);
}

const char *session_id(const session_t *s) { return json_get_str(s->header, "id"); }
const char *session_cwd(const session_t *s) { return json_get_str(s->header, "cwd"); }

const char *session_short_id(const char *id) {
    size_t n = id ? strlen(id) : 0;
    return n > 8 ? id + n - 8 : (id ? id : "");
}

json_t *session_get(const session_t *s, const char *id) {
    if (!id) return NULL;
    session_lock(s);
    json_t *e = hmap_get(&s->by_id, id);
    session_unlock(s);
    return e;
}

json_t *session_current_path(const session_t *s) {
    session_lock(s);
    json_t *path = s->leaf_id ? session_path_to(s, s->leaf_id) : json_arr();
    session_unlock(s);
    return path;
}

char *session_leaf_dup(const session_t *s) {
    session_lock(s);
    char *id = s->leaf_id ? xstrdup(s->leaf_id) : NULL;
    session_unlock(s);
    return id;
}

char *session_title_dup(const session_t *s) {
    if (!s) return NULL;
    session_lock(s);
    char *t = s->title ? xstrdup(s->title) : NULL;
    session_unlock(s);
    return t;
}

static char *title_slot_line(const char *title, const char *source) {
    char *ts = iso8601_now();
    char *t = xstrdup(title ? title : "");
    char *line = NULL;
    for (;;) {
        json_t *o = json_obj();
        json_set_str(o, "type", "title");
        json_set_int(o, "v", 1);
        json_set_str(o, "title", t);
        if (source) json_set_str(o, "source", source);
        json_set_str(o, "updatedAt", ts);
        json_set_str(o, "pad", "");
        char *base = json_dump(o, 0);
        json_free(o);
        size_t len = strlen(base) + 1; /* newline */
        if (len <= SESSION_TITLE_SLOT_BYTES) {
            /* re-serialize with padding so total == 256 */
            size_t pad = SESSION_TITLE_SLOT_BYTES - len;
            json_t *o2 = json_obj();
            json_set_str(o2, "type", "title");
            json_set_int(o2, "v", 1);
            json_set_str(o2, "title", t);
            if (source) json_set_str(o2, "source", source);
            json_set_str(o2, "updatedAt", ts);
            char *sp = xmalloc(pad + 1);
            memset(sp, ' ', pad);
            sp[pad] = 0;
            json_set_str(o2, "pad", sp);
            free(sp);
            char *full = json_dump(o2, 0);
            json_free(o2);
            line = xasprintf("%s\n", full);
            free(full);
            free(base);
            break;
        }
        free(base);
        /* truncate title by one code point */
        size_t n = strlen(t);
        if (!n) break;
        do n--;
        while (n && ((unsigned char)t[n] & 0xC0) == 0x80);
        t[n] = 0;
    }
    free(t);
    free(ts);
    return line;
}

static int write_full_locked(session_t *s);

/* #noteDiskFailure: log every failure, report the first one of an episode; the next persist rewrites everything */
static void note_disk_failure(session_t *s, int err, const char *syscall) {
    bool path_err = err == EACCES || err == EPERM || err == EROFS || err == ENOENT || err == ENOTDIR || err == EISDIR;
    char *msg = fs_node_error(err, syscall, path_err ? s->file : NULL);
    LOGE("session persistence error: %s: %s", s->file, msg);
    s->rewrite_required = true;
    free(s->last_persist_error);
    s->last_persist_error = xstrdup(msg);
    if (s->disk_failed) {
        free(msg);
        return;
    }
    s->disk_failed = true;
    free(s->persist_error);
    s->persist_error = msg;
}

char *session_persist_retry(session_t *s) {
    if (!s || !s->persist || !s->disk_failed) return NULL;
    session_lock(s);
    char *e = NULL;
    if (s->rewrite_required && write_full_locked(s) != 0) e = xstrdup(s->last_persist_error ? s->last_persist_error : "write failed");
    session_unlock(s);
    return e;
}

char *session_take_persist_error(session_t *s) {
    if (!s) return NULL;
    session_lock(s);
    char *e = s->persist_error;
    s->persist_error = NULL;
    session_unlock(s);
    return e;
}

static int write_full(session_t *s) {
    session_lock(s);
    int rc = write_full_locked(s);
    session_unlock(s);
    return rc;
}

static int write_full_locked(session_t *s) {
    if (!s->persist || !s->file) return 0;
    sb_t b = SB_INIT;
    char *slot = title_slot_line(s->title, s->title_source);
    if (slot) sb_append(&b, slot);
    free(slot);
    json_dump_sb(&b, s->header, 0);
    sb_putc(&b, '\n');
    json_t *e;
    json_arr_foreach(s->entries, i, e) {
        json_t *pe = session_blobs_externalize(e); /* images live in the blob store, as in omp */
        json_dump_sb(&b, pe, 0);
        json_free(pe);
        sb_putc(&b, '\n');
    }
    fs_mkdir_p(s->dir, 0700);
    int rc = fs_write_atomic(s->file, b.s, b.len, 0600);
    sb_free(&b);
    if (rc == 0) {
        s->on_disk = true;
        s->rewrite_required = false;
        s->disk_failed = false; /* #clearDiskError: a later failure is reported again */
    } else {
        note_disk_failure(s, -rc, "open");
    }
    return rc;
}

int session_rewrite(session_t *s) {
    if (!s->persist || !s->on_disk) return 0;
    return write_full(s);
}

int session_ensure_on_disk(session_t *s) {
    if (!s->persist || s->on_disk) return 0;
    return write_full(s);
}

static void persist_entry(session_t *s, const json_t *e) {
    if (!s->persist) return;
    if (!s->on_disk) {
        /* lazy gate: materialize once an assistant message exists */
        bool has_assistant = false;
        json_t *x;
        json_arr_foreach(s->entries, i, x) {
            if (str_eq(json_get_str(x, "type"), "message") &&
                str_eq(json_get_str(json_get(x, "message"), "role"), "assistant"))
                has_assistant = true;
        }
        if (has_assistant) write_full(s);
        return;
    }
    if (s->rewrite_required) { /* a write failed earlier: the full rewrite restores every entry still in memory */
        write_full(s);
        return;
    }
    json_t *pe = session_blobs_externalize(e);
    char *line = json_dump(pe, 0);
    json_free(pe);
    sb_t b = SB_INIT;
    sb_append(&b, line);
    sb_putc(&b, '\n');
    free(line);
    int rc = fs_append(s->file, b.s, b.len);
    if (rc != 0) note_disk_failure(s, -rc, rc == -ENOSPC || rc == -EDQUOT || rc == -EIO || rc == -EFBIG ? "write" : "open");
    sb_free(&b);
}

static char *new_entry_id(session_t *s) {
    for (int i = 0; i < 1000; i++) {
        char *id = rand_hex(4);
        if (!hmap_has(&s->by_id, id)) return id;
        free(id);
    }
    return uuid_v7();
}

const char *session_append_entry(session_t *s, json_t *entry) {
    session_lock(s);
    char *id = new_entry_id(s);
    json_t *e = json_obj();
    json_set_str(e, "type", json_get_str(entry, "type"));
    json_set_str(e, "id", id);
    json_set(e, "parentId", s->leaf_id ? json_str(s->leaf_id) : json_null());
    char *ts = iso8601_now();
    json_set_str(e, "timestamp", ts);
    free(ts);
    const char *k;
    json_t *v;
    json_obj_foreach(entry, i, k, v) {
        if (strcmp(k, "type") && strcmp(k, "id") && strcmp(k, "parentId") && strcmp(k, "timestamp"))
            json_set(e, k, json_clone(v));
    }
    json_free(entry);
    json_push(s->entries, e);
    hmap_put(&s->by_id, id, e);
    free(s->leaf_id);
    s->leaf_id = id;
    persist_entry(s, e);
    session_unlock(s);
    return json_get_str(e, "id");
}

static json_t *typed(const char *type) {
    json_t *o = json_obj();
    json_set_str(o, "type", type);
    return o;
}

const char *session_append_message(session_t *s, const json_t *message) {
    json_t *e = typed("message");
    json_set(e, "message", json_clone(message));
    return session_append_entry(s, e);
}

const char *session_append_model_change_ex(session_t *s, const char *pm, const char *role, bool is_fallback) {
    json_t *e = typed("model_change");
    json_set_str(e, "model", pm);
    if (role) json_set_str(e, "role", role);
    json_set_bool(e, "resolvedModelIsFallback", is_fallback);
    return session_append_entry(s, e);
}

const char *session_append_model_change(session_t *s, const char *pm, const char *role) {
    return session_append_model_change_ex(s, pm, role, false);
}

const char *session_append_thinking_change(session_t *s, const char *level) {
    json_t *e = typed("thinking_level_change");
    json_set_str(e, "thinkingLevel", level);
    return session_append_entry(s, e);
}

const char *session_append_thinking_change_ex(session_t *s, const char *level, const char *configured) {
    json_t *e = typed("thinking_level_change");
    if (level) json_set_str(e, "thinkingLevel", level);
    else json_set(e, "thinkingLevel", json_null());
    if (configured) json_set_str(e, "configured", configured);
    else json_set(e, "configured", json_null());
    return session_append_entry(s, e);
}

const char *session_append_compaction(session_t *s, const char *summary, const char *first_kept_id, int64_t before,
                                      json_t *details) {
    json_t *e = typed("compaction");
    json_set_str(e, "summary", summary);
    if (first_kept_id) json_set_str(e, "firstKeptEntryId", first_kept_id);
    json_set_int(e, "tokensBefore", before);
    if (details) json_set(e, "details", details);
    return session_append_entry(s, e);
}

const char *session_append_compaction_ex(session_t *s, const session_compaction_t *c) {
    json_t *e = typed("compaction");
    json_set_str(e, "summary", c->summary ? c->summary : "");
    if (c->short_summary) json_set_str(e, "shortSummary", c->short_summary);
    json_set_str(e, "firstKeptEntryId", c->first_kept_id ? c->first_kept_id : "");
    json_set_int(e, "tokensBefore", c->tokens_before);
    if (c->tokens_after >= 0) json_set_int(e, "tokensAfter", c->tokens_after);
    if (c->method) json_set_str(e, "method", c->method);
    if (c->replay_through_id) json_set_str(e, "providerReplayThroughEntryId", c->replay_through_id);
    if (c->details) json_set(e, "details", json_clone(c->details));
    json_set_bool(e, "fromExtension", c->from_extension);
    if (c->preserve_data) json_set(e, "preserveData", json_clone(c->preserve_data));
    return session_append_entry(s, e);
}

const char *session_append_branch_summary(session_t *s, const char *from_id, const char *summary) {
    json_t *e = typed("branch_summary");
    json_set_str(e, "fromId", from_id ? from_id : "root");
    json_set_str(e, "summary", summary);
    return session_append_entry(s, e);
}

const char *session_append_custom(session_t *s, const char *custom_type, json_t *data) {
    json_t *e = typed("custom");
    json_set_str(e, "customType", custom_type);
    json_set(e, "data", data ? data : json_null());
    return session_append_entry(s, e);
}

const char *session_append_custom_message(session_t *s, const char *custom_type, const char *content, bool display) {
    json_t *e = typed("custom_message");
    json_set_str(e, "customType", custom_type);
    json_set_str(e, "content", content);
    json_set_bool(e, "display", display);
    return session_append_entry(s, e);
}

const char *session_append_label(session_t *s, const char *target, const char *label) {
    json_t *e = typed("label");
    json_set_str(e, "targetId", target);
    if (label) json_set_str(e, "label", label);
    return session_append_entry(s, e);
}

const char *session_append_reset_boundary(session_t *s) { return session_append_entry(s, typed("reset_boundary")); }

static void set_title_locked(session_t *s, const char *title, const char *source);

void session_set_title(session_t *s, const char *title, const char *source) {
    session_lock(s);
    set_title_locked(s, title, source);
    session_unlock(s);
}

static void set_title_locked(session_t *s, const char *title, const char *source) {
    if (str_eq(s->title_source, "user") && str_eq(source, "auto")) return;
    free(s->title);
    free(s->title_source);
    s->title = xstrdup(title);
    s->title_source = xstrdup(source ? source : "auto");
    json_set_str(s->header, "title", title);
    json_set_str(s->header, "titleSource", s->title_source);
    if (s->on_disk) {
        /* rewrite the fixed-width slot in place if the file starts with one, else full rewrite */
        int fd = open(s->file, O_RDWR | O_CLOEXEC);
        bool done = false;
        if (fd >= 0) {
            char head[SESSION_TITLE_SLOT_BYTES];
            if (read(fd, head, sizeof head) == (ssize_t)sizeof head && head[SESSION_TITLE_SLOT_BYTES - 1] == '\n' &&
                strncmp(head, "{\"type\":\"title\"", 15) == 0) {
                char *slot = title_slot_line(title, s->title_source);
                if (slot && strlen(slot) == SESSION_TITLE_SLOT_BYTES && pwrite(fd, slot, SESSION_TITLE_SLOT_BYTES, 0) == SESSION_TITLE_SLOT_BYTES)
                    done = true;
                free(slot);
            }
            close(fd);
        }
        json_t *e = typed("title_change");
        json_set_str(e, "title", title);
        json_set_str(e, "source", s->title_source);
        session_append_entry(s, e);
        if (!done) write_full(s);
    }
}

/* ---------- load ---------- */

static void index_entries(session_t *s) {
    json_t *e;
    json_arr_foreach(s->entries, i, e) {
        const char *id = json_get_str(e, "id");
        if (id) hmap_put(&s->by_id, id, e);
    }
}

session_t *session_open(const char *path, char **err) {
    size_t len;
    char *data = fs_read(path, &len);
    if (!data) {
        if (err) *err = xasprintf("cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    session_t *s = xcalloc(1, sizeof(*s));
    s->mu = mu_new();
    s->entries = json_arr();
    hmap_init(&s->by_id);
    s->persist = true;
    s->on_disk = true;
    s->file = path_abs(path, NULL);
    s->dir = path_dirname(s->file);
    strvec_t lines = str_split_lines(data, len);
    free(data);
    for (size_t i = 0; i < lines.n; i++) {
        if (!*lines.v[i]) continue;
        json_t *j = json_parse_str(lines.v[i]);
        if (!json_is_obj(j)) {
            json_free(j);
            continue;
        }
        const char *type = json_get_str(j, "type");
        if (str_eq(type, "title") && !s->header) {
            free(s->title);
            s->title = xstrdup(json_get_str(j, "title"));
            if (json_get_str(j, "source")) {
                free(s->title_source);
                s->title_source = xstrdup(json_get_str(j, "source"));
            }
            json_free(j);
        } else if (str_eq(type, "session") && !s->header) {
            s->header = j;
        } else if (s->header) {
            session_blobs_resolve(j);
            if (str_eq(type, "title_change")) {
                free(s->title);
                s->title = xstrdup(json_get_str(j, "title"));
            }
            json_push(s->entries, j);
        } else {
            json_free(j);
        }
    }
    strvec_free(&lines);
    if (!s->header) {
        if (err) *err = xasprintf("%s is not a session file", path);
        session_free(s);
        return NULL;
    }
    if (!s->title && json_get_str(s->header, "title")) s->title = xstrdup(json_get_str(s->header, "title"));
    index_entries(s);
    size_t n = json_len(s->entries);
    if (n) s->leaf_id = xstrdup(json_get_str(json_at(s->entries, n - 1), "id"));
    return s;
}

/* ---------- tree ---------- */

void session_branch(session_t *s, const char *entry_id) {
    session_lock(s);
    if (session_get(s, entry_id)) {
        char *id = xstrdup(entry_id); /* entry_id may be the current leaf_id */
        free(s->leaf_id);
        s->leaf_id = id;
    }
    session_unlock(s);
}

void session_reset_leaf(session_t *s) {
    session_lock(s);
    free(s->leaf_id);
    s->leaf_id = NULL;
    session_unlock(s);
}

json_t *session_path_to(const session_t *s, const char *leaf) {
    session_lock(s);
    json_t *rev = json_arr();
    hmap_t seen;
    hmap_init(&seen);
    const char *cur = leaf;
    while (cur) {
        if (hmap_has(&seen, cur)) break;
        hmap_put(&seen, cur, (void *)1);
        json_t *e = session_get(s, cur);
        if (!e) break;
        json_push(rev, json_clone(e));
        cur = json_get_str(e, "parentId");
    }
    hmap_free(&seen, NULL);
    session_unlock(s);
    json_t *out = json_arr();
    for (size_t i = json_len(rev); i > 0; i--) json_push(out, json_clone(json_at(rev, i - 1)));
    json_free(rev);
    return out;
}

json_t *session_children(const session_t *s, const char *id) {
    json_t *out = json_arr();
    json_t *e;
    session_lock(s);
    json_arr_foreach(s->entries, i, e) {
        const char *p = json_get_str(e, "parentId");
        if ((id && str_eq(p, id)) || (!id && !p)) json_push(out, json_clone(e));
    }
    session_unlock(s);
    return out;
}

/* ---------- context ---------- */

static json_t *user_text_msg(const char *text, int64_t ts) {
    json_t *m = json_obj();
    json_set_str(m, "role", "user");
    json_t *c = json_arr();
    json_t *t = json_obj();
    json_set_str(t, "type", "text");
    json_set_str(t, "text", text);
    json_push(c, t);
    json_set(m, "content", c);
    json_set_int(m, "timestamp", ts);
    return m;
}

static json_t *summary_msg(const char *tpl_path, const char *summary) {
    json_t *ctx = json_obj();
    json_set_str(ctx, "summary", summary ? summary : "");
    char *txt = prompt_render(tpl_path, ctx);
    json_free(ctx);
    json_t *m = user_text_msg(txt, now_ms());
    free(txt);
    json_set_bool(m, "historyRewrite", true); /* context usage anchors only on turns after this summary */
    json_set_bool(m, "sessionSummary", true); /* omp's compactionSummary / branchSummary roles: not a prompt to re-run */
    return m;
}

/* convert a persisted message to an LLM message (custom roles) or NULL to skip */
static json_t *to_llm(const json_t *msg);
json_t *session_message_to_llm(const json_t *msg) { return to_llm(msg); }

bool session_message_is_user(const json_t *m) {
    return str_eq(json_get_str(m, "role"), "user") && !json_get_str(m, "sourceRole") &&
           !json_get_bool(m, "sessionSummary", false);
}

static json_t *to_llm(const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    if (!role) return NULL;
    if (!strcmp(role, "user") || !strcmp(role, "assistant") || !strcmp(role, "toolResult") || !strcmp(role, "developer"))
        return json_clone(msg);
    if (!strcmp(role, "bashExecution") || !strcmp(role, "pythonExecution")) {
        if (json_get_bool(msg, "excludeFromContext", false)) return NULL;
        /* omp bashExecutionToText / pythonExecutionToText (incl. the meta notice) */
        char *t = !strcmp(role, "bashExecution") ? out_bash_execution_text(msg) : out_python_execution_text(msg);
        json_t *m = user_text_msg(t, json_get_int(msg, "timestamp", now_ms()));
        free(t);
        json_set_str(m, "sourceRole", role);
        return m;
    }
    if (!strcmp(role, "custom") || !strcmp(role, "hookMessage") || !strcmp(role, "fileMention")) {
        json_t *m = json_obj();
        json_set_str(m, "role", "user");
        json_t *c = json_get(msg, "content");
        json_set(m, "content", c ? json_clone(c) : json_str(""));
        json_set_int(m, "timestamp", json_get_int(msg, "timestamp", now_ms()));
        json_set_str(m, "sourceRole", role);
        return m;
    }
    return NULL;
}

static void emit_entry(json_t *out, const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "message")) {
        json_t *m = to_llm(json_get(e, "message"));
        if (m) json_push(out, m);
    } else if (str_eq(t, "custom_message")) {
        json_t *m = json_obj();
        /* attributed custom messages (image-attachment notices) replay as omp's developer turns */
        const char *attr = json_get_str(e, "attribution");
        /* isUserInvokedSkillPrompt: a /skill: invocation replays as the user's own turn */
        bool skill = str_eq(json_get_str(e, "customType"), "skill-prompt") && str_eq(attr, "user");
        json_set_str(m, "role", attr && !skill ? "developer" : "user");
        json_set_str(m, "sourceRole", "custom");
        if (attr) json_set_str(m, "attribution", attr);
        if (skill) {
            json_set_bool(m, "display", true);
            if (json_get(e, "details")) json_set(m, "details", json_clone(json_get(e, "details")));
        } else if ((str_eq(json_get_str(e, "customType"), "tool-command-mount-notice") ||
                    str_eq(json_get_str(e, "customType"), "skill-notice")) &&
                   json_get(e, "details")) {
            /* announced-mount / announced-skill replay (xdnotice.c, autolearn.c) */
            json_set(m, "details", json_clone(json_get(e, "details")));
        }
        json_t *c = json_get(e, "content");
        if (attr && json_is_str(c)) {
            json_t *arr = json_arr(), *tb = json_obj();
            json_set_str(tb, "type", "text");
            json_set_str(tb, "text", json_sv(c));
            json_push(arr, tb);
            json_set(m, "content", arr);
        } else json_set(m, "content", c ? json_clone(c) : json_str(""));
        json_set_int(m, "timestamp", now_ms());
        if (!json_get_bool(e, "display", true)) json_set_bool(m, "synthetic", true);
        if (json_get_str(e, "customType")) json_set_str(m, "customType", json_get_str(e, "customType"));
        json_push(out, m);
    } else if (str_eq(t, "branch_summary")) {
        json_t *bm = summary_msg("agent/compaction/prompts/branch-summary-context.md", json_get_str(e, "summary"));
        json_del(bm, "historyRewrite"); /* only a compaction floors the context gauge (getContextBreakdown) */
        json_push(out, bm);
    }
}

/* the replay payload a compaction entry carries for its provider (session-context.ts
 * getOpenAiRemoteCompactionPayload / compaction/anthropic.ts getAnthropicCompactionPayload); owned or NULL */
json_t *session_compaction_payload(const json_t *compaction, bool *openai_history) {
    *openai_history = false;
    const json_t *pd = json_get_obj(compaction, "preserveData");
    const json_t *oa = json_get_obj(pd, "openaiRemoteCompaction");
    if (oa && json_get_str(oa, "provider") && *json_get_str(oa, "provider") && json_is_arr(json_get(oa, "replacementHistory"))) {
        json_t *p = json_obj();
        json_set_str(p, "type", "openaiResponsesHistory");
        json_set_str(p, "provider", json_get_str(oa, "provider"));
        json_set(p, "items", json_clone(json_get(oa, "replacementHistory")));
        *openai_history = true;
        return p;
    }
    const json_t *an = json_get_obj(pd, "anthropicCompaction");
    if (an && json_get_str(an, "provider") && *json_get_str(an, "provider") && json_get_str(an, "content") &&
        *json_get_str(an, "content")) {
        json_t *p = json_clone(an);
        json_del(p, "model");
        json_del(p, "usedTokens");
        json_t *o = json_obj();
        json_set_str(o, "type", "anthropicCompaction");
        const char *k;
        json_t *v;
        json_obj_foreach(p, i, k, v) json_set(o, k, json_clone(v));
        json_free(p);
        return o;
    }
    return NULL;
}

typedef struct {
    const char *provider, *api; /* NULL provider: any reader */
} reader_t;

static void build_context_path(const json_t *path, session_context_t *out, const reader_t *rd);

/* isOpenAiRemoteCompactionApi */
static bool responses_api(const char *api) {
    return str_eq(api, "openai-responses") || str_eq(api, "azure-openai-responses") || str_eq(api, "openai-codex-responses");
}

bool session_compaction_readable(const json_t *c, const char *provider, const char *api) {
    if (!provider) return true;
    const json_t *oa = json_get_obj(json_get_obj(c, "preserveData"), "openaiRemoteCompaction");
    if (!oa || !json_is_arr(json_get(oa, "replacementHistory"))) return true;
    return str_eq(json_get_str(oa, "provider"), provider) && responses_api(api);
}

void session_build_context(const session_t *s, session_context_t *out) {
    session_build_context_as(s, NULL, NULL, out);
}

void session_build_context_as(const session_t *s, const char *provider, const char *api, session_context_t *out) {
    memset(out, 0, sizeof(*out));
    out->messages = json_arr();
    json_t *path = session_current_path(s);
    reader_t rd = {provider, api};
    build_context_path(path, out, &rd);
    json_free(path);
}

const char *session_readable_compaction(const session_t *s, const char *provider, const char *api) {
    json_t *path = session_current_path(s);
    const char *id = NULL;
    for (size_t i = json_len(path); i-- > 0 && !id;) {
        const json_t *e = json_at(path, i);
        if (str_eq(json_get_str(e, "type"), "compaction") && session_compaction_readable(e, provider, api)) {
            const json_t *orig = session_get(s, json_get_str(e, "id"));
            id = orig ? json_get_str(orig, "id") : NULL;
        }
    }
    json_free(path);
    return id;
}

void session_build_context_for(const json_t *path, session_context_t *out) {
    memset(out, 0, sizeof(*out));
    out->messages = json_arr();
    reader_t rd = {NULL, NULL};
    build_context_path(path, out, &rd);
}

static void build_context_path(const json_t *path, session_context_t *out, const reader_t *rd) {
    size_t n = json_len(path);
    ssize_t boundary = -1, compaction = -1;
    /* omp ca42dc150a: a retry-fallback hop is ephemeral, so the thinking level it applied (a chain entry's `:level`)
     * does not outlive it: the level in effect before the first hop wins unless a later change superseded the
     * hop's own entry */
    bool have_pre = false, from_hop = false, has_thinking = false;
    char *pre_thinking = NULL, *pre_configured = NULL;
    const json_t *prev = NULL;
    for (size_t i = 0; i < n; i++) {
        json_t *e = json_at(path, i);
        const char *t = json_get_str(e, "type");
        if (str_eq(t, "thinking_level_change")) {
            has_thinking = true;
            from_hop = prev && str_eq(json_get_str(prev, "type"), "model_change") && json_get_bool(prev, "resolvedModelIsFallback", false);
        } else if (str_eq(t, "model_change")) {
            if (!json_get_bool(e, "resolvedModelIsFallback", false)) {
                have_pre = false;
                free(pre_thinking);
                free(pre_configured);
                pre_thinking = pre_configured = NULL;
            } else if (!have_pre && has_thinking) {
                have_pre = true;
                pre_thinking = out->thinking ? xstrdup(out->thinking) : NULL;
                pre_configured = out->thinking_configured ? xstrdup(out->thinking_configured) : NULL;
            }
        }
        prev = e;
        if (str_eq(t, "model_change")) {
            const char *role = json_get_str(e, "role");
            if (!role || !strcmp(role, "default")) {
                free(out->model);
                out->model = xstrdup(json_get_str(e, "model"));
            }
        } else if (str_eq(t, "thinking_level_change")) {
            free(out->thinking);
            const char *lv = json_get_str(e, "thinkingLevel");
            out->thinking = xstrdup(lv ? lv : "off");
            free(out->thinking_configured);
            const char *cf = json_get_str(e, "configured");
            out->thinking_configured = cf ? xstrdup(cf) : NULL;
        } else if (str_eq(t, "service_tier_change")) {
            out->has_service_tier = true;
            json_free(out->service_tier);
            const json_t *st = json_get(e, "serviceTier");
            out->service_tier = st ? json_clone(st) : NULL;
        } else if (str_eq(t, "reset_boundary")) {
            boundary = (ssize_t)i;
            compaction = -1;
        } else if (str_eq(t, "compaction")) {
            if (session_compaction_readable(e, rd->provider, rd->api)) compaction = (ssize_t)i;
        }
    }
    if (have_pre && from_hop) {
        free(out->thinking);
        free(out->thinking_configured);
        out->thinking = pre_thinking;
        out->thinking_configured = pre_configured;
    } else {
        free(pre_thinking);
        free(pre_configured);
    }
    size_t start = (size_t)(boundary + 1);
    if (compaction >= 0) {
        json_t *c = json_at(path, (size_t)compaction);
        out->compaction_tokens_before = json_get_int(c, "tokensBefore", 0);
        /* convertToLlm compactionSummary: a handoff document renders through handoff-summary-context */
        json_t *sm = summary_msg(str_eq(json_get_str(c, "method"), "handoff") ? "agent/compaction/prompts/handoff-summary-context.md"
                                                                               : "agent/compaction/prompts/compaction-summary-context.md",
                                 json_get_str(c, "summary"));
        double ts;
        if (json_get_str(c, "timestamp") && trace_parse_iso_ms(json_get_str(c, "timestamp"), &ts))
            json_set_int(sm, "timestamp", (int64_t)ts); /* the summary carries the compaction's commit time */
        bool openai_history = false;
        json_t *payload = session_compaction_payload(c, &openai_history);
        if (payload) json_set(sm, "providerPayload", payload);
        json_push(out->messages, sm);
        const char *first_kept = json_get_str(c, "firstKeptEntryId");
        const char *replay_through = json_get_str(c, "providerReplayThroughEntryId");
        ssize_t kept = -1;
        for (size_t i = start; first_kept && i < (size_t)compaction; i++)
            if (str_eq(json_get_str(json_at(path, i), "id"), first_kept)) kept = (ssize_t)i;
        if (openai_history) {
            /* the replacement history carries the kept turns; only entries after a partial (snapshot) replay
             * point replay normally */
            ssize_t through = -1;
            for (size_t i = start; replay_through && i < (size_t)compaction; i++)
                if (str_eq(json_get_str(json_at(path, i), "id"), replay_through)) through = (ssize_t)i;
            if (through >= 0)
                for (size_t i = (size_t)through + 1; i < (size_t)compaction; i++) emit_entry(out->messages, json_at(path, i));
        } else if (kept >= 0) {
            for (size_t i = (size_t)kept; i < (size_t)compaction; i++) emit_entry(out->messages, json_at(path, i));
        } else if (first_kept && !*first_kept && replay_through) {
            /* an empty kept tail: turns appended after the summarized snapshot replay */
            ssize_t snap = -1;
            for (size_t i = start; i < (size_t)compaction; i++)
                if (str_eq(json_get_str(json_at(path, i), "id"), replay_through)) snap = (ssize_t)i;
            if (snap >= 0)
                for (size_t i = (size_t)snap + 1; i < (size_t)compaction; i++) emit_entry(out->messages, json_at(path, i));
        }
        start = (size_t)compaction + 1;
    }
    for (size_t i = start; i < n; i++) emit_entry(out->messages, json_at(path, i));
}

void session_context_free(session_context_t *c) {
    json_free(c->messages);
    free(c->model);
    free(c->thinking);
    free(c->thinking_configured);
    json_free(c->service_tier);
    memset(c, 0, sizeof(*c));
}

session_t *session_fork(const session_t *s, const char *leaf_id, const char *session_dir) {
    session_lock(s);
    session_t *n = session_create(session_cwd(s), session_dir ? session_dir : s->dir);
    if (s->title) { /* the fork keeps the session name (createBranchedSession / fork) */
        n->title = xstrdup(s->title);
        n->title_source = xstrdup(s->title_source ? s->title_source : "auto");
        json_set_str(n->header, "title", n->title);
        json_set_str(n->header, "titleSource", n->title_source);
    }
    json_set_str(n->header, "parentSession", session_id(s));
    /* the fork keeps the source's extra workspace roots (minus its own cwd) */
    strvec_t roots = session_additional_dirs(s);
    session_set_additional_dirs(n, &roots);
    strvec_free(&roots);
    json_t *path = session_path_to(s, leaf_id ? leaf_id : s->leaf_id);
    session_unlock(s);
    json_t *e;
    json_arr_foreach(path, i, e) {
        json_t *c = json_clone(e);
        const char *id = json_get_str(c, "id");
        json_push(n->entries, c);
        hmap_put(&n->by_id, id, c);
        free(n->leaf_id);
        n->leaf_id = xstrdup(id);
    }
    json_free(path);
    if (json_len(n->entries)) write_full(n);
    return n;
}

/* ---------- multi-root workspace (session-workspace.ts + SessionManager workspace directories) ---------- */

char *session_normalize_workspace_dir(const char *dir, const char *base) { return path_abs(dir, base); }

strvec_t session_additional_dirs(const session_t *s) {
    strvec_t out = {0};
    json_t *x;
    session_lock(s);
    json_arr_foreach(json_get_arr(s->header, "additionalDirectories"), i, x) if (json_is_str(x)) strvec_push(&out, json_sv(x));
    session_unlock(s);
    return out;
}

/* header.additionalDirectories = dirs (key dropped when empty); rewrite a session already on disk */
static void store_dirs(session_t *s, const strvec_t *dirs) {
    session_lock(s);
    if (!dirs->n) json_del(s->header, "additionalDirectories");
    else {
        json_t *a = json_arr();
        for (size_t i = 0; i < dirs->n; i++) json_push_str(a, dirs->v[i]);
        json_set(s->header, "additionalDirectories", a);
    }
    session_rewrite(s);
    session_unlock(s);
}

int session_add_workspace_dir(session_t *s, const char *dir, char **resolved, char **err) {
    *resolved = NULL;
    *err = NULL;
    char *r = session_normalize_workspace_dir(dir, session_cwd(s));
    char *cwd = path_abs(session_cwd(s), NULL);
    bool is_cwd = !strcmp(r, cwd);
    free(cwd);
    if (is_cwd) {
        free(r);
        *err = xstrdup("The current working directory is already the primary workspace root.");
        return -1;
    }
    strvec_t cur = session_additional_dirs(s);
    if (strvec_contains(&cur, r)) {
        strvec_free(&cur);
        *resolved = r;
        return 0;
    }
    strvec_push(&cur, r);
    store_dirs(s, &cur);
    strvec_free(&cur);
    *resolved = r;
    return 1;
}

int session_remove_workspace_dir(session_t *s, const char *dir, char **resolved) {
    char *r = session_normalize_workspace_dir(dir, session_cwd(s));
    *resolved = r;
    strvec_t cur = session_additional_dirs(s), next = {0};
    bool hit = false;
    for (size_t i = 0; i < cur.n; i++) {
        char *n = path_abs(cur.v[i], NULL);
        if (!hit && !strcmp(n, r)) hit = true;
        else strvec_push(&next, cur.v[i]);
        free(n);
    }
    if (hit) store_dirs(s, &next);
    strvec_free(&cur);
    strvec_free(&next);
    return hit ? 1 : 0;
}

void session_set_additional_dirs(session_t *s, const strvec_t *dirs) {
    const char *cwd = session_cwd(s);
    strvec_t next = {0};
    for (size_t i = 0; i < dirs->n; i++) {
        char *r = session_normalize_workspace_dir(dirs->v[i], cwd);
        if (strcmp(r, cwd) != 0 && !strvec_contains(&next, r)) strvec_push(&next, r);
        free(r);
    }
    strvec_t cur = session_additional_dirs(s);
    bool same = cur.n == next.n;
    for (size_t i = 0; same && i < cur.n; i++) same = !strcmp(cur.v[i], next.v[i]);
    if (!same) store_dirs(s, &next);
    strvec_free(&cur);
    strvec_free(&next);
}

/* ---------- move (SessionManager.moveTo) ---------- */

static int move_path(const char *from, const char *to) {
    if (rename(from, to) == 0) return 0;
    if (errno != EXDEV) return -errno;
    size_t n = 0;
    char *data = fs_read(from, &n);
    if (!data) return -errno;
    int rc = fs_write_atomic(to, data, n, 0600);
    free(data);
    if (rc == 0) unlink(from);
    return rc;
}

static bool has_assistant(const session_t *s) {
    json_t *x;
    json_arr_foreach(s->entries, i, x) {
        if (str_eq(json_get_str(x, "type"), "message") && str_eq(json_get_str(json_get(x, "message"), "role"), "assistant"))
            return true;
    }
    return false;
}

static int move_locked(session_t *s, const char *new_cwd, char **err);

int session_move(session_t *s, const char *new_cwd, char **err) {
    session_lock(s);
    int rc = move_locked(s, new_cwd, err);
    session_unlock(s);
    return rc;
}

static int move_locked(session_t *s, const char *new_cwd, char **err) {
    *err = NULL;
    char *cwd = path_abs(new_cwd, NULL);
    char *old_cwd = path_abs(session_cwd(s), NULL);
    char *next_dir = s->persist ? session_dir_for_cwd(cwd) : NULL;
    char *new_file = s->file && next_dir ? path_join(next_dir, path_basename(s->file)) : NULL;
    int rc = 0;
    bool existed = s->file && fs_exists(s->file);
    if (!strcmp(cwd, old_cwd) && (!new_file || !strcmp(new_file, s->file))) goto out;
    if (new_file && strcmp(new_file, s->file) != 0) {
        fs_mkdir_p(next_dir, 0700);
        if (existed) {
            if (fs_exists(new_file)) {
                *err = xasprintf("Cannot move session to \"%s\": the destination already exists. Nothing was moved.", new_file);
                rc = -1;
                goto out;
            }
            int mr = move_path(s->file, new_file);
            if (mr != 0) {
                *err = xstrdup(strerror(-mr));
                rc = -1;
                goto out;
            }
        }
        /* artifacts (<file minus .jsonl>/) and the local:// root (<dir>/<id>/) follow the file */
        const char *sid = session_id(s);
        char *oa = str_ends(s->file, ".jsonl") ? xstrndup(s->file, strlen(s->file) - 6) : NULL;
        char *na = str_ends(new_file, ".jsonl") ? xstrndup(new_file, strlen(new_file) - 6) : NULL;
        char *ol = path_join(s->dir, sid), *nl = path_join(next_dir, sid);
        if (oa && na && fs_is_dir(oa) && !fs_exists(na) && rename(oa, na) != 0) LOGW("artifacts not moved: %s", strerror(errno));
        if (fs_is_dir(ol) && !fs_exists(nl) && rename(ol, nl) != 0) LOGW("local:// root not moved: %s", strerror(errno));
        free(oa);
        free(na);
        free(ol);
        free(nl);
        if (existed) { /* header.previousSessionFiles */
            if (!json_get_arr(s->header, "previousSessionFiles")) json_set(s->header, "previousSessionFiles", json_arr());
            json_t *prev = json_get_arr(s->header, "previousSessionFiles");
            bool dup = false;
            json_t *x;
            json_arr_foreach(prev, i, x) dup |= str_eq(json_sv(x), s->file);
            if (!dup) json_push_str(prev, s->file);
        }
        free(s->file);
        s->file = new_file;
        new_file = NULL;
        free(s->dir);
        s->dir = next_dir;
        next_dir = NULL;
    }
    json_set_str(s->header, "cwd", cwd);
    /* re-filter the extra roots: the new cwd may have been one of them */
    strvec_t roots = session_additional_dirs(s), keep = {0};
    for (size_t i = 0; i < roots.n; i++)
        if (strcmp(roots.v[i], cwd) != 0) strvec_push(&keep, roots.v[i]);
    if (!keep.n) json_del(s->header, "additionalDirectories");
    else if (keep.n != roots.n) {
        json_t *a = json_arr();
        for (size_t i = 0; i < keep.n; i++) json_push_str(a, keep.v[i]);
        json_set(s->header, "additionalDirectories", a);
    }
    strvec_free(&roots);
    strvec_free(&keep);
    if (s->persist && s->file && (existed || has_assistant(s))) write_full(s);
out:
    free(cwd);
    free(old_cwd);
    free(next_dir);
    free(new_file);
    return rc;
}

/* ---------- pins (session-pins.ts: <agent>/session-pins.json, a tab-indented JSON array of ids) ---------- */

static strvec_t pins_read(const char *path) {
    strvec_t out = {0};
    size_t n = 0;
    char *data = fs_read(path, &n);
    json_t *j = data ? json_parse(data, n, NULL) : NULL;
    free(data);
    json_t *x;
    json_arr_foreach(json_is_arr(j) ? j : NULL, i, x) if (json_is_str(x)) strvec_push(&out, json_sv(x));
    json_free(j);
    return out;
}

bool session_is_pinned(const char *id) {
    char *p = omc_agent_path("session-pins.json");
    strvec_t pins = pins_read(p);
    bool r = id && strvec_contains(&pins, id);
    strvec_free(&pins);
    free(p);
    return r;
}

void session_pins_sync(void);

/* updateSessionPin: read-modify-write under the file lock, atomic replace; `want` 1 pin, 0 unpin, -1 toggle. Returns whether it was
 * pinned before the update, -1 (+ *err) on failure. */
static int pin_update(const char *id, int want, char **err) {
    *err = NULL;
    char *p = omc_agent_path("session-pins.json");
    fs_mkdir_p(omc_agent_dir(), 0700);
    char *lockp = xasprintf("%s.lock", p);
    int lfd = open(lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lfd >= 0) flock(lfd, LOCK_EX);
    strvec_t pins = pins_read(p), next = {0};
    bool was = strvec_contains(&pins, id);
    for (size_t i = 0; i < pins.n; i++)
        if (strcmp(pins.v[i], id) != 0) strvec_push(&next, pins.v[i]);
    if (want < 0 ? !was : want) strvec_push(&next, id);
    sb_t b = SB_INIT;
    if (!next.n) sb_append(&b, "[]");
    else {
        sb_append(&b, "[\n");
        for (size_t i = 0; i < next.n; i++) {
            char *q = json_quote(next.v[i]);
            sb_appendf(&b, "\t%s%s\n", q, i + 1 < next.n ? "," : "");
            free(q);
        }
        sb_putc(&b, ']');
    }
    int rc = fs_write_atomic(p, b.s, b.len, 0600);
    sb_free(&b);
    if (lfd >= 0) {
        flock(lfd, LOCK_UN);
        close(lfd);
    }
    free(lockp);
    free(p);
    strvec_free(&pins);
    strvec_free(&next);
    if (rc != 0) {
        *err = xasprintf("Failed to save session pins: %s", strerror(-rc));
        return -1;
    }
    session_pins_sync();
    return was ? 1 : 0;
}

int session_pin_toggle(const char *id, char **err) {
    int was = pin_update(id, -1, err);
    return was < 0 ? -1 : !was;
}

int session_pin_clear(const char *id, char **err) { return pin_update(id, 0, err); }

/* the set the status line reads every frame, refreshed at startup, on a session switch and after every update of ours */
static strvec_t g_pin_cache;
static pthread_mutex_t g_pin_mu = PTHREAD_MUTEX_INITIALIZER;

void session_pins_sync(void) {
    char *p = omc_agent_path("session-pins.json");
    strvec_t pins = pins_read(p);
    free(p);
    pthread_mutex_lock(&g_pin_mu);
    strvec_free(&g_pin_cache);
    g_pin_cache = pins;
    pthread_mutex_unlock(&g_pin_mu);
}

bool session_pin_cached(const char *id) {
    if (!id) return false;
    pthread_mutex_lock(&g_pin_mu);
    bool r = strvec_contains(&g_pin_cache, id);
    pthread_mutex_unlock(&g_pin_mu);
    return r;
}

/* ---------- listing ---------- */

static void session_info_free(session_info_t *i) {
    free(i->path);
    free(i->id);
    free(i->cwd);
    free(i->title);
    free(i->first_message);
}

void session_list_free(session_list_t *l) {
    for (size_t i = 0; i < l->n; i++) session_info_free(&l->v[i]);
    vec_free(l);
}

static bool read_info(const char *path, session_info_t *info) {
    memset(info, 0, sizeof(*info));
    bool trunc = false;
    size_t len;
    char *data = fs_read_limit(path, 4u << 20, &len, &trunc);
    if (!data) return false;
    strvec_t lines = str_split_lines(data, len);
    free(data);
    bool ok = false;
    for (size_t i = 0; i < lines.n; i++) {
        if (!*lines.v[i]) continue;
        json_t *j = json_parse_str(lines.v[i]);
        const char *t = json_get_str(j, "type");
        if (str_eq(t, "title") && !info->title) {
            const char *tt = json_get_str(j, "title");
            if (tt && *tt) info->title = xstrdup(tt);
        } else if (str_eq(t, "session") && !ok) { /* the first header wins, as in session_open */
            ok = true;
            info->id = xstrdup(json_get_str(j, "id"));
            info->cwd = xstrdup(json_get_str(j, "cwd"));
            if (!info->title && json_get_str(j, "title")) info->title = xstrdup(json_get_str(j, "title"));
        } else if (str_eq(t, "message")) {
            json_t *m = json_get(j, "message");
            const char *role = json_get_str(m, "role");
            if (role && (!strcmp(role, "user") || !strcmp(role, "assistant"))) info->message_count++;
            if (str_eq(role, "user") && !info->first_message) {
                json_t *c = json_get(m, "content");
                if (json_is_str(c)) info->first_message = xstrdup(json_sv(c));
                else {
                    json_t *b;
                    json_arr_foreach(c, k, b) {
                        if (!info->first_message && str_eq(json_get_str(b, "type"), "text"))
                            info->first_message = xstrdup(json_get_str(b, "text"));
                    }
                }
            }
        }
        json_free(j);
    }
    strvec_free(&lines);
    if (!ok) {
        session_info_free(info);
        return false;
    }
    info->path = xstrdup(path);
    info->mtime_ms = fs_mtime_ms(path);
    return true;
}

/* a preliminary row: what the first bytes of the file say. The header holds the title slot, so nothing past it is read
 * (no prompts, status or counts). *is_session tells a session file from any other. */
static bool read_head_info(const char *path, session_info_t *info, bool *is_session) {
    memset(info, 0, sizeof(*info));
    *is_session = false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char head[8192];
    size_t n = fread(head, 1, sizeof head - 1, f);
    fclose(f);
    head[n] = 0;
    /* the session header and the title slot beside it; a header past the window waits for the full scan */
    char *line = head;
    for (int k = 0; k < 2; k++) {
        char *nl = strchr(line, '\n');
        if (!nl) break;
        *nl = 0;
        json_t *j = json_parse_str(line);
        const char *type = json_get_str(j, "type");
        const char *title = json_get_str(j, "title");
        if (str_eq(type, "session")) {
            *is_session = true;
            info->id = xstrdup(json_get_str(j, "id"));
            info->cwd = xstrdup(json_get_str(j, "cwd"));
        }
        if ((str_eq(type, "session") || str_eq(type, "title")) && title && *title && !info->title) info->title = xstrdup(title);
        json_free(j);
        line = nl + 1;
    }
    if (!*is_session || !info->title) {
        free(info->id);
        free(info->cwd);
        free(info->title);
        memset(info, 0, sizeof(*info));
        return false;
    }
    info->path = xstrdup(path);
    info->mtime_ms = fs_mtime_ms(path);
    return info->title != NULL;
}

static int cmp_info(const void *a, const void *b) {
    int64_t x = ((const session_info_t *)a)->mtime_ms, y = ((const session_info_t *)b)->mtime_ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

int session_list(const char *dir, session_list_t *out) {
    memset(out, 0, sizeof(*out));
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return -1;
    for (size_t i = 0; i < dl.n; i++) {
        if (dl.v[i].is_dir || !str_ends(dl.v[i].name, ".jsonl")) continue;
        char *p = path_join(dir, dl.v[i].name);
        session_info_t info;
        if (read_info(p, &info)) vec_push(out, info);
        free(p);
    }
    dirlist_free(&dl);
    if (out->n > 1) xqsort(out->v, out->n, sizeof(session_info_t), cmp_info);
    return 0;
}

int session_list_titled(const char *dir, session_list_t *out) {
    memset(out, 0, sizeof(*out));
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return -1;
    for (size_t i = 0; i < dl.n; i++) {
        if (dl.v[i].is_dir || !str_ends(dl.v[i].name, ".jsonl")) continue;
        char *p = path_join(dir, dl.v[i].name);
        session_info_t info;
        bool is_session;
        if (read_head_info(p, &info, &is_session)) vec_push(out, info);
        free(p);
    }
    dirlist_free(&dl);
    if (out->n > 1) xqsort(out->v, out->n, sizeof(session_info_t), cmp_info);
    return 0;
}

int session_list_all_titled(session_list_t *out) {
    memset(out, 0, sizeof(*out));
    char *root = omc_agent_path("sessions");
    dirlist_t dl;
    if (fs_list_dir(root, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!dl.v[i].is_dir) continue;
            char *d = path_join(root, dl.v[i].name);
            session_list_t sub;
            if (session_list_titled(d, &sub) == 0) {
                for (size_t k = 0; k < sub.n; k++) vec_push(out, sub.v[k]);
                vec_free(&sub);
            }
            free(d);
        }
        dirlist_free(&dl);
    }
    free(root);
    if (out->n > 1) xqsort(out->v, out->n, sizeof(session_info_t), cmp_info);
    return 0;
}

bool session_dir_has_sessions(const char *dir) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return false;
    bool found = false;
    for (size_t i = 0; i < dl.n && !found; i++) {
        if (dl.v[i].is_dir || !str_ends(dl.v[i].name, ".jsonl")) continue;
        char *p = path_join(dir, dl.v[i].name);
        session_info_t info;
        bool is_session;
        bool titled = read_head_info(p, &info, &is_session);
        if (titled) session_info_free(&info);
        found = is_session;
        if (!found && read_info(p, &info)) { /* a header past the head window */
            session_info_free(&info);
            found = true;
        }
        free(p);
    }
    dirlist_free(&dl);
    return found;
}

bool session_any_sessions(void) {
    char *root = omc_agent_path("sessions");
    dirlist_t dl;
    bool found = false;
    if (fs_list_dir(root, &dl) == 0) {
        for (size_t i = 0; i < dl.n && !found; i++) {
            if (!dl.v[i].is_dir) continue;
            char *d = path_join(root, dl.v[i].name);
            found = session_dir_has_sessions(d);
            free(d);
        }
        dirlist_free(&dl);
    }
    free(root);
    return found;
}

void session_list_remove(session_list_t *l, size_t i) {
    if (i >= l->n) return;
    session_info_free(&l->v[i]);
    memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof l->v[0]);
    l->n--;
}

int session_list_all(session_list_t *out) {
    memset(out, 0, sizeof(*out));
    char *root = omc_agent_path("sessions");
    dirlist_t dl;
    if (fs_list_dir(root, &dl) == 0) {
        for (size_t i = 0; i < dl.n; i++) {
            if (!dl.v[i].is_dir) continue;
            char *d = path_join(root, dl.v[i].name);
            session_list_t sub;
            if (session_list(d, &sub) == 0) {
                for (size_t k = 0; k < sub.n; k++) vec_push(out, sub.v[k]);
                vec_free(&sub);
            }
            free(d);
        }
        dirlist_free(&dl);
    }
    free(root);
    if (out->n > 1) xqsort(out->v, out->n, sizeof(session_info_t), cmp_info);
    return 0;
}

session_t *session_continue_recent(const char *cwd, const char *session_dir) {
    char *dir = session_dir ? xstrdup(session_dir) : session_dir_for_cwd(cwd);
    session_list_t l;
    session_t *s = NULL;
    if (session_list(dir, &l) == 0 && l.n) s = session_open(l.v[0].path, NULL);
    session_list_free(&l);
    free(dir);
    return s;
}

char *session_resolve(const char *query, const char *cwd) {
    if (fs_is_file(query)) return path_abs(query, NULL);
    char *dir = session_dir_for_cwd(cwd);
    session_list_t l;
    char *found = NULL;
    for (int pass = 0; pass < 2 && !found; pass++) {
        if (pass == 0) session_list(dir, &l);
        else session_list_all(&l);
        for (size_t i = 0; i < l.n && !found; i++) {
            if ((l.v[i].id && strstr(l.v[i].id, query)) || strstr(l.v[i].path, query))
                found = xstrdup(l.v[i].path);
        }
        session_list_free(&l);
    }
    free(dir);
    return found;
}

/* ---------- export ---------- */

static void md_content(sb_t *b, const json_t *content) {
    if (json_is_str(content)) {
        sb_append(b, json_sv(content));
        return;
    }
    json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text")) sb_appendf(b, "%s\n", json_get_str(blk, "text"));
        else if (str_eq(t, "thinking")) {
            char *q = str_indent(json_get_str(blk, "thinking") ? json_get_str(blk, "thinking") : "", "> ");
            sb_appendf(b, "%s\n\n", q);
            free(q);
        } else if (str_eq(t, "toolCall")) {
            char *args = json_dump(json_get(blk, "arguments"), 2);
            sb_appendf(b, "**Tool call `%s`**\n```json\n%s\n```\n", json_get_str(blk, "name"), args);
            free(args);
        } else if (str_eq(t, "image")) {
            sb_append(b, "[image]\n");
        }
    }
}

char *session_export_markdown(const session_t *s) {
    sb_t b = SB_INIT;
    session_lock(s);
    sb_appendf(&b, "# %s\n\n", s->title ? s->title : "Session");
    sb_appendf(&b, "- Session: `%s`\n- Directory: `%s`\n- Created: %s\n\n", session_id(s), session_cwd(s),
               json_get_str(s->header, "timestamp"));
    json_t *path = session_current_path(s);
    session_unlock(s);
    json_t *e;
    json_arr_foreach(path, i, e) {
        if (!str_eq(json_get_str(e, "type"), "message")) continue;
        json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) sb_append(&b, "## User\n\n");
        else if (str_eq(role, "assistant")) sb_appendf(&b, "## Assistant (%s)\n\n", json_get_str(m, "model"));
        else if (str_eq(role, "toolResult")) sb_appendf(&b, "### Tool result: %s%s\n\n", json_get_str(m, "toolName"),
                                                      json_get_bool(m, "isError", false) ? " (error)" : "");
        else continue;
        if (str_eq(role, "toolResult")) sb_append(&b, "```\n");
        md_content(&b, json_get(m, "content"));
        if (str_eq(role, "toolResult")) sb_append(&b, "```\n");
        sb_putc(&b, '\n');
    }
    json_free(path);
    return sb_detach(&b);
}

