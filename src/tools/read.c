/* read tool: files (line selectors, hashline anchors), directories, images, URLs, archives, SQLite, PDF, notebooks. */
#include "tools.h"
#include "xdev.h"
#include "vurl.h"

#include "config/settings.h"
#include "core/embed.h"
#include "core/fs.h"
#include "mcp/mcp.h"
#include "readfmt.h"
#include "summary.h"
#include "web.h"

#include <errno.h>

#include <ctype.h>
#include <sys/stat.h>

#define READ_MAX_BYTES (50u * 1024)
#define READ_MAX_LINE_COLS 2000
#define DEFAULT_LIMIT 300
#define CTX_BEFORE 1
#define CTX_AFTER 3

typedef struct {
    int64_t start; /* 1-based; negative => last N lines */
    int64_t end;   /* inclusive; 0 => open-ended */
} lrange_t;

typedef struct {
    bool raw, img, conflicts;
    VEC(lrange_t) ranges;
    bool has_sel;
    /* in-memory documents (archive members, PDFs, notebooks): no hashline anchors, no summary */
    bool immutable;
    const char *entity; /* "file" when NULL */
} selector_t;

static bool parse_range_chunk(const char *c, lrange_t *r) {
    char *end;
    if (*c == 'L') c++;
    if (*c == '-') {
        long long n = strtoll(c + 1, &end, 10);
        if (end == c + 1 || *end || n <= 0) return false;
        r->start = -n;
        r->end = 0;
        return true;
    }
    long long a = strtoll(c, &end, 10);
    if (end == c || a <= 0) return false;
    if (!*end) {
        r->start = a;
        r->end = 0; /* resolved by caller: single N is open-ended unless raw-single */
        return true;
    }
    if (*end == '-' || (end[0] == '.' && end[1] == '.')) {
        const char *b = *end == '-' ? end + 1 : end + 2;
        if (*b == 'L') b++;
        if (!*b) {
            r->start = a;
            r->end = 0;
            return true;
        }
        long long e2 = strtoll(b, &end, 10);
        if (*end || e2 < a) return false;
        r->start = a;
        r->end = e2;
        return true;
    }
    if (*end == '+') {
        const char *b = end + 1;
        if (*b == 'L') b++;
        long long cnt = strtoll(b, &end, 10);
        if (*end || cnt < 1) return false;
        r->start = a;
        r->end = a + cnt - 1;
        return true;
    }
    return false;
}

/* parse a single selector token (between colons) into sel; returns false if not a selector */
static bool parse_sel_token(const char *tok, selector_t *sel, bool *single_bare) {
    if (strcmp(tok, "raw") == 0) {
        sel->raw = true;
        return true;
    }
    if (strcmp(tok, "img") == 0) {
        sel->img = true;
        return true;
    }
    if (strcmp(tok, "conflicts") == 0) {
        sel->conflicts = true;
        return true;
    }
    strvec_t parts = str_split(tok, ",");
    VEC(lrange_t) tmp = {0};
    bool ok = parts.n > 0;
    for (size_t i = 0; i < parts.n && ok; i++) {
        const char *p = parts.v[i];
        if (*p == ':') p++;
        if (!*p) continue;
        lrange_t r;
        if (!parse_range_chunk(p, &r)) ok = false;
        else vec_push(&tmp, r);
    }
    if (ok && tmp.n) {
        *single_bare = parts.n == 1 && strspn(tok, "0123456789") == strlen(tok);
        for (size_t i = 0; i < tmp.n; i++) vec_push(&sel->ranges, tmp.v[i]);
    } else {
        ok = false;
    }
    vec_free(&tmp);
    strvec_free(&parts);
    return ok;
}

/* split "path:sel:sel" preferring literal existing files */
/* strip trailing :selector tokens from work (in place) into sel */
static void strip_sel(char *work, selector_t *sel) {
    bool single_bare = false;
    for (int guard = 0; guard < 3; guard++) {
        char *colon = strrchr(work, ':');
        if (!colon || colon == work) break;
        /* allow ':1-30,:-30' — find the earliest colon that starts a pure selector list */
        char *start = colon;
        while (start > work) {
            char *prev = start - 1;
            while (prev > work && *prev != ':') prev--;
            if (*prev != ':' || prev == work) break;
            if (prev[1] && strchr("0123456789-L", prev[1]) && start[-1] == ',') start = prev;
            else break;
        }
        selector_t trial = *sel;
        memset(&trial.ranges, 0, sizeof(trial.ranges));
        bool sb = false;
        if (!parse_sel_token(start + 1, &trial, &sb)) {
            vec_free(&trial.ranges);
            break;
        }
        for (size_t i = 0; i < trial.ranges.n; i++) vec_push(&sel->ranges, trial.ranges.v[i]);
        vec_free(&trial.ranges);
        sel->raw = trial.raw;
        sel->img = trial.img;
        sel->conflicts = trial.conflicts;
        if (sb) single_bare = true;
        sel->has_sel = true;
        *start = 0;
    }
    /* a lone bare N beside :raw is that single line */
    if (sel->raw && single_bare && sel->ranges.n == 1 && sel->ranges.v[0].end == 0 && sel->ranges.v[0].start > 0)
        sel->ranges.v[0].end = sel->ranges.v[0].start;
}

