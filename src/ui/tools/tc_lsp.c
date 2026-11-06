/* tc_lsp.c - lsp card (port of omp tools/lsp.ts renderCall/renderResult): one status line carrying the request and the
 * result counts, over the result rows. The response kind is detected from its text: `Found N definition(s):` locations,
 * `Found N reference(s):` grouped by file with source previews, ```lang hover blocks, diagnostics, `Symbols in <file>:` trees,
 * otherwise a line-by-line list. */
#include "ui/tools/tc.h"

#include <ctype.h>
#include <strings.h>

#include "tui/ansi.h"
#include "tui/markdown.h"

static char *flat(const char *s) {
    char *t = tc_replace_tabs(s ? s : "");
    sb_t b = SB_INIT;
    for (char *p = t; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        sb_putc(&b, *p == '\n' || *p == '\r' ? ' ' : *p);
    }
    free(t);
    return sb_detach(&b);
}

static char *action_label(const char *a) {
    char *r = xstrdup(a ? a : "request");
    for (char *p = r; *p; p++)
        if (*p == '_') *p = ' ';
    return r;
}

/* lspRequestStatus: `action target` as the description (`references src/a.ts:42 (validateToken)`), the call facts as meta */
static void lsp_request_status(const json_t *a, const char *fallback, char **desc, strvec_t *meta) {
    char *act = action_label(json_get_str(a, "action") ? json_get_str(a, "action") : fallback);
    const char *q = json_get_str(a, "query");
    char *qp = q && *q ? tui_truncate_to_width(q, 40, NULL) : NULL;
    char *sp = NULL;
    if (json_get_str(a, "symbol") && *json_get_str(a, "symbol")) {
        char *f = flat(json_get_str(a, "symbol"));
        sp = tui_truncate_to_width(f, 40, NULL);
        free(f);
    }
    char *target = NULL;
    bool has_line = json_get(a, "line") != NULL;
    char *ln = has_line ? json_dump(json_get(a, "line"), 0) : NULL;
    if (json_get_str(a, "file") && *json_get_str(a, "file")) {
        char *s = tc_shorten_path(json_get_str(a, "file"));
        if (has_line) target = sp ? xasprintf("%s:%s (%s)", s, ln, sp) : xasprintf("%s:%s", s, ln);
        else target = xstrdup(s);
        free(s);
    } else if (has_line) target = sp ? xasprintf("line %s (%s)", ln, sp) : xasprintf("line %s", ln);
    if (qp && target) strvec_push_own(meta, xasprintf("query:%s", qp));
    if (json_get_str(a, "new_name") && *json_get_str(a, "new_name")) strvec_push_own(meta, xasprintf("new:%s", json_get_str(a, "new_name")));
    if (json_get(a, "apply")) strvec_push_own(meta, xasprintf("apply:%s", json_get_bool(a, "apply", false) ? "true" : "false"));
    *desc = target ? xasprintf("%s %s", act, target) : qp ? xasprintf("%s %s", act, qp) : xstrdup(act);
    free(ln);
    free(target);
    free(sp);
    free(qp);
    free(act);
}

/* renderCall: `LSP: <action> <file:line (symbol)>` */
static void lsp_call(const tc_call_t *c, int width, strvec_t *out) {
    char *desc = NULL;
    strvec_t meta = {0};
    lsp_request_status(c->args, NULL, &desc, &meta);
    tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "LSP", .title_slot = -1, .description = desc,
                           .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *h = tc_status_line(&sl);
    tc_push_wrapped(out, h, width);
    free(h);
    free(desc);
    strvec_free(&meta);
}

static tui_tc_t sev_color(const char *s) {
    return str_eq(s, "error") ? TUI_TC_ERROR : str_eq(s, "warning") ? TUI_TC_WARNING : str_eq(s, "info") ? TUI_TC_ACCENT : TUI_TC_DIM;
}

/* parseDiagnosticMessage: `file:line:col [sev] [source]? message (code)?` */
typedef struct {
    char *file, *sev, *source, *msg, *code;
    int line, col;
} diag_t;

static bool parse_diag(const char *s, diag_t *d) {
    memset(d, 0, sizeof *d);
    const char *br = strstr(s, " [");
    while (br) {
        /* back up over `:line:col` and whitespace */
        const char *e = br;
        while (e > s && isspace((unsigned char)e[-1])) e--;
        const char *p = e;
        while (p > s && isdigit((unsigned char)p[-1])) p--;
        if (p < e && p > s && p[-1] == ':') {
            const char *c2 = p - 1, *q = c2;
            while (q > s && isdigit((unsigned char)q[-1])) q--;
            if (q < c2 && q > s + 1 && q[-1] == ':') {
                const char *sv = br + 2, *se = strchr(sv, ']');
                if (se && se > sv) {
                    bool word = true;
                    for (const char *x = sv; x < se; x++) word &= isalnum((unsigned char)*x) || *x == '_';
                    if (word) {
                        d->file = xstrndup(s, (size_t)(q - 1 - s));
                        d->line = atoi(q);
                        d->col = atoi(p);
                        d->sev = xstrndup(sv, (size_t)(se - sv));
                        const char *rest = se + 1;
                        while (isspace((unsigned char)*rest)) rest++;
                        if (*rest == '[') {
                            const char *re = strchr(rest, ']');
                            if (re && re[1] && isspace((unsigned char)re[1])) {
                                d->source = xstrndup(rest + 1, (size_t)(re - rest - 1));
                                rest = re + 1;
                                while (isspace((unsigned char)*rest)) rest++;
                            }
                        }
                        size_t rl = strlen(rest);
                        if (rl && rest[rl - 1] == ')') {
                            const char *op = strrchr(rest, '(');
                            if (op && op > rest && isspace((unsigned char)op[-1])) {
                                d->code = xstrndup(op + 1, rl - (size_t)(op - rest) - 2);
                                const char *me = op;
                                while (me > rest && isspace((unsigned char)me[-1])) me--;
                                d->msg = xstrndup(rest, (size_t)(me - rest));
                            }
                        }
                        if (!d->msg) d->msg = xstrdup(rest);
                        if (!*d->msg) {
                            free(d->msg);
                            d->msg = NULL;
                            free(d->file), free(d->sev), free(d->source), free(d->code);
                            memset(d, 0, sizeof *d);
                            return false;
                        }
                        return true;
                    }
                }
            }
        }
        br = strstr(br + 2, " [");
    }
    return false;
}

