/* components.c - Text, TruncatedText, Box, Spacer, Loader, SelectList */
#include "tui/components.h"

#include "tui/fuzzy.h"
#include "tui/symbols.h"

#include <ctype.h>
#include <math.h>

/* Apply a background across a full-width line, re-establishing it after resets. */
static char *apply_bg(const char *line, int width, const char *bgseq) {
    sb_t b = SB_INIT;
    sb_append(&b, bgseq);
    const char *p = line;
    const char *r;
    while ((r = strstr(p, "\x1b[0m"))) {
        sb_appendn(&b, p, (size_t)(r - p) + 4);
        sb_append(&b, bgseq);
        p = r + 4;
    }
    sb_append(&b, p);
    tui_sb_spaces(&b, width - tui_visible_width(line));
    sb_append(&b, "\x1b[49m");
    return sb_detach(&b);
}

/* ---------- Text ---------- */

static void text_invalidate(tui_component_t *c) {
    tui_text_t *t = (tui_text_t *)c;
    strvec_free(&t->cache);
    free(t->cache_text);
    t->cache_text = NULL;
    t->cache_width = -1;
}

static void text_render(tui_component_t *c, int width, strvec_t *out) {
    tui_text_t *t = (tui_text_t *)c;
    if (!t->text || (!t->text[0] && t->padding_y == 0)) return;
    if (!(t->cache_text && t->cache_width == width && !strcmp(t->cache_text, t->text))) {
        text_invalidate(c);
        int inner = MAX(1, width - 2 * t->padding_x);
        strvec_t wrapped = {0};
        char *clean = tui_replace_tabs(t->text);
        tui_wrap_text_with_ansi(clean, inner, &wrapped);
        free(clean);
        const char *bgseq = t->bg_slot >= 0 ? tui_bg_seq(NULL, (tui_tc_t)t->bg_slot) : NULL;
        for (int i = 0; i < t->padding_y; i++)
            strvec_push_own(&t->cache, bgseq ? apply_bg("", width, bgseq) : xstrdup(""));
        for (size_t i = 0; i < wrapped.n; i++) {
            sb_t b = SB_INIT;
            tui_sb_spaces(&b, t->padding_x);
            sb_append(&b, wrapped.v[i]);
            if (bgseq) {
                char *l = apply_bg(sb_cstr(&b), width, bgseq);
                sb_free(&b);
                strvec_push_own(&t->cache, l);
            } else {
                strvec_push_own(&t->cache, sb_detach(&b));
            }
        }
        for (int i = 0; i < t->padding_y; i++)
            strvec_push_own(&t->cache, bgseq ? apply_bg("", width, bgseq) : xstrdup(""));
        strvec_free(&wrapped);
        t->cache_text = xstrdup(t->text);
        t->cache_width = width;
    }
    for (size_t i = 0; i < t->cache.n; i++) strvec_push(out, t->cache.v[i]);
}

static void text_destroy(tui_component_t *c) {
    tui_text_t *t = (tui_text_t *)c;
    text_invalidate(c);
    free(t->text);
    free(t);
}

static const tui_component_vt TEXT_VT = {"text", text_render, NULL, text_invalidate, text_destroy};

tui_text_t *tui_text_new(const char *text, int padding_x, int padding_y) {
    tui_text_t *t = xcalloc(1, sizeof *t);
    tui_component_init(&t->base, &TEXT_VT);
    t->text = xstrdup(text ? text : "");
    t->padding_x = MAX(0, padding_x);
    t->padding_y = MAX(0, padding_y);
    t->bg_slot = -1;
    t->cache_width = -1;
    return t;
}

void tui_text_set(tui_text_t *t, const char *text) {
    free(t->text);
    t->text = xstrdup(text ? text : "");
    tui_component_request_render(&t->base);
}

void tui_text_set_bg(tui_text_t *t, int slot) {
    t->bg_slot = slot;
    text_invalidate(&t->base);
    tui_component_request_render(&t->base);
}

/* ---------- TruncatedText ---------- */

static void trunc_render(tui_component_t *c, int width, strvec_t *out) {
    tui_trunc_text_t *t = (tui_trunc_text_t *)c;
    const char *nl = strchr(t->text, '\n');
    char *first = nl ? xstrndup(t->text, (size_t)(nl - t->text)) : xstrdup(t->text);
    char *tr = tui_truncate_to_width(first, MAX(0, width - 2 * t->padding_x), NULL);
    sb_t b = SB_INIT;
    tui_sb_spaces(&b, t->padding_x);
    sb_append(&b, tr);
    strvec_push_own(out, sb_detach(&b));
    free(tr);
    free(first);
}

