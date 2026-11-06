/* write + bash tools */
#include "tools.h"
#include "jobs.h"
#include "xdev.h"
#include "services.h"
#include "vurl.h"
#include "iurl.h"
#include "coding/agents.h"
#include "prompt/template.h"

#include "config/settings.h"
#include "config/usage.h"
#include "core/fs.h"
#include "core/proc.h"
#include "shell.h"
#include "artifacts.h"
#include "outsink.h"
#include "patch.h"
#include "hashline.h"
#include "fuzzy.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------- write ---------- */

/* Node's `<CODE>: <description>, <syscall> '<path>'` error text for a failed fs call (libuv descriptions) */
static char *node_fs_error(int err, const char *syscall, const char *path) {
    static const struct {
        int no;
        const char *code, *desc;
    } T[] = {{ENOENT, "ENOENT", "no such file or directory"},
             {EACCES, "EACCES", "permission denied"},
             {EISDIR, "EISDIR", "illegal operation on a directory"},
             {ENOTDIR, "ENOTDIR", "not a directory"},
             {EROFS, "EROFS", "read-only file system"},
             {ENOSPC, "ENOSPC", "no space left on device"},
             {EEXIST, "EEXIST", "file already exists"},
             {EPERM, "EPERM", "operation not permitted"},
             {ENAMETOOLONG, "ENAMETOOLONG", "name too long"},
             {ELOOP, "ELOOP", "too many symbolic links encountered"},
             {EMFILE, "EMFILE", "too many open files"},
             {EBUSY, "EBUSY", "resource busy or locked"},
             {ETXTBSY, "ETXTBSY", "text file is busy"},
             {EINVAL, "EINVAL", "invalid argument"},
             {EIO, "EIO", "i/o error"},
             {EDQUOT, "EDQUOT", "disk quota exceeded"},
             {EFBIG, "EFBIG", "file too large"}};
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (T[i].no == err) return xasprintf("%s: %s, %s '%s'", T[i].code, T[i].desc, syscall, path);
    return xasprintf("%s, %s '%s'", strerror(err), syscall, path);
}

/* formatPathRelativeToCwd: cwd-relative inside cwd ("." for cwd itself), else absolute */
static char *rel_to_cwd(const char *abs, const char *cwd) {
    size_t cl = strlen(cwd);
    if (!strcmp(abs, cwd)) return xstrdup(".");
    if (cl == 1 && cwd[0] == '/') return xstrdup(abs[0] == '/' && abs[1] ? abs + 1 : abs);
    if (!strncmp(abs, cwd, cl) && abs[cl] == '/') return xstrdup(abs + cl + 1);
    return xstrdup(abs);
}

/* one chunk of a read line selector: L?N, L?N-[L?M], L?N..[L?M], L?N+K, or a -N tail */
static bool sel_chunk(const char *s, size_t n) {
    size_t i = 0;
    if (n >= 2 && s[0] == '-') {
        for (i = 1; i < n; i++)
            if (!isdigit((unsigned char)s[i])) return false;
        return true;
    }
    if (i < n && (s[i] == 'L' || s[i] == 'l')) i++;
    size_t d = i;
    while (i < n && isdigit((unsigned char)s[i])) i++;
    if (i == d) return false;
    if (i == n) return true;
    if (s[i] == '+') {
        i++;
        size_t k = i;
        while (i < n && isdigit((unsigned char)s[i])) i++;
        return i == n && i > k;
    }
    if (s[i] == '-') i++;
    else if (i + 1 < n && s[i] == '.' && s[i + 1] == '.') i += 2;
    else return false;
    if (i < n && (s[i] == 'L' || s[i] == 'l')) i++;
    while (i < n && isdigit((unsigned char)s[i])) i++;
    return i == n && (isdigit((unsigned char)s[n - 1]) || s[n - 1] == '.' || s[n - 1] == '-');
}

static bool sel_list(const char *s) {
    if (!*s) return false;
    for (const char *p = s;;) {
        const char *c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (!sel_chunk(p, n)) return false;
        if (!c) return true;
        p = c + 1;
    }
}

/* splitPathAndSel(...).sel for a filesystem path: the trailing read selector, or NULL */
static char *read_selector_of(const char *path) {
    const char *colon = strrchr(path, ':');
    if (!colon || colon == path) return NULL;
    const char *cand = colon + 1;
    bool word = !strcasecmp(cand, "raw") || !strcasecmp(cand, "conflicts") || !strcasecmp(cand, "img");
    if (!word && !sel_list(cand)) return NULL;
    if (!strcasecmp(cand, "raw")) {
        /* `path:1-50:raw` */
        const char *inner = colon;
        while (inner > path && inner[-1] != ':') inner--;
        if (inner > path + 1) {
            char *ic = xstrndup(inner, (size_t)(colon - inner));
            bool lst = sel_list(ic);
            if (lst) {
                char *r = xasprintf("%s:%s", ic, cand);
                free(ic);
                return r;
            }
            free(ic);
        }
    } else if (!word) {
        /* `path:raw:1-50` */
        const char *inner = colon;
        while (inner > path && inner[-1] != ':') inner--;
        if (inner > path + 1 && (size_t)(colon - inner) == 3 && !strncasecmp(inner, "raw", 3))
            return xasprintf("%.3s:%s", inner, cand);
    }
    return xstrdup(cand);
}

/* unwrapHashlineHeaderPath: `[PATH]` / `[PATH#XXXX]` -> PATH */
static char *unwrap_header(const char *p) {
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1])) n--;
    if (n < 2 || p[0] != '[' || p[n - 1] != ']') return xstrdup(p);
    char *inner = xstrndup(p + 1, n - 2);
    size_t il = strlen(inner);
    bool tagged = il >= 5 && inner[il - 5] == '#';
    for (size_t i = il >= 4 ? il - 4 : 0; tagged && i < il; i++) tagged = isxdigit((unsigned char)inner[i]) != 0;
    if (tagged) inner[il - 5] = 0;
    if (!*inner || (!tagged && strchr(inner, '#'))) {
        free(inner);
        return xstrdup(p);
    }
    return inner;
}

static bool one_edit_away(const char *typo, const char *word) {
    size_t a = strlen(typo), b = strlen(word);
    if (!strcmp(typo, word) || (a > b ? a - b : b - a) > 1) return false;
    size_t c = 0;
    while (c < a && c < b && typo[c] == word[c]) c++;
    const char *x = typo + c, *y = word + c;
    size_t xl = a - c, yl = b - c;
    if (xl && yl && !strcmp(x + 1, y + 1)) return true;
    if (xl && !strcmp(x + 1, y)) return true;
    if (yl && !strcmp(x, y + 1)) return true;
    return xl >= 2 && yl >= 2 && x[0] == y[1] && x[1] == y[0] && !strcmp(x + 2, y + 2);
}

