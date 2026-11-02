/* planreview.c - fullscreen plan-review overlay and the "Save and quit" path prompt (ports of omp
 * packages/tui/src/overlays/plan-review-overlay.ts, plan-toc.ts, plan-save-overlay.ts and the
 * chrome/overlay-box.ts split-frame helpers).
 *
 * The plan is split into sections (preamble + one per ATX heading), each rendered through its own
 * markdown component and windowed by a scroll view; the approval options (plus the optional
 * model-tier slider) sit beneath inside the same outlined box. With >= 2 headings on a wide enough
 * terminal a Contents sidebar tracks the scrolled section, jumps between sections, deletes sections
 * (with undo, mirrored through on_plan_edited) and annotates them (feedback for Refine). Focus
 * regions (toc / body / actions) cycle with Tab / Shift+Tab. */
#include "ui/overlays.h"
#include "ui/ui.h"
#include "config/settings.h"

#include <ctype.h>
#include <math.h>
#include <regex.h>

#define OVERLAY_TITLE "Plan Review"
#define MIN_BODY_ROWS 3
#define MAX_ANNOTATION_EDITOR_ROWS 6
#define SIDEBAR_MIN_HEADINGS 2
#define SIDEBAR_MIN_TOTAL_WIDTH 64
#define SIDEBAR_MIN_BODY_WIDTH 40
#define MAX_ANNOTATION_CONTEXT_WIDTH 120
#define MAX_UNDO_ENTRIES 100
#define FAST_SCROLL_LINES 5

enum { F_TOC, F_BODY, F_ACTIONS };

/* ---------- plan-toc.ts ---------- */

typedef struct {
    int level; /* 0 = preamble */
    char *title;
    char *raw;
} plan_sec_t;

static bool is_ws(char c) { return c == ' ' || c == '\t'; }

/* stripInlineMarkdown: images/links/refs to their text, autolinks, code, emphasis; collapse whitespace */
static char *strip_inline_md(const char *text) {
    static const char *const PATS[][2] = {
        {"!\\[([^]]*)\\]\\([^)]*\\)", "\\1"}, {"\\[([^]]*)\\]\\([^)]*\\)", "\\1"}, {"\\[([^]]*)\\]\\[[^]]*\\]", "\\1"},
        {"<([^> \t\n]+)>", "\\1"},            {"`([^`]+)`", "\\1"},
    };
    char *out = xstrdup(text);
    for (size_t p = 0; p < ARRAY_LEN(PATS); p++) {
        regex_t re;
        if (regcomp(&re, PATS[p][0], REG_EXTENDED) != 0) continue;
        sb_t b = SB_INIT;
        const char *s = out;
        regmatch_t m[2];
        while (*s && regexec(&re, s, 2, m, s == out ? 0 : REG_NOTBOL) == 0) {
            sb_appendn(&b, s, (size_t)m[0].rm_so);
            sb_appendn(&b, s + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
            if (m[0].rm_eo == 0) {
                sb_putc(&b, *s);
                s++;
            } else s += m[0].rm_eo;
        }
        sb_append(&b, s);
        free(out);
        out = sb_detach(&b);
        regfree(&re);
    }
    /* (\*\*|__)(.+?)\1, then (\*|_)(.+?)\1, then ~~(.+?)~~: lazy pairs, scanned left to right */
    static const char *const DELIMS[] = {"**", "__", "*", "_", "~~"};
    for (int pass = 0; pass < 3; pass++) {
        sb_t b = SB_INIT;
        const char *s = out;
        while (*s) {
            const char *hit = NULL;
            size_t dl = 0;
            for (size_t d = 0; d < ARRAY_LEN(DELIMS); d++) {
                size_t l = strlen(DELIMS[d]);
                bool want = pass == 0 ? l == 2 && DELIMS[d][0] != '~' : pass == 1 ? l == 1 : DELIMS[d][0] == '~';
                if (!want || strncmp(s, DELIMS[d], l) != 0) continue;
                const char *close = strstr(s + l + 1, DELIMS[d]);
                if (close && close > s + l) {
                    hit = close;
                    dl = l;
                    break;
                }
            }
            if (hit) {
                sb_appendn(&b, s + dl, (size_t)(hit - s - dl));
                s = hit + dl;
            } else sb_putc(&b, *s++);
        }
        free(out);
        out = sb_detach(&b);
    }
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *s = out; *s; s++) {
        if (isspace((unsigned char)*s)) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, *s);
    }
    free(out);
    return sb_detach(&b);
}

/* HEADING_RE = /^(#{1,6})[ \t]+(.+?)[ \t]*#*[ \t]*$/ */
static bool parse_heading(const char *line, size_t len, int *level, char **title) {
    size_t i = 0;
    while (i < len && line[i] == '#') i++;
    if (i < 1 || i > 6 || i >= len || !is_ws(line[i])) return false;
    size_t s = i;
    while (s < len && is_ws(line[s])) s++;
    size_t e = len;
    while (e > s && is_ws(line[e - 1])) e--;
    size_t h = e;
    while (h > s && line[h - 1] == '#') h--;
    /* closing #s only when preceded by whitespace or the text is otherwise empty (lazy .+? keeps one char) */
    if (h < e && (h == s || is_ws(line[h - 1]))) {
        e = h;
        while (e > s && is_ws(line[e - 1])) e--;
    }
    if (e <= s) {
        /* `.+?` needs one char: "# #" keeps "#" */
        e = s + 1;
        if (e > len) return false;
    }
    char *t = xstrndup(line + s, e - s);
    *title = strip_inline_md(t);
    free(t);
    *level = (int)i;
    return true;
}

/* FENCE_RE = /^ {0,3}(`{3,}|~{3,})(.*)$/ -> run char/len, rest */
static bool parse_fence(const char *line, size_t len, char *ch, size_t *run, const char **rest, size_t *rest_len) {
    size_t i = 0;
    while (i < len && i < 3 && line[i] == ' ') i++;
    if (i >= len || (line[i] != '`' && line[i] != '~')) return false;
    char c = line[i];
    size_t j = i;
    while (j < len && line[j] == c) j++;
    if (j - i < 3) return false;
    *ch = c;
    *run = j - i;
    *rest = line + j;
    *rest_len = len - j;
    return true;
}

typedef VEC(plan_sec_t) plan_secs_t;
typedef VEC(bool) boolvec_t;

static void plan_secs_free(plan_secs_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].title);
        free(v->v[i].raw);
    }
    vec_free(v);
}

static plan_secs_t parse_plan_sections(const char *text) {
    plan_secs_t out = {0};
    typedef struct {
        size_t off;
        int level;
        char *title;
    } head_t;
    VEC(head_t) heads = {0};
    char fence = 0;
    size_t fence_len = 0;
    size_t n = strlen(text);
    for (size_t off = 0; off <= n;) {
        const char *nl = memchr(text + off, '\n', n - off);
        size_t len = nl ? (size_t)(nl - (text + off)) : n - off;
        const char *line = text + off;
        char fc;
        size_t run, rl;
        const char *rest;
        bool is_fence = parse_fence(line, len, &fc, &run, &rest, &rl);
        if (!fence) {
            if (is_fence) {
                fence = fc;
                fence_len = run;
            } else {
                int lvl;
                char *title;
                if (parse_heading(line, len, &lvl, &title)) vec_push(&heads, ((head_t){off, lvl, title}));
            }
        } else if (is_fence && fc == fence && run >= fence_len) {
            bool blank = true;
            for (size_t k = 0; k < rl; k++)
                if (!isspace((unsigned char)rest[k])) blank = false;
            if (blank) fence = 0, fence_len = 0;
        }
        if (!nl) break;
        off += len + 1;
    }
    size_t first = heads.n ? heads.v[0].off : n;
    if (first > 0) vec_push(&out, ((plan_sec_t){0, xstrdup(""), xstrndup(text, first)}));
    for (size_t h = 0; h < heads.n; h++) {
        size_t end = h + 1 < heads.n ? heads.v[h + 1].off : n;
        vec_push(&out, ((plan_sec_t){heads.v[h].level, heads.v[h].title, xstrndup(text + heads.v[h].off, end - heads.v[h].off)}));
    }
    vec_free(&heads);
    return out;
}

/* ---------- overlay-box.ts split frame ---------- */

static char *paint_border(const char *s) { return tui_fgs(NULL, TUI_TC_BORDER, s); }

static int split_body_width(int width, int sidebar) { return MAX(0, width - sidebar - 7); }

static char *top_border_split(int width, const char *title, int sidebar) {
    const char *h = tui_sym("boxRound.horizontal");
    int divider = sidebar + 3, left_len = MAX(0, divider - 1), right_len = MAX(0, width - 2 - divider);
    sb_t r = SB_INIT, b = SB_INIT;
    char *t = xasprintf(" %s ", title);
    char *shown = tui_truncate_to_width(t, MAX(0, left_len - 1), NULL);
    int fill = MAX(0, left_len - 1 - tui_visible_width(shown));
    sb_append(&r, tui_sym("boxRound.topLeft"));
    sb_append(&r, h);
    char *p = paint_border(sb_cstr(&r));
    sb_append(&b, p);
    free(p);
    char *acc = tui_fgs(NULL, TUI_TC_ACCENT, shown), *bold = tui_bold(acc);
    sb_append(&b, bold);
    free(bold);
    free(acc);
    sb_clear(&r);
    sb_repeat(&r, h, fill);
    p = paint_border(sb_cstr(&r));
    sb_append(&b, p);
    free(p);
    sb_clear(&r);
    sb_append(&r, tui_sym("boxSharp.teeDown"));
    sb_repeat(&r, h, right_len);
    sb_append(&r, tui_sym("boxRound.topRight"));
    p = paint_border(sb_cstr(&r));
    sb_append(&b, p);
    free(p);
    sb_free(&r);
    free(shown);
    free(t);
    return sb_detach(&b);
}

static char *divider_split(int width, int sidebar) {
    const char *h = tui_sym("boxRound.horizontal");
    int divider = sidebar + 3, left_len = MAX(0, divider - 1), right_len = MAX(0, width - 2 - divider);
    sb_t r = SB_INIT;
    sb_append(&r, tui_sym("boxSharp.teeRight"));
    sb_repeat(&r, h, left_len);
    sb_append(&r, tui_sym("boxSharp.teeUp"));
    sb_repeat(&r, h, right_len);
    sb_append(&r, tui_sym("boxSharp.teeLeft"));
    char *p = paint_border(sb_cstr(&r));
    sb_free(&r);
    return p;
}

