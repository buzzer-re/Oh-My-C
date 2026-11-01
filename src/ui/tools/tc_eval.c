/* tc_eval.c - eval card (port of omp tools/eval.ts and render/code-cell.ts): one framed code cell per cell
 * (language icon, status, [i/n], title, duration; highlighted code with a viewport tail window; Output
 * section with a tail preview). Not ported: status events (agent/mcp progress rows), json/markdown views,
 * and the display re-indenter (eval-format) that normalizes oddly indented model code. */
#include "ui/tools/tc.h"

#include "tui/ansi.h"
#include "tui/color.h"

typedef struct {
    const char *code, *title, *output;
    const char *status; /* pending running complete error */
    int64_t duration;   /* < 0: none */
    bool error_output;
} cell_t;
typedef VEC(cell_t) cells_t;

static char *py_icon(void) {
    const char *icon = tc_lang_icon("python");
    if (!*icon) return xstrdup("");
    char buf[48];
    const char *seq = tui_rgb_fg(tui_theme(), 0x3776ab, buf, sizeof buf);
    return xasprintf("%s%s\x1b[39m", seq, icon);
}

/* renderCodeCell */
static void code_cell(const cell_t *cl, int index, int total, int spinner, bool expanded, int code_max, int width,
                      const strvec_t *output, strvec_t *out) {
    strvec_t parts = {0};
    strvec_push_own(&parts, py_icon());
    const char *st = cl->status;
    tc_status_t s = str_eq(st, "complete") ? TC_ST_DONE : str_eq(st, "error") ? TC_ST_ERROR : str_eq(st, "running") ? TC_ST_RUNNING
                                                                                                                 : TC_ST_PENDING;
    char *ic = tc_status_icon(s, str_eq(st, "running") ? spinner : -1);
    if (str_eq(st, "pending") || str_eq(st, "running")) {
        char *m = tc_fg(TUI_TC_MUTED, st);
        strvec_push_own(&parts, xasprintf("%s %s", ic, m));
        free(m);
        free(ic);
    } else strvec_push_own(&parts, ic);
    if (total > 1) {
        char b[32];
        snprintf(b, sizeof b, "[%d/%d]", index + 1, total);
        strvec_push_own(&parts, tc_fg(TUI_TC_ACCENT, b));
    }
    if (cl->title && *cl->title) strvec_push_own(&parts, tc_fg(TUI_TC_TOOL_TITLE, cl->title));
    char *title = strvec_join(&parts, " ");
    char *meta = NULL;
    if (cl->duration >= 0) {
        char *d = tc_format_duration(cl->duration);
        char *p = xasprintf("(%s)", d);
        meta = tc_fg(TUI_TC_DIM, p);
        free(p);
        free(d);
    }
    char *code = tc_replace_tabs(cl->code ? cl->code : "");
    strvec_t raw = {0};
    tc_split_lines(code, &raw);
    size_t maxl = expanded ? raw.n : MIN(raw.n, (size_t)MAX(0, code_max));
    size_t hidden = raw.n - maxl;
    bool tail = !expanded && hidden > 0;
    size_t start = tail ? raw.n - maxl : 0;
    sb_t vis = SB_INIT;
    for (size_t i = start; i < start + maxl; i++) {
        if (i > start) sb_putc(&vis, '\n');
        sb_append(&vis, raw.v[i]);
    }
    tc_section_t secs[2] = {0};
    if (hidden) {
        char *hint = tc_expand_hint();
        char *e = xasprintf("\xe2\x80\xa6 %zu earlier line%s %s", hidden, hidden == 1 ? "" : "s", hint);
        strvec_push_own(&secs[0].lines, tc_fg(TUI_TC_DIM, e));
        free(e);
        free(hint);
    }
    tc_highlight(sb_cstr(&vis), "python", &secs[0].lines);
    size_t ns = 1;
    if (output && output->n) {
        secs[1].label = tc_fg(TUI_TC_TOOL_TITLE, "Output");
        for (size_t i = 0; i < output->n; i++) strvec_push(&secs[1].lines, output->v[i]);
        ns = 2;
    }
    tc_state_t state = str_eq(st, "complete") ? TC_STATE_SUCCESS : str_eq(st, "error") ? TC_STATE_ERROR
                       : str_eq(st, "running")                         ? TC_STATE_RUNNING
                                                                        : TC_STATE_PENDING;
    tc_block_t blk = {.header = title, .header_meta = meta, .state = state, .sections = secs, .nsections = ns, .width = width,
                      .pad_left = -1, .pad_right = -1, .border_slot = -1};
    tc_output_block(&blk, out);
    free((char *)secs[1].label);
    strvec_free(&secs[0].lines);
    strvec_free(&secs[1].lines);
    sb_free(&vis);
    strvec_free(&raw);
    free(code);
    free(meta);
    free(title);
    strvec_free(&parts);
}