static void trunc_destroy(tui_component_t *c) {
    free(((tui_trunc_text_t *)c)->text);
    free(c);
}

static const tui_component_vt TRUNC_VT = {"truncated-text", trunc_render, NULL, NULL, trunc_destroy};

tui_trunc_text_t *tui_trunc_text_new(const char *text, int padding_x) {
    tui_trunc_text_t *t = xcalloc(1, sizeof *t);
    tui_component_init(&t->base, &TRUNC_VT);
    t->text = xstrdup(text ? text : "");
    t->padding_x = MAX(0, padding_x);
    return t;
}

void tui_trunc_text_set(tui_trunc_text_t *t, const char *text) {
    free(t->text);
    t->text = xstrdup(text ? text : "");
    tui_component_request_render(&t->base);
}

/* ---------- Box ---------- */

static void box_render(tui_component_t *c, int width, strvec_t *out) {
    tui_box_t *b = (tui_box_t *)c;
    int inner = MAX(1, width - 2 * b->padding_x);
    strvec_t lines = {0};
    tui_container_render_children(&b->c, inner, &lines);
    if (!lines.n) {
        strvec_free(&lines);
        return;
    }
    const char *bgseq = b->bg_slot >= 0 ? tui_bg_seq(NULL, (tui_tc_t)b->bg_slot) : NULL;
    for (int i = 0; i < b->padding_y; i++) strvec_push_own(out, bgseq ? apply_bg("", width, bgseq) : xstrdup(""));
    for (size_t i = 0; i < lines.n; i++) {
        sb_t s = SB_INIT;
        tui_sb_spaces(&s, b->padding_x);
        sb_append(&s, lines.v[i]);
        if (bgseq) {
            strvec_push_own(out, apply_bg(sb_cstr(&s), width, bgseq));
            sb_free(&s);
        } else {
            strvec_push_own(out, sb_detach(&s));
        }
    }
    for (int i = 0; i < b->padding_y; i++) strvec_push_own(out, bgseq ? apply_bg("", width, bgseq) : xstrdup(""));
    strvec_free(&lines);
}

static void box_invalidate(tui_component_t *c) {
    tui_box_t *b = (tui_box_t *)c;
    for (size_t i = 0; i < b->c.children.n; i++) tui_component_invalidate(b->c.children.v[i]);
}

static void box_destroy(tui_component_t *c) {
    tui_container_destroy_children((tui_container_t *)c);
    free(c);
}

static const tui_component_vt BOX_VT = {"box", box_render, NULL, box_invalidate, box_destroy};

tui_box_t *tui_box_new(int padding_x, int padding_y, int bg_slot) {
    tui_box_t *b = xcalloc(1, sizeof *b);
    tui_container_init(&b->c);
    b->c.base.vt = &BOX_VT;
    b->padding_x = MAX(0, padding_x);
    b->padding_y = MAX(0, padding_y);
    b->bg_slot = bg_slot;
    return b;
}

/* ---------- Spacer ---------- */

static void spacer_render(tui_component_t *c, int width, strvec_t *out) {
    for (int i = 0; i < ((tui_spacer_t *)c)->lines; i++) strvec_push(out, "");
}

static const tui_component_vt SPACER_VT = {"spacer", spacer_render, NULL, NULL, NULL};

tui_spacer_t *tui_spacer_new(int lines) {
    tui_spacer_t *s = xcalloc(1, sizeof *s);
    tui_component_init(&s->base, &SPACER_VT);
    s->lines = MAX(0, lines);
    return s;
}

/* ---------- Loader ---------- */

static void loader_render(tui_component_t *c, int width, strvec_t *out) {
    tui_loader_t *l = (tui_loader_t *)c;
    const tui_theme_t *th = tui_theme();
    if (l->leading_blank) strvec_push(out, "");
    sb_t b = SB_INIT;
    const char *frame = th->nspinner ? th->spinner[l->frame % th->nspinner] : "*";
    tui_fg(&b, th, (tui_tc_t)l->spinner_slot, frame);
    sb_putc(&b, ' ');
    tui_fg(&b, th, (tui_tc_t)l->message_slot, l->message);
    char *tr = tui_truncate_to_width(sb_cstr(&b), width, NULL);
    sb_free(&b);
    strvec_push_own(out, tr);
}