static char *split_row(const char *sidebar, const char *body, int width, int sidebar_w) {
    int bw = split_body_width(width, sidebar_w);
    char *bar = paint_border(tui_sym("boxRound.vertical"));
    char *sp = sidebar_w > 0 ? tui_pad_to_width(sidebar, sidebar_w) : xstrdup("");
    char *bp = bw > 0 ? tui_pad_to_width(body, bw) : xstrdup("");
    char *r = xasprintf("%s %s %s %s %s", bar, sp, bar, bp, bar);
    free(bp);
    free(sp);
    free(bar);
    return r;
}

/* ---------- overlay ---------- */

typedef struct {
    char *note;
    bool line;      /* target kind: line (else section) */
    int row;
    char *context;
    bool truncated;
} annot_t;

typedef VEC(annot_t) annots_t;

typedef struct {
    int level;
    char *title;
    char *raw;
    tui_markdown_t *md;
    annots_t ann;
} sec_t;

typedef struct {
    int section, row;
    const char *context; /* borrowed from the layout's context table */
    bool truncated;
} anchor_t;

typedef struct {
    char *text;
    annots_t *ann; /* per section */
    size_t nsec;
    strvec_t deleted;
} undo_t;

typedef struct {
    int section, index;
} annsel_t;

struct ui_plan_review {
    tui_component_t base;
    tui_t *tui;
    VEC(sec_t) secs;
    VEC(int) toc;
    int title_index, toc_base_level;
    VEC(int) sec_offsets;
    VEC(anchor_t) anchors;
    strvec_t ctx_texts; /* owned context strings the anchors point at */
    strvec_t body_lines;
    VEC(undo_t) undo;
    strvec_t deleted;

    strvec_t options;
    bool *disabled;
    char *help_suffix, *editor_label, *prompt_title;
    int selected;
    /* slider */
    bool has_slider;
    char *slider_caption;
    strvec_t seg_labels, seg_details;
    int slider_index;

    int focus, toc_cursor;
    bool sidebar_shown, pending_scroll_to_toc;
    double scroll_progress;
    int scroll, height; /* scroll view */

    bool committed;
    char *committed_label;
    bool annotating;
    tui_editor_t *editor;
    bool target_set, target_line;
    int target_section, target_row;
    char *target_context;
    bool target_truncated;
    bool editing;
    annsel_t editing_sel;
    bool chooser_open;
    VEC(annsel_t) chooser;
    int chooser_sel;
    int annotation_rev;

    ui_plan_review_cb_t cb;
};

static void annot_free(annot_t *a) {
    free(a->note);
    free(a->context);
}

static annot_t annot_clone(const annot_t *a) {
    return (annot_t){xstrdup(a->note), a->line, a->row, a->context ? xstrdup(a->context) : NULL, a->truncated};
}

static void annots_free(annots_t *v) {
    for (size_t i = 0; i < v->n; i++) annot_free(&v->v[i]);
    vec_free(v);
}

static void secs_clear(ui_plan_review_t *p) {
    for (size_t i = 0; i < p->secs.n; i++) {
        free(p->secs.v[i].title);
        free(p->secs.v[i].raw);
        tui_component_destroy(&p->secs.v[i].md->base);
        annots_free(&p->secs.v[i].ann);
    }
    p->secs.n = 0;
}

static char *join_sections(const ui_plan_review_t *p) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < p->secs.n; i++) sb_append(&b, p->secs.v[i].raw);
    if (!b.len) return sb_detach(&b);
    if (b.s[b.len - 1] != '\n') sb_putc(&b, '\n');
    return sb_detach(&b);
}

static void rebuild_toc(ui_plan_review_t *p) {
    p->toc.n = 0;
    VEC(int) heads = {0};
    int min_level = INT32_MAX;
    for (size_t i = 0; i < p->secs.n; i++)
        if (p->secs.v[i].level >= 1) {
            vec_push(&heads, (int)i);
            min_level = MIN(min_level, p->secs.v[i].level);
        }
    int top = 0, top_index = -1;
    for (size_t k = 0; k < heads.n; k++)
        if (p->secs.v[heads.v[k]].level == min_level) {
            if (!top) top_index = heads.v[k];
            top++;
        }
    int title = top == 1 && heads.n && heads.v[0] == top_index ? top_index : -1;
    p->title_index = title;
    p->toc_base_level = INT32_MAX;
    for (size_t k = 0; k < heads.n; k++)
        if (heads.v[k] != title) {
            vec_push(&p->toc, heads.v[k]);
            p->toc_base_level = MIN(p->toc_base_level, p->secs.v[heads.v[k]].level);
        }
    if (!p->toc.n) p->toc_base_level = 1;
    vec_free(&heads);
}

static void set_sections(ui_plan_review_t *p, const char *plan) {
    secs_clear(p);
    plan_secs_t ps = parse_plan_sections(plan ? plan : "");
    for (size_t i = 0; i < ps.n; i++) {
        sec_t s = {ps.v[i].level, xstrdup(ps.v[i].title), xstrdup(ps.v[i].raw), tui_markdown_new(ps.v[i].raw, 1, 0), {0}};
        vec_push(&p->secs, s);
    }
    plan_secs_free(&ps);
    rebuild_toc(p);
    p->toc_cursor = MIN(p->toc_cursor, MAX(0, (int)p->toc.n - 1));
}

/* sanitizeStatusText: escapes stripped, controls/newlines/tabs -> space, space runs collapsed, trimmed */
static char *sanitize_status(const char *s) {
    char *plain = tui_strip_ansi(s ? s : "");
    sb_t b = SB_INIT;
    bool sp = false;
    for (const unsigned char *c = (const unsigned char *)plain; *c; c++) {
        bool space = *c == ' ' || *c < 0x20 || *c == 0x7f;
        if (space) {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, (char)*c);
    }
    free(plain);
    return sb_detach(&b);
}

/* lineContexts: per rendered row, the sanitized text capped at 120 columns ("(blank line)" when empty) */
static void line_contexts(const strvec_t *rendered, strvec_t *texts, boolvec_t *trunc) {
    for (size_t i = 0; i < rendered->n; i++) {
        char *s = sanitize_status(rendered->v[i]);
        bool t = tui_visible_width(s) > MAX_ANNOTATION_CONTEXT_WIDTH;
        char *tt = tui_truncate_to_width(s, MAX_ANNOTATION_CONTEXT_WIDTH, "\xe2\x80\xa6");
        free(s);
        if (!*tt) {
            free(tt);
            tt = xstrdup("(blank line)");
        }
        strvec_push_own(texts, tt);
        vec_push(trunc, t);
    }
}

static char *normalize_context(const char *text, bool truncated) {
    char *n = sanitize_status(text);
    size_t l = strlen(n);
    if (truncated && l >= 3 && !strcmp(n + l - 3, "\xe2\x80\xa6")) n[l - 3] = 0;
    return n;
}

/* resolveLineRow: row nearest stored_row whose context matches (equal or either contains the other) */
static int resolve_line_row(int stored_row, const char *stored, bool stored_trunc, const strvec_t *texts,
                            const bool *truncs) {
    if (!texts->n) return -1;
    int target = MAX(0, stored_row);
    char *ns = normalize_context(stored, stored_trunc);
    if (!*ns) {
        free(ns);
        return -1;
    }
    int best = -1, best_d = INT32_MAX;
    for (size_t r = 0; r < texts->n; r++) {
        char *nc = normalize_context(texts->v[r], truncs[r]);
        bool ok = !strcmp(nc, ns) || strstr(nc, ns) || strstr(ns, nc);
        free(nc);
        if (!ok) continue;
        int d = abs((int)r - target);
        if (d < best_d) best = (int)r, best_d = d;
    }
    free(ns);
    return best;
}

static void render_section(ui_plan_review_t *p, size_t i, int width, strvec_t *out) {
    size_t before = out->n;
    tui_component_render(&p->secs.v[i].md->base, width, out);
    /* marked emits a trailing `space` token (one blank row) for a source ending in a blank line */
    const char *raw = p->secs.v[i].raw;
    size_t n = strlen(raw);
    if (out->n > before && n >= 2 && raw[n - 1] == '\n' && raw[n - 2] == '\n' && *out->v[out->n - 1])
        strvec_push(out, "");
}

static int term_rows(ui_plan_review_t *p) {
    int rows = p->tui ? tui_term(p->tui)->rows : 0;
    return rows > 0 ? rows : 40;
}

/* ---------- annotation state (serializable, PlanReviewAnnotationState) ---------- */

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

static char *content_hash(const sec_t *s) { return xasprintf("%zu:%llx", strlen(s->raw), (unsigned long long)fnv1a(s->raw)); }

static json_t *section_path(const ui_plan_review_t *p, size_t idx) {
    json_t *path = json_arr();
    if (p->secs.v[idx].level < 1) return path;
    VEC(int) stack = {0};
    for (size_t i = 0; i <= idx; i++) {
        if (p->secs.v[i].level < 1) continue;
        while (stack.n && p->secs.v[vec_last(&stack)].level >= p->secs.v[i].level) stack.n--;
        vec_push(&stack, (int)i);
    }
    for (size_t k = 0; k < stack.n; k++) json_push_str(path, p->secs.v[stack.v[k]].title);
    vec_free(&stack);
    return path;
}

static json_t *annotation_state(const ui_plan_review_t *p) {
    json_t *st = json_obj(), *arr = json_arr();
    for (size_t i = 0; i < p->secs.n; i++) {
        const sec_t *s = &p->secs.v[i];
        for (size_t k = 0; k < s->ann.n; k++) {
            const annot_t *a = &s->ann.v[k];
            json_t *e = json_obj(), *sec = json_obj(), *tg = json_obj();
            json_set_int(sec, "index", (int64_t)i);
            json_set_str(sec, "title", s->title);
            json_set(sec, "path", section_path(p, i));
            char *h = content_hash(s);
            json_set_str(sec, "contentHash", h);
            free(h);
            json_set(e, "section", sec);
            json_set_str(tg, "kind", a->line ? "line" : "section");
            if (a->line) {
                json_set_int(tg, "row", a->row);
                json_set_str(tg, "context", a->context);
                json_set_bool(tg, "contextTruncated", a->truncated);
            }
            json_set(e, "target", tg);
            json_set_str(e, "note", a->note);
            json_push(arr, e);
        }
    }
    json_set(st, "annotations", arr);
    return st;
}

