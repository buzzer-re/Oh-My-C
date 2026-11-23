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

static char *bullets(const char *s, size_t cap) {
    size_t n = MIN(hf_utf16_len(s), cap);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) sb_append(&b, "\xe2\x80\xa2");
    return sb_detach(&b);
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
        /* notes on the selected options of this answer (detailsNotes / notesForAnswer) */
        strvec_t note_lines = {0};
        const json_t *nt;
        json_arr_foreach(json_get_arr(c->details, "notes"), j, nt) {
            const char *nn = json_get_str(nt, "name"), *nq = json_get_str(nt, "question"),
                       *no = json_get_str(nt, "option"), *nx = json_get_str(nt, "text");
            if (!nn || !nq || !no || !nx || !json_is_str(json_get(nt, "value"))) continue;
            if (!str_eq(nn, name ? name : "") || !str_eq(nq, q)) continue;
            char *ol = one_line(no), *xl = one_line(nx), *label = xasprintf("\xe2\x9c\x8e %s: %s", ol, xl);
            char *m = tc_fg(TUI_TC_MUTED, label), *it = tc_italic(m);
            tc_wrap_prefixed("   ", "   ", it, cw, &note_lines);
            free(ol), free(xl), free(label), free(m), free(it);
        }
        cap_lines(&note_lines, c->expanded ? OUTPUT_EXPANDED : OUTPUT_COLLAPSED, "   ");
        for (size_t k = 0; k < note_lines.n; k++) strvec_push(&sec.lines, note_lines.v[k]);
        strvec_free(&note_lines);
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

/* renderCall paints nothing: the ask panel over the composer owns the questions while they wait; the transcript shows
 * the answers (omp 093f499464) */
void tc_ask(const tc_call_t *c, int width, strvec_t *out) {
    if (c->has_result) ask_result(c, width, out);
}
