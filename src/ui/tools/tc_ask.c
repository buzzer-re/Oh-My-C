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
        char *ic = tc_status_icon(TC_ST_WARNING, -1), *t = tc_fg(TUI_TC_WARNING, "Cancelled");
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
        /* continuation rows line up under the answer text, past the glyph's own width */
        char *ic = tc_status_icon(TC_ST_SUCCESS, -1);
        int indent = tui_visible_width(ic) + 2;
        for (size_t i = 0; i < lines.n; i++) {
            char *t = tc_fg(TUI_TC_TOOL_OUTPUT, lines.v[i]);
            if (i == 0) strvec_push_own(out, xasprintf(" %s %s", ic, t));
            else strvec_push_own(out, xasprintf("%*s%s", indent, "", t));
            free(t);
        }
        free(ic);
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

/* questionFlags: `multi` and the option count; joined dim for a footer */
static void question_flags(bool multi, size_t no, strvec_t *flags) {
    if (multi) strvec_push(flags, "multi");
    if (no) strvec_push_own(flags, tc_format_count("option", (int64_t)no));
}

/* formatQuestionFlags: dim `multi · 3 options` footer, NULL when there are none */
static char *format_question_flags(bool multi, size_t no) {
    strvec_t flags = {0};
    question_flags(multi, no, &flags);
    char *r = NULL;
    if (flags.n) {
        char *j = strvec_join(&flags, tc_sym("sep.dot"));
        r = tc_fg(TUI_TC_DIM, j);
        free(j);
    }
    strvec_free(&flags);
    return r;
}

