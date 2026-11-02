/* editor.c - multi-line editor component with undo, kill/yank, history, paste markers and
 * autocomplete. */
#include "tui/editor.h"
#include "core/fs.h"

#include <ctype.h>

enum { ACT_NONE, ACT_TYPE, ACT_OTHER };
#define UNDO_MAX 200
#define HISTORY_MAX 100

/* ---------- autocomplete result ---------- */

void tui_ac_result_add(tui_ac_result_t *r, const char *value, const char *label, const char *description) {
    tui_ac_result_add_icon(r, value, label, description, NULL);
}

void tui_ac_result_add_icon(tui_ac_result_t *r, const char *value, const char *label, const char *description,
                            const char *icon) {
    tui_select_item_t it = {xstrdup(value ? value : ""), label ? xstrdup(label) : NULL,
                            description && *description ? xstrdup(description) : NULL, icon && *icon ? xstrdup(icon) : NULL,
                            NULL, false};
    vec_push(&r->items, it);
}

void tui_ac_result_free(tui_ac_result_t *r) {
    for (size_t i = 0; i < r->items.n; i++) {
        free(r->items.v[i].value);
        free(r->items.v[i].label);
        free(r->items.v[i].description);
        free(r->items.v[i].icon);
        free(r->items.v[i].hint);
    }
    vec_free(&r->items);
    r->prefix_len = 0;
}

/* ---------- slash-command scoring (upstream autocomplete.ts) ---------- */

static bool ac_subsequence(const char *q, const char *t) {
    size_t ql = strlen(q), tl = strlen(t);
    if (!ql) return true;
    if (ql > tl) return false;
    size_t qi = 0;
    for (size_t ti = 0; ti < tl && qi < ql; ti++)
        if (q[qi] == t[ti]) qi++;
    return qi == ql;
}

static double ac_subsequence_score(const char *q, const char *t) {
    if (!*q) return 1;
    if (!strcmp(t, q)) return 100;
    if (str_starts(t, q)) return 80;
    if (strstr(t, q)) return 60;
    size_t ql = strlen(q), qi = 0;
    int gaps = 0;
    long last = -1;
    for (size_t ti = 0; t[ti] && qi < ql; ti++)
        if (q[qi] == t[ti]) {
            if (last >= 0 && (long)ti - last > 1) gaps++;
            last = (long)ti;
            qi++;
        }
    if (qi != ql) return 0;
    return MAX(1, 40 - gaps * 5);
}

static double ac_command_score(const char *q, const char *t) {
    if (!*q) return 1;
    if (!strcmp(q, t)) return 1000;
    if (str_starts(t, q)) return 900;
    return ac_subsequence(q, t) ? ac_subsequence_score(q, t) : 0;
}

/* ---------- default provider ---------- */

tui_ac_default_t *tui_ac_default_new(const char *cwd) {
    tui_ac_default_t *p = xcalloc(1, sizeof *p);
    p->cwd = cwd ? xstrdup(cwd) : NULL;
    p->max_items = 50;
    return p;
}

void tui_ac_default_add_command(tui_ac_default_t *p, const char *name, const char *description) {
    tui_ac_default_add_command_icon(p, name, description, NULL);
}

void tui_ac_default_add_command_icon(tui_ac_default_t *p, const char *name, const char *description, const char *icon) {
    strvec_push(&p->commands, name);
    strvec_push(&p->descriptions, description ? description : "");
    strvec_push(&p->icons, icon ? icon : "");
}

void tui_ac_default_free(tui_ac_default_t *p) {
    if (!p) return;
    strvec_free(&p->commands);
    strvec_free(&p->descriptions);
    strvec_free(&p->icons);
    free(p->cwd);
    free(p);
}

static bool complete_path(tui_ac_default_t *p, const char *token, size_t token_len, bool at, tui_ac_result_t *out) {
    /* token excludes the '@' */
    char *tok = xstrndup(token, token_len);
    const char *slash = strrchr(tok, '/');
    char *dir_part = slash ? xstrndup(tok, (size_t)(slash - tok) + 1) : xstrdup("");
    const char *base = slash ? slash + 1 : tok;
    char *cwd = p->cwd ? xstrdup(p->cwd) : get_cwd();
    char *dir_abs;
    if (!*dir_part) dir_abs = xstrdup(cwd ? cwd : ".");
    else dir_abs = path_abs(dir_part, cwd ? cwd : ".");
    dirlist_t dl = {0};
    bool ok = false;
    if (fs_list_dir(dir_abs, &dl) == 0) {
        /* directories first, then files; prefix match (case-insensitive) */
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < dl.n && (int)out->items.n < p->max_items; i++) {
                dirent_info_t *e = &dl.v[i];
                if ((pass == 0) != e->is_dir) continue;
                if (e->name[0] == '.' && base[0] != '.') continue;
                if (strncasecmp(e->name, base, strlen(base)) != 0) continue;
                char *val = xasprintf("%s%s%s%s", at ? "@" : "", dir_part, e->name, e->is_dir ? "/" : "");
                char *label = xasprintf("%s%s", e->name, e->is_dir ? "/" : "");
                tui_ac_result_add(out, val, label, NULL);
                free(val);
                free(label);
            }
        }
        ok = out->items.n > 0;
        out->prefix_len = token_len + (at ? 1 : 0);
    }
    dirlist_free(&dl);
    free(dir_abs);
    free(cwd);
    free(dir_part);
    free(tok);
    return ok;
}