static void loader_tick(void *ud) {
    tui_loader_t *l = ud;
    l->frame++;
    if (l->tui) tui_request_render(l->tui);
}

void tui_loader_start(tui_loader_t *l) {
    if (l->timer_id || !l->tui) return;
    l->timer_id = tui_add_timer(l->tui, l->interval_ms, true, loader_tick, l);
}

void tui_loader_stop(tui_loader_t *l) {
    if (l->timer_id && l->tui) tui_remove_timer(l->tui, l->timer_id);
    l->timer_id = 0;
}

static void loader_destroy(tui_component_t *c) {
    tui_loader_t *l = (tui_loader_t *)c;
    tui_loader_stop(l);
    free(l->message);
    free(l);
}

static const tui_component_vt LOADER_VT = {"loader", loader_render, NULL, NULL, loader_destroy};

tui_loader_t *tui_loader_new(tui_t *tui, const char *message) {
    tui_loader_t *l = xcalloc(1, sizeof *l);
    tui_component_init(&l->base, &LOADER_VT);
    l->base.tui = tui;
    l->tui = tui;
    l->message = xstrdup(message ? message : "Loading...");
    l->interval_ms = 80;
    l->spinner_slot = TUI_TC_ACCENT;
    l->message_slot = TUI_TC_MUTED;
    l->leading_blank = true;
    return l;
}

void tui_loader_set_message(tui_loader_t *l, const char *message) {
    free(l->message);
    l->message = xstrdup(message ? message : "");
    tui_component_request_render(&l->base);
}

/* ---------- fuzzy ---------- */

int tui_fuzzy_score(const char *query, const char *text) {
    if (!query || !*query) return 0;
    if (!text) return -1;
    int score = 0, run = 0;
    const char *t = text;
    const char *q = query;
    bool first = true;
    while (*q) {
        int qc = tolower((unsigned char)*q);
        const char *found = NULL;
        for (const char *p = t; *p; p++)
            if (tolower((unsigned char)*p) == qc) {
                found = p;
                break;
            }
        if (!found) return -1;
        int gap = (int)(found - t);
        if (gap == 0 && !first) run++;
        else run = 0;
        score += 10 + run * 5 - MIN(gap, 10);
        if (found == text || strchr(" -_/.:@", found[-1])) score += 8;
        if (first && found == text) score += 15;
        first = false;
        t = found + 1;
        q++;
    }
    score -= (int)MIN(strlen(text), 40) / 4;
    return score;
}

/* ---------- SelectList (port of omp components/select-list.ts + menu-selection.ts) ---------- */

#define SL_DEFAULT_PRIMARY 32
#define SL_PRIMARY_GAP 2
#define SL_MIN_DESCRIPTION 10

static const char *item_label(const tui_select_item_t *it) {
    return it->label && *it->label ? it->label : it->value;
}

/* replaceTabs + collapse whitespace runs (incl. newlines) to one space + trim */
static char *sl_sanitize(const char *s) {
    char *t = tui_replace_tabs(s ? s : "");
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = t; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
            sp = b.len > 0;
            continue;
        }
        if (sp) sb_putc(&b, ' ');
        sp = false;
        sb_putc(&b, *p);
    }
    free(t);
    return sb_detach(&b);
}

static int sl_policy(const tui_select_list_t *l) {
    return l->filter_enabled ? l->layout.search : TUI_SELECT_SEARCH_NEVER;
}
static bool sl_can_edit_search(const tui_select_list_t *l) {
    int p = sl_policy(l);
    return p == TUI_SELECT_SEARCH_ALWAYS || (p == TUI_SELECT_SEARCH_OVERFLOW && (int)l->items.n > l->max_visible);
}
static bool sl_show_status(const tui_select_list_t *l) {
    bool q = l->filter && *l->filter;
    return (sl_policy(l) != TUI_SELECT_SEARCH_NEVER && q) || (sl_can_edit_search(l) && (int)l->items.n > l->max_visible);
}

static const tui_select_item_t *sl_vis(const tui_select_list_t *l, size_t i) { return &l->items.v[l->visible.v[i]]; }

