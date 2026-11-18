/* histsearch.c - Ctrl+R prompt history search (port of omp packages/tui/src/overlays/history-search.ts, shown
 * through SelectorController.showHistorySearch, over the HistoryStorage semantics of
 * coding-agent/src/session/history-storage.ts).
 *
 * omp keeps the prompt history in SQLite (history.db, FTS5 prefix match plus a token-AND LIKE substring
 * fallback); omc's store is prompt-history.jsonl ({text, cwd, ts} per submission, oldest first). The jsonl is
 * folded into HistoryStorage's rows: one row per normalized prompt (normalizePrompt), its created_at / cwd from
 * the latest submission and its id from the first. getRecent orders by created_at, id descending; search keeps
 * the rows where every query token (split on non-letters/digits, lowercased) is a case-insensitive substring,
 * which is the union FTS prefix + LIKE substring returns. At most 100 results (the component's resultLimit).
 *
 * The overlay replaces the composer: an OverlayPanel "History" with the search Input, up to 10 result rows
 * (selection-centered window, scrollbar column when they overflow, query tokens in the accent color, the
 * age flush right) and the key hints. Enter puts the selected prompt in the composer; Esc cancels.
 */
#include "ui/ui.h"

#include <ctype.h>

#define MAX_VISIBLE 10
#define RESULT_LIMIT 100

/* ---------- history storage ---------- */

/* normalizePrompt: CRLF/CR -> LF, trailing whitespace stripped from every line, outer whitespace trimmed */
static char *normalize_prompt(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (*p == '\r') {
            if (p[1] == '\n') p++;
            sb_putc(&b, '\n');
        } else sb_putc(&b, *p);
    }
    sb_t o = SB_INIT;
    const char *t = sb_cstr(&b);
    for (size_t i = 0; t[i]; i++) {
        if (t[i] == '\n') {
            while (o.len && (o.s[o.len - 1] == ' ' || o.s[o.len - 1] == '\t' || o.s[o.len - 1] == '\v' ||
                             o.s[o.len - 1] == '\f'))
                o.len--;
            if (o.s) o.s[o.len] = 0;
        }
        sb_putc(&o, t[i]);
    }
    sb_free(&b);
    char *r = str_trim(sb_cstr(&o));
    sb_free(&o);
    return r;
}

/* #tokenize / queryTokens: lowercase, split on runs of non-letter / non-digit characters */
static void query_tokens(const char *q, strvec_t *out) {
    char *l = str_lower(q ? q : "");
    sb_t cur = SB_INIT;
    for (const unsigned char *p = (const unsigned char *)l;; p++) {
        bool word = *p && (isalnum(*p) || *p >= 0x80);
        if (word) sb_putc(&cur, (char)*p);
        else if (cur.len) {
            strvec_push(out, sb_cstr(&cur));
            cur.len = 0;
            cur.s[0] = 0;
        }
        if (!*p) break;
    }
    sb_free(&cur);
    free(l);
}

/* ASCII case-insensitive substring (SQLite LIKE semantics; tokens are already lowercase) */
static bool contains_ci(const char *hay, const char *tok) {
    size_t n = strlen(tok);
    if (!n) return true;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == (unsigned char)tok[i]) i++;
        if (i == n) return true;
    }
    return false;
}

typedef struct {
    char *prompt, *cwd;
    int64_t created_at;
    size_t id;
} row_t;

static int row_cmp(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->created_at != y->created_at) return x->created_at < y->created_at ? 1 : -1;
    return x->id < y->id ? 1 : x->id > y->id ? -1 : 0;
}