bool tui_ac_default_provider(void *ud, const char *line_before, bool first_line, bool forced, tui_ac_result_t *out) {
    tui_ac_default_t *p = ud;
    size_t len = strlen(line_before);
    /* slash commands: "/xyz" alone on the first line (upstream buildSlashCommandCompletions ranking:
     * exact 1000, prefix 900, else subsequence tiers; a description subsequence scores half) */
    if (first_line && line_before[0] == '/' && !strpbrk(line_before, " \t")) {
        char *q = str_lower(line_before + 1);
        size_t n = p->commands.n;
        double *score = xcalloc(n ? n : 1, sizeof *score);
        size_t *order = xcalloc(n ? n : 1, sizeof *order);
        bool *desc_only = xcalloc(n ? n : 1, sizeof *desc_only);
        size_t m = 0;
        for (size_t i = 0; i < n; i++) {
            char *ln = str_lower(p->commands.v[i]), *ld = str_lower(p->descriptions.v[i]);
            double ns = ac_command_score(q, ln);
            double ds = *ld && ac_subsequence(q, ld) ? ac_subsequence_score(q, ld) * 0.5 : 0;
            double s = ns > ds ? ns : ds;
            desc_only[i] = ns <= 0;
            free(ln);
            free(ld);
            if (s <= 0) continue;
            score[i] = s;
            order[m++] = i;
        }
        /* stable sort by score desc */
        for (size_t a = 1; a < m; a++)
            for (size_t b = a; b > 0 && score[order[b]] > score[order[b - 1]]; b--) {
                size_t t = order[b];
                order[b] = order[b - 1];
                order[b - 1] = t;
            }
        for (size_t k = 0; k < m && (int)out->items.n < p->max_items; k++) {
            size_t i = order[k];
            char *val = xasprintf("/%s ", p->commands.v[i]);
            char *live = p->describe ? p->describe(p->describe_ud, p->commands.v[i]) : NULL;
            tui_ac_result_add_icon(out, val, p->commands.v[i], live ? live : p->descriptions.v[i],
                                   i < p->icons.n ? p->icons.v[i] : NULL);
            free(live);
            free(val);
        }
        if (m) out->first_desc_match = desc_only[order[0]];
        free(desc_only);
        free(score);
        free(order);
        free(q);
        out->prefix_len = len;
        out->slash = true;
        return out->items.n > 0;
    }
    /* current token */
    size_t s = len;
    while (s > 0 && line_before[s - 1] != ' ' && line_before[s - 1] != '\t') s--;
    const char *tok = line_before + s;
    size_t tl = len - s;
    if (tl > 0 && tok[0] == '@') return complete_path(p, tok + 1, tl - 1, true, out);
    if (forced && tl > 0) return complete_path(p, tok, tl, false, out);
    return false;
}

/* ---------- helpers ---------- */

static char *cur_line(tui_editor_t *ed) { return ed->lines.v[ed->cl]; }

static void notify_change(tui_editor_t *ed) {
    if (ed->on_change) ed->on_change(ed, ed->cb_ud);
    tui_component_request_render(&ed->base);
}

static size_t prev_char(const char *s, size_t i) {
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t next_char(const char *s, size_t i) {
    size_t n = strlen(s);
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void set_line(tui_editor_t *ed, size_t idx, char *s) {
    free(ed->lines.v[idx]);
    ed->lines.v[idx] = s;
}

static void insert_line(tui_editor_t *ed, size_t idx, char *s) {
    vec_push(&ed->lines, NULL);
    memmove(ed->lines.v + idx + 1, ed->lines.v + idx, (ed->lines.n - 1 - idx) * sizeof(char *));
    ed->lines.v[idx] = s;
}

static void remove_line(tui_editor_t *ed, size_t idx) {
    free(ed->lines.v[idx]);
    memmove(ed->lines.v + idx, ed->lines.v + idx + 1, (ed->lines.n - idx - 1) * sizeof(char *));
    ed->lines.n--;
}

static void snapshot_free(tui_editor_snapshot_t *s) { strvec_free(&s->lines); }

static void push_undo(tui_editor_t *ed, int action) {
    if (action == ACT_TYPE && ed->last_action == ACT_TYPE) return;
    ed->last_action = action;
    tui_editor_snapshot_t s = {{0}, ed->cl, ed->cc};
    for (size_t i = 0; i < ed->lines.n; i++) strvec_push(&s.lines, ed->lines.v[i]);
    if (ed->undo.n >= UNDO_MAX) {
        snapshot_free(&ed->undo.v[0]);
        memmove(ed->undo.v, ed->undo.v + 1, (ed->undo.n - 1) * sizeof(s));
        ed->undo.n--;
    }
    vec_push(&ed->undo, s);
}

void tui_editor_undo(tui_editor_t *ed) {
    if (!ed->undo.n) return;
    tui_editor_snapshot_t s = vec_pop(&ed->undo);
    strvec_free(&ed->lines);
    ed->lines = s.lines;
    ed->cl = MIN(s.cl, ed->lines.n - 1);
    ed->cc = MIN(s.cc, strlen(cur_line(ed)));
    ed->last_action = ACT_NONE;
    ed->preferred_col = -1;
    notify_change(ed);
}

static void close_ac(tui_editor_t *ed) {
    if (ed->ac_list) tui_component_destroy(&ed->ac_list->base);
    ed->ac_list = NULL;
}

/* raw insertion of text (may contain '\n') at the cursor */
static void insert_raw(tui_editor_t *ed, const char *text) {
    char *line = cur_line(ed);
    char *after = xstrdup(line + ed->cc);
    line[ed->cc] = 0;
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *joined = xasprintf("%s%.*s", cur_line(ed), (int)n, p);
        set_line(ed, ed->cl, joined);
        ed->cc = strlen(joined);
        if (!nl) break;
        ed->cl++;
        insert_line(ed, ed->cl, xstrdup(""));
        ed->cc = 0;
        p = nl + 1;
    }
    char *fin = xasprintf("%s%s", cur_line(ed), after);
    set_line(ed, ed->cl, fin);
    free(after);
}

/* delete [from_line:from_col, to_line:to_col) and return it (owned) */
static char *delete_range(tui_editor_t *ed, size_t fl, size_t fc, size_t tl, size_t tc) {
    sb_t killed = SB_INIT;
    if (fl == tl) {
        char *l = ed->lines.v[fl];
        sb_appendn(&killed, l + fc, tc - fc);
        memmove(l + fc, l + tc, strlen(l + tc) + 1);
    } else {
        sb_append(&killed, ed->lines.v[fl] + fc);
        for (size_t i = fl + 1; i < tl; i++) {
            sb_putc(&killed, '\n');
            sb_append(&killed, ed->lines.v[i]);
        }
        sb_putc(&killed, '\n');
        sb_appendn(&killed, ed->lines.v[tl], tc);
        char *merged = xasprintf("%.*s%s", (int)fc, ed->lines.v[fl], ed->lines.v[tl] + tc);
        for (size_t i = tl; i > fl; i--) remove_line(ed, i);
        set_line(ed, fl, merged);
    }
    ed->cl = fl;
    ed->cc = fc;
    return sb_detach(&killed);
}

static void set_kill(tui_editor_t *ed, char *k) {
    if (!k || !*k) {
        free(k);
        return;
    }
    free(ed->kill);
    ed->kill = k;
}

/* ---------- paste markers ---------- */

/* If a known paste marker ends exactly at `col`, return its start; else -1. */
static long marker_ending_at(const tui_editor_t *ed, const char *line, size_t col) {
    if (col == 0 || line[col - 1] != ']') return -1;
    for (size_t i = 0; i < ed->pastes.n; i++) {
        const char *m = ed->pastes.v[i].marker;
        size_t ml = strlen(m);
        if (ml <= col && !memcmp(line + col - ml, m, ml)) return (long)(col - ml);
    }
    return -1;
}

static long marker_starting_at(const tui_editor_t *ed, const char *line, size_t col) {
    if (line[col] != '[') return -1;
    for (size_t i = 0; i < ed->pastes.n; i++) {
        const char *m = ed->pastes.v[i].marker;
        size_t ml = strlen(m);
        if (!strncmp(line + col, m, ml)) return (long)(col + ml);
    }
    return -1;
}

static char *sanitize_paste(const char *text) {
    sb_t b = SB_INIT;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r') {
            sb_putc(&b, '\n');
            if (p[1] == '\n') p++;
        } else if (c == '\t') {
            tui_sb_spaces(&b, TUI_TAB_WIDTH);
        } else if (c == '\n' || c >= 0x20) {
            if (c != 0x7f) sb_putc(&b, (char)c);
        }
    }
    char *s = sb_detach(&b);
    char *clean = utf8_sanitize(s, strlen(s));
    free(s);
    return clean;
}

