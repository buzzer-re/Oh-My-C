/* modelbrowser.c - searchable model list with metric columns and a detail pane (port of omp
 * overlays/model-browser.ts ModelBrowser, as used by the setup wizard and the model picker). */
#include "ui/modelbrowser.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "config/settings.h"
#include "tui/fuzzy.h"
#include "ui/overlays.h"

#include "tui/symbols.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>

#define LIST_ROW_START 2
#define DETAIL_ROWS 3
#define PERF_TPS_MIN_WIDTH 76
#define PERF_FULL_MIN_WIDTH 96
#define MIN_NAME_WIDTH 16

/* upstream utils formatNumber */
char *ui_format_number(double n) {
    char buf[32];
    if (n < 1000) {
        snprintf(buf, sizeof buf, "%.0f", n);
        return xstrdup(buf);
    }
    const char *suf;
    double v;
    bool trim;
    if (n < 1e4) v = n / 1e3, suf = "K", trim = true;
    else if (n < 1e6) v = round(n / 1e3), suf = "K", trim = false;
    else if (n < 1e7) v = n / 1e6, suf = "M", trim = true;
    else if (n < 1e9) v = round(n / 1e6), suf = "M", trim = false;
    else if (n < 1e10) v = n / 1e9, suf = "B", trim = true;
    else v = round(n / 1e9), suf = "B", trim = false;
    if (trim) {
        snprintf(buf, sizeof buf, "%.1f", v);
        size_t l = strlen(buf);
        if (l > 2 && !strcmp(buf + l - 2, ".0")) buf[l - 2] = 0;
    } else {
        snprintf(buf, sizeof buf, "%.0f", v);
    }
    return xasprintf("%s%s", buf, suf);
}

