/* patch.c - replace / patch / apply_patch / sloppy edit modes.
 * Port of omp pi-edit: modes/{replace,patch,apply_patch}.rs, diff_string.rs hunk parsing, files.rs reads and the
 * session apply (stage all files in memory, then write in payload order, then render result text). Error strings
 * are byte-identical to upstream: models are trained on them. */
#include "patch.h"

#include "core/fs.h"
#include "text/regex.h"
#include "diff.h"
#include "fuzzy.h"
#include "hashline.h"
#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

/* ======================================================================================
 * errors
 * ====================================================================================== */

typedef struct {
    char *msg;
    const char *kind;
} perr_t;

#define K_PARSE "parse"
#define K_APPLY "apply"
#define K_UTF8 "invalid_utf8"
#define K_MATCH "no_match"
#define K_NOTFOUND "file_not_found"
#define K_NOCHANGE "no_change"
#define K_IO "io"
#define K_WRITE "write_failed"

static bool fail(perr_t *e, const char *kind, char *msg) {
    free(e->msg);
    e->msg = msg;
    e->kind = kind;
    return false;
}

#define FAIL(e, kind, ...) fail((e), (kind), xasprintf(__VA_ARGS__))

/* "[path]: msg\nNo files were modified — sections apply atomically." (keeps the source kind) */
static void wrap_in_file(perr_t *e, const char *path) {
    char *m = xasprintf("[%s]: %s\nNo files were modified — sections apply atomically.", path, e->msg);
    free(e->msg);
    e->msg = m;
}

/* Rust `{:?}` of a &str */
static char *dbg_str(const char *s) {
    sb_t b = SB_INIT;
    sb_putc(&b, '"');
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        switch (cp) {
        case '"': sb_append(&b, "\\\""); break;
        case '\\': sb_append(&b, "\\\\"); break;
        case '\n': sb_append(&b, "\\n"); break;
        case '\r': sb_append(&b, "\\r"); break;
        case '\t': sb_append(&b, "\\t"); break;
        case 0: sb_append(&b, "\\0"); break;
        default:
            if (cp < 0x20 || cp == 0x7f) sb_appendf(&b, "\\u{%x}", cp);
            else sb_appendn(&b, s + i, (size_t)k);
        }
        i += (size_t)k;
    }
    sb_putc(&b, '"');
    return sb_detach(&b);
}

static char *join(char *const *v, size_t n, const char *sep) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n; i++) {
        if (i) sb_append(&b, sep);
        sb_append(&b, v[i]);
    }
    return sb_detach(&b);
}

static bool vec_eq(const strvec_t *a, const strvec_t *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->v[i], b->v[i]) != 0) return false;
    return true;
}

static strvec_t vec_copy(char *const *v, size_t n) {
    strvec_t o = {0};
    for (size_t i = 0; i < n; i++) strvec_push(&o, v[i]);
    return o;
}

/* Rust str::trim (Unicode White_Space ~ JS set) */
static bool rs_blank(const char *s) { return fz_blank(s); }

/* ======================================================================================
 * file source
 * ====================================================================================== */

typedef struct {
    char *abs, *display;
    char *raw;
    bool bom, crlf, notebook;
    char *text;
} fread_t;

typedef struct {
    char *abs;
    bool deleted;
    char *raw;
} vfile_t;

typedef struct {
    const pe_opts_t *o;
    VEC(vfile_t) virt;
    VEC(fread_t *) reads;
} fsrc_t;

static void fsrc_free(fsrc_t *fs) {
    for (size_t i = 0; i < fs->virt.n; i++) {
        free(fs->virt.v[i].abs);
        free(fs->virt.v[i].raw);
    }
    vec_free(&fs->virt);
    for (size_t i = 0; i < fs->reads.n; i++) {
        fread_t *r = fs->reads.v[i];
        free(r->abs);
        free(r->display);
        free(r->raw);
        free(r->text);
        free(r);
    }
    vec_free(&fs->reads);
}

/* `[path#ABCD]` / `[path]` header spellings address the bare path */
static char *unwrap_header_path(const char *authored) {
    char *t = fz_trim_end(authored);
    size_t n = strlen(t);
    if (n >= 2 && t[0] == '[' && t[n - 1] == ']') {
        char *inner = xstrndup(t + 1, n - 2);
        char *hash = strrchr(inner, '#');
        if (hash) {
            const char *tag = hash + 1;
            bool ok = strlen(tag) == 4;
            for (int i = 0; ok && i < 4; i++) ok = isxdigit((unsigned char)tag[i]) != 0;
            if (!ok) {
                free(inner);
                free(t);
                return xstrdup(authored);
            }
            *hash = 0;
        }
        free(t);
        if (!*inner) {
            free(inner);
            return xstrdup(authored);
        }
        return inner;
    }
    free(t);
    return xstrdup(authored);
}

static bool fs_resolve(fsrc_t *fs, const char *authored, char **abs, char **display, perr_t *e) {
    *display = unwrap_header_path(authored);
    char *err = NULL;
    if (fs->o->resolve) *abs = fs->o->resolve(fs->o->ud, *display, &err);
    else *abs = path_abs(*display, fs->o->cwd);
    if (!*abs) {
        fail(e, K_APPLY, err ? err : xasprintf("Cannot resolve path: %s", *display));
        free(*display);
        *display = NULL;
        return false;
    }
    free(err);
    return true;
}

static vfile_t *vfind(fsrc_t *fs, const char *abs) {
    for (size_t i = 0; i < fs->virt.n; i++)
        if (!strcmp(fs->virt.v[i].abs, abs)) return &fs->virt.v[i];
    return NULL;
}

static bool is_remote(const fsrc_t *fs, const char *abs) {
    return fs->o->remote && fs->o->remote_read && fs->o->remote(fs->o->ud, abs);
}

static bool fs_exists_abs(fsrc_t *fs, const char *abs) {
    vfile_t *v = vfind(fs, abs);
    if (v) return !v->deleted;
    if (is_remote(fs, abs)) {
        char *data = NULL, *err = NULL;
        size_t len;
        int rc = fs->o->remote_read(fs->o->ud, abs, &data, &len, &err);
        free(data);
        free(err);
        return rc != ENOENT;
    }
    struct stat st;
    return stat(abs, &st) == 0;
}

static size_t utf8_valid_up_to(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!n || i + n > len || !utf8_valid(s + i, n)) return i;
        i += n;
    }
    return len;
}


/* ---------- Jupyter notebooks (notebook.rs): editable "# %% [type] cell:N" projection ---------- */

static bool is_notebook_path(const char *p) { return !strcasecmp(path_ext(p), ".ipynb"); }

static bool nb_cell_type_ok(const char *t) {
    return t && (!strcmp(t, "code") || !strcmp(t, "markdown") || !strcmp(t, "raw"));
}

/* ^# %{min,} \[(code|markdown|raw)\]( cell:\d+)?$ ; exact = exactly min '%' */
static bool nb_marker(const char *l, int min_pct, bool exact, char **type, long *index) {
    if (!str_starts(l, "# ")) return false;
    const char *p = l + 2;
    int n = 0;
    while (*p == '%') {
        p++;
        n++;
    }
    if (n < min_pct || (exact && n != min_pct) || *p != ' ' || p[1] != '[') return false;
    p += 2;
    const char *t0 = p;
    while (*p && *p != ']') p++;
    if (*p != ']') return false;
    char *t = xstrndup(t0, (size_t)(p - t0));
    long idx = -1;
    bool ok = nb_cell_type_ok(t);
    p++;
    if (ok && *p) {
        if (!str_starts(p, " cell:") || !isdigit((unsigned char)p[6])) ok = false;
        else {
            p += 6;
            idx = 0;
            while (isdigit((unsigned char)*p) && idx < 100000000) idx = idx * 10 + (*p++ - '0');
            if (*p) ok = false;
        }
    }
    if (ok && type) *type = t;
    else free(t);
    if (index) *index = idx;
    return ok;
}

static char *nb_source_text(const json_t *src) {
    if (json_is_str(src)) return xstrdup(json_sv(src));
    sb_t b = SB_INIT;
    if (json_is_arr(src))
        for (size_t i = 0; i < json_len(src); i++)
            if (json_sv(json_at(src, i))) sb_append(&b, json_sv(json_at(src, i)));
    return sb_detach(&b);
}

static json_t *nb_parse_validate(const char *raw, const char *display, char **err) {
    const char *j = str_starts(raw, "\xEF\xBB\xBF") ? raw + 3 : raw;
    json_t *nb = json_parse_str(j);
    if (!nb) {
        *err = xasprintf("Invalid JSON in notebook: %s", display);
        return NULL;
    }
    if (!json_is_obj(nb)) {
        json_free(nb);
        *err = xasprintf("Invalid notebook structure (expected object): %s", display);
        return NULL;
    }
    json_t *cells = json_get_arr(nb, "cells");
    if (!cells) {
        json_free(nb);
        *err = xasprintf("Invalid notebook structure (missing cells array): %s", display);
        return NULL;
    }
    for (size_t i = 0; i < json_len(cells); i++) {
        json_t *c = json_at(cells, i);
        if (!json_is_obj(c) || !nb_cell_type_ok(json_get_str(c, "cell_type"))) {
            json_free(nb);
            *err = xasprintf("Invalid notebook cell %zu in %s", i, display);
            return NULL;
        }
    }
    return nb;
}

static char *nb_to_text(const char *raw, const char *display, char **err) {
    json_t *nb = nb_parse_validate(raw, display, err);
    if (!nb) return NULL;
    json_t *cells = json_get_arr(nb, "cells");
    sb_t b = SB_INIT;
    for (size_t i = 0; i < json_len(cells); i++) {
        json_t *c = json_at(cells, i);
        char *src = nb_source_text(json_get(c, "source"));
        if (i) sb_putc(&b, '\n');
        sb_appendf(&b, "# %%%% [%s] cell:%zu", json_get_str(c, "cell_type"), i);
        if (*src) {
            sb_putc(&b, '\n');
            strvec_t ls = str_split(src, "\n");
            for (size_t k = 0; k < ls.n; k++) {
                if (k) sb_putc(&b, '\n');
                /* escape marker-like source lines: "# %%" -> "# %%%" */
                if (nb_marker(ls.v[k], 2, false, NULL, NULL)) sb_appendf(&b, "# %%%s", ls.v[k] + 2);
                else sb_append(&b, ls.v[k]);
            }
            strvec_free(&ls);
        }
        free(src);
    }
    json_free(nb);
    return sb_detach(&b);
}

static json_t *nb_source_array(const char *src) {
    json_t *a = json_arr();
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        json_push(a, json_strn(p, len));
        p += len;
    }
    return a;
}

/* JavaScript Number.prototype.toString for finite doubles */
static void nb_js_number(sb_t *b, const json_t *v) {
    if (v->is_int) {
        sb_appendf(b, "%lld", (long long)v->u.num.i);
        return;
    }
    double d = v->u.num.d;
    if (d == 0) {
        sb_append(b, "0");
        return;
    }
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, d);
        if (strtod(buf, NULL) == d) break;
    }
    const char *q = buf;
    bool neg = *q == '-';
    if (neg) q++;
    char digits[32];
    size_t nd = 0;
    for (; *q && *q != 'e' && nd < sizeof(digits) - 1; q++)
        if (isdigit((unsigned char)*q)) digits[nd++] = *q;
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    int pos = (*q == 'e' ? atoi(q + 1) : 0) + 1;
    if (neg) sb_putc(b, '-');
    if (pos > 0 && pos <= 21) {
        if ((size_t)pos >= nd) {
            sb_append(b, digits);
            for (size_t k = nd; k < (size_t)pos; k++) sb_putc(b, '0');
        } else {
            sb_appendf(b, "%.*s.%s", pos, digits, digits + pos);
        }
    } else if (pos <= 0 && pos > -6) {
        sb_append(b, "0.");
        for (int k = 0; k < -pos; k++) sb_putc(b, '0');
        sb_append(b, digits);
    } else {
        sb_putc(b, digits[0]);
        if (nd > 1) sb_appendf(b, ".%s", digits + 1);
        sb_appendf(b, "e%s%d", pos - 1 >= 0 ? "+" : "", pos - 1);
    }
}

static void nb_json_string(sb_t *b, const char *s) {
    sb_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') sb_append(b, "\\\"");
        else if (c == '\\') sb_append(b, "\\\\");
        else if (c == '\b') sb_append(b, "\\b");
        else if (c == '\f') sb_append(b, "\\f");
        else if (c == '\n') sb_append(b, "\\n");
        else if (c == '\r') sb_append(b, "\\r");
        else if (c == '\t') sb_append(b, "\\t");
        else if (c < 0x20) sb_appendf(b, "\\u%04x", c);
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
}

/* JSON.stringify(value, null, 1) */
static void nb_stringify(sb_t *b, const json_t *v, int depth) {
    switch (v ? v->type : JSON_NULL) {
    case JSON_NULL: sb_append(b, "null"); break;
    case JSON_BOOL: sb_append(b, v->u.b ? "true" : "false"); break;
    case JSON_NUMBER: nb_js_number(b, v); break;
    case JSON_STRING: nb_json_string(b, v->u.str.s); break;
    case JSON_ARRAY:
        if (!v->u.arr.n) {
            sb_append(b, "[]");
            break;
        }
        sb_append(b, "[\n");
        for (size_t i = 0; i < v->u.arr.n; i++) {
            for (int k = 0; k <= depth; k++) sb_putc(b, ' ');
            nb_stringify(b, v->u.arr.items[i], depth + 1);
            sb_append(b, i + 1 < v->u.arr.n ? ",\n" : "\n");
        }
        for (int k = 0; k < depth; k++) sb_putc(b, ' ');
        sb_putc(b, ']');
        break;
    case JSON_OBJECT:
        if (!v->u.obj.n) {
            sb_append(b, "{}");
            break;
        }
        sb_append(b, "{\n");
        for (size_t i = 0; i < v->u.obj.n; i++) {
            for (int k = 0; k <= depth; k++) sb_putc(b, ' ');
            nb_json_string(b, v->u.obj.keys[i]);
            sb_append(b, ": ");
            nb_stringify(b, v->u.obj.vals[i], depth + 1);
            sb_append(b, i + 1 < v->u.obj.n ? ",\n" : "\n");
        }
        for (int k = 0; k < depth; k++) sb_putc(b, ' ');
        sb_putc(b, '}');
        break;
    }
}

static json_t *nb_cell(const json_t *orig_cells, size_t norig, bool *used, const char *type, long idx,
                       const strvec_t *src_lines) {
    char *src = strvec_join(src_lines, "\n");
    json_t *cell;
    if (idx >= 0 && (size_t)idx < norig && !used[idx]) {
        used[idx] = true;
        cell = json_clone(json_at(orig_cells, (size_t)idx));
        json_set_str(cell, "cell_type", type);
        json_set(cell, "source", nb_source_array(src));
        if (!strcmp(type, "code")) {
            if (json_is_null(json_get(cell, "execution_count"))) json_set(cell, "execution_count", json_null());
            if (json_is_null(json_get(cell, "outputs"))) json_set(cell, "outputs", json_arr());
        } else {
            json_del(cell, "execution_count");
            json_del(cell, "outputs");
        }
    } else {
        cell = json_obj();
        json_set_str(cell, "cell_type", type);
        json_set(cell, "metadata", json_obj());
        json_set(cell, "source", nb_source_array(src));
        if (!strcmp(type, "code")) {
            json_set(cell, "execution_count", json_null());
            json_set(cell, "outputs", json_arr());
        }
    }
    free(src);
    return cell;
}

/* apply edited text onto the original notebook JSON (NULL = new nbformat 4.5 document) and serialize with
 * 1-space indentation; owned JSON text or NULL + *err */
static char *nb_serialize(const char *original, const char *text, const char *display, char **err) {
    json_t *nb;
    if (original) {
        nb = nb_parse_validate(original, display, err);
        if (!nb) return NULL;
    } else {
        nb = json_obj();
        json_set(nb, "cells", json_arr());
        json_set(nb, "metadata", json_obj());
        json_set(nb, "nbformat", json_int(4));
        json_set(nb, "nbformat_minor", json_int(5));
    }
    json_t *orig = json_get_arr(nb, "cells");
    size_t norig = json_len(orig);
    bool *used = xcalloc(norig + 1, sizeof(bool));
    json_t *next = json_arr();
    strvec_t lines = {0};
    if (*text) lines = str_split(text, "\n");
    char *ctype = NULL;
    long cidx = -1;
    strvec_t src = {0};
    bool ok = true;
    for (size_t i = 0; i < lines.n; i++) {
        char *t = NULL;
        long idx = -1;
        if (nb_marker(lines.v[i], 2, true, &t, &idx)) {
            if (ctype) json_push(next, nb_cell(orig, norig, used, ctype, cidx, &src));
            free(ctype);
            strvec_free(&src);
            ctype = t;
            cidx = idx;
            continue;
        }
        if (!ctype) {
            *err = xasprintf("Invalid notebook editable representation for %s: expected first line to be \"# %%%% "
                             "[code] cell:0\", \"# %%%% [markdown] cell:0\", or \"# %%%% [raw] cell:0\".",
                             display);
            ok = false;
            break;
        }
        /* unescape "# %%%..." source lines */
        if (nb_marker(lines.v[i], 3, false, NULL, NULL)) strvec_push_own(&src, xasprintf("# %s", lines.v[i] + 3));
        else strvec_push(&src, lines.v[i]);
    }
    if (ok && ctype) json_push(next, nb_cell(orig, norig, used, ctype, cidx, &src));
    char *out = NULL;
    if (ok) {
        json_set(nb, "cells", next);
        next = NULL;
        sb_t b = SB_INIT;
        nb_stringify(&b, nb, 0);
        out = sb_detach(&b);
    }
    json_free(next);
    free(ctype);
    strvec_free(&src);
    strvec_free(&lines);
    free(used);
    json_free(nb);
    return out;
}