static char *split_path_sel(tool_env_t *env, const char *in, selector_t *sel) {
    memset(sel, 0, sizeof(*sel));
    char *abs = tool_resolve_path(env, in);
    if (fs_exists(abs)) return abs;
    free(abs);
    char *work = xstrdup(in);
    strip_sel(work, sel);
    char *r = tool_resolve_path(env, work);
    free(work);
    return r;
}

static const char *mime_for(const char *path) {
    const char *e = path_ext(path);
    if (!strcasecmp(e, ".png")) return "image/png";
    if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(e, ".gif")) return "image/gif";
    if (!strcasecmp(e, ".webp")) return "image/webp";
    return NULL;
}

static char *fmt_bytes(int64_t n) {
    if (n < 1024) return xasprintf("%lldB", (long long)n);
    if (n < 1024 * 1024) return xasprintf("%.1fKB", n / 1024.0);
    if (n < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", n / (1024.0 * 1024));
    return xasprintf("%.1fGB", n / (1024.0 * 1024 * 1024));
}

static char *fmt_age(int64_t mtime_ms) {
    int64_t d = (now_ms() - mtime_ms) / 1000;
    if (d < 0) d = 0;
    if (d < 60) return xasprintf("%llds ago", (long long)d);
    if (d < 3600) return xasprintf("%lldm ago", (long long)(d / 60));
    if (d < 86400) return xasprintf("%lldh ago", (long long)(d / 3600));
    if (d < 86400 * 30) return xasprintf("%lldd ago", (long long)(d / 86400));
    if (d < 86400 * 365) return xasprintf("%lldmo ago", (long long)(d / (86400 * 30)));
    return xasprintf("%lldy ago", (long long)(d / (86400 * 365)));
}

static int cmp_recent(const void *a, const void *b) {
    const dirent_info_t *x = a, *y = b;
    if (x->mtime_ms != y->mtime_ms) return x->mtime_ms < y->mtime_ms ? 1 : -1;
    return strcmp(x->name, y->name);
}

static bool skip_entry(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 || strcmp(name, ".DS_Store") == 0;
}

static void dir_tree(sb_t *out, const char *dir, int depth, int max_depth, const char *indent, bool root) {
    dirlist_t dl;
    if (fs_list_dir(dir, &dl) != 0) return;
    if (dl.n > 1) qsort(dl.v, dl.n, sizeof(dirent_info_t), cmp_recent);
    size_t shown = 0, limit = root ? SIZE_MAX : 12;
    size_t total = 0;
    for (size_t i = 0; i < dl.n; i++)
        if (!skip_entry(dl.v[i].name)) total++;
    for (size_t i = 0; i < dl.n; i++) {
        dirent_info_t *e = &dl.v[i];
        if (skip_entry(e->name)) continue;
        if (shown >= limit) break;
        shown++;
        char *age = fmt_age(e->mtime_ms);
        if (e->is_dir) {
            sb_appendf(out, "%s%s/ (%s)\n", indent, e->name, age);
            if (depth + 1 < max_depth) {
                char *sub = path_join(dir, e->name);
                char *ind = xasprintf("%s  ", indent);
                dir_tree(out, sub, depth + 1, max_depth, ind, false);
                free(ind);
                free(sub);
            }
        } else {
            char *sz = fmt_bytes(e->size);
            sb_appendf(out, "%s%s (%s, %s)\n", indent, e->name, sz, age);
            free(sz);
        }
        free(age);
    }
    if (shown < total) sb_appendf(out, "%s… %zu more\n", indent, total - shown);
    if (root && total == 0) sb_append(out, "(empty directory)\n");
    dirlist_free(&dl);
}

/* render selected lines of text */
typedef struct {
    strvec_t lines; /* without trailing newline */
    bool trailing_nl;
} text_lines_t;

static void split_text(const char *s, size_t len, text_lines_t *t) {
    memset(t, 0, sizeof(*t));
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(s + i, '\n', len - i);
        size_t end = nl ? (size_t)(nl - s) : len;
        size_t e = end;
        if (e > i && s[e - 1] == '\r') e--;
        strvec_push_own(&t->lines, xstrndup(s + i, e - i));
        i = nl ? end + 1 : len;
        if (!nl) break;
    }
    t->trailing_nl = len > 0 && s[len - 1] == '\n';
}

static void emit_line(sb_t *out, int64_t no, const char *text, bool numbered) {
    size_t n = strlen(text);
    bool cut = n > READ_MAX_LINE_COLS;
    if (numbered) sb_appendf(out, "%lld:", (long long)no);
    if (cut) {
        size_t k = READ_MAX_LINE_COLS;
        while (k > 0 && ((unsigned char)text[k] & 0xC0) == 0x80) k--;
        sb_appendn(out, text, k);
        sb_appendf(out, "… [line truncated, %zu chars]", n);
    } else {
        sb_append(out, text);
    }
    sb_putc(out, '\n');
}

