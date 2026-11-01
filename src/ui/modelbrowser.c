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
#include <math.h>

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

static char *context_cell(const ai_model_t *m) {
    if (m->context_window <= 0) return xstrdup("");
    char *n = lower_num((double)m->context_window);
    char *icon = xstrdup(tui_sym("icon.context"));
    size_t il = strlen(icon);
    if (il && icon[il - 1] == ':') icon[il - 1] = 0;
    char *r = xasprintf("%s %s", n, icon);
    free(n);
    free(icon);
    return r;
}

static char *pad_left(const char *s, int w) {
    int miss = w - tui_visible_width(s);
    return miss > 0 ? xasprintf("%*s%s", miss, "", s) : xstrdup(s);
}

ui_model_browser_t *ui_model_browser_new(void) {
    ui_model_browser_t *b = xcalloc(1, sizeof *b);
    b->max_visible = 10;
    b->focused = true;
    b->show_provider = true;
    return b;
}

void ui_model_browser_free(ui_model_browser_t *b) {
    if (!b) return;
    for (size_t i = 0; i < b->items.n; i++) free(b->items.v[i].selector);
    vec_free(&b->items);
    vec_free(&b->visible);
    free(b->query);
    free(b->current);
    free(b);
}

void ui_model_browser_add(ui_model_browser_t *b, const ai_model_t *m) {
    ui_model_item_t it = {m, xasprintf("%s/%s", m->provider, m->id)};
    vec_push(&b->items, it);
}

static void refilter(ui_model_browser_t *b, const char *keep) {
    b->visible.n = 0;
    size_t n = b->items.n;
    char **texts = xcalloc(n ? n : 1, sizeof *texts);
    for (size_t i = 0; i < n; i++)
        texts[i] = is_free(b->items.v[i].model) ? xasprintf("%s free", b->items.v[i].selector) : xstrdup(b->items.v[i].selector);
    size_t *idx = xmalloc((n ? n : 1) * sizeof *idx);
    size_t m = tui_fuzzy_rank(b->query ? b->query : "", (const char *const *)texts, n, idx);
    for (size_t i = 0; i < m; i++) vec_push(&b->visible, idx[i]);
    free(idx);
    for (size_t i = 0; i < n; i++) free(texts[i]);
    free(texts);
    b->selected = 0;
    if (keep)
        for (size_t i = 0; i < b->visible.n; i++)
            if (!strcmp(b->items.v[b->visible.v[i]].selector, keep)) b->selected = i;
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
        if (str_eq(b->items.v[b->visible.v[i]].selector, selector)) b->selected = i;
    ensure_visible(b);
}

void ui_model_browser_set_max_visible(ui_model_browser_t *b, int rows) {
    b->max_visible = MAX(1, rows);
    ensure_visible(b);
}

const ui_model_item_t *ui_model_browser_selected(const ui_model_browser_t *b) {
    if (!b->visible.n || b->selected >= b->visible.n) return NULL;
    return &b->items.v[b->visible.v[b->selected]];
}

static void move_sel(ui_model_browser_t *b, int delta, bool wrap) {
    long n = (long)b->visible.n;
    if (!n) return;
    long s = (long)b->selected + delta;
    if (wrap && (delta == 1 || delta == -1)) s = (s % n + n) % n;
    else s = s < 0 ? 0 : s >= n ? n - 1 : s;
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
    if (m->context_window > 0) {
        char *n = lower_num((double)m->context_window);
        strvec_push_own(&facts, xasprintf("%s ctx", n));
        free(n);
    }
    if (m->max_tokens > 0) {
        char *n = lower_num((double)m->max_tokens);
        strvec_push_own(&facts, xasprintf("%s out", n));
        free(n);
    }
    char *cp = cost_pair(m);
    strvec_push_own(&facts, xasprintf("%s per M", cp));
    free(cp);
    if (m->reasoning) strvec_push(&facts, "reasoning");
    if (m->input_image) strvec_push(&facts, "vision");
    char *joined = strvec_join(&facts, " \xc2\xb7 ");
    char *l1 = xasprintf("  %s", joined);
    char *l1s = tui_fgs(NULL, TUI_TC_MUTED, l1);
    strvec_push_own(out, tui_truncate_to_width(l1s, width, NULL));
    free(l1s);
    free(l1);
    free(joined);
    strvec_free(&facts);

    strvec_t chips = {0};
    if (str_eq(sel->selector, b->current)) {
        char *c = xasprintf("%s current", tui_sym("status.enabled"));
        strvec_push_own(&chips, tui_fgs(NULL, TUI_TC_SUCCESS, c));
        free(c);
    }
    /* every role resolves to this model when it is the only candidate; configured roles show solid */
    for (size_t i = 0; i < ARRAY_LEN(CHIP_ROLES); i++) {
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
    char *cursor = b->focused ? xstrdup("\x1b[7m \x1b[27m") : xstrdup("");
    strvec_push_own(out, xasprintf(" %s %s%s", si, ft, cursor));
    free(cursor);
    free(ft);
    free(field);
    free(si);
    strvec_push(out, "");

    int total = (int)b->visible.n;
    int start = b->window, end = MIN(start + b->max_visible, total);
    if (total == 0) {
        const char *msg = b->query && *b->query ? "  No matching models" : "  No models available in this scope";
        char *m = tui_fgs(NULL, TUI_TC_MUTED, msg);
        strvec_push_own(out, tui_truncate_to_width(m, width, NULL));
        free(m);
        for (int i = 1; i < b->max_visible; i++) strvec_push(out, "");
    } else {
        int ctx_w = 0, cost_w = 0;
        for (int i = start; i < end; i++) {
            const ai_model_t *m = b->items.v[b->visible.v[i]].model;
            char *c = context_cell(m), *p = cost_pair(m);
            ctx_w = MAX(ctx_w, tui_visible_width(c));
            cost_w = MAX(cost_w, tui_visible_width(p));
            free(c);
            free(p);
        }
        int meta = (ctx_w ? ctx_w : 0) + (cost_w ? cost_w : 0) + (ctx_w && cost_w ? 2 : 0);
        if (width - 2 - meta < MIN_NAME_WIDTH) cost_w = 0;
        meta = ctx_w + cost_w + (ctx_w && cost_w ? 2 : 0);
        if (width - 2 - meta < MIN_NAME_WIDTH) ctx_w = 0;
        meta = ctx_w + cost_w + (ctx_w && cost_w ? 2 : 0);
        strvec_t rows = {0};
        int rw = width - 1;
        for (int i = start; i < end; i++) {
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
            if (sel) tui_fg(&l, NULL, TUI_TC_ACCENT, it->model->id);
            else sb_append(&l, it->model->id);
            if (str_eq(it->selector, b->current)) {
                sb_putc(&l, ' ');
                tui_fg(&l, NULL, TUI_TC_SUCCESS, tui_sym("status.enabled"));
            }
            int ncols = (ctx_w > 0) + (cost_w > 0);
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
                if (ctx_w) {
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
        if (!auth_has_credentials(m->provider)) continue;
        ui_model_browser_add(b, m);
    }
    ui_model_browser_done_adding(b);
}
