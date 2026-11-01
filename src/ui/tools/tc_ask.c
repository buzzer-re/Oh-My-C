/* tc_ask.c - ask card (port of omp tools/ask.ts: pending question form, answered options, multi-part
 * sections, chat redirect). */
#include "ui/tools/tc.h"

#include "tui/ansi.h"
#include "tui/markdown.h"

static char *no_cr(const char *s) {
    if (!s) return xstrdup("");
    sb_t b = SB_INIT;
    for (const char *p = s; *p; p++) {
        if (*p == '\r') {
            if (p[1] == '\n') continue;
            while (p[1] == '\r') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    return sb_detach(&b);
}

/* Markdown(text, 1, 0, mdTheme, accent) rendered at contentWidth + 1 */
static void ask_md(const char *text, int width, strvec_t *out) {
    tui_md_opts_t mo;
    tui_md_opts_default(&mo);
    mo.padding_x = 1;
    mo.text_slot = TUI_TC_ACCENT;
    char *t = no_cr(text);
    tui_md_render(t, MAX(1, tc_block_content_width(width, -1, -1) + 1), &mo, out);
    free(t);
}

static char *inline_md(const char *text, tui_tc_t base) {
    tui_md_opts_t mo;
    tui_md_opts_default(&mo);
    mo.text_slot = (int)base;
    char *t = no_cr(text);
    char *r = tui_md_inline(t, &mo);
    free(t);
    /* renderInlineMarkdown wraps every text run in the base color; close it like theme.fg does */
    char *x = xasprintf("%s%s\x1b[39m", tui_fg_seq(NULL, base), r);
    free(r);
    return x;
}

/* wrapPrefixedLine */
void tc_wrap_prefixed(const char *prefix, const char *cont, const char *content, int width, strvec_t *out) {
    int pw = tui_visible_width(prefix), cwd = tui_visible_width(cont);
    if (width <= MAX(pw, cwd)) {
        char *j = xasprintf("%s%s", prefix, content);
        tc_push_wrapped(out, j, width);
        free(j);
        return;
    }
    strvec_t rows = {0};
    tui_wrap_text_with_ansi(content, width - MAX(pw, cwd), &rows);
    for (size_t i = 0; i < rows.n; i++) strvec_push_own(out, xasprintf("%s%s", i ? cont : prefix, rows.v[i]));
    strvec_free(&rows);
}

static const char *marker(bool multi, bool selected) {
    return tc_sym(multi ? (selected ? "checkbox.checked" : "checkbox.unchecked") : (selected ? "radio.selected" : "radio.unselected"));
}

static void wrap_option(const char *mk, const char *label, int width, strvec_t *out) {
    char *prefix = xasprintf(" %s ", mk);
    int pw = tui_visible_width(prefix);
    char *cont = xasprintf("%*s", pw, "");
    tc_wrap_prefixed(prefix, cont, label, width, out);
    free(cont);
    free(prefix);
}

static const char *opt_label(const json_t *o) { return json_is_str(o) ? json_sv(o) : json_get_str(o, "label"); }

static void question_options(const json_t *options, bool multi, int width, strvec_t *out) {
    json_t *o;
    json_arr_foreach(options, i, o) {
        const char *label = opt_label(o);
        if (!label) continue;
        char *ol = inline_md(label, TUI_TC_MUTED);
        char *mk = tc_fg(TUI_TC_DIM, marker(multi, false));
        wrap_option(mk, ol, width, out);
        free(mk);
        free(ol);
        const char *desc = json_is_obj(o) ? json_get_str(o, "description") : NULL;
        char *dt = desc ? str_trim(desc) : NULL;
        if (dt && *dt) {
            char *d = inline_md(dt, TUI_TC_DIM);
            char *arrow = tc_fg(TUI_TC_DIM, "\xe2\x86\xb3");
            char *prefix = xasprintf("   %s ", arrow);
            char *cont = xasprintf("%*s", tui_visible_width(prefix), "");
            tc_wrap_prefixed(prefix, cont, d, width, out);
            free(cont);
            free(prefix);
            free(arrow);
            free(d);
        }
        free(dt);
    }
}

static size_t count_options(const json_t *options) {
    size_t n = 0;
    json_t *o;
    json_arr_foreach(options, i, o) if (opt_label(o)) n++;
    return n;
}

static bool str_in(const json_t *arr, const char *s) {
    json_t *e;
    json_arr_foreach(arr, i, e) if (json_is_str(e) && str_eq(json_sv(e), s)) return true;
    return false;
}

static void answer_options(const json_t *options, const json_t *selected, bool multi, const char *custom, const char *note,
                           int width, int cw, strvec_t *out) {
    size_t nsel = json_len(selected);
    const json_t *list = options && json_len(options) ? options : selected;
    if (nsel == 0 && !custom && !note) {
        char *ic = tc_styled_sym("status.warning", TUI_TC_WARNING), *t = tc_fg(TUI_TC_WARNING, "Cancelled");
        strvec_push_own(out, xasprintf(" %s %s", ic, t));
        free(t);
        free(ic);
        return;
    }
    json_t *o;
    json_arr_foreach(list, i, o) {
        if (!json_is_str(o)) continue;
        bool sel = str_in(selected, json_sv(o));
        char *mk = tc_fg(sel ? TUI_TC_SUCCESS : TUI_TC_DIM, marker(multi, sel));
        char *lab = inline_md(json_sv(o), sel ? TUI_TC_TOOL_OUTPUT : TUI_TC_MUTED);
        wrap_option(mk, lab, cw, out);
        free(lab);
        free(mk);
    }
    if (custom) {
        char *c = no_cr(custom);
        strvec_t lines = {0};
        tc_split_lines(c, &lines);
        for (size_t i = 0; i < lines.n; i++) {
            char *t = tc_fg(TUI_TC_TOOL_OUTPUT, lines.v[i]);
            if (i == 0) {
                char *ic = tc_styled_sym("status.success", TUI_TC_SUCCESS);
                strvec_push_own(out, xasprintf(" %s %s", ic, t));
                free(ic);
            } else strvec_push_own(out, xasprintf("   %s", t));
            free(t);
        }
        strvec_free(&lines);
        free(c);
    }
    if (note) {
        char *n0 = no_cr(note);
        char *n = tc_replace_tabs(n0);
        strvec_t lines = {0};
        tc_split_lines(n, &lines);
        for (size_t i = 0; i < lines.n; i++) {
            int mw = MAX(1, width - 7);
            char *tr = tui_truncate_to_width(lines.v[i], mw, NULL);
            char *t = tc_fg(TUI_TC_TOOL_OUTPUT, tr);
            if (i == 0) {
                char *p = tc_fg(TUI_TC_DIM, " Note:");
                strvec_push_own(out, xasprintf("%s %s", p, t));
                free(p);
            } else strvec_push_own(out, xasprintf("       %s", t));
            free(t);
            free(tr);
        }
        strvec_free(&lines);
        free(n);
        free(n0);
    }
}

static void ask_call(const tc_call_t *c, int width, strvec_t *out) {
    int cw = tc_block_content_width(width, -1, -1);
    char *label = tc_fg(TUI_TC_TOOL_TITLE, "\x1b[1mAsk\x1b[22m");
    const json_t *qs = json_get(c->args, "questions");
    json_t *parsed = NULL;
    if (json_is_str(qs)) qs = parsed = json_parse_str(json_sv(qs));
    if (json_is_arr(qs) && json_len(qs)) {
        size_t nq = 0;
        json_t *q;
        json_arr_foreach(qs, i, q) if (json_is_obj(q)) nq++;
        char *cnt = xasprintf("%zu questions", nq);
        char *cm = tc_fg(TUI_TC_MUTED, cnt);
        char *header = xasprintf("%s %s", label, cm);
        tc_section_t *secs = xcalloc(nq ? nq : 1, sizeof *secs);
        size_t k = 0;
        json_arr_foreach(qs, i, q) {
            if (!json_is_obj(q)) continue;
            const json_t *opts = json_get_arr(q, "options");
            bool multi = json_get_bool(q, "multi", false);
            size_t no = count_options(opts);
            strvec_t meta = {0};
            if (multi) strvec_push(&meta, "multi");
            if (no) strvec_push_own(&meta, xasprintf("options:%zu", no));
            char *mj = strvec_join(&meta, " \xc2\xb7 ");
            char *ms = meta.n ? xasprintf(" \xc2\xb7 %s", mj) : xstrdup("");
            char *msd = meta.n ? tc_fg(TUI_TC_DIM, ms) : xstrdup("");
            char *id0 = no_cr(json_get_str(q, "id") ? json_get_str(q, "id") : "?");
            char *idb = xasprintf("[%s]", id0);
            char *idd = tc_fg(TUI_TC_DIM, idb);
            secs[k].label = xasprintf("%s%s", idd, msd);
            ask_md(json_get_str(q, "question") ? json_get_str(q, "question") : "", width, &secs[k].lines);
            if (no) question_options(opts, multi, cw, &secs[k].lines);
            free(idd);
            free(idb);
            free(id0);
            free(msd);
            free(ms);
            free(mj);
            strvec_free(&meta);
            k++;
        }
        tc_block_t blk = {.header = header, .state = TC_STATE_PENDING, .sections = secs, .nsections = k, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        for (size_t i = 0; i < k; i++) {
            free((char *)secs[i].label);
            strvec_free(&secs[i].lines);
        }
        free(secs);
        free(header);
        free(cm);
        free(cnt);
        free(label);
        json_free(parsed);
        return;
    }
    json_free(parsed);
    const char *question = json_get_str(c->args, "question");
    if (!question || !*question) {
        char *e = tc_error_message("No question provided");
        tc_block_t blk = {.header = e, .state = TC_STATE_ERROR, .width = width, .pad_left = -1, .pad_right = -1,
                          .border_slot = TUI_TC_ERROR};
        tc_output_block(&blk, out);
        free(e);
        free(label);
        return;
    }
    const json_t *opts = json_get_arr(c->args, "options");
    bool multi = json_get_bool(c->args, "multi", false);
    size_t no = count_options(opts);
    strvec_t meta = {0};
    if (multi) strvec_push(&meta, "multi");
    if (no) strvec_push_own(&meta, xasprintf("options:%zu", no));
    char *header;
    if (meta.n) {
        char *mj = strvec_join(&meta, tc_sym("sep.dot"));
        char *mm = tc_fg(TUI_TC_MUTED, mj);
        header = xasprintf("%s %s", label, mm);
        free(mm);
        free(mj);
    } else header = xstrdup(label);
    tc_section_t sec = {0};
    ask_md(question, width, &sec.lines);
    if (no) question_options(opts, multi, cw, &sec.lines);
    tc_block_t blk = {.header = header, .state = TC_STATE_PENDING, .sections = &sec, .nsections = sec.lines.n ? 1 : 0,
                      .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    strvec_free(&meta);
    free(header);
    free(label);
}

static bool has_selection(const json_t *r) {
    return json_get(r, "customInput") || json_get(r, "note") || json_len(json_get_arr(r, "selectedOptions")) > 0;
}

static void ask_result(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    int cw = tc_block_content_width(width, -1, -1);
    if (!d) {
        char *t0 = tc_text_of(c->content);
        char *t = no_cr(t0);
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Ask", .title_slot = -1};
        char *h = tc_status_line(&sl);
        char *td = *t ? tc_fg(TUI_TC_DIM, t) : NULL;
        char *all = td ? xasprintf("%s\n%s", h, td) : xstrdup(h);
        tc_push_wrapped(out, all, width);
        free(all);
        free(td);
        free(h);
        free(t);
        free(t0);
        return;
    }
    if (json_get_bool(d, "chatRedirect", false)) {
        const char *m[] = {"chat redirect"};
        tc_status_line_t sl = {.icon = TC_ST_INFO, .spinner = -1, .title = "Ask", .title_slot = -1, .meta = m, .nmeta = 1};
        char *h = tc_status_line(&sl);
        tc_section_t sec = {0};
        json_t *q;
        json_arr_foreach(json_get_arr(d, "questions"), i, q) if (json_is_str(q)) ask_md(json_sv(q), width, &sec.lines);
        tc_block_t blk = {.header = h, .state = TC_STATE_WARNING, .sections = &sec, .nsections = json_len(json_get_arr(d, "questions")) ? 1 : 0,
                          .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(h);
        return;
    }
    const json_t *results = json_get_arr(d, "results");
    if (results && json_len(results)) {
        size_t n = json_len(results);
        bool any = false;
        for (size_t i = 0; i < n; i++) any |= has_selection(json_at(results, i));
        char *cnt = xasprintf("%zu questions", n);
        const char *m[] = {cnt};
        tc_status_line_t sl = {.icon = any ? TC_ST_SUCCESS : TC_ST_WARNING, .spinner = -1, .title = "Ask", .title_slot = -1,
                               .meta = m, .nmeta = 1};
        char *h = tc_status_line(&sl);
        tc_section_t *secs = xcalloc(n, sizeof *secs);
        for (size_t i = 0; i < n; i++) {
            const json_t *r = json_at(results, i);
            char *id0 = no_cr(json_get_str(r, "id") ? json_get_str(r, "id") : "");
            char *idb = xasprintf("[%s]", id0);
            secs[i].label = tc_fg(TUI_TC_DIM, idb);
            ask_md(json_get_str(r, "question") ? json_get_str(r, "question") : "", width, &secs[i].lines);
            answer_options(json_get_arr(r, "options"), json_get_arr(r, "selectedOptions"), json_get_bool(r, "multi", false),
                           json_get_str(r, "customInput"), json_get_str(r, "note"), width, cw, &secs[i].lines);
            free(idb);
            free(id0);
        }
        tc_block_t blk = {.header = h, .state = any ? TC_STATE_SUCCESS : TC_STATE_WARNING, .sections = secs, .nsections = n,
                          .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        for (size_t i = 0; i < n; i++) {
            free((char *)secs[i].label);
            strvec_free(&secs[i].lines);
        }
        free(secs);
        free(h);
        free(cnt);
        return;
    }
    const char *question = json_get_str(d, "question");
    if (!question || !*question) {
        char *t0 = tc_text_of(c->content);
        char *t = no_cr(t0);
        tc_push_wrapped(out, t, width);
        free(t);
        free(t0);
        return;
    }
    bool sel = has_selection(d);
    char *ov = sel ? tc_styled_sym("tool.ask", TUI_TC_ACCENT) : NULL;
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ov, .spinner = -1, .title = "Ask", .title_slot = -1};
    char *h = tc_status_line(&sl);
    tc_section_t sec = {0};
    ask_md(question, width, &sec.lines);
    answer_options(json_get_arr(d, "options"), json_get_arr(d, "selectedOptions"), json_get_bool(d, "multi", false),
                   json_get_str(d, "customInput"), json_get_str(d, "note"), width, cw, &sec.lines);
    if (json_get_bool(d, "timedOut", false))
        strvec_push_own(&sec.lines, tc_fg(TUI_TC_DIM, "auto-selected after timeout \xe2\x80\x94 not a user choice"));
    tc_block_t blk = {.header = h, .state = sel ? TC_STATE_SUCCESS : TC_STATE_WARNING, .sections = &sec, .nsections = sec.lines.n ? 1 : 0,
                      .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    free(h);
    free(ov);
}

void tc_ask(const tc_call_t *c, int width, strvec_t *out) {
    if (c->has_result) ask_result(c, width, out);
    else ask_call(c, width, out);
}
