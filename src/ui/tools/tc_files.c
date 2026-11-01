/* tc_files.c - read (grouped), write and edit cards (port of omp chat/read-tool-group.ts,
 * tools/write.ts, tools/edit.ts and chrome/diff.ts). */
#include "ui/tools/tc.h"

#include <ctype.h>

#include "tui/ansi.h"
#include "tui/markdown.h"

/* ---------- read group ---------- */

static bool is_sel_chunk(const char *s) {
    if (!*s) return false;
    if (!strcmp(s, "raw") || !strcmp(s, "conflicts")) return true;
    for (const char *p = s; *p; p++)
        if (!strchr("0123456789-+,$ .", *p)) return false;
    return true;
}

/* splitPathAndSel (simplified): trailing `:sel` where sel is a line list / raw */
static void split_path_sel(const char *raw, char **path, char **sel) {
    const char *colon = strrchr(raw, ':');
    *sel = NULL;
    if (!colon || colon == raw || !is_sel_chunk(colon + 1) || strstr(raw, "://")) {
        *path = xstrdup(raw);
        return;
    }
    *path = xstrndup(raw, (size_t)(colon - raw));
    *sel = xstrdup(colon + 1);
}

bool tc_read_groupable(const json_t *args) {
    const char *p = json_get_str(args, "path");
    if (!p) p = json_get_str(args, "file_path");
    if (!p) return false;
    /* internal URL schemes render full cards in omp; filesystem / external targets collapse */
    static const char *internal[] = {"skill://", "rule://", "agent://", "artifact://", "local://", "memory://", "omp://",
                                     "proc://", "cfg://", "xd://", "mcp://", "pr://", "issue://", "jobs://"};
    for (size_t i = 0; i < ARRAY_LEN(internal); i++)
        if (str_starts(p, internal[i])) return false;
    return true;
}

static char *read_status_glyph(const tc_call_t *c) {
    if (!c->has_result || c->partial) return tc_fg(TUI_TC_DIM, tc_sym("status.pending"));
    if (c->is_error) return tc_fg(TUI_TC_ERROR, tc_sym("status.error"));
    if (c->details && json_get_obj(c->details, "suffixResolution")) return tc_fg(TUI_TC_WARNING, tc_sym("status.warning"));
    return tc_fg(TUI_TC_TEXT, tc_sym("status.enabled"));
}

static bool read_ok(const tc_call_t *c) {
    return c->has_result && !c->partial && !c->is_error && !(c->details && json_get_obj(c->details, "suffixResolution"));
}

static char *read_path_display(const tc_call_t *c) {
    const char *raw = json_get_str(c->args, "path");
    if (!raw) raw = json_get_str(c->args, "file_path");
    const json_t *sr = c->details ? json_get_obj(c->details, "suffixResolution") : NULL;
    char *path, *sel;
    split_path_sel(raw ? raw : "", &path, &sel);
    if (sr && json_get_str(sr, "to")) {
        free(path);
        path = xstrdup(json_get_str(sr, "to"));
    }
    char *shown = tc_shorten_path(path);
    sb_t b = SB_INIT;
    if (*shown) tc_fg_sb(&b, TUI_TC_ACCENT, shown);
    else tc_fg_sb(&b, TUI_TC_TOOL_OUTPUT, "\xe2\x80\xa6");
    if (sel) {
        char *s = xasprintf(":%s", sel);
        tc_fg_sb(&b, TUI_TC_ACCENT, s);
        free(s);
    }
    if (sr && json_get_str(sr, "from")) {
        char *f = tc_shorten_path(json_get_str(sr, "from"));
        char *t = xasprintf(" (corrected from %s)", f);
        tc_fg_sb(&b, TUI_TC_DIM, t);
        free(t);
        free(f);
    }
    int64_t conflicts = c->details ? json_get_int(c->details, "conflictCount", 0) : 0;
    if (conflicts > 0) {
        char *t = xasprintf("(\xe2\x9a\xa0 %lld conflict%s)", (long long)conflicts, conflicts == 1 ? "" : "s");
        sb_putc(&b, ' ');
        tc_fg_sb(&b, TUI_TC_WARNING, t);
        free(t);
    }
    free(shown);
    free(path);
    free(sel);
    return sb_detach(&b);
}

void tc_render_read_group(const tc_call_t *calls, size_t n, int width, strvec_t *out) {
    char *title_b = tc_bold("Read");
    char *title = tc_fg(TUI_TC_TOOL_TITLE, title_b);
    strvec_t lines = {0};
    if (n == 0) {
        strvec_push_own(&lines, xasprintf(" %s %s", tc_sym("format.bullet"), title));
    } else if (n == 1) {
        char *st = read_status_glyph(&calls[0]);
        char *pd = read_path_display(&calls[0]);
        char *l = xasprintf(" %s %s %s", st, title, pd);
        strvec_push_own(&lines, tc_trim_end(l));
        free(l);
        free(pd);
        free(st);
    } else {
        char *cnt = xasprintf(" (%zu)", n);
        char *dc = tc_fg(TUI_TC_DIM, cnt);
        strvec_push_own(&lines, xasprintf(" %s %s%s", tc_sym("format.bullet"), title, dc));
        free(dc);
        free(cnt);
        for (size_t i = 0; i < n; i++) {
            char *conn = tc_fg(TUI_TC_DIM, i + 1 == n ? tc_sym("tree.last") : tc_sym("tree.branch"));
            char *pd = read_path_display(&calls[i]);
            char *st = read_ok(&calls[i]) ? xstrdup("") : read_status_glyph(&calls[i]);
            char *l = xasprintf("   %s %s%s%s", conn, st, *st ? " " : "", pd);
            strvec_push_own(&lines, tc_trim_end(l));
            free(l);
            free(st);
            free(pd);
            free(conn);
        }
    }
    for (size_t i = 0; i < lines.n; i++) tc_push_wrapped(out, lines.v[i], width);
    strvec_free(&lines);
    free(title);
    free(title_b);
}