static char *lower_num(double n) {
    char *s = ui_format_number(n);
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

static bool is_free(const ai_model_t *m) { return m->cost_input == 0 && m->cost_output == 0; }

static char *cost_leg(double n) {
    if (!isfinite(n) || n < 0) return xstrdup("?");
    char buf[64];
    if (n > 0 && n < 0.01) snprintf(buf, sizeof buf, "%.10g", n);
    else if (n >= 100) snprintf(buf, sizeof buf, "%.0f", round(n));
    else if (n >= 10) snprintf(buf, sizeof buf, "%.1f", n);
    else snprintf(buf, sizeof buf, "%.2f", n);
    if (strchr(buf, '.') && !(n > 0 && n < 0.01)) {
        size_t l = strlen(buf);
        while (l && buf[l - 1] == '0') buf[--l] = 0;
        if (l && buf[l - 1] == '.') buf[--l] = 0;
    }
    return xstrdup(buf);
}

static char *cost_pair(const ai_model_t *m) {
    if (is_free(m)) return xstrdup("free");
    char *a = cost_leg(m->cost_input), *b = cost_leg(m->cost_output);
    char *r = xasprintf("$%s/%s", a, b);
    free(a);
    free(b);
    return r;
}

/* a catalog row whose contextWindow is null (unknown): no context cell, like omp */
static bool null_field(const ai_model_t *m, const char *key) {
    json_t *v = m->raw ? json_get(m->raw, key) : NULL;
    return v && v->type == JSON_NULL;
}

static bool null_window(const ai_model_t *m) { return null_field(m, "contextWindow"); }

static char *context_cell(const ai_model_t *m) {
    if (m->context_window <= 0 || null_window(m)) return xstrdup("");
    char *n = lower_num((double)m->context_window);
    char *icon = xstrdup(tui_sym("icon.context"));
    size_t il = strlen(icon);
    if (il && icon[il - 1] == ':') icon[il - 1] = 0;
    char *r = xasprintf("%s %s", n, icon);
    free(n);
    free(icon);
    return r;
}

/* brain-icon intelligence score delivered with the catalog (raw "int") */
static char *intelligence_cell(const ai_model_t *m) {
    json_t *v = m && m->raw ? json_get(m->raw, "int") : NULL;
    if (!json_is_num(v) || !isfinite(v->u.num.d)) return xstrdup("");
    return xasprintf("%s %.0f", tui_sym("icon.intelligence"), round(v->u.num.d));
}

/* metaColumnsWidth */
static int meta_width(const int *w) {
    int total = 0, count = 0;
    for (int i = 0; i < 4; i++)
        if (w[i] > 0) total += w[i], count++;
    return count ? total + 2 * (count - 1) : 0;
}

static char *pad_left(const char *s, int w) {
    int miss = w - tui_visible_width(s);
    return miss > 0 ? xasprintf("%*s%s", miss, "", s) : xstrdup(s);
}

ui_model_browser_t *ui_model_browser_new(void) {
    ui_model_browser_t *b = xcalloc(1, sizeof *b);
    b->max_visible = 10;
    b->hovered = -1;
    b->focused = true;
    b->show_provider = true;
    return b;
}

void ui_model_browser_free(ui_model_browser_t *b) {
    if (!b) return;
    ui_model_browser_clear_items(b);
    vec_free(&b->items);
    vec_free(&b->visible);
    free(b->query);
    free(b->current);
    for (size_t i = 0; i < b->nroles; i++) {
        free(b->roles[i].role);
        free(b->roles[i].label);
    }
    free(b->roles);
    strvec_free(&b->mru);
    free(b->empty_text);
    free(b);
}

void ui_model_browser_add(ui_model_browser_t *b, const ai_model_t *m) {
    ui_model_item_t it = {m, xasprintf("%s/%s", m->provider, m->id), NULL, -1};
    vec_push(&b->items, it);
}

void ui_model_browser_add_labeled(ui_model_browser_t *b, const ai_model_t *m, const char *selector, const char *label,
                                  int color) {
    ui_model_item_t it = {m, xstrdup(selector), label ? xstrdup(label) : NULL, color};
    vec_push(&b->items, it);
}

void ui_model_browser_clear_items(ui_model_browser_t *b) {
    for (size_t i = 0; i < b->items.n; i++) {
        free(b->items.v[i].selector);
        free(b->items.v[i].label);
    }
    b->items.n = 0;
    b->visible.n = 0;
    b->selected = 0;
    b->window = 0;
}

bool ui_model_browser_over_context(const ui_model_browser_t *b, const ui_model_item_t *it) {
    if (!it || !b->mark_over_context || b->ctx_tokens <= 0) return false;
    return it->model->context_window > 0 && b->ctx_tokens > it->model->context_window;
}

static bool recent_or_role(const ui_model_browser_t *b, const ui_model_item_t *it) {
    if (strvec_contains(&b->mru, it->selector)) return true;
    for (size_t i = 0; i < b->nroles; i++)
        if (b->roles[i].model == it->model) return true;
    return false;
}

static void refilter(ui_model_browser_t *b, const char *keep) {
    b->visible.n = 0;
    size_t n = b->items.n;
    char **texts = xcalloc(n ? n : 1, sizeof *texts);
    for (size_t i = 0; i < n; i++)
    {
        /* modelSearchText: `${provider}/${id}` (+ " free"); a quick-role row has provider "" and id `@role` */
        const ui_model_item_t *it = &b->items.v[i];
        char *base = it->label ? xasprintf("/%s", it->label) : xstrdup(it->selector);
        texts[i] = is_free(it->model) ? xasprintf("%s free", base) : xstrdup(base);
        free(base);
    }
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx);
    size_t m = tui_fuzzy_rank(b->query ? b->query : "", (const char *const *)texts, n, idx);
    /* #insertSeparator: a rule after the leading recent/role-assigned rows */
    size_t first_other = m;
    if (b->hub)
        for (size_t i = 0; i < m && first_other == m; i++)
            if (!recent_or_role(b, &b->items.v[idx[i]])) first_other = i;
    for (size_t i = 0; i < m; i++) {
        if (i == first_other && i > 0) vec_push(&b->visible, UI_MB_SEPARATOR);
        vec_push(&b->visible, idx[i]);
    }
    free(idx);
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    b->selected = 0;
    if (keep)
        for (size_t i = 0; i < b->visible.n; i++)
            if (b->visible.v[i] != UI_MB_SEPARATOR && !strcmp(b->items.v[b->visible.v[i]].selector, keep)) b->selected = i;
}

static void ensure_visible(ui_model_browser_t *b) {
    int total = (int)b->visible.n, h = b->max_visible;
    if (!total || h <= 0) {
        b->window = 0;
        return;
    }
    int sel = (int)b->selected;
    int maxoff = MAX(0, total - h);
    if (sel < b->window) b->window = sel;
    else if (sel >= b->window + h) b->window = sel - h + 1;
    b->window = MIN(MAX(0, b->window), maxoff);
}

void ui_model_browser_done_adding(ui_model_browser_t *b) {
    refilter(b, b->current);
    ensure_visible(b);
}

void ui_model_browser_set_current(ui_model_browser_t *b, const char *selector) {
    free(b->current);
    b->current = selector ? xstrdup(selector) : NULL;
    for (size_t i = 0; i < b->visible.n; i++)
        if (b->visible.v[i] != UI_MB_SEPARATOR && str_eq(b->items.v[b->visible.v[i]].selector, selector)) b->selected = i;
    ensure_visible(b);
}