void tui_editor_paste(tui_editor_t *ed, const char *text) {
    char *clean = sanitize_paste(text ? text : "");
    if (!*clean) {
        free(clean);
        return;
    }
    if ((clean[0] == '/' || clean[0] == '~' || clean[0] == '.') && ed->cc > 0) {
        char prev = cur_line(ed)[ed->cc - 1];
        if (isalnum((unsigned char)prev) || prev == '_') {
            char *t = xasprintf(" %s", clean);
            free(clean);
            clean = t;
        }
    }
    size_t nlines = 1 + str_count(clean, "\n");
    size_t nchars = utf8_strlen(clean);
    ed->hist_index = -1;
    push_undo(ed, ACT_OTHER);
    if (nlines > 10 || nchars > 1000) {
        int id = ++ed->paste_counter;
        char *marker = nlines > 10 ? xasprintf("[Paste #%d, +%zu lines]", id, nlines)
                                   : xasprintf("[Paste #%d, %zu chars]", id, nchars);
        tui_editor_paste_t pp = {id, marker, clean};
        vec_push(&ed->pastes, pp);
        insert_raw(ed, marker);
    } else {
        insert_raw(ed, clean);
        free(clean);
    }
    ed->last_action = ACT_OTHER;
    ed->preferred_col = -1;
    notify_change(ed);
}

/* ---------- layout ---------- */

typedef struct {
    size_t line, start, end;
} seg_t;
typedef VEC(seg_t) segvec_t;

static void layout(const tui_editor_t *ed, int W, segvec_t *segs) {
    if (W < 1) W = 1;
    for (size_t li = 0; li < ed->lines.n; li++) {
        const char *s = ed->lines.v[li];
        size_t n = strlen(s);
        size_t seg_start = 0, last_break = 0;
        int col = 0;
        size_t i = 0;
        while (i < n) {
            uint32_t cp;
            int k = utf8_decode(s + i, n - i, &cp);
            int cw = tui_cp_width(cp);
            if (col + cw > W && i > seg_start) {
                size_t brk = last_break > seg_start ? last_break : i;
                seg_t sg = {li, seg_start, brk};
                vec_push(segs, sg);
                seg_start = brk;
                last_break = 0;
                col = tui_visible_width_n(s + seg_start, i - seg_start);
            }
            col += cw;
            i += (size_t)k;
            if (cp == ' ') last_break = i;
        }
        seg_t sg = {li, seg_start, n};
        vec_push(segs, sg);
    }
}

static size_t cursor_seg(const tui_editor_t *ed, const segvec_t *segs) {
    for (size_t i = 0; i < segs->n; i++) {
        const seg_t *s = &segs->v[i];
        if (s->line != ed->cl) continue;
        bool last_of_line = i + 1 == segs->n || segs->v[i + 1].line != s->line;
        if (ed->cc >= s->start && (ed->cc < s->end || (last_of_line && ed->cc == s->end))) return i;
    }
    return 0;
}

static int effective_layout_width(const tui_editor_t *ed, int width) {
    int content = MAX(1, width - 2 * ed->padding_x);
    return MAX(1, content - 1);
}

/* ---------- movement ---------- */

static size_t offset_for_col(const char *s, size_t start, size_t end, int target, bool is_last) {
    int col = 0;
    size_t i = start;
    while (i < end) {
        uint32_t cp;
        int k = utf8_decode(s + i, end - i, &cp);
        int cw = tui_cp_width(cp);
        if (col + cw > target) break;
        col += cw;
        i += (size_t)k;
    }
    if (!is_last && i >= end && end > start) i = prev_char(s, end);
    return i;
}

/* Move cursor by visual rows; returns false if at the boundary. */
static bool move_vertical(tui_editor_t *ed, int dir) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    size_t si = cursor_seg(ed, &segs);
    const seg_t *cs = &segs.v[si];
    int col = tui_visible_width_n(ed->lines.v[ed->cl] + cs->start, ed->cc - cs->start);
    if (ed->preferred_col < 0) ed->preferred_col = col;
    long ti = (long)si + dir;
    if (ti < 0 || ti >= (long)segs.n) {
        vec_free(&segs);
        return false;
    }
    const seg_t *ts = &segs.v[ti];
    bool is_last = (size_t)ti + 1 == segs.n || segs.v[ti + 1].line != ts->line;
    ed->cl = ts->line;
    ed->cc = offset_for_col(ed->lines.v[ts->line], ts->start, ts->end, ed->preferred_col, is_last);
    vec_free(&segs);
    return true;
}

static bool on_first_visual_line(tui_editor_t *ed) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    bool r = cursor_seg(ed, &segs) == 0;
    vec_free(&segs);
    return r;
}

static bool on_last_visual_line(tui_editor_t *ed) {
    segvec_t segs = {0};
    layout(ed, ed->layout_width > 0 ? ed->layout_width : 79, &segs);
    bool r = cursor_seg(ed, &segs) + 1 == segs.n;
    vec_free(&segs);
    return r;
}

