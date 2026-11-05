/* tc_lsp.c - lsp card (port of omp tools/lsp.ts renderCall/renderResult). The result body is derived from
 * the response text the way omp does it: `Found N definition(s):` locations, ```lang hover blocks,
 * `N error(s)` / `N warning(s)` diagnostics, `N reference(s)` grouped by file, `Symbols in <file>:`
 * trees, otherwise a generic first-lines preview. Expanded rows are not re-wrapped under their guides
 * (wrapGuidedLine); the frame wraps them flush. */
#include "ui/tools/tc.h"

#include <ctype.h>

#include "tui/ansi.h"

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

static void lsp_call(const tc_call_t *c, int width, strvec_t *out) {
    const json_t *a = c->args;
    char *act = action_label(json_get_str(a, "action"));
    const char *q = json_get_str(a, "query");
    char *qp = q ? tui_truncate_to_width(q, 40, NULL) : NULL;
    char *sp = NULL;
    if (json_get_str(a, "symbol")) {
        char *f = flat(json_get_str(a, "symbol"));
        sp = tui_truncate_to_width(f, 40, NULL);
        free(f);
    }
    char *target = NULL;
    bool has_line = json_get(a, "line") != NULL;
    char *ln = has_line ? json_dump(json_get(a, "line"), 0) : NULL;
    if (json_get_str(a, "file")) {
        char *s = tc_shorten_path(json_get_str(a, "file"));
        if (has_line) target = sp ? xasprintf("%s:%s (%s)", s, ln, sp) : xasprintf("%s:%s", s, ln);
        else target = xstrdup(s);
        free(s);
    } else if (has_line) target = sp ? xasprintf("line %s (%s)", ln, sp) : xasprintf("line %s", ln);
    strvec_t meta = {0};
    if (qp && target) strvec_push_own(&meta, xasprintf("query:%s", qp));
    if (json_get_str(a, "new_name")) strvec_push_own(&meta, xasprintf("new:%s", json_get_str(a, "new_name")));
    if (json_get(a, "apply")) strvec_push_own(&meta, xasprintf("apply:%s", json_get_bool(a, "apply", false) ? "true" : "false"));
    char *desc = target ? xasprintf("%s %s", act, target) : qp ? xasprintf("%s %s", act, qp) : xstrdup(act);
    tc_status_line_t sl = {.icon = TC_ST_PENDING, .spinner = -1, .title = "LSP", .title_slot = -1, .description = desc,
                           .meta = (const char *const *)meta.v, .nmeta = meta.n};
    char *h = tc_status_line(&sl);
    tc_push_wrapped(out, h, width);
    free(h);
    free(desc);
    strvec_free(&meta);
    free(ln);
    free(target);
    free(sp);
    free(qp);
    free(act);
}

/* `N word(s)` count in text, -1 when absent */
static int count_of(const char *text, const char *word) {
    char *needle = xasprintf(" %s(s)", word);
    const char *p = text;
    int r = -1;
    while ((p = strstr(p, needle))) {
        const char *d = p;
        while (d > text && isspace((unsigned char)d[-1]) && d - 1 >= text) d--;
        const char *e = d;
        while (d > text && isdigit((unsigned char)d[-1])) d--;
        if (d < e) {
            r = atoi(d);
            break;
        }
        p += strlen(needle);
    }
    free(needle);
    return r;
}

static tc_status_t sev_slot_icon;

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

static bool has_line_col(const char *l) {
    for (const char *p = strchr(l, ':'); p; p = strchr(p + 1, ':')) {
        const char *q = p + 1;
        if (!isdigit((unsigned char)*q)) continue;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == ':' && isdigit((unsigned char)q[1])) return true;
    }
    return false;
}

static void push_split(strvec_t *out, const char *s) { tc_split_lines(s, out); }

static char *icon_lsp(void) { return tc_styled_sym("tool.lsp", TUI_TC_ACCENT); }