void ui_hist_query(const char *jsonl, const char *query, int limit, ui_hist_list_t *out) {
    if (!jsonl || limit <= 0) return;
    strvec_t lines = str_split_lines(jsonl, strlen(jsonl));
    VEC(row_t) rows = {0};
    hmap_t idx;
    hmap_init(&idx);
    for (size_t i = 0; i < lines.n; i++) {
        json_t *j = json_parse_str(lines.v[i]);
        const char *t = json_get_str(j, "text");
        char *p = t ? normalize_prompt(t) : NULL;
        if (p && *p) {
            int64_t ts = json_get_int(j, "ts", 0) / 1000; /* omc stores ms, HistoryStorage seconds */
            const char *cwd = json_get_str(j, "cwd");
            size_t *at = hmap_get(&idx, p);
            if (at) { /* ON CONFLICT(prompt): the latest submission's created_at / cwd, the row id stays */
                row_t *r = &rows.v[*at];
                r->created_at = ts;
                free(r->cwd);
                r->cwd = cwd ? xstrdup(cwd) : NULL;
            } else {
                row_t r = {xstrdup(p), cwd ? xstrdup(cwd) : NULL, ts, rows.n};
                size_t *slot = xmalloc(sizeof *slot);
                *slot = rows.n;
                hmap_put(&idx, p, slot);
                vec_push(&rows, r);
            }
        }
        free(p);
        json_free(j);
    }
    hmap_free(&idx, free);
    strvec_free(&lines);
    strvec_t toks = {0};
    query_tokens(query, &toks);
    char *trimmed = str_trim(query ? query : "");
    bool searching = *trimmed;
    free(trimmed);
    if (rows.n) qsort(rows.v, rows.n, sizeof(row_t), row_cmp);
    /* search() with no tokens (punctuation only) returns nothing */
    for (size_t i = 0; i < rows.n; i++) {
        row_t *r = &rows.v[i];
        bool keep = (int)out->n < limit && (!searching || toks.n > 0);
        for (size_t k = 0; keep && searching && k < toks.n; k++) keep = contains_ci(r->prompt, toks.v[k]);
        if (keep) {
            ui_hist_entry_t e = {r->prompt, r->created_at, r->cwd};
            vec_push(out, e);
        } else {
            free(r->prompt);
            free(r->cwd);
        }
    }
    vec_free(&rows);
    strvec_free(&toks);
}

void ui_hist_list_free(ui_hist_list_t *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].prompt);
        free(l->v[i].cwd);
    }
    vec_free(l);
}

/* ---------- component ---------- */

typedef struct {
    tui_component_t base;
    ui_t *u;
    char *jsonl;           /* prompt-history.jsonl, read when the overlay opened */
    ui_input_t input;
    ui_hist_list_t items;
    strvec_t tokens;       /* highlight tokens of the current results' query */
    int sel;               /* -1 when no results */
    char *results_query;   /* trimmed query the results were fetched for */
    tui_editor_t *target;  /* receives the pick: the composer, or the prompt Ctrl+R was pressed in */
} hs_t;

/* pickerAge */
static void picker_age(int64_t at_ms, char *buf, size_t n) {
    int64_t s = (now_ms() - at_ms) / 1000;
    if (s < 0) s = 0;
    if (s < 60) {
        snprintf(buf, n, "now");
        return;
    }
    int64_t m = s / 60;
    if (m < 60) {
        snprintf(buf, n, "%lldm", (long long)m);
        return;
    }
    int64_t h = m / 60;
    if (h < 24) {
        snprintf(buf, n, "%lldh", (long long)h);
        return;
    }
    int64_t d = h / 24;
    if (d < 7) snprintf(buf, n, "%lldd", (long long)d);
    else if (d < 30) snprintf(buf, n, "%lldw", (long long)(d / 7));
    else if (d < 365) snprintf(buf, n, "%lldmo", (long long)(d / 30));
    else snprintf(buf, n, "%lldy", (long long)(d / 365));
}

static void hs_update(hs_t *h) {
    char *q = str_trim(ui_input_value(&h->input));
    free(h->results_query);
    h->results_query = q;
    ui_hist_list_free(&h->items);
    memset(&h->items, 0, sizeof h->items);
    ui_hist_query(h->jsonl, *q ? q : NULL, RESULT_LIMIT, &h->items);
    h->sel = h->items.n ? 0 : -1;
    strvec_free(&h->tokens);
    memset(&h->tokens, 0, sizeof h->tokens);
    if (*q) query_tokens(q, &h->tokens);
}