void tc_read(const tc_call_t *c, int width, strvec_t *out) { tc_render_read_group(c, 1, width, out); }

/* ---------- write ---------- */

static char *count_suffix(int64_t n) {
    if (n <= 0) return xstrdup("");
    char *t = xasprintf(" \xc2\xb7 %lld line%s", (long long)n, n == 1 ? "" : "s");
    char *r = tc_fg(TUI_TC_DIM, t);
    free(t);
    return r;
}

static int64_t count_lines(const char *s) {
    if (!s || !*s) return 0;
    return (int64_t)str_count(s, "\n") + 1;
}

static char *strip_cr(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s ? s : ""; *p; p++)
        if (*p != '\r') sb_putc(&b, *p);
    return sb_detach(&b);
}

/* wrapWritePreviewRows: wrap a `<dim gutter> body` row keeping the gutter aligned */
static void wrap_gutter_row(const char *line, int width, strvec_t *out) {
    /* gutter is "\e[..m  N \e[39m" */
    const char *p = line;
    if (*p == 0x1b) {
        const char *m = strchr(p, 'm');
        const char *g = m ? m + 1 : NULL;
        const char *gend = g ? strstr(g, "\x1b[39m") : NULL;
        if (g && gend) {
            bool ok = gend > g;
            for (const char *q = g; q < gend; q++)
                if (!(*q == ' ' || isdigit((unsigned char)*q))) ok = false;
            if (ok) {
                size_t plen = (size_t)(gend + 5 - line);
                int gw = (int)(gend - g);
                char *prefix = xstrndup(line, plen);
                strvec_t wl = {0};
                tui_wrap_text_with_ansi(gend + 5, MAX(1, width - gw), &wl);
                for (size_t i = 0; i < wl.n; i++) {
                    if (i == 0) strvec_push_own(out, xasprintf("%s%s", prefix, wl.v[i]));
                    else {
                        sb_t b = SB_INIT;
                        tui_sb_spaces(&b, gw);
                        sb_append(&b, wl.v[i]);
                        strvec_push_own(out, sb_detach(&b));
                    }
                }
                strvec_free(&wl);
                free(prefix);
                return;
            }
        }
    }
    strvec_push(out, line);
}

static void write_preview(const char *content, const char *lang, bool expanded, strvec_t *out) {
    char *norm = strip_cr(content);
    strvec_t raw = {0};
    tc_split_lines(norm, &raw);
    size_t total = raw.n;
    size_t maxl = expanded ? total : MIN(total, (size_t)6);
    sb_t vis = SB_INIT;
    for (size_t i = 0; i < maxl; i++) {
        if (i) sb_putc(&vis, '\n');
        sb_append(&vis, raw.v[i]);
    }
    strvec_t hl = {0};
    tc_highlight(sb_cstr(&vis), lang, &hl);
    char nb[24];
    int lnw = MAX(3, snprintf(nb, sizeof nb, "%zu", total));
    for (size_t i = 0; i < hl.n; i++) {
        char *g = xasprintf("%*zu ", lnw, i + 1);
        char *gd = tc_fg(TUI_TC_DIM, g);
        char *body = tc_replace_tabs(hl.v[i]);
        strvec_push_own(out, xasprintf("%s%s", gd, body));
        free(body);
        free(gd);
        free(g);
    }
    size_t hidden = total - maxl;
    if (!expanded && hidden > 0) {
        char *m = tc_more_items((int64_t)hidden, "line");
        char *hint = tc_expand_hint();
        char *line = xasprintf("%s %s", m, hint);
        strvec_push_own(out, tc_fg(TUI_TC_DIM, line));
        free(line);
        free(hint);
        free(m);
    }
    strvec_free(&hl);
    sb_free(&vis);
    strvec_free(&raw);
    free(norm);
}

/* `write xd://<tool>`: the device dispatch renders as the mounted tool's own card (omp tools/xdev.ts
 * renderXdevCall / renderXdevResult). Returns false when the write is not a device dispatch. */