/* ======================================================================================
 * generated-file guard (pi-edit path_policy.rs: generated_filename, detect_generated_marker,
 * comment_styles, leading_comment_text, auto_generated_message)
 * ====================================================================================== */

enum { CS_SLASH = 1, CS_HASH = 2, CS_SQL = 4, CS_HTML = 8 };

static bool word_in(const char *w, const char *const *list) {
    for (; *list; list++)
        if (str_eq(w, *list)) return true;
    return false;
}

/* final path component with `\` read as `/` (borrowed from `norm`, a writable copy of the display path) */
static const char *display_basename(char *norm) {
    for (char *p = norm; *p; p++)
        if (*p == '\\') *p = '/';
    const char *slash = strrchr(norm, '/');
    return slash ? slash + 1 : norm;
}

/* basenames only generators produce: these block without inspecting content */
static bool generated_filename(const char *name) {
    static const char *const patterns[] = {
        "^zz_generated\\.",       "\\.pb\\.(go|cc|h|c|js|ts)$", "_pb2\\.py$",
        "_pb2_grpc\\.py$",        "\\.gen\\.(go|ts|js|py)$",    "\\.swagger\\.json$",
        "\\.mock\\.(go|ts)$",     "\\.mocks?\\.(go|ts|js)$",
    };
    for (size_t i = 0; i < ARRAY_LEN(patterns); i++) {
        rx_t *rx = rx_compile(patterns[i], 0, NULL);
        bool hit = rx && rx_matches(rx, name);
        rx_free(rx);
        if (hit) return true;
    }
    return false;
}

static int comment_styles(const char *basename) {
    static const char *const slash[] = {"c", "cc", "cpp", "cs", "dart", "go", "h", "hpp", "java", "js", "jsx", "kt", "kts",
                                        "mjs", "cjs", "php", "rs", "scala", "swift", "ts", "tsx", NULL};
    static const char *const hash[] = {"py", "rb", "sh", "bash", "zsh", "yml", "yaml", "toml", "ini", "cfg", "conf", "env",
                                       "pl", "r", NULL};
    static const char *const html[] = {"html", "htm", "xml", "svg", "xhtml", NULL};
    char *name = xstrdup(basename);
    for (char *p = name; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    int styles = 0;
    /* Path::extension: text after the last dot, none for a leading dot (`.env`) */
    const char *dot = strrchr(name, '.');
    const char *ext = dot && dot != name ? dot + 1 : "";
    if (str_eq(name, "dockerfile") || str_eq(name, "makefile") || str_eq(name, "justfile")) styles = CS_HASH;
    else if (word_in(ext, slash)) styles = CS_SLASH;
    else if (word_in(ext, hash)) styles = CS_HASH;
    else if (str_eq(ext, "sql")) styles = CS_SQL;
    else if (word_in(ext, html)) styles = CS_HTML;
    free(name);
    return styles;
}

/* the file's leading comment block, at most its first 40 lines (owned) */
static char *leading_comment_text(const char *content, int styles) {
    sb_t out = SB_INIT;
    bool first = true, started = false, slash_block = false, html_block = false;
#define PUSH_LINE(text)                  \
    do {                                 \
        if (!first) sb_putc(&out, '\n'); \
        first = false;                   \
        sb_append(&out, (text));         \
    } while (0)
    const char *p = content;
    for (int index = 0; index < 40 && *p; index++) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *raw = xstrndup(p, n);
        p += n + (nl ? 1 : 0);
        char *line = fz_trim(raw);
        free(raw);
        bool stop = false;
        if (index == 0 && str_starts(line, "#!")) {
            /* shebang: skipped */
        } else if (slash_block) {
            PUSH_LINE(line);
            slash_block = !strstr(line, "*/");
        } else if (html_block) {
            PUSH_LINE(line);
            html_block = !strstr(line, "-->");
        } else if (!*line) {
            if (started) PUSH_LINE("");
        } else {
            bool opens_slash = (styles & CS_SLASH) && str_starts(line, "/*");
            bool opens_html = (styles & CS_HTML) && str_starts(line, "<!--");
            bool accepted = ((styles & CS_SLASH) && str_starts(line, "//")) || ((styles & CS_HASH) && *line == '#') ||
                            ((styles & CS_SQL) && str_starts(line, "--")) || opens_html;
            if (!accepted && !opens_slash) {
                stop = true;
            } else {
                started = true;
                PUSH_LINE(line);
                if (opens_slash) slash_block = !strstr(line, "*/");
                if (opens_html) html_block = !strstr(line, "-->");
            }
        }
        free(line);
        if (stop) break;
    }
#undef PUSH_LINE
    return sb_detach(&out);
}

/* the first marker found in the header comment (owned) or NULL */
static char *detect_generated_marker(const char *content, const char *basename) {
    int styles = comment_styles(basename);
    if (!styles) return NULL;
#define KNOWN_GENERATORS                                                                                                       \
    "(?:protoc(?:-gen-[\\w-]+)?|sqlc|buf|swagger(?:-codegen)?|openapi(?:-generator)?|grpc-gateway|mockery|stringer|easyjson|" \
    "deepcopy-gen|defaulter-gen|conversion-gen|client-gen|lister-gen|informer-gen|kysely-codegen|napi-rs)"
    static const char *const patterns[] = {
        "(?i)@generated\\b",
        "(?i)\\bcode\\s+generated\\s+by\\s+[a-z0-9_.-]+",
        "(?i)\\bcode\\s+generated\\b.*\\bdo\\s+not\\s+edit\\b",
        "(?i)\\bthis\\s+file\\s+was\\s+automatically\\s+generated\\b",
        "(?i)\\bgenerated\\s+by\\s+" KNOWN_GENERATORS "\\b",
    };
#undef KNOWN_GENERATORS
    if (str_starts(content, "\xEF\xBB\xBF")) content += 3;
    char *header = leading_comment_text(content, styles);
    char *marker = NULL;
    for (size_t i = 0; i < ARRAY_LEN(patterns) && !marker; i++) {
        rx_t *rx = rx_compile(patterns[i], 0, NULL);
        size_t ms, me;
        if (rx && rx_find(rx, header, strlen(header), 0, &ms, &me)) marker = xstrndup(header + ms, me - ms);
        rx_free(rx);
    }
    free(header);
    return marker;
}

char *pe_auto_generated_message(const char *display, const char *head, size_t len) {
    char *norm = xstrdup(display);
    const char *name = display_basename(norm);
    char *marker = NULL;
    if (generated_filename(name)) {
        marker = xstrdup(name);
    } else {
        char *prefix = utf8_sanitize(head, MIN(len, (size_t)1024));
        marker = detect_generated_marker(prefix, name);
        free(prefix);
    }
    free(norm);
    if (!marker) return NULL;
    char *msg = xasprintf("Cannot modify auto-generated file: %s\n\nThis file appears to be automatically generated (detected "
                          "marker: \"%s\").\nAuto-generated files should not be edited directly. Instead:\n1. Find the source "
                          "file or generator configuration\n2. Make changes to the source\n3. Regenerate the file",
                          display, marker);
    free(marker);
    return msg;
}

char *pe_auto_generated_file_message(const char *abs_path, const char *display) {
    int fd = open(abs_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    char head[1024];
    size_t total = 0;
    bool ok = true;
    while (total < sizeof head) {
        ssize_t n = read(fd, head + total, sizeof head - total);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ok = false; /* a directory or an unreadable file is editable */
        if (n <= 0) break;
        total += (size_t)n;
    }
    close(fd);
    return ok ? pe_auto_generated_message(display, head, total) : NULL;
}

static fread_t *build_read(fsrc_t *fs, const char *abs, const char *display, char *raw, size_t len, perr_t *e) {
    if (fs->o->block_auto_generated) {
        char *guard = pe_auto_generated_message(display, raw, len);
        if (guard) {
            fail(e, K_APPLY, guard);
            free(raw);
            return NULL;
        }
    }
    size_t ok = utf8_valid_up_to(raw, len);
    if (ok < len) {
        FAIL(e, K_UTF8, "%s is not valid UTF-8 at byte %zu; refusing to edit", display, ok);
        free(raw);
        return NULL;
    }
    if (memchr(raw, 0, len)) {
        FAIL(e, K_APPLY, "Cannot edit '%s': it contains NUL bytes.", display);
        free(raw);
        return NULL;
    }
    fread_t *r = xcalloc(1, sizeof(*r));
    r->abs = xstrdup(abs);
    r->display = xstrdup(display);
    r->raw = raw;
    const char *body = raw;
    if (str_starts(raw, "\xEF\xBB\xBF")) {
        r->bom = true;
        body += 3;
    }
    const char *lf = strchr(body, '\n'), *crlf = strstr(body, "\r\n");
    r->crlf = lf && crlf && crlf < lf;
    if (is_notebook_path(abs)) {
        char *err = NULL, *t = nb_to_text(raw, display, &err);
        if (!t) {
            fail(e, K_APPLY, err);
            free(r->abs);
            free(r->display);
            free(r->raw);
            free(r);
            return NULL;
        }
        r->notebook = true;
        r->bom = r->crlf = false;
        r->text = fz_normalize_to_lf(t);
        free(t);
    } else {
        r->text = fz_normalize_to_lf(body);
    }
    vec_push(&fs->reads, r);
    return r;
}

/* *out NULL when the target does not exist */
static bool fs_try_read(fsrc_t *fs, const char *abs, const char *display, fread_t **out, perr_t *e) {
    *out = NULL;
    vfile_t *v = vfind(fs, abs);
    if (v) {
        if (v->deleted) return true;
        *out = build_read(fs, abs, display, xstrdup(v->raw), strlen(v->raw), e);
        return *out != NULL;
    }
    if (is_remote(fs, abs)) {
        char *data = NULL, *err = NULL;
        size_t len = 0;
        int rc = fs->o->remote_read(fs->o->ud, abs, &data, &len, &err);
        if (rc == ENOENT) {
            free(err);
            return true;
        }
        if (rc == EISDIR) {
            free(err);
            free(data);
            return FAIL(e, K_IO, "%s (os error %d)", "Is a directory", EISDIR);
        }
        if (rc != 0) return fail(e, K_IO, err ? err : xstrdup("remote read failed"));
        *out = build_read(fs, abs, display, data, len, e);
        return *out != NULL;
    }
    struct stat st;
    if (stat(abs, &st) != 0) return true;
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
        const char *kind = S_ISCHR(st.st_mode)    ? "character device"
                           : S_ISBLK(st.st_mode)  ? "block device"
                           : S_ISFIFO(st.st_mode) ? "FIFO"
                           : S_ISSOCK(st.st_mode) ? "socket"
                                                  : "special file";
        return FAIL(e, K_APPLY, "Cannot edit '%s': it is a %s, not a regular file or directory.", display, kind);
    }
    if (S_ISDIR(st.st_mode)) return FAIL(e, K_IO, "%s (os error %d)", "Is a directory", EISDIR);
    size_t len;
    errno = 0;
    char *raw = fs_read(abs, &len);
    if (!raw) {
        if (errno == ENOENT) return true;
        int er = errno ? errno : EIO;
        return FAIL(e, K_IO, "%s (os error %d)", strerror(er), er);
    }
    *out = build_read(fs, abs, display, raw, len, e);
    return *out != NULL;
}

static fread_t *fs_read_path(fsrc_t *fs, const char *authored, perr_t *e) {
    char *abs, *display;
    if (!fs_resolve(fs, authored, &abs, &display, e)) return NULL;
    fread_t *r = NULL;
    if (fs_try_read(fs, abs, display, &r, e) && !r) FAIL(e, K_NOTFOUND, "File not found: %s", display);
    free(abs);
    free(display);
    return r;
}

static char *persist(const fread_t *r, const char *after) {
    if (r->notebook) {
        char *err = NULL, *j = nb_serialize(r->raw, after, r->display, &err);
        free(err);
        return j ? j : xstrdup(after);
    }
    sb_t b = SB_INIT;
    if (r->bom) sb_append(&b, "\xEF\xBB\xBF");
    if (r->crlf) {
        for (const char *p = after; *p; p++) {
            if (*p == '\n') sb_append(&b, "\r\n");
            else sb_putc(&b, *p);
        }
    } else {
        sb_append(&b, after);
    }
    return sb_detach(&b);
}

/* ======================================================================================
 * staged files
 * ====================================================================================== */

enum { OP_CREATE, OP_UPDATE, OP_DELETE, OP_NOOP };

typedef struct {
    char *display, *abs;
    int op;
    char *move_abs, *move_display;
    bool existed;
    char *before_raw;
    char *before, *after;
    char *persisted;
    char *diff, *preview_diff;
    int64_t first_changed;
    strvec_t after_preview, warnings;
    /* hashline: `[path#TAG]` header, block-resolution rows above the preview, no-op text */
    bool hashline;
    strvec_t before_preview;
    char *text_override;
} staged_t;
typedef VEC(staged_t) stageds_t;

static void staged_free(staged_t *s) {
    free(s->display);
    free(s->abs);
    free(s->move_abs);
    free(s->move_display);
    free(s->before_raw);
    free(s->before);
    free(s->after);
    free(s->persisted);
    free(s->diff);
    free(s->preview_diff);
    strvec_free(&s->after_preview);
    strvec_free(&s->warnings);
    strvec_free(&s->before_preview);
    free(s->text_override);
}

static void stageds_free(stageds_t *v) {
    for (size_t i = 0; i < v->n; i++) staged_free(&v->v[i]);
    vec_free(v);
}

/* ======================================================================================
 * unified-diff hunk parsing (diff_string.rs)
 * ====================================================================================== */

typedef struct {
    char *change_context;      /* NULL = none */
    int64_t old_start, new_start; /* -1 = none */
    bool has_context_lines;
    strvec_t old_lines, new_lines;
    bool eof;
} hunk_t;
typedef VEC(hunk_t) hunks_t;

static void hunks_free(hunks_t *h) {
    for (size_t i = 0; i < h->n; i++) {
        free(h->v[i].change_context);
        strvec_free(&h->v[i].old_lines);
        strvec_free(&h->v[i].new_lines);
    }
    vec_free(h);
}

static const char *const MULTI_FILE_MARKERS[] = {"*** Update File:", "*** Add File:", "*** Delete File:", "diff --git "};
static const char *const METADATA_PREFIXES[] = {"*** Update File:", "*** Add File:", "*** Delete File:", "diff --git ",
                                                "index ", "--- ", "+++ ", "new file mode ", "deleted file mode ",
                                                "rename from ", "rename to ", "similarity index ",
                                                "dissimilarity index ", "old mode ", "new mode "};

static bool is_diff_content_line(const char *l) {
    if (l[0] == ' ') return true;
    if (l[0] == '+') return !str_starts(l, "+++ ");
    if (l[0] == '-') return !str_starts(l, "--- ");
    return false;
}

static bool starts_any(const char *l, const char *const *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (str_starts(l, p[i])) return true;
    return false;
}

static bool is_wrapper_line(const char *t) {
    return !strcmp(t, "***") || str_starts(t, "*** Begin Patch") || str_starts(t, "*** End Patch");
}

static char *normalize_diff(const char *diff) {
    strvec_t lines = fz_split(diff);
    while (lines.n) {
        const char *l = lines.v[lines.n - 1];
        if (!*l || (rs_blank(l) && !is_diff_content_line(l))) free(lines.v[--lines.n]);
        else break;
    }
    if (lines.n) {
        char *t = fz_trim(lines.v[0]);
        if (is_wrapper_line(t)) {
            free(lines.v[0]);
            memmove(lines.v, lines.v + 1, (lines.n - 1) * sizeof(char *));
            lines.n--;
        }
        free(t);
    }
    if (lines.n) {
        char *t = fz_trim(lines.v[lines.n - 1]);
        if (is_wrapper_line(t)) free(lines.v[--lines.n]);
        free(t);
    }
    strvec_t kept = {0};
    for (size_t i = 0; i < lines.n; i++) {
        char *t = fz_trim(lines.v[i]);
        if (is_diff_content_line(lines.v[i]) || !starts_any(t, METADATA_PREFIXES, ARRAY_LEN(METADATA_PREFIXES)))
            strvec_push(&kept, lines.v[i]);
        free(t);
    }
    char *r = join(kept.v, kept.n, "\n");
    strvec_free(&kept);
    strvec_free(&lines);
    return r;
}

static char *normalize_create_content(const char *content) {
    strvec_t lines = fz_split(content);
    bool any = false, all = true;
    for (size_t i = 0; i < lines.n; i++) {
        if (!*lines.v[i]) continue;
        any = true;
        if (lines.v[i][0] != '+') all = false;
    }
    char *r;
    if (any && all) {
        sb_t b = SB_INIT;
        for (size_t i = 0; i < lines.n; i++) {
            if (i) sb_putc(&b, '\n');
            const char *l = lines.v[i];
            if (str_starts(l, "+ ")) l += 2;
            else if (l[0] == '+') l += 1;
            sb_append(&b, l);
        }
        r = sb_detach(&b);
    } else {
        r = xstrdup(content);
    }
    strvec_free(&lines);
    return r;
}

static bool is_ws_cp(uint32_t c) { return fz_is_js_ws(c) || c == 0x85; }

/* skip Unicode whitespace; returns pointer */
static const char *skip_ws(const char *p) {
    while (*p) {
        uint32_t cp;
        int k = utf8_decode(p, strlen(p), &cp);
        if (!is_ws_cp(cp)) break;
        p += k;
    }
    return p;
}

static bool parse_digits(const char **pp, uint64_t *v) {
    const char *p = *pp;
    if (*p < '0' || *p > '9') return false;
    uint64_t x = 0;
    while (*p >= '0' && *p <= '9') {
        x = x * 10 + (uint64_t)(*p - '0');
        if (x > UINT32_MAX) x = (uint64_t)UINT32_MAX + 1;
        p++;
    }
    *v = x;
    *pp = p;
    return true;
}