static void render_diagnostics(int errs, int warns, const strvec_t *lines, bool expanded, strvec_t *out) {
    strvec_t unreported = {0};
    for (size_t i = 0; i < lines->n; i++)
        if (strstr(lines->v[i], "no diagnostics report yet")) strvec_push(&unreported, lines->v[i]);
    char *icon = errs > 0 ? tc_styled_sym("status.error", TUI_TC_ERROR)
                 : (warns > 0 || unreported.n) ? tc_styled_sym("status.warning", TUI_TC_WARNING)
                                               : icon_lsp();
    strvec_t meta = {0};
    if (errs > 0) strvec_push_own(&meta, xasprintf("%d error%s", errs, errs != 1 ? "s" : ""));
    if (warns > 0) strvec_push_own(&meta, xasprintf("%d warning%s", warns, warns != 1 ? "s" : ""));
    if (unreported.n) strvec_push_own(&meta, tc_format_count("unverified file", (int64_t)unreported.n));
    if (!meta.n) strvec_push(&meta, "No issues");
    char *mj = strvec_join(&meta, tc_sym("sep.dot"));
    char *md = tc_fg(TUI_TC_DIM, mj);
    /* items: unreported rows first, then parsed diagnostics (raw rows when none parse) */
    VEC(diag_t) parsed = {0};
    strvec_t raws = {0};
    const char *err_sym = tc_sym("status.error");
    for (size_t i = 0; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (!(is_status_row(l, err_sym) || has_line_col(l))) continue;
        char *t = str_trim(l);
        diag_t d;
        if (parse_diag(t, &d)) vec_push(&parsed, d);
        strvec_push_own(&raws, tc_replace_tabs(t));
        free(t);
    }
    typedef struct {
        const char *raw;
        diag_t *d;
    } item_t;
    VEC(item_t) items = {0};
    char **utrim = xcalloc(unreported.n + 1, sizeof(char *));
    for (size_t i = 0; i < unreported.n; i++) {
        char *t = str_trim(unreported.v[i]);
        utrim[i] = tc_replace_tabs(t);
        free(t);
        vec_push(&items, ((item_t){utrim[i], NULL}));
    }
    if (parsed.n)
        for (size_t i = 0; i < parsed.n; i++) vec_push(&items, ((item_t){NULL, &parsed.v[i]}));
    else
        for (size_t i = 0; i < raws.n; i++) vec_push(&items, ((item_t){raws.v[i], NULL}));
    sb_t o = SB_INIT;
    if (expanded) {
        sb_appendf(&o, "%s %s", icon, md);
        for (size_t i = 0; i < items.n; i++) {
            bool last = i + 1 == items.n;
            char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
            if (items.v[i].raw) {
                char *m = tc_fg(TUI_TC_MUTED, items.v[i].raw);
                sb_appendf(&o, "\n %s %s", br, m);
                free(m);
            } else {
                diag_t *d = items.v[i].d;
                char *loc = diag_location(d);
                char *sv = severity_tag(d->sev);
                sb_appendf(&o, "\n %s %s %s", br, loc, sv);
                char *msg = with_code_close(diag_message(d), d);
                if (*msg) {
                    char *dp = tc_fg(TUI_TC_DIM, last ? "   " : "\xe2\x94\x82  ");
                    char *mm = tc_fg(TUI_TC_MUTED, msg);
                    sb_appendf(&o, "\n %s%s", dp, mm);
                    free(mm);
                    free(dp);
                }
                free(msg);
                free(sv);
                free(loc);
            }
            free(br);
        }
    } else {
        size_t shown = MIN(items.n, (size_t)3), remaining = items.n - shown;
        bool clipped = false;
        for (size_t i = 0; i < shown; i++)
            if (items.v[i].d) {
                char *m = with_code_close(diag_message(items.v[i].d), items.v[i].d);
                clipped |= tui_visible_width(m) > 80;
                free(m);
            }
        char *hint = remaining > 0 || clipped ? tc_expand_hint() : xstrdup("");
        sb_appendf(&o, "%s %s%s", icon, md, hint);
        free(hint);
        for (size_t i = 0; i < shown; i++) {
            bool last = i + 1 == shown && remaining == 0;
            char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
            if (items.v[i].raw) {
                char *m = tc_fg(TUI_TC_MUTED, items.v[i].raw);
                sb_appendf(&o, "\n %s %s", br, m);
                free(m);
            } else {
                diag_t *d = items.v[i].d;
                char *loc = diag_location(d);
                char *sv = severity_tag(d->sev);
                char *msg = with_code_close(diag_message(d), d);
                sb_appendf(&o, "\n %s %s %s", br, loc, sv);
                if (*msg) {
                    char *t = tui_truncate_to_width(msg, 80, NULL);
                    char *mm = tc_fg(TUI_TC_MUTED, t);
                    sb_appendf(&o, " %s", mm);
                    free(mm);
                    free(t);
                }
                free(msg);
                free(sv);
                free(loc);
            }
            free(br);
        }
        if (remaining > 0) {
            char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
            char *t = xasprintf("\xe2\x80\xa6 %zu more", remaining);
            char *m = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s %s", br, m);
            free(m);
            free(t);
            free(br);
        }
    }
    push_split(out, sb_cstr(&o));
    sb_free(&o);
    for (size_t i = 0; i < unreported.n; i++) free(utrim[i]);
    free(utrim);
    vec_free(&items);
    for (size_t i = 0; i < parsed.n; i++) diag_free(&parsed.v[i]);
    vec_free(&parsed);
    strvec_free(&raws);
    free(md);
    free(mj);
    strvec_free(&meta);
    free(icon);
    strvec_free(&unreported);
}

