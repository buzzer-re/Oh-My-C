/* tc_find.c - find (semantic grep) card (port of omp tools/find.ts). */
#include "ui/tools/tc.h"

#include <math.h>

#include "tui/ansi.h"
#include "tui/markdown.h"

static tui_tc_t score_color(double p) { return p >= 0.7 ? TUI_TC_SUCCESS : p >= 0.4 ? TUI_TC_WARNING : TUI_TC_MUTED; }

/* renderProgressBar(value, width) with filled/empty glyphs */
char *tc_progress_bar(double frac, int width, tui_tc_t filled_slot, tui_tc_t empty_slot) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int f = (int)floor(frac * width + 0.5);
    sb_t a = SB_INIT, e = SB_INIT;
    for (int i = 0; i < f; i++) sb_append(&a, tc_sym("progress.filled"));
    for (int i = f; i < width; i++) sb_append(&e, tc_sym("progress.empty"));
    char *fa = tc_fg(filled_slot, sb_cstr(&a)), *ea = tc_fg(empty_slot, sb_cstr(&e));
    char *r = xasprintf("%s%s", fa, ea);
    free(fa);
    free(ea);
    sb_free(&a);
    sb_free(&e);
    return r;
}

static char *quote_query(const char *q) { return q ? xasprintf("\"%s\"", q) : NULL; }

static int cmp_range(const void *a, const void *b) {
    const json_t *x = *(const json_t *const *)a, *y = *(const json_t *const *)b;
    double px = json_get_num(x, "p", 0), py = json_get_num(y, "p", 0);
    if (px != py) return px < py ? 1 : -1;
    double sx = json_get_num(x, "start", 0), sy = json_get_num(y, "start", 0);
    return sx < sy ? -1 : sx > sy;
}

static char *render_hit(const json_t *hit, int range_limit) {
    double score = json_get_num(hit, "contentScore", 0);
    const char *rel = json_get_str(hit, "rel") ? json_get_str(hit, "rel") : "";
    char *g = tc_progress_bar(score, 6, score_color(score), TUI_TC_DIM);
    char sc[32];
    snprintf(sc, sizeof sc, "%.2f", score);
    char *scs = tc_fg(score_color(score), sc);
    /* URL hits show whole in the file-name color; file hits get the dim directory */
    char *rl = tc_is_url_like(rel) ? tc_fg(TC_FILE_NAME_COLOR, rel) : tc_styled_path(rel);
    char *cov = json_get_bool(hit, "truncated", false)
                    ? xasprintf("%lld lines judged, partial", (long long)json_get_int(hit, "linesSeen", 0))
                    : xasprintf("%lld lines judged", (long long)json_get_int(hit, "linesSeen", 0));
    char *covd = tc_fg(TUI_TC_DIM, cov);
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s %s %s %s", g, scs, rl, covd);
    const json_t *ranges = json_get_arr(hit, "ranges");
    size_t nr = json_len(ranges);
    const json_t **rv = xcalloc(nr ? nr : 1, sizeof *rv);
    for (size_t i = 0; i < nr; i++) rv[i] = json_at(ranges, i);
    xqsort(rv, nr, sizeof *rv, cmp_range);
    size_t shown = MIN(nr, (size_t)range_limit);
    const char *lang = tc_lang_from_path(rel);
    for (size_t i = 0; i < shown; i++) {
        int64_t s = json_get_int(rv[i], "start", 0), e = json_get_int(rv[i], "end", 0);
        char *span = s == e ? xasprintf(":%lld", (long long)s) : xasprintf(":%lld-%lld", (long long)s, (long long)e);
        char *ref = tc_fg(TUI_TC_MUTED, span);
        char *sn0 = str_trim(json_get_str(rv[i], "snippet") ? json_get_str(rv[i], "snippet") : "");
        char *src = tc_replace_tabs(sn0);
        char *snip;
        if (lang && strcmp(lang, "text")) { /* omp isHighlightLanguage: grammars omc lacks come back plain, like syntect plain text */
            strvec_t hl = {0};
            tc_highlight(src, lang, &hl);
            snip = strvec_join(&hl, "\n");
            strvec_free(&hl);
        } else snip = tc_fg(TUI_TC_TOOL_OUTPUT, src);
        char *br = tc_fg(TUI_TC_DIM, tc_sym(i + 1 == shown ? "tree.last" : "tree.branch"));
        char pp[32];
        snprintf(pp, sizeof pp, "%.2f", json_get_num(rv[i], "p", 0));
        char *ppd = tc_fg(TUI_TC_DIM, pp);
        sb_appendf(&b, "\n  %s %s %s %s", br, ref, ppd, snip);
        free(ppd);
        free(br);
        free(snip);
        free(src);
        free(sn0);
        free(ref);
        free(span);
    }
    free(rv);
    free(covd);
    free(cov);
    free(rl);
    free(scs);
    free(g);
    return sb_detach(&b);
}

