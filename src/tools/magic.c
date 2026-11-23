/* magic.c - `edit.mode: magic` (see magic.h). Port of omp edit/magic.ts, magic-lines.ts, magic-view.ts and
 * tui tools/magic-sections.ts at v19.1.0. Lengths the model or a limit sees are UTF-16 units, like JS. */
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
 * JS string helpers (String.prototype.trim / \s, ASCII \w)
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

static bool word_ch(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool digit_ch(unsigned char c) { return c >= '0' && c <= '9'; }

/* bytes of the longest prefix of s that is at most `units` UTF-16 units */
static size_t utf16_prefix(const char *s, size_t units) {
    size_t i = 0, u = 0, len = strlen(s);
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        size_t w = cp >= 0x10000 ? 2 : 1;
        if (u + w > units) break;
        u += w;
        i += (size_t)n;
    }
    return i;
}

/* String.prototype.replaceAll(old, replacement) with a string pattern: GetSubstitution's $$, $&, $` and $' */
static char *js_replace_all(const char *s, const char *old, const char *rep) {
    sb_t b = SB_INIT;
    size_t ol = strlen(old);
    const char *p = s, *m;
    while ((m = strstr(p, old))) {
        sb_appendn(&b, p, (size_t)(m - p));
        for (const char *r = rep; *r; r++) {
            if (r[0] == '$' && r[1] == '$') {
                sb_putc(&b, '$');
                r++;
            } else if (r[0] == '$' && r[1] == '&') {
                sb_appendn(&b, m, ol);
                r++;
            } else if (r[0] == '$' && r[1] == '`') {
                sb_appendn(&b, s, (size_t)(m - s));
                r++;
            } else if (r[0] == '$' && r[1] == '\'') {
                sb_append(&b, m + ol);
                r++;
            } else {
                sb_putc(&b, *r);
            }
        }
        p = m + ol;
    }
    sb_append(&b, p);
    return sb_detach(&b);
}

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
 * file views (edit/magic-view.ts)
 * ====================================================================================== */

#define CONTEXT_LINES 12
#define MAX_BLOCK_LINES 150
#define MAX_DISTINCT_HITS 5
#define NEAR_LINES 3
#define REPEAT_SAMPLE 3
#define MIN_FILE_LINES 200
#define MIN_OMITTED_SHARE 0.5

typedef VEC(magic_span_t) spans_t;
typedef VEC(int) ints_t;

/* blockEnd: last line of the block lines[start] opens (its deeper-indented lines and their closer) */
static int block_end(char *const *lines, int n, int start, int max_lines) {
    int indent = indent_of(lines[start]);
    int limit = start + max_lines < n && start + max_lines > start ? start + max_lines : n;
    int end = start;
    for (int i = start + 1; i < limit; i++) {
        const char *line = lines[i];
        if (is_blank(line)) continue;
        int depth = indent_of(line);
        if (depth > indent) {
            end = i;
            continue;
        }
        const char *body = line + ws_run(line);
        if (depth < indent || end == start || !(*body == ']' || *body == ')' || *body == '}')) break;
        end = i;
        /* `} else {` closes one block and opens the next */
        const char *te = trim_end_ptr(line);
        if (!(te > line && (te[-1] == '[' || te[-1] == '(' || te[-1] == '{'))) break;
    }
    return end;
}

/* numbered: lines[from..to] behind their numbers; a line opening a block that ends within them shows `12-30` */
static void numbered(char *const *lines, int n, int from, int to, strvec_t *out) {
    for (int i = from; i <= to; i++) {
        int end = is_blank(lines[i]) ? i : block_end(lines, n, i, n);
        if (end > i && end <= to) strvec_push_own(out, xasprintf("%d-%d\t%s", i + 1, end + 1, lines[i]));
        else strvec_push_own(out, xasprintf("%d\t%s", i + 1, lines[i]));
    }
}

/* add(): split on /\.{3}|…/, squeeze, keep fragments with at least three ASCII word characters */
static void add_fragment(strvec_t *found, const char *text) {
    const char *p = text;
    for (;;) {
        const char *dots = strstr(p, "..."), *ell = strstr(p, "\xe2\x80\xa6");
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

/* IDENTIFIER at s[i]: camelCase | snake_case | dotted.name; match end or 0 */
static size_t identifier_at(const char *s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (!id_start(c)) return 0;
    size_t r = i + 1;
    while (id_char((unsigned char)s[r])) r++;
    /* [A-Za-z_$][\w$]*[a-z0-9][A-Z][\w$]* */
    for (size_t k = i + 1; k + 1 < r; k++) {
        unsigned char x = (unsigned char)s[k], y = (unsigned char)s[k + 1];
        if (((x >= 'a' && x <= 'z') || digit_ch(x)) && y >= 'A' && y <= 'Z') return r;
    }
    /* [A-Za-z$][\w$]*_[\w$]+ */
    if (c != '_')
        for (size_t k = i + 1; k + 1 < r; k++)
            if (s[k] == '_') return r;
    /* [A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)+ */
    size_t e = r;
    bool group = false;
    while (s[e] == '.' && id_start((unsigned char)s[e + 1])) {
        e += 2;
        while (id_char((unsigned char)s[e])) e++;
        group = true;
    }
    return group ? e : 0;
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
            magic_span_t sp = {(int)from - 1, (int)fmin(to, nlines) - 1};
            vec_push(out, sp);
        }
    }
    free(t);
}

