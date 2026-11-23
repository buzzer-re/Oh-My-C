/* tc_resolve.c - cards for the staged-action and plan tools run top-level (omp packages/tui/src/tools/resolve.ts,
 * report-tool-issue.ts): `resolve` / `reject` (the accepted or discarded preview as an inverse block), `propose` and
 * `report_issue` (textArgumentRenderer: `Title: <first line>`, then the result text). */
#include "tc.h"

#include "tui/ansi.h"

/* firstLine: the trimmed argument's first line, tabs expanded ("" for a non-string) */
static char *first_line(const json_t *args, const char *field) {
    const char *v = json_get_str(args, field);
    if (!v) return xstrdup("");
    char *t = str_trim(v);
    char *nl = strchr(t, '\n');
    if (nl) *nl = 0;
    char *r = tc_replace_tabs(t);
    free(t);
    return r;
}

/* resultText: the joined text blocks, trimmed */
static char *result_text(const json_t *content) {
    sb_t b = SB_INIT;
    const json_t *blk;
    json_arr_foreach(content, i, blk) {
        const char *t = str_eq(json_get_str(blk, "type"), "text") ? json_get_str(blk, "text") : NULL;
        if (!t || !*t) continue;
        if (b.len) sb_putc(&b, '\n');
        sb_append(&b, t);
    }
    char *r = str_trim(sb_cstr(&b));
    sb_free(&b);
    return r;
}

/* textArgumentRenderer(title, field) */
static void text_argument(const tc_call_t *c, const char *title, const char *field, int width, strvec_t *out) {
    char *body = first_line(c->args, field);
    char *desc = *body ? tui_truncate_to_width(body, 72, "") : NULL;
    bool done = c->has_result && !c->partial;
    tc_status_line_t sl = {.icon = !done ? TC_ST_PENDING : c->is_error ? TC_ST_ERROR : TC_ST_SUCCESS,
                           .spinner = -1,
                           .title = title,
                           .title_slot = -1,
                           .description = desc};
    strvec_push_own(out, tc_status_line(&sl));
    if (done) {
        char *rt = result_text(c->content), *output = tc_replace_tabs(rt);
        if (*output) {
            char *col = tc_fg(c->is_error ? TUI_TC_ERROR : TUI_TC_TOOL_OUTPUT, output);
            tc_push_wrapped(out, col, width);
            free(col);
        }
        free(output);
        free(rt);
    }
    free(desc);
    free(body);
}

/* resolutionRenderer(defaultAction).renderCall */
static void resolution_call(const tc_call_t *c, const char *default_action, strvec_t *out) {
    const char *action = json_get_str(c->args, "action");
    if (!action) action = default_action;
    bool apply = str_eq(action, "apply");
    const char *reason_raw = json_get_str(c->args, "reason");
    char *reason_trim = reason_raw ? str_trim(reason_raw) : xstrdup("");
    char *reason = *reason_trim ? tui_truncate_to_width(reason_trim, 72, "") : NULL;
    char *meta = reason ? tc_fg(TUI_TC_MUTED, reason) : NULL;
    const char *metas[1] = {meta};
    tc_status_line_t sl = {.icon = TC_ST_PENDING,
                           .spinner = -1,
                           .title = "Resolve",
                           .title_slot = -1,
                           .description = action,
                           .badge = apply ? "proposed -> resolved" : "proposed -> rejected",
                           .badge_slot = apply ? TUI_TC_SUCCESS : TUI_TC_WARNING,
                           .meta = meta ? metas : NULL,
                           .nmeta = meta ? 1 : 0};
    strvec_push_own(out, tc_status_line(&sl));
    free(meta);
    free(reason);
    free(reason_trim);
}

/* resolutionRenderer.renderResult: a blank-framed inverse block in the outcome colour */
static void resolution_result(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    char *label = tc_replace_tabs(json_get_str(d, "label") ? json_get_str(d, "label") : "pending action");
    char *rtrim = json_get_str(d, "reason") ? str_trim(json_get_str(d, "reason")) : xstrdup("");
    char *reason = tc_replace_tabs(*rtrim ? rtrim : "No reason provided");
    const char *action = json_get_str(d, "action") ? json_get_str(d, "action") : "apply";
    bool is_apply = str_eq(action, "apply") && !c->is_error, failed_apply = str_eq(action, "apply") && c->is_error;
    tui_tc_t bg = c->is_error ? TUI_TC_ERROR : is_apply ? TUI_TC_SUCCESS : TUI_TC_WARNING;
    const char *icon = tc_sym(is_apply ? "tool.resolve" : "status.error");
    const char *verb = is_apply ? "Accept" : failed_apply ? "Failed" : "Discard";
    const char *sep = strstr(label, ": ");
    char *source = NULL, *summary;
    if (sep && sep > label) {
        char *s = xstrndup(label, (size_t)(sep - label));
        source = str_trim(s);
        free(s);
        summary = str_trim(sep + 2);
    } else {
        summary = xstrdup(label);
    }
    char *vb = xasprintf("%s:", verb), *bverb = tc_bold(vb);
    char *badge = NULL;
    if (source && *source) {
        char *br = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), source, tc_sym("format.bracketRight"));
        badge = tc_bold(br);
        free(br);
    }
    char *header = xasprintf("%s %s %s%s%s", icon, bverb, summary, badge ? " " : "", badge ? badge : "");
    char *ital = tc_italic(reason);
    const char *lines[] = {"", header, "", ital, ""};
    int lw = MAX(3, width), inner = MAX(1, lw - 2);
    for (size_t i = 0; i < ARRAY_LEN(lines); i++) {
        char *tr = tui_truncate_to_width(lines[i], inner, "");
        char *pt = tui_pad_to_width(tr, inner);
        char *framed = xasprintf(" %s ", pt);
        char *padded = tui_pad_to_width(framed, lw);
        char *col = tc_fg(bg, padded);
        strvec_push_own(out, tui_inverse(col));
        free(col);
        free(padded);
        free(framed);
        free(pt);
        free(tr);
    }
    free(ital);
    free(header);
    free(badge);
    free(bverb);
    free(vb);
    free(summary);
    free(source);
    free(reason);
    free(rtrim);
    free(label);
}

bool tc_resolve_tools(const tc_call_t *c, int width, strvec_t *out) {
    const char *n = c->name ? c->name : "";
    if (!strcmp(n, "propose")) text_argument(c, "Propose", "title", width, out);
    else if (!strcmp(n, "report_issue")) text_argument(c, "Report Tool Issue", "report", width, out);
    else if (!strcmp(n, "resolve") || !strcmp(n, "reject")) {
        if (c->has_result && !c->partial) resolution_result(c, width, out);
        else resolution_call(c, !strcmp(n, "reject") ? "discard" : "apply", out);
    } else {
        return false;
    }
    return true;
}