/* ^@@\s*-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s*@@(?:\s*(.*))?$ */
static bool parse_unified_header(const char *line, uint64_t *old_start, uint64_t *new_start, char **ctx) {
    if (!str_starts(line, "@@")) return false;
    const char *p = skip_ws(line + 2);
    uint64_t a, b, c, d;
    if (*p++ != '-' || !parse_digits(&p, &a)) return false;
    if (*p == ',') {
        p++;
        if (!parse_digits(&p, &b)) return false;
    }
    const char *q = skip_ws(p);
    if (q == p) return false;
    p = q;
    if (*p++ != '+' || !parse_digits(&p, &c)) return false;
    if (*p == ',') {
        p++;
        if (!parse_digits(&p, &d)) return false;
    }
    p = skip_ws(p);
    if (!str_starts(p, "@@")) return false;
    p += 2;
    if (a > UINT32_MAX || c > UINT32_MAX) return false;
    *old_start = a;
    *new_start = c;
    char *t = fz_trim(skip_ws(p));
    if (*t) *ctx = t;
    else {
        free(t);
        *ctx = NULL;
    }
    return true;
}

/* (?i)^lines?\s+(\d+)(?:\s*-\s*(\d+))?(?:\s*@@)?$ */
static bool parse_line_hint(const char *s, uint64_t *v) {
    if (strncasecmp(s, "line", 4) != 0) return false;
    const char *p = s + 4;
    if (*p == 's' || *p == 'S') p++;
    const char *q = skip_ws(p);
    if (q == p) return false;
    p = q;
    if (!parse_digits(&p, v)) return false;
    const char *save = p;
    p = skip_ws(p);
    if (*p == '-') {
        p = skip_ws(p + 1);
        uint64_t w;
        if (!parse_digits(&p, &w)) p = save;
    } else {
        p = save;
    }
    save = p;
    p = skip_ws(p);
    if (str_starts(p, "@@")) p += 2;
    else p = save;
    return *p == 0;
}

/* (?i)^(top|start|beginning)\s+of\s+file$ */
static bool is_top_of_file(const char *s) {
    const char *p = s;
    if (!strncasecmp(p, "top", 3)) p += 3;
    else if (!strncasecmp(p, "start", 5)) p += 5;
    else if (!strncasecmp(p, "beginning", 9)) p += 9;
    else return false;
    const char *q = skip_ws(p);
    if (q == p || strncasecmp(q, "of", 2)) return false;
    p = q + 2;
    q = skip_ws(p);
    if (q == p || strncasecmp(q, "file", 4)) return false;
    return q[4] == 0;
}

/* ^\s*(\d{1,6})\s+(.+)$ -> *num, *rest (borrowed) */
static bool numbered_line(const char *line, uint64_t *num, const char **rest) {
    const char *p = skip_ws(line);
    const char *d = p;
    while (*p >= '0' && *p <= '9') p++;
    size_t nd = (size_t)(p - d);
    if (nd < 1 || nd > 6) return false;
    const char *w = p;
    /* \s+ then .+ : greedy whitespace, backtracking one char when nothing is left */
    const char *last = NULL, *q = w;
    while (*q) {
        uint32_t cp;
        int k = utf8_decode(q, strlen(q), &cp);
        if (!is_ws_cp(cp)) break;
        last = q;
        q += k;
    }
    if (q == w) return false;
    if (!*q) {
        if (last == w) return false; /* single whitespace char, nothing for .+ */
        *rest = last;
    } else {
        *rest = q;
    }
    *num = 0;
    for (const char *k = d; k < p; k++) *num = *num * 10 + (uint64_t)(*k - '0');
    return true;
}

static void strip_line_number_prefixes(hunk_t *h) {
    strvec_t all = {0};
    for (size_t i = 0; i < h->old_lines.n; i++)
        if (!rs_blank(h->old_lines.v[i])) strvec_push(&all, h->old_lines.v[i]);
    for (size_t i = 0; i < h->new_lines.n; i++)
        if (!rs_blank(h->new_lines.v[i])) strvec_push(&all, h->new_lines.v[i]);
    if (all.n < 2) {
        strvec_free(&all);
        return;
    }
    VEC(uint64_t) nums = {0};
    for (size_t i = 0; i < all.n; i++) {
        uint64_t n;
        const char *r;
        if (numbered_line(all.v[i], &n, &r)) vec_push(&nums, n);
    }
    size_t minimum = MAX((size_t)2, (all.n * 6 + 9) / 10);
    bool go = nums.n >= minimum;
    if (go) {
        size_t seq = 0;
        for (size_t i = 0; i + 1 < nums.n; i++)
            if (nums.v[i + 1] == nums.v[i] + 1) seq++;
        if (nums.n >= 3 && seq < MAX((size_t)1, nums.n - 2)) go = false;
    }
    if (go) {
        strvec_t *sides[2] = {&h->old_lines, &h->new_lines};
        for (int s = 0; s < 2; s++)
            for (size_t i = 0; i < sides[s]->n; i++) {
                uint64_t n;
                const char *r;
                if (numbered_line(sides[s]->v[i], &n, &r)) {
                    char *x = xstrdup(r);
                    free(sides[s]->v[i]);
                    sides[s]->v[i] = x;
                }
            }
    }
    vec_free(&nums);
    strvec_free(&all);
}

static bool parse_one_hunk(char *const *lines, size_t n, uint32_t line_number, bool recover, hunk_t *h,
                           size_t *consumed, perr_t *e) {
    memset(h, 0, sizeof(*h));
    h->old_start = h->new_start = -1;
    if (!n) return FAIL(e, K_PARSE, "Line %u: Diff does not contain any lines", line_number);
    strvec_t ctxs = {0};
    size_t start_index = 0;
    const char *header = lines[0];
    char *ht = fz_trim_end(header);
    bool is_header = str_starts(header, "@@");
    bool empty_marker = false;
    size_t htl = strlen(ht);
    if (htl >= 4 && str_starts(ht, "@@") && str_ends(ht, "@@")) {
        char *mid = xstrndup(ht + 2, htl - 4);
        empty_marker = rs_blank(mid);
        free(mid);
    }
    uint64_t uo, un;
    char *uctx = NULL;
    bool unified = is_header && parse_unified_header(ht, &uo, &un, &uctx);
    bool ok = true;
    if (is_header && (!strcmp(ht, "@@") || empty_marker)) {
        start_index = 1;
        if (unified) free(uctx);
    } else if (unified) {
        if (uo < 1 || un < 1) {
            free(uctx);
            ok = FAIL(e, K_PARSE, "Line %u: Line numbers in @@ header must be >= 1", line_number);
            goto out;
        }
        if (uctx) strvec_push_own(&ctxs, uctx);
        h->old_start = (int64_t)uo;
        h->new_start = (int64_t)un;
        start_index = 1;
    } else if (is_header && str_starts(ht, "@@ ")) {
        const char *cv = ht + 3;
        char *tcv = fz_trim(cv);
        const char *norm = tcv;
        if (str_starts(norm, "@@")) norm = fz_trim_start(norm + 2);
        uint64_t hv;
        if (parse_line_hint(norm, &hv)) {
            if (hv > UINT32_MAX) hv = 0;
            h->old_start = h->new_start = (int64_t)hv;
            if (hv < 1) {
                free(tcv);
                ok = FAIL(e, K_PARSE, "Line %u: Line hint must be >= 1", line_number);
                goto out;
            }
        } else if (is_top_of_file(norm)) {
            h->old_start = h->new_start = 1;
        } else if (*tcv) {
            strvec_push(&ctxs, cv);
        }
        free(tcv);
        start_index = 1;
    } else if (is_header) {
        char *cv = fz_trim(ht + 2);
        if (*cv) strvec_push(&ctxs, cv);
        free(cv);
        start_index = 1;
    } else {
        start_index = 0;
    }
    while (start_index < n) {
        const char *nl = lines[start_index];
        if (!str_starts(nl, "@@")) break;
        char *t = fz_trim_end(nl);
        if (str_starts(t, "@@ ")) {
            if (!rs_blank(t + 3)) strvec_push(&ctxs, t + 3);
            start_index++;
        } else if (!strcmp(t, "@@")) {
            start_index++;
        } else {
            free(t);
            break;
        }
        free(t);
    }
    if (start_index >= n) {
        ok = FAIL(e, K_PARSE, "Line %u: Hunk does not contain any lines", line_number + 1);
        goto out;
    }
    if (ctxs.n) h->change_context = join(ctxs.v, ctxs.n, "\n");
    size_t parsed = 0;
    for (size_t i = start_index; i < n; i++) {
        const char *line = lines[i];
        const char *next = i + 1 < n ? lines[i + 1] : NULL;
        if (!*line && parsed > 0 && next && str_starts(fz_trim_start(next), "@@")) break;
        if (!is_diff_content_line(line) && str_starts(line, "*** End of File")) {
            char *te = fz_trim_end(line);
            bool eofm = !strcmp(te, "*** End of File");
            free(te);
            if (eofm) {
                if (!parsed) {
                    ok = FAIL(e, K_PARSE, "Line %u: Hunk does not contain any lines", line_number + 1);
                    goto out;
                }
                h->eof = true;
                parsed++;
                break;
            }
        }
        char *tr = fz_trim(line);
        bool ell = !strcmp(tr, "...") || !strcmp(tr, "…");
        free(tr);
        if (ell) {
            h->has_context_lines = true;
            parsed++;
            continue;
        }
        if (!*line) {
            h->has_context_lines = true;
            strvec_push(&h->old_lines, "");
            strvec_push(&h->new_lines, "");
        } else if (line[0] == ' ') {
            h->has_context_lines = true;
            strvec_push(&h->old_lines, line + 1);
            strvec_push(&h->new_lines, line + 1);
        } else if (line[0] == '+') {
            strvec_push(&h->new_lines, line + 1);
        } else if (line[0] == '-') {
            strvec_push(&h->old_lines, line + 1);
        } else if (!str_starts(line, "@@")) {
            h->has_context_lines = true;
            strvec_push(&h->old_lines, line);
            strvec_push(&h->new_lines, line);
        } else if (!parsed) {
            ok = FAIL(e, K_PARSE,
                      "Line %u: Unexpected line in hunk: '%s'. Lines must start with ' ' (context), '+' (add), or '-' "
                      "(remove)",
                      line_number + 1, line);
            goto out;
        } else {
            break;
        }
        parsed++;
    }
    if (!parsed) {
        ok = FAIL(e, K_PARSE, "Line %u: Hunk does not contain any lines", line_number + (uint32_t)start_index);
        goto out;
    }
    if (recover) strip_line_number_prefixes(h);
    *consumed = parsed + start_index;
out:
    free(ht);
    strvec_free(&ctxs);
    if (!ok) {
        free(h->change_context);
        h->change_context = NULL;
        strvec_free(&h->old_lines);
        strvec_free(&h->new_lines);
    }
    return ok;
}

static char *marker_path(const char *line) {
    if (str_starts(line, "diff --git ")) {
        strvec_t parts = {0};
        const char *p = line + 11;
        while (*p) {
            p = skip_ws(p);
            if (!*p) break;
            const char *s = p;
            while (*p) {
                uint32_t cp;
                int k = utf8_decode(p, strlen(p), &cp);
                if (is_ws_cp(cp)) break;
                p += k;
            }
            strvec_push_own(&parts, xstrndup(s, (size_t)(p - s)));
        }
        char *r = NULL;
        if (parts.n) {
            const char *c = parts.n > 1 ? parts.v[1] : parts.v[0];
            if (str_starts(c, "a/") || str_starts(c, "b/")) c += 2;
            r = xstrdup(c);
        }
        strvec_free(&parts);
        return r;
    }
    for (int i = 0; i < 3; i++)
        if (str_starts(line, MULTI_FILE_MARKERS[i])) return fz_trim(line + strlen(MULTI_FILE_MARKERS[i]));
    return NULL;
}

static size_t count_multi_file_markers(const char *diff) {
    strvec_t lines = fz_split(diff), paths = {0};
    size_t counts[4] = {0};
    for (size_t i = 0; i < lines.n; i++) {
        if (is_diff_content_line(lines.v[i])) continue;
        char *t = fz_trim(lines.v[i]);
        for (int m = 0; m < 4; m++) {
            if (!str_starts(t, MULTI_FILE_MARKERS[m])) continue;
            char *p = marker_path(t);
            if (p && *p && !strvec_contains(&paths, p)) strvec_push(&paths, p);
            free(p);
            counts[m]++;
            break;
        }
        free(t);
    }
    size_t r = 0;
    if (!paths.n)
        for (int m = 0; m < 4; m++) r = MAX(r, counts[m]);
    else r = paths.n;
    strvec_free(&paths);
    strvec_free(&lines);
    return r;
}

static bool parse_hunks(const char *diff, bool recover, hunks_t *out, perr_t *e) {
    memset(out, 0, sizeof(*out));
    size_t mf = count_multi_file_markers(diff);
    if (mf > 1)
        return FAIL(e, K_APPLY, "Diff contains %zu file markers. Single-file patches cannot contain multi-file markers.",
                    mf);
    char *norm = normalize_diff(diff);
    strvec_t lines = fz_split(norm);
    free(norm);
    size_t i = 0;
    bool ok = true;
    while (i < lines.n) {
        const char *line = lines.v[i];
        char *t = fz_trim(line);
        if (!*t) {
            free(t);
            i++;
            continue;
        }
        bool content = line[0] == ' ' || line[0] == '+' || line[0] == '-';
        bool meta = false;
        for (size_t m = 0; m < ARRAY_LEN(METADATA_PREFIXES); m++)
            if (!str_starts(METADATA_PREFIXES[m], "*** ") && str_starts(t, METADATA_PREFIXES[m])) meta = true;
        if (!content && meta) {
            free(t);
            i++;
            continue;
        }
        if (str_starts(t, "@@")) {
            bool rest_blank = true;
            for (size_t k = i + 1; k < lines.n && rest_blank; k++) rest_blank = rs_blank(lines.v[k]);
            if (rest_blank) {
                free(t);
                break;
            }
        }
        free(t);
        hunk_t h;
        size_t consumed = 0;
        if (!parse_one_hunk(lines.v + i, lines.n - i, (uint32_t)(i + 1), recover, &h, &consumed, e)) {
            ok = false;
            break;
        }
        vec_push(out, h);
        i += consumed;
    }
    strvec_free(&lines);
    if (!ok) hunks_free(out);
    return ok;
}

/* ======================================================================================
 * hunk placement (modes/patch.rs)
 * ====================================================================================== */

typedef struct {
    size_t start, old_len;
    strvec_t new_lines;
} repl_t;
typedef VEC(repl_t) repls_t;

static void repls_free(repls_t *r) {
    for (size_t i = 0; i < r->n; i++) strvec_free(&r->v[i].new_lines);
    vec_free(r);
}