/* assertWriteTargetAddressable: URI-like targets no router scheme handles */
static char *write_target_error(const char *target) {
    static const char *const KNOWN[] = {"local", "xd",  "ssh",   "proc",     "agent", "history", "artifact",
                                        "memory", "skill", "rule", "omp", "omc", "mcp", "pr",
                                        "issue", "conflict", "jobs", "file", "out", "cfg"};
    char *t = str_trim(target);
    char *msg = NULL;
    size_t sl = 0;
    if (isalpha((unsigned char)t[0])) {
        sl = 1;
        while (isalnum((unsigned char)t[sl]) || t[sl] == '+' || t[sl] == '.' || t[sl] == '-') sl++;
    }
    char *scheme = sl ? xstrndup(t, sl) : NULL;
    for (char *c = scheme; c && *c; c++) *c = (char)tolower((unsigned char)*c);
    bool known = false;
    for (size_t i = 0; scheme && i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (!strcmp(scheme, KNOWN[i])) known = true;
    if (scheme && t[sl] == ':' && t[sl + 1] == '/' && t[sl + 2] == '/' && known) goto done;
    if (scheme && t[sl] == '/' && !strcmp(scheme, "xd")) {
        const char *rest = t + sl;
        while (*rest == '/') rest++;
        msg = xasprintf("Unknown URI-like write target '%s'. Did you mean 'xd://%s'? Prefix the path with './' to write it "
                        "as a filesystem path.",
                        t, rest);
        goto done;
    }
    if (scheme && t[sl] == ':' && t[sl + 1] == '/') {
        const char *rest = t + sl + 2;
        if (*rest == '/') rest++;
        char *sugg = known || one_edit_away(scheme, "xd") ? xasprintf(" Did you mean '%s://%s'?", known ? scheme : "xd", rest)
                                                          : xstrdup(" Tool devices use 'xd://<tool>'.");
        msg = xasprintf("Unknown URI-like write target '%s'.%s Prefix the path with './' to write it as a filesystem path.",
                        t, sugg);
        free(sugg);
    }
done:
    free(scheme);
    free(t);
    return msg;
}

/* restore the replaced file's BOM / CRLF convention (write.ts applyTextFileConvention) */
static char *apply_text_convention(const char *abs, const char *content, bool *crlf_applied, bool *bom_applied) {
    *crlf_applied = *bom_applied = false;
    int fd = open(abs, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    bool binary = n > 0 && memchr(buf, 0, (size_t)n) != NULL;
    bool bom = n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF;
    bool crlf = false;
    char prev = 0;
    while (n > 0 && !binary) {
        char *lf = memchr(buf, '\n', (size_t)n);
        if (lf) {
            crlf = (lf > buf ? lf[-1] : prev) == '\r';
            break;
        }
        prev = buf[n - 1];
        n = read(fd, buf, sizeof buf);
    }
    close(fd);
    if (binary) return NULL;
    sb_t b = SB_INIT;
    bool has_cr = strchr(content, '\r') != NULL;
    if (bom && *content && !str_starts(content, "\xEF\xBB\xBF")) {
        sb_append(&b, "\xEF\xBB\xBF");
        *bom_applied = true;
    }
    if (crlf && strchr(content, '\n') && !has_cr) {
        for (const char *p = content; *p; p++) {
            if (*p == '\n') sb_append(&b, "\r\n");
            else sb_putc(&b, *p);
        }
        *crlf_applied = true;
    } else {
        sb_append(&b, content);
    }
    if (!*crlf_applied && !*bom_applied) {
        sb_free(&b);
        return NULL;
    }
    return sb_detach(&b);
}

/* ---- write.ts read-projection guard ---- */

/* the last non-blank line of `content`, bounded in the raw text by CR or LF (owned), NULL when blank */
static char *last_content_line(const char *content) {
    size_t end = strlen(content), n = end;
    while (end && isspace((unsigned char)content[end - 1])) end--;
    if (!end) return NULL;
    size_t start = end;
    while (start && content[start - 1] != '\n' && content[start - 1] != '\r') start--;
    while (end < n && content[end] != '\n' && content[end] != '\r') end++;
    return xstrndup(content + start, end - start);
}

/* isReadProjectionNotice: a `read` truncation notice, or the column-cap notice of a page whose long lines are shown cut */
static bool ends_with_read_projection_notice(const char *content) {
    char *line = last_content_line(content);
    bool r = line && (hl_is_read_truncation_notice(line) || out_is_column_truncation_notice(line));
    free(line);
    return r;
}

/* JS string length (UTF-16 units) of `n` bytes */
static int64_t js_length(const char *s, size_t n) {
    int64_t units = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        units += cp >= 0x10000 ? 2 : 1;
        i += (size_t)k;
    }
    return units;
}

/* normalizeToLF(text).length: each CRLF collapses to one LF */
static int64_t lf_normalized_length(const char *s) {
    int64_t len = js_length(s, strlen(s));
    for (const char *p = s; (p = strstr(p, "\r\n")); p += 2) len--;
    return len;
}

/* readProjectionPayloadLength: length of the shown payload, everything up to but excluding the trailing notice and the
 * blank separator before it. A single line truncated just past the budget renders longer than its source yet covers
 * strictly less of it: the marker, not the rendered length, proves the page incomplete. */
static int64_t read_projection_payload_length(const char *content) {
    char *text = fz_normalize_to_lf(content);
    size_t end = strlen(text), payload_end = 0;
    bool found = false;
    while (end > 0) {
        size_t start = end;
        while (start && text[start - 1] != '\n') start--;
        char *line = xstrndup(text + start, end - start);
        char *trimmed = str_trim(line);
        bool blank = !*trimmed;
        bool notice = !blank && (hl_is_read_truncation_notice(line) || out_is_column_truncation_notice(line));
        free(trimmed);
        free(line);
        if (!blank) {
            found = notice;
            payload_end = start;
            break;
        }
        if (!start) break;
        end = start - 1;
    }
    int64_t r = -1;
    if (found) {
        while (payload_end > 0) { /* back over the blank lines separating the payload from the notice */
            size_t prev = payload_end > 1 ? payload_end - 1 : 0;
            while (prev && text[prev - 1] != '\n') prev--;
            char *line = xstrndup(text + prev, payload_end - 1 - prev);
            char *trimmed = str_trim(line);
            bool blank = !*trimmed;
            free(trimmed);
            free(line);
            if (!blank) break;
            payload_end = prev;
        }
        r = payload_end ? js_length(text, payload_end - 1) : 0;
    }
    free(text);
    return r;
}

/* assertNotShorterReadProjection: owned refusal message or NULL. `write_content` is the text after display prefixes
 * were stripped. */
static char *read_projection_refusal(const char *display, const char *raw_content, const char *current, const char *write_content) {
    int64_t raw_payload = read_projection_payload_length(raw_content);
    if (raw_payload < 0 || !current) return NULL;
    /* A column-cap notice proves the page lossy at any length: a cut line ends in `…` in place of its tail, and a line one
     * column past the cap renders exactly as long as its source. So does a truncation notice carrying the column clause,
     * whose line-number prefixes pad the payload. */
    char *last = last_content_line(raw_content);
    bool column_cut = last && out_has_column_truncation_clause(last);
    free(last);
    if (!column_cut) {
        int64_t payload = !strcmp(write_content, raw_content) ? raw_payload : lf_normalized_length(write_content);
        if (payload >= lf_normalized_length(current)) return NULL;
    }
    return column_cut
               ? xasprintf("Refusing to overwrite '%s' with an incomplete read projection: the content ends with an omp read column-cap notice, so its cut "
                           "lines would replace the full ones. Re-read the cut lines in raw mode and write the complete file, or use edit for a partial change.",
                           display)
               : xasprintf("Refusing to overwrite '%s' with an incomplete read projection: the content ends with an omp read truncation notice and covers "
                           "less than the current source, so it would discard unseen content. Re-read the omitted ranges and write the complete file, or use "
                           "edit for a partial change.",
                           display);
}

static int write_local(tool_env_t *env, tool_ctx_t *ctx, const char *raw_path, const char *raw_content, tool_result_t *out) {
    char *path = unwrap_header(raw_path);
    if (!raw_content) {
        tool_result_error(out, "content is required for %s.", path);
        free(path);
        return 0;
    }
    bool stripped = false;
    char *clean = str_eq(env->edit_mode, "hashline") ? hl_strip_write_content(raw_content, &stripped) : xstrdup(raw_content);
    char *abs = NULL, *display = NULL, *final = NULL;
    char *terr = write_target_error(path);
    if (terr) {
        tool_result_error(out, "%s", terr);
        free(terr);
        goto done;
    }
    abs = tool_resolve_path(env, path);
    /* assertNotReadSelectorMisfire */
    if (strchr(path, ';') && !fs_exists(abs)) {
        strvec_t segs = str_split(path, ";");
        bool all = segs.n >= 2;
        for (size_t i = 0; all && i < segs.n; i++) {
            char *s = str_trim(segs.v[i]);
            char *sel = *s ? read_selector_of(s) : NULL;
            all = sel != NULL;
            free(sel);
            free(s);
        }
        size_t nseg = segs.n;
        strvec_free(&segs);
        if (all) {
            tool_result_error(out,
                              "write target '%s' is a semicolon-joined list of %zu read-tool selectors, not a filesystem "
                              "path — refusing to create it. write creates a single file; issue one read() per path to "
                              "read these ranges (e.g. read({ path: \"<one path>:<range>\" })).",
                              path, nseg);
            goto done;
        }
    }
    if (!*raw_content) {
        char *sel = read_selector_of(path);
        if (sel && !fs_exists(abs)) {
            tool_result_error(out,
                              "write target '%s' ends with a read-tool selector ':%s' and no such file exists — refusing "
                              "to create a literal file by that name. If you meant to read it, use read({ path: \"%s\" }). "
                              "If you truly intend to create this file, pass its contents in `content` (a non-empty write "
                              "is never blocked).",
                              path, sel, path);
            free(sel);
            goto done;
        }
        free(sel);
    }
    display = str_starts(path, "local://") ? xstrdup(path) : rel_to_cwd(abs, env->cwd);
    struct stat st;
    bool existed = stat(abs, &st) == 0;
    if (existed && !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        const char *kind = S_ISCHR(st.st_mode)    ? "character device"
                           : S_ISBLK(st.st_mode)  ? "block device"
                           : S_ISFIFO(st.st_mode) ? "FIFO"
                           : S_ISSOCK(st.st_mode) ? "socket"
                                                  : "special file";
        tool_result_error(out, "Cannot write '%s': it is a %s, not a regular file or directory.", path, kind);
        goto done;
    }
    /* omp assertEditableFile: an existing generated file is not overwritten */
    if (existed && settings_bool("edit.blockAutoGenerated", true)) {
        char *guard = pe_auto_generated_file_message(abs, path);
        if (guard) {
            tool_result_error(out, "%s", guard);
            free(guard);
            goto done;
        }
    }
    bool crlf_applied = false, bom_applied = false;
    /* an ACP editor that advertises fs.writeTextFile owns editor-visible files (routeWriteThroughBridge): the write
     * goes as given (its buffer keeps its own line endings); OMP-owned artifacts (internal URLs, local://) stay local */
    bool bridged = env->bridge && env->bridge->write_text && !strstr(path, "://") &&
                   !(env->local_dir && str_starts(abs, env->local_dir));
    if (ends_with_read_projection_notice(raw_content)) { /* assertNotTruncatedFileReadProjection */
        char *current = NULL;
        if (bridged && env->bridge->read_text) { /* readCurrentWriteSource: the editor's buffer, else the disk */
            char *berr = NULL;
            current = env->bridge->read_text(env->bridge, abs, &berr);
            free(berr);
        }
        if (!current && existed && S_ISREG(st.st_mode)) current = fs_read(abs, NULL);
        char *refusal = read_projection_refusal(display, raw_content, current, clean);
        free(current);
        if (refusal) {
            tool_result_error(out, "%s", refusal);
            free(refusal);
            goto done;
        }
    }
    char *conv = !bridged && existed && S_ISREG(st.st_mode) ? apply_text_convention(abs, clean, &crlf_applied, &bom_applied) : NULL;
    final = conv ? conv : xstrdup(clean);
    size_t n = strlen(final);
    if (ctx && ctx->update) { /* emitWriteProgress */
        tool_result_t pr = {0};
        tool_result_textf(&pr, "Writing %zu bytes to %s...", n, display);
        pr.details = json_obj();
        json_set_str(pr.details, "resolvedPath", abs);
        ctx->update(ctx, &pr);
        tool_result_free(&pr);
    }
    if (bridged) {
        char *berr = NULL;
        if (env->bridge->write_text(env->bridge, abs, final, &berr) != 0) {
            tool_result_error(out, "%s", berr ? berr : "write failed");
            free(berr);
            goto done;
        }
        /* the client may have transformed the text (format on save): what is on disk now is what callers see */
        char *back = fs_read(abs, NULL);
        if (back) {
            free(final);
            final = back;
        }
        goto written;
    }
    {
        char *dir = path_dirname(abs);
        fs_mkdir_p(dir, 0755);
        free(dir);
    }
    int fd = open(abs, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        char *m = node_fs_error(errno, "open", abs);
        tool_result_error(out, "%s", m);
        free(m);
        goto done;
    }
    size_t off = 0;
    int werr = 0;
    while (off < n) {
        ssize_t w = write(fd, final + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            werr = errno;
            break;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0 && !werr) werr = errno;
    if (werr) {
        char *m = node_fs_error(werr, "write", abs);
        tool_result_error(out, "%s", m);
        free(m);
        goto done;
    }
written:;
    /* shebang: chmod a+x */
    bool made_exec = false;
    if (str_starts(final, "#!") && !str_starts(final, "#![")) {
        struct stat s2;
        if (stat(abs, &s2) == 0) {
            mode_t cur = s2.st_mode & 07777, nm = cur | 0111;
            if (nm != cur && chmod(abs, nm) == 0 && stat(abs, &s2) == 0) made_exec = (s2.st_mode & 0111) == 0111;
        }
    }
    sb_t b = SB_INIT;
    if (str_eq(env->edit_mode, "hashline")) {
        char *lf = fz_normalize_to_lf(str_starts(final, "\xEF\xBB\xBF") ? final + 3 : final);
        const char *tag = edit_store_record(&env->store, abs, lf, 0, 0);
        sb_appendf(&b, "[%s#%s]\n", display, tag);
        free(lf);
    }
    sb_appendf(&b, "Successfully wrote %zu bytes to %s", n, display);
    if (stripped) sb_append(&b, "\nNote: auto-stripped hashline display prefixes from content before writing.");
    if (crlf_applied || bom_applied)
        sb_appendf(&b, "\nNote: kept the replaced file's %s%s%s.", crlf_applied ? "CRLF line endings" : "",
                   crlf_applied && bom_applied ? " and " : "", bom_applied ? "UTF-8 BOM" : "");
    if (made_exec) sb_append(&b, "\n[Notice: Made executable via chmod +x]");
    if (env->after_write) {
        char *diag = env->after_write(env, abs);
        if (diag) {
            sb_appendf(&b, "\n\n%s", diag); /* omp: diagnostics notice at the message boundary */
            free(diag);
        }
    }
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    out->details = json_obj();
    json_set_str(out->details, "resolvedPath", abs);
    if (made_exec) json_set_bool(out->details, "madeExecutable", true);
done:
    free(final);
    free(clean);
    free(abs);
    free(display);
    free(path);
    return 0;
}

static int write_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *path = json_get_str(args, "path");
    const char *raw_content = json_get_str(args, "content");
    const char *content = raw_content;
    if (!path) {
        tool_result_error(out, "path is required");
        return 0;
    }
    if (vurl_is(path)) return vurl_write(env, path, content, out);
    if (iurl_write(env, path, content, out)) return 0;
    if (!content) content = "";
    if (!strcmp(path, "xd://resolve") || !strcmp(path, "xd://reject")) {
        bool err = false;
        char *r = astedit_resolve(env, !strcmp(path, "xd://resolve"), content, &err);
        tool_result_text(out, r);
        out->is_error = err;
        free(r);
        return 0;
    }
    if (!strcmp(path, "xd://propose")) { /* resolve.ts dispatchResolutionDevice */
        if (!env->propose_plan) {
            tool_result_error(out, "No plan is awaiting approval \xe2\x80\x94 xd://propose only accepts a plan title while plan mode is active.");
            return 0;
        }
        char *title = str_trim(content);
        bool err = false;
        json_t *inner = NULL;
        char *r = env->propose_plan(env, title, &err, &inner);
        tool_result_text(out, r ? r : "");
        out->is_error = err;
        if (!err) { /* XdevDispatch {tool, mode, args, inner} on the write details */
            json_t *x = json_obj(), *xa = json_obj();
            json_set_str(x, "tool", "propose");
            json_set_str(x, "mode", "execute");
            json_set_str(xa, "title", title);
            json_set(x, "args", xa);
            if (inner) json_set(x, "inner", inner);
            inner = NULL;
            out->details = json_obj();
            json_set(out->details, "xdev", x);
        }
        json_free(inner);
        free(r);
        free(title);
        return 0;
    }
    if (xdev_write(ctx, path, content, out)) return 0;
    if (!str_starts(path, "ssh://")) return write_local(env, ctx, path, raw_content, out);
    char *err = NULL;
    size_t n = strlen(content);
    if (ssh_store(env, path, content, n, &err) != 0) {
        tool_result_error(out, "%s", err);
    } else {
        size_t lines = n ? str_count(content, "\n") + (content[n - 1] != '\n') : 0;
        tool_result_textf(out, "Successfully wrote %zu bytes to %s", n, path); /* omp handler-written URL result */
        out->details = json_obj();
        json_set_str(out->details, "path", path);
        json_set_int(out->details, "lines", (int64_t)lines);
    }
    free(err);
    return 0;
}

agent_tool_t *tool_write_new(tool_env_t *env) {
    char *desc = tool_prompt("write", NULL);
    json_t *s = schema_lit("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":"
                           "\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("write", "Write", desc, s, write_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- bash ---------- */

typedef struct {
    tool_ctx_t *ctx;
    out_sink_t *sink;
    size_t pushed; /* raw bytes fed to the sink */
    int64_t last_emit;
} bash_stream_t;

static void bash_on_output(void *ud, int stream, const char *data, size_t len) {
    bash_stream_t *s = ud;
    UNUSED(stream);
    out_sink_push(s->sink, data, len);
    s->pushed += len;
    int64_t now = mono_ms();
    if (s->ctx && s->ctx->update && now - s->last_emit >= 50) {
        s->last_emit = now;
        char *pv = out_sink_preview(s->sink);
        tool_result_t partial = {0};
        tool_result_text(&partial, pv);
        s->ctx->update(s->ctx, &partial);
        tool_result_free(&partial);
        free(pv);
    }
}

/* leading `cd <dir> && rest` → (dir, rest) when cwd omitted */
static bool split_leading_cd(const char *cmd, char **dir, char **rest) {
    const char *p = cmd;
    while (isspace((unsigned char)*p)) p++;
    if (strncmp(p, "cd ", 3) != 0) return false;
    p += 3;
    while (*p == ' ') p++;
    const char *d = p;
    char q = 0;
    if (*p == '"' || *p == '\'') {
        q = *p++;
        d = p;
        while (*p && *p != q) p++;
        if (!*p) return false;
    } else {
        while (*p && !isspace((unsigned char)*p) && *p != '&' && *p != ';') p++;
    }
    size_t dl = (size_t)(p - d);
    if (q) p++;
    while (*p == ' ') p++;
    if (strncmp(p, "&&", 2) != 0) return false;
    p += 2;
    while (*p == ' ') p++;
    if (!*p || dl == 0) return false;
    *dir = xstrndup(d, dl);
    *rest = xstrdup(p);
    return true;
}

#define BASH_TIMEOUT_DESC "timeout in seconds; 0 disables the command deadline; nonzero values are clamped to 1-3600"

static char *background_notice(const char *jid, double tsec) {
    if (tsec > 0)
        return xasprintf("Backgrounded as job %s (killed once it has run %.0fs in total; `timeout: 0` disables the deadline); its "
                         "output is injected into the conversation as a follow-up the moment it finishes. Do NOT poll for it (no "
                         "`sleep`, `ps`, `pgrep`, `top`, `pidwait`, log tailing): every poll is a wasted turn. Do other work, or end "
                         "your reply and wait to be woken.",
                         jid, tsec);
    return xasprintf("Backgrounded as job %s (no deadline); its output is injected into the conversation as a follow-up the "
                     "moment it finishes. Do NOT poll for it (no `sleep`, `ps`, `pgrep`, `top`, `pidwait`, log tailing): every "
                     "poll is a wasted turn. Do other work, or end your reply and wait to be woken.",
                     jid);
}

static void background_result(tool_result_t *out, const char *jid, const char *preview, const strvec_t *notices, double tsec) {
    sb_t b = SB_INIT;
    size_t pl = preview ? strlen(preview) : 0;
    while (pl && isspace((unsigned char)preview[pl - 1])) pl--;
    if (pl) {
        sb_appendn(&b, preview, pl);
        sb_append(&b, "\n\n");
    }
    for (size_t i = 0; notices && i < notices->n; i++) sb_appendf(&b, "%s\n", notices->v[i]);
    if (notices && notices->n) sb_putc(&b, '\n');
    char *n = background_notice(jid, tsec);
    sb_append(&b, n);
    free(n);
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    out->details = json_obj();
    json_t *as = json_obj();
    json_set_str(as, "state", "running");
    json_set_str(as, "jobId", jid);
    json_set_str(as, "type", "bash");
    json_set(out->details, "async", as);
    if (tsec > 0) json_set_int(out->details, "timeoutSeconds", (int64_t)tsec);
    else json_set_bool(out->details, "timeoutDisabled", true);
}

/* completed foreground command -> tool result (omp BashTool #buildCompletedResult / #throwIfUnfinished) */
static void bash_finish(tool_env_t *env, tool_result_t *out, proc_result_t *r, bash_stream_t *bs, double tsec, double req_tsec,
                        int64_t timeout_ms, const char *cwd, int64_t dur, const strvec_t *notices) {
    if (r->spawn_errno && !r->out_len && !bs->pushed) {
        tool_result_error(out, "Failed to spawn shell: %s", strerror(r->spawn_errno));
        return;
    }
    /* output the streaming callbacks did not deliver (e.g. a held-back tail when the shell exits) */
    if (r->out && r->out_len > bs->pushed && !r->truncated) out_sink_push(bs->sink, r->out + bs->pushed, r->out_len - bs->pushed);
    bool timed_out = r->timed_out, cancelled = r->cancelled && !r->timed_out;
    char *tnotice = timed_out ? xasprintf("Command timed out after %lld seconds", (long long)((timeout_ms + 500) / 1000)) : NULL;
    out_summary_t sum;
    out_sink_dump(bs->sink, timed_out ? tnotice : cancelled ? "Command cancelled" : NULL, &sum);
    free(tnotice);
    char *errn = sum.artifact_error ? xasprintf("\n\n[Full output was not saved completely (artifact %s failed)]", sum.artifact_error)
                                    : xstrdup("");
    if (cancelled) {
        /* omp throws: the local executor output already leads with `[Command cancelled]` */
        char *msg = str_starts(sum.output, "[Command cancelled]") ? xstrdup(sum.output)
                    : *sum.output                                 ? xasprintf("%s\n\n[Command aborted]", sum.output)
                                                                  : xstrdup("Command aborted");
        tool_result_textf(out, "%s%s", msg, errn);
        out->is_error = true;
        free(msg);
        free(errn);
        out_summary_free(&sum);
        return;
    }
    int exit_code = r->term_signal ? 128 + r->term_signal : r->exit_code;
    bool failed = !timed_out && exit_code != 0;
    sb_t b = SB_INIT;
    /* a failed exit gets its own notice; a clean one says so, or an empty result reads like a broken command */
    sb_append(&b, *sum.output ? sum.output : !timed_out && exit_code == 0 ? "(no output, exit code 0)" : "(no output)");
    bool wall_notice = dur >= 1000;
    if (wall_notice || (notices && notices->n)) {
        sb_append(&b, "\n");
        if (wall_notice) sb_appendf(&b, "\nWall time: %.2f seconds", (double)dur / 1000.0);
        for (size_t i = 0; notices && i < notices->n; i++)
            if (notices->v[i] && *notices->v[i]) sb_appendf(&b, "\n%s", notices->v[i]);
    }
    if (failed) sb_appendf(&b, "\n\nCommand exited with code %d", exit_code);
    if (timed_out) {
        char *m = tsec > 0 ? xasprintf("Command timed out after %.0f seconds", tsec) : xstrdup("Command timed out");
        char *lead = xasprintf("[%s]\n", m);
        /* the sink's own leading notice: exactly once in the text sent */
        if (strncmp(sum.output, lead, strlen(lead) - 1) || (sum.output[strlen(lead) - 1] && sum.output[strlen(lead) - 1] != '\n'))
            sb_appendf(&b, "\n\n[%s]", m);
        free(lead);
        free(m);
    }
    /* final-defense inline cap (spill threshold + notice slack); reuses the sink's artifact */
    size_t cap = tool_spill_threshold() + OUT_INLINE_CAP_SLACK_BYTES;
    char *aid = NULL;
    if (b.len > cap && !sum.artifact_error)
        aid = sum.artifact_id ? xstrdup(sum.artifact_id) : artifacts_save(env->artifacts, "bash-original", b.s, b.len);
    char *text = out_enforce_inline_cap(b.s ? b.s : "", b.len, cap, aid, NULL);
    free(aid);
    tool_result_text(out, text);
    free(text);
    sb_free(&b);
    out->is_error = timed_out || failed;
    out->details = json_obj();
    json_set_bool(out->details, "wallTimeNoticeEmitted", wall_notice);
    if (tsec > 0) json_set_int(out->details, "timeoutSeconds", (int64_t)tsec);
    else json_set_bool(out->details, "timeoutDisabled", true);
    if (req_tsec >= 0 && req_tsec != tsec) json_set_num(out->details, "requestedTimeoutSeconds", req_tsec);
    json_set_int(out->details, "wallTimeMs", dur);
    if (failed) json_set_int(out->details, "exitCode", exit_code);
    if (timed_out) json_set_bool(out->details, "timedOut", true);
    json_set_int(out->details, "durationMs", dur);
    json_set_str(out->details, "cwd", cwd);
    json_t *meta = NULL;
    out_meta_from_summary(&meta, &sum, "tail", 1, 0);
    if (meta) json_set(out->details, "meta", meta);
    free(errn);
    out_summary_free(&sum);
}

static const char *blank_null(const char *s) {
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    return *s ? s : NULL;
}

static void bash_service(tool_env_t *env, tool_ctx_t *ctx, const char *name, const char *cmd, const char *cwd, const json_t *args,
                         strvec_t *notices, tool_result_t *out) {
    const json_t *rj = json_get(args, "ready");
    service_ready_t ready = {0};
    bool has_ready = false;
    if (json_is_obj(rj)) {
        ready.log = blank_null(json_get_str(rj, "log"));
        ready.host = blank_null(json_get_str(rj, "host"));
        ready.port = (int)json_get_num(rj, "port", 0);
        ready.timeout = json_get_num(rj, "timeout", 0);
        has_ready = ready.log || ready.host || json_get(rj, "port") || json_get(rj, "timeout");
    }
    bool pty = json_get_bool(args, "pty", true);
    json_t *snap = NULL;
    bool timed_out = false;
    char *err = NULL;
    if (services_start(env->jobs, env->cwd, name, cmd, cwd, pty, has_ready ? &ready : NULL, ctx ? ctx->cancel : NULL, &snap,
                       &timed_out, &err) != 0) {
        tool_result_error(out, "%s", err ? err : "service start failed");
        free(err);
        return;
    }
    sb_t b = SB_INIT;
    sb_appendf(&b, "%s: %s", json_get_str(snap, "name"), json_get_str(snap, "state"));
    if (json_get(snap, "pid")) sb_appendf(&b, " pid=%lld", (long long)json_get_int(snap, "pid", 0));
    if (json_get(snap, "readyAt")) sb_append(&b, " ready");
    if (timed_out)
        sb_appendf(&b, "\nNOT ready — readiness timed out after %gs; process state: %s.", ready.timeout > 0 ? ready.timeout : 30,
                   json_get_str(snap, "state"));
    for (size_t i = 0; i < notices->n; i++) sb_appendf(&b, "\n%s", notices->v[i]);
    char *log = services_logs(env->cwd, name);
    if (*log) sb_appendf(&b, "\n%s", log);
    free(log);
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    const char *st = json_get_str(snap, "state");
    out->details = json_obj();
    json_t *sv = json_obj();
    json_set_str(sv, "name", name);
    json_set_str(sv, "state", st);
    json_set_bool(sv, "ready", json_get(snap, "readyAt") || str_eq(st, "running"));
    json_set_bool(sv, "timedOut", timed_out);
    if (json_get(snap, "pid")) json_set_int(sv, "pid", json_get_int(snap, "pid", 0));
    json_set(out->details, "service", sv);
    out->is_error = str_eq(st, "failed");
    json_free(snap);
}

static bool steer_queued(tool_ctx_t *ctx) {
    return ctx && ctx->agent && ctx->agent->steering && json_len(ctx->agent->steering) > 0;
}

static int bash_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    const char *command = json_get_str(args, "command");
    if (!command || !*command) {
        tool_result_error(out, "command is required");
        return 0;
    }
    bool launch = settings_bool("launch.enabled", true);
    bool async_on = settings_bool("async.enabled", true);
    const char *name = blank_null(json_get_str(args, "name"));
    bool async_req = json_get_bool(args, "async", false);
    strvec_t notices = {0};
    if (name) {
        if (!launch) {
            tool_result_error(out, "Service launch is disabled in this session.");
            return 0;
        }
        if (json_get(args, "timeout")) strvec_push(&notices, "Ignored timeout: service-only; readiness follows ready.timeout.");
        if (async_req) strvec_push(&notices, "Ignored async: service-only; a service start is asynchronous already.");
    } else {
        const json_t *rj = json_get(args, "ready");
        if (json_is_obj(rj) && (blank_null(json_get_str(rj, "log")) || json_get_num(rj, "port", 0) != 0))
            strvec_push(&notices, "Ignored ready: service-only, and no service name was given.");
        if (async_req && !async_on) {
            tool_result_error(out, "Async bash execution is disabled. Enable async.enabled to use async mode.");
            strvec_free(&notices);
            return 0;
        }
    }
    char *cmd = xstrdup(command);
    const char *cwd_arg = json_get_str(args, "cwd");
    char *cwd = NULL;
    char *full = NULL;
    if (cwd_arg && *cwd_arg) {
        cwd = tool_resolve_path(env, cwd_arg);
    } else {
        char *d, *rest;
        if (split_leading_cd(command, &d, &rest)) {
            char *cand = tool_resolve_path(env, d);
            if (fs_is_dir(cand)) {
                cwd = cand;
                free(cmd);
                cmd = rest;
            } else {
                free(cand);
                free(rest);
            }
            free(d);
        }
    }
    if (!cwd) cwd = xstrdup(env->cwd);
    if (!fs_is_dir(cwd)) {
        tool_result_error(out, "Working directory does not exist: %s", cwd);
        goto done;
    }
    const char *prefix = settings_str("shellCommandPrefix", "");
    full = (prefix && *prefix) ? xasprintf("%s\n%s", prefix, cmd) : xstrdup(cmd);
    if (name) {
        bash_service(env, ctx, name, full, cwd, args, &notices, out);
        goto done;
    }
    double tsec = json_get_num(args, "timeout", 300);
    double req_tsec = json_get(args, "timeout") ? tsec : -1;
    int64_t max_to = settings_int("tools.maxTimeout", 0);
    if (tsec > 0 && max_to > 0 && tsec > (double)max_to) tsec = (double)max_to;
    if (tsec > 0 && tsec < 1) tsec = 1;
    if (tsec > 3600) tsec = 3600;
    int64_t timeout_ms = tsec > 0 ? (int64_t)(tsec * 1000) : 0;
    bool pty = json_get_bool(args, "pty", false);
    if (async_req && env->jobs) {
        char *jid = jobs_start_bash(env->jobs, full, cwd, timeout_ms, NULL, ctx ? ctx->intent : NULL);
        background_result(out, jid, NULL, &notices, tsec);
        free(jid);
        goto done;
    }

    /* `cmd > out://`: replies reach the user only from a foreground run */
    strvec_t reply_files = {0};
    char *redirected = out_redirect_rewrite(env, full, &reply_files);
    if (redirected) {
        free(full);
        full = redirected;
    }
    /* omp allocates the call's artifact up front (ids advance per call); the sink writes it only on spill */
    char *art_id = NULL;
    char *art_path = artifacts_alloc(env->artifacts, "bash", &art_id);
    out_sink_opts_t so;
    out_sink_opts_from_settings(&so);
    so.artifact_path = art_path;
    so.artifact_id = art_id;
    bash_stream_t bs = {.ctx = ctx, .sink = out_sink_new(&so)};
    free(art_path);
    free(art_id);
    /* the client terminal (an ACP client advertising `terminal`) shows a live terminal card in the editor; it wins
     * over auto-backgrounding. PTY runs need the local terminal UI. */
    if (!pty && env->bridge && env->bridge->run_terminal) {
        strvec_t targs = {0};
        const char *nologin = env_get("PI_BASH_NO_LOGIN");
        if (!nologin || !*nologin) nologin = env_get("CLAUDE_BASH_NO_LOGIN");
        if (!nologin || !*nologin) strvec_push(&targs, "-l");
        strvec_push(&targs, "-c");
        const char *sprefix = env_get("PI_SHELL_PREFIX");
        if (!sprefix || !*sprefix) sprefix = env_get("CLAUDE_CODE_SHELL_PREFIX");
        strvec_push_own(&targs, sprefix && *sprefix ? xasprintf("%s %s", sprefix, full) : xstrdup(full));
        char *shell = NULL;
        const char *sp = settings_str("shellPath", NULL);
        const char *ushell = env_get("SHELL");
        if (sp && *sp) shell = xstrdup(sp);
        else if (ushell && (strstr(ushell, "bash") || strstr(ushell, "zsh")) && fs_is_exec(ushell)) shell = xstrdup(ushell);
        else shell = xstrdup(proc_shell_path());
        tool_terminal_req_t req = {.command = shell, .args = &targs, .cwd = cwd, .timeout_ms = timeout_ms, .output_limit = 51200};
        tool_terminal_res_t tr = {.exit_code = -1};
        char *terr = NULL;
        int64_t tt0 = mono_ms();
        int trc = env->bridge->run_terminal(env->bridge, ctx, &req, &tr, &terr);
        int64_t tdur = mono_ms() - tt0;
        free(shell);
        strvec_free(&targs);
        if (trc != 0) {
            tool_result_error(out, "%s", terr ? terr : "terminal failed");
            free(terr);
            free(tr.output);
            out_sink_free(bs.sink);
            out_redirect_discard(&reply_files);
            goto done;
        }
        proc_result_t r = {0};
        r.out = tr.output ? tr.output : xstrdup("");
        r.out_len = strlen(r.out);
        r.timed_out = tr.timed_out;
        r.cancelled = tr.aborted;
        r.exit_code = tr.exit_code >= 0 ? tr.exit_code : 0;
        if (tr.signaled && tr.exit_code < 0) r.term_signal = 9; /* null exit code with a signal: 137 */
        if (tr.truncated) strvec_push(&notices, "(output truncated)");
        bash_finish(env, out, &r, &bs, tsec, req_tsec, timeout_ms, cwd, tdur, &notices);
        out_redirect_collect(&reply_files, out);
        out_sink_free(bs.sink);
        proc_result_free(&r);
        goto done;
    }
    bool persistent = settings_bool("bash.persistent", true);
    if (!pty && persistent) {
        pthread_mutex_lock(&env->mu);
        if (!env->shell) env->shell = shell_new();
        pthread_mutex_unlock(&env->mu);
    }
    int64_t cap = MIN(100, settings_int("async.maxJobs", 100));
    bool auto_bg = !pty && env->jobs && async_on && settings_bool("bash.autoBackground.enabled", true) &&
                   (int64_t)jobs_running(env->jobs) < cap;
    int64_t t0 = mono_ms();
    proc_result_t r;
    if (auto_bg) {
        int64_t thr = settings_int("bash.autoBackground.thresholdMs", 60000);
        int64_t wait_ms = thr <= 0 ? 0 : timeout_ms ? MAX(0, MIN(thr, timeout_ms - 1000)) : thr;
        char *fid = jobs_start_bash_fg(env->jobs, full, cwd, timeout_ms, ctx ? ctx->intent : NULL, persistent ? env->shell : NULL, bash_on_output, &bs);
        bool fin = false, steered = false;
        int64_t deadline = mono_ms() + wait_ms;
        for (;;) {
            fin = jobs_fg_wait(env->jobs, fid, MIN(100, MAX(0, deadline - mono_ms())), ctx ? ctx->cancel : NULL);
            if (fin) break;
            if (ctx && ctx->cancel && *ctx->cancel) continue; /* the wait propagated the cancel; let it finish */
            if (mono_ms() >= deadline) break;
            if (steer_queued(ctx)) {
                steered = true;
                break;
            }
        }
        if (!fin) {
            char *jid = jobs_fg_detach(env->jobs, fid);
            if (jid) {
                if (steered) strvec_push(&notices, "Backgrounded early to handle an incoming message; the command keeps running.");
                char *pv = out_sink_preview(bs.sink);
                background_result(out, jid, pv, &notices, tsec);
                free(pv);
                free(jid);
                free(fid);
                out_sink_free(bs.sink);
                out_redirect_discard(&reply_files);
                goto done;
            }
            jobs_fg_wait(env->jobs, fid, 60000, NULL); /* finished right at the threshold */
        }
        char *notice = NULL;
        if (!jobs_fg_take(env->jobs, fid, &r, &notice)) {
            tool_result_error(out, "Failed to run command");
            free(fid);
            out_sink_free(bs.sink);
            out_redirect_discard(&reply_files);
            goto done;
        }
        if (notice) strvec_push_own(&notices, notice);
        free(fid);
    } else {
        proc_opts_t o = {.cwd = cwd,
                         .env_extra = tool_bash_env(),
                         .timeout_ms = timeout_ms,
                         .max_output = 64u << 20,
                         .merge_stderr = true,
                         .new_session = true,
                         .use_pty = pty,
                         .cancel = ctx->cancel,
                         .on_output = bash_on_output,
                         .ud = &bs};
        int rc = (!pty && persistent) ? shell_run(env->shell, full, cwd, &o, &r) : shell_exec_once(full, &o, &r);
        if (rc != 0 && !r.spawn_errno) r.spawn_errno = errno ? errno : EIO;
    }
    int64_t dur = mono_ms() - t0;
    bash_finish(env, out, &r, &bs, tsec, req_tsec, timeout_ms, cwd, dur, &notices);
    out_redirect_collect(&reply_files, out);
    out_sink_free(bs.sink);
    proc_result_free(&r);
done:
    strvec_free(&notices);
    free(full);
    free(cwd);
    free(cmd);
    return 0;
}

static bool python_available(void) {
    const char *cfg = settings_str("python.interpreter", NULL);
    if (cfg && *cfg) return true;
    char *p = which("python3");
    if (!p) p = which("python");
    bool ok = p != NULL;
    free(p);
    return ok;
}

char *bash_tool_description(bool has_eval) {
    json_t *ctx = json_obj();
    json_set_bool(ctx, "hasEval", has_eval);
    json_set_bool(ctx, "asyncEnabled", settings_bool("async.enabled", true));
    int64_t max_to = settings_int("tools.maxTimeout", 0);
    json_set_int(ctx, "defaultTimeoutSec", max_to > 0 && max_to < 300 ? max_to : 300);
    json_set_bool(ctx, "autoBackgroundEnabled", settings_bool("bash.autoBackground.enabled", true));
    int64_t thr = settings_int("bash.autoBackground.thresholdMs", 60000);
    char *secs = thr % 1000 ? xasprintf("%g", (double)thr / 1000.0) : xasprintf("%lld", (long long)(thr / 1000));
    json_set_str(ctx, "autoBackgroundSeconds", secs);
    free(secs);
    json_set_bool(ctx, "hasLaunch", settings_bool("launch.enabled", true));
    char *desc = tool_prompt("bash", ctx);
    json_free(ctx);
    return desc;
}

json_t *bash_tool_schema(void) {
    bool async_on = settings_bool("async.enabled", true), launch = settings_bool("launch.enabled", true);
    sb_t b = SB_INIT;
    sb_append(&b, "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
                  "\"timeout\":{\"type\":\"number\",\"description\":\"" BASH_TIMEOUT_DESC "\"},"
                  "\"cwd\":{\"type\":\"string\"},\"pty\":{\"type\":\"boolean\"}");
    if (async_on) sb_append(&b, ",\"async\":{\"type\":\"boolean\"}");
    if (launch)
        sb_append(&b, ",\"name\":{\"type\":\"string\",\"maxLength\":48},\"ready\":{\"type\":\"object\",\"properties\":{"
                      "\"log\":{\"type\":\"string\"},\"port\":{\"type\":\"number\"},\"host\":{\"type\":\"string\"},"
                      "\"timeout\":{\"type\":\"number\"}},\"additionalProperties\":false}");
    sb_append(&b, "},\"required\":[\"command\"],\"additionalProperties\":false}");
    json_t *s = json_parse_str(sb_cstr(&b));
    sb_free(&b);
    return s;
}

agent_tool_t *tool_bash_new(tool_env_t *env) {
    char *desc = bash_tool_description(python_available());
    agent_tool_t *t = agent_tool_new("bash", "Bash", desc, bash_tool_schema(), bash_execute);
    t->exclusive = true;
    free(desc);
    t->ud = env;
    return t;
}

/* ---------- wait (background jobs, services, peer replies) ---------- */

char *jobs_render_results(const json_t *jobs) {
    json_t *c = json_obj();
    json_set(c, "jobs", json_clone(jobs));
    json_set_bool(c, "multiple", json_len(jobs) > 1);
    char *s = prompt_render("coding-agent/prompts/tools/async-result.md", c);
    json_free(c);
    return s;
}

/* Windows a blocking wait gives its first wake before it returns with nothing new; each back-to-back idle wait climbs one
 * rung, so the agent gets its turn back early while work warms up, and a wait loop backs off. */
static const int64_t WAIT_LADDER_MS[] = {5000, 10000, 30000, 60000, 300000};
#define WAIT_LADDER_RUNGS 5
#define WAIT_LADDER_RESET_MS 60000 /* a gap this long since the last idle wait means the agent worked in between */
#define WAIT_NOTICE_RUNG 1         /* one idle wait is patience, a second back-to-back is a loop */
#define WAIT_SETTLE_WINDOW_MS 1500 /* after one watched job settles, near-simultaneous completions share the result */
#define WAIT_PROGRESS_INTERVAL_MS 500

/* buildJobResult("wait"): the settled jobs, each led by what it was for, then what still runs. `recovered`: the job had
 * settled before this wait started (its delivery was still pending). */
static char *job_result_text(jobs_t *jobs, const json_t *done, bool recovered) {
    sb_t b = SB_INIT;
    const json_t *x;
    if (json_len(done)) sb_appendf(&b, "## Completed (%zu)\n\n", json_len(done));
    json_arr_foreach(done, i, x) {
        const char *id = json_get_str(x, "jobId"), *status = json_get_str(x, "status");
        const char *intent = json_get_str(x, "intent"), *agent = json_get_str(x, "agentId"), *type = json_get_str(x, "type");
        sb_appendf(&b, "### %s [%s] \xE2\x80\x94 %s\n", id, type ? type : "bash", status ? status : "completed");
        if (intent) sb_appendf(&b, "Intent: %s\n", intent);
        sb_appendf(&b, "Label: %s\n", json_get_str(x, "label") ? json_get_str(x, "label") : "");
        if (str_eq(type, "task") && agent && !str_eq(agent, id)) { /* taskAgentHandles */
            if (str_eq(status, "completed")) sb_appendf(&b, "Agent: agent://%s, history://%s\n", agent, agent);
            else sb_appendf(&b, "Agent: history://%s\n", agent);
        }
        if (!str_eq(status, "cancelled"))
            sb_append(&b, recovered ? "Delivery: not auto-delivered; recovered by this snapshot.\n" : "Delivery: delivered directly to wait.\n");
        const char *res = json_get_str(x, "result");
        if (res && *res) {
            if (json_get_bool(x, "failed", false)) sb_appendf(&b, "Error: %s\n", res);
            else sb_appendf(&b, "```\n%s\n```\n", res);
        }
        sb_putc(&b, '\n');
    }
    json_t *snap = jobs ? jobs_snapshot(jobs) : json_arr();
    size_t running = 0;
    json_arr_foreach(snap, i, x) if (str_eq(json_get_str(x, "status"), "running") && !json_get_bool(x, "quiet", false)) running++;
    if (running) {
        sb_appendf(&b, "## Still Running (%zu)\n\n", running);
        json_arr_foreach(snap, i, x) {
            if (!str_eq(json_get_str(x, "status"), "running") || json_get_bool(x, "quiet", false)) continue;
            const char *intent = json_get_str(x, "intent");
            sb_appendf(&b, "- `%s` [%s] \xE2\x80\x94 %s%s%s\n", json_get_str(x, "id"), json_get_str(x, "type"), intent ? intent : "",
                       intent ? " \xE2\x80\x94 " : "", json_get_str(x, "label") ? json_get_str(x, "label") : "");
        }
    }
    json_free(snap);
    while (b.len && isspace((unsigned char)b.s[b.len - 1])) sb_truncate(&b, b.len - 1);
    return sb_detach(&b);
}

/* noReplyText (tools/wait.ts): why a reply wait cannot, or can no longer, get its answer */
static char *wait_ids(const strvec_t *v) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < v->n; i++) sb_appendf(&b, "%s`%s`", i ? ", " : "", v->v[i]);
    return sb_detach(&b);
}