static size_t sl_coerce(const tui_select_list_t *l, long index, int dir) {
    long n = (long)l->visible.n;
    if (!n) return 0;
    long c = index < 0 ? 0 : index >= n ? n - 1 : index;
    if (!sl_vis(l, (size_t)c)->disabled) return (size_t)c;
    for (long i = c + dir; i >= 0 && i < n; i += dir)
        if (!sl_vis(l, (size_t)i)->disabled) return (size_t)i;
    for (long i = c - dir; i >= 0 && i < n; i -= dir)
        if (!sl_vis(l, (size_t)i)->disabled) return (size_t)i;
    return (size_t)c;
}

static void sl_notify(tui_select_list_t *l) {
    const tui_select_item_t *it = tui_select_list_selected(l);
    if (it && l->on_change) l->on_change(l, it, l->ud);
}

/* re-filter, keeping `keep` (a value) selected when it survives */
static void select_refilter_keep(tui_select_list_t *l, const char *keep) {
    l->visible.n = 0;
    bool blank = true;
    for (const char *p = l->filter ? l->filter : ""; *p; p++)
        if (!isspace((unsigned char)*p)) blank = false;
    if (blank) {
        for (size_t i = 0; i < l->items.n; i++) vec_push(&l->visible, i);
    } else {
        size_t n = l->items.n;
        char **texts = xcalloc(n ? n : 1, sizeof *texts);
        for (size_t i = 0; i < n; i++) {
            const tui_select_item_t *it = &l->items.v[i];
            char *raw = xasprintf("%s %s%s%s%s%s", it->label ? it->label : "", it->value, it->description ? " " : "",
                                  it->description ? it->description : "", it->hint ? " " : "", it->hint ? it->hint : "");
            texts[i] = sl_sanitize(raw);
            free(raw);
        }
        size_t *idx = xmalloc((n ? n : 1) * sizeof *idx);
        size_t m = tui_fuzzy_rank(l->filter, (const char *const *)texts, n, idx);
        for (size_t i = 0; i < m; i++) vec_push(&l->visible, idx[i]);
        free(idx);
        for (size_t i = 0; i < n; i++) free(texts[i]);
        free(texts);
    }
    l->selected = 0;
    if (keep)
        for (size_t i = 0; i < l->visible.n; i++)
            if (!strcmp(sl_vis(l, i)->value, keep) && !sl_vis(l, i)->disabled) {
                l->selected = i;
                return;
            }
    l->selected = sl_coerce(l, 0, 1);
}

static void select_refilter(tui_select_list_t *l) {
    const tui_select_item_t *cur = tui_select_list_selected(l);
    char *keep = cur ? xstrdup(cur->value) : NULL;
    select_refilter_keep(l, keep);
    free(keep);
}

typedef struct {
    size_t start, end;
    int row_offset, total_rows;
} sl_window_t;

/* getMenuWindow: selection-centered contiguous window fitting `budget` visual rows */
static sl_window_t sl_menu_window(const int *counts, size_t n, size_t selected, int budget) {
    sl_window_t w = {0, 0, 0, 0};
    for (size_t i = 0; i < n; i++) w.total_rows += MAX(0, counts[i]);
    if (!n) return w;
    budget = MAX(1, budget);
    size_t sel = MIN(selected, n - 1);
    int sel_rows = MAX(0, counts[sel]);
    int target = MAX(0, budget - MIN(sel_rows, budget)) / 2;
    size_t start = sel;
    int before = 0;
    while (start > 0) {
        int cost = MAX(0, counts[start - 1]);
        if (before + cost > target) break;
        start--;
        before += cost;
    }
    size_t end = sel + 1;
    int used = before + sel_rows;
    while (end < n) {
        int cost = MAX(0, counts[end]);
        if (used + cost > budget) break;
        used += cost;
        end++;
    }
    while (start > 0) {
        int cost = MAX(0, counts[start - 1]);
        if (used + cost > budget) break;
        start--;
        used += cost;
    }
    for (size_t i = 0; i < start; i++) w.row_offset += MAX(0, counts[i]);
    w.start = start;
    w.end = end;
    return w;
}

typedef struct {
    bool has_desc;
    char *prefix, *icon_cell, *value, *spacing, *desc;
    int desc_start, remaining;
} sl_layout_t;

static void sl_layout_free(sl_layout_t *y) {
    free(y->prefix);
    free(y->icon_cell);
    free(y->value);
    free(y->spacing);
    free(y->desc);
}

