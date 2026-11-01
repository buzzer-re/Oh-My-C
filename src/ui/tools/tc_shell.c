/* tc_shell.c - bash tool card (port of omp tools/bash.ts createShellRenderer). */
#include "ui/tools/tc.h"
#include "tools/outsink.h"

#include <ctype.h>

#include "tui/ansi.h"

/* `cd <dir> &&` prefix for a `cwd` arg outside/inside the project (formatToolWorkingDirectory) */
static char *display_workdir(const char *workdir, const char *cwd) {
    if (!workdir || !*workdir) return NULL;
    char *abs = workdir[0] == '/' || !cwd ? xstrdup(workdir) : xasprintf("%s/%s", cwd, workdir);
    size_t n = strlen(abs);
    while (n > 1 && abs[n - 1] == '/') abs[--n] = 0;
    if (cwd && !strcmp(abs, cwd)) {
        free(abs);
        return NULL;
    }
    char *r = tc_rel_path(abs, cwd);
    free(abs);
    char *t = tc_replace_tabs(r);
    free(r);
    return t;
}

static int cmp_keys(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static char *env_assignments(const json_t *env) {
    if (!json_is_obj(env) || json_len(env) == 0) return NULL;
    strvec_t keys = {0};
    const char *k;
    json_t *v;
    json_obj_foreach(env, i, k, v) strvec_push(&keys, k);
    qsort(keys.v, keys.n, sizeof(char *), cmp_keys);
    sb_t b = SB_INIT;
    for (size_t j = 0; j < keys.n; j++) {
        json_t *val = json_get(env, keys.v[j]);
        char *s = json_is_str(val) ? xstrdup(json_sv(val)) : json_dump(val, 0);
        if (j) sb_putc(&b, ' ');
        sb_appendf(&b, "%s=\"", keys.v[j]);
        for (const char *p = s; *p; p++) {
            switch (*p) {
            case '\\': sb_append(&b, "\\\\"); break;
            case '\n': sb_append(&b, "\\n"); break;
            case '\r': sb_append(&b, "\\r"); break;
            case '\t': sb_append(&b, "\\t"); break;
            case '"': sb_append(&b, "\\\""); break;
            case '$': sb_append(&b, "\\$"); break;
            case '`': sb_append(&b, "\\`"); break;
            default: sb_putc(&b, *p);
            }
        }
        sb_putc(&b, '"');
        free(s);
    }
    strvec_free(&keys);
    return sb_detach(&b);
}

/* ---------- shell highlighting ----------
 * omp highlights through syntect's Sublime Bash grammar, where a whole simple command (name + arguments)
 * is scoped meta.function-call (function color), strings/variables/comments/keywords keep their own
 * colors, and operators/terminators are keyword.operator. This tokenizer reproduces those scopes for
 * one-line commands. */

static bool bash_kw(const char *w, size_t n) {
    static const char *kws[] = {"if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case",
                                "esac", "in", "function", "select", "time", "!", "{", "}"};
    for (size_t i = 0; i < ARRAY_LEN(kws); i++)
        if (strlen(kws[i]) == n && !strncmp(w, kws[i], n)) return true;
    return false;
}

static void put(sb_t *b, tui_tc_t slot, const char *s, size_t n) {
    char *t = xstrndup(s, n);
    tc_fg_sb(b, slot, t);
    free(t);
}

/* `$NAME`, `${...}`, `$(...)`-less variable length at s (0 when not a variable) */
static size_t var_len(const char *s) {
    if (s[0] != '$') return 0;
    if (s[1] == '{') {
        const char *e = strchr(s, '}');
        return e ? (size_t)(e - s) + 1 : strlen(s);
    }
    size_t i = 1;
    if (isdigit((unsigned char)s[1]) || strchr("@*#?$!-", s[1])) return s[1] ? 2 : 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    return i > 1 ? i : 0;
}

static void highlight_bash_line(const char *s, sb_t *b) {
    size_t n = strlen(s), i = 0;
    bool cmd_start = true; /* next word is in command position */
    bool in_cmd = false;   /* inside a simple command (meta.function-call) */
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            /* spaces inside a command belong to its function-call scope */
            bool more = j < n && !strchr(";|&#)", s[j]);
            bool opt = in_cmd && s[j] == '-' && j + 1 < n && s[j + 1] != ' ';
            if (opt) put(b, TUI_TC_SYNTAX_PUNCTUATION, s + i, j - i);
            else if (in_cmd && more) put(b, TUI_TC_SYNTAX_FUNCTION, s + i, j - i);
            else sb_appendn(b, s + i, j - i);
            i = j;
            continue;
        }
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            put(b, TUI_TC_SYNTAX_COMMENT, s + i, n - i);
            break;
        }
        if (c == ';' || c == '|' || c == '&') {
            size_t j = i + 1;
            if ((c == '|' || c == '&') && s[j] == c) j++;
            else if (c == ';' && s[j] == ';') j++;
            else if (c == '&' && s[j] == '>') {
                j++;
                if (s[j] == '>') j++;
                put(b, TUI_TC_SYNTAX_OPERATOR, s + i, j - i);
                i = j;
                continue;
            }
            put(b, TUI_TC_SYNTAX_OPERATOR, s + i, j - i);
            i = j;
            cmd_start = true;
            in_cmd = false;
            continue;
        }
        if (c == '(' || c == ')') {
            put(b, TUI_TC_SYNTAX_PUNCTUATION, s + i, 1);
            i++;
            cmd_start = c == '(';
            in_cmd = false;
            continue;
        }
        /* redirections: [n]> [n]>> < <<< >&n */
        {
            size_t j = i;
            while (j < n && isdigit((unsigned char)s[j])) j++;
            if (j < n && (s[j] == '>' || s[j] == '<') && (j == i || (j > i && in_cmd))) {
                size_t k = j + 1;
                while (k < n && (s[k] == '>' || s[k] == '<' || s[k] == '&')) k++;
                while (k < n && isdigit((unsigned char)s[k]) && s[k - 1] == '&') k++;
                if (j > i) put(b, TUI_TC_SYNTAX_NUMBER, s + i, j - i);
                put(b, TUI_TC_SYNTAX_OPERATOR, s + j, k - j);
                i = k;
                continue;
            }
        }
        if (c == '\'' || c == '"') {
            /* quotes are punctuation.definition.string, the body string, `$` punctuation + variable */
            size_t j = i + 1;
            put(b, TUI_TC_SYNTAX_PUNCTUATION, s + i, 1);
            size_t seg = j;
            while (j < n && s[j] != c) {
                if (c == '"' && s[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                size_t vl = c == '"' ? var_len(s + j) : 0;
                if (vl) {
                    if (j > seg) put(b, TUI_TC_SYNTAX_STRING, s + seg, j - seg);
                    put(b, TUI_TC_SYNTAX_PUNCTUATION, s + j, 1);
                    put(b, TUI_TC_SYNTAX_VARIABLE, s + j + 1, vl - 1);
                    j += vl;
                    seg = j;
                    continue;
                }
                j++;
            }
            if (j > seg) put(b, TUI_TC_SYNTAX_STRING, s + seg, j - seg);
            if (j < n) {
                put(b, TUI_TC_SYNTAX_PUNCTUATION, s + j, 1);
                j++;
            }
            i = j;
            if (cmd_start) cmd_start = false, in_cmd = true;
            continue;
        }
        {
            size_t vl = var_len(s + i);
            if (vl) {
                put(b, TUI_TC_SYNTAX_PUNCTUATION, s + i, 1);
                put(b, TUI_TC_SYNTAX_VARIABLE, s + i + 1, vl - 1);
                i += vl;
                if (cmd_start) cmd_start = false, in_cmd = true;
                continue;
            }
        }
        /* a word */
        size_t j = i;
        while (j < n && !strchr(" \t;|&()<>'\"", s[j]) && !(s[j] == '$' && j > i && var_len(s + j))) j++;
        if (j == i) j = i + 1;
        if (cmd_start && bash_kw(s + i, j - i)) {
            put(b, TUI_TC_SYNTAX_KEYWORD, s + i, j - i);
            i = j;
            continue;
        }
        const char *eq = memchr(s + i, '=', j - i);
        if (cmd_start && eq && eq > s + i) {
            /* NAME=value assignment prefix */
            put(b, TUI_TC_SYNTAX_VARIABLE, s + i, (size_t)(eq - s - i));
            put(b, TUI_TC_SYNTAX_OPERATOR, eq, 1);
            if (s + j > eq + 1) put(b, TUI_TC_SYNTAX_STRING, eq + 1, (size_t)(s + j - eq - 1));
            i = j;
            continue;
        }
        if (in_cmd && s[i] == '-' && j - i > 1) {
            /* option: `-`/`--` punctuation (with the space before it), name variable.parameter */
            size_t d = i;
            while (d < j && s[d] == '-') d++;
            put(b, TUI_TC_SYNTAX_PUNCTUATION, s + i, d - i);
            put(b, TUI_TC_SYNTAX_VARIABLE, s + d, j - d);
            i = j;
            continue;
        }
        put(b, TUI_TC_SYNTAX_FUNCTION, s + i, j - i);
        cmd_start = false;
        in_cmd = true;
        i = j;
    }
}