static int char_class(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t') return 0;
    if (isalnum(c) || c == '_' || c >= 0x80) return 1;
    return 2;
}

static void word_left(tui_editor_t *ed) {
    if (ed->cc == 0) {
        if (ed->cl > 0) {
            ed->cl--;
            ed->cc = strlen(cur_line(ed));
        }
        return;
    }
    const char *s = cur_line(ed);
    size_t i = ed->cc;
    while (i > 0 && char_class(s, prev_char(s, i)) == 0) i = prev_char(s, i);
    if (i > 0) {
        int cls = char_class(s, prev_char(s, i));
        while (i > 0 && char_class(s, prev_char(s, i)) == cls) i = prev_char(s, i);
    }
    ed->cc = i;
}

static size_t word_right_pos(const char *s, size_t i) {
    size_t n = strlen(s);
    while (i < n && char_class(s, i) == 0) i = next_char(s, i);
    if (i < n) {
        int cls = char_class(s, i);
        while (i < n && char_class(s, i) == cls) i = next_char(s, i);
    }
    return i;
}

static void word_right(tui_editor_t *ed) {
    const char *s = cur_line(ed);
    if (ed->cc >= strlen(s)) {
        if (ed->cl + 1 < ed->lines.n) {
            ed->cl++;
            ed->cc = 0;
        }
        return;
    }
    ed->cc = word_right_pos(s, ed->cc);
}

/* ---------- history ---------- */

void tui_editor_add_history(tui_editor_t *ed, const char *text) {
    if (!text || !*text) return;
    if (ed->history.n && !strcmp(ed->history.v[ed->history.n - 1], text)) return;
    if (ed->history.n >= HISTORY_MAX) {
        free(ed->history.v[0]);
        memmove(ed->history.v, ed->history.v + 1, (ed->history.n - 1) * sizeof(char *));
        ed->history.n--;
    }
    strvec_push(&ed->history, text);
}

static void set_text_internal(tui_editor_t *ed, const char *text) {
    strvec_free(&ed->lines);
    ed->lines = str_split(text ? text : "", "\n");
    if (!ed->lines.n) strvec_push(&ed->lines, "");
    ed->cl = ed->lines.n - 1;
    ed->cc = strlen(cur_line(ed));
    ed->preferred_col = -1;
}

static void history_nav(tui_editor_t *ed, int dir) {
    /* dir -1 = older, +1 = newer */
    int n = (int)ed->history.n;
    if (!n) return;
    int idx = ed->hist_index;
    if (dir < 0) {
        if (idx + 1 >= n) return;
        if (idx == -1) {
            free(ed->hist_draft);
            ed->hist_draft = tui_editor_get_text(ed);
        }
        idx++;
    } else {
        if (idx < 0) return;
        idx--;
    }
    ed->hist_index = idx;
    push_undo(ed, ACT_OTHER);
    set_text_internal(ed, idx < 0 ? (ed->hist_draft ? ed->hist_draft : "") : ed->history.v[n - 1 - idx]);
    notify_change(ed);
}

/* ---------- autocomplete ---------- */

static void refresh_ac(tui_editor_t *ed, bool forced) {
    if (!ed->ac_fn) return;
    char *before = xstrndup(cur_line(ed), ed->cc);
    tui_ac_result_t r;
    memset(&r, 0, sizeof r);
    bool ok = ed->ac_fn(ed->ac_ud, before, ed->cl == 0, forced, &r);
    free(before);
    if (!ok || !r.items.n || r.prefix_len > ed->cc) {
        tui_ac_result_free(&r);
        close_ac(ed);
        return;
    }
    size_t keep_sel = 0;
    const tui_select_item_t *prev = ed->ac_list ? tui_select_list_selected(ed->ac_list) : NULL;
    char *prev_val = prev ? xstrdup(prev->value) : NULL;
    close_ac(ed);
    ed->ac_list = tui_select_list_new(ed->ac_max_visible);
    ed->ac_list->filter_enabled = false;
    if (r.slash) {
        /* SLASH_COMMAND_SELECT_LIST_LAYOUT */
        ed->ac_list->layout.min_primary = 12;
        ed->ac_list->layout.max_primary = 32;
        ed->ac_list->layout.wrap_description = true;
        ed->ac_list->layout.max_description_rows = 2;
    }
    ed->ac_list->base.tui = ed->base.tui;
    for (size_t i = 0; i < r.items.n; i++) {
        tui_select_list_add_item(ed->ac_list, r.items.v[i].value, r.items.v[i].label, r.items.v[i].description,
                                 r.items.v[i].icon, false);
        /* upstream creates a fresh list per update: the selection restarts at the top for slash
         * commands; other completions keep the previously selected value */
        if (!r.slash && prev_val && !strcmp(prev_val, r.items.v[i].value)) keep_sel = i;
    }
    ed->ac_list->selected = keep_sel;
    ed->ac_enter_accepts = !r.first_desc_match;
    ed->ac_prefix_len = r.prefix_len;
    free(prev_val);
    tui_ac_result_free(&r);
}

static void maybe_trigger_ac(tui_editor_t *ed) {
    if (!ed->ac_fn) return;
    if (ed->ac_list) {
        refresh_ac(ed, false);
        return;
    }
    const char *line = cur_line(ed);
    bool trig = false;
    if (ed->cl == 0 && line[0] == '/') {
        trig = true;
        for (size_t i = 0; i < ed->cc; i++)
            if (line[i] == ' ' || line[i] == '\t') trig = false;
    }
    if (!trig) {
        size_t s = ed->cc;
        while (s > 0 && line[s - 1] != ' ' && line[s - 1] != '\t') s--;
        if (s < ed->cc && line[s] == '@') trig = true;
    }
    if (trig) refresh_ac(ed, false);
}

static void accept_ac(tui_editor_t *ed) {
    const tui_select_item_t *it = ed->ac_list ? tui_select_list_selected(ed->ac_list) : NULL;
    if (!it) {
        close_ac(ed);
        return;
    }
    char *val = xstrdup(it->value);
    push_undo(ed, ACT_OTHER);
    size_t start = ed->cc - MIN(ed->ac_prefix_len, ed->cc);
    char *line = cur_line(ed);
    char *nl = xasprintf("%.*s%s%s", (int)start, line, val, line + ed->cc);
    ed->cc = start + strlen(val);
    set_line(ed, ed->cl, nl);
    close_ac(ed);
    if (str_ends(val, "/")) refresh_ac(ed, true);
    free(val);
    ed->last_action = ACT_OTHER;
    notify_change(ed);
}

