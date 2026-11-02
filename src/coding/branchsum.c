/* branchsum.c - branch summaries for tree navigation (see branchsum.h). */
#include "coding/branchsum.h"

#include "prompt/template.h"

#include <ctype.h>

#define TOOL_RESULT_MAX_CHARS 2000
#define FILE_LIST_LIMIT 20

/* ---------- paths (compaction/utils.ts) ---------- */

/* `L?\d+(?:(?:[-+]|\.\.)L?\d+|-|\.\.)?` at *p; advances past it */
static bool sel_range(const char **p) {
    const char *s = *p;
    if (*s == 'L' || *s == 'l') s++;
    if (!isdigit((unsigned char)*s)) return false;
    while (isdigit((unsigned char)*s)) s++;
    const char *save = s;
    if (*s == '-' || *s == '+' || (s[0] == '.' && s[1] == '.')) {
        const char *t = s + (*s == '.' ? 2 : 1);
        const char *u = t;
        if (*u == 'L' || *u == 'l') u++;
        if (isdigit((unsigned char)*u)) {
            while (isdigit((unsigned char)*u)) u++;
            s = u;
        } else if (*save == '-' || *save == '.') s = t; /* trailing `-` / `..` */
        else s = save;
    }
    *p = s;
    return true;
}

/* `^range(,range)*$` */
static bool sel_ranges(const char *s) {
    if (!*s) return false;
    for (;;) {
        if (!sel_range(&s)) return false;
        if (!*s) return true;
        if (*s != ',') return false;
        s++;
    }
}

static bool is_raw(const char *s) { return !strcasecmp(s, "raw"); }

/* stripReadSelector: `path:<lines|raw|conflicts>` -> path */
static char *strip_read_selector(const char *path) {
    const char *colon = strrchr(path, ':');
    if (!colon || colon == path) return xstrdup(path);
    const char *sel = colon + 1;
    if (!sel_ranges(sel) && !is_raw(sel) && strcasecmp(sel, "conflicts")) return xstrdup(path);
    size_t base = (size_t)(colon - path);
    /* a second selector level: `raw` with line ranges either way round */
    char *head = xstrndup(path, base);
    const char *c2 = strrchr(head, ':');
    if (c2 && c2 != head) {
        const char *inner = c2 + 1;
        if ((is_raw(inner) && sel_ranges(sel)) || (sel_ranges(inner) && is_raw(sel))) base = (size_t)(c2 - head);
    }
    free(head);
    return xstrndup(path, base);
}

/* `[a-z][a-z0-9+.-]*://` anywhere (internal URLs are not files) */
static bool has_url_scheme(const char *s) {
    for (const char *p = strstr(s, "://"); p; p = strstr(p + 1, "://")) {
        const char *q = p;
        while (q > s && (isalnum((unsigned char)q[-1]) || q[-1] == '+' || q[-1] == '.' || q[-1] == '-')) q--;
        while (q < p && !isalpha((unsigned char)*q)) q++;
        if (q < p) return true;
    }
    return false;
}

static void set_add(json_t *arr, const char *v) {
    const json_t *x;
    json_arr_foreach(arr, i, x) if (str_eq(json_sv(x), v)) return;
    json_push_str(arr, v);
}

static bool set_has(const json_t *arr, const char *v) {
    const json_t *x;
    json_arr_foreach(arr, i, x) if (str_eq(json_sv(x), v)) return true;
    return false;
}