static int cmp_repl(const void *a, const void *b) {
    const repl_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static bool has_char(const char *s, char c, size_t n) { return memchr(s, c, n) != NULL; }

static char *apply_delta_line(const char *line, long delta, char ch) {
    if (fz_blank(line)) return xstrdup(line);
    if (delta > 0) {
        sb_t b = SB_INIT;
        for (long k = 0; k < delta; k++) sb_putc(&b, ch);
        sb_append(&b, line);
        return sb_detach(&b);
    }
    size_t rm = MIN((size_t)(-delta), fz_leading_ws(line));
    return xstrdup(line + rm);
}

static strvec_t adjust_lines_indentation(const strvec_t *pat, const strvec_t *act, const strvec_t *nl) {
    if (!pat->n || !act->n || !nl->n || vec_eq(pat, act)) return vec_copy(nl->v, nl->n);
    if (pat->n == nl->n) {
        bool eq = true;
        for (size_t i = 0; i < pat->n && eq; i++) eq = fz_trim_eq(pat->v[i], nl->v[i]);
        if (eq) return vec_copy(nl->v, nl->n);
    }
    bool p_tab = true, a_space = true, p_space = true, a_tab = true, p_mixed = false, a_mixed = false;
    for (size_t i = 0; i < pat->n; i++) {
        const char *l = pat->v[i];
        size_t w = fz_leading_ws(l);
        bool sp = has_char(l, ' ', w), tb = has_char(l, '\t', w);
        if (sp && tb) p_mixed = true;
        if (fz_blank(l)) continue;
        if (sp) p_tab = false;
        if (tb) p_space = false;
    }
    for (size_t i = 0; i < act->n; i++) {
        const char *l = act->v[i];
        size_t w = fz_leading_ws(l);
        bool sp = has_char(l, ' ', w), tb = has_char(l, '\t', w);
        if (sp && tb) a_mixed = true;
        if (fz_blank(l)) continue;
        if (tb) a_space = false;
        if (sp) a_tab = false;
    }
    size_t pairs = MIN(pat->n, act->n);
    if (!p_mixed && !a_mixed && p_tab && a_space) {
        size_t ratio = 0;
        bool have = false, consistent = true;
        for (size_t i = 0; i < pairs; i++) {
            const char *o = pat->v[i], *f = act->v[i];
            if (fz_blank(o) || fz_blank(f)) continue;
            size_t oi = fz_leading_ws(o), fi = fz_leading_ws(f);
            if (!oi) continue;
            if (fi % oi) {
                consistent = false;
                break;
            }
            size_t nx = fi / oi;
            if (have && ratio != nx) {
                consistent = false;
                break;
            }
            have = true;
            ratio = nx;
        }
        if (consistent && have && ratio > 0) {
            bool valid = true;
            for (size_t i = 0; i < pairs && valid; i++) {
                const char *o = pat->v[i], *f = act->v[i];
                valid = fz_blank(o) || fz_blank(f) || !fz_leading_ws(o) || fz_leading_ws(f) == fz_leading_ws(o) * ratio;
            }
            if (valid) {
                char *j = join(nl->v, nl->n, "\n");
                char *c = fz_convert_leading_tabs(j, ratio);
                strvec_t r = fz_split(c);
                free(j);
                free(c);
                return r;
            }
        }
    }
    if (!p_mixed && !a_mixed && p_space && a_tab) {
        /* samples: tabs -> spaces (BTreeMap) */
        VEC(size_t) tabs = {0};
        VEC(size_t) spaces = {0};
        bool consistent = true;
        for (size_t i = 0; i < pairs && consistent; i++) {
            const char *o = pat->v[i], *f = act->v[i];
            if (fz_blank(o) || fz_blank(f)) continue;
            size_t sp = fz_leading_ws(o), tb = fz_leading_ws(f);
            if (!tb) continue;
            bool found = false;
            for (size_t k = 0; k < tabs.n; k++)
                if (tabs.v[k] == tb) {
                    found = true;
                    if (spaces.v[k] != sp) consistent = false;
                    spaces.v[k] = sp;
                }
            if (!found) {
                size_t at = tabs.n;
                while (at > 0 && tabs.v[at - 1] > tb) at--;
                vec_push(&tabs, tb);
                vec_push(&spaces, sp);
                memmove(tabs.v + at + 1, tabs.v + at, (tabs.n - 1 - at) * sizeof(size_t));
                memmove(spaces.v + at + 1, spaces.v + at, (spaces.n - 1 - at) * sizeof(size_t));
                tabs.v[at] = tb;
                spaces.v[at] = sp;
            }
        }
        long width = -1, offset = 0;
        if (consistent && tabs.n) {
            if (tabs.n == 1) {
                size_t t = tabs.v[0], s = spaces.v[0];
                if (t && s % t == 0) width = (long)(s / t);
            } else {
                long td = (long)tabs.v[1] - (long)tabs.v[0], sd = (long)spaces.v[1] - (long)spaces.v[0];
                if (td != 0 && sd > 0 && sd % td == 0) {
                    long w = sd / td, off = (long)spaces.v[0] - (long)tabs.v[0] * w;
                    bool valid = w > 0;
                    for (size_t k = 0; k < tabs.n && valid; k++) valid = (long)tabs.v[k] * w + off == (long)spaces.v[k];
                    if (valid) {
                        width = w;
                        offset = off;
                    }
                }
            }
        }
        vec_free(&tabs);
        vec_free(&spaces);
        if (width > 0) {
            strvec_t r = {0};
            for (size_t i = 0; i < nl->n; i++) {
                const char *l = nl->v[i];
                size_t sp = fz_leading_ws(l);
                long adj = (long)sp - offset;
                if (fz_blank(l) || !sp || adj < 0) {
                    strvec_push(&r, l);
                    continue;
                }
                size_t t = (size_t)adj / (size_t)width, rem = (size_t)adj - t * (size_t)width;
                sb_t b = SB_INIT;
                for (size_t k = 0; k < t; k++) sb_putc(&b, '\t');
                for (size_t k = 0; k < rem; k++) sb_putc(&b, ' ');
                sb_append(&b, l + sp);
                strvec_push_own(&r, sb_detach(&b));
            }
            return r;
        }
    }
    /* uniform delta / content echo */
    bool have_delta = false, uniform = true;
    long delta = 0;
    for (size_t i = 0; i < pairs; i++) {
        if (fz_blank(pat->v[i]) || fz_blank(act->v[i])) continue;
        long d = (long)fz_leading_ws(act->v[i]) - (long)fz_leading_ws(pat->v[i]);
        if (!have_delta) {
            have_delta = true;
            delta = d;
        } else if (d != delta) {
            uniform = false;
        }
    }
    bool use_delta = have_delta && uniform;
    size_t pmin = SIZE_MAX;
    for (size_t i = 0; i < pat->n; i++)
        if (!fz_blank(pat->v[i])) pmin = MIN(pmin, fz_leading_ws(pat->v[i]));
    if (pmin == SIZE_MAX) pmin = 0;
    char ich = ' ';
    for (size_t i = 0; i < act->n; i++)
        if (fz_leading_ws(act->v[i])) {
            ich = act->v[i][0];
            break;
        }
    /* by_content: trimmed text -> actual lines (non-blank), in order */
    strvec_t keys = {0};
    VEC(strvec_t) vals = {0};
    for (size_t i = 0; i < act->n; i++) {
        if (fz_blank(act->v[i])) continue;
        char *k = fz_trim(act->v[i]);
        size_t at = keys.n;
        for (size_t q = 0; q < keys.n; q++)
            if (!strcmp(keys.v[q], k)) at = q;
        if (at == keys.n) {
            strvec_push(&keys, k);
            strvec_t e = {0};
            vec_push(&vals, e);
        }
        strvec_push(&vals.v[at], act->v[i]);
        free(k);
    }
    size_t *used = xcalloc(keys.n + 1, sizeof(size_t));
    strvec_t r = {0};
    for (size_t li = 0; li < nl->n; li++) {
        const char *line = nl->v[li];
        if (fz_blank(line)) {
            strvec_push(&r, line);
            continue;
        }
        char *t = fz_trim(line);
        size_t at = keys.n;
        for (size_t q = 0; q < keys.n; q++)
            if (!strcmp(keys.v[q], t)) at = q;
        free(t);
        if (at < keys.n) {
            strvec_t *m = &vals.v[at];
            if (m->n == 1) {
                strvec_push(&r, m->v[0]);
                continue;
            }
            if (strvec_contains(m, line)) {
                strvec_push(&r, line);
                continue;
            }
            if (used[at] < m->n) {
                strvec_push(&r, m->v[used[at]++]);
                continue;
            }
        }
        if (pat->n == nl->n && li < pat->n && li < act->n && !fz_blank(pat->v[li]) && !fz_blank(act->v[li])) {
            long ld = (long)fz_leading_ws(act->v[li]) - (long)fz_leading_ws(pat->v[li]);
            if (ld != 0 && fz_leading_ws(line) == fz_leading_ws(pat->v[li])) {
                strvec_push_own(&r, apply_delta_line(line, ld, ich));
                continue;
            }
        }
        if (use_delta && delta != 0 && fz_leading_ws(line) == pmin) {
            strvec_push_own(&r, apply_delta_line(line, delta, ich));
            continue;
        }
        strvec_push(&r, line);
    }
    free(used);
    for (size_t i = 0; i < vals.n; i++) strvec_free(&vals.v[i]);
    vec_free(&vals);
    strvec_free(&keys);
    return r;
}

/* ---- fallback variants ---- */

enum { V_TRIM_COMMON, V_DEDUPE_SHARED, V_COLLAPSE_REPEATED, V_SINGLE_LINE };

typedef struct {
    strvec_t old_lines, new_lines;
    int kind;
} variant_t;
typedef VEC(variant_t) variants_t;

static void variants_free(variants_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        strvec_free(&v->v[i].old_lines);
        strvec_free(&v->v[i].new_lines);
    }
    vec_free(v);
}

static bool trim_common(const strvec_t *o, const strvec_t *n, variant_t *out) {
    size_t s = 0, oe = o->n, ne = n->n;
    while (s < oe && s < ne && !strcmp(o->v[s], n->v[s])) s++;
    while (oe > s && ne > s && !strcmp(o->v[oe - 1], n->v[ne - 1])) {
        oe--;
        ne--;
    }
    if (s == 0 && oe == o->n && ne == n->n) return false;
    if (oe == s && ne == s) return false;
    out->old_lines = vec_copy(o->v + s, oe - s);
    out->new_lines = vec_copy(n->v + s, ne - s);
    out->kind = V_TRIM_COMMON;
    return true;
}

static strvec_t shared_lines(const strvec_t *o, const strvec_t *n) {
    strvec_t sh = {0};
    for (size_t i = 0; i < o->n; i++)
        if (strvec_contains(n, o->v[i]) && !strvec_contains(&sh, o->v[i])) strvec_push(&sh, o->v[i]);
    return sh;
}

static strvec_t collapse_consec(const strvec_t *l, const strvec_t *shared) {
    strvec_t r = {0};
    for (size_t i = 0; i < l->n; i++) {
        if (i > 0 && !strcmp(l->v[i], l->v[i - 1]) && strvec_contains(shared, l->v[i - 1])) continue;
        strvec_push(&r, l->v[i]);
    }
    return r;
}

static bool dedupe_shared(const strvec_t *o, const strvec_t *n, variant_t *out) {
    strvec_t sh = shared_lines(o, n);
    strvec_t a = collapse_consec(o, &sh), b = collapse_consec(n, &sh);
    strvec_free(&sh);
    if (a.n == o->n && b.n == n->n) {
        strvec_free(&a);
        strvec_free(&b);
        return false;
    }
    out->old_lines = a;
    out->new_lines = b;
    out->kind = V_DEDUPE_SHARED;
    return true;
}

static bool collapse_blocks(const strvec_t *in, const strvec_t *shared, strvec_t *out) {
    *out = vec_copy(in->v, in->n);
    bool changed = false;
    size_t idx = 0;
    while (idx < out->n) {
        bool collapsed = false;
        if (strvec_contains(shared, out->v[idx])) {
            for (size_t size = (out->n - idx) / 2; size >= 2; size--) {
                bool all = true;
                for (size_t off = 0; off < size && all; off++)
                    all = !strcmp(out->v[idx + off], out->v[idx + size + off]) && strvec_contains(shared, out->v[idx + off]);
                if (all) {
                    for (size_t k = idx + size; k < idx + size * 2; k++) free(out->v[k]);
                    memmove(out->v + idx + size, out->v + idx + size * 2, (out->n - idx - size * 2) * sizeof(char *));
                    out->n -= size;
                    changed = collapsed = true;
                    break;
                }
            }
        }
        if (!collapsed) idx++;
    }
    return changed;
}

static bool collapse_repeated(const strvec_t *o, const strvec_t *n, variant_t *out) {
    strvec_t sh = shared_lines(o, n);
    strvec_t a, b;
    bool ca = collapse_blocks(o, &sh, &a), cb = collapse_blocks(n, &sh, &b);
    strvec_free(&sh);
    if (!ca && !cb) {
        strvec_free(&a);
        strvec_free(&b);
        return false;
    }
    out->old_lines = a;
    out->new_lines = b;
    out->kind = V_COLLAPSE_REPEATED;
    return true;
}

static bool single_line(const strvec_t *o, const strvec_t *n, variant_t *out) {
    if (!o->n || o->n != n->n) return false;
    size_t changed = 0, at = 0;
    for (size_t i = 0; i < o->n; i++)
        if (strcmp(o->v[i], n->v[i]) != 0) {
            changed++;
            at = i;
        }
    if (changed != 1) return false;
    out->old_lines = vec_copy(&o->v[at], 1);
    out->new_lines = vec_copy(&n->v[at], 1);
    out->kind = V_SINGLE_LINE;
    return true;
}

static variants_t fallback_variants(const hunk_t *h, bool aggressive) {
    variants_t all = {0};
    variant_t t, d, c, s;
    bool ht = trim_common(&h->old_lines, &h->new_lines, &t);
    if (ht) vec_push(&all, t);
    const strvec_t *bo = ht ? &all.v[0].old_lines : &h->old_lines, *bn = ht ? &all.v[0].new_lines : &h->new_lines;
    strvec_t bo_c = vec_copy(bo->v, bo->n), bn_c = vec_copy(bn->v, bn->n);
    bool hd = dedupe_shared(&bo_c, &bn_c, &d);
    if (hd) vec_push(&all, d);
    const strvec_t *co = hd ? &all.v[all.n - 1].old_lines : &bo_c, *cn = hd ? &all.v[all.n - 1].new_lines : &bn_c;
    strvec_t co_c = vec_copy(co->v, co->n), cn_c = vec_copy(cn->v, cn->n);
    if (collapse_repeated(&co_c, &cn_c, &c)) vec_push(&all, c);
    if (single_line(&bo_c, &bn_c, &s)) vec_push(&all, s);
    strvec_free(&bo_c);
    strvec_free(&bn_c);
    strvec_free(&co_c);
    strvec_free(&cn_c);
    variants_t out = {0};
    strvec_t seen = {0};
    for (size_t i = 0; i < all.n; i++) {
        variant_t *v = &all.v[i];
        bool keep = aggressive || (v->kind != V_COLLAPSE_REPEATED && v->kind != V_SINGLE_LINE);
        char *jo = join(v->old_lines.v, v->old_lines.n, "\n"), *jn = join(v->new_lines.v, v->new_lines.n, "\n");
        char *key = xasprintf("%s||%s", jo, jn);
        free(jo);
        free(jn);
        if (keep && !strvec_contains(&seen, key)) {
            strvec_push(&seen, key);
            vec_push(&out, *v);
        } else {
            strvec_free(&v->old_lines);
            strvec_free(&v->new_lines);
        }
        free(key);
    }
    strvec_free(&seen);
    vec_free(&all);
    return out;
}

/* ---- previews ---- */

static void sequence_preview(sb_t *b, char *const *lines, size_t n, size_t index) {
    size_t start = index >= 2 ? index - 2 : 0, end = MIN(n, index + 3);
    for (size_t i = start; i < end; i++) {
        if (i > start) sb_putc(b, '\n');
        const char *l = lines[i];
        sb_appendf(b, "  %zu | ", i + 1);
        if (fz_char_count(l) > 80) {
            size_t cnt = 0;
            const char *p = l;
            while (*p && cnt < 79) {
                p++;
                while (((unsigned char)*p & 0xc0) == 0x80) p++;
                cnt++;
            }
            sb_appendn(b, l, (size_t)(p - l));
            sb_append(b, "…");
        } else {
            sb_append(b, l);
        }
    }
}

static char *sequence_previews(char *const *lines, size_t n, const fz_seq_t *r) {
    if (!r->has_indices || !r->nindices) return NULL;
    sb_t b = SB_INIT;
    for (size_t i = 0; i < r->nindices; i++) {
        if (i) sb_append(&b, "\n\n");
        sequence_preview(&b, lines, n, r->indices[i]);
    }
    if (r->match_count > (int64_t)r->nindices)
        sb_appendf(&b, " (showing first %zu of %lld)", r->nindices, (long long)r->match_count);
    return sb_detach(&b);
}

/* ---- sequence search helpers ---- */

static fz_seq_t seq_hint(char *const *L, size_t n, char *const *P, size_t m, size_t current, int64_t hint, bool eof,
                         bool fz) {
    fz_seq_t primary = fz_seek_sequence(L, n, P, m, current, eof, fz);
    if (primary.match_count > 1 && hint >= 0 && (size_t)hint != current) {
        fz_seq_t h = fz_seek_sequence(L, n, P, m, (size_t)hint, eof, fz);
        if (h.index >= 0 || h.match_count > 1) return h;
    }
    if (primary.index >= 0 || primary.match_count > 1) return primary;
    if (hint >= 0 && (size_t)hint != current) {
        fz_seq_t h = fz_seek_sequence(L, n, P, m, (size_t)hint, eof, fz);
        if (h.index >= 0 || h.match_count > 1) return h;
    }
    if (current != 0) {
        fz_seq_t s = fz_seek_sequence(L, n, P, m, 0, eof, fz);
        if (s.index >= 0 || s.match_count > 1) return s;
    }
    return primary;
}

static int64_t mc_or(int64_t mc, int64_t def) { return mc < 0 ? def : mc; }

typedef struct {
    bool built;
    variants_t v;
} lazyvar_t;

static variants_t *lazy_variants(lazyvar_t *l, const hunk_t *h, bool aggressive) {
    if (!l->built) {
        l->v = fallback_variants(h, aggressive);
        l->built = true;
    }
    return &l->v;
}

static int64_t attempt_sequence_fallback(char *const *L, size_t n, const hunk_t *h, size_t current, int64_t hint,
                                         bool fz, bool aggressive, lazyvar_t *lv) {
    if (!h->old_lines.n) return -1;
    char *const *P = h->old_lines.v;
    size_t m = h->old_lines.n;
    fz_seq_t r = seq_hint(L, n, P, m, current, hint, false, fz);
    if (r.index >= 0 && mc_or(r.match_count, 1) <= 1) {
        size_t next = (size_t)r.index + 1;
        if (next <= (n > m ? n - m : 0) && fz_seek_sequence(L, n, P, m, next, false, fz).index >= 0) return -1;
        return r.index;
    }
    variants_t *vs = lazy_variants(lv, h, aggressive);
    for (size_t i = 0; i < vs->n; i++) {
        variant_t *v = &vs->v[i];
        if (!v->old_lines.n) continue;
        fz_seq_t x = seq_hint(L, n, v->old_lines.v, v->old_lines.n, current, hint, false, fz);
        if (mc_or(x.match_count, 1) <= 1 && x.index >= 0) return x.index;
    }
    return -1;
}

static fz_seq_t ctx_none(void) {
    fz_seq_t r;
    memset(&r, 0, sizeof(r));
    r.index = -1;
    r.match_count = -1;
    r.strategy = -1;
    return r;
}

static void single_index(fz_seq_t *r, int64_t idx) {
    r->match_count = 1;
    r->has_indices = true;
    r->nindices = 1;
    r->indices[0] = (size_t)idx;
}