/* references grouped by file */
static void render_references(int count, const strvec_t *lines, bool expanded, strvec_t *out) {
    char *icon = count > 0 ? icon_lsp() : tc_styled_sym("status.warning", TUI_TC_WARNING);
    strvec_t files = {0};
    VEC(strvec_t) locs = {0};
    for (size_t i = 0; i < lines->n; i++) {
        char *t = str_trim(lines->v[i]);
        /* file:line:col at the end */
        char *c2 = strrchr(t, ':');
        bool ok = false;
        if (c2 && c2 > t && c2[1] && strspn(c2 + 1, "0123456789") == strlen(c2 + 1)) {
            *c2 = 0;
            char *c1 = strrchr(t, ':');
            if (c1 && c1 > t && c1[1] && strspn(c1 + 1, "0123456789") == strlen(c1 + 1)) {
                *c1 = 0;
                ok = !strchr(t, ' ') || true;
                size_t fi = files.n;
                for (size_t k = 0; k < files.n; k++)
                    if (!strcmp(files.v[k], t)) fi = k;
                if (fi == files.n) {
                    strvec_push(&files, t);
                    strvec_t e = {0};
                    vec_push(&locs, e);
                }
                strvec_push_own(&locs.v[fi], xasprintf("%s:%s", c1 + 1, c2 + 1));
            }
        }
        (void)ok;
        free(t);
    }
    size_t max_files = expanded ? files.n : 3, max_locs = expanded ? SIZE_MAX : 1;
    bool hidden = files.n > max_files;
    for (size_t k = 0; k < files.n; k++) hidden |= locs.v[k].n > max_locs;
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d found", count);
    char *cd = tc_fg(TUI_TC_DIM, cnt);
    char *hint = !expanded && hidden ? tc_expand_hint() : xstrdup("");
    sb_t o = SB_INIT;
    sb_appendf(&o, "%s %s%s", icon, cd, hint);
    size_t nf = MIN(files.n, max_files);
    for (size_t fi = 0; fi < nf; fi++) {
        bool lastf = fi + 1 == nf && files.n <= max_files;
        char *fb = tc_fg(TUI_TC_DIM, tc_sym(lastf ? "tree.last" : "tree.branch"));
        const char *fc = lastf ? "   " : "\xe2\x94\x82  ";
        char *fa = tc_styled_path(files.v[fi]);
        char *fm0 = xasprintf("%zu reference%s", locs.v[fi].n, locs.v[fi].n != 1 ? "s" : "");
        char *fm = tc_fg(TUI_TC_DIM, fm0);
        sb_appendf(&o, "\n %s %s %s", fb, fa, fm);
        size_t nl = MIN(locs.v[fi].n, max_locs);
        for (size_t li = 0; li < nl; li++) {
            bool lastl = li + 1 == nl && locs.v[fi].n <= max_locs;
            char *lc = xstrdup(locs.v[fi].v[li]);
            char *colon = strchr(lc, ':');
            *colon = 0;
            char *fcd = tc_fg(TUI_TC_DIM, fc);
            char *lb = tc_fg(TUI_TC_DIM, tc_sym(lastl ? "tree.last" : "tree.branch"));
            char *t = xasprintf("line %s, col %s", lc, colon + 1);
            char *tm = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s%s %s", fcd, lb, tm);
            if (expanded) {
                char *lcd = tc_fg(TUI_TC_DIM, lastl ? "   " : "\xe2\x94\x82  ");
                char *at0 = tc_fg(TUI_TC_MUTED, "at ");
                char *atp = tc_styled_path(files.v[fi]);
                char *atl0 = xasprintf(":%s:%s", lc, colon + 1);
                char *atl = tc_fg(TUI_TC_MUTED, atl0);
                char *am = xasprintf("%s%s%s", at0, atp, atl);
                free(atl);
                free(atl0);
                free(atp);
                free(at0);
                sb_appendf(&o, "\n %s%s%s", fcd, lcd, am);
                free(am);
                free(lcd);
            }
            free(tm);
            free(t);
            free(lb);
            free(fcd);
            free(lc);
        }
        if (locs.v[fi].n > max_locs) {
            char *fcd = tc_fg(TUI_TC_DIM, fc);
            char *lb = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
            char *t = xasprintf("\xe2\x80\xa6 %zu more", locs.v[fi].n - max_locs);
            char *tm = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s%s %s", fcd, lb, tm);
            free(tm);
            free(t);
            free(lb);
            free(fcd);
        }
        free(fm);
        free(fm0);
        free(fa);
        free(fb);
    }
    if (files.n > max_files) {
        char *lb = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *mi = tc_more_items((int64_t)(files.n - max_files), "file");
        char *mm = tc_fg(TUI_TC_MUTED, mi);
        sb_appendf(&o, "\n %s %s", lb, mm);
        free(mm);
        free(mi);
        free(lb);
    }
    push_split(out, sb_cstr(&o));
    sb_free(&o);
    free(hint);
    free(cd);
    for (size_t k = 0; k < locs.n; k++) strvec_free(&locs.v[k]);
    vec_free(&locs);
    strvec_free(&files);
    free(icon);
}