bool ui_model_browser_select(ui_model_browser_t *b, const char *selector) {
    for (size_t i = 0; selector && i < b->visible.n; i++)
        if (b->visible.v[i] != UI_MB_SEPARATOR && !strcmp(b->items.v[b->visible.v[i]].selector, selector)) {
            b->selected = i;
            ensure_visible(b);
            return true;
        }
    return false;
}

void ui_model_browser_set_query(ui_model_browser_t *b, const char *q) {
    free(b->query);
    b->query = q && *q ? xstrdup(q) : NULL;
    refilter(b, NULL);
    b->window = 0;
    ensure_visible(b);
}

void ui_model_browser_set_roles(ui_model_browser_t *b, const ui_role_assign_t *roles, size_t n, const strvec_t *mru) {
    b->hub = true;
    b->roles = xcalloc(n ? n : 1, sizeof *b->roles);
    b->nroles = n;
    for (size_t i = 0; i < n; i++) {
        b->roles[i] = roles[i];
        b->roles[i].role = xstrdup(roles[i].role);
        b->roles[i].label = xstrdup(roles[i].label);
    }
    for (size_t i = 0; mru && i < mru->n; i++) strvec_push(&b->mru, mru->v[i]);
}

void ui_model_browser_set_max_visible(ui_model_browser_t *b, int rows) {
    b->max_visible = MAX(1, rows);
    ensure_visible(b);
}

const ui_model_item_t *ui_model_browser_selected(const ui_model_browser_t *b) {
    if (!b->visible.n || b->selected >= b->visible.n || b->visible.v[b->selected] == UI_MB_SEPARATOR) return NULL;
    return &b->items.v[b->visible.v[b->selected]];
}

static void move_sel(ui_model_browser_t *b, int delta, bool wrap) {
    long n = (long)b->visible.n;
    if (!n) return;
    long s = (long)b->selected + delta;
    if (wrap && (delta == 1 || delta == -1)) s = (s % n + n) % n;
    else s = s < 0 ? 0 : s >= n ? n - 1 : s;
    if (b->visible.v[s] == UI_MB_SEPARATOR) { /* disabled row: keep moving the same way */
        long d = delta < 0 ? -1 : 1;
        long t = s + d;
        if (wrap) t = (t % n + n) % n;
        if (t < 0 || t >= n) t = s - d;
        if (t >= 0 && t < n) s = t;
    }
    b->selected = (size_t)s;
    ensure_visible(b);
}

int ui_model_browser_key(ui_model_browser_t *b, const tui_key_t *k) {
    if (tui_kb(k, "tui.select.cancel")) {
        if (b->query && *b->query) {
            free(b->query);
            b->query = NULL;
            const ui_model_item_t *cur = ui_model_browser_selected(b);
            char *keep = cur ? xstrdup(cur->selector) : NULL;
            refilter(b, keep);
            free(keep);
            ensure_visible(b);
            return UI_MB_HANDLED;
        }
        return UI_MB_CANCEL;
    }
    if (tui_kb(k, "tui.select.up")) move_sel(b, -1, true);
    else if (tui_kb(k, "tui.select.down")) move_sel(b, 1, true);
    else if (tui_kb(k, "tui.select.pageUp")) move_sel(b, -b->max_visible, false);
    else if (tui_kb(k, "tui.select.pageDown")) move_sel(b, b->max_visible, false);
    else if (tui_key_matches(k, "home")) move_sel(b, -(int)b->visible.n, false);
    else if (tui_key_matches(k, "end")) move_sel(b, (int)b->visible.n, false);
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "ctrl+j"))
        return ui_model_browser_selected(b) ? UI_MB_ACTIVATE : UI_MB_HANDLED;
    else {
        const char *before = b->query ? b->query : "";
        char *next = NULL;
        const char *txt = tui_key_text(k);
        if (k->kind == TUI_KEY_CHAR && txt && *txt) next = xasprintf("%s%s", before, txt);
        else if (tui_kb(k, "tui.editor.deleteCharBackward") && *before) {
            size_t i = strlen(before) - 1;
            while (i > 0 && ((unsigned char)before[i] & 0xC0) == 0x80) i--;
            next = xstrndup(before, i);
        } else if (tui_kb(k, "tui.editor.deleteWordBackward")) {
            size_t i = strlen(before);
            while (i > 0 && before[i - 1] == ' ') i--;
            while (i > 0 && before[i - 1] != ' ') i--;
            next = xstrndup(before, i);
        } else if (tui_kb(k, "tui.editor.deleteToLineStart")) {
            next = xstrdup("");
        }
        if (!next) return UI_MB_IGNORED;
        if (strcmp(next, before)) {
            free(b->query);
            b->query = next;
            refilter(b, NULL);
            b->window = 0;
            ensure_visible(b);
        } else {
            free(next);
        }
    }
    return UI_MB_HANDLED;
}