/* ids are model-supplied: a newline or tab would split or hole the row */
static char *one_row_id(const char *id) {
    char *clean = no_cr(id ? id : "");
    sb_t b = SB_INIT;
    for (const char *p = clean; *p; p++) {
        if (*p == '\n') {
            while (p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    char *r = tc_replace_tabs(sb_cstr(&b));
    sb_free(&b);
    free(clean);
    return r;
}

/* questionTag + appendQuestionTag: a question's dim `(id · multi · 3 options)` tag continues the last row of a question that
 * ends in a plain paragraph when it fits, else takes a row of its own (a tag wider than the row wraps under its one-column
 * indent) */
static void append_question_tag(const char *question, const char *id, const strvec_t *flags, int cw, strvec_t *lines) {
    char *label = one_row_id(id);
    strvec_t parts = {0};
    strvec_push(&parts, label);
    for (size_t i = 0; i < flags->n; i++) strvec_push(&parts, flags->v[i]);
    char *j = strvec_join(&parts, tc_sym("sep.dot"));
    char *t0 = xasprintf("(%s)", j);
    char *tag = tc_fg(TUI_TC_DIM, t0);
    char *q = no_cr(question);
    bool after_text = tui_md_ends_in_paragraph(q);
    /* Markdown pads each row to the full width; the tag follows the text, not the padding */
    char *last = lines->n ? tc_trim_end(lines->v[lines->n - 1]) : xstrdup("");
    char *plain = tui_strip_ansi(last);
    bool blank = strspn(plain, " \t") == strlen(plain);
    bool fits = !blank && tui_visible_width(last) + 1 + tui_visible_width(tag) <= cw;
    if (after_text && fits) {
        free(lines->v[lines->n - 1]);
        lines->v[lines->n - 1] = xasprintf("%s %s", last, tag);
    } else {
        tc_wrap_prefixed(" ", " ", tag, cw, lines);
    }
    free(plain);
    free(last);
    free(q);
    free(tag);
    free(t0);
    free(j);
    strvec_free(&parts);
    free(label);
}

/* askSubject: the lone question's id on one row, or the question count */
static char *ask_subject(const json_t *questions) {
    size_t n = 0;
    const json_t *lone = NULL;
    json_t *q;
    json_arr_foreach(questions, i, q) if (json_is_obj(q)) {
        if (!n) lone = q;
        n++;
    }
    if (n == 1) return one_row_id(json_get_str(lone, "id") ? json_get_str(lone, "id") : "?");
    return tc_format_count("question", (int64_t)n);
}

/* normalizeRenderQuestions of the call's arguments, NULL when they hold none: `args.questions` may be a JSON string */
static char *arg_subject(const json_t *args) {
    const json_t *qs = json_get(args, "questions");
    json_t *parsed = NULL;
    if (json_is_str(qs)) qs = parsed = json_parse_str(json_sv(qs));
    char *r = NULL;
    if (json_is_arr(qs)) {
        size_t n = 0;
        json_t *q;
        json_arr_foreach(qs, i, q) if (json_is_obj(q)) n++;
        if (n) r = ask_subject(qs);
    }
    json_free(parsed);
    return r;
}

static void ask_call(const tc_call_t *c, int width, strvec_t *out) {
    int cw = tc_block_content_width(width, -1, -1);
    char *label = tc_fg(TUI_TC_TOOL_TITLE, "\x1b[1mAsk\x1b[22m");
    const json_t *qs = json_get(c->args, "questions");
    json_t *parsed = NULL;
    if (json_is_str(qs)) qs = parsed = json_parse_str(json_sv(qs));
    if (json_is_arr(qs) && json_len(qs)) {
        size_t nq = 0;
        const json_t *lone = NULL;
        json_t *q;
        json_arr_foreach(qs, i, q) if (json_is_obj(q)) {
            if (!nq) lone = q;
            nq++;
        }
        char *cnt = ask_subject(qs);
        char *cm = tc_fg(TUI_TC_MUTED, cnt);
        char *header = xasprintf("%s: %s", label, cm);
        /* one question: its id is the header subject and its flags the footer; several: each question is tagged with its
         * id and flags, divided by plain separators */
        char *footer = nq == 1 ? format_question_flags(json_get_bool(lone, "multi", false), count_options(json_get_arr(lone, "options"))) : NULL;
        tc_section_t *secs = xcalloc(nq ? nq : 1, sizeof *secs);
        size_t k = 0;
        json_arr_foreach(qs, i, q) {
            if (!json_is_obj(q)) continue;
            const json_t *opts = json_get_arr(q, "options");
            bool multi = json_get_bool(q, "multi", false);
            size_t no = count_options(opts);
            const char *qt = json_get_str(q, "question") ? json_get_str(q, "question") : "";
            ask_md(qt, width, &secs[k].lines);
            if (nq > 1) {
                strvec_t flags = {0};
                question_flags(multi, no, &flags);
                append_question_tag(qt, json_get_str(q, "id") ? json_get_str(q, "id") : "?", &flags, cw, &secs[k].lines);
                strvec_free(&flags);
            }
            if (no) question_options(opts, multi, cw, &secs[k].lines);
            secs[k].separator = k > 0;
            k++;
        }
        tc_block_t blk = {.header = header, .footer_meta = footer, .state = TC_STATE_PENDING, .sections = secs, .nsections = k, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        for (size_t i = 0; i < k; i++) strvec_free(&secs[i].lines);
        free(footer);
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
    char *footer = format_question_flags(multi, no);
    char *header = xstrdup(label);
    tc_section_t sec = {0};
    ask_md(question, width, &sec.lines);
    if (no) question_options(opts, multi, cw, &sec.lines);
    tc_block_t blk = {.header = header, .footer_meta = footer, .state = TC_STATE_PENDING, .sections = &sec, .nsections = sec.lines.n ? 1 : 0,
                      .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    free(footer);
    free(header);
    free(label);
}

static bool has_selection(const json_t *r) {
    return json_get(r, "customInput") || json_get(r, "note") || json_len(json_get_arr(r, "selectedOptions")) > 0;
}

static void ask_result(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    int cw = tc_block_content_width(width, -1, -1);
    char *subject = arg_subject(c->args);
    if (!d) {
        char *t0 = tc_text_of(c->content);
        char *t = no_cr(t0);
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Ask", .title_slot = -1, .description = subject};
        char *h = tc_status_line(&sl);
        char *td = *t ? tc_fg(TUI_TC_DIM, t) : NULL;
        char *all = td ? xasprintf("%s\n%s", h, td) : xstrdup(h);
        tc_push_wrapped(out, all, width);
        free(all);
        free(td);
        free(h);
        free(t);
        free(t0);
        free(subject);
        return;
    }
    if (json_get_bool(d, "chatRedirect", false)) {
        const char *m[] = {"chat redirect"};
        tc_status_line_t sl = {.icon = TC_ST_INFO, .spinner = -1, .title = "Ask", .title_slot = -1, .description = subject, .meta = m, .nmeta = 1};
        char *h = tc_status_line(&sl);
        tc_section_t sec = {0};
        json_t *q;
        json_arr_foreach(json_get_arr(d, "questions"), i, q) if (json_is_str(q)) ask_md(json_sv(q), width, &sec.lines);
        tc_block_t blk = {.header = h, .state = TC_STATE_WARNING, .sections = &sec, .nsections = json_len(json_get_arr(d, "questions")) ? 1 : 0,
                          .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(h);
        free(subject);
        return;
    }
    const json_t *results = json_get_arr(d, "results");
    if (results && json_len(results)) {
        size_t n = json_len(results);
        bool any = false;
        for (size_t i = 0; i < n; i++) any |= has_selection(json_at(results, i));
        char *cnt = ask_subject(results);
        tc_status_line_t sl = {.icon = any ? TC_ST_SUCCESS : TC_ST_WARNING, .spinner = -1, .title = "Ask", .title_slot = -1,
                               .description = cnt};
        char *h = tc_status_line(&sl);
        tc_section_t *secs = xcalloc(n, sizeof *secs);
        for (size_t i = 0; i < n; i++) {
            const json_t *r = json_at(results, i);
            const char *qt = json_get_str(r, "question") ? json_get_str(r, "question") : "";
            ask_md(qt, width, &secs[i].lines);
            if (n > 1) {
                strvec_t flags = {0};
                if (json_get_bool(r, "multi", false)) strvec_push(&flags, "multi");
                append_question_tag(qt, json_get_str(r, "id") ? json_get_str(r, "id") : "", &flags, cw, &secs[i].lines);
                strvec_free(&flags);
            }
            answer_options(json_get_arr(r, "options"), json_get_arr(r, "selectedOptions"), json_get_bool(r, "multi", false),
                           json_get_str(r, "customInput"), json_get_str(r, "note"), width, cw, &secs[i].lines);
            secs[i].separator = i > 0;
        }
        tc_block_t blk = {.header = h, .state = any ? TC_STATE_SUCCESS : TC_STATE_WARNING, .sections = secs, .nsections = n,
                          .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        for (size_t i = 0; i < n; i++) strvec_free(&secs[i].lines);
        free(secs);
        free(h);
        free(cnt);
        free(subject);
        return;
    }
    const char *question = json_get_str(d, "question");
    if (!question || !*question) {
        char *t0 = tc_text_of(c->content);
        char *t = no_cr(t0);
        tc_push_wrapped(out, t, width);
        free(t);
        free(t0);
        free(subject);
        return;
    }
    bool sel = has_selection(d);
    char *ov = sel ? tc_styled_sym("tool.ask", TUI_TC_ACCENT) : NULL;
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ov, .spinner = -1, .title = "Ask", .title_slot = -1, .description = subject};
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
    free(subject);
}

void tc_ask(const tc_call_t *c, int width, strvec_t *out) {
    if (c->has_result) ask_result(c, width, out);
    else ask_call(c, width, out);
}