/* numberedSpans: LINE_REFERENCE /\blines?\s+(\d+(?:\s*(?:,|-|–|to|through|and)\s*\d+)*)/gi */
static void numbered_spans(const char *change, int nlines, spans_t *out) {
    for (size_t i = 0; change[i];) {
        if ((i == 0 || !word_ch((unsigned char)change[i - 1])) && match_ci(change + i, "line")) {
            size_t p = i + 4;
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
#define SPAN_OF(line) ((magic_span_t){(line), block_end(lines, n, (line), MAX_BLOCK_LINES)})
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

static int span_cmp(const void *a, const void *b) {
    const magic_span_t *x = a, *y = b;
    return (x->from > y->from) - (x->from < y->from);
}

/* stable sort by `from` (JS Array.prototype.sort is stable) */
static void spans_sort(spans_t *s) {
    for (size_t i = 1; i < s->n; i++) {
        magic_span_t t = s->v[i];
        size_t j = i;
        while (j > 0 && span_cmp(&s->v[j - 1], &t) > 0) {
            s->v[j] = s->v[j - 1];
            j--;
        }
        s->v[j] = t;
    }
}

/* excerpt: the numbered lines the change names, or false */
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
        magic_span_t s = {named.v[i].from - CONTEXT_LINES < 0 ? 0 : named.v[i].from - CONTEXT_LINES,
                          named.v[i].to + CONTEXT_LINES > last ? last : named.v[i].to + CONTEXT_LINES};
        vec_push(&spans, s);
    }
    vec_free(&named);
    /* the head and tail anchor changes placed by position: imports, a new last function */
    magic_span_t head = {0, CONTEXT_LINES - 1}, tail = {last - CONTEXT_LINES + 1, last};
    vec_push(&spans, head);
    vec_push(&spans, tail);
    spans_sort(&spans);
    spans_t shown = {0};
    for (size_t i = 0; i < spans.n; i++) {
        /* a gap of one line costs as much to mark as to show */
        if (shown.n && spans.v[i].from <= vec_last(&shown).to + 2) {
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
    strvec_t out = {0};
    int next = 0;
    for (size_t i = 0; i < shown.n; i++) {
        if (shown.v[i].from > next) strvec_push_own(&out, xasprintf("\xe2\x8b\xae lines %d-%d not shown", next + 1, shown.v[i].from));
        numbered(lines, n, shown.v[i].from, shown.v[i].to, &out);
        next = shown.v[i].to + 1;
    }
    view->text = strvec_join(&out, "\n");
    strvec_free(&out);
    view->partial = true;
    view->shown = shown.v;
    view->nshown = shown.n;
    return true;
}

void magic_file_views(const char *content, const char *change, double limit, magic_views_t *out) {
    memset(out, 0, sizeof(*out));
    strvec_t lines = file_lines(content);
    int n = (int)lines.n;
    magic_views_t all = {0};
    magic_view_t part = {0};
    if (excerpt(lines.v, n, change, &part)) vec_push(&all, part);
    strvec_t whole = {0};
    numbered(lines.v, n, 0, n - 1, &whole);
    magic_view_t w = {strvec_join(&whole, "\n"), false, NULL, 0};
    strvec_free(&whole);
    vec_push(&all, w);
    for (size_t i = 0; i < all.n; i++) {
        if (limit < 0 || (double)fz_utf16_len(all.v[i].text) <= limit) {
            vec_push(out, all.v[i]);
        } else {
            free(all.v[i].text);
            free(all.v[i].shown);
        }
    }
    vec_free(&all);
    strvec_free(&lines);
}

void magic_views_free(magic_views_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].text);
        free(v->v[i].shown);
    }
    vec_free(v);
}

/* ======================================================================================
 * line operations (edit/magic-lines.ts)
 * ====================================================================================== */

static const char *const OP_NAMES[] = {"delete", "replace", "sub", "insert", "move", "swap", "indent", "dedent"};
static const char *const OP_FORMS[] = {
    "[\"delete\", first, last]",
    "[\"replace\", first, last, \"text\"]",
    "[\"sub\", line or \"all\", \"old\", \"new\"]",
    "[\"insert\", \"before\" or \"after\", line, \"text\"]",
    "[\"move\", first, last, \"before\" or \"after\", line]",
    "[\"swap\", first, last, first2, last2]",
    "[\"indent\", first, last, spaces or \"tab\"]",
    "[\"dedent\", first, last, spaces or \"tab\"]",
};
enum { OP_DELETE, OP_REPLACE, OP_SUB, OP_INSERT, OP_MOVE, OP_SWAP, OP_INDENT, OP_DEDENT, OP_COUNT };

static int op_index(const json_t *name) {
    if (!json_is_str(name)) return -1;
    for (int i = 0; i < OP_COUNT; i++)
        if (!strcmp(name->u.str.s, OP_NAMES[i])) return i;
    return -1;
}

/* NUMBERS: positions holding line numbers or counts (quoted ones are converted) */
static const int *op_numbers(int op, size_t *n) {
    static const int del[] = {1, 2}, sub[] = {1}, ins[] = {2}, mov[] = {1, 2, 4}, swp[] = {1, 2, 3, 4}, ind[] = {1, 2, 3};
    switch (op) {
    case OP_DELETE:
    case OP_REPLACE: *n = 2; return del;
    case OP_SUB: *n = 1; return sub;
    case OP_INSERT: *n = 1; return ins;
    case OP_MOVE: *n = 3; return mov;
    case OP_SWAP: *n = 4; return swp;
    case OP_INDENT:
    case OP_DEDENT: *n = 3; return ind;
    default: *n = 0; return NULL;
    }
}

static json_t *op_at(const json_t *op, size_t i) { return i < json_len(op) ? json_at(op, i) : NULL; }

static void op_set(json_t *op, size_t i, json_t *v) {
    if (i < json_len(op)) {
        json_free(json_arr_remove(op, i));
        json_arr_insert(op, i, v);
    } else {
        json_push(op, v);
    }
}

/* `{` opening an operation written with braces: /^\{\s*"[a-z]+"\s*,/ on the next 40 characters */
static bool braced_operation(const char *p) {
    char *t = xstrndup(p, utf16_prefix(p, 40));
    const char *q = t + 1;
    bool ok = false;
    if (t[0] == '{') {
        q += ws_run(q);
        if (*q == '"') {
            q++;
            const char *a = q;
            while (*q >= 'a' && *q <= 'z') q++;
            if (q > a && *q == '"') {
                q++;
                q += ws_run(q);
                ok = *q == ',';
            }
        }
    }
    free(t);
    return ok;
}