static void diag_free(diag_t *d) {
    free(d->file);
    free(d->sev);
    free(d->source);
    free(d->msg);
    free(d->code);
}

static char *diag_message(const diag_t *d) {
    return xasprintf("%s%s%s%s%s%s", d->source ? "[" : "", d->source ? d->source : "", d->source ? "] " : "", d->msg,
                     d->code ? " (" : "", d->code ? d->code : "");
}

static char *with_code_close(char *m, const diag_t *d) {
    if (!d->code) return m;
    char *r = xasprintf("%s)", m);
    free(m);
    return r;
}

/* one of the LSP tool's per-file status rows (`<glyph> <path>: …`): only a row's start counts, since
 * response text such as source or hover content can hold the same characters (ASCII `[-]`) */
static bool is_status_row(const char *line, const char *glyph) {
    size_t n = strlen(glyph);
    return !strncmp(line, glyph, n) && line[n] == ' ';
}

/* Only the diagnostics action writes status rows; any other action's text is the server's (a `request`
 * response verbatim), so a glyph there is content even at a line start. An unknown action keeps the inference. */
static bool has_status_row(const char *action, const strvec_t *lines, const char *glyph) {
    if (action && !str_eq(action, "diagnostics")) return false;
    for (size_t i = 0; i < lines->n; i++)
        if (is_status_row(lines->v[i], glyph)) return true;
    return false;
}

/* formatDiagnosticLocation: the muted file-type icon, the styled path and the muted `:line:col` */
static char *diag_location(const diag_t *d) {
    char *ic = tc_fg(TUI_TC_MUTED, tc_lang_icon(tc_lang_from_path(d->file)));
    char *p = tc_styled_path(d->file);
    char *pos0 = xasprintf(":%d:%d", d->line, d->col);
    char *pos = tc_fg(TUI_TC_MUTED, pos0);
    char *r = xasprintf("%s %s%s", ic, p, pos);
    free(pos);
    free(pos0);
    free(p);
    free(ic);
    return r;
}

/* formatSeverityTag: the `[severity]` tag in its severity's color: the path no longer carries it */
static char *severity_tag(const char *sev) {
    char *t = xasprintf("[%s]", sev);
    char *r = tc_fg(sev_color(sev), t);
    free(t);
    return r;
}

static char *icon_lsp(void) { return tc_styled_sym("tool.lsp", TUI_TC_ACCENT); }

/* wrapTreeLine / wrapGuidedLine: hang a tree row beneath its connector, or a connector-less detail row beneath its rails */
static void wrap_guided(const char *line, int width, strvec_t *out) {
    const char *vertical = tc_sym("tree.vertical");
    const char *const conns[2] = {tc_sym("tree.branch"), tc_sym("tree.last")};
    for (int i = 0; i < 2; i++) {
        char *marker = tc_fg(TUI_TC_DIM, conns[i]);
        const char *at = strstr(line, marker);
        size_t ml = strlen(marker);
        free(marker);
        if (!at) continue;
        char *before = xstrndup(line, (size_t)(at - line));
        char *plain = tui_strip_ansi(before);
        bool guides = true;
        for (const char *p = plain; *p && guides;) {
            if (*p == ' ') p++;
            else if (*vertical && str_starts(p, vertical)) p += strlen(vertical);
            else guides = false;
        }
        free(plain);
        size_t end = (size_t)(at - line) + ml + 1;
        if (!guides || strlen(line) < end || line[end - 1] != ' ') {
            free(before);
            continue;
        }
        int cw = tui_visible_width(conns[i]);
        sb_t cont = SB_INIT;
        sb_append(&cont, before);
        if (i == 1) tui_sb_spaces(&cont, cw);
        else {
            tc_fg_sb(&cont, TUI_TC_DIM, vertical);
            tui_sb_spaces(&cont, MAX(0, cw - tui_visible_width(vertical)));
        }
        sb_putc(&cont, ' ');
        char *pre = xstrndup(line, end);
        tc_wrap_prefixed(pre, sb_cstr(&cont), line + end, width, out);
        free(pre);
        sb_free(&cont);
        free(before);
        return;
    }
    /* guideRunEnd: the leading run of spaces, vertical guides and SGR escapes */
    const char *p = line;
    for (;;) {
        if (*p == ' ') p++;
        else if (*vertical && str_starts(p, vertical)) p += strlen(vertical);
        else if (p[0] == 0x1b && p[1] == '[') {
            const char *q = p + 2;
            while (isdigit((unsigned char)*q) || *q == ';') q++;
            if (*q != 'm') break;
            p = q + 1;
        } else break;
    }
    char *pre = xstrndup(line, (size_t)(p - line));
    tc_wrap_prefixed(pre, pre, p, width, out);
    free(pre);
}

/* one view of an LSP response: the header's status and counts, the body rows for one expansion state
 * (LspResultView + its rows()) */