static int read_text(tool_env_t *env, const char *abs, const char *display, const char *data, size_t len,
                     selector_t *sel, tool_result_t *out) {
    text_lines_t t;
    split_text(data, len, &t);
    int64_t total = (int64_t)t.lines.n;
    bool hashline = !sel->raw && !sel->immutable && str_eq(env->edit_mode, "hashline");
    bool numbered = hashline || (!sel->raw && settings_bool("readLineNumbers", false));
    const char *ent = sel->entity ? sel->entity : "file";
    sb_t b = SB_INIT;
    json_t *details = json_obj();
    json_set_str(details, "resolvedPath", abs);
    int64_t seen_lo = -1, seen_hi = -1;
    bool whole = false;

    summary_lang_t slang = summary_lang(abs);
    bool want_summary = !sel->ranges.n && !sel->raw && !sel->immutable && slang != SUM_NONE && total >= 100 && len <= (2u << 20) &&
                        total <= 20000 && settings_bool("read.summarize.enabled", true);
    bool *keep = NULL;
    size_t elided = 0;
    if (want_summary) {
        keep = xcalloc((size_t)total, sizeof(bool));
        summary_build(&t.lines, slang, keep);
        for (int64_t i = 0; i < total; i++)
            if (!keep[i]) elided++;
        if (elided < 5) {
            free(keep);
            keep = NULL;
        }
    }
    if (keep) {
        sb_t body = SB_INIT;
        VEC(line_span_t) runs = {0};
        int64_t i = 0;
        const char *tag = NULL;
        while (i < total) {
            if (keep[i]) {
                int64_t j = i;
                while (j + 1 < total && keep[j + 1]) j++;
                if (hashline) tag = edit_store_record(&env->store, abs, data, i + 1, j + 1);
                for (int64_t k = i; k <= j; k++) {
                    /* merge "opener {" + elided body + "}" into one anchored line */
                    bool merged = false;
                    if (k == j && j + 1 < total && !keep[j + 1]) {
                        int64_t e = j + 1;
                        while (e < total && !keep[e]) e++;
                        const char *op = t.lines.v[k];
                        size_t ol = strlen(op);
                        while (ol && isspace((unsigned char)op[ol - 1])) ol--;
                        const char *cl = e < total ? t.lines.v[e] + strspn(t.lines.v[e], " \t") : "";
                        if (ol && op[ol - 1] == '{' && e < total && (!strcmp(cl, "}") || !strcmp(cl, "};") || !strcmp(cl, "},"))) {
                            char *m = xasprintf("%.*s … %s", (int)ol, op, cl);
                            emit_line(&body, k + 1, m, numbered);
                            free(m);
                            line_span_t r = {j + 2, e};
                            vec_push(&runs, r);
                            i = e + 1; /* closer consumed */
                            merged = true;
                        }
                    }
                    if (!merged) emit_line(&body, k + 1, t.lines.v[k], numbered);
                    if (merged) goto next_run;
                }
                i = j + 1;
            } else {
                int64_t j = i;
                while (j + 1 < total && !keep[j + 1]) j++;
                sb_append(&body, "…\n");
                line_span_t r = {i + 1, j + 1};
                vec_push(&runs, r);
                i = j + 1;
            }
        next_run:;
        }
        if (hashline) format_hashline_header(&b, display, tag ? tag : edit_store_record(&env->store, abs, data, 1, 1));
        sb_append(&b, sb_cstr(&body));
        sb_free(&body);
        int64_t elines = 0;
        for (size_t k = 0; k < runs.n; k++) elines += runs.v[k].hi - runs.v[k].lo + 1;
        sb_t ex = SB_INIT;
        size_t samples = MIN(runs.n, (size_t)3);
        for (size_t k = 0; k < samples; k++) {
            if (k) sb_putc(&ex, ',');
            if (runs.v[k].lo == runs.v[k].hi) sb_appendf(&ex, "%lld", (long long)runs.v[k].lo);
            else sb_appendf(&ex, "%lld-%lld", (long long)runs.v[k].lo, (long long)runs.v[k].hi);
        }
        sb_appendf(&b, "[…%lldln elided; re-read needed ranges%s %s:%s]", (long long)elines,
                   runs.n > samples ? ", e.g." : " with", display, sb_cstr(&ex));
        sb_free(&ex);
        json_t *sum = json_obj();
        json_set_int(sum, "lines", total);
        json_set_int(sum, "elidedSpans", (int64_t)runs.n);
        json_set_int(sum, "elidedLines", elines);
        json_set(details, "summary", sum);
        vec_free(&runs);
        free(keep);
    } else if (!sel->ranges.n) {
        int64_t limit = DEFAULT_LIMIT;
        int64_t end = MIN(total, limit);
        size_t bytes = 0;
        int64_t i;
        sb_t body = SB_INIT;
        for (i = 1; i <= end; i++) {
            if (bytes > READ_MAX_BYTES) break;
            size_t before = body.len;
            emit_line(&body, i, t.lines.v[i - 1], numbered);
            bytes += body.len - before;
        }
        int64_t last = i - 1;
        if (hashline) {
            const char *tag = edit_store_record(&env->store, abs, data, last >= total ? 0 : 1, last);
            format_hashline_header(&b, display, tag);
        }
        sb_append(&b, sb_cstr(&body));
        sb_free(&body);
        if (last < total)
            sb_appendf(&b, "\n[%lld more lines in %s. Use :%lld to continue]", (long long)(total - last), ent,
                       (long long)(last + 1));
        else if (total == 0)
            sb_appendf(&b, "(empty %s)", ent);
        whole = last >= total;
        seen_lo = 1;
        seen_hi = last;
    } else {
        /* normalize ranges */
        VEC(lrange_t) rs = {0};
        sb_t notes = SB_INIT;
        for (size_t k = 0; k < sel->ranges.n; k++) {
            lrange_t r = sel->ranges.v[k];
            if (r.start < 0) {
                r.start = MAX(1, total + r.start + 1);
                r.end = total;
            } else if (r.end == 0) {
                r.end = MIN(total, r.start + DEFAULT_LIMIT - 1);
            }
            if (r.start > total) {
                if (sel->ranges.n == 1) {
                    sb_appendf(&notes, "Line %lld is beyond end of %s (%lld lines total). Use :%lld to read the end.",
                               (long long)r.start, ent, (long long)total, (long long)MAX(1, total - 20));
                } else {
                    sb_appendf(&notes, "[Range %lld is beyond end of %s (%lld lines total); skipped]\n", (long long)r.start,
                               ent, (long long)total);
                }
                continue;
            }
            if (r.end > total) r.end = total;
            vec_push(&rs, r);
        }
        /* sort + merge */
        for (size_t i = 1; i < rs.n; i++)
            for (size_t j = i; j > 0 && rs.v[j].start < rs.v[j - 1].start; j--) {
                lrange_t tmp = rs.v[j];
                rs.v[j] = rs.v[j - 1];
                rs.v[j - 1] = tmp;
            }
        size_t m = 0;
        for (size_t i = 0; i < rs.n; i++) {
            if (m && rs.v[i].start <= rs.v[m - 1].end + 1) rs.v[m - 1].end = MAX(rs.v[m - 1].end, rs.v[i].end);
            else rs.v[m++] = rs.v[i];
        }
        rs.n = m;
        /* context lines for a single bounded non-raw range */
        bool explicit_end = sel->ranges.n == 1 && sel->ranges.v[0].end != 0 && sel->ranges.v[0].start > 0;
        if (rs.n == 1 && !sel->raw && explicit_end) {
            rs.v[0].start = MAX(1, rs.v[0].start - CTX_BEFORE);
            rs.v[0].end = MIN(total, rs.v[0].end + CTX_AFTER);
        }
        sb_t body = SB_INIT;
        int64_t last_shown = 0;
        size_t bytes = 0;
        bool cut = false;
        for (size_t i = 0; i < rs.n && !cut; i++) {
            if (i && numbered) sb_append(&body, "…\n");
            for (int64_t ln = rs.v[i].start; ln <= rs.v[i].end; ln++) {
                if (bytes > READ_MAX_BYTES) {
                    cut = true;
                    break;
                }
                size_t before = body.len;
                emit_line(&body, ln, t.lines.v[ln - 1], numbered);
                bytes += body.len - before;
                last_shown = ln;
            }
        }
        if (sel->raw && body.len && body.s[body.len - 1] == '\n') {
            bool open_to_eof = rs.n == 1 && sel->ranges.v[0].end == 0 && last_shown == total && t.trailing_nl;
            if (!open_to_eof) sb_truncate(&body, body.len - 1);
        }
        if (hashline && rs.n) {
            const char *tag = NULL;
            for (size_t i = 0; i < rs.n; i++) {
                int64_t hi = (i + 1 == rs.n) ? last_shown : rs.v[i].end;
                tag = edit_store_record(&env->store, abs, data, rs.v[i].start, hi);
            }
            format_hashline_header(&b, display, tag);
        }
        sb_append(&b, sb_cstr(&body));
        sb_free(&body);
        if (cut || (rs.n == 1 && sel->ranges.v[0].end == 0 && last_shown < total && !sel->raw))
            sb_appendf(&b, "\n[%lld more lines in %s. Use :%lld to continue]", (long long)(total - last_shown), ent,
                       (long long)(last_shown + 1));
        if (notes.len) {
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, sb_cstr(&notes));
        }
        sb_free(&notes);
        vec_free(&rs);
    }
    UNUSED(whole);
    UNUSED(seen_lo);
    UNUSED(seen_hi);
    tool_result_text(out, sb_cstr(&b));
    json_set_int(details, "totalLines", total);
    out->details = details;
    sb_free(&b);
    strvec_free(&t.lines);
    return 0;
}