static void sl_compute_layout(const tui_select_list_t *l, const tui_select_item_t *it, bool sel, int width, int primary,
                              int icon_w, sl_layout_t *y) {
    memset(y, 0, sizeof *y);
    const char *cur = l->cursor ? l->cursor : ">";
    int cw = tui_visible_width(cur);
    y->prefix = sel ? xasprintf("%s ", cur) : xasprintf("%*s", cw + 1, "");
    if (icon_w > 0) {
        const char *ic = it->icon ? it->icon : "";
        y->icon_cell = xasprintf("%s%*s", ic, icon_w - tui_visible_width(ic) + 1, "");
    } else {
        y->icon_cell = xstrdup("");
    }
    int prefix_w = tui_visible_width(y->prefix) + (icon_w > 0 ? icon_w + 1 : 0);
    char *label = sl_sanitize(item_label(it));
    char *desc = it->description ? sl_sanitize(it->description) : NULL;
    if (desc && *desc && width > 40) {
        int eff = MAX(1, MIN(primary, width - prefix_w - 4));
        int maxp = MAX(1, eff - SL_PRIMARY_GAP);
        char *tv = tui_truncate_to_width(label, maxp, "");
        int tvw = tui_visible_width(tv);
        int spw = MAX(1, eff - tvw);
        int dstart = prefix_w + tvw + spw;
        int rem = width - dstart - 2;
        if (rem > SL_MIN_DESCRIPTION) {
            y->has_desc = true;
            y->value = tv;
            y->spacing = xasprintf("%*s", spw, "");
            y->desc = desc;
            y->desc_start = dstart;
            y->remaining = rem;
            free(label);
            return;
        }
        free(tv);
    }
    free(desc);
    y->value = tui_truncate_to_width(label, MAX(0, width - prefix_w - 2), "");
    free(label);
}

static void sl_wrap_desc(const tui_select_list_t *l, const char *desc, int width, strvec_t *out) {
    tui_wrap_text_with_ansi(desc, MAX(1, width), out);
    int cap = l->layout.max_description_rows;
    if (cap >= 1 && (int)out->n > cap) {
        while ((int)out->n > cap) free(out->v[--out->n]);
        char *last = xasprintf("%s \xe2\x80\xa6", out->v[cap - 1]);
        free(out->v[cap - 1]);
        out->v[cap - 1] = tui_truncate_to_width(last, width, NULL);
        free(last);
    }
}

static int sl_row_count(const tui_select_list_t *l, const tui_select_item_t *it, int width, int primary, int icon_w) {
    sl_layout_t y;
    sl_compute_layout(l, it, false, width, primary, icon_w, &y);
    int n = 1;
    if (y.has_desc) {
        strvec_t w = {0};
        sl_wrap_desc(l, y.desc, y.remaining, &w);
        n = MAX(1, (int)w.n);
        strvec_free(&w);
    }
    sl_layout_free(&y);
    return n;
}

static void sl_render_item(const tui_select_list_t *l, const tui_select_item_t *it, bool sel, int width, int primary,
                           int icon_w, strvec_t *rows) {
    const tui_theme_t *th = tui_theme();
    sl_layout_t y;
    sl_compute_layout(l, it, sel, width, primary, icon_w, &y);
    char *icon = (*y.icon_cell && !sel) ? tui_fgs(th, TUI_TC_MUTED, y.icon_cell) : xstrdup(y.icon_cell);
    if (y.has_desc) {
        strvec_t w = {0};
        if (l->layout.wrap_description) sl_wrap_desc(l, y.desc, y.remaining, &w);
        else strvec_push_own(&w, tui_truncate_to_width(y.desc, y.remaining, ""));
        if (!w.n) strvec_push(&w, "");
        char *indent = xasprintf("%*s", y.desc_start, "");
        if (sel) {
            char *r0 = xasprintf("%s%s%s%s%s", y.prefix, icon, y.value, y.spacing, w.v[0]);
            strvec_push_own(rows, tui_fgs(th, TUI_TC_ACCENT, r0));
            free(r0);
            for (size_t i = 1; i < w.n; i++) {
                char *r = xasprintf("%s%s", indent, w.v[i]);
                strvec_push_own(rows, tui_fgs(th, TUI_TC_ACCENT, r));
                free(r);
            }
        } else {
            char *d0 = xasprintf("%s%s", y.spacing, w.v[0]);
            char *md = tui_fgs(th, TUI_TC_MUTED, d0);
            strvec_push_own(rows, xasprintf("%s%s%s%s", y.prefix, icon, y.value, md));
            free(md);
            free(d0);
            for (size_t i = 1; i < w.n; i++) {
                char *r = xasprintf("%s%s", indent, w.v[i]);
                strvec_push_own(rows, tui_fgs(th, TUI_TC_MUTED, r));
                free(r);
            }
        }
        free(indent);
        strvec_free(&w);
    } else if (sel) {
        char *r = xasprintf("%s%s%s", y.prefix, icon, y.value);
        strvec_push_own(rows, tui_fgs(th, TUI_TC_ACCENT, r));
        free(r);
    } else {
        strvec_push_own(rows, xasprintf("%s%s%s", y.prefix, icon, y.value));
    }
    free(icon);
    sl_layout_free(&y);
}

