/* magic.c - `edit.mode: magic` (see magic.h). Port of omp edit/magic.ts, magic-tools.ts, magic-calls.ts,
 * magic-view.ts and tui tools/magic-sections.ts at v19.1.1. Lengths the model or a limit sees are UTF-16 units, like
 * JS. */
#include "magic.h"

#include "ai/ai.h"
#include "config/auth.h"
#include "config/model_resolver.h"
#include "config/settings.h"
#include "core/fs.h"
#include "fuzzy.h"
#include "prompt/template.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================================
 * JS string helpers (String.prototype.trim / \s, ASCII \w, UTF-16 lengths)
 * ====================================================================================== */

/* bytes of the JS whitespace or line terminator at p, 0 if none */
static size_t ws_at(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || (c >= 9 && c <= 13)) return 1;
    if (c < 0x80) return 0;
    uint32_t cp;
    int n = utf8_decode(p, strnlen(p, 4), &cp);
    if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
        cp == 0x205F || cp == 0x3000 || cp == 0xFEFF)
        return (size_t)n;
    return 0;
}

static size_t ws_run(const char *p) {
    size_t k = 0, w;
    while ((w = ws_at(p + k))) k += w;
    return k;
}

static bool is_blank(const char *s) { return !s[ws_run(s)]; }

/* leading whitespace in UTF-16 units (every JS whitespace character is one unit) */
static int indent_of(const char *s) {
    int n = 0;
    size_t k = 0, w;
    while ((w = ws_at(s + k))) {
        k += w;
        n++;
    }
    return n;
}

/* end of s without trailing whitespace */
static const char *trim_end_ptr(const char *s) {
    const char *end = s, *p = s;
    while (*p) {
        size_t w = ws_at(p);
        if (w) {
            p += w;
            continue;
        }
        int n = 1;
        if ((unsigned char)*p >= 0x80) {
            uint32_t cp;
            n = utf8_decode(p, strnlen(p, 4), &cp);
            if (n <= 0) n = 1;
        }
        p += n;
        end = p;
    }
    return end;
}

static char *js_trim(const char *s) {
    const char *a = s + ws_run(s);
    return xstrndup(a, (size_t)(trim_end_ptr(a) - a));
}