static int read_conflicts(const char *display, const char *data, tool_result_t *out) {
    strvec_t lines = str_split_lines(data, strlen(data));
    sb_t b = SB_INIT;
    int n = 0;
    int64_t start = 0;
    for (size_t i = 0; i < lines.n; i++) {
        if (str_starts(lines.v[i], "<<<<<<<")) start = (int64_t)i + 1;
        else if (str_starts(lines.v[i], ">>>>>>>") && start) {
            sb_appendf(&b, "#%d L%lld-L%lld\n", ++n, (long long)start, (long long)i + 1);
            start = 0;
        }
    }
    if (!n) sb_appendf(&b, "No unresolved merge conflicts in %s", display);
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    strvec_free(&lines);
    return 0;
}

/* ---------- structured formats: archives, SQLite, PDF, notebooks (readfmt.c) ---------- */

/* render in-memory text through the selector pipeline as an immutable document */
static void read_mem(tool_env_t *env, const char *abs, const char *text, size_t len, selector_t *sel, const char *entity,
                     tool_result_t *out) {
    sel->immutable = true;
    sel->entity = entity;
    if (sel->conflicts) read_conflicts(abs, text, out);
    else read_text(env, abs, abs, text, len, sel, out);
}

/* upstream PDF_PAGE_MEMBER_RE ("p2.png", "page-3_img0.png") plus omc's "page2" / "page2-4" text selectors */
static bool pdf_page_token(const char *tok, int *first, int *last) {
    const char *p = tok;
    if (!strncasecmp(p, "page", 4)) {
        p += 4;
        if (*p == '-' || *p == '_') p++;
    } else if (*p == 'p' || *p == 'P') {
        p++;
    } else {
        return false;
    }
    if (!isdigit((unsigned char)*p)) return false;
    char *e;
    long a = strtol(p, &e, 10), b = a;
    if (a < 1 || a > 1000000) return false;
    size_t el = strlen(e);
    if (!strcasecmp(e, ".png") || ((*e == '-' || *e == '_') && el >= 5 && !strcasecmp(e + el - 4, ".png"))) {
        *first = *last = (int)a;
        return true;
    }
    if (*e == '-' && isdigit((unsigned char)e[1])) {
        b = strtol(e + 1, &e, 10);
        if (b < a || b > 1000000) return false;
    }
    if (*e) return false;
    *first = (int)a;
    *last = (int)b;
    return true;
}