static void highlight_bash(const char *code, strvec_t *out) {
    strvec_t lines = {0};
    tc_split_lines(code, &lines);
    for (size_t i = 0; i < lines.n; i++) {
        sb_t b = SB_INIT;
        highlight_bash_line(lines.v[i], &b);
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&lines);
}

/* formatBashCommandLines: dim `$ [cd dir &&] [ENV=..] ` + highlighted command */
static void command_lines(const json_t *args, const char *cwd, strvec_t *out) {
    const char *cmd = json_get_str(args, "command");
    char *command = tc_replace_tabs(cmd && *cmd ? cmd : "\xe2\x80\xa6");
    char *wd = display_workdir(json_get_str(args, "cwd"), cwd);
    char *envs = env_assignments(json_get(args, "env"));
    sb_t pre = SB_INIT;
    sb_append(&pre, "$");
    if (wd) sb_appendf(&pre, " cd %s &&", wd);
    if (envs) sb_appendf(&pre, " %s", envs);
    sb_putc(&pre, ' ');
    char *prefix = tc_fg(TUI_TC_DIM, sb_cstr(&pre));
    strvec_t hl = {0};
    highlight_bash(command, &hl);
    for (size_t i = 0; i < hl.n; i++) {
        if (i == 0) strvec_push_own(out, xasprintf("%s%s", prefix, hl.v[i]));
        else strvec_push(out, hl.v[i]);
    }
    if (!hl.n) strvec_push_own(out, tc_trim_end(prefix));
    strvec_free(&hl);
    free(prefix);
    sb_free(&pre);
    free(envs);
    free(wd);
    free(command);
}