/* #scheduleResults: no-op while the trimmed query is unchanged (cursor moves, trailing spaces) */
static void hs_schedule(hs_t *h) {
    char *q = str_trim(ui_input_value(&h->input));
    bool same = h->results_query && !strcmp(q, h->results_query);
    free(q);
    if (!same) hs_update(h);
}

/* highlightTokens over pickerHits: merged case-insensitive occurrences of any token, in the accent color */
static char *highlight(const char *text, const strvec_t *tokens) {
    size_t n = strlen(text);
    if (!tokens->n || !n) return xstrdup(text);
    char *lower = str_lower(text);
    bool *hit = xcalloc(n + 1, 1);
    for (size_t t = 0; t < tokens->n; t++) {
        size_t tl = strlen(tokens->v[t]);
        if (!tl) continue;
        for (const char *p = strstr(lower, tokens->v[t]); p; p = strstr(p + tl, tokens->v[t]))
            memset(hit + (p - lower), 1, tl);
    }
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && hit[j] == hit[i]) j++;
        char *seg = xstrndup(text + i, j - i);
        if (hit[i]) {
            char *s = tui_fgs(NULL, TUI_TC_ACCENT, seg);
            sb_append(&b, s);
            free(s);
        } else sb_append(&b, seg);
        free(seg);
        i = j;
    }
    free(hit);
    free(lower);
    return sb_detach(&b);
}

