#include "tools.h"

#include "core/fs.h"
#include "prompt/template.h"

/* ---------- edit store ---------- */

void edit_store_init(edit_store_t *s) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    hmap_init(&s->registers);
}

static void snap_free(snapshot_t *sn) {
    free(sn->path);
    free(sn->tag);
    free(sn->content);
    vec_free(&sn->seen);
    free(sn);
}

void edit_store_free(edit_store_t *s) {
    for (size_t i = 0; i < s->snaps.n; i++) snap_free(s->snaps.v[i]);
    vec_free(&s->snaps);
    hmap_free(&s->registers, free);
    pthread_mutex_destroy(&s->mu);
}

char *file_tag(const char *text, size_t len) {
    /* normalize: strip trailing [ \t\r] on every line */
    sb_t n = SB_INIT;
    sb_reserve(&n, len);
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t end = nl ? (size_t)(nl - text) : len;
        size_t e = end;
        while (e > i && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) e--;
        sb_appendn(&n, text + i, e - i);
        if (nl) sb_putc(&n, '\n');
        i = nl ? end + 1 : len;
    }
    uint32_t h = hash_xx32(n.s ? n.s : "", n.len, 0) & 0xffff;
    sb_free(&n);
    return xasprintf("%04X", h);
}

#define MAX_SNAPSHOTS 256

const char *edit_store_record(edit_store_t *s, const char *abs_path, const char *content, int64_t lo, int64_t hi) {
    char *tag = file_tag(content, strlen(content));
    pthread_mutex_lock(&s->mu);
    snapshot_t *sn = NULL;
    for (size_t i = 0; i < s->snaps.n; i++) {
        snapshot_t *x = s->snaps.v[i];
        if (strcmp(x->path, abs_path) == 0 && strcmp(x->tag, tag) == 0) {
            sn = x;
            /* move to end (most recent) */
            memmove(s->snaps.v + i, s->snaps.v + i + 1, (s->snaps.n - i - 1) * sizeof(void *));
            s->snaps.v[s->snaps.n - 1] = sn;
            break;
        }
    }
    if (!sn) {
        sn = xcalloc(1, sizeof(*sn));
        sn->path = xstrdup(abs_path);
        sn->tag = xstrdup(tag);
        sn->content = xstrdup(content);
        if (s->snaps.n >= MAX_SNAPSHOTS) {
            snap_free(s->snaps.v[0]);
            memmove(s->snaps.v, s->snaps.v + 1, (s->snaps.n - 1) * sizeof(void *));
            s->snaps.n--;
        }
        vec_push(&s->snaps, sn);
    }
    if (lo > 0 && hi >= lo) {
        line_span_t r = {lo, hi};
        vec_push(&sn->seen, r);
    } else if (lo == 0) {
        /* unrestricted view (whole file shown) */
        sn->seen.n = 0;
        line_span_t r = {1, INT64_MAX};
        vec_push(&sn->seen, r);
    }
    pthread_mutex_unlock(&s->mu);
    free(tag);
    return sn->tag;
}