static bool space_hierarchy(char *const *L, size_t n, const strvec_t *parts, size_t start, bool fz, fz_seq_t *out) {
    if (parts->n < 2) return false;
    char *outer = join(parts->v, parts->n - 1, " ");
    const char *inner = parts->v[parts->n - 1];
    fz_seq_t o = fz_find_context_line(L, n, outer, start, fz, false);
    free(outer);
    if (o.match_count > 1) {
        *out = o;
        out->index = -1;
        return true;
    }
    if (o.index < 0) return false;
    fz_seq_t in = fz_find_context_line(L, n, inner, (size_t)o.index + 1, fz, false);
    if (in.index >= 0) {
        *out = in;
        if (in.match_count > 1) single_index(out, in.index);
        return true;
    }
    if (in.match_count > 1) {
        *out = in;
        out->match_count = 1;
        return true;
    }
    *out = in;
    return true;
}

static strvec_t split_ws(const char *s) {
    strvec_t parts = {0};
    const char *p = s;
    while (*p) {
        p = skip_ws(p);
        if (!*p) break;
        const char *st = p;
        while (*p) {
            uint32_t cp;
            int k = utf8_decode(p, strlen(p), &cp);
            if (is_ws_cp(cp)) break;
            p += k;
        }
        strvec_push_own(&parts, xstrndup(st, (size_t)(p - st)));
    }
    return parts;
}

static fz_seq_t find_hierarchical_context(char *const *L, size_t n, const char *context, size_t start, int64_t hint,
                                          bool fz) {
    strvec_t raw = fz_split(context), parts = {0};
    for (size_t i = 0; i < raw.n; i++) {
        char *t = fz_trim(raw.v[i]);
        if (*t) strvec_push(&parts, t);
        free(t);
    }
    strvec_free(&raw);
    fz_seq_t res = ctx_none();
    if (parts.n > 1) {
        size_t current = start;
        for (size_t i = 0; i < parts.n; i++) {
            bool last = i + 1 == parts.n;
            fz_seq_t r = fz_find_context_line(L, n, parts.v[i], current, fz, false);
            if (last && (r.index < 0 || r.match_count > 1) && hint >= 0) {
                size_t hs = hint > 0 ? (size_t)hint - 1 : 0;
                if (hs >= current) {
                    fz_seq_t h = fz_find_context_line(L, n, parts.v[i], hs, fz, false);
                    if (h.index >= 0) {
                        res = h;
                        single_index(&res, h.index);
                        goto done;
                    }
                }
            }
            if (r.index < 0) {
                res = r;
                goto done;
            }
            if (r.match_count > 1) {
                res = r;
                res.index = -1;
                goto done;
            }
            if (last) {
                res = r;
                goto done;
            }
            current = (size_t)r.index + 1;
        }
    }
    {
        strvec_t sp = split_ws(context);
        bool sig = strpbrk(context, "()[]{}") != NULL;
        fz_seq_t h;
        if (!sig && sp.n > 2 && space_hierarchy(L, n, &sp, start, fz, &h) && (h.index >= 0 || h.match_count > 1)) {
            res = h;
            strvec_free(&sp);
            goto done;
        }
        fz_seq_t r = fz_find_context_line(L, n, context, start, fz, false);
        if ((r.index < 0 || r.match_count > 1) && hint >= 0) {
            fz_seq_t hh = fz_find_context_line(L, n, context, hint > 0 ? (size_t)hint - 1 : 0, fz, false);
            if (hh.index >= 0) {
                res = hh;
                single_index(&res, hh.index);
                strvec_free(&sp);
                goto done;
            }
        }
        if ((r.index >= 0 && mc_or(r.match_count, 0) <= 1) || r.match_count > 1) {
            res = r;
            strvec_free(&sp);
            goto done;
        }
        if (r.index < 0 && start != 0) {
            fz_seq_t f = fz_find_context_line(L, n, context, 0, fz, false);
            if ((f.index >= 0 && mc_or(f.match_count, 0) <= 1) || f.match_count > 1) {
                res = f;
                strvec_free(&sp);
                goto done;
            }
        }
        if (!sig && sp.n > 1 && space_hierarchy(L, n, &sp, start, fz, &h)) r = h;
        res = r;
        strvec_free(&sp);
    }
done:
    strvec_free(&parts);
    return res;
}

static int64_t find_context_relative(char *const *L, size_t n, const char *pattern, size_t ctx, bool prefer_second) {
    char *p = fz_trim(pattern);
    int64_t first = -1, second = -1, r = -1;
    for (size_t i = ctx + 1; i < n; i++) {
        if (!fz_trim_eq(L[i], p)) continue;
        if (first < 0) first = (int64_t)i;
        else if (second < 0) second = (int64_t)i;
    }
    if (first >= 0) r = prefer_second && second >= 0 ? second : first;
    else
        for (size_t i = ctx; i > 0; i--)
            if (fz_trim_eq(L[i - 1], p)) {
                r = (int64_t)i - 1;
                break;
            }
    free(p);
    return r;
}

/* single context-free hunk placed at the character level */
static bool character_match(const char *content, const char *path, const hunk_t *h, double threshold, bool fz,
                            char **out, strvec_t *warnings, perr_t *e) {
    char *old_text = join(h->old_lines.v, h->old_lines.n, "\n"), *new_text = join(h->new_lines.v, h->new_lines.n, "\n");
    fz_outcome_t o;
    fz_find_match(content, old_text, fz, threshold, NULL, 0, &o);
    if (!o.matched && fz) {
        double relaxed = MIN(threshold, 0.92);
        if (relaxed < threshold) {
            fz_outcome_t nx;
            fz_find_match(content, old_text, fz, relaxed, NULL, 0, &nx);
            if (nx.matched) {
                fz_outcome_free(&o);
                o = nx;
            } else {
                fz_outcome_free(&nx);
            }
        }
    }
    bool ok = true;
    if (o.occurrences > 1) {
        char *prev = strvec_join(&o.occurrence_previews, "\n\n");
        char *more = o.occurrences > 5 ? xasprintf(" (showing first 5 of %lld)", (long long)o.occurrences) : xstrdup("");
        ok = FAIL(e, K_APPLY, "Found %lld occurrences in %s%s:\n\n%s\n\nAdd more context lines to disambiguate.",
                  (long long)o.occurrences, path, more, prev);
        free(prev);
        free(more);
    } else if (o.fuzzy_matches > 1) {
        ok = FAIL(e, K_APPLY,
                  "Found %lld high-confidence matches in %s. The text must be unique. Please provide more context to "
                  "make it unique.",
                  (long long)o.fuzzy_matches, path);
    } else if (!o.matched) {
        if (o.closest)
            ok = FAIL(e, K_APPLY, "Could not find a close enough match in %s. Closest match (%.0f%% similar) at line %u.",
                      path, o.closest->confidence * 100.0, o.closest->start_line);
        else ok = FAIL(e, K_APPLY, "Failed to find expected lines in %s:\n%s", path, old_text);
    } else {
        char *adj = fz_adjust_indentation(old_text, o.matched->actual_text, new_text);
        if (o.dominant_fuzzy)
            strvec_push_own(warnings, xasprintf("Dominant fuzzy match selected in %s near line %u (%.0f%% similar).", path,
                                                o.matched->start_line, o.matched->confidence * 100.0));
        sb_t b = SB_INIT;
        sb_appendn(&b, content, o.matched->start_index);
        sb_append(&b, adj);
        sb_append(&b, content + o.matched->start_index + strlen(o.matched->actual_text));
        free(adj);
        *out = sb_detach(&b);
    }
    fz_outcome_free(&o);
    free(old_text);
    free(new_text);
    return ok;
}

static bool assert_partial_match(const char *path, const strvec_t *pattern, char *const *matched, size_t nm,
                                 const strvec_t *new_lines, size_t start, perr_t *e) {
    strvec_t nn = {0};
    for (size_t i = 0; i < new_lines->n; i++) strvec_push_own(&nn, fz_normalize_for_fuzzy(new_lines->v[i]));
    char *new_norm = join(nn.v, nn.n, "\n");
    strvec_free(&nn);
    bool ok = true;
    for (size_t off = 0; off < pattern->n && off < nm && ok; off++) {
        char *a = fz_normalize_for_fuzzy(matched[off]), *x = fz_normalize_for_fuzzy(pattern->v[off]);
        if (strcmp(a, x) != 0) {
            const char *at = strstr(a, x);
            if (at) {
                char *pieces[2] = {xstrndup(a, (size_t)(at - a)), xstrdup(at + strlen(x))};
                for (int k = 0; k < 2 && ok; k++) {
                    char *d = fz_trim(pieces[k]);
                    if (*d && !strstr(new_norm, d)) {
                        char *dq = dbg_str(d);
                        ok = FAIL(e, K_APPLY,
                                  "Refusing partial-line match in %s at line %zu: the file line also contains %s, which "
                                  "the replacement would silently drop. Provide the complete line in the hunk.",
                                  path, start + off + 1, dq);
                        free(dq);
                    }
                    free(d);
                }
                free(pieces[0]);
                free(pieces[1]);
            }
        }
        free(a);
        free(x);
    }
    free(new_norm);
    return ok;
}

static void range_str(sb_t *b, const repl_t *r) {
    if (!r->old_len) sb_appendf(b, "%zu (insertion)", r->start + 1);
    else sb_appendf(b, "%zu-%zu", r->start + 1, r->start + r->old_len);
}

static bool compute_replacements(const strvec_t *lines_v, const char *path, const hunks_t *hunks, bool fz,
                                 repls_t *out, strvec_t *warnings, perr_t *e) {
    char *const *L = lines_v->v;
    size_t n = lines_v->n;
    size_t line_index = 0;
    bool ok = true;
    for (size_t hi = 0; hi < hunks->n && ok; hi++) {
        const hunk_t *h = &hunks->v[hi];
        if (h->old_start == 0 || h->new_start == 0)
            return FAIL(e, K_APPLY, "Line hint 0 is out of range for %s (line numbers start at 1)", path);
        int64_t line_hint = h->old_start;
        bool aggressive = h->change_context || line_hint >= 0 || h->eof;
        lazyvar_t lv = {0};
        if (line_hint >= 0 && !h->change_context && !h->has_context_lines)
            line_index = MIN((size_t)(line_hint - 1), n > 0 ? n - 1 : 0);
        int64_t context_index = -1;
        if (h->change_context) {
            fz_seq_t r = find_hierarchical_context(L, n, h->change_context, line_index, line_hint, fz);
            context_index = r.index;
            if (r.index >= 0 && r.match_count <= 1) {
                char *first = h->old_lines.n ? fz_trim(h->old_lines.v[0]) : NULL;
                const char *lastnl = strrchr(h->change_context, '\n');
                char *fin = fz_trim(lastnl ? lastnl + 1 : h->change_context);
                strvec_t sp = split_ws(h->change_context);
                bool hier = strchr(h->change_context, '\n') || sp.n > 2;
                strvec_free(&sp);
                bool same = first && !strcmp(first, fin);
                line_index = (same || hier) ? (size_t)r.index : (size_t)r.index + 1;
                free(first);
                free(fin);
            } else {
                int64_t fb = attempt_sequence_fallback(L, n, h, line_index, line_hint >= 0 ? line_hint - 1 : -1, fz,
                                                       aggressive, &lv);
                if (fb >= 0) {
                    line_index = (size_t)fb;
                } else if (r.match_count > 1) {
                    const char *lastnl = strrchr(h->change_context, '\n');
                    const char *disp = lastnl ? lastnl + 1 : h->change_context;
                    char *strat = r.strategy >= 0 ? xasprintf(" Matching strategy: %s.", fz_ctx_label(r.strategy)) : xstrdup("");
                    char *pv = sequence_previews(L, n, &r);
                    char *prev = pv ? xasprintf("\n\n%s", pv) : xstrdup("");
                    ok = FAIL(e, K_APPLY,
                              "Found %lld matches for context '%s' in %s.%s%s\n\nAdd more surrounding context or "
                              "additional @@ anchors to make it unique.",
                              (long long)r.match_count, disp, path, strat, prev);
                    free(strat);
                    free(pv);
                    free(prev);
                } else {
                    char *c = str_replace(h->change_context, "\n", " > ");
                    ok = FAIL(e, K_APPLY, "Failed to find context '%s' in %s", c, path);
                    free(c);
                }
            }
        }
        if (!ok) {
            variants_free(&lv.v);
            break;
        }
        if (!h->old_lines.n) {
            size_t ins;
            int64_t hint2 = h->old_start >= 0 ? h->old_start : h->new_start;
            if (h->change_context) {
                ins = line_index;
            } else if (hint2 >= 0) {
                if (hint2 < 1) ok = FAIL(e, K_APPLY, "Line hint %lld is out of range for insertion in %s (line numbers start at 1)", (long long)hint2, path);
                else if ((size_t)hint2 > n + 1)
                    ok = FAIL(e, K_APPLY, "Line hint %lld is out of range for insertion in %s (file has %zu lines)",
                              (long long)hint2, path, n);
                ins = hint2 > 0 ? (size_t)hint2 - 1 : 0;
            } else if (n && !*L[n - 1]) {
                ins = n - 1;
            } else {
                ins = n;
            }
            if (ok) {
                repl_t rp = {ins, 0, vec_copy(h->new_lines.v, h->new_lines.n)};
                vec_push(out, rp);
            }
            variants_free(&lv.v);
            continue;
        }
        strvec_t pattern = vec_copy(h->old_lines.v, h->old_lines.n), new_lines = vec_copy(h->new_lines.v, h->new_lines.n);
        int64_t match_hint = h->old_start >= 0 && (size_t)(h->old_start - 1) >= line_index ? h->old_start - 1 : -1;
        fz_seq_t res = seq_hint(L, n, pattern.v, pattern.n, line_index, match_hint, h->eof, fz);
        if (res.index < 0 && pattern.n && !*pattern.v[pattern.n - 1]) {
            free(pattern.v[--pattern.n]);
            if (new_lines.n && !*new_lines.v[new_lines.n - 1]) free(new_lines.v[--new_lines.n]);
            res = seq_hint(L, n, pattern.v, pattern.n, line_index, match_hint, h->eof, fz);
        }
        if (res.index < 0 || res.match_count > 1) {
            variants_t *vs = lazy_variants(&lv, h, aggressive);
            for (size_t i = 0; i < vs->n; i++) {
                variant_t *v = &vs->v[i];
                if (!v->old_lines.n) continue;
                fz_seq_t c = seq_hint(L, n, v->old_lines.v, v->old_lines.n, line_index, match_hint, h->eof, fz);
                if (c.index >= 0 && mc_or(c.match_count, 1) <= 1) {
                    strvec_free(&pattern);
                    strvec_free(&new_lines);
                    pattern = vec_copy(v->old_lines.v, v->old_lines.n);
                    new_lines = vec_copy(v->new_lines.v, v->new_lines.n);
                    res = c;
                    break;
                }
            }
        }
        if (res.index < 0 && context_index >= 0) {
            variants_t *vs = lazy_variants(&lv, h, aggressive);
            for (size_t i = 0; i < vs->n; i++) {
                variant_t *v = &vs->v[i];
                if (v->old_lines.n != 1 || v->new_lines.n != 1) continue;
                bool prefer = false;
                for (size_t k = 0; k < h->new_lines.n && !prefer; k++) prefer = fz_trim_eq(h->new_lines.v[k], v->old_lines.v[0]);
                int64_t f = find_context_relative(L, n, v->old_lines.v[0], (size_t)context_index, prefer);
                if (f >= 0) {
                    strvec_free(&pattern);
                    strvec_free(&new_lines);
                    pattern = vec_copy(v->old_lines.v, 1);
                    new_lines = vec_copy(v->new_lines.v, 1);
                    res.index = f;
                    res.confidence = 0.95;
                    break;
                }
            }
        }
        if (res.index >= 0 && context_index >= 0 && pattern.n == 1) {
            size_t dup = 0;
            for (size_t i = 0; i < n; i++)
                if (fz_trim_eq(L[i], pattern.v[0])) dup++;
            if (dup > 1) {
                bool prefer = false;
                for (size_t k = 0; k < h->new_lines.n && !prefer; k++) prefer = fz_trim_eq(h->new_lines.v[k], pattern.v[0]);
                int64_t f = find_context_relative(L, n, pattern.v[0], (size_t)context_index, prefer);
                if (f >= 0) res.index = f;
            }
        }
        if (res.match_count > 1) {
            int64_t hint = match_hint >= 0 ? match_hint : line_hint >= 0 ? line_hint - 1 : -1;
            if (hint >= 0) {
                size_t cand = 0, cnt = 0;
                for (size_t i = 0; res.has_indices && i < res.nindices; i++) {
                    int64_t v = (int64_t)res.indices[i];
                    if (llabs(v - hint) <= 200) {
                        cand = res.indices[i];
                        cnt++;
                    }
                }
                if (cnt == 1) {
                    res.index = (int64_t)cand;
                    res.match_count = 1;
                }
            }
        }
        if (res.index < 0) {
            if (res.match_count > 1) {
                char *strat = res.strategy >= 0 ? xasprintf(" Matching strategy: %s.", fz_seq_label(res.strategy)) : xstrdup("");
                char *pv = sequence_previews(L, n, &res);
                char *prev = pv ? xasprintf("\n\n%s", pv) : xstrdup("");
                ok = FAIL(e, K_APPLY,
                          "Found %lld matches for the text in %s.%s%s\n\nAdd more surrounding context or additional @@ "
                          "anchors to make it unique.",
                          (long long)res.match_count, path, strat, prev);
                free(strat);
                free(pv);
                free(prev);
            } else {
                int64_t ci;
                double conf;
                fz_closest_sequence(L, n, pattern.v, pattern.n, line_index, h->eof, &ci, &conf);
                char *old = join(h->old_lines.v, h->old_lines.n, "\n");
                if (ci >= 0 && conf > 0.0) {
                    sb_t pb = SB_INIT;
                    sequence_preview(&pb, L, n, (size_t)ci);
                    ok = FAIL(e, K_APPLY, "Failed to find expected lines in %s:\n%s\n\nClosest match (%.0f%% similar) near line %lld:\n%s",
                              path, old, conf * 100.0, (long long)ci + 1, sb_cstr(&pb));
                    sb_free(&pb);
                } else {
                    ok = FAIL(e, K_APPLY, "Failed to find expected lines in %s:\n%s", path, old);
                }
                free(old);
            }
            goto next;
        }
        {
            size_t found = (size_t)res.index;
            if (res.strategy == SEQ_FUZZY_DOMINANT) {
                strvec_push_own(warnings, xasprintf("Dominant fuzzy match selected in %s near line %zu (%.0f%% similar).",
                                                    path, found + 1, res.confidence * 100.0));
            } else if (res.strategy == SEQ_COMMENT_PREFIX || res.strategy == SEQ_PREFIX || res.strategy == SEQ_SUBSTRING ||
                       res.strategy == SEQ_FUZZY || res.strategy == SEQ_CHARACTER) {
                strvec_push_own(warnings,
                                xasprintf("Inexact match in %s near line %zu: matched via %s strategy (%.0f%% similar). "
                                          "Re-read the file if the result is not what you intended.",
                                          path, found + 1, fz_seq_label(res.strategy), res.confidence * 100.0));
            }
            if (!h->change_context && !h->has_context_lines && !h->eof && line_hint < 0) {
                if (res.match_count > 1) {
                    char *pv = sequence_previews(L, n, &res);
                    char *prev = pv ? xasprintf(":\n\n%s", pv) : xstrdup("");
                    ok = FAIL(e, K_APPLY, "Found %lld occurrences in %s%s\n\nAdd more context lines to disambiguate.",
                              (long long)res.match_count, path, prev);
                    free(pv);
                    free(prev);
                    goto next;
                }
                fz_seq_t second = fz_seek_sequence(L, n, pattern.v, pattern.n, found + 1, false, fz);
                if (second.index >= 0) {
                    sb_t a = SB_INIT, b = SB_INIT;
                    sequence_preview(&a, L, n, found);
                    sequence_preview(&b, L, n, (size_t)second.index);
                    ok = FAIL(e, K_APPLY, "Found 2 occurrences in %s:\n\n%s\n\n%s\n\nAdd more context lines to disambiguate.",
                              path, sb_cstr(&a), sb_cstr(&b));
                    sb_free(&a);
                    sb_free(&b);
                    goto next;
                }
            }
            if (vec_eq(&pattern, &new_lines)) {
                line_index = found + pattern.n;
                goto next;
            }
            /* the character-level fallback places a match past an exhausted file at `start` (> n): nothing to copy */
            size_t nm = found < n ? MIN(found + pattern.n, n) - found : 0;
            if (res.strategy == SEQ_PREFIX || res.strategy == SEQ_SUBSTRING) {
                if (!(ok = assert_partial_match(path, &pattern, L + MIN(found, n), nm, &new_lines, found, e))) goto next;
            }
            strvec_t matched = vec_copy(L + MIN(found, n), nm);
            repl_t rp = {found, pattern.n, adjust_lines_indentation(&pattern, &matched, &new_lines)};
            strvec_free(&matched);
            vec_push(out, rp);
            line_index = found + pattern.n;
        }
    next:
        strvec_free(&pattern);
        strvec_free(&new_lines);
        variants_free(&lv.v);
    }
    if (!ok) return false;
    if (out->n > 1) xqsort(out->v, out->n, sizeof(repl_t), cmp_repl);
    for (size_t i = 0; i + 1 < out->n; i++) {
        repl_t *l = &out->v[i], *r = &out->v[i + 1];
        if (r->start < l->start + l->old_len) {
            sb_t a = SB_INIT, b = SB_INIT;
            range_str(&a, l);
            range_str(&b, r);
            FAIL(e, K_APPLY,
                 "Overlapping hunks detected in %s at lines %s and %s. Split hunks or add more context to avoid overlap.",
                 path, sb_cstr(&a), sb_cstr(&b));
            sb_free(&a);
            sb_free(&b);
            return false;
        }
    }
    return true;
}

