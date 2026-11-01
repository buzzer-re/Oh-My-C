/* toolcards.c - tool execution cards. */
#include "ui/ui.h"

char *args_summary(const char *tool, const json_t *args) {
    const char *keys[] = {"path", "command", "pattern", "query", "url", "op", "input"};
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        const char *v = json_get_str(args, keys[i]);
        if (v) {
            char *first = xstrndup(v, strcspn(v, "\n"));
            if (strlen(first) > 120) strcpy(first + 117, "...");
            if (!strcmp(tool, "edit") && !strcmp(keys[i], "input")) {
                const char *lb = strchr(v, '['), *rb = lb ? strchr(lb, ']') : NULL;
                if (lb && rb) {
                    free(first);
                    first = xstrndup(lb + 1, (size_t)(rb - lb - 1));
                }
            }
            return first;
        }
    }
    json_t *tasks = json_get_arr(args, "tasks");
    if (tasks) return xasprintf("%zu task(s)", json_len(tasks));
    char *s = json_dump(args, 0);
    if (strlen(s) > 100) strcpy(s + 97, "...");
    return s;
}

void render_tool(msgview_t *v, int width, strvec_t *out) {
    tui_tc_t bg = !v->done ? TUI_TC_TOOL_PENDING_BG : v->is_error ? TUI_TC_TOOL_ERROR_BG : TUI_TC_TOOL_SUCCESS_BG;
    char *sum = args_summary(v->tool_name, v->args);
    const char *icon = !v->done ? "…" : v->is_error ? "✗" : "✓";
    char *t1 = fg(TUI_TC_TOOL_TITLE, v->tool_name);
    char *b1 = tui_bold(t1);
    char *t2 = fg(TUI_TC_MUTED, sum);
    char *title = xasprintf(" %s %s %s", icon, b1, t2);
    strvec_t lines = {0};
    push_wrapped(&lines, title, width - 1, 0);
    if (v->output && *v->output) {
        strvec_t ol = str_split_lines(v->output, strlen(v->output));
        size_t max = g_ui && g_ui->expand_tools ? ol.n : 10;
        size_t start = ol.n > max ? ol.n - max : 0;
        if (start) {
            char *m = xasprintf("   … %zu earlier lines (ctrl+o to expand)", start);
            char *mm = fg(TUI_TC_DIM, m);
            strvec_push_own(&lines, mm);
            free(m);
        }
        for (size_t i = start; i < ol.n; i++) {
            char *clean = tui_replace_tabs(ol.v[i]);
            char *stripped = tui_strip_ansi(clean);
            tui_tc_t slot = TUI_TC_TOOL_OUTPUT;
            if (!strcmp(v->tool_name, "edit") || !strcmp(v->tool_name, "write")) {
                if (stripped[0] == '+') slot = TUI_TC_TOOL_DIFF_ADDED;
                else if (stripped[0] == '-') slot = TUI_TC_TOOL_DIFF_REMOVED;
            }
            char *c = fg(slot, stripped);
            char *line = xasprintf("   %s", c);
            char *tr = tui_truncate_to_width(line, width - 1, "…");
            strvec_push_own(&lines, tr);
            free(line);
            free(c);
            free(stripped);
            free(clean);
        }
        strvec_free(&ol);
    }
    for (size_t i = 0; i < lines.n; i++) {
        char *padded = tui_pad_to_width(lines.v[i], width);
        char *b = tui_bgs(TH(), bg, padded);
        strvec_push_own(out, b);
        free(padded);
    }
    strvec_free(&lines);
    free(title);
    free(t2);
    free(b1);
    free(t1);
    free(sum);
}

