/* tree.c - the /tree session tree selector (port of omp overlays/tree-selector.ts TreeSelectorComponent
 * over components/tree-view.ts TreeView, as shipped in omp 18.14.2).
 *
 * A "Session Tree" panel in the composer's place: key help, the "Search:" input, a divider, then the
 * session's entries as a tree. Real branch points draw ├─ / └─ connectors with │ gutters, linear chains
 * keep their branch head's depth, the current branch is listed first, entries on the active path carry
 * an accent bullet and labels a warning `[label]` badge. The list is a selection-centred window of
 * max(5, rows/2) rows (capped at rows - 8) with a scrollbar column when it overflows.
 *
 * Filters (treeFilterMode, Ctrl+O cycles, Alt+D/T/U/L/A pick): default hides settings/bookkeeping
 * entries, no-tools also tool results, user-only keeps user requests, labeled-only labelled entries,
 * all everything. Assistant messages without text are always hidden (unless errored/aborted or the
 * current leaf). Typing fuzzy-filters by entry text; Esc clears the search first. Shift+L edits the
 * selected entry's label, Enter switches, Shift+Enter summarizes & switches. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "core/fs.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <math.h>

#define SEARCH_TEXT_LIMIT 200
#define NONE ((size_t)-1)

static const char *const FILTER_MODES[] = {"default", "no-tools", "user-only", "labeled-only", "all"};
enum { F_DEFAULT, F_NO_TOOLS, F_USER, F_LABELED, F_ALL, F__N };

typedef struct {
    const json_t *e;
    const char *id;
    char *label;  /* owned, NULL = none */
    size_t parent; /* NONE for roots */
    VEC(size_t) kids;
    bool active; /* the current leaf or one of its ancestors */
    char *search; /* lazily built search text (owned) */
    char *search_label; /* label the search text was built with */
} tnode_t;

typedef struct {
    size_t key;
    int depth;
    bool is_last;
    int sib_count;
    ssize_t parent; /* chain index */
} tanc_t;

typedef struct {
    size_t node;
    size_t parent_node; /* NONE for roots */
    int depth;
    int sib_index, sib_count;
    bool is_last;
    ssize_t chain; /* ancestor chain (tanc_t index), -1 = none */
} trow_t;

typedef struct tree_sel {
    tui_component_t base;
    VEC(tnode_t) nodes;
    VEC(size_t) roots; /* ordered: the branch containing the leaf first */
    hmap_t by_id;      /* id -> node index + 1 */
    json_t *tool_calls; /* toolCallId -> {name, arguments} */
    size_t leaf;       /* NONE when no leaf */
    bool multiple_roots;
    /* projection */
    VEC(trow_t) rows;
    VEC(tanc_t) chains;
    size_t *row_of; /* node -> row index */
    VEC(size_t) vis; /* visible row indices */
    ssize_t *vis_of; /* node -> visible index, -1 hidden */
    size_t sel, anchor; /* node indices, NONE */
    int filter;
    int max_rows;
    ui_input_t search;
    /* label editor */
    bool labeling;
    size_t label_node;
    ui_input_t label_in;
    ui_tree_cb_t cb;
    tui_tc_t assistant_tc;
} tree_sel_t;

/* ---------- text helpers ---------- */

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

/* plainText(value with line breaks folded): strip ANSI and control characters */
static char *row_text(const char *v) {
    char *plain = tui_strip_ansi(v ? v : "");
    sb_t b = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)plain; *p; p++) {
        if (*p == '\r' || *p == '\n') sb_putc(&b, ' ');
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
            sb_putc(&b, ' ');
            p += 2;
        } else if (*p == '\t') sb_putc(&b, '\t');
        else if (*p < 0x20 || *p == 0x7f) continue;
        else sb_putc(&b, (char)*p);
    }
    free(plain);
    return sb_detach(&b);
}