/* ---------- key handling ---------- */

static void do_submit(tui_editor_t *ed) {
    if (ed->disable_submit) return;
    char *expanded = tui_editor_get_expanded_text(ed);
    char *trimmed = str_trim(expanded);
    free(expanded);
    tui_editor_add_history(ed, trimmed);
    tui_editor_clear(ed);
    if (ed->on_submit) ed->on_submit(ed, trimmed, ed->cb_ud);
    free(trimmed);
}

static void delete_backward(tui_editor_t *ed) {
    if (ed->cc == 0) {
        if (ed->cl == 0) return;
        push_undo(ed, ACT_OTHER);
        size_t pl = strlen(ed->lines.v[ed->cl - 1]);
        free(delete_range(ed, ed->cl - 1, pl, ed->cl, 0));
        return;
    }
    push_undo(ed, ACT_OTHER);
    long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
    size_t from = ms >= 0 ? (size_t)ms : prev_char(cur_line(ed), ed->cc);
    free(delete_range(ed, ed->cl, from, ed->cl, ed->cc));
}

static void delete_forward(tui_editor_t *ed) {
    const char *l = cur_line(ed);
    size_t n = strlen(l);
    if (ed->cc >= n) {
        if (ed->cl + 1 >= ed->lines.n) return;
        push_undo(ed, ACT_OTHER);
        free(delete_range(ed, ed->cl, n, ed->cl + 1, 0));
        return;
    }
    push_undo(ed, ACT_OTHER);
    long me = marker_starting_at(ed, l, ed->cc);
    size_t to = me >= 0 ? (size_t)me : next_char(l, ed->cc);
    free(delete_range(ed, ed->cl, ed->cc, ed->cl, to));
}

static void delete_word_backward(tui_editor_t *ed) {
    if (ed->cc == 0) {
        delete_backward(ed);
        return;
    }
    push_undo(ed, ACT_OTHER);
    size_t end = ed->cc;
    long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
    if (ms >= 0) ed->cc = (size_t)ms;
    else word_left(ed);
    set_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, end));
}

static void delete_word_forward(tui_editor_t *ed) {
    const char *l = cur_line(ed);
    if (ed->cc >= strlen(l)) {
        delete_forward(ed);
        return;
    }
    push_undo(ed, ACT_OTHER);
    long me = marker_starting_at(ed, l, ed->cc);
    size_t to = me >= 0 ? (size_t)me : word_right_pos(l, ed->cc);
    set_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, to));
}

static void insert_typed(tui_editor_t *ed, const char *text) {
    bool space = text[0] == ' ' && !text[1];
    if (space) ed->last_action = ACT_NONE;
    push_undo(ed, ACT_TYPE);
    insert_raw(ed, text);
    ed->hist_index = -1;
}

bool tui_editor_handle_key(tui_editor_t *ed, const tui_key_t *k) {
    /* autocomplete navigation */
    if (ed->ac_list) {
        if (k->kind == TUI_KEY_ESCAPE && !k->mods) {
            close_ac(ed);
            tui_component_request_render(&ed->base);
            return true;
        }
        if (tui_kb(k, "tui.select.up") || tui_kb(k, "tui.select.down") || tui_kb(k, "tui.select.pageUp") ||
            tui_kb(k, "tui.select.pageDown")) {
            tui_component_handle_key(&ed->ac_list->base, k);
            tui_component_request_render(&ed->base);
            return true;
        }
        if (tui_kb(k, "tui.input.submit") && !ed->ac_enter_accepts) {
            /* the top suggestion only matched a description: Enter submits the typed text */
            close_ac(ed);
        } else if (tui_kb(k, "tui.input.tab") || tui_kb(k, "tui.input.submit")) {
            bool submit = tui_kb(k, "tui.input.submit");
            accept_ac(ed);
            /* like upstream: Enter on a slash-command completion runs the command */
            if (submit && ed->cl == 0 && !ed->ac_list) {
                char *line = cur_line(ed);
                size_t n = line ? strlen(line) : 0;
                while (n && line[n - 1] == ' ') n--;
                if (line && line[0] == '/' && !memchr(line, ' ', n)) do_submit(ed);
            }
            return true;
        }
    }

    bool changed = true, moved = false, retrigger = false;
    int prev_last = ed->last_action;
    ed->last_action = ACT_NONE;

    if (k->kind == TUI_KEY_PASTE) {
        tui_editor_paste(ed, k->text ? k->text : "");
        maybe_trigger_ac(ed);
        return true;
    } else if (tui_kb(k, "tui.editor.undo")) {
        ed->last_action = prev_last;
        tui_editor_undo(ed);
        close_ac(ed);
        return true;
    } else if (tui_kb(k, "tui.input.submit")) {
        do_submit(ed);
        return true;
    } else if (tui_kb(k, "tui.input.newLine")) {
        push_undo(ed, ACT_OTHER);
        insert_raw(ed, "\n");
        close_ac(ed);
    } else if (tui_kb(k, "tui.input.tab")) {
        if (ed->ac_fn) refresh_ac(ed, true);
        changed = false;
    } else if (tui_kb(k, "tui.editor.deleteCharBackward")) {
        delete_backward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteCharForward")) {
        delete_forward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        delete_word_backward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteWordForward")) {
        delete_word_forward(ed);
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) {
        if (ed->cc == 0) delete_backward(ed);
        else {
            push_undo(ed, ACT_OTHER);
            set_kill(ed, delete_range(ed, ed->cl, 0, ed->cl, ed->cc));
        }
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.deleteToLineEnd")) {
        size_t n = strlen(cur_line(ed));
        if (ed->cc >= n) delete_forward(ed);
        else {
            push_undo(ed, ACT_OTHER);
            set_kill(ed, delete_range(ed, ed->cl, ed->cc, ed->cl, n));
        }
        retrigger = true;
    } else if (tui_kb(k, "tui.editor.yank")) {
        if (ed->kill) {
            push_undo(ed, ACT_OTHER);
            insert_raw(ed, ed->kill);
        } else changed = false;
    } else if (tui_kb(k, "tui.editor.cursorLeft")) {
        if (ed->cc > 0) {
            long ms = marker_ending_at(ed, cur_line(ed), ed->cc);
            ed->cc = ms >= 0 ? (size_t)ms : prev_char(cur_line(ed), ed->cc);
        } else if (ed->cl > 0) {
            ed->cl--;
            ed->cc = strlen(cur_line(ed));
        }
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorRight")) {
        const char *l = cur_line(ed);
        if (ed->cc < strlen(l)) {
            long me = marker_starting_at(ed, l, ed->cc);
            ed->cc = me >= 0 ? (size_t)me : next_char(l, ed->cc);
        } else if (ed->cl + 1 < ed->lines.n) {
            ed->cl++;
            ed->cc = 0;
        }
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorWordLeft")) {
        word_left(ed);
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorWordRight")) {
        word_right(ed);
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorLineStart")) {
        ed->cc = 0;
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorLineEnd")) {
        ed->cc = strlen(cur_line(ed));
        moved = true;
    } else if (tui_kb(k, "tui.editor.cursorUp")) {
        if (on_first_visual_line(ed) && (tui_editor_is_empty(ed) || ed->hist_index >= 0) && ed->history.n) {
            history_nav(ed, -1);
            return true;
        }
        if (!move_vertical(ed, -1)) ed->cc = 0;
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_kb(k, "tui.editor.cursorDown")) {
        if (ed->hist_index >= 0 && on_last_visual_line(ed)) {
            history_nav(ed, 1);
            return true;
        }
        if (!move_vertical(ed, 1)) ed->cc = strlen(cur_line(ed));
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_kb(k, "tui.editor.pageUp") || tui_kb(k, "tui.editor.pageDown")) {
        int dir = tui_kb(k, "tui.editor.pageUp") ? -1 : 1;
        int rows = ed->max_height > 0 ? ed->max_height : 10;
        for (int i = 0; i < rows; i++)
            if (!move_vertical(ed, dir)) break;
        tui_component_request_render(&ed->base);
        return true;
    } else if (tui_key_text(k) && k->kind == TUI_KEY_CHAR) {
        insert_typed(ed, tui_key_text(k));
        retrigger = true;
    } else {
        ed->last_action = prev_last;
        return false;
    }

    if (moved) {
        ed->preferred_col = -1;
        if (ed->ac_list) refresh_ac(ed, false);
        tui_component_request_render(&ed->base);
        return true;
    }
    ed->preferred_col = -1;
    if (retrigger) maybe_trigger_ac(ed);
    if (changed) notify_change(ed);
    else tui_component_request_render(&ed->base);
    return true;
}