snapshot_t *edit_store_find(edit_store_t *s, const char *abs_path, const char *tag) {
    snapshot_t *r = NULL;
    pthread_mutex_lock(&s->mu);
    for (size_t i = s->snaps.n; i > 0; i--) {
        snapshot_t *x = s->snaps.v[i - 1];
        if (strcmp(x->path, abs_path) == 0 && (!tag || strcasecmp(x->tag, tag) == 0)) {
            r = x;
            break;
        }
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}

/* ---------- env ---------- */

tool_env_t *tool_env_new(const char *cwd) {
    tool_env_t *e = xcalloc(1, sizeof(*e));
    e->cwd = cwd ? path_abs(cwd, NULL) : get_cwd();
    edit_store_init(&e->store);
    e->todo.phases = json_arr();
    e->edit_mode = xstrdup("hashline");
    pthread_mutex_init(&e->mu, NULL);
    return e;
}

void tool_env_free(tool_env_t *e) {
    if (!e) return;
    free(e->cwd);
    edit_store_free(&e->store);
    json_free(e->todo.phases);
    free(e->edit_mode);
    free(e->artifacts_dir);
    pthread_mutex_destroy(&e->mu);
    free(e);
}

char *tool_resolve_path(tool_env_t *env, const char *p) {
    if (!p || !*p || strcmp(p, "/") == 0) return xstrdup(env->cwd); /* omp: bare "/" means session cwd */
    if (str_starts(p, "file://")) p += 7;
    return path_abs(p, env->cwd);
}

/* ---------- truncation ---------- */

char *tool_truncate_tail(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated) {
    *truncated = false;
    size_t start = 0;
    if (max_lines) {
        size_t lines = 0;
        for (size_t i = len; i > 0; i--) {
            if (s[i - 1] == '\n' && i != len) {
                if (++lines >= max_lines) {
                    start = i;
                    *truncated = true;
                    break;
                }
            }
        }
    }
    if (max_bytes && len - start > max_bytes) {
        start = len - max_bytes;
        while (start < len && ((unsigned char)s[start] & 0xC0) == 0x80) start++;
        const char *nl = memchr(s + start, '\n', len - start);
        if (nl && (size_t)(nl - s) < len - 1) start = (size_t)(nl - s) + 1;
        *truncated = true;
    }
    return xstrndup(s + start, len - start);
}

char *tool_truncate_head(const char *s, size_t len, size_t max_bytes, size_t max_lines, bool *truncated) {
    *truncated = false;
    size_t end = len;
    if (max_lines) {
        size_t lines = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '\n' && ++lines >= max_lines) {
                end = i;
                *truncated = i + 1 < len;
                break;
            }
        }
    }
    if (max_bytes && end > max_bytes) {
        end = max_bytes;
        while (end > 0 && ((unsigned char)s[end] & 0xC0) == 0x80) end--;
        *truncated = true;
    }
    return xstrndup(s, end);
}

/* ---------- schemas ---------- */

json_t *schema_obj(void) {
    json_t *s = json_obj();
    json_set_str(s, "type", "object");
    json_set(s, "properties", json_obj());
    json_set(s, "required", json_arr());
    return s;
}

void schema_prop_json(json_t *schema, const char *name, json_t *prop, bool required) {
    json_set(json_get(schema, "properties"), name, prop);
    if (required) json_push_str(json_get(schema, "required"), name);
}

void schema_prop(json_t *schema, const char *name, const char *type, const char *desc, bool required) {
    json_t *p = json_obj();
    json_set_str(p, "type", type);
    if (desc) json_set_str(p, "description", desc);
    schema_prop_json(schema, name, p, required);
}

char *tool_prompt(const char *name, const json_t *ctx) {
    char *rel = xasprintf("coding-agent/prompts/tools/%s.md", name);
    char *r = prompt_render(rel, ctx);
    free(rel);
    return r;
}

void format_hashline_header(sb_t *b, const char *display_path, const char *tag) {
    sb_appendf(b, "[%s#%s]\n", display_path, tag);
}

/* ---------- registration ---------- */

static const char *const DEFAULT_TOOLS[] = {"read", "write", "edit", "bash", "grep", "glob", "todo", "ask",
                                            "web_search", "task", NULL};

const char *const *tools_default_names(void) { return DEFAULT_TOOLS; }

typedef agent_tool_t *(*tool_ctor)(tool_env_t *);
static const struct {
    const char *name;
    tool_ctor ctor;
} CTORS[] = {
    {"read", tool_read_new},   {"write", tool_write_new}, {"edit", tool_edit_new},
    {"bash", tool_bash_new},   {"grep", tool_grep_new},   {"glob", tool_glob_new},
    {"todo", tool_todo_new},   {"ask", tool_ask_new},     {"web_search", tool_web_search_new},
    {"task", tool_task_new},
};

void tools_register_builtin(agent_t *a, tool_env_t *env, const strvec_t *enabled) {
    for (size_t i = 0; i < ARRAY_LEN(CTORS); i++) {
        if (enabled && !strvec_contains(enabled, CTORS[i].name)) continue;
        agent_tool_t *t = CTORS[i].ctor(env);
        if (t) agent_add_tool(a, t);
    }
}