/* drop a trailing notice line (stripTrailingNotice with trimResult) */
static void strip_trailing(char **text, const char *notice) {
    char *t = *text;
    char *at = NULL;
    for (char *p = strstr(t, notice); p; p = strstr(p + 1, notice)) at = p;
    if (!at) return;
    char *end = at + strlen(notice);
    char *start = at;
    if (start > t && start[-1] == '\n') start--;
    if (*end == '\n') end++;
    sb_t b = SB_INIT;
    sb_appendn(&b, t, (size_t)(start - t));
    sb_append(&b, end);
    char *r = tc_trim_end(sb_cstr(&b));
    sb_free(&b);
    free(t);
    *text = r;
}

/* remove LLM-facing notices omc appends to bash output so they aren't printed twice */
static char *strip_bash_notices(const char *raw, const json_t *d) {
    char *t = xstrdup(raw ? raw : "");
    const json_t *as = json_get_obj(d, "async");
    if (as && str_eq(json_get_str(as, "state"), "running")) {
        char *n = xasprintf("Backgrounded as job %s", json_get_str(as, "jobId") ? json_get_str(as, "jobId") : "");
        char *at = strstr(t, n);
        if (at) {
            *at = 0;
            char *r = tc_trim_end(t);
            free(t);
            t = r;
        }
        free(n);
    }
    /* the details.meta notice (truncation / column cap / artifact) appended at the message boundary */
    if (json_get_obj(d, "meta")) {
        char *st = out_strip_notice(t, json_get_obj(d, "meta"));
        free(t);
        t = st;
    }
    if (json_get(d, "exitCode")) {
        char *n = xasprintf("Command exited with code %lld", (long long)json_get_int(d, "exitCode", 0));
        strip_trailing(&t, n);
        free(n);
    }
    char *p = strstr(t, "\n\nCommand timed out after ");
    if (!p && str_starts(t, "Command timed out after ")) p = t;
    if (p) *p = 0;
    p = strstr(t, "\n\nCommand terminated by signal ");
    if (p) *p = 0;
    if (str_ends(t, "\n\nCommand aborted")) t[strlen(t) - strlen("\n\nCommand aborted")] = 0;
    if (json_get(d, "wallTimeMs") && json_get_bool(d, "wallTimeNoticeEmitted", true)) {
        char *n = xasprintf("Wall time: %.2f seconds", json_get_num(d, "wallTimeMs", 0) / 1000.0);
        strip_trailing(&t, n);
        free(n);
    }
    return t;
}