/* ---------- rendering ---------- */

static void border_line(sb_t *b, int width, int slot, const char *label) {
    const tui_theme_t *th = tui_theme();
    sb_t inner = SB_INIT;
    if (label) {
        sb_append(&inner, "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ");
        sb_append(&inner, label);
        sb_putc(&inner, ' ');
    }
    int w = tui_visible_width(sb_cstr(&inner));
    for (int i = w; i < width; i++) sb_append(&inner, "\xe2\x94\x80");
    char *tr = tui_truncate_to_width(sb_cstr(&inner), width, "");
    tui_fg(b, th, (tui_tc_t)slot, tr);
    free(tr);
    sb_free(&inner);
}

/* ---------- composer chrome rendering (omp components/editor.ts render) ---------- */

#define PLACEHOLDER_MIN_GAP 2

/* upstream Theme.fgResolved: color the text, re-opening it after nested fg/full resets */
static char *text_colored(const tui_editor_t *ed, const char *s) {
    if (!ed->text_fg[0]) return xstrdup(s);
    sb_t b = SB_INIT;
    sb_append(&b, ed->text_fg);
    for (const char *p = s; *p;) {
        if (!strncmp(p, "\x1b[0m", 4) || !strncmp(p, "\x1b[39m", 5)) {
            size_t n = p[2] == '0' ? 4 : 5;
            sb_appendn(&b, p, n);
            sb_append(&b, ed->text_fg);
            p += n;
        } else {
            sb_putc(&b, *p++);
        }
    }
    sb_append(&b, "\x1b[39m");
    return sb_detach(&b);
}

static int composer_padding(const tui_editor_t *ed) {
    return ed->composer->default_padding_x(ed->theme_padding_x);
}

static const char *composer_gutter(const tui_editor_t *ed) {
    if (ed->composer->side_borders) return NULL;
    return ed->gutter ? ed->gutter : ed->composer->default_gutter;
}

