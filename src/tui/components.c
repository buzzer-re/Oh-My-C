/* components.c - Text, TruncatedText, Box, Spacer, Loader, SelectList */
#include "tui/components.h"

#include <ctype.h>

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

/* ---------- SelectList ---------- */

static const char *item_label(const tui_select_item_t *it) { return it->label ? it->label : it->value; }

static int cmp_scored(const void *a, const void *b) {
    const int64_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] > y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

static void select_refilter(tui_select_list_t *l) {
    l->visible.n = 0;
    if (!l->filter || !*l->filter) {
        for (size_t i = 0; i < l->items.n; i++) vec_push(&l->visible, i);
    } else {
        int64_t(*sc)[2] = xcalloc(l->items.n ? l->items.n : 1, sizeof *sc);
        size_t n = 0;
        for (size_t i = 0; i < l->items.n; i++) {
            int s = tui_fuzzy_score(l->filter, item_label(&l->items.v[i]));
            int s2 = tui_fuzzy_score(l->filter, l->items.v[i].value);
            if (s2 > s) s = s2;
            if (s >= 0) {
                sc[n][0] = s;
                sc[n][1] = (int64_t)i;
                n++;
            }
        }
        qsort(sc, n, sizeof *sc, cmp_scored);
        for (size_t i = 0; i < n; i++) vec_push(&l->visible, (size_t)sc[i][1]);
        free(sc);
    }
    if (l->selected >= l->visible.n) l->selected = l->visible.n ? l->visible.n - 1 : 0;
}

static void select_render(tui_component_t *c, int width, strvec_t *out) {
    tui_select_list_t *l = (tui_select_list_t *)c;
    const tui_theme_t *th = tui_theme();
    size_t n = l->visible.n;
    if (n == 0) {
        bool q = l->filter && *l->filter;
        char *m = tui_fgs(th, TUI_TC_MUTED, q ? "  No matching items" : "  No items");
        strvec_push_own(out, m);
        return;
    }
    int maxv = MAX(1, l->max_visible);
    size_t start = 0;
    if ((int)n > maxv) {
        long s = (long)l->selected - maxv / 2;
        if (s < 0) s = 0;
        if (s > (long)n - maxv) s = (long)n - maxv;
        start = (size_t)s;
    }
    size_t end = MIN(n, start + (size_t)maxv);
    int label_w = 0;
    bool any_desc = false;
    for (size_t i = start; i < end; i++) {
        const tui_select_item_t *it = &l->items.v[l->visible.v[i]];
        label_w = MAX(label_w, tui_visible_width(item_label(it)));
        if (it->description && *it->description) any_desc = true;
    }
    int prefix_w = 2;
    int col_w = MIN(label_w, MAX(10, (width - prefix_w) / 2));
    for (size_t i = start; i < end; i++) {
        const tui_select_item_t *it = &l->items.v[l->visible.v[i]];
        bool sel = i == l->selected;
        sb_t b = SB_INIT;
        int avail = width - prefix_w;
        bool show_desc = any_desc && it->description && *it->description && width > 40;
        int lw = show_desc ? col_w : avail;
        char *lab = tui_truncate_to_width(item_label(it), MAX(1, lw), NULL);
        if (sel) {
            tui_fg(&b, th, TUI_TC_ACCENT, "\xe2\x86\x92 ");
            tui_fg(&b, th, TUI_TC_ACCENT, lab);
        } else {
            sb_append(&b, "  ");
            sb_append(&b, lab);
        }
        if (show_desc) {
            int used = prefix_w + tui_visible_width(lab);
            int gap = prefix_w + col_w + 2 - used;
            int dw = width - (prefix_w + col_w + 2);
            if (dw > 4) {
                tui_sb_spaces(&b, gap);
                char *one = str_replace(it->description, "\n", " ");
                char *d = tui_truncate_to_width(one, dw, NULL);
                tui_fg(&b, th, sel ? TUI_TC_ACCENT : TUI_TC_MUTED, d);
                free(d);
                free(one);
            }
        }
        free(lab);
        char *line = tui_truncate_to_width(sb_cstr(&b), width, "");
        sb_free(&b);
        strvec_push_own(out, line);
    }
    if ((int)n > maxv || (l->filter && *l->filter)) {
        char *st;
        if (l->filter && *l->filter) st = xasprintf("  (%zu/%zu) filter: %s", l->selected + 1, n, l->filter);
        else st = xasprintf("  (%zu/%zu)", l->selected + 1, n);
        char *tr = tui_truncate_to_width(st, width, NULL);
        strvec_push_own(out, tui_fgs(th, TUI_TC_MUTED, tr));
        free(tr);
        free(st);
    }
}