static char *no_reply_text(const agents_replies_t *r) {
    char *ids, *t;
    if (r->blocked.n) {
        ids = wait_ids(&r->blocked);
        t = xasprintf("%s cannot reply before you finish: it is waiting on your result. Finish without the answer.", ids);
    } else if (r->on_user.n) {
        ids = wait_ids(&r->on_user);
        t = xasprintf("%s is waiting on the user and cannot reply until they answer. Go on without the answer; its reply, if it comes, arrives as a message.", ids);
    } else if (r->stopped.n) {
        ids = wait_ids(&r->stopped);
        t = xasprintf("%s finished its turn without replying. Ask again, or finish without the answer.", ids);
    } else {
        return xstrdup("no background job or service you started is running, and no agent you messaged this run owes a reply. "
                       "Other agents' results and messages arrive on their own.");
    }
    free(ids);
    return t;
}

/* messageResult (irc/messaging.ts): `msgs` (owned) in the result text and details.waited */
static void wait_message_result(tool_env_t *env, json_t *msgs, tool_result_t *out) {
    sb_t b = SB_INIT;
    const json_t *m;
    json_arr_foreach(msgs, i, m) {
        const char *reply = json_get_str(m, "replyTo");
        if (i) sb_append(&b, "\n\n");
        sb_appendf(&b, "[%s] %s", json_get_str(m, "id"), json_get_str(m, "from"));
        if (reply) sb_appendf(&b, " (reply to %s)", reply);
        if (json_get_bool(m, "progress", false)) sb_append(&b, " (progress; reply follows)");
        sb_appendf(&b, ": %s", json_get_str(m, "body"));
    }
    tool_result_text(out, sb_cstr(&b));
    sb_free(&b);
    const char *self = agents_id_of_env(env);
    if (self) json_set_str(out->details, "from", self);
    json_set(out->details, "waited", msgs);
}