typedef struct {
    tc_status_t icon;     /* TC_ST_SUCCESS shows the LSP icon */
    const char *action;   /* names the header when the request is unknown */
    strvec_t meta;
    strvec_t rows;
    bool hidden;          /* the rows hold something back */
    bool wrap_collapsed;  /* rows wrap while collapsed too (hover) */
} view_t;

/* ---------- hover ---------- */

/* /```(\w*)\n([\s\S]*?)```/ */
static bool find_code_block(const char *text, const char **start, const char **lang, size_t *lang_n, const char **code, size_t *code_n,
                            const char **end) {
    for (const char *f = strstr(text, "```"); f; f = strstr(f + 1, "```")) {
        const char *p = f + 3;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        if (*p != '\n') continue;
        const char *close = strstr(p + 1, "```");
        if (!close) return false;
        *start = f;
        *lang = f + 3;
        *lang_n = (size_t)(p - f - 3);
        *code = p + 1;
        *code_n = (size_t)(close - p - 1);
        *end = close + 3;
        return true;
    }
    return false;
}

static void doc_rows(const char *doc, strvec_t *out) {
    if (!*doc) return;
    strvec_t l = {0};
    tc_split_lines(doc, &l);
    for (size_t i = 0; i < l.n; i++) {
        char *t = tc_replace_tabs(l.v[i]);
        strvec_push_own(out, tc_fg(TUI_TC_MUTED, t));
        free(t);
    }
    strvec_free(&l);
}

static void hover_view(const char *text, const char *start, const char *lang, size_t lang_n, const char *code, size_t code_n,
                       const char *end, bool expanded, view_t *v) {
    v->action = "hover";
    v->icon = TC_ST_SUCCESS;
    v->wrap_collapsed = true;
    char *lg = xstrndup(lang, lang_n);
    char *c0 = xstrndup(code, code_n);
    char *ct = str_trim(c0);
    strvec_t cl = {0}, full = {0};
    tc_highlight(ct, *lg ? lg : NULL, &cl);
    char *b0 = xstrndup(text, (size_t)(start - text));
    char *before = str_trim(b0), *after = str_trim(end);
    doc_rows(before, &full);
    for (size_t i = 0; i < cl.n; i++) strvec_push(&full, cl.v[i]);
    doc_rows(after, &full);
    size_t hidden = full.n ? full.n - 1 : 0;
    if (expanded || hidden == 0) {
        for (size_t i = 0; i < full.n; i++) strvec_push(&v->rows, full.v[i]);
    } else {
        strvec_push(&v->rows, cl.n ? cl.v[0] : full.v[0]);
        char *m = tc_more_items((int64_t)hidden, "line");
        strvec_push_own(&v->rows, tc_fg(TUI_TC_MUTED, m));
        free(m);
        v->hidden = true;
    }
    strvec_free(&full);
    strvec_free(&cl);
    free(after);
    free(before);
    free(b0);
    free(ct);
    free(c0);
    free(lg);
}

/* ---------- diagnostics ---------- */

static bool word_char(unsigned char c) { return isalnum(c) || c == '_'; }

/* `N word(s)(, N word(s))*` starting at p; returns the end, NULL when p doesn't hold one */
static const char *summary_run(const char *p) {
    for (;;) {
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) p++;
        if (*p != ' ') return NULL;
        p++;
        if (!word_char((unsigned char)*p)) return NULL;
        while (word_char((unsigned char)*p)) p++;
        if (!str_starts(p, "(s)")) return NULL;
        p += 3;
        if (!str_starts(p, ", ")) return p;
        p += 2;
    }
}

/* DIAGNOSTICS_SUMMARY_RE: /^\d+ \w+\(s\)(?:, \d+ \w+\(s\))*:?$/ */
static bool is_summary_line(const char *l) {
    const char *e = summary_run(l);
    if (!e) return false;
    if (*e == ':') e++;
    return !*e;
}

/* FILE_SUMMARY_RE: /: (?:no issues|\d+ \w+\(s\)(?:, \d+ \w+\(s\))*)$/ */
static bool is_file_summary(const char *l) {
    for (const char *p = strstr(l, ": "); p; p = strstr(p + 1, ": ")) {
        if (!strcmp(p + 2, "no issues")) return true;
        const char *e = summary_run(p + 2);
        if (e && !*e) return true;
    }
    return false;
}

/* DIAGNOSTIC_AT_RE: /^\d+:\d+\s+\[/ */
static bool is_diagnostic_at(const char *l) {
    if (!isdigit((unsigned char)*l)) return false;
    while (isdigit((unsigned char)*l)) l++;
    if (*l != ':' || !isdigit((unsigned char)l[1])) return false;
    l++;
    while (isdigit((unsigned char)*l)) l++;
    if (!isspace((unsigned char)*l)) return false;
    while (isspace((unsigned char)*l)) l++;
    return *l == '[';
}

/* STATUS_GLYPH_RE: /^(?:\[[^\]\s]*\]|[^\p{L}\p{N}\s./~\\]+)\s+/u, stripped. Non-ASCII symbol blocks and the private use
 * area count as glyphs; other non-ASCII letters as text. */
static const char *strip_status_glyph(const char *l) {
    const char *p = l;
    if (*p == '[') {
        const char *q = p + 1;
        while (*q && *q != ']' && !isspace((unsigned char)*q)) q++;
        if (*q == ']' && isspace((unsigned char)q[1])) {
            q++;
            while (isspace((unsigned char)*q)) q++;
            return q;
        }
    }
    size_t n = strlen(p), i = 0;
    while (i < n) {
        uint32_t cp;
        int k = utf8_decode(p + i, n - i, &cp);
        bool glyph = cp < 0x80 ? !(isalnum((int)cp) || isspace((int)cp) || strchr("./~\\", (int)cp))
                               : (cp >= 0x2000 && cp <= 0x2bff) || (cp >= 0xe000 && cp <= 0xf8ff) || cp >= 0x1f000;
        if (!glyph) break;
        i += (size_t)k;
    }
    if (!i || !isspace((unsigned char)p[i])) return l;
    while (isspace((unsigned char)p[i])) i++;
    return p + i;
}