/* extractFileOpsFromMessage: read / write / edit tool calls of an assistant message */
static void extract_file_ops(const json_t *msg, json_t *ops) {
    if (!str_eq(json_get_str(msg, "role"), "assistant")) return;
    const json_t *b;
    json_arr_foreach(json_get_arr(msg, "content"), i, b) {
        if (!str_eq(json_get_str(b, "type"), "toolCall")) continue;
        const char *path = json_get_str(json_get(b, "arguments"), "path");
        if (!path || !*path || has_url_scheme(path)) continue;
        const char *name = json_get_str(b, "name");
        if (str_eq(name, "read")) {
            char *p = strip_read_selector(path);
            set_add(json_get(ops, "read"), p);
            free(p);
        } else if (str_eq(name, "write")) set_add(json_get(ops, "written"), path);
        else if (str_eq(name, "edit")) set_add(json_get(ops, "edited"), path);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static json_t *sorted(strvec_t *v) {
    qsort(v->v, v->n, sizeof *v->v, cmp_str);
    json_t *out = json_arr();
    for (size_t i = 0; i < v->n; i++) json_push_str(out, v->v[i]);
    return out;
}

/* computeFileLists */
static void file_lists(const json_t *ops, json_t **read_out, json_t **mod_out) {
    strvec_t mod = {0}, rd = {0};
    const json_t *x;
    json_arr_foreach(json_get(ops, "edited"), i, x) if (!has_url_scheme(json_sv(x)) && !strvec_contains(&mod, json_sv(x)))
        strvec_push(&mod, json_sv(x));
    json_arr_foreach(json_get(ops, "written"), i, x) if (!has_url_scheme(json_sv(x)) && !strvec_contains(&mod, json_sv(x)))
        strvec_push(&mod, json_sv(x));
    json_arr_foreach(json_get(ops, "read"), i, x) if (!has_url_scheme(json_sv(x)) && !strvec_contains(&mod, json_sv(x)))
        strvec_push(&rd, json_sv(x));
    *read_out = sorted(&rd);
    *mod_out = sorted(&mod);
    strvec_free(&rd);
    strvec_free(&mod);
}

/* path tree (utils formatPathTree): files first, then directories as `#`-depth headings, single-child
 * directory chains collapsed */
typedef struct pdir {
    char *name;
    strvec_t files, tags;
    VEC(struct pdir *) dirs;
} pdir_t;

static pdir_t *pdir_new(const char *name) {
    pdir_t *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(name);
    return d;
}

static void pdir_free(pdir_t *d) {
    for (size_t i = 0; i < d->dirs.n; i++) pdir_free(d->dirs.v[i]);
    vec_free(&d->dirs);
    strvec_free(&d->files);
    strvec_free(&d->tags);
    free(d->name);
    free(d);
}

static void pdir_emit(const pdir_t *d, int depth, sb_t *out) {
    for (size_t i = 0; i < d->files.n; i++) {
        if (out->len) sb_putc(out, '\n');
        sb_appendf(out, "%s%s", d->files.v[i], d->tags.v[i]);
    }
    for (size_t i = 0; i < d->dirs.n; i++) {
        const pdir_t *node = d->dirs.v[i];
        sb_t name = SB_INIT;
        sb_append(&name, node->name);
        while (!node->files.n && node->dirs.n == 1) {
            node = node->dirs.v[0];
            sb_appendf(&name, "/%s", node->name);
        }
        if (out->len) sb_putc(out, '\n');
        for (int k = 0; k <= depth; k++) sb_putc(out, '#');
        sb_appendf(out, " %s/", sb_cstr(&name));
        sb_free(&name);
        pdir_emit(node, depth + 1, out);
    }
}

static char *path_tree(const strvec_t *paths, const strvec_t *tags) {
    pdir_t *root = pdir_new("");
    for (size_t i = 0; i < paths->n; i++) {
        const char *p = paths->v[i];
        if (has_url_scheme(p)) {
            if (!strvec_contains(&root->files, p)) strvec_push(&root->files, p), strvec_push(&root->tags, tags->v[i]);
            continue;
        }
        char *norm = xstrdup(p);
        size_t n = strlen(norm);
        bool is_dir = n && norm[n - 1] == '/';
        if (is_dir) norm[--n] = 0;
        if (!n) {
            free(norm);
            continue;
        }
        strvec_t segs = str_split(norm, "/");
        size_t ndirs = is_dir ? segs.n : segs.n - 1;
        pdir_t *d = root;
        for (size_t k = 0; k < ndirs; k++) {
            pdir_t *next = NULL;
            for (size_t j = 0; j < d->dirs.n && !next; j++)
                if (str_eq(d->dirs.v[j]->name, segs.v[k])) next = d->dirs.v[j];
            if (!next) {
                next = pdir_new(segs.v[k]);
                vec_push(&d->dirs, next);
            }
            d = next;
        }
        if (!is_dir && !strvec_contains(&d->files, segs.v[segs.n - 1])) {
            strvec_push(&d->files, segs.v[segs.n - 1]);
            strvec_push(&d->tags, tags->v[i]);
        }
        strvec_free(&segs);
        free(norm);
    }
    sb_t out = SB_INIT;
    pdir_emit(root, 0, &out);
    pdir_free(root);
    return sb_detach(&out);
}

/* drop earlier <files> / <read-files> / <modified-files> sections, trimEnd */
static char *strip_file_sections(const char *s) {
    static const char *const TAGS[] = {"files", "read-files", "modified-files"};
    char *cur = xstrdup(s ? s : "");
    for (size_t t = 0; t < 3; t++) {
        char *open = xasprintf("<%s>", TAGS[t]), *close = xasprintf("</%s>", TAGS[t]);
        sb_t b = SB_INIT;
        const char *p = cur;
        for (;;) {
            const char *a = strstr(p, open);
            const char *z = a ? strstr(a + strlen(open), close) : NULL;
            if (!a || !z) {
                sb_append(&b, p);
                break;
            }
            sb_appendn(&b, p, (size_t)(a - p));
            p = z + strlen(close);
            while (*p && isspace((unsigned char)*p)) p++;
        }
        free(cur);
        cur = sb_detach(&b);
        free(open), free(close);
    }
    size_t n = strlen(cur);
    while (n && isspace((unsigned char)cur[n - 1])) cur[--n] = 0;
    return cur;
}

/* upsertFileOperations */
static char *upsert_file_ops(const char *summary, const json_t *read_files, const json_t *mod_files, const json_t *read_set) {
    char *base = strip_file_sections(summary);
    if (!json_len(read_files) && !json_len(mod_files)) return base;
    /* path -> Read / RW / Write, keys sorted */
    strvec_t keys = {0}, tags = {0};
    const json_t *x;
    json_arr_foreach(read_files, i, x) {
        strvec_push(&keys, json_sv(x));
        strvec_push(&tags, " (Read)");
    }
    json_arr_foreach(mod_files, i, x) {
        const char *tag = set_has(read_set, json_sv(x)) ? " (RW)" : " (Write)";
        ssize_t at = -1;
        for (size_t k = 0; k < keys.n; k++)
            if (str_eq(keys.v[k], json_sv(x))) at = (ssize_t)k;
        if (at >= 0) {
            free(tags.v[at]);
            tags.v[at] = xstrdup(tag);
        } else {
            strvec_push(&keys, json_sv(x));
            strvec_push(&tags, tag);
        }
    }
    /* sort keys together with their tags */
    for (size_t i = 1; i < keys.n; i++)
        for (size_t k = i; k > 0 && strcmp(keys.v[k], keys.v[k - 1]) < 0; k--) {
            char *t1 = keys.v[k];
            keys.v[k] = keys.v[k - 1];
            keys.v[k - 1] = t1;
            char *t2 = tags.v[k];
            tags.v[k] = tags.v[k - 1];
            tags.v[k - 1] = t2;
        }
    size_t total = keys.n;
    while (keys.n > FILE_LIST_LIMIT) free(keys.v[--keys.n]), free(tags.v[--tags.n]);
    char *tree = path_tree(&keys, &tags);
    char *files = total > FILE_LIST_LIMIT ? xasprintf("%s\n[\xe2\x80\xa6%zu files elided\xe2\x80\xa6]", tree, total - FILE_LIST_LIMIT)
                                          : xstrdup(tree);
    json_t *ctx = json_obj();
    json_set_str(ctx, "files", files);
    char *section = prompt_render("agent/compaction/prompts/file-operations.md", ctx);
    json_free(ctx);
    char *out;
    if (!section || !*section) out = xstrdup(base);
    else if (!*base) out = xstrdup(section);
    else out = xasprintf("%s\n\n%s", base, section);
    free(section), free(files), free(tree), free(base);
    strvec_free(&keys);
    strvec_free(&tags);
    return out;
}

/* ---------- entries -> messages ---------- */

json_t *branchsum_collect(const session_t *s, const char *old_leaf, const char *target, char **common) {
    if (common) *common = NULL;
    json_t *out = json_arr();
    if (!old_leaf) return out;
    json_t *old_path = session_path_to(s, old_leaf), *target_path = session_path_to(s, target);
    const char *anc = NULL;
    for (size_t i = json_len(target_path); i-- > 0 && !anc;) {
        const char *id = json_get_str(json_at(target_path, i), "id");
        const json_t *e;
        json_arr_foreach(old_path, k, e) if (str_eq(json_get_str(e, "id"), id)) anc = id;
    }
    /* old_path is root..old_leaf: everything after the common ancestor */
    bool after = anc == NULL;
    const json_t *e;
    json_arr_foreach(old_path, i, e) {
        if (after) json_push(out, json_clone(e));
        else if (str_eq(json_get_str(e, "id"), anc)) after = true;
    }
    if (common && anc) *common = xstrdup(anc);
    json_free(old_path);
    json_free(target_path);
    return out;
}

static json_t *text_user(const char *text) {
    json_t *m = json_obj(), *c = json_arr(), *b = json_obj();
    json_set_str(m, "role", "user");
    json_set_str(b, "type", "text");
    json_set_str(b, "text", text);
    json_push(c, b);
    json_set(m, "content", c);
    return m;
}

/* getMessageFromEntry + convertToLlm for the summarizer: NULL when the entry adds nothing */
static json_t *entry_message(const json_t *e, bool *is_summary) {
    const char *t = json_get_str(e, "type");
    *is_summary = false;
    if (str_eq(t, "message")) {
        const json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "toolResult") && json_get_bool(m, "useless", false) && !json_get_bool(m, "isError", false)) return NULL;
        return json_clone(m);
    }
    json_t *ctx = json_obj();
    char *text = NULL;
    if (str_eq(t, "branch_summary")) {
        json_set_str(ctx, "summary", json_get_str(e, "summary") ? json_get_str(e, "summary") : "");
        text = prompt_render("agent/compaction/prompts/branch-summary-context.md", ctx);
    } else if (str_eq(t, "compaction")) {
        json_set_str(ctx, "summary", json_get_str(e, "summary") ? json_get_str(e, "summary") : "");
        text = prompt_render("agent/compaction/prompts/compaction-summary-context.md", ctx);
    }
    json_free(ctx);
    if (!text) return NULL; /* custom messages become developer turns, which the summary transcript drops */
    *is_summary = true;
    json_t *m = text_user(text);
    free(text);
    return m;
}