static char *sl_status_line(const tui_select_list_t *l, int width) {
    const tui_theme_t *th = tui_theme();
    int avail = MAX(1, width - 2);
    if (l->filter && *l->filter) {
        char *lab = tui_truncate_to_width("  Search: ", avail, "");
        int fw = avail - tui_visible_width(lab);
        char *styled = tui_fgs(th, TUI_TC_MUTED, lab);
        free(lab);
        if (fw <= 0) return styled;
        char *tail = tui_truncate_to_width(l->filter, MAX(0, fw - 1), "");
        char *field = xasprintf("%s\x1b[7m \x1b[27m", tail); /* Input renders its cursor cell */
        free(tail);
        char *r = xasprintf("%s%s", styled, field);
        free(field);
        free(styled);
        return r;
    }
    char *t = tui_truncate_to_width(sl_can_edit_search(l) ? "  Type to search" : "", avail, "");
    char *r = tui_fgs(th, TUI_TC_MUTED, t);
    free(t);
    return r;
}

static void select_render(tui_component_t *c, int width, strvec_t *out) {
    tui_select_list_t *l = (tui_select_list_t *)c;
    const tui_theme_t *th = tui_theme();
    size_t n = l->visible.n;
    bool show_status = sl_show_status(l);
    if (n == 0) {
        if (show_status) strvec_push_own(out, sl_status_line(l, width));
        bool q = l->filter && *l->filter;
        const char *msg = q ? (l->layout.no_match_text ? l->layout.no_match_text : "No matching items")
                            : (l->layout.empty_text ? l->layout.empty_text : "No items");
        char *m = xasprintf("  %s", msg);
        strvec_push_own(out, tui_fgs(th, TUI_TC_MUTED, m));
        free(m);
        return;
    }
    int rmin = l->layout.min_primary ? l->layout.min_primary : l->layout.max_primary ? l->layout.max_primary : SL_DEFAULT_PRIMARY;
    int rmax = l->layout.max_primary ? l->layout.max_primary : l->layout.min_primary ? l->layout.min_primary : SL_DEFAULT_PRIMARY;
    int pmin = MAX(1, MIN(rmin, rmax)), pmax = MAX(1, MAX(rmin, rmax));
    int widest = 0, icon_w = 0;
    for (size_t i = 0; i < n; i++) {
        const tui_select_item_t *it = sl_vis(l, i);
        char *lab = sl_sanitize(item_label(it));
        widest = MAX(widest, tui_visible_width(lab) + SL_PRIMARY_GAP);
        free(lab);
        if (it->icon) icon_w = MAX(icon_w, tui_visible_width(it->icon));
    }
    int primary = widest < pmin ? pmin : widest > pmax ? pmax : widest;
    int budget = MAX(1, l->max_visible);
    int *counts = xmalloc(n * sizeof *counts);
    for (size_t i = 0; i < n; i++)
        counts[i] = l->layout.wrap_description ? sl_row_count(l, sl_vis(l, i), MAX(0, width - 1), primary, icon_w) : 1;
    sl_window_t win = sl_menu_window(counts, n, l->selected, budget);
    free(counts);
    bool overflow = win.total_rows > budget;
    int row_w = MAX(0, width - (overflow ? 1 : 0));
    strvec_t rows = {0};
    for (size_t i = win.start; i < win.end && (int)rows.n < budget; i++) {
        const tui_select_item_t *it = sl_vis(l, i);
        strvec_t ir = {0};
        sl_render_item(l, it, i == l->selected, row_w, primary, icon_w, &ir);
        for (size_t k = 0; k < ir.n && (int)rows.n < budget; k++) {
            if (it->disabled) strvec_push_own(&rows, tui_fgs(th, TUI_TC_MUTED, ir.v[k]));
            else strvec_push(&rows, ir.v[k]);
        }
        strvec_free(&ir);
    }
    /* ScrollView(scrollbar "auto", totalRows) */
    int h = (int)rows.n;
    bool bar = h > 0 && width > 0 && win.total_rows > h;
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = h;
    if (bar) {
        int size = MAX(1, MIN((h * h) / win.total_rows, h));
        int travel = h - size, maxoff = MAX(0, win.total_rows - h);
        int off = MIN(MAX(0, win.row_offset), maxoff);
        ts = maxoff == 0 ? 0 : (int)lround((double)off / maxoff * travel);
        te = ts + size;
    }
    for (int r = 0; r < h; r++) {
        char *tr = tui_truncate_to_width(rows.v[r], cw, NULL);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        sb_t b = SB_INIT;
        sb_append(&b, tr);
        tui_sb_spaces(&b, cw - tui_visible_width(tr));
        free(tr);
        bool thumb = r >= ts && r < te;
        tui_fg(&b, th, thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&rows);
    if (show_status) strvec_push_own(out, sl_status_line(l, width));
}

void tui_select_list_move(tui_select_list_t *l, int delta, bool wrap) {
    long total = (long)l->visible.n;
    if (!total || !delta) return;
    int dir = delta < 0 ? -1 : 1;
    long remaining = MAX(1, labs((long)delta));
    long index = (long)l->selected;
    for (long attempts = 0; attempts < total && remaining > 0; attempts++) {
        long next = index + dir;
        if (next < 0 || next >= total) {
            if (!wrap) break;
            next = dir > 0 ? 0 : total - 1;
        }
        if (next == index) break;
        index = next;
        if (!sl_vis(l, (size_t)index)->disabled) remaining--;
    }
    if (index == (long)l->selected) return;
    size_t nx = sl_coerce(l, index, dir);
    if (nx == l->selected) return;
    l->selected = nx;
    sl_notify(l);
}

static void sl_set_query(tui_select_list_t *l, char *q /* owned, may be NULL */) {
    const tui_select_item_t *cur = tui_select_list_selected(l);
    char *prev = cur ? xstrdup(cur->value) : NULL;
    free(l->filter);
    l->filter = q;
    select_refilter_keep(l, prev);
    const tui_select_item_t *now = tui_select_list_selected(l);
    bool changed = (prev == NULL) != (now == NULL) || (prev && now && strcmp(prev, now->value));
    free(prev);
    if (changed) sl_notify(l);
}

/* the list's search field (upstream Input): returns true when consumed */
static bool sl_search_input(tui_select_list_t *l, const tui_key_t *k) {
    if (!sl_can_edit_search(l)) return false;
    const char *before = l->filter ? l->filter : "";
    const char *txt = tui_key_text(k);
    bool printable = k->kind == TUI_KEY_CHAR && txt && *txt;
    if (!*before) {
        if (tui_kb(k, "tui.editor.deleteCharBackward")) return false;
        if (printable) {
            bool ws = true;
            for (const char *p = txt; *p; p++)
                if (!isspace((unsigned char)*p)) ws = false;
            if (ws) return false;
        }
    }
    size_t len = strlen(before);
    char *next = NULL;
    if (printable) {
        next = xasprintf("%s%s", before, txt);
    } else if (tui_kb(k, "tui.editor.deleteCharBackward")) {
        if (!len) return true;
        size_t i = len - 1;
        while (i > 0 && ((unsigned char)before[i] & 0xC0) == 0x80) i--;
        next = xstrndup(before, i);
    } else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        size_t i = len;
        while (i > 0 && before[i - 1] == ' ') i--;
        while (i > 0 && before[i - 1] != ' ') i--;
        next = xstrndup(before, i);
    } else if (tui_kb(k, "tui.editor.deleteToLineStart")) {
        next = xstrdup("");
    } else {
        return false;
    }
    if (!strcmp(next, before)) {
        free(next);
        return true;
    }
    if (!*next) {
        free(next);
        next = NULL;
    }
    sl_set_query(l, next);
    return true;
}