static void diag_item(const diag_t *d, bool expanded, strvec_t *items) {
    char *loc = diag_location(d);
    char *sv = severity_tag(d->sev);
    char *msg = with_code_close(diag_message(d), d);
    char *m0 = tc_replace_tabs(msg);
    char *mm = tc_fg(TUI_TC_MUTED, m0);
    strvec_push_own(items, !*msg ? xasprintf("%s %s", loc, sv) : xasprintf(expanded ? "%s %s\n%s" : "%s %s %s", loc, sv, mm));
    free(mm);
    free(m0);
    free(msg);
    free(sv);
    free(loc);
}

static void push_raw(strvec_t *items, const strvec_t *raw, tui_tc_t slot) {
    for (size_t i = 0; i < raw->n; i++) {
        char *t = tc_replace_tabs(raw->v[i]);
        strvec_push_own(items, tc_fg(slot, t));
        free(t);
    }
}

/* diagnosticsView + parseLspDiagnostics: failures and unverified files lead, partial-failure warnings and unparsed lines
 * follow; counted from the rows (a multi-file response carries one summary per file) */
static void diagnostics_view(const char *text, bool expanded, view_t *v) {
    v->action = "diagnostics";
    tc_grouped_t g;
    tc_grouped_parse(&g, text, NULL, NULL);
    strvec_t failures = {0}, warnings = {0}, unreported = {0}, other = {0};
    VEC(diag_t) diags = {0};
    const char *file = NULL;
    for (size_t i = 0; i < g.lines.n; i++) {
        if (g.rows[i].kind != TC_GROW_CONTENT) {
            file = g.rows[i].kind == TC_GROW_FILE ? g.rows[i].display_path : NULL;
            continue;
        }
        char *line = str_trim(g.lines.v[i]);
        if (!*line || !strcmp(line, "OK") || is_summary_line(line)) {
        } else if (strstr(line, "all language servers failed")) {
            strvec_push(&failures, strip_status_glyph(line));
        } else if (strstr(line, "some servers failed")) {
            strvec_push(&warnings, strip_status_glyph(line));
        } else if (strstr(line, "no diagnostics report yet")) {
            strvec_push(&unreported, strip_status_glyph(line));
        } else if (!is_file_summary(line)) {
            char *q = file && is_diagnostic_at(line) ? xasprintf("%s:%s", file, line) : xstrdup(line);
            diag_t d;
            if (parse_diag(q, &d)) vec_push(&diags, d);
            else strvec_push(&other, line);
            free(q);
        }
        free(line);
    }
    static const char *const SEV[] = {"error", "warning", "info", "hint"};
    int64_t counts[4] = {0};
    for (size_t i = 0; i < diags.n; i++)
        for (int k = 0; k < 4; k++)
            if (!strcmp(diags.v[i].sev, SEV[k])) counts[k]++;
    for (int k = 0; k < 4; k++)
        if (counts[k] > 0) strvec_push_own(&v->meta, tc_format_count(SEV[k], counts[k]));
    if (failures.n) strvec_push_own(&v->meta, tc_format_count("server failure", (int64_t)failures.n));
    if (unreported.n) strvec_push_own(&v->meta, tc_format_count("unverified file", (int64_t)unreported.n));
    strvec_t items = {0};
    push_raw(&items, &failures, TUI_TC_ERROR);
    push_raw(&items, &unreported, TUI_TC_WARNING);
    for (size_t i = 0; i < diags.n; i++) diag_item(&diags.v[i], expanded, &items);
    push_raw(&items, &warnings, TUI_TC_WARNING);
    push_raw(&items, &other, TUI_TC_MUTED);
    /* only a response with nothing to count and nothing to show claims a clean file */
    if (!v->meta.n && !items.n) strvec_push(&v->meta, "No issues");
    v->icon = counts[0] > 0 || failures.n ? TC_ST_ERROR : counts[1] > 0 || unreported.n || warnings.n ? TC_ST_WARNING : TC_ST_SUCCESS;
    tc_tree_list(&items, expanded, 3, "diagnostic", &v->rows);
    v->hidden = !expanded && items.n > 3;
    strvec_free(&items);
    for (size_t i = 0; i < diags.n; i++) diag_free(&diags.v[i]);
    vec_free(&diags);
    strvec_free(&failures);
    strvec_free(&warnings);
    strvec_free(&unreported);
    strvec_free(&other);
    tc_grouped_free(&g);
}

/* ---------- locations and references ---------- */

/* Found N definition(s) / type definition(s) / implementation(s) / reference(s): `  file:line:col` rows, each with the
 * `    N: source` context after it; a row matching both is context of the current location */
typedef struct {
    char *file;
    int line, col;
    strvec_t ctx;
    VEC(int) nums;
} loc_t;
typedef VEC(loc_t) locs_t;