/* roles shown as chips in the detail pane, in MODEL_ROLE_IDS order (vision only for image models) */
static const char *const CHIP_ROLES[] = {"default", "smol", "slow", "vision", "tiny", "memory", "widget", "judge"};

static char *thinking_glyph(const char *level) {
    if (!level || !*level || !strcmp(level, "inherit")) return xstrdup("");
    if (!strcmp(level, "off")) return xstrdup(tui_sym("status.disabled"));
    char *key = xasprintf("thinking.%s", !strcmp(level, "auto") ? "autoPending" : level);
    const char *sym = tui_sym(key);
    free(key);
    return xstrndup(sym, strcspn(sym, " "));
}

static void detail_lines(const ui_model_browser_t *b, int width, strvec_t *out) {
    const ui_model_item_t *sel = ui_model_browser_selected(b);
    if (!sel) {
        strvec_push(out, "");
        strvec_push(out, "");
        return;
    }
    const ai_model_t *m = sel->model;
    strvec_t facts = {0};
    strvec_push(&facts, m->name && *m->name ? m->name : m->id);
    if (json_get_bool(m->raw, "isNew", false)) strvec_push(&facts, "new");
    if (json_get_bool(m->raw, "isBeta", false)) strvec_push(&facts, "beta");
    if (json_get_bool(m->raw, "isRecommended", false)) strvec_push(&facts, "recommended");
    if (m->context_window > 0 && !null_window(m)) {
        char *n = lower_num((double)m->context_window);
        strvec_push_own(&facts, xasprintf("%s ctx", n));
        free(n);
    }
    if (m->max_tokens > 0 && !null_field(m, "maxTokens")) {
        char *n = lower_num((double)m->max_tokens);
        strvec_push_own(&facts, xasprintf("%s out", n));
        free(n);
    }
    char *cp = cost_pair(m);
    strvec_push_own(&facts, xasprintf("%s per M", cp));
    free(cp);
    if (m->reasoning) strvec_push(&facts, "reasoning");
    if (m->input_image) strvec_push(&facts, "vision");
    char *iq = intelligence_cell(m);
    if (*iq) strvec_push(&facts, iq);
    free(iq);
    {
        char *key = xasprintf("%s/%s", m->provider, m->id);
        double tps, ttft;
        if (model_perf_get(key, &tps, &ttft, NULL)) {
            strvec_push_own(&facts, tps >= 10 ? xasprintf("~%.0ft/s", round(tps)) : xasprintf("~%.1ft/s", tps));
            if (ttft >= 0)
                strvec_push_own(&facts, ttft >= 10000 ? xasprintf("%.0fs ttft", round(ttft / 1000)) : xasprintf("%.1fs ttft", ttft / 1000));
        } else {
            json_t *ct = json_get(m->raw, "tps");
            if (json_is_num(ct) && ct->u.num.d > 0 && isfinite(ct->u.num.d))
                strvec_push_own(&facts, ct->u.num.d >= 10 ? xasprintf("~%.0ft/s", round(ct->u.num.d)) : xasprintf("~%.1ft/s", ct->u.num.d));
        }
        free(key);
    }
    {
        const char *desc = json_get_str(m->raw, "description");
        if (desc && *desc) {
            sb_t d = SB_INIT;
            bool nl = false;
            for (const char *c = desc; *c; c++) {
                if (*c == '\r' || *c == '\n') {
                    if (!nl) sb_putc(&d, ' ');
                    nl = true;
                    continue;
                }
                nl = false;
                if (*c == '\t') sb_append(&d, "   ");
                else sb_putc(&d, *c);
            }
            char *t = str_trim(sb_cstr(&d));
            if (*t) strvec_push(&facts, t);
            free(t);
            sb_free(&d);
        }
    }
    char *joined = strvec_join(&facts, " \xc2\xb7 ");
    char *l1 = xasprintf("  %s", joined);
    char *l1s = tui_fgs(NULL, TUI_TC_MUTED, l1);
    strvec_push_own(out, tui_truncate_to_width(l1s, width, NULL));
    free(l1s);
    free(l1);
    free(joined);
    strvec_free(&facts);

    if (ui_model_browser_over_context(b, sel)) {
        char *t = lower_num((double)b->ctx_tokens), *w = lower_num((double)m->context_window);
        char *warn = xasprintf("  %s context %s exceeds %s limit \xc2\xb7 compacts with current model, then switches",
                               tui_sym("status.disabled"), t, w);
        char *ws = tui_fgs(NULL, TUI_TC_WARNING, warn);
        strvec_push_own(out, tui_truncate_to_width(ws, width, NULL));
        free(ws);
        free(warn);
        free(t);
        free(w);
        return;
    }
    strvec_t chips = {0};
    if (str_eq(sel->selector, b->current)) {
        char *c = xasprintf("%s current", tui_sym("status.enabled"));
        strvec_push_own(&chips, tui_fgs(NULL, TUI_TC_SUCCESS, c));
        free(c);
    }
    /* hub: formatRoleChip for every assignment holding this model (MODEL_ROLE_IDS order, then custom) */
    for (size_t i = 0; b->hub && i < b->nroles; i++) {
        const ui_role_assign_t *ra = &b->roles[i];
        if (ra->model != m || ra->hidden) continue;
        char *glyph = thinking_glyph(ra->level);
        char *suffix = NULL;
        if (*glyph) {
            char *g = tui_fgs(NULL, TUI_TC_DIM, glyph);
            suffix = xasprintf(" %s", g);
            free(g);
        } else
            suffix = xstrdup("");
        char *body = xasprintf("%s %s", tui_sym(ra->auto_selected ? "status.shadowed" : "status.enabled"), ra->label);
        char *styled = tui_fgs(NULL, ra->auto_selected ? TUI_TC_DIM : (tui_tc_t)ra->color, body);
        strvec_push_own(&chips, xasprintf("%s%s", styled, suffix));
        free(styled);
        free(body);
        free(suffix);
        free(glyph);
    }
    /* every role resolves to this model when it is the only candidate; configured roles show solid */
    for (size_t i = 0; !b->hub && i < ARRAY_LEN(CHIP_ROLES); i++) {
        const char *role = CHIP_ROLES[i];
        if (!strcmp(role, "vision") && !m->input_image) continue;
        char *path = xasprintf("modelRoles.%s", role);
        const char *val = settings_str(path, NULL);
        free(path);
        bool configured = val && *val;
        if (configured) {
            char *bare = xstrndup(val, strcspn(val, ":"));
            bool match = !strcmp(bare, sel->selector);
            free(bare);
            if (!match) continue;
        } else if (b->items.n != 1 && !b->single_pool) {
            continue; /* auto-selection is only predictable with a single candidate */
        }
        char *glyph = thinking_glyph(!strcmp(role, "default") ? settings_str("defaultThinkingLevel", "high") : NULL);
        char *suffix = *glyph ? xasprintf(" %s", glyph) : xstrdup("");
        char *suffix_s = *glyph ? tui_fgs(NULL, TUI_TC_DIM, suffix) : xstrdup("");
        char *body = xasprintf("%s %s", tui_sym(configured ? "status.enabled" : "status.shadowed"), role);
        char *styled = tui_fgs(NULL, configured ? TUI_TC_MUTED : TUI_TC_DIM, body);
        strvec_push_own(&chips, xasprintf("%s%s", styled, suffix_s));
        free(styled);
        free(body);
        free(suffix_s);
        free(suffix);
        free(glyph);
    }
    if (chips.n) {
        char *sep = tui_fgs(NULL, TUI_TC_DIM, " \xc2\xb7 ");
        char *j = strvec_join(&chips, sep);
        char *l2 = xasprintf("  %s", j);
        strvec_push_own(out, tui_truncate_to_width(l2, width, NULL));
        free(l2);
        free(j);
        free(sep);
    } else {
        strvec_push(out, "");
    }
    strvec_free(&chips);
}