static void restore_annotation_state(ui_plan_review_t *p, const json_t *state) {
    const json_t *arr = json_get(state, "annotations");
    if (!json_is_arr(arr)) return;
    const json_t *e;
    json_arr_foreach(arr, i, e) {
        const char *note0 = json_get_str(e, "note");
        const json_t *sec = json_get(e, "section"), *tg = json_get(e, "target");
        const char *title = json_get_str(sec, "title");
        if (!note0 || !sec || !tg || !title) continue;
        char *note = str_trim(note0);
        if (!*note) {
            free(note);
            continue;
        }
        const json_t *iv = json_get(sec, "index");
        int stored = iv && json_is_num(iv) ? (int)json_get_int(sec, "index", -1) : -1;
        const json_t *spath = json_get(sec, "path");
        const char *shash = json_get_str(sec, "contentHash");
        VEC(int) match = {0};
        if (json_is_arr(spath) && shash) {
            for (size_t k = 0; k < p->secs.n; k++) {
                char *h = content_hash(&p->secs.v[k]);
                json_t *path = section_path(p, k);
                bool same = str_eq(p->secs.v[k].title, title) && !strcmp(h, shash) && json_len(path) == json_len(spath);
                for (size_t q = 0; same && q < json_len(path); q++)
                    if (!str_eq(json_sv(json_at(path, q)), json_sv(json_at(spath, q)))) same = false;
                json_free(path);
                free(h);
                if (same) vec_push(&match, (int)k);
            }
        } else if (stored >= 0 && stored < (int)p->secs.n && str_eq(p->secs.v[stored].title, title)) {
            vec_push(&match, stored);
        }
        if (!match.n) {
            vec_free(&match);
            free(note);
            continue;
        }
        int si = match.v[0];
        for (size_t k = 1; k < match.n; k++)
            if (abs(match.v[k] - stored) < abs(si - stored)) si = match.v[k];
        vec_free(&match);
        sec_t *s = &p->secs.v[si];
        const char *kind = json_get_str(tg, "kind");
        if (str_eq(kind, "section")) {
            if (s->level >= 1) vec_push(&s->ann, ((annot_t){note, false, 0, NULL, false}));
            else free(note);
            continue;
        }
        const char *ctx = json_get_str(tg, "context");
        if (!str_eq(kind, "line") || !ctx || !json_is_num(json_get(tg, "row"))) {
            free(note);
            continue;
        }
        strvec_t rendered = {0}, texts = {0};
        boolvec_t truncs = {0};
        render_section(p, (size_t)si, MAX_ANNOTATION_CONTEXT_WIDTH, &rendered);
        line_contexts(&rendered, &texts, &truncs);
        int row = resolve_line_row((int)json_get_int(tg, "row", 0), ctx, json_get_bool(tg, "contextTruncated", false),
                                   &texts, truncs.v);
        if (row >= 0) vec_push(&s->ann, ((annot_t){note, true, row, xstrdup(texts.v[row]), truncs.v[row]}));
        else free(note);
        strvec_free(&rendered);
        strvec_free(&texts);
        vec_free(&truncs);
    }
}

static char *md_fence_for(const char *text) {
    sb_t f = SB_INIT;
    sb_append(&f, "```");
    while (strstr(text, sb_cstr(&f))) sb_putc(&f, '`');
    return sb_detach(&f);
}

static void recompute_feedback(ui_plan_review_t *p) {
    p->annotation_rev++;
    if (p->cb.on_annotation_state) {
        json_t *st = annotation_state(p);
        p->cb.on_annotation_state(p->cb.ud, st);
        json_free(st);
    }
    bool any = false;
    for (size_t i = 0; i < p->secs.n; i++)
        if (p->secs.v[i].ann.n) any = true;
    if (!any && !p->deleted.n) {
        if (p->cb.on_feedback) p->cb.on_feedback(p->cb.ud, "");
        return;
    }
    sb_t b = SB_INIT;
    sb_append(&b, "Refinement feedback on the plan:\n");
    if (p->deleted.n) {
        sb_append(&b, "\nRemove these sections:\n");
        for (size_t i = 0; i < p->deleted.n; i++) sb_appendf(&b, "- %s\n", p->deleted.v[i]);
    }
    for (size_t i = 0; i < p->secs.n; i++) {
        const sec_t *s = &p->secs.v[i];
        if (!s->ann.n) continue;
        sb_appendf(&b, "\n## %s\n", *s->title ? s->title : "Plan preamble");
        for (size_t k = 0; k < s->ann.n; k++) {
            const annot_t *a = &s->ann.v[k];
            if (a->line) sb_appendf(&b, "> Line: %s\n", a->context);
            if (!strchr(a->note, '\n')) sb_appendf(&b, "- %s\n", a->note);
            else {
                char *f = md_fence_for(a->note);
                sb_appendf(&b, "%smd\n%s\n%s\n", f, a->note, f);
                free(f);
            }
        }
    }
    if (p->cb.on_feedback) p->cb.on_feedback(p->cb.ud, sb_cstr(&b));
    sb_free(&b);
}

/* ---------- options / slider ---------- */

static int coerce_index(ui_plan_review_t *p, int index) {
    int max = (int)p->options.n - 1;
    if (max < 0) return -1;
    int c = MAX(0, MIN(index, max));
    if (!p->disabled[c]) return c;
    for (int i = c + 1; i <= max; i++)
        if (!p->disabled[i]) return i;
    for (int i = c - 1; i >= 0; i--)
        if (!p->disabled[i]) return i;
    return c;
}

static int first_enabled(ui_plan_review_t *p) {
    for (size_t i = 0; i < p->options.n; i++)
        if (!p->disabled[i]) return (int)i;
    return -1;
}

static void move_selection(ui_plan_review_t *p, int delta) {
    int max = (int)p->options.n - 1;
    if (max < 0) return;
    int index = p->selected;
    for (;;) {
        int next = MAX(0, MIN(index + delta, max));
        if (next == index) return;
        index = next;
        if (!p->disabled[index]) {
            p->selected = index;
            return;
        }
    }
}

static void move_slider(ui_plan_review_t *p, int delta) {
    if (!p->has_slider) return;
    int next = MAX(0, MIN((int)p->seg_labels.n - 1, p->slider_index + delta));
    if (next == p->slider_index) return;
    p->slider_index = next;
    if (p->cb.on_slider) p->cb.on_slider(p->cb.ud, next);
}

static void confirm_selection(ui_plan_review_t *p) {
    int i = p->selected;
    if (i < 0 || i >= (int)p->options.n || p->disabled[i]) return;
    p->committed = true;
    free(p->committed_label);
    p->committed_label = xstrdup(p->options.v[i]);
    if (p->cb.on_pick) p->cb.on_pick(p->cb.ud, p->options.v[i]);
}

/* ---------- scroll view ---------- */

static int max_offset(ui_plan_review_t *p) { return MAX(0, (int)p->body_lines.n - p->height); }

static void set_scroll(ui_plan_review_t *p, int off) { p->scroll = MAX(0, MIN(off, max_offset(p))); }

static void capture_progress(ui_plan_review_t *p) {
    int m = max_offset(p);
    if (m > 0) p->scroll_progress = (double)p->scroll / m;
}

static bool handle_scroll_key(ui_plan_review_t *p, const tui_key_t *k) {
    if (tui_key_matches(k, "shift+up")) set_scroll(p, p->scroll - FAST_SCROLL_LINES);
    else if (tui_key_matches(k, "shift+down")) set_scroll(p, p->scroll + FAST_SCROLL_LINES);
    else if (tui_key_matches(k, "up")) set_scroll(p, p->scroll - 1);
    else if (tui_key_matches(k, "down")) set_scroll(p, p->scroll + 1);
    else if (tui_key_matches(k, "pageUp")) set_scroll(p, p->scroll - MAX(1, p->height - 1));
    else if (tui_key_matches(k, "pageDown")) set_scroll(p, p->scroll + MAX(1, p->height - 1));
    else if (tui_key_matches(k, "home")) set_scroll(p, 0);
    else if (tui_key_matches(k, "end")) set_scroll(p, max_offset(p));
    else return false;
    return true;
}

static void body_scroll(ui_plan_review_t *p, const tui_key_t *k) {
    if (handle_scroll_key(p, k)) {
        if (tui_key_matches(k, "home")) p->scroll_progress = 0;
        else if (tui_key_matches(k, "end")) p->scroll_progress = 1;
        else capture_progress(p);
        return;
    }
    if (k->text && !strcmp(k->text, "g") && !k->mods) {
        set_scroll(p, 0);
        p->scroll_progress = 0;
    } else if (k->text && !strcmp(k->text, "G")) {
        set_scroll(p, max_offset(p));
        p->scroll_progress = 1;
    }
}

/* ---------- toc ---------- */

static void scrub_body_to_toc(ui_plan_review_t *p) {
    if (p->toc_cursor < 0 || p->toc_cursor >= (int)p->toc.n) return;
    int si = p->toc.v[p->toc_cursor];
    if (si < (int)p->sec_offsets.n) {
        set_scroll(p, p->sec_offsets.v[si]);
        capture_progress(p);
    }
}

static int toc_cursor_from_scroll(ui_plan_review_t *p) {
    if (!p->toc.n) return 0;
    int current = 0;
    for (size_t i = 0; i < p->secs.n; i++) {
        int off = i < p->sec_offsets.n ? p->sec_offsets.v[i] : 0;
        if (off <= p->scroll) current = (int)i;
        else break;
    }
    int pos = 0;
    for (size_t k = 0; k < p->toc.n; k++) {
        if (p->toc.v[k] <= current) pos = (int)k;
        else break;
    }
    return pos;
}

static void set_focus(ui_plan_review_t *p, int f) {
    p->focus = f;
    if (f == F_TOC) p->toc_cursor = toc_cursor_from_scroll(p);
}

static void cycle_region(ui_plan_review_t *p, int dir) {
    int regions[3], n = 0;
    if (p->sidebar_shown) regions[n++] = F_TOC;
    regions[n++] = F_BODY;
    regions[n++] = F_ACTIONS;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (regions[i] == p->focus) cur = i;
    int base = cur < 0 ? n - 1 : cur;
    set_focus(p, regions[((base + dir) % n + n) % n]);
}

static void move_toc_cursor(ui_plan_review_t *p, int delta) {
    if (!p->toc.n) return;
    int next = MAX(0, MIN((int)p->toc.n - 1, p->toc_cursor + delta));
    if (next == p->toc_cursor) return;
    p->toc_cursor = next;
    scrub_body_to_toc(p);
}