static int64_t estimate_tokens(const json_t *msg) {
    const char *role = json_get_str(msg, "role");
    char *s;
    if (str_eq(role, "toolResult")) {
        sb_t b = SB_INIT;
        const json_t *blk;
        json_arr_foreach(json_get_arr(msg, "content"), i, blk) if (str_eq(json_get_str(blk, "type"), "text"))
            sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        if (!b.len) return 0;
        size_t n = MIN(b.len, (size_t)TOOL_RESULT_MAX_CHARS + 48);
        sb_free(&b);
        return (int64_t)n / 4 + 4;
    }
    s = json_dump(json_get(msg, "content"), 0);
    int64_t n = (int64_t)strlen(s) / 4 + 4;
    free(s);
    return n;
}

/* truncateToolResultForSummary */
static char *truncate_result(const char *s) {
    size_t cps = 0, cut = 0, len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xc0) == 0x80) continue;
        if (cps == TOOL_RESULT_MAX_CHARS) cut = i;
        cps++;
    }
    if (cps <= TOOL_RESULT_MAX_CHARS) return xstrdup(s);
    return xasprintf("%.*s\n\n[... %zu more characters truncated]", (int)cut, s, cps - TOOL_RESULT_MAX_CHARS);
}


static void xml_attr(sb_t *b, const char *s) {
    for (; *s; s++) {
        if (*s == '&') sb_append(b, "&amp;");
        else if (*s == '"') sb_append(b, "&quot;");
        else if (*s == '<') sb_append(b, "&lt;");
        else if (*s == '>') sb_append(b, "&gt;");
        else sb_putc(b, *s);
    }
}