void ui_model_browser_render(const ui_model_browser_t *b, int width, strvec_t *out) {
    const char *icon = tui_sym("icon.search");
    char *si = tui_fgs(NULL, TUI_TC_ACCENT, icon);
    int input_w = MAX(4, width - tui_visible_width(icon) - 2);
    char *field = xasprintf("> %s", b->query ? b->query : "");
    char *ft = tui_truncate_to_width(field, MAX(1, input_w - 1), "");
    char *cursor = b->focused || b->hub ? xstrdup("\x1b[7m \x1b[27m") : xstrdup("");
    strvec_push_own(out, xasprintf(" %s %s%s", si, ft, cursor));
    free(cursor);
    free(ft);
    free(field);
    free(si);
    strvec_push(out, "");

    int total = (int)b->visible.n;
    int start = b->window, end = MIN(start + b->max_visible, total);
    if (total == 0) {
        const char *msg = b->empty_text ? b->empty_text : b->query && *b->query ? "  No matching models" : "  No models available in this scope";
        char *m = tui_fgs(NULL, TUI_TC_MUTED, msg);
        strvec_push_own(out, tui_truncate_to_width(m, width, NULL));
        free(m);
        for (int i = 1; i < b->max_visible; i++) strvec_push(out, "");
    } else {
        int ctx_w = 0, cost_w = 0, int_w = 0, perf_w = 0;
        /* perf / intelligence columns: hub browsers (the setup wizard's has no perf data to show) */
        int perf_mode = width >= PERF_FULL_MIN_WIDTH ? 2 : width >= PERF_TPS_MIN_WIDTH ? 1 : 0;
        ((ui_model_browser_t *)b)->window_count = MAX(0, end - start);
        for (int i = start; i < end; i++) {
            if (b->visible.v[i] == UI_MB_SEPARATOR) continue;
            const ai_model_t *m = b->items.v[b->visible.v[i]].model;
            char *c = context_cell(m), *p = cost_pair(m);
            ctx_w = MAX(ctx_w, tui_visible_width(c));
            cost_w = MAX(cost_w, tui_visible_width(p));
            free(c);
            free(p);
            if (perf_mode) {
                char *iq = intelligence_cell(m), *pc = model_perf_cell(m, perf_mode == 2);
                int_w = MAX(int_w, tui_visible_width(iq));
                perf_w = MAX(perf_w, tui_visible_width(pc));
                free(iq);
                free(pc);
            }
        }
        int cols_w[4] = {int_w, perf_w, ctx_w, cost_w};
        if (width - 2 - meta_width(cols_w) < MIN_NAME_WIDTH) cost_w = cols_w[3] = 0;
        if (width - 2 - meta_width(cols_w) < MIN_NAME_WIDTH) ctx_w = cols_w[2] = 0;
        int meta = meta_width(cols_w);
        strvec_t rows = {0};
        int rw = width - 1;
        for (int i = start; i < end; i++) {
            if (b->visible.v[i] == UI_MB_SEPARATOR) {
                sb_t d = SB_INIT;
                sb_repeat(&d, "\xe2\x94\x80", MAX(0, rw - 4));
                char *dl = tui_fgs(NULL, TUI_TC_MUTED, sb_cstr(&d));
                strvec_push_own(&rows, xasprintf("  %s  ", dl));
                free(dl);
                sb_free(&d);
                continue;
            }
            const ui_model_item_t *it = &b->items.v[b->visible.v[i]];
            bool sel = (size_t)i == b->selected;
            sb_t l = SB_INIT;
            if (sel && b->focused) {
                tui_fg(&l, NULL, TUI_TC_ACCENT, tui_sym("nav.cursor"));
                sb_putc(&l, ' ');
            } else {
                sb_append(&l, "  ");
            }
            if (b->show_provider) {
                char *pp = xasprintf("%s/", it->model->provider);
                tui_fg(&l, NULL, TUI_TC_DIM, pp);
                free(pp);
            }
            const char *name = it->label ? it->label : it->model->id;
            if (it->label_color >= 0) tui_fg(&l, NULL, (tui_tc_t)it->label_color, name);
            else if (sel) tui_fg(&l, NULL, TUI_TC_ACCENT, name);
            else sb_append(&l, name);
            if (str_eq(it->selector, b->current)) {
                sb_putc(&l, ' ');
                tui_fg(&l, NULL, TUI_TC_SUCCESS, tui_sym("status.enabled"));
            }
            bool over = ui_model_browser_over_context(b, it);
            if (over) {
                char *n = lower_num((double)it->model->context_window);
                sb_appendf(&l, " %s context>%s", tui_sym("status.disabled"), n);
                free(n);
            }
            int ncols = (int_w > 0) + (perf_w > 0) + (ctx_w > 0) + (cost_w > 0);
            int avail = MAX(1, rw - meta - (ncols ? 1 : 0));
            char *left = tui_truncate_to_width(sb_cstr(&l), avail, NULL);
            sb_free(&l);
            sb_t line = SB_INIT;
            sb_append(&line, left);
            tui_sb_spaces(&line, avail - tui_visible_width(left));
            free(left);
            if (ncols) {
                sb_putc(&line, ' ');
                bool first = true;
                if (int_w) {
                    char *c = intelligence_cell(it->model), *pc = pad_left(c, int_w);
                    tui_fg(&line, NULL, TUI_TC_DIM, pc);
                    free(pc);
                    free(c);
                    first = false;
                }
                if (perf_w) {
                    if (!first) sb_append(&line, "  ");
                    char *c = model_perf_cell(it->model, perf_mode == 2), *pc = pad_left(c, perf_w);
                    tui_fg(&line, NULL, TUI_TC_DIM, pc);
                    free(pc);
                    free(c);
                    first = false;
                }
                if (ctx_w) {
                    if (!first) sb_append(&line, "  ");
                    char *c = context_cell(it->model), *pc = pad_left(c, ctx_w);
                    tui_fg(&line, NULL, TUI_TC_DIM, pc);
                    free(pc);
                    free(c);
                    first = false;
                }
                if (cost_w) {
                    if (!first) sb_append(&line, "  ");
                    char *c = cost_pair(it->model), *pc = pad_left(c, cost_w);
                    tui_fg(&line, NULL, TUI_TC_DIM, pc);
                    free(pc);
                    free(c);
                }
            }
            if (over) {
                /* gray the whole row but keep the selection cursor */
                char *plain = tui_strip_ansi(sb_cstr(&line));
                bool cur = sel && b->focused;
                size_t pl = cur ? strlen(tui_sym("nav.cursor")) + 1 : 2;
                if (pl > strlen(plain)) pl = strlen(plain);
                sb_t g = SB_INIT;
                if (cur) {
                    tui_fg(&g, NULL, TUI_TC_ACCENT, tui_sym("nav.cursor"));
                    sb_putc(&g, ' ');
                } else
                    sb_append(&g, "  ");
                tui_fg(&g, NULL, TUI_TC_DIM, plain + pl);
                free(plain);
                sb_free(&line);
                line = g;
            }
            if (i == b->hovered) {
                char *band = tui_bgs(NULL, TUI_TC_SELECTED_BG, sb_cstr(&line));
                sb_clear(&line);
                sb_append(&line, band);
                free(band);
            }
            strvec_push_own(&rows, sb_detach(&line));
        }
        /* ScrollView(scrollbar auto, totalRows = total) */
        int h = (int)rows.n;
        bool bar = total > h;
        int cw = MAX(0, width - (bar ? 1 : 0));
        int ts = 0, te = h;
        if (bar) {
            int size = MAX(1, MIN((h * h) / total, h));
            int maxoff = MAX(0, total - h);
            ts = maxoff == 0 ? 0 : (int)lround((double)MIN(start, maxoff) / maxoff * (h - size));
            te = ts + size;
        }
        for (int r = 0; r < h; r++) {
            char *tr = tui_truncate_to_width(rows.v[r], cw, NULL);
            if (!bar) {
                strvec_push_own(out, tr);
                continue;
            }
            sb_t s = SB_INIT;
            sb_append(&s, tr);
            tui_sb_spaces(&s, cw - tui_visible_width(tr));
            free(tr);
            bool thumb = r >= ts && r < te;
            tui_fg(&s, NULL, thumb ? TUI_TC_ACCENT : TUI_TC_MUTED, thumb ? "\xe2\x96\x88" : "\xe2\x94\x82");
            strvec_push_own(out, sb_detach(&s));
        }
        for (int i = h; i < b->max_visible; i++) strvec_push(out, "");
        strvec_free(&rows);
    }
    strvec_push(out, "");
    detail_lines(b, width, out);
}