/* /\s+/g -> " ", then trim */
static char *squeeze(const char *s) {
    sb_t b = SB_INIT;
    const char *p = s + ws_run(s);
    const char *end = trim_end_ptr(p);
    while (p < end) {
        size_t w = ws_at(p);
        if (w) {
            p += ws_run(p);
            sb_putc(&b, ' ');
            continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

/* every JS whitespace character removed (/\s+/g -> "") */
static char *strip_ws(const char *s) {
    sb_t b = SB_INIT;
    for (const char *p = s; *p;) {
        size_t w = ws_at(p);
        if (w) p += w;
        else sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

static bool word_ch(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool digit_ch(unsigned char c) { return c >= '0' && c <= '9'; }

/* bytes of the UTF-8 character at s (at most avail) */
static size_t u8len(const char *s, size_t avail) {
    unsigned char c = (unsigned char)*s;
    size_t k = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return k > avail ? (avail ? avail : 1) : k;
}

/* UTF-16 units of s[0..n) */
static size_t u16n(const char *s, size_t n) {
    size_t u = 0;
    for (size_t i = 0; i < n;) {
        size_t k = u8len(s + i, n - i);
        u += k == 4 ? 2 : 1;
        i += k;
    }
    return u;
}

/* byte offset `units` UTF-16 units after pos, clamped to len (a character is never split) */
static size_t u16_fwd(const char *s, size_t len, size_t pos, double units) {
    double u = 0;
    while (pos < len) {
        size_t k = u8len(s + pos, len - pos);
        double w = k == 4 ? 2 : 1;
        if (u + w > units) break;
        u += w;
        pos += k;
    }
    return pos;
}

/* byte offset `units` UTF-16 units before pos, clamped to 0 */
static size_t u16_back(const char *s, size_t pos, double units) {
    double u = 0;
    while (pos > 0) {
        size_t st = pos - 1;
        while (st > 0 && ((unsigned char)s[st] & 0xC0) == 0x80) st--;
        double w = pos - st == 4 ? 2 : 1;
        if (u + w > units) break;
        u += w;
        pos = st;
    }
    return pos;
}

/* bytes of the longest prefix of s that is at most `units` UTF-16 units */
static size_t utf16_prefix(const char *s, size_t units) { return u16_fwd(s, strlen(s), 0, (double)units); }

/* fileLines: split on "\n", without the empty field after a final newline */
static strvec_t file_lines(const char *content) {
    strvec_t v = {0};
    const char *p = content;
    for (;;) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            strvec_push(&v, p);
            break;
        }
        strvec_push_own(&v, xstrndup(p, (size_t)(nl - p)));
        p = nl + 1;
    }
    if (*content && str_ends(content, "\n")) free(vec_pop(&v));
    return v;
}

/* normalizeToLF: "\r\n" -> "\n" */
static char *to_lf(const char *s) { return strchr(s, '\r') ? str_replace(s, "\r\n", "\n") : xstrdup(s); }

/* is position i a line start for /m `^` (start, or after \n, \r, U+2028, U+2029) */
static bool line_start(const char *t, size_t i) {
    if (!i || t[i - 1] == '\n' || t[i - 1] == '\r') return true;
    return i >= 3 && (!strncmp(t + i - 3, "\xe2\x80\xa8", 3) || !strncmp(t + i - 3, "\xe2\x80\xa9", 3));
}

/* 1-based line of byte offset `at` (pre-edit newlines before it) */
static int line_number_at(const char *text, size_t at) {
    int line = 1;
    for (size_t i = 0; i < at && text[i]; i++) line += text[i] == '\n';
    return line;
}

/* ======================================================================================
 * sections (tui tools/magic-sections.ts, edit/magic.ts parseMagicInput)
 * ====================================================================================== */

/* PATH_HEADER /^# +(\S.*?)\s*$/: the path, or NULL */
static char *path_header(const char *line) {
    if (line[0] != '#' || line[1] != ' ') return NULL;
    const char *p = line + 1;
    while (*p == ' ') p++;
    if (!*p || ws_at(p)) return NULL;
    const char *end = trim_end_ptr(p);
    return xstrndup(p, (size_t)(end - p));
}

/* FENCE /^\s*```/ */
static bool fence_line(const char *line) { return str_starts(line + ws_run(line), "```"); }

void magic_split_sections(const char *input, magic_runs_t *runs, char **outside) {
    memset(runs, 0, sizeof(*runs));
    sb_t out = SB_INIT;
    char *norm = str_replace(input ? input : "", "\r\n", "\n");
    strvec_t lines = str_split(norm, "\n");
    if (!lines.n) strvec_push(&lines, "");
    sb_t cur = SB_INIT;
    bool have = false, fenced = false;
    for (size_t i = 0; i < lines.n; i++) {
        const char *line = lines.v[i];
        char *header = fenced ? NULL : path_header(line);
        if (header) {
            if (have) vec_last(runs).text = sb_detach(&cur);
            magic_run_t r = {header, NULL};
            vec_push(runs, r);
            have = true;
            continue;
        }
        if (fence_line(line)) fenced = !fenced;
        if (have) {
            sb_append(&cur, line);
            sb_putc(&cur, '\n');
        } else {
            sb_append(&out, line);
        }
    }
    if (have) vec_last(runs).text = sb_detach(&cur);
    sb_free(&cur);
    strvec_free(&lines);
    free(norm);
    if (outside) *outside = sb_detach(&out);
    sb_free(&out);
}

void magic_runs_free(magic_runs_t *r) {
    for (size_t i = 0; i < r->n; i++) {
        free(r->v[i].path);
        free(r->v[i].text);
    }
    vec_free(r);
}

void magic_edit_paths(const char *input, strvec_t *out) {
    magic_runs_t runs;
    magic_split_sections(input, &runs, NULL);
    for (size_t i = 0; i < runs.n; i++)
        if (!strvec_contains(out, runs.v[i].path)) strvec_push(out, runs.v[i].path);
    magic_runs_free(&runs);
}

bool magic_parse_input(const char *input, magic_sections_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    magic_runs_t runs;
    char *outside = NULL;
    magic_split_sections(input, &runs, &outside);
    bool ok = true;
    if (!runs.n) {
        *err = xstrdup("Give one `# path` line followed by its changes per file.");
        ok = false;
    } else if (!is_blank(outside)) {
        *err = xstrdup("Every change belongs to a file: put it under that file's `# path` line.");
        ok = false;
    }
    free(outside);
    if (!ok) {
        magic_runs_free(&runs);
        return false;
    }
    /* bodies per path, in first-seen order */
    strvec_t paths = {0};
    VEC(strvec_t) bodies = {0};
    for (size_t i = 0; i < runs.n; i++) {
        size_t k = 0;
        while (k < paths.n && strcmp(paths.v[k], runs.v[i].path)) k++;
        if (k == paths.n) {
            strvec_push(&paths, runs.v[i].path);
            strvec_t e = {0};
            vec_push(&bodies, e);
        }
        strvec_push_own(&bodies.v[k], js_trim(runs.v[i].text));
    }
    for (size_t k = 0; k < paths.n && ok; k++) {
        sb_t b = SB_INIT;
        for (size_t j = 0; j < bodies.v[k].n; j++) {
            if (!*bodies.v[k].v[j]) continue;
            if (b.len) sb_append(&b, "\n\n");
            sb_append(&b, bodies.v[k].v[j]);
        }
        if (!b.len) {
            *err = xasprintf("No change is described for %s.", paths.v[k]);
            ok = false;
            sb_free(&b);
            break;
        }
        magic_section_t s = {xstrdup(paths.v[k]), sb_detach(&b)};
        vec_push(out, s);
    }
    for (size_t k = 0; k < bodies.n; k++) strvec_free(&bodies.v[k]);
    vec_free(&bodies);
    strvec_free(&paths);
    magic_runs_free(&runs);
    if (!ok) magic_sections_free(out);
    return ok;
}

void magic_sections_free(magic_sections_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].path);
        free(s->v[i].change);
    }
    vec_free(s);
}

/* ======================================================================================
 * initial file views (edit/magic-view.ts)
 * ====================================================================================== */

#define CONTEXT_LINES 12
#define FALLBACK_HEAD_LINES 40
#define FALLBACK_TAIL_LINES 20
#define MAX_BLOCK_LINES 150
#define MAX_DISTINCT_HITS 5
#define NEAR_LINES 3
#define REPEAT_SAMPLE 3
#define MIN_FILE_LINES 200
#define MIN_OMITTED_SHARE 0.5
#define MIN_OMITTED_GAP 8

#define GUTTER "\xe2\x94\x82"   /* │ */
#define ELLIPSIS "\xe2\x80\xa6" /* … */
#define OMITTED "\xe2\x8b\xae"  /* ⋮ */

typedef struct {
    int from, to; /* inclusive, zero-based */
} span_t;
typedef VEC(span_t) spans_t;
typedef VEC(int) ints_t;

/* numberedLines: lines[from..to] as `N│text` (N = index + offset + 1) within `limit` UTF-16 units (limit < 0: none). A
 * first line over the budget is cut with `…`, so a read always makes progress. */
static void numbered_lines(char *const *lines, int from, int to, double limit, int offset, strvec_t *out) {
    size_t start = out->n;
    bool unlimited = limit < 0;
    for (int i = from; i <= to; i++) {
        char num[24];
        int nl = snprintf(num, sizeof num, "%d", i + offset + 1);
        const char *line = lines[i];
        if (!unlimited) {
            double plen = (double)nl + 1; /* `│` is one unit */
            double llen = (double)fz_utf16_len(line);
            double remaining = limit - plen;
            if (llen > remaining) {
                if (out->n == start && remaining > 1) {
                    size_t keep = utf16_prefix(line, (size_t)(remaining - 1));
                    strvec_push_own(out, xasprintf("%s" GUTTER "%.*s" ELLIPSIS, num, (int)keep, line));
                }
                break;
            }
            limit -= plen + llen + 1;
        }
        strvec_push_own(out, xasprintf("%s" GUTTER "%s", num, line));
    }
}

/* OPENER /[[({]\s*$/ */
static bool opener_end(const char *s) {
    const char *te = trim_end_ptr(s);
    return te > s && (te[-1] == '[' || te[-1] == '(' || te[-1] == '{');
}

/* CLOSER /^[\])}]/ on the trimmed line */
static bool closer_start(const char *s) {
    const char *b = s + ws_run(s);
    return *b == ']' || *b == ')' || *b == '}';
}

/* blockEnd: last line of the block lines[start] opens (its deeper-indented lines and their closer) */
static int block_end(char *const *lines, int n, int start, int max_lines) {
    const char *first = lines[start];
    /* blank lines and pure closing lines never open a block */
    if (is_blank(first) || (closer_start(first) && !opener_end(first))) return start;
    int indent = indent_of(first);
    int limit = n - start > max_lines ? start + max_lines : n;
    int end = start;
    for (int i = start + 1; i < limit; i++) {
        const char *line = lines[i];
        if (is_blank(line)) continue;
        int depth = indent_of(line);
        if (depth > indent) {
            end = i;
            continue;
        }
        if (depth < indent || end == start) break;
        /* a bare `{` opens the body of a multi-line header */
        const char *b = line + ws_run(line);
        if (*b == '{' && trim_end_ptr(b) == b + 1) {
            const char *pe = trim_end_ptr(lines[end]);
            if (!(pe > lines[end] && pe[-1] == ';')) {
                end = i;
                continue;
            }
        }
        if (!closer_start(line)) break;
        end = i;
        /* `} else {` closes one block and opens the next */
        if (!opener_end(line)) break;
    }
    return end;
}

/* add(): split on /\.{3}|…/, squeeze, keep fragments with at least three ASCII word characters */
static void add_fragment(strvec_t *found, const char *text) {
    const char *p = text;
    for (;;) {
        const char *dots = strstr(p, "..."), *ell = strstr(p, ELLIPSIS);
        const char *cut = dots && (!ell || dots < ell) ? dots : ell;
        size_t len = cut ? (size_t)(cut - p) : strlen(p);
        char *part = xstrndup(p, len);
        char *frag = squeeze(part);
        free(part);
        int words = 0;
        for (const char *q = frag; *q; q++) words += word_ch((unsigned char)*q);
        if (words >= 3 && !strvec_contains(found, frag)) strvec_push_own(found, frag);
        else free(frag);
        if (!cut) break;
        p = cut + 3;
    }
}

/* CODE_SPAN /`([^`\n]+)`/g: next span after *pos (inner [*a, *b)), false when none */
static bool next_code_span(const char *s, size_t *pos, size_t *a, size_t *b) {
    for (size_t i = *pos; s[i]; i++) {
        if (s[i] != '`') continue;
        size_t j = i + 1;
        while (s[j] && s[j] != '`' && s[j] != '\n') j++;
        if (s[j] == '`' && j > i + 1) {
            *a = i + 1;
            *b = j;
            *pos = j + 1;
            return true;
        }
    }
    return false;
}

static bool id_start(unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'; }
static bool id_char(unsigned char c) { return word_ch(c) || c == '$'; }
static bool upper_ch(unsigned char c) { return c >= 'A' && c <= 'Z'; }

/* IDENTIFIER at s[i]: camelCase | snake_case | dotted / `::`-qualified name; match end or 0 */
static size_t identifier_at(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (!id_start(c)) return 0;
    size_t r = i + 1;
    while (id_char((unsigned char)s[r])) r++;
    /* [A-Za-z_$][\w$]*[a-z0-9][A-Z][\w$]* */
    for (size_t k = i + 1; k + 1 < r; k++) {
        unsigned char x = (unsigned char)s[k], y = (unsigned char)s[k + 1];
        if (((x >= 'a' && x <= 'z') || digit_ch(x)) && upper_ch(y)) return r;
    }
    /* [A-Za-z$][\w$]*_[\w$]+ */
    if (c != '_')
        for (size_t k = i + 1; k + 1 < r; k++)
            if (s[k] == '_') return r;
    /* [A-Za-z_$][\w$]*(?:(?:\.|::)[A-Za-z_$][\w$]*)+ */
    size_t e = r;
    bool group = false;
    for (;;) {
        size_t sep = s[e] == '.' ? 1 : (s[e] == ':' && s[e + 1] == ':') ? 2 : 0;
        if (!sep || !id_start((unsigned char)s[e + sep])) break;
        e += sep + 1;
        while (id_char((unsigned char)s[e])) e++;
        group = true;
    }
    return group ? e : 0;
}

/* QUALIFIED_SCOPE /^(?:[A-Za-z_$][\w$]*::)+([A-Za-z_$][\w$]*)$/, else QUALIFIED_TYPE_MEMBER
 * /^[A-Z][\w$]*\.([A-Za-z_$][\w$]*)$/: the member (borrowed from s) or NULL */
static const char *qualified_member(const char *s) {
    size_t i = 0;
    int groups = 0;
    for (;;) {
        if (!id_start((unsigned char)s[i])) break;
        size_t st = i++;
        while (id_char((unsigned char)s[i])) i++;
        if (s[i] == ':' && s[i + 1] == ':') {
            groups++;
            i += 2;
            continue;
        }
        if (!s[i] && groups) return s + st;
        break;
    }
    if (!upper_ch((unsigned char)s[0])) return NULL;
    i = 1;
    while (id_char((unsigned char)s[i])) i++;
    if (s[i] != '.' || !id_start((unsigned char)s[i + 1])) return NULL;
    size_t st = ++i;
    i++;
    while (id_char((unsigned char)s[i])) i++;
    return s[i] ? NULL : s + st;
}

/* DISTINCTIVE_MEMBER /[a-z0-9][A-Z]|_|^[A-Z][A-Z0-9_]+$/ */
static bool distinctive_member(const char *m) {
    for (size_t k = 0; m[k]; k++) {
        unsigned char x = (unsigned char)m[k];
        if (x == '_') return true;
        if (((x >= 'a' && x <= 'z') || digit_ch(x)) && upper_ch((unsigned char)m[k + 1])) return true;
    }
    if (!upper_ch((unsigned char)m[0]) || !m[1]) return false;
    for (size_t k = 1; m[k]; k++)
        if (!upper_ch((unsigned char)m[k]) && !digit_ch((unsigned char)m[k]) && m[k] != '_') return false;
    return true;
}

/* addQualified: `Line::blank` and `Parser.parseLine` name `blank(` / `parseLine(`, and a distinctive member itself */
static void add_qualified(strvec_t *found, const char *name) {
    char *t = js_trim(name);
    char *paren = strchr(t, '(');
    if (paren) *paren = 0; /* replace(/\(.*$/, "") on a one-line name */
    const char *member = qualified_member(t);
    if (member) {
        char *call = xasprintf("%s(", member);
        add_fragment(found, call);
        free(call);
        if (distinctive_member(member)) add_fragment(found, member);
    }
    free(t);
}

/* namedFragments: code spans, whole lines and identifiers in prose that may quote the file */
static void named_fragments(const char *change, strvec_t *found) {
    strvec_t lines = str_split(change, "\n");
    for (size_t i = 0; i < lines.n; i++) add_fragment(found, lines.v[i]);
    strvec_free(&lines);
    size_t pos = 0, a, b;
    while (next_code_span(change, &pos, &a, &b)) {
        char *span = xstrndup(change + a, b - a);
        add_fragment(found, span);
        add_qualified(found, span);
        /* `drain()` names `function drain(queue)` too */
        if (str_ends(span, ")")) {
            const char *paren = strchr(span, '(');
            char *head = xstrndup(span, paren ? (size_t)(paren - span) + 1 : 0);
            add_fragment(found, head);
            free(head);
        }
        free(span);
    }
    /* identifiers in the prose outside code spans */
    sb_t prose = SB_INIT;
    size_t last = 0;
    pos = 0;
    while (next_code_span(change, &pos, &a, &b)) {
        sb_appendn(&prose, change + last, a - 1 - last);
        sb_putc(&prose, ' ');
        last = b + 1;
    }
    sb_append(&prose, change + last);
    const char *p = sb_cstr(&prose);
    for (size_t i = 0; p[i];) {
        size_t e = identifier_at(p, i);
        if (e) {
            char *name = xstrndup(p + i, e - i);
            add_fragment(found, name);
            add_qualified(found, name);
            free(name);
            i = e;
        } else {
            i++;
        }
    }
    sb_free(&prose);
}

static size_t match_ci(const char *p, const char *lit) {
    size_t n = strlen(lit);
    return strncasecmp(p, lit, n) == 0 ? n : 0;
}

static double digits_value(const char *s, size_t *len) {
    size_t n = 0;
    while (digit_ch((unsigned char)s[n])) n++;
    char *t = xstrndup(s, n);
    double v = strtod(t, NULL);
    free(t);
    *len = n;
    return v;
}

/* LINE_SPAN /(\d+)(?:\s*(?:-|–|to|through)\s*(\d+))?/g over one captured reference */
static void line_spans(const char *s, size_t len, int nlines, spans_t *out) {
    char *t = xstrndup(s, len);
    for (size_t k = 0; t[k];) {
        if (!digit_ch((unsigned char)t[k])) {
            k++;
            continue;
        }
        size_t n;
        double first = digits_value(t + k, &n), last = first;
        k += n;
        size_t r = k + ws_run(t + k), sep = 0;
        if (t[r] == '-') sep = 1;
        else if (!strncmp(t + r, "\xe2\x80\x93", 3)) sep = 3;
        else if (!strncmp(t + r, "to", 2)) sep = 2;
        else if (!strncmp(t + r, "through", 7)) sep = 7;
        if (sep) {
            r += sep;
            r += ws_run(t + r);
            if (digit_ch((unsigned char)t[r])) {
                last = digits_value(t + r, &n);
                k = r + n;
            }
        }
        double from = fmin(first, last), to = fmax(first, last);
        if (from >= 1 && from <= nlines) {
            span_t sp = {(int)from - 1, (int)fmin(to, nlines) - 1};
            vec_push(out, sp);
        }
    }
    free(t);
}

/* numberedSpans: LINE_REFERENCE /\b(?:lines?|lns?)\s+(\d+(?:\s*(?:,|-|–|to|through|and)\s*\d+)*)/gi */
static void numbered_spans(const char *change, int nlines, spans_t *out) {
    for (size_t i = 0; change[i];) {
        size_t word = (i == 0 || !word_ch((unsigned char)change[i - 1])) ? (match_ci(change + i, "line") ? 4 : match_ci(change + i, "ln") ? 2 : 0) : 0;
        if (word) {
            size_t p = i + word;
            if (change[p] == 's' || change[p] == 'S') p++;
            size_t w = ws_run(change + p);
            if (w && digit_ch((unsigned char)change[p + w])) {
                size_t cap = p + w, q = cap;
                while (digit_ch((unsigned char)change[q])) q++;
                for (;;) {
                    size_t r = q + ws_run(change + q), s;
                    if (change[r] == ',' || change[r] == '-') s = 1;
                    else if (!strncmp(change + r, "\xe2\x80\x93", 3)) s = 3;
                    else if (!(s = match_ci(change + r, "to")) && !(s = match_ci(change + r, "through")) &&
                             !(s = match_ci(change + r, "and")))
                        break;
                    r += s;
                    r += ws_run(change + r);
                    if (!digit_ch((unsigned char)change[r])) break;
                    while (digit_ch((unsigned char)change[r])) r++;
                    q = r;
                }
                line_spans(change + cap, q - cap, nlines, out);
                i = q;
                continue;
            }
        }
        i++;
    }
}

/* namedSpans: the lines the change quotes or names, each with the block it opens */
static void named_spans(char *const *lines, int n, const char *change, spans_t *out) {
    strvec_t frags = {0};
    named_fragments(change, &frags);
    char **sq = xcalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) sq[i] = squeeze(lines[i]);
    VEC(ints_t) hits = {0};
    bool few = true;
    for (size_t f = 0; f < frags.n; f++) {
        ints_t found = {0};
        for (int i = 0; i < n; i++)
            if (strstr(sq[i], frags.v[f])) vec_push(&found, i);
        if (!found.n) continue;
        if (found.n > MAX_DISTINCT_HITS) few = false;
        vec_push(&hits, found);
    }
#define SPAN_OF(line) ((span_t){(line), block_end(lines, n, (line), MAX_BLOCK_LINES)})
    if (few) {
        for (size_t h = 0; h < hits.n; h++)
            for (size_t k = 0; k < hits.v[h].n; k++) vec_push(out, SPAN_OF(hits.v[h].v[k]));
    } else {
        /* fragments with a hit on each line, and with a hit whose block reaches it */
        ints_t *on = xcalloc((size_t)n + 1, sizeof(ints_t)), *reaching = xcalloc((size_t)n + 1, sizeof(ints_t));
        for (size_t h = 0; h < hits.n; h++)
            for (size_t k = 0; k < hits.v[h].n; k++) {
                int line = hits.v[h].v[k];
                vec_push(&on[line], (int)h);
                int from = line - NEAR_LINES < 0 ? 0 : line - NEAR_LINES;
                int to = block_end(lines, n, line, MAX_BLOCK_LINES) + NEAR_LINES;
                if (to > n - 1) to = n - 1;
                for (int i = from; i <= to; i++) vec_push(&reaching[i], (int)h);
            }
        for (size_t h = 0; h < hits.n; h++) {
            ints_t *found = &hits.v[h];
            if (found->n <= MAX_DISTINCT_HITS) {
                for (size_t k = 0; k < found->n; k++) vec_push(out, SPAN_OF(found->v[k]));
                continue;
            }
            /* a fragment that repeats names its copies in or beside the block of another named line */
            ints_t together = {0};
            for (size_t k = 0; k < found->n; k++) {
                int line = found->v[k];
                bool keep = false;
                for (size_t j = 0; j < reaching[line].n && !keep; j++) keep = reaching[line].v[j] != (int)h;
                if (!keep) {
                    int from = line - NEAR_LINES < 0 ? 0 : line - NEAR_LINES;
                    int to = block_end(lines, n, line, MAX_BLOCK_LINES) + NEAR_LINES;
                    if (to > n - 1) to = n - 1;
                    for (int i = from; i <= to && !keep; i++)
                        for (size_t j = 0; j < on[i].n && !keep; j++) keep = on[i].v[j] != (int)h;
                }
                if (keep) vec_push(&together, line);
            }
            if (together.n)
                for (size_t k = 0; k < together.n; k++) vec_push(out, SPAN_OF(together.v[k]));
            else
                for (size_t k = 0; k < found->n && k < REPEAT_SAMPLE; k++) vec_push(out, SPAN_OF(found->v[k]));
            vec_free(&together);
        }
        for (int i = 0; i < n; i++) {
            vec_free(&on[i]);
            vec_free(&reaching[i]);
        }
        free(on);
        free(reaching);
    }
#undef SPAN_OF
    for (size_t h = 0; h < hits.n; h++) vec_free(&hits.v[h]);
    vec_free(&hits);
    for (int i = 0; i < n; i++) free(sq[i]);
    free(sq);
    strvec_free(&frags);
}

/* stable sort by `from` (JS Array.prototype.sort is stable) */
static void spans_sort(spans_t *s) {
    for (size_t i = 1; i < s->n; i++) {
        span_t t = s->v[i];
        size_t j = i;
        while (j > 0 && s->v[j - 1].from > t.from) {
            s->v[j] = s->v[j - 1];
            j--;
        }
        s->v[j] = t;
    }
}

/* excerpt: the numbered lines the change names, or false when it names nothing or too much of a file this short */
static bool excerpt(char *const *lines, int n, const char *change, magic_view_t *view) {
    int last = n - 1;
    if (n < MIN_FILE_LINES) return false;
    spans_t named = {0};
    numbered_spans(change, n, &named);
    named_spans(lines, n, change, &named);
    if (!named.n) {
        vec_free(&named);
        return false;
    }
    spans_t spans = {0};
    for (size_t i = 0; i < named.n; i++) {
        span_t s = {named.v[i].from - CONTEXT_LINES < 0 ? 0 : named.v[i].from - CONTEXT_LINES,
                    named.v[i].to + CONTEXT_LINES > last ? last : named.v[i].to + CONTEXT_LINES};
        vec_push(&spans, s);
    }
    vec_free(&named);
    /* the head and tail anchor changes placed by position: imports, a new last function */
    span_t head = {0, CONTEXT_LINES - 1}, tail = {last - CONTEXT_LINES + 1, last};
    vec_push(&spans, head);
    vec_push(&spans, tail);
    spans_sort(&spans);
    spans_t shown = {0};
    for (size_t i = 0; i < spans.n; i++) {
        /* small gaps cost as much to mark as to show */
        if (shown.n && spans.v[i].from <= vec_last(&shown).to + MIN_OMITTED_GAP + 1) {
            if (spans.v[i].to > vec_last(&shown).to) vec_last(&shown).to = spans.v[i].to;
        } else {
            vec_push(&shown, spans.v[i]);
        }
    }
    vec_free(&spans);
    long count = 0;
    for (size_t i = 0; i < shown.n; i++) count += shown.v[i].to - shown.v[i].from + 1;
    if ((double)(n - count) < n * MIN_OMITTED_SHARE) {
        vec_free(&shown);
        return false;
    }
    /* renderExcerpt */
    strvec_t out = {0};
    int next = 0;
    for (size_t i = 0; i < shown.n; i++) {
        if (shown.v[i].from > next) strvec_push_own(&out, xasprintf(OMITTED " lines %d-%d not shown", next + 1, shown.v[i].from));
        numbered_lines(lines, shown.v[i].from, shown.v[i].to, -1, 0, &out);
        next = shown.v[i].to + 1;
    }
    if (next < n) strvec_push_own(&out, xasprintf(OMITTED " lines %d-%d not shown", next + 1, n));
    vec_free(&shown);
    view->text = strvec_join(&out, "\n");
    strvec_free(&out);
    view->total_lines = n;
    view->partial = true;
    return true;
}

void magic_file_views(const char *content, const char *change, double limit, magic_views_t *out) {
    memset(out, 0, sizeof(*out));
    strvec_t lines = file_lines(content);
    int n = (int)lines.n;
    bool unlimited = limit < 0;
    magic_view_t part = {0};
    if (excerpt(lines.v, n, change, &part)) {
        if (unlimited || (double)fz_utf16_len(part.text) <= limit) vec_push(out, part);
        else free(part.text);
    }
    if (unlimited || (double)fz_utf16_len(content) <= limit) {
        strvec_t whole = {0};
        numbered_lines(lines.v, 0, n - 1, -1, 0, &whole);
        char *text = strvec_join(&whole, "\n");
        strvec_free(&whole);
        if (unlimited || (double)fz_utf16_len(text) <= limit) {
            magic_view_t w = {text, n, false};
            vec_push(out, w);
        } else {
            free(text);
        }
    }
    if (!out->n) {
        /* nothing named fits: head and tail, the model opens the rest with read and search */
        strvec_t h = {0}, t = {0};
        numbered_lines(lines.v, 0, (n < FALLBACK_HEAD_LINES ? n : FALLBACK_HEAD_LINES) - 1, limit / 2, 0, &h);
        int tail_from = n - FALLBACK_TAIL_LINES > FALLBACK_HEAD_LINES ? n - FALLBACK_TAIL_LINES : FALLBACK_HEAD_LINES;
        numbered_lines(lines.v, tail_from, n - 1, limit / 2, 0, &t);
        char *head = strvec_join(&h, "\n"), *tail = strvec_join(&t, "\n");
        sb_t b = SB_INIT;
        const char *parts[3] = {head, OMITTED " Open more of the file with read or search.", tail};
        for (int i = 0; i < 3; i++) {
            if (!*parts[i]) continue;
            if (b.len) sb_putc(&b, '\n');
            sb_append(&b, parts[i]);
        }
        free(head);
        free(tail);
        strvec_free(&h);
        strvec_free(&t);
        char *text = sb_detach(&b);
        if ((double)fz_utf16_len(text) > limit) {
            free(text);
            text = xstrdup("");
        }
        magic_view_t f = {text, n, true};
        vec_push(out, f);
    }
    strvec_free(&lines);
}

void magic_views_free(magic_views_t *v) {
    for (size_t i = 0; i < v->n; i++) free(v->v[i].text);
    vec_free(v);
}

/* ======================================================================================
 * draft tools (edit/magic-tools.ts)
 * ====================================================================================== */

#define MAX_READ_LINES 160
#define MAX_TEXT_CHARS 8000
#define MAX_SEARCH_HITS 20
#define REPLACE_CONTEXT_LINES 2
#define MAX_REPLACE_PREVIEW_LINES 9
#define MAX_REPLACE_PREVIEW_CHARS 1600
#define MAX_REPLACE_MATCH_LINES 10

/* APPLY_TOOLS, as omptype turns their schemas into wire JSON */
json_t *magic_apply_tools(void) {
    return json_parse_str(
        "[{\"name\":\"read\",\"description\":\"Read numbered lines from the current draft.\",\"parameters\":{\"type\":"
        "\"object\",\"properties\":{\"start_line\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"First line, "
        "1-based.\"},\"end_line\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Last line, inclusive.\"}},"
        "\"required\":[\"start_line\",\"end_line\"],\"additionalProperties\":false}},"
        "{\"name\":\"search\",\"description\":\"Find lines containing a literal substring in the current draft.\","
        "\"parameters\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,"
        "\"description\":\"Literal substring to find.\"}},\"required\":[\"query\"],\"additionalProperties\":false}},"
        "{\"name\":\"replace\",\"description\":\"Replace exact text in the current draft.\",\"parameters\":{\"type\":"
        "\"object\",\"properties\":{\"old_string\":{\"type\":\"string\",\"minLength\":1,\"description\":\"Exact text, "
        "including any whitespace you want to match.\"},\"new_string\":{\"type\":\"string\",\"description\":"
        "\"Replacement text; empty to delete.\"},\"replace_all\":{\"type\":\"boolean\",\"description\":\"Default false. "
        "Set true only to replace every occurrence in the draft.\"}},\"required\":[\"old_string\",\"new_string\"],"
        "\"additionalProperties\":false}}]");
}

#define READ_START_DESC "First line, 1-based."
#define READ_END_DESC "Last line, inclusive."
#define SEARCH_QUERY_DESC "Literal substring to find."
#define REPLACE_OLD_DESC "Exact text, including any whitespace you want to match."
#define REPLACE_NEW_DESC "Replacement text; empty to delete."
#define REPLACE_ALL_DESC "Default false. Set true only to replace every occurrence in the draft."

/* ---------- omptype validation failures (errors.ts) ---------- */

typedef struct {
    const char *path;     /* the property */
    const char *expected; /* `.describe()` replaces the expectation */
    char *actual;         /* describeValue; NULL: missing */
    double num;           /* the failing number (the summary's shared-data form) */
    bool is_num;
} vfail_t;
typedef VEC(vfail_t) vfails_t;

/* describeKind */
static char *describe_kind(const json_t *v) {
    switch (v->type) {
    case JSON_NULL: return xstrdup("null");
    case JSON_BOOL: return xstrdup("boolean");
    case JSON_NUMBER: return xstrdup(isnan(v->u.num.d) ? "NaN" : "a number");
    case JSON_STRING: return xstrdup("a string");
    default: return xstrdup("an object");
    }
}

/* describeValue for a non-domain failure */
static char *describe_value(const json_t *v) {
    sb_t b = SB_INIT;
    switch (v->type) {
    case JSON_NULL: sb_append(&b, "null"); break;
    case JSON_BOOL: sb_append(&b, v->u.b ? "true" : "false"); break;
    case JSON_NUMBER: json_js_num_sb(&b, v->u.num.d); break;
    case JSON_STRING:
        if (u16n(v->u.str.s, v->u.str.len) <= 40) json_js_quote_sb(&b, v->u.str.s, v->u.str.len);
        else sb_appendf(&b, "a string (length %zu)", u16n(v->u.str.s, v->u.str.len));
        break;
    case JSON_ARRAY: sb_append(&b, "an array"); break;
    default: json_dump_js_sb(&b, v); break;
    }
    return sb_detach(&b);
}

static void vfail_push(vfails_t *f, const char *path, const char *expected, char *actual, const json_t *v) {
    vfail_t e = {path, expected, actual, json_is_num(v) ? v->u.num.d : 0, json_is_num(v)};
    vec_push(f, e);
}

/* OmpErrors.summary: one message per failure, or `path (data) must be...` when every failure shares path and data */
static char *vsummary(vfails_t *f) {
    sb_t b = SB_INIT;
    bool shared = f->n > 1;
    for (size_t i = 1; i < f->n && shared; i++)
        shared = !strcmp(f->v[i].path, f->v[0].path) && f->v[i].is_num && f->v[0].is_num && f->v[i].num == f->v[0].num;
    if (shared) {
        sb_appendf(&b, "%s (", f->v[0].path);
        json_js_num_sb(&b, f->v[0].num);
        sb_append(&b, ") must be...");
        for (size_t i = 0; i < f->n; i++) sb_appendf(&b, "\n  \xe2\x97\xa6 %s", f->v[i].expected);
    } else {
        for (size_t i = 0; i < f->n; i++) {
            if (i) sb_putc(&b, '\n');
            if (!f->v[i].actual) sb_appendf(&b, "%s must be %s (was missing)", f->v[i].path, f->v[i].expected);
            else if (!*f->v[i].actual) sb_appendf(&b, "%s must be %s", f->v[i].path, f->v[i].expected);
            else sb_appendf(&b, "%s must be %s (was %s)", f->v[i].path, f->v[i].expected, f->v[i].actual);
        }
    }
    for (size_t i = 0; i < f->n; i++) free(f->v[i].actual);
    vec_free(f);
    return sb_detach(&b);
}

/* `number.integer >= 1` */
static void v_line(vfails_t *f, const json_t *args, const char *key, const char *desc) {
    const json_t *v = json_get(args, key);
    if (!v) {
        vfail_push(f, key, desc, NULL, NULL);
        return;
    }
    if (!json_is_num(v) || !isfinite(v->u.num.d)) { /* "an integer": a divisor failure shows the value */
        vfail_push(f, key, desc, describe_value(v), v);
        return;
    }
    double d = v->u.num.d;
    if (d != floor(d)) vfail_push(f, key, desc, describe_value(v), v);
    if (d < 1) vfail_push(f, key, desc, describe_value(v), v);
}

/* `string` (nonempty: `string > 0`) */
static void v_string(vfails_t *f, const json_t *args, const char *key, const char *desc, bool nonempty) {
    const json_t *v = json_get(args, key);
    if (!v) vfail_push(f, key, desc, NULL, NULL);
    else if (!json_is_str(v)) vfail_push(f, key, desc, describe_kind(v), NULL); /* "a string": a domain failure */
    else if (nonempty && !v->u.str.len) vfail_push(f, key, desc, xstrdup("0"), NULL); /* the length is the data */
}

/* ---------- replace ---------- */

void magic_steps_free(magic_steps_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].old_s);
        free(s->v[i].rep);
    }
    vec_free(s);
}