/* extract a PDF and render pages [first,last] (0 = all) through the selector pipeline */
static void read_pdf_bytes(tool_env_t *env, const char *abs, const char *data, size_t len, int first, int last,
                           selector_t *sel, tool_result_t *out) {
    strvec_t pages;
    char *err = NULL;
    if (rf_pdf_extract(data, len, &pages, &err) != 0) {
        tool_result_textf(out, "[Cannot read .pdf file: %s]", err);
        free(err);
        return;
    }
    char *text = rf_pdf_render(&pages, first, last, &err);
    if (!text) {
        tool_result_textf(out, "[Cannot read .pdf file: %s]", err);
        free(err);
    } else {
        sb_t b = SB_INIT;
        if (first > 0) {
            sb_appendf(&b, "[omc cannot render PDF pages as images; showing the extracted text of page %d", first);
            if (last > first) sb_appendf(&b, "-%d", last);
            sb_append(&b, "]\n");
        }
        sb_append(&b, text);
        read_mem(env, abs, sb_cstr(&b), b.len, sel, "document", out);
        sb_free(&b);
        free(text);
    }
    strvec_free(&pages);
}

static void read_archive(tool_env_t *env, const char *path, const char *abs, const char *sub, tool_result_t *out) {
    char *err = NULL;
    rf_archive_t *a = rf_archive_open(abs, &err);
    if (!a) {
        tool_result_error(out, "%s", err);
        free(err);
        return;
    }
    selector_t sel;
    memset(&sel, 0, sizeof sel);
    char *member = xstrdup(sub);
    strip_sel(member, &sel);
    int64_t size = 0;
    int kind = rf_archive_stat(a, member, &size);
    int pdf_first = 0, pdf_last = 0;
    if (!kind && *sub) {
        if (rf_archive_stat(a, sub, &size)) { /* member names win over selector-looking suffixes */
            free(member);
            member = xstrdup(sub);
            vec_free(&sel.ranges);
            memset(&sel, 0, sizeof sel);
            kind = rf_archive_stat(a, member, &size);
        } else {
            /* "archive.zip:500" / "archive.zip:raw": the whole remainder is a selector on the root */
            selector_t s2;
            memset(&s2, 0, sizeof s2);
            char *w = xasprintf("x:%s", sub);
            strip_sel(w, &s2);
            if (!strcmp(w, "x") && s2.has_sel) {
                free(member);
                member = xstrdup("");
                vec_free(&sel.ranges);
                sel = s2;
                kind = 2;
            } else {
                vec_free(&s2.ranges);
            }
            free(w);
        }
    }
    if (!kind) { /* "doc.pdf:page2.png" inside the archive */
        char *colon = strrchr(member, ':');
        if (colon && colon - member >= 4 && !strncasecmp(colon - 4, ".pdf", 4) &&
            pdf_page_token(colon + 1, &pdf_first, &pdf_last)) {
            *colon = 0;
            kind = rf_archive_stat(a, member, &size) == 1 ? 1 : 0;
        }
    }
    json_t *details = NULL;
    if (!kind) {
        tool_result_error(out, "Path '%s' not found inside archive", path);
    } else if (kind == 2) {
        rf_dirents_t ents;
        if (sel.ranges.n > 1) {
            tool_result_error(out, "Multi-range line selectors are not supported for archive directory listings.");
        } else if (rf_archive_list(a, member, &ents, &err) != 0) {
            tool_result_error(out, "%s", err);
            free(err);
        } else {
            /* selectors address entries with line semantics: ":50" starts at the 50th, ":-20" lists the last 20 */
            int64_t total = (int64_t)ents.n, off = 1, limit = 500;
            if (sel.ranges.n == 1) {
                lrange_t r = sel.ranges.v[0];
                if (r.start < 0) {
                    off = MAX(1, total + r.start + 1);
                    limit = MAX((int64_t)1, MIN(-r.start, total));
                } else {
                    off = r.start;
                    if (r.end) limit = r.end - r.start + 1;
                }
            }
            int64_t avail = MAX((int64_t)0, total - (off - 1));
            int64_t shown = MIN(avail, limit);
            char *list = shown > 0 ? rf_format_dirents(ents.v + (off - 1), (size_t)shown) : xstrdup("(empty archive directory)");
            if (limit > 0 && avail >= limit)
                tool_result_textf(out, "%s\n\n[%lld results limit reached. Use limit=%lld for more]", list, (long long)limit,
                                  (long long)limit * 2);
            else
                tool_result_text(out, list);
            free(list);
            details = json_obj();
            json_set_str(details, "resolvedPath", abs);
            json_set_bool(details, "isDirectory", true);
            rf_dirents_free(&ents);
        }
    } else {
        size_t len = 0;
        char *norm = NULL;
        char *data = rf_archive_read(a, member, &len, &norm, &err);
        if (!data) {
            tool_result_error(out, "%s", err);
            free(err);
        } else if (strlen(norm) >= 4 && !strcasecmp(norm + strlen(norm) - 4, ".pdf")) {
            char *mp = xasprintf("%s:%s", abs, norm);
            read_pdf_bytes(env, mp, data, len, pdf_first, pdf_last, &sel, out);
            free(mp);
        } else if (!rf_is_text(data, len)) {
            char *sz = rf_fmt_bytes((int64_t)len);
            tool_result_textf(out, "[Cannot read binary archive entry '%s' (%s)]", norm, sz);
            free(sz);
        } else {
            read_mem(env, abs, data, len, &sel, "archive entry", out);
        }
        free(norm);
        free(data);
    }
    if (details) {
        json_free(out->details);
        out->details = details;
    }
    vec_free(&sel.ranges);
    free(member);
    rf_archive_free(a);
}