static bool parse_locations(const strvec_t *lines, char **noun, int *count, locs_t *locs) {
    if (!lines->n || !str_starts(lines->v[0], "Found ")) return false;
    const char *p = lines->v[0] + 6;
    if (!isdigit((unsigned char)*p)) return false;
    int n = atoi(p);
    while (isdigit((unsigned char)*p)) p++;
    if (*p != ' ') return false;
    p++;
    static const char *const NOUNS[] = {"type definition", "implementation", "definition", "reference"};
    const char *nn = NULL;
    for (size_t i = 0; i < ARRAY_LEN(NOUNS); i++) {
        char *f = xasprintf("%s(s):", NOUNS[i]);
        if (!strcmp(p, f)) nn = NOUNS[i];
        free(f);
    }
    if (!nn) return false;
    *noun = xstrdup(nn);
    *count = n;
    for (size_t i = 1; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (locs->n && str_starts(l, "    ") && isdigit((unsigned char)l[4])) {
            const char *q = l + 4;
            int num = atoi(q);
            while (isdigit((unsigned char)*q)) q++;
            if (*q == ':') {
                q++;
                if (*q == ' ') q++;
                char *t = tc_trim_end(q);
                loc_t *cur = &locs->v[locs->n - 1];
                strvec_push_own(&cur->ctx, t);
                vec_push(&cur->nums, num);
                continue;
            }
        }
        if (str_starts(l, "  ")) {
            char *t = xstrdup(l + 2);
            char *c2 = strrchr(t, ':');
            if (c2 && c2[1] && strspn(c2 + 1, "0123456789") == strlen(c2 + 1)) {
                *c2 = 0;
                char *c1 = strrchr(t, ':');
                if (c1 && c1 > t && c1[1] && strspn(c1 + 1, "0123456789") == strlen(c1 + 1)) {
                    *c1 = 0;
                    loc_t lc = {.file = xstrdup(t), .line = atoi(c1 + 1), .col = atoi(c2 + 1)};
                    vec_push(locs, lc);
                }
            }
            free(t);
        }
    }
    return true;
}

/* highlightSourceLine: one source line in its file's language, or in `fallback` when the highlighter doesn't know it */
static char *highlight_source_line(const char *source, const char *lang, tui_tc_t fallback) {
    char *plain = tc_replace_tabs(source);
    if (lang && !strcmp(lang, "markdown")) return plain; /* the native highlighter knows it; plain rows stay uncolored */
    if (!lang || !tui_highlight_supported(lang)) {
        char *r = tc_fg(fallback, plain);
        free(plain);
        return r;
    }
    strvec_t hl = {0};
    tc_highlight(plain, lang, &hl);
    char *r = xstrdup(hl.n ? hl.v[0] : plain);
    strvec_free(&hl);
    free(plain);
    return r;
}

static char *styled_shortened(const char *file) {
    char *sp = tc_shorten_path(file);
    char *t = tc_replace_tabs(sp);
    char *r = tc_styled_path(t);
    free(t);
    free(sp);
    return r;
}

/* referencesView: one branch per file (`file (N)`) over its locations, each previewing its source line; all positions
 * share one width, so the rules line up across files and stay put when the view expands */
static void references_view(int count, const locs_t *locs, bool expanded, view_t *v) {
    v->action = "references";
    v->icon = count > 0 ? TC_ST_SUCCESS : TC_ST_WARNING;
    strvec_t files = {0};
    size_t *counts = xcalloc(locs->n + 1, sizeof *counts);
    int pos_w = 0;
    for (size_t i = 0; i < locs->n; i++) {
        size_t fi = files.n;
        for (size_t k = 0; k < files.n; k++)
            if (!strcmp(files.v[k], locs->v[i].file)) fi = k;
        if (fi == files.n) strvec_push(&files, locs->v[i].file);
        counts[fi]++;
        char b[48];
        pos_w = MAX(pos_w, snprintf(b, sizeof b, "%d:%d", locs->v[i].line, locs->v[i].col));
    }
    strvec_push_own(&v->meta, tc_format_count("reference", count));
    if (files.n) strvec_push_own(&v->meta, tc_format_count("file", (int64_t)files.n));
    bool hides = files.n > 3;
    for (size_t k = 0; k < MIN(files.n, (size_t)3); k++) hides |= counts[k] > 2;
    strvec_t items = {0};
    for (size_t fi = 0; fi < files.n; fi++) {
        const char *lang = tc_lang_from_path(files.v[fi]);
        strvec_t rows = {0};
        for (size_t i = 0; i < locs->n; i++) {
            const loc_t *l = &locs->v[i];
            if (strcmp(l->file, files.v[fi])) continue;
            char pos[48];
            char b[48];
            snprintf(b, sizeof b, "%d:%d", l->line, l->col);
            snprintf(pos, sizeof pos, "%*s", pos_w, b);
            char *source = NULL;
            for (size_t k = 0; k < l->ctx.n && !source; k++)
                if (l->nums.v[k] == l->line) source = str_trim(l->ctx.v[k]);
            if (!source || !*source) strvec_push_own(&rows, tc_fg(TUI_TC_MUTED, pos));
            else {
                char *g0 = xasprintf("%s\xe2\x94\x82", pos);
                char *g = tc_fg(TUI_TC_MUTED, g0);
                char *s = highlight_source_line(source, lang, TUI_TC_TOOL_OUTPUT);
                strvec_push_own(&rows, xasprintf("%s %s", g, s));
                free(s);
                free(g);
                free(g0);
            }
            free(source);
        }
        strvec_t nested = {0};
        tc_tree_list(&rows, expanded, 2, "location", &nested);
        char *p = styled_shortened(files.v[fi]);
        char *c0 = xasprintf("(%zu)", counts[fi]);
        char *cd = tc_fg(TUI_TC_DIM, c0);
        sb_t it = SB_INIT;
        sb_appendf(&it, "%s %s", p, cd);
        for (size_t i = 0; i < nested.n; i++) sb_appendf(&it, "\n%s", nested.v[i]);
        strvec_push_own(&items, sb_detach(&it));
        free(cd);
        free(c0);
        free(p);
        strvec_free(&nested);
        strvec_free(&rows);
    }
    tc_tree_list(&items, expanded, 3, "file", &v->rows);
    v->hidden = !expanded && hides;
    strvec_free(&items);
    free(counts);
    strvec_free(&files);
}