static void push_undo(ui_plan_review_t *p) {
    if (p->undo.n >= MAX_UNDO_ENTRIES) {
        undo_t *u = &p->undo.v[0];
        free(u->text);
        for (size_t i = 0; i < u->nsec; i++) annots_free(&u->ann[i]);
        free(u->ann);
        strvec_free(&u->deleted);
        memmove(p->undo.v, p->undo.v + 1, (p->undo.n - 1) * sizeof *p->undo.v);
        p->undo.n--;
    }
    undo_t u = {join_sections(p), xcalloc(MAX(1, p->secs.n), sizeof(annots_t)), p->secs.n, {0}};
    for (size_t i = 0; i < p->secs.n; i++)
        for (size_t k = 0; k < p->secs.v[i].ann.n; k++) vec_push(&u.ann[i], annot_clone(&p->secs.v[i].ann.v[k]));
    for (size_t i = 0; i < p->deleted.n; i++) strvec_push(&u.deleted, p->deleted.v[i]);
    vec_push(&p->undo, u);
}

static void plan_edited(ui_plan_review_t *p) {
    if (!p->cb.on_plan_edited) return;
    char *t = join_sections(p);
    p->cb.on_plan_edited(p->cb.ud, t);
    free(t);
}

static void delete_selected_section(ui_plan_review_t *p) {
    if (p->toc_cursor < 0 || p->toc_cursor >= (int)p->toc.n) return;
    int si = p->toc.v[p->toc_cursor];
    if (p->secs.v[si].level == 0) return;
    int end = si + 1;
    while (end < (int)p->secs.n && p->secs.v[end].level > p->secs.v[si].level) end++;
    push_undo(p);
    for (int i = si; i < end; i++)
        if (p->secs.v[i].level >= 1 && *p->secs.v[i].title) strvec_push(&p->deleted, p->secs.v[i].title);
    for (int i = end - 1; i >= si; i--) {
        sec_t *s = &p->secs.v[i];
        free(s->title);
        free(s->raw);
        tui_component_destroy(&s->md->base);
        annots_free(&s->ann);
        memmove(p->secs.v + i, p->secs.v + i + 1, (p->secs.n - i - 1) * sizeof *p->secs.v);
        p->secs.n--;
    }
    rebuild_toc(p);
    p->toc_cursor = MIN(p->toc_cursor, MAX(0, (int)p->toc.n - 1));
    p->pending_scroll_to_toc = true;
    plan_edited(p);
    recompute_feedback(p);
}

static void undo_last(ui_plan_review_t *p) {
    if (!p->undo.n) return;
    undo_t u = vec_pop(&p->undo);
    set_sections(p, u.text);
    for (size_t i = 0; i < p->secs.n; i++) {
        annots_free(&p->secs.v[i].ann);
        if (i < u.nsec) {
            p->secs.v[i].ann = u.ann[i];
            memset(&u.ann[i], 0, sizeof u.ann[i]);
        }
    }
    for (size_t i = 0; i < u.nsec; i++) annots_free(&u.ann[i]);
    free(u.ann);
    strvec_free(&p->deleted);
    p->deleted = u.deleted;
    free(u.text);
    p->toc_cursor = MIN(p->toc_cursor, MAX(0, (int)p->toc.n - 1));
    p->pending_scroll_to_toc = true;
    plan_edited(p);
    recompute_feedback(p);
}

/* ---------- annotations ---------- */

static void editor_reset(ui_plan_review_t *p, const char *text) {
    tui_editor_clear(p->editor);
    if (text && *text) tui_editor_set_text(p->editor, text);
}

static void start_annotate(ui_plan_review_t *p, int section, bool line, int row, const char *context, bool truncated) {
    p->target_set = true;
    p->target_section = section;
    p->target_line = line;
    p->target_row = row;
    free(p->target_context);
    p->target_context = context ? xstrdup(context) : NULL;
    p->target_truncated = truncated;
    p->editing = false;
    p->annotating = true;
    editor_reset(p, "");
}

static void start_section_annotate(ui_plan_review_t *p) {
    if (p->toc_cursor < 0 || p->toc_cursor >= (int)p->toc.n) return;
    start_annotate(p, p->toc.v[p->toc_cursor], false, 0, NULL, false);
}

static int top_anchor(ui_plan_review_t *p) {
    int max = (int)p->anchors.n - 1;
    if (max < 0) return -1;
    return MAX(0, MIN(max, p->scroll));
}

static void start_body_annotate(ui_plan_review_t *p) {
    int r = top_anchor(p);
    if (r < 0) return;
    anchor_t *a = &p->anchors.v[r];
    start_annotate(p, a->section, true, a->row, a->context, a->truncated);
}

static void start_existing(ui_plan_review_t *p, annsel_t sel) {
    if (sel.section < 0 || sel.section >= (int)p->secs.n) return;
    sec_t *s = &p->secs.v[sel.section];
    if (sel.index < 0 || sel.index >= (int)s->ann.n) return;
    annot_t *a = &s->ann.v[sel.index];
    p->chooser_open = false;
    p->editing = true;
    p->editing_sel = sel;
    p->target_set = true;
    p->target_section = sel.section;
    p->target_line = a->line;
    p->target_row = a->row;
    free(p->target_context);
    p->target_context = a->line ? xstrdup(a->context) : NULL;
    p->target_truncated = a->truncated;
    p->annotating = true;
    editor_reset(p, a->note);
}

static bool annotation_matches_anchor(ui_plan_review_t *p, const annot_t *a, const anchor_t *anchor) {
    if (!a->line) return true;
    if (a->row == anchor->row && str_eq(a->context, anchor->context)) return true;
    strvec_t texts = {0};
    boolvec_t truncs = {0};
    VEC(int) seen = {0};
    for (size_t i = 0; i < p->anchors.n; i++) {
        const anchor_t *c = &p->anchors.v[i];
        if (c->section != anchor->section) continue;
        bool dup = false;
        for (size_t k = 0; k < seen.n; k++)
            if (seen.v[k] == c->row) dup = true;
        if (dup) continue;
        vec_push(&seen, c->row);
        strvec_push(&texts, c->context);
        vec_push(&truncs, c->truncated);
    }
    int r = resolve_line_row(a->row, a->context, a->truncated, &texts, truncs.v);
    strvec_free(&texts);
    vec_free(&truncs);
    vec_free(&seen);
    return r == anchor->row;
}

static void edit_current_annotation(ui_plan_review_t *p) {
    p->chooser.n = 0;
    if (p->focus == F_TOC) {
        if (p->toc_cursor < 0 || p->toc_cursor >= (int)p->toc.n) return;
        int si = p->toc.v[p->toc_cursor];
        for (size_t k = 0; k < p->secs.v[si].ann.n; k++) vec_push(&p->chooser, ((annsel_t){si, (int)k}));
    } else if (p->focus == F_BODY) {
        int r = top_anchor(p);
        if (r < 0) return;
        anchor_t a = p->anchors.v[r];
        sec_t *s = &p->secs.v[a.section];
        for (size_t k = 0; k < s->ann.n; k++)
            if (annotation_matches_anchor(p, &s->ann.v[k], &a)) vec_push(&p->chooser, ((annsel_t){a.section, (int)k}));
    }
    if (!p->chooser.n) return;
    if (p->chooser.n == 1) {
        start_existing(p, p->chooser.v[0]);
        return;
    }
    p->chooser_open = true;
    p->chooser_sel = 0;
}

static void exit_annotate(ui_plan_review_t *p) {
    p->annotating = false;
    p->target_set = false;
    p->editing = false;
    editor_reset(p, "");
}

static void submit_annotation(tui_editor_t *ed, const char *value, void *ud) {
    ui_plan_review_t *p = ud;
    (void)ed;
    char *note = str_trim(value ? value : "");
    bool had_target = p->target_set;
    int tsec = p->target_section, trow = p->target_row;
    bool tline = p->target_line, ttrunc = p->target_truncated;
    char *tctx = p->target_context;
    p->target_context = NULL;
    bool editing = p->editing;
    annsel_t es = p->editing_sel;
    p->annotating = false;
    p->target_set = false;
    p->editing = false;
    tui_editor_clear(p->editor);
    if (editing) {
        if (es.section >= 0 && es.section < (int)p->secs.n && es.index >= 0 && es.index < (int)p->secs.v[es.section].ann.n) {
            push_undo(p);
            annots_t *v = &p->secs.v[es.section].ann;
            if (*note) {
                free(v->v[es.index].note);
                v->v[es.index].note = note;
                note = NULL;
            } else {
                annot_free(&v->v[es.index]);
                memmove(v->v + es.index, v->v + es.index + 1, (v->n - es.index - 1) * sizeof *v->v);
                v->n--;
            }
            recompute_feedback(p);
        }
    } else if (*note && had_target && tsec >= 0 && tsec < (int)p->secs.n) {
        push_undo(p);
        vec_push(&p->secs.v[tsec].ann, ((annot_t){note, tline, tline ? trow : 0, tline ? tctx : NULL, ttrunc}));
        if (tline) tctx = NULL;
        note = NULL;
        recompute_feedback(p);
    }
    free(note);
    free(tctx);
}

static void handle_chooser(ui_plan_review_t *p, const tui_key_t *k) {
    int n = (int)p->chooser.n;
    if (tui_kb(k, "tui.select.cancel")) p->chooser_open = false;
    else if (tui_kb(k, "tui.select.up") || tui_key_matches(k, "k")) p->chooser_sel = (p->chooser_sel - 1 + n) % n;
    else if (tui_kb(k, "tui.select.down") || tui_key_matches(k, "j")) p->chooser_sel = (p->chooser_sel + 1) % n;
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return")) {
        annsel_t s = p->chooser.v[p->chooser_sel];
        p->chooser_open = false;
        start_existing(p, s);
    }
}

/* ---------- body layout ---------- */

static void append_callout(ui_plan_review_t *p, const char *note, anchor_t anchor, int width) {
    strvec_t nl = str_split(note, "\n");
    for (size_t i = 0; i < nl.n; i++) {
        char *bar = tui_fgs(NULL, TUI_TC_WARNING, "\xe2\x96\x8e "), *lab = tui_fgs(NULL, TUI_TC_DIM, i == 0 ? "note: " : "      ");
        char *prefix = xasprintf("%s%s", bar, lab);
        int avail = MAX(0, width - tui_visible_width(prefix));
        size_t ll = strlen(nl.v[i]);
        if (ll && nl.v[i][ll - 1] == '\r') nl.v[i][ll - 1] = 0;
        char *san = tui_strip_ansi(nl.v[i]), *tabs = tui_replace_tabs(san);
        char *disp = tui_truncate_to_width(tabs, avail, "\xe2\x80\xa6");
        char *acc = tui_fgs(NULL, TUI_TC_ACCENT, disp);
        char *line = xasprintf("%s%s", prefix, acc);
        strvec_push_own(&p->body_lines, tui_truncate_to_width(line, width, NULL));
        vec_push(&p->anchors, anchor);
        free(line);
        free(acc);
        free(disp);
        free(tabs);
        free(san);
        free(prefix);
        free(lab);
        free(bar);
    }
    strvec_free(&nl);
}

