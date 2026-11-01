/* tc_goal.c - goal card (port of omp tools/goal.ts). */
#include "ui/tools/tc.h"

#include "tui/ansi.h"

static const char *describe_op(const char *op) {
    if (str_eq(op, "create")) return "set";
    if (str_eq(op, "get")) return "check";
    return op ? op : "?";
}

static tui_tc_t badge_color(const char *st) {
    if (str_eq(st, "complete")) return TUI_TC_SUCCESS;
    if (str_eq(st, "budget-limited")) return TUI_TC_WARNING;
    if (str_eq(st, "paused") || str_eq(st, "dropped")) return TUI_TC_MUTED;
    return TUI_TC_ACCENT;
}

static char *quoted_italic(const char *text, int max) {
    char *t = str_trim(text ? text : "");
    char *tr = tui_truncate_to_width(t, max, NULL);
    char *q = xasprintf("\"%s\"", tr);
    char *m = tc_fg(TUI_TC_MUTED, q);
    char *r = tc_italic(m);
    free(m);
    free(q);
    free(tr);
    free(t);
    return r;
}

void tc_goal(const tc_call_t *c, int width, strvec_t *out) {
    const char *aop = json_get_str(c->args, "op");
    if (!c->has_result) {
        strvec_t meta = {0};
        const char *obj = json_get_str(c->args, "objective");
        char *ot = obj ? str_trim(obj) : NULL;
        if (str_eq(aop, "create") && ot && *ot) strvec_push_own(&meta, quoted_italic(ot, 60));
        if (str_eq(aop, "create") && json_get(c->args, "token_budget")) {
            char *n = tc_format_number(json_get_num(c->args, "token_budget", 0));
            strvec_push_own(&meta, xasprintf("budget %s", n));
            free(n);
        }
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Goal", .title_slot = -1,
                               .description = describe_op(aop), .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(ot);
        strvec_free(&meta);
        return;
    }
    const json_t *d = c->details;
    const char *op = json_get_str(d, "op") ? json_get_str(d, "op") : aop;
    const char *desc = describe_op(op);
    if (c->is_error) {
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "Goal", .title_slot = -1, .description = desc};
        char *h = tc_status_line(&sl);
        char *t = tc_text_of(c->content);
        char *e = tc_error_detail(*t ? t : "Goal tool failed");
        tc_section_t sec = {0};
        tc_split_lines(e, &sec.lines);
        tc_block_t blk = {.header = h, .state = TC_STATE_ERROR, .sections = &sec, .nsections = 1, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_ERROR};
        tc_output_block(&blk, out);
        strvec_free(&sec.lines);
        free(e);
        free(t);
        free(h);
        return;
    }
    const json_t *g = json_get_obj(d, "goal");
    if (!g) {
        const char *m[] = {"no active goal"};
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "Goal", .title_slot = -1, .description = desc,
                               .meta = m, .nmeta = 1};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        return;
    }
    const char *st = json_get_str(g, "status") ? json_get_str(g, "status") : "active";
    char *ov = tc_styled_sym("tool.goal", TUI_TC_ACCENT);
    tc_status_line_t sl = {.icon_override = ov, .spinner = -1, .title = "Goal", .title_slot = -1, .description = desc,
                           .badge = st, .badge_slot = badge_color(st)};
    char *h = tc_status_line(&sl);
    tc_section_t secs[2] = {0};
    strvec_push_own(&secs[0].lines, quoted_italic(json_get_str(g, "objective"), 100));
    double usedn = json_get_num(g, "tokensUsed", 0);
    char *used = tc_format_number(usedn);
    sb_t m = SB_INIT;
    if (json_get(g, "tokenBudget")) {
        double bud = json_get_num(g, "tokenBudget", 0);
        char *b = tc_format_number(bud), *l = tc_format_number(MAX(0, bud - usedn));
        sb_appendf(&m, "%s / %s tokens (%s left)", used, b, l);
        free(b);
        free(l);
    } else sb_appendf(&m, "%s tokens", used);
    double secsu = json_get_num(g, "timeUsedSeconds", 0);
    if (secsu > 0) {
        char *du = tc_format_duration((int64_t)(secsu * 1000));
        sb_appendf(&m, " \xc2\xb7 %s elapsed", du);
        free(du);
    }
    strvec_push_own(&secs[0].lines, tc_fg(TUI_TC_DIM, sb_cstr(&m)));
    size_t ns = 1;
    const char *report = json_get_str(d, "completionBudgetReport");
    if (report && *report) {
        secs[1].label = "Report";
        strvec_t rl = {0};
        tc_split_lines(report, &rl);
        for (size_t i = 0; i < rl.n; i++) strvec_push_own(&secs[1].lines, tc_fg(TUI_TC_MUTED, rl.v[i]));
        strvec_free(&rl);
        ns = 2;
    }
    tc_block_t blk = {.header = h, .state = TC_STATE_SUCCESS, .sections = secs, .nsections = ns, .width = width,
                      .pad_left = -1, .pad_right = -1, .border_slot = TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    for (size_t i = 0; i < ns; i++) strvec_free(&secs[i].lines);
    sb_free(&m);
    free(used);
    free(h);
    free(ov);
}