/* archive members ("x.zip:dir/file:10-20") and SQLite selectors ("x.db:table?limit=5"); true when handled */
static bool read_structured(tool_env_t *env, const char *path, tool_result_t *out) {
    char *full = tool_resolve_path(env, path);
    bool literal = fs_exists(full);
    free(full);
    rf_splits_t c;
    rf_archive_candidates(path, &c);
    for (size_t i = 0; i < c.n; i++) {
        if (literal && *c.v[i].sub) continue; /* a real file name with a selector-looking suffix wins */
        char *abs = tool_resolve_path(env, c.v[i].container);
        struct stat st;
        bool ok = stat(abs, &st) == 0 && !S_ISDIR(st.st_mode);
        if (ok) {
            read_archive(env, path, abs, c.v[i].sub, out);
            if (!out->details) {
                out->details = json_obj();
                json_set_str(out->details, "resolvedPath", abs);
            }
        }
        free(abs);
        if (ok) {
            rf_splits_free(&c);
            return true;
        }
    }
    rf_splits_free(&c);
    rf_sqlite_candidates(path, &c);
    for (size_t i = 0; i < c.n; i++) {
        if (literal && (*c.v[i].sub || *c.v[i].query)) continue;
        char *abs = tool_resolve_path(env, c.v[i].container);
        struct stat st;
        bool ok = stat(abs, &st) == 0 && S_ISREG(st.st_mode) && rf_sqlite_sniff(abs);
        if (ok) {
            bool is_err = false;
            char *txt = rf_sqlite_read(abs, c.v[i].sub, c.v[i].query, &is_err);
            if (is_err) tool_result_error(out, "%s", txt);
            else tool_result_text(out, txt);
            free(txt);
            json_free(out->details);
            out->details = json_obj();
            json_set_str(out->details, "resolvedPath", abs);
        }
        free(abs);
        if (ok) {
            rf_splits_free(&c);
            return true;
        }
    }
    rf_splits_free(&c);
    return false;
}

/* "doc.pdf:page2.png" / "doc.pdf:page2-4[:sel]" when no such literal file exists; true when handled */
static bool read_pdf_pages(tool_env_t *env, const char *path, tool_result_t *out) {
    const char *hit = NULL;
    for (const char *p = path; (p = strchr(p, ':')); p++)
        if (p - path >= 4 && !strncasecmp(p - 4, ".pdf", 4)) hit = p;
    if (!hit) return false;
    char *full = tool_resolve_path(env, path);
    bool literal = fs_exists(full);
    free(full);
    if (literal) return false;
    char *rest = xstrdup(hit + 1);
    char *selpart = strchr(rest, ':');
    if (selpart) *selpart++ = 0;
    int first, last;
    if (!pdf_page_token(rest, &first, &last)) {
        free(rest);
        return false;
    }
    char *pdf = xstrndup(path, (size_t)(hit - path));
    char *abs = tool_resolve_path(env, pdf);
    selector_t sel;
    memset(&sel, 0, sizeof sel);
    if (selpart) {
        char *w = xasprintf("x:%s", selpart);
        strip_sel(w, &sel);
        free(w);
    }
    size_t len = 0;
    char *data = fs_is_file(abs) ? fs_read_limit(abs, 256u << 20, &len, NULL) : NULL;
    if (!data) tool_result_error(out, "Path '%s' not found", pdf);
    else read_pdf_bytes(env, abs, data, len, first, last, &sel, out);
    free(data);
    vec_free(&sel.ranges);
    free(abs);
    free(pdf);
    free(rest);
    return true;
}

static int read_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out);