/* omp JobSnapshot rows: {id, type, status, label, durationMs, resultText | errorText}, settled ones then what still runs */
static json_t *wait_job_rows(jobs_t *jobs, const json_t *done) {
    json_t *rows = json_arr();
    const json_t *x;
    json_arr_foreach(done, i, x) {
        json_t *s = json_obj();
        bool failed = json_get_bool(x, "failed", false);
        json_set_str(s, "id", json_get_str(x, "jobId") ? json_get_str(x, "jobId") : "");
        json_set_str(s, "type", json_get_str(x, "type") ? json_get_str(x, "type") : "bash");
        json_set_str(s, "status", failed ? "failed" : "completed");
        json_set_str(s, "label", json_get_str(x, "label") ? json_get_str(x, "label") : "");
        if (json_get_str(x, "intent")) json_set_str(s, "intent", json_get_str(x, "intent"));
        json_set_int(s, "durationMs", json_get_int(x, "durationMs", 0));
        json_set_str(s, failed ? "errorText" : "resultText", json_get_str(x, "result") ? json_get_str(x, "result") : "");
        json_push(rows, s);
    }
    json_t *snap = jobs ? jobs_snapshot(jobs) : json_arr();
    json_arr_foreach(snap, i, x) {
        if (!str_eq(json_get_str(x, "status"), "running") || json_get_bool(x, "quiet", false)) continue;
        json_t *s = json_obj();
        json_set_str(s, "id", json_get_str(x, "id"));
        json_set_str(s, "type", json_get_str(x, "type"));
        json_set_str(s, "status", "running");
        json_set_str(s, "label", json_get_str(x, "label") ? json_get_str(x, "label") : "");
        if (json_get_str(x, "intent")) json_set_str(s, "intent", json_get_str(x, "intent"));
        json_set_int(s, "durationMs", json_get_int(x, "durationMs", 0));
        json_push(rows, s);
    }
    json_free(snap);
    return rows;
}