void tc_bash(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    int cw = tc_block_content_width(width, -1, -1);
    strvec_t cmd = {0};
    if (c->args) command_lines(c->args, c->cwd, &cmd);
    tc_cap_preview(&cmd, c->expanded, tc_preview_window_rows(c->term_rows));

    if (!c->has_result) {
        char *footer = NULL;
        if (c->exec_started && c->elapsed_ms >= 1000) {
            char *dur = tc_format_duration((c->elapsed_ms / 1000) * 1000);
            char *t = xasprintf("elapsed %s", dur);
            footer = tc_fg(TUI_TC_DIM, t);
            free(t);
            free(dur);
        }
        tc_section_t sec = {.lines = cmd};
        tc_block_t blk = {.footer_meta = footer, .state = TC_STATE_PENDING, .sections = &sec, .nsections = 1, .width = width,
                          .pad_left = -1, .pad_right = -1, .border_slot = -1};
        tc_output_block(&blk, out);
        free(footer);
        strvec_free(&cmd);
        return;
    }

    bool is_error = c->is_error, partial = c->partial;
    bool timed_out = json_get_bool(d, "timedOut", false);
    char *raw = tc_text_of(c->content);
    char *output = strip_bash_notices(raw, d);
    char *display = tc_trim_end(output);

    /* stats footer */
    strvec_t stats = {0};
    if (partial && !json_get(d, "wallTimeMs") && c->elapsed_ms >= 1000) {
        char *dur = tc_format_duration((c->elapsed_ms / 1000) * 1000);
        strvec_push_own(&stats, xasprintf("elapsed %s", dur));
        free(dur);
    }
    const json_t *as = json_get_obj(d, "async");
    if (as && str_eq(json_get_str(as, "state"), "running"))
        strvec_push_own(&stats, xasprintf("Backgrounded: %s", json_get_str(as, "jobId") ? json_get_str(as, "jobId") : ""));
    if (json_get(d, "wallTimeMs")) strvec_push_own(&stats, xasprintf("Wall: %.2fs", json_get_num(d, "wallTimeMs", 0) / 1000.0));
    bool tdis = json_get_bool(d, "timeoutDisabled", false);
    if (tdis) strvec_push(&stats, "Timeout: disabled");
    else if (json_get(d, "timeoutSeconds")) {
        int64_t ts = json_get_int(d, "timeoutSeconds", 0);
        if (json_get(d, "requestedTimeoutSeconds") && json_get_int(d, "requestedTimeoutSeconds", 0) != ts)
            strvec_push_own(&stats, xasprintf("Timeout: %llds (requested %llds clamped)", (long long)ts,
                                              (long long)json_get_int(d, "requestedTimeoutSeconds", 0)));
        else strvec_push_own(&stats, xasprintf("Timeout: %llds", (long long)ts));
    }
    if (json_get_str(d, "artifactId")) strvec_push_own(&stats, xasprintf("Artifact: %s", json_get_str(d, "artifactId")));
    if (is_error && json_get(d, "exitCode") && json_is_num(json_get(d, "exitCode")))
        strvec_push_own(&stats, xasprintf("Exit: %lld", (long long)json_get_int(d, "exitCode", 0)));
    char *footer = NULL;
    if (stats.n) {
        char *j = strvec_join(&stats, tc_sym("sep.dot"));
        footer = tc_fg(TUI_TC_DIM, j);
        free(j);
    }

    strvec_t olines = {0};
    bool has_output = false;
    for (const char *p = display; *p; p++)
        if (!isspace((unsigned char)*p)) {
            has_output = true;
            break;
        }
    strvec_t raw_lines = {0};
    if (has_output) {
        tc_split_lines(display, &raw_lines);
        for (size_t i = 0; i < raw_lines.n; i++) {
            char *s = tc_sanitize_line(raw_lines.v[i]);
            free(raw_lines.v[i]);
            raw_lines.v[i] = s;
        }
    }
    tc_pane_t pane = {.lines = &raw_lines, .expanded = c->expanded,
                      .collapsed_max = MIN(10, tc_preview_window_rows(c->term_rows)), .tail = true, .visual = true,
                      .width = cw, .style = TUI_TC_TOOL_OUTPUT};
    tc_output_pane(&pane, &olines);
    const json_t *meta = json_get_obj(d, "meta");
    if (meta && (json_get_str(meta, "artifactError") || (json_get_obj(meta, "truncation") && !c->expanded))) {
        /* omp formatStyledTruncationWarning */
        char *wt = out_format_warning(meta);
        if (wt) {
            char *w = xasprintf("%s%s%s", tc_sym("format.bracketLeft"), wt, tc_sym("format.bracketRight"));
            strvec_push_own(&olines, tc_fg(TUI_TC_WARNING, w));
            free(w);
            free(wt);
        }
    }

    tc_section_t secs[2] = {{.lines = cmd}, {.lines = olines}};
    char *label = tc_fg(TUI_TC_TOOL_TITLE, "Output");
    secs[1].label = label;
    tc_block_t blk = {.footer_meta = footer,
                      .state = partial ? TC_STATE_PENDING : is_error ? (timed_out ? TC_STATE_WARNING : TC_STATE_ERROR) : TC_STATE_SUCCESS,
                      .sections = secs, .nsections = 2, .width = width, .pad_left = -1, .pad_right = -1, .border_slot = -1};
    tc_output_block(&blk, out);
    free(label);
    strvec_free(&olines);
    strvec_free(&raw_lines);
    free(footer);
    strvec_free(&stats);
    free(display);
    free(output);
    free(raw);
    strvec_free(&cmd);
}