static void xml_text(sb_t *b, const char *s) {
    for (; *s; s++) {
        if (*s == '&') sb_append(b, "&amp;");
        else if (*s == '<') sb_append(b, "&lt;");
        else if (*s == '>') sb_append(b, "&gt;");
        else sb_putc(b, *s);
    }
}

/* renderInvoke without declared arg shapes: every value as JSON */
static void render_invoke(sb_t *b, const json_t *call) {
    sb_append(b, "<invoke name=\"");
    xml_attr(b, json_get_str(call, "name") ? json_get_str(call, "name") : "");
    sb_append(b, "\">");
    const char *key;
    const json_t *v;
    json_obj_foreach(json_get_obj(call, "arguments"), i, key, v) {
        char *js = json_dump(v, 0);
        sb_append(b, "<parameter name=\"");
        xml_attr(b, key);
        sb_appendf(b, "\">%s</parameter>", js ? js : "null");
        free(js);
    }
    sb_append(b, "</invoke>");
}

/* messageContentText: text blocks, images as [Image: mime] */
static void content_text(const json_t *content, sb_t *b) {
    if (json_is_str(content)) {
        sb_append(b, json_sv(content));
        return;
    }
    const json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *t = json_get_str(blk, "type");
        if (str_eq(t, "text") && json_get_str(blk, "text")) sb_append(b, json_get_str(blk, "text"));
        else if (str_eq(t, "image")) {
            const char *mime = json_get_str(blk, "mimeType");
            if (mime && *mime) sb_appendf(b, "[Image: %s]", mime);
            else sb_append(b, "[Image]");
        }
    }
}