/* #elapsed: a window ran out with nothing new. Extend the idle streak and, from WAIT_NOTICE_RUNG on, append the reminder. */
static void wait_elapsed(tool_env_t *env, tool_result_t *out, int rung, int64_t streak_ms) {
    int64_t waited = streak_ms + WAIT_LADDER_MS[rung];
    env->wait_idle = true;
    env->wait_idle_rung = rung;
    env->wait_idle_ended = mono_ms();
    env->wait_idle_waited_ms = waited;
    if (rung < WAIT_NOTICE_RUNG) return;
    char *span = usage_fmt_duration((double)waited);
    json_t *c = json_obj();
    json_set_str(c, "waited", span);
    char *notice = prompt_render("coding-agent/prompts/tools/wait-idle.md", c);
    tool_result_text(out, notice);
    free(notice);
    json_free(c);
    free(span);
}

static void wait_emit_progress(tool_ctx_t *ctx, jobs_t *jobs) {
    if (!ctx || !ctx->update) return;
    tool_result_t p = {0};
    tool_result_text(&p, "");
    p.details = json_obj();
    json_set_str(p.details, "op", "wait");
    json_set(p.details, "jobs", wait_job_rows(jobs, NULL));
    ctx->update(ctx, &p);
    tool_result_free(&p);
}