/* spliceReplacement: the edit replayed literally (every match for replace_all) */
static char *splice_replacement(const char *text, const magic_step_t *st) {
    size_t ol = strlen(st->old_s);
    if (!st->all) return xasprintf("%.*s%s%s", (int)st->at, text, st->rep, text + st->at + ol);
    sb_t b = SB_INIT;
    const char *p = text, *m;
    while ((m = strstr(p, st->old_s))) {
        sb_appendn(&b, p, (size_t)(m - p));
        sb_append(&b, st->rep);
        p = m + ol;
    }
    sb_append(&b, p);
    return sb_detach(&b);
}

/* draftAnchor: numbered draft lines of a 1-based inclusive window, bounded for previews */
static char *draft_anchor(char *const *lines, int nlines, int from, int to) {
    int start = from - 1;
    int end = nlines < to ? nlines : to;
    if (start + MAX_READ_LINES < end) end = start + MAX_READ_LINES;
    strvec_t out = {0};
    numbered_lines(lines, start, end - 1, MAX_TEXT_CHARS, 0, &out);
    char *j = strvec_join(&out, "\n");
    bool more = (int)out.n < to - from + 1;
    strvec_free(&out);
    if (!more) return j;
    char *r = xasprintf("%s\n" OMITTED " Use read for more draft lines.", j);
    free(j);
    return r;
}