const char *branchsum_dialect(const char *model_class, const char *model_id) {
    const char *c = model_class;
    if (!c || !*c) {
        char *low = xstrdup(model_id ? model_id : "");
        for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
        bool claude = strstr(low, "claude") != NULL;
        free(low);
        return claude ? "anthropic" : "xml";
    }
    return str_eq(c, "anthropic") ? "anthropic" : "xml";
}

/* serializeConversation with a dialect: the legacy text transcript (`Human:` / `Assistant:` segments) */
char *branchsum_serialize(const json_t *messages, const char *dialect) {
    bool anthropic = str_eq(dialect, "anthropic");
    /* useless, non-error tool results are dropped along with their calls */
    strvec_t useless = {0};
    const json_t *m;
    json_arr_foreach(messages, i, m) {
        if (str_eq(json_get_str(m, "role"), "toolResult") && json_get_bool(m, "useless", false) &&
            !json_get_bool(m, "isError", false) && json_get_str(m, "toolCallId"))
            strvec_push(&useless, json_get_str(m, "toolCallId"));
    }
    sb_t out = SB_INIT;
    size_t n = json_len(messages);
    for (size_t i = 0; i < n;) {
        m = json_at(messages, i);
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "assistant")) {
            sb_t text = SB_INIT, think = SB_INIT, calls = SB_INIT;
            const json_t *blk;
            size_t ncalls = 0, kept = 0;
            bool any_think = false;
            json_arr_foreach(json_get_arr(m, "content"), k, blk) {
                const char *t = json_get_str(blk, "type");
                if (!(str_eq(t, "toolCall") && json_get_str(blk, "id") && strvec_contains(&useless, json_get_str(blk, "id"))))
                    kept++;
                if (str_eq(t, "text")) sb_append(&text, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
                else if (str_eq(t, "thinking")) {
                    if (any_think) sb_putc(&think, '\n');
                    sb_append(&think, json_get_str(blk, "thinking") ? json_get_str(blk, "thinking") : "");
                    any_think = true;
                } else if (str_eq(t, "toolCall")) {
                    if (json_get_str(blk, "id") && strvec_contains(&useless, json_get_str(blk, "id"))) continue;
                    if (ncalls++) sb_putc(&calls, '\n');
                    render_invoke(&calls, blk);
                }
            }
            if (kept) { /* an assistant left with no blocks is dropped */
                sb_append(&out, "\n\nAssistant: ");
                if (think.len) sb_appendf(&out, "<thinking>\n%s\n</thinking>", sb_cstr(&think));
                sb_append(&out, sb_cstr(&text));
                if (ncalls) {
                    if (anthropic) sb_appendf(&out, "<function_calls>\n%s\n</function_calls>", sb_cstr(&calls));
                    else sb_append(&out, sb_cstr(&calls));
                }
            }
            sb_free(&text), sb_free(&think), sb_free(&calls);
            i++;
            continue;
        }
        if (str_eq(role, "toolResult")) {
            /* a run of results: one Human segment */
            sb_t body = SB_INIT;
            size_t k = 0;
            for (; i < n && str_eq(json_get_str(json_at(messages, i), "role"), "toolResult"); i++) {
                const json_t *r = json_at(messages, i);
                if (json_get_str(r, "toolCallId") && strvec_contains(&useless, json_get_str(r, "toolCallId"))) continue;
                sb_t t = SB_INIT;
                const json_t *blk;
                json_arr_foreach(json_get_arr(r, "content"), x, blk) if (str_eq(json_get_str(blk, "type"), "text"))
                    sb_append(&t, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
                if (!t.len) {
                    sb_free(&t);
                    continue;
                }
                char *tr = truncate_result(sb_cstr(&t));
                sb_free(&t);
                if (k++) sb_putc(&body, '\n');
                if (anthropic) {
                    bool err = json_get_bool(r, "isError", false);
                    sb_appendf(&body, "<%s>\n<tool_name>", err ? "error" : "result");
                    xml_text(&body, json_get_str(r, "toolName") ? json_get_str(r, "toolName") : "");
                    sb_appendf(&body, "</tool_name>\n<%s>%s</%s>\n</%s>", err ? "stderr" : "stdout", tr, err ? "stderr" : "stdout",
                               err ? "error" : "result");
                } else sb_appendf(&body, "<tool_response>\n%s\n</tool_response>", tr);
                free(tr);
            }
            if (k) {
                if (anthropic) sb_appendf(&out, "\n\nHuman: <function_results>\n%s\n</function_results>", sb_cstr(&body));
                else sb_appendf(&out, "\n\nHuman: %s", sb_cstr(&body));
            }
            sb_free(&body);
            continue;
        }
        sb_t text = SB_INIT;
        content_text(json_get(m, "content"), &text);
        if (str_eq(role, "developer")) {
            if (text.len) sb_appendf(&out, "%s%s", out.len ? "\n\n" : "", sb_cstr(&text));
        } else sb_appendf(&out, "\n\nHuman: %s", sb_cstr(&text));
        sb_free(&text);
        i++;
    }
    strvec_free(&useless);
    /* a closing/opening conversation or previous-summary tag in the content must not end the envelope */
    char *raw = sb_detach(&out);
    sb_t esc = SB_INIT;
    for (const char *p = raw; *p;) {
        if (*p == '<') {
            const char *q = p + 1;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == '/') q++;
            while (*q && isspace((unsigned char)*q)) q++;
            size_t tl = !strncasecmp(q, "conversation", 12) ? 12 : !strncasecmp(q, "previous-summary", 16) ? 16 : 0;
            if (tl) {
                const char *r = q + tl;
                while (*r && isspace((unsigned char)*r)) r++;
                if (*r == '>') {
                    sb_append(&esc, "&lt;");
                    sb_appendn(&esc, p + 1, (size_t)(r + 1 - (p + 1)));
                    p = r + 1;
                    continue;
                }
            }
        }
        sb_putc(&esc, *p++);
    }
    free(raw);
    return sb_detach(&esc);
}