static bool write_xdev(const tc_call_t *c, const char *raw, int width, strvec_t *out) {
    if (strncasecmp(raw, "xd://", 5) != 0) return false;
    const char *nm = strstr(raw, "://") + 3;
    size_t nl = strcspn(nm, "/?#");
    if (!nl || nm[nl]) return false; /* bare xd:// and xd://<tool>/<topic> keep the write card */
    char *name = xstrndup(nm, nl);
    if (!strcmp(name, "report_issue") || !strcmp(name, "resolve") || !strcmp(name, "reject") || !strcmp(name, "propose")) {
        free(name);
        return false;
    }
    const json_t *xd = c->has_result ? json_get_obj(c->details, "xdev") : NULL;
    if (c->has_result && !xd) {
        free(name);
        return false;
    }
    const char *label = c->label_of ? c->label_of(name) : NULL;
    tc_call_t ic = *c;
    ic.name = name;
    ic.label = label;
    if (xd) {
        bool ok = true;
        if (str_eq(json_get_str(xd, "mode"), "help")) {
            sb_t tb = SB_INIT;
            const json_t *blk;
            json_arr_foreach(c->content, i, blk) {
                const char *t = str_eq(json_get_str(blk, "type"), "text") ? json_get_str(blk, "text") : NULL;
                if (!t || !*t) continue;
                if (tb.len) sb_putc(&tb, '\n');
                sb_append(&tb, t);
            }
            if (tb.len) {
                char *txt = tc_replace_tabs(sb_cstr(&tb));
                char *col = tc_fg(TUI_TC_TOOL_OUTPUT, txt);
                tc_push_wrapped(out, col, width);
                free(col);
                free(txt);
            } else {
                ok = false;
            }
            sb_free(&tb);
            free(name);
            return ok;
        }
        json_t *empty = json_obj();
        const json_t *ia = json_get_obj(xd, "args");
        ic.args = ia ? ia : empty;
        ic.details = json_get_obj(xd, "inner");
        if (!ic.label) ic.label = name;
        tc_render(&ic, width, out);
        json_free(empty);
        free(name);
        return true;
    }
    /* call phase: nothing until the path settles (content started), queued until execution, then the device card */
    const json_t *cj = json_get(c->args, "content");
    if (!cj) {
        free(name);
        return true;
    }
    const char *content = json_sv(cj);
    json_t *args = content ? json_parse_partial(content, strlen(content)) : NULL;
    if (!json_is_obj(args)) {
        json_free(args);
        args = json_obj();
    }
    ic.args = args;
    ic.content = NULL;
    ic.details = NULL;
    ic.has_result = false;
    if (!c->exec_started) {
        char *ql = xasprintf("queued %s", label ? label : name);
        ic.label = ql;
        ic.partial = true;
        ic.spinner = -1;
        tc_generic(&ic, width, out);
        free(ql);
    } else {
        if (!ic.label) ic.label = name;
        tc_render(&ic, width, out);
    }
    json_free(args);
    free(name);
    return true;
}

void tc_write(const tc_call_t *c, int width, strvec_t *out) {
    const char *raw = json_get_str(c->args, "path");
    if (!raw) raw = json_get_str(c->args, "file_path");
    if (!raw) raw = "";
    if (write_xdev(c, raw, width, out)) return;
    const char *content = json_get_str(c->args, "content");
    char *fp = tc_shorten_path(raw);
    const char *lang = *raw ? tc_lang_from_path(raw) : NULL;
    char *icon = tc_fg(TUI_TC_MUTED, tc_lang_icon(lang ? lang : (*raw ? "text" : NULL)));
    char *path = *fp ? tc_fg(TUI_TC_ACCENT, fp) : tc_fg(TUI_TC_TOOL_OUTPUT, "\xe2\x80\xa6");
    int cw = tc_block_content_width(width, -1, -1);
    strvec_t body = {0};
    char *header;
    tc_state_t state;
    int border = TUI_TC_BORDER_MUTED;
    if (!c->has_result) {
        char *desc = xasprintf("%s %s", icon, path);
        tc_status_line_t sl = {.icon = TC_ST_NONE, .spinner = -1, .title = "Write", .title_slot = -1, .description = desc};
        header = tc_status_line(&sl);
        free(desc);
        if (content && *content) {
            /* formatStreamingContent: tail window of the streamed body + "… (streaming)" */
            char *norm = strip_cr(content);
            strvec_t lines = {0};
            tc_split_lines(norm, &lines);
            size_t total = lines.n, start = c->expanded ? 0 : (total > 12 ? total - 12 : 0);
            sb_t vis = SB_INIT;
            for (size_t i = start; i < total; i++) {
                if (i > start) sb_putc(&vis, '\n');
                sb_append(&vis, lines.v[i]);
            }
            strvec_t hl = {0};
            tc_highlight(sb_cstr(&vis), lang, &hl);
            char nb[24];
            int lnw = MAX(3, snprintf(nb, sizeof nb, "%zu", total));
            if (start > 0) {
                char *t = xasprintf("\xe2\x80\xa6 (%zu earlier line%s)", start, start == 1 ? "" : "s");
                strvec_push_own(&body, tc_fg(TUI_TC_DIM, t));
                free(t);
            }
            for (size_t i = 0; i < hl.n; i++) {
                char *g = xasprintf("%*zu ", lnw, start + i + 1);
                char *gd = tc_fg(TUI_TC_DIM, g);
                char *b2 = tc_replace_tabs(hl.v[i]);
                strvec_push_own(&body, xasprintf("%s%s", gd, b2));
                free(b2);
                free(gd);
                free(g);
            }
            char *spin = c->spinner >= 0 ? xasprintf("%s ", tc_spinner_frame(c->spinner)) : xstrdup("");
            char *st = tc_fg(TUI_TC_DIM, "\xe2\x80\xa6 (streaming)");
            strvec_push_own(&body, xasprintf("%s%s", spin, st));
            free(st);
            free(spin);
            strvec_free(&hl);
            sb_free(&vis);
            strvec_free(&lines);
            free(norm);
        }
        state = TC_STATE_PENDING;
    } else if (c->is_error) {
        char *desc = xasprintf("%s %s", icon, path);
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "Write", .title_slot = -1, .description = desc};
        header = tc_status_line(&sl);
        free(desc);
        char *et = tc_text_of(c->content);
        char *ed = tc_error_detail(et);
        tc_split_lines(ed, &body);
        free(ed);
        free(et);
        state = TC_STATE_ERROR;
        border = TUI_TC_ERROR;
    } else {
        char *suffix = count_suffix(count_lines(content));
        char *exec = !c->partial && json_get_bool(c->details, "madeExecutable", false)
                         ? xasprintf("%s%s", tc_fg(TUI_TC_DIM, " \xc2\xb7 "), tc_fg(TUI_TC_SUCCESS, "made executable!"))
                         : xstrdup("");
        char *desc = xasprintf("%s %s%s%s", icon, path, suffix, exec);
        char *ov = c->partial ? NULL : tc_styled_sym("tool.write", TUI_TC_ACCENT);
        tc_status_line_t sl = {.icon = c->partial ? TC_ST_RUNNING : TC_ST_NONE, .icon_override = ov, .spinner = c->spinner,
                               .title = "Write", .title_slot = -1, .description = desc};
        header = tc_status_line(&sl);
        free(ov);
        free(desc);
        free(exec);
        free(suffix);
        strvec_t prev = {0};
        if (content) write_preview(content, lang, c->expanded, &prev);
        if (c->partial) {
            char *pt = tc_text_of(c->content);
            if (*pt) {
                char *t = tc_replace_tabs(pt);
                char *tr = tui_truncate_to_width(t, 110, NULL);
                strvec_push_own(&body, tc_fg(TUI_TC_MUTED, tr));
                free(tr);
                free(t);
            }
            free(pt);
        }
        for (size_t i = 0; i < prev.n; i++) strvec_push(&body, prev.v[i]);
        strvec_free(&prev);
        state = c->partial ? TC_STATE_PENDING : TC_STATE_SUCCESS;
    }
    while (body.n && !*body.v[0]) {
        free(body.v[0]);
        memmove(body.v, body.v + 1, (body.n - 1) * sizeof(char *));
        body.n--;
    }
    strvec_t wrapped = {0};
    for (size_t i = 0; i < body.n; i++) wrap_gutter_row(body.v[i], cw, &wrapped);
    tc_section_t sec = {.lines = wrapped};
    tc_block_t blk = {.header = header, .state = state, .sections = &sec, .nsections = wrapped.n ? 1 : 0, .width = width,
                      .pad_left = -1, .pad_right = -1, .border_slot = border};
    tc_output_block(&blk, out);
    strvec_free(&wrapped);
    strvec_free(&body);
    free(header);
    free(path);
    free(icon);
    free(fp);
}