/* locationsView: one branch per `file:line:col` over its source context, the location's line marked like a grep match */
static void locations_view(const char *noun, int count, const locs_t *locs, bool expanded, view_t *v) {
    v->action = noun;
    v->icon = count > 0 ? TC_ST_SUCCESS : TC_ST_WARNING;
    strvec_push_own(&v->meta, tc_format_count(noun, count));
    strvec_t items = {0};
    size_t limit = expanded ? locs->n : MIN(locs->n, (size_t)2);
    for (size_t i = 0; i < limit; i++) {
        const loc_t *l = &locs->v[i];
        char *pos0 = xasprintf(":%d:%d", l->line, l->col);
        char *pos = tc_fg(TUI_TC_MUTED, pos0);
        char *fa = styled_shortened(l->file);
        sb_t it = SB_INIT;
        sb_appendf(&it, "%s%s", fa, pos);
        int w = 0;
        for (size_t k = 0; k < l->nums.n; k++) {
            char b[16];
            w = MAX(w, snprintf(b, sizeof b, "%d", l->nums.v[k]));
        }
        /* renderCodeFrameRun: each gutter in toolOutput, the sources highlighted as one run */
        const char *lang = tc_lang_from_path(l->file);
        bool plain = lang && !strcmp(lang, "markdown");
        bool known = lang && tui_highlight_supported(lang);
        strvec_t src = {0}, hl = {0};
        for (size_t k = 0; k < l->ctx.n; k++) strvec_push_own(&src, tc_replace_tabs(l->ctx.v[k]));
        if (known) {
            char *joined = strvec_join(&src, "\n");
            tui_highlight_code(joined, lang, NULL, &hl);
            free(joined);
        }
        for (size_t k = 0; k < l->ctx.n; k++) {
            char g[32], gg[64];
            snprintf(g, sizeof g, "%s%d", l->nums.v[k] == l->line ? "*" : "", l->nums.v[k]);
            snprintf(gg, sizeof gg, "%*s\xe2\x94\x82", w + 1, g);
            char *gs = tc_fg(TUI_TC_TOOL_OUTPUT, gg);
            char *ss = plain ? xstrdup(src.v[k]) : known && k < hl.n ? xstrdup(hl.v[k]) : tc_fg(TUI_TC_TOOL_OUTPUT, src.v[k]);
            sb_appendf(&it, "\n%s%s", gs, ss);
            free(ss);
            free(gs);
        }
        strvec_free(&hl);
        strvec_free(&src);
        strvec_push_own(&items, sb_detach(&it));
        free(fa);
        free(pos);
        free(pos0);
    }
    for (size_t i = limit; i < locs->n; i++) strvec_push(&items, "");
    tc_tree_list(&items, expanded, 2, noun, &v->rows);
    v->hidden = !expanded && locs->n > 2;
    strvec_free(&items);
}

/* ---------- symbols ---------- */

typedef struct {
    char *icon, *name, *line;
    int indent;
} sym_t;

/* /^(\S+)\s+(.+?)\s*@\s*line\s*(\d+)/ on a trimmed line */
static bool parse_symbol(const char *t, int indent, sym_t *s) {
    const char *sp = strpbrk(t, " \t");
    if (!sp || sp == t) return false;
    const char *ns = sp;
    while (isspace((unsigned char)*ns)) ns++;
    for (const char *at = strchr(ns + 1, '@'); at; at = strchr(at + 1, '@')) {
        const char *ln = at + 1;
        while (isspace((unsigned char)*ln)) ln++;
        if (!str_starts(ln, "line")) continue;
        ln += 4;
        while (isspace((unsigned char)*ln)) ln++;
        if (!isdigit((unsigned char)*ln)) continue;
        const char *ne = at;
        while (ne > ns + 1 && isspace((unsigned char)ne[-1])) ne--;
        s->icon = xstrndup(t, (size_t)(sp - t));
        s->name = xstrndup(ns, (size_t)(ne - ns));
        s->line = xstrndup(ln, strspn(ln, "0123456789"));
        s->indent = indent;
        return true;
    }
    return false;
}

static char *symbol_label(const sym_t *s, const char *lang) {
    char *ic = tc_fg(TUI_TC_ACCENT, s->icon);
    char *nm = highlight_source_line(s->name, lang, TUI_TC_ACCENT);
    char *r = xasprintf("%s %s", ic, nm);
    free(nm);
    free(ic);
    return r;
}

