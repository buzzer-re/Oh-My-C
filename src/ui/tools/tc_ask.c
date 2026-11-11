/* tc_ask.c - ask card (port of omp packages/tui/src/tools/ask.ts: the pending HTML form summarized per fieldset,
 * then each field's question over its submitted answer). */
#include "ui/tools/tc.h"

#include "text/htmlform.h"
#include "tools/outsink.h"
#include "tui/ansi.h"

#define TIMED_OUT_NOTE "auto-selected after timeout \xe2\x80\x94 not a user choice"
#define NO_ANSWER "(no answer)"
#define COLLAPSED_ITEMS 8
#define COLLAPSED_LINES 3
#define EXPANDED_LINES 12
#define OUTPUT_COLLAPSED 3
#define OUTPUT_EXPANDED 10

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

/* clean: model-authored text made safe for a terminal row (no escapes or control characters, tabs expanded) */
static char *clean(const char *s) {
    size_t n = 0;
    char *a = out_sanitize(s ? s : "", strlen(s ? s : ""), &n);
    char *r = tc_replace_tabs(a);
    free(a);
    return r;
}

static char *one_line(const char *s) {
    char *c = clean(s), *r = hf_one_line(c);
    free(c);
    return r;
}

static const char *marker(bool multi, bool selected) {
    return tc_sym(multi ? (selected ? "checkbox.checked" : "checkbox.unchecked") : (selected ? "radio.selected" : "radio.unselected"));
}

static char *spaces(int n) { return xasprintf("%*s", MAX(0, n), ""); }

/* capLines: `lines` capped at `limit`, a dim `… N more lines` row standing in for the rest */
static void cap_lines(strvec_t *lines, size_t limit, const char *indent) {
    if (lines->n <= limit) return;
    size_t more = lines->n - limit;
    while (lines->n > limit) free(lines->v[--lines->n]);
    char *cnt = tc_format_count("more line", (int64_t)more), *t = xasprintf("\xe2\x80\xa6 %s", cnt), *d = tc_fg(TUI_TC_DIM, t);
    strvec_push_own(lines, xasprintf("%s%s", indent, d));
    free(cnt), free(t), free(d);
}

/* the form in untrusted (streamed or model-mangled) call args; NULL until markup arrives */
static hf_form_t *args_form(const json_t *args) {
    const char *html = json_get_str(args, "html");
    if (!html) return NULL;
    char *t = str_trim(html);
    bool blank = !*t;
    free(t);
    return blank ? NULL : hf_parse(html);
}

static void choice_field_lines(const hf_field_t *f, int width, bool expanded, strvec_t *out) {
    size_t limit = expanded ? f->opts.n : MIN(f->opts.n, COLLAPSED_ITEMS);
    for (size_t i = 0; i < limit; i++) {
        const hf_option_t *o = &f->opts.v[i];
        char *mk = tc_fg(o->checked ? TUI_TC_SUCCESS : TUI_TC_DIM, marker(f->multi, o->checked));
        tui_tc_t color = o->disabled ? TUI_TC_DIM : o->checked ? TUI_TC_TOOL_OUTPUT : TUI_TC_MUTED;
        char *prefix = xasprintf(" %s ", mk), *ind = spaces(tui_visible_width(prefix));
        char *lt = hf_spans_text(&o->label), *label = one_line(lt);
        if (!*label) {
            free(label);
            label = one_line(o->value);
        }
        char *styled = tc_fg(color, label);
        tc_wrap_prefixed(prefix, ind, styled, width, out);
        free(mk), free(prefix), free(ind), free(lt), free(label), free(styled);
    }
    size_t hidden = f->opts.n - limit;
    if (hidden > 0) {
        char *cnt = tc_format_count("more option", (int64_t)hidden), *t = xasprintf("\xe2\x80\xa6 %s", cnt), *d = tc_fg(TUI_TC_DIM, t);
        strvec_push_own(out, xasprintf("   %s", d));
        free(cnt), free(t), free(d);
    } else if (f->other) {
        char *t = xasprintf("%s " HF_OTHER_LABEL "\xe2\x80\xa6", marker(f->multi, false)), *d = tc_fg(TUI_TC_DIM, t);
        strvec_push_own(out, xasprintf(" %s", d));
        free(t), free(d);
    }
}