static void build_body(ui_plan_review_t *p, int width) {
    strvec_free(&p->body_lines);
    strvec_free(&p->ctx_texts);
    p->anchors.n = 0;
    p->sec_offsets.n = 0;
    for (size_t si = 0; si < p->secs.n; si++) {
        sec_t *s = &p->secs.v[si];
        vec_push(&p->sec_offsets, (int)p->body_lines.n);
        strvec_t rendered = {0}, texts = {0};
        boolvec_t truncs = {0};
        render_section(p, si, width, &rendered);
        line_contexts(&rendered, &texts, &truncs);
        int *rows = xcalloc(MAX(1, s->ann.n), sizeof(int));
        for (size_t k = 0; k < s->ann.n; k++)
            rows[k] = s->ann.v[k].line ? resolve_line_row(s->ann.v[k].row, s->ann.v[k].context, s->ann.v[k].truncated, &texts, truncs.v) : 0;
        size_t ctx_base = p->ctx_texts.n;
        for (size_t r = 0; r < texts.n; r++) strvec_push(&p->ctx_texts, texts.v[r]);
        for (size_t r = 0; r < rendered.n; r++) {
            anchor_t a = {(int)si, (int)r, NULL, truncs.v[r]};
            a.context = p->ctx_texts.v[ctx_base + r];
            strvec_push(&p->body_lines, rendered.v[r]);
            vec_push(&p->anchors, a);
            for (size_t k = 0; k < s->ann.n; k++)
                if (rows[k] == (int)r) append_callout(p, s->ann.v[k].note, a, width);
        }
        free(rows);
        strvec_free(&rendered);
        strvec_free(&texts);
        vec_free(&truncs);
    }
}

/* ---------- rendering ---------- */

static int sidebar_width_for(int width) { return MAX(18, MIN(30, (int)lround(width * 0.24))); }

static bool sidebar_visible(ui_plan_review_t *p, int width) {
    if ((int)p->toc.n < SIDEBAR_MIN_HEADINGS || width < SIDEBAR_MIN_TOTAL_WIDTH) return false;
    return split_body_width(width, sidebar_width_for(width)) >= SIDEBAR_MIN_BODY_WIDTH;
}

static char *toc_entry(ui_plan_review_t *p, int pos, int width) {
    sec_t *s = &p->secs.v[p->toc.v[pos]];
    bool hl = pos == p->toc_cursor, selected = hl && p->focus == F_TOC, glow = hl && p->focus != F_TOC;
    int indent = MAX(0, s->level - p->toc_base_level);
    const char *ann = s->ann.n ? " \xe2\x9c\x8e" : "";
    int avail = MAX(0, width - 1 - indent - tui_visible_width(ann));
    char *title = tui_truncate_to_width(*s->title ? s->title : "(untitled)", avail, "\xe2\x80\xa6");
    const char *gutter = selected ? "\xe2\x80\xba" : glow ? "\xe2\x96\x8e" : " ";
    char *line = xasprintf("%s%*s%s%s", gutter, indent, "", title, ann);
    free(title);
    char *r;
    if (selected) {
        char *pad = width > 0 ? tui_pad_to_width(line, width) : xstrdup("");
        char *b = tui_bold(pad);
        r = tui_bgs(NULL, TUI_TC_SELECTED_BG, b);
        free(b);
        free(pad);
    } else r = tui_fgs(NULL, glow ? TUI_TC_ACCENT : TUI_TC_MUTED, line);
    free(line);
    return r;
}

/* chrome/segment-track.ts renderSegmentTrack */
static char *segment_track(const strvec_t *labels, int active) {
    static const tui_tc_t CAND[] = {TUI_TC_ACCENT,        TUI_TC_SUCCESS,       TUI_TC_WARNING,        TUI_TC_ERROR,
                                    TUI_TC_MD_CODE,       TUI_TC_MD_LINK,       TUI_TC_SYNTAX_STRING,  TUI_TC_SYNTAX_KEYWORD,
                                    TUI_TC_SYNTAX_FUNCTION, TUI_TC_SYNTAX_NUMBER, TUI_TC_SYNTAX_OPERATOR, TUI_TC_SYNTAX_VARIABLE};
    size_t n = labels->n;
    tui_tc_t pal[ARRAY_LEN(CAND)];
    size_t np = 0;
    for (size_t i = 0; i < ARRAY_LEN(CAND) && np < n; i++) {
        bool dup = false;
        for (size_t k = 0; k < np; k++)
            if (!strcmp(tui_fg_seq(NULL, pal[k]), tui_fg_seq(NULL, CAND[i]))) dup = true;
        if (!dup) pal[np++] = CAND[i];
    }
    if (!np) pal[np++] = TUI_TC_ACCENT;
    char *thin = tui_fgs(NULL, TUI_TC_STATUS_LINE_SEP, tui_sym("sep.powerlineThin"));
    sb_t t = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            if ((int)i == active || (int)i - 1 == active) sb_append(&t, "  ");
            else sb_appendf(&t, " %s ", thin);
        }
        const char *fg = tui_fg_seq(NULL, pal[i % np]);
        if ((int)i != active) {
            sb_appendf(&t, "%s%s\x1b[39m", fg, labels->v[i]);
            continue;
        }
        char bg[40];
        snprintf(bg, sizeof bg, "%s", fg);
        char *q = strstr(bg, "\x1b[38;");
        if (q) q[2] = '4';
        int64_t rgb = tui_theme()->rgb[pal[i % np]];
        const char *contrast;
        if (rgb >= 0) {
            double luma = 0.299 * ((rgb >> 16) & 255) + 0.587 * ((rgb >> 8) & 255) + 0.114 * (rgb & 255);
            contrast = luma > 140 ? "\x1b[38;2;0;0;0m" : "\x1b[38;2;255;255;255m";
        } else contrast = tui_fg_seq(NULL, TUI_TC_TEXT);
        sb_appendf(&t, "%s%s%s%s\x1b[1m %s \x1b[22m\x1b[49m%s%s\x1b[39m", fg, tui_sym("sep.powerlineRight"), bg, contrast,
                   labels->v[i], fg, tui_sym("sep.powerlineLeft"));
    }
    free(thin);
    return sb_detach(&t);
}

static void slider_lines(ui_plan_review_t *p, strvec_t *out) {
    if (!p->has_slider) return;
    int a = p->slider_index;
    char *track = segment_track(&p->seg_labels, a);
    char *la = tui_fgs(NULL, a > 0 ? TUI_TC_ACCENT : TUI_TC_DIM, "\xe2\x97\x82");
    char *ra = tui_fgs(NULL, a < (int)p->seg_labels.n - 1 ? TUI_TC_ACCENT : TUI_TC_DIM, "\xe2\x96\xb8");
    char *cap = p->slider_caption && *p->slider_caption ? tui_fgs(NULL, TUI_TC_DIM, p->slider_caption) : NULL;
    strvec_push_own(out, xasprintf("%s%s%s  %s  %s", cap ? cap : "", cap ? "  " : "", la, track, ra));
    const char *detail = a < (int)p->seg_details.n ? p->seg_details.v[a] : NULL;
    if (detail && *detail) {
        char *arrow = tui_fgs(NULL, TUI_TC_DIM, "\xe2\x86\xb3"), *d = tui_fgs(NULL, TUI_TC_MUTED, detail);
        strvec_push_own(out, xasprintf("  %s %s", arrow, d));
        free(d);
        free(arrow);
    }
    free(cap);
    free(ra);
    free(la);
    free(track);
}

static void option_lines(ui_plan_review_t *p, strvec_t *out) {
    bool active = p->focus == F_ACTIONS;
    for (size_t i = 0; i < p->options.n; i++) {
        bool sel = (int)i == p->selected, dis = p->disabled[i];
        char *cursor;
        if (sel) {
            char *c = xasprintf("%s ", tui_sym("nav.cursor"));
            cursor = tui_fgs(NULL, active ? TUI_TC_ACCENT : TUI_TC_DIM, c);
            free(c);
        } else cursor = xstrdup("  ");
        char *text;
        if (dis) text = tui_fgs(NULL, TUI_TC_DIM, p->options.v[i]);
        else if (sel && active) {
            char *a = tui_fgs(NULL, TUI_TC_ACCENT, p->options.v[i]);
            text = tui_bold(a);
            free(a);
        } else text = tui_fgs(NULL, TUI_TC_TEXT, p->options.v[i]);
        strvec_push_own(out, xasprintf("%s%s", cursor, text));
        free(text);
        free(cursor);
    }
}

static char *khint(const char *key) { return ui_key_hint(key); }

