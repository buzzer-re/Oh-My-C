/* read tool: files (line selectors, hashline anchors), directories, images, URLs. */
#include "tools.h"

#include "config/settings.h"
#include "core/fs.h"
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
static char *split_path_sel(tool_env_t *env, const char *in, selector_t *sel) {
    memset(sel, 0, sizeof(*sel));
    char *abs = tool_resolve_path(env, in);
    if (fs_exists(abs)) return abs;
    free(abs);
    char *work = xstrdup(in);
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
    bool hashline = !sel->raw && str_eq(env->edit_mode, "hashline");
    bool numbered = hashline || (!sel->raw && settings_bool("readLineNumbers", false));
    sb_t b = SB_INIT;
    json_t *details = json_obj();
    json_set_str(details, "resolvedPath", abs);
    int64_t seen_lo = -1, seen_hi = -1;
    bool whole = false;

    summary_lang_t slang = summary_lang(abs);
    bool want_summary = !sel->ranges.n && !sel->raw && slang != SUM_NONE && total >= 100 && len <= (2u << 20) &&
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
            sb_appendf(&b, "\n[%lld more lines in file. Use :%lld to continue]", (long long)(total - last),
                       (long long)(last + 1));
        else if (total == 0)
            sb_append(&b, "(empty file)");
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
                    sb_appendf(&notes, "Line %lld is beyond end of file (%lld lines total). Use :%lld to read the end.",
                               (long long)r.start, (long long)total, (long long)MAX(1, total - 20));
                } else {
                    sb_appendf(&notes, "[Range %lld is beyond end of file (%lld lines total); skipped]\n", (long long)r.start,
                               (long long)total);
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
            sb_appendf(&b, "\n[%lld more lines in file. Use :%lld to continue]", (long long)(total - last_shown),
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

static int read_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out);

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
    size_t len;
    bool trunc = false;
    char *data = fs_read_limit(abs, 64u << 20, &len, &trunc);
    if (!data) {
        tool_result_error(out, "Cannot read %s: %s", display, strerror(errno));
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
    json_t *s = schema_obj();
    schema_prop(s, "path", "string", "Path or URL to read, with optional :selector suffix", true);
    agent_tool_t *t = agent_tool_new("read", "Read", desc, s, read_execute);
    free(desc);
    t->ud = env;
    return t;
}