/* Codex ladder: exact, trim-end, trim, unicode; first match at/after the cursor */
static int64_t seek_apply_patch_sequence(char *const *L, size_t n, char *const *P, size_t m, size_t start, bool eof) {
    if (m > n) return -1;
    size_t end = n - m;
    if (start > end) return -1;
    size_t preferred = eof ? end : start;
    size_t froms[2] = {preferred, start};
    for (int fi = 0; fi < 2; fi++) {
        for (int cmp = 0; cmp < 4; cmp++) {
            for (size_t i = froms[fi]; i <= end; i++) {
                bool all = true;
                for (size_t k = 0; k < m && all; k++) {
                    const char *a = L[i + k], *b = P[k];
                    if (cmp == 0) all = !strcmp(a, b);
                    else if (cmp == 1) {
                        char *x = fz_trim_end(a), *y = fz_trim_end(b);
                        all = !strcmp(x, y);
                        free(x);
                        free(y);
                    } else if (cmp == 2) {
                        all = fz_trim_eq(a, b);
                    } else {
                        char *x = fz_normalize_unicode(a), *y = fz_normalize_unicode(b);
                        all = !strcmp(x, y);
                        free(x);
                        free(y);
                    }
                }
                if (all) return (int64_t)i;
            }
        }
        if (preferred == start) break;
    }
    return -1;
}

static bool compute_apply_patch_replacements(const strvec_t *lines, const char *path, const hunks_t *hunks,
                                             repls_t *out, perr_t *e) {
    char *const *L = lines->v;
    size_t n = lines->n, cursor = 0;
    for (size_t hi = 0; hi < hunks->n; hi++) {
        const hunk_t *h = &hunks->v[hi];
        int64_t hint = h->old_start >= 0 ? h->old_start : h->new_start;
        if (hint >= 0) {
            if (hint < 1 || (size_t)hint > n + 1) return FAIL(e, K_APPLY, "Line hint %lld is out of range for %s", (long long)hint, path);
            cursor = MAX(cursor, (size_t)hint - 1);
        }
        if (h->change_context) {
            strvec_t anchors = fz_split(h->change_context);
            for (size_t a = 0; a < anchors.n; a++) {
                char *const P[1] = {anchors.v[a]};
                int64_t f = seek_apply_patch_sequence(L, n, P, 1, cursor, false);
                if (f < 0) {
                    FAIL(e, K_APPLY, "Failed to find context '%s' in %s", anchors.v[a], path);
                    strvec_free(&anchors);
                    return false;
                }
                cursor = (size_t)f + 1;
            }
            strvec_free(&anchors);
        }
        size_t found;
        if (!h->old_lines.n) {
            found = (h->change_context || hint >= 0) ? cursor : n;
        } else {
            int64_t f = seek_apply_patch_sequence(L, n, h->old_lines.v, h->old_lines.n, cursor, h->eof);
            if (f < 0) {
                char *old = join(h->old_lines.v, h->old_lines.n, "\n");
                FAIL(e, K_APPLY, "Failed to find expected lines in %s:\n%s", path, old);
                free(old);
                return false;
            }
            found = (size_t)f;
        }
        if (found > n) found = n;
        strvec_t matched = vec_copy(L + found, MIN(h->old_lines.n, n - found));
        repl_t rp = {found, h->old_lines.n, adjust_lines_indentation(&h->old_lines, &matched, &h->new_lines)};
        strvec_free(&matched);
        vec_push(out, rp);
        cursor = found + h->old_lines.n;
    }
    return true;
}

static void trim_trailing_newlines(sb_t *b) {
    while (b->len && b->s[b->len - 1] == '\n') b->len--;
    if (b->s) b->s[b->len] = 0;
}

static bool apply_hunks(const char *content, const char *path, const hunks_t *hunks, double threshold, bool fz,
                        bool first_match, char **out, strvec_t *warnings, perr_t *e) {
    size_t clen = strlen(content);
    bool had_nl = clen && content[clen - 1] == '\n';
    if (!first_match && hunks->n == 1) {
        const hunk_t *h = &hunks->v[0];
        if (!h->change_context && !h->has_context_lines && h->old_lines.n && h->old_start < 0 && !h->eof) {
            char *next = NULL;
            if (!character_match(content, path, h, threshold, fz, &next, warnings, e)) return false;
            sb_t b = SB_INIT;
            sb_append(&b, next);
            free(next);
            if (had_nl && !(b.len && b.s[b.len - 1] == '\n')) sb_putc(&b, '\n');
            else if (!had_nl) trim_trailing_newlines(&b);
            *out = sb_detach(&b);
            return true;
        }
    }
    strvec_t lines = fz_split(content);
    if (first_match && !clen) strvec_free(&lines);
    bool stripped = false;
    if (had_nl && lines.n && !*lines.v[lines.n - 1]) {
        free(lines.v[--lines.n]);
        stripped = true;
    }
    repls_t reps = {0};
    bool ok = first_match ? compute_apply_patch_replacements(&lines, path, hunks, &reps, e)
                          : compute_replacements(&lines, path, hunks, fz, &reps, warnings, e);
    if (!ok) {
        repls_free(&reps);
        strvec_free(&lines);
        return false;
    }
    strvec_t res = {0};
    size_t copied = 0;
    for (size_t i = 0; i < reps.n; i++) {
        repl_t *r = &reps.v[i];
        for (size_t k = copied; k < r->start && k < lines.n; k++) strvec_push(&res, lines.v[k]);
        for (size_t k = 0; k < r->new_lines.n; k++) strvec_push(&res, r->new_lines.v[k]);
        copied = r->start + r->old_len;
    }
    for (size_t k = copied; k < lines.n; k++) strvec_push(&res, lines.v[k]);
    repls_free(&reps);
    strvec_free(&lines);
    if (first_match && !res.n) {
        *out = xstrdup("");
        return true;
    }
    if (stripped) strvec_push(&res, "");
    sb_t b = SB_INIT;
    for (size_t i = 0; i < res.n; i++) {
        if (i) sb_putc(&b, '\n');
        sb_append(&b, res.v[i]);
    }
    strvec_free(&res);
    if (had_nl && !(b.len && b.s[b.len - 1] == '\n')) sb_putc(&b, '\n');
    else if (!had_nl) trim_trailing_newlines(&b);
    *out = sb_detach(&b);
    return true;
}

/* ======================================================================================
 * patch entries (modes/patch.rs)
 * ====================================================================================== */

enum { POP_CREATE, POP_DELETE, POP_UPDATE };

typedef struct {
    const char *path;
    int op;
    const char *rename; /* NULL none */
    const char *diff;   /* NULL none */
    bool first_match;
} pinput_t;

static bool parse_op(const char *v, int *op, perr_t *e) {
    if (!v || !strcmp(v, "update")) *op = POP_UPDATE;
    else if (!strcmp(v, "create")) *op = POP_CREATE;
    else if (!strcmp(v, "delete")) *op = POP_DELETE;
    else return FAIL(e, K_APPLY, "Invalid patch operation: %s", v);
    return true;
}


static char *create_content(const char *diff) {
    char *n = normalize_create_content(diff);
    if (str_ends(n, "\n")) return n;
    char *r = xasprintf("%s\n", n);
    free(n);
    return r;
}

/* move destination (abs + display) or none; false on error */
static bool validate_rename(fsrc_t *fs, const pinput_t *in, const char *src_abs, char **mabs, char **mdisp, perr_t *e) {
    *mabs = *mdisp = NULL;
    if (!in->rename) return true;
    if (!fs_resolve(fs, in->rename, mabs, mdisp, e)) return false;
    if (!strcmp(*mabs, src_abs)) {
        FAIL(e, K_APPLY, "rename path is the same as source path");
        goto bad;
    }
    if (fs_exists_abs(fs, *mabs)) {
        FAIL(e, K_APPLY, "Cannot rename %s to %s: destination already exists.", in->path, in->rename);
        goto bad;
    }
    return true;
bad:
    free(*mabs);
    free(*mdisp);
    *mabs = *mdisp = NULL;
    return false;
}

static void stage_from_parts(staged_t *s, int op, const char *abs, const char *display, const fread_t *read,
                             char *after /* owned or NULL */, strvec_t warnings, char *mabs, char *mdisp,
                             bool use_new_encoding) {
    memset(s, 0, sizeof(*s));
    const char *before = read ? read->text : "";
    const char *cmp_after = after ? after : "";
    s->diff = diff_numbered_unified(before, cmp_after, -1, &s->first_changed);
    s->preview_diff = diff_numbered(before, cmp_after, -1, NULL);
    if (after) {
        if (use_new_encoding || !read) {
            char *nerr = NULL, *nj = is_notebook_path(abs) ? nb_serialize(NULL, after, display, &nerr) : NULL;
            free(nerr);
            s->persisted = nj ? nj : xstrdup(after);
        }
        else if (mabs && !strcmp(read->text, after)) s->persisted = xstrdup(read->raw);
        else s->persisted = persist(read, after);
    }
    s->display = xstrdup(display);
    s->abs = xstrdup(abs);
    s->op = op;
    s->move_abs = mabs;
    s->move_display = mdisp;
    s->existed = read != NULL;
    s->before_raw = read ? xstrdup(read->raw) : NULL;
    s->before = xstrdup(before);
    s->after = after ? after : xstrdup(before);
    s->warnings = warnings;
}

static bool stage_patch(fsrc_t *fs, const pinput_t *in, bool fz, double threshold, bool allow_overwrite, staged_t *out,
                        perr_t *e) {
    char *abs = NULL, *display = NULL, *mabs = NULL, *mdisp = NULL;
    if (!fs_resolve(fs, in->path, &abs, &display, e)) return false;
    bool ok = validate_rename(fs, in, abs, &mabs, &mdisp, e);
    if (!ok) goto out;
    if (in->op == POP_CREATE) {
        if (!in->diff) {
            ok = FAIL(e, K_APPLY, "Create operation requires diff (file content)");
            goto out;
        }
        fread_t *r = NULL;
        perr_t te = {0};
        bool rok = fs_try_read(fs, abs, display, &r, &te);
        if (!rok && strcmp(te.kind, K_UTF8) != 0) {
            fail(e, te.kind, te.msg);
            ok = false;
            goto out;
        }
        free(te.msg);
        if ((r || !rok) && !allow_overwrite) {
            ok = FAIL(e, K_APPLY, "Cannot create %s: file already exists. Use *** Update File to modify it in place.", in->path);
            goto out;
        }
        stage_from_parts(out, OP_CREATE, abs, display, NULL, create_content(in->diff), (strvec_t){0}, mabs, mdisp, true);
        mabs = mdisp = NULL;
    } else if (in->op == POP_DELETE) {
        fread_t *r = NULL;
        perr_t te = {0};
        bool rok = fs_try_read(fs, abs, display, &r, &te);
        if (!rok && strcmp(te.kind, K_UTF8) != 0) {
            fail(e, te.kind, te.msg);
            ok = false;
            goto out;
        }
        free(te.msg);
        if (rok && !r) {
            ok = FAIL(e, K_NOTFOUND, "File not found: %s", display);
            goto out;
        }
        stage_from_parts(out, OP_DELETE, r ? r->abs : abs, r ? r->display : display, r, NULL, (strvec_t){0}, mabs, mdisp,
                         false);
        mabs = mdisp = NULL;
        if (!rok) out->existed = true;
    } else {
        if (!in->diff) {
            ok = FAIL(e, K_APPLY, "Update operation requires diff (hunks)");
            goto out;
        }
        fread_t *r = fs_read_path(fs, in->path, e);
        if (!r) {
            ok = false;
            goto out;
        }
        char *after = NULL;
        strvec_t warnings = {0};
        if (in->first_match && mabs && fz_blank(in->diff)) {
            after = xstrdup(r->text);
        } else {
            hunks_t hunks;
            if (!parse_hunks(in->diff, !in->first_match, &hunks, e)) {
                ok = false;
                goto out;
            }
            if (!hunks.n) {
                hunks_free(&hunks);
                ok = FAIL(e, K_APPLY, "Diff contains no hunks");
                goto out;
            }
            ok = apply_hunks(r->text, in->path, &hunks, threshold, fz, in->first_match, &after, &warnings, e);
            hunks_free(&hunks);
            if (!ok) {
                strvec_free(&warnings);
                goto out;
            }
        }
        stage_from_parts(out, OP_UPDATE, r->abs, r->display, r, after, warnings, mabs, mdisp, false);
        mabs = mdisp = NULL;
    }
out:
    free(abs);
    free(display);
    free(mabs);
    free(mdisp);
    return ok;
}

typedef struct {
    const char *op;
    const char *rename;
    const char *diff;
} pentry_t;