char *branchsum_prompt(const json_t *entries, int64_t budget, const char *custom, const char *dialect, json_t **fileops) {
    json_t *ops = json_obj();
    json_set(ops, "read", json_arr());
    json_set(ops, "written", json_arr());
    json_set(ops, "edited", json_arr());
    *fileops = ops;
    const json_t *e;
    /* cumulative file tracking from earlier (non-extension) branch summaries */
    json_arr_foreach(entries, i, e) {
        if (!str_eq(json_get_str(e, "type"), "branch_summary") || json_get_bool(e, "fromExtension", false)) continue;
        const json_t *d = json_get(e, "details"), *x;
        json_arr_foreach(json_get_arr(d, "readFiles"), k, x) if (json_is_str(x)) {
            char *p = strip_read_selector(json_sv(x));
            set_add(json_get(ops, "read"), p);
            free(p);
        }
        json_arr_foreach(json_get_arr(d, "modifiedFiles"), k, x) if (json_is_str(x)) set_add(json_get(ops, "edited"), json_sv(x));
    }
    /* newest to oldest until the token budget */
    json_t *rev = json_arr();
    int64_t total = 0;
    for (size_t i = json_len(entries); i-- > 0;) {
        e = json_at(entries, i);
        bool is_summary;
        json_t *m = entry_message(e, &is_summary);
        if (!m) continue;
        extract_file_ops(m, ops);
        int64_t tok = estimate_tokens(m);
        if (budget > 0 && total + tok > budget) {
            if (is_summary && (double)total < (double)budget * 0.9) {
                json_push(rev, m);
                total += tok;
            } else json_free(m);
            break;
        }
        json_push(rev, m);
        total += tok;
    }
    if (!json_len(rev)) {
        json_free(rev);
        return NULL;
    }
    json_t *msgs = json_arr();
    for (size_t i = json_len(rev); i-- > 0;) json_push(msgs, json_clone(json_at(rev, i)));
    json_free(rev);
    char *conv = branchsum_serialize(msgs, dialect);
    json_free(msgs);
    char *instructions = custom && *custom ? xstrdup(custom) : prompt_render("agent/compaction/prompts/branch-summary.md", NULL);
    char *out = xasprintf("<conversation>\n%s\n</conversation>\n\n%s", conv, instructions ? instructions : "");
    free(conv);
    free(instructions);
    return out;
}

char *branchsum_finish(const char *model_text, const json_t *fileops, json_t **read_files, json_t **modified_files) {
    char *pre = prompt_render("agent/compaction/prompts/branch-summary-preamble.md", NULL);
    char *summary = xasprintf("%s%s", pre ? pre : "", model_text ? model_text : "");
    free(pre);
    file_lists(fileops, read_files, modified_files);
    char *out = upsert_file_ops(summary, *read_files, *modified_files, json_get(fileops, "read"));
    free(summary);
    if (!*out) {
        free(out);
        out = xstrdup("No summary generated");
    }
    return out;
}
