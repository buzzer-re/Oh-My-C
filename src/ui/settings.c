/* settings.c - the /settings menu (port of omp overlays/settings-selector.ts SettingsSelectorComponent with
 * components/settings-list.ts, settings-defs.ts and the select / text / multi-select form fields).
 *
 * Definitions (tabs, groups, labels, descriptions, options, defaults, visibility conditions) come from
 * assets/tui/settings-ui.json, dumped from omp's own registry by build-tools/extract_settings.sh, so the
 * menu lists exactly omp's settings in omp's order. Fullscreen on the alternate screen: "Settings" frame,
 * tab bar (icons collapse when narrow), the active tab as a section sidebar + setting rows (rows outside
 * the selected section dimmed) or a flat list, a 3-row description area, the status-line preview on the
 * Appearance tab, and a key-hint footer. Typing searches every tab (tab bar reorders by matches). Values
 * persist to the global config.yml like omp; theme/status-line choices preview live. */
#include "ui/overlays.h"
#include "ui/ui.h"

#include "config/settings.h"
#include "coding/autolearn.h"
#include "core/embed.h"
#include "tui/composer.h"
#include "tui/fuzzy.h"

#include <ctype.h>
#include <math.h>

static char *S(tui_tc_t slot, const char *s) { return tui_fgs(NULL, slot, s); }