void tui_select_list_move(tui_select_list_t *l, int delta, bool wrap) {
    size_t n = l->visible.n;
    if (!n) return;
    long s = (long)l->selected + delta;
    if (wrap && (delta == 1 || delta == -1)) {
        s = (s % (long)n + (long)n) % (long)n;
    } else {
        if (s < 0) s = 0;
        if (s >= (long)n) s = (long)n - 1;
    }
    if ((size_t)s != l->selected) {
        l->selected = (size_t)s;
        if (l->on_change) l->on_change(l, tui_select_list_selected(l), l->ud);
    }
}

static bool select_key(tui_component_t *c, const tui_key_t *k) {
    tui_select_list_t *l = (tui_select_list_t *)c;
    if (tui_kb(k, "tui.select.cancel")) {
        if (l->on_cancel) l->on_cancel(l, l->ud);
        return true;
    }
    if (tui_kb(k, "tui.select.up")) tui_select_list_move(l, -1, true);
    else if (tui_kb(k, "tui.select.down")) tui_select_list_move(l, 1, true);
    else if (tui_kb(k, "tui.select.pageUp")) tui_select_list_move(l, -MAX(1, l->max_visible), false);
    else if (tui_kb(k, "tui.select.pageDown")) tui_select_list_move(l, MAX(1, l->max_visible), false);
    else if (tui_key_matches(k, "home")) tui_select_list_move(l, -(int)l->visible.n, false);
    else if (tui_key_matches(k, "end")) tui_select_list_move(l, (int)l->visible.n, false);
    else if (tui_kb(k, "tui.select.confirm") || tui_key_matches(k, "ctrl+j")) {
        const tui_select_item_t *it = tui_select_list_selected(l);
        if (it && l->on_select) l->on_select(l, it, l->ud);
    } else if (l->filter_enabled && k->kind == TUI_KEY_BACKSPACE) {
        size_t len = l->filter ? strlen(l->filter) : 0;
        if (!len) return true;
        size_t i = len - 1;
        while (i > 0 && ((unsigned char)l->filter[i] & 0xC0) == 0x80) i--;
        l->filter[i] = 0;
        select_refilter(l);
    } else if (l->filter_enabled && tui_key_text(k) && k->kind == TUI_KEY_CHAR) {
        char *nf = xasprintf("%s%s", l->filter ? l->filter : "", tui_key_text(k));
        free(l->filter);
        l->filter = nf;
        l->selected = 0;
        select_refilter(l);
    } else {
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
    free(l);
}

static const tui_component_vt SELECT_VT = {"select-list", select_render, select_key, NULL, select_destroy};

tui_select_list_t *tui_select_list_new(int max_visible) {
    tui_select_list_t *l = xcalloc(1, sizeof *l);
    tui_component_init(&l->base, &SELECT_VT);
    l->max_visible = max_visible > 0 ? max_visible : 5;
    l->filter_enabled = true;
    return l;
}

void tui_select_list_add(tui_select_list_t *l, const char *value, const char *label, const char *description) {
    tui_select_item_t it = {xstrdup(value ? value : ""), label ? xstrdup(label) : NULL,
                            description ? xstrdup(description) : NULL};
    vec_push(&l->items, it);
    select_refilter(l);
}

void tui_select_list_clear(tui_select_list_t *l) {
    for (size_t i = 0; i < l->items.n; i++) {
        free(l->items.v[i].value);
        free(l->items.v[i].label);
        free(l->items.v[i].description);
    }
    l->items.n = 0;
    l->visible.n = 0;
    l->selected = 0;
}

void tui_select_list_set_filter(tui_select_list_t *l, const char *filter) {
    free(l->filter);
    l->filter = filter && *filter ? xstrdup(filter) : NULL;
    l->selected = 0;
    select_refilter(l);
    tui_component_request_render(&l->base);
}

const tui_select_item_t *tui_select_list_selected(const tui_select_list_t *l) {
    if (!l->visible.n || l->selected >= l->visible.n) return NULL;
    return &l->items.v[l->visible.v[l->selected]];
}

size_t tui_select_list_visible_count(const tui_select_list_t *l) { return l->visible.n; }