static char *khints(const char *a, const char *b) {
    char *x = ui_key_hint(a), *y = ui_key_hint(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static char *ekeys(const char *a, const char *b) {
    char *x = ui_editor_key(a), *y = ui_editor_key(b), *r = xasprintf("%s/%s", x, y);
    free(x);
    free(y);
    return r;
}

static char *build_help(ui_plan_review_t *p) {
    strvec_t parts = {0};
    char *ud = ekeys("tui.select.up", "tui.select.down"), *enter = khint("enter");
    switch (p->focus) {
    case F_ACTIONS:
        strvec_push_own(&parts, xasprintf("%s select", ud));
        strvec_push_own(&parts, xasprintf("%s confirm", enter));
        if (p->has_slider) {
            char *lr = khints("left", "right");
            strvec_push_own(&parts, xasprintf("%s model", lr));
            free(lr);
        }
        break;
    case F_TOC: {
        char *a = khint("a"), *e = khint("e"), *d = khint("d"), *u = khint("u");
        strvec_push_own(&parts, xasprintf("%s section", ud));
        strvec_push_own(&parts, xasprintf("%s open", enter));
        strvec_push_own(&parts, xasprintf("%s annotate", a));
        strvec_push_own(&parts, xasprintf("%s edit", e));
        strvec_push_own(&parts, xasprintf("%s delete", d));
        strvec_push_own(&parts, xasprintf("%s undo", u));
        free(a), free(e), free(d), free(u);
        break;
    }
    case F_BODY: {
        char *sh = khint("shift"), *pg = khints("pageup", "pagedown"), *gg = khints("g", "shift+g"), *a = khint("a"),
             *e = khint("e"), *u = khint("u");
        strvec_push_own(&parts, xasprintf("%s scroll", ud));
        strvec_push_own(&parts, xasprintf("%s faster", sh));
        strvec_push(&parts, pg);
        strvec_push_own(&parts, xasprintf("%s ends", gg));
        strvec_push_own(&parts, xasprintf("%s annotate", a));
        strvec_push_own(&parts, xasprintf("%s edit", e));
        strvec_push_own(&parts, xasprintf("%s undo", u));
        free(sh), free(pg), free(gg), free(a), free(e), free(u);
        break;
    }
    }
    if (p->cb.on_copy) {
        char *c = khint("c");
        strvec_push_own(&parts, xasprintf("%s copy", c));
        free(c);
    }
    char *tab = khint("tab");
    strvec_push_own(&parts, xasprintf("%s regions", tab));
    free(tab);
    if (p->editor_label && *p->editor_label && p->focus != F_TOC) strvec_push_own(&parts, xasprintf("%s editor", p->editor_label));
    if (p->help_suffix) strvec_push(&parts, p->help_suffix);
    else {
        char *c = ui_editor_key("tui.select.cancel");
        strvec_push_own(&parts, xasprintf("%s cancel", c));
        free(c);
    }
    char *r = strvec_join(&parts, " \xc2\xb7 ");
    strvec_free(&parts);
    free(ud);
    free(enter);
    return r;
}

static void chooser_lines(ui_plan_review_t *p, int inner, int max_rows, strvec_t *out) {
    if (!p->chooser_open || max_rows <= 0) return;
    int n = (int)p->chooser.n;
    int limit = max_rows == INT32_MAX ? n : MAX(0, max_rows - 2);
    int start = limit >= n ? 0 : MAX(0, MIN(p->chooser_sel - limit / 2, n - limit));
    strvec_t lines = {0};
    char *h = tui_fgs(NULL, TUI_TC_ACCENT, "Edit annotation");
    strvec_push_own(&lines, tui_bold(h));
    free(h);
    for (int w = 0; w < limit; w++) {
        int oi = start + w;
        if (oi >= n) continue;
        annsel_t s = p->chooser.v[oi];
        sec_t *sec = &p->secs.v[s.section];
        annot_t *a = &sec->ann.v[s.index];
        char *target = a->line ? xasprintf("line %s", a->context) : xstrdup("section");
        char *note = sanitize_status(a->note), *st = sanitize_status(*sec->title ? sec->title : "Plan preamble");
        char *marker = oi == p->chooser_sel ? tui_fgs(NULL, TUI_TC_ACCENT, "\xe2\x80\xba ") : xstrdup("  ");
        char *label = xasprintf("%s%s \xc2\xb7 %s \xc2\xb7 %s", marker, st, target, note);
        strvec_push_own(&lines, tui_truncate_to_width(label, inner, "\xe2\x80\xa6"));
        free(label), free(marker), free(st), free(note), free(target);
    }
    if (max_rows == INT32_MAX || max_rows >= 2) {
        char *ud = ekeys("tui.select.up", "tui.select.down"), *en = khint("enter"), *c = ui_editor_key("tui.select.cancel");
        char *t = xasprintf("%s choose \xc2\xb7 %s edit \xc2\xb7 %s cancel", ud, en, c);
        strvec_push_own(&lines, tui_fgs(NULL, TUI_TC_DIM, t));
        free(t), free(c), free(en), free(ud);
    }
    for (size_t i = 0; i < lines.n && (int)i < max_rows; i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
}

static void footer_lines(ui_plan_review_t *p, int inner, int max_chooser_rows, strvec_t *out) {
    if (p->chooser_open) {
        chooser_lines(p, inner, max_chooser_rows, out);
        return;
    }
    if (p->annotating) {
        sec_t *s = p->target_set && p->target_section < (int)p->secs.n ? &p->secs.v[p->target_section] : NULL;
        char *title = sanitize_status(s && *s->title ? s->title : "Plan preamble");
        char *loc;
        if (!p->target_line) loc = xasprintf("\xe2\x80\xb9%s\xe2\x80\xba", title);
        else {
            char *c = tui_truncate_to_width(p->target_context ? p->target_context : "", MAX(1, inner - 16), "\xe2\x80\xa6");
            loc = xasprintf("\xe2\x80\xb9%s\xe2\x80\xba \xc2\xb7 %s", title, c);
            free(c);
        }
        char *dl = tui_fgs(NULL, TUI_TC_DIM, "Annotate"), *al = tui_fgs(NULL, TUI_TC_ACCENT, loc);
        char *cap = xasprintf("%s %s", dl, al);
        strvec_push_own(out, tui_truncate_to_width(cap, inner, "\xe2\x80\xa6"));
        free(cap), free(al), free(dl), free(loc), free(title);
        p->editor->max_height = MAX(1, MIN(MAX_ANNOTATION_EDITOR_ROWS, term_rows(p) - 12));
        p->editor->base.focused = true;
        tui_component_render(&p->editor->base, inner, out);
        char *sub = ui_editor_key("tui.input.submit"), *nl = ui_editor_key("tui.input.newLine"),
             *c = ui_editor_key("tui.select.cancel");
        sb_t h = SB_INIT;
        sb_appendf(&h, "%s save \xc2\xb7 %s newline \xc2\xb7 %s cancel", sub, nl, c);
        if (p->editing) sb_append(&h, " \xc2\xb7 empty deletes");
        if (p->editor_label && *p->editor_label) sb_appendf(&h, " \xc2\xb7 %s editor", p->editor_label);
        strvec_push_own(out, tui_fgs(NULL, TUI_TC_DIM, sb_cstr(&h)));
        sb_free(&h);
        free(sub), free(nl), free(c);
        return;
    }
    char *help = build_help(p);
    strvec_push_own(out, tui_fgs(NULL, TUI_TC_DIM, help));
    free(help);
}

/* ScrollView(scrollbar auto, ellipsis Omit, track dim / thumb accent) over the body lines */
static void scroll_render(ui_plan_review_t *p, int width, strvec_t *out) {
    int total = (int)p->body_lines.n, h = p->height;
    set_scroll(p, p->scroll);
    bool bar = total > h && width > 1;
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / total, h)), maxoff = max_offset(p);
        ts = maxoff ? (int)lround((double)p->scroll / maxoff * (h - size)) : 0;
        te = ts + size;
    }
    for (int r = 0; r < h; r++) {
        int i = p->scroll + r;
        const char *src = i < total ? p->body_lines.v[i] : "";
        char *rt = tui_replace_tabs(src), *tr = tui_truncate_to_width(rt, cw, "");
        free(rt);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        char *pad = tui_pad_to_width(tr, cw);
        bool thumb = r >= ts && r < te;
        char *g = tui_fgs(NULL, thumb ? TUI_TC_ACCENT : TUI_TC_DIM, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", pad, g));
        free(g);
        free(pad);
        free(tr);
    }
}

static void pr_render(tui_component_t *c, int width, strvec_t *out) {
    ui_plan_review_t *p = (ui_plan_review_t *)c;
    int rows = term_rows(p);
    bool side = sidebar_visible(p, width);
    p->sidebar_shown = side;
    int sw = side ? sidebar_width_for(width) : 0;
    int inner = MAX(1, width - 4);
    int body_w = side ? split_body_width(width, sw) : inner;

    strvec_t slider = {0}, opts = {0}, prompt = {0}, footer = {0};
    if (!p->committed) slider_lines(p, &slider);
    if (p->committed) {
        char *l = p->committed_label ? xasprintf("%s \xe2\x80\x94 submitting\xe2\x80\xa6", p->committed_label)
                                     : xstrdup("Submitting\xe2\x80\xa6");
        char *a = tui_fgs(NULL, TUI_TC_ACCENT, l);
        strvec_push_own(&opts, tui_bold(a));
        free(a);
        free(l);
    } else option_lines(p, &opts);
    if (p->prompt_title && *p->prompt_title) {
        char *a = tui_fgs(NULL, TUI_TC_ACCENT, p->prompt_title);
        strvec_push_own(&prompt, tui_bold(a));
        free(a);
    }
    int base_chrome = 4 + (int)prompt.n + (int)slider.n + (int)opts.n;
    int chooser_rows = p->chooser_open ? MAX(0, rows - base_chrome - MIN_BODY_ROWS) : INT32_MAX;
    if (p->committed)
        strvec_push_own(&footer, tui_fgs(NULL, TUI_TC_DIM, "Applying your selection \xe2\x80\x94 this can take a moment while context is prepared."));
    else footer_lines(p, inner, chooser_rows, &footer);
    int chrome = base_chrome + (int)footer.n;
    int region = p->chooser_open ? MAX(0, rows - chrome) : MAX(MIN_BODY_ROWS, rows - chrome);

    /* layoutBody: keep the relative scroll position across reflows */
    capture_progress(p);
    build_body(p, body_w);
    p->height = region;
    int m = max_offset(p);
    if (m > 0) set_scroll(p, (int)lround(p->scroll_progress * m));
    if (p->pending_scroll_to_toc) {
        p->pending_scroll_to_toc = false;
        scrub_body_to_toc(p);
    }
    if (p->focus != F_TOC) p->toc_cursor = toc_cursor_from_scroll(p);
    strvec_t body = {0};
    scroll_render(p, body_w, &body);

    if (side) {
        strvec_push_own(out, top_border_split(width, OVERLAY_TITLE, sw));
        int total = (int)p->toc.n, start = 0;
        if (total > region) start = MAX(0, MIN(p->toc_cursor - region / 2, total - region));
        for (int i = 0; i < region; i++) {
            int pos = start + i;
            char *sl = pos < total ? toc_entry(p, pos, sw) : xstrdup("");
            strvec_push_own(out, split_row(sl, i < (int)body.n ? body.v[i] : "", width, sw));
            free(sl);
        }
        strvec_push_own(out, divider_split(width, sw));
    } else {
        strvec_push_own(out, ui_box_top(width, OVERLAY_TITLE));
        for (size_t i = 0; i < body.n; i++) strvec_push_own(out, ui_box_row(body.v[i], width));
        strvec_push_own(out, ui_box_divider(width));
    }
    for (size_t i = 0; i < prompt.n; i++) strvec_push_own(out, ui_box_row(prompt.v[i], width));
    for (size_t i = 0; i < slider.n; i++) strvec_push_own(out, ui_box_row(slider.v[i], width));
    for (size_t i = 0; i < opts.n; i++) strvec_push_own(out, ui_box_row(opts.v[i], width));
    strvec_push_own(out, ui_box_divider(width));
    for (size_t i = 0; i < footer.n; i++) strvec_push_own(out, ui_box_row(footer.v[i], width));
    strvec_push_own(out, ui_box_bottom(width));
    strvec_free(&body);
    strvec_free(&slider);
    strvec_free(&opts);
    strvec_free(&prompt);
    strvec_free(&footer);
}

/* ---------- input ---------- */

static bool key_is(const tui_key_t *k, const char *ch) { return k->text && !k->mods && !strcmp(k->text, ch); }

static void handle_actions(ui_plan_review_t *p, const tui_key_t *k) {
    bool left = tui_key_matches(k, "left") || (p->has_slider && tui_key_matches(k, "h"));
    bool right = tui_key_matches(k, "right") || (p->has_slider && tui_key_matches(k, "l"));
    if (left) move_slider(p, -1);
    else if (right) move_slider(p, 1);
    else if (tui_kb(k, "tui.select.up") || tui_key_matches(k, "k")) {
        if (p->selected == first_enabled(p)) set_focus(p, F_BODY);
        else move_selection(p, -1);
    } else if (tui_kb(k, "tui.select.down") || tui_key_matches(k, "j")) move_selection(p, 1);
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return")) confirm_selection(p);
    else body_scroll(p, k);
}

static void handle_body(ui_plan_review_t *p, const tui_key_t *k) {
    if (key_is(k, "a")) start_body_annotate(p);
    else if (tui_key_matches(k, "left") || tui_key_matches(k, "h")) {
        if (p->sidebar_shown) set_focus(p, F_TOC);
    } else if (tui_key_matches(k, "right") || tui_key_matches(k, "l") || tui_key_matches(k, "enter") || tui_key_matches(k, "return"))
        set_focus(p, F_ACTIONS);
    else if (tui_kb(k, "tui.select.up") || tui_key_matches(k, "k")) {
        if (p->scroll <= 0 && p->sidebar_shown) set_focus(p, F_TOC);
        else {
            set_scroll(p, p->scroll - 1);
            capture_progress(p);
        }
    } else if (tui_kb(k, "tui.select.down") || tui_key_matches(k, "j")) {
        if (p->scroll >= max_offset(p)) set_focus(p, F_ACTIONS);
        else {
            set_scroll(p, p->scroll + 1);
            capture_progress(p);
        }
    } else body_scroll(p, k);
}

static void handle_toc(ui_plan_review_t *p, const tui_key_t *k) {
    if (tui_kb(k, "tui.select.up") || tui_key_matches(k, "k")) move_toc_cursor(p, -1);
    else if (tui_kb(k, "tui.select.down") || tui_key_matches(k, "j")) {
        if (p->toc_cursor >= (int)p->toc.n - 1) set_focus(p, F_ACTIONS);
        else move_toc_cursor(p, 1);
    } else if (tui_key_matches(k, "right") || tui_key_matches(k, "l") || tui_key_matches(k, "enter") || tui_key_matches(k, "return"))
        set_focus(p, F_BODY);
    else if (key_is(k, "d") || tui_key_matches(k, "delete")) delete_selected_section(p);
    else if (key_is(k, "a")) start_section_annotate(p);
}

static bool pr_key(tui_component_t *c, const tui_key_t *k) {
    ui_plan_review_t *p = (ui_plan_review_t *)c;
    if (p->committed) return true;
    if (k->kind == TUI_KEY_MOUSE) return true;
    if (p->chooser_open) {
        handle_chooser(p, k);
    } else if (p->annotating) {
        if (p->cb.on_annotation_editor && tui_kb(k, "app.editor.external")) {
            char *draft = tui_editor_get_expanded_text(p->editor);
            p->cb.on_annotation_editor(p->cb.ud, draft);
            free(draft);
        } else if (tui_kb(k, "tui.select.cancel")) exit_annotate(p);
        else tui_editor_handle_key(p->editor, k);
    } else if (tui_kb(k, "tui.select.cancel")) {
        p->committed = true;
        if (p->cb.on_cancel) p->cb.on_cancel(p->cb.ud);
        return true;
    } else if (p->focus != F_ACTIONS && key_is(k, "u")) undo_last(p);
    else if (p->focus != F_ACTIONS && key_is(k, "e")) edit_current_annotation(p);
    else if (p->cb.on_external_editor && tui_kb(k, "app.editor.external")) p->cb.on_external_editor(p->cb.ud);
    else if (p->cb.on_copy && key_is(k, "c")) {
        char *t = join_sections(p);
        p->cb.on_copy(p->cb.ud, t);
        free(t);
    } else if (tui_key_matches(k, "tab")) cycle_region(p, 1);
    else if (tui_key_matches(k, "shift+tab")) cycle_region(p, -1);
    else if (p->focus == F_ACTIONS) handle_actions(p, k);
    else if (p->focus == F_BODY) handle_body(p, k);
    else handle_toc(p, k);
    tui_component_request_render(c);
    return true;
}

static void pr_invalidate(tui_component_t *c) {
    ui_plan_review_t *p = (ui_plan_review_t *)c;
    for (size_t i = 0; i < p->secs.n; i++) tui_component_invalidate(&p->secs.v[i].md->base);
}

static void pr_destroy(tui_component_t *c) {
    ui_plan_review_t *p = (ui_plan_review_t *)c;
    secs_clear(p);
    vec_free(&p->secs);
    vec_free(&p->toc);
    vec_free(&p->sec_offsets);
    vec_free(&p->anchors);
    strvec_free(&p->ctx_texts);
    strvec_free(&p->body_lines);
    while (p->undo.n) {
        undo_t u = vec_pop(&p->undo);
        free(u.text);
        for (size_t i = 0; i < u.nsec; i++) annots_free(&u.ann[i]);
        free(u.ann);
        strvec_free(&u.deleted);
    }
    vec_free(&p->undo);
    strvec_free(&p->deleted);
    strvec_free(&p->options);
    free(p->disabled);
    free(p->help_suffix);
    free(p->editor_label);
    free(p->prompt_title);
    free(p->slider_caption);
    strvec_free(&p->seg_labels);
    strvec_free(&p->seg_details);
    free(p->committed_label);
    free(p->target_context);
    vec_free(&p->chooser);
    tui_component_destroy(&p->editor->base);
    free(p);
}

static const tui_component_vt PR_VT = {"plan-review", pr_render, pr_key, pr_invalidate, pr_destroy};

ui_plan_review_t *ui_plan_review_new(tui_t *tui, const char *plan, const ui_plan_review_opts_t *o,
                                     const ui_plan_review_cb_t *cb) {
    ui_plan_review_t *p = xcalloc(1, sizeof *p);
    tui_component_init(&p->base, &PR_VT);
    p->tui = tui;
    p->cb = *cb;
    for (size_t i = 0; i < o->noptions; i++) strvec_push(&p->options, o->options[i]);
    p->disabled = xcalloc(MAX(1, o->noptions), sizeof(bool));
    for (size_t i = 0; i < o->ndisabled; i++)
        if (o->disabled[i] >= 0 && (size_t)o->disabled[i] < o->noptions) p->disabled[o->disabled[i]] = true;
    p->help_suffix = o->help_text ? xstrdup(o->help_text) : NULL;
    p->editor_label = o->external_editor_label ? xstrdup(o->external_editor_label) : NULL;
    p->prompt_title = o->prompt_title ? xstrdup(o->prompt_title) : NULL;
    p->selected = coerce_index(p, o->initial_index);
    if (o->nsegments > 0) {
        p->has_slider = true;
        p->slider_caption = o->slider_caption ? xstrdup(o->slider_caption) : NULL;
        for (size_t i = 0; i < o->nsegments; i++) {
            strvec_push(&p->seg_labels, o->segment_labels[i]);
            strvec_push(&p->seg_details, o->segment_details && o->segment_details[i] ? o->segment_details[i] : "");
        }
        p->slider_index = MAX(0, MIN(o->slider_index, (int)o->nsegments - 1));
    }
    p->focus = F_ACTIONS;
    p->height = MIN_BODY_ROWS;
    p->editor = tui_editor_new();
    tui_editor_set_composer(p->editor, "borderless");
    free(p->editor->gutter);
    p->editor->gutter = xstrdup("> ");
    p->editor->padding_x = 0;
    p->editor->use_terminal_cursor = false;
    p->editor->eol_cursor = str_eq(settings_str("symbolPreset", "unicode"), "ascii") ? "|" : "\xe2\x96\x8f";
    if (g_ui && g_ui->editor) snprintf(p->editor->text_fg, sizeof p->editor->text_fg, "%s", g_ui->editor->text_fg);
    tui_editor_set_on_submit(p->editor, submit_annotation, p);
    p->editor->base.tui = tui;
    set_sections(p, plan);
    if (o->annotation_state) {
        restore_annotation_state(p, o->annotation_state);
        if (json_len(json_get(o->annotation_state, "annotations")) > 0) recompute_feedback(p);
    }
    return p;
}

tui_component_t *ui_plan_review_component(ui_plan_review_t *p) { return &p->base; }

void ui_plan_review_set_plan(ui_plan_review_t *p, const char *plan) {
    json_t *st = annotation_state(p);
    set_sections(p, plan);
    restore_annotation_state(p, st);
    json_free(st);
    p->scroll = 0;
    p->scroll_progress = 0;
    p->toc_cursor = 0;
    strvec_free(&p->deleted);
    while (p->undo.n) {
        undo_t u = vec_pop(&p->undo);
        free(u.text);
        for (size_t i = 0; i < u.nsec; i++) annots_free(&u.ann[i]);
        free(u.ann);
        strvec_free(&u.deleted);
    }
    recompute_feedback(p);
    tui_component_request_render(&p->base);
}

void ui_plan_review_set_annotation_draft(ui_plan_review_t *p, const char *text) {
    if (p->annotating && text) tui_editor_set_text(p->editor, text);
    tui_component_request_render(&p->base);
}

void ui_plan_review_set_slider(ui_plan_review_t *p, int index) {
    if (p->committed || !p->has_slider) return;
    move_slider(p, index - p->slider_index);
}

bool ui_plan_review_select(ui_plan_review_t *p, const char *label) {
    if (p->committed) return false;
    for (size_t i = 0; i < p->options.n; i++) {
        if (strcmp(p->options.v[i], label) != 0) continue;
        if (p->disabled[i]) return false;
        p->selected = (int)i;
        confirm_selection(p);
        tui_component_request_render(&p->base);
        return p->committed;
    }
    return false;
}

/* ---------- plan-save-overlay.ts ---------- */

struct ui_plan_save {
    tui_component_t base;
    sb_t value;
    size_t cursor; /* byte offset */
    char *suggested;
    void (*done)(void *ud, const char *path);
    void *ud;
    bool finished;
};

static void ps_render(tui_component_t *c, int width, strvec_t *out) {
    ui_plan_save_t *s = (ui_plan_save_t *)c;
    int inner = MAX(0, width - 4);
    char *prompt = tui_fgs(NULL, TUI_TC_DIM, "Path: ");
    char *input;
    int avail = inner - tui_visible_width(prompt);
    if (!s->value.len) {
        if (avail <= 0) input = xstrdup(prompt);
        else {
            char *sug = tui_truncate_to_width(s->suggested, MAX(0, avail - 1), "\xe2\x80\xa6");
            char *d = tui_fgs(NULL, TUI_TC_DIM, sug);
            input = xasprintf("%s%s\x1b[7m \x1b[27m%s", prompt, c->focused ? TUI_CURSOR_MARKER : "", d);
            free(d);
            free(sug);
        }
    } else {
        /* Input: text with a reverse-video cursor cell, scrolled to keep the cursor visible */
        const char *v = sb_cstr(&s->value);
        char *before = xstrndup(v, s->cursor);
        const char *at = v + s->cursor;
        size_t cl = 0;
        if (*at) {
            cl = 1;
            while (at[cl] && ((unsigned char)at[cl] & 0xC0) == 0x80) cl++;
        }
        char *cur = cl ? xstrndup(at, cl) : xstrdup(" ");
        char *after = xstrdup(at + cl);
        int bw = tui_visible_width(before);
        int room = MAX(1, avail - 1);
        char *shown_before = bw > room ? tui_slice_cols(before, bw - room, room) : xstrdup(before);
        input = xasprintf("%s%s%s\x1b[7m%s\x1b[27m%s", prompt, shown_before, c->focused ? TUI_CURSOR_MARKER : "", cur, after);
        char *t = tui_truncate_to_width(input, inner, "");
        free(input);
        input = t;
        free(shown_before);
        free(after);
        free(cur);
        free(before);
    }
    char *sub = ui_editor_key("tui.input.submit"), *can = ui_editor_key("tui.select.cancel");
    char *hint = xasprintf("%s save and quit \xc2\xb7 %s cancel", sub, can);
    char *dh = tui_fgs(NULL, TUI_TC_DIM, hint);
    strvec_push_own(out, ui_box_top(width, "Save and quit"));
    strvec_push_own(out, ui_box_row(input, width));
    strvec_push_own(out, ui_box_row(dh, width));
    strvec_push_own(out, ui_box_bottom(width));
    free(dh), free(hint), free(sub), free(can), free(input), free(prompt);
}

static void ps_finish(ui_plan_save_t *s, const char *path) {
    if (s->finished) return;
    s->finished = true;
    s->done(s->ud, path);
}

static bool ps_key(tui_component_t *c, const tui_key_t *k) {
    ui_plan_save_t *s = (ui_plan_save_t *)c;
    if (s->finished) return true;
    if (tui_kb(k, "tui.select.cancel")) {
        ps_finish(s, NULL);
        return true;
    }
    if (tui_kb(k, "tui.input.submit") || tui_key_matches(k, "enter")) {
        char *t = str_trim(sb_cstr(&s->value));
        ps_finish(s, *t ? t : s->suggested);
        free(t);
        return true;
    }
    const char *v = sb_cstr(&s->value);
    if (tui_key_matches(k, "backspace")) {
        if (s->cursor > 0) {
            size_t st = s->cursor - 1;
            while (st > 0 && ((unsigned char)v[st] & 0xC0) == 0x80) st--;
            char *rest = xstrdup(v + s->cursor);
            sb_truncate(&s->value, st);
            sb_append(&s->value, rest);
            free(rest);
            s->cursor = st;
        }
    } else if (tui_key_matches(k, "delete")) {
        if (v[s->cursor]) {
            size_t en = s->cursor + 1;
            while (v[en] && ((unsigned char)v[en] & 0xC0) == 0x80) en++;
            char *rest = xstrdup(v + en);
            sb_truncate(&s->value, s->cursor);
            sb_append(&s->value, rest);
            free(rest);
        }
    } else if (tui_key_matches(k, "left")) {
        while (s->cursor > 0) {
            s->cursor--;
            if (((unsigned char)v[s->cursor] & 0xC0) != 0x80) break;
        }
    } else if (tui_key_matches(k, "right")) {
        if (v[s->cursor]) {
            s->cursor++;
            while (v[s->cursor] && ((unsigned char)v[s->cursor] & 0xC0) == 0x80) s->cursor++;
        }
    } else if (tui_key_matches(k, "home") || tui_key_matches(k, "ctrl+a")) s->cursor = 0;
    else if (tui_key_matches(k, "end") || tui_key_matches(k, "ctrl+e")) s->cursor = s->value.len;
    else if (tui_key_matches(k, "ctrl+u")) {
        char *rest = xstrdup(v + s->cursor);
        sb_clear(&s->value);
        sb_append(&s->value, rest);
        free(rest);
        s->cursor = 0;
    } else if ((k->kind == TUI_KEY_PASTE || (k->text && !(k->mods & ~TUI_MOD_SHIFT))) && k->text) {
        char *rest = xstrdup(v + s->cursor);
        sb_truncate(&s->value, s->cursor);
        for (const char *q = k->text; *q; q++)
            if ((unsigned char)*q >= 0x20 && *q != 0x7f) sb_putc(&s->value, *q);
        size_t nc = s->value.len;
        sb_append(&s->value, rest);
        free(rest);
        s->cursor = nc;
    }
    tui_component_request_render(c);
    return true;
}

static void ps_destroy(tui_component_t *c) {
    ui_plan_save_t *s = (ui_plan_save_t *)c;
    sb_free(&s->value);
    free(s->suggested);
    free(s);
}

static const tui_component_vt PS_VT = {"plan-save", ps_render, ps_key, NULL, ps_destroy};

ui_plan_save_t *ui_plan_save_new(const char *suggested, void (*done)(void *ud, const char *path), void *ud) {
    ui_plan_save_t *s = xcalloc(1, sizeof *s);
    tui_component_init(&s->base, &PS_VT);
    s->suggested = xstrdup(suggested ? suggested : "PLAN.md");
    s->done = done;
    s->ud = ud;
    return s;
}

tui_component_t *ui_plan_save_component(ui_plan_save_t *s) { return &s->base; }

void ui_plan_save_set_suggested(ui_plan_save_t *s, const char *path) {
    free(s->suggested);
    s->suggested = xstrdup(path ? path : "PLAN.md");
    tui_component_request_render(&s->base);
}

/* ---------- showHookConfirm: HookSelectorComponent with Yes / No in the composer's place ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    char *title, *message;
    int sel;
    bool done;
    void (*cb)(void *ud, bool yes);
    void *ud;
} hook_confirm_t;

static void hc_render(tui_component_t *c, int width, strvec_t *out) {
    hook_confirm_t *h = (hook_confirm_t *)c;
    int inner = MAX(1, width - 4);
    strvec_push_own(out, ui_box_top(width, h->title));
    strvec_push_own(out, ui_box_row("", width));
    strvec_t ml = {0};
    char *acc = tui_fgs(NULL, TUI_TC_ACCENT, h->message);
    ui_push_text(&ml, acc, inner);
    free(acc);
    for (size_t i = 0; i < ml.n; i++) strvec_push_own(out, ui_box_row(ml.v[i], width));
    strvec_free(&ml);
    strvec_push_own(out, ui_box_row("", width));
    static const char *const OPTS[] = {"Yes", "No"};
    for (int i = 0; i < 2; i++) {
        if (i == h->sel) {
            char *l = xasprintf("%s %s", tui_sym("nav.cursor"), OPTS[i]);
            char *a = tui_fgs(NULL, TUI_TC_ACCENT, l);
            char *line = xasprintf(" %s", a);
            char *pad = tui_pad_to_width(line, inner);
            char *bg = tui_bgs(NULL, TUI_TC_SELECTED_BG, pad);
            strvec_push_own(out, ui_box_row(bg, width));
            free(bg), free(pad), free(line), free(a), free(l);
        } else {
            char *line = xasprintf("   %s", OPTS[i]);
            strvec_push_own(out, ui_box_row(line, width));
            free(line);
        }
    }
    strvec_push_own(out, ui_box_row("", width));
    char *ud = xasprintf("%s/%s", tui_sym("key.up"), tui_sym("key.down"));
    char *en = ui_key_hint("enter"), *esc = ui_editor_key("tui.select.cancel");
    char *hint = xasprintf("%s navigate  %s select  %s cancel", ud, en, esc);
    char *dh = tui_fgs(NULL, TUI_TC_DIM, hint);
    strvec_push_own(out, ui_box_row(dh, width));
    strvec_push_own(out, ui_box_row("", width));
    strvec_push_own(out, ui_box_bottom(width));
    free(dh), free(hint), free(esc), free(en), free(ud);
}

typedef struct {
    hook_confirm_t *h;
    bool yes;
} hc_done_t;

static void hc_finish_posted(void *p) {
    hc_done_t *d = p;
    hook_confirm_t *h = d->h;
    void (*cb)(void *, bool) = h->cb;
    void *ud = h->ud;
    bool yes = d->yes;
    ui_close_selector(h->u); /* destroys h */
    free(d);
    if (cb) cb(ud, yes);
}