void tc_find(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    char *text = tc_text_of(c->content);
    const char *aq = json_get_str(c->args, "query");
    if (!c->has_result || (!c->is_error && (c->partial || !d))) {
        strvec_t meta = {0};
        if (!c->has_result) {
            const json_t *kw = json_get_arr(c->args, "grep_keywords");
            if (json_len(kw)) {
                strvec_t k = {0};
                json_t *e;
                json_arr_foreach(kw, i, e) if (json_is_str(e)) strvec_push(&k, json_sv(e));
                strvec_push_own(&meta, strvec_join(&k, " "));
                strvec_free(&k);
            }
            if (json_get_str(c->args, "path") && *json_get_str(c->args, "path"))
                strvec_push_own(&meta, xasprintf("in %s", json_get_str(c->args, "path")));
        } else if (*text) strvec_push(&meta, text);
        char *q = quote_query(aq ? aq : json_get_str(d, "query"));
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = c->spinner, .title = "Find", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = q, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(q);
        strvec_free(&meta);
        free(text);
        return;
    }
    if (c->is_error) {
        char *e = tc_error_message(*text ? text : "Unknown error");
        tc_push_wrapped(out, e, width);
        free(e);
        free(text);
        return;
    }
    const json_t *hits = json_get_arr(d, "hits"), *stats = json_get_obj(d, "stats");
    size_t nh = json_len(hits);
    char *desc = quote_query(json_get_str(d, "query"));
    const char *scope = json_get_str(d, "scopePath");
    char *scope_s = scope ? xasprintf("in %s", scope) : NULL;
    char cost[48], thr[32];
    snprintf(cost, sizeof cost, "$%.4f", json_get_num(stats, "cost", 0));
    snprintf(thr, sizeof thr, "\xcf\x84 %.2f", json_get_num(d, "threshold", 0));
    char *dur = tc_format_duration(json_get_int(d, "elapsedMs", 0));
    int64_t failed = json_get_int(stats, "failed", 0);
    char *failed_s = NULL;
    if (failed > 0) {
        char *f = xasprintf("%lld/%lld requests failed", (long long)failed, (long long)json_get_int(stats, "requests", 0));
        failed_s = tc_fg(TUI_TC_WARNING, f);
        free(f);
    }
    /* details.timeout: the call's deadline cut verification short */
    char *timeout_s = NULL;
    const json_t *tmo = json_get_obj(d, "timeout");
    if (tmo) {
        char *f = xasprintf("timed out, %lld/%lld passages verified", (long long)json_get_int(tmo, "verified", 0), (long long)json_get_int(tmo, "passages", 0));
        timeout_s = tc_fg(TUI_TC_WARNING, f);
        free(f);
    }
    if (nh == 0) {
        strvec_t meta = {0};
        strvec_push(&meta, "0 hits");
        if (scope_s) strvec_push(&meta, scope_s);
        strvec_push(&meta, cost);
        strvec_push(&meta, dur);
        if (failed_s) strvec_push(&meta, failed_s);
        if (timeout_s) strvec_push(&meta, timeout_s);
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Find", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = desc, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        char *em = tc_empty_message(str_eq(json_get_str(d, "coverage"), "partial") ? "No relevant passages found in the judged files"
                                                                                   : "No relevant passages found");
        sb_t b = SB_INIT;
        sb_appendf(&b, "%s\n%s", h, em);
        json_t *f;
        json_arr_foreach(json_get_arr(stats, "failures"), i, f) if (json_is_str(f)) {
            char *w = tc_fg(TUI_TC_WARNING, json_sv(f));
            sb_appendf(&b, "\n%s", w);
            free(w);
        }
        tc_push_wrapped(out, sb_cstr(&b), width);
        sb_free(&b);
        free(em);
        free(h);
        strvec_free(&meta);
    } else {
        strvec_t meta = {0};
        strvec_push_own(&meta, tc_format_count("hit", (int64_t)nh));
        if (scope_s) strvec_push(&meta, scope_s);
        strvec_push_own(&meta, xasprintf("%lld files read", (long long)json_get_int(stats, "filesRead", 0)));
        strvec_push(&meta, thr);
        char *tok = tc_format_number(json_get_num(stats, "inputTokens", 0));
        strvec_push_own(&meta, xasprintf("%s tokens", tok));
        free(tok);
        strvec_push(&meta, cost);
        strvec_push(&meta, dur);
        if (failed_s) strvec_push(&meta, failed_s);
        if (timeout_s) strvec_push(&meta, timeout_s);
        char *ov = tc_fg(TUI_TC_TOOL_TITLE, tc_sym("icon.search"));
        tc_status_line_t sl = {.icon_override = ov, .spinner = -1, .title = "Find", .title_slot = TUI_TC_TOOL_TITLE,
                               .description = desc, .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        strvec_t items = {0}, rows = {0};
        for (size_t i = 0; i < nh; i++) strvec_push_own(&items, render_hit(json_at(hits, i), c->expanded ? 3 : 1));
        tc_tree_list_ex(&items, c->expanded, 5, c->expanded ? -1 : 12, "hit", &rows);
        if (c->expanded) {
            strvec_t kw = {0};
            json_t *e;
            json_arr_foreach(json_get_arr(d, "keywords"), i, e) if (json_is_str(e)) strvec_push(&kw, json_sv(e));
            char *kj = strvec_join(&kw, ", ");
            char *kl = xasprintf("keywords: %s", kj);
            strvec_push_own(&rows, tc_fg(TUI_TC_DIM, kl));
            free(kl);
            free(kj);
            strvec_free(&kw);
            json_arr_foreach(json_get_arr(stats, "failures"), i, e) if (json_is_str(e)) strvec_push_own(&rows, tc_fg(TUI_TC_WARNING, json_sv(e)));
        }
        tc_fit_headered(h, &rows, width, out);
        strvec_free(&rows);
        strvec_free(&items);
        free(h);
        free(ov);
        strvec_free(&meta);
    }
    free(failed_s);
    free(timeout_s);
    free(dur);
    free(scope_s);
    free(desc);
    free(text);
}