/* findGutterMatch: `old` located with the line-number gutter it copied from a view removed; unique matches only */
static bool find_gutter_match(const char *text, const char *old, size_t *at_out, size_t *len_out) {
    /* /^(?:\d*│|\d*\t)/m: some line of old starts with a gutter or a tab */
    bool any = false;
    for (size_t i = 0; old[i] && !any; i++) {
        if (!line_start(old, i)) continue;
        size_t k = i;
        while (digit_ch((unsigned char)old[k])) k++;
        any = str_starts(old + k, GUTTER) || old[k] == '\t';
    }
    if (!any) return false;
    strvec_t lines = str_split(old, "\n");
    size_t n = lines.n;
    const char **lit = xcalloc(n ? n : 1, sizeof(char *));
    bool *tab = xcalloc(n ? n : 1, sizeof(bool));
    bool boundary = false;
    for (size_t i = 0; i < n; i++) {
        const char *line = lines.v[i];
        /* EXPLICIT_GUTTER /^\d*│|^\d+\t/ */
        size_t k = 0, glen = 0;
        while (digit_ch((unsigned char)line[k])) k++;
        if (str_starts(line + k, GUTTER)) glen = k + 3;
        else if (k && line[k] == '\t') glen = k + 1;
        /* a bare tab may be real indentation on one line and leaked on the next */
        tab[i] = !glen && line[0] == '\t';
        if (i == 0 && (glen || tab[i])) boundary = true;
        lit[i] = line + (glen ? glen : tab[i] ? 1 : 0);
    }
    size_t tlen = strlen(text);
    size_t found = 0, flen = 0;
    int matches = 0;
    for (size_t start = 0; start <= tlen && matches < 2; start++) {
        if (boundary && start && text[start - 1] != '\n') continue;
        if (matches == 1 && start < found + 1) continue;
        size_t pos = start;
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
            size_t ll = strlen(lit[i]);
            bool last = i + 1 == n;
            /* `\t?` is greedy: the tab is taken when the rest of the line still matches */
            if (tab[i] && pos < tlen && text[pos] == '\t' && tlen - pos - 1 >= ll && !memcmp(text + pos + 1, lit[i], ll) &&
                (last || text[pos + 1 + ll] == '\n')) {
                pos += 1 + ll;
            } else if (tlen - pos >= ll && !memcmp(text + pos, lit[i], ll) && (last || text[pos + ll] == '\n')) {
                pos += ll;
            } else {
                ok = false;
                break;
            }
            if (!last) pos++;
        }
        if (!ok) continue;
        if (!matches) {
            found = start;
            flen = pos - start;
            if (!flen) break; /* a zero-length match locates nothing */
        }
        matches++;
    }
    free(lit);
    free(tab);
    strvec_free(&lines);
    if (matches != 1 || !flen) return false;
    *at_out = found;
    *len_out = flen;
    return true;
}

/* whitespaceLead: a unique whitespace-insensitive match, mapped back to the draft lines it covers (owned) or NULL */
static char *whitespace_lead(const char *text, const char *old) {
    char *compact = strip_ws(old);
    if (!*compact) {
        free(compact);
        return NULL;
    }
    char *flat = strip_ws(text);
    const char *hit = strstr(flat, compact);
    char *r = NULL;
    if (hit && !strstr(hit + 1, compact)) {
        size_t at = (size_t)(hit - flat), clen = strlen(compact);
        strvec_t lines = file_lines(text);
        size_t offset = 0;
        int from = 0;
        for (size_t i = 0; i < lines.n; i++) {
            char *s = strip_ws(lines.v[i]);
            offset += strlen(s);
            free(s);
            if (from == 0 && offset > at) from = (int)i + 1;
            if (offset >= at + clen) {
                r = draft_anchor(lines.v, (int)lines.n, from, (int)i + 1);
                break;
            }
        }
        strvec_free(&lines);
    }
    free(flat);
    free(compact);
    return r;
}

/* missingAnchor: the likeliest reason an exact old_string lookup failed */
static char *missing_anchor(const char *text, const char *old, const char *rep) {
    size_t gat, glen;
    if (find_gutter_match(text, old, &gat, &glen)) {
        strvec_t lines = file_lines(text);
        char *a = draft_anchor(lines.v, (int)lines.n, line_number_at(text, gat), line_number_at(text, gat + glen - 1));
        char *r = xasprintf("old_string not found. The line-number gutter was copied. Remove it from both strings and copy "
                            "only the text after " GUTTER ":\n%s",
                            a);
        free(a);
        strvec_free(&lines);
        return r;
    }
    char *lead = whitespace_lead(text, old);
    if (lead) {
        char *r = xasprintf("old_string not found. Whitespace differs. Copy the exact text after " GUTTER ":\n%s", lead);
        free(lead);
        return r;
    }
    if (*rep) {
        const char *m = strstr(text, rep);
        if (m)
            return xasprintf("old_string not found. new_string is already present on line %d. Check if this change was "
                             "already applied.",
                             line_number_at(text, (size_t)(m - text)));
    }
    return xstrdup("old_string not found. Search for a short fragment, then read and copy the exact text.");
}

/* replaceAllWithLocations: every match replaced, the original lines of the first few listed */
static char *replace_all_with_locations(const char *text, const magic_step_t *st, char **message) {
    size_t ol = strlen(st->old_s);
    int count = 0, line = st->line;
    const char *newline = strchr(text + st->at, '\n');
    ints_t lines = {0};
    sb_t b = SB_INIT;
    const char *p = text, *m;
    while ((m = strstr(p, st->old_s))) {
        count++;
        if (lines.n <= MAX_REPLACE_MATCH_LINES) {
            while (newline && newline < m) {
                line++;
                newline = strchr(newline + 1, '\n');
            }
            if (!lines.n || vec_last(&lines) != line) vec_push(&lines, line);
        }
        sb_appendn(&b, p, (size_t)(m - p));
        sb_append(&b, st->rep);
        p = m + ol;
    }
    sb_append(&b, p);
    bool capped = lines.n > MAX_REPLACE_MATCH_LINES;
    if (capped) lines.n--;
    sb_t msg = SB_INIT;
    sb_appendf(&msg, "Replaced %d %s (original %s ", count, count == 1 ? "match" : "matches", lines.n == 1 ? "line" : "lines");
    for (size_t i = 0; i < lines.n; i++) sb_appendf(&msg, "%s%d", i ? ", " : "", lines.v[i]);
    sb_appendf(&msg, "%s).", capped ? ", " ELLIPSIS : "");
    vec_free(&lines);
    *message = sb_detach(&msg);
    return sb_detach(&b);
}

/* replacementPreview: numbered lines of the first edited region in the post-edit draft */
static char *replacement_preview(const char *content, const magic_step_t *st) {
    size_t len = strlen(content);
    if (!len) return xstrdup("The draft is empty.");
    size_t at = st->at, rl = strlen(st->rep);
    size_t els = 0; /* the edited line's start */
    for (size_t i = at; i > 0; i--)
        if (content[i - 1] == '\n') {
            els = i;
            break;
        }
    /* crop huge leading indentation on very long lines so the edit stays visible */
    bool cropped = (double)u16n(content + els, at - els) > MAX_REPLACE_PREVIEW_CHARS / 2;
    size_t from = cropped ? u16_back(content, at, MAX_REPLACE_PREVIEW_CHARS / 4) : els;
    int first = st->line - 1;
    for (int i = 0; i < REPLACE_CONTEXT_LINES && from > 0 && !cropped; i++) {
        size_t previous = 0;
        if (from > 1)
            for (size_t k = from - 1; k > 0; k--)
                if (content[k - 1] == '\n') {
                    previous = k;
                    break;
                }
        if ((double)u16n(content + previous, els - previous) > MAX_REPLACE_PREVIEW_CHARS / 3.0) break;
        from = previous;
        first--;
    }
    size_t to = from;
    int after = 0;
    size_t limit = u16_fwd(content, len, from, MAX_REPLACE_PREVIEW_CHARS + 1);
    for (int i = 0; i < MAX_REPLACE_PREVIEW_LINES && to < limit; i++) {
        if (to > at && to >= at + rl && after++ >= REPLACE_CONTEXT_LINES) break;
        const char *nl = memchr(content + to, '\n', len - to);
        to = !nl ? limit : ((size_t)(nl - content) + 1 < limit ? (size_t)(nl - content) + 1 : limit);
    }
    char *slice = xstrndup(content + from, to - from);
    strvec_t lines = file_lines(slice);
    free(slice);
    if (cropped && lines.n) {
        char *c = xasprintf(ELLIPSIS "%s", lines.v[0]);
        free(lines.v[0]);
        lines.v[0] = c;
    }
    strvec_t out = {0};
    numbered_lines(lines.v, 0, (int)lines.n - 1, MAX_REPLACE_PREVIEW_CHARS, first, &out);
    char *r = strvec_join(&out, "\n");
    strvec_free(&out);
    strvec_free(&lines);
    return r;
}

bool magic_apply_replace(const char *text, const json_t *args, char **content, magic_step_t *step, char **message,
                         char **error) {
    const json_t *ns = json_get(args, "new_string");
    if (json_is_null(ns)) {
        *error = xstrdup("new_string is required; pass \"\" to delete.");
        return false;
    }
    vfails_t f = {0};
    v_string(&f, args, "old_string", REPLACE_OLD_DESC, true);
    v_string(&f, args, "new_string", REPLACE_NEW_DESC, false);
    const json_t *all_v = json_get(args, "replace_all");
    /* a described boolean fails with its description as the raw expectation: a predicate code, the value shown */
    if (all_v && !json_is_bool(all_v)) vfail_push(&f, "replace_all", REPLACE_ALL_DESC, describe_value(all_v), all_v);
    if (f.n) {
        *error = vsummary(&f);
        return false;
    }
    vec_free(&f);
    const json_t *os = json_get(args, "old_string");
    if (strlen(os->u.str.s) != os->u.str.len || strlen(ns->u.str.s) != ns->u.str.len) {
        *error = xstrdup("NUL bytes are not allowed in text edits.");
        return false;
    }
    char *old = to_lf(os->u.str.s), *rep = to_lf(ns->u.str.s);
    bool all = all_v && all_v->u.b;
    const char *fail = NULL;
    if (all && is_blank(old)) fail = "replace_all cannot be used on whitespace-only old_string.";
    else if (!strcmp(old, rep)) fail = "old_string and new_string are identical.";
    if (fail) {
        *error = xstrdup(fail);
        free(old);
        free(rep);
        return false;
    }
    const char *m = strstr(text, old);
    if (!m) {
        *error = missing_anchor(text, old, rep);
        free(old);
        free(rep);
        return false;
    }
    size_t at = (size_t)(m - text);
    int line = line_number_at(text, at);
    if (!all && strstr(m + strlen(old), old)) {
        *error = xasprintf("old_string matches more than once (first on line %d). Include more context or pass "
                           "replace_all: true.",
                           line);
        free(old);
        free(rep);
        return false;
    }
    magic_step_t st = {old, rep, all, at, line};
    char *msg = NULL;
    char *next = all ? replace_all_with_locations(text, &st, &msg) : splice_replacement(text, &st);
    if (!all) msg = xasprintf("Replaced text on line %d.", line);
    char *preview = replacement_preview(next, &st);
    *message = xasprintf("%s\nDraft near the %sedit:\n%s", msg, all ? "first " : "", preview);
    free(preview);
    free(msg);
    *content = next;
    *step = st;
    return true;
}

char *magic_read_lines(const char *text, const json_t *args, bool *is_error) {
    vfails_t f = {0};
    v_line(&f, args, "start_line", READ_START_DESC);
    v_line(&f, args, "end_line", READ_END_DESC);
    *is_error = f.n > 0;
    if (f.n) return vsummary(&f);
    vec_free(&f);
    if (!*text) return xstrdup("The draft is empty.");
    strvec_t lines = file_lines(text);
    double start = json_get_num(args, "start_line", 1) - 1, end_line = json_get_num(args, "end_line", 1);
    int n = (int)lines.n;
    if (start >= n || end_line <= start) {
        *is_error = true;
        strvec_free(&lines);
        return xasprintf("Invalid range for %d lines.", n);
    }
    int s = (int)start;
    int end = end_line < n ? (int)end_line : n;
    if (s + MAX_READ_LINES < end) end = s + MAX_READ_LINES;
    strvec_t out = {0};
    numbered_lines(lines.v, s, end - 1, MAX_TEXT_CHARS, 0, &out);
    int last = s + (int)out.n;
    char *body = strvec_join(&out, "\n");
    int upto = end_line < n ? (int)end_line : n;
    char *r = last < upto ? xasprintf("Lines %d-%d of %d:\n%s\n" OMITTED " Read again starting at line %d.", s + 1, last, n, body, last + 1)
                          : xasprintf("Lines %d-%d of %d:\n%s", s + 1, last, n, body);
    free(body);
    strvec_free(&out);
    strvec_free(&lines);
    return r;
}

char *magic_search(const char *text, const json_t *args, bool *is_error) {
    vfails_t f = {0};
    v_string(&f, args, "query", SEARCH_QUERY_DESC, true);
    *is_error = f.n > 0;
    if (f.n) return vsummary(&f);
    vec_free(&f);
    const json_t *qv = json_get(args, "query");
    const char *query = qv->u.str.s;
    strvec_t lines = file_lines(text);
    strvec_t matches = {0};
    int total = 0;
    for (size_t i = 0; i < lines.n; i++) {
        const char *line = lines.v[i];
        /* a query holding NUL or a line break never matches one line */
        const char *m = strlen(query) == qv->u.str.len ? strstr(line, query) : NULL;
        if (!m) continue;
        total++;
        if ((int)matches.n >= MAX_SEARCH_HITS) continue;
        size_t ll = strlen(line);
        double at = (double)u16n(line, (size_t)(m - line));
        double from_u = at - 100 > 0 ? at - 100 : 0;
        size_t from = u16_fwd(line, ll, 0, from_u);
        size_t to = u16_fwd(line, ll, from, 300);
        strvec_push_own(&matches, xasprintf("%zu" GUTTER "%s%.*s%s", i + 1, from > 0 ? ELLIPSIS : "", (int)(to - from), line + from,
                                            to < ll ? ELLIPSIS : ""));
    }
    char *r;
    if (total) {
        char *j = strvec_join(&matches, "\n");
        r = xasprintf("%d matches:\n%s", total, j);
        free(j);
    } else {
        r = xstrdup("No matches.");
    }
    strvec_free(&matches);
    strvec_free(&lines);
    return r;
}