static void hc_finish(hook_confirm_t *h, bool yes) {
    if (h->done) return;
    h->done = true;
    hc_done_t *d = xcalloc(1, sizeof *d);
    d->h = h;
    d->yes = yes;
    tui_post(h->u->tui, hc_finish_posted, d);
}

static bool hc_key(tui_component_t *c, const tui_key_t *k) {
    hook_confirm_t *h = (hook_confirm_t *)c;
    if (h->done) return true;
    if (tui_kb(k, "tui.select.cancel")) hc_finish(h, false);
    else if (tui_kb(k, "tui.select.up")) h->sel = (h->sel + 1) % 2;
    else if (tui_kb(k, "tui.select.down")) h->sel = (h->sel + 1) % 2;
    else if (tui_kb(k, "tui.select.confirm") || tui_key_matches(k, "enter")) hc_finish(h, h->sel == 0);
    tui_component_request_render(c);
    return true;
}

static void hc_destroy(tui_component_t *c) {
    hook_confirm_t *h = (hook_confirm_t *)c;
    free(h->title);
    free(h->message);
    free(h);
}

static const tui_component_vt HC_VT = {"hook-confirm", hc_render, hc_key, NULL, hc_destroy};

void ui_hook_confirm(ui_t *u, const char *title, const char *message, void (*cb)(void *ud, bool yes), void *ud) {
    hook_confirm_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HC_VT);
    h->u = u;
    h->title = xstrdup(title);
    h->message = xstrdup(message);
    h->cb = cb;
    h->ud = ud;
    ui_show_selector(u, &h->base, &h->base);
}
