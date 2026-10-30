#define _GNU_SOURCE
#include "session.h"

#include "core/fs.h"
#include "prompt/template.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    free(s);
}

const char *session_id(const session_t *s) { return json_get_str(s->header, "id"); }
const char *session_cwd(const session_t *s) { return json_get_str(s->header, "cwd"); }

json_t *session_get(const session_t *s, const char *id) { return id ? hmap_get(&s->by_id, id) : NULL; }

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

static int write_full(session_t *s) {
    if (!s->persist || !s->file) return 0;
    sb_t b = SB_INIT;
    char *slot = title_slot_line(s->title, s->title_source);
    if (slot) sb_append(&b, slot);
    free(slot);
    json_dump_sb(&b, s->header, 0);
    sb_putc(&b, '\n');
    json_t *e;
    json_arr_foreach(s->entries, i, e) {
        json_dump_sb(&b, e, 0);
        sb_putc(&b, '\n');
    }
    fs_mkdir_p(s->dir, 0700);
    int rc = fs_write_atomic(s->file, b.s, b.len, 0600);
    sb_free(&b);
    if (rc == 0) s->on_disk = true;
    else LOGE("session write failed: %s: %s", s->file, strerror(-rc));
    return rc;
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
    char *line = json_dump(e, 0);
    sb_t b = SB_INIT;
    sb_append(&b, line);
    sb_putc(&b, '\n');
    free(line);
    int rc = fs_append(s->file, b.s, b.len);
    if (rc != 0) LOGE("session append failed: %s", strerror(-rc));
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

const char *session_append_model_change(session_t *s, const char *pm, const char *role) {
    json_t *e = typed("model_change");
    json_set_str(e, "model", pm);
    if (role) json_set_str(e, "role", role);
    return session_append_entry(s, e);
}

const char *session_append_thinking_change(session_t *s, const char *level) {
    json_t *e = typed("thinking_level_change");
    json_set_str(e, "thinkingLevel", level);
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

void session_set_title(session_t *s, const char *title, const char *source) {
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
            s->title = xstrdup(json_get_str(j, "title"));
            if (json_get_str(j, "source")) s->title_source = xstrdup(json_get_str(j, "source"));
            json_free(j);
        } else if (str_eq(type, "session") && !s->header) {
            s->header = j;
        } else if (s->header) {
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
    if (!session_get(s, entry_id)) return;
    free(s->leaf_id);
    s->leaf_id = xstrdup(entry_id);
}

void session_reset_leaf(session_t *s) {
    free(s->leaf_id);
    s->leaf_id = NULL;
}

json_t *session_path_to(const session_t *s, const char *leaf) {
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
    json_t *out = json_arr();
    for (size_t i = json_len(rev); i > 0; i--) json_push(out, json_clone(json_at(rev, i - 1)));
    json_free(rev);
    return out;
}

json_t *session_children(const session_t *s, const char *id) {
    json_t *out = json_arr();
    json_t *e;
    json_arr_foreach(s->entries, i, e) {
        const char *p = json_get_str(e, "parentId");
        if ((id && str_eq(p, id)) || (!id && !p)) json_push(out, json_clone(e));
    }
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
    return m;
}

/* convert a persisted message to an LLM message (custom roles) or NULL to skip */
static json_t *to_llm(const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    if (!role) return NULL;
    if (!strcmp(role, "user") || !strcmp(role, "assistant") || !strcmp(role, "toolResult") || !strcmp(role, "developer"))
        return json_clone(msg);
    if (!strcmp(role, "bashExecution")) {
        if (json_get_bool(msg, "excludeFromContext", false)) return NULL;
        sb_t b = SB_INIT;
        sb_appendf(&b, "Ran `%s`\n", json_get_str(msg, "command") ? json_get_str(msg, "command") : "");
        const char *out = json_get_str(msg, "output");
        if (out && *out) sb_appendf(&b, "```\n%s\n```", out);
        else sb_append(&b, "(no output)");
        int64_t code = json_get_int(msg, "exitCode", 0);
        if (code) sb_appendf(&b, "\n\nCommand exited with code %lld", (long long)code);
        if (json_get_bool(msg, "cancelled", false)) sb_append(&b, "\n\n(command cancelled)");
        json_t *m = user_text_msg(sb_cstr(&b), json_get_int(msg, "timestamp", now_ms()));
        sb_free(&b);
        return m;
    }
    if (!strcmp(role, "custom") || !strcmp(role, "hookMessage") || !strcmp(role, "fileMention")) {
        json_t *m = json_obj();
        json_set_str(m, "role", "user");
        json_t *c = json_get(msg, "content");
        json_set(m, "content", c ? json_clone(c) : json_str(""));
        json_set_int(m, "timestamp", json_get_int(msg, "timestamp", now_ms()));
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
        json_set_str(m, "role", "user");
        json_t *c = json_get(e, "content");
        json_set(m, "content", c ? json_clone(c) : json_str(""));
        json_set_int(m, "timestamp", now_ms());
        json_push(out, m);
    } else if (str_eq(t, "branch_summary")) {
        json_push(out, summary_msg("agent/compaction/prompts/branch-summary-context.md", json_get_str(e, "summary")));
    }
}

void session_build_context(const session_t *s, session_context_t *out) {
    memset(out, 0, sizeof(*out));
    out->messages = json_arr();
    if (!s->leaf_id) return;
    json_t *path = session_path_to(s, s->leaf_id);
    size_t n = json_len(path);
    ssize_t boundary = -1, compaction = -1;
    for (size_t i = 0; i < n; i++) {
        json_t *e = json_at(path, i);
        const char *t = json_get_str(e, "type");
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
        } else if (str_eq(t, "reset_boundary")) {
            boundary = (ssize_t)i;
            compaction = -1;
        } else if (str_eq(t, "compaction")) {
            compaction = (ssize_t)i;
        }
    }
    size_t start = (size_t)(boundary + 1);
    if (compaction >= 0) {
        json_t *c = json_at(path, (size_t)compaction);
        out->compaction_tokens_before = json_get_int(c, "tokensBefore", 0);
        json_push(out->messages, summary_msg("agent/compaction/prompts/compaction-summary-context.md", json_get_str(c, "summary")));
        const char *first_kept = json_get_str(c, "firstKeptEntryId");
        ssize_t kept = -1;
        for (size_t i = start; first_kept && i < (size_t)compaction; i++)
            if (str_eq(json_get_str(json_at(path, i), "id"), first_kept)) kept = (ssize_t)i;
        if (kept >= 0)
            for (size_t i = (size_t)kept; i < (size_t)compaction; i++) emit_entry(out->messages, json_at(path, i));
        start = (size_t)compaction + 1;
    }
    for (size_t i = start; i < n; i++) emit_entry(out->messages, json_at(path, i));
    json_free(path);
}

void session_context_free(session_context_t *c) {
    json_free(c->messages);
    free(c->model);
    free(c->thinking);
    memset(c, 0, sizeof(*c));
}

session_t *session_fork(const session_t *s, const char *leaf_id, const char *session_dir) {
    session_t *n = session_create(session_cwd(s), session_dir ? session_dir : s->dir);
    json_set_str(n->header, "parentSession", session_id(s));
    json_t *path = session_path_to(s, leaf_id ? leaf_id : s->leaf_id);
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
        } else if (str_eq(t, "session")) {
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
    if (out->n > 1) qsort(out->v, out->n, sizeof(session_info_t), cmp_info);
    return 0;
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
    if (out->n > 1) qsort(out->v, out->n, sizeof(session_info_t), cmp_info);
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
            if ((l.v[i].id && str_starts(l.v[i].id, query)) || strstr(l.v[i].path, query))
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
    sb_appendf(&b, "# %s\n\n", s->title ? s->title : "Session");
    sb_appendf(&b, "- Session: `%s`\n- Directory: `%s`\n- Created: %s\n\n", session_id(s), session_cwd(s),
               json_get_str(s->header, "timestamp"));
    json_t *path = session_path_to(s, s->leaf_id);
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

static void html_escape(sb_t *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
        case '<': sb_append(b, "&lt;"); break;
        case '>': sb_append(b, "&gt;"); break;
        case '&': sb_append(b, "&amp;"); break;
        case '"': sb_append(b, "&quot;"); break;
        default: sb_putc(b, *s);
        }
    }
}

char *session_export_html(const session_t *s) {
    char *md = session_export_markdown(s);
    sb_t b = SB_INIT;
    sb_append(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><title>");
    html_escape(&b, s->title ? s->title : "omc session");
    sb_append(&b, "</title><style>body{font:14px/1.5 ui-monospace,monospace;max-width:960px;margin:2em auto;padding:0 1em;"
                  "background:#111;color:#ddd}pre{white-space:pre-wrap;word-wrap:break-word}</style></head><body><pre>");
    html_escape(&b, md);
    sb_append(&b, "</pre></body></html>\n");
    free(md);
    return sb_detach(&b);
}