char *magic_diagnose_syntax(const char *original, const char *working, const magic_step_t *applied, size_t n,
                            bool (*parses)(const char *code, void *ud), void *ud) {
    char *draft = xstrdup(original);
    long last_good = -1;
    for (size_t i = 0; i + 1 < n; i++) {
        char *next = splice_replacement(draft, &applied[i]);
        free(draft);
        draft = next;
        if (parses(draft, ud)) last_good = (long)i;
    }
    free(draft);
    if (!n) return xstrdup("The draft no longer parses.");
    const magic_step_t *st = &applied[last_good + 1];
    strvec_t lines = file_lines(working);
    int nl = (int)lines.n;
    int near = st->line - 1 < nl - 1 ? st->line - 1 : nl - 1;
    strvec_t ctx = {0};
    numbered_lines(lines.v, near - 3 > 0 ? near - 3 : 0, nl - 1 < near + 5 ? nl - 1 : near + 5, MAX_TEXT_CHARS, 0, &ctx);
    char *j = strvec_join(&ctx, "\n");
    char *r = xasprintf("The draft first stopped parsing after replace #%ld (then at line %d). Current lines near that "
                        "location:\n%s",
                        last_good + 2, st->line, j);
    free(j);
    strvec_free(&ctx);
    strvec_free(&lines);
    return r;
}

/* ======================================================================================
 * written tool calls (edit/magic-calls.ts)
 * ====================================================================================== */

typedef struct {
    const char *key;
    char type; /* 'i' integer, 's' string, 'b' boolean */
    bool optional;
} tparam_t;

static const tparam_t READ_PARAMS[] = {{"start_line", 'i', false}, {"end_line", 'i', false}};
static const tparam_t SEARCH_PARAMS[] = {{"query", 's', false}};
static const tparam_t REPLACE_PARAMS[] = {{"old_string", 's', false}, {"new_string", 's', false}, {"replace_all", 'b', true}};

/* TOOL_PARAMETERS.get(name) */
static const tparam_t *tool_params(const char *name, size_t *n) {
    if (str_eq(name, "read")) return *n = 2, READ_PARAMS;
    if (str_eq(name, "search")) return *n = 1, SEARCH_PARAMS;
    if (str_eq(name, "replace")) return *n = 3, REPLACE_PARAMS;
    *n = 0;
    return NULL;
}
static bool apply_tool(const char *name) { return str_eq(name, "read") || str_eq(name, "search") || str_eq(name, "replace"); }

/* repeatedJsonObject: some providers duplicate argument deltas; recover only when every complete object agrees */
static json_t *repeated_json_object(const char *raw) {
    size_t len = strlen(raw), start = 0;
    int depth = 0;
    bool quoted = false, repeated = false;
    json_t *first = NULL;
    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        if (depth == 0) {
            size_t w = ws_at(raw + i);
            if (w) {
                i += w - 1;
                continue;
            }
            if (c != '{') goto fail;
            start = i;
            depth = 1;
        } else if (quoted) {
            if (c == '\\') i++;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            json_t *v = json_parse(raw + start, i + 1 - start, NULL);
            if (!json_is_obj(v)) {
                json_free(v);
                goto fail;
            }
            if (first) {
                bool same = json_equal(first, v);
                json_free(v);
                if (!same) goto fail;
                repeated = true;
            } else {
                first = v;
            }
        }
    }
    if (depth == 0 && repeated) return first;
fail:
    json_free(first);
    return NULL;
}

/* INTEGER_STRING /^[+-]?\d+$/ as a safe integer */
static bool integer_string(const char *s, double *out) {
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!*p) return false;
    for (const char *q = p; *q; q++)
        if (!digit_ch((unsigned char)*q)) return false;
    double v = strtod(p, NULL);
    if (v > 9007199254740991.0) return false;
    *out = *s == '-' ? -v : v;
    return true;
}

json_t *magic_normalize_args(const json_t *call, bool *repaired, char **error) {
    const char *name = json_get_str(call, "name");
    const json_t *args = json_get(call, "arguments");
    json_t *out = NULL;
    *repaired = false;
    if (json_is_obj(args) && (json_get(args, "__parseError") || json_get(args, "__rawJson"))) {
        const char *raw = json_get_str(args, "__rawJson");
        json_t *rec = raw ? repeated_json_object(raw) : NULL;
        if (!rec || json_get(rec, "__parseError") || json_get(rec, "__rawJson")) {
            const char *pe = json_get_str(args, "__parseError");
            *error = xasprintf("Malformed %s arguments: %s\nSend one JSON object per call.", name ? name : "",
                               pe && *pe ? pe : "Invalid JSON.");
            json_free(rec);
            return NULL;
        }
        out = rec;
        *repaired = json_get(args, "__rawJson") != NULL;
    } else {
        out = json_is_obj(args) ? json_clone(args) : json_obj();
    }
    size_t np;
    const tparam_t *ps = tool_params(name, &np);
    for (size_t i = 0; i < np; i++) {
        json_t *v = json_get(out, ps[i].key);
        if (v && v->type == JSON_NULL && ps[i].optional) {
            json_del(out, ps[i].key);
        } else if (json_is_str(v)) {
            double d;
            if (ps[i].type == 'b' && (str_eq(v->u.str.s, "true") || str_eq(v->u.str.s, "false")))
                json_set_bool(out, ps[i].key, str_eq(v->u.str.s, "true"));
            else if (ps[i].type == 'i' && integer_string(v->u.str.s, &d))
                json_set(out, ps[i].key, json_num(d));
        }
    }
    return out;
}

/* JSON_PYTHON_LITERALS: Python's True/False/None outside strings as JSON's (the text when nothing is translated) */
static char *translate_python_literals(const char *text, bool *changed) {
    sb_t b = SB_INIT;
    *changed = false;
    for (size_t i = 0; text[i];) {
        if (text[i] == '"') {
            size_t k = i + 1;
            while (text[k] && text[k] != '"') k += text[k] == '\\' && text[k + 1] ? 2 : 1;
            if (text[k] == '"') { /* a whole string token stays as written */
                sb_appendn(&b, text + i, k + 1 - i);
                i = k + 1;
                continue;
            }
        }
        bool left = i == 0 || !word_ch((unsigned char)text[i - 1]);
        static const char *const PY[3][2] = {{"True", "true"}, {"False", "false"}, {"None", "null"}};
        bool done = false;
        for (int w = 0; w < 3 && left && !done; w++) {
            size_t n = strlen(PY[w][0]);
            if (!strncmp(text + i, PY[w][0], n) && !word_ch((unsigned char)text[i + n])) {
                sb_append(&b, PY[w][1]);
                i += n;
                done = *changed = true;
            }
        }
        if (!done) sb_putc(&b, text[i++]);
    }
    return sb_detach(&b);
}

/* parseWrittenJson: JSON, else JSON with Python's literals outside strings translated; NULL when neither parses */
static json_t *parse_written_json(const char *text) {
    json_t *v = json_parse(text, strlen(text), NULL);
    if (v) return v;
    bool changed;
    char *norm = translate_python_literals(text, &changed);
    if (changed) v = json_parse(norm, strlen(norm), NULL);
    free(norm);
    return v;
}

static json_t *written_call_new(const char *name, const json_t *params) {
    json_t *c = json_obj();
    json_set_str(c, "name", name);
    json_set(c, "arguments", json_clone(params));
    return c;
}

/* writtenToolCall: one call spelled as JSON ({name, arguments}, {function, parameters}, the tool name as the only key,
 * Gemma's arguments beside `function`), or NULL */
static json_t *written_tool_call(const json_t *entry, int depth) {
    if (!json_is_obj(entry) || depth > 32) return NULL;
    const json_t *fn = json_get(entry, "function"), *name = json_get(entry, "name");
    if (json_is_obj(fn)) return written_tool_call(fn, depth + 1);
    const char *tool = json_is_str(name) ? name->u.str.s : json_is_str(fn) ? fn->u.str.s : NULL;
    if (!tool) {
        if (json_len(entry) != 1) return NULL;
        const char *only = entry->u.obj.keys[0];
        const json_t *params = entry->u.obj.vals[0];
        return apply_tool(only) && json_is_obj(params) ? written_call_new(only, params) : NULL;
    }
    /* parameters ?? arguments ?? the other keys */
    const json_t *params = json_get(entry, "parameters");
    if (json_is_null(params)) params = json_get(entry, "arguments");
    json_t *owned = NULL;
    if (json_is_null(params)) {
        owned = json_obj();
        const char *k;
        json_t *v;
        json_obj_foreach(entry, i, k, v) {
            if (str_eq(k, "function") || str_eq(k, "name") || str_eq(k, "parameters") || str_eq(k, "arguments")) continue;
            json_set(owned, k, json_clone(v));
        }
        params = owned;
    }
    if (json_is_str(params)) {
        json_t *parsed = parse_written_json(params->u.str.s);
        json_free(owned);
        owned = parsed;
        params = parsed;
    }
    json_t *r = apply_tool(tool) && json_is_obj(params) ? written_call_new(tool, params) : NULL;
    json_free(owned);
    return r;
}

/* ---------- PythonCalls: a restricted literal grammar; never evaluates or recovers a partial call ---------- */

typedef struct {
    const char *s;
    size_t len, at;
} pyp_t;

static void py_space(pyp_t *p) {
    size_t w;
    while (p->at < p->len && (w = ws_at(p->s + p->at))) p->at += w;
}

static bool py_take(pyp_t *p, const char *tok) {
    py_space(p);
    size_t n = strlen(tok);
    if (p->len - p->at < n || memcmp(p->s + p->at, tok, n)) return false;
    p->at += n;
    return true;
}

/* IDENTIFIER /[A-Za-z_]\w*\/y (owned) or NULL */
static char *py_ident(pyp_t *p) {
    unsigned char c = (unsigned char)p->s[p->at];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) return NULL;
    size_t st = p->at++;
    while (p->at < p->len && word_ch((unsigned char)p->s[p->at])) p->at++;
    return xstrndup(p->s + st, p->at - st);
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void put_cp(sb_t *b, uint32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; /* a lone surrogate has no UTF-8 form */
    char u[4];
    int n = utf8_encode(cp, u);
    sb_appendn(b, u, (size_t)n);
}

/* a Python string literal: '…', "…", '''…''' or """…""" (owned json string, NUL bytes kept) or NULL */
static json_t *py_string(pyp_t *p, char quote) {
    char triple[4] = {quote, quote, quote, 0};
    size_t dl = p->len - p->at >= 3 && !memcmp(p->s + p->at, triple, 3) ? 3 : 1;
    p->at += dl;
    sb_t b = SB_INIT;
    while (p->at < p->len) {
        if (p->len - p->at >= dl && !memcmp(p->s + p->at, triple, dl)) {
            p->at += dl;
            json_t *v = json_strn(b.s ? b.s : "", b.len);
            sb_free(&b);
            return v;
        }
        char c = p->s[p->at];
        if (c == '\\') {
            size_t q = p->at + 1;
            if (q >= p->len) break;
            char e = p->s[q];
            if (e == '\r' && q + 1 < p->len && p->s[q + 1] == '\n') { /* a line continuation */
                p->at = q + 2;
                continue;
            }
            if (e == '\n') {
                p->at = q + 1;
                continue;
            }
            if (e >= '0' && e <= '7') {
                uint32_t v = 0;
                size_t k = q;
                while (k < p->len && k < q + 3 && p->s[k] >= '0' && p->s[k] <= '7') v = v * 8 + (uint32_t)(p->s[k++] - '0');
                put_cp(&b, v);
                p->at = k;
                continue;
            }
            if (e == 'x' || e == 'u' || e == 'U') {
                size_t need = e == 'x' ? 2 : e == 'u' ? 4 : 8, k = q + 1;
                uint64_t v = 0;
                while (k < p->len && k < q + 1 + need && hexv(p->s[k]) >= 0) v = v * 16 + (uint64_t)hexv(p->s[k++]);
                if (k != q + 1 + need || v > 0x10FFFF) break; /* `\x` without its digits, or past Unicode */
                put_cp(&b, (uint32_t)v);
                p->at = k;
                continue;
            }
            if (e == 'N') break; /* named Unicode escapes need Python's name table */
            static const char SIMPLE[] = "abfnrtv'\"\\";
            static const char MAPPED[] = "\a\b\f\n\r\t\v'\"\\";
            const char *hit = e ? strchr(SIMPLE, e) : NULL;
            size_t el = u8len(p->s + q, p->len - q);
            if (hit) sb_putc(&b, MAPPED[hit - SIMPLE]);
            else sb_appendn(&b, p->s + p->at, 1 + el); /* an unknown escape stays as written */
            p->at = q + el;
            continue;
        }
        if (dl == 1 && (c == '\n' || c == '\r')) break;
        sb_putc(&b, c);
        p->at++;
    }
    sb_free(&b);
    return NULL;
}