static void composer_render(tui_editor_t *ed, int width, strvec_t *out) {
    const tui_composer_style_t *st = ed->composer;
    tui_component_t *c = &ed->base;
    int px = composer_padding(ed);
    bool side = st->side_borders;
    int chrome = st->side_chrome_width(px);
    const char *gsrc = composer_gutter(ed);
    int gw = gsrc ? MIN(tui_visible_width(gsrc), MAX(0, width - 2 * chrome)) : 0;
    char *g_first = gw ? tui_slice_cols(gsrc, 0, gw) : NULL;
    sb_t gc = SB_INIT;
    tui_sb_spaces(&gc, gw);
    char *g_cont = sb_detach(&gc);
    int lcw = MAX(0, width - 2 * chrome - gw);
    int W = MAX(1, lcw - (side && px == 0 ? 1 : 0));
    ed->layout_width = W;

    segvec_t segs = {0};
    layout(ed, W, &segs);
    size_t csi = cursor_seg(ed, &segs);
    int total = (int)segs.n;
    int vis = ed->max_height > 0 ? MIN(ed->max_height, total) : total;
    if (ed->scroll > (int)csi) ed->scroll = (int)csi;
    if ((int)csi >= ed->scroll + vis) ed->scroll = (int)csi - vis + 1;
    if (ed->scroll > total - vis) ed->scroll = total - vis;
    if (ed->scroll < 0) ed->scroll = 0;

    tui_composer_ctx_t ctx = {.width = width, .padding_x = px, .border = ed->chrome_border, .accent = ed->chrome_accent,
                              .surface = ed->chrome_surface, .ud = ed->chrome_ud};
    char *top = NULL;
    if (st->status_attachment != TUI_ATTACH_NONE && ed->top_fn) {
        int tw = 0;
        top = ed->top_fn(ed->top_ud, MAX(0, width - chrome * 2), &tw);
        ctx.top_content = top;
        ctx.top_width = tw;
    }
    char *row = st->render_top(&ctx);
    if (row) strvec_push_own(out, row);

    bool empty = tui_editor_is_empty(ed);
    char *ph = empty && !ed->ac_list && ed->placeholder_fn ? ed->placeholder_fn(ed->placeholder_ud) : NULL;
    if (ph && !*ph) {
        free(ph);
        ph = NULL;
    }
    int phw = ph ? tui_visible_width(ph) : 0;

    for (int vi = ed->scroll; vi < ed->scroll + vis; vi++) {
        const seg_t *sg = &segs.v[vi];
        const char *line = ed->lines.v[sg->line];
        char *text = xstrndup(line + sg->start, sg->end - sg->start);
        int dw = tui_visible_width(text);
        const char *gutter = !gw ? "" : vi == ed->scroll ? g_first : g_cont;
        bool has_cursor = (size_t)vi == csi && c->focused;
        int overflow = 0;
        if (!side && dw > lcw) {
            char *t = tui_slice_cols(text, 0, lcw);
            free(text);
            text = t;
            dw = tui_visible_width(text);
        }
        sb_t d = SB_INIT;
        if (has_cursor) {
            size_t cpos = MIN(ed->cc - sg->start, strlen(text));
            const char *after = text + cpos;
            if (ed->use_terminal_cursor) {
                int avail = MAX(0, lcw - dw);
                if (!*after && ph && avail - phw >= PLACEHOLDER_MIN_GAP) {
                    sb_appendn(&d, text, cpos);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    tui_sb_spaces(&d, avail - phw);
                    sb_append(&d, ph);
                    dw += avail;
                } else if (!*after && !side && dw >= lcw && cpos > 0) {
                    size_t last = cpos;
                    while (last > 0 && ((unsigned char)text[last - 1] & 0xC0) == 0x80) last--;
                    if (last > 0) last--;
                    sb_appendn(&d, text, last);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    sb_append(&d, text + last);
                } else {
                    sb_appendn(&d, text, cpos);
                    sb_append(&d, TUI_CURSOR_MARKER);
                    sb_append(&d, after);
                }
            } else if (*after) {
                size_t nx = next_char(text, cpos);
                sb_appendn(&d, text, cpos);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, "\x1b[7m");
                sb_appendn(&d, after, nx - cpos);
                sb_append(&d, "\x1b[0m");
                sb_append(&d, text + nx);
            } else if (!side && dw + 1 > lcw && cpos > 0) {
                size_t last = cpos;
                while (last > 0 && ((unsigned char)text[last - 1] & 0xC0) == 0x80) last--;
                if (last > 0) last--;
                sb_appendn(&d, text, last);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, "\x1b[7m");
                sb_append(&d, text + last);
                sb_append(&d, "\x1b[0m");
            } else {
                sb_appendn(&d, text, cpos);
                sb_append(&d, TUI_CURSOR_MARKER);
                sb_append(&d, ed->eol_cursor ? ed->eol_cursor : "\x1b[7m \x1b[0m");
                dw += 1;
                int avail = MAX(0, lcw - dw);
                if (ph && avail - phw >= PLACEHOLDER_MIN_GAP) {
                    tui_sb_spaces(&d, avail - phw);
                    sb_append(&d, ph);
                    dw += avail;
                }
                if (dw > lcw && px > 0) overflow = dw - lcw;
            }
        } else {
            if (dw > lcw) {
                char *t = tui_truncate_to_width(text, lcw, NULL);
                free(text);
                text = t;
                dw = tui_visible_width(text);
            }
            sb_append(&d, text);
        }
        char *disp = sb_detach(&d);
        char *rendered = st->filled_surface ? xstrdup(disp) : text_colored(ed, disp);
        tui_composer_row_t r = {.ctx = &ctx,
                                .text = rendered,
                                .pad = MAX(0, lcw - dw),
                                .gutter = gutter,
                                .is_last_row = vi == ed->scroll + vis - 1,
                                .cursor_overflow = overflow};
        st->render_row(&r, out);
        free(rendered);
        free(disp);
        free(text);
    }
    row = st->render_bottom(&ctx);
    if (row) strvec_push_own(out, row);
    free(top);
    free(ph);
    free(g_first);
    free(g_cont);
    vec_free(&segs);
    if (ed->ac_list) tui_component_render(&ed->ac_list->base, width, out);
}

static void editor_render(tui_component_t *c, int width, strvec_t *out) {
    tui_editor_t *ed = (tui_editor_t *)c;
    if (ed->composer) {
        composer_render(ed, width, out);
        return;
    }
    int content_w = MAX(1, width - 2 * ed->padding_x);
    int W = effective_layout_width(ed, width);
    ed->layout_width = W;
    segvec_t segs = {0};
    layout(ed, W, &segs);
    size_t csi = cursor_seg(ed, &segs);
    int total = (int)segs.n;
    int vis = ed->max_height > 0 ? MIN(ed->max_height, total) : total;
    if (ed->scroll > (int)csi) ed->scroll = (int)csi;
    if ((int)csi >= ed->scroll + vis) ed->scroll = (int)csi - vis + 1;
    if (ed->scroll > total - vis) ed->scroll = total - vis;
    if (ed->scroll < 0) ed->scroll = 0;

    sb_t b = SB_INIT;
    char lab[48];
    if (ed->scroll > 0) snprintf(lab, sizeof lab, "\xe2\x86\x91 %d more", ed->scroll);
    border_line(&b, width, ed->border_slot, ed->scroll > 0 ? lab : NULL);
    strvec_push_own(out, sb_detach(&b));

    bool empty = tui_editor_is_empty(ed);
    for (int vi = ed->scroll; vi < ed->scroll + vis; vi++) {
        const seg_t *sg = &segs.v[vi];
        const char *line = ed->lines.v[sg->line];
        sb_t l = SB_INIT;
        tui_sb_spaces(&l, ed->padding_x);
        int used;
        if ((size_t)vi == csi && c->focused) {
            sb_appendn(&l, line + sg->start, ed->cc - sg->start);
            sb_append(&l, TUI_CURSOR_MARKER);
            size_t after = ed->cc;
            if (ed->cc < sg->end) {
                size_t nx = next_char(line, ed->cc);
                sb_append(&l, "\x1b[7m");
                sb_appendn(&l, line + ed->cc, nx - ed->cc);
                sb_append(&l, "\x1b[27m");
                after = nx;
                sb_appendn(&l, line + after, sg->end - after);
            } else {
                sb_append(&l, "\x1b[7m \x1b[27m");
                if (empty && ed->placeholder) {
                    char *ph = tui_truncate_to_width(ed->placeholder, MAX(0, content_w - 1), NULL);
                    char *d = tui_dim(ph);
                    sb_append(&l, d);
                    free(d);
                    free(ph);
                }
            }
        } else {
            sb_appendn(&l, line + sg->start, sg->end - sg->start);
            if (empty && ed->placeholder && !c->focused) {
                char *ph = tui_truncate_to_width(ed->placeholder, content_w, NULL);
                char *d = tui_dim(ph);
                sb_append(&l, d);
                free(d);
                free(ph);
            }
        }
        used = tui_visible_width(sb_cstr(&l));
        if (used > width) { /* only at absurdly small widths */
            char *t = tui_truncate_to_width(sb_cstr(&l), width, "");
            sb_free(&l);
            strvec_push_own(out, t);
            continue;
        }
        tui_sb_spaces(&l, width - used);
        strvec_push_own(out, sb_detach(&l));
    }
    int below = total - (ed->scroll + vis);
    if (below > 0) snprintf(lab, sizeof lab, "\xe2\x86\x93 %d more", below);
    border_line(&b, width, ed->border_slot, below > 0 ? lab : NULL);
    strvec_push_own(out, sb_detach(&b));
    vec_free(&segs);
    if (ed->ac_list) tui_component_render(&ed->ac_list->base, width, out);
}