/* available chat models (credentials present), catalog order */
void ui_model_browser_load_available(ui_model_browser_t *b) {
    for (size_t i = 0; i < catalog_count(); i++) {
        const ai_model_t *m = catalog_at(i);
        if (!auth_has_credentials(m->provider) && !catalog_provider_keyless(m->provider)) continue;
        ui_model_browser_add(b, m);
    }
    ui_model_browser_done_adding(b);
}

int ui_model_browser_mouse(ui_model_browser_t *b, int line, int wheel, bool motion, bool click) {
    int list_line = line - LIST_ROW_START;
    int idx = list_line >= 0 && list_line < b->window_count ? b->window + list_line : -1;
    if (idx >= (int)b->visible.n || (idx >= 0 && b->visible.v[idx] == UI_MB_SEPARATOR)) idx = -1;
    if (wheel) {
        int maxoff = MAX(0, (int)b->visible.n - b->max_visible);
        b->window = MAX(0, MIN(maxoff, b->window + wheel));
        list_line = line - LIST_ROW_START;
        idx = list_line >= 0 && list_line < b->window_count ? b->window + list_line : -1;
        if (idx >= (int)b->visible.n || (idx >= 0 && b->visible.v[idx] == UI_MB_SEPARATOR)) idx = -1;
        b->hovered = idx;
        return UI_MB_HANDLED;
    }
    if (motion) {
        b->hovered = idx;
        return UI_MB_HANDLED;
    }
    if (!click || idx < 0) return UI_MB_IGNORED;
    if ((size_t)idx == b->selected) return UI_MB_ACTIVATE;
    b->selected = (size_t)idx;
    ensure_visible(b);
    return UI_MB_HANDLED;
}