/* prompt.replace(/\s+/g, " ").trim() */
static char *flatten(const char *s) {
    sb_t b = SB_INIT;
    bool sp = false;
    for (const char *p = s; *p; p++) {
        if (isspace((unsigned char)*p)) sp = true;
        else {
            if (sp && b.len) sb_putc(&b, ' ');
            sp = false;
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

static char *fit(const char *s, int w, const char *ellipsis, bool pad) {
    char *t = tui_truncate_to_width(s, w, ellipsis);
    if (!pad) return t;
    char *p = tui_pad_to_width(t, w);
    free(t);
    return p;
}

/* HistoryResultsList.render */
static void hs_render_list(hs_t *h, int width, strvec_t *out) {
    int total = (int)h->items.n;
    if (!total) {
        char *m = xasprintf("  %s %s", tui_sym("status.info"), h->tokens.n ? "No matching history" : "No history yet");
        strvec_push_own(out, tui_fgs(NULL, TUI_TC_MUTED, m));
        free(m);
        return;
    }
    char *cursor_sym = xasprintf("%s ", tui_sym("nav.cursor"));
    int gutter = tui_visible_width(cursor_sym);
    /* centeredViewportRange */
    int height = MIN(MAX_VISIBLE, total);
    int start = h->sel - MAX_VISIBLE / 2;
    start = MAX(0, MIN(start, total - height));
    int end = MIN(total, start + MAX_VISIBLE);
    bool overflow = total > MAX_VISIBLE;
    int row_w = MAX(0, width - (overflow ? 1 : 0));
    strvec_t rows = {0};
    for (int i = 0; i < total; i++) {
        if (i < start || i >= end) {
            strvec_push(&rows, "");
            continue;
        }
        const ui_hist_entry_t *e = &h->items.v[i];
        bool selected = i == h->sel;
        char age[16];
        picker_age(e->created_at * 1000, age, sizeof age);
        int tw = tui_visible_width(age);
        bool show_time = row_w >= gutter + 12 + tw;
        int budget = MAX(4, row_w - gutter - (show_time ? tw + 1 : 0));
        char *flat = flatten(e->prompt);
        char *cut = tui_truncate_to_width(flat, budget, NULL), *plain = tui_strip_ansi(cut);
        char *hl = highlight(plain, &h->tokens);
        char *cursor = selected ? tui_fgs(NULL, TUI_TC_ACCENT, cursor_sym) : xasprintf("%*s", gutter, "");
        char *body = selected ? tui_bold(hl) : xstrdup(hl);
        char *line = xasprintf("%s%s", cursor, body);
        if (show_time) {
            char *l = fit(line, row_w - tw - 1, NULL, true), *d = tui_fgs(NULL, TUI_TC_DIM, age);
            free(line);
            line = xasprintf("%s %s", l, d);
            free(l);
            free(d);
        }
        if (selected) {
            char *f = fit(line, row_w, "", true);
            strvec_push_own(&rows, tui_bgs(NULL, TUI_TC_SELECTED_BG, f));
            free(f);
        } else strvec_push_own(&rows, fit(line, row_w, NULL, false));
        free(flat), free(cut), free(plain), free(hl), free(cursor), free(body), free(line);
    }
    /* renderScrollableList: the window, plus a track / thumb column when the list overflows */
    int off = start;
    ui_scroll_view(&rows, end - start, &off, width, out);
    strvec_free(&rows);
    free(cursor_sym);
}

static void hs_render(tui_component_t *c, int width, strvec_t *out) {
    hs_t *h = (hs_t *)c;
    strvec_push(out, "");
    /* the panel holds focus, so the nested Input paints its own (inverse) cursor cell */
    strvec_push_own(out, ui_input_render(&h->input, "> ", width, false));
    strvec_push(out, "");
    hs_render_list(h, width, out);
    strvec_push(out, "");
    char *dot = tui_fgs(NULL, TUI_TC_DIM, tui_sym("sep.dot"));
    char *up = ui_editor_key("tui.select.up"), *down = ui_editor_key("tui.select.down");
    char *ud = xasprintf("%s/%s", up, down), *ud_s = tui_fgs(NULL, TUI_TC_DIM, ud);
    char *nav = tui_fgs(NULL, TUI_TC_MUTED, " navigate");
    char *enter = ui_key_hint("enter"), *enter_s = tui_fgs(NULL, TUI_TC_DIM, enter);
    char *sel = tui_fgs(NULL, TUI_TC_MUTED, " select");
    char *del = ui_key_hint("delete"), *del_s = tui_fgs(NULL, TUI_TC_DIM, del);
    char *dl = tui_fgs(NULL, TUI_TC_MUTED, " delete at end");
    char *cancel = ui_editor_key("tui.select.cancel"), *cancel_s = tui_fgs(NULL, TUI_TC_DIM, cancel);
    char *cn = tui_fgs(NULL, TUI_TC_MUTED, " cancel");
    char *hint = xasprintf("%s%s%s%s%s%s%s%s%s%s%s", ud_s, nav, dot, enter_s, sel, dot, del_s, dl, dot, cancel_s, cn);
    free(del), free(del_s), free(dl);
    ui_push_text(out, hint, width);
    strvec_push(out, "");
    free(dot), free(up), free(down), free(ud), free(ud_s), free(nav), free(enter), free(enter_s), free(sel);
    free(cancel), free(cancel_s), free(cn), free(hint);
}

static void hs_pick(hs_t *h) {
    if (h->sel < 0 || h->sel >= (int)h->items.n) return;
    char *prompt = xstrdup(h->items.v[h->sel].prompt);
    ui_t *u = h->u;
    tui_editor_t *target = h->target;
    ui_close_selector(u); /* destroys h */
    tui_editor_set_text(target, prompt);
    free(prompt);
    tui_request_render(u->tui);
}

/* HistoryStorage.delete (omp 2be5bed17f): the stored prompt goes, every submission of it (the jsonl's lines whose
 * normalized text is that prompt); the results refresh, the selection staying at its row */
static void hs_delete(hs_t *h) {
    if (h->sel < 0 || h->sel >= (int)h->items.n) return;
    char *gone = xstrdup(h->items.v[h->sel].prompt);
    int sel = h->sel;
    char *path = omc_agent_path("prompt-history.jsonl");
    size_t n = 0;
    char *raw = fs_read(path, &n);
    if (raw) {
        sb_t keep = SB_INIT;
        for (const char *ln = raw; *ln;) {
            const char *e = strchr(ln, '\n');
            size_t len = e ? (size_t)(e - ln) : strlen(ln);
            char *line = xstrndup(ln, len);
            json_t *j = json_parse_str(line);
            const char *t = json_get_str(j, "text");
            char *np = t ? normalize_prompt(t) : NULL;
            bool drop = np && !strcmp(np, gone);
            free(np);
            json_free(j);
            if (!drop && len) {
                sb_append(&keep, line);
                sb_putc(&keep, '\n');
            }
            free(line);
            if (!e) break;
            ln = e + 1;
        }
        if (fs_write_atomic(path, keep.s ? keep.s : "", keep.len, 0600) != 0) LOGW("history: could not rewrite %s", path);
        free(h->jsonl);
        h->jsonl = sb_detach(&keep);
        free(raw);
    }
    free(path);
    free(gone);
    hs_update(h);
    if (h->items.n) h->sel = MIN(sel, (int)h->items.n - 1);
}

static void hs_move(hs_t *h, int delta) {
    int n = (int)h->items.n;
    if (!n) return;
    h->sel = MAX(0, MIN(n - 1, h->sel + delta));
}

static bool hs_key(tui_component_t *c, const tui_key_t *k) {
    hs_t *h = (hs_t *)c;
    if (tui_kb(k, "tui.select.up")) hs_move(h, -1);
    else if (tui_kb(k, "tui.select.down")) hs_move(h, 1);
    else if (tui_kb(k, "tui.select.pageUp")) hs_move(h, -MAX_VISIBLE);
    else if (tui_kb(k, "tui.select.pageDown")) hs_move(h, MAX_VISIBLE);
    else if (tui_key_matches(k, "home")) h->sel = h->items.n ? 0 : -1;
    else if (tui_key_matches(k, "end")) h->sel = (int)h->items.n - 1;
    else if (tui_key_matches(k, "enter") || tui_key_matches(k, "return") ||
             (k->kind == TUI_KEY_CHAR && k->codepoint == '\n' && !k->mods)) {
        hs_pick(h);
        return true;
    } else if (tui_kb(k, "app.interrupt") || tui_kb(k, "tui.select.cancel")) {
        ui_close_selector(h->u);
        return true;
    } else if (tui_kb(k, "tui.input.submit")) {
        hs_pick(h);
        return true;
    } else if (tui_key_matches(k, "delete") && h->input.cur == h->input.v.len) {
        hs_delete(h); /* Delete at the end of the query removes the selected prompt */
    } else {
        ui_input_key(&h->input, k);
        hs_schedule(h);
    }
    tui_component_request_render(c);
    return true;
}

static void hs_destroy(tui_component_t *c) {
    hs_t *h = (hs_t *)c;
    ui_input_free(&h->input);
    ui_hist_list_free(&h->items);
    strvec_free(&h->tokens);
    free(h->results_query);
    free(h->jsonl);
    free(h);
}

static const tui_component_vt HS_VT = {"history-search", hs_render, hs_key, NULL, hs_destroy};

void ui_history_search(ui_t *u) {
    /* Ctrl+R also opens over hook/ask prompts that replace the composer in the editor slot; the pick belongs
     * to that prompt, not the hidden composer */
    tui_component_t *top = ui_selector_top(u);
    tui_editor_t *prompt = ui_hook_editor_of(top);
    if (!prompt) prompt = ui_ask_prompt_editor(top);
    hs_t *h = xcalloc(1, sizeof *h);
    tui_component_init(&h->base, &HS_VT);
    h->u = u;
    h->target = prompt ? prompt : u->editor;
    char *p = omc_agent_path("prompt-history.jsonl");
    size_t n;
    h->jsonl = fs_read(p, &n);
    free(p);
    hs_update(h);
    ui_panel_t *panel = ui_panel_new("History");
    tui_container_add(&panel->c, &h->base);
    if (top) ui_show_selector_over(u, &panel->c.base, &h->base);
    else ui_show_selector(u, &panel->c.base, &h->base);
}