/* `Found N definition(s):` + `  file:line:col` + `    N: source` context */
typedef struct {
    char *file;
    int line, col;
    strvec_t ctx;  /* raw source */
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
    static const char *const NOUNS[] = {"type definition", "implementation", "definition"};
    const char *nn = NULL;
    for (size_t i = 0; i < 3; i++) {
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

static void render_locations(const char *noun, int count, locs_t *locs, bool expanded, int cw, strvec_t *out) {
    size_t shown = expanded ? locs->n : MIN(locs->n, (size_t)2), hidden = locs->n - shown;
    char *icon = count > 0 ? icon_lsp() : tc_styled_sym("status.warning", TUI_TC_WARNING);
    char *fc = tc_format_count(noun, count);
    char *fd = tc_fg(TUI_TC_DIM, fc);
    char *hint = !expanded && hidden ? tc_expand_hint() : xstrdup("");
    strvec_t rows = {0};
    strvec_push_own(&rows, xasprintf("%s %s%s", icon, fd, hint));
    for (size_t i = 0; i < shown; i++) {
        loc_t *l = &locs->v[i];
        bool last = i + 1 == shown && hidden == 0;
        char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
        char *rail = tc_fg(TUI_TC_DIM, last ? "   " : "\xe2\x94\x82  ");
        char *pos0 = xasprintf(":%d:%d", l->line, l->col);
        char *pos = tc_fg(TUI_TC_MUTED, pos0);
        char *sp = tc_shorten_path(l->file);
        char *spr = tc_replace_tabs(sp);
        char *fa = tc_styled_path(spr);
        strvec_push_own(&rows, xasprintf(" %s %s%s", br, fa, pos));
        int w = 0;
        for (size_t k = 0; k < l->nums.n; k++) {
            char b[16];
            w = MAX(w, snprintf(b, sizeof b, "%d", l->nums.v[k]));
        }
        for (size_t k = 0; k < l->ctx.n; k++) {
            char g[32];
            snprintf(g, sizeof g, "%s%d", l->nums.v[k] == l->line ? "*" : "", l->nums.v[k]);
            char *src = tc_replace_tabs(l->ctx.v[k]);
            char *frame = xasprintf("%*s\xe2\x94\x82%s", w + 1, g, src);
            char *fo = tc_fg(TUI_TC_TOOL_OUTPUT, frame);
            strvec_push_own(&rows, xasprintf(" %s%s", rail, fo));
            free(fo);
            free(frame);
            free(src);
        }
        free(fa);
        free(spr);
        free(sp);
        free(pos);
        free(pos0);
        free(rail);
        free(br);
    }
    if (hidden) {
        char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
        char *mi = tc_more_items((int64_t)hidden, noun);
        char *mm = tc_fg(TUI_TC_MUTED, mi);
        strvec_push_own(&rows, xasprintf(" %s %s", br, mm));
        free(mm);
        free(mi);
        free(br);
    }
    if (expanded) {
        for (size_t i = 0; i < rows.n; i++) strvec_push(out, rows.v[i]);
    } else {
        /* clipCollapsedLocations */
        bool clipped = false;
        strvec_t cl = {0};
        for (size_t i = 0; i < rows.n; i++) {
            char *t = tui_truncate_to_width(rows.v[i], cw, NULL);
            clipped |= strcmp(t, rows.v[i]) != 0;
            strvec_push_own(&cl, t);
        }
        if (locs->n <= 2 && clipped) {
            char *h = tc_expand_hint();
            char *j = xasprintf("%s%s", rows.v[0], h);
            free(cl.v[0]);
            cl.v[0] = tui_truncate_to_width(j, cw, NULL);
            free(j);
            free(h);
        }
        for (size_t i = 0; i < cl.n; i++) strvec_push(out, cl.v[i]);
        strvec_free(&cl);
    }
    strvec_free(&rows);
    free(hint);
    free(fd);
    free(fc);
    free(icon);
}

static void render_hover(const char *text, const char *lang, const char *code_s, const char *code_e, bool expanded, strvec_t *out) {
    /* code between the fence line and the closing ``` */
    char *code0 = xstrndup(code_s, (size_t)(code_e - code_s));
    char *code = str_trim(code0);
    char *before0 = xstrndup(text, (size_t)(strstr(text, "```") - text));
    char *before = tc_trim_end(before0);
    /* omp: fullText.slice(fullText.indexOf("```", 3) + 3) — the first fence at offset >= 3, which is the opening
     * fence itself whenever text precedes it (so the code block then repeats as the "after" text) */
    const char *close = strlen(text) > 3 ? strstr(text + 3, "```") : NULL;
    char *after = str_trim(close ? close + 3 : text + MIN(strlen(text), (size_t)2));
    strvec_t cl = {0};
    tc_highlight(code, *lang ? lang : NULL, &cl);
    char *icon = tc_styled_sym("status.info", TUI_TC_ACCENT);
    char *ll = NULL;
    if (*lang) {
        char *t = xasprintf(" %s", lang);
        ll = tc_fg(TUI_TC_MD_CODE_BLOCK_BORDER, t);
        free(t);
    } else ll = xstrdup("");
    const char *h = tc_sym("boxRound.horizontal"), *v = tc_sym("boxRound.vertical");
    char *vb = tc_fg(TUI_TC_MD_CODE_BLOCK_BORDER, v);
    char *bottom0 = xasprintf("%s%s%s%s", tc_sym("boxRound.bottomLeft"), h, h, h);
    char *bottom = tc_fg(TUI_TC_MD_CODE_BLOCK_BORDER, bottom0);
    sb_t o = SB_INIT;
    if (expanded) {
        char *top0 = xasprintf("%s%s%s%s", tc_sym("boxRound.topLeft"), h, h, h);
        char *top = tc_fg(TUI_TC_MD_CODE_BLOCK_BORDER, top0);
        sb_appendf(&o, "%s%s", icon, ll);
        if (*before) {
            strvec_t bl = {0};
            tc_split_lines(before, &bl);
            for (size_t i = 0; i < bl.n; i++) {
                char *m = tc_fg(TUI_TC_MUTED, bl.v[i]);
                sb_appendf(&o, "\n %s", m);
                free(m);
            }
            strvec_free(&bl);
        }
        sb_appendf(&o, "\n %s", top);
        for (size_t i = 0; i < cl.n; i++) sb_appendf(&o, "\n %s %s", vb, cl.v[i]);
        sb_appendf(&o, "\n %s", bottom);
        if (*after) {
            char *m = tc_fg(TUI_TC_MUTED, after);
            sb_appendf(&o, "\n %s", m);
            free(m);
        }
        free(top);
        free(top0);
    } else {
        bool more = cl.n > 1 || *after || *before;
        char *hint = more ? tc_expand_hint() : xstrdup("");
        sb_appendf(&o, "%s%s%s", icon, ll, hint);
        free(hint);
        if (*before) {
            char *t = tui_truncate_to_width(before, 60, NULL);
            char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.branch")), *m = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s %s", br, m);
            free(m);
            free(br);
            free(t);
        }
        sb_appendf(&o, "\n %s %s", vb, cl.n ? cl.v[0] : "");
        if (cl.n > 1) {
            char *t = xasprintf("\xe2\x80\xa6 %zu more lines", cl.n - 1);
            char *m = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s %s", vb, m);
            free(m);
            free(t);
        }
        if (*after) {
            char *t = tui_truncate_to_width(after, 60, NULL);
            char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last")), *m = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s %s", br, m);
            free(m);
            free(br);
            free(t);
        } else sb_appendf(&o, "\n %s", bottom);
    }
    push_split(out, sb_cstr(&o));
    sb_free(&o);
    free(bottom);
    free(bottom0);
    free(vb);
    free(ll);
    free(icon);
    strvec_free(&cl);
    free(after);
    free(before);
    free(before0);
    free(code);
    free(code0);
}