/* INTEGER /[+-]?(?:0x_?[\da-f](?:_?[\da-f])*|0o…|0b…|\d(?:_?\d)*)/iy as a safe integer, or false */
static bool py_integer(pyp_t *p, double *out) {
    size_t i = p->at;
    int sign = 1;
    if (i < p->len && (p->s[i] == '+' || p->s[i] == '-')) sign = p->s[i++] == '-' ? -1 : 1;
    int base = 10;
    if (i + 1 < p->len && p->s[i] == '0') {
        char k = (char)tolower((unsigned char)p->s[i + 1]);
        base = k == 'x' ? 16 : k == 'o' ? 8 : k == 'b' ? 2 : 10;
    }
    size_t j = i;
    uint64_t v = 0;
    bool over = false;
    if (base != 10) {
        size_t k = i + 2;
        if (k < p->len && p->s[k] == '_') k++;
        int d = k < p->len ? hexv(p->s[k]) : -1;
        if (d < 0 || d >= base) base = 10; /* `0b2`: the prefix does not form a literal; `0` does */
        else {
            j = k;
            for (;;) {
                d = j < p->len ? hexv(p->s[j]) : -1;
                if (d < 0 || d >= base) {
                    if (j + 1 < p->len && p->s[j] == '_' && (d = hexv(p->s[j + 1])) >= 0 && d < base) {
                        j++;
                        continue;
                    }
                    break;
                }
                if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) over = true;
                else v = v * (uint64_t)base + (uint64_t)d;
                j++;
            }
        }
    }
    if (base == 10) {
        j = i;
        if (j >= p->len || !digit_ch((unsigned char)p->s[j])) return false;
        for (;;) {
            if (j < p->len && digit_ch((unsigned char)p->s[j])) {
                uint64_t d = (uint64_t)(p->s[j] - '0');
                if (v > (UINT64_MAX - d) / 10) over = true;
                else v = v * 10 + d;
                j++;
            } else if (j + 1 < p->len && p->s[j] == '_' && digit_ch((unsigned char)p->s[j + 1]) && j > i) {
                j++;
            } else {
                break;
            }
        }
    }
    if (over || v > 9007199254740991ULL) return false;
    p->at = j;
    *out = sign * (double)v;
    return true;
}

/* a literal: string, True/False/None (or true/false/null), safe integer; NULL for anything else */
static json_t *py_literal(pyp_t *p) {
    py_space(p);
    if (p->at >= p->len) return NULL;
    char q = p->s[p->at];
    if (q == '"' || q == '\'') return py_string(p, q);
    char *word = py_ident(p);
    if (word) {
        json_t *v = NULL;
        if (str_eq(word, "True") || str_eq(word, "true")) v = json_bool(true);
        else if (str_eq(word, "False") || str_eq(word, "false")) v = json_bool(false);
        else if (str_eq(word, "None") || str_eq(word, "null")) v = json_null();
        free(word);
        return v;
    }
    double d;
    return py_integer(p, &d) ? json_num(d) : NULL;
}

static json_t *py_call(pyp_t *p) {
    char *name = py_ident(p);
    if (!name || !apply_tool(name) || !py_take(p, "(")) {
        free(name);
        return NULL;
    }
    json_t *args = json_obj();
    while (!py_take(p, ")")) {
        char *key = py_ident(p);
        if (!key || json_get(args, key) || !py_take(p, "=")) {
            free(key);
            json_free(args);
            free(name);
            return NULL;
        }
        json_t *v = py_literal(p);
        if (!v) {
            free(key);
            json_free(args);
            free(name);
            return NULL;
        }
        json_set(args, key, v);
        free(key);
        if (py_take(p, ")")) break;
        if (!py_take(p, ",")) {
            json_free(args);
            free(name);
            return NULL;
        }
    }
    json_t *c = json_obj();
    json_set_str(c, "name", name);
    json_set(c, "arguments", args);
    free(name);
    return c;
}

/* PythonCalls.parse: the whole text is one call or a list of calls (owned array) or NULL */
static json_t *py_parse(const char *text, size_t len) {
    pyp_t p = {text, len, 0};
    bool list = py_take(&p, "[");
    json_t *calls = json_arr();
    for (;;) {
        if (list && py_take(&p, "]")) break;
        json_t *c = py_call(&p);
        if (!c) {
            json_free(calls);
            return NULL;
        }
        json_push(calls, c);
        if (!list || py_take(&p, "]")) break;
        if (!py_take(&p, ",")) {
            json_free(calls);
            return NULL;
        }
    }
    py_space(&p);
    if (p.at != p.len) {
        json_free(calls);
        return NULL;
    }
    return calls;
}

/* PYTHON_START /^\s*(?:\[\s*)?(?:read|search|replace)\s*\(/ */
static bool python_start(const char *s) {
    s += ws_run(s);
    if (*s == '[') {
        s++;
        s += ws_run(s);
    }
    size_t n = str_starts(s, "read") ? 4 : str_starts(s, "search") ? 6 : str_starts(s, "replace") ? 7 : 0;
    if (!n) return false;
    s += n;
    s += ws_run(s);
    return *s == '(';
}

/* JSON_START /^\s*(?:\[\s*)?\{\s*"(?:name|function|arguments|parameters|read|search|replace)"/ */
static bool json_start(const char *s) {
    s += ws_run(s);
    if (*s == '[') {
        s++;
        s += ws_run(s);
    }
    if (*s++ != '{') return false;
    s += ws_run(s);
    if (*s++ != '"') return false;
    static const char *const KEYS[] = {"name", "function", "arguments", "parameters", "read", "search", "replace"};
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
        size_t n = strlen(KEYS[i]);
        if (!strncmp(s, KEYS[i], n) && s[n] == '"') return true;
    }
    return false;
}

static void push_calls(json_t *calls, json_t *parsed) {
    if (!parsed) return;
    while (json_len(parsed)) json_push(calls, json_arr_remove(parsed, 0));
    json_free(parsed);
}

/* bare lines outside fences: a Python call line runs; a JSON-looking line only counts as an attempt */
static void parse_bare(const char *s, size_t n, json_t *calls, bool *attempted) {
    char *outside = xstrndup(s, n);
    strvec_t lines = str_split(outside, "\n");
    for (size_t i = 0; i < lines.n; i++) {
        if (python_start(lines.v[i])) {
            *attempted = true;
            push_calls(calls, py_parse(lines.v[i], strlen(lines.v[i])));
        } else if (json_start(lines.v[i])) {
            *attempted = true;
        }
    }
    strvec_free(&lines);
    free(outside);
}

/* FENCE_LINE /^[\t ]*(`{3,}|~{3,})([^\r\n]*)\r?$/gm from `from`: match start, length, marker and tag span */
static bool next_fence(const char *t, size_t len, size_t from, size_t *idx, size_t *mlen, size_t *ma, size_t *mb,
                       size_t *ta, size_t *tb) {
    for (size_t i = from; i < len; i++) {
        if (!line_start(t, i)) continue;
        size_t k = i;
        while (k < len && (t[k] == ' ' || t[k] == '\t')) k++;
        char c = t[k];
        if (c != '`' && c != '~') continue;
        size_t m = k;
        while (m < len && t[m] == c) m++;
        if (m - k < 3) continue;
        size_t e = m;
        while (e < len && t[e] != '\r' && t[e] != '\n') e++;
        size_t end = e;
        if (e < len && t[e] == '\r' && (e + 1 >= len || t[e + 1] == '\n')) end = e + 1;
        *idx = i;
        *mlen = end - i;
        *ma = k;
        *mb = m;
        *ta = m;
        *tb = e;
        return true;
    }
    return false;
}

json_t *magic_parse_written_calls(const char *text, bool *attempted) {
    json_t *calls = json_arr();
    *attempted = false;
    size_t len = strlen(text), from = 0, pos = 0;
    bool fenced = false, tool_fence = false;
    size_t fma = 0, fmb = 0; /* the open fence's marker */
    size_t idx, mlen, ma, mb, ta, tb;
    while (next_fence(text, len, pos, &idx, &mlen, &ma, &mb, &ta, &tb)) {
        pos = idx + mlen;
        char *tagraw = xstrndup(text + ta, tb - ta);
        char *tag = js_trim(tagraw);
        free(tagraw);
        if (!fenced) {
            parse_bare(text + (from < idx ? from : idx), from < idx ? idx - from : 0, calls, attempted);
            fenced = true;
            fma = ma;
            fmb = mb;
            tool_fence = str_eq(tag, "tool_code") || str_eq(tag, "tool_response");
        } else {
            /* a closing fence repeats the opening marker (or a longer run of it) and carries no tag */
            bool closes = mb - ma >= fmb - fma && text[ma] == text[fma] && !*tag;
            if (!closes) {
                free(tag);
                continue;
            }
            char *body = xstrndup(text + (from < idx ? from : idx), from < idx ? idx - from : 0);
            if (python_start(body)) {
                *attempted = true;
                push_calls(calls, py_parse(body, strlen(body)));
            } else {
                if (tool_fence || json_start(body)) *attempted = true;
                json_t *v = parse_written_json(body);
                if (json_is_arr(v)) {
                    json_t *e;
                    json_arr_foreach(v, i, e) {
                        json_t *c = written_tool_call(e, 0);
                        if (c) json_push(calls, c);
                    }
                } else if (v) {
                    json_t *c = written_tool_call(v, 0);
                    if (c) json_push(calls, c);
                }
                json_free(v);
            }
            free(body);
            fenced = false;
        }
        free(tag);
        from = idx + mlen + 1;
    }
    const char *rest = text + (from < len ? from : len);
    if (fenced) {
        if (tool_fence || python_start(rest) || json_start(rest)) *attempted = true;
    } else {
        parse_bare(rest, strlen(rest), calls, attempted);
    }
    return calls;
}

/* ======================================================================================
 * syntax: tree-sitter-json's grammar (omp's parseCodeStatus for `.json`)
 * ====================================================================================== */

/* extras: ASCII whitespace (omp's parser takes no Unicode space between tokens) and comments (`// …`, block
 * comments); false on an unterminated comment */
static bool tsj_skip(const char *s, size_t len, size_t *i) {
    for (;;) {
        if (*i >= len) return true;
        unsigned char c = (unsigned char)s[*i];
        if (c == ' ' || (c >= 9 && c <= 13)) {
            (*i)++;
            continue;
        }
        if (c == '/' && *i + 1 < len && s[*i + 1] == '/') {
            while (*i < len && s[*i] != '\n') (*i)++;
            continue;
        }
        if (c == '/' && *i + 1 < len && s[*i + 1] == '*') {
            size_t k = *i + 2;
            while (k + 1 < len && !(s[k] == '*' && s[k + 1] == '/')) k++;
            if (k + 1 >= len) return false;
            *i = k + 2;
            continue;
        }
        return true;
    }
}

/* string: `"` (string_content /[^\\"\n]+/ | escape_sequence /\\["\\\/bfnrtu]/)* `"` */
static bool tsj_string(const char *s, size_t len, size_t *i) {
    size_t k = *i + 1;
    while (k < len) {
        char c = s[k];
        if (c == '"') {
            *i = k + 1;
            return true;
        }
        if (c == '\n') return false;
        if (c == '\\') {
            if (k + 1 >= len || !strchr("\"\\/bfnrtu", s[k + 1]) || !s[k + 1]) return false;
            k += 2;
            continue;
        }
        k++;
    }
    return false;
}

/* number: -?(0|[1-9]\d*)(\.\d*)?([eE]-?\d+)? as one token (longest match; `1e+5` is not one) */
static bool tsj_number(const char *s, size_t len, size_t *i) {
    size_t k = *i;
    if (k < len && s[k] == '-') k++;
    if (k >= len || !digit_ch((unsigned char)s[k])) return false;
    if (s[k] == '0') k++;
    else
        while (k < len && digit_ch((unsigned char)s[k])) k++;
    if (k < len && s[k] == '.') {
        k++;
        while (k < len && digit_ch((unsigned char)s[k])) k++;
    }
    if (k < len && (s[k] == 'e' || s[k] == 'E')) {
        size_t e = k + 1;
        if (e < len && s[e] == '-') e++;
        if (e < len && digit_ch((unsigned char)s[e])) {
            while (e < len && digit_ch((unsigned char)s[e])) e++;
            k = e;
        }
    }
    *i = k;
    return true;
}

static bool tsj_scalar(const char *s, size_t len, size_t *i) {
    char c = s[*i];
    if (c == '"') return tsj_string(s, len, i);
    static const char *const WORDS[] = {"true", "false", "null"};
    for (int w = 0; w < 3; w++) {
        size_t n = strlen(WORDS[w]);
        if (len - *i >= n && !memcmp(s + *i, WORDS[w], n)) {
            *i += n;
            return true;
        }
    }
    return tsj_number(s, len, i);
}

/* document: repeat(value) without an ERROR or MISSING node */
static bool tsjson_parses(const char *s, size_t len) {
    enum { W_VALUE, W_VALUE_OR_CLOSE, W_KEY_OR_CLOSE, W_KEY, W_COLON, W_COMMA_OR_CLOSE };
    VEC(char) st = {0};
    int want = W_VALUE;
    size_t i = 0;
    bool ok = true;
    for (;;) {
        if (!tsj_skip(s, len, &i)) {
            ok = false;
            break;
        }
        if (i >= len) {
            ok = st.n == 0;
            break;
        }
        char c = s[i];
        if (want == W_VALUE_OR_CLOSE && c == ']') {
            st.n--;
            i++;
            want = st.n ? W_COMMA_OR_CLOSE : W_VALUE;
            continue;
        }
        if (want == W_KEY_OR_CLOSE && c == '}') {
            st.n--;
            i++;
            want = st.n ? W_COMMA_OR_CLOSE : W_VALUE;
            continue;
        }
        if (want == W_VALUE || want == W_VALUE_OR_CLOSE) {
            if (c == '{' || c == '[') {
                vec_push(&st, c);
                i++;
                want = c == '{' ? W_KEY_OR_CLOSE : W_VALUE_OR_CLOSE;
                continue;
            }
            if (!tsj_scalar(s, len, &i)) {
                ok = false;
                break;
            }
            want = st.n ? W_COMMA_OR_CLOSE : W_VALUE;
            continue;
        }
        if (want == W_KEY || want == W_KEY_OR_CLOSE) {
            if (c != '"' || !tsj_string(s, len, &i)) {
                ok = false;
                break;
            }
            want = W_COLON;
            continue;
        }
        if (want == W_COLON) {
            if (c != ':') {
                ok = false;
                break;
            }
            i++;
            want = W_VALUE;
            continue;
        }
        /* W_COMMA_OR_CLOSE */
        char top = vec_last(&st);
        if (c == ',') {
            i++;
            want = top == '{' ? W_KEY : W_VALUE;
            continue;
        }
        if ((c == '}' && top == '{') || (c == ']' && top == '[')) {
            st.n--;
            i++;
            want = st.n ? W_COMMA_OR_CLOSE : W_VALUE;
            continue;
        }
        ok = false;
        break;
    }
    vec_free(&st);
    return ok;
}

