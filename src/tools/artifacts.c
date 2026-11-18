/* artifacts.c - see artifacts.h */
#include "artifacts.h"
#include "tools.h"

#include "config/settings.h"
#include "core/fs.h"

#include <ctype.h>
#include <pthread.h>

struct artifact_store {
    pthread_mutex_t mu;
    int refs;
    char *dir;      /* session dir, or NULL */
    char *mem_dir;  /* temp dir for a non-persistent session's saves (lazy) */
    int64_t next;   /* -1: not scanned yet */
    int64_t mem_next;
};

artifact_store_t *artifacts_new(const char *dir) {
    artifact_store_t *s = xcalloc(1, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->refs = 1;
    s->dir = dir ? xstrdup(dir) : NULL;
    s->next = -1;
    return s;
}

artifact_store_t *artifacts_ref(artifact_store_t *s) {
    if (!s) return NULL;
    pthread_mutex_lock(&s->mu);
    s->refs++;
    pthread_mutex_unlock(&s->mu);
    return s;
}

void artifacts_unref(artifact_store_t *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    int r = --s->refs;
    pthread_mutex_unlock(&s->mu);
    if (r > 0) return;
    if (s->mem_dir) fs_rm_rf(s->mem_dir);
    free(s->mem_dir);
    free(s->dir);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

const char *artifacts_dir(const artifact_store_t *s) { return s ? s->dir : NULL; }

char *artifacts_dir_for_session(const char *session_file) {
    if (!session_file || !str_ends(session_file, ".jsonl")) return NULL;
    return xstrndup(session_file, strlen(session_file) - 6);
}

/* `<n>.<anything>.log` -> n, else -1 */
static int64_t file_id(const char *name) {
    if (!isdigit((unsigned char)*name) || !str_ends(name, ".log")) return -1;
    const char *p = name;
    int64_t v = 0;
    while (isdigit((unsigned char)*p)) {
        if (v > (INT64_MAX - 9) / 10) return -1;
        v = v * 10 + (*p++ - '0');
    }
    if (*p != '.' || strlen(p) < 5) return -1; /* ".log" alone would need `n..log`; omp's /^(\d+)\..*\.log$/ */
    return v;
}

static int64_t scan_max(const char *dir) {
    dirlist_t dl = {0};
    int64_t max = -1;
    if (fs_list_dir(dir, &dl) == 0)
        for (size_t i = 0; i < dl.n; i++) {
            int64_t id = file_id(dl.v[i].name);
            if (id > max) max = id;
        }
    dirlist_free(&dl);
    return max;
}

/* sanitizeToolType */
static char *tool_type(const char *t) {
    sb_t b = SB_INIT;
    bool in_bad = false;
    for (const char *p = t ? t : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '_' || c == '-') {
            sb_putc(&b, (char)c);
            in_bad = false;
        } else if (!in_bad) {
            sb_putc(&b, '_');
            in_bad = true;
        }
    }
    char *s = sb_detach(&b);
    if (strlen(s) > 64) s[64] = 0;
    size_t a = 0, e = strlen(s);
    while (a < e && s[a] == '_') a++;
    while (e > a && s[e - 1] == '_') e--;
    char *r = e > a ? xstrndup(s + a, e - a) : xstrdup("tool");
    free(s);
    return r;
}

static char *alloc_in(artifact_store_t *s, const char *dir, int64_t *counter, bool scan, const char *tool, char **id) {
    pthread_mutex_lock(&s->mu);
    if (*counter < 0) {
        fs_mkdir_p(dir, 0700);
        *counter = scan ? scan_max(dir) + 1 : 0;
    }
    int64_t n = (*counter)++;
    pthread_mutex_unlock(&s->mu);
    char *tt = tool_type(tool);
    char *path = xasprintf("%s/%lld.%s.log", dir, (long long)n, tt);
    free(tt);
    *id = xasprintf("%lld", (long long)n);
    return path;
}

char *artifacts_alloc(artifact_store_t *s, const char *tool, char **id) {
    *id = NULL;
    if (!s || !s->dir) return NULL;
    return alloc_in(s, s->dir, &s->next, true, tool, id);
}

char *artifacts_save(artifact_store_t *s, const char *tool, const char *data, size_t len) {
    if (!s) return NULL;
    char *id = NULL, *path;
    if (s->dir) {
        path = alloc_in(s, s->dir, &s->next, true, tool, &id);
    } else {
        pthread_mutex_lock(&s->mu);
        if (!s->mem_dir) {
            s->mem_dir = fs_mktempdir("omc-artifacts");
            s->mem_next = -1;
        }
        pthread_mutex_unlock(&s->mu);
        if (!s->mem_dir) return NULL;
        path = alloc_in(s, s->mem_dir, &s->mem_next, false, tool, &id);
    }
    int rc = fs_write_atomic(path, data, len, 0600);
    free(path);
    if (rc != 0) {
        free(id);
        return NULL;
    }
    return id;
}

static char *find_in(const char *dir, const char *id) {
    if (!dir) return NULL;
    dirlist_t dl = {0};
    char *r = NULL;
    size_t il = strlen(id);
    if (fs_list_dir(dir, &dl) == 0)
        for (size_t i = 0; i < dl.n && !r; i++)
            if (!strncmp(dl.v[i].name, id, il) && dl.v[i].name[il] == '.') r = path_join(dir, dl.v[i].name);
    dirlist_free(&dl);
    return r;
}

char *artifacts_find(artifact_store_t *s, const char *id) {
    if (!s || !id || !*id) return NULL;
    char *p = find_in(s->dir, id);
    if (!p) {
        pthread_mutex_lock(&s->mu);
        char *md = s->mem_dir ? xstrdup(s->mem_dir) : NULL;
        pthread_mutex_unlock(&s->mu);
        p = find_in(md, id);
        free(md);
    }
    return p;
}

static int cmp_num(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return x < y ? -1 : x > y;
}

char *artifacts_missing_error(artifact_store_t *s, const char *id) {
    pthread_mutex_lock(&s->mu);
    char *md = s->mem_dir ? xstrdup(s->mem_dir) : NULL;
    pthread_mutex_unlock(&s->mu);
    const char *dirs[] = {s->dir, md};
    bool any = false;
    VEC(long long) ids = {0};
    for (size_t k = 0; k < ARRAY_LEN(dirs); k++) {
        dirlist_t dl = {0};
        if (!dirs[k] || fs_list_dir(dirs[k], &dl) != 0) continue;
        any = true;
        for (size_t i = 0; i < dl.n; i++) {
            const char *n = dl.v[i].name;
            size_t dn = strspn(n, "0123456789");
            if (!dn || n[dn] != '.') continue;
            long long v = strtoll(n, NULL, 10);
            bool dup = false;
            for (size_t j = 0; j < ids.n; j++) dup |= ids.v[j] == v;
            if (!dup) vec_push(&ids, v);
        }
        dirlist_free(&dl);
    }
    free(md);
    if (!any) {
        vec_free(&ids);
        return xstrdup("No artifacts directory found");
    }
    qsort(ids.v, ids.n, sizeof ids.v[0], cmp_num);
    sb_t b = SB_INIT;
    for (size_t j = 0; j < ids.n; j++) sb_appendf(&b, "%s%lld", j ? ", " : "", ids.v[j]);
    char *r = xasprintf("Artifact %s not found. Available: %s", id, b.len ? b.s : "none");
    sb_free(&b);
    vec_free(&ids);
    return r;
}

void artifacts_copy_dir(const char *src_dir, const char *dst_dir) {
    if (!src_dir || !dst_dir || !fs_is_dir(src_dir)) return;
    dirlist_t dl = {0};
    if (fs_list_dir(src_dir, &dl) != 0) return;
    fs_mkdir_p(dst_dir, 0700);
    for (size_t i = 0; i < dl.n; i++) {
        if (dl.v[i].is_dir) continue;
        char *src = path_join(src_dir, dl.v[i].name), *dst = path_join(dst_dir, dl.v[i].name);
        if (!fs_exists(dst)) {
            size_t n;
            char *data = fs_read(src, &n);
            if (data) fs_write(dst, data, n);
            free(data);
        }
        free(src);
        free(dst);
    }
    dirlist_free(&dl);
}

/* ---------- settings ---------- */

static int64_t kb_setting(const char *key, double def) {
    double v = settings_num(key, def);
    if (v < 0) v = 0;
    return (int64_t)(v * 1024);
}

size_t tool_spill_threshold(void) { return (size_t)kb_setting("tools.artifactSpillThreshold", 50); }

void out_sink_opts_from_settings(out_sink_opts_t *o) {
    memset(o, 0, sizeof *o);
    o->spill_threshold = tool_spill_threshold();
    if (!o->spill_threshold) o->spill_threshold = 1; /* 0 KB: spill everything */
    o->head_bytes = (size_t)kb_setting("tools.artifactHeadBytes", 20);
    int64_t cols = settings_int("tools.outputMaxColumns", 768);
    o->max_columns = cols > 0 ? (size_t)cols : 0;
    double mb = settings_num("tools.artifactMaxBytes", 16);
    o->artifact_max_bytes = mb > 0 ? (int64_t)(mb * 1024 * 1024) : 0;
    o->artifact_head_bytes = -1;
}

/* ---------- spill + notice ---------- */

static json_t *range(int64_t a, int64_t b) {
    json_t *r = json_obj();
    json_set_int(r, "start", a);
    json_set_int(r, "end", b);
    return r;
}

static void spill_large(tool_env_t *env, const char *tool_name, tool_result_t *res) {
    json_t *meta = json_get_obj(res->details, "meta");
    if (json_get_str(json_get_obj(meta, "truncation"), "artifactId")) return;
    if (json_get_bool(meta, "pagedSource", false)) return;
    size_t threshold = tool_spill_threshold();
    sb_t full = SB_INIT;
    size_t parts = 0;
    json_t *blk;
    json_arr_foreach(res->content, i, blk) {
        const char *t = json_get_str(blk, "text");
        if (!str_eq(json_get_str(blk, "type"), "text") || !t || !*t) continue;
        if (parts++) sb_putc(&full, '\n');
        sb_append(&full, t);
    }
    if (!parts || full.len <= threshold) {
        sb_free(&full);
        return;
    }
    char *aid = NULL;
    if (!json_get_str(meta, "artifactError")) aid = artifacts_save(env->artifacts, tool_name, full.s, full.len);
    int64_t tail_b = (int64_t)(settings_num("tools.artifactTailBytes", 20) * 1024);
    int64_t tail_l = settings_int("tools.artifactTailLines", 500);
    int64_t head_b = (int64_t)(settings_num("tools.artifactHeadBytes", 20) * 1024);
    out_trunc_t tr;
    if (head_b > 0) out_truncate_middle(full.s, full.len, head_b + tail_b, tail_l * 2, head_b, tail_l, &tr);
    else out_truncate_tail(full.s, full.len, tail_b, tail_l, &tr);
    json_t *nc = json_arr();
    json_arr_foreach(res->content, i, blk) if (!str_eq(json_get_str(blk, "type"), "text")) json_push(nc, json_clone(blk));
    json_t *tb = json_obj();
    json_set_str(tb, "type", "text");
    json_set(tb, "text", json_strn(tr.content, tr.content_len));
    json_push(nc, tb);
    json_free(res->content);
    res->content = nc;

    const json_t *old_t = json_get_obj(meta, "truncation");
    bool has_next = old_t && json_get(old_t, "nextOffset");
    int64_t next = json_get_int(old_t, "nextOffset", 0);
    json_t *t = json_obj();
    int64_t ol = tr.output_lines, ob = tr.output_bytes;
    if (tr.by == OUT_BY_MIDDLE) {
        int64_t el = tr.elided_lines >= 0 ? tr.elided_lines : MAX(0, tr.total_lines - ol);
        int64_t eb = tr.elided_bytes >= 0 ? tr.elided_bytes : MAX(0, tr.total_bytes - ob);
        int64_t kept = MAX(0, ol - 1);
        int64_t hl = tr.head_lines >= 0 ? tr.head_lines : (kept + 1) / 2;
        int64_t tl = tr.tail_lines >= 0 ? tr.tail_lines : kept - hl;
        json_set_str(t, "direction", "middle");
        json_set_str(t, "truncatedBy", "middle");
        json_set_int(t, "totalLines", tr.total_lines);
        json_set_int(t, "totalBytes", tr.total_bytes);
        json_set_int(t, "outputLines", ol);
        json_set_int(t, "outputBytes", ob);
        json_set_int(t, "maxBytes", head_b + tail_b);
        if (tr.total_lines > 1 && !tr.partial_byte_windows) {
            if (hl > 0) json_set(t, "headRange", range(1, hl));
            if (tl > 0) json_set(t, "tailRange", range(tr.total_lines - tl + 1, tr.total_lines));
        }
        json_set_int(t, "elidedLines", el);
        json_set_int(t, "elidedBytes", eb);
    } else {
        json_set_str(t, "direction", "tail");
        json_set_str(t, "truncatedBy", tr.by == OUT_BY_LINES ? "lines" : "bytes");
        json_set_int(t, "totalLines", tr.total_lines);
        json_set_int(t, "totalBytes", tr.total_bytes);
        json_set_int(t, "outputLines", ol);
        json_set_int(t, "outputBytes", ob);
        json_set_int(t, "maxBytes", tail_b);
        json_set(t, "shownRange", range(tr.total_lines - ol + 1, tr.total_lines));
    }
    if (aid) json_set_str(t, "artifactId", aid);
    if (has_next) json_set_int(t, "nextOffset", next);
    if (!res->details) res->details = json_obj();
    json_t *m = json_get_obj(res->details, "meta");
    if (!m) {
        m = json_obj();
        json_set(res->details, "meta", m);
    }
    json_set(m, "truncation", t);
    /* MCP results duplicate their payload into details.rawContent: drop what the artifact now holds */
    json_t *raw = json_get_arr(res->details, "rawContent");
    if (json_get_str(res->details, "serverName") && json_get_str(res->details, "mcpToolName") && raw) {
        json_t *keep = json_arr(), *x;
        json_arr_foreach(raw, i, x) {
            const char *ty = json_get_str(x, "type");
            if (json_is_obj(x) && (str_eq(ty, "text") || str_eq(ty, "image"))) continue;
            json_t *c = json_clone(x);
            json_t *r = json_get_obj(c, "resource");
            if (json_is_obj(c) && str_eq(ty, "resource") && r && json_get(r, "text")) json_del(r, "text");
            json_push(keep, c);
        }
        if (json_len(keep)) json_set(res->details, "rawContent", keep);
        else {
            json_free(keep);
            json_del(res->details, "rawContent");
        }
    }
    free(aid);
    out_trunc_free(&tr);
    sb_free(&full);
}

void tool_postprocess_result(tool_env_t *env, const char *tool_name, tool_result_t *res) {
    if (!res || !res->content) return;
    /* omp's wrapper only sees results; thrown tool errors (omc: detail-less error results) bypass it */
    if (res->is_error && !res->details) return;
    if (env && env->artifacts) spill_large(env, tool_name, res);
    const json_t *meta = json_get_obj(res->details, "meta");
    if (meta) out_append_notice(res->content, meta);
}