static void render_generic(const char *text, const strvec_t *lines, const char *action, bool expanded, strvec_t *out) {
    bool err = strstr(text, "Error:") || has_status_row(action, lines, tc_sym("status.error"));
    bool ok = has_status_row(action, lines, tc_sym("status.success")) || strstr(text, "Applied");
    char *icon = err && !ok ? tc_styled_sym("status.error", TUI_TC_ERROR) : ok && !err ? icon_lsp() : tc_styled_sym("status.info", TUI_TC_ACCENT);
    sb_t o = SB_INIT;
    if (expanded) {
        char *d = tc_fg(TUI_TC_DIM, "Output");
        sb_appendf(&o, "%s %s", icon, d);
        free(d);
        for (size_t i = 0; i < lines->n; i++) {
            char *br = tc_fg(TUI_TC_DIM, tc_sym(i + 1 == lines->n ? "tree.last" : "tree.branch"));
            char *t = tc_replace_tabs(lines->v[i]);
            sb_appendf(&o, "\n %s %s", br, t);
            free(t);
            free(br);
        }
    } else {
        const char *first = lines->n && *lines->v[0] ? lines->v[0] : "No output";
        bool more = lines->n > 1 || tui_visible_width(first) > 60;
        char *hint = more ? tc_expand_hint() : xstrdup("");
        char *t = tui_truncate_to_width(first, 60, NULL);
        char *d = tc_fg(TUI_TC_DIM, t);
        sb_appendf(&o, "%s %s%s", icon, d, hint);
        free(d);
        free(t);
        free(hint);
        size_t pv = lines->n > 1 ? MIN(lines->n - 1, (size_t)3) : 0;
        for (size_t i = 1; i <= pv; i++) {
            bool last = i == pv && lines->n <= 4;
            char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
            char *tr = str_trim(lines->v[i]);
            char *tt = tui_truncate_to_width(tr, 80, NULL);
            char *dd = tc_fg(TUI_TC_DIM, tt);
            sb_appendf(&o, "\n %s %s", br, dd);
            free(dd);
            free(tt);
            free(tr);
            free(br);
        }
        if (lines->n > 4) {
            char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
            char *mi = tc_more_items((int64_t)(lines->n - 4), "line");
            char *mm = tc_fg(TUI_TC_MUTED, mi);
            sb_appendf(&o, "\n %s %s", br, mm);
            free(mm);
            free(mi);
            free(br);
        }
    }
    push_split(out, sb_cstr(&o));
    sb_free(&o);
    free(icon);
}