/* formatCellOutputLines: styled lines, visual tail window of `preview` rows */
static void cell_output(const char *output, bool err, bool expanded, int preview, int width, strvec_t *out) {
    if (!output || !*output) return;
    strvec_t lines = {0}, styled = {0};
    tc_split_lines(output, &lines);
    for (size_t i = 0; i < lines.n; i++) {
        char *t = tc_replace_tabs(lines.v[i]);
        strvec_push_own(&styled, tc_fg(err ? TUI_TC_ERROR : TUI_TC_TOOL_OUTPUT, t));
        free(t);
    }
    int iw = tc_block_content_width(width, -1, -1);
    if (expanded) {
        for (size_t i = 0; i < styled.n; i++) strvec_push(out, styled.v[i]);
    } else {
        strvec_t vis = {0};
        for (size_t i = 0; i < styled.n; i++) tui_wrap_text_with_ansi(styled.v[i], MAX(1, iw), &vis);
        size_t hidden = vis.n > (size_t)preview ? vis.n - (size_t)preview : 0;
        for (size_t i = hidden; i < vis.n; i++) strvec_push(out, vis.v[i]);
        if (hidden) {
            char *t = xasprintf("\xe2\x80\xa6 %zu more lines (Ctrl+O to expand)", hidden);
            strvec_push_own(out, tc_fg(TUI_TC_DIM, t));
            free(t);
        }
        strvec_free(&vis);
    }
    strvec_free(&styled);
    strvec_free(&lines);
}

static int window_rows(const tc_call_t *c) { return tc_preview_window_rows(c->term_rows); }

static void arg_cells(const json_t *a, cells_t *cells) {
    const json_t *arr = json_get_arr(a, "cells");
    if (arr) {
        json_t *x;
        json_arr_foreach(arr, i, x) {
            if (!json_is_obj(x)) continue;
            cell_t cl = {.code = json_get_str(x, "code") ? json_get_str(x, "code") : "", .title = json_get_str(x, "title"),
                         .duration = -1};
            vec_push(cells, cl);
        }
    } else if (json_get_str(a, "code")) {
        cell_t cl = {.code = json_get_str(a, "code"), .title = json_get_str(a, "title"), .duration = -1};
        vec_push(cells, cl);
    }
}

