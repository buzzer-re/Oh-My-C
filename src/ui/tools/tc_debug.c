/* tc_debug.c - debug (DAP) card (port of omp tools/debug.ts). */
#include "ui/tools/tc.h"

#include "tools/outsink.h"
#include "tui/ansi.h"

/* sanitizeDebugArg: model-authored arg text is untrusted: strip ANSI escapes and control characters, then
 * singleLine (tab expansion, line-break runs collapsed to one space) */
static char *sanitize_arg(const char *v) {
    char *s = out_sanitize(v ? v : "", strlen(v ? v : ""), NULL);
    char *t = tc_replace_tabs(s);
    free(s);
    sb_t b = SB_INIT;
    for (const char *p = t; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            while (p[1] == '\r' || p[1] == '\n') p++;
            sb_putc(&b, ' ');
        } else sb_putc(&b, *p);
    }
    free(t);
    return sb_detach(&b);
}

static char *underscores_to_spaces(const char *s) {
    char *r = xstrdup(s);
    for (char *p = r; *p; p++)
        if (*p == '_') *p = ' ';
    char *c = sanitize_arg(r);
    free(r);
    return c;
}

static char *summarize_call(const json_t *a) {
    const char *act = json_get_str(a, "action");
    char *action = underscores_to_spaces(act ? act : "request");
    char *target = NULL;
    static const char *const KEYS[] = {"program", "function", "expression", "command", "memory_reference",
                                       "instruction_reference", "data_id", "name"};
    if (json_get_str(a, "program")) target = xstrdup(json_get_str(a, "program"));
    else if (json_get_str(a, "file") && json_get(a, "line")) {
        char *ln = json_dump(json_get(a, "line"), 0);
        target = xasprintf("%s:%s", json_get_str(a, "file"), ln);
        free(ln);
    } else
        for (size_t i = 1; i < ARRAY_LEN(KEYS) && !target; i++)
            if (json_get_str(a, KEYS[i])) target = xstrdup(json_get_str(a, KEYS[i]));
    char *r;
    if (target) {
        char *st = sanitize_arg(target);
        char *t = tui_truncate_to_width(st, 60, NULL);
        free(st);
        r = xasprintf("%s %s", action, t);
        free(t);
        free(action);
    } else r = action;
    free(target);
    return r;
}

void tc_debug(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        char *d = summarize_call(c->args);
        tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "Debug", .title_slot = -1, .description = d};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        free(d);
        return;
    }
    const json_t *det = c->details;
    const char *act = json_get_str(c->args, "action") ? json_get_str(c->args, "action")
                      : json_get_str(det, "action")   ? json_get_str(det, "action")
                                                      : "debug";
    char *action = underscores_to_spaces(act);
    bool success = !c->partial && !c->is_error;
    char *icon = success ? tc_styled_sym("tool.debug", TUI_TC_ACCENT) : tc_status_icon(c->partial ? TC_ST_RUNNING : TC_ST_ERROR, c->spinner);
    char *header = xasprintf("%s Debug %s", icon, action);
    tc_section_t secs[2] = {0};
    size_t ns = 0;
    char *labels[2] = {tc_fg(TUI_TC_TOOL_TITLE, "Session"), tc_fg(TUI_TC_TOOL_TITLE, "Output")};
    const json_t *snap = json_get_obj(det, "snapshot");
    if (snap) {
        strvec_t *l = &secs[ns].lines;
        secs[ns].label = labels[0];
        strvec_push_own(l, xasprintf("Session %s", json_get_str(snap, "id") ? json_get_str(snap, "id") : ""));
        strvec_push_own(l, xasprintf("Adapter: %s", json_get_str(snap, "adapter") ? json_get_str(snap, "adapter") : ""));
        strvec_push_own(l, xasprintf("Status: %s", json_get_str(snap, "status") ? json_get_str(snap, "status") : ""));
        strvec_push_own(l, xasprintf("CWD: %s", json_get_str(snap, "cwd") ? json_get_str(snap, "cwd") : ""));
        if (json_get_str(snap, "program")) strvec_push_own(l, xasprintf("Program: %s", json_get_str(snap, "program")));
        if (json_get_str(snap, "stopReason")) strvec_push_own(l, xasprintf("Stop reason: %s", json_get_str(snap, "stopReason")));
        if (json_get_str(snap, "frameName")) strvec_push_own(l, xasprintf("Frame: %s", json_get_str(snap, "frameName")));
        if (json_get_str(snap, "instructionPointerReference"))
            strvec_push_own(l, xasprintf("Instruction pointer: %s", json_get_str(snap, "instructionPointerReference")));
        const char *sp = json_get_str(json_get_obj(snap, "source"), "path");
        if (sp && json_get(snap, "line")) {
            if (json_get(snap, "column"))
                strvec_push_own(l, xasprintf("Location: %s:%lld:%lld", sp, (long long)json_get_int(snap, "line", 0),
                                             (long long)json_get_int(snap, "column", 0)));
            else strvec_push_own(l, xasprintf("Location: %s:%lld", sp, (long long)json_get_int(snap, "line", 0)));
        }
        if (json_get_bool(snap, "needsConfigurationDone", false))
            strvec_push(l, "Configuration: pending configurationDone; set breakpoints, then continue.");
        if (json_get(snap, "exitCode")) strvec_push_own(l, xasprintf("Exit code: %lld", (long long)json_get_int(snap, "exitCode", 0)));
        for (size_t i = 0; i < l->n; i++) {
            char *t = tc_replace_tabs(l->v[i]);
            free(l->v[i]);
            l->v[i] = t;
        }
        ns++;
    }
    secs[ns].label = labels[1];
    {
        bool has_text = false;
        json_t *blk;
        json_arr_foreach(c->content, i, blk) if (str_eq(json_get_str(blk, "type"), "text")) has_text = true;
        char *t0 = has_text ? tc_text_of(c->content) : xstrdup("No output");
        char *t = tc_replace_tabs(t0);
        strvec_t raw = {0};
        tc_split_lines(t, &raw);
        size_t limit = c->expanded ? 12 : 3;
        size_t shown = MIN(raw.n, limit);
        for (size_t i = 0; i < shown; i++)
            strvec_push_own(&secs[ns].lines, c->expanded ? xstrdup(raw.v[i]) : tui_truncate_to_width(raw.v[i], 110, NULL));
        if (raw.n > shown) {
            char *hint = c->expanded ? xstrdup("") : tc_expand_hint();
            char *m = xasprintf("\xe2\x80\xa6 %zu more lines %s", raw.n - shown, hint);
            strvec_push_own(&secs[ns].lines, tc_fg(TUI_TC_MUTED, m));
            free(m);
            free(hint);
        }
        strvec_free(&raw);
        free(t);
        free(t0);
        ns++;
    }
    tc_block_t blk = {.header = header, .state = c->partial ? TC_STATE_PENDING : c->is_error ? TC_STATE_ERROR : TC_STATE_SUCCESS,
                      .sections = secs, .nsections = ns, .width = width, .no_bg = true, .pad_left = -1, .pad_right = -1,
                      .border_slot = -1};
    tc_output_block(&blk, out);
    for (size_t i = 0; i < ns; i++) strvec_free(&secs[i].lines);
    free(labels[0]);
    free(labels[1]);
    free(header);
    free(icon);
    free(action);
}