/* symbols: `Symbols in <file>:` + indented `<icon> <name> @ line N` rows (top-level only collapsed) */
static void render_symbols(const char *file, const strvec_t *lines, bool expanded, strvec_t *out) {
    typedef struct {
        char *icon, *name, *line;
        int indent;
    } sym_t;
    VEC(sym_t) syms = {0};
    for (size_t i = 0; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (!strchr(l, '@') || !strstr(l, "line")) continue;
        int ind = (int)strspn(l, " \t");
        char *t = str_trim(l);
        char *sp = strpbrk(t, " \t");
        char *at = strrchr(t, '@');
        if (sp && at && at > sp) {
            const char *ln = at + 1;
            while (*ln == ' ') ln++;
            if (str_starts(ln, "line")) {
                ln += 4;
                while (*ln == ' ') ln++;
                if (isdigit((unsigned char)*ln)) {
                    char *name = xstrndup(sp, (size_t)(at - sp));
                    char *nt = str_trim(name);
                    sym_t s = {xstrndup(t, (size_t)(sp - t)), nt, xstrndup(ln, strspn(ln, "0123456789")), ind};
                    vec_push(&syms, s);
                    free(name);
                }
            }
        }
        free(t);
    }
    char *icon = tc_styled_sym("status.info", TUI_TC_ACCENT);
    char *in0 = xasprintf("in %s", file);
    char *in = tc_fg(TUI_TC_DIM, in0);
    const char *lang = tc_lang_from_path(file);
    sb_t o = SB_INIT;
    size_t top = 0;
    for (size_t i = 0; i < syms.n; i++) top += syms.v[i].indent == 0;
    if (expanded) {
        sb_appendf(&o, "%s %s", icon, in);
        for (size_t i = 0; i < syms.n; i++) {
            /* isLast: no later sibling at the same indent before a shallower one */
            bool last = true;
            for (size_t k = i + 1; k < syms.n; k++) {
                if (syms.v[k].indent < syms.v[i].indent) break;
                if (syms.v[k].indent == syms.v[i].indent) {
                    last = false;
                    break;
                }
            }
            sb_t pre = SB_INIT;
            sb_putc(&pre, ' ');
            for (int lvl = 2; lvl <= syms.v[i].indent; lvl += 2) {
                /* ancestor at indent lvl-2: nearest earlier symbol at that indent */
                bool alast = true;
                for (size_t a = i; a-- > 0;)
                    if (syms.v[a].indent == lvl - 2) {
                        for (size_t k = a + 1; k < syms.n; k++) {
                            if (syms.v[k].indent < lvl - 2) break;
                            if (syms.v[k].indent == lvl - 2) {
                                alast = false;
                                break;
                            }
                        }
                        break;
                    }
                sb_append(&pre, alast ? "   " : "\xe2\x94\x82  ");
            }
            char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
            char *ic = tc_fg(TUI_TC_ACCENT, syms.v[i].icon);
            strvec_t hl = {0};
            char *nm = tc_replace_tabs(syms.v[i].name);
            if (lang) tc_highlight(nm, lang, &hl);
            char *name = lang ? xstrdup(hl.n ? hl.v[0] : nm) : tc_fg(TUI_TC_ACCENT, nm);
            char *dp = tc_fg(TUI_TC_DIM, last ? "   " : "\xe2\x94\x82  ");
            char *lt = xasprintf("line %s", syms.v[i].line);
            char *lm = tc_fg(TUI_TC_MUTED, lt);
            sb_appendf(&o, "\n%s%s %s %s\n%s%s%s", sb_cstr(&pre), br, ic, name, sb_cstr(&pre), dp, lm);
            free(lm);
            free(lt);
            free(dp);
            free(name);
            free(nm);
            strvec_free(&hl);
            free(ic);
            free(br);
            sb_free(&pre);
        }
    } else {
        size_t shown = MIN(top, (size_t)3);
        char *hint = syms.n > shown ? tc_expand_hint() : xstrdup("");
        sb_appendf(&o, "%s %s%s", icon, in, hint);
        free(hint);
        size_t k = 0;
        for (size_t i = 0; i < syms.n && k < shown; i++) {
            if (syms.v[i].indent) continue;
            k++;
            bool last = k == shown && top <= 3;
            char *br = tc_fg(TUI_TC_DIM, tc_sym(last ? "tree.last" : "tree.branch"));
            char *ic = tc_fg(TUI_TC_ACCENT, syms.v[i].icon);
            strvec_t hl = {0};
            char *nm = tc_replace_tabs(syms.v[i].name);
            if (lang) tc_highlight(nm, lang, &hl);
            char *name = lang ? xstrdup(hl.n ? hl.v[0] : nm) : tc_fg(TUI_TC_ACCENT, nm);
            char *lt = xasprintf("line %s", syms.v[i].line);
            char *lm = tc_fg(TUI_TC_MUTED, lt);
            sb_appendf(&o, "\n %s %s %s %s", br, ic, name, lm);
            free(lm);
            free(lt);
            free(name);
            free(nm);
            strvec_free(&hl);
            free(ic);
            free(br);
        }
        if (top > 3) {
            char *br = tc_fg(TUI_TC_DIM, tc_sym("tree.last"));
            char *t = xasprintf("\xe2\x80\xa6 %zu more", top - 3);
            char *m = tc_fg(TUI_TC_MUTED, t);
            sb_appendf(&o, "\n %s %s", br, m);
            free(m);
            free(t);
            free(br);
        }
    }
    push_split(out, sb_cstr(&o));
    sb_free(&o);
    for (size_t i = 0; i < syms.n; i++) free(syms.v[i].icon), free(syms.v[i].name), free(syms.v[i].line);
    vec_free(&syms);
    free(in);
    free(in0);
    free(icon);
}