/* ---------- diff rendering (chrome/diff.ts) ---------- */

typedef struct {
    char prefix; /* '+', '-', ' ' */
    char *num;   /* may be "" */
    char *content;
} dline_t;

static bool parse_diff_line(const char *line, dline_t *d) {
    if (!line[0] || !strchr("+- ", line[0])) return false;
    const char *p = line + 1;
    const char *q = p;
    while (*q == ' ') q++;
    const char *digits = q;
    while (isdigit((unsigned char)*q)) q++;
    if (q > digits && *q == '|') {
        d->prefix = line[0];
        d->num = xstrndup(p, (size_t)(q - p));
        d->content = xstrdup(q + 1);
        return true;
    }
    /* legacy: "+123 content" or "+content" */
    q = p;
    while (*q == ' ') q++;
    digits = q;
    while (isdigit((unsigned char)*q)) q++;
    d->prefix = line[0];
    if (q > digits && *q == ' ') {
        d->num = xstrndup(p, (size_t)(q - p));
        d->content = xstrdup(q + 1);
    } else {
        d->num = xstrdup("");
        d->content = xstrdup(p);
    }
    return true;
}

static char *visualize_indent(const char *text) {
    size_t n = strspn(text, " \t");
    if (n == 0) return tc_replace_tabs(text);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\t') sb_append(&b, "\x1b[2m \xe2\x86\x92 \x1b[22m");
        else sb_append(&b, "\x1b[2m\xc2\xb7\x1b[22m");
    }
    char *rest = tc_replace_tabs(text + n);
    sb_append(&b, rest);
    free(rest);
    return sb_detach(&b);
}

/* word tokens: runs of word chars, runs of whitespace, single punctuation */
static void tokenize(const char *s, strvec_t *out) {
    const char *p = s;
    while (*p) {
        const char *q = p;
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            while (*q && isspace((unsigned char)*q)) q++;
        } else if (isalnum(c) || c == '_' || c >= 0x80) {
            while (*q && (isalnum((unsigned char)*q) || *q == '_' || (unsigned char)*q >= 0x80)) q++;
        } else q++;
        strvec_push_own(out, xstrndup(p, (size_t)(q - p)));
        p = q;
    }
}

/* renderIntraLineDiff via token LCS; inverse on changed tokens (leading whitespace of the first change kept plain) */
static void intra_line(const char *old_s, const char *new_s, char **rem, char **add) {
    strvec_t a = {0}, b = {0};
    tokenize(old_s, &a);
    tokenize(new_s, &b);
    size_t n = a.n, m = b.n;
    if (n * m > 250000) {
        *rem = tui_inverse(old_s);
        *add = tui_inverse(new_s);
        strvec_free(&a);
        strvec_free(&b);
        return;
    }
    int *L = xcalloc((n + 1) * (m + 1), sizeof(int));
#define LL(i, j) L[(i) * (m + 1) + (j)]
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            LL(i, j) = !strcmp(a.v[i], b.v[j]) ? LL(i + 1, j + 1) + 1 : MAX(LL(i + 1, j), LL(i, j + 1));
    sb_t rb = SB_INIT, ab = SB_INIT;
    bool first_r = true, first_a = true;
    size_t i = 0, j = 0;
    /* group consecutive removed/added tokens into parts like jsdiff */
    sb_t pr = SB_INIT, pa = SB_INIT;
#define FLUSH_PARTS()                                                         \
    do {                                                                      \
        if (pr.len) {                                                         \
            const char *v = sb_cstr(&pr);                                     \
            if (first_r) {                                                    \
                size_t ws = strspn(v, " \t");                                 \
                sb_appendn(&rb, v, ws);                                       \
                v += ws;                                                      \
                first_r = false;                                              \
            }                                                                 \
            if (*v) {                                                         \
                char *iv = tui_inverse(v);                                    \
                sb_append(&rb, iv);                                           \
                free(iv);                                                     \
            }                                                                 \
            sb_clear(&pr);                                                    \
        }                                                                     \
        if (pa.len) {                                                         \
            const char *v = sb_cstr(&pa);                                     \
            if (first_a) {                                                    \
                size_t ws = strspn(v, " \t");                                 \
                sb_appendn(&ab, v, ws);                                       \
                v += ws;                                                      \
                first_a = false;                                              \
            }                                                                 \
            if (*v) {                                                         \
                char *iv = tui_inverse(v);                                    \
                sb_append(&ab, iv);                                           \
                free(iv);                                                     \
            }                                                                 \
            sb_clear(&pa);                                                    \
        }                                                                     \
    } while (0)
    while (i < n || j < m) {
        if (i < n && j < m && !strcmp(a.v[i], b.v[j])) {
            FLUSH_PARTS();
            sb_append(&rb, a.v[i]);
            sb_append(&ab, b.v[j]);
            i++;
            j++;
        } else if (j < m && (i == n || LL(i, j + 1) >= LL(i + 1, j))) {
            sb_append(&pa, b.v[j++]);
        } else {
            sb_append(&pr, a.v[i++]);
        }
    }
    FLUSH_PARTS();
#undef FLUSH_PARTS
#undef LL
    free(L);
    sb_free(&pr);
    sb_free(&pa);
    *rem = sb_detach(&rb);
    *add = sb_detach(&ab);
    strvec_free(&a);
    strvec_free(&b);
}