static char *bullets(const char *s, size_t cap) {
    size_t n = MIN(hf_utf16_len(s), cap);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) sb_append(&b, "\xe2\x80\xa2");
    return sb_detach(&b);
}

static void text_field_lines(const hf_field_t *f, int width, bool expanded, strvec_t *out) {
    char *bar = tc_fg(TUI_TC_BORDER, tc_sym("boxRound.vertical")), *prefix = xasprintf("   %s ", bar);
    int inner = MAX(1, width - tui_visible_width(prefix));
    char *shown = str_eq(f->type, "password") ? bullets(f->value, 12) : clean(f->value);
    strvec_t lines = {0};
    if (*shown) {
        strvec_t parts = {0};
        tc_split_lines(shown, &parts);
        for (size_t i = 0; i < parts.n; i++) {
            char *t = tui_truncate_to_width(parts.v[i], inner, NULL), *c = tc_fg(TUI_TC_TOOL_OUTPUT, t);
            strvec_push_own(&lines, xasprintf("%s%s", prefix, c));
            free(t), free(c);
        }
        strvec_free(&parts);
    } else {
        char *p = one_line(f->placeholder), *t = tui_truncate_to_width(p, inner, NULL), *d = tc_fg(TUI_TC_DIM, t);
        strvec_push_own(&lines, xasprintf("%s%s", prefix, d));
        free(p), free(t), free(d);
    }
    cap_lines(&lines, expanded ? EXPANDED_LINES : COLLAPSED_LINES, "   ");
    for (size_t i = 0; i < lines.n; i++) strvec_push(out, lines.v[i]);
    strvec_free(&lines);
    free(shown), free(prefix), free(bar);
}

/* formSections: one card section per fieldset, each field's question over its options or input */
static size_t form_sections(const hf_form_t *form, int width, bool expanded, tc_section_t **out) {
    tc_section_t *secs = xcalloc(form->sections.n + 1, sizeof *secs);
    size_t n = 0;
    for (size_t si = 0; si < form->sections.n; si++) {
        const hf_section_t *sec = &form->sections.v[si];
        tc_section_t *s = &secs[n];
        char *tt = hf_spans_text(&sec->title), *title = one_line(tt);
        free(tt);
        if (*title) {
            char *a = tc_fg(TUI_TC_ACCENT, title), *b = tc_bold(a);
            tc_wrap_prefixed(" ", " ", b, width, &s->lines);
            free(a), free(b);
        }
        for (size_t bi = 0; bi < sec->blocks.n; bi++) {
            const hf_block_t *blk = &sec->blocks.v[bi];
            if (blk->kind != HF_B_FIELD) continue;
            const hf_field_t *f = blk->field;
            char *q = one_line(f->question ? f->question : f->name);
            /* a fieldset's lone group answers its legend; the title already says it */
            if (*q && strcmp(q, title)) {
                char *qt = tc_fg(TUI_TC_TEXT, q), *req = f->required ? tc_fg(TUI_TC_ERROR, " *") : xstrdup("");
                char *c = xasprintf("%s%s", qt, req);
                tc_wrap_prefixed(" ", " ", c, width, &s->lines);
                free(qt), free(req), free(c);
            }
            free(q);
            if (f->kind == HF_CHOICE) choice_field_lines(f, width, expanded, &s->lines);
            else text_field_lines(f, width, expanded, &s->lines);
        }
        free(title);
        if (s->lines.n) {
            s->separator = n > 0;
            n++;
        }
    }
    *out = secs;
    return n;
}