/* /Symbols in (.+):/ — the last ':' on the line (NULL-safe: returns s when none) */
static const char *sym_colon(const char *s) {
    const char *eol = s + strcspn(s, "\n"), *last = s;
    for (const char *p = s; p < eol; p++)
        if (*p == ':') last = p;
    return last;
}

void tc_lsp(const tc_call_t *c, int width, strvec_t *out) {
    if (!c->has_result) {
        lsp_call(c, width, out);
        return;
    }
    const json_t *first = json_at(c->content, 0);
    const char *text = str_eq(json_get_str(first, "type"), "text") ? json_get_str(first, "text") : NULL;
    if (!text || !*text) {
        char *ic = tc_status_icon(TC_ST_WARNING, -1);
        char *d = tc_fg(TUI_TC_DIM, "No result");
        char *j = xasprintf("%s LSP\n%s", ic, d);
        tc_push_wrapped(out, j, width);
        free(j);
        free(d);
        free(ic);
        return;
    }
    int cw = tc_block_content_width(width, -1, -1);
    strvec_t lines = {0};
    tc_split_lines(text, &lines);
    const json_t *req = c->args ? c->args : json_get_obj(c->details, "request");
    const char *action = json_get_str(req, "action") ? json_get_str(req, "action") : json_get_str(c->details, "action");
    tc_section_t secs[2] = {0};
    size_t ns = 0;
    {
        strvec_t *rl = &secs[0].lines;
        if (json_get_str(req, "file")) strvec_push_own(rl, tc_styled_path(json_get_str(req, "file")));
        if (json_get(req, "line")) {
            char *n = json_dump(json_get(req, "line"), 0);
            char *t = xasprintf("line %s", n);
            strvec_push_own(rl, tc_fg(TUI_TC_DIM, t));
            free(t);
            free(n);
        }
        if (json_get_str(req, "symbol")) {
            char *f = flat(json_get_str(req, "symbol"));
            char *t = xasprintf("symbol: %s", f);
            strvec_push_own(rl, tc_fg(TUI_TC_DIM, t));
            free(t);
            free(f);
        }
        if (json_get_str(req, "query")) {
            char *t = xasprintf("query: %s", json_get_str(req, "query"));
            strvec_push_own(rl, tc_fg(TUI_TC_DIM, t));
            free(t);
        }
        if (json_get_str(req, "new_name")) {
            char *t = xasprintf("new name: %s", json_get_str(req, "new_name"));
            strvec_push_own(rl, tc_fg(TUI_TC_DIM, t));
            free(t);
        }
        if (json_get(req, "apply")) {
            char *t = xasprintf("apply: %s", json_get_bool(req, "apply", false) ? "true" : "false");
            strvec_push_own(rl, tc_fg(TUI_TC_DIM, t));
            free(t);
        }
        if (rl->n) ns = 1;
    }
    strvec_t body = {0};
    const char *label = "Response";
    tc_state_t state = TC_STATE_SUCCESS;
    char *noun = NULL;
    int lcount = 0;
    locs_t locs = {0};
    const char *fence = strstr(text, "```");
    const char *fence_nl = fence ? strchr(fence + 3, '\n') : NULL;
    const char *fence_end = fence_nl ? strstr(fence_nl + 1, "```") : NULL;
    bool word_lang = fence_nl != NULL;
    if (fence_nl)
        for (const char *p = fence + 3; p < fence_nl; p++) word_lang &= isalnum((unsigned char)*p) || *p == '_';
    int errs = count_of(text, "error"), warns = count_of(text, "warning"), refs = count_of(text, "reference");
    size_t unreported = 0;
    for (size_t i = 0; i < lines.n; i++) unreported += strstr(lines.v[i], "no diagnostics report yet") != NULL;
    const char *syms = strstr(text, "Symbols in ");
    if (parse_locations(&lines, &noun, &lcount, &locs)) {
        label = "Locations";
        state = lcount > 0 ? TC_STATE_SUCCESS : TC_STATE_WARNING;
        render_locations(noun, lcount, &locs, c->expanded, cw, &body);
    } else if (fence && word_lang && fence_end) {
        label = "Hover";
        char *lang = xstrndup(fence + 3, (size_t)(fence_nl - fence - 3));
        render_hover(text, lang, fence_nl + 1, fence_end, c->expanded, &body);
        free(lang);
    } else if (errs >= 0 || warns >= 0 || has_status_row(action, &lines, tc_sym("status.error")) || unreported) {
        label = "Diagnostics";
        state = errs > 0 ? TC_STATE_ERROR : (warns > 0 || unreported) ? TC_STATE_WARNING : TC_STATE_SUCCESS;
        render_diagnostics(MAX(errs, 0), MAX(warns, 0), &lines, c->expanded, &body);
    } else if (refs >= 0) {
        label = "References";
        render_references(refs, &lines, c->expanded, &body);
    } else if (syms && sym_colon(syms + 11) > syms + 11) {
        label = "Symbols";
        const char *e = sym_colon(syms + 11);
        char *file = xstrndup(syms + 11, (size_t)(e - syms - 11));
        render_symbols(file, &lines, c->expanded, &body);
        free(file);
    } else if (str_eq(json_get_str(c->details, "action"), "diagnostics") && !strcmp(text, "OK")) {
        label = "Diagnostics";
        char *ic = icon_lsp(), *d = tc_fg(TUI_TC_DIM, "OK");
        strvec_push_own(&body, xasprintf("%s %s", ic, d));
        free(d);
        free(ic);
    } else {
        render_generic(text, &lines, action, c->expanded, &body);
    }
    const char *act0 = json_get_str(req, "action") ? json_get_str(req, "action") : json_get_str(c->details, "action");
    char *lower = NULL;
    if (!act0) {
        lower = xstrdup(label);
        for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        act0 = lower;
    }
    char *act = action_label(act0);
    bool success = !c->partial && !c->is_error;
    char *icon = success ? icon_lsp() : tc_status_icon(c->partial ? TC_ST_RUNNING : TC_ST_ERROR, c->spinner);
    char *header = xasprintf("%s LSP %s", icon, act);
    secs[ns].label = tc_fg(TUI_TC_TOOL_TITLE, "Response");
    for (size_t i = 0; i < body.n; i++) strvec_push(&secs[ns].lines, body.v[i]);
    ns++;
    tc_block_t blk = {.header = header, .state = c->partial ? TC_STATE_PENDING : state, .sections = secs, .nsections = ns, .width = width,
                      .no_bg = true, .pad_left = -1, .pad_right = -1, .border_slot = -1};
    tc_output_block(&blk, out);
    free((char *)secs[ns - 1].label);
    for (size_t i = 0; i < 2; i++) strvec_free(&secs[i].lines);
    free(header);
    free(icon);
    free(act);
    free(lower);
    for (size_t i = 0; i < locs.n; i++) {
        free(locs.v[i].file);
        strvec_free(&locs.v[i].ctx);
        vec_free(&locs.v[i].nums);
    }
    vec_free(&locs);
    free(noun);
    strvec_free(&body);
    strvec_free(&lines);
    (void)sev_slot_icon;
}