static char *code_frame(char marker, const char *num, const char *content, int lnw) {
    char *mt = marker == ' ' ? xstrdup("") : xasprintf("%c", marker);
    char *nt = str_trim(num);
    char *g = *mt && *nt ? xasprintf("%s%s", mt, nt) : *nt ? xstrdup(nt) : xstrdup(mt);
    char *r = xasprintf("%*s\xe2\x94\x82%s", lnw + 1, g, content);
    free(g);
    free(nt);
    free(mt);
    return r;
}

void tc_render_diff(const char *diff, const char *path, strvec_t *out) {
    strvec_t lines = {0};
    tc_split_lines(diff ? diff : "", &lines);
    int lnw = 3;
    VEC(dline_t) pl = {0};
    VEC(bool) okv = {0};
    for (size_t i = 0; i < lines.n; i++) {
        dline_t d = {0};
        bool ok = parse_diff_line(lines.v[i], &d);
        if (ok) {
            char *t = str_trim(d.num);
            lnw = MAX(lnw, (int)strlen(t));
            free(t);
        }
        vec_push(&pl, d);
        vec_push(&okv, ok);
    }
    /* context highlighting by language */
    const char *lang = path ? tc_lang_from_path(path) : NULL;
    char **ctx_hl = xcalloc(lines.n + 1, sizeof(char *));
    if (lang && strcmp(lang, "text")) {
        size_t i = 0;
        while (i < lines.n) {
            if (!okv.v[i] || pl.v[i].prefix != ' ' || !strcmp(pl.v[i].content, "...") || !strcmp(pl.v[i].content, "\xe2\x80\xa6")) {
                i++;
                continue;
            }
            size_t j = i;
            sb_t run = SB_INIT;
            while (j < lines.n && okv.v[j] && pl.v[j].prefix == ' ' && strcmp(pl.v[j].content, "...") &&
                   strcmp(pl.v[j].content, "\xe2\x80\xa6")) {
                if (j > i) sb_putc(&run, '\n');
                sb_append(&run, pl.v[j].content);
                j++;
            }
            if (tui_highlight_supported(lang)) {
                strvec_t hl = {0};
                tc_highlight(sb_cstr(&run), lang, &hl);
                for (size_t k = i; k < j && k - i < hl.n; k++) ctx_hl[k] = xstrdup(hl.v[k - i]);
                strvec_free(&hl);
            }
            sb_free(&run);
            i = j;
        }
    }
    char prev[32] = "";
    size_t i = 0;
    while (i < lines.n) {
        if (!okv.v[i]) {
            prev[0] = 0;
            char *t = str_trim(lines.v[i]);
            bool gap = !*t || !strcmp(t, "...") || !strcmp(t, "\xe2\x80\xa6");
            char *rt = tc_replace_tabs(lines.v[i]);
            strvec_push_own(out, tc_fg(TUI_TC_TOOL_DIFF_CONTEXT, gap ? "\xe2\x80\xa6" : rt));
            free(rt);
            free(t);
            i++;
            continue;
        }
        dline_t *d = &pl.v[i];
        if (d->prefix == '-') {
            size_t rs = i;
            while (i < lines.n && okv.v[i] && pl.v[i].prefix == '-') i++;
            size_t re = i, as = i;
            while (i < lines.n && okv.v[i] && pl.v[i].prefix == '+') i++;
            size_t ae = i;
            if (re - rs == 1 && ae - as == 1) {
                char *ro = tc_replace_tabs(pl.v[rs].content), *ao = tc_replace_tabs(pl.v[as].content);
                char *rl, *al;
                intra_line(ro, ao, &rl, &al);
                char *rv = visualize_indent(rl), *av = visualize_indent(al);
                char *l1 = NULL, *l2 = NULL;
                /* line-number dedup (single-line replacement repeats N) */
                {
                    char *tn = str_trim(pl.v[rs].num);
                    if (!*tn) {
                        prev[0] = 0;
                        l1 = xasprintf("-%s", rv);
                    } else {
                        const char *dn = !strcmp(tn, prev) ? "" : tn;
                        l1 = code_frame('-', dn, rv, lnw);
                        snprintf(prev, sizeof prev, "%s", tn);
                    }
                    free(tn);
                    tn = str_trim(pl.v[as].num);
                    if (!*tn) {
                        prev[0] = 0;
                        l2 = xasprintf("+%s", av);
                    } else {
                        const char *dn = !strcmp(tn, prev) ? "" : tn;
                        l2 = code_frame('+', dn, av, lnw);
                        snprintf(prev, sizeof prev, "%s", tn);
                    }
                    free(tn);
                }
                strvec_push_own(out, tc_fg(TUI_TC_TOOL_DIFF_REMOVED, l1));
                strvec_push_own(out, tc_fg(TUI_TC_TOOL_DIFF_ADDED, l2));
                free(l1);
                free(l2);
                free(rv);
                free(av);
                free(rl);
                free(al);
                free(ro);
                free(ao);
            } else {
                for (size_t k = rs; k < ae; k++) {
                    char *v = visualize_indent(pl.v[k].content);
                    char *tn = str_trim(pl.v[k].num);
                    char *l;
                    if (!*tn) {
                        prev[0] = 0;
                        l = xasprintf("%c%s", pl.v[k].prefix, v);
                    } else {
                        l = code_frame(pl.v[k].prefix, !strcmp(tn, prev) ? "" : tn, v, lnw);
                        snprintf(prev, sizeof prev, "%s", tn);
                    }
                    strvec_push_own(out, tc_fg(pl.v[k].prefix == '-' ? TUI_TC_TOOL_DIFF_REMOVED : TUI_TC_TOOL_DIFF_ADDED, l));
                    free(l);
                    free(tn);
                    free(v);
                }
            }
            continue;
        }
        char *v = d->prefix == ' ' && ctx_hl[i] ? tc_replace_tabs(ctx_hl[i]) : visualize_indent(d->content);
        char *tn = str_trim(d->num);
        char *l;
        if (!*tn) {
            prev[0] = 0;
            l = xasprintf("%c%s", d->prefix, v);
        } else {
            l = code_frame(d->prefix, !strcmp(tn, prev) ? "" : tn, v, lnw);
            snprintf(prev, sizeof prev, "%s", tn);
        }
        strvec_push_own(out, tc_fg(d->prefix == '+' ? TUI_TC_TOOL_DIFF_ADDED : TUI_TC_TOOL_DIFF_CONTEXT, l));
        free(l);
        free(tn);
        free(v);
        i++;
    }
    for (size_t k = 0; k < pl.n; k++) {
        free(pl.v[k].num);
        free(pl.v[k].content);
        free(ctx_hl[k]);
    }
    free(ctx_hl);
    vec_free(&pl);
    vec_free(&okv);
    strvec_free(&lines);
}