static bool editor_key(tui_component_t *c, const tui_key_t *k) { return tui_editor_handle_key((tui_editor_t *)c, k); }

static void editor_destroy(tui_component_t *c) {
    tui_editor_t *ed = (tui_editor_t *)c;
    close_ac(ed);
    strvec_free(&ed->lines);
    for (size_t i = 0; i < ed->undo.n; i++) snapshot_free(&ed->undo.v[i]);
    vec_free(&ed->undo);
    free(ed->kill);
    strvec_free(&ed->history);
    free(ed->hist_draft);
    for (size_t i = 0; i < ed->pastes.n; i++) {
        free(ed->pastes.v[i].marker);
        free(ed->pastes.v[i].content);
    }
    vec_free(&ed->pastes);
    free(ed->placeholder);
    free(ed->gutter);
    free(ed);
}

static const tui_component_vt EDITOR_VT = {"editor", editor_render, editor_key, NULL, editor_destroy};

tui_editor_t *tui_editor_new(void) {
    tui_editor_t *ed = xcalloc(1, sizeof *ed);
    tui_component_init(&ed->base, &EDITOR_VT);
    ed->theme_padding_x = -1;
    strvec_push(&ed->lines, "");
    ed->preferred_col = -1;
    ed->hist_index = -1;
    ed->ac_max_visible = 10;
    ed->border_slot = TUI_TC_BORDER;
    return ed;
}

void tui_editor_set_text(tui_editor_t *ed, const char *text) {
    push_undo(ed, ACT_OTHER);
    char *clean = sanitize_paste(text ? text : "");
    set_text_internal(ed, clean);
    free(clean);
    ed->hist_index = -1;
    notify_change(ed);
}

char *tui_editor_get_text(const tui_editor_t *ed) { return strvec_join(&ed->lines, "\n"); }

char *tui_editor_get_expanded_text(const tui_editor_t *ed) {
    char *text = tui_editor_get_text(ed);
    for (size_t i = 0; i < ed->pastes.n; i++) {
        if (!strstr(text, ed->pastes.v[i].marker)) continue;
        char *r = str_replace(text, ed->pastes.v[i].marker, ed->pastes.v[i].content);
        free(text);
        text = r;
    }
    return text;
}

void tui_editor_insert_text(tui_editor_t *ed, const char *text) {
    push_undo(ed, ACT_OTHER);
    char *clean = sanitize_paste(text ? text : "");
    insert_raw(ed, clean);
    free(clean);
    ed->preferred_col = -1;
    notify_change(ed);
}

bool tui_editor_is_empty(const tui_editor_t *ed) { return ed->lines.n == 1 && ed->lines.v[0][0] == 0; }

void tui_editor_clear(tui_editor_t *ed) {
    strvec_free(&ed->lines);
    strvec_push(&ed->lines, "");
    ed->cl = ed->cc = 0;
    ed->scroll = 0;
    ed->preferred_col = -1;
    ed->hist_index = -1;
    for (size_t i = 0; i < ed->pastes.n; i++) {
        free(ed->pastes.v[i].marker);
        free(ed->pastes.v[i].content);
    }
    ed->pastes.n = 0;
    for (size_t i = 0; i < ed->undo.n; i++) snapshot_free(&ed->undo.v[i]);
    ed->undo.n = 0;
    ed->last_action = ACT_NONE;
    close_ac(ed);
    notify_change(ed);
}

void tui_editor_set_on_submit(tui_editor_t *ed, tui_editor_submit_fn fn, void *ud) {
    ed->on_submit = fn;
    ed->cb_ud = ud;
}

void tui_editor_set_on_change(tui_editor_t *ed, tui_editor_change_fn fn) { ed->on_change = fn; }

void tui_editor_set_autocomplete(tui_editor_t *ed, tui_autocomplete_fn fn, void *ud) {
    ed->ac_fn = fn;
    ed->ac_ud = ud;
    close_ac(ed);
}

void tui_editor_set_placeholder(tui_editor_t *ed, const char *text) {
    free(ed->placeholder);
    ed->placeholder = text ? xstrdup(text) : NULL;
}

bool tui_editor_autocomplete_open(const tui_editor_t *ed) { return ed->ac_list != NULL; }

void tui_editor_set_composer(tui_editor_t *ed, const char *shape) {
    ed->composer = shape ? tui_composer_style(shape) : NULL;
    ed->scroll = 0;
    tui_component_request_render(&ed->base);
}

void tui_editor_set_chrome_colors(tui_editor_t *ed, tui_composer_color_fn border, tui_composer_color_fn accent,
                                  tui_composer_color_fn surface, void *ud) {
    ed->chrome_border = border;
    ed->chrome_accent = accent;
    ed->chrome_surface = surface;
    ed->chrome_ud = ud;
}

void tui_editor_set_top_provider(tui_editor_t *ed, char *(*fn)(void *ud, int avail, int *width), void *ud) {
    ed->top_fn = fn;
    ed->top_ud = ud;
}

void tui_editor_set_placeholder_fn(tui_editor_t *ed, char *(*fn)(void *ud), void *ud) {
    ed->placeholder_fn = fn;
    ed->placeholder_ud = ud;
}