/* extractVersionNumber */
static double version_number(const char *id) {
    size_t n = strlen(id);
    for (size_t i = 0; i < n; i++) { /* (?:^|[-_])(\d+\.\d+) */
        if (i && id[i - 1] != '-' && id[i - 1] != '_') continue;
        size_t j = i;
        while (j < n && isdigit((unsigned char)id[j])) j++;
        if (j > i && id[j] == '.' && isdigit((unsigned char)id[j + 1])) return strtod(id + i, NULL);
    }
    for (size_t i = 0; i < n; i++) { /* (?:^|[-_])(\d{1,2})-(\d{1,2})(?=-|$) */
        if (i && id[i - 1] != '-' && id[i - 1] != '_') continue;
        size_t j = i;
        while (j < n && isdigit((unsigned char)id[j])) j++;
        if (j == i || j - i > 2 || id[j] != '-') continue;
        size_t k = j + 1;
        while (k < n && isdigit((unsigned char)id[k])) k++;
        if (k == j + 1 || k - j - 1 > 2 || (id[k] && id[k] != '-')) continue;
        char buf[16];
        snprintf(buf, sizeof buf, "%.*s.%.*s", (int)(j - i), id + i, (int)(k - j - 1), id + j + 1);
        return strtod(buf, NULL);
    }
    for (size_t i = 0; i < n; i++) { /* (?:^|[-_])(\d+) */
        if (i && id[i - 1] != '-' && id[i - 1] != '_') continue;
        if (isdigit((unsigned char)id[i])) return strtod(id + i, NULL);
    }
    return 0;
}