/* ---------- edit ---------- */

static void diff_stats(const char *diff, int *added, int *removed, int *hunks) {
    strvec_t l = {0};
    tc_split_lines(diff, &l);
    *added = *removed = *hunks = 0;
    bool in = false;
    for (size_t i = 0; i < l.n; i++) {
        bool a = l.v[i][0] == '+', r = l.v[i][0] == '-';
        if (a) (*added)++;
        if (r) (*removed)++;
        if ((a || r) && !in) {
            (*hunks)++;
            in = true;
        } else if (!(a || r)) in = false;
    }
    strvec_free(&l);
}

static char *stats_suffix(const char *diff) {
    int a, r, h;
    diff_stats(diff, &a, &r, &h);
    if (!a && !r) return xstrdup("");
    sb_t b = SB_INIT;
    sb_putc(&b, ' ');
    tc_fg_sb(&b, TUI_TC_DIM, tc_sym("format.bracketLeft"));
    if (a) {
        char *t = xasprintf("+%d", a);
        tc_fg_sb(&b, TUI_TC_TOOL_DIFF_ADDED, t);
        free(t);
    }
    if (a && r) tc_fg_sb(&b, TUI_TC_DIM, "/");
    if (r) {
        char *t = xasprintf("-%d", r);
        tc_fg_sb(&b, TUI_TC_TOOL_DIFF_REMOVED, t);
        free(t);
    }
    tc_fg_sb(&b, TUI_TC_DIM, tc_sym("format.bracketRight"));
    return sb_detach(&b);
}

/* truncateEditTitlePath: middle ellipsis */
static char *mid_trunc(const char *s, int maxw) {
    int w = tui_visible_width(s);
    if (maxw < 0 || w <= maxw) return xstrdup(s);
    int cw = maxw - 1;
    if (cw <= 0) return xstrdup("\xe2\x80\xa6");
    int head = cw / 2, tail = cw - head;
    char *h = tui_slice_cols(s, 0, head), *t = tui_slice_cols(s, w - tail, tail);
    char *r = xasprintf("%s\xe2\x80\xa6%s", h, t);
    free(h);
    free(t);
    return r;
}

static char *edit_header(int width, const char *title, tc_status_t icon, const char *icon_override, const char *raw_path,
                         const char *rename, const char *suffix) {
    const char *lang = tc_lang_from_path(raw_path);
    char *ic = tc_fg(TUI_TC_MUTED, tc_lang_icon(lang ? lang : "text"));
    char *res = NULL;
    /* build; on overflow rebuild with the path middle-truncated to fit (renderEditHeader) */
    char *shown = tc_shorten_path(raw_path);
    char *tabs = tc_replace_tabs(shown);
    int budget = MAX(0, width - 4 - 1 - 2);
    int pathw = tui_visible_width(tabs);
    for (int attempt = 0; attempt < 2; attempt++) {
        int cap = -1;
        if (attempt == 1) {
            int overflow = tui_visible_width(res) - budget;
            free(res);
            res = NULL;
            int count = MAX(1, (*raw_path ? 1 : 0) + (rename ? 1 : 0));
            cap = MAX(1, (pathw - overflow) / count);
        }
        char *pd = *raw_path ? mid_trunc(tabs, cap) : xstrdup("\xe2\x80\xa6");
        sb_t d = SB_INIT;
        sb_append(&d, ic);
        sb_putc(&d, ' ');
        tc_fg_sb(&d, *raw_path ? TUI_TC_ACCENT : TUI_TC_TOOL_OUTPUT, pd);
        if (rename) {
            char *rs = tc_shorten_path(rename);
            char *rt = mid_trunc(rs, cap);
            sb_putc(&d, ' ');
            tc_fg_sb(&d, TUI_TC_DIM, "\xe2\x86\x92");
            sb_putc(&d, ' ');
            tc_fg_sb(&d, TUI_TC_ACCENT, rt);
            free(rt);
            free(rs);
        }
        tc_status_line_t sl = {.icon = icon, .icon_override = icon_override, .spinner = -1, .title = title, .title_slot = -1,
                               .description = sb_cstr(&d)};
        char *line = tc_status_line(&sl);
        res = xasprintf("%s%s", line, suffix ? suffix : "");
        free(line);
        sb_free(&d);
        free(pd);
        if (attempt == 0 && (tui_visible_width(res) - budget <= 0 || pathw <= 1)) break;
    }
    free(tabs);
    free(shown);
    free(ic);
    return res;
}