/* index of the bracket closing the one at text[start] ([ or {), or -1 */
static long bracket_end(const char *text, size_t len, size_t start) {
    int depth = 0;
    bool quoted = false;
    for (size_t i = start; i < len; i++) {
        char c = text[i];
        if (quoted) {
            if (c == '\\') i++;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if ((c == ']' || c == '}') && --depth == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* readOperations: the operations a reply holds (owned array) and whether commentary follows them; NULL when the
 * reply is unusable */
static json_t *read_operations(const char *reply, size_t len, bool *commented_out, int depth) {
    if (depth > 64) return NULL;
    json_t *ops = json_arr();
    bool commented = false;
    size_t index = 0;
    while (index < len) {
        char c = reply[index];
        size_t w = ws_at(reply + index);
        if (w || c == ',' || c == ']') {
            index += w ? w : 1;
            continue;
        }
        bool braced = c == '{' && braced_operation(reply + index);
        if (c != '[' && !braced) {
            /* a fence line, or commentary: neither holds an operation */
            if (!(len - index >= 3 && !strncmp(reply + index, "```", 3)) && json_len(ops) > 0) commented = true;
            const char *nl = memchr(reply + index, '\n', len - index);
            index = nl ? (size_t)(nl - reply) + 1 : len;
            continue;
        }
        long end = bracket_end(reply, len, index);
        if (end < 0 || commented) {
            json_free(ops);
            return NULL;
        }
        size_t inner_len = (size_t)end - index - 1;
        sb_t src = SB_INIT;
        sb_putc(&src, '[');
        sb_appendn(&src, reply + index + 1, inner_len);
        sb_putc(&src, ']');
        char *perr = NULL;
        json_t *value = json_parse(src.s, src.len, &perr);
        free(perr);
        sb_free(&src);
        if (!value) {
            /* an outer array of braced operations: read its inside the same way, once */
            bool icomm = false;
            json_t *inner = reply[index] == '[' ? read_operations(reply + index + 1, inner_len, &icomm, depth + 1) : NULL;
            const char *in = reply + index + 1;
            char *head = xstrndup(in, inner_len);
            bool brace = head[ws_run(head)] == '{';
            free(head);
            if (!inner || icomm || !brace) {
                json_free(inner);
                json_free(ops);
                return NULL;
            }
            value = inner;
        }
        bool all_arrays = json_is_arr(value);
        for (size_t i = 0; all_arrays && i < json_len(value); i++) all_arrays = json_is_arr(json_at(value, i));
        json_t *found;
        if (all_arrays) {
            found = value;
        } else {
            found = json_arr();
            json_push(found, value);
        }
        while (json_len(found)) {
            json_t *op = json_arr_remove(found, 0);
            if (!json_is_arr(op) || !json_len(op)) {
                json_free(op);
                continue;
            }
            size_t nn;
            const int *pos = op_numbers(op_index(json_at(op, 0)), &nn);
            for (size_t k = 0; k < nn; k++) {
                json_t *v = op_at(op, (size_t)pos[k]);
                if (!json_is_str(v) || !v->u.str.len) continue;
                bool digits = true;
                for (size_t q = 0; q < v->u.str.len && digits; q++) digits = digit_ch((unsigned char)v->u.str.s[q]);
                if (digits) op_set(op, (size_t)pos[k], json_num(strtod(v->u.str.s, NULL)));
            }
            json_push(ops, op);
        }
        json_free(found);
        index = (size_t)end + 1;
    }
    *commented_out = commented;
    return ops;
}

/* JS === between operation items (arrays/objects only equal themselves) */
static bool js_strict_eq(const json_t *a, const json_t *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return false;
    switch (a->type) {
    case JSON_NULL: return true;
    case JSON_BOOL: return a->u.b == b->u.b;
    case JSON_NUMBER: return a->u.num.d == b->u.num.d;
    case JSON_STRING: return a->u.str.len == b->u.str.len && !memcmp(a->u.str.s, b->u.str.s, a->u.str.len);
    default: return a == b;
    }
}

/* detectIndentFromContent (lsp/format-options.ts): *insert_spaces -1 unknown, 0 tabs, 1 spaces; *tab_size 0 unknown */
static void detect_indent(const char *content, int *insert_spaces, int *tab_size) {
    *insert_spaces = -1;
    *tab_size = 0;
    if (!*content) return;
    bool in_block = false;
    int prev = 0;
    int counts[512] = {0};
    strvec_t lines = str_split(content, "\n");
    for (size_t i = 0; i < lines.n; i++) {
        const char *line = lines.v[i];
        char *body = js_trim(line);
        if (!*body) {
            free(body);
            continue;
        }
        if (in_block) {
            if (strstr(body, "*/")) in_block = false;
            free(body);
            continue;
        }
        if (str_starts(body, "/*") && !strstr(body + 2, "*/")) in_block = true;
        free(body);
        if (line[0] == '\t') {
            if (*insert_spaces < 0) *insert_spaces = 0;
            continue;
        }
        int width = 0;
        while (line[width] == ' ') width++;
        if (width > 0 && *insert_spaces < 0) *insert_spaces = 1;
        if (width > prev) {
            int step = width - prev;
            if (step < 512) counts[step]++;
        }
        prev = width;
    }
    strvec_free(&lines);
    if (*insert_spaces == 1) {
        int best = 0, best_count = 0;
        for (int s = 1; s < 512; s++)
            if (counts[s] > best_count) {
                best = s;
                best_count = counts[s];
            }
        if (best > 0) *tab_size = best;
    }
}

/* normalize: near-miss forms rewritten in place */
static void normalize_op(json_t *op, const char *content) {
    json_t *n0 = op_at(op, 0);
    if (json_is_str(n0) && !strcmp(n0->u.str.s, "remove")) op_set(op, 0, json_str("delete"));
    int name = op_index(op_at(op, 0));
    size_t len = json_len(op);
    json_t *a = op_at(op, 1), *b = op_at(op, 2), *c = op_at(op, 3), *d = op_at(op, 4);
    if (name == OP_DELETE && len == 2) {
        json_push(op, json_clone(a));
    } else if (name == OP_SWAP && (len == 3 || (len == 5 && js_strict_eq(a, d) && js_strict_eq(b, c) && !js_strict_eq(a, b)))) {
        json_t *ca = json_clone(a), *cb = json_clone(b);
        while (json_len(op) > 1) json_free(json_arr_remove(op, json_len(op) - 1));
        json_push(op, json_clone(ca));
        json_push(op, ca);
        json_push(op, json_clone(cb));
        json_push(op, cb);
    } else if (name == OP_INDENT || name == OP_DEDENT) {
        if (len == 3) {
            int spaces, size;
            detect_indent(content, &spaces, &size);
            if (spaces == 0) json_push(op, json_str("tab"));
            else if (size > 0) json_push(op, json_num(size));
        } else if (json_is_num(c) && c->u.num.d < 0) {
            double amount = -c->u.num.d;
            op_set(op, 0, json_str(name == OP_INDENT ? "dedent" : "indent"));
            op_set(op, 3, json_num(amount));
        }
    }
}

static bool shown_within(const magic_view_t *view, int from, int to) {
    if (!view->partial) return true;
    for (size_t i = 0; i < view->nshown; i++)
        if (from >= view->shown[i].from && to <= view->shown[i].to) return true;
    return false;
}

char *magic_block_hints(const char *content, int (*ranges)[2], size_t nr, const magic_view_t *view) {
    strvec_t lv = file_lines(content);
    char **lines = lv.v;
    int n = (int)lv.n;
    strvec_t hints = {0};
    for (size_t r = 0; r < nr; r++) {
        int start = ranges[r][0] - 1, stop = ranges[r][1] - 1;
        if (start < 0 || stop >= n) continue;
        while (start < stop && is_blank(lines[start])) start++;
        while (stop > start && is_blank(lines[stop])) stop--;
        /* blank lines open nothing */
        if (is_blank(lines[start])) continue;
        int end = block_end(lines, n, start, n);
        if (end == start || end == stop || !shown_within(view, start, end)) continue;
        /* unwrapping: the opening line goes alone, and another range takes the line that closes its block */
        if (start == stop) {
            bool unwrap = false;
            for (size_t k = 0; k < nr && !unwrap; k++) unwrap = ranges[k][0] == end + 1 && ranges[k][1] == end + 1;
            if (unwrap) continue;
        }
        bool whole = stop > end;
        for (int at = end + 1; whole && at <= stop; at++) {
            if (is_blank(lines[at])) continue;
            int unit = block_end(lines, n, at, n);
            whole = unit > at && unit <= stop && shown_within(view, at, unit);
            at = unit;
        }
        if (whole) continue;
        char *text = js_trim(lines[stop]);
        size_t cut = utf16_prefix(text, 60);
        char *shown = text[cut] ? xasprintf("%.*s\xe2\x80\xa6", (int)cut, text) : xstrdup(text);
        strvec_push_own(&hints, xasprintf("line %d opens a block that ends on line %d, but the range ends on line %d (`%s`)",
                                          start + 1, end + 1, stop + 1, shown));
        free(shown);
        free(text);
    }
    char *out = strvec_join(&hints, "; ");
    strvec_free(&hints);
    strvec_free(&lv);
    return out;
}

typedef struct {
    int from, to; /* half-open, zero-based */
    strvec_t lines;
    size_t seq;
} splice_t;
typedef VEC(splice_t) splices_t;

typedef struct {
    int from, to;
    char *prefix;
    bool dedent;
    size_t seq;
} shift_t;
typedef VEC(shift_t) shifts_t;

typedef struct {
    char **lines;
    int n;
    const magic_view_t *view;
} cctx_t;

static bool cvisible(const cctx_t *c, int from, int to) { return shown_within(c->view, from, to); }
/* blank lines separate blocks; only shown lines count */
static bool cblank(const cctx_t *c, int i) { return is_blank(c->lines[i]) && cvisible(c, i, i); }

static bool opens_block(const char *line) {
    const char *te = trim_end_ptr(line);
    return te > line && strchr("[({:", te[-1]) && te[-1];
}

static bool closes_block(const char *line) {
    const char *p = line + ws_run(line);
    return *p == ']' || *p == ')' || *p == '}';
}

/* withSeparator: the lines that leave with [from, to), one run of blank lines included when set off on both sides */
static void with_separator(const cctx_t *c, int from, int to, int *f, int *t, int *carried) {
    *f = from;
    *t = to;
    *carried = 0;
    if (from == to || cblank(c, from) || cblank(c, to - 1)) return;
    int above = 0, below = 0;
    while (from - above > 0 && cblank(c, from - above - 1)) above++;
    while (to + below < c->n && cblank(c, to + below)) below++;
    bool top = from - above == 0 || (above == 0 && opens_block(c->lines[from - 1]));
    bool bottom = to + below == c->n || (below == 0 && closes_block(c->lines[to]));
    if (above > 0 && below > 0) {
        *carried = above < below ? above : below;
        *t = to + *carried;
    } else if (below > 0 && top) {
        *t = to + below;
        *carried = below;
    } else if (above > 0 && bottom) {
        *f = from - above;
        *carried = above;
    }
}

static void core_range(const cctx_t *c, int *from, int *to) {
    while (*from < *to && cblank(c, *from)) (*from)++;
    while (*to > *from && cblank(c, *to - 1)) (*to)--;
}

static bool is_line_v(const json_t *v, int n) {
    return json_is_num(v) && v->u.num.d == floor(v->u.num.d) && v->u.num.d >= 1 && v->u.num.d <= n;
}
static int line_of(const json_t *v) { return (int)v->u.num.d; }
static bool is_text_v(const json_t *v) {
    return json_is_str(v) && !memchr(v->u.str.s, '\r', v->u.str.len) && !memchr(v->u.str.s, 0, v->u.str.len) &&
           strlen(v->u.str.s) == v->u.str.len;
}
static bool is_side_v(const json_t *v) { return json_is_str(v) && (!strcmp(v->u.str.s, "before") || !strcmp(v->u.str.s, "after")); }

static void split_text(const char *s, strvec_t *out) {
    strvec_t parts = str_split(s, "\n");
    if (!parts.n) strvec_push(&parts, "");
    for (size_t i = 0; i < parts.n; i++) strvec_push(out, parts.v[i]);
    strvec_free(&parts);
}

static int splice_cmp(const splice_t *a, const splice_t *b) {
    if (a->from != b->from) return a->from < b->from ? -1 : 1;
    if (a->to != b->to) return a->to < b->to ? -1 : 1;
    return (a->seq > b->seq) - (a->seq < b->seq);
}

static int splice_qcmp(const void *a, const void *b) { return splice_cmp(a, b); }

static int shift_qcmp(const void *a, const void *b) {
    const shift_t *x = a, *y = b;
    if (x->from != y->from) return x->from < y->from ? -1 : 1;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

static void push_range(magic_compiled_t *out, int first, int last) {
    out->ranges = xrealloc(out->ranges, (out->nranges + 1) * sizeof(*out->ranges));
    out->ranges[out->nranges][0] = first;
    out->ranges[out->nranges][1] = last;
    out->nranges++;
}

void magic_compiled_free(magic_compiled_t *c) {
    free(c->old_s);
    free(c->new_s);
    free(c->ranges);
    free(c->problem);
    memset(c, 0, sizeof(*c));
}

void magic_compile(const char *reply, const char *content, const magic_view_t *view, bool cut, magic_compiled_t *out) {
    memset(out, 0, sizeof(*out));
    bool commented = false;
    json_t *parsed = read_operations(reply, strlen(reply), &commented, 0);
    /* a reply cut at the output limit counts only if commentary shows the model had finished its list */
    if (!parsed || !json_len(parsed) || (cut && !commented)) {
        json_free(parsed);
        out->problem = xstrdup("Output only JSON operation arrays, one per line, and nothing else.");
        return;
    }
    if (!*content) {
        json_free(parsed);
        out->problem = xstrdup("Cannot anchor an edit in an empty file; use write instead.");
        return;
    }
    /* a repeated operation is the same request twice */
    json_t *ops = json_arr();
    strvec_t seen = {0};
    while (json_len(parsed)) {
        json_t *op = json_arr_remove(parsed, 0);
        char *key = json_dump_js(op);
        if (strvec_contains(&seen, key)) {
            free(key);
            json_free(op);
            continue;
        }
        strvec_push_own(&seen, key);
        json_push(ops, op);
    }
    strvec_free(&seen);
    json_free(parsed);

    strvec_t lv = file_lines(content);
    int n = (int)lv.n;
    cctx_t c = {lv.v, n, view};
    splices_t splices = {0};
    shifts_t shifts = {0};
    VEC(magic_span_t) moved = {0};
    char **subs = xcalloc((size_t)n + 1, sizeof(char *));
    ints_t sub_order = {0};
    strvec_t *inserts = xcalloc((size_t)n + 2, sizeof(strvec_t));
    ints_t ins_order = {0};
    size_t seq = 0;
    char *problem = NULL;

    for (size_t index = 0; index < json_len(ops) && !problem; index++) {
        json_t *op = json_at(ops, index);
        /* problems quote the operation as the model wrote it */
        char *written = json_dump_js(op);
#define PROBLEM(...)                                                                                                   \
    do {                                                                                                               \
        char *reason_ = xasprintf(__VA_ARGS__);                                                                        \
        problem = xasprintf("Operation %zu (%s): %s", index + 1, written, reason_);                                    \
        free(reason_);                                                                                                 \
    } while (0)
        if (!json_is_arr(op)) {
            PROBLEM("expected an operation array.");
            free(written);
            break;
        }
        normalize_op(op, content);
        int name = op_index(op_at(op, 0));
        if (name < 0) {
            PROBLEM("unknown operation; use one of delete, replace, sub, insert, move, swap, indent, dedent.");
            free(written);
            break;
        }
        const char *form = OP_FORMS[name];
        size_t len = json_len(op);
#define MALFORMED() PROBLEM("expected %s.", form)
        /* range(first, last): half-open zero-based span, recorded for blockHints when it deletes/moves/swaps */
#define RANGE(fv, lv_, ok_, f_, t_)                                                                                     \
    do {                                                                                                               \
        ok_ = is_line_v(fv, n) && is_line_v(lv_, n) && line_of(fv) <= line_of(lv_);                                    \
        if (ok_) {                                                                                                     \
            f_ = line_of(fv) - 1;                                                                                      \
            t_ = line_of(lv_);                                                                                         \
            if (name == OP_DELETE || name == OP_MOVE || name == OP_SWAP) push_range(out, line_of(fv), line_of(lv_));   \
        }                                                                                                              \
    } while (0)
#define HIDDEN(f_, t_) (!cvisible(&c, (f_), (t_) - 1))
        switch (name) {
        case OP_DELETE: {
            bool ok = false;
            int f = 0, t = 0;
            if (len == 3) RANGE(op_at(op, 1), op_at(op, 2), ok, f, t);
            if (!ok) {
                MALFORMED();
                break;
            }
            if (HIDDEN(f, t)) {
                PROBLEM("range includes lines not shown.");
                break;
            }
            int sf, st, carried;
            with_separator(&c, f, t, &sf, &st, &carried);
            splice_t s = {sf, st, {0}, seq++};
            vec_push(&splices, s);
            break;
        }
        case OP_REPLACE: {
            bool ok = false;
            int f = 0, t = 0;
            if (len == 4) RANGE(op_at(op, 1), op_at(op, 2), ok, f, t);
            if (!ok || !is_text_v(op_at(op, 3))) {
                MALFORMED();
                break;
            }
            if (HIDDEN(f, t)) {
                PROBLEM("range includes lines not shown.");
                break;
            }
            splice_t s = {f, t, {0}, seq++};
            split_text(op_at(op, 3)->u.str.s, &s.lines);
            vec_push(&splices, s);
            break;
        }
        case OP_SUB: {
            json_t *line = op_at(op, 1), *old = op_at(op, 2), *rep = op_at(op, 3);
            bool all = json_is_str(line) && !strcmp(line->u.str.s, "all");
            if (len != 4 || (!all && !is_line_v(line, n)) || !is_text_v(old) || !old->u.str.len || !is_text_v(rep)) {
                MALFORMED();
                break;
            }
            const char *os = old->u.str.s, *rs = rep->u.str.s;
            if (strchr(os, '\n') || strchr(rs, '\n')) {
                PROBLEM("sub changes text within one line.");
                break;
            }
#define CURRENT(at) (subs[at] ? subs[at] : c.lines[at])
#define SET_SUB(at)                                                                                                    \
    do {                                                                                                               \
        char *next_ = js_replace_all(CURRENT(at), os, rs);                                                             \
        if (!subs[at]) vec_push(&sub_order, (at));                                                                     \
        free(subs[at]);                                                                                                \
        subs[at] = next_;                                                                                              \
    } while (0)
            if (all) {
                bool found = false;
                for (int at = 0; at < n; at++) {
                    if (!strstr(CURRENT(at), os)) continue;
                    SET_SUB(at);
                    found = true;
                }
                if (!found) {
                    sb_t q = SB_INIT;
                    json_js_quote_sb(&q, os, strlen(os));
                    PROBLEM("the file does not contain %s.", q.s);
                    sb_free(&q);
                }
                break;
            }
            int ln = line_of(line);
            if (HIDDEN(ln - 1, ln)) {
                PROBLEM("line is not shown.");
                break;
            }
            if (!strstr(CURRENT(ln - 1), os)) {
                ints_t holders = {0};
                for (int at = 0; at < n; at++)
                    if (strstr(c.lines[at], os) && cvisible(&c, at, at)) vec_push(&holders, at);
                /* the line number is a hint: text that only one shown line holds is meant there */
                if (holders.n == 1) {
                    int at = holders.v[0];
                    SET_SUB(at);
                    vec_free(&holders);
                    break;
                }
                int gap = INT32_MAX;
                for (size_t k = 0; k < holders.n; k++) {
                    int g = abs(holders.v[k] + 1 - ln);
                    if (g < gap) gap = g;
                }
                sb_t where = SB_INIT;
                for (size_t k = 0; k < holders.n; k++)
                    if (abs(holders.v[k] + 1 - ln) == gap)
                        sb_appendf(&where, "%s%d", where.len ? " or " : "; the nearest line that does is ", holders.v[k] + 1);
                sb_t q = SB_INIT;
                json_js_quote_sb(&q, os, strlen(os));
                PROBLEM("line %d does not contain %s%s.", ln, q.s, sb_cstr(&where));
                sb_free(&q);
                sb_free(&where);
                vec_free(&holders);
                break;
            }
            SET_SUB(ln - 1);
#undef SET_SUB
#undef CURRENT
            break;
        }
        case OP_INSERT: {
            json_t *side = op_at(op, 1), *line = op_at(op, 2), *text = op_at(op, 3);
            if (len != 4 || !is_side_v(side) || !is_line_v(line, n) || !is_text_v(text)) {
                MALFORMED();
                break;
            }
            int ln = line_of(line);
            if (HIDDEN(ln - 1, ln)) {
                PROBLEM("anchor line is not shown.");
                break;
            }
            int at = !strcmp(side->u.str.s, "before") ? ln - 1 : ln;
            if (!inserts[at].n && !inserts[at].v) vec_push(&ins_order, at);
            if (!inserts[at].v) inserts[at].v = xcalloc(1, sizeof(char *)), inserts[at].cap = 1;
            split_text(text->u.str.s, &inserts[at]);
            break;
        }
        case OP_MOVE: {
            json_t *first = op_at(op, 1), *last = op_at(op, 2), *side = op_at(op, 3), *line = op_at(op, 4);
            bool ok = false;
            int f = 0, t = 0;
            if (len == 5 && is_side_v(side) && is_line_v(line, n)) RANGE(first, last, ok, f, t);
            if (!ok) {
                MALFORMED();
                break;
            }
            int ln = line_of(line);
            if (HIDDEN(f, t) || HIDDEN(ln - 1, ln)) {
                PROBLEM("range or destination is not shown.");
                break;
            }
            int bf = f, bt = t;
            core_range(&c, &bf, &bt);
            if (bf == bt) {
                PROBLEM("the range holds only blank lines.");
                break;
            }
            int sf, st, carried;
            with_separator(&c, bf, bt, &sf, &st, &carried);
            bool before = !strcmp(side->u.str.s, "before");
            int at = before ? ln - 1 : ln;
            if (at > sf && at < st) {
                PROBLEM("the destination is inside the moved lines.");
                break;
            }
            if (at == sf || at == st) {
                PROBLEM("lines %d-%d are already %s line %d (blank lines between blocks stay as separators), so nothing "
                        "moves; to exchange two blocks, use swap.",
                        line_of(first), line_of(last), side->u.str.s, ln);
                break;
            }
            /* a block set off by blank lines is set off again where it lands, except right inside a block */
            int pad_above = carried > 0 && at - 1 >= 0 && at - 1 < n && !cblank(&c, at - 1) && !opens_block(c.lines[at - 1]) ? carried : 0;
            int pad_below = carried > 0 && at >= 0 && at < n && !cblank(&c, at) && !closes_block(c.lines[at]) ? carried : 0;
            splice_t rm = {sf, st, {0}, seq++};
            vec_push(&splices, rm);
            splice_t ins = {at, at, {0}, seq++};
            for (int k = 0; k < pad_above; k++) strvec_push(&ins.lines, "");
            for (int k = bf; k < bt; k++) strvec_push(&ins.lines, c.lines[k]);
            for (int k = 0; k < pad_below; k++) strvec_push(&ins.lines, "");
            vec_push(&splices, ins);
            magic_span_t mv = {sf, st};
            vec_push(&moved, mv);
            break;
        }
        case OP_SWAP: {
            bool ok1 = false, ok2 = false;
            int f1 = 0, t1 = 0, f2 = 0, t2 = 0;
            if (len == 5) {
                RANGE(op_at(op, 1), op_at(op, 2), ok1, f1, t1);
                RANGE(op_at(op, 3), op_at(op, 4), ok2, f2, t2);
            }
            if (!ok1 || !ok2) {
                MALFORMED();
                break;
            }
            if (HIDDEN(f1, t1) || HIDDEN(f2, t2)) {
                PROBLEM("a range includes lines not shown.");
                break;
            }
            /* blank lines at the edges are separators, not content: they stay in place */
            core_range(&c, &f1, &t1);
            core_range(&c, &f2, &t2);
            if (f2 < f1) {
                int tf = f1, tt = t1;
                f1 = f2;
                t1 = t2;
                f2 = tf;
                t2 = tt;
            }
            if (f1 == t1 || f2 == t2) {
                PROBLEM("a range holds only blank lines.");
                break;
            }
            if (f2 < t1) {
                PROBLEM("the two ranges overlap.");
                break;
            }
            splice_t s1 = {f1, t1, {0}, seq++}, s2 = {f2, t2, {0}, seq++};
            for (int k = f2; k < t2; k++) strvec_push(&s1.lines, c.lines[k]);
            for (int k = f1; k < t1; k++) strvec_push(&s2.lines, c.lines[k]);
            vec_push(&splices, s1);
            vec_push(&splices, s2);
            magic_span_t m1 = {f1, t1}, m2 = {f2, t2};
            vec_push(&moved, m1);
            vec_push(&moved, m2);
            break;
        }
        case OP_INDENT:
        case OP_DEDENT: {
            json_t *amount = op_at(op, 3);
            bool ok = false;
            int f = 0, t = 0;
            if (len == 4) RANGE(op_at(op, 1), op_at(op, 2), ok, f, t);
            bool tab = json_is_str(amount) && !strcmp(amount->u.str.s, "tab");
            bool spaces = json_is_num(amount) && amount->u.num.d == floor(amount->u.num.d) && amount->u.num.d >= 1 &&
                          amount->u.num.d <= 64;
            if (!ok || (!tab && !spaces)) {
                MALFORMED();
                break;
            }
            if (HIDDEN(f, t)) {
                PROBLEM("range includes lines not shown.");
                break;
            }
            shift_t s = {f, t, NULL, name == OP_DEDENT, seq++};
            if (tab) {
                s.prefix = xstrdup("\t");
            } else {
                int k = (int)amount->u.num.d;
                s.prefix = xmalloc((size_t)k + 1);
                memset(s.prefix, ' ', (size_t)k);
                s.prefix[k] = 0;
            }
            vec_push(&shifts, s);
            break;
        }
        }
#undef HIDDEN
#undef RANGE
#undef MALFORMED
#undef PROBLEM
        free(written);
    }

    if (!problem) {
        for (size_t k = 0; k < sub_order.n; k++) {
            int at = sub_order.v[k];
            splice_t s = {at, at + 1, {0}, seq++};
            strvec_push(&s.lines, subs[at]);
            vec_push(&splices, s);
        }
        for (size_t k = 0; k < ins_order.n; k++) {
            int at = ins_order.v[k];
            splice_t s = {at, at, inserts[at], seq++};
            inserts[at] = (strvec_t){0};
            vec_push(&splices, s);
        }
        if (splices.n) qsort(splices.v, splices.n, sizeof(splice_t), splice_qcmp);
        for (size_t i = 1; i < splices.n && !problem; i++) {
            splice_t *prev = &splices.v[i - 1], *next = &splices.v[i];
            if (next->from < prev->to || (next->from == next->to && prev->from == next->from))
                problem = xstrdup("Two operations touch the same lines or insert at the same place. Give each line at "
                                  "most one operation, numbered against the original file.");
        }
    }
    if (!problem) {
        if (shifts.n) qsort(shifts.v, shifts.n, sizeof(shift_t), shift_qcmp);
        for (size_t i = 0; i < shifts.n && !problem; i++) {
            if (i > 0 && shifts.v[i].from < shifts.v[i - 1].to) problem = xstrdup("Indentation ranges overlap.");
            for (size_t k = 0; k < moved.n && !problem; k++)
                if (moved.v[k].from < shifts.v[i].to && moved.v[k].to > shifts.v[i].from)
                    problem = xstrdup("Indentation ranges must not overlap lines that move or swap.");
        }
    }
    if (!problem) {
        /* keep one neighbouring line at each end to anchor empty lines and insertions; fall back to the complete
         * snapshot if that span occurs more than once */
        int lo = splices.n ? splices.v[0].from : n, slo = shifts.n ? shifts.v[0].from : n;
        int hi = splices.n ? vec_last(&splices).to : 0, shi = shifts.n ? vec_last(&shifts).to : 0;
        int from = (lo < slo ? lo : slo) - 1;
        if (from < 0) from = 0;
        int to = (hi > shi ? hi : shi) + 1;
        if (to > n) to = n;
        bool final_nl = str_ends(content, "\n");
        sb_t old = SB_INIT;
        for (int i = from; i < to; i++) {
            if (i > from) sb_putc(&old, '\n');
            sb_append(&old, c.lines[i]);
        }
        if (to < n || final_nl) sb_putc(&old, '\n');
        const char *first = old.len ? strstr(content, old.s) : NULL;
        if (!old.len || !first || strstr(first + 1, old.s)) {
            from = 0;
            to = n;
            sb_clear(&old);
            sb_append(&old, content);
        }
        strvec_t rep = {0};
        int cursor = from;
        size_t si = 0;
        splice_t sentinel = {to, to, {0}, 0};
        for (size_t k = 0; k <= splices.n && !problem; k++) {
            const splice_t *sp = k < splices.n ? &splices.v[k] : &sentinel;
            while (cursor < sp->from) {
                const char *line = c.lines[cursor];
                while (si < shifts.n && shifts.v[si].to <= cursor) si++;
                const shift_t *sh = si < shifts.n ? &shifts.v[si] : NULL;
                if (!sh || sh->from > cursor || is_blank(line)) strvec_push(&rep, line);
                else if (!sh->dedent) strvec_push_own(&rep, xasprintf("%s%s", sh->prefix, line));
                else if (str_starts(line, sh->prefix)) strvec_push(&rep, line + strlen(sh->prefix));
                else {
                    problem = xasprintf("Line %d does not have the specified indentation to remove.", cursor + 1);
                    break;
                }
                cursor++;
            }
            if (problem) break;
            for (size_t j = 0; j < sp->lines.n; j++) strvec_push(&rep, sp->lines.v[j]);
            cursor = sp->to;
        }
        if (!problem) {
            sb_t next = SB_INIT;
            for (size_t j = 0; j < rep.n; j++) {
                if (j) sb_putc(&next, '\n');
                sb_append(&next, rep.v[j]);
            }
            if (rep.n && (to < n || final_nl)) sb_putc(&next, '\n');
            if (!strcmp(old.s ? old.s : "", sb_cstr(&next))) {
                problem = xstrdup("The operations produce no changes.");
                sb_free(&next);
            } else {
                out->old_s = sb_detach(&old);
                out->new_s = sb_detach(&next);
            }
        }
        strvec_free(&rep);
        sb_free(&old);
    }

    for (size_t i = 0; i < splices.n; i++) strvec_free(&splices.v[i].lines);
    vec_free(&splices);
    for (size_t i = 0; i < shifts.n; i++) free(shifts.v[i].prefix);
    vec_free(&shifts);
    vec_free(&moved);
    for (int i = 0; i < n; i++) free(subs[i]);
    free(subs);
    for (int i = 0; i <= n; i++) strvec_free(&inserts[i]);
    free(inserts);
    vec_free(&sub_order);
    vec_free(&ins_order);
    strvec_free(&lv);
    json_free(ops);
    if (problem) {
        free(out->ranges);
        out->ranges = NULL;
        out->nranges = 0;
        out->problem = problem;
    }
}

/* ======================================================================================
 * applyDescribedChange (edit/magic.ts)
 * ====================================================================================== */

#define MAX_ATTEMPTS 3
#define OUTPUT_TOKENS_PER_CHAR 0.5
#define OUTPUT_SLACK_TOKENS 256

/* normalizeToLF: "\r\n" -> "\n" */
static char *to_lf(const char *s) { return strchr(s, '\r') ? str_replace(s, "\r\n", "\n") : xstrdup(s); }

/* REFUSAL /^CANNOT:\s*(.*)/i: the reason (owned, up to the first line terminator), or NULL */
static char *refusal(const char *reply) {
    if (strncasecmp(reply, "CANNOT:", 7)) return NULL;
    const char *p = reply + 7;
    p += ws_run(p);
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r' && strncmp(e, "\xe2\x80\xa8", 3) && strncmp(e, "\xe2\x80\xa9", 3)) e++;
    return xstrndup(p, (size_t)(e - p));
}

static char *result_text(const pe_result_t *r) { return xstrdup(r->text ? r->text : ""); }

bool magic_apply_described(const char *path, const char *change, const char *content, const magic_views_t *views,
                           magic_io_t *io, pe_result_t *res, char **err) {
    memset(res, 0, sizeof(*res));
    size_t view = 0;
    char *previous = NULL, *rejection = xstrdup("");
    /* a change whose ranges were doubted once; repeating it confirms it */
    char *doubted_old = NULL, *doubted_new = NULL;
    bool ok = false, done = false;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS && !done; attempt++) {
        const magic_view_t *cur = &views->v[view];
        /* whatever goes wrong against an excerpt, the next attempt sees more of the file */
        bool wider = view < views->n - 1;
        if (wider) view++;
        json_t *ctx = json_obj();
        json_set_str(ctx, "path", path);
        json_set_str(ctx, "file", cur->text);
        json_set_bool(ctx, "partial", cur->partial);
        json_set_str(ctx, "change", change);
        if (previous) json_set_str(ctx, "previous", previous);
        json_set_str(ctx, "rejection", rejection);
        char *request = prompt_render("coding-agent/edit/magic-apply.md", ctx);
        json_free(ctx);
        int64_t max_tokens = (int64_t)ceil((double)(fz_utf16_len(cur->text) + fz_utf16_len(change)) * OUTPUT_TOKENS_PER_CHAR) +
                             OUTPUT_SLACK_TOKENS;
        bool truncated = false;
        char *cerr = NULL;
        char *reply = io->complete(io, request ? request : "", max_tokens, &truncated, &cerr);
        free(request);
        if (!reply) {
            *err = cerr ? cerr : xstrdup("The edit failed.");
            done = true;
            break;
        }
        char *trimmed = js_trim(reply);
        char *reason = refusal(trimmed);
        if (reason) {
            /* the model may have missed lines the excerpt left out: ask afresh */
            if (wider) {
                free(reason);
                free(trimmed);
                free(reply);
                continue;
            }
            char *why = js_trim(reason);
            if (!*why) {
                free(why);
                why = xstrdup("no reason given");
            }
            *err = cur->partial ? xasprintf("No change was made to %s: %s\nThe model saw only the lines your description "
                                            "names: quote the lines to change, or give their line numbers.",
                                            path, why)
                                : xasprintf("No change was made to %s: %s", path, why);
            free(why);
            free(reason);
            free(trimmed);
            free(reply);
            done = true;
            break;
        }
        char *lf = to_lf(trimmed);
        free(trimmed);
        magic_compiled_t compiled;
        magic_compile(lf, content, cur, truncated, &compiled);
        free(lf);
        bool edit = compiled.old_s != NULL;
        char *hints = edit ? magic_block_hints(content, compiled.ranges, compiled.nranges, cur) : xstrdup("");
        /* omp also refuses a change after which a parsing file no longer parses (native parseCodeStatus); omc has
         * no parser to ask, so a range that cuts a block reaches the model through the block hints below */
        bool same = edit && doubted_old && !strcmp(doubted_old, compiled.old_s) && !strcmp(doubted_new, compiled.new_s);
        if (*hints && !same) {
            /* a range that does not match the block it starts may be off by a line: the model confirms it */
            free(doubted_old);
            free(doubted_new);
            doubted_old = xstrdup(compiled.old_s);
            doubted_new = xstrdup(compiled.new_s);
            free(rejection);
            rejection = xasprintf("Nothing was applied: %s. Correct the range, or send the same operations again if that "
                                  "is intended.",
                                  hints);
        } else if (edit) {
            pe_result_t r;
            io->apply(io, compiled.old_s, compiled.new_s, true, &r);
            if (!r.is_error) {
                *res = r;
                ok = done = true;
            } else {
                free(rejection);
                rejection = result_text(&r);
                pe_result_free(&r);
            }
        } else if (truncated) {
            *err = xasprintf("The model ran out of output before finishing the change to %s; the file is unchanged. Split "
                             "the change into smaller calls.",
                             path);
            done = true;
        } else {
            free(rejection);
            rejection = xstrdup(compiled.problem);
        }
        free(hints);
        magic_compiled_free(&compiled);
        free(previous);
        previous = reply;
    }
    if (!done)
        *err = xasprintf("The change to %s could not be applied in %d attempts; the file is unchanged. Last rejection:\n%s\n"
                         "Describe the change more exactly: quote the lines it touches.",
                         path, MAX_ATTEMPTS, rejection);
    free(previous);
    free(rejection);
    free(doubted_old);
    free(doubted_new);
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

static char *mc_complete(magic_io_t *io, const char *request, int64_t max_tokens, bool *truncated, char **err) {
    magic_job_t *job = io->ud;
    magic_call_t *mc = job->mc;
    const ai_model_t *m = mc->model;
    int64_t ceiling = m->max_tokens > 0 && m->max_tokens < COMPLETION_MAX_TOKENS ? m->max_tokens : COMPLETION_MAX_TOKENS;
    json_t *sys = json_arr();
    json_push_str(sys, mc->system);
    json_t *msgs = json_arr();
    json_t *u = json_obj();
    json_set_str(u, "role", "user");
    json_set_str(u, "content", request);
    json_set_int(u, "timestamp", now_ms());
    json_push(msgs, u);
    ai_context_t ctx = {.system_prompt = sys, .messages = msgs};
    json_t *r = NULL;
    /* retryTransientCompletion: up to three attempts of a transient failure, 500 ms doubling (capped 8 s) */
    for (int attempt = 1;; attempt++) {
        if (mc->cancel) break;
        ai_stream_opts_t o;
        ai_stream_opts_init(&o);
        o.api_key = mc->api_key;
        o.session_id = mc->session_id;
        o.max_tokens = m->reasoning ? ceiling : (max_tokens < ceiling ? max_tokens : ceiling);
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
    json_free(msgs);
    const char *stop = json_get_str(r, "stopReason");
    char *text = NULL;
    if (!r || mc->cancel || str_eq(stop, "aborted")) {
        /* an abort resolves with empty content rather than rejecting */
        *err = mc->timed_out ? xasprintf("The edit timed out after %ds waiting on %s; the file is unchanged.",
                                         APPLY_TIMEOUT_MS / 1000, mc->name)
                             : xstrdup("The edit was aborted; the file is unchanged.");
    } else if (str_eq(stop, "error")) {
        const char *em = json_get_str(r, "errorMessage");
        *err = xasprintf("%s failed while writing the edit: %s", mc->name, em ? em : "unknown error");
    } else {
        sb_t b = SB_INIT;
        json_t *blk;
        json_arr_foreach(json_get_arr(r, "content"), i, blk) if (str_eq(json_get_str(blk, "type"), "text"))
            sb_append(&b, json_get_str(blk, "text") ? json_get_str(blk, "text") : "");
        text = sb_detach(&b);
        *truncated = str_eq(stop, "length");
    }
    json_free(r);
    return text;
}

static void mc_apply(magic_io_t *io, const char *old_s, const char *new_s, bool check_snapshot, pe_result_t *r) {
    magic_job_t *job = io->ud;
    magic_call_t *mc = job->mc;
    json_t *args = json_obj();
    json_set_str(args, "path", job->section->path);
    json_t *edits = json_arr(), *e = json_obj();
    json_set_str(e, "old_string", old_s);
    json_set_str(e, "new_string", new_s);
    json_push(edits, e);
    json_set(args, "edits", edits);
    pe_opts_t o = *mc->base;
    if (check_snapshot) {
        o.raw_snapshot = job->raw;
        o.raw_snapshot_len = job->raw_len;
    }
    /* completions overlap; writes go one at a time, as within any other edit call */
    pthread_mutex_lock(&mc->env->mu);
    pe_run(PE_REPLACE, args, &o, r);
    pthread_mutex_unlock(&mc->env->mu);
    json_free(args);
}

/* applySection: read the file, have the apply model write the change, apply it against the snapshot */
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
    if (!views.n) {
        job->err = xasprintf("%s is too large for %s to read whole, and the change names no part of it to show instead: "
                             "quote the lines to change, or give their line numbers.",
                             target, mc->name);
    } else {
        magic_io_t io = {mc_complete, mc_apply, job};
        job->ok = magic_apply_described(target, job->section->change, content, &views, &io, &job->res, &job->err);
    }
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
    mc.system = prompt_load_or("coding-agent/edit/magic-apply-system.md", "");
    /* one budget across every attempt: the calling agent's turn blocks on the edit */
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