typedef enum { WAKE_TIMEOUT, WAKE_JOB, WAKE_MESSAGE, WAKE_ABORT, WAKE_STEER, WAKE_UNANSWERABLE, WAKE_SERVICE } wait_wake_t;

static int wait_execute(agent_tool_t *self, tool_ctx_t *ctx, const json_t *args, tool_result_t *out) {
    tool_env_t *env = self->ud;
    UNUSED(args);
    out->details = json_obj();
    json_set_str(out->details, "op", "wait");
    /* any wait ends the idle streak unless its window elapses too */
    bool idle = env->wait_idle;
    int idle_rung = env->wait_idle_rung;
    int64_t idle_ended = env->wait_idle_ended, idle_waited = env->wait_idle_waited_ms;
    env->wait_idle = false;
    json_t *msgs = agents_take_messages(env);
    if (msgs) {
        wait_message_result(env, msgs, out);
        return 0;
    }
    jobs_t *jobs = env->jobs;
    /* an accepted completion whose delivery has not reached the transcript yet is exactly what this wait is for */
    if (jobs && jobs_undelivered(jobs)) {
        json_t *done = jobs_take_finished(jobs);
        char *txt = job_result_text(jobs, done, true);
        tool_result_text(out, txt);
        free(txt);
        json_set(out->details, "jobs", wait_job_rows(jobs, done));
        json_free(done);
        return 0;
    }
    size_t running = jobs ? jobs_running_watchable(jobs) : 0;
    bool service_running = jobs && jobs_running_all(jobs) > jobs_running(jobs);
    agents_replies_t replies;
    agents_pending_replies(env, &replies);
    if (!running && !service_running && !replies.from.n) {
        char *why = no_reply_text(&replies);
        tool_result_textf(out, "Nothing to wait for: %s", why);
        free(why);
        /* an idle poll, not a failure, unless an interrupt landed meanwhile or the text names an agent that can no longer answer */
        out->useless = !(ctx && ctx->cancel && *ctx->cancel) && !steer_queued(ctx) && !replies.blocked.n && !replies.on_user.n && !replies.stopped.n;
        json_set(out->details, "jobs", json_arr());
        agents_replies_free(&replies);
        return 0;
    }
    bool streak = idle && mono_ms() - idle_ended < WAIT_LADDER_RESET_MS;
    int rung = streak ? MIN(idle_rung + 1, WAIT_LADDER_RUNGS - 1) : 0;
    int64_t streak_ms = streak ? idle_waited : 0;
    bool reply_wait = !running && !service_running;
    /* a wait only these subagents' results can end blocks the caller on them; a bash job or service is another way out */
    strvec_t watched = {0};
    if (running && !service_running) {
        json_t *snap = jobs_snapshot(jobs);
        const json_t *x;
        size_t agents = 0;
        json_arr_foreach(snap, i, x) {
            if (!str_eq(json_get_str(x, "status"), "running") || json_get_bool(x, "quiet", false)) continue;
            if (json_get_str(x, "agentId")) {
                strvec_push(&watched, json_get_str(x, "agentId"));
                agents++;
            }
        }
        json_free(snap);
        if (agents != running) strvec_free(&watched);
    }
    agents_wait_enter(env, reply_wait ? &replies.from : NULL, watched.n ? &watched : NULL);
    strvec_free(&watched);
    agents_replies_free(&replies);
    agents_replies_t no_reply = {0};
    int64_t t0 = mono_ms(), deadline = t0 + WAIT_LADDER_MS[rung], next_progress = t0;
    wait_wake_t wake;
    volatile sig_atomic_t *cancel = ctx ? ctx->cancel : NULL;
    for (;;) {
        msgs = agents_take_messages(env);
        if (msgs) {
            wake = WAKE_MESSAGE;
            break;
        }
        if (cancel && *cancel) {
            wake = WAKE_ABORT;
            break;
        }
        if (steer_queued(ctx)) {
            wake = WAKE_STEER;
            break;
        }
        if (jobs && jobs_undelivered(jobs)) {
            wake = WAKE_JOB;
            break;
        }
        if (service_running && !running && jobs_running_all(jobs) == jobs_running(jobs)) {
            wake = WAKE_SERVICE;
            break;
        }
        if (reply_wait) {
            agents_replies_free(&no_reply);
            agents_pending_replies(env, &no_reply);
            if (!no_reply.from.n) {
                wake = WAKE_UNANSWERABLE;
                break;
            }
        }
        int64_t now = mono_ms();
        if (now >= deadline) {
            wake = WAKE_TIMEOUT;
            break;
        }
        if (running && now >= next_progress) {
            wait_emit_progress(ctx, jobs);
            next_progress = now + WAIT_PROGRESS_INTERVAL_MS;
        }
        int64_t nap = MIN(100, deadline - now);
        if (jobs && (running || service_running)) jobs_wait_any_ex(jobs, cancel, nap, true);
        else sleep_ms(nap);
    }
    if (wake == WAKE_JOB && jobs_running_watchable(jobs)) {
        /* keep collecting briefly so near-simultaneous completions share one result; other wakes still end it */
        int64_t settle = mono_ms() + WAIT_SETTLE_WINDOW_MS;
        while (jobs_running_watchable(jobs) && mono_ms() < settle && !(cancel && *cancel) && !steer_queued(ctx)) {
            msgs = agents_take_messages(env);
            if (msgs) {
                wake = WAKE_MESSAGE;
                break;
            }
            sleep_ms(50);
        }
    }
    agents_wait_leave(env);
    /* a dequeued message wins a photo-finish with a job: the job remains deliverable, a lost message cannot be recovered */
    if (!msgs) msgs = agents_take_messages(env);
    if (msgs) {
        wait_message_result(env, msgs, out);
    } else if (cancel && *cancel) {
        tool_result_error(out, "Operation aborted");
    } else if (wake == WAKE_STEER) {
        /* not useless: a message woke the agent, which the loop guard must treat as progress */
        tool_result_text(out, "Wait interrupted by message.");
        json_set(out->details, "jobs", json_arr());
        json_set_bool(out->details, "interrupted", true);
    } else if (running || (jobs && jobs_undelivered(jobs))) {
        json_t *done = jobs_take_finished(jobs);
        char *txt = job_result_text(jobs, done, false);
        tool_result_text(out, *txt ? txt : "No background jobs.");
        free(txt);
        json_set(out->details, "jobs", wait_job_rows(jobs, done));
        /* a wait where everything is still running carries no new information once a later wait exists */
        out->useless = !json_len(done) && wake != WAKE_SERVICE;
        if (wake == WAKE_TIMEOUT) wait_elapsed(env, out, rung, streak_ms);
        json_free(done);
    } else if (wake == WAKE_TIMEOUT) {
        char *span = usage_fmt_duration((double)WAIT_LADDER_MS[rung]);
        if (reply_wait) tool_result_textf(out, "No reply within %s.", span);
        else tool_result_textf(out, "No service finished within %s. Read proc:// for status.", span);
        free(span);
        out->useless = true; /* a wait that ran out with nothing to report is an idle poll */
        json_set(out->details, "jobs", json_arr());
        wait_elapsed(env, out, rung, streak_ms);
    } else {
        if (wake == WAKE_UNANSWERABLE) {
            char *why = no_reply_text(&no_reply);
            tool_result_text(out, why);
            free(why);
        } else {
            tool_result_text(out, "A service finished. Read proc:// for its status and output.");
        }
        json_set(out->details, "jobs", json_arr());
    }
    agents_replies_free(&no_reply);
    return 0;
}

agent_tool_t *tool_wait_new(tool_env_t *env) {
    char *desc = tool_prompt("wait", NULL);
    json_t *s = json_parse_str("{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}");
    agent_tool_t *t = agent_tool_new("wait", "Wait", desc, s, wait_execute);
    free(desc);
    t->intent_mode = INTENT_OPTIONAL; /* wait.ts: intent = "optional" */
    t->ud = env;
    return t;
}