static bool select_key(tui_component_t *c, const tui_key_t *k) {
    tui_select_list_t *l = (tui_select_list_t *)c;
    if (tui_kb(k, "tui.select.cancel")) {
        if (l->on_cancel) l->on_cancel(l, l->ud);
        return true;
    }
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_CHAR && txt && *txt && sl_search_input(l, k)) {
        tui_component_request_render(c);
        return true;
    }
    bool wrap = !l->layout.no_wrap_navigation;
    if (tui_kb(k, "tui.select.up")) tui_select_list_move(l, -1, wrap);
    else if (tui_kb(k, "tui.select.down")) tui_select_list_move(l, 1, wrap);
    else if (tui_kb(k, "tui.select.pageUp")) tui_select_list_move(l, -MAX(1, l->max_visible), false);
    else if (tui_kb(k, "tui.select.pageDown")) tui_select_list_move(l, MAX(1, l->max_visible), false);
    else if (tui_key_matches(k, "home") || tui_key_matches(k, "end")) {
        bool home = tui_key_matches(k, "home");
        size_t nx = sl_coerce(l, home ? 0 : (long)l->visible.n - 1, home ? 1 : -1);
        if (l->visible.n && nx != l->selected) {
            l->selected = nx;
            sl_notify(l);
        }
    } else if (tui_kb(k, "tui.select.confirm") || tui_key_matches(k, "ctrl+j")) {
        const tui_select_item_t *it = tui_select_list_selected(l);
        if (it && !it->disabled && l->on_select) l->on_select(l, it, l->ud);
        return true;
    } else if (!sl_search_input(l, k)) {
        return false;
    }
    tui_component_request_render(c);
    return true;
}