/* ---------- user `!cmd` (chat/bash-execution.ts + execution-shared.ts) ---------- */

void tc_render_bang(const tc_bang_t *b, int width, strvec_t *out) {
    /* buildExecutionFrame: bashMode (dim for `!!`) rules around a bold `$ cmd` header, the output pane
     * (leading blank, muted rows, 10-row tail while collapsed) and the status footer */
    tui_tc_t key = b->exclude ? TUI_TC_DIM : TUI_TC_BASH_MODE;
    sb_t r = SB_INIT;
    for (int i = 0; i < width; i++) sb_append(&r, tc_sym("boxRound.horizontal"));
    char *rule = tc_fg(key, sb_cstr(&r));
    sb_free(&r);
    strvec_push(out, rule);
    char *hdr = xasprintf("$ %s", b->command ? b->command : "");
    char *bold = tc_bold(hdr);
    char *head = tc_fg(key, bold);
    strvec_t lines = {0};
    tc_push_wrapped(&lines, head, MAX(1, width - 2));
    size_t hidden = 0;
    if (b->output && *b->output) {
        strvec_t raw = {0};
        tc_split_lines(b->output, &raw);
        for (size_t i = 0; i < raw.n; i++) {
            char *t = tc_sanitize_line(raw.v[i]);
            free(raw.v[i]);
            raw.v[i] = t;
        }
        size_t start = 0;
        if (!b->expanded && raw.n > 10) {
            hidden = raw.n - 10;
            start = hidden;
        }
        strvec_push(&lines, "");
        for (size_t i = start; i < raw.n; i++) {
            char *m = tc_fg(TUI_TC_MUTED, raw.v[i]);
            tc_push_wrapped(&lines, m, MAX(1, width - 2));
            free(m);
        }
        strvec_free(&raw);
    }
    if (b->running) {
        char *sp = tc_fg(key, tc_spinner_frame(b->spinner));
        char *msg = tc_fg(TUI_TC_MUTED, "Running\xe2\x80\xa6 (Esc to cancel)");
        strvec_push_own(&lines, xasprintf("%s %s", sp, msg));
        free(msg);
        free(sp);
    } else {
        strvec_t parts = {0};
        if (hidden) {
            char *t = xasprintf("\xe2\x80\xa6 %zu more lines (Ctrl+O to expand)", hidden);
            strvec_push_own(&parts, tc_fg(TUI_TC_DIM, t));
            free(t);
        }
        if (b->cancelled) strvec_push_own(&parts, tc_fg(TUI_TC_WARNING, "(cancelled)"));
        else if (b->exit_code != 0) {
            char *t = xasprintf("(exit %d)", b->exit_code);
            strvec_push_own(&parts, tc_fg(TUI_TC_ERROR, t));
            free(t);
        }
        if (parts.n) {
            strvec_push(&lines, "");
            for (size_t i = 0; i < parts.n; i++) strvec_push(&lines, parts.v[i]);
        }
        strvec_free(&parts);
    }
    for (size_t i = 0; i < lines.n; i++) strvec_push_own(out, *lines.v[i] ? xasprintf(" %s", lines.v[i]) : xstrdup(""));
    strvec_push(out, rule);
    strvec_free(&lines);
    free(head);
    free(bold);
    free(hdr);
    free(rule);
}