static void symbols_view(const char *file, bool show_file, const strvec_t *lines, bool expanded, view_t *v) {
    v->action = "symbols";
    VEC(sym_t) syms = {0};
    for (size_t i = 0; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (!strchr(l, '@') || !strstr(l, "line")) continue;
        int ind = (int)strspn(l, " \t");
        char *t = str_trim(l);
        sym_t s;
        if (parse_symbol(t, ind, &s)) vec_push(&syms, s);
        free(t);
    }
    v->icon = syms.n ? TC_ST_SUCCESS : TC_ST_WARNING;
    strvec_push_own(&v->meta, tc_format_count("symbol", (int64_t)syms.n));
    if (show_file) strvec_push_own(&v->meta, xasprintf("in %s", file));
    const char *lang = tc_lang_from_path(file);
    const char *vertical = tc_sym("tree.vertical");
    if (expanded) {
        /* one linear pass each: isLast (monotonic stack, right to left); per ancestor level, the nearest earlier symbol at
         * that indent decides the rail */
        bool *is_last = xcalloc(syms.n + 1, sizeof *is_last);
        size_t *pending = xcalloc(syms.n + 1, sizeof *pending);
        size_t np = 0;
        for (size_t i = syms.n; i-- > 0;) {
            int my = syms.v[i].indent;
            while (np && syms.v[pending[np - 1]].indent > my) np--;
            is_last[i] = !np || syms.v[pending[np - 1]].indent != my;
            pending[np++] = i;
        }
        for (size_t i = 0; i < syms.n; i++) {
            int my = syms.v[i].indent;
            sb_t pre = SB_INIT;
            for (int level = 2; level <= my; level += 2) {
                bool rail = false;
                for (size_t a = i; a-- > 0;)
                    if (syms.v[a].indent == level - 2) {
                        rail = !is_last[a];
                        break;
                    }
                bool seen = false;
                for (size_t a = i; a-- > 0 && !seen;) seen = syms.v[a].indent == level - 2;
                if (seen && !rail) sb_append(&pre, "   ");
                else sb_appendf(&pre, "%s  ", vertical);
            }
            char *prefix = tc_fg(TUI_TC_DIM, sb_cstr(&pre));
            char *br = tc_fg(TUI_TC_DIM, tc_sym(is_last[i] ? "tree.last" : "tree.branch"));
            char *det0 = is_last[i] ? xstrdup("   ") : xasprintf("%s  ", vertical);
            char *det = tc_fg(TUI_TC_DIM, det0);
            char *lb = symbol_label(&syms.v[i], lang);
            char *lt = xasprintf("line %s", syms.v[i].line);
            char *lm = tc_fg(TUI_TC_MUTED, lt);
            strvec_push_own(&v->rows, xasprintf("%s%s %s", prefix, br, lb));
            strvec_push_own(&v->rows, xasprintf("%s%s%s", prefix, det, lm));
            free(lm);
            free(lt);
            free(lb);
            free(det);
            free(det0);
            free(br);
            free(prefix);
            sb_free(&pre);
        }
        free(pending);
        free(is_last);
    } else {
        /* the marker counts every hidden symbol, nested ones included, so it adds up to the header's count */
        strvec_t items = {0};
        for (size_t i = 0; i < syms.n && items.n < 3; i++) {
            if (syms.v[i].indent) continue;
            char *lb = symbol_label(&syms.v[i], lang);
            char *lt = xasprintf("line %s", syms.v[i].line);
            char *lm = tc_fg(TUI_TC_MUTED, lt);
            strvec_push_own(&items, xasprintf("%s %s", lb, lm));
            free(lm);
            free(lt);
            free(lb);
        }
        size_t hidden = syms.n - items.n;
        char *summary = hidden ? tc_more_items((int64_t)hidden, "symbol") : NULL;
        tc_tree_list_summary(&items, summary, &v->rows);
        v->hidden = hidden > 0;
        free(summary);
        strvec_free(&items);
    }
    for (size_t i = 0; i < syms.n; i++) free(syms.v[i].icon), free(syms.v[i].name), free(syms.v[i].line);
    vec_free(&syms);
}

/* ---------- generic ---------- */

/* /^no\b.*\bfound\b/i on the trimmed text */
static bool is_no_found(const char *text) {
    char *t = str_trim(text);
    bool r = false;
    if (!strncasecmp(t, "no", 2) && !word_char((unsigned char)t[2])) {
        size_t eol = strcspn(t, "\n");
        for (size_t i = 2; i + 5 <= eol && !r; i++)
            r = !strncasecmp(t + i, "found", 5) && !word_char((unsigned char)t[i - 1]) && !word_char((unsigned char)t[i + 5]);
    }
    free(t);
    return r;
}

static void generic_view(const char *text, const strvec_t *lines, const char *action, bool expanded, view_t *v) {
    v->action = "request";
    bool err = strstr(text, "Error:") || has_status_row(action, lines, tc_sym("status.error"));
    bool ok = has_status_row(action, lines, tc_sym("status.success")) || strstr(text, "Applied");
    /* the call completed; an error reported inside its output, or an empty answer, needs attention */
    v->icon = (err && !ok) || is_no_found(text) ? TC_ST_WARNING : TC_ST_SUCCESS;
    strvec_t items = {0};
    push_raw(&items, lines, TUI_TC_TOOL_OUTPUT);
    tc_tree_list(&items, expanded, 4, "line", &v->rows);
    v->hidden = !expanded && items.n > 4;
    strvec_free(&items);
}

/* ---------- renderResult ---------- */

/* /\d+\s+(?:error|warning)\(s\)/ */
static bool has_severity_count(const char *text) {
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p)) continue;
        const char *q = p;
        while (isdigit((unsigned char)*q)) q++;
        if (!isspace((unsigned char)*q)) continue;
        while (isspace((unsigned char)*q)) q++;
        if (str_starts(q, "error(s)") || str_starts(q, "warning(s)")) return true;
    }
    return false;
}

/* /Symbols in (.+):/ — the last ':' on the line (NULL when there is no non-empty match) */
static const char *symbols_header(const char *text, const char **file_end) {
    for (const char *s = strstr(text, "Symbols in "); s; s = strstr(s + 1, "Symbols in ")) {
        const char *b = s + 11, *eol = b + strcspn(b, "\n"), *last = NULL;
        for (const char *p = b + 1; p < eol; p++)
            if (*p == ':') last = p;
        if (last) {
            *file_end = last;
            return b;
        }
    }
    return NULL;
}