/* wrapEditRendererLine: wrap diff rows keeping the `N│` gutter */
static void wrap_edit_line(const char *line, int width, strvec_t *out) {
    if (!*line) {
        strvec_push(out, "");
        return;
    }
    /* leading SGR */
    const char *p = line;
    while (p[0] == 0x1b && p[1] == '[') {
        const char *m = p + 2;
        while (*m && (isdigit((unsigned char)*m) || *m == ';')) m++;
        if (*m != 'm') break;
        p = m + 1;
    }
    char *start = xstrndup(line, (size_t)(p - line));
    char *body = xstrdup(p);
    size_t bl = strlen(body);
    if (bl >= 5 && !strcmp(body + bl - 5, "\x1b[39m")) body[bl - 5] = 0;
    /* gutter: spaces, optional +/-, spaces, digits, then │ */
    const char *q = body;
    while (*q == ' ') q++;
    if (*q == '+' || *q == '-') q++;
    while (*q == ' ') q++;
    while (isdigit((unsigned char)*q)) q++;
    if (q == body || !str_starts(q, "\xe2\x94\x82")) {
        tui_wrap_text_with_ansi(line, MAX(1, width), out);
        free(start);
        free(body);
        return;
    }
    char *prefix = xstrndup(body, (size_t)(q - body) + 3);
    int pw = tui_visible_width(prefix);
    strvec_t wl = {0};
    tui_wrap_text_with_ansi(q + 3, MAX(1, width - pw), &wl);
    for (size_t i = 0; i < wl.n; i++) {
        if (i == 0) strvec_push_own(out, xasprintf("%s%s%s\x1b[27m\x1b[39m", start, prefix, wl.v[i]));
        else {
            sb_t b = SB_INIT;
            sb_append(&b, start);
            tui_sb_spaces(&b, MAX(0, pw - 1));
            sb_append(&b, "\xe2\x94\x82");
            sb_append(&b, wl.v[i]);
            sb_append(&b, "\x1b[27m\x1b[39m");
            strvec_push_own(out, sb_detach(&b));
        }
    }
    strvec_free(&wl);
    free(prefix);
    free(start);
    free(body);
}

/* truncateDiffByHunk (head mode) */
static char *truncate_diff(const char *diff, int max_hunks, int max_lines, int *hidden_hunks, int *hidden_lines) {
    strvec_t l = {0};
    tc_split_lines(diff, &l);
    int a, r, hunks;
    diff_stats(diff, &a, &r, &hunks);
    *hidden_hunks = *hidden_lines = 0;
    if ((int)l.n <= max_lines && hunks <= max_hunks) {
        strvec_free(&l);
        return xstrdup(diff);
    }
    /* keep whole segments until budget; change segments count toward hunks */
    strvec_t kept = {0};
    int kept_hunks = 0;
    size_t i = 0;
    while (i < l.n && (int)kept.n < max_lines) {
        bool ch = l.v[i][0] == '+' || l.v[i][0] == '-';
        size_t j = i;
        while (j < l.n && ((l.v[j][0] == '+' || l.v[j][0] == '-') == ch)) j++;
        if (ch) {
            if (kept_hunks >= max_hunks) break;
            kept_hunks++;
        }
        for (size_t k = i; k < j && (int)kept.n < max_lines; k++) strvec_push(&kept, l.v[k]);
        i = j;
    }
    *hidden_hunks = MAX(0, hunks - kept_hunks);
    *hidden_lines = MAX(0, (int)l.n - (int)kept.n);
    char *res = strvec_join(&kept, "\n");
    strvec_free(&kept);
    strvec_free(&l);
    return res;
}