/* several entries against one path, applied in sequence (PatchEngine::stage multi-entry path) */
static bool stage_patch_multi(fsrc_t *fs, const char *path, const pentry_t *entries, size_t n, bool first_match,
                              bool fz, double threshold, staged_t *out, perr_t *e) {
    int first_op;
    if (!parse_op(entries[0].op, &first_op, e)) return false;
    char *abs = NULL, *display = NULL, *mabs = NULL, *mdisp = NULL;
    if (!fs_resolve(fs, path, &abs, &display, e)) return false;
    bool needs_content = first_op == POP_UPDATE;
    fread_t *initial = NULL;
    bool initially_existed;
    perr_t te = {0};
    bool ok = true;
    if (fs_try_read(fs, abs, display, &initial, &te)) {
        initially_existed = initial != NULL;
    } else if (!strcmp(te.kind, K_UTF8) && !needs_content) {
        initially_existed = true;
        free(te.msg);
    } else {
        fail(e, te.kind, te.msg);
        free(abs);
        free(display);
        return false;
    }
    char *current = xstrdup(initial ? initial->text : "");
    bool exists = initially_existed, use_new = false;
    int final_op = OP_UPDATE;
    strvec_t warnings = {0};
    for (size_t i = 0; i < n && ok; i++) {
        int op;
        if (!(ok = parse_op(entries[i].op, &op, e))) break;
        if (entries[i].rename) {
            char *da, *dd;
            if (!(ok = fs_resolve(fs, entries[i].rename, &da, &dd, e))) break;
            if (!strcmp(da, abs)) ok = FAIL(e, K_APPLY, "rename path is the same as source path");
            else if (fs_exists_abs(fs, da))
                ok = FAIL(e, K_APPLY, "Cannot rename %s to %s: destination already exists.", path, entries[i].rename);
            if (ok && op == POP_UPDATE) {
                free(mabs);
                free(mdisp);
                mabs = da;
                mdisp = dd;
            } else {
                free(da);
                free(dd);
            }
            if (!ok) break;
        }
        if (op == POP_CREATE) {
            if (!entries[i].diff) {
                ok = FAIL(e, K_APPLY, "Create operation requires diff (file content)");
                break;
            }
            free(current);
            current = create_content(entries[i].diff);
            exists = true;
            final_op = OP_CREATE;
            use_new = true;
        } else if (op == POP_DELETE) {
            if (!exists) {
                ok = FAIL(e, K_NOTFOUND, "File not found: %s", path);
                break;
            }
            exists = false;
            final_op = OP_DELETE;
        } else {
            if (!exists) {
                ok = FAIL(e, K_NOTFOUND, "File not found: %s", path);
                break;
            }
            if (!entries[i].diff) {
                ok = FAIL(e, K_APPLY, "Update operation requires diff (hunks)");
                break;
            }
            hunks_t hunks;
            if (!(ok = parse_hunks(entries[i].diff, !first_match, &hunks, e))) break;
            if (!hunks.n) {
                hunks_free(&hunks);
                ok = FAIL(e, K_APPLY, "Diff contains no hunks");
                break;
            }
            char *next = NULL;
            ok = apply_hunks(current, path, &hunks, threshold, fz, first_match, &next, &warnings, e);
            hunks_free(&hunks);
            if (!ok) break;
            free(current);
            current = next;
            final_op = OP_UPDATE;
        }
    }
    if (ok) {
        char *after = exists ? current : NULL;
        if (!exists) free(current);
        current = NULL;
        stage_from_parts(out, final_op, abs, display, initial, after, warnings, mabs, mdisp, use_new);
        mabs = mdisp = NULL;
        warnings = (strvec_t){0};
        if (!needs_content && initially_existed) out->existed = true;
    }
    free(current);
    strvec_free(&warnings);
    free(abs);
    free(display);
    free(mabs);
    free(mdisp);
    return ok;
}

static bool patch_stage(fsrc_t *fs, const json_t *args, bool fz, double threshold, stageds_t *out, perr_t *e) {
    const char *path = json_get_str(args, "path");
    if (!path) return FAIL(e, K_PARSE, "Patch path is required");
    json_t *edits = json_get_arr(args, "edits");
    size_t n = edits ? json_len(edits) : 0;
    if (!n) return FAIL(e, K_APPLY, "No files were modified.");
    pentry_t *es = xcalloc(n, sizeof(pentry_t));
    for (size_t i = 0; i < n; i++) {
        json_t *x = json_at(edits, i);
        es[i].op = json_get_str(x, "op");
        es[i].rename = json_get_str(x, "rename");
        es[i].diff = json_get_str(x, "diff");
    }
    staged_t s;
    bool ok;
    if (n == 1) {
        pinput_t in = {path, POP_UPDATE, es[0].rename, es[0].diff, false};
        ok = parse_op(es[0].op, &in.op, e) && stage_patch(fs, &in, fz, threshold, true, &s, e);
    } else {
        ok = stage_patch_multi(fs, path, es, n, false, fz, threshold, &s, e);
    }
    if (ok) vec_push(out, s);
    free(es);
    return ok;
}

/* ======================================================================================
 * apply_patch envelope (modes/apply_patch.rs)
 * ====================================================================================== */

typedef struct {
    char *path;
    int op;
    char *rename;
    char *diff;
} apentry_t;
typedef VEC(apentry_t) apentries_t;

static void apentries_free(apentries_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].path);
        free(v->v[i].rename);
        free(v->v[i].diff);
    }
    vec_free(v);
}

static bool parse_apply_patch(const char *text, apentries_t *out, perr_t *e) {
    memset(out, 0, sizeof(*out));
    char *t = fz_trim(text);
    strvec_t all = fz_split(t);
    free(t);
    size_t lo = 0, hi = all.n;
    if (all.n >= 2 && (!strcmp(all.v[0], "<<EOF") || !strcmp(all.v[0], "<<'EOF'") || !strcmp(all.v[0], "<<\"EOF\""))) {
        char *last = fz_trim(all.v[all.n - 1]);
        if (!strcmp(last, "EOF")) {
            lo = 1;
            hi = all.n - 1;
        }
        free(last);
    }
    char *const *lines = all.v + lo;
    size_t nl = hi - lo;
    bool ok = true;
    char *first = nl ? fz_trim(lines[0]) : NULL;
    if (!nl || strcmp(first, "*** Begin Patch") != 0) {
        free(first);
        strvec_free(&all);
        return FAIL(e, K_PARSE, "The first line of the patch must be '*** Begin Patch'");
    }
    free(first);
    char *last = fz_trim(lines[nl - 1]);
    bool has_end = !strcmp(last, "*** End Patch");
    free(last);
    if (!has_end) {
        strvec_free(&all);
        return FAIL(e, K_PARSE, "The last line of the patch must be '*** End Patch'");
    }
    size_t end = nl - 1, i = 1;
    while (i < end && ok) {
        char *f = fz_trim(lines[i]);
        if (!*f) {
            free(f);
            i++;
            continue;
        }
        if (str_starts(f, "*** Add File: ")) {
            apentry_t en = {xstrdup(f + 14), POP_CREATE, NULL, NULL};
            i++;
            sb_t b = SB_INIT;
            while (i < end && lines[i][0] == '+') {
                sb_append(&b, lines[i] + 1);
                sb_putc(&b, '\n');
                i++;
            }
            en.diff = sb_detach(&b);
            vec_push(out, en);
        } else if (str_starts(f, "*** Delete File: ")) {
            apentry_t en = {xstrdup(f + 17), POP_DELETE, NULL, NULL};
            vec_push(out, en);
            i++;
        } else if (str_starts(f, "*** Update File: ")) {
            char *path = xstrdup(f + 17), *rename = NULL;
            i++;
            if (i < end && str_starts(lines[i], "*** Move to: ")) {
                rename = xstrdup(lines[i] + 13);
                i++;
            }
            size_t body = i;
            while (i < end && !str_starts(lines[i], "*** Add File:") && !str_starts(lines[i], "*** Delete File:") &&
                   !str_starts(lines[i], "*** Update File:"))
                i++;
            if (body == i) {
                if (rename) {
                    apentry_t en = {path, POP_UPDATE, rename, xstrdup("")};
                    vec_push(out, en);
                } else {
                    ok = FAIL(e, K_PARSE, "Line %zu: Update file hunk for path '%s' is empty", body + 1, path);
                    free(path);
                }
            } else {
                apentry_t en = {path, POP_UPDATE, rename, join(lines + body, i - body, "\n")};
                vec_push(out, en);
            }
        } else {
            ok = FAIL(e, K_PARSE,
                      "Line %zu: '%s' is not a valid hunk header. Valid hunk headers: '*** Add File: {path}', '*** "
                      "Delete File: {path}', '*** Update File: {path}'",
                      i + 1, f);
        }
        free(f);
    }
    strvec_free(&all);
    if (!ok) apentries_free(out);
    return ok;
}

static void vrecord(fsrc_t *fs, const staged_t *s) {
    if (s->op == OP_DELETE || s->move_abs) {
        vfile_t *v = vfind(fs, s->abs);
        if (!v) {
            vfile_t nv = {xstrdup(s->abs), true, NULL};
            vec_push(&fs->virt, nv);
        } else {
            v->deleted = true;
            free(v->raw);
            v->raw = NULL;
        }
    }
    if (s->persisted) {
        const char *target = s->move_abs ? s->move_abs : s->abs;
        vfile_t *v = vfind(fs, target);
        if (!v) {
            vfile_t nv = {xstrdup(target), false, xstrdup(s->persisted)};
            vec_push(&fs->virt, nv);
        } else {
            v->deleted = false;
            free(v->raw);
            v->raw = xstrdup(s->persisted);
        }
    }
}

static bool apply_patch_stage(fsrc_t *fs, const json_t *args, bool fz, double threshold, stageds_t *out, perr_t *e) {
    const char *input = json_get_str(args, "input");
    if (!input) return FAIL(e, K_PARSE, "The first line of the patch must be '*** Begin Patch'");
    apentries_t en;
    if (!parse_apply_patch(input, &en, e)) return false;
    if (!en.n) {
        apentries_free(&en);
        return FAIL(e, K_APPLY, "No files were modified.");
    }
    strvec_t distinct = {0};
    for (size_t i = 0; i < en.n; i++)
        if (!strvec_contains(&distinct, en.v[i].path)) strvec_push(&distinct, en.v[i].path);
    bool multi = distinct.n > 1;
    strvec_free(&distinct);
    bool ok = true;
    size_t idx = 0;
    while (idx < en.n && ok) {
        size_t start = idx++;
        const char *path = en.v[start].path;
        if (!en.v[start].rename)
            while (idx < en.n && !strcmp(en.v[idx].path, path) && !en.v[idx].rename) idx++;
        size_t gn = idx - start;
        staged_t s;
        if (gn == 1) {
            pinput_t in = {path, en.v[start].op, en.v[start].rename, en.v[start].diff, true};
            ok = stage_patch(fs, &in, fz, threshold, false, &s, e);
        } else {
            char *abs, *display;
            ok = fs_resolve(fs, path, &abs, &display, e);
            if (ok) {
                bool exists = fs_exists_abs(fs, abs);
                for (size_t k = start; k < idx && ok; k++) {
                    int op = en.v[k].op;
                    if (op == POP_CREATE && exists)
                        ok = FAIL(e, K_APPLY,
                                  "Cannot create %s: file already exists. Use *** Update File to modify it in place.", path);
                    else if (op == POP_CREATE) exists = true;
                    else if (op == POP_DELETE) exists = false;
                }
                free(abs);
                free(display);
            }
            if (ok) {
                pentry_t *pe = xcalloc(gn, sizeof(pentry_t));
                static const char *const OPS[] = {"create", "delete", "update"};
                for (size_t k = 0; k < gn; k++) {
                    pe[k].op = OPS[en.v[start + k].op];
                    pe[k].rename = en.v[start + k].rename;
                    pe[k].diff = en.v[start + k].diff;
                }
                ok = stage_patch_multi(fs, path, pe, gn, true, fz, threshold, &s, e);
                free(pe);
            }
        }
        if (!ok) {
            if (multi) wrap_in_file(e, path);
            break;
        }
        if (idx < en.n) vrecord(fs, &s);
        vec_push(out, s);
    }
    apentries_free(&en);
    return ok;
}

/* ======================================================================================
 * replace mode (modes/replace.rs)
 * ====================================================================================== */

static bool replace_stage(fsrc_t *fs, const json_t *args, bool fz, double threshold, stageds_t *out, perr_t *e) {
    const char *path = json_get_str(args, "path");
    if (!path || !*path) return FAIL(e, K_APPLY, "path is required.");
    json_t *edits = json_get_arr(args, "edits");
    size_t n = edits ? json_len(edits) : 1;
    typedef struct {
        const char *old_s, *new_s;
        bool all;
    } rentry_t;
    rentry_t *es = xcalloc(n ? n : 1, sizeof(rentry_t));
    for (size_t i = 0; i < n; i++) {
        const json_t *src = edits ? json_at(edits, i) : args;
        es[i].old_s = json_get_str(src, "old_string");
        es[i].new_s = json_get_str(src, "new_string");
        es[i].all = json_get_bool(src, "replace_all", false);
    }
    bool ok = true;
    for (size_t i = 0; i < n; i++)
        if (!es[i].old_s || !*es[i].old_s) {
            free(es);
            return FAIL(e, K_APPLY, "old_string must not be empty.");
        }
    fread_t *r = fs_read_path(fs, path, e);
    if (!r) {
        free(es);
        return false;
    }
    char *after = NULL;
    for (size_t i = 0; i < n && ok; i++) {
        const char *cur = after ? after : r->text;
        char *next = NULL, *err = NULL;
        size_t count;
        fz_outcome_t miss;
        if (fz_replace_text(cur, es[i].old_s, es[i].new_s ? es[i].new_s : "", fz, es[i].all, threshold, &next, &count,
                            &miss, &err)) {
            free(after);
            after = next;
            continue;
        }
        if (err) ok = fail(e, K_APPLY, err);
        else if (miss.occurrences > 1) ok = fail(e, K_APPLY, fz_format_occurrence_error(r->display, &miss));
        else
            ok = fail(e, K_MATCH, fz_format_match_error(r->display, es[i].old_s, miss.closest, fz, threshold,
                                                        miss.fuzzy_matches));
        fz_outcome_free(&miss);
    }
    free(es);
    if (!ok) {
        free(after);
        return false;
    }
    if (!after) after = xstrdup(r->text);
    if (!strcmp(r->text, after)) {
        free(after);
        return FAIL(e, K_NOCHANGE, "Edits to %s resulted in no changes being made.", r->display);
    }
    staged_t s;
    memset(&s, 0, sizeof(s));
    s.display = xstrdup(r->display);
    s.abs = xstrdup(r->abs);
    s.op = OP_UPDATE;
    s.existed = true;
    s.before_raw = xstrdup(r->raw);
    s.before = xstrdup(r->text);
    s.after = after;
    s.persisted = persist(r, after);
    s.diff = diff_numbered(r->text, after, -1, &s.first_changed);
    vec_push(out, s);
    return true;
}

/* ======================================================================================
 * sloppy mode (modes/sloppy/mod.rs)
 * ====================================================================================== */

/* PARTIAL_NOTICE: leads a file's rejected operations, so the model re-sends only those */
#define SLOPPY_PARTIAL_NOTICE                                                                                          \
    "Rejected the operations below; the payload's other operations were applied. Re-send only the rejected "          \
    "operations — the applied ones are already in place."

static bool sloppy_stage(fsrc_t *fs, const json_t *args, stageds_t *out, perr_t *e) {
    const char *input = json_get_str(args, "input");
    sloppy_sections_t secs = {0};
    char *err = NULL;
    if (!sloppy_split_sections(input ? input : "", &secs, &err)) return fail(e, K_PARSE, err);
    if (!secs.n) {
        sloppy_sections_free(&secs);
        return FAIL(e, K_PARSE, "Missing file target: start the payload with *** Edit File: relative/path.ts.");
    }
    bool multi = secs.n > 1, ok = true;
    strvec_t unchanged = {0}; /* each unchanged file's rejections; the call fails with them when no file changes */
    const char *unchanged_kind = NULL;
    for (size_t i = 0; i < secs.n && ok; i++) {
        fread_t *r = fs_read_path(fs, secs.v[i].path, e);
        if (!r) {
            if (multi) wrap_in_file(e, secs.v[i].path);
            ok = false;
            break;
        }
        strvec_t notes = {0};
        const char *kind = K_APPLY;
        char *after = NULL;
        sloppy_outcome_t oc = sloppy_apply(r->text, secs.v[i].body, r->display, &notes, &after, &err, &kind);
        if (oc == SLOPPY_FAILED) { /* a section that does not parse fails the whole payload */
            fail(e, kind, err);
            err = NULL;
            if (multi) wrap_in_file(e, r->display);
            strvec_free(&notes);
            ok = false;
            break;
        }
        staged_t s;
        memset(&s, 0, sizeof(s));
        s.display = xstrdup(r->display);
        s.abs = xstrdup(r->abs);
        s.existed = true;
        s.before_raw = xstrdup(r->raw);
        s.before = xstrdup(r->text);
        if (oc == SLOPPY_CHANGED) {
            s.op = OP_UPDATE;
            s.after = after;
            s.persisted = persist(r, after);
            s.diff = diff_numbered(r->text, after, -1, &s.first_changed);
            if (err) strvec_push_own(&notes, xasprintf("%s\n%s", SLOPPY_PARTIAL_NOTICE, err));
        } else {
            s.op = OP_NOOP;
            s.after = xstrdup(r->text);
            s.diff = xstrdup("");
            strvec_push_own(&notes, xasprintf("%s\n%s", SLOPPY_PARTIAL_NOTICE, err));
            if (!unchanged_kind) unchanged_kind = kind;
            strvec_push_own(&unchanged, multi ? xasprintf("[%s]: %s", r->display, err) : xstrdup(err));
        }
        free(err);
        err = NULL;
        s.after_preview = notes;
        vec_push(out, s);
    }
    if (ok && unchanged.n) {
        bool changed = false;
        for (size_t i = 0; i < out->n; i++) changed |= out->v[i].op != OP_NOOP;
        if (!changed) {
            /* no file changed: the call fails with every file's rejections (repeats dropped) */
            strvec_t uniq = {0};
            for (size_t i = 0; i < unchanged.n; i++)
                if (!strvec_contains(&uniq, unchanged.v[i])) strvec_push(&uniq, unchanged.v[i]);
            ok = fail(e, unchanged_kind ? unchanged_kind : K_APPLY, strvec_join(&uniq, "\n\n"));
            strvec_free(&uniq);
        }
    }
    strvec_free(&unchanged);
    sloppy_sections_free(&secs);
    return ok;
}

/* ======================================================================================
 * hashline mode (modes/hashline/mod.rs + patcher.rs stage_patch)
 * ====================================================================================== */