/* s.replace(/[\n\t]/g, " ").trim() */
static char *normalize(const char *s) {
    char *t = xstrdup(s ? s : "");
    for (char *p = t; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    char *r = str_trim(t);
    free(t);
    return r;
}

/* sanitizeTreeField: strip control characters, fold whitespace breaks, trim */
static char *field(const char *s) {
    char *t = row_text(s);
    for (char *p = t; *p; p++)
        if (*p == '\t') *p = ' ';
    char *r = str_trim(t);
    free(t);
    return r;
}

/* first `limit` codepoints */
static void cap_cps(sb_t *b, size_t start, size_t limit) {
    size_t cps = 0;
    for (size_t i = start; i < b->len; i++) {
        if (((unsigned char)b->s[i] & 0xc0) == 0x80) continue;
        if (cps++ == limit) {
            b->s[i] = 0;
            b->len = i;
            return;
        }
    }
}

/* joinTextContent: concatenated text blocks (string content as is), capped at `limit` codepoints */
static char *join_text(const json_t *content, size_t limit) {
    sb_t b = SB_INIT;
    if (json_is_str(content)) sb_append(&b, json_sv(content));
    else {
        const json_t *blk;
        json_arr_foreach(content, i, blk) {
            if (!str_eq(json_get_str(blk, "type"), "text") || !json_get_str(blk, "text")) continue;
            sb_append(&b, json_get_str(blk, "text"));
            if (limit && b.len >= limit) {
                cap_cps(&b, 0, limit);
                if (b.len >= limit) break;
            }
        }
    }
    if (limit) cap_cps(&b, 0, limit);
    return sb_detach(&b);
}

static bool canonical_nonblank(const char *t) {
    for (const unsigned char *p = (const unsigned char *)(t ? t : ""); *p; p++) {
        if (*p == '.' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') continue;
        if (p[0] == 0xe2 && p[1] == 0x80 && p[2] == 0xa6) { /* … */
            p += 2;
            continue;
        }
        return true;
    }
    return false;
}

static bool has_text_content(const json_t *content) {
    if (json_is_str(content)) return canonical_nonblank(json_sv(content));
    const json_t *blk;
    json_arr_foreach(content, i, blk) {
        if (str_eq(json_get_str(blk, "type"), "text") && canonical_nonblank(json_get_str(blk, "text"))) return true;
    }
    return false;
}

/* stripSystemWrapperTags: one `<system-*>` envelope around custom-message content */
static char *strip_system_wrapper(const char *content) {
    char *trimmed = str_trim(content ? content : "");
    size_t n = strlen(trimmed);
    if (strncasecmp(trimmed, "<system-", 8)) goto keep;
    size_t name_end = 8;
    while (name_end < n && (isalnum((unsigned char)trimmed[name_end]) || trimmed[name_end] == '_' || trimmed[name_end] == '-'))
        name_end++;
    if (name_end == 8) goto keep;
    if (trimmed[name_end] != '>' && !isspace((unsigned char)trimmed[name_end])) goto keep;
    char quote = 0;
    ssize_t open_end = -1;
    for (size_t i = name_end; i < n; i++) {
        char c = trimmed[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') quote = c;
        else if (c == '<') goto keep;
        else if (c == '>') {
            open_end = (ssize_t)i;
            break;
        }
    }
    if (open_end < 0 || quote) goto keep;
    char *close = xasprintf("</%.*s>", (int)(name_end - 1), trimmed + 1);
    size_t cl = strlen(close);
    if (n < cl || n - cl <= (size_t)open_end || strcasecmp(trimmed + n - cl, close)) {
        free(close);
        goto keep;
    }
    char *inner = xstrndup(trimmed + open_end + 1, n - cl - (size_t)open_end - 1);
    char *r = str_trim(inner);
    free(inner);
    free(close);
    free(trimmed);
    return r;
keep:
    free(trimmed);
    return xstrdup(content ? content : "");
}

static char *custom_content_text(const json_t *content) { return join_text(content, 0); }

/* advisor note metadata: non-default advisor names then severities, and the joined note bodies */
static void advisor_display(const json_t *details, char **qualifier, char **text) {
    strvec_t names = {0}, sev = {0};
    sb_t q = SB_INIT, t = SB_INIT;
    const json_t *note;
    json_arr_foreach(json_get_arr(details, "notes"), i, note) {
        if (json_get_str(note, "note")) {
            if (t.len) sb_putc(&t, ' ');
            sb_append(&t, json_get_str(note, "note"));
        }
        if (json_get_str(note, "advisor")) {
            char *nm = field(json_get_str(note, "advisor"));
            if (*nm && strcmp(nm, "default") && !strvec_contains(&names, nm)) strvec_push(&names, nm);
            free(nm);
        }
        if (json_get_str(note, "severity")) {
            char *sv = field(json_get_str(note, "severity"));
            if (*sv && !strvec_contains(&sev, sv)) strvec_push(&sev, sv);
            free(sv);
        }
    }
    for (size_t i = 0; i < names.n + sev.n; i++) {
        if (q.len) sb_append(&q, ", ");
        sb_append(&q, i < names.n ? names.v[i] : sev.v[i - names.n]);
    }
    strvec_free(&names);
    strvec_free(&sev);
    *qualifier = sb_detach(&q);
    *text = sb_detach(&t);
}

/* ---------- user requests (transcript-entry.ts isUserRequestEntry) ---------- */

/* isUserTurnInitiator: a user-invoked /skill: prompt or a collab peer's prompt */
static bool is_user_initiator(const json_t *m) {
    const char *ct = json_get_str(m, "customType");
    return (str_eq(ct, "skill-prompt") || str_eq(ct, "collab-prompt")) && str_eq(json_get_str(m, "attribution"), "user");
}

static bool is_user_request(const json_t *e) {
    const char *t = json_get_str(e, "type");
    if (str_eq(t, "message")) {
        const json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) return true;
        return str_eq(role, "custom") && is_user_initiator(m);
    }
    if (str_eq(t, "custom_message")) return is_user_initiator(e);
    return false;
}

/* ---------- display parts ---------- */

typedef struct {
    char *t;
    int s; /* theme slot, -1 none */
    bool strong;
} part_t;

typedef VEC(part_t) parts_t;

static void part(parts_t *p, char *t, int s, bool strong) {
    part_t x = {t, s, strong};
    vec_push(p, x);
}

static void parts_free(parts_t *p) {
    for (size_t i = 0; i < p->n; i++) free(p->v[i].t);
    vec_free(p);
}

/* shortenPath: a leading home directory becomes ~ */
static char *shorten(const char *p) {
    const char *home = home_dir();
    size_t hl = home ? strlen(home) : 0;
    if (hl && !strncmp(p, home, hl) && (p[hl] == '/' || !p[hl])) return xasprintf("~%s", p + hl);
    return xstrdup(p);
}

static const char *arg_str(const json_t *args, const char *a, const char *b) {
    const char *v = json_get_str(args, a);
    if (v && *v) return v;
    v = b ? json_get_str(args, b) : NULL;
    return v && *v ? v : NULL;
}

/* toPathList(paths|path) joined with ", " (string-encoded JSON arrays expand), "." when empty */
static char *path_scope(const json_t *input) {
    sb_t b = SB_INIT;
    json_t *parsed = NULL;
    if (json_is_str(input)) {
        char *tr = str_trim(json_sv(input));
        size_t n = strlen(tr);
        if (n >= 2 && tr[0] == '[' && tr[n - 1] == ']') {
            parsed = json_parse_str(tr);
            const json_t *x;
            bool ok = parsed && json_is_arr(parsed);
            json_arr_foreach(parsed, i, x) if (!json_is_str(x)) ok = false;
            if (!ok) json_free(parsed), parsed = NULL;
        }
        free(tr);
        if (!parsed) sb_append(&b, json_sv(input));
    }
    const json_t *arr = parsed ? parsed : json_is_arr(input) ? input : NULL;
    const json_t *x;
    json_arr_foreach(arr, i, x) {
        if (b.len) sb_append(&b, ", ");
        sb_append(&b, json_is_str(x) ? json_sv(x) : "");
    }
    json_free(parsed);
    if (!b.len) sb_append(&b, ".");
    char *r = shorten(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* first `n` codepoints of s, plus whether it was longer (JS string slice for these short caps) */
static char *slice_cps(const char *s, size_t n, bool *longer) {
    size_t cps = 0, i = 0, len = strlen(s);
    for (; i < len; i++) {
        if (((unsigned char)s[i] & 0xc0) == 0x80) continue;
        if (cps++ == n) break;
    }
    if (longer) *longer = i < len;
    return xstrndup(s, i);
}

static char *format_tool_call(const char *name, const json_t *args) {
    if (str_eq(name, "read")) {
        const char *p = arg_str(args, "path", "file_path");
        char *path = shorten(p ? p : "");
        const json_t *off = json_get(args, "offset"), *lim = json_get(args, "limit");
        char *r;
        if (off || lim) {
            int64_t start = off ? json_get_int(args, "offset", 1) : 1;
            if (lim) r = xasprintf("[read: %s:%lld-%lld]", path, (long long)start, (long long)(start + json_get_int(args, "limit", 0) - 1));
            else r = xasprintf("[read: %s:%lld]", path, (long long)start);
        } else r = xasprintf("[read: %s]", path);
        free(path);
        return r;
    }
    if (str_eq(name, "write") || str_eq(name, "edit")) {
        const char *p = arg_str(args, "path", "file_path");
        char *path = shorten(p ? p : "");
        char *r = xasprintf("[%s: %s]", name, path);
        free(path);
        return r;
    }
    if (str_eq(name, "bash")) {
        const char *raw = json_get_str(args, "command") ? json_get_str(args, "command") : "";
        char *n = normalize(raw);
        char *cmd = slice_cps(n, 50, NULL);
        bool longer;
        free(slice_cps(raw, 50, &longer));
        char *r = xasprintf("[bash: %s%s]", cmd, longer ? "..." : "");
        free(n), free(cmd);
        return r;
    }
    if (str_eq(name, "grep")) {
        const json_t *paths = json_get(args, "paths");
        const json_t *in = json_is_str(paths) || json_is_arr(paths) ? paths : json_is_str(json_get(args, "path")) ? json_get(args, "path") : NULL;
        char *scope = path_scope(in);
        char *r = xasprintf("[grep: /%s/ in %s]", json_get_str(args, "pattern") ? json_get_str(args, "pattern") : "", scope);
        free(scope);
        return r;
    }
    if (str_eq(name, "glob")) {
        const json_t *path = json_get(args, "path"), *paths = json_get(args, "paths");
        const json_t *in = json_is_str(path) ? path : json_is_str(paths) || json_is_arr(paths) ? paths : NULL;
        char *scope = path_scope(in);
        char *r = xasprintf("[glob: %s]", scope);
        free(scope);
        return r;
    }
    if (str_eq(name, "ls")) {
        const char *p = arg_str(args, "path", NULL);
        char *path = shorten(p ? p : ".");
        char *r = xasprintf("[ls: %s]", path);
        free(path);
        return r;
    }
    char *js = json_dump(args ? args : json_obj(), 0);
    bool longer;
    char *head = slice_cps(js, 40, &longer);
    char *r = xasprintf("[%s: %s%s]", name ? name : "", head, longer ? "..." : "");
    free(head), free(js);
    return r;
}

static void entry_parts(tree_sel_t *t, const tnode_t *nd, parts_t *out) {
    const json_t *e = nd->e;
    const char *type = json_get_str(e, "type");
    if (str_eq(type, "message")) {
        const json_t *m = json_get(e, "message");
        const char *role = json_get_str(m, "role");
        if (str_eq(role, "user")) {
            char *c = join_text(json_get(m, "content"), SEARCH_TEXT_LIMIT), *n = normalize(c);
            free(c);
            part(out, xstrdup("user: "), TUI_TC_ACCENT, true);
            part(out, n, TUI_TC_ACCENT, true);
            return;
        }
        if (str_eq(role, "developer")) {
            char *c = join_text(json_get(m, "content"), SEARCH_TEXT_LIMIT), *n = normalize(c);
            free(c);
            part(out, xstrdup("developer: "), TUI_TC_DIM, false);
            part(out, n, TUI_TC_MUTED, false);
            return;
        }
        if (str_eq(role, "assistant")) {
            part(out, xstrdup("assistant: "), (int)t->assistant_tc, false);
            const json_t *rr = json_get(m, "retryRecovery");
            if (str_eq(json_get_str(rr, "status"), "recovered")) {
                char *note = field(json_get_str(rr, "note") ? json_get_str(rr, "note") : "");
                part(out, note, TUI_TC_DIM, false);
                return;
            }
            char *c = join_text(json_get(m, "content"), SEARCH_TEXT_LIMIT), *n = normalize(c);
            free(c);
            if (*n) {
                part(out, n, -1, false);
                return;
            }
            free(n);
            const char *stop = json_get_str(m, "stopReason"), *err = json_get_str(m, "errorMessage");
            const char *full = NULL;
            if (str_eq(json_get_str(rr, "status"), "superseded")) full = NULL;
            else if (str_eq(stop, "error")) full = err && *err ? err : "Error";
            else if (str_eq(stop, "aborted")) full = err && *err && strcmp(err, "Request was aborted") ? err : NULL;
            if (full) {
                char *fn = normalize(full);
                char *cut = slice_cps(fn, 80, NULL);
                free(fn);
                part(out, cut, TUI_TC_ERROR, false);
            } else part(out, xstrdup(str_eq(stop, "aborted") ? "(aborted)" : "(no content)"), TUI_TC_MUTED, false);
            return;
        }
        if (str_eq(role, "toolResult")) {
            const char *cid = json_get_str(m, "toolCallId");
            const json_t *tc = cid ? json_get(t->tool_calls, cid) : NULL;
            if (tc) part(out, format_tool_call(json_get_str(tc, "name"), json_get(tc, "arguments")), TUI_TC_MUTED, false);
            else part(out, xasprintf("[%s]", json_get_str(m, "toolName") ? json_get_str(m, "toolName") : "tool"), TUI_TC_MUTED, false);
            return;
        }
        if (str_eq(role, "bashExecution")) {
            char *n = normalize(json_get_str(m, "command") ? json_get_str(m, "command") : "");
            part(out, xasprintf("[bash]: %s", n), TUI_TC_DIM, false);
            free(n);
            return;
        }
        part(out, xasprintf("[%s]", role ? role : ""), TUI_TC_DIM, false);
        return;
    }
    if (str_eq(type, "custom_message")) {
        const char *ct = json_get_str(e, "customType") ? json_get_str(e, "customType") : "";
        if (str_eq(ct, "advisor")) {
            char *q, *tx;
            advisor_display(json_get(e, "details"), &q, &tx);
            part(out, *q ? xasprintf("advisor (%s): ", q) : xstrdup("advisor: "), TUI_TC_CUSTOM_MESSAGE_LABEL, false);
            part(out, normalize(tx), TUI_TC_MUTED, false);
            free(q), free(tx);
            return;
        }
        char *raw = custom_content_text(json_get(e, "content")), *body = strip_system_wrapper(raw);
        part(out, xasprintf("[%s]: ", ct), TUI_TC_CUSTOM_MESSAGE_LABEL, false);
        part(out, normalize(body), TUI_TC_MUTED, false);
        free(raw), free(body);
        return;
    }
    if (str_eq(type, "compaction")) {
        double tb = (double)json_get_int(e, "tokensBefore", 0);
        part(out, xasprintf("[compaction: %lldk tokens]", (long long)llround(tb / 1000.0)), TUI_TC_BORDER_ACCENT, false);
        return;
    }
    if (str_eq(type, "branch_summary")) {
        part(out, xstrdup("[branch summary]: "), TUI_TC_WARNING, false);
        part(out, normalize(json_get_str(e, "summary")), -1, false);
        return;
    }
    char *s = NULL;
    if (str_eq(type, "model_change")) s = xasprintf("[model: %s]", json_get_str(e, "model") ? json_get_str(e, "model") : "");
    else if (str_eq(type, "model_usage")) {
        char *pu = field(json_get_str(e, "purpose")), *ro = field(json_get_str(e, "role")), *pr = field(json_get_str(e, "provider")),
             *mo = field(json_get_str(e, "model"));
        s = xasprintf("[model usage: %s %s%s%s/%s]", pu, ro, *ro ? " " : "", pr, mo);
        free(pu), free(ro), free(pr), free(mo);
    } else if (str_eq(type, "thinking_level_change"))
        s = xasprintf("[thinking: %s]", json_get_str(e, "thinkingLevel") ? json_get_str(e, "thinkingLevel") : "off");
    else if (str_eq(type, "custom"))
        s = xasprintf("[custom: %s]", json_get_str(e, "customType") ? json_get_str(e, "customType") : "");
    else if (str_eq(type, "label"))
        s = xasprintf("[label: %s]", json_get_str(e, "label") ? json_get_str(e, "label") : "(cleared)");
    else if (str_eq(type, "service_tier_change")) {
        const json_t *st = json_get_obj(e, "serviceTier");
        sb_t b = SB_INIT;
        const char *k;
        const json_t *v;
        json_obj_foreach(st, i, k, v) {
            if (b.len) sb_putc(&b, ' ');
            char *vs = json_is_str(v) ? xstrdup(json_sv(v)) : json_dump(v, 0);
            sb_appendf(&b, "%s:%s", k, vs);
            free(vs);
        }
        s = xasprintf("[service tier: %s]", st ? sb_cstr(&b) : "(default)");
        sb_free(&b);
    } else if (str_eq(type, "title_change")) {
        char *n = normalize(json_get_str(e, "title"));
        s = xasprintf("[title: %s]", n);
        free(n);
    } else if (str_eq(type, "mode_change")) s = xasprintf("[mode: %s]", json_get_str(e, "mode") ? json_get_str(e, "mode") : "");
    else if (str_eq(type, "credential_pin"))
        s = xasprintf("[credential pin: %s]", json_get_str(e, "provider") ? json_get_str(e, "provider") : "");
    else {
        char *ty = xstrdup(type ? type : "");
        for (char *p = ty; *p; p++)
            if (*p == '_') *p = ' ';
        s = xasprintf("[%s]", ty);
        free(ty);
    }
    part(out, s, TUI_TC_DIM, false);
}

/* searchable text of a node (label, role and content, type keywords) */
static const char *search_text(tree_sel_t *t, tnode_t *nd) {
    if (nd->search && str_eq(nd->search_label, nd->label)) return nd->search;
    free(nd->search);
    free(nd->search_label);
    strvec_t p = {0};
    if (nd->label) strvec_push(&p, nd->label);
    const json_t *e = nd->e;
    const char *type = json_get_str(e, "type");
    if (str_eq(type, "message")) {
        const json_t *m = json_get(e, "message");
        strvec_push(&p, json_get_str(m, "role") ? json_get_str(m, "role") : "");
        if (json_get(m, "content")) strvec_push_own(&p, join_text(json_get(m, "content"), SEARCH_TEXT_LIMIT));
        if (str_eq(json_get_str(m, "role"), "bashExecution") && json_get_str(m, "command")) strvec_push(&p, json_get_str(m, "command"));
    } else if (str_eq(type, "custom_message")) {
        const char *ct = json_get_str(e, "customType") ? json_get_str(e, "customType") : "";
        strvec_push(&p, ct);
        if (str_eq(ct, "advisor")) {
            char *q, *tx;
            advisor_display(json_get(e, "details"), &q, &tx);
            if (*q) strvec_push(&p, q);
            if (*tx) strvec_push(&p, tx);
            free(q), free(tx);
        } else {
            char *raw = custom_content_text(json_get(e, "content")), *body = strip_system_wrapper(raw);
            char *cut = slice_cps(body, SEARCH_TEXT_LIMIT, NULL);
            if (*cut) strvec_push(&p, cut);
            free(cut), free(raw), free(body);
        }
    } else if (str_eq(type, "compaction")) strvec_push(&p, "compaction");
    else if (str_eq(type, "branch_summary")) {
        strvec_push(&p, "branch summary");
        strvec_push(&p, json_get_str(e, "summary") ? json_get_str(e, "summary") : "");
    } else if (str_eq(type, "model_change")) {
        strvec_push(&p, "model");
        strvec_push(&p, json_get_str(e, "model") ? json_get_str(e, "model") : "");
    } else if (str_eq(type, "thinking_level_change")) {
        strvec_push(&p, "thinking");
        strvec_push(&p, json_get_str(e, "thinkingLevel") ? json_get_str(e, "thinkingLevel") : "off");
    } else if (str_eq(type, "custom")) {
        strvec_push(&p, "custom");
        strvec_push(&p, json_get_str(e, "customType") ? json_get_str(e, "customType") : "");
    } else if (str_eq(type, "label")) {
        strvec_push(&p, "label");
        strvec_push(&p, json_get_str(e, "label") ? json_get_str(e, "label") : "");
    } else if (type) {
        /* model usage, service tier, title, mode, credential pin, ttsr injection, reset boundary, session init */
        parts_t parts = {0};
        entry_parts(t, nd, &parts);
        for (size_t i = 0; i < parts.n; i++) strvec_push(&p, parts.v[i].t);
        parts_free(&parts);
    }
    sb_t b = SB_INIT;
    for (size_t i = 0; i < p.n; i++) {
        if (i) sb_putc(&b, ' ');
        sb_append(&b, p.v[i]);
    }
    strvec_free(&p);
    nd->search = sb_detach(&b);
    nd->search_label = nd->label ? xstrdup(nd->label) : NULL;
    return nd->search;
}

/* ---------- tree model ---------- */

static size_t node_of(const tree_sel_t *t, const char *id) {
    void *v = id ? hmap_get(&t->by_id, id) : NULL;
    return v ? (size_t)(uintptr_t)v - 1 : NONE;
}

static size_t nkids(const tree_sel_t *t, size_t n) { return n == NONE ? t->roots.n : t->nodes.v[n].kids.n; }
static size_t kid(const tree_sel_t *t, size_t n, size_t i) { return n == NONE ? t->roots.v[i] : t->nodes.v[n].kids.v[i]; }

/* children with the branch containing the active leaf first (stable) */
static void order_active_first(tree_sel_t *t, size_t *v, size_t n) {
    size_t *tmp = xmalloc((n ? n : 1) * sizeof *tmp);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (t->nodes.v[v[i]].active) tmp[k++] = v[i];
    for (size_t i = 0; i < n; i++)
        if (!t->nodes.v[v[i]].active) tmp[k++] = v[i];
    memcpy(v, tmp, n * sizeof *v);
    free(tmp);
}

static void build_model(tree_sel_t *t, const json_t *entries, const char *leaf_id) {
    hmap_init(&t->by_id);
    t->tool_calls = json_obj();
    const json_t *e;
    json_arr_foreach(entries, i, e) {
        const char *id = json_get_str(e, "id");
        if (!id || hmap_has(&t->by_id, id)) continue;
        tnode_t nd = {0};
        nd.e = e;
        nd.id = id;
        nd.parent = NONE;
        vec_push(&t->nodes, nd);
        hmap_put(&t->by_id, id, (void *)(uintptr_t)t->nodes.n);
    }
    for (size_t i = 0; i < t->nodes.n; i++) {
        tnode_t *nd = &t->nodes.v[i];
        size_t p = node_of(t, json_get_str(nd->e, "parentId"));
        if (p != NONE && p != i) {
            nd->parent = p;
            vec_push(&t->nodes.v[p].kids, i);
        } else vec_push(&t->roots, i);
        const json_t *e2 = nd->e;
        const char *type = json_get_str(e2, "type");
        if (str_eq(type, "label")) {
            size_t tg = node_of(t, json_get_str(e2, "targetId"));
            if (tg != NONE) {
                const char *l = json_get_str(e2, "label");
                free(t->nodes.v[tg].label);
                t->nodes.v[tg].label = l && *l ? xstrdup(l) : NULL;
            }
        } else if (str_eq(type, "message") && str_eq(json_get_str(json_get(e2, "message"), "role"), "assistant")) {
            const json_t *blk;
            json_arr_foreach(json_get_arr(json_get(e2, "message"), "content"), k, blk) {
                if (!str_eq(json_get_str(blk, "type"), "toolCall") || !json_get_str(blk, "id")) continue;
                json_t *tc = json_obj();
                json_set_str(tc, "name", json_get_str(blk, "name") ? json_get_str(blk, "name") : "");
                json_set(tc, "arguments", json_get(blk, "arguments") ? json_clone(json_get(blk, "arguments")) : json_obj());
                json_set(t->tool_calls, json_get_str(blk, "id"), tc);
            }
        }
    }
    t->leaf = node_of(t, leaf_id);
    for (size_t n = t->leaf, guard = 0; n != NONE && guard <= t->nodes.n; n = t->nodes.v[n].parent, guard++)
        t->nodes.v[n].active = true;
    t->multiple_roots = t->roots.n > 1;
    order_active_first(t, t->roots.v, t->roots.n);
    for (size_t i = 0; i < t->nodes.n; i++) order_active_first(t, t->nodes.v[i].kids.v, t->nodes.v[i].kids.n);
}

/* TreeView projectTree: iterative depth-first rows with ancestor chains */
static void project(tree_sel_t *t) {
    typedef struct {
        size_t node, parent;
        int depth, sib_index, sib_count;
        ssize_t chain;
    } pend_t;
    VEC(pend_t) stack = {0};
    for (size_t i = t->roots.n; i-- > 0;) {
        pend_t p = {t->roots.v[i], NONE, 0, (int)i, (int)t->roots.n, -1};
        vec_push(&stack, p);
    }
    t->row_of = xmalloc((t->nodes.n ? t->nodes.n : 1) * sizeof *t->row_of);
    for (size_t i = 0; i < t->nodes.n; i++) t->row_of[i] = NONE;
    while (stack.n) {
        pend_t p = stack.v[--stack.n];
        if (t->row_of[p.node] != NONE) continue; /* cycle guard */
        trow_t r = {p.node, p.parent, p.depth, p.sib_index, p.sib_count, p.sib_index == p.sib_count - 1, p.chain};
        t->row_of[p.node] = t->rows.n;
        vec_push(&t->rows, r);
        size_t nk = nkids(t, p.node);
        if (!nk) continue;
        int child_depth = nk > 1 || (t->multiple_roots && p.parent == NONE) ? p.depth + 1 : p.depth;
        ssize_t child_chain;
        if (child_depth == p.depth && p.sib_count == 1) child_chain = p.chain;
        else {
            tanc_t a = {p.node, p.depth, r.is_last, p.sib_count, p.chain};
            vec_push(&t->chains, a);
            child_chain = (ssize_t)t->chains.n - 1;
        }
        for (size_t i = nk; i-- > 0;) {
            pend_t c = {kid(t, p.node, i), p.node, child_depth, (int)i, (int)nk, child_chain};
            vec_push(&stack, c);
        }
    }
    vec_free(&stack);
    t->vis_of = xmalloc((t->nodes.n ? t->nodes.n : 1) * sizeof *t->vis_of);
}

static bool row_visible(tree_sel_t *t, size_t node, tui_fuzzy_query_t **tokens, size_t ntok) {
    tnode_t *nd = &t->nodes.v[node];
    const json_t *e = nd->e;
    const char *type = json_get_str(e, "type");
    if (str_eq(type, "message") && node != t->leaf) {
        const json_t *m = json_get(e, "message");
        if (str_eq(json_get_str(m, "role"), "assistant")) {
            const char *stop = json_get_str(m, "stopReason");
            bool err = stop && *stop && strcmp(stop, "stop") && strcmp(stop, "toolUse");
            if (!has_text_content(json_get(m, "content")) && !err) return false;
        }
    }
    static const char *const SETTINGS[] = {"label", "custom", "model_change", "model_usage", "thinking_level_change",
                                           "service_tier_change", "title_change", "credential_pin", "session_init",
                                           "ttsr_injection", "mode_change", "reset_boundary"};
    bool settings = false;
    for (size_t i = 0; i < ARRAY_LEN(SETTINGS); i++)
        if (str_eq(type, SETTINGS[i])) settings = true;
    bool pass;
    switch (t->filter) {
    case F_USER: pass = is_user_request(e); break;
    case F_NO_TOOLS:
        pass = !settings && !(str_eq(type, "message") && str_eq(json_get_str(json_get(e, "message"), "role"), "toolResult"));
        break;
    case F_LABELED: pass = nd->label != NULL; break;
    case F_ALL: pass = true; break;
    default: pass = !settings; break;
    }
    if (!pass) return false;
    if (ntok) {
        const char *txt = search_text(t, nd);
        for (size_t i = 0; i < ntok; i++)
            if (!tui_fuzzy_query_match(tokens[i], txt, NULL)) return false;
    }
    return true;
}

static size_t nearest_visible(tree_sel_t *t, size_t key) {
    if (!t->vis.n) return NONE;
    if (key == NONE) return t->rows.v[t->vis.v[0]].node;
    for (size_t c = key, guard = 0; c != NONE && guard <= t->nodes.n; guard++) {
        if (t->vis_of[c] >= 0) return c;
        size_t r = t->row_of[c];
        c = r == NONE ? NONE : t->rows.v[r].parent_node;
    }
    return t->rows.v[t->vis.v[t->vis.n - 1]].node;
}

static void apply_filter(tree_sel_t *t) {
    t->vis.n = 0;
    for (size_t i = 0; i < t->nodes.n; i++) t->vis_of[i] = -1;
    /* the query splits on whitespace, every token must fuzzy-match */
    char *q = xstrdup(ui_input_value(&t->search));
    for (char *p = q; *p; p++) *p = (char)tolower((unsigned char)*p);
    VEC(tui_fuzzy_query_t *) toks = {0};
    for (char *s = strtok(q, " \t\n"); s; s = strtok(NULL, " \t\n")) vec_push(&toks, tui_fuzzy_query_new(s));
    for (size_t r = 0; r < t->rows.n; r++) {
        if (!row_visible(t, t->rows.v[r].node, toks.v, toks.n)) continue;
        t->vis_of[t->rows.v[r].node] = (ssize_t)t->vis.n;
        vec_push(&t->vis, r);
    }
    for (size_t i = 0; i < toks.n; i++) tui_fuzzy_query_free(toks.v[i]);
    vec_free(&toks);
    free(q);
    t->sel = nearest_visible(t, t->anchor);
    if (t->sel != NONE) t->anchor = t->sel;
}

static void set_selected(tree_sel_t *t, size_t key) {
    size_t r = nearest_visible(t, key);
    t->sel = r;
    if (r != NONE) t->anchor = r;
    else if (key != NONE) t->anchor = key;
}

static ssize_t sel_index(const tree_sel_t *t) { return t->sel == NONE ? -1 : t->vis_of[t->sel]; }

static void set_index(tree_sel_t *t, ssize_t i) {
    if (!t->vis.n) {
        set_selected(t, NONE);
        return;
    }
    i = MAX(0, MIN(i, (ssize_t)t->vis.n - 1));
    set_selected(t, t->rows.v[t->vis.v[i]].node);
}

static void move_sel(tree_sel_t *t, ssize_t delta, bool wrap) {
    ssize_t n = (ssize_t)t->vis.n;
    if (!n) return;
    ssize_t next = MAX(0, sel_index(t)) + delta;
    if (wrap) next = ((next % n) + n) % n;
    set_index(t, next);
}

static bool is_turn(const tree_sel_t *t, size_t node) {
    const json_t *e = t->nodes.v[node].e;
    if (!str_eq(json_get_str(e, "type"), "message")) return false;
    const char *role = json_get_str(json_get(e, "message"), "role");
    return str_eq(role, "user") || str_eq(role, "assistant");
}

static void move_turn(tree_sel_t *t, int dir) {
    for (ssize_t i = sel_index(t) + dir; i >= 0 && i < (ssize_t)t->vis.n; i += dir) {
        size_t node = t->rows.v[t->vis.v[i]].node;
        if (is_turn(t, node)) {
            set_selected(t, node);
            return;
        }
    }
}

/* app.tree.previousBranch / nextBranch: every visible branch head in rendered order, a filtered head
 * standing in by its first visible descendant */
static void move_branch(tree_sel_t *t, int dir) {
    if (t->sel == NONE) return;
    size_t nn = t->nodes.n;
    size_t *first_vis = xmalloc((nn ? nn : 1) * sizeof *first_vis);
    for (ssize_t r = (ssize_t)t->rows.n - 1; r >= 0; r--) {
        size_t node = t->rows.v[r].node;
        size_t target = t->vis_of[node] >= 0 ? node : NONE;
        for (size_t i = 0; target == NONE && i < nkids(t, node); i++) target = first_vis[kid(t, node, i)];
        first_vis[node] = target;
    }
    for (size_t i = 0; i < nn; i++)
        if (t->row_of[i] == NONE) first_vis[i] = NONE;
    /* targets keyed by target node; every raw fork branch maps to its target */
    size_t *target_of_root = xmalloc((nn ? nn : 1) * sizeof *target_of_root);
    for (size_t i = 0; i < nn; i++) target_of_root[i] = NONE;
    VEC(size_t) targets = {0};
    for (ssize_t r = -1; r < (ssize_t)t->rows.n; r++) {
        size_t parent = r < 0 ? NONE : t->rows.v[r].node;
        size_t nk = nkids(t, parent);
        if (nk < 2) continue;
        for (size_t i = 0; i < nk; i++) {
            size_t b = kid(t, parent, i), tg = first_vis[b];
            if (tg == NONE) continue;
            target_of_root[b] = tg;
            bool seen = false;
            for (size_t k = 0; k < targets.n; k++) seen |= targets.v[k] == tg;
            if (!seen) vec_push(&targets, tg);
        }
    }
    /* sort targets by visible index */
    for (size_t i = 1; i < targets.n; i++)
        for (size_t k = i; k > 0 && t->vis_of[targets.v[k]] < t->vis_of[targets.v[k - 1]]; k--) {
            size_t tmp = targets.v[k];
            targets.v[k] = targets.v[k - 1];
            targets.v[k - 1] = tmp;
        }
    if (targets.n) {
        /* nearest ancestor fork's branch root containing the selection */
        size_t branch = t->sel, root = NONE;
        for (size_t guard = 0; guard <= nn; guard++) {
            size_t parent = t->nodes.v[branch].parent;
            if (nkids(t, parent) > 1) {
                root = branch;
                break;
            }
            if (parent == NONE) break;
            branch = parent;
        }
        ssize_t cur = -1;
        if (root != NONE && target_of_root[root] != NONE)
            for (size_t k = 0; k < targets.n; k++)
                if (targets.v[k] == target_of_root[root]) cur = (ssize_t)k;
        ssize_t n = (ssize_t)targets.n;
        ssize_t next = cur < 0 ? (dir > 0 ? 0 : n - 1) : (cur + dir + n) % n;
        set_selected(t, targets.v[next]);
    }
    vec_free(&targets);
    free(first_vis);
    free(target_of_root);
}

/* ---------- rendering ---------- */

static const char *filter_label(const tree_sel_t *t) {
    switch (t->filter) {
    case F_NO_TOOLS: return " [no-tools]";
    case F_USER: return " [user]";
    case F_LABELED: return " [labeled]";
    case F_ALL: return " [all]";
    default: return "";
    }
}

static void push_muted(strvec_t *out, const char *text, int width) {
    char *m = S(TUI_TC_MUTED, text);
    strvec_push_own(out, tui_truncate_to_width(m, width, NULL));
    free(m);
}

/* renderPrefix: connectors only at real branch points, gutters for ancestors with siblings below, a
 * leading … when older levels scrolled off (one shared horizontal offset for the window) */
static char *gutter(tree_sel_t *t, const trow_t *row, int width, int window_deepest) {
    int reserve = MAX(24, width / 2);
    int max_levels = MAX(1, (width - reserve - 4) / 3);
    int window_offset = MAX(0, window_deepest - max_levels);
    int indent = row->depth;
    bool conn = row->parent_node != NONE && row->sib_count > 1;
    int scroll = MIN(window_offset, indent);
    int rendered = indent - scroll;
    int conn_level = conn ? rendered - 1 : -1;
    const tanc_t **by_level = xcalloc((size_t)MAX(1, rendered), sizeof *by_level);
    for (ssize_t c = row->chain; c >= 0; c = t->chains.v[c].parent) {
        const tanc_t *a = &t->chains.v[c];
        if (a->sib_count <= 1) continue;
        if (t->multiple_roots && t->nodes.v[a->key].parent == NONE) continue;
        int level = a->depth - 1 - scroll;
        /* ancestors are walked innermost first; the outermost (first in upstream order) wins a level */
        if (level >= 0 && level < rendered) by_level[level] = a;
    }
    sb_t b = SB_INIT;
    const char *cs = conn ? (row->is_last ? tui_sym("tree.last") : tui_sym("tree.branch")) : "";
    /* connector glyphs as code points */
    char cg[3][8] = {{0}};
    {
        const char *p = cs;
        for (int k = 0; k < 3 && *p; k++) {
            size_t l = 1;
            while (p[l] && ((unsigned char)p[l] & 0xc0) == 0x80) l++;
            memcpy(cg[k], p, MIN(l, sizeof cg[k] - 1));
            p += l;
        }
    }
    for (int i = 0; i < rendered * 3; i++) {
        int level = i / 3, pos = i % 3;
        const tanc_t *g = by_level[level];
        if (i == 0 && scroll > 0) {
            sb_append(&b, "\xe2\x80\xa6");
            continue;
        }
        if (g) sb_append(&b, pos == 0 && !g->is_last ? tui_sym("tree.vertical") : " ");
        else if (conn && level == conn_level)
            sb_append(&b, pos == 0 ? (*cg[0] ? cg[0] : " ") : pos == 1 ? (*cg[1] ? cg[1] : tui_sym("tree.horizontal")) : (*cg[2] ? cg[2] : " "));
        else sb_putc(&b, ' ');
    }
    free(by_level);
    char *r = S(TUI_TC_DIM, sb_cstr(&b));
    sb_free(&b);
    return r;
}

static char *row_body(tree_sel_t *t, size_t node, bool selected) {
    tnode_t *nd = &t->nodes.v[node];
    sb_t b = SB_INIT;
    if (nd->active) {
        char *m = xasprintf("%s ", tui_sym("md.bullet")), *a = S(TUI_TC_ACCENT, m);
        sb_append(&b, a);
        free(m), free(a);
    }
    if (nd->label) {
        char *lt = row_text(nd->label), *l = xasprintf("[%s] ", lt), *w = S(TUI_TC_WARNING, l);
        sb_append(&b, w);
        free(lt), free(l), free(w);
    }
    parts_t parts = {0};
    entry_parts(t, nd, &parts);
    sb_t d = SB_INIT;
    for (size_t i = 0; i < parts.n; i++) {
        char *v = row_text(parts.v[i].t);
        if (parts.v[i].s >= 0) {
            char *c = S((tui_tc_t)parts.v[i].s, v);
            free(v);
            v = c;
        }
        if (parts.v[i].strong && !selected) {
            char *c = tui_bold(v);
            free(v);
            v = c;
        }
        sb_append(&d, v);
        free(v);
    }
    parts_free(&parts);
    if (selected) {
        char *bd = tui_bold(sb_cstr(&d));
        sb_append(&b, bd);
        free(bd);
    } else sb_append(&b, sb_cstr(&d));
    sb_free(&d);
    return sb_detach(&b);
}

static void render_list(tree_sel_t *t, int width, strvec_t *out) {
    size_t total = t->rows.n;
    if (!t->vis.n) {
        char *s;
        if (!total) {
            push_muted(out, "No entries found", width);
            s = xasprintf("(0/0)%s", filter_label(t));
        } else if (*ui_input_value(&t->search)) {
            s = xasprintf("No entries match search \"%s\"", ui_input_value(&t->search));
            push_muted(out, s, width);
            free(s);
            char *k = ui_key_hint("backspace");
            s = xasprintf("Press %s to clear the search", k);
            free(k);
            push_muted(out, s, width);
            free(s);
            s = xasprintf("(0/%zu)%s", total, filter_label(t));
        } else {
            const char *fl = filter_label(t);
            s = xasprintf("%zu entries hidden by the current filter %s", total, *fl ? fl + 1 : "[default]");
            push_muted(out, s, width);
            free(s);
            char *ka = ui_key_hint("alt+a"), *kd = ui_key_hint("alt+d");
            s = xasprintf("Press %s to show all, %s for default", ka, kd);
            free(ka), free(kd);
            push_muted(out, s, width);
            free(s);
            s = xasprintf("(0/%zu)%s", total, fl);
        }
        push_muted(out, s, width);
        free(s);
        return;
    }
    /* centred window over the visible rows */
    int n = (int)t->vis.n, h = MIN(t->max_rows, n);
    int si = (int)MAX(0, sel_index(t));
    int start = MAX(0, MIN(si - t->max_rows / 2, n - h)), end = MIN(n, start + h);
    int deepest = 0;
    for (int i = start; i < end; i++) deepest = MAX(deepest, t->rows.v[t->vis.v[i]].depth);
    bool bar = t->max_rows < n;
    int cw = bar ? MAX(0, width - 1) : width;
    int ts = 0, te = 0;
    if (bar) {
        int lines = end - start;
        int size = MAX(1, MIN((lines * lines) / n, lines));
        int maxoff = MAX(0, n - lines);
        ts = maxoff ? (int)lround((double)MIN(start, maxoff) / maxoff * (lines - size)) : 0;
        te = ts + size;
    }
    for (int i = start; i < end; i++) {
        const trow_t *row = &t->rows.v[t->vis.v[i]];
        bool selected = row->node == t->sel;
        char *g = gutter(t, row, width, deepest);
        char *body = row_body(t, row->node, selected);
        char *lead = selected ? S(TUI_TC_ACCENT, "\xe2\x80\xba ") : xstrdup("  ");
        char *bt = tui_replace_tabs(body);
        char *line = xasprintf("%s%s%s", lead, g, bt);
        if (selected) {
            char *s = tui_bgs(NULL, TUI_TC_SELECTED_BG, line);
            free(line);
            line = s;
        }
        char *tr = tui_truncate_to_width(line, width, NULL);
        if (bar) {
            char *c = tui_truncate_to_width(tr, cw, NULL), *pad = tui_pad_to_width(c, cw);
            bool thumb = i - start >= ts && i - start < te;
            char *gl = S(thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
            strvec_push_own(out, xasprintf("%s%s", pad, gl));
            free(c), free(pad), free(gl), free(tr);
        } else strvec_push_own(out, tr);
        free(g), free(body), free(lead), free(bt), free(line);
    }
    const char *fl = filter_label(t);
    if (*fl) push_muted(out, fl + 1, width);
}

static char *help_text(void) {
    sb_t b = SB_INIT;
    char *k[14];
    k[0] = ui_key_hint("enter");
    k[1] = ui_key_hint("alt+up");
    k[2] = ui_key_hint("alt+down");
    k[3] = ui_editor_key("app.tree.previousBranch");
    k[4] = ui_editor_key("app.tree.nextBranch");
    k[5] = ui_editor_key("tui.select.pageUp");
    k[6] = ui_editor_key("tui.select.pageDown");
    k[7] = ui_key_hint("left");
    k[8] = ui_key_hint("right");
    k[9] = ui_key_hint("home");
    k[10] = ui_key_hint("end");
    k[11] = ui_key_hint("shift+enter");
    k[12] = ui_key_hint("shift+l");
    k[13] = ui_key_hint("ctrl+o");
    sb_appendf(&b, "%s: switch. %s/%s: previous/next turn.", k[0], k[1], k[2]);
    if (*k[3] || *k[4]) sb_appendf(&b, " %s%s%s: previous/next branch.", k[3], *k[3] && *k[4] ? "/" : "", k[4]);
    sb_appendf(&b, " %s/%s (%s/%s): page. %s/%s: first/last item. %s: summarize & switch. %s: label. %s: filter.", k[5],
               k[6], k[7], k[8], k[9], k[10], k[11], k[12], k[13]);
    static const char *const AK[] = {"alt+d", "alt+t", "alt+u", "alt+l", "alt+a"};
    sb_putc(&b, ' ');
    for (size_t i = 0; i < ARRAY_LEN(AK); i++) {
        char *h = ui_key_hint(AK[i]);
        sb_appendf(&b, "%s%s", i ? "/" : "", h);
        free(h);
    }
    sb_append(&b, ": filter. Type to search");
    for (size_t i = 0; i < ARRAY_LEN(k); i++) free(k[i]);
    return sb_detach(&b);
}

static void tree_render(tui_component_t *c, int width, strvec_t *out) {
    tree_sel_t *t = (tree_sel_t *)c;
    int inner = MAX(1, width - 4);
    strvec_t body = {0};
    strvec_push(&body, "");
    char *help = help_text(), *hm = S(TUI_TC_MUTED, help);
    strvec_push_own(&body, tui_truncate_to_width(hm, inner, NULL));
    free(help), free(hm);
    char *prompt = S(TUI_TC_MUTED, "Search:"), *pr = xasprintf("%s ", prompt);
    strvec_push_own(&body, ui_input_render(&t->search, pr, inner, t->base.focused && !t->labeling));
    free(prompt), free(pr);
    size_t divider_at = body.n;
    strvec_push(&body, "");
    if (t->labeling) {
        push_muted(&body, "Label (empty to remove):", inner);
        strvec_push_own(&body, ui_input_render(&t->label_in, "> ", inner, t->base.focused));
        char *en = ui_key_hint("enter"), *esc = ui_editor_key("app.interrupt");
        if (!*esc) free(esc), esc = ui_key_hint("escape");
        char *h = xasprintf("%s: save  %s: cancel", en, esc), *d = S(TUI_TC_DIM, h);
        strvec_push_own(&body, tui_truncate_to_width(d, inner, NULL));
        free(en), free(esc), free(h), free(d);
    } else render_list(t, inner, &body);
    strvec_push(&body, "");
    strvec_push_own(out, ui_box_top(width, "Session Tree"));
    for (size_t i = 0; i < body.n; i++) {
        if (i == divider_at) strvec_push_own(out, ui_box_divider(width));
        strvec_push_own(out, ui_box_row(body.v[i], width));
    }
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
}

/* ---------- input ---------- */

static bool kb_interrupt(const tui_key_t *k) { return tui_kb(k, "app.interrupt"); }

static void edit_label(tree_sel_t *t) {
    if (t->sel == NONE) return;
    t->labeling = true;
    t->label_node = t->sel;
    ui_input_free(&t->label_in);
    ui_input_set(&t->label_in, t->nodes.v[t->sel].label ? t->nodes.v[t->sel].label : "");
}

static void label_key(tree_sel_t *t, const tui_key_t *k) {
    if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j")) {
        char *v = str_trim(ui_input_value(&t->label_in));
        tnode_t *nd = &t->nodes.v[t->label_node];
        free(nd->label);
        nd->label = *v ? xstrdup(v) : NULL;
        t->labeling = false;
        apply_filter(t);
        if (t->cb.on_label) t->cb.on_label(t->cb.ud, nd->id, nd->label);
        free(v);
    } else if (kb_interrupt(k)) t->labeling = false;
    else ui_input_key(&t->label_in, k);
}

static bool tree_key(tui_component_t *c, const tui_key_t *k) {
    tree_sel_t *t = (tree_sel_t *)c;
    if (t->labeling) {
        label_key(t, k);
    } else if (tui_kb(k, "tui.select.up")) move_sel(t, -1, true);
    else if (tui_kb(k, "tui.select.down")) move_sel(t, 1, true);
    else if (tui_key_matches(k, "alt+up")) move_turn(t, -1);
    else if (tui_key_matches(k, "alt+down")) move_turn(t, 1);
    else if (tui_kb(k, "app.tree.previousBranch")) move_branch(t, -1);
    else if (tui_kb(k, "app.tree.nextBranch")) move_branch(t, 1);
    else if (tui_key_matches(k, "home")) set_index(t, 0);
    else if (tui_key_matches(k, "end")) set_index(t, (ssize_t)t->vis.n - 1);
    else if (tui_kb(k, "tui.select.pageUp") || tui_key_matches(k, "left")) move_sel(t, -t->max_rows, false);
    else if (tui_kb(k, "tui.select.pageDown") || tui_key_matches(k, "right")) move_sel(t, t->max_rows, false);
    else if (tui_key_matches(k, "shift+enter") || tui_key_matches(k, "ctrl+j")) {
        if (t->sel != NONE && t->cb.on_select) t->cb.on_select(t->cb.ud, t->nodes.v[t->sel].id, true);
        return true;
    } else if (tui_key_matches(k, "enter")) {
        if (t->sel != NONE && t->cb.on_select) t->cb.on_select(t->cb.ud, t->nodes.v[t->sel].id, false);
        return true;
    } else if (kb_interrupt(k)) {
        if (*ui_input_value(&t->search)) {
            ui_input_set(&t->search, "");
            apply_filter(t);
        } else {
            if (t->cb.on_cancel) t->cb.on_cancel(t->cb.ud);
            return true;
        }
    } else if (tui_key_matches(k, "ctrl+c")) {
        if (t->cb.on_cancel) t->cb.on_cancel(t->cb.ud);
        return true;
    } else if (tui_key_matches(k, "ctrl+shift+o")) {
        t->filter = (t->filter + F__N - 1) % F__N;
        apply_filter(t);
    } else if (tui_key_matches(k, "ctrl+o")) {
        t->filter = (t->filter + 1) % F__N;
        apply_filter(t);
    } else if (tui_key_matches(k, "alt+d")) t->filter = F_DEFAULT, apply_filter(t);
    else if (tui_key_matches(k, "alt+t")) t->filter = F_NO_TOOLS, apply_filter(t);
    else if (tui_key_matches(k, "alt+u")) t->filter = F_USER, apply_filter(t);
    else if (tui_key_matches(k, "alt+l")) t->filter = F_LABELED, apply_filter(t);
    else if (tui_key_matches(k, "alt+a")) t->filter = F_ALL, apply_filter(t);
    else if ((tui_key_matches(k, "shift+l") || str_eq(tui_key_text(k), "L")) && !*ui_input_value(&t->search))
        edit_label(t);
    else {
        char *before = xstrdup(ui_input_value(&t->search));
        if (ui_input_key(&t->search, k) && strcmp(before, ui_input_value(&t->search))) apply_filter(t);
        free(before);
    }
    tui_component_request_render(c);
    return true;
}

static void tree_destroy(tui_component_t *c) {
    tree_sel_t *t = (tree_sel_t *)c;
    for (size_t i = 0; i < t->nodes.n; i++) {
        free(t->nodes.v[i].label);
        free(t->nodes.v[i].search);
        free(t->nodes.v[i].search_label);
        vec_free(&t->nodes.v[i].kids);
    }
    vec_free(&t->nodes);
    vec_free(&t->roots);
    vec_free(&t->rows);
    vec_free(&t->chains);
    vec_free(&t->vis);
    free(t->row_of);
    free(t->vis_of);
    hmap_free(&t->by_id, NULL);
    json_free(t->tool_calls);
    ui_input_free(&t->search);
    ui_input_free(&t->label_in);
    free(t);
}

static const tui_component_vt TREE_VT = {"tree-selector", tree_render, tree_key, NULL, tree_destroy};

/* sRGB 0xRRGGBB -> OKLab */
static void oklab(int64_t rgb, double out[3]) {
    double c[3];
    for (int i = 0; i < 3; i++) {
        double v = (double)((rgb >> (16 - 8 * i)) & 255) / 255.0;
        c[i] = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    }
    double l = cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
    double m = cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
    double s = cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);
    out[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    out[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    out[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

/* the assistant role color: success, else statusLinePath, else muted - the first that stays apart from
 * accent (OKLab distance >= 0.05) */
static tui_tc_t assistant_color(void) {
    const tui_theme_t *th = tui_theme();
    if (!th || th->rgb[TUI_TC_ACCENT] < 0) return TUI_TC_SUCCESS;
    double a[3];
    oklab(th->rgb[TUI_TC_ACCENT], a);
    const tui_tc_t cand[2] = {TUI_TC_SUCCESS, TUI_TC_STATUS_LINE_PATH};
    for (int i = 0; i < 2; i++) {
        if (th->rgb[cand[i]] < 0) continue;
        double b[3];
        oklab(th->rgb[cand[i]], b);
        if (sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) >= 0.05)
            return cand[i];
    }
    return TUI_TC_MUTED;
}

tui_component_t *ui_tree_selector_new(const json_t *entries, const char *leaf_id, int term_rows,
                                      const char *filter_mode, const ui_tree_cb_t *cb) {
    tree_sel_t *t = xcalloc(1, sizeof *t);
    tui_component_init(&t->base, &TREE_VT);
    if (cb) t->cb = *cb;
    t->assistant_tc = assistant_color();
    for (int i = 0; i < F__N; i++)
        if (str_eq(filter_mode, FILTER_MODES[i])) t->filter = i;
    if (term_rows <= 0) term_rows = 40;
    t->max_rows = MAX(1, MIN(MAX(5, term_rows / 2), term_rows - 8));
    t->sel = t->anchor = NONE;
    build_model(t, entries, leaf_id);
    project(t);
    apply_filter(t);
    if (t->leaf == NONE) set_index(t, (ssize_t)t->vis.n - 1);
    else set_selected(t, t->leaf);
    return &t->base;
}