int magic_parse_status(const char *path, const char *code) {
    /* SupportLang::from_path: the extension after the last dot of the file name, ASCII case-insensitive */
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *bs = strrchr(path, '\\');
    if (bs && (!base || bs > base)) base = bs;
#endif
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || strcasecmp(dot + 1, "json")) return -1;
    /* parseCodeStatus({code: code === "" ? "\n" : code}): an empty document parses */
    return tsjson_parses(code, strlen(code)) ? 1 : 0;
}

/* ======================================================================================
 * applyDescribedChange (edit/magic.ts)
 * ====================================================================================== */

#define MAX_TURNS 12

/* REFUSAL /^CANNOT:\s*(.*)/i: the reason (owned, up to the first line terminator), or NULL */
static char *refusal(const char *reply) {
    if (strncasecmp(reply, "CANNOT:", 7)) return NULL;
    const char *p = reply + 7;
    p += ws_run(p);
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r' && strncmp(e, "\xe2\x80\xa8", 3) && strncmp(e, "\xe2\x80\xa9", 3)) e++;
    return xstrndup(p, (size_t)(e - p));
}

/* /^DONE[.!]?$/i */
static bool done_reply(const char *t) {
    if (strncasecmp(t, "DONE", 4)) return false;
    return !t[4] || ((t[4] == '.' || t[4] == '!') && !t[5]);
}

static json_t *user_message(const char *text) {
    json_t *u = json_obj();
    json_set_str(u, "role", "user");
    json_set_str(u, "content", text);
    json_set_int(u, "timestamp", now_ms());
    return u;
}

static json_t *tool_result(const json_t *call, const char *text, bool is_error) {
    json_t *m = json_obj();
    json_set_str(m, "role", "toolResult");
    json_set_str(m, "toolCallId", json_get_str(call, "id") ? json_get_str(call, "id") : "");
    json_set_str(m, "toolName", json_get_str(call, "name") ? json_get_str(call, "name") : "");
    json_t *content = json_arr(), *t = json_obj();
    json_set_str(t, "type", "text");
    json_set_str(t, "text", text);
    json_push(content, t);
    json_set(m, "content", content);
    json_set_bool(m, "isError", is_error);
    json_set_int(m, "timestamp", now_ms());
    return m;
}

/* the reply's text blocks joined and trimmed (owned) */
static char *reply_text(const json_t *msg) {
    sb_t b = SB_INIT;
    json_t *blk;
    json_arr_foreach(json_get_arr(msg, "content"), i, blk) if (str_eq(json_get_str(blk, "type"), "text"))
        sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
    char *t = js_trim(sb_cstr(&b));
    sb_free(&b);
    return t;
}

typedef struct {
    json_t *args; /* normalized arguments */
    char *error;  /* NULL: applied */
} attempt_t;
typedef VEC(attempt_t) attempts_t;

static const attempt_t *attempt_find(const attempts_t *a, const json_t *args) {
    for (size_t i = 0; i < a->n; i++)
        if (json_equal(a->v[i].args, args)) return &a->v[i];
    return NULL;
}

typedef struct {
    const char *path;
} parse_ctx_t;

static bool parses_cb(const char *code, void *ud) {
    const parse_ctx_t *c = ud;
    return magic_parse_status(c->path, *code ? code : "\n") == 1;
}

bool magic_apply_described(const char *path, const char *change, const char *content, const magic_views_t *views,
                           magic_io_t *io, pe_result_t *res, char **err) {
    memset(res, 0, sizeof(*res));
    if (!*content) {
        *err = xasprintf("%s is empty; use write instead.", path);
        return false;
    }
    parse_ctx_t pc = {path};
    bool parsed = parses_cb(content, &pc);
    json_t *tools = magic_apply_tools();
    attempts_t attempts = {0}; /* replaceAttempts: every exact replace tried, across views */
    bool ok = false, finished = false;
    for (size_t vi = 0; vi < views->n && !finished; vi++) {
        const magic_view_t *view = &views->v[vi];
        char *working = xstrdup(content);
        magic_steps_t applied = {0};
        bool inspected = false, failed_replacement = false, widen = false;
        char *last_feedback = NULL;
        json_t *messages = json_arr();
        json_t *ctx = json_obj();
        json_set_str(ctx, "path", path);
        json_set_str(ctx, "change", change);
        json_set_str(ctx, "file", view->text);
        json_set_bool(ctx, "partial", view->partial);
        json_set_int(ctx, "totalLines", view->total_lines);
        char *request = prompt_render("coding-agent/edit/magic-apply.md", ctx);
        json_free(ctx);
        json_push(messages, user_message(request ? request : ""));
        free(request);
        for (int turn = 1; turn <= MAX_TURNS && !finished && !widen; turn++) {
            char *cerr = NULL;
            json_t *response = io->complete(io, messages, tools, &cerr);
            if (!response) {
                *err = cerr ? cerr : xstrdup("The edit failed.");
                finished = true;
                break;
            }
            bool truncated = false, call_format = false, failed = false;
            char *syntax = NULL;
            if (str_eq(json_get_str(response, "stopReason"), "length")) {
                /* providers may expose partly parsed tool calls on a truncated turn: neither staged nor replayed */
                truncated = true;
                failed_replacement = true;
                json_free(response);
            } else {
                json_t *reply = response; /* owned; replaced by the written-call form */
                json_t *calls = json_arr(); /* borrowed-shape clones of the reply's tool calls */
                json_t *blk;
                json_arr_foreach(json_get_arr(reply, "content"), i, blk) if (str_eq(json_get_str(blk, "type"), "toolCall"))
                    json_push(calls, json_clone(blk));
                char *text = xstrdup("");
                if (!json_len(calls)) {
                    free(text);
                    text = reply_text(response);
                    bool attempted = false;
                    json_t *written = magic_parse_written_calls(text, &attempted);
                    if (!json_len(written)) {
                        call_format = attempted;
                    } else {
                        /* withWrittenToolCalls: replayed as native calls instead of the text or its Responses items */
                        json_t *w;
                        json_arr_foreach(written, n, w) {
                            json_t *c = json_obj();
                            json_set_str(c, "type", "toolCall");
                            char *id = xasprintf("written_%d_%zu", turn, n);
                            json_set_str(c, "id", id);
                            free(id);
                            json_set_str(c, "name", json_get_str(w, "name"));
                            json_set(c, "arguments", json_clone(json_get(w, "arguments")));
                            json_push(calls, c);
                        }
                        reply = json_clone(response);
                        json_del(reply, "providerPayload");
                        json_t *kept = json_arr();
                        json_arr_foreach(json_get_arr(response, "content"), i, blk) if (!str_eq(json_get_str(blk, "type"), "text"))
                            json_push(kept, json_clone(blk));
                        json_arr_foreach(calls, i, blk) json_push(kept, json_clone(blk));
                        json_set(reply, "content", kept);
                        json_set_str(reply, "stopReason", "toolUse");
                        json_free(response);
                        response = NULL;
                    }
                    json_free(written);
                }
                if (json_len(calls)) {
                    size_t reply_index = json_len(messages);
                    json_push(messages, reply);
                    bool replaced = false, call_failed = false, repaired_any = false;
                    json_t *repaired_args = json_arr(); /* per call: the repaired arguments or null */
                    json_t *call;
                    json_arr_foreach(calls, ci, call) {
                        const char *name = json_get_str(call, "name");
                        bool repaired = false;
                        char *aerr = NULL;
                        json_t *args = magic_normalize_args(call, &repaired, &aerr);
                        if (!args) {
                            if (!str_eq(name, "read") && !str_eq(name, "search")) call_failed = true;
                            json_push(messages, tool_result(call, aerr, true));
                            free(aerr);
                            json_push(repaired_args, json_null());
                            continue;
                        }
                        json_push(repaired_args, repaired ? json_clone(args) : json_null());
                        repaired_any = repaired_any || repaired;
                        if (str_eq(name, "replace")) {
                            const attempt_t *prev = attempt_find(&attempts, args);
                            if (prev) {
                                if (!prev->error) {
                                    json_push(messages, tool_result(call, "This exact replace was already applied. Do not repeat it.", true));
                                } else {
                                    call_failed = true;
                                    char *t = xasprintf("This exact replace already failed: %s", prev->error);
                                    json_push(messages, tool_result(call, t, true));
                                    free(t);
                                }
                                json_free(args);
                                continue;
                            }
                            char *next = NULL, *msg = NULL, *rerr = NULL;
                            magic_step_t st;
                            if (!magic_apply_replace(working, args, &next, &st, &msg, &rerr)) {
                                call_failed = true;
                                attempt_t a = {args, xstrdup(rerr)};
                                vec_push(&attempts, a);
                                json_push(messages, tool_result(call, rerr, true));
                                free(rerr);
                            } else {
                                replaced = true;
                                attempt_t a = {args, NULL};
                                vec_push(&attempts, a);
                                free(working);
                                working = next;
                                vec_push(&applied, st);
                                json_push(messages, tool_result(call, msg, false));
                                free(msg);
                            }
                        } else if (str_eq(name, "read") || str_eq(name, "search")) {
                            bool is_error = false;
                            char *out = str_eq(name, "read") ? magic_read_lines(working, args, &is_error)
                                                             : magic_search(working, args, &is_error);
                            if (!is_error) inspected = true;
                            json_push(messages, tool_result(call, out, is_error));
                            free(out);
                            json_free(args);
                        } else {
                            call_failed = true;
                            json_push(messages, tool_result(call, "Available tools: replace, read, search.", true));
                            json_free(args);
                        }
                    }
                    if (repaired_any) { /* the reply replays the recovered arguments, without its native payload */
                        json_t *r = json_at(messages, reply_index);
                        json_del(r, "providerPayload");
                        size_t k = 0;
                        json_arr_foreach(json_get_arr(r, "content"), i, blk) {
                            if (!str_eq(json_get_str(blk, "type"), "toolCall")) continue;
                            json_t *ra = json_at(repaired_args, k++);
                            if (ra && !json_is_null(ra)) json_set(blk, "arguments", json_clone(ra));
                        }
                    }
                    json_free(repaired_args);
                    if (replaced || call_failed) failed_replacement = call_failed;
                    if (replaced) {
                        free(last_feedback);
                        last_feedback = NULL;
                    }
                    json_free(calls);
                    free(text);
                    continue;
                }
                json_free(calls);
                char *reason = refusal(text);
                if (reason) {
                    if (!inspected && !applied.n && vi + 1 < views->n) { /* declined on an excerpt: afresh, wider */
                        free(reason);
                        free(text);
                        json_free(reply);
                        widen = true;
                        break;
                    }
                    char *why = js_trim(reason);
                    *err = xasprintf("No change was made to %s: %s", path, *why ? why : "no reason given");
                    free(why);
                    free(reason);
                    free(text);
                    json_free(reply);
                    finished = true;
                    break;
                }
                if (failed_replacement) {
                    failed = true;
                } else if (done_reply(text) && strcmp(working, content)) {
                    if (parsed && !parses_cb(working, &pc)) {
                        syntax = magic_diagnose_syntax(content, working, applied.v, applied.n, parses_cb, &pc);
                    } else {
                        pe_result_t r;
                        io->apply(io, content, working, &r);
                        if (!r.is_error) {
                            *res = r;
                            ok = finished = true;
                        } else if (str_eq(r.error_kind, "write_failed")) { /* thrown by the write: as it is */
                            *err = xstrdup(r.text ? r.text : "");
                            finished = true;
                            pe_result_free(&r);
                        } else {
                            *err = xasprintf("The change to %s could not be applied: %s", path, r.text ? r.text : "");
                            finished = true;
                            pe_result_free(&r);
                        }
                        if (finished) {
                            free(text);
                            json_free(reply);
                            break;
                        }
                    }
                }
                json_push(messages, reply);
                free(text);
            }
            json_t *fb = json_obj();
            if (truncated) json_set_bool(fb, "truncated", true);
            if (syntax) json_set_str(fb, "syntax", syntax);
            if (failed) json_set_bool(fb, "failed", true);
            if (call_format) json_set_bool(fb, "callFormat", true);
            free(syntax);
            char *rendered = prompt_render("coding-agent/edit/magic-apply-feedback.md", fb);
            json_free(fb);
            char *correction = js_trim(rendered ? rendered : "");
            free(rendered);
            if (turn == MAX_TURNS || (last_feedback && !strcmp(correction, last_feedback))) {
                *err = xasprintf("The change to %s did not finish; the file is unchanged. %s", path, correction);
                free(correction);
                finished = true;
                break;
            }
            free(last_feedback);
            last_feedback = correction;
            json_push(messages, user_message(correction));
        }
        if (!finished && !widen) {
            *err = xasprintf("The change to %s did not finish within %d turns; the file is unchanged.", path, MAX_TURNS);
            finished = true;
        }
        free(last_feedback);
        json_free(messages);
        magic_steps_free(&applied);
        free(working);
    }
    if (!finished) *err = xasprintf("The change to %s could not be applied; the file is unchanged.", path);
    for (size_t i = 0; i < attempts.n; i++) {
        json_free(attempts.v[i].args);
        free(attempts.v[i].error);
    }
    vec_free(&attempts);
    json_free(tools);
    return ok;
}

/* ======================================================================================
 * the edit call (edit/magic.ts resolveApplier, applySection, runMagicEdit)
 * ====================================================================================== */