static void ask_call(const tc_call_t *c, int width, strvec_t *out) {
    int cw = tc_block_content_width(width, -1, -1);
    hf_form_t *form = args_form(c->args);
    size_t named = hf_named_count(form);
    char *footer = NULL;
    if (named) {
        char *cnt = tc_format_count("field", (int64_t)named);
        footer = tc_fg(TUI_TC_DIM, cnt);
        free(cnt);
    }
    char *header = tc_fg(TUI_TC_TOOL_TITLE, "\x1b[1mAsk\x1b[22m");
    tc_section_t *secs = NULL;
    size_t n = form ? form_sections(form, cw, c->expanded, &secs) : 0;
    tc_block_t blk = {.header = header, .footer_meta = footer, .state = TC_STATE_PENDING, .sections = secs, .nsections = n,
                      .width = width, .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    if (secs)
        for (size_t i = 0; i <= (form ? form->sections.n : 0); i++) strvec_free(&secs[i].lines);
    free(secs);
    free(header);
    free(footer);
    hf_form_free(form);
}

/* answerLines: a success check before the answer with continuation rows beneath it, or a dim placeholder */
static void answer_lines(const char *shown, int width, bool expanded, strvec_t *out) {
    if (!*shown) {
        char *d = tc_fg(TUI_TC_DIM, NO_ANSWER);
        strvec_push_own(out, xasprintf("   %s", d));
        free(d);
        return;
    }
    char *ic = tc_status_icon(TC_ST_SUCCESS, -1), *prefix = xasprintf(" %s ", ic), *ind = spaces(tui_visible_width(prefix));
    strvec_t rows = {0}, parts = {0};
    tc_split_lines(shown, &parts);
    for (size_t i = 0; i < parts.n; i++) {
        char *t = tc_fg(TUI_TC_TOOL_OUTPUT, parts.v[i]);
        tc_wrap_prefixed(i ? ind : prefix, ind, t, width, &rows);
        free(t);
    }
    cap_lines(&rows, expanded ? OUTPUT_EXPANDED : OUTPUT_COLLAPSED, ind);
    for (size_t i = 0; i < rows.n; i++) strvec_push(out, rows.v[i]);
    strvec_free(&rows), strvec_free(&parts);
    free(ic), free(prefix), free(ind);
}

static bool is_password(const hf_form_t *form, const char *name) {
    for (size_t i = 0; form && i < form->fields.n; i++) {
        const hf_field_t *f = form->fields.v[i];
        if (f->kind == HF_TEXT && str_eq(f->type, "password") && *f->name && str_eq(f->name, name)) return true;
    }
    return false;
}

static void ask_result(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *answers = json_get_arr(c->details, "answers");
    if (!answers) {
        char *t0 = tc_text_of(c->content), *t = clean(t0);
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Ask", .title_slot = -1};
        char *h = tc_status_line(&sl);
        char *td = *t ? tc_fg(TUI_TC_DIM, t) : NULL;
        char *all = td ? xasprintf("%s\n%s", h, td) : xstrdup(h);
        tc_push_wrapped(out, all, width);
        free(all), free(td), free(h), free(t), free(t0);
        return;
    }
    int cw = tc_block_content_width(width, -1, -1);
    bool timed_out = json_get_bool(c->details, "timedOut", false);
    hf_form_t *form = args_form(c->args);
    char *ov = timed_out ? NULL : tc_styled_sym("tool.ask", TUI_TC_ACCENT);
    tc_status_line_t sl = {.icon = TC_ST_WARNING, .icon_override = ov, .spinner = -1, .title = "Ask", .title_slot = -1};
    char *h = tc_status_line(&sl);
    tc_section_t sec = {0};
    const json_t *a;
    json_arr_foreach(answers, i, a) {
        const char *q = json_get_str(a, "question"), *ans = json_get_str(a, "answer");
        if (!q || !ans) continue;
        const char *name = json_get_str(a, "name");
        char *ql = one_line(q), *qt = tc_fg(TUI_TC_TEXT, ql);
        tc_wrap_prefixed(" ", " ", qt, cw, &sec.lines);
        char *shown = is_password(form, name ? name : "") ? bullets(ans, 12) : clean(ans);
        answer_lines(shown, cw, c->expanded, &sec.lines);
        free(ql), free(qt), free(shown);
    }
    if (timed_out) {
        char *d = tc_fg(TUI_TC_DIM, TIMED_OUT_NOTE);
        tc_wrap_prefixed(" ", " ", d, cw, &sec.lines);
        free(d);
    }
    tc_block_t blk = {.header = h, .state = timed_out ? TC_STATE_WARNING : TC_STATE_SUCCESS, .sections = &sec,
                      .nsections = sec.lines.n ? 1 : 0, .width = width, .pad_left = -1, .pad_right = -1,
                      .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&sec.lines);
    free(h), free(ov);
    hf_form_free(form);
}

void tc_ask(const tc_call_t *c, int width, strvec_t *out) {
    if (c->has_result) ask_result(c, width, out);
    else ask_call(c, width, out);
}