static bool hashline_header_only(fsrc_t *fs, const char *path, perr_t *e) {
    char *abs, *display;
    if (!fs_resolve(fs, path, &abs, &display, e)) return false;
    fread_t *r = NULL;
    if (fs_try_read(fs, abs, display, &r, e)) {
        if (r) {
            char *tag = file_tag(r->text, strlen(r->text));
            fail(e, K_APPLY, hl_header_only_message(path, tag));
            free(tag);
        } else {
            FAIL(e, K_NOTFOUND, "File not found: %s. Use the write tool to create new files.", path);
        }
    }
    free(abs);
    free(display);
    return false;
}

static bool hashline_stage(fsrc_t *fs, const json_t *args, hl_clip_t *clip, stageds_t *out, perr_t *e) {
    const pe_opts_t *o = fs->o;
    const char *input = json_get_str(args, "input");
    if (!input) input = "";
    char *err = NULL;
    hl_patch_t *p = hl_patch_parse(input, o->cwd, &err);
    if (!p) return fail(e, K_PARSE, err);
    if (!hl_patch_count(p)) {
        hl_patch_free(p);
        char *path = hl_first_header_path(input, o->cwd);
        if (!path) return FAIL(e, K_PARSE, "No hashline sections found in input.");
        bool r = hashline_header_only(fs, path, e);
        free(path);
        return r;
    }
    bool ok = true;
    strvec_t canon = {0}, canon_src = {0};
    for (size_t i = 0; i < hl_patch_count(p) && ok; i++) {
        const char *path = hl_section_path(p, i);
        const char *tag = hl_section_tag(p, i);
        bool range_err = false;
        if (!hl_section_parse(p, i, &err, &range_err)) {
            if (range_err) {
                char *abs = path_abs(path, o->cwd);
                size_t len;
                char *raw = abs ? fs_read(abs, &len) : NULL;
                char *text = raw ? fz_normalize_to_lf(raw) : NULL;
                free(err);
                err = hl_range_error_message(p, i, text);
                free(text);
                free(raw);
                free(abs);
            }
            ok = fail(e, K_PARSE, err);
            break;
        }
        char *abs, *display;
        if (!fs_resolve(fs, path, &abs, &display, e)) {
            ok = false;
            break;
        }
        fread_t *r = NULL;
        if (!fs_try_read(fs, abs, display, &r, e)) ok = false;
        else if (!r) ok = FAIL(e, K_NOTFOUND, "File not found: %s. Use the write tool to create new files.", path);
        else if (!tag) ok = fail(e, K_APPLY, hl_mismatch_message(p, i, abs, r->text, NULL, o->store));
        if (ok)
            for (size_t k = 0; k < canon.n; k++)
                if (!strcmp(canon.v[k], abs)) {
                    ok = FAIL(e, K_APPLY,
                              "Multiple hashline sections resolve to the same file (%s and %s). Merge their ops under "
                              "one header before applying.",
                              canon_src.v[k], path);
                    break;
                }
        const char *dest = NULL;
        int fop = ok ? hl_section_file_op(p, i, &dest) : 0;
        char *mabs = NULL, *mdisp = NULL;
        if (ok && fop == 2) {
            if (!fs_resolve(fs, dest, &mabs, &mdisp, e)) ok = false;
            else if (!strcmp(mabs, abs)) ok = FAIL(e, K_APPLY, "MV destination is the same as %s.", path);
        }
        hl_applied_t ap;
        memset(&ap, 0, sizeof(ap));
        if (ok) {
            strvec_push(&canon, abs);
            strvec_push(&canon_src, path);
            if (!hl_section_apply(p, i, abs, r->text, o->store, clip, o->enforce_seen, o->max_columns, &ap, &err))
                ok = fail(e, K_APPLY, err);
        }
        if (!ok) {
            free(abs);
            free(display);
            free(mabs);
            free(mdisp);
            break;
        }
        staged_t s;
        memset(&s, 0, sizeof(s));
        s.display = display;
        s.abs = abs;
        s.op = fop == 1 ? OP_DELETE : (!strcmp(ap.text, r->text) && !fop) ? OP_NOOP : OP_UPDATE;
        s.move_abs = mabs;
        s.move_display = mdisp;
        s.existed = true;
        s.before_raw = xstrdup(r->raw);
        s.before = xstrdup(r->text);
        s.after = ap.text;
        ap.text = NULL;
        int64_t dfirst = 0;
        s.diff = diff_numbered(s.before, s.after, -1, &dfirst);
        s.first_changed = ap.first_changed ? ap.first_changed : dfirst;
        if (s.op == OP_UPDATE) s.persisted = persist(r, s.after);
        s.hashline = true;
        s.before_preview = ap.block_preview;
        s.warnings = ap.warnings;
        memset(&ap.block_preview, 0, sizeof(ap.block_preview));
        memset(&ap.warnings, 0, sizeof(ap.warnings));
        hl_applied_free(&ap);
        if (s.op == OP_NOOP && hl_patch_count(p) == 1) {
            bool escalate = false;
            uint32_t count = hl_record_noop(o->store, abs, input, &escalate);
            if (escalate) {
                ok = fail(e, K_NOCHANGE, hl_no_change_loop_message(path, count));
                staged_free(&s);
                break;
            }
            s.text_override = hl_no_change_message(path);
        }
        vec_push(out, s);
    }
    if (ok && out->n > 1)
        for (size_t i = 0; i < out->n; i++)
            if (out->v[i].op == OP_NOOP) {
                bool escalate = false;
                uint32_t count = hl_record_noop(o->store, out->v[i].abs, input, &escalate);
                ok = fail(e, K_NOCHANGE, escalate ? hl_no_change_loop_message(out->v[i].display, count)
                                                  : hl_no_change_message(out->v[i].display));
                break;
            }
    strvec_free(&canon);
    strvec_free(&canon_src);
    hl_patch_free(p);
    return ok;
}

/* ======================================================================================
 * apply (session.rs) + rendering
 * ====================================================================================== */


int pe_mode_parse(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "replace")) return PE_REPLACE;
    if (!strcmp(name, "patch")) return PE_PATCH;
    if (!strcmp(name, "apply_patch")) return PE_APPLY_PATCH;
    if (!strcmp(name, "sloppy")) return PE_SLOPPY;
    if (!strcmp(name, "hashline")) return PE_HASHLINE;
    return -1;
}

const char *pe_mode_name(pe_mode_t m) {
    static const char *const N[] = {"replace", "patch", "apply_patch", "sloppy", "hashline"};
    return N[m];
}

void pe_result_free(pe_result_t *r) {
    free(r->text);
    json_free(r->details);
    memset(r, 0, sizeof(*r));
}

static const char *op_label(int op) { return op == OP_CREATE ? "create" : op == OP_DELETE ? "delete" : "update"; }

static char *format_file_text(const staged_t *s, const char *tag) {
    if (s->text_override) return xstrdup(s->text_override);
    sb_t b = SB_INIT;
    if (s->op == OP_DELETE) {
        sb_appendf(&b, "Deleted %s", s->display);
    } else {
        const char *src = s->preview_diff ? s->preview_diff : s->diff;
        char *preview = diff_compact_preview(src, NULL, NULL);
        strvec_t parts = {0};
        const char *hp = s->move_display ? s->move_display : s->display;
        strvec_push_own(&parts, tag ? xasprintf("[%s#%s]", hp, tag) : xasprintf("[%s]", hp));
        for (size_t i = 0; i < s->before_preview.n; i++) strvec_push(&parts, s->before_preview.v[i]);
        if (s->move_display) strvec_push_own(&parts, xasprintf("Moved to %s", s->move_display));
        if (*preview) strvec_push(&parts, preview);
        for (size_t i = 0; i < s->after_preview.n; i++) strvec_push(&parts, s->after_preview.v[i]);
        bool first = true;
        for (size_t i = 0; i < parts.n; i++) {
            if (!*parts.v[i]) continue;
            if (!first) sb_putc(&b, '\n');
            sb_append(&b, parts.v[i]);
            first = false;
        }
        strvec_free(&parts);
        free(preview);
    }
    bool any = false;
    for (size_t i = 0; i < s->warnings.n; i++)
        if (*s->warnings.v[i]) any = true;
    if (any) {
        sb_append(&b, "\n\nWarnings:");
        for (size_t i = 0; i < s->warnings.n; i++)
            if (*s->warnings.v[i]) sb_appendf(&b, "\n%s", s->warnings.v[i]);
    }
    return sb_detach(&b);
}

static int write_file(const pe_opts_t *o, const staged_t *s, char **err) {
    bool remote = o->remote && o->remote_write && o->remote(o->ud, s->abs);
    if (remote) {
        if (s->op == OP_DELETE || s->move_abs) {
            *err = xasprintf("%s: deleting or moving files is not supported for remote targets; use bash with a remote "
                             "command",
                             s->display);
            return -1;
        }
        const char *content = s->persisted ? s->persisted : "";
        return o->remote_write(o->ud, s->abs, content, strlen(content), err) != 0 ? -1 : 0;
    }
    if (o->bridge_write && s->op != OP_DELETE && !s->move_abs) {
        const char *content = s->persisted ? s->persisted : "";
        int rc = o->bridge_write(o->ud, s->abs, content, strlen(content), err);
        if (rc <= 0) return rc;
    }
    if (s->op == OP_DELETE) {
        if (unlink(s->abs) != 0) {
            *err = xasprintf("%s (os error %d)", strerror(errno), errno);
            return -1;
        }
        return 0;
    }
    const char *content = s->persisted ? s->persisted : "";
    if (s->move_abs) {
        struct stat st;
        mode_t mode = stat(s->abs, &st) == 0 ? (st.st_mode & 07777) : 0644;
        char *dir = path_dirname(s->move_abs);
        fs_mkdir_p(dir, 0755);
        free(dir);
        int rc = fs_write_atomic(s->move_abs, content, strlen(content), mode);
        if (rc != 0) {
            *err = xasprintf("%s (os error %d)", strerror(-rc), -rc);
            return -1;
        }
        chmod(s->move_abs, mode);
        if (unlink(s->abs) != 0) {
            *err = xasprintf("%s (os error %d)", strerror(errno), errno);
            return -1;
        }
        return 0;
    }
    struct stat st;
    mode_t mode = 0644;
    if (stat(s->abs, &st) == 0) mode = st.st_mode & 07777;
    if (s->op == OP_CREATE) {
        char *dir = path_dirname(s->abs);
        fs_mkdir_p(dir, 0755);
        free(dir);
    }
    int rc = fs_write_atomic(s->abs, content, strlen(content), mode);
    if (rc != 0) {
        *err = xasprintf("%s (os error %d)", strerror(-rc), -rc);
        return -1;
    }
    return 0;
}

#define MAX_SNAPSHOT_CHARS 32768

static json_t *per_file_json(const staged_t *s, pe_mode_t mode, bool single, bool *pruned_out, size_t *size_out) {
    json_t *o = json_obj();
    const char *path = s->move_abs ? s->move_abs : s->abs;
    const char *old_text = s->op == OP_CREATE ? NULL : s->before_raw;
    const char *new_text = s->op == OP_DELETE ? NULL : s->persisted;
    size_t size = (old_text ? fz_utf16_len(old_text) : 0) + (new_text ? fz_utf16_len(new_text) : 0);
    bool pruned = size > MAX_SNAPSHOT_CHARS;
    if (pruned) old_text = new_text = NULL;
    json_t *diag = NULL;
    if ((mode == PE_PATCH || mode == PE_APPLY_PATCH) && s->warnings.n) {
        diag = json_obj();
        json_set_str(diag, "server", "patch");
        json_t *msgs = json_arr();
        for (size_t i = 0; i < s->warnings.n; i++) {
            char *m = xasprintf("patch: %s", s->warnings.v[i]);
            json_push_str(msgs, m);
            free(m);
        }
        json_set(diag, "messages", msgs);
        char *sum = xasprintf("Patch warnings: %zu", s->warnings.n);
        json_set_str(diag, "summary", sum);
        free(sum);
        json_set_bool(diag, "errored", false);
    }
    if (!single) json_set_str(o, "path", path);
    json_set_str(o, "diff", s->diff);
    if (s->first_changed) json_set_int(o, "firstChangedLine", s->first_changed);
    if (diag) json_set(o, "diagnostics", diag);
    json_set_str(o, "op", op_label(s->op));
    if (s->move_abs) {
        json_set_str(o, "move", s->move_abs);
        json_set_str(o, "sourcePath", s->abs);
    }
    if (single) json_set_str(o, "path", path);
    if (old_text) json_set_str(o, "oldText", old_text);
    if (new_text) json_set_str(o, "newText", new_text);
    if (pruned) json_set_bool(o, "snapshotsPruned", true);
    *pruned_out = pruned;
    *size_out = size;
    return o;
}

static void finish(stageds_t *st, pe_mode_t mode, const pe_opts_t *o, hl_clip_t *clip, pe_result_t *out) {
    strvec_t texts = {0};
    for (size_t i = 0; i < st->n; i++) {
        staged_t *s = &st->v[i];
        if (s->op != OP_NOOP) {
            char *err = NULL;
            if (o->raw_snapshot) { /* magic: refuse a file that changed after the numbered snapshot was read */
                size_t len = 0;
                char *now = fs_read(s->abs, &len);
                bool same = now && len == o->raw_snapshot_len && !memcmp(now, o->raw_snapshot, len);
                free(now);
                if (!same) err = xstrdup("The file changed after the numbered snapshot was read; re-read and retry.");
            }
            if (err || write_file(o, s, &err) != 0) {
                out->is_error = true;
                out->error_kind = K_WRITE;
                out->text = err;
                out->details = json_obj();
                json_set_str(out->details, "diff", "");
                json_set_str(out->details, "errorKind", K_WRITE);
                strvec_free(&texts);
                return;
            }
            out->writes++;
        }
        char *tag = NULL;
        const char *key = s->move_abs ? s->move_abs : s->abs;
        if (s->hashline && o->store) {
            if (s->op == OP_DELETE) hl_store_invalidate(o->store, s->abs);
            else tag = file_tag(s->after, strlen(s->after));
        }
        char *t = format_file_text(s, tag);
        if (s->hashline && o->store && s->op != OP_DELETE) {
            char *rt = s->op == OP_NOOP ? hl_record_plain(o->store, key, s->after)
                                        : hl_record_after(o->store, s->abs, key, s->before, s->after, t, true);
            free(rt);
            if (s->op != OP_NOOP) hl_reset_noop(o->store, s->abs);
        }
        free(tag);
        if (clip && i + 1 == st->n) hl_clip_commit(clip, o->store);
        if (s->op != OP_DELETE && o->after_write) {
            char *d = o->after_write(o->ud, s->move_abs ? s->move_abs : s->abs);
            if (d && *d) {
                char *t2 = xasprintf("%s\n\n%s", t, d); /* omp: diagnostics notice at the message boundary */
                free(t);
                t = t2;
            }
            free(d);
        }
        strvec_push_own(&texts, t);
    }
    sb_t b = SB_INIT;
    bool first = true;
    for (size_t i = 0; i < texts.n; i++) {
        if (!*texts.v[i]) continue;
        if (!first) sb_append(&b, "\n\n");
        sb_append(&b, texts.v[i]);
        first = false;
    }
    strvec_free(&texts);
    out->text = sb_detach(&b);
    if (!st->n) return;
    if (st->n == 1) {
        bool pr;
        size_t sz;
        out->details = per_file_json(&st->v[0], mode, true, &pr, &sz);
        return;
    }
    json_t *d = json_obj(), *files = json_arr();
    sb_t diff = SB_INIT;
    int64_t first_changed = 0;
    size_t remaining = MAX_SNAPSHOT_CHARS;
    for (size_t i = 0; i < st->n; i++) {
        staged_t *s = &st->v[i];
        if (*s->diff) {
            if (diff.len) sb_putc(&diff, '\n');
            sb_append(&diff, s->diff);
        }
        if (!first_changed && s->first_changed) first_changed = s->first_changed;
        bool pr;
        size_t sz;
        json_t *f = per_file_json(s, mode, false, &pr, &sz);
        if (!pr && sz) {
            if (sz <= remaining) remaining -= sz;
            else {
                json_del(f, "oldText");
                json_del(f, "newText");
                json_set_bool(f, "snapshotsPruned", true);
            }
        }
        json_push(files, f);
    }
    json_set_str(d, "diff", sb_cstr(&diff));
    sb_free(&diff);
    if (first_changed) json_set_int(d, "firstChangedLine", first_changed);
    json_set(d, "perFileResults", files);
    out->details = d;
}

void pe_run(pe_mode_t mode, const json_t *args, const pe_opts_t *o, pe_result_t *out) {
    memset(out, 0, sizeof(*out));
    fsrc_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.o = o;
    stageds_t st = {0};
    perr_t e = {0};
    hl_clip_t *clip = NULL;
    bool ok;
    switch (mode) {
    case PE_REPLACE: ok = replace_stage(&fs, args, o->allow_fuzzy, o->fuzzy_threshold, &st, &e); break;
    case PE_PATCH: ok = patch_stage(&fs, args, o->allow_fuzzy, o->fuzzy_threshold, &st, &e); break;
    case PE_APPLY_PATCH: ok = apply_patch_stage(&fs, args, o->allow_fuzzy, o->fuzzy_threshold, &st, &e); break;
    case PE_HASHLINE:
        clip = hl_clip_start(o->store);
        ok = hashline_stage(&fs, args, clip, &st, &e);
        break;
    default: ok = sloppy_stage(&fs, args, &st, &e); break;
    }
    if (!ok) {
        out->is_error = true;
        out->error_kind = e.kind ? e.kind : K_APPLY;
        out->text = e.msg ? e.msg : xstrdup("Edit failed");
        out->details = json_obj();
        json_set_str(out->details, "diff", "");
        json_set_str(out->details, "errorKind", out->error_kind);
    } else {
        finish(&st, mode, o, clip, out);
    }
    hl_clip_free(clip);
    stageds_free(&st);
    fsrc_free(&fs);
}