void tc_edit(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *d = c->details;
    const char *detail_path = json_get_str(d, "path");
    const char *raw_path = json_get_str(d, "sourcePath");
    if (!raw_path) raw_path = json_get_str(c->args, "path");
    if (!raw_path) raw_path = json_get_str(c->args, "file_path");
    char *hl_path = NULL;
    if (!raw_path && detail_path) raw_path = detail_path;
    if (!raw_path) {
        /* first `[PATH#TAG]` header of hashline input */
        const char *in = json_get_str(c->args, "input");
        const char *lb = in ? strchr(in, '[') : NULL;
        const char *hash = lb ? strpbrk(lb, "#]") : NULL;
        if (lb && hash) hl_path = xstrndup(lb + 1, (size_t)(hash - lb - 1));
        raw_path = hl_path ? hl_path : "";
    }
    const char *op = json_get_str(c->args, "op");
    if (!op) op = json_get_str(d, "op");
    const char *rename = json_get_str(c->args, "rename");
    if (!rename) rename = json_get_str(d, "move");
    const char *title = str_eq(op, "create") ? "Create" : str_eq(op, "delete") ? "Delete" : "Edit";
    const char *diff = c->is_error ? NULL : json_get_str(d, "diff");
    if (diff && !*diff) diff = NULL;

    if (c->has_result && !c->partial && !c->is_error && !diff && (str_eq(op, "delete") || rename)) {
        bool del = str_eq(op, "delete");
        char *ov = tc_styled_sym(del ? "tool.delete" : "tool.move", TUI_TC_ACCENT);
        char *h = edit_header(width, del ? "Delete" : "Move", TC_ST_NONE, ov, raw_path, rename, NULL);
        tc_push_wrapped(out, h, width);
        free(h);
        free(ov);
        free(hl_path);
        return;
    }
    if (!c->has_result) {
        /* pending call: header + hourglass, no body (omc applies the edit at once) */
        char *h = edit_header(width, title, TC_ST_PENDING, NULL, raw_path, rename, NULL);
        tc_block_t blk = {.header = h, .state = TC_STATE_PENDING, .width = width, .pad_left = 0, .pad_right = -1,
                          .border_slot = TUI_TC_BORDER_MUTED};
        tc_output_block(&blk, out);
        free(h);
        free(hl_path);
        return;
    }
    char *suffix = diff ? stats_suffix(diff) : xstrdup("");
    char *ov = !c->is_error && !c->partial ? tc_styled_sym("tool.edit", TUI_TC_ACCENT) : NULL;
    char *header = edit_header(width, title, c->is_error ? TC_ST_ERROR : TC_ST_SUCCESS, ov, raw_path, rename, suffix);
    int inner = MAX(1, width - 2);
    strvec_t body = {0};
    if (c->is_error) {
        const char *et = json_get_str(d, "displayErrorText");
        char *t = et ? xstrdup(et) : json_get_str(d, "errorText") ? xstrdup(json_get_str(d, "errorText")) : tc_text_of(c->content);
        if (*t) {
            char *rt = tc_replace_tabs(t);
            strvec_t el = {0};
            tc_split_lines(rt, &el);
            for (size_t i = 0; i < el.n; i++) strvec_push_own(&body, tc_fg(TUI_TC_ERROR, el.v[i]));
            strvec_free(&el);
            free(rt);
        }
        free(t);
    } else if (diff) {
        int hh = 0, hl = 0;
        char *shown = c->expanded ? xstrdup(diff) : truncate_diff(diff, 8, 40, &hh, &hl);
        strvec_t rd = {0};
        tc_render_diff(shown, raw_path, &rd);
        strvec_t rows = {0};
        size_t complete = 0;
        for (size_t i = 0; i < rd.n; i++) {
            strvec_t w = {0};
            wrap_edit_line(rd.v[i], inner, &w);
            if (c->expanded || rows.n + w.n <= 40) {
                for (size_t k = 0; k < w.n; k++) strvec_push(&rows, w.v[k]);
                complete++;
                strvec_free(&w);
                continue;
            }
            for (size_t k = 0; rows.n < 40 && k < w.n; k++) strvec_push(&rows, w.v[k]);
            strvec_free(&w);
            break;
        }
        if (!c->expanded) hl += (int)(rd.n - complete);
        for (size_t i = 0; i < rows.n; i++) strvec_push(&body, rows.v[i]);
        if (!c->expanded && (hh > 0 || hl > 0)) {
            sb_t r = SB_INIT;
            if (hh > 0) sb_appendf(&r, "%d more hunks", hh);
            if (hl > 0) sb_appendf(&r, "%s%d more lines", hh > 0 ? ", " : "", hl);
            char *t = xasprintf("\xe2\x80\xa6 (%s) ", sb_cstr(&r));
            char *hint = tc_expand_hint();
            char *tt = xasprintf("%s%s", t, hint);
            strvec_push_own(&body, tc_fg(TUI_TC_TOOL_OUTPUT, tt));
            free(tt);
            free(hint);
            free(t);
            sb_free(&r);
        }
        strvec_free(&rows);
        strvec_free(&rd);
        free(shown);
    } else if (d && !str_eq(op, "delete") && !str_eq(op, "create") && !rename) {
        char *p = tc_shorten_path(detail_path ? detail_path : raw_path);
        char *t = *p ? xasprintf("No changes were made to %s.", p) : xstrdup("No changes were made.");
        strvec_push_own(&body, tc_fg(TUI_TC_DIM, t));
        free(t);
        free(p);
    }
    /* body lines self-wrap; error/no-change rows wrap generically */
    strvec_t wrapped = {0};
    for (size_t i = 0; i < body.n; i++) {
        if (diff && !c->is_error) strvec_push(&wrapped, body.v[i]);
        else tui_wrap_text_with_ansi(body.v[i], inner, &wrapped);
    }
    while (wrapped.n && !*wrapped.v[0]) {
        free(wrapped.v[0]);
        memmove(wrapped.v, wrapped.v + 1, (wrapped.n - 1) * sizeof(char *));
        wrapped.n--;
    }
    tc_section_t sec = {.lines = wrapped};
    tc_block_t blk = {.header = header, .state = c->is_error ? TC_STATE_ERROR : c->partial ? TC_STATE_PENDING : TC_STATE_SUCCESS,
                      .sections = &sec, .nsections = wrapped.n ? 1 : 0, .width = width, .pad_left = 0, .pad_right = -1,
                      .border_slot = c->is_error ? TUI_TC_ERROR : TUI_TC_BORDER_MUTED};
    tc_output_block(&blk, out);
    strvec_free(&wrapped);
    strvec_free(&body);
    free(header);
    free(ov);
    free(suffix);
    free(hl_path);
}
