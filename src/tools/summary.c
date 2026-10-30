/* summary.c - structural code summaries for whole-file reads (declarations kept, bodies elided). */
#include "summary.h"

#include <ctype.h>

static bool ext_in(const char *ext, const char *const *list) {
    for (size_t i = 0; list[i]; i++)
        if (!strcasecmp(ext, list[i])) return true;
    return false;
}

static const char *const BRACE_EXT[] = {".c",  ".h",   ".cc",    ".cpp", ".cxx", ".hpp", ".hh",  ".js",  ".jsx", ".mjs",
                                        ".cjs", ".ts",  ".tsx",   ".mts", ".cts", ".java", ".go", ".rs",  ".cs",  ".swift",
                                        ".kt", ".kts", ".scala", ".php", ".dart", ".m",   ".mm",  ".zig", ".sol", ".proto",
                                        ".css", ".scss", ".less", ".groovy", ".gradle", NULL};
static const char *const INDENT_EXT[] = {".py", ".pyi", ".rb", ".nim", NULL};

summary_lang_t summary_lang(const char *path) {
    const char *e = path_ext(path);
    if (ext_in(e, BRACE_EXT)) return SUM_BRACE;
    if (ext_in(e, INDENT_EXT)) return SUM_INDENT;
    return SUM_NONE;
}

static bool blank(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s)) return false;
    return true;
}

static size_t indent_of(const char *s) {
    size_t n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += *s == '\t' ? 4 : 1;
    return n;
}

static bool starts_word(const char *s, const char *w) {
    size_t n = strlen(w);
    return strncmp(s, w, n) == 0 && !isalnum((unsigned char)s[n]) && s[n] != '_';
}

/* does an opener line introduce a container whose members should stay visible? */
static bool is_container(const char *line) {
    const char *s = line + strspn(line, " \t");
    static const char *const mods[] = {"export", "public", "private", "protected", "internal", "abstract", "static",
                                       "final", "sealed", "default", "pub", "pub(crate)", "unsafe", "partial", "data",
                                       "open", "declare", "typedef", NULL};
    for (int guard = 0; guard < 6; guard++) {
        bool hit = false;
        for (size_t i = 0; mods[i]; i++) {
            if (starts_word(s, mods[i])) {
                s += strlen(mods[i]);
                s += strspn(s, " \t");
                hit = true;
            }
        }
        if (!hit) break;
    }
    static const char *const kw[] = {"class", "struct", "interface", "impl", "trait", "enum", "namespace", "mod",
                                     "module", "object", "extension", "protocol", "union", "type", "record", NULL};
    for (size_t i = 0; kw[i]; i++)
        if (starts_word(s, kw[i])) return true;
    /* `extern "C" {` and object literals of exported consts are not containers */
    return false;
}

/* brace depth scan with string/comment awareness; returns depth at the start of each line */
static void brace_depths(const strvec_t *lines, int *depth) {
    int d = 0;
    bool in_block = false;
    for (size_t i = 0; i < lines->n; i++) {
        depth[i] = d;
        const char *p = lines->v[i];
        char q = 0;
        for (; *p; p++) {
            if (in_block) {
                if (p[0] == '*' && p[1] == '/') {
                    in_block = false;
                    p++;
                }
                continue;
            }
            if (q) {
                if (*p == '\\' && p[1]) p++;
                else if (*p == q) q = 0;
                continue;
            }
            if (p[0] == '/' && p[1] == '/') break;
            if (p[0] == '/' && p[1] == '*') {
                in_block = true;
                p++;
                continue;
            }
            if (*p == '"' || *p == '\'' || *p == '`') q = *p;
            else if (*p == '{') d++;
            else if (*p == '}') d = d > 0 ? d - 1 : 0;
        }
    }
}

void summary_build(const strvec_t *lines, summary_lang_t lang, bool *keep) {
    size_t n = lines->n;
    for (size_t i = 0; i < n; i++) keep[i] = true;
    if (lang == SUM_BRACE) {
        int *depth = xcalloc(n + 1, sizeof(int));
        brace_depths(lines, depth);
        /* container stack: depth at which members are visible */
        bool cont[256] = {0};
        cont[0] = true;
        for (size_t i = 0; i < n; i++) {
            int d = depth[i];
            int dnext = i + 1 < n ? depth[i + 1] : 0;
            bool visible = d < 256 && cont[d];
            keep[i] = visible;
            /* the closing brace line of a visible block stays visible */
            if (!visible && dnext < d && dnext < 256 && cont[dnext]) {
                const char *t = lines->v[i] + strspn(lines->v[i], " \t");
                if (*t == '}' || *t == ')' || *t == ']') keep[i] = true;
            }
            if (dnext > d && dnext < 256) {
                for (int k = d + 1; k <= dnext && k < 256; k++) cont[k] = visible && is_container(lines->v[i]);
            }
        }
        free(depth);
    } else if (lang == SUM_INDENT) {
        /* keep module- and class-level statements; elide function bodies (signatures kept whole) */
        VEC(size_t) cls_indent = {0};
        bool in_func = false, in_sig = false;
        size_t func_indent = 0;
        int paren = 0;
        for (size_t i = 0; i < n; i++) {
            const char *l = lines->v[i];
            if (in_sig) {
                keep[i] = true;
                for (const char *p = l; *p; p++) paren += *p == '(' || *p == '[' ? 1 : *p == ')' || *p == ']' ? -1 : 0;
                if (paren <= 0) in_sig = false;
                continue;
            }
            if (blank(l)) {
                keep[i] = !in_func;
                continue;
            }
            size_t ind = indent_of(l);
            const char *t = l + strspn(l, " \t");
            if (in_func && ind <= func_indent) in_func = false;
            while (cls_indent.n && ind <= vec_last(&cls_indent)) cls_indent.n--;
            if (in_func) {
                keep[i] = false;
                continue;
            }
            bool member_level = ind == 0 || (cls_indent.n && ind > vec_last(&cls_indent));
            keep[i] = member_level;
            bool is_func = starts_word(t, "def") || (starts_word(t, "async") && strstr(t, "def "));
            if (member_level && (is_func || starts_word(t, "class"))) {
                paren = 0;
                for (const char *p = t; *p; p++) paren += *p == '(' || *p == '[' ? 1 : *p == ')' || *p == ']' ? -1 : 0;
                if (paren > 0) in_sig = true;
            }
            if (starts_word(t, "class") && member_level) {
                vec_push(&cls_indent, ind);
            } else if (is_func && member_level) {
                in_func = true;
                func_indent = ind;
            }
        }
        vec_free(&cls_indent);
    }
    /* blank lines between elided regions add nothing */
    for (size_t i = 0; i < n; i++) {
        if (!keep[i] || !blank(lines->v[i])) continue;
        size_t a2 = i, b2 = i;
        while (a2 > 0 && blank(lines->v[a2 - 1])) a2--;
        while (b2 + 1 < n && blank(lines->v[b2 + 1])) b2++;
        bool prev_el = a2 > 0 && !keep[a2 - 1], next_el = b2 + 1 < n && !keep[b2 + 1];
        if (prev_el && next_el) for (size_t k = a2; k <= b2; k++) keep[k] = false;
    }
}