static const char *date_suffix(const char *id) {
    size_t n = strlen(id);
    if (n < 9 || id[n - 9] != '-') return "";
    for (size_t i = n - 8; i < n; i++)
        if (!isdigit((unsigned char)id[i])) return "";
    return id + n - 8;
}

static const ui_role_assign_t *g_sort_roles;
static size_t g_sort_nroles;
static const strvec_t *g_sort_mru;

static int role_rank(const ai_model_t *m) {
    static const char *const IDS[] = {"default", "smol", "slow", "vision", "plan", "commit", "tiny", "memory",
                                      "task",    "advisor", "widget", "image", "web", "speech", "dictation", "judge"};
    size_t i = 0;
    for (; i < ARRAY_LEN(IDS); i++) {
        for (size_t k = 0; k < g_sort_nroles; k++)
            if (!strcmp(g_sort_roles[k].role, IDS[i]) && g_sort_roles[k].model == m) return (int)i;
    }
    return (int)i;
}

static long mru_index(const ai_model_t *m) {
    char *k = xasprintf("%s/%s", m->provider, m->id);
    long r = LONG_MAX;
    for (size_t i = 0; g_sort_mru && i < g_sort_mru->n; i++)
        if (!strcmp(g_sort_mru->v[i], k)) {
            r = (long)i;
            break;
        }
    free(k);
    return r;
}

/* String.localeCompare approximation: case-insensitive first, then lowercase before uppercase */
static int locale_cmp(const char *a, const char *b) {
    int c = strcasecmp(a, b);
    if (c) return c;
    return -strcmp(a, b);
}

static int item_cmp(const void *pa, const void *pb) {
    const ai_model_t *a = *(const ai_model_t *const *)pa, *b = *(const ai_model_t *const *)pb;
    int ra = role_rank(a), rb = role_rank(b);
    if (ra != rb) return ra - rb;
    long ma = mru_index(a), mb = mru_index(b);
    if (ma != mb) return ma < mb ? -1 : 1;
    int pc = locale_cmp(a->provider, b->provider);
    if (pc) return pc;
    double pa_ = json_get_num(a->raw, "priority", 9007199254740991.0), pb_ = json_get_num(b->raw, "priority", 9007199254740991.0);
    if (pa_ != pb_) return pa_ < pb_ ? -1 : 1;
    double va = version_number(a->id), vb = version_number(b->id);
    if (va != vb) return vb < va ? -1 : 1;
    bool la = str_ends(a->id, "-latest"), lb = str_ends(b->id, "-latest");
    const char *da = date_suffix(a->id), *db = date_suffix(b->id);
    bool ra_ = la || *da, rb_ = lb || *db;
    if (ra_ != rb_) return ra_ ? -1 : 1;
    if (!ra_) return locale_cmp(a->id, b->id);
    if (la != lb) return la ? -1 : 1;
    if (*da && *db) return strcmp(db, da);
    return locale_cmp(a->id, b->id);
}

void ui_model_sort_items(const ai_model_t **models, size_t n, const ui_role_assign_t *roles, size_t nroles,
                         const strvec_t *mru) {
    g_sort_roles = roles;
    g_sort_nroles = nroles;
    g_sort_mru = mru;
    /* stable: insertion order breaks remaining ties */
    for (size_t i = 1; i < n; i++) {
        const ai_model_t *x = models[i];
        size_t j = i;
        while (j > 0 && item_cmp(&models[j - 1], &x) > 0) {
            models[j] = models[j - 1];
            j--;
        }
        models[j] = x;
    }
}