static int read_ssh(tool_env_t *env, const char *path, tool_result_t *out) {
    selector_t sel;
    memset(&sel, 0, sizeof sel);
    char *url = xstrdup(path);
    char *ps = strchr(url + 6, '/'); /* selectors only after the authority */
    if (ps) strip_sel(ps, &sel);
    char *data = NULL, *err = NULL;
    size_t len = 0;
    bool is_dir = false;
    if (ssh_fetch(env, url, &data, &len, &is_dir, &err) != 0) {
        tool_result_error(out, "%s", err ? err : "ssh:// read failed");
    } else if (is_dir || sel.raw || sel.conflicts) {
        if (sel.ranges.n) {
            tool_env_t tmp = *env;
            tmp.edit_mode = "raw";
            sel.raw = true;
            read_text(&tmp, url, url, data, len, &sel, out);
        } else {
            tool_result_text(out, data);
        }
        if (is_dir) {
            if (!out->details) out->details = json_obj();
            json_set_bool(out->details, "isDirectory", true);
        }
    } else {
        read_text(env, url, url, data, len, &sel, out);
    }
    free(err);
    free(data);
    free(url);
    vec_free(&sel.ranges);
    return 0;
}

/* render a virtual (non-file) text resource honoring line selectors */
static void read_virtual(tool_env_t *env, const char *url, const char *text, selector_t *sel, tool_result_t *out) {
    sel->immutable = true;
    tool_env_t tmp = *env;
    tmp.edit_mode = "raw";
    read_text(&tmp, url, url, text, strlen(text), sel, out);
}

/* omc:// harness docs (port of omp:// docs): README.md and docs/ (*.md) embedded at build time */
static bool omc_doc_name(const char *embed_name, const char **doc) {
    if (!strcmp(embed_name, "README.md")) {
        *doc = embed_name;
        return true;
    }
    if (str_starts(embed_name, "docs/") && str_ends(embed_name, ".md") && !strchr(embed_name + 5, '/')) {
        *doc = embed_name + 5;
        return true;
    }
    return false;
}

static int read_omc_docs(tool_env_t *env, const char *path, tool_result_t *out) {
    char *work = xstrdup(path + strlen("omc://"));
    selector_t sel;
    memset(&sel, 0, sizeof sel);
    strip_sel(work, &sel);
    char *name = work;
    while (*name == '/') name++;
    if (str_starts(name, "docs/")) name += 5;
    else if (!strcmp(name, "docs")) name += 4;
    size_t nl = strlen(name);
    while (nl && name[nl - 1] == '/') name[--nl] = 0;
    if (strstr(name, "..") || name[0] == '/') {
        tool_result_error(out, "Invalid omc:// path: %s", path);
        goto done;
    }
    if (!*name) {
        sb_t b = SB_INIT;
        size_t count = 0;
        const char *doc;
        for (size_t i = 0; i < omc_embedded_count; i++)
            if (omc_doc_name(omc_embedded[i].name, &doc)) count++;
        sb_appendf(&b, "# Documentation\n\n%zu files available:\n\n", count);
        for (size_t i = 0; i < omc_embedded_count; i++)
            if (omc_doc_name(omc_embedded[i].name, &doc)) sb_appendf(&b, "- [%s](omc://%s)\n", doc, doc);
        char *listing = sb_detach(&b);
        read_virtual(env, path, listing, &sel, out);
        free(listing);
        goto done;
    }
    char *want = str_ends(name, ".md") ? xstrdup(name) : xasprintf("%s.md", name);
    const char *found = NULL;
    const char *doc;
    for (size_t i = 0; i < omc_embedded_count && !found; i++)
        if (omc_doc_name(omc_embedded[i].name, &doc) && !strcmp(doc, want)) found = omc_embedded[i].name;
    if (found) {
        size_t len;
        const char *data = embed_get(found, &len);
        read_virtual(env, path, data ? data : "", &sel, out);
    } else {
        sb_t sug = SB_INIT;
        int n = 0;
        char *stem = xstrndup(want, strlen(want) - 3);
        for (size_t i = 0; i < omc_embedded_count && n < 5; i++) {
            if (!omc_doc_name(omc_embedded[i].name, &doc)) continue;
            char *dstem = xstrndup(doc, strlen(doc) - 3);
            if (strstr(doc, stem) || strstr(stem, dstem)) sb_appendf(&sug, "%s%s", n++ ? ", " : "", doc);
            free(dstem);
        }
        char *s2 = sb_detach(&sug);
        if (n) tool_result_error(out, "Documentation file not found: %s\nDid you mean: %s", want, s2);
        else tool_result_error(out, "Documentation file not found: %s\nUse omc:// to list available files.", want);
        free(s2);
        free(stem);
    }
    free(want);
done:
    free(work);
    vec_free(&sel.ranges);
    return 0;
}

/* mcp://<resource-uri>: read a resource from the connected MCP server that advertises it */
static int read_mcp(tool_env_t *env, const char *path, tool_result_t *out) {
    const char *rest = path + strlen("mcp://");
    char *uri = str_trim(rest);
    size_t ul = strlen(uri);
    if (ul && uri[ul - 1] == '/' && !strchr(uri, '/')[1]) uri[ul - 1] = 0; /* host-only form: drop bare "/" */
    if (!*uri) {
        tool_result_error(out, "mcp:// URL requires a resource URI: mcp://<resource-uri>");
        free(uri);
        return 0;
    }
    char *text = NULL, *server = NULL, *err = NULL;
    if (mcp_read_resource(mcp_default_manager(), uri, &text, &server, &err) != 0) {
        tool_result_error(out, "%s", err ? err : "MCP resource read failed");
    } else {
        tool_result_text(out, text);
        out->details = json_obj();
        json_set_str(out->details, "url", path);
        json_set_str(out->details, "server", server);
    }
    free(text);
    free(server);
    free(err);
    free(uri);
    return 0;
}

