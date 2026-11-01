/* statusline.c - status band and composer chrome. */
#include "ui/ui.h"


static void status_render(tui_component_t *c, int width, strvec_t *out) {
    ui_t *u = c->user;
    app_t *a = u->app;
    char *model = app_model_label(a);
    char *cwd = path_display(a->cwd, NULL);
    json_t *st = app_stats(a);
    double pct = json_get_num(st, "contextPercent", 0);
    double cost = json_get_num(st, "cost", 0);
    json_free(st);
    tui_tc_t tslot = (tui_tc_t)(TUI_TC_THINKING_OFF + (int)a->thinking);
    char *m = fg(TUI_TC_STATUS_LINE_MODEL, model);
    char *t = fg(tslot, effort_name(a->thinking));
    char *p = fg(TUI_TC_STATUS_LINE_PATH, cwd);
    char ctx[64], cs[32];
    snprintf(ctx, sizeof ctx, "%.1f%%", pct);
    snprintf(cs, sizeof cs, "$%.3f", cost);
    char *cx = fg(TUI_TC_STATUS_LINE_CONTEXT, ctx);
    char *co = fg(TUI_TC_STATUS_LINE_COST, cs);
    char *sep = fg(TUI_TC_STATUS_LINE_SEP, " · ");
    char *line = xasprintf(" %s%s%s%s%s%s%s%s%s%s", a->advisor && !a->plan_mode ? "[advisor] " : a->plan_mode ? "[plan] " : app_goal(a) && goal_enabled(app_goal(a)) ? "[goal] " : app_goal(a) ? "[goal paused] " : "", m, sep, t, sep, p, sep, cx, sep, co);
    if (a->session && a->session->title) {
        char *tt = fg(TUI_TC_MUTED, a->session->title);
        char *l2 = xasprintf("%s%s%s", line, sep, tt);
        free(line);
        line = l2;
        free(tt);
    }
    strvec_push_own(out, tui_truncate_to_width(line, width, "…"));
    free(line);
    free(sep);
    free(co);
    free(cx);
    free(p);
    free(t);
    free(m);
    free(cwd);
    free(model);
}

static const tui_component_vt STATUS_VT = {"status", status_render, NULL, NULL, NULL};


tui_component_t *ui_status_new(ui_t *u) {
    tui_component_t *c = xcalloc(1, sizeof(tui_component_t));
    tui_component_init(c, &STATUS_VT);
    c->user = u;
    return c;
}