static void select_destroy(tui_component_t *c) {
    tui_select_list_t *l = (tui_select_list_t *)c;
    tui_select_list_clear(l);
    vec_free(&l->items);
    vec_free(&l->visible);
    free(l->filter);
    free(l->cursor);
    free(l);
}

static const tui_component_vt SELECT_VT = {"select-list", select_render, select_key, NULL, select_destroy};

tui_select_list_t *tui_select_list_new(int max_visible) {
    tui_select_list_t *l = xcalloc(1, sizeof *l);
    tui_component_init(&l->base, &SELECT_VT);
    l->max_visible = max_visible > 0 ? max_visible : 5;
    l->filter_enabled = true;
    const char *cur = tui_sym("nav.cursor");
    l->cursor = xstrdup(cur && *cur ? cur : ">");
    return l;
}

tui_select_item_t *tui_select_list_add_item(tui_select_list_t *l, const char *value, const char *label,
                                            const char *description, const char *icon, bool disabled) {
    tui_select_item_t it = {xstrdup(value ? value : ""), label ? xstrdup(label) : NULL,
                            description ? xstrdup(description) : NULL, icon ? xstrdup(icon) : NULL, NULL, disabled};
    vec_push(&l->items, it);
    select_refilter(l);
    return &l->items.v[l->items.n - 1];
}

void tui_select_list_add(tui_select_list_t *l, const char *value, const char *label, const char *description) {
    tui_select_list_add_item(l, value, label, description, NULL, false);
}

void tui_select_list_clear(tui_select_list_t *l) {
    for (size_t i = 0; i < l->items.n; i++) {
        free(l->items.v[i].value);
        free(l->items.v[i].label);
        free(l->items.v[i].description);
        free(l->items.v[i].icon);
        free(l->items.v[i].hint);
    }
    l->items.n = 0;
    l->visible.n = 0;
    l->selected = 0;
}

void tui_select_list_set_filter(tui_select_list_t *l, const char *filter) {
    free(l->filter);
    l->filter = filter && *filter ? xstrdup(filter) : NULL;
    select_refilter(l);
    tui_component_request_render(&l->base);
}

const tui_select_item_t *tui_select_list_selected(const tui_select_list_t *l) {
    if (!l->visible.n || l->selected >= l->visible.n) return NULL;
    return sl_vis(l, l->selected);
}

void tui_select_list_set_selected(tui_select_list_t *l, size_t visible_index) {
    if (!l->visible.n) return;
    l->selected = sl_coerce(l, (long)visible_index, 1);
}

bool tui_select_list_select_value(tui_select_list_t *l, const char *value) {
    for (size_t i = 0; i < l->visible.n; i++)
        if (str_eq(sl_vis(l, i)->value, value)) {
            l->selected = sl_coerce(l, (long)i, 1);
            return true;
        }
    return false;
}

void tui_select_list_set_max_visible(tui_select_list_t *l, int rows) { l->max_visible = MAX(1, rows); }

size_t tui_select_list_visible_count(const tui_select_list_t *l) { return l->visible.n; }