static bool blank_str(const char *s) {
    for (; s && *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

/* statusLine.getPreviewLines(editor.getTopBorderAvailableWidth(columns)): the status bar exactly as the
 * active composer shape renders it (box top border, band row, claude rule chip, then any bottom bar) */
char *ui_settings_status_preview(ui_t *u, int cols) {
    const tui_composer_style_t *st = tui_composer_style(ui_composer_shape());
    int pad = st->default_padding_x ? st->default_padding_x(-1) : 0;
    int chrome = st->side_chrome_width ? st->side_chrome_width(pad) : 0;
    int w = MAX(0, cols - chrome * 2), tw = 0;
    strvec_t lines = {0};
    char *top = NULL;
    if (st->status_attachment == TUI_ATTACH_TOP_BORDER) top = ui_status_top_border(u, w, NULL, &tw);
    else if (st->status_attachment == TUI_ATTACH_TOP_BAND) top = ui_status_band_top(u, w, NULL, &tw);
    else if (st->status_attachment == TUI_ATTACH_TOP_RULE_CHIP) top = ui_status_rule_chip(u, w, NULL, &tw);
    if (top && *top) strvec_push(&lines, top);
    free(top);
    if (st->bottom_bar != TUI_BOTTOM_NONE) strvec_push_own(&lines, ui_status_bottom_bar(u, w, st->bottom_bar == TUI_BOTTOM_FULL, NULL));
    char *r = strvec_join(&lines, "\n");
    strvec_free(&lines);
    return r;
}

/* ---------- definitions ---------- */

typedef struct {
    char *value, *label, *description;
} sopt_t;

typedef struct {
    const char *path, *schema, *type, *group, *label, *description, *warning, *condition;
    int tab;
    const json_t *def; /* default value (NULL = undefined) */
    const json_t *values;
    sopt_t *opts;
    size_t nopt;
    bool secret, ordered;
} sdef_t;

#define NTABS 10
typedef struct {
    const char *id, *label, *icon;
    const json_t *groups;
} stab_t;

static json_t *g_json;
static sdef_t *g_defs;
static size_t g_ndefs;
static stab_t g_tabs[NTABS];
static size_t g_ntabs;
static int g_sidebar_width;

static int group_rank(const sdef_t *d) {
    if (!d->group) return -1;
    const json_t *gs = g_tabs[d->tab].groups;
    for (size_t i = 0; i < json_len(gs); i++)
        if (str_eq(json_sv(json_at(gs, i)), d->group)) return (int)i;
    return (int)json_len(gs);
}

static void load_defs(void) {
    if (g_json) return;
    size_t len = 0;
    const char *data = embed_get("assets/tui/settings-ui.json", &len);
    g_json = data ? json_parse(data, len, NULL) : NULL;
    if (!g_json) g_json = json_obj();
    json_t *t;
    json_arr_foreach(json_get_arr(g_json, "tabs"), i, t) {
        if (g_ntabs >= NTABS) break;
        g_tabs[g_ntabs++] = (stab_t){json_get_str(t, "id"), json_get_str(t, "label"), json_get_str(t, "icon"),
                                     json_get_arr(t, "groups")};
    }
    json_t *ss = json_get_arr(g_json, "settings");
    g_defs = xcalloc(json_len(ss) + 1, sizeof *g_defs);
    json_t *s;
    json_arr_foreach(ss, i, s) {
        sdef_t *d = &g_defs[g_ndefs];
        d->path = json_get_str(s, "path");
        d->schema = json_get_str(s, "schemaType");
        d->type = json_get_str(s, "type");
        d->group = json_get_str(s, "group");
        d->label = json_get_str(s, "label");
        d->description = json_get_str(s, "description");
        d->warning = json_get_str(s, "warning");
        d->condition = json_get_str(s, "condition");
        d->def = json_get(s, "default");
        d->values = json_get_arr(s, "values");
        d->secret = json_get_bool(s, "secret", false);
        d->ordered = json_get_bool(s, "ordered", false);
        d->tab = -1;
        for (size_t k = 0; k < g_ntabs; k++)
            if (str_eq(g_tabs[k].id, json_get_str(s, "tab"))) d->tab = (int)k;
        json_t *opts = json_get_arr(s, "options");
        d->opts = xcalloc(json_len(opts) + 1, sizeof *d->opts);
        json_t *o;
        json_arr_foreach(opts, k, o) {
            const char *v = json_get_str(o, "value"), *l = json_get_str(o, "label"), *de = json_get_str(o, "description");
            d->opts[d->nopt++] = (sopt_t){xstrdup(v ? v : ""), xstrdup(l ? l : (v ? v : "")), de ? xstrdup(de) : NULL};
        }
        if (d->path && d->label && d->type && d->tab >= 0) g_ndefs++;
    }
    int nw = 0;
    for (size_t i = 0; i < g_ndefs; i++)
        if (g_defs[i].group) nw = MAX(nw, tui_visible_width(g_defs[i].group));
    g_sidebar_width = MIN(22, nw) + 4;
}

static const sdef_t *def_by_path(const char *path) {
    for (size_t i = 0; i < g_ndefs; i++)
        if (!strcmp(g_defs[i].path, path)) return &g_defs[i];
    return NULL;
}

/* definitions of a tab, ordered by TAB_GROUPS (stable) */
static size_t tab_defs(int tab, const sdef_t **out) {
    size_t n = 0;
    for (size_t i = 0; i < g_ndefs; i++)
        if (g_defs[i].tab == tab) out[n++] = &g_defs[i];
    for (size_t i = 1; i < n; i++) { /* insertion sort: stable */
        const sdef_t *x = out[i];
        int r = group_rank(x);
        size_t j = i;
        while (j > 0 && group_rank(out[j - 1]) > r) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = x;
    }
    return n;
}

/* ---------- values ---------- */

static const json_t *get_value(const sdef_t *d) {
    const json_t *v = settings_get(d->path);
    return v ? v : d->def;
}

static bool get_bool(const char *path, bool def) {
    const sdef_t *d = def_by_path(path);
    const json_t *v = d ? get_value(d) : settings_get(path);
    return json_is_bool(v) ? v->u.b : def;
}

static const char *get_str(const char *path, const char *def) {
    const sdef_t *d = def_by_path(path);
    const json_t *v = d ? get_value(d) : settings_get(path);
    return json_is_str(v) ? json_sv(v) : def;
}

static bool condition_ok(const char *c) {
    if (!c) return true;
    if (!strcmp(c, "macOS")) {
#ifdef __APPLE__
        return true;
#else
        return false;
#endif
    }
    if (!strcmp(c, "hasImageProtocol") || !strcmp(c, "nativeRendering")) return false;
    if (!strcmp(c, "advisorEnabled")) return get_bool("advisor.enabled", false);
    if (!strcmp(c, "vimModeEnabled")) return get_bool("tui.vimMode", false);
    if (!strcmp(c, "hindsightActive")) return str_eq(get_str("memory.backend", "off"), "hindsight");
    if (!strcmp(c, "mnemopiActive")) return str_eq(get_str("memory.backend", "off"), "mnemopi");
    if (!strcmp(c, "autolearnActive")) return get_bool("autolearn.enabled", false);
    if (!strcmp(c, "autoThinkingActive")) return str_eq(get_str("defaultThinkingLevel", "high"), "auto");
    if (!strcmp(c, "usageAwareFallbackEnabled")) return get_bool("retry.usageAwareFallback", false);
    if (!strcmp(c, "planModeEnabled")) return get_bool("plan.enabled", true);
    if (!strcmp(c, "planAutosaveEnabled")) return get_bool("plan.enabled", true) && get_bool("plan.autosave", false);
    return true; /* *NotProject: the global layer edits apply */
}

/* JavaScript String(number) */
static char *js_num(double x) {
    if (isnan(x)) return xstrdup("NaN");
    if (isinf(x)) return xstrdup(x > 0 ? "Infinity" : "-Infinity");
    if (x == floor(x) && fabs(x) < 1e21) return xasprintf("%.0f", x);
    for (int p = 1; p <= 17; p++) {
        char *s = xasprintf("%.*g", p, x);
        if (strtod(s, NULL) == x) return s;
        free(s);
    }
    return xasprintf("%.17g", x);
}

/* String(value ?? "") with objects as JSON (formatTextInputEditValue) */
static char *value_str(const json_t *v) {
    if (!v || json_is_null(v)) return xstrdup("");
    if (json_is_str(v)) return xstrdup(json_sv(v));
    if (json_is_bool(v)) return xstrdup(v->u.b ? "true" : "false");
    if (json_is_num(v)) return js_num(v->u.num.d);
    return json_dump(v, 0);
}

static char *multiselect_str(const sdef_t *d, const json_t *v) {
    sb_t b = SB_INIT;
    int n = 0;
    json_t *e;
    json_arr_foreach(json_is_arr(v) ? v : NULL, i, e) {
        if (!json_is_str(e)) continue;
        for (size_t k = 0; k < d->nopt; k++)
            if (!strcmp(d->opts[k].value, json_sv(e))) {
                sb_append(&b, n++ ? (d->ordered ? " \xe2\x86\x92 " : ", ") : "");
                sb_append(&b, d->opts[k].label);
                break;
            }
    }
    if (!n) {
        sb_free(&b);
        return xstrdup(d->ordered ? "default" : "none");
    }
    return sb_detach(&b);
}

static char *display_value(const sdef_t *d) {
    const json_t *v = get_value(d);
    if (!strcmp(d->type, "boolean")) return xstrdup(json_truthy(v) ? "true" : "false");
    if (!strcmp(d->type, "multiselect")) return multiselect_str(d, v);
    if (!strcmp(d->type, "text") && d->secret) return xstrdup(json_truthy(v) ? "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2" : "");
    if (!strcmp(d->type, "providerLimits")) { /* formatProviderLimitsValue */
        strvec_t parts = {0};
        json_t *e;
        const char *key;
        const json_t *ov = json_is_obj(v) ? v : NULL;
        json_obj_foreach(ov, ki, key, e) {
            if (!json_is_num(e) || e->u.num.d <= 0) continue;
            char *n = js_num(e->u.num.d);
            strvec_push_own(&parts, xasprintf("%s: %s", key, n));
            free(n);
        }
        strvec_sort(&parts);
        char *r = parts.n ? strvec_join(&parts, ", ") : xstrdup("Unlimited");
        strvec_free(&parts);
        return r;
    }
    char *s = value_str(v);
    if ((!strcmp(d->path, "compaction.thresholdPercent") || !strcmp(d->path, "compaction.thresholdTokens")) &&
        (!*s || !strcmp(s, "-1"))) {
        free(s);
        return xstrdup("default");
    }
    return s;
}

static bool is_changed(const sdef_t *d) {
    const json_t *v = get_value(d);
    if (!v && !d->def) return false;
    if (!v || !d->def) return true;
    return !json_equal(v, d->def);
}

/* ---------- items / list (components/settings-list.ts) ---------- */

typedef struct {
    const sdef_t *def; /* NULL for headings */
    char *label;
    char *value;
    bool heading, changed;
    char *id; /* setting path or heading id */
} sitem_t;

typedef struct {
    VEC(sitem_t) items;
    size_t sel;
    bool section_focus, flat;
    int max_visible;
} slist_t;

static void items_free(slist_t *l) {
    for (size_t i = 0; i < l->items.n; i++) {
        free(l->items.v[i].label);
        free(l->items.v[i].value);
        free(l->items.v[i].id);
    }
    l->items.n = 0;
}

static void push_heading(slist_t *l, const char *label, const char *id) {
    sitem_t it = {0};
    it.heading = true;
    it.label = xstrdup(label);
    it.value = xstrdup("");
    it.id = xstrdup(id);
    vec_push(&l->items, it);
}

static bool push_def(slist_t *l, const sdef_t *d) {
    if (!condition_ok(d->condition)) return false;
    sitem_t it = {0};
    it.def = d;
    it.label = xstrdup(d->label);
    it.value = display_value(d);
    it.changed = is_changed(d);
    it.id = xstrdup(d->path);
    vec_push(&l->items, it);
    return true;
}

static const sitem_t *list_selected(const slist_t *l) {
    if (l->sel >= l->items.n || l->items.v[l->sel].heading) return NULL;
    return &l->items.v[l->sel];
}

static void list_first_selectable(slist_t *l) {
    l->sel = 0;
    while (l->sel < l->items.n && l->items.v[l->sel].heading) l->sel++;
    if (l->sel >= l->items.n) l->sel = 0;
}

/* replace items keeping the selection by id (else clamp to the previous index) */
static void list_reselect(slist_t *l, const char *keep_id, size_t prev_index) {
    if (keep_id)
        for (size_t i = 0; i < l->items.n; i++)
            if (!l->items.v[i].heading && !strcmp(l->items.v[i].id, keep_id)) {
                l->sel = i;
                return;
            }
    if (!l->items.n) {
        l->sel = 0;
        return;
    }
    l->sel = MIN(prev_index, l->items.n - 1);
    while (l->sel < l->items.n && l->items.v[l->sel].heading) l->sel++;
    if (l->sel >= l->items.n) {
        l->sel = l->items.n - 1;
        while (l->sel > 0 && l->items.v[l->sel].heading) l->sel--;
    }
}

static void list_move(slist_t *l, int delta) {
    size_t n = l->items.n;
    if (!n) return;
    size_t i = l->sel;
    for (size_t step = 0; step < n; step++) {
        i = (i + n + (size_t)(delta < 0 ? n - 1 : 1)) % n;
        if (!l->items.v[i].heading) {
            l->sel = i;
            return;
        }
    }
}

typedef struct {
    const char *name;
    size_t first, last;
} section_t;

static size_t list_sections(const slist_t *l, section_t *out) {
    size_t n = 0;
    bool cur = false;
    for (size_t i = 0; i < l->items.n; i++) {
        const sitem_t *it = &l->items.v[i];
        if (it->heading) {
            out[n] = (section_t){it->label, SIZE_MAX, SIZE_MAX};
            n++;
            cur = true;
            continue;
        }
        if (!cur) {
            out[n++] = (section_t){"", i, i};
            cur = true;
        }
        if (out[n - 1].first == SIZE_MAX) out[n - 1].first = i;
        out[n - 1].last = i;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (out[i].first != SIZE_MAX) out[k++] = out[i];
    return k;
}

static size_t active_section(const slist_t *l, const section_t *s, size_t n) {
    for (size_t i = n; i-- > 0;)
        if (s[i].first <= l->sel) return i;
    return 0;
}

static void list_jump_section(slist_t *l, int delta) {
    section_t *s = xcalloc(l->items.n + 1, sizeof *s);
    size_t n = list_sections(l, s);
    if (n < 2) {
        size_t len = l->items.n;
        if (len) {
            long idx = (long)l->sel + (long)delta * l->max_visible;
            l->sel = (size_t)MAX(0, MIN(idx, (long)len - 1));
        }
    } else {
        size_t a = active_section(l, s, n);
        l->sel = s[(a + n + (size_t)(delta < 0 ? n - 1 : 1)) % n].first;
    }
    free(s);
}

static char *warning_mark(const sitem_t *it) {
    return it->def && it->def->warning ? xasprintf(" %s", tui_sym("status.warning")) : xstrdup("");
}

static char *heading_style(const char *text, bool dimmed) {
    char *u = tui_underline(text), *r;
    if (dimmed) r = S(TUI_TC_DIM, u);
    else {
        char *b = tui_bold(u);
        r = S(TUI_TC_MUTED, b);
        free(b);
    }
    free(u);
    return r;
}

static char *cursor_glyph(void) {
    char *c = xasprintf("%s ", tui_sym("nav.cursor")), *r = S(TUI_TC_ACCENT, c);
    free(c);
    return r;
}

static char *label_style(const char *t, bool sel, bool changed) {
    return changed ? S(TUI_TC_STATUS_LINE_GIT_DIRTY, t) : sel ? S(TUI_TC_ACCENT, t) : xstrdup(t);
}
static char *value_style(const char *t, bool sel, bool changed) {
    return changed ? S(TUI_TC_STATUS_LINE_GIT_DIRTY, t) : sel ? S(TUI_TC_ACCENT, t) : S(TUI_TC_MUTED, t);
}

static char *render_row(const slist_t *l, size_t index, int max_label, int row_width, bool dimmed, bool heading_cursor) {
    const sitem_t *it = &l->items.v[index];
    if (it->heading) {
        char *pre = heading_cursor ? cursor_glyph() : xstrdup("  "), *h = heading_style(it->label, dimmed);
        char *line = xasprintf("%s%s", pre, h), *r = tui_truncate_to_width(line, MAX(0, row_width), NULL);
        free(pre);
        free(h);
        free(line);
        return r;
    }
    bool selected = index == l->sel && !l->section_focus;
    char *prefix = selected ? cursor_glyph() : xstrdup("  ");
    int pw = tui_visible_width(prefix);
    char *mark = warning_mark(it);
    char *lp = xasprintf("%s%s", it->label, mark);
    int pad = MAX(0, max_label - tui_visible_width(lp));
    int vmax = row_width - pw - max_label - 2 - 2;
    char *vp = tui_truncate_to_width(it->value, MAX(0, vmax), "");
    char *r;
    if (dimmed && !selected) {
        char *t = xasprintf("  %s%*s  %s", lp, pad, "", vp), *tr = tui_truncate_to_width(t, MAX(0, row_width), NULL);
        r = S(TUI_TC_DIM, tr);
        free(t);
        free(tr);
    } else {
        char *lt = label_style(it->label, selected, it->changed);
        char *mk = *mark ? S(TUI_TC_WARNING, mark) : xstrdup("");
        char *vt = value_style(vp, selected, it->changed);
        char *t = xasprintf("%s%s%s%*s  %s", prefix, lt, mk, pad, "", vt);
        r = tui_truncate_to_width(t, MAX(0, row_width), NULL);
        free(lt);
        free(mk);
        free(vt);
        free(t);
    }
    free(prefix);
    free(mark);
    free(lp);
    free(vp);
    return r;
}

static int max_label_width(const slist_t *l) {
    int w = 0;
    for (size_t i = 0; i < l->items.n; i++) {
        if (l->items.v[i].heading) continue;
        char *m = warning_mark(&l->items.v[i]), *t = xasprintf("%s%s", l->items.v[i].label, m);
        w = MAX(w, tui_visible_width(t));
        free(m);
        free(t);
    }
    return MIN(30, w);
}

/* ScrollView over a window of rows with totalRows/offset (track dim, thumb accent) */
static void window_view(strvec_t *rows, int height, int total, int offset, int width, strvec_t *out) {
    bool bar = total > height && width > 1;
    int cw = MAX(0, width - (bar ? 1 : 0));
    int ts = 0, te = height;
    if (bar) {
        int size = MAX(1, MIN((height * height) / total, height)), maxoff = MAX(0, total - height);
        int off = MAX(0, MIN(offset, maxoff));
        ts = maxoff ? (int)lround((double)off / maxoff * (height - size)) : 0;
        te = ts + size;
    }
    for (int r = 0; r < height; r++) {
        const char *src = r < (int)rows->n ? rows->v[r] : "";
        char *rt = tui_replace_tabs(src), *tr = tui_truncate_to_width(rt, cw, NULL);
        free(rt);
        if (!bar) {
            strvec_push_own(out, tr);
            continue;
        }
        char *pad = tui_pad_to_width(tr, cw);
        bool thumb = r >= ts && r < te;
        char *g = S(thumb ? TUI_TC_ACCENT : TUI_TC_DIM, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
        strvec_push_own(out, xasprintf("%s%s", pad, g));
        free(g);
        free(pad);
        free(tr);
    }
}

static bool render_split(const slist_t *l, int width, const section_t *secs, size_t ns, strvec_t *out) {
    int sw = g_sidebar_width, pane = width - sw - 2;
    if (pane < 60) return false;
    size_t ai = active_section(l, secs, ns);
    const section_t *act = &secs[ai];
    strvec_t side = {0};
    for (size_t i = 0; i < ns; i++) {
        const char *name = *secs[i].name ? secs[i].name : "Other";
        char *lab = tui_truncate_to_width(name, sw - 4, "");
        char *pre = l->section_focus && i == ai ? cursor_glyph() : xstrdup("  ");
        char *st;
        if (i == ai) {
            char *b = tui_bold(lab);
            st = S(TUI_TC_ACCENT, b);
            free(b);
        } else st = S(TUI_TC_MUTED, lab);
        int padn = MAX(0, sw - tui_visible_width(pre) - tui_visible_width(lab));
        strvec_push_own(&side, xasprintf("%s%s%*s", pre, st, padn, ""));
        free(lab);
        free(pre);
        free(st);
    }
    size_t act_start = *act->name ? act->first - 1 : act->first;
    int n = (int)l->items.n, vh = MIN(l->max_visible, n);
    int start = MAX(0, MIN((int)l->sel - vh / 2, n - vh));
    int ml = max_label_width(l);
    bool overflow = n > vh;
    int rw = MAX(0, pane - (overflow ? 1 : 0));
    strvec_t rows = {0};
    for (int r = 0; r < vh; r++) {
        size_t idx = (size_t)(start + r);
        if (idx >= l->items.n) break;
        bool dimmed = idx < act_start || idx > act->last;
        strvec_push_own(&rows, render_row(l, idx, ml, rw, dimmed, false));
    }
    strvec_t pr = {0};
    window_view(&rows, vh, n, start, pane, &pr);
    char *sep = S(TUI_TC_DIM, "\xe2\x94\x82 ");
    int h = MAX(l->max_visible, (int)side.n);
    for (int i = 0; i < h; i++) {
        char *left = i < (int)side.n ? xstrdup(side.v[i]) : xasprintf("%*s", sw, "");
        char *line = xasprintf("%s%s%s", left, sep, i < (int)pr.n ? pr.v[i] : "");
        strvec_push_own(out, tui_truncate_to_width(line, width, NULL));
        free(line);
        free(left);
    }
    free(sep);
    strvec_free(&side);
    strvec_free(&rows);
    strvec_free(&pr);
    return true;
}

static void render_list(slist_t *l, int width, const char *empty_text, strvec_t *out) {
    size_t before = out->n;
    int stable = l->max_visible + 4;
    if (!l->items.n) {
        char *t = xasprintf("  %s", empty_text ? empty_text : "No settings available");
        strvec_push_own(out, S(TUI_TC_DIM, t));
        free(t);
    } else {
        section_t *secs = xcalloc(l->items.n + 1, sizeof *secs);
        size_t ns = list_sections(l, secs);
        bool split = !l->flat && ns >= 2 && render_split(l, width, secs, ns, out);
        if (!split) {
            int n = (int)l->items.n, vh = MIN(l->max_visible, n);
            int start = MAX(0, MIN((int)l->sel - vh / 2, n - vh));
            int ml = max_label_width(l);
            int rw = MAX(0, width - (n > vh ? 1 : 0));
            size_t ai = ns ? active_section(l, secs, ns) : 0;
            long fh = l->section_focus && ns && *secs[ai].name ? (long)secs[ai].first - 1 : -1;
            strvec_t rows = {0};
            for (int r = 0; r < vh; r++)
                strvec_push_own(&rows, render_row(l, (size_t)(start + r), ml, rw, false, (long)(start + r) == fh));
            window_view(&rows, vh, n, start, width, out);
            strvec_free(&rows);
            while ((int)(out->n - before) < l->max_visible) strvec_push(out, "");
        }
        free(secs);
        strvec_push(out, "");
        const sitem_t *it = list_selected(l);
        strvec_t desc = {0};
        if (it) {
            if (it->def->warning) {
                char *t = xasprintf("%s %s", tui_sym("status.warning"), it->def->warning);
                strvec_t w = {0};
                tui_wrap_text_with_ansi(t, MAX(1, width - 4), &w);
                for (size_t i = 0; i < w.n; i++) {
                    char *x = xasprintf("  %s", w.v[i]);
                    strvec_push_own(&desc, S(TUI_TC_WARNING, x));
                    free(x);
                }
                strvec_free(&w);
                free(t);
            }
            if (it->def->description && *it->def->description) {
                strvec_t w = {0};
                tui_wrap_text_with_ansi(it->def->description, MAX(1, width - 4), &w);
                for (size_t i = 0; i < w.n; i++) {
                    char *x = xasprintf("  %s", w.v[i]);
                    strvec_push_own(&desc, S(TUI_TC_DIM, x));
                    free(x);
                }
                strvec_free(&w);
            }
            if (desc.n > 3) {
                while (desc.n > 3) free(desc.v[--desc.n]);
                char *x = xasprintf("%s\xe2\x80\xa6", desc.v[2]);
                free(desc.v[2]);
                desc.v[2] = tui_truncate_to_width(x, width, NULL);
                free(x);
            }
        }
        while (desc.n < 3) strvec_push(&desc, "");
        for (size_t i = 0; i < desc.n; i++) strvec_push(out, desc.v[i]);
        strvec_free(&desc);
    }
    while ((int)(out->n - before) < stable) strvec_push(out, "");
}

/* ---------- single-line input (components/input.ts, inverse-cell cursor) ---------- */

typedef struct {
    sb_t text;
    size_t cur; /* byte offset */
    bool mask;
} sinput_t;

static void input_set(sinput_t *in, const char *s) {
    sb_clear(&in->text);
    sb_append(&in->text, s ? s : "");
    in->cur = in->text.len;
}

static size_t prev_cp(const char *s, size_t i) {
    while (i > 0 && ((unsigned char)s[--i] & 0xC0) == 0x80) {
    }
    return i;
}
static size_t next_cp(const char *s, size_t n, size_t i) {
    if (i >= n) return n;
    i++;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

/* edit keys; returns true when the key was an editing key */
static bool input_key(sinput_t *in, const tui_key_t *k) {
    const char *s = in->text.s ? in->text.s : "";
    size_t n = in->text.len;
    const char *txt = tui_key_text(k);
    if (k->kind == TUI_KEY_PASTE && k->text) {
        char *ins = xstrdup(k->text);
        for (char *p = ins; *p; p++)
            if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        sb_t nb = SB_INIT;
        sb_appendn(&nb, s, in->cur);
        sb_append(&nb, ins);
        sb_appendn(&nb, s + in->cur, n - in->cur);
        in->cur += strlen(ins);
        sb_free(&in->text);
        in->text = nb;
        free(ins);
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteCharBackward")) {
        if (in->cur) {
            size_t p = prev_cp(s, in->cur);
            memmove(in->text.s + p, in->text.s + in->cur, n - in->cur + 1);
            in->text.len -= in->cur - p;
            in->cur = p;
        }
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteCharForward")) {
        if (in->cur < n) {
            size_t q = next_cp(s, n, in->cur);
            memmove(in->text.s + in->cur, in->text.s + q, n - q + 1);
            in->text.len -= q - in->cur;
        }
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteWordBackward")) {
        size_t p = in->cur;
        while (p > 0 && s[p - 1] == ' ') p--;
        while (p > 0 && s[p - 1] != ' ') p--;
        memmove(in->text.s + p, in->text.s + in->cur, n - in->cur + 1);
        in->text.len -= in->cur - p;
        in->cur = p;
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteToLineStart")) {
        memmove(in->text.s, in->text.s + in->cur, n - in->cur + 1);
        in->text.len -= in->cur;
        in->cur = 0;
        return true;
    }
    if (tui_kb(k, "tui.editor.deleteToLineEnd")) {
        in->text.len = in->cur;
        if (in->text.s) in->text.s[in->cur] = 0;
        return true;
    }
    if (tui_kb(k, "tui.editor.cursorLeft")) {
        in->cur = prev_cp(s, in->cur);
        return true;
    }
    if (tui_kb(k, "tui.editor.cursorRight")) {
        in->cur = next_cp(s, n, in->cur);
        return true;
    }
    if (tui_kb(k, "tui.editor.cursorLineStart")) {
        in->cur = 0;
        return true;
    }
    if (tui_kb(k, "tui.editor.cursorLineEnd")) {
        in->cur = n;
        return true;
    }
    if (k->kind == TUI_KEY_CHAR && txt && *txt && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER))) {
        sb_t nb = SB_INIT;
        sb_appendn(&nb, s, in->cur);
        sb_append(&nb, txt);
        sb_appendn(&nb, s + in->cur, n - in->cur);
        in->cur += strlen(txt);
        sb_free(&in->text);
        in->text = nb;
        return true;
    }
    return false;
}

static char *input_render(const sinput_t *in, const char *prompt, int width) {
    int avail = width - tui_visible_width(prompt);
    if (avail <= 0) return tui_truncate_to_width(prompt, width, "");
    const char *s = in->text.s ? in->text.s : "";
    sb_t shown = SB_INIT;
    size_t cur = in->cur;
    if (in->mask) {
        size_t count = 0, before = 0;
        for (size_t i = 0; i < in->text.len; i = next_cp(s, in->text.len, i)) {
            if (i < in->cur) before++;
            count++;
        }
        for (size_t i = 0; i < count; i++) sb_append(&shown, "\xe2\x80\xa2");
        cur = before * 3;
    } else sb_append(&shown, s);
    bool at_end = cur >= shown.len;
    if (at_end) sb_putc(&shown, ' ');
    /* window: keep the cursor visible */
    const char *v = sb_cstr(&shown);
    int before_w = tui_visible_width_n(v, cur);
    int skip = MAX(0, before_w - (avail - 1));
    char *vis = skip ? tui_slice_cols(v, skip, 1 << 20) : xstrdup(v);
    size_t vcur = cur;
    if (skip) {
        char *pre = tui_slice_cols(v, 0, skip);
        vcur = cur - strlen(pre);
        free(pre);
    }
    size_t cend = next_cp(vis, strlen(vis), vcur);
    char *b = xstrndup(vis, vcur), *c = xstrndup(vis + vcur, cend - vcur), *a = xstrdup(vis + cend);
    int rem = MAX(0, avail - tui_visible_width(b) - tui_visible_width(c));
    char *ac = tui_slice_cols(a, 0, rem);
    char *line = xasprintf("%s%s\x1b[7m%s\x1b[27m%s", prompt, b, c, ac);
    int padn = MAX(0, avail - tui_visible_width(b) - tui_visible_width(c) - tui_visible_width(ac));
    char *r = xasprintf("%s%*s", line, padn, "");
    free(line);
    free(b);
    free(c);
    free(a);
    free(ac);
    free(vis);
    sb_free(&shown);
    return r;
}

/* ---------- the selector ---------- */

enum { SUB_NONE, SUB_SELECT, SUB_TEXT, SUB_MULTI };

typedef struct settings_view {
    tui_component_t base;
    ui_t *u;
    int tab; /* 0..NTABS-1; NTABS = plugins */
    slist_t list;
    /* search */
    bool searching;
    int pre_search_tab;
    sinput_t search;
    int match_count;
    ui_tab_t *stabs; /* search-mode tab bar */
    int *stab_ids;
    size_t nstabs, stab_active;
    char **stab_labels;
    /* submenu */
    int sub;
    const sdef_t *sub_def;
    tui_select_list_t *sel;
    sopt_t *sub_opts;
    size_t sub_nopt;
    char *preview_restore; /* theme active before preview */
    char *sub_error;
    sinput_t input;
    json_t *multi; /* current multiselect array */
    size_t multi_cursor;
} settings_view_t;

static void apply_change(settings_view_t *v, const char *path);

static void clear_search_tabs(settings_view_t *v) {
    for (size_t i = 0; i < v->nstabs; i++) free(v->stab_labels[i]);
    free(v->stab_labels);
    free(v->stabs);
    free(v->stab_ids);
    v->stab_labels = NULL;
    v->stabs = NULL;
    v->stab_ids = NULL;
    v->nstabs = 0;
}

static void build_tab_items(settings_view_t *v, int tab) {
    const char *keep = NULL;
    char *keep_id = NULL;
    size_t prev = v->list.sel;
    if (list_selected(&v->list)) keep_id = xstrdup(list_selected(&v->list)->id);
    keep = keep_id;
    items_free(&v->list);
    const sdef_t **defs = xcalloc(g_ndefs + 1, sizeof *defs);
    size_t n = tab_defs(tab, defs);
    const char *last = NULL;
    for (size_t i = 0; i < n; i++) {
        if (!condition_ok(defs[i]->condition)) continue;
        if (defs[i]->group && !str_eq(defs[i]->group, last)) {
            char *hid = xasprintf("__heading:%s", defs[i]->group);
            push_heading(&v->list, defs[i]->group, hid);
            free(hid);
            last = defs[i]->group;
        }
        push_def(&v->list, defs[i]);
    }
    free(defs);
    list_reselect(&v->list, keep, prev);
    free(keep_id);
}

static void switch_tab(settings_view_t *v, int tab) {
    v->tab = tab;
    v->list.section_focus = false;
    v->list.flat = false;
    items_free(&v->list);
    if (tab < NTABS) {
        build_tab_items(v, tab);
        list_first_selectable(&v->list);
    }
}

static void tab_meta(int tab, const char **icon, const char **label) {
    if (tab >= (int)g_ntabs) {
        *icon = tui_sym("icon.package");
        *label = "Plugins";
    } else {
        *icon = tui_sym(g_tabs[tab].icon);
        *label = g_tabs[tab].label;
    }
}

/* setSearchQuery: flat result list with a heading per matching tab, tabs ordered by best score */
static void set_search_query(settings_view_t *v) {
    const char *q = sb_cstr(&v->search.text);
    char *keep_id = list_selected(&v->list) ? xstrdup(list_selected(&v->list)->id) : NULL;
    size_t prev = v->list.sel;
    items_free(&v->list);
    tui_fuzzy_query_t *fq = tui_fuzzy_query_new(q);
    typedef struct {
        int tab;
        size_t n;
        double best;
        const sdef_t **m;
    } tres_t;
    tres_t res[NTABS];
    int counts[NTABS] = {0};
    size_t nres = 0;
    v->match_count = 0;
    for (int t = 0; t < (int)g_ntabs; t++) {
        const sdef_t **defs = xcalloc(g_ndefs + 1, sizeof *defs);
        size_t n = tab_defs(t, defs);
        const sdef_t **m = xcalloc(n + 1, sizeof *m);
        double *sc = xcalloc(n + 1, sizeof *sc);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (!condition_ok(defs[i]->condition)) continue;
            char *val = display_value(defs[i]);
            sb_t text = SB_INIT;
            sb_appendf(&text, "%s %s %s", defs[i]->label, defs[i]->path, val);
            if (defs[i]->description && *defs[i]->description) sb_appendf(&text, " %s", defs[i]->description);
            if (defs[i]->warning) sb_appendf(&text, " %s", defs[i]->warning);
            if (!strcmp(defs[i]->type, "boolean")) sb_append(&text, " true false");
            else if (!strcmp(defs[i]->type, "enum"))
                for (size_t e = 0; e < json_len(defs[i]->values); e++) sb_appendf(&text, " %s", json_sv(json_at(defs[i]->values, e)));
            char *flat = sb_detach(&text);
            for (char *p = flat; *p; p++)
                if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            double s = 0;
            if (tui_fuzzy_query_match(fq, flat, &s)) {
                /* stable insert by score */
                size_t j = k;
                while (j > 0 && sc[j - 1] > s) {
                    m[j] = m[j - 1];
                    sc[j] = sc[j - 1];
                    j--;
                }
                m[j] = defs[i];
                sc[j] = s;
                k++;
            }
            free(flat);
            free(val);
        }
        counts[t] = (int)k;
        if (k) {
            res[nres++] = (tres_t){t, k, sc[0], m};
            v->match_count += (int)k;
        } else free(m);
        free(sc);
        free(defs);
    }
    tui_fuzzy_query_free(fq);
    for (size_t i = 1; i < nres; i++) { /* stable sort by best score then tab order */
        tres_t x = res[i];
        size_t j = i;
        while (j > 0 && res[j - 1].best > x.best) {
            res[j] = res[j - 1];
            j--;
        }
        res[j] = x;
    }
    for (size_t i = 0; i < nres; i++) {
        const char *icon, *label;
        tab_meta(res[i].tab, &icon, &label);
        char *h = xasprintf("%s %s", icon, label), *hid = xasprintf("__tab:%s", g_tabs[res[i].tab].id);
        push_heading(&v->list, h, hid);
        free(h);
        free(hid);
        for (size_t k = 0; k < res[i].n; k++) push_def(&v->list, res[i].m[k]);
        free(res[i].m);
    }
    if (keep_id || prev) list_reselect(&v->list, keep_id, prev);
    else list_first_selectable(&v->list);
    if (list_selected(&v->list) == NULL) list_first_selectable(&v->list);
    free(keep_id);
    /* tab bar: matching tabs (counts) first, the rest muted */
    clear_search_tabs(v);
    v->stabs = xcalloc(NTABS + 2, sizeof *v->stabs);
    v->stab_ids = xcalloc(NTABS + 2, sizeof *v->stab_ids);
    v->stab_labels = xcalloc(2 * (NTABS + 2), sizeof *v->stab_labels);
    size_t nl = 0;
    for (size_t i = 0; i < nres; i++) {
        const char *icon, *label;
        tab_meta(res[i].tab, &icon, &label);
        char *l = xasprintf("%s %s (%d)", icon, label, counts[res[i].tab]), *s = xasprintf("%s %d", icon, counts[res[i].tab]);
        v->stab_labels[nl++] = l;
        v->stab_labels[nl++] = s;
        v->stabs[v->nstabs] = (ui_tab_t){l, s, false};
        v->stab_ids[v->nstabs++] = res[i].tab;
    }
    for (int t = 0; t <= (int)g_ntabs; t++) {
        bool matched = false;
        for (size_t i = 0; i < nres; i++)
            if (res[i].tab == t) matched = true;
        if (matched) continue;
        const char *icon, *label;
        tab_meta(t, &icon, &label);
        char *l = xasprintf("%s %s", icon, label), *s = xstrdup(icon);
        v->stab_labels[nl++] = l;
        v->stab_labels[nl++] = s;
        v->stabs[v->nstabs] = (ui_tab_t){l, s, true};
        v->stab_ids[v->nstabs++] = t;
    }
    /* sync the active tab to the selection */
    const sitem_t *it = list_selected(&v->list);
    v->stab_active = 0;
    if (it)
        for (size_t i = 0; i < v->nstabs; i++)
            if (v->stab_ids[i] == it->def->tab) v->stab_active = i;
}

static void end_search(settings_view_t *v, bool jump) {
    if (!v->searching) return;
    const sitem_t *it = jump ? list_selected(&v->list) : NULL;
    char *path = it ? xstrdup(it->def->path) : NULL;
    int target = it ? it->def->tab : v->pre_search_tab;
    v->searching = false;
    input_set(&v->search, "");
    v->match_count = 0;
    clear_search_tabs(v);
    switch_tab(v, target);
    if (path) {
        for (size_t i = 0; i < v->list.items.n; i++)
            if (!v->list.items.v[i].heading && !strcmp(v->list.items.v[i].id, path)) v->list.sel = i;
        free(path);
    }
}

static void start_search(settings_view_t *v, const char *initial) {
    v->pre_search_tab = v->tab;
    v->searching = true;
    v->list.flat = true;
    v->list.section_focus = false;
    input_set(&v->search, initial);
    items_free(&v->list);
    v->list.sel = 0;
    set_search_query(v);
}

static void refresh_items(settings_view_t *v) {
    if (v->searching) set_search_query(v);
    else if (v->tab < NTABS) build_tab_items(v, v->tab);
}

/* ---------- persistence (setSettingValue) ---------- */

static bool set_value(const sdef_t *d, const char *value, char **err) {
    const json_t *cur = get_value(d);
    json_t *val = NULL;
    if ((!strcmp(d->path, "compaction.thresholdPercent") || !strcmp(d->path, "compaction.thresholdTokens")) &&
        !strcmp(value, "default")) {
        val = json_num(-1);
    } else if (!strcmp(d->schema, "array") || !strcmp(d->schema, "record")) {
        bool rec = !strcmp(d->schema, "record");
        val = json_parse_str(*value || !rec ? value : "{}");
        if (!val || (rec ? !json_is_obj(val) : !json_is_arr(val))) {
            json_free(val);
            *err = xasprintf("Invalid %s JSON for %s", rec ? "record" : "array", d->path);
            return false;
        }
    } else if (json_is_num(cur)) {
        char *end = NULL;
        double x = strtod(value, &end);
        val = json_num(end && *end == 0 && *value ? x : NAN);
    } else if (json_is_bool(cur)) {
        val = json_bool(!strcmp(value, "true"));
    } else {
        val = json_str(value);
    }
    settings_set_global(d->path, val);
    return true;
}

/* ---------- submenus ---------- */

static void close_sub(settings_view_t *v) {
    if (v->sel) tui_component_destroy(&v->sel->base);
    v->sel = NULL;
    for (size_t i = 0; i < v->sub_nopt; i++) {
        free(v->sub_opts[i].value);
        free(v->sub_opts[i].label);
        free(v->sub_opts[i].description);
    }
    free(v->sub_opts);
    v->sub_opts = NULL;
    v->sub_nopt = 0;
    free(v->preview_restore);
    v->preview_restore = NULL;
    free(v->sub_error);
    v->sub_error = NULL;
    json_free(v->multi);
    v->multi = NULL;
    v->sub = SUB_NONE;
    v->sub_def = NULL;
}

static bool is_theme_setting(const sdef_t *d) { return !strcmp(d->path, "theme.dark") || !strcmp(d->path, "theme.light"); }

static void sub_opts_for(settings_view_t *v, const sdef_t *d) {
    VEC(sopt_t) o = {0};
    if (!strcmp(d->path, "defaultThinkingLevel")) {
        const char *levels[] = {"auto", "off", "minimal", "low", "medium", "high", "xhigh", "max"};
        app_t *a = v->u->app;
        for (size_t i = 0; i < ARRAY_LEN(levels); i++) {
            ai_effort_t e;
            if (i >= 2 && (!a->model || !a->model->reasoning || effort_parse(levels[i], &e) != 0 ||
                           !model_supports_effort(a->model, e)))
                continue;
            if (i == 1 && a->model && !a->model->reasoning) continue;
            sopt_t s = {xstrdup(levels[i]), xstrdup(levels[i]), NULL};
            for (size_t k = 0; k < d->nopt; k++)
                if (!strcmp(d->opts[k].value, levels[i])) {
                    free(s.label);
                    s.label = xstrdup(d->opts[k].label);
                    s.description = d->opts[k].description ? xstrdup(d->opts[k].description) : NULL;
                }
            vec_push(&o, s);
        }
    } else if (is_theme_setting(d)) {
        strvec_t names = {0};
        ui_theme_list(&names);
        for (size_t i = 0; i < names.n; i++) {
            sopt_t s = {xstrdup(names.v[i]), xstrdup(names.v[i]), NULL};
            vec_push(&o, s);
        }
        strvec_free(&names);
    } else if (!strcmp(d->path, "composer.shape")) {
        for (size_t i = 0; i < ARRAY_LEN(UI_COMPOSER_SHAPES); i++) {
            sopt_t s = {xstrdup(UI_COMPOSER_SHAPES[i].value), xstrdup(UI_COMPOSER_SHAPES[i].label),
                        xstrdup(UI_COMPOSER_SHAPES[i].description)};
            vec_push(&o, s);
        }
    } else {
        for (size_t k = 0; k < d->nopt; k++) {
            sopt_t s = {xstrdup(d->opts[k].value), xstrdup(d->opts[k].label),
                        d->opts[k].description ? xstrdup(d->opts[k].description) : NULL};
            vec_push(&o, s);
        }
    }
    v->sub_opts = o.v;
    v->sub_nopt = o.n;
}

static void sel_preview(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    settings_view_t *v = ud;
    if (!v->sub_def) return;
    const char *p = v->sub_def->path;
    if (is_theme_setting(v->sub_def)) {
        char *err = NULL;
        if (!ui_theme_preview(it->value, &err)) {
            free(v->sub_error);
            v->sub_error = err;
        } else {
            free(v->sub_error);
            v->sub_error = NULL;
        }
        tui_request_full_render(v->u->tui);
    } else if (!strcmp(p, "statusLine.preset") || !strcmp(p, "statusLine.separator") ||
               !strcmp(p, "statusLine.contextLine")) {
        settings_set_runtime(p, json_str(it->value));
    }
}

static void sel_pick(tui_select_list_t *l, const tui_select_item_t *it, void *ud) {
    settings_view_t *v = ud;
    const sdef_t *d = v->sub_def;
    char *val = xstrdup(it->value), *err = NULL;
    if (!strcmp(d->path, "statusLine.preset") || !strcmp(d->path, "statusLine.separator") ||
        !strcmp(d->path, "statusLine.contextLine"))
        settings_unset_runtime(d->path);
    if (!set_value(d, val, &err)) {
        free(v->sub_error);
        v->sub_error = err;
        free(val);
        return;
    }
    free(v->preview_restore);
    v->preview_restore = NULL;
    char *path = xstrdup(d->path);
    close_sub(v);
    apply_change(v, path);
    refresh_items(v);
    free(path);
    free(val);
}

static void sel_cancel(tui_select_list_t *l, void *ud) {
    settings_view_t *v = ud;
    const sdef_t *d = v->sub_def;
    if (d && is_theme_setting(d) && v->preview_restore) ui_theme_preview(v->preview_restore, NULL);
    if (d && (!strcmp(d->path, "statusLine.preset") || !strcmp(d->path, "statusLine.separator") ||
              !strcmp(d->path, "statusLine.contextLine")))
        settings_unset_runtime(d->path);
    close_sub(v);
    tui_request_full_render(v->u->tui);
}

static void multi_rebuild(settings_view_t *v) {
    const sdef_t *d = v->sub_def;
    if (v->sel) tui_component_destroy(&v->sel->base);
    v->sel = tui_select_list_new((int)MIN(d->nopt, 12));
    for (size_t i = 0; i < d->nopt; i++) {
        int pos = -1;
        for (size_t k = 0; k < json_len(v->multi); k++)
            if (str_eq(json_sv(json_at(v->multi, k)), d->opts[i].value)) pos = (int)k;
        char *mark;
        if (pos < 0) mark = S(TUI_TC_DIM, d->ordered ? " \xc2\xb7 " : " \xe2\x97\x8b ");
        else if (d->ordered) {
            char *t = xasprintf("%2d.", pos + 1);
            mark = S(TUI_TC_ACCENT, t);
            free(t);
        } else mark = S(TUI_TC_ACCENT, " \xe2\x97\x8f ");
        char *lab = xasprintf("%s %s", mark, d->opts[i].label);
        tui_select_list_add(v->sel, d->opts[i].value, lab, d->opts[i].description);
        free(lab);
        free(mark);
    }
    tui_select_list_set_selected(v->sel, v->multi_cursor);
    v->sel->ud = v;
}

static void multi_apply(settings_view_t *v, json_t *next) {
    json_free(v->multi);
    v->multi = next;
    settings_set_global(v->sub_def->path, json_clone(next));
    multi_rebuild(v);
}

static void open_sub(settings_view_t *v, const sitem_t *it) {
    const sdef_t *d = it->def;
    v->sub_def = d;
    if (!strcmp(d->type, "submenu")) {
        v->sub = SUB_SELECT;
        sub_opts_for(v, d);
        v->sel = tui_select_list_new((int)MIN(MAX(v->sub_nopt, 1), 10));
        v->sel->layout.search = TUI_SELECT_SEARCH_OVERFLOW;
        for (size_t i = 0; i < v->sub_nopt; i++)
            tui_select_list_add(v->sel, v->sub_opts[i].value, v->sub_opts[i].label, v->sub_opts[i].description);
        tui_select_list_select_value(v->sel, it->value);
        v->sel->on_select = sel_pick;
        v->sel->on_cancel = sel_cancel;
        v->sel->on_change = sel_preview;
        v->sel->ud = v;
        if (is_theme_setting(d)) v->preview_restore = xstrdup(ui_theme_current_name());
    } else if (!strcmp(d->type, "multiselect")) {
        v->sub = SUB_MULTI;
        v->multi = json_arr();
        const json_t *cur = get_value(d);
        json_t *e;
        json_arr_foreach(json_is_arr(cur) ? (json_t *)cur : NULL, i, e) {
            if (!json_is_str(e)) continue;
            for (size_t k = 0; k < d->nopt; k++)
                if (!strcmp(d->opts[k].value, json_sv(e))) json_push_str(v->multi, json_sv(e));
        }
        v->multi_cursor = 0;
        multi_rebuild(v);
    } else { /* text / providerLimits */
        v->sub = SUB_TEXT;
        char *cur = value_str(get_value(d));
        input_set(&v->input, cur);
        v->input.mask = d->secret;
        free(cur);
    }
}

/* FormField: label, description, preview, control, error, hint */
static void render_field(settings_view_t *v, int width, strvec_t *out) {
    const sdef_t *d = v->sub_def;
    char *lab = S(TUI_TC_ACCENT, d->label), *lb = tui_bold(lab);
    ui_push_text(out, lb, width);
    free(lab);
    free(lb);
    if (d->description && *d->description) {
        strvec_push(out, "");
        char *ds = S(TUI_TC_MUTED, d->description);
        ui_push_text(out, ds, width);
        free(ds);
    }
    if (v->sub == SUB_SELECT && is_theme_setting(d)) {
        strvec_push(out, "");
        char *pl = S(TUI_TC_MUTED, "Preview:");
        strvec_push_own(out, pl);
        int cols = tui_term(v->u->tui)->cols;
        char *pv = ui_settings_status_preview(v->u, cols > 0 ? cols : width);
        /* StyledText wrap: like omp's wrapTextWithAnsi the styles stay open at a wrapped row's end (the
         * status bar background runs on to the frame) */
        strvec_t w = {0};
        ui_push_text(&w, pv, width);
        for (size_t i = 0; i < w.n; i++) {
            if (i + 1 < w.n && str_ends(w.v[i], TUI_RESET)) w.v[i][strlen(w.v[i]) - strlen(TUI_RESET)] = 0;
            strvec_push(out, w.v[i]);
        }
        strvec_free(&w);
        free(pv);
    }
    strvec_push(out, "");
    if (v->sub == SUB_TEXT) {
        strvec_push_own(out, input_render(&v->input, "> ", width));
    } else {
        tui_component_render(&v->sel->base, width, out);
    }
    strvec_push(out, "");
    if (v->sub_error) {
        char *e = S(TUI_TC_ERROR, v->sub_error);
        ui_push_text(out, e, width);
        free(e);
    }
    char *hint;
    char *conf = ui_editor_key(v->sub == SUB_TEXT ? "tui.input.submit" : "tui.select.confirm");
    char *canc = ui_editor_key("tui.select.cancel");
    if (v->sub == SUB_TEXT) hint = xasprintf("  %s to save \xc2\xb7 %s to cancel \xc2\xb7 Clear field to unset", conf, canc);
    else if (v->sub == SUB_MULTI) {
        if (d->ordered) {
            char *l = ui_key_hint("left"), *r = ui_key_hint("right");
            hint = xasprintf("  Click to toggle \xc2\xb7 drag selected items to reorder \xc2\xb7 %s/%s move \xc2\xb7 1-9 place \xc2\xb7 %s to go back", l, r, canc);
            free(l);
            free(r);
        } else {
            char *sp = ui_key_hint("space");
            hint = xasprintf("  Click/%s/%s to toggle \xc2\xb7 %s to go back", conf, sp, canc);
            free(sp);
        }
    } else hint = xasprintf("  %s to select \xc2\xb7 %s to go back", conf, canc);
    char *hs = S(TUI_TC_DIM, hint);
    ui_push_text(out, hs, width);
    free(hs);
    free(hint);
    free(conf);
    free(canc);
    if (v->sub == SUB_SELECT && !strcmp(d->path, "composer.shape")) {
        const tui_select_item_t *it = tui_select_list_selected(v->sel);
        strvec_push(out, "");
        ui_composer_preview(v->u, it ? it->value : "band", width, out);
    }
}

static bool sub_key(settings_view_t *v, const tui_key_t *k) {
    if (v->sub == SUB_TEXT) {
        if (tui_kb(k, "tui.select.cancel") && !tui_key_matches(k, "ctrl+c")) {
            close_sub(v);
            return true;
        }
        if (tui_key_matches(k, "enter") || tui_key_matches(k, "return")) {
            const sdef_t *d = v->sub_def;
            const char *val = sb_cstr(&v->input.text);
            char *err = NULL;
            if (!*val) settings_unset_global(d->path);
            else if (!set_value(d, val, &err)) {
                free(v->sub_error);
                v->sub_error = err;
                return true;
            }
            char *path = xstrdup(d->path);
            close_sub(v);
            apply_change(v, path);
            refresh_items(v);
            free(path);
            return true;
        }
        if (tui_kb(k, "tui.select.cancel")) {
            close_sub(v);
            return true;
        }
        input_key(&v->input, k);
        return true;
    }
    if (v->sub == SUB_MULTI) {
        const sdef_t *d = v->sub_def;
        const char *cur = v->multi_cursor < d->nopt ? d->opts[v->multi_cursor].value : NULL;
        bool toggle = cur && (tui_key_matches(k, "space") || tui_kb(k, "tui.select.confirm"));
        if (tui_kb(k, "tui.select.cancel")) {
            char *path = xstrdup(d->path);
            close_sub(v);
            apply_change(v, path);
            refresh_items(v);
            free(path);
            return true;
        }
        if (toggle) {
            json_t *next = json_arr();
            bool had = false;
            for (size_t i = 0; i < json_len(v->multi); i++) {
                const char *x = json_sv(json_at(v->multi, i));
                if (str_eq(x, cur)) had = true;
                else json_push_str(next, x);
            }
            if (!had) json_push_str(next, cur);
            multi_apply(v, next);
            return true;
        }
        if (d->ordered && cur && (tui_key_matches(k, "left") || tui_key_matches(k, "right"))) {
            long from = -1;
            for (size_t i = 0; i < json_len(v->multi); i++)
                if (str_eq(json_sv(json_at(v->multi, i)), cur)) from = (long)i;
            long to = from + (tui_key_matches(k, "left") ? -1 : 1);
            if (from >= 0 && to >= 0 && to < (long)json_len(v->multi)) {
                json_t *next = json_clone(v->multi);
                json_t *a = json_at(next, (size_t)from), *b = json_at(next, (size_t)to);
                char *as = xstrdup(json_sv(a));
                json_t *na = json_str(json_sv(b)), *nb = json_str(as);
                free(as);
                json_t *rebuilt = json_arr();
                for (size_t i = 0; i < json_len(next); i++)
                    json_push(rebuilt, (long)i == from ? na : (long)i == to ? nb : json_clone(json_at(next, i)));
                json_free(next);
                multi_apply(v, rebuilt);
            }
            return true;
        }
        if (d->ordered && cur && k->kind == TUI_KEY_CHAR && k->codepoint >= '1' && k->codepoint <= '9' && !k->mods) {
            json_t *next = json_arr();
            for (size_t i = 0; i < json_len(v->multi); i++)
                if (!str_eq(json_sv(json_at(v->multi, i)), cur)) json_push_str(next, json_sv(json_at(v->multi, i)));
            size_t pos = MIN((size_t)(k->codepoint - '1'), json_len(next));
            json_t *ins = json_arr();
            for (size_t i = 0; i <= json_len(next); i++) {
                if (i == pos) json_push_str(ins, cur);
                if (i < json_len(next)) json_push_str(ins, json_sv(json_at(next, i)));
            }
            json_free(next);
            multi_apply(v, ins);
            return true;
        }
        tui_component_handle_key(&v->sel->base, k);
        const tui_select_item_t *it = tui_select_list_selected(v->sel);
        for (size_t i = 0; it && i < d->nopt; i++)
            if (!strcmp(d->opts[i].value, it->value)) v->multi_cursor = i;
        return true;
    }
    tui_component_handle_key(&v->sel->base, k);
    return true;
}

/* ---------- selector render / input ---------- */

static char *footer_hint(settings_view_t *v) {
    char *conf = ui_editor_key("tui.select.confirm"), *canc = ui_editor_key("tui.select.cancel");
    char *tab = ui_key_hint("tab"), *l = ui_key_hint("left"), *r = ui_key_hint("right");
    char *sw = xasprintf("%s/%s to switch tabs", l, r), *out;
    if (v->searching) out = xasprintf("%s to change \xc2\xb7 %s to jump tabs \xc2\xb7 %s to exit search", conf, tab, canc);
    else if (v->tab >= NTABS) out = xasprintf("%s to switch tabs \xc2\xb7 %s to close", tab, canc);
    else if (v->list.section_focus) {
        char *u = ui_key_hint("up"), *d = ui_key_hint("down");
        out = xasprintf("%s/%s to jump sections \xc2\xb7 %s/%s to settings \xc2\xb7 %s \xc2\xb7 %s to close", u, d, tab, conf, sw, canc);
        free(u);
        free(d);
    } else {
        section_t *s = xcalloc(v->list.items.n + 1, sizeof *s);
        bool jump = list_sections(&v->list, s) >= 2;
        free(s);
        char *nav = jump ? xasprintf("%s to jump sections \xc2\xb7 %s", tab, sw) : xasprintf("%s to switch tabs", tab);
        char *sp = ui_key_hint("space");
        out = xasprintf("%s/%s to change \xc2\xb7 %s \xc2\xb7 Type to search \xc2\xb7 %s to close", conf, sp, nav, canc);
        free(sp);
        free(nav);
    }
    free(conf);
    free(canc);
    free(tab);
    free(l);
    free(r);
    free(sw);
    return out;
}

static char *search_banner(settings_view_t *v, int width) {
    char *count = v->match_count == 1 ? xstrdup("1 match") : xasprintf("%d matches", v->match_count);
    int rw = tui_visible_width(count) + 1;
    char *icon = S(TUI_TC_ACCENT, tui_sym("icon.search"));
    char *prefix = xasprintf(" %s ", icon);
    int iw = MAX(4, width - tui_visible_width(prefix) - rw - 1);
    char *in = input_render(&v->search, "", iw), *inb = tui_bold(in);
    char *cs = S(v->match_count > 0 ? TUI_TC_DIM : TUI_TC_WARNING, count);
    char *line = xasprintf("%s%s %s ", prefix, inb, cs), *r = tui_truncate_to_width(line, width, NULL);
    free(count);
    free(icon);
    free(prefix);
    free(in);
    free(inb);
    free(cs);
    free(line);
    return r;
}

static void settings_render(tui_component_t *c, int width, strvec_t *out) {
    settings_view_t *v = (settings_view_t *)c;
    int height = MAX(14, tui_term(v->u->tui)->rows > 0 ? tui_term(v->u->tui)->rows : 40);
    int inner = MAX(1, width - 4);
    strvec_t tabs = {0};
    if (v->searching) {
        ui_tab_bar_render("", v->stabs, v->nstabs, v->stab_active, false, inner, &tabs);
    } else {
        ui_tab_t t[NTABS + 1];
        char *labels[NTABS + 1];
        size_t nt = 0;
        for (int i = 0; i <= (int)g_ntabs; i++) {
            const char *icon, *label;
            tab_meta(i, &icon, &label);
            labels[nt] = xasprintf("%s %s", icon, label);
            t[nt] = (ui_tab_t){labels[nt], icon, false};
            nt++;
        }
        ui_tab_bar_render("", t, nt, (size_t)MIN(v->tab, (int)g_ntabs), false, inner, &tabs);
        for (size_t i = 0; i < nt; i++) free(labels[i]);
    }
    bool preview = !v->searching && v->tab == 0;
    int fixed = 1 + (int)tabs.n + 1 + (v->searching ? 1 : 0) + 1 + 1 + 1;
    int content_rows = MAX(7, height - fixed - (preview ? 3 : 0));
    strvec_t content = {0};
    if (v->tab < NTABS || v->searching) {
        v->list.max_visible = content_rows - 4;
        if (v->sub) {
            render_field(v, inner, &content);
        } else {
            render_list(&v->list, inner, v->searching ? "No matching settings" : NULL, &content);
        }
    } else {
        /* PluginSettingsComponent with no plugins: a "Plugins" panel and install hints */
        strvec_t body = {0};
        strvec_push(&body, "");
        strvec_push_own(&body, S(TUI_TC_MUTED, "No plugins installed"));
        strvec_push(&body, "");
        strvec_push_own(&body, S(TUI_TC_DIM, "Install npm plugins:        " UI_APP_NAME " plugin install <package>"));
        strvec_push_own(&body, S(TUI_TC_DIM, "Install marketplace plugins: " UI_APP_NAME " plugin install <name>@<marketplace>"));
        strvec_push(&body, "");
        strvec_push_own(&content, ui_box_top(inner, "Plugins"));
        for (size_t i = 0; i < body.n; i++) strvec_push_own(&content, ui_box_row(body.v[i], inner));
        strvec_push_own(&content, ui_box_bottom(inner));
        strvec_free(&body);
    }
    strvec_push_own(out, ui_box_top(width, "Settings"));
    for (size_t i = 0; i < tabs.n; i++) strvec_push_own(out, ui_box_row(tabs.v[i], width));
    strvec_push_own(out, ui_box_divider(width));
    if (v->searching) {
        char *b = search_banner(v, inner);
        strvec_push_own(out, ui_box_row(b, width));
        free(b);
    }
    for (int i = 0; i < content_rows; i++) strvec_push_own(out, ui_box_row(i < (int)content.n ? content.v[i] : "", width));
    if (preview) {
        strvec_push_own(out, ui_box_row("", width));
        char *p = S(TUI_TC_MUTED, "Preview:");
        strvec_push_own(out, ui_box_row(p, width));
        free(p);
        int cols = tui_term(v->u->tui)->cols;
        char *pv = ui_settings_status_preview(v->u, cols > 0 ? cols : width);
        char *first = xstrdup(pv);
        char *nl = strchr(first, '\n');
        if (nl) *nl = 0;
        strvec_push_own(out, ui_box_row(first, width));
        free(first);
        free(pv);
    }
    strvec_push_own(out, ui_box_divider(width));
    char *f = footer_hint(v), *fs = S(TUI_TC_DIM, f);
    strvec_push_own(out, ui_box_row(fs, width));
    strvec_push_own(out, ui_box_bottom(width));
    free(f);
    free(fs);
    strvec_free(&tabs);
    strvec_free(&content);
}

static void settings_close(settings_view_t *v) {
    ui_t *u = v->u;
    tui_overlay_hide(u->tui, &v->base);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049l");
    tui_set_focus(u->tui, &u->editor->base);
    ui_chrome_setup(u);
    tui_request_full_render(u->tui);
    tui_component_destroy(&v->base);
}

/* SettingsList.activateItem: submenus open, enum/boolean values cycle */
static void activate(settings_view_t *v) {
    const sitem_t *it = list_selected(&v->list);
    if (!it) return;
    const sdef_t *d = it->def;
    if (!strcmp(d->type, "boolean") || !strcmp(d->type, "enum")) {
        size_t n = !strcmp(d->type, "boolean") ? 2 : json_len(d->values);
        if (!n) return;
        size_t idx = n; /* -1 */
        for (size_t i = 0; i < n; i++) {
            const char *x = !strcmp(d->type, "boolean") ? (i == 0 ? "true" : "false") : json_sv(json_at(d->values, i));
            if (str_eq(x, it->value)) idx = i;
        }
        size_t next = idx == n ? 0 : (idx + 1) % n;
        if (!strcmp(d->type, "boolean")) settings_set_global(d->path, json_bool(next == 0));
        else settings_set_global(d->path, json_str(json_sv(json_at(d->values, next))));
        apply_change(v, d->path);
        refresh_items(v);
        return;
    }
    open_sub(v, it);
}

static bool printable(const tui_key_t *k, const char **txt) {
    *txt = tui_key_text(k);
    return k->kind == TUI_KEY_CHAR && *txt && **txt && !(k->mods & (TUI_MOD_CTRL | TUI_MOD_ALT | TUI_MOD_SUPER));
}

static void tab_cycle(settings_view_t *v, int dir) {
    if (v->searching) {
        if (!v->nstabs) return;
        size_t i = v->stab_active;
        for (size_t step = 0; step < v->nstabs; step++) {
            i = (i + v->nstabs + (size_t)(dir < 0 ? v->nstabs - 1 : 1)) % v->nstabs;
            if (!v->stabs[i].muted) break;
        }
        if (v->stabs[i].muted) return;
        v->stab_active = i;
        /* jump to the tab's first match */
        int tab = v->stab_ids[i];
        for (size_t k = 0; k < v->list.items.n; k++)
            if (!v->list.items.v[k].heading && v->list.items.v[k].def->tab == tab) {
                v->list.sel = k;
                break;
            }
        return;
    }
    int n = (int)g_ntabs + 1;
    switch_tab(v, (v->tab + n + dir) % n);
}

static bool settings_key(tui_component_t *c, const tui_key_t *k) {
    settings_view_t *v = (settings_view_t *)c;
    if (v->sub) {
        sub_key(v, k);
        tui_component_request_render(c);
        return true;
    }
    const char *txt;
    slist_t *l = &v->list;
    if (v->searching) {
        if (tui_kb(k, "tui.select.cancel")) end_search(v, true);
        else if (tui_key_matches(k, "tab")) tab_cycle(v, 1);
        else if (tui_key_matches(k, "shift+tab")) tab_cycle(v, -1);
        else if (tui_kb(k, "tui.select.up")) list_move(l, -1);
        else if (tui_kb(k, "tui.select.down")) list_move(l, 1);
        else if (tui_kb(k, "tui.select.pageUp")) list_jump_section(l, -1);
        else if (tui_kb(k, "tui.select.pageDown")) list_jump_section(l, 1);
        else if (tui_kb(k, "tui.select.confirm")) activate(v);
        else {
            char *before = xstrdup(sb_cstr(&v->search.text));
            input_key(&v->search, k);
            if (strcmp(before, sb_cstr(&v->search.text))) {
                if (!v->search.text.len) end_search(v, false);
                else set_search_query(v);
            }
            free(before);
        }
        if (v->searching) {
            const sitem_t *it = list_selected(l);
            for (size_t i = 0; it && i < v->nstabs; i++)
                if (v->stab_ids[i] == it->def->tab) v->stab_active = i;
        }
        tui_component_request_render(c);
        return true;
    }
    section_t *secs = xcalloc(l->items.n + 1, sizeof *secs);
    size_t ns = list_sections(l, secs);
    free(secs);
    if (tui_key_matches(k, "tab") || tui_key_matches(k, "shift+tab")) {
        if (v->tab < NTABS && ns >= 2) l->section_focus = !l->section_focus;
        else tab_cycle(v, tui_key_matches(k, "tab") ? 1 : -1);
    } else if (tui_key_matches(k, "left") || tui_key_matches(k, "right")) {
        tab_cycle(v, tui_key_matches(k, "right") ? 1 : -1);
    } else if (v->tab < NTABS && printable(k, &txt) && !blank_str(txt)) {
        start_search(v, txt);
    } else if (tui_kb(k, "tui.select.cancel")) {
        if (l->section_focus) l->section_focus = false;
        else {
            settings_close(v);
            return true;
        }
    } else if (v->tab >= NTABS || !l->items.n) {
    } else if (tui_kb(k, "tui.select.up")) {
        if (l->section_focus) list_jump_section(l, -1);
        else list_move(l, -1);
    } else if (tui_kb(k, "tui.select.down")) {
        if (l->section_focus) list_jump_section(l, 1);
        else list_move(l, 1);
    } else if (tui_kb(k, "tui.select.pageDown")) list_jump_section(l, 1);
    else if (tui_kb(k, "tui.select.pageUp")) list_jump_section(l, -1);
    else if (tui_kb(k, "tui.select.confirm") || tui_key_matches(k, "space")) {
        if (l->section_focus) l->section_focus = false;
        else activate(v);
    }
    tui_component_request_render(c);
    return true;
}

static void settings_destroy(tui_component_t *c) {
    settings_view_t *v = (settings_view_t *)c;
    close_sub(v);
    items_free(&v->list);
    vec_free(&v->list.items);
    clear_search_tabs(v);
    sb_free(&v->search.text);
    sb_free(&v->input.text);
    free(v);
}

static const tui_component_vt SETTINGS_VT = {"settings-selector", settings_render, settings_key, NULL, settings_destroy};

/* live effects of a changed setting (omp's settings subscribers) */
static void apply_change(settings_view_t *v, const char *path) {
    ui_t *u = v->u;
    if (!strcmp(path, "theme.dark") || !strcmp(path, "theme.light") || !strcmp(path, "symbolPreset") ||
        !strcmp(path, "colorBlindMode"))
        ui_apply_appearance();
    if (!strcmp(path, "defaultThinkingLevel")) {
        app_set_thinking_level(u->app, get_str(path, "high"));
    }
    if (!strcmp(path, "hideThinkingBlock")) u->hide_thinking = get_bool(path, false);
    if (!strcmp(path, "autolearn.enabled") || !strcmp(path, "memory.backend")) autolearn_reconcile(u->app);
    ui_chrome_setup(u);
    tui_request_full_render(u->tui);
}

void ui_overlay_settings(ui_t *u) {
    load_defs();
    settings_view_t *v = xcalloc(1, sizeof *v);
    tui_component_init(&v->base, &SETTINGS_VT);
    v->u = u;
    v->list.max_visible = 10;
    switch_tab(v, 0);
    tui_term_puts(tui_term(u->tui), "\x1b[?1049h");
    tui_overlay_opts_t o = {TUI_ANCHOR_TOP, 0, 0, 0, 0};
    tui_overlay_show(u->tui, &v->base, &o);
    tui_set_focus(u->tui, &v->base);
    tui_request_full_render(u->tui);
}

/* ---------- test hooks ---------- */

size_t ui_settings_def_count(void) {
    load_defs();
    return g_ndefs;
}