#define COMPLETION_MAX_TOKENS 16384
#define APPLY_TIMEOUT_MS 120000
#define MAX_CONTEXT_SHARE 0.5
#define CHARS_PER_TOKEN 3

typedef struct {
    tool_env_t *env;
    const pe_opts_t *base;
    const ai_model_t *model;
    char *name;
    char *api_key, *session_id;
    char *system;
    ai_effort_t effort;
    bool has_effort;
    volatile sig_atomic_t cancel;     /* the completions' abort flag (tool abort or the deadline) */
    volatile sig_atomic_t *tool_cancel;
    volatile sig_atomic_t timed_out, finished;
    int64_t deadline;
} magic_call_t;

typedef struct {
    magic_call_t *mc;
    const magic_section_t *section;
    char *raw; /* the file as read (the snapshot an apply checks against) */
    size_t raw_len;
    bool ok;
    pe_result_t res;
    char *err;
} magic_job_t;

static void *magic_watch(void *arg) {
    magic_call_t *mc = arg;
    while (!mc->finished) {
        if (mc->tool_cancel && *mc->tool_cancel) mc->cancel = 1;
        if (now_ms() >= mc->deadline && !mc->cancel) {
            mc->timed_out = 1;
            mc->cancel = 1;
        }
        struct timespec ts = {0, 50 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* one turn: the conversation so far with the apply tools; the assistant message or NULL with *err */
static json_t *mc_complete(magic_io_t *io, const json_t *messages, const json_t *tools, char **err) {
    magic_job_t *job = io->ud;
    magic_call_t *mc = job->mc;
    const ai_model_t *m = mc->model;
    int64_t ceiling = m->max_tokens > 0 && m->max_tokens < COMPLETION_MAX_TOKENS ? m->max_tokens : COMPLETION_MAX_TOKENS;
    json_t *sys = json_arr();
    json_push_str(sys, mc->system);
    ai_context_t ctx = {.system_prompt = sys, .messages = (json_t *)messages, .tools = (json_t *)tools};
    json_t *r = NULL;
    /* retryTransientCompletion: up to three attempts of a transient failure, 500 ms doubling (capped 8 s) */
    for (int attempt = 1;; attempt++) {
        if (mc->cancel) break;
        ai_stream_opts_t o;
        ai_stream_opts_init(&o);
        o.api_key = mc->api_key;
        o.session_id = mc->session_id;
        o.max_tokens = ceiling;
        o.reasoning = mc->has_effort ? model_clamp_effort(m, mc->effort) : EFFORT_OFF;
        o.disable_reasoning = !mc->has_effort;
        o.cancel = &mc->cancel;
        json_free(r);
        r = ai_complete(m, &ctx, &o);
        if (mc->env->side_usage) mc->env->side_usage(mc->env, "magic-edit", r);
        if (!str_eq(json_get_str(r, "stopReason"), "error") || mc->cancel || attempt >= 3) break;
        const char *em = json_get_str(r, "errorMessage");
        if (!ai_provider_retryable_error(em ? em : "", mr_error_status(em ? em : ""))) break;
        int64_t delay = 500LL << (attempt - 1);
        if (delay > 8000) delay = 8000;
        delay = (int64_t)(delay * (0.75 + (double)(rand() % 1000) / 4000.0));
        for (int64_t waited = 0; waited < delay && !mc->cancel; waited += 50) {
            struct timespec ts = {0, 50 * 1000000L};
            nanosleep(&ts, NULL);
        }
    }
    json_free(sys);
    const char *stop = json_get_str(r, "stopReason");
    if (!r || mc->cancel || str_eq(stop, "aborted")) {
        /* an abort resolves with empty content rather than rejecting */
        *err = mc->timed_out ? xasprintf("The edit timed out after %ds waiting on %s; the file is unchanged.",
                                         APPLY_TIMEOUT_MS / 1000, mc->name)
                             : xstrdup("The edit was aborted; the file is unchanged.");
        json_free(r);
        return NULL;
    }
    if (str_eq(stop, "error")) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xasprintf("%s failed while writing the edit: %s", mc->name, em ? em : "unknown error");
        json_free(r);
        return NULL;
    }
    return r;
}

/* the finished draft as one replace_all edit of the whole snapshot (always checked against the raw file) */
static void mc_apply(magic_io_t *io, const char *old_s, const char *new_s, pe_result_t *r) {
    magic_job_t *job = io->ud;
    magic_call_t *mc = job->mc;
    json_t *args = json_obj();
    json_set_str(args, "path", job->section->path);
    json_t *edits = json_arr(), *e = json_obj();
    json_set_str(e, "old_string", old_s);
    json_set_str(e, "new_string", new_s);
    json_set_bool(e, "replace_all", true);
    json_push(edits, e);
    json_set(args, "edits", edits);
    pe_opts_t o = *mc->base;
    o.raw_snapshot = job->raw;
    o.raw_snapshot_len = job->raw_len;
    /* completions overlap; writes go one at a time, as within any other edit call */
    pthread_mutex_lock(&mc->env->mu);
    pe_run(PE_REPLACE, args, &o, r);
    pthread_mutex_unlock(&mc->env->mu);
    json_free(args);
}

/* applySection: read the file, run the draft loop against it, commit against the snapshot */
static void *magic_section_run(void *arg) {
    magic_job_t *job = arg;
    magic_call_t *mc = job->mc;
    const char *target = job->section->path;
    char *e = NULL;
    char *abs = mc->base->resolve ? mc->base->resolve(mc->base->ud, target, &e) : path_abs(target, mc->base->cwd);
    if (!abs) {
        job->err = e ? e : xasprintf("%s could not be resolved.", target);
        return NULL;
    }
    struct stat st;
    if (stat(abs, &st) == 0 && S_ISDIR(st.st_mode)) {
        job->err = xasprintf("EISDIR: illegal operation on a directory, read '%s'", abs);
        free(abs);
        return NULL;
    }
    size_t len = 0;
    char *raw = fs_read(abs, &len);
    if (!raw) {
        int en = errno;
        job->err = en == ENOENT ? xasprintf("%s does not exist; this edit changes existing files.", target)
                                : xasprintf("%s (os error %d)", strerror(en), en);
        free(abs);
        return NULL;
    }
    free(abs);
    if (memchr(raw, 0, len)) {
        job->err = xasprintf("%s is binary; this edit changes text files.", target);
        free(raw);
        return NULL;
    }
    job->raw = raw;
    job->raw_len = len;
    /* LF line ends and no BOM, as the edit engine matches it */
    char *content = to_lf(str_starts(raw, "\xef\xbb\xbf") ? raw + 3 : raw);
    int64_t window = mc->model->context_window;
    double limit = window > 0 ? (double)window * MAX_CONTEXT_SHARE * CHARS_PER_TOKEN : -1;
    magic_views_t views;
    magic_file_views(content, job->section->change, limit, &views);
    magic_io_t io = {mc_complete, mc_apply, job};
    job->ok = magic_apply_described(target, job->section->change, content, &views, &io, &job->res, &job->err);
    magic_views_free(&views);
    free(content);
    return NULL;
}

/* resolveApplier: edit.magic.model (a selector or role alias), else the smol role; models.offline keeps it on the
 * session's provider */
static bool magic_resolve(magic_call_t *mc, char **err) {
    tool_env_t *env = mc->env;
    char *sid = NULL;
    const ai_model_t *active = env->active_model ? env->active_model(env, &sid) : NULL;
    mc->session_id = sid;
    mr_pool_t avail = {0}, pool = {0};
    mr_pool_available(&avail, "chat");
    bool offline = settings_bool("models.offline", false);
    for (size_t i = 0; i < avail.n; i++)
        if (!offline || (active && str_eq(avail.v[i]->provider, active->provider))) mr_pool_push(&pool, avail.v[i]);
    mr_pool_free(&avail);
    const char *selector = settings_str("edit.magic.model", NULL);
    if (selector && !*selector) selector = NULL;
    const char *level = NULL;
    const ai_model_t *m = NULL;
    if (selector) {
        mr_result_t r;
        if (mr_resolve_value(selector, &pool, &r)) {
            m = r.model;
            level = r.level;
        }
        mr_result_clear(&r);
    } else {
        m = mr_role_selection("smol", &pool, &level);
    }
    mr_pool_free(&pool);
    /* typing a described change needs no deliberation: reasoning runs only when the selector asks (`…:low`) */
    ai_effort_t e;
    if (level && strcmp(level, "off") && strcmp(level, "inherit") && strcmp(level, "auto") && effort_parse(level, &e) == 0) {
        mc->effort = e;
        mc->has_effort = true;
    }
    if (!m) {
        *err = selector ? xasprintf("`edit.magic.model` (%s) matches no available model.", selector)
                        : xstrdup("`edit.mode: magic` needs a model to apply the change: configure the `smol` role or "
                                  "`edit.magic.model`.");
        return false;
    }
    mc->model = m;
    mc->name = xasprintf("%s/%s", m->provider, m->id);
    mc->api_key = auth_get_api_key(m->provider);
    if (!mc->api_key && !mr_provider_available(m->provider)) {
        *err = xasprintf("The edit cannot reach %s: no credential for %s.", mc->name, m->provider);
        return false;
    }
    return true;
}

static void magic_call_free(magic_call_t *mc) {
    free(mc->name);
    free(mc->api_key);
    free(mc->session_id);
    free(mc->system);
}

void magic_edit_run(tool_env_t *env, volatile sig_atomic_t *cancel, const char *input, const pe_opts_t *base,
                    tool_result_t *out) {
    magic_sections_t sections;
    char *err = NULL;
    if (!magic_parse_input(input, &sections, &err)) {
        tool_result_text(out, err);
        out->is_error = true;
        free(err);
        return;
    }
    magic_call_t mc = {0};
    mc.env = env;
    mc.base = base;
    mc.tool_cancel = cancel;
    if (!magic_resolve(&mc, &err)) {
        tool_result_text(out, err);
        out->is_error = true;
        free(err);
        magic_call_free(&mc);
        magic_sections_free(&sections);
        return;
    }
    json_t *sctx = json_obj();
    mc.system = prompt_render("coding-agent/edit/magic-apply-system.md", sctx);
    json_free(sctx);
    if (!mc.system) mc.system = xstrdup("");
    /* one budget across every turn: the calling agent's turn blocks on the edit */
    mc.deadline = now_ms() + APPLY_TIMEOUT_MS;
    pthread_t watcher;
    bool watching = pthread_create(&watcher, NULL, magic_watch, &mc) == 0;
    size_t n = sections.n;
    magic_job_t *jobs = xcalloc(n, sizeof(magic_job_t));
    pthread_t *th = xcalloc(n, sizeof(pthread_t));
    bool *started = xcalloc(n, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        jobs[i].mc = &mc;
        jobs[i].section = &sections.v[i];
    }
    if (n == 1) {
        magic_section_run(&jobs[0]);
    } else {
        /* the files' completions run concurrently; their writes do not */
        for (size_t i = 0; i < n; i++) started[i] = pthread_create(&th[i], NULL, magic_section_run, &jobs[i]) == 0;
        for (size_t i = 0; i < n; i++) {
            if (started[i]) pthread_join(th[i], NULL);
            else magic_section_run(&jobs[i]);
        }
    }
    mc.finished = 1;
    if (watching) pthread_join(watcher, NULL);

    if (n == 1) {
        if (jobs[0].ok) {
            tool_result_text(out, jobs[0].res.text ? jobs[0].res.text : "");
            out->is_error = jobs[0].res.is_error;
            out->details = jobs[0].res.details;
            jobs[0].res.details = NULL;
        } else {
            tool_result_text(out, jobs[0].err ? jobs[0].err : "");
            out->is_error = true;
        }
    } else {
        json_t *files = json_arr();
        sb_t diff = SB_INIT;
        int64_t first_changed = 0;
        size_t applied = 0;
        for (size_t i = 0; i < n; i++) {
            magic_job_t *j = &jobs[i];
            json_t *f = json_obj();
            if (j->ok) {
                tool_result_text(out, j->res.text ? j->res.text : "");
                const json_t *d = j->res.details;
                const char *dp = json_get_str(d, "path");
                const char *dd = json_get_str(d, "diff");
                json_set_str(f, "path", dp ? dp : j->section->path);
                json_set_str(f, "diff", dd ? dd : "");
                if (json_get(d, "firstChangedLine")) json_set(f, "firstChangedLine", json_clone(json_get(d, "firstChangedLine")));
                if (json_get(d, "diagnostics")) json_set(f, "diagnostics", json_clone(json_get(d, "diagnostics")));
                if (json_get(d, "op")) json_set(f, "op", json_clone(json_get(d, "op")));
                if (json_get(d, "meta")) json_set(f, "meta", json_clone(json_get(d, "meta")));
                if (applied++) sb_putc(&diff, '\n');
                sb_append(&diff, dd ? dd : "");
                if (applied == 1) first_changed = json_get_int(d, "firstChangedLine", 0);
            } else {
                char *t = xasprintf("[%s] not applied: %s", j->section->path, j->err ? j->err : "");
                tool_result_text(out, t);
                free(t);
                json_set_str(f, "path", j->section->path);
                json_set_str(f, "diff", "");
                json_set_bool(f, "isError", true);
                json_set_str(f, "errorText", j->err ? j->err : "");
            }
            json_push(files, f);
        }
        json_t *d = json_obj();
        json_set_str(d, "diff", sb_cstr(&diff));
        if (first_changed) json_set_int(d, "firstChangedLine", first_changed);
        json_set(d, "perFileResults", files);
        out->details = d;
        out->is_error = applied == 0;
        sb_free(&diff);
    }
    for (size_t i = 0; i < n; i++) {
        free(jobs[i].raw);
        free(jobs[i].err);
        pe_result_free(&jobs[i].res);
    }
    free(jobs);
    free(th);
    free(started);
    magic_call_free(&mc);
    magic_sections_free(&sections);
}