static int read_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *path = json_get_str(args, "path");
    if (!path || !*path) {
        tool_result_error(out, "path is required");
        return 0;
    }
    if (str_starts(path, "http://") || str_starts(path, "https://")) {
        return web_read_url(env, path, ctx->cancel, out);
    }
    if (xdev_read(ctx ? ctx->agent : NULL, path, out)) return 0;
    if (str_starts(path, "pr://") || str_starts(path, "issue://")) return github_read(env, path, out);
    if (str_starts(path, "ssh://")) return read_ssh(env, path, out);
    if (vurl_is(path)) return vurl_read(env, path, out);
    if (str_starts(path, "omc://")) return read_omc_docs(env, path, out);
    if (str_starts(path, "mcp://")) return read_mcp(env, path, out);
    char *uerr = NULL;
    char *internal = tool_resolve_internal_url(env, path, &uerr);
    if (uerr) {
        tool_result_error(out, "%s", uerr);
        free(uerr);
        return 0;
    }
    if (internal) {
        json_t *a2 = json_obj();
        json_set_str(a2, "path", internal);
        free(internal);
        int rc = read_execute(self, ctx, a2, out);
        json_free(a2);
        return rc;
    }
    if (read_structured(env, path, out) || read_pdf_pages(env, path, out)) return 0;
    selector_t sel;
    char *abs = split_path_sel(env, path, &sel);
    char *display = path_display(abs, env->cwd);
    struct stat st;
    if (stat(abs, &st) != 0) {
        tool_result_error(out, "Path '%s' not found", path);
        goto done;
    }
    if (S_ISDIR(st.st_mode)) {
        sb_t b = SB_INIT;
        dir_tree(&b, abs, 0, 2, "", true);
        char *listing = sb_detach(&b);
        if (sel.ranges.n) {
            /* slice rendered lines */
            selector_t s2 = sel;
            s2.raw = true;
            tool_env_t tmp = *env;
            char *mode = tmp.edit_mode;
            tmp.edit_mode = "raw";
            read_text(&tmp, abs, display, listing, strlen(listing), &s2, out);
            tmp.edit_mode = mode;
        } else {
            tool_result_text(out, listing);
        }
        free(listing);
        if (!out->details) out->details = json_obj();
        json_set_bool(out->details, "isDirectory", true);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        const char *kind = S_ISCHR(st.st_mode) ? "character device" : S_ISBLK(st.st_mode) ? "block device"
                           : S_ISFIFO(st.st_mode) ? "FIFO" : S_ISSOCK(st.st_mode) ? "socket" : "special file";
        tool_result_error(out, "Cannot read '%s': it is a %s, not a regular file or directory.", path, kind);
        goto done;
    }
    const char *mime = mime_for(abs);
    if (mime && !sel.raw) {
        size_t n;
        char *data = fs_read_limit(abs, 20u << 20, &n, NULL);
        if (!data) {
            tool_result_error(out, "Cannot read %s: %s", display, strerror(errno));
            goto done;
        }
        char *b64 = base64_encode(data, n);
        char *sz = fmt_bytes((int64_t)n);
        tool_result_textf(out, "Read image file [%s] (%s)", mime, sz);
        tool_result_image(out, b64, mime);
        free(sz);
        free(b64);
        free(data);
        goto done;
    }
    if (!strcasecmp(path_ext(abs), ".pdf") && !sel.raw) {
        size_t plen = 0;
        char *pdata = fs_read_limit(abs, 256u << 20, &plen, NULL);
        if (!pdata) tool_result_error(out, "Cannot read %s: %s", display, strerror(errno));
        else read_pdf_bytes(env, abs, pdata, plen, 0, 0, &sel, out);
        free(pdata);
        goto done;
    }
    size_t len;
    bool trunc = false;
    char *data = fs_read_limit(abs, 64u << 20, &len, &trunc);
    if (!data) {
        tool_result_error(out, "Cannot read %s: %s", display, strerror(errno));
        goto done;
    }
    if (!strcasecmp(path_ext(abs), ".ipynb") && !sel.raw) {
        char *nerr = NULL;
        char *nb = rf_ipynb_text(data, len, display, &nerr);
        if (!nb) tool_result_error(out, "%s", nerr);
        else read_mem(env, abs, nb, strlen(nb), &sel, "notebook", out);
        free(nb);
        free(nerr);
        free(data);
        goto done;
    }
    if (!sel.raw && fs_is_binary(data, len)) {
        char *sz = fmt_bytes((int64_t)len);
        tool_result_textf(out, "[Binary file %s (%s); use :raw to force a text read]", display, sz);
        free(sz);
        free(data);
        goto done;
    }
    if (sel.conflicts) read_conflicts(display, data, out);
    else read_text(env, abs, display, data, len, &sel, out);
    free(data);
done:
    vec_free(&sel.ranges);
    free(abs);
    free(display);
    return 0;
}

agent_tool_t *tool_read_new(tool_env_t *env) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "IS_HL_MODE", str_eq(env->edit_mode, "hashline"));
    char *desc = tool_prompt("read", ctx);
    json_free(ctx);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Local path, "
                           "internal URI, or URL; selectors inline.\"}},\"required\":[\"path\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("read", "Read", desc, s, read_execute);
    free(desc);
    t->ud = env;
    return t;
}