void tc_eval(const tc_call_t *c, int width, strvec_t *out) {
    int wr = window_rows(c);
    if (!c->has_result) {
        cells_t cells = {0};
        arg_cells(c->args, &cells);
        if (!cells.n) {
            char *p = tc_fg(TUI_TC_ACCENT, ">>>");
            char *t = xasprintf("\x1b[1m%s \xe2\x80\xa6\x1b[22m", p);
            char *r = tc_fg(TUI_TC_TOOL_TITLE, t);
            tc_push_wrapped(out, r, width);
            free(r);
            free(t);
            free(p);
            return;
        }
        for (size_t i = 0; i < cells.n; i++) {
            cells.v[i].status = c->spinner >= 0 ? "running" : "pending";
            code_cell(&cells.v[i], (int)i, (int)cells.n, c->spinner, c->expanded, wr, width, NULL, out);
            if (i + 1 < cells.n) strvec_push(out, "");
        }
        vec_free(&cells);
        return;
    }
    const json_t *d = c->details;
    const json_t *rc = json_get_arr(d, "cells");
    int preview = MIN(10, wr);
    const json_t *as = json_get_obj(d, "async");
    char *async_line = NULL;
    if (str_eq(json_get_str(as, "state"), "running")) {
        char *t = xasprintf("%sBackgrounded: %s%s", tc_sym("format.bracketLeft"), json_get_str(as, "jobId") ? json_get_str(as, "jobId") : "",
                            tc_sym("format.bracketRight"));
        async_line = tc_fg(TUI_TC_DIM, t);
        free(t);
    }
    char *notice_line = NULL;
    if (json_get_str(d, "notice")) {
        char *t = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), json_get_str(d, "notice"), tc_sym("format.bracketRight"));
        notice_line = tc_fg(TUI_TC_DIM, t);
        free(t);
    }
    if (json_len(rc)) {
        size_t n = json_len(rc);
        for (size_t i = 0; i < n; i++) {
            const json_t *x = json_at(rc, i);
            cell_t cl = {.code = json_get_str(x, "code") ? json_get_str(x, "code") : "", .title = json_get_str(x, "title"),
                         .status = json_get_str(x, "status") ? json_get_str(x, "status") : "complete",
                         .duration = json_get(x, "durationMs") ? json_get_int(x, "durationMs", 0) : -1};
            strvec_t ol = {0};
            cell_output(json_get_str(x, "output"), str_eq(cl.status, "error"), c->expanded, preview, width, &ol);
            code_cell(&cl, (int)i, (int)n, c->spinner, c->expanded, wr, width, &ol, out);
            strvec_free(&ol);
            if (i + 1 < n) strvec_push(out, "");
        }
        if (notice_line) strvec_push(out, notice_line);
        if (async_line) strvec_push(out, async_line);
        free(notice_line);
        free(async_line);
        return;
    }
    /* no per-cell details: the output text */
    char *raw = tc_text_of(c->content);
    char *outp = tc_trim_end(raw);
    free(raw);
    if (!*outp) {
        if (notice_line) strvec_push(out, notice_line);
        if (async_line) strvec_push(out, async_line);
    } else {
        strvec_t lines = {0}, styled = {0};
        tc_split_lines(outp, &lines);
        for (size_t i = 0; i < lines.n; i++) strvec_push_own(&styled, tc_fg(TUI_TC_TOOL_OUTPUT, lines.v[i]));
        if (c->expanded) {
            for (size_t i = 0; i < styled.n; i++) tc_push_wrapped(out, styled.v[i], width);
        } else {
            strvec_t vis = {0};
            strvec_push(&vis, "");
            for (size_t i = 0; i < styled.n; i++) tui_wrap_text_with_ansi(styled.v[i], MAX(1, width), &vis);
            size_t skipped = vis.n > (size_t)preview ? vis.n - (size_t)preview : 0;
            if (skipped) {
                strvec_push(out, "");
                char *t = xasprintf("\xe2\x80\xa6 (%zu earlier lines, showing %zu of %zu) (Ctrl+O to expand)", skipped,
                                    vis.n - skipped, vis.n);
                char *dl = tc_fg(TUI_TC_DIM, t);
                strvec_push_own(out, tui_truncate_to_width(dl, width, NULL));
                free(dl);
                free(t);
            }
            for (size_t i = skipped; i < vis.n; i++) strvec_push(out, vis.v[i]);
            strvec_free(&vis);
        }
        if (notice_line) strvec_push(out, notice_line);
        if (async_line) strvec_push(out, async_line);
        strvec_free(&styled);
        strvec_free(&lines);
    }
    free(outp);
    free(notice_line);
    free(async_line);
}