void tc_lsp(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        lsp_call(c, width, out);
        return;
    }
    const json_t *req = c->args ? c->args : json_get_obj(c->details, "request");
    const char *details_action = json_get_str(c->details, "action");
    const json_t *first = json_at(c->content, 0);
    const char *text = str_eq(json_get_str(first, "type"), "text") ? json_get_str(first, "text") : NULL;
    if (!text) text = "";
    if (c->is_error) {
        char *desc = NULL;
        strvec_t meta = {0};
        lsp_request_status(req, details_action, &desc, &meta);
        tc_status_line_t sl = {.icon = TC_ST_ERROR, .spinner = -1, .title = "LSP", .title_slot = -1, .description = desc,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        strvec_t rows = {0};
        strvec_push_own(&rows, tc_status_line(&sl));
        /* the message hangs under the title, like a Grep or AST Grep error's, and wraps in full */
        char *tr = str_trim(text);
        strvec_t ml = {0};
        tc_split_lines(*tr ? tr : "LSP request failed", &ml);
        for (size_t i = 0; i < ml.n; i++) {
            char *mt = str_trim(ml.v[i]);
            if (*mt) {
                char *t = tc_replace_tabs(ml.v[i]);
                char *e = tc_fg(TUI_TC_ERROR, t);
                tc_wrap_prefixed("  ", "  ", e, width, &rows);
                free(e);
                free(t);
            }
            free(mt);
        }
        for (size_t i = 0; i < rows.n; i++) tc_push_wrapped(out, rows.v[i], width);
        strvec_free(&rows);
        strvec_free(&ml);
        free(tr);
        strvec_free(&meta);
        free(desc);
        return;
    }
    if (!*text) {
        char *desc = NULL;
        strvec_t meta = {0};
        lsp_request_status(req, details_action, &desc, &meta);
        strvec_push(&meta, "No result");
        tc_status_line_t sl = {.icon = TC_ST_WARNING, .spinner = -1, .title = "LSP", .title_slot = -1, .description = desc,
                               .meta = (const char *const *)meta.v, .nmeta = meta.n};
        char *h = tc_status_line(&sl);
        tc_push_wrapped(out, h, width);
        free(h);
        strvec_free(&meta);
        free(desc);
        return;
    }
    strvec_t lines = {0};
    tc_split_lines(text, &lines);
    const char *action = json_get_str(req, "action") ? json_get_str(req, "action") : details_action;
    view_t v = {0};
    char *noun = NULL;
    int lcount = 0;
    locs_t locs = {0};
    const char *block, *lang = NULL, *code = NULL, *block_end = NULL, *sym_end = NULL, *sym_file = NULL;
    size_t lang_n = 0, code_n = 0;
    /* locations and references are anchored at the response's first line, so they win over hover and diagnostics checks
     * their source context could trip; a diagnostics request answers with diagnostics whatever its rows hold */
    if (parse_locations(&lines, &noun, &lcount, &locs)) {
        if (!strcmp(noun, "reference")) references_view(lcount, &locs, c->expanded, &v);
        else locations_view(noun, lcount, &locs, c->expanded, &v);
    } else if (str_eq(action, "diagnostics")) {
        diagnostics_view(text, c->expanded, &v);
    } else if (find_code_block(text, &block, &lang, &lang_n, &code, &code_n, &block_end)) {
        hover_view(text, block, lang, lang_n, code, code_n, block_end, c->expanded, &v);
    } else if (has_severity_count(text) || has_status_row(action, &lines, tc_sym("status.error")) ||
               strstr(text, "no diagnostics report yet")) {
        diagnostics_view(text, c->expanded, &v);
    } else if ((sym_file = symbols_header(text, &sym_end))) {
        char *file = xstrndup(sym_file, (size_t)(sym_end - sym_file));
        symbols_view(file, !json_get_str(req, "file"), &lines, c->expanded, &v);
        free(file);
    } else {
        generic_view(text, &lines, action, c->expanded, &v);
    }
    char *desc = NULL;
    strvec_t meta = {0};
    lsp_request_status(req, details_action ? details_action : v.action, &desc, &meta);
    for (size_t i = 0; i < v.meta.n; i++) strvec_push(&meta, v.meta.v[i]);
    /* layoutBody: expanded rows hang under their connector or guide rails; collapsed rows clip to one row each, and a
     * collapsed view that hides rows or clips one gets the expand hint */
    strvec_t body = {0};
    bool hinted = !c->expanded && v.hidden;
    if (c->expanded || v.wrap_collapsed) {
        for (size_t i = 0; i < v.rows.n; i++) wrap_guided(v.rows.v[i], width, &body);
    } else {
        for (size_t i = 0; i < v.rows.n; i++) {
            char *t = tui_truncate_to_width(v.rows.v[i], width, NULL);
            hinted |= strcmp(t, v.rows.v[i]) != 0;
            strvec_push_own(&body, t);
        }
    }
    char *ic = !c->partial && v.icon == TC_ST_SUCCESS ? icon_lsp() : NULL;
    tc_status_line_t sl = {.icon = c->partial ? TC_ST_RUNNING : v.icon, .icon_override = ic, .spinner = c->partial ? c->spinner : -1,
                           .title = "LSP", .title_slot = -1, .description = desc, .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *h = tc_status_line(&sl);
    char *hint = hinted ? tc_expand_hint() : xstrdup("");
    char *header = xasprintf("%s%s", h, hint);
    tc_push_wrapped(out, header, width);
    for (size_t i = 0; i < body.n; i++) tc_push_wrapped(out, body.v[i], width);
    free(header);
    free(hint);
    free(h);
    free(ic);
    strvec_free(&body);
    strvec_free(&meta);
    free(desc);
    strvec_free(&v.meta);
    strvec_free(&v.rows);
    for (size_t i = 0; i < locs.n; i++) {
        free(locs.v[i].file);
        strvec_free(&locs.v[i].ctx);
        vec_free(&locs.v[i].nums);
    }
    vec_free(&locs);
    free(noun);
    strvec_free(&lines);
}
