/* hashline.c - hashline edit language engine (port of omp pi-edit modes/hashline). See hashline.h. */
#include "hashline.h"

#include "core/fs.h"
#include "fuzzy.h"
#include "tools.h"

#include <ctype.h>
#include <errno.h>

/* ======================================================================================
 * small helpers
 * ====================================================================================== */

#define HL_PUT "PUT"
#define HL_CUT "CUT"
#define HL_REM "REM"
#define HL_MV "MV"
#define BEGIN_MARK "*** Begin Patch"
#define END_MARK "*** End Patch"
#define ABORT_MARK "*** Abort"
#define MAX_EXPANDED_RANGE_LINES 100000u
#define MISMATCH_CONTEXT 2u
#define SEEN_LINE_REVEAL_CAP 40
#define SEEN_LINE_REVEAL_MAX_COLUMNS 512
#define NOOP_HARD_LIMIT 3

/* JS JSON.stringify(str) */
static char *jq(const char *s) {
    sb_t b = SB_INIT;
    sb_putc(&b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_append(&b, "\\\""); break;
        case '\\': sb_append(&b, "\\\\"); break;
        case '\n': sb_append(&b, "\\n"); break;
        case '\r': sb_append(&b, "\\r"); break;
        case '\t': sb_append(&b, "\\t"); break;
        case 8: sb_append(&b, "\\b"); break;
        case 12: sb_append(&b, "\\f"); break;
        default:
            if (*p < 0x20) sb_appendf(&b, "\\u%04x", *p);
            else sb_putc(&b, (char)*p);
        }
    }
    sb_putc(&b, '"');
    return sb_detach(&b);
}

/* length of the Unicode whitespace code point at s, 0 when none */
static size_t ws_at(const char *s) {
    if (!*s) return 0;
    uint32_t cp;
    int k = utf8_decode(s, strlen(s) < 4 ? strlen(s) : 4, &cp);
    if (k <= 0) return 0;
    return (fz_is_js_ws(cp) || cp == 0x85) ? (size_t)k : 0;
}

static const char *skip_ws(const char *s) {
    size_t k;
    while ((k = ws_at(s)) > 0) s += k;
    return s;
}

static char *trim(const char *s) { return fz_trim(s); }
static char *trim_end(const char *s) { return fz_trim_end(s); }
static bool blank(const char *s) { return fz_blank(s); }

static bool starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static char *chars_prefix(const char *s, size_t n, bool *cut) {
    size_t i = 0, c = 0, len = strlen(s);
    while (i < len && c < n) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        i += k > 0 ? (size_t)k : 1;
        c++;
    }
    if (cut) *cut = i < len;
    return xstrndup(s, i);
}

/* split on '\n' keeping empty fields (Rust str::split('\n')) */
static strvec_t split_nl(const char *s) { return str_split(s, "\n"); }

/* ---- u32 -> u32 map (open addressing) ---- */
typedef struct {
    uint32_t *keys, *vals;
    bool *used;
    size_t cap, n;
} umap_t;

static void umap_free(umap_t *m) {
    free(m->keys);
    free(m->vals);
    free(m->used);
    memset(m, 0, sizeof(*m));
}

static size_t umap_slot(const umap_t *m, uint32_t k) {
    size_t i = (k * 2654435761u) & (m->cap - 1);
    while (m->used[i] && m->keys[i] != k) i = (i + 1) & (m->cap - 1);
    return i;
}

static void umap_put(umap_t *m, uint32_t k, uint32_t v);
static void umap_grow(umap_t *m) {
    umap_t o = *m;
    m->cap = o.cap ? o.cap * 2 : 64;
    m->keys = xcalloc(m->cap, sizeof(uint32_t));
    m->vals = xcalloc(m->cap, sizeof(uint32_t));
    m->used = xcalloc(m->cap, sizeof(bool));
    m->n = 0;
    for (size_t i = 0; i < o.cap; i++)
        if (o.used[i]) umap_put(m, o.keys[i], o.vals[i]);
    umap_free(&o);
}

static void umap_put(umap_t *m, uint32_t k, uint32_t v) {
    if ((m->n + 1) * 2 > m->cap) umap_grow(m);
    size_t i = umap_slot(m, k);
    if (!m->used[i]) {
        m->used[i] = true;
        m->keys[i] = k;
        m->n++;
    }
    m->vals[i] = v;
}

static bool umap_get(const umap_t *m, uint32_t k, uint32_t *v) {
    if (!m->cap) return false;
    size_t i = umap_slot(m, k);
    if (!m->used[i]) return false;
    if (v) *v = m->vals[i];
    return true;
}

/* sorted unique u32 set */
typedef VEC(uint32_t) u32v_t;
static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static void u32_sort_uniq(u32v_t *v) {
    if (!v->n) return;
    xqsort(v->v, v->n, sizeof(uint32_t), cmp_u32);
    size_t w = 1;
    for (size_t i = 1; i < v->n; i++)
        if (v->v[i] != v->v[w - 1]) v->v[w++] = v->v[i];
    v->n = w;
}
static bool u32_has(const u32v_t *v, uint32_t x) {
    size_t lo = 0, hi = v->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v->v[mid] == x) return true;
        if (v->v[mid] < x) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

/* ======================================================================================
 * read-output metadata rows (prefixes.rs)
 * ====================================================================================== */

bool hl_is_read_truncation_notice(const char *line) {
    char *t = trim(line);
    size_t n = strlen(t);
    bool r = false;
    if (n >= 2 && t[0] == '[' && t[n - 1] == ']') {
        t[n - 1] = 0;
        const char *body = t + 1;
        bool showing = starts(body, "Showing ") &&
                       (strstr(body, " line") || strstr(body, "lines ") || strstr(body, "bytes ")) &&
                       (strstr(body, " of ") || strstr(body, " elided"));
        bool more = false;
        {
            bool head = starts(body, "More lines in ");
            if (!head) {
                const char *m = strstr(body, " more line");
                if (m && m > body) {
                    head = true;
                    for (const char *p = body; p < m; p++)
                        if (!isdigit((unsigned char)*p)) head = false;
                }
            }
            size_t bl = strlen(body);
            more = head && strstr(body, " in ") && strstr(body, ". Use ") && bl >= 12 &&
                   !strcmp(body + bl - 12, " to continue");
        }
        bool elided = (starts(body, "\xE2\x80\xA6") || starts(body, "...")) && strstr(body, "ln elided;") &&
                      strstr(body, "re-read needed ranges");
        bool oversized = starts(body, "Line ") && strstr(body, " exceeds ") && strstr(body, " limit.");
        r = showing || more || elided || oversized;
    }
    free(t);
    return r;
}

static bool contains_ellipsis(const char *s) { return strstr(s, "\xE2\x80\xA6") || strstr(s, "..."); }

static bool is_read_metadata_line(const char *line) {
    if (hl_is_read_truncation_notice(line)) return true;
    /* ^\s*[1-9]\d*\s*-\s*[1-9]\d*:.*(?:…|\.\.\.).*$ */
    const char *p = skip_ws(line);
    if (*p >= '1' && *p <= '9') {
        while (isdigit((unsigned char)*p)) p++;
        p = skip_ws(p);
        if (*p == '-') {
            p = skip_ws(p + 1);
            if (*p >= '1' && *p <= '9') {
                while (isdigit((unsigned char)*p)) p++;
                if (*p == ':' && contains_ellipsis(p + 1) && !strchr(p + 1, '\n')) return true;
            }
        }
    }
    /* ^\s*(?:…|\.\.\.)\s*$ */
    p = skip_ws(line);
    if (starts(p, "\xE2\x80\xA6")) p += 3;
    else if (starts(p, "...")) p += 3;
    else return false;
    return *skip_ws(p) == 0;
}

/* HL_PREFIX_RE: ^\s*(?:(?:>>>|>>)\s*)?(?:[+*-]\s*)?(\d+)[:|] -> match length, 0 when none */
static size_t hl_prefix_len(const char *line) {
    const char *p = skip_ws(line);
    if (starts(p, ">>>")) p = skip_ws(p + 3);
    else if (starts(p, ">>")) p = skip_ws(p + 2);
    if (*p == '+' || *p == '*' || *p == '-') p = skip_ws(p + 1);
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) p++;
    if (*p != ':' && *p != '|') return 0;
    return (size_t)(p + 1 - line);
}

static char *strip_one_prefix(const char *line) {
    size_t k = hl_prefix_len(line);
    return xstrdup(line + k);
}

/* ======================================================================================
 * tokenizer (tokenizer.rs)
 * ====================================================================================== */

typedef enum { T_REPLACE, T_BLOCK, T_INS_BEFORE, T_INS_AFTER, T_INS_AFTER_BLOCK, T_CUT, T_CUT_BLOCK, T_BOF, T_EOF, T_REM, T_MOVE } tk_t;

typedef struct {
    tk_t k;
    uint32_t s, e; /* range / anchor in s */
    char *reg;
    char *dest;
} target_t;

static void target_free(target_t *t) {
    free(t->reg);
    free(t->dest);
    t->reg = t->dest = NULL;
}

typedef enum { TOK_BLANK, TOK_ENV_BEGIN, TOK_ENV_END, TOK_ABORT, TOK_HEADER, TOK_OP, TOK_PAYLOAD, TOK_RAW } tokk_t;

typedef struct {
    tokk_t k;
    uint32_t line_num;
    char *text;      /* payload / raw / header path */
    char *file_hash; /* header */
    target_t target;
    bool had_colon;
} token_t;

static void token_free(token_t *t) {
    free(t->text);
    free(t->file_hash);
    target_free(&t->target);
}

static bool parse_number_prefix(const char *raw, uint32_t *val, size_t *used) {
    if (!(raw[0] >= '1' && raw[0] <= '9')) return false;
    size_t end = 1;
    while (isdigit((unsigned char)raw[end])) end++;
    uint64_t v = 0;
    for (size_t i = 0; i < end; i++) {
        v = v * 10 + (uint64_t)(raw[i] - '0');
        if (v > UINT32_MAX) return false;
    }
    *val = (uint32_t)v;
    *used = end;
    return true;
}

static bool parse_range(const char *raw, bool allow_single, uint32_t *s, uint32_t *e, size_t *used, bool *sep) {
    const char *lead = skip_ws(raw);
    size_t leading = (size_t)(lead - raw);
    uint32_t start;
    size_t n;
    if (!parse_number_prefix(lead, &start, &n)) return false;
    size_t cur = leading + n;
    bool saw_non_ws = false;
    for (;;) {
        size_t k = ws_at(raw + cur);
        if (k) {
            cur += k;
            continue;
        }
        char c = raw[cur];
        if (c == '-' || c == '.' || c == '=') {
            saw_non_ws = true;
            cur++;
            continue;
        }
        if (starts(raw + cur, "\xE2\x80\xA6")) {
            saw_non_ws = true;
            cur += 3;
            continue;
        }
        break;
    }
    uint32_t end;
    if (parse_number_prefix(raw + cur, &end, &n)) {
        cur += n;
        while (raw[cur] && isspace((unsigned char)raw[cur])) cur++;
        *s = start;
        *e = end;
        *used = cur;
        *sep = true;
        return true;
    }
    if (!allow_single) return false;
    if (saw_non_ws && (raw[cur] == 0 || raw[cur] == ':' || raw[cur] == '@')) {
        *s = *e = start;
        *used = cur;
        *sep = true;
        return true;
    }
    *s = *e = start;
    *used = cur;
    *sep = false;
    return true;
}

static bool parse_register_and_colon(const char *raw, target_t *t, bool *had_colon) {
    const char *rest = skip_ws(raw);
    if (*rest == '@') {
        const char *r = rest + 1;
        size_t len = 0;
        while (isalnum((unsigned char)r[len]) || r[len] == '_' || r[len] == '-') len++;
        if (len == 0 || len > 64) return false;
        free(t->reg);
        t->reg = xstrndup(r, len);
        rest = skip_ws(r + len);
    }
    *had_colon = *rest == ':';
    if (*had_colon) rest = skip_ws(rest + 1);
    if (*rest) {
        free(t->reg);
        t->reg = NULL;
        return false;
    }
    return true;
}

static bool parse_put_target(const char *raw, target_t *t, bool *colon) {
    const char *rest = skip_ws(raw);
    memset(t, 0, sizeof(*t));
    if (*rest == '>') {
        const char *after = skip_ws(rest + 1);
        if (*after == '$') {
            t->k = T_EOF;
            return parse_register_and_colon(after + 1, t, colon);
        }
        uint32_t line;
        size_t used;
        if (!parse_number_prefix(after, &line, &used)) return false;
        const char *tail = after + used;
        bool block = *tail == '*';
        if (block) tail++;
        t->k = block ? T_INS_AFTER_BLOCK : T_INS_AFTER;
        t->s = t->e = line;
        return parse_register_and_colon(tail, t, colon);
    }
    if (*rest == '<') {
        const char *after = skip_ws(rest + 1);
        uint32_t line;
        size_t used;
        if (!parse_number_prefix(after, &line, &used)) return false;
        const char *tail = after + used;
        if (*tail == '*') tail++;
        if (line == 1) t->k = T_BOF;
        else {
            t->k = T_INS_BEFORE;
            t->s = t->e = line;
        }
        return parse_register_and_colon(tail, t, colon);
    }
    uint32_t s, e;
    size_t used;
    bool sep;
    if (!parse_range(rest, true, &s, &e, &used, &sep)) return false;
    const char *tail = rest + used;
    if (*tail == '*') {
        if (sep) return false;
        t->k = T_BLOCK;
        t->s = t->e = s;
        return parse_register_and_colon(tail + 1, t, colon);
    }
    t->k = T_REPLACE;
    t->s = s;
    t->e = e;
    return parse_register_and_colon(tail, t, colon);
}

static bool parse_cut_target(const char *raw, target_t *t, bool *colon) {
    const char *rest = skip_ws(raw);
    memset(t, 0, sizeof(*t));
    uint32_t s, e;
    size_t used;
    bool sep;
    if (!parse_range(rest, true, &s, &e, &used, &sep)) return false;
    const char *tail = rest + used;
    if (*tail == '*') {
        if (sep) return false;
        t->k = T_CUT_BLOCK;
        t->s = t->e = s;
        return parse_register_and_colon(tail + 1, t, colon);
    }
    t->k = T_CUT;
    t->s = s;
    t->e = e;
    return parse_register_and_colon(tail, t, colon);
}

static const char *keyword_tail(const char *line, const char *kw) {
    if (!starts(line, kw)) return NULL;
    const char *rest = line + strlen(kw);
    if (!*rest || *rest == ':' || ws_at(rest)) return rest;
    return NULL;
}

static char *unquote_path(const char *raw) {
    size_t n = strlen(raw);
    if (n >= 2 && ((raw[0] == '"' && raw[n - 1] == '"') || (raw[0] == '\'' && raw[n - 1] == '\'')))
        return xstrndup(raw + 1, n - 2);
    if (raw[0] == '"' || raw[0] == '\'') return NULL;
    return xstrdup(raw);
}

static bool parse_hunk_header(const char *line_in, target_t *t, bool *colon) {
    char *line = trim(line_in);
    bool ok = false;
    const char *rest;
    memset(t, 0, sizeof(*t));
    *colon = false;
    if ((rest = keyword_tail(line, HL_REM))) {
        ok = blank(rest);
        t->k = T_REM;
    } else if ((rest = keyword_tail(line, HL_MV))) {
        char *r = trim(rest);
        char *dest = unquote_path(r);
        free(r);
        if (dest && *dest) {
            t->k = T_MOVE;
            t->dest = dest;
            ok = true;
        } else {
            free(dest);
        }
    } else if ((rest = keyword_tail(line, HL_PUT))) {
        ok = parse_put_target(rest, t, colon);
    } else if ((rest = keyword_tail(line, HL_CUT))) {
        ok = parse_cut_target(rest, t, colon);
    }
    if (!ok) target_free(t);
    free(line);
    return ok;
}

static bool header_path_has_orphan_bracket(const char *path) {
    int depth = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') {
            if (!depth) return true;
            depth--;
        }
    }
    return false;
}

/* [PATH] / [PATH#TAG] */
static bool parse_header(const char *line_in, char **path, char **hash) {
    char *line = trim_end(line_in);
    size_t n = strlen(line);
    bool ok = false;
    *path = *hash = NULL;
    if (n >= 2 && line[0] == '[' && line[n - 1] == ']') {
        char *body = xstrndup(line + 1, n - 2);
        if (*body) {
            char *h = strrchr(body, '#');
            if (h) {
                *h = 0;
                const char *tag = h + 1;
                bool hex = strlen(tag) == 4;
                for (int i = 0; hex && i < 4; i++) hex = isxdigit((unsigned char)tag[i]) != 0;
                if (*body && !header_path_has_orphan_bracket(body) && hex) {
                    *path = xstrdup(body);
                    *hash = xstrdup(tag);
                    for (char *c = *hash; *c; c++) *c = (char)toupper((unsigned char)*c);
                    ok = true;
                }
            } else if (!header_path_has_orphan_bracket(body)) {
                *path = xstrdup(body);
                ok = true;
            }
        }
        free(body);
    }
    free(line);
    return ok;
}

static bool marker_eq(const char *line, const char *marker) {
    char *t = trim_end(line);
    bool r = !strcmp(t, marker);
    free(t);
    return r;
}

static token_t classify_line(const char *line, uint32_t line_num) {
    token_t t;
    memset(&t, 0, sizeof(t));
    t.line_num = line_num;
    if (!*line) {
        t.k = TOK_BLANK;
        return t;
    }
    if (marker_eq(line, BEGIN_MARK)) {
        t.k = TOK_ENV_BEGIN;
        return t;
    }
    if (marker_eq(line, END_MARK)) {
        t.k = TOK_ENV_END;
        return t;
    }
    if (marker_eq(line, ABORT_MARK)) {
        t.k = TOK_ABORT;
        return t;
    }
    if (parse_header(line, &t.text, &t.file_hash)) {
        t.k = TOK_HEADER;
        return t;
    }
    if (parse_hunk_header(line, &t.target, &t.had_colon)) {
        t.k = TOK_OP;
        return t;
    }
    if (line[0] == '+') {
        t.k = TOK_PAYLOAD;
        t.text = xstrdup(line + 1);
        return t;
    }
    t.k = TOK_RAW;
    t.text = xstrdup(line);
    return t;
}

static bool is_hunk_header_text(const char *text) {
    const char *lead = skip_ws(text);
    if (!starts(lead, HL_PUT) && !starts(lead, HL_CUT) && !starts(lead, HL_REM) && !starts(lead, HL_MV)) return false;
    target_t t;
    bool c;
    if (!parse_hunk_header(text, &t, &c)) return false;
    target_free(&t);
    return true;
}

/* ======================================================================================
 * edits (types.rs)
 * ====================================================================================== */

typedef enum { E_INSERT, E_DELETE, E_CUT, E_PASTE, E_BLOCK } ek_t;
typedef enum { C_BOF, C_EOF, C_BEFORE, C_AFTER } ck_t;
typedef enum { BM_NONE, BM_INSERT_AFTER, BM_CUT, BM_PASTE_AFTER } bm_t;

typedef struct {
    ek_t k;
    ck_t cur;        /* insert / gap paste cursor */
    uint32_t anchor; /* cursor anchor, delete anchor, block anchor */
    bool span;       /* paste over a range */
    uint32_t s, e;   /* cut / span-paste range */
    char *text;      /* insert */
    bool replacement;
    char *reg;
    strvec_t payloads; /* block */
    bm_t mode;
    uint32_t line_num, index;
} hedit_t;
typedef VEC(hedit_t) hedits_t;

static hedit_t hedit_clone(const hedit_t *e) {
    hedit_t c = *e;
    c.text = e->text ? xstrdup(e->text) : NULL;
    c.reg = e->reg ? xstrdup(e->reg) : NULL;
    memset(&c.payloads, 0, sizeof(c.payloads));
    for (size_t i = 0; i < e->payloads.n; i++) strvec_push(&c.payloads, e->payloads.v[i]);
    return c;
}

static void hedit_free(hedit_t *e) {
    free(e->text);
    free(e->reg);
    strvec_free(&e->payloads);
}

static void hedits_free(hedits_t *v) {
    for (size_t i = 0; i < v->n; i++) hedit_free(&v->v[i]);
    vec_free(v);
}

/* ======================================================================================
 * messages.rs
 * ====================================================================================== */

#define REPLACE_PAIR_COALESCED_WARNING                                                                                 \
    "Multiple hunks targeted the same exact range; kept only the last. Issue one `PUT` or `CUT` hunk per range."
#define REPLACEMENT_INDENT_AUTO_SHIFT_WARNING                                                                          \
    "Auto-indented a replacement body to match unchanged structural rows in its source range."
#define BARE_BODY_AUTO_PIPED_WARNING "Auto-prefixed bare body row(s) with `+`. Body rows must be `+TEXT` literal lines."
#define SNAPSHOT_ROWS_AUTO_PUT_WARNING                                                                                 \
    "Recovered top-level `N:TEXT` snapshot row(s) as single-line `PUT N.=N:` replacements. Use explicit `PUT` "       \
    "headers for reliable edits."
#define BARE_RANGE_AUTO_PUT_WARNING "Recovered a bare `N.=M:` header as `PUT N.=M:`. Prefix replacement ranges with `PUT`."
#define READ_METADATA_IGNORED_WARNING                                                                                  \
    "Ignored copied read-output elision row(s). Re-read elided ranges before editing them."
#define EMPTY_PUT_AUTO_CUT_WARNING                                                                                     \
    "Interpreted an empty `PUT` body as deletion. Use `CUT N.=M` or `CUT N*` for bodyless deletes."
#define CUT_COLON_IGNORED_WARNING                                                                                      \
    "Ignored a trailing `:` on bodyless `CUT`. Prefer `CUT N.=M` / `CUT N*` without a colon."
#define MINUS_BULLET_AUTO_PIPED_WARNING                                                                                \
    "Auto-prefixed bare `- ` bullet row(s) as literal content. `-` rows never remove lines — the range does that; "  \
    "always prefix literal body rows with `+`: `+- item`."
#define DIFF_OLD_ROWS_IGNORED_WARNING                                                                                  \
    "Ignored unified-diff `-old` row(s); the range already removes old content, so only `+new` rows were kept."
#define MINUS_ROW_REJECTED                                                                                             \
    "`-` rows are not valid; the range already names the lines being changed. For Markdown bullets or other "        \
    "literal `-` lines, prefix the literal row with `+`: `+- item`."
#define REM_TAKES_NO_BODY                                                                                              \
    "`REM` deletes the whole file and takes no body rows or line ops. Issue it alone under the header."
#define MOVE_TAKES_NO_BODY                                                                                             \
    "`MV DEST` does not take body rows. Put line edits above the `MV` row; the destination path follows `MV` on the " \
    "same line."
#define CUT_TAKES_NO_BODY                                                                                              \
    "`CUT` deletes (and captures) the named lines and takes no body rows. To write new content, use `PUT N.=M:` "    \
    "with `+TEXT` rows."
#define COLON_ON_REGISTER_PUT                                                                                          \
    "`PUT … @name` pastes the register and never takes `:` — the colon promises body rows. Drop the colon (`PUT "    \
    ">40 @name`), or drop `@name` and write `+TEXT` body rows."
#define REGISTER_PUT_TAKES_NO_BODY                                                                                     \
    "A register `PUT` pastes captured lines and takes no `+` body rows. To write literal text, drop the `@name` and " \
    "use `PUT …:` with body rows."
#define COLONLESS_PUT_TAKES_NO_BODY                                                                                    \
    "`PUT` without `:` is clipboard-backed and takes no body rows. Add `:` after the locator to write literal "      \
    "content (`PUT >40:` then `+TEXT` rows)."
#define COLONLESS_SPAN_PUT                                                                                             \
    "Colonless `PUT` is clipboard-backed, and span targets need a named register (`PUT 5.=9 @name`); the anonymous " \
    "register pastes only at gaps (`PUT >40`). To write literal content, add `:` and `+TEXT` body rows."
#define EMPTY_PASTE                                                                                                    \
    "Nothing to paste: no unlabeled `CUT` precedes this `PUT` in this call, and the anonymous register never "       \
    "carries across calls. Put `CUT N.=M` / `CUT N*` above it, or use named registers (`CUT … @name` → `PUT … "      \
    "@name`) for cross-call moves."
#define CLIPBOARD_INTERLEAVED_SECTIONS                                                                                 \
    "`CUT`/register-`PUT` ops cannot be used in a file whose sections are interleaved with another file's: "         \
    "same-path sections merge into the first occurrence, which would reorder the register sequence. Keep each "      \
    "file's ops under ONE `[path#TAG]` header."
#define EMPTY_INSERT                                                                                                   \
    "`PUT <N:` / `PUT >N:` promises body rows and got none. Write `+TEXT` rows, or drop the `:` to paste a register " \
    "(`PUT >N` = anonymous, `PUT >N @name` = named)."
#define HEADTAIL_DRIFT_WARNING                                                                                         \
    "Applied the `PUT <1:`/`PUT >$:` edit despite a stale snapshot tag (file changed since your read) — head/tail "   \
    "position is content-independent. Re-read if the drift was unexpected."

typedef enum { RO_REPLACE, RO_CUT } rop_t;

static char *reg_suffix(const char *reg) { return reg ? xasprintf(" @%s", reg) : xstrdup(""); }

static char *range_op_single(rop_t op, uint32_t line, const char *reg) {
    char *sfx = reg_suffix(reg);
    char *r = op == RO_REPLACE ? (reg ? xasprintf("PUT %u%s", line, sfx) : xasprintf("PUT %u:", line))
                               : xasprintf("CUT %u%s", line, sfx);
    free(sfx);
    return r;
}

static char *range_op_range(rop_t op, uint32_t s, uint32_t e, const char *reg) {
    char *sfx = reg_suffix(reg);
    char *r = op == RO_REPLACE ? (reg ? xasprintf("PUT %u.=%u%s", s, e, sfx) : xasprintf("PUT %u.=%u:", s, e))
                               : xasprintf("CUT %u.=%u%s", s, e, sfx);
    free(sfx);
    return r;
}

static char *block_form_at(rop_t op, uint32_t line, const char *reg) {
    char *sfx = reg_suffix(reg);
    char *r = op == RO_REPLACE ? (reg ? xasprintf("PUT %u*%s", line, sfx) : xasprintf("PUT %u*:", line))
                               : xasprintf("CUT %u*%s", line, sfx);
    free(sfx);
    return r;
}

static char *invalid_absolute_range_message(uint32_t patch_line, uint32_t s, uint32_t e, rop_t op, bool has_block,
                                            uint32_t bs, uint32_t be, const char *reg) {
    char *single = range_op_single(op, s, reg);
    sb_t b = SB_INIT;
    sb_appendf(&b,
               "line %u: Invalid absolute range: start %u, end %u. The value after `.=` is an absolute source line, "
               "not a line count or replacement length. For one line use `%s`.",
               patch_line, s, e, single);
    uint64_t counted = (uint64_t)s + e;
    if (counted >= 1 && counted - 1 <= UINT32_MAX && counted - 1 >= s) {
        char *c = range_op_range(op, s, (uint32_t)(counted - 1), reg);
        sb_appendf(&b, " For %u lines starting at %u, use `%s`.", e, s, c);
        free(c);
    }
    if (has_block && bs == s && be > s) {
        char *bf = block_form_at(op, s, reg);
        sb_appendf(&b, " The syntactic block beginning at %u ends at %u, so `%s` is also valid.", s, be, bf);
        free(bf);
    }
    free(single);
    return sb_detach(&b);
}

static char *repeated_snapshot_row_message(uint32_t line) {
    return xasprintf("two or more pasted `%u:TEXT` read-output rows name line %u. Such rows are recovered as "
                     "single-line `PUT %u.=%u:` replacements, so repeating a number would keep only the last row and "
                     "drop the rest. Write the hunk explicitly: one `PUT %u.=M:` header covering exactly the lines that "
                     "change, followed by `+TEXT` body rows holding their complete final content.",
                     line, line, line, line, line);
}

static char *literal_op_row_warning(uint32_t line, const char *text) {
    return xasprintf("line %u: body row `+%s` is itself a valid hunk header, so it was inserted into the file as "
                     "literal text rather than executed. Ops are never `+`-prefixed — drop the `+` to run it, and "
                     "re-issue if this line landed in the file by mistake.",
                     line, text);
}

/* anchored context rows: each in-range anchor ±2, `*` on anchors, `...` between runs */
static void format_anchored_context(const u32v_t *anchors, char *const *lines, size_t nlines, strvec_t *out) {
    u32v_t disp = {0};
    for (size_t i = 0; i < anchors->n; i++) {
        uint32_t l = anchors->v[i];
        if (l < 1 || l > nlines) continue;
        uint32_t lo = l > MISMATCH_CONTEXT ? l - MISMATCH_CONTEXT : 1;
        uint32_t hi = l + MISMATCH_CONTEXT < nlines ? l + MISMATCH_CONTEXT : (uint32_t)nlines;
        for (uint32_t k = lo; k <= hi; k++) vec_push(&disp, k);
    }
    u32_sort_uniq(&disp);
    u32v_t aset = {0};
    for (size_t i = 0; i < anchors->n; i++) vec_push(&aset, anchors->v[i]);
    u32_sort_uniq(&aset);
    int64_t prev = -1;
    for (size_t i = 0; i < disp.n; i++) {
        uint32_t l = disp.v[i];
        if (prev >= 0 && l > prev + 1) strvec_push(out, "...");
        prev = l;
        strvec_push_own(out, xasprintf("%s%u:%s", u32_has(&aset, l) ? "*" : " ", l, l - 1 < nlines ? lines[l - 1] : ""));
    }
    vec_free(&disp);
    vec_free(&aset);
}

static char *format_line_ranges(const u32v_t *lines_in) {
    u32v_t v = {0};
    for (size_t i = 0; i < lines_in->n; i++) vec_push(&v, lines_in->v[i]);
    u32_sort_uniq(&v);
    sb_t b = SB_INIT;
    size_t i = 0;
    while (i < v.n) {
        size_t j = i;
        while (j + 1 < v.n && v.v[j + 1] == v.v[j] + 1) j++;
        if (b.len) sb_append(&b, ", ");
        if (i == j) sb_appendf(&b, "%u", v.v[i]);
        else sb_appendf(&b, "%u-%u", v.v[i], v.v[j]);
        i = j + 1;
    }
    vec_free(&v);
    return sb_detach(&b);
}

static char *selector_of(const char *ranges) {
    sb_t b = SB_INIT;
    for (const char *p = ranges; *p; p++) {
        if (p[0] == ',' && p[1] == ' ') {
            sb_putc(&b, ',');
            p++;
        } else {
            sb_putc(&b, *p);
        }
    }
    return sb_detach(&b);
}

/* ======================================================================================
 * parser executor (parser.rs)
 * ====================================================================================== */

typedef struct {
    char *text;
    uint32_t line_num;
    bool bare, minus;
} prow_t;
typedef VEC(prow_t) prows_t;

static void prows_free(prows_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].text);
    vec_free(r);
}

typedef struct {
    bool active;
    target_t target;
    uint32_t line_num;
    prows_t payloads;
    bool had_colon;
    prows_t deferred;
} pending_t;

typedef struct {
    int kind; /* 0 none, 1 rem, 2 move */
    char *dest;
} fileop_t;

typedef struct {
    hedits_t edits;
    strvec_t warnings;
    uint32_t edit_index;
    pending_t pending;
    fileop_t file_op;
    bool terminated;
    VEC(prow_t) comments; /* skippable `#` rows */
    u32v_t recovered_lines;
    /* failure */
    char *err;
    bool range_err;
    uint32_t r_line, r_s, r_e;
    rop_t r_op;
    char *r_reg;
} exec_t;

static void pending_clear(pending_t *p) {
    target_free(&p->target);
    prows_free(&p->payloads);
    prows_free(&p->deferred);
    memset(p, 0, sizeof(*p));
}

static void exec_free(exec_t *x) {
    hedits_free(&x->edits);
    strvec_free(&x->warnings);
    pending_clear(&x->pending);
    free(x->file_op.dest);
    for (size_t i = 0; i < x->comments.n; i++) free(x->comments.v[i].text);
    vec_free(&x->comments);
    vec_free(&x->recovered_lines);
    free(x->err);
    free(x->r_reg);
}

static bool fail_at(exec_t *x, uint32_t line, const char *msg) {
    free(x->err);
    x->err = xasprintf("line %u: %s", line, msg);
    return false;
}

static bool fail_at_own(exec_t *x, uint32_t line, char *msg) {
    bool r = fail_at(x, line, msg);
    free(msg);
    return r;
}

static void warn_once(exec_t *x, const char *w) {
    for (size_t i = 0; i < x->warnings.n; i++)
        if (!strcmp(x->warnings.v[i], w)) return;
    strvec_push(&x->warnings, w);
}

static uint32_t next_index(exec_t *x) { return x->edit_index++; }

static void push_insert(exec_t *x, ck_t cur, uint32_t anchor, const char *text, uint32_t line_num, bool replacement) {
    hedit_t e;
    memset(&e, 0, sizeof(e));
    e.k = E_INSERT;
    e.cur = cur;
    e.anchor = anchor;
    e.text = xstrdup(text);
    e.replacement = replacement;
    e.line_num = line_num;
    e.index = next_index(x);
    vec_push(&x->edits, e);
}

static void push_delete(exec_t *x, uint32_t anchor, uint32_t line_num) {
    hedit_t e;
    memset(&e, 0, sizeof(e));
    e.k = E_DELETE;
    e.anchor = anchor;
    e.line_num = line_num;
    e.index = next_index(x);
    vec_push(&x->edits, e);
}

static void push_delete_range(exec_t *x, uint32_t s, uint32_t e, uint32_t line_num) {
    for (uint32_t l = s; l <= e && l >= s; l++) push_delete(x, l, line_num);
}

static void push_cut(exec_t *x, uint32_t s, uint32_t e, const char *reg, uint32_t line_num) {
    hedit_t c;
    memset(&c, 0, sizeof(c));
    c.k = E_CUT;
    c.s = s;
    c.e = e;
    c.reg = reg ? xstrdup(reg) : NULL;
    c.line_num = line_num;
    c.index = next_index(x);
    vec_push(&x->edits, c);
    push_delete_range(x, s, e, line_num);
}

static void push_paste(exec_t *x, bool span, ck_t cur, uint32_t anchor, uint32_t s, uint32_t e, const char *reg,
                       uint32_t line_num) {
    hedit_t p;
    memset(&p, 0, sizeof(p));
    p.k = E_PASTE;
    p.span = span;
    p.cur = cur;
    p.anchor = anchor;
    p.s = s;
    p.e = e;
    p.reg = reg ? xstrdup(reg) : NULL;
    p.line_num = line_num;
    p.index = next_index(x);
    vec_push(&x->edits, p);
}

static void push_block(exec_t *x, uint32_t anchor, prows_t *payloads, bm_t mode, const char *reg, uint32_t line_num) {
    hedit_t b;
    memset(&b, 0, sizeof(b));
    b.k = E_BLOCK;
    b.anchor = anchor;
    if (payloads)
        for (size_t i = 0; i < payloads->n; i++) strvec_push(&b.payloads, payloads->v[i].text);
    b.mode = mode;
    b.reg = reg ? xstrdup(reg) : NULL;
    b.line_num = line_num;
    b.index = next_index(x);
    vec_push(&x->edits, b);
}

static const char *bodyless_message(const target_t *t, bool had_colon) {
    if (t->k == T_CUT || t->k == T_CUT_BLOCK) return CUT_TAKES_NO_BODY;
    if (t->k == T_REM || t->k == T_MOVE) return NULL;
    if (t->reg) return REGISTER_PUT_TAKES_NO_BODY;
    if (!had_colon) return COLONLESS_PUT_TAKES_NO_BODY;
    return NULL;
}

static bool markdown_bullet(const char *text) {
    const char *t = fz_trim_start(text);
    if (!starts(t, "- ")) return false;
    return t[2] && !ws_at(t + 2);
}

static bool literal_value(const char *text) {
    char *t = trim(text);
    size_t n = strlen(t);
    while (n && t[n - 1] == ',') t[--n] = 0;
    char *v = trim(t);
    free(t);
    n = strlen(v);
    bool r = false;
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) r = true;
    else if (n) {
        /* Rust f64::from_str: decimal / exponent forms, inf, nan */
        char *end;
        errno = 0;
        (void)strtod(v, &end);
        bool hexish = strchr(v, 'x') || strchr(v, 'X');
        r = *end == 0 && !hexish && !isspace((unsigned char)v[0]);
    }
    free(v);
    return r;
}

static void strip_uniform_bare_prefixes(prows_t *rows) {
    bool saw = false, all_literal = true;
    for (size_t i = 0; i < rows->n; i++) {
        prow_t *r = &rows->v[i];
        if (!r->bare || blank(r->text)) continue;
        saw = true;
        size_t k = hl_prefix_len(r->text);
        if (!k) return;
        all_literal = all_literal && literal_value(r->text + k);
    }
    if (!saw || all_literal) return;
    for (size_t i = 0; i < rows->n; i++) {
        prow_t *r = &rows->v[i];
        if (!r->bare || blank(r->text)) continue;
        char *s = strip_one_prefix(r->text);
        free(r->text);
        r->text = s;
    }
}

static bool resolve_minus_rows(exec_t *x, prows_t *rows) {
    size_t nminus = 0, first_minus = 0;
    bool all_bullets = true;
    for (size_t i = 0; i < rows->n; i++) {
        if (!rows->v[i].minus) continue;
        if (!nminus) first_minus = i;
        nminus++;
        if (!markdown_bullet(rows->v[i].text)) all_bullets = false;
    }
    if (!nminus) return true;
    size_t nexplicit = 0;
    bool explicit_bullet = false;
    for (size_t i = 0; i < rows->n; i++)
        if (!rows->v[i].bare) {
            nexplicit++;
            if (markdown_bullet(rows->v[i].text)) explicit_bullet = true;
        }
    if (all_bullets && (nexplicit == 0 || explicit_bullet)) {
        warn_once(x, MINUS_BULLET_AUTO_PIPED_WARNING);
        return true;
    }
    if (nexplicit && !all_bullets) {
        size_t w = 0;
        for (size_t i = 0; i < rows->n; i++) {
            if (rows->v[i].minus) free(rows->v[i].text);
            else rows->v[w++] = rows->v[i];
        }
        rows->n = w;
        warn_once(x, DIFF_OLD_ROWS_IGNORED_WARNING);
        return true;
    }
    return fail_at(x, rows->v[first_minus].line_num, MINUS_ROW_REJECTED);
}

static bool flush_pending(exec_t *x) {
    if (!x->pending.active) return true;
    pending_t p = x->pending;
    memset(&x->pending, 0, sizeof(x->pending));
    bool ok = resolve_minus_rows(x, &p.payloads);
    if (ok) {
        strip_uniform_bare_prefixes(&p.payloads);
        uint32_t line = p.line_num;
        target_t *t = &p.target;
        switch (t->k) {
        case T_REM:
        case T_MOVE: break;
        case T_CUT: push_cut(x, t->s, t->e, t->reg, line); break;
        case T_CUT_BLOCK: push_block(x, t->s, NULL, BM_CUT, t->reg, line); break;
        case T_REPLACE:
            if (t->reg) push_paste(x, true, C_BEFORE, 0, t->s, t->e, t->reg, line);
            else if (!p.payloads.n) {
                if (!p.had_colon) {
                    ok = fail_at(x, line, COLONLESS_SPAN_PUT);
                    break;
                }
                push_delete_range(x, t->s, t->e, line);
                warn_once(x, EMPTY_PUT_AUTO_CUT_WARNING);
            } else {
                for (size_t i = 0; i < p.payloads.n; i++) push_insert(x, C_BEFORE, t->s, p.payloads.v[i].text, line, true);
                push_delete_range(x, t->s, t->e, line);
            }
            break;
        case T_BLOCK:
            if (t->reg) push_block(x, t->s, NULL, BM_NONE, t->reg, line);
            else if (!p.payloads.n) {
                if (!p.had_colon) {
                    ok = fail_at(x, line, COLONLESS_SPAN_PUT);
                    break;
                }
                push_block(x, t->s, NULL, BM_NONE, NULL, line);
                warn_once(x, EMPTY_PUT_AUTO_CUT_WARNING);
            } else {
                push_block(x, t->s, &p.payloads, BM_NONE, NULL, line);
            }
            break;
        case T_INS_AFTER_BLOCK:
            if (t->reg || (!p.had_colon && !p.payloads.n)) push_block(x, t->s, NULL, BM_PASTE_AFTER, t->reg, line);
            else if (!p.payloads.n) ok = fail_at(x, line, EMPTY_INSERT);
            else push_block(x, t->s, &p.payloads, BM_INSERT_AFTER, NULL, line);
            break;
        default: {
            ck_t cur = t->k == T_INS_BEFORE ? C_BEFORE : t->k == T_INS_AFTER ? C_AFTER : t->k == T_BOF ? C_BOF : C_EOF;
            uint32_t anchor = (t->k == T_INS_BEFORE || t->k == T_INS_AFTER) ? t->s : 0;
            if (t->reg || (!p.had_colon && !p.payloads.n)) push_paste(x, false, cur, anchor, 0, 0, t->reg, line);
            else if (!p.payloads.n) ok = fail_at(x, line, EMPTY_INSERT);
            else
                for (size_t i = 0; i < p.payloads.n; i++) push_insert(x, cur, anchor, p.payloads.v[i].text, line, false);
        }
        }
    }
    pending_clear(&p);
    return ok;
}

static bool validate_range(exec_t *x, const target_t *t, uint32_t line_num, rop_t op) {
    if (t->e < t->s) {
        x->range_err = true;
        x->r_line = line_num;
        x->r_s = t->s;
        x->r_e = t->e;
        x->r_op = op;
        free(x->r_reg);
        x->r_reg = t->reg ? xstrdup(t->reg) : NULL;
        free(x->err);
        x->err = invalid_absolute_range_message(line_num, t->s, t->e, op, false, 0, 0, t->reg);
        return false;
    }
    uint64_t span = (uint64_t)t->e - t->s + 1;
    if (span > MAX_EXPANDED_RANGE_LINES)
        return fail_at_own(x, line_num,
                           xasprintf("%s range spans %llu lines; the maximum is %u. Split it into smaller hunks.",
                                     op == RO_REPLACE ? "replace" : "cut", (unsigned long long)span,
                                     MAX_EXPANDED_RANGE_LINES));
    return true;
}

static bool set_file_op(exec_t *x, int kind, char *dest, uint32_t line_num) {
    if (x->file_op.kind) {
        free(dest);
        return fail_at(x, line_num, "only one file-level op (`REM` or `MV`) per section. Merge them under one header.");
    }
    if (kind == 1 && x->edits.n) {
        free(dest);
        return fail_at(x, line_num, REM_TAKES_NO_BODY);
    }
    x->file_op.kind = kind;
    x->file_op.dest = dest;
    return true;
}

static void handle_blank(exec_t *x, const char *text, uint32_t line_num) {
    pending_t *p = &x->pending;
    if (!p->active) return;
    if (bodyless_message(&p->target, p->had_colon) || !p->payloads.n) return;
    prow_t r = {xstrdup(text), line_num, true, false};
    vec_push(&p->deferred, r);
}

static void take_deferred(pending_t *p) {
    for (size_t i = 0; i < p->deferred.n; i++) vec_push(&p->payloads, p->deferred.v[i]);
    p->deferred.n = 0;
}

static bool handle_literal(exec_t *x, const char *text, uint32_t line_num) {
    pending_t *p = &x->pending;
    if (!p->active) {
        if (x->file_op.kind) return fail_at(x, line_num, MOVE_TAKES_NO_BODY);
        char *plus = xasprintf("+%s", text);
        char *q = jq(plus);
        free(plus);
        bool r = fail_at_own(x, line_num, xasprintf("payload line has no preceding hunk header. Got %s.", q));
        free(q);
        return r;
    }
    const char *m = bodyless_message(&p->target, p->had_colon);
    if (m) return fail_at(x, line_num, m);
    take_deferred(p);
    if (is_hunk_header_text(text)) strvec_push_own(&x->warnings, literal_op_row_warning(line_num, text));
    prow_t r = {xstrdup(text), line_num, false, false};
    vec_push(&p->payloads, r);
    return true;
}

static char *preview48(const char *trimmed) {
    bool cut;
    char *p = chars_prefix(trimmed, 48, &cut);
    if (!cut) return p;
    char *r = xasprintf("%s\xE2\x80\xA6", p);
    free(p);
    return r;
}

static bool unified_hunk_header(const char *s) {
    /* ^@@\s+[-+]?\d+,\d+\s+[-+]?\d+,\d+\s+@@ */
    if (!starts(s, "@@")) return false;
    const char *p = s + 2;
    for (int part = 0; part < 2; part++) {
        if (!isspace((unsigned char)*p)) return false;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '-' || *p == '+') p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
        if (*p++ != ',') return false;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (!isspace((unsigned char)*p)) return false;
    while (isspace((unsigned char)*p)) p++;
    return starts(p, "@@");
}

static char *contamination_message(const char *text) {
    const char *t = fz_trim_start(text);
    static const char *const P[] = {"*** Update File:", "*** Add File:", "*** Delete File:", "*** Move to:"};
    for (size_t i = 0; i < 4; i++)
        if (starts(t, P[i])) {
            char *pv = preview48(t);
            char *q = jq(pv);
            char *m = xasprintf("apply_patch sentinel %s is not valid in hashline. File sections start with `[path#HASH]` "
                                "(no `Update File:` / `Add File:` keyword). Use `PUT N.=M:`, `CUT N.=M`, or `PUT "
                                "<N:`/`PUT >N:` ops.",
                                q);
            free(pv);
            free(q);
            return m;
        }
    if (starts(t, "@@")) {
        if (unified_hunk_header(t))
            return xstrdup("unified-diff hunk header (`@@ -N,M +N,M @@`) is not valid in hashline. Use `PUT N.=M:`, "
                           "`CUT N.=M`, or `PUT <N:`/`PUT >N:` ops.");
        char *pv = preview48(t);
        char *q = jq(pv);
        char *m = xasprintf("`@@`-bracketed hunk header %s is not valid in hashline. Drop the `@@ ... @@` brackets and "
                            "write a header such as `PUT N.=M:`.",
                            q);
        free(pv);
        free(q);
        return m;
    }
    if (*t) {
        bool digits_ws = true;
        size_t words = 0;
        bool in = false;
        for (const char *p = t; *p; p++) {
            if (!isdigit((unsigned char)*p) && !isspace((unsigned char)*p)) digits_ws = false;
            if (isspace((unsigned char)*p)) in = false;
            else if (!in) {
                in = true;
                words++;
            }
        }
        if (digits_ws && words == 1) {
            char *n = trim(t);
            char *m = xasprintf("hunk headers need a verb and both endpoints. Use `PUT %s.=%s:` to replace, or `CUT "
                                "%s.=%s` to delete.",
                                n, n, n, n);
            free(n);
            return m;
        }
    }
    /* pieces of trim_end_matches(':').split_whitespace() == 2 numbers */
    {
        char *c = xstrdup(t);
        size_t n = strlen(c);
        while (n && c[n - 1] == ':') c[--n] = 0;
        strvec_t pieces = {0};
        char *save = NULL;
        for (char *w = strtok_r(c, " \t\r\n\v\f", &save); w; w = strtok_r(NULL, " \t\r\n\v\f", &save)) strvec_push(&pieces, w);
        bool two = pieces.n == 2;
        for (size_t i = 0; two && i < pieces.n; i++)
            for (const char *p = pieces.v[i]; *p; p++)
                if (!isdigit((unsigned char)*p)) two = false;
        strvec_free(&pieces);
        free(c);
        if (two) {
            char *q = jq(t);
            char *m = xasprintf("bare range hunk header %s is not valid. Hunk headers need a verb: use `PUT N.=M:` or "
                                "`CUT N.=M`.",
                                q);
            free(q);
            return m;
        }
    }
    return NULL;
}

static bool parse_bare_range(const char *text, uint32_t *s, uint32_t *e) {
    char *t = trim(text);
    size_t n = strlen(t);
    bool ok = false;
    if (n && t[n - 1] == ':') {
        t[n - 1] = 0;
        /* split on whitespace, '-', '.', '=', '…' */
        strvec_t parts = {0};
        sb_t cur = SB_INIT;
        for (const char *p = t; *p;) {
            size_t k = ws_at(p);
            bool sep = k || *p == '-' || *p == '.' || *p == '=' || starts(p, "\xE2\x80\xA6");
            if (sep) {
                if (cur.len) strvec_push_own(&parts, sb_detach(&cur));
                cur = (sb_t)SB_INIT;
                p += k ? k : starts(p, "\xE2\x80\xA6") ? 3 : 1;
                continue;
            }
            sb_putc(&cur, *p++);
        }
        if (cur.len) strvec_push_own(&parts, sb_detach(&cur));
        else sb_free(&cur);
        if (parts.n == 2) {
            char *end1, *end2;
            errno = 0;
            unsigned long a = strtoul(parts.v[0], &end1, 10), b = strtoul(parts.v[1], &end2, 10);
            bool d1 = *parts.v[0] && *end1 == 0 && isdigit((unsigned char)parts.v[0][0]);
            bool d2 = *parts.v[1] && *end2 == 0 && isdigit((unsigned char)parts.v[1][0]);
            if (d1 && d2 && !errno && a <= UINT32_MAX && b <= UINT32_MAX && a && b) {
                *s = (uint32_t)a;
                *e = (uint32_t)b;
                ok = true;
            }
        }
        strvec_free(&parts);
    }
    free(t);
    return ok;
}

static bool parse_snapshot_row(const char *text, uint32_t *line, char **value) {
    const char *t = fz_trim_start(text);
    const char *sp = strpbrk(t, ":|");
    if (!sp || sp == t || t[0] == '0') return false;
    for (const char *p = t; p < sp; p++)
        if (!isdigit((unsigned char)*p)) return false;
    uint64_t v = 0;
    for (const char *p = t; p < sp; p++) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return false;
    }
    *line = (uint32_t)v;
    *value = xstrdup(sp + 1);
    return true;
}

static bool handle_raw(exec_t *x, const char *text, uint32_t line_num) {
    if (!x->pending.active && is_read_metadata_line(text)) {
        warn_once(x, READ_METADATA_IGNORED_WARNING);
        return true;
    }
    char *cm = contamination_message(text);
    if (cm) return fail_at_own(x, line_num, cm);
    if (x->file_op.kind) return fail_at(x, line_num, MOVE_TAKES_NO_BODY);
    pending_t *p = &x->pending;
    if (p->active) {
        if (blank(text)) {
            handle_blank(x, text, line_num);
            return true;
        }
        const char *m = bodyless_message(&p->target, p->had_colon);
        if (m) return fail_at(x, line_num, m);
        bool minus = *fz_trim_start(text) == '-';
        if (!minus) warn_once(x, BARE_BODY_AUTO_PIPED_WARNING);
        take_deferred(p);
        prow_t r = {xstrdup(text), line_num, true, minus};
        vec_push(&p->payloads, r);
        return true;
    }
    if (blank(text)) return true;
    uint32_t s, e;
    if (parse_bare_range(text, &s, &e)) {
        target_t t = {.k = T_REPLACE, .s = s, .e = e};
        if (!validate_range(x, &t, line_num, RO_REPLACE)) return false;
        p->active = true;
        p->target = t;
        p->line_num = line_num;
        p->had_colon = true;
        warn_once(x, BARE_RANGE_AUTO_PUT_WARNING);
        return true;
    }
    uint32_t line;
    char *value;
    if (parse_snapshot_row(text, &line, &value)) {
        for (size_t i = 0; i < x->recovered_lines.n; i++)
            if (x->recovered_lines.v[i] == line) {
                free(value);
                return fail_at_own(x, line_num, repeated_snapshot_row_message(line));
            }
        vec_push(&x->recovered_lines, line);
        push_insert(x, C_BEFORE, line, value, line_num, true);
        push_delete_range(x, line, line, line_num);
        warn_once(x, SNAPSHOT_ROWS_AUTO_PUT_WARNING);
        free(value);
        return true;
    }
    char *q = jq(text);
    bool r = fail_at_own(x, line_num,
                         xasprintf("payload line has no preceding hunk header. Use `PUT N.=M:`, `CUT N.=M`, or `PUT "
                                   "<N:`/`PUT >N:` above the body. Got %s.",
                                   q));
    free(q);
    return r;
}

static bool consume_comments(exec_t *x) {
    size_t n = x->comments.n;
    prow_t *v = x->comments.v;
    x->comments.n = 0;
    x->comments.v = NULL;
    x->comments.cap = 0;
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        if (ok) ok = handle_raw(x, v[i].text, v[i].line_num);
        free(v[i].text);
    }
    free(v);
    return ok;
}

/* takes ownership of the token's heap parts */
static bool exec_feed(exec_t *x, token_t *t) {
    if (x->terminated) return true;
    switch (t->k) {
    case TOK_ENV_BEGIN: return consume_comments(x);
    case TOK_ENV_END:
        if (!consume_comments(x)) return false;
        x->terminated = true;
        return true;
    case TOK_ABORT: x->terminated = true; return true;
    case TOK_HEADER:
        if (!consume_comments(x)) return false;
        return flush_pending(x);
    case TOK_BLANK:
        if (!consume_comments(x)) return false;
        handle_blank(x, "", t->line_num);
        return true;
    case TOK_PAYLOAD:
        if (!consume_comments(x)) return false;
        return handle_literal(x, t->text, t->line_num);
    case TOK_RAW:
        if (!x->pending.active && *fz_trim_start(t->text) == '#') {
            prow_t r = {t->text, t->line_num, false, false};
            t->text = NULL;
            vec_push(&x->comments, r);
            return true;
        }
        if (!consume_comments(x)) return false;
        return handle_raw(x, t->text, t->line_num);
    case TOK_OP: {
        for (size_t i = 0; i < x->comments.n; i++) free(x->comments.v[i].text);
        x->comments.n = 0;
        target_t *tg = &t->target;
        if (tg->k == T_REPLACE && !validate_range(x, tg, t->line_num, RO_REPLACE)) return false;
        if (tg->k == T_CUT && !validate_range(x, tg, t->line_num, RO_CUT)) return false;
        if (t->had_colon && (tg->k == T_CUT || tg->k == T_CUT_BLOCK)) warn_once(x, CUT_COLON_IGNORED_WARNING);
        if (t->had_colon && tg->k != T_REM && tg->k != T_MOVE && tg->reg)
            return fail_at(x, t->line_num, COLON_ON_REGISTER_PUT);
        if (tg->k == T_REM) {
            if (!flush_pending(x)) return false;
            return set_file_op(x, 1, NULL, t->line_num);
        }
        if (tg->k == T_MOVE) {
            if (!flush_pending(x)) return false;
            char *d = tg->dest;
            tg->dest = NULL;
            return set_file_op(x, 2, d, t->line_num);
        }
        if (!flush_pending(x)) return false;
        x->pending.active = true;
        x->pending.target = *tg;
        memset(tg, 0, sizeof(*tg));
        x->pending.line_num = t->line_num;
        x->pending.had_colon = t->had_colon;
        return true;
    }
    }
    return true;
}

typedef struct {
    uint32_t line_num;
    u32v_t lines;
    bool clipboard;
} hunk_t;

static int cmp_hunk(const void *a, const void *b) {
    const hunk_t *x = a, *y = b;
    return x->line_num < y->line_num ? -1 : x->line_num > y->line_num;
}

typedef VEC(hunk_t) hunks_v;

static hunk_t *hunk_get(hunks_v *hs, uint32_t ln) {
    for (size_t i = 0; i < hs->n; i++)
        if (hs->v[i].line_num == ln) return &hs->v[i];
    hunk_t h = {ln, {0}, false};
    vec_push(hs, h);
    return &hs->v[hs->n - 1];
}

static bool normalize_overlaps(exec_t *x) {
    hunks_v hs = {0};
    for (size_t i = 0; i < x->edits.n; i++) {
        hedit_t *e = &x->edits.v[i];
        if (e->k == E_CUT) hunk_get(&hs, e->line_num)->clipboard = true;
        else if (e->k == E_PASTE && e->span) {
            hunk_t *h = hunk_get(&hs, e->line_num);
            h->clipboard = true;
            for (uint32_t l = e->s; l <= e->e && l >= e->s; l++) vec_push(&h->lines, l);
        } else if (e->k == E_DELETE) {
            vec_push(&hunk_get(&hs, e->line_num)->lines, e->anchor);
        }
    }
    for (size_t i = 0; i < hs.n; i++) u32_sort_uniq(&hs.v[i].lines);
    if (hs.n > 1) xqsort(hs.v, hs.n, sizeof(hunk_t), cmp_hunk);
    umap_t owner = {0};
    u32v_t dropped = {0};
    bool ok = true;
    for (size_t i = 0; i < hs.n && ok; i++) {
        hunk_t *h = &hs.v[i];
        if (!h->lines.n) continue;
        u32v_t overlaps = {0};
        for (size_t k = 0; k < h->lines.n; k++) {
            uint32_t o;
            if (umap_get(&owner, h->lines.v[k], &o)) vec_push(&overlaps, o);
        }
        u32_sort_uniq(&overlaps);
        if (!overlaps.n) {
            for (size_t k = 0; k < h->lines.n; k++) umap_put(&owner, h->lines.v[k], h->line_num);
            vec_free(&overlaps);
            continue;
        }
        bool has_prev = overlaps.n == 1;
        uint32_t prev = has_prev ? overlaps.v[0] : 0;
        bool exact = false;
        if (has_prev)
            for (size_t k = 0; k < hs.n; k++)
                if (hs.v[k].line_num == prev) {
                    hunk_t *old = &hs.v[k];
                    exact = !old->clipboard && old->lines.n == h->lines.n &&
                            !memcmp(old->lines.v, h->lines.v, h->lines.n * sizeof(uint32_t));
                }
        if (exact) {
            vec_push(&dropped, prev);
            for (size_t k = 0; k < owner.cap; k++)
                if (owner.used[k] && owner.vals[k] == prev) owner.vals[k] = h->line_num;
            for (size_t k = 0; k < h->lines.n; k++) umap_put(&owner, h->lines.v[k], h->line_num);
            warn_once(x, REPLACE_PAIR_COALESCED_WARNING);
            vec_free(&overlaps);
            continue;
        }
        uint32_t first = 0;
        for (size_t k = 0; k < h->lines.n; k++)
            if (umap_get(&owner, h->lines.v[k], NULL)) {
                first = h->lines.v[k];
                break;
            }
        char *prior = has_prev ? xasprintf("%u", prev) : xstrdup("an earlier line");
        ok = fail_at_own(x, h->line_num,
                         xasprintf("anchor line %u is already targeted by another hunk on line %s. Issue ONE hunk per "
                                   "range; payload is only the final desired content, never a before/after pair.",
                                   first, prior));
        free(prior);
        vec_free(&overlaps);
    }
    if (ok && dropped.n) {
        u32_sort_uniq(&dropped);
        size_t w = 0;
        for (size_t i = 0; i < x->edits.n; i++) {
            if (u32_has(&dropped, x->edits.v[i].line_num)) hedit_free(&x->edits.v[i]);
            else x->edits.v[w++] = x->edits.v[i];
        }
        x->edits.n = w;
    }
    for (size_t i = 0; i < hs.n; i++) vec_free(&hs.v[i].lines);
    vec_free(&hs);
    vec_free(&dropped);
    umap_free(&owner);
    return ok;
}

static bool exec_finish(exec_t *x) {
    if (!consume_comments(x)) return false;
    if (!flush_pending(x)) return false;
    if (x->file_op.kind == 1 && x->edits.n) {
        free(x->err);
        x->err = xstrdup("`REM` deletes the whole file and cannot be combined with line ops.");
        return false;
    }
    return normalize_overlaps(x);
}

/* tokenizer.feed + end: split on '\n', strip one trailing '\r' per row; the final unterminated row only when non-empty */
static bool parse_body(const char *diff, exec_t *x) {
    memset(x, 0, sizeof(*x));
    size_t len = strlen(diff);
    size_t start = 0;
    uint32_t ln = 1;
    bool ok = true;
    while (ok && start <= len) {
        const char *nl = memchr(diff + start, '\n', len - start);
        if (!nl && start == len) break;
        size_t end = nl ? (size_t)(nl - diff) : len;
        size_t e2 = end;
        if (e2 > start && diff[e2 - 1] == '\r') e2--;
        char *line = xstrndup(diff + start, e2 - start);
        token_t t = classify_line(line, ln++);
        free(line);
        ok = exec_feed(x, &t);
        token_free(&t);
        if (!nl) break;
        start = end + 1;
    }
    if (ok) ok = exec_finish(x);
    return ok;
}

/* ======================================================================================
 * input sections (input.rs)
 * ====================================================================================== */

typedef struct {
    char *path;
    char *file_hash;
    char *diff;
    bool interleaved, placeholder;
    /* memoized parse */
    int parsed; /* 0 not yet, 1 ok, -1 failed */
    exec_t exec;
    char *perr;
    bool range_err;
} hsection_t;

struct hl_patch {
    VEC(hsection_t) secs;
};

/* (?i)^\*{0,3}\s*(?:(?:update|add|delete|move)[^A-Za-z0-9]*(?:file|to)?[^A-Za-z0-9]*:)?\s*\*{0,3}\s* */
static const char *strip_apply_patch_noise(const char *s) {
    const char *p = s;
    for (int i = 0; i < 3 && *p == '*'; i++) p++;
    p = skip_ws(p);
    static const char *const KW[] = {"update", "add", "delete", "move"};
    for (size_t k = 0; k < 4; k++) {
        size_t n = strlen(KW[k]);
        if (strncasecmp(p, KW[k], n)) continue;
        const char *q = p + n;
        const char *best = NULL;
        /* [^A-Za-z0-9]* (file|to)? [^A-Za-z0-9]* : — try with and without the word */
        for (int attempt = 0; attempt < 2 && !best; attempt++) {
            const char *r = q;
            while (*r && !isalnum((unsigned char)*r) && *r != ':') r++;
            if (attempt == 0) {
                if (!strncasecmp(r, "file", 4)) r += 4;
                else if (!strncasecmp(r, "to", 2)) r += 2;
                else continue;
            }
            while (*r && !isalnum((unsigned char)*r) && *r != ':') r++;
            if (*r == ':') best = r + 1;
        }
        if (best) p = best;
        break;
    }
    p = skip_ws(p);
    for (int i = 0; i < 3 && *p == '*'; i++) p++;
    return skip_ws(p);
}

static char *unquote(const char *path) {
    size_t n = strlen(path);
    if (n >= 2 && ((path[0] == '"' && path[n - 1] == '"') || (path[0] == '\'' && path[n - 1] == '\'')))
        return xstrndup(path + 1, n - 2);
    return xstrdup(path);
}

static char *normalize_hashline_path(const char *raw, const char *cwd) {
    char *t = trim(raw);
    char *u = unquote(t);
    free(t);
    char *cleaned = xstrdup(strip_apply_patch_noise(u));
    free(u);
    if (!cwd || cleaned[0] != '/') return cleaned;
    char *pn = path_abs(cleaned, "/");
    char *cn = path_abs(cwd, "/");
    size_t cl = strlen(cn);
    char *r = NULL;
    if (!strcmp(pn, cn)) r = xstrdup(".");
    else if (cl == 1 && cn[0] == '/') r = xstrdup(pn + 1);
    else if (!strncmp(pn, cn, cl) && pn[cl] == '/') r = xstrdup(pn + cl + 1);
    free(pn);
    free(cn);
    if (r) {
        free(cleaned);
        return r;
    }
    return cleaned;
}

/* unwrap leading bracketed envelope markers (`[*** Begin Patch] [a.ts#1A2B]`) */
static const char *unbracket_envelope_markers(const char *line) {
    static const char *const M[] = {BEGIN_MARK, END_MARK, ABORT_MARK};
    const char *rest = line;
    for (;;) {
        if (*rest != '[') return rest;
        const char *inner = skip_ws(rest + 1);
        const char *mk = NULL;
        for (size_t i = 0; i < 3; i++)
            if (starts(inner, M[i])) mk = M[i];
        if (!mk) return rest;
        const char *tail = skip_ws(inner + strlen(mk));
        if (*tail == ']') tail++;
        tail = skip_ws(tail);
        if (!*tail) return mk;
        rest = tail;
    }
}

typedef struct {
    char *path, *file_hash;
    bool placeholder;
} rawhdr_t;

static bool is_hex4_tail(const char *body, size_t *start, char **tag) {
    /* #([0-9A-Fa-f]{4})\s*$ */
    size_t n = strlen(body);
    while (n && isspace((unsigned char)body[n - 1])) n--;
    if (n < 5 || body[n - 5] != '#') return false;
    for (size_t i = n - 4; i < n; i++)
        if (!isxdigit((unsigned char)body[i])) return false;
    *start = n - 5;
    *tag = xstrndup(body + n - 4, 4);
    for (char *c = *tag; *c; c++) *c = (char)toupper((unsigned char)*c);
    return true;
}

static bool placeholder_tail(const char *body, size_t *start) {
    /* #[^\s#./\\:\[\]]{0,16}\s*$ (leftmost match) */
    for (const char *h = strchr(body, '#'); h; h = strchr(h + 1, '#')) {
        const char *p = h + 1;
        size_t cnt = 0;
        while (*p && cnt < 16) {
            if (ws_at(p) || strchr("#./\\:[]", *p)) break;
            uint32_t cp;
            int k = utf8_decode(p, strlen(p), &cp);
            p += k > 0 ? k : 1;
            cnt++;
        }
        if (!*skip_ws(p)) {
            *start = (size_t)(h - body);
            return true;
        }
    }
    return false;
}

static bool recover_header(const char *line, const char *cwd, rawhdr_t *out) {
    size_t n = strlen(line);
    if (n < 2 || line[0] != '[' || line[n - 1] != ']') return false;
    char *inner = xstrndup(line + 1, n - 2);
    char *tb = trim(inner);
    free(inner);
    char *body = xstrdup(strip_apply_patch_noise(tb));
    free(tb);
    if (!*body) {
        free(body);
        return false;
    }
    size_t at;
    char *tag = NULL;
    char *path_text;
    bool placeholder = false;
    if (is_hex4_tail(body, &at, &tag)) path_text = xstrndup(body, at);
    else if (placeholder_tail(body, &at)) {
        path_text = xstrndup(body, at);
        placeholder = true;
    } else path_text = trim_end(body);
    free(body);
    if ((!tag && strchr(path_text, '#')) || header_path_has_orphan_bracket(path_text)) {
        free(path_text);
        free(tag);
        return false;
    }
    char *path = normalize_hashline_path(path_text, cwd);
    free(path_text);
    if (!*path) {
        free(path);
        free(tag);
        return false;
    }
    out->path = path;
    out->file_hash = tag;
    out->placeholder = placeholder;
    return true;
}

/* 1 = header (out filled), 0 = not a header, -1 = error (*err) */
static int parse_header_line(const char *line, const char *cwd, rawhdr_t *out, char **err) {
    char *te = trim_end(line);
    const char *trimmed = unbracket_envelope_markers(te);
    memset(out, 0, sizeof(*out));
    int r = 0;
    if (*trimmed == '[') {
        char *path, *hash;
        token_t t = classify_line(trimmed, 0);
        if (t.k == TOK_HEADER) {
            path = t.text;
            hash = t.file_hash;
            t.text = t.file_hash = NULL;
            char *norm = normalize_hashline_path(path, cwd);
            free(path);
            if (!*norm) {
                free(norm);
                free(hash);
                *err = xstrdup("Input header \"[]\" is empty; provide a file path.");
                r = -1;
            } else {
                out->path = norm;
                out->file_hash = hash;
                r = 1;
            }
        } else if (recover_header(trimmed, cwd, out)) {
            r = 1;
        } else {
            char *q = jq(trimmed);
            *err = xasprintf("Input header must be [PATH#TAG] with a 4-hex content-hash tag; got %s.", q);
            free(q);
            r = -1;
        }
        token_free(&t);
    }
    free(te);
    return r;
}

static void hsection_free(hsection_t *s) {
    free(s->path);
    free(s->file_hash);
    free(s->diff);
    if (s->parsed) exec_free(&s->exec);
    free(s->perr);
}

void hl_patch_free(hl_patch_t *p) {
    if (!p) return;
    for (size_t i = 0; i < p->secs.n; i++) hsection_free(&p->secs.v[i]);
    vec_free(&p->secs);
    free(p);
}

static strvec_t split_rows(const char *input) {
    const char *in = starts(input, "\xEF\xBB\xBF") ? input + 3 : input;
    strvec_t lines = split_nl(in);
    /* strip leading blank / envelope-begin rows */
    size_t skip = 0;
    while (skip < lines.n) {
        char *l = lines.v[skip];
        size_t n = strlen(l);
        char *c = n && l[n - 1] == '\r' ? xstrndup(l, n - 1) : xstrdup(l);
        const char *clean = unbracket_envelope_markers(c);
        bool drop = blank(clean);
        if (!drop) {
            token_t t = classify_line(clean, 0);
            drop = t.k == TOK_ENV_BEGIN;
            token_free(&t);
        }
        free(c);
        if (!drop) break;
        skip++;
    }
    strvec_t out = {0};
    for (size_t i = skip; i < lines.n; i++) {
        char *l = lines.v[i];
        size_t n = strlen(l);
        strvec_push_own(&out, n && l[n - 1] == '\r' ? xstrndup(l, n - 1) : xstrdup(l));
    }
    if (!out.n) strvec_push(&out, "");
    strvec_free(&lines);
    return out;
}

hl_patch_t *hl_patch_parse(const char *input, const char *cwd, char **err) {
    *err = NULL;
    /* normalize_fallback validates every header-looking row first */
    {
        const char *in = starts(input, "\xEF\xBB\xBF") ? input + 3 : input;
        strvec_t ls = str_split_lines(in, strlen(in));
        for (size_t i = 0; i < ls.n; i++) {
            rawhdr_t h;
            int r = parse_header_line(ls.v[i], cwd, &h, err);
            if (r < 0) {
                strvec_free(&ls);
                return NULL;
            }
            if (r > 0) {
                free(h.path);
                free(h.file_hash);
                break;
            }
        }
        strvec_free(&ls);
    }
    strvec_t lines = split_rows(input);
    rawhdr_t first;
    int r = parse_header_line(lines.v[0], cwd, &first, err);
    if (r < 0) {
        strvec_free(&lines);
        return NULL;
    }
    if (r == 0) {
        char *te = trim_end(lines.v[0]);
        if (unified_hunk_header(te)) {
            *err = xstrdup("unified-diff hunk header (`@@ -N,M +N,M @@`) is not valid in hashline. File sections start "
                           "with `[path#HASH]`; use `replace`, `delete`, or `insert` ops.");
        } else {
            char *pv = chars_prefix(lines.v[0], 120, NULL);
            char *q = jq(pv);
            *err = xasprintf("input must begin with \"[PATH#HASH]\" on the first non-blank line for anchored edits; "
                             "got: %s. Example: \"[src/foo.ts#1A2B]\" then edit ops.",
                             q);
            free(pv);
            free(q);
        }
        free(te);
        strvec_free(&lines);
        return NULL;
    }
    free(first.path);
    free(first.file_hash);
    /* split */
    VEC(hsection_t) raw = {0};
    hsection_t cur;
    bool have = false;
    strvec_t body = {0};
#define FLUSH()                                                                                                        \
    do {                                                                                                               \
        if (have) {                                                                                                    \
            bool any = false;                                                                                          \
            for (size_t q_ = 0; q_ < body.n; q_++)                                                                     \
                if (!blank(body.v[q_])) any = true;                                                                    \
            if (any) {                                                                                                 \
                cur.diff = strvec_join(&body, "\n");                                                                   \
                vec_push(&raw, cur);                                                                                   \
            } else {                                                                                                   \
                hsection_free(&cur);                                                                                   \
            }                                                                                                          \
            have = false;                                                                                              \
        }                                                                                                              \
        strvec_free(&body);                                                                                            \
        body = (strvec_t){0};                                                                                          \
    } while (0)
    for (size_t i = 0; i < lines.n; i++) {
        char *te = trim_end(lines.v[i]);
        const char *clean = unbracket_envelope_markers(te);
        token_t t = classify_line(clean, 0);
        tokk_t k = t.k;
        token_free(&t);
        if (k == TOK_ENV_END || k == TOK_ABORT) {
            free(te);
            break;
        }
        if (k == TOK_ENV_BEGIN) {
            free(te);
            continue;
        }
        if (*clean == '[') {
            rawhdr_t h;
            int hr = parse_header_line(clean, cwd, &h, err);
            if (hr < 0) {
                free(te);
                FLUSH();
                for (size_t q = 0; q < raw.n; q++) hsection_free(&raw.v[q]);
                vec_free(&raw);
                strvec_free(&lines);
                return NULL;
            }
            if (hr > 0) {
                FLUSH();
                memset(&cur, 0, sizeof(cur));
                cur.path = h.path;
                cur.file_hash = h.file_hash;
                cur.placeholder = h.placeholder;
                have = true;
                free(te);
                continue;
            }
        }
        free(te);
        strvec_push(&body, lines.v[i]);
    }
    FLUSH();
#undef FLUSH
    strvec_free(&lines);
    /* merge same-path sections */
    hl_patch_t *p = xcalloc(1, sizeof(*p));
    const char *previous = NULL;
    for (size_t i = 0; i < raw.n; i++) {
        hsection_t *s = &raw.v[i];
        hsection_t *ex = NULL;
        for (size_t k = 0; k < p->secs.n; k++)
            if (!strcmp(p->secs.v[k].path, s->path)) ex = &p->secs.v[k];
        if (ex) {
            if (ex->file_hash && s->file_hash && strcmp(ex->file_hash, s->file_hash) != 0) {
                *err = xasprintf("Conflicting hashline snapshot tags for %s: #%s and #%s. Re-read the file and retry "
                                 "with one current header.",
                                 s->path, ex->file_hash, s->file_hash);
                for (size_t q = i; q < raw.n; q++) hsection_free(&raw.v[q]);
                vec_free(&raw);
                hl_patch_free(p);
                return NULL;
            }
            if (!ex->file_hash && s->file_hash) {
                ex->file_hash = s->file_hash;
                s->file_hash = NULL;
            }
            if (s->placeholder || ex->placeholder) {
                ex->placeholder = true;
                free(ex->file_hash);
                ex->file_hash = NULL;
            }
            if (!previous || strcmp(previous, s->path) != 0) ex->interleaved = true;
            char *d = xasprintf("%s\n%s", ex->diff, s->diff);
            free(ex->diff);
            ex->diff = d;
            previous = ex->path;
            hsection_free(s);
            continue;
        }
        vec_push(&p->secs, *s);
        previous = p->secs.v[p->secs.n - 1].path;
    }
    vec_free(&raw);
    return p;
}

size_t hl_patch_count(const hl_patch_t *p) { return p->secs.n; }
const char *hl_section_path(const hl_patch_t *p, size_t i) { return p->secs.v[i].path; }
const char *hl_section_tag(const hl_patch_t *p, size_t i) { return p->secs.v[i].file_hash; }

char *hl_first_header_path(const char *input, const char *cwd) {
    const char *in = starts(input, "\xEF\xBB\xBF") ? input + 3 : input;
    strvec_t ls = str_split_lines(in, strlen(in));
    char *r = NULL;
    for (size_t i = 0; i < ls.n && !r; i++) {
        char *te = trim_end(ls.v[i]);
        const char *clean = unbracket_envelope_markers(te);
        rawhdr_t h;
        char *err = NULL;
        if (parse_header_line(clean, cwd, &h, &err) > 0) {
            r = h.path;
            free(h.file_hash);
        }
        free(err);
        free(te);
    }
    strvec_free(&ls);
    return r;
}

static char *current_tag_advice(const char *actual) {
    return xasprintf("The current file hashes to #%s. Re-read the file with `read` to copy a current [path#tag] header "
                     "— never invent the tag and never reuse one from a prior session.",
                     actual);
}

char *hl_header_only_message(const char *path, const char *actual) {
    char *adv = current_tag_advice(actual);
    char *r = xasprintf("Edit rejected for %s: no ops follow the header; put them under this file's current tag, #%s.\n%s",
                        path, actual, adv);
    free(adv);
    return r;
}

static bool has_clipboard_edit(const hedits_t *e) {
    for (size_t i = 0; i < e->n; i++) {
        const hedit_t *x = &e->v[i];
        if (x->k == E_CUT || x->k == E_PASTE) return true;
        if (x->k == E_BLOCK && (x->mode == BM_CUT || x->mode == BM_PASTE_AFTER || x->reg)) return true;
    }
    return false;
}

bool hl_section_parse(hl_patch_t *p, size_t i, char **err, bool *range_err) {
    hsection_t *s = &p->secs.v[i];
    if (!s->parsed) {
        bool ok = parse_body(s->diff, &s->exec);
        if (!ok) {
            s->perr = s->exec.err ? xstrdup(s->exec.err) : xstrdup("parse failed");
            s->range_err = s->exec.range_err;
        } else if (s->interleaved && has_clipboard_edit(&s->exec.edits)) {
            s->perr = xstrdup(CLIPBOARD_INTERLEAVED_SECTIONS);
            ok = false;
        } else if (s->exec.file_op.kind == 2) {
            char *d = normalize_hashline_path(s->exec.file_op.dest, NULL);
            free(s->exec.file_op.dest);
            s->exec.file_op.dest = d;
        }
        s->parsed = ok ? 1 : -1;
    }
    if (range_err) *range_err = s->parsed < 0 && s->range_err;
    if (s->parsed < 0) {
        *err = xstrdup(s->perr);
        return false;
    }
    return true;
}

int hl_section_file_op(hl_patch_t *p, size_t i, const char **dest) {
    hsection_t *s = &p->secs.v[i];
    if (s->parsed != 1) return 0;
    if (dest) *dest = s->exec.file_op.dest;
    return s->exec.file_op.kind;
}

/* ======================================================================================
 * block resolution (heuristic stand-in for pi-ast block_range_at)
 * ====================================================================================== */

static size_t indent_width(const char *s) {
    size_t n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += *s == '\t' ? 4 : 1;
    return n;
}

static bool is_markdown(const char *path) {
    const char *e = path_ext(path);
    return !strcasecmp(e, ".md") || !strcasecmp(e, ".markdown") || !strcasecmp(e, ".mdx");
}

static bool is_indent_lang(const char *path) {
    const char *e = path_ext(path);
    return !strcasecmp(e, ".py") || !strcasecmp(e, ".pyi") || !strcasecmp(e, ".yaml") || !strcasecmp(e, ".yml") ||
           !strcasecmp(e, ".nim") || !strcasecmp(e, ".coffee");
}

static int heading_level(const char *s) {
    int n = 0;
    while (s[n] == '#') n++;
    return (n > 0 && n <= 6 && (s[n] == ' ' || s[n] == 0)) ? n : 0;
}

static int bracket_delta(const char *s, int *opened_any) {
    int d = 0;
    char q = 0;
    for (const char *p = s; *p; p++) {
        if (q) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == q) q = 0;
            continue;
        }
        if (*p == '"' || *p == '\'' || *p == '`') {
            q = *p;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') break;
        if (*p == '{' || *p == '(' || *p == '[') {
            d++;
            if (opened_any) *opened_any = 1;
        } else if (*p == '}' || *p == ')' || *p == ']') {
            d--;
        }
    }
    return d;
}

/* span of the construct opening at 1-based line n; false when none (blank, out of range, unbalanced) */
static bool block_span(char *const *L, size_t total_sz, const char *path, uint32_t n, uint32_t *bs, uint32_t *be) {
    int64_t total = (int64_t)total_sz;
    if (n < 1 || n > total) return false;
    const char *first = L[n - 1];
    if (blank(first)) return false;
    *bs = n;
    if (is_markdown(path)) {
        int lv = heading_level(first);
        if (!lv) {
            int64_t e = n;
            while (e < total && !blank(L[e])) e++;
            *be = (uint32_t)e;
            return true;
        }
        int64_t e = n;
        bool in_fence = false;
        for (int64_t i = n; i < total; i++) {
            const char *l = L[i];
            if (starts(l, "```") || starts(l, "~~~")) in_fence = !in_fence;
            if (!in_fence) {
                int hl = heading_level(l);
                if (hl && hl <= lv) break;
            }
            e = i + 1;
        }
        *be = (uint32_t)e;
        return true;
    }
    int64_t decl = n;
    while (decl <= total) {
        const char *l = L[decl - 1];
        while (*l == ' ' || *l == '\t') l++;
        bool deco = (*l == '@' && isalpha((unsigned char)l[1])) || starts(l, "#[") || starts(l, "///") ||
                    starts(l, "/**") || starts(l, "* ") || starts(l, "*/") || starts(l, "//!");
        if (starts(l, "/**")) {
            while (decl <= total && !strstr(L[decl - 1], "*/")) decl++;
            decl++;
            continue;
        }
        if (!deco) break;
        if (*l == '@' && bracket_delta(l, NULL) > 0) {
            int depth = 0;
            while (decl <= total) {
                depth += bracket_delta(L[decl - 1], NULL);
                if (depth <= 0) break;
                decl++;
            }
        }
        decl++;
    }
    if (decl > total) return false;
    if (!is_indent_lang(path)) {
        int depth = 0, opened = 0;
        for (int64_t i = decl; i <= total; i++) {
            depth += bracket_delta(L[i - 1], &opened);
            if (opened && depth <= 0) {
                *be = (uint32_t)i;
                return true;
            }
            if (!opened) {
                /* a statement that ends before opening any bracket is a single-line node */
                char *te = trim_end(L[i - 1]);
                size_t tl = strlen(te);
                bool ends = tl && (te[tl - 1] == ';' || te[tl - 1] == ',');
                free(te);
                if (ends) {
                    *be = (uint32_t)i;
                    return true;
                }
            }
            if (!opened && i > decl + 3) break;
        }
        if (opened) return false;
    }
    size_t base = indent_width(L[decl - 1]);
    int64_t e = decl;
    for (int64_t i = decl + 1; i <= total; i++) {
        const char *l = L[i - 1];
        if (blank(l)) continue;
        if (indent_width(l) <= base) {
            const char *t = l + strspn(l, " \t");
            if (*t == '}' || *t == ')' || *t == ']' || starts(t, "end")) e = i;
            break;
        }
        e = i;
    }
    *be = (uint32_t)e;
    return true;
}

char *hl_range_error_message(hl_patch_t *p, size_t i, const char *text) {
    hsection_t *s = &p->secs.v[i];
    exec_t *x = &s->exec;
    bool hb = false;
    uint32_t bs = 0, be = 0;
    if (text) {
        strvec_t L = split_nl(text);
        hb = block_span(L.v, L.n, s->path, x->r_s, &bs, &be) && bs == x->r_s && be > bs;
        strvec_free(&L);
    }
    return invalid_absolute_range_message(x->r_line, x->r_s, x->r_e, x->r_op, hb, bs, be, x->r_reg);
}

/* ======================================================================================
 * clipboard (clipboard.rs + store Clipboard)
 * ====================================================================================== */

typedef struct {
    char *name;
    strvec_t lines;
} reg_t;

struct hl_clip {
    VEC(reg_t) named;
    bool has_anon;
    strvec_t anon;
    bool pending_set;
    strvec_t pending; /* describe_cut of pending anonymous cuts */
};

static void clip_clear(hl_clip_t *c) {
    for (size_t i = 0; i < c->named.n; i++) {
        free(c->named.v[i].name);
        strvec_free(&c->named.v[i].lines);
    }
    vec_free(&c->named);
    strvec_free(&c->anon);
    strvec_free(&c->pending);
    memset(c, 0, sizeof(*c));
}

void hl_clip_free(hl_clip_t *c) {
    if (!c) return;
    clip_clear(c);
    free(c);
}

static void clip_copy(hl_clip_t *dst, const hl_clip_t *src) {
    memset(dst, 0, sizeof(*dst));
    for (size_t i = 0; i < src->named.n; i++) {
        reg_t r = {xstrdup(src->named.v[i].name), {0}};
        for (size_t k = 0; k < src->named.v[i].lines.n; k++) strvec_push(&r.lines, src->named.v[i].lines.v[k]);
        vec_push(&dst->named, r);
    }
    dst->has_anon = src->has_anon;
    for (size_t k = 0; k < src->anon.n; k++) strvec_push(&dst->anon, src->anon.v[k]);
    dst->pending_set = src->pending_set;
    for (size_t k = 0; k < src->pending.n; k++) strvec_push(&dst->pending, src->pending.v[k]);
}

static reg_t *clip_named(hl_clip_t *c, const char *name) {
    for (size_t i = 0; i < c->named.n; i++)
        if (!strcmp(c->named.v[i].name, name)) return &c->named.v[i];
    return NULL;
}

static void clip_set_named(hl_clip_t *c, const char *name, strvec_t lines) {
    reg_t *r = clip_named(c, name);
    if (r) {
        strvec_free(&r->lines);
        r->lines = lines;
        return;
    }
    reg_t n = {xstrdup(name), lines};
    vec_push(&c->named, n);
}

hl_clip_t *hl_clip_start(void *store_v) {
    edit_store_t *st = store_v;
    hl_clip_t *c = xcalloc(1, sizeof(*c));
    if (!st) return c;
    pthread_mutex_lock(&st->mu);
    hmap_foreach(&st->registers, it) clip_set_named(c, it->key, str_split((const char *)it->val, "\n"));
    pthread_mutex_unlock(&st->mu);
    return c;
}

void hl_clip_commit(hl_clip_t *c, void *store_v) {
    edit_store_t *st = store_v;
    if (!st || !c) return;
    pthread_mutex_lock(&st->mu);
    for (size_t i = 0; i < c->named.n; i++)
        free(hmap_put(&st->registers, c->named.v[i].name, strvec_join(&c->named.v[i].lines, "\n")));
    pthread_mutex_unlock(&st->mu);
}

static char *describe_cut(const hedit_t *e) {
    char *span = e->s == e->e ? xasprintf("%u", e->s) : xasprintf("%u.=%u", e->s, e->e);
    char *r = e->reg ? xasprintf("CUT %s @%s", span, e->reg) : xasprintf("CUT %s", span);
    free(span);
    return r;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *known_registers(hl_clip_t *c) {
    strvec_t k = {0};
    for (size_t i = 0; i < c->named.n; i++) strvec_push(&k, c->named.v[i].name);
    if (k.n > 1) xqsort(k.v, k.n, sizeof(char *), cmp_str);
    sb_t b = SB_INIT;
    for (size_t i = 0; i < k.n; i++) sb_appendf(&b, "%s`@%s`", i ? ", " : "", k.v[i]);
    strvec_free(&k);
    return sb_detach(&b);
}

/* 1 = lines in *out, 0 = drop (unused), -1 = error */
static int read_register(const char *reg, bool span, hl_clip_t *c, uint32_t line_num, strvec_t *warnings, strvec_t *out,
                         char **err) {
    memset(out, 0, sizeof(*out));
    if (reg) {
        reg_t *r = clip_named(c, reg);
        if (r) {
            for (size_t i = 0; i < r->lines.n; i++) strvec_push(out, r->lines.v[i]);
            return 1;
        }
        char *known = known_registers(c);
        char *avail = *known ? xasprintf(" Available registers: %s.", known) : xstrdup("");
        if (span) {
            *err = xasprintf("line %u: `@%s` is empty — no `CUT … @%s` precedes this op in this call and no persisted "
                             "register has that name — so pasting it over a range would delete those lines and write "
                             "nothing back. Capture the register first (`CUT … @%s`), or use `CUT` if deleting the "
                             "range is what you meant.%s",
                             line_num, reg, reg, reg, avail);
            free(known);
            free(avail);
            return -1;
        }
        if (warnings)
            strvec_push_own(warnings, xasprintf("line %u: `@%s` was empty — no `CUT … @%s` precedes this op in this call "
                                                "and no persisted register has that name — so nothing was pasted.%s",
                                                line_num, reg, reg, avail));
        free(known);
        free(avail);
        return 1;
    }
    if (c->pending.n > 1) {
        char *j = strvec_join(&c->pending, ", ");
        *err = xasprintf("line %u: %zu unlabeled `CUT`s are pending (%s) — an unlabeled paste cannot tell which one you "
                         "meant. Label the moves (`CUT … @name` → `PUT … @name`), or keep at most one unlabeled `CUT` "
                         "before each unlabeled paste.",
                         line_num, c->pending.n, j);
        free(j);
        return -1;
    }
    if (!c->has_anon) {
        *err = xasprintf("line %u: %s", line_num, EMPTY_PASTE);
        return -1;
    }
    for (size_t i = 0; i < c->anon.n; i++) strvec_push(out, c->anon.v[i]);
    strvec_free(&c->pending);
    c->pending_set = true;
    return 1;
}

static bool write_register(const hedit_t *e, char *const *L, size_t nl, hl_clip_t *c, char **err) {
    if (e->s == 0 || e->e > nl) {
        char *d = describe_cut(e);
        *err = xasprintf("line %u: `%s` is out of range (file has %zu lines).", e->line_num, d, nl);
        free(d);
        return false;
    }
    strvec_t cap = {0};
    for (uint32_t l = e->s; l <= e->e; l++) strvec_push(&cap, L[l - 1]);
    if (e->reg) clip_set_named(c, e->reg, cap);
    else {
        strvec_free(&c->anon);
        c->anon = cap;
        c->has_anon = true;
        c->pending_set = true;
        strvec_push_own(&c->pending, describe_cut(e));
    }
    return true;
}

static bool validate_clipboard_sequence(const hedits_t *edits, const hl_clip_t *clip, char **err) {
    hl_clip_t fork;
    clip_copy(&fork, clip);
    bool ok = true;
    for (size_t i = 0; i < edits->n && ok; i++) {
        const hedit_t *e = &edits->v[i];
        if (e->k == E_CUT) {
            if (e->reg) clip_set_named(&fork, e->reg, (strvec_t){0});
            else {
                strvec_free(&fork.anon);
                fork.has_anon = true;
                fork.pending_set = true;
                strvec_push_own(&fork.pending, describe_cut(e));
            }
        } else if (e->k == E_PASTE) {
            strvec_t out;
            if (read_register(e->reg, e->span, &fork, e->line_num, NULL, &out, err) < 0) ok = false;
            strvec_free(&out);
        }
    }
    clip_clear(&fork);
    return ok;
}

static bool resolve_clipboard_edits(const hedits_t *in, char *const *L, size_t nl, hl_clip_t *c, strvec_t *warnings,
                                    hedits_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    uint32_t synth = 0;
    for (size_t i = 0; i < in->n; i++) {
        const hedit_t *e = &in->v[i];
        if (e->k == E_CUT) {
            if (!write_register(e, L, nl, c, err)) return false;
            continue;
        }
        if (e->k != E_PASTE) {
            vec_push(out, hedit_clone(e));
            continue;
        }
        strvec_t lines;
        if (read_register(e->reg, e->span, c, e->line_num, warnings, &lines, err) < 0) return false;
        if (!e->span) {
            for (size_t k = 0; k < lines.n; k++) {
                hedit_t ins;
                memset(&ins, 0, sizeof(ins));
                ins.k = E_INSERT;
                ins.cur = e->cur;
                ins.anchor = e->anchor;
                ins.text = xstrdup(lines.v[k]);
                ins.line_num = e->line_num;
                ins.index = synth++;
                vec_push(out, ins);
            }
        } else {
            if (e->s == 0 || e->e > nl) {
                char *sfx = e->reg ? xasprintf(" @%s", e->reg) : xstrdup("");
                *err = xasprintf("line %u: `PUT %u.=%u%s` is out of range (file has %zu lines).", e->line_num, e->s,
                                 e->e, sfx, nl);
                free(sfx);
                strvec_free(&lines);
                return false;
            }
            for (size_t k = 0; k < lines.n; k++) {
                hedit_t ins;
                memset(&ins, 0, sizeof(ins));
                ins.k = E_INSERT;
                ins.cur = C_BEFORE;
                ins.anchor = e->s;
                ins.text = xstrdup(lines.v[k]);
                ins.replacement = true;
                ins.line_num = e->line_num;
                ins.index = synth++;
                vec_push(out, ins);
            }
            for (uint32_t l = e->s; l <= e->e; l++) {
                hedit_t d;
                memset(&d, 0, sizeof(d));
                d.k = E_DELETE;
                d.anchor = l;
                d.line_num = e->line_num;
                d.index = synth++;
                vec_push(out, d);
            }
        }
        strvec_free(&lines);
    }
    return true;
}

/* ======================================================================================
 * block lowering (block.rs)
 * ====================================================================================== */

static bool structural_closer(const char *line) {
    /* ^\s*[)\]}]+[;,]?\s*$ */
    const char *p = skip_ws(line);
    if (!(*p == ')' || *p == ']' || *p == '}')) return false;
    while (*p == ')' || *p == ']' || *p == '}') p++;
    if (*p == ';' || *p == ',') p++;
    return *skip_ws(p) == 0;
}

static bool has_block_edit(const hedits_t *e) {
    for (size_t i = 0; i < e->n; i++)
        if (e->v[i].k == E_BLOCK) return true;
    return false;
}

typedef struct {
    uint32_t anchor, s, e;
    bm_t op;
} bres_t;
typedef VEC(bres_t) breses_t;

static const char *block_op_form_fmt(bm_t m) {
    return m == BM_NONE ? "PUT %u*:" : m == BM_INSERT_AFTER ? "PUT >%u*:" : m == BM_CUT ? "CUT %u*" : "PUT >%u*";
}
static const char *block_op_plain_fmt(bm_t m) {
    return m == BM_NONE ? "PUT %u:" : m == BM_INSERT_AFTER ? "PUT >%u:" : m == BM_CUT ? "CUT %u" : "PUT >%u";
}

static bool find_next_block(uint32_t anchor, char *const *L, size_t n, const char *path, uint32_t *bs, uint32_t *be) {
    uint32_t last = (uint32_t)MIN((uint64_t)n, (uint64_t)anchor + 64);
    for (uint32_t l = anchor + 1; l <= last; l++) {
        if (blank(L[l - 1])) continue;
        uint32_t s, e;
        if (block_span(L, n, path, l, &s, &e) && s == l && e > l) {
            *bs = s;
            *be = e;
            return true;
        }
    }
    return false;
}

static bool find_enclosing_block(uint32_t anchor, char *const *L, size_t n, const char *path, uint32_t *bs,
                                 uint32_t *be) {
    uint32_t first = anchor > 64 ? anchor - 64 : 1;
    for (uint32_t l = anchor; l-- > first;) {
        if (l < 1) break;
        if (l - 1 >= n || blank(L[l - 1])) continue;
        uint32_t s, e;
        if (block_span(L, n, path, l, &s, &e) && s == l && e >= anchor && e > l) {
            *bs = s;
            *be = e;
            return true;
        }
    }
    return false;
}

static bool resolve_block_edits(const hedits_t *in, const char *text, const char *path, breses_t *res,
                                strvec_t *warnings, hedits_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    strvec_t L = split_nl(text);
    uint32_t synth = 0;
    bool ok = true;
    for (size_t i = 0; i < in->n && ok; i++) {
        const hedit_t *e = &in->v[i];
        if (e->k != E_BLOCK) {
            vec_push(out, hedit_clone(e));
            continue;
        }
        uint32_t bs = 0, be = 0;
        bool found = block_span(L.v, L.n, path, e->anchor, &bs, &be);
        if (!found) {
            if (e->mode == BM_INSERT_AFTER || e->mode == BM_PASTE_AFTER) {
                bool closer = e->anchor >= 1 && e->anchor - 1 < L.n && structural_closer(L.v[e->anchor - 1]);
                const char *bf = e->mode == BM_PASTE_AFTER ? "PUT >%u*" : "PUT >%u*:";
                const char *pf = e->mode == BM_PASTE_AFTER ? "PUT >%u" : "PUT >%u:";
                char *b = xasprintf(bf, e->anchor), *pl = xasprintf(pf, e->anchor);
                if (closer)
                    strvec_push_own(warnings, xasprintf("`%s` anchors on a closing delimiter, so it was applied as plain "
                                                        "`%s`. Anchor on the line that OPENS the construct.",
                                                        b, pl));
                else
                    strvec_push_own(warnings,
                                    xasprintf("`%s` could not resolve a syntactic block on line %u, so it was applied as "
                                              "plain `%s`. Verify the landing line; anchor on a line that OPENS a "
                                              "construct.",
                                              b, e->anchor, pl));
                free(b);
                free(pl);
                if (e->mode == BM_PASTE_AFTER) {
                    hedit_t p;
                    memset(&p, 0, sizeof(p));
                    p.k = E_PASTE;
                    p.cur = C_AFTER;
                    p.anchor = e->anchor;
                    p.reg = e->reg ? xstrdup(e->reg) : NULL;
                    p.line_num = e->line_num;
                    p.index = synth++;
                    vec_push(out, p);
                } else {
                    for (size_t k = 0; k < e->payloads.n; k++) {
                        hedit_t ins;
                        memset(&ins, 0, sizeof(ins));
                        ins.k = E_INSERT;
                        ins.cur = C_AFTER;
                        ins.anchor = e->anchor;
                        ins.text = xstrdup(e->payloads.v[k]);
                        ins.line_num = e->line_num;
                        ins.index = synth++;
                        vec_push(out, ins);
                    }
                }
                continue;
            }
            rop_t rop = e->mode == BM_CUT ? RO_CUT : RO_REPLACE;
            uint32_t ns = 0, ne = 0, es = 0, ee = 0;
            bool anchor_blank = e->anchor >= 1 && e->anchor - 1 < L.n && blank(L.v[e->anchor - 1]);
            bool has_next = anchor_blank && find_next_block(e->anchor, L.v, L.n, path, &ns, &ne);
            bool has_enc = !has_next && find_enclosing_block(e->anchor, L.v, L.n, path, &es, &ee);
            sb_t m = SB_INIT;
            if (anchor_blank && has_next) {
                char *retry = block_form_at(rop, ns, e->reg);
                sb_appendf(&m, "Line %u is blank; no syntactic block can begin there. The next multi-line block begins at "
                               "line %u and ends at line %u. Retry `%s`.",
                           e->anchor, ns, ne, retry);
                free(retry);
            } else {
                char *phrase = block_form_at(rop, e->anchor, e->reg);
                char *fb = rop == RO_REPLACE
                               ? (e->reg ? xasprintf("PUT %u.=M @%s", e->anchor, e->reg) : xasprintf("PUT %u.=M:", e->anchor))
                               : (e->reg ? xasprintf("CUT %u.=M @%s", e->anchor, e->reg) : xasprintf("CUT %u.=M", e->anchor));
                sb_appendf(&m, "`%s` could not resolve a syntactic block beginning on line %u (unsupported language, "
                               "blank/closer line, or parse error). Use `%s` with explicit lines.",
                           phrase, e->anchor, fb);
                free(phrase);
                free(fb);
            }
            if (has_enc) {
                char *retry = block_form_at(rop, es, e->reg);
                sb_appendf(&m, " The nearest enclosing multi-line block begins at line %u and ends at line %u; use `%s` "
                               "to target it.",
                           es, ee, retry);
                free(retry);
            }
            u32v_t a = {0};
            vec_push(&a, e->anchor);
            strvec_t ctx = {0};
            format_anchored_context(&a, L.v, L.n, &ctx);
            if (ctx.n) {
                char *j = strvec_join(&ctx, "\n");
                sb_appendf(&m, "\n\n%s", j);
                free(j);
            }
            strvec_free(&ctx);
            vec_free(&a);
            *err = xasprintf("line %u: %s", e->line_num, sb_cstr(&m));
            sb_free(&m);
            ok = false;
            break;
        }
        if (bs == be) {
            uint32_t es = 0, ee = 0;
            bool has_enc = find_enclosing_block(e->anchor, L.v, L.n, path, &es, &ee);
            char *form = xasprintf(block_op_form_fmt(e->mode), e->anchor);
            char *plain = xasprintf(block_op_plain_fmt(e->mode), e->anchor);
            sb_t m = SB_INIT;
            sb_appendf(&m, "`%s` resolved a single-line block — line %u is a bare statement, not the opening line of a "
                           "multi-line construct. For only this statement use `%s`.",
                       form, e->anchor, plain);
            if (has_enc) {
                char *ef = xasprintf(block_op_form_fmt(e->mode), es);
                sb_appendf(&m, " The nearest enclosing multi-line block begins at line %u and ends at line %u; use `%s` "
                               "to target it.",
                           es, ee, ef);
                free(ef);
            }
            *err = xasprintf("line %u: %s", e->line_num, sb_cstr(&m));
            sb_free(&m);
            free(form);
            free(plain);
            ok = false;
            break;
        }
        bres_t r = {e->anchor, bs, be, e->mode};
        vec_push(res, r);
        if (e->mode == BM_PASTE_AFTER) {
            hedit_t p;
            memset(&p, 0, sizeof(p));
            p.k = E_PASTE;
            p.cur = C_AFTER;
            p.anchor = be;
            p.reg = e->reg ? xstrdup(e->reg) : NULL;
            p.line_num = e->line_num;
            p.index = synth++;
            vec_push(out, p);
        } else if (e->mode == BM_CUT) {
            hedit_t c;
            memset(&c, 0, sizeof(c));
            c.k = E_CUT;
            c.s = bs;
            c.e = be;
            c.reg = e->reg ? xstrdup(e->reg) : NULL;
            c.line_num = e->line_num;
            c.index = synth++;
            vec_push(out, c);
            for (uint32_t l = bs; l <= be; l++) {
                hedit_t d;
                memset(&d, 0, sizeof(d));
                d.k = E_DELETE;
                d.anchor = l;
                d.line_num = e->line_num;
                d.index = synth++;
                vec_push(out, d);
            }
        } else if (e->mode == BM_INSERT_AFTER) {
            for (size_t k = 0; k < e->payloads.n; k++) {
                hedit_t ins;
                memset(&ins, 0, sizeof(ins));
                ins.k = E_INSERT;
                ins.cur = C_AFTER;
                ins.anchor = be;
                ins.text = xstrdup(e->payloads.v[k]);
                ins.line_num = e->line_num;
                ins.index = synth++;
                vec_push(out, ins);
            }
        } else if (e->reg) {
            hedit_t p;
            memset(&p, 0, sizeof(p));
            p.k = E_PASTE;
            p.span = true;
            p.s = bs;
            p.e = be;
            p.reg = xstrdup(e->reg);
            p.line_num = e->line_num;
            p.index = synth++;
            vec_push(out, p);
        } else {
            for (size_t k = 0; k < e->payloads.n; k++) {
                hedit_t ins;
                memset(&ins, 0, sizeof(ins));
                ins.k = E_INSERT;
                ins.cur = C_BEFORE;
                ins.anchor = bs;
                ins.text = xstrdup(e->payloads.v[k]);
                ins.replacement = true;
                ins.line_num = e->line_num;
                ins.index = synth++;
                vec_push(out, ins);
            }
            for (uint32_t l = bs; l <= be; l++) {
                hedit_t d;
                memset(&d, 0, sizeof(d));
                d.k = E_DELETE;
                d.anchor = l;
                d.line_num = e->line_num;
                d.index = synth++;
                vec_push(out, d);
            }
        }
    }
    strvec_free(&L);
    if (!ok) hedits_free(out);
    return ok;
}

static char *format_block_resolution(const bres_t *r) {
    const char *tpl = r->op == BM_NONE ? "PUT %u*:" : r->op == BM_INSERT_AFTER ? "PUT >%u*:" : r->op == BM_CUT ? "CUT %u*" : "PUT >%u*";
    const char *sfx = r->op == BM_INSERT_AFTER ? "; body lands after line " : r->op == BM_PASTE_AFTER ? "; clipboard lands after line " : "";
    char *op = xasprintf(tpl, r->anchor);
    uint32_t lines = r->e - r->s + 1;
    char *span = r->s == r->e ? xasprintf("line %u", r->s) : xasprintf("lines %u-%u", r->s, r->e);
    char *suffix = *sfx ? xasprintf("%s%u", sfx, r->e) : xstrdup("");
    char *out = xasprintf("%s \xE2\x86\x92 resolved %s (%u line%s)%s", op, span, lines, lines == 1 ? "" : "s", suffix);
    free(op);
    free(span);
    free(suffix);
    return out;
}

/* ======================================================================================
 * applier core (apply.rs: bounds, phantom line, indentation repair, materialize)
 * ====================================================================================== */

static bool anchored(const hedit_t *e, uint32_t *a) {
    if (e->k == E_DELETE || (e->k == E_INSERT && (e->cur == C_BEFORE || e->cur == C_AFTER))) {
        *a = e->anchor;
        return true;
    }
    return false;
}

static const char *leading_indent_end(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool indent_deeper(const char *deeper, size_t dl, const char *shallower, size_t sl) {
    return dl > sl && !strncmp(deeper, shallower, sl);
}

typedef struct {
    VEC(size_t) ins, del;
    uint32_t start, end;
} rgroup_t;

static bool replacement_group(const hedits_t *E, size_t start, rgroup_t *g) {
    memset(g, 0, sizeof(*g));
    if (start >= E->n) return false;
    const hedit_t *f = &E->v[start];
    if (!(f->k == E_INSERT && f->cur == C_BEFORE && f->replacement)) return false;
    uint32_t al = f->anchor, op = f->line_num;
    size_t i = start;
    while (i < E->n && E->v[i].k == E_INSERT && E->v[i].cur == C_BEFORE && E->v[i].replacement) {
        if (E->v[i].anchor != al || E->v[i].line_num != op) break;
        vec_push(&g->ins, i);
        i++;
    }
    uint32_t expected = al;
    while (i < E->n && E->v[i].k == E_DELETE) {
        if (E->v[i].anchor != expected || E->v[i].line_num != op) break;
        vec_push(&g->del, i);
        expected++;
        i++;
    }
    if (!g->del.n) {
        vec_free(&g->ins);
        return false;
    }
    g->start = al;
    g->end = expected - 1;
    return true;
}

static bool repair_indentation(hedits_t *E, char *const *L, size_t nl) {
    bool repaired = false;
    size_t start = 0;
    while (start < E->n) {
        rgroup_t g;
        if (!replacement_group(E, start, &g)) {
            start++;
            continue;
        }
        start = g.del.v[g.del.n - 1] + 1;
        bool skip = g.ins.n != g.del.n;
        const char *preceding = g.start >= 2 && g.start - 2 < nl ? L[g.start - 2] : "";
        const char *source_first = g.start >= 1 && g.start - 1 < nl ? L[g.start - 1] : "";
        const char *payload_first = g.ins.n ? E->v[g.ins.v[0]].text : "";
        if (!skip) {
            char *pt = trim_end(preceding);
            size_t pl = strlen(pt);
            bool opens = pl && pt[pl - 1] == '{';
            free(pt);
            size_t pi = (size_t)(leading_indent_end(preceding) - preceding);
            size_t si = (size_t)(leading_indent_end(source_first) - source_first);
            size_t yi = (size_t)(leading_indent_end(payload_first) - payload_first);
            if (!opens || !indent_deeper(source_first, si, preceding, pi) || indent_deeper(payload_first, yi, preceding, pi))
                skip = true;
        }
        if (!skip) {
            const char *shift = NULL;
            size_t shift_len = 0, matches = 0;
            bool consistent = true;
            for (size_t off = 0; off < g.ins.n; off++) {
                const char *payload = E->v[g.ins.v[off]].text;
                size_t idx = (size_t)(g.start - 1) + off;
                const char *source = idx < nl ? L[idx] : "";
                if (blank(source) || strcmp(fz_trim_start(source), fz_trim_start(payload)) != 0) continue;
                size_t si = (size_t)(leading_indent_end(source) - source);
                size_t yi = (size_t)(leading_indent_end(payload) - payload);
                if (si < yi || strncmp(source + si - yi, payload, yi) != 0) {
                    consistent = false;
                    break;
                }
                size_t cl = si - yi;
                if (shift && (shift_len != cl || strncmp(shift, source, cl) != 0)) {
                    consistent = false;
                    break;
                }
                shift = source;
                shift_len = cl;
                matches++;
            }
            if (shift && consistent && shift_len && matches >= 2 && matches * 2 > g.ins.n) {
                for (size_t k = 0; k < g.ins.n; k++) {
                    hedit_t *e = &E->v[g.ins.v[k]];
                    if (blank(e->text)) continue;
                    char *t = xasprintf("%.*s%s", (int)shift_len, shift, e->text);
                    free(e->text);
                    e->text = t;
                }
                repaired = true;
            }
        }
        vec_free(&g.ins);
        vec_free(&g.del);
    }
    return repaired;
}

typedef enum { SLOT_BEFORE, SLOT_REPL, SLOT_DELETE, SLOT_AFTER } slot_t;
typedef struct {
    uint32_t line;
    slot_t slot;
    const char *text;
    size_t order;
} aent_t;

static int cmp_aent(const void *a, const void *b) {
    const aent_t *x = a, *y = b;
    if (x->line != y->line) return x->line < y->line ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static char *materialize(char *const *orig, size_t n, const hedits_t *E, int64_t *first_out) {
    int64_t first = 0;
    strvec_t bof = {0}, eof = {0};
    VEC(aent_t) an = {0};
    for (size_t i = 0; i < E->n; i++) {
        const hedit_t *e = &E->v[i];
        aent_t a = {0, SLOT_BEFORE, "", i};
        if (e->k == E_INSERT) {
            if (e->cur == C_BOF) {
                strvec_push(&bof, e->text);
                continue;
            }
            if (e->cur == C_EOF) {
                strvec_push(&eof, e->text);
                continue;
            }
            a.line = e->anchor;
            a.text = e->text;
            a.slot = e->cur == C_AFTER ? SLOT_AFTER : e->replacement ? SLOT_REPL : SLOT_BEFORE;
        } else if (e->k == E_DELETE) {
            a.line = e->anchor;
            a.slot = SLOT_DELETE;
        } else {
            continue;
        }
        vec_push(&an, a);
    }
    if (an.n > 1) xqsort(an.v, an.n, sizeof(aent_t), cmp_aent);
    VEC(const char *) out = {0};
    size_t copied = 0;
    for (size_t i = 0; i < an.n;) {
        size_t j = i;
        while (j < an.n && an.v[j].line == an.v[i].line) j++;
        uint32_t line = an.v[i].line;
        size_t index = line - 1;
        size_t end = MIN(index, n);
        for (size_t k = MIN(copied, end); k < end; k++) vec_push(&out, orig[k]);
        bool del = false;
        for (size_t k = i; k < j; k++)
            if (an.v[k].slot == SLOT_BEFORE) vec_push(&out, an.v[k].text);
        for (size_t k = i; k < j; k++)
            if (an.v[k].slot == SLOT_REPL) vec_push(&out, an.v[k].text);
        for (size_t k = i; k < j; k++)
            if (an.v[k].slot == SLOT_DELETE) del = true;
        if (!del) vec_push(&out, index < n ? orig[index] : "");
        for (size_t k = i; k < j; k++)
            if (an.v[k].slot == SLOT_AFTER) vec_push(&out, an.v[k].text);
        copied = index + 1;
        if (!first) first = line;
        i = j;
    }
    for (size_t k = copied; k < n; k++) vec_push(&out, orig[k]);
    if (bof.n) {
        if (out.n == 1 && !*out.v[0]) out.n = 0;
        VEC(const char *) nb = {0};
        for (size_t k = 0; k < bof.n; k++) vec_push(&nb, bof.v[k]);
        for (size_t k = 0; k < out.n; k++) vec_push(&nb, out.v[k]);
        vec_free(&out);
        out.v = nb.v;
        out.n = nb.n;
        out.cap = nb.cap;
        first = 1;
    }
    if (eof.n) {
        size_t at;
        if (out.n == 1 && !*out.v[0]) {
            out.n = 0;
            for (size_t k = 0; k < eof.n; k++) vec_push(&out, eof.v[k]);
            at = 1;
        } else {
            size_t index = out.n && !*out.v[out.n - 1] ? out.n - 1 : out.n;
            VEC(const char *) nb = {0};
            for (size_t k = 0; k < index; k++) vec_push(&nb, out.v[k]);
            for (size_t k = 0; k < eof.n; k++) vec_push(&nb, eof.v[k]);
            for (size_t k = index; k < out.n; k++) vec_push(&nb, out.v[k]);
            vec_free(&out);
            out.v = nb.v;
            out.n = nb.n;
            out.cap = nb.cap;
            at = index + 1;
        }
        first = first ? MIN(first, (int64_t)at) : (int64_t)at;
    }
    sb_t b = SB_INIT;
    for (size_t k = 0; k < out.n; k++) {
        if (k) sb_putc(&b, '\n');
        sb_append(&b, out.v[k]);
    }
    vec_free(&out);
    vec_free(&an);
    strvec_free(&bof);
    strvec_free(&eof);
    *first_out = first;
    return sb_detach(&b);
}

/* apply_edits: concrete edits (no blocks) against LF text */
static bool apply_edits(const char *text, const hedits_t *edits, hl_clip_t *clip, char **new_text, int64_t *first,
                        strvec_t *warnings, char **err) {
    if (!edits->n) {
        *new_text = xstrdup(text);
        *first = 0;
        return true;
    }
    strvec_t L = split_nl(text);
    hedits_t target;
    if (!resolve_clipboard_edits(edits, L.v, L.n, clip, warnings, &target, err)) {
        hedits_free(&target);
        strvec_free(&L);
        return false;
    }
    /* phantom final empty line: deletes of it are dropped */
    if (L.n > 1 && !*L.v[L.n - 1]) {
        uint32_t ph = (uint32_t)L.n;
        size_t w = 0;
        for (size_t i = 0; i < target.n; i++) {
            if (target.v[i].k == E_DELETE && target.v[i].anchor == ph) hedit_free(&target.v[i]);
            else target.v[w++] = target.v[i];
        }
        target.n = w;
    }
    for (size_t i = 0; i < target.n; i++) target.v[i].index = (uint32_t)i;
    for (size_t i = 0; i < target.n; i++) {
        uint32_t a;
        if (anchored(&target.v[i], &a) && (a < 1 || a > L.n)) {
            *err = xasprintf("Line %u does not exist (file has %zu lines)", a, L.n);
            hedits_free(&target);
            strvec_free(&L);
            return false;
        }
    }
    if (repair_indentation(&target, L.v, L.n)) strvec_push(warnings, REPLACEMENT_INDENT_AUTO_SHIFT_WARNING);
    *new_text = materialize(L.v, L.n, &target, first);
    hedits_free(&target);
    strvec_free(&L);
    return true;
}

/* ======================================================================================
 * store glue
 * ====================================================================================== */

static snapshot_t *store_by_content(edit_store_t *st, const char *path, const char *text) {
    char *tag = file_tag(text, strlen(text));
    snapshot_t *r = NULL;
    for (size_t i = st->snaps.n; i > 0; i--) {
        snapshot_t *x = st->snaps.v[i - 1];
        if (!strcmp(x->path, path) && !strcmp(x->tag, tag) && !strcmp(x->content, text)) {
            r = x;
            break;
        }
    }
    free(tag);
    return r;
}

static snapshot_t *store_by_hash(edit_store_t *st, const char *path, const char *tag) {
    for (size_t i = st->snaps.n; i > 0; i--) {
        snapshot_t *x = st->snaps.v[i - 1];
        if (!strcmp(x->path, path) && !strcasecmp(x->tag, tag)) return x;
    }
    return NULL;
}

/* seen state: 0 = unrestricted (None / empty), 1 = restricted set */
static bool snap_restricted(const snapshot_t *s) {
    if (!s->seen.n) return false;
    for (size_t i = 0; i < s->seen.n; i++)
        if (s->seen.v[i].lo <= 1 && s->seen.v[i].hi == INT64_MAX) return false;
    return true;
}

static bool snap_seen(const snapshot_t *s, uint32_t line) {
    for (size_t i = 0; i < s->seen.n; i++)
        if ((int64_t)line >= s->seen.v[i].lo && (int64_t)line <= s->seen.v[i].hi) return true;
    return false;
}

static void snap_add_lines(snapshot_t *s, const u32v_t *lines) {
    for (size_t i = 0; i < lines->n; i++) {
        line_span_t r = {lines->v[i], lines->v[i]};
        vec_push(&s->seen, r);
    }
}

/* record text (moving it to newest); lines NULL = leave seen untouched */
static snapshot_t *store_record(edit_store_t *st, const char *path, const char *text, const u32v_t *lines) {
    edit_store_record(st, path, text, -1, -1);
    pthread_mutex_lock(&st->mu);
    snapshot_t *s = store_by_content(st, path, text);
    if (s && lines) snap_add_lines(s, lines);
    pthread_mutex_unlock(&st->mu);
    return s;
}

static void collect_anchor_lines(const hedits_t *E, u32v_t *out) {
    for (size_t i = 0; i < E->n; i++) {
        const hedit_t *e = &E->v[i];
        if (e->k == E_DELETE || e->k == E_BLOCK) vec_push(out, e->anchor);
        else if (e->k == E_CUT || (e->k == E_PASTE && e->span))
            for (uint32_t l = e->s; l <= e->e && l >= e->s; l++) vec_push(out, l);
        else if ((e->k == E_PASTE || e->k == E_INSERT) && (e->cur == C_BEFORE || e->cur == C_AFTER))
            vec_push(out, e->anchor);
    }
    u32_sort_uniq(out);
}

static char *mismatch(hsection_t *s, const char *canonical, const char *normalized, const char *expected,
                      edit_store_t *st) {
    char *actual = file_tag(normalized, strlen(normalized));
    strvec_t fl = split_nl(normalized);
    u32v_t anchors = {0};
    if (s->parsed == 1) collect_anchor_lines(&s->exec.edits, &anchors);
    /* record_reply_provenance: rows the reply prints count as seen under the current tag */
    pthread_mutex_lock(&st->mu);
    snapshot_t *cur = store_by_content(st, canonical, normalized);
    bool restricted = !cur || snap_restricted(cur);
    pthread_mutex_unlock(&st->mu);
    if (restricted) {
        u32v_t disp = {0};
        for (size_t i = 0; i < anchors.n; i++) {
            uint32_t l = anchors.v[i];
            if (l < 1 || l > fl.n) continue;
            uint32_t lo = l > MISMATCH_CONTEXT ? l - MISMATCH_CONTEXT : 1;
            uint32_t hi = l + MISMATCH_CONTEXT < fl.n ? l + MISMATCH_CONTEXT : (uint32_t)fl.n;
            for (uint32_t k = lo; k <= hi; k++) vec_push(&disp, k);
        }
        vec_push(&disp, 0);
        bool fresh = !cur;
        snapshot_t *sn = store_record(st, canonical, normalized, NULL);
        if (sn) {
            pthread_mutex_lock(&st->mu);
            if (fresh) sn->seen.n = 0;
            snap_add_lines(sn, &disp);
            pthread_mutex_unlock(&st->mu);
        }
        vec_free(&disp);
    } else {
        store_record(st, canonical, normalized, NULL);
    }
    bool recognized = false;
    strvec_t origins = {0};
    pthread_mutex_lock(&st->mu);
    if (*expected) {
        recognized = store_by_hash(st, canonical, expected) != NULL;
        if (!recognized)
            for (size_t i = 0; i < st->snaps.n; i++) {
                snapshot_t *x = st->snaps.v[i];
                if (!strcasecmp(x->tag, expected) && strcmp(x->path, canonical) != 0) {
                    bool dup = false;
                    for (size_t k = 0; k < origins.n; k++)
                        if (!strcmp(origins.v[k], x->path)) dup = true;
                    if (!dup) strvec_push(&origins, x->path);
                }
            }
    }
    pthread_mutex_unlock(&st->mu);
    strvec_t lines = {0};
    const char *path = s->path;
    if (recognized) {
        strvec_push_own(&lines, xasprintf("Edit rejected for %s: file changed between read and edit.", path));
        strvec_push_own(&lines, xasprintf("Section is bound to #%s, but the current file hashes to #%s. If a prior edit "
                                          "in this session modified this file, copy the [path#newhash] header from that "
                                          "edit's response; otherwise re-read the file with `read` to refresh the tag "
                                          "before retrying.",
                                          expected, actual));
    } else {
        if (!*expected)
            strvec_push_own(&lines, xasprintf("Edit rejected for %s: the header needs this file's current tag, #%s.",
                                              path, actual));
        else strvec_push_own(&lines, xasprintf("Edit rejected for %s: hash #%s is not from this session.", path, expected));
        for (size_t i = 0; i < origins.n; i++)
            strvec_push_own(&lines, xasprintf("Hash #%s was issued in this session for %s.", expected, origins.v[i]));
        strvec_push_own(&lines, current_tag_advice(actual));
    }
    strvec_t ctx = {0};
    format_anchored_context(&anchors, fl.v, fl.n, &ctx);
    if (ctx.n) {
        strvec_push(&lines, "");
        for (size_t i = 0; i < ctx.n; i++) strvec_push(&lines, ctx.v[i]);
    }
    char *r = strvec_join(&lines, "\n");
    strvec_free(&ctx);
    strvec_free(&lines);
    strvec_free(&origins);
    strvec_free(&fl);
    vec_free(&anchors);
    free(actual);
    return r;
}

char *hl_mismatch_message(hl_patch_t *p, size_t i, const char *canonical, const char *text, const char *expected,
                          void *store) {
    return mismatch(&p->secs.v[i], canonical, text, expected ? expected : "", store);
}

static bool has_anchor_scoped_edit(const hedits_t *E) {
    for (size_t i = 0; i < E->n; i++) {
        const hedit_t *e = &E->v[i];
        if (e->k == E_DELETE || e->k == E_BLOCK || e->k == E_CUT) return true;
        if (e->k == E_PASTE && e->span) return true;
        if ((e->k == E_PASTE || e->k == E_INSERT) && (e->cur == C_BEFORE || e->cur == C_AFTER)) return true;
    }
    return false;
}

/* guarded lines: anchors + every line a resolved replace/cut block covers */
static void guarded_lines(const hedits_t *section_edits, const breses_t *blocks, u32v_t *out) {
    collect_anchor_lines(section_edits, out);
    for (size_t i = 0; i < blocks->n; i++)
        if (blocks->v[i].op == BM_NONE || blocks->v[i].op == BM_CUT)
            for (uint32_t l = blocks->v[i].s; l <= blocks->v[i].e; l++) vec_push(out, l);
    u32_sort_uniq(out);
}

static bool assert_seen_lines(hsection_t *s, const breses_t *blocks, const char *expected, const char *canonical,
                              edit_store_t *st, const char *text, size_t max_columns, char **err) {
    u32v_t guarded = {0};
    guarded_lines(&s->exec.edits, blocks, &guarded);
    pthread_mutex_lock(&st->mu);
    snapshot_t *sn = store_by_content(st, canonical, text);
    bool ok = true;
    if (!sn) {
        pthread_mutex_unlock(&st->mu);
        if (max_columns) {
            strvec_t L = split_nl(text);
            u32v_t wide = {0};
            for (size_t i = 0; i < guarded.n; i++) {
                uint32_t l = guarded.v[i];
                if (l >= 1 && l <= L.n && strlen(L.v[l - 1]) > max_columns) vec_push(&wide, l);
            }
            if (wide.n) {
                char *ranges = format_line_ranges(&wide);
                char *sel = selector_of(ranges);
                *err = xasprintf("This edit anchors to lines %s of %s, which exceed the display column cap, and no "
                                 "retained read shows them in full. Read `%s:%s:raw` for their full content, then "
                                 "re-issue the edit.",
                                 ranges, s->path, s->path, sel);
                free(ranges);
                free(sel);
                ok = false;
            }
            vec_free(&wide);
            strvec_free(&L);
        }
        vec_free(&guarded);
        return ok;
    }
    if (!snap_restricted(sn)) {
        pthread_mutex_unlock(&st->mu);
        vec_free(&guarded);
        return true;
    }
    u32v_t unseen = {0};
    for (size_t i = 0; i < guarded.n; i++)
        if (!snap_seen(sn, guarded.v[i])) vec_push(&unseen, guarded.v[i]);
    if (!unseen.n) {
        pthread_mutex_unlock(&st->mu);
        vec_free(&guarded);
        return true;
    }
    strvec_t src = split_nl(sn->content);
    for (size_t i = 0; i < unseen.n; i++) {
        uint32_t l = unseen.v[i];
        if (l >= 1 && l <= src.n && fz_char_count(src.v[l - 1]) > SEEN_LINE_REVEAL_MAX_COLUMNS) {
            pthread_mutex_unlock(&st->mu);
            char *ranges = format_line_ranges(&unseen);
            char *sel = selector_of(ranges);
            *err = xasprintf("This edit anchors to lines %s of %s that were not fully displayed. Read `%s:%s:raw` for "
                             "their full content, then re-issue the edit.",
                             ranges, s->path, s->path, sel);
            free(ranges);
            free(sel);
            strvec_free(&src);
            vec_free(&unseen);
            vec_free(&guarded);
            return false;
        }
    }
    u32v_t revealed = {0};
    sb_t preview = SB_INIT;
    for (size_t i = 0; i < unseen.n && revealed.n < SEEN_LINE_REVEAL_CAP; i++) {
        uint32_t l = unseen.v[i];
        if (l < 1 || l > src.n) continue;
        vec_push(&revealed, l);
        if (preview.len) sb_putc(&preview, '\n');
        sb_appendf(&preview, "  %u:%s", l, src.v[l - 1]);
    }
    bool truncated = unseen.n > revealed.n;
    if (!truncated) snap_add_lines(sn, &revealed);
    pthread_mutex_unlock(&st->mu);
    char *ranges = format_line_ranges(&unseen);
    char *sel = selector_of(ranges);
    char *header = xasprintf("This edit anchors to lines %s of %s that [%s#%s] never displayed (it showed a partial "
                             "range, a search hit, or a folded summary).",
                             ranges, s->path, s->path, expected);
    if (!revealed.n)
        *err = xasprintf("%s Re-read them in full first with a ranged read like `%s:%s` — it skips summarization and "
                         "mints a fresh tag (a plain re-read just re-folds them) — then re-issue the edit.",
                         header, s->path, sel);
    else if (truncated)
        *err = xasprintf("%s Preview of the actual file content at the first %zu unseen line(s):\n%s\nThe range exceeds "
                         "the inline preview cap — re-read the remainder with `%s:%s` before re-issuing the edit.",
                         header, revealed.n, sb_cstr(&preview), s->path, sel);
    else
        *err = xasprintf("%s Actual file content at those lines:\n%s\nVerify the content matches what you intend to "
                         "touch, then re-issue the edit with the same [path#tag] header — a straight retry now succeeds "
                         "without a re-read. If the content does NOT match, fix your line numbers.",
                         header, sb_cstr(&preview));
    free(header);
    free(ranges);
    free(sel);
    sb_free(&preview);
    vec_free(&revealed);
    strvec_free(&src);
    vec_free(&unseen);
    vec_free(&guarded);
    return false;
}

void hl_applied_free(hl_applied_t *a) {
    free(a->text);
    strvec_free(&a->warnings);
    strvec_free(&a->block_preview);
    memset(a, 0, sizeof(*a));
}

bool hl_section_apply(hl_patch_t *p, size_t i, const char *canonical, const char *text, void *store_v, hl_clip_t *clip,
                      bool enforce_seen, size_t max_columns, hl_applied_t *out, char **err) {
    edit_store_t *st = store_v;
    hsection_t *s = &p->secs.v[i];
    memset(out, 0, sizeof(*out));
    const char *expected = s->file_hash ? s->file_hash : "";
    hedits_t none = {0};
    const hedits_t *edits = s->exec.file_op.kind == 1 ? &none : &s->exec.edits;
    char *actual = file_tag(text, strlen(text));
    bool live = !strcasecmp(actual, expected);
    free(actual);
    pthread_mutex_lock(&st->mu);
    snapshot_t *stored = store_by_hash(st, canonical, expected);
    char *stored_text = stored ? xstrdup(stored->content) : NULL;
    pthread_mutex_unlock(&st->mu);
    breses_t blocks = {0};
    strvec_t resolve_warnings = {0};
    hedits_t resolved = {0};
    bool ok = true;
    if (has_block_edit(edits)) {
        const char *base = live ? text : stored_text;
        if (!base) {
            *err = mismatch(s, canonical, text, expected, st);
            free(stored_text);
            return false;
        }
        ok = resolve_block_edits(edits, base, s->path, &blocks, &resolve_warnings, &resolved, err);
    } else {
        for (size_t k = 0; k < edits->n; k++) vec_push(&resolved, hedit_clone(&edits->v[k]));
    }
    if (ok) ok = validate_clipboard_sequence(&resolved, clip, err);
    if (ok) {
        if (live) {
            if (enforce_seen) ok = assert_seen_lines(s, &blocks, expected, canonical, st, text, max_columns, err);
            if (ok) ok = apply_edits(text, &resolved, clip, &out->text, &out->first_changed, &resolve_warnings, err);
        } else if (!resolved.n || has_anchor_scoped_edit(&resolved)) {
            *err = mismatch(s, canonical, text, expected, st);
            ok = false;
        } else {
            strvec_t aw = {0};
            ok = apply_edits(text, &resolved, clip, &out->text, &out->first_changed, &aw, err);
            strvec_push(&resolve_warnings, HEADTAIL_DRIFT_WARNING);
            for (size_t k = 0; k < aw.n; k++) strvec_push(&resolve_warnings, aw.v[k]);
            strvec_free(&aw);
            vec_free(&blocks);
        }
    }
    if (ok) {
        for (size_t k = 0; k < s->exec.warnings.n; k++) strvec_push(&out->warnings, s->exec.warnings.v[k]);
        for (size_t k = 0; k < resolve_warnings.n; k++) strvec_push(&out->warnings, resolve_warnings.v[k]);
        for (size_t k = 0; k < blocks.n; k++) strvec_push_own(&out->block_preview, format_block_resolution(&blocks.v[k]));
    } else {
        hl_applied_free(out);
    }
    strvec_free(&resolve_warnings);
    hedits_free(&resolved);
    vec_free(&blocks);
    free(stored_text);
    return ok;
}

/* ======================================================================================
 * no-op bookkeeping + post-commit recording
 * ====================================================================================== */

char *hl_no_change_message(const char *path) {
    return xasprintf("Edits to %s parsed and applied cleanly, but produced no change: your body row(s) are "
                     "byte-identical to the file at the targeted lines. The bug is somewhere else — re-read the file "
                     "before issuing another edit. Do NOT widen the payload or add lines; verify the anchor first.",
                     path);
}

char *hl_no_change_loop_message(const char *path, uint32_t count) {
    return xasprintf("STOP. Edits to %s have been a byte-identical no-op %u times in a row — the patch body matches the "
                     "file at the targeted lines and the soft hint did not break the cycle. Cease re-issuing this "
                     "payload. Either the intended change is already on disk (move on), or your anchor is wrong "
                     "(re-read the file with `read` to observe the current line numbers and tag, then author a "
                     "different edit). This exact payload will keep being rejected until it changes.",
                     path, count);
}

uint32_t hl_record_noop(void *store_v, const char *canonical, const char *input, bool *escalate) {
    edit_store_t *st = store_v;
    uint64_t h = hash_xx64(input, strlen(input), 0);
    char key[32];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)h);
    pthread_mutex_lock(&st->mu);
    char *prev = hmap_get(&st->noop, canonical);
    uint32_t count = 1;
    if (prev && !strncmp(prev, key, 16) && prev[16] == ':') count = (uint32_t)strtoul(prev + 17, NULL, 10) + 1;
    free(hmap_put(&st->noop, canonical, xasprintf("%s:%u", key, count)));
    pthread_mutex_unlock(&st->mu);
    *escalate = count >= NOOP_HARD_LIMIT;
    return count;
}

void hl_reset_noop(void *store_v, const char *canonical) {
    edit_store_t *st = store_v;
    pthread_mutex_lock(&st->mu);
    free(hmap_del(&st->noop, canonical));
    pthread_mutex_unlock(&st->mu);
}

char *hl_record_plain(void *store_v, const char *key, const char *text) {
    edit_store_t *st = store_v;
    snapshot_t *s = store_record(st, key, text, NULL);
    return xstrdup(s ? s->tag : "");
}

void hl_store_invalidate(void *store_v, const char *key) {
    edit_store_t *st = store_v;
    pthread_mutex_lock(&st->mu);
    size_t w = 0;
    for (size_t i = 0; i < st->snaps.n; i++) {
        snapshot_t *x = st->snaps.v[i];
        if (!strcmp(x->path, key)) {
            free(x->path);
            free(x->tag);
            free(x->content);
            vec_free(&x->seen);
            free(x);
        } else {
            st->snaps.v[w++] = x;
        }
    }
    st->snaps.n = w;
    pthread_mutex_unlock(&st->mu);
}

/* ^[ *]?(\d+)(?:-(\d+))?: */
static void seen_lines_from_body(const char *body, u32v_t *out) {
    strvec_t rows = split_nl(body);
    for (size_t i = 0; i < rows.n; i++) {
        const char *p = rows.v[i];
        if (*p == ' ' || *p == '*') p++;
        if (!isdigit((unsigned char)*p)) continue;
        char *e;
        unsigned long a = strtoul(p, &e, 10);
        unsigned long b = 0;
        bool hb = false;
        if (*e == '-' && isdigit((unsigned char)e[1])) {
            b = strtoul(e + 1, &e, 10);
            hb = true;
        }
        if (*e != ':') continue;
        if (a <= UINT32_MAX) vec_push(out, (uint32_t)a);
        if (hb && b <= UINT32_MAX) vec_push(out, (uint32_t)b);
    }
    strvec_free(&rows);
}

/* unshifted equal runs of before -> after (LCS over lines), filtered by the prior snapshot's seen set */
static void carried_seen_lines(const char *before, const char *after, const snapshot_t *prior, u32v_t *out) {
    strvec_t a = split_nl(before), b = split_nl(after);
    /* common prefix/suffix: the leading run is always unshifted; the trailing run is when line counts match */
    size_t pre = 0;
    while (pre < a.n && pre < b.n && !strcmp(a.v[pre], b.v[pre])) pre++;
    size_t suf = 0;
    while (suf < a.n - pre && suf < b.n - pre && !strcmp(a.v[a.n - 1 - suf], b.v[b.n - 1 - suf])) suf++;
    bool restricted = prior && snap_restricted(prior);
    for (size_t l = 1; l <= pre; l++)
        if (!restricted || snap_seen(prior, (uint32_t)l)) vec_push(out, (uint32_t)l);
    if (a.n == b.n)
        for (size_t l = a.n - suf + 1; l <= a.n; l++)
            if (l > pre && (!restricted || snap_seen(prior, (uint32_t)l))) vec_push(out, (uint32_t)l);
    strvec_free(&a);
    strvec_free(&b);
}

char *hl_record_after(void *store_v, const char *before_key, const char *key, const char *before, const char *after,
                      const char *body, bool track) {
    edit_store_t *st = store_v;
    u32v_t carried = {0};
    if (track) {
        pthread_mutex_lock(&st->mu);
        snapshot_t *prior = store_by_content(st, before_key, before);
        carried_seen_lines(before, after, prior, &carried);
        pthread_mutex_unlock(&st->mu);
        if (body) seen_lines_from_body(body, &carried);
    }
    pthread_mutex_lock(&st->mu);
    bool existed = store_by_content(st, key, after) != NULL;
    pthread_mutex_unlock(&st->mu);
    snapshot_t *s = store_record(st, key, after, NULL);
    char *tag = xstrdup(s ? s->tag : "");
    if (s && track && carried.n) {
        pthread_mutex_lock(&st->mu);
        if (!existed) s->seen.n = 0;
        if (!existed || snap_restricted(s)) snap_add_lines(s, &carried);
        pthread_mutex_unlock(&st->mu);
    }
    vec_free(&carried);
    return tag;
}
/* ---------- edit store ---------- */

void edit_store_init(edit_store_t *s) {
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mu, NULL);
    hmap_init(&s->registers);
    hmap_init(&s->noop);
}

static void snap_free(snapshot_t *sn) {
    free(sn->path);
    free(sn->tag);
    free(sn->content);
    vec_free(&sn->seen);
    free(sn);
}

void edit_store_free(edit_store_t *s) {
    for (size_t i = 0; i < s->snaps.n; i++) snap_free(s->snaps.v[i]);
    vec_free(&s->snaps);
    hmap_free(&s->registers, free);
    hmap_free(&s->noop, free);
    pthread_mutex_destroy(&s->mu);
}

char *file_tag(const char *text, size_t len) {
    /* normalize: strip trailing [ \t\r] on every line */
    sb_t n = SB_INIT;
    sb_reserve(&n, len);
    size_t i = 0;
    while (i < len) {
        const char *nl = memchr(text + i, '\n', len - i);
        size_t end = nl ? (size_t)(nl - text) : len;
        size_t e = end;
        while (e > i && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) e--;
        sb_appendn(&n, text + i, e - i);
        if (nl) sb_putc(&n, '\n');
        i = nl ? end + 1 : len;
    }
    uint32_t h = hash_xx32(n.s ? n.s : "", n.len, 0) & 0xffff;
    sb_free(&n);
    return xasprintf("%04X", h);
}

#define MAX_SNAPSHOTS 256

const char *edit_store_record(edit_store_t *s, const char *abs_path, const char *content, int64_t lo, int64_t hi) {
    /* the snapshot store hashes and keeps the BOM-stripped text, what `Bun.file(path).text()` returns (read.ts
     * deriveBufferedFileText strippedText); the patcher reads the live file the same way */
    if (str_starts(content, "\xEF\xBB\xBF")) content += 3;
    char *tag = file_tag(content, strlen(content));
    pthread_mutex_lock(&s->mu);
    snapshot_t *sn = NULL;
    for (size_t i = 0; i < s->snaps.n; i++) {
        snapshot_t *x = s->snaps.v[i];
        if (strcmp(x->path, abs_path) == 0 && strcmp(x->tag, tag) == 0) {
            sn = x;
            /* move to end (most recent) */
            memmove(s->snaps.v + i, s->snaps.v + i + 1, (s->snaps.n - i - 1) * sizeof(void *));
            s->snaps.v[s->snaps.n - 1] = sn;
            break;
        }
    }
    if (!sn) {
        sn = xcalloc(1, sizeof(*sn));
        sn->path = xstrdup(abs_path);
        sn->tag = xstrdup(tag);
        sn->content = xstrdup(content);
        if (s->snaps.n >= MAX_SNAPSHOTS) {
            snap_free(s->snaps.v[0]);
            memmove(s->snaps.v, s->snaps.v + 1, (s->snaps.n - 1) * sizeof(void *));
            s->snaps.n--;
        }
        vec_push(&s->snaps, sn);
    }
    if (lo > 0 && hi >= lo) {
        line_span_t r = {lo, hi};
        vec_push(&sn->seen, r);
    } else if (lo == 0) {
        /* unrestricted view (whole file shown) */
        sn->seen.n = 0;
        line_span_t r = {1, INT64_MAX};
        vec_push(&sn->seen, r);
    }
    pthread_mutex_unlock(&s->mu);
    free(tag);
    return sn->tag;
}

snapshot_t *edit_store_find(edit_store_t *s, const char *abs_path, const char *tag) {
    snapshot_t *r = NULL;
    pthread_mutex_lock(&s->mu);
    for (size_t i = s->snaps.n; i > 0; i--) {
        snapshot_t *x = s->snaps.v[i - 1];
        if (strcmp(x->path, abs_path) == 0 && (!tag || strcasecmp(x->tag, tag) == 0)) {
            r = x;
            break;
        }
    }
    pthread_mutex_unlock(&s->mu);
    return r;
}


/* ---------- write-content prefix stripping (prefixes.rs strip_hashline_prefixes + write-content.ts) ---------- */

/* HL_HEADER_RE: ^\s*\[[^#\r\n]+#[0-9a-fA-F]{4}\]\s*$ */
static bool hl_header_row(const char *line) {
    const char *p = skip_ws(line);
    if (*p != '[') return false;
    p++;
    const char *start = p;
    while (*p && *p != '#' && *p != '\r' && *p != '\n') p++;
    if (p == start || *p != '#') return false;
    p++;
    for (int i = 0; i < 4; i++, p++)
        if (!isxdigit((unsigned char)*p)) return false;
    if (*p != ']') return false;
    return *skip_ws(p + 1) == 0;
}

static bool numbers_count_up_by_one(char *const *L, size_t n) {
    int64_t prev = -1;
    for (size_t i = 0; i < n; i++) {
        const char *l = L[i];
        if (!*l || is_read_metadata_line(l) || hl_header_row(l)) {
            prev = -1;
            continue;
        }
        size_t k = hl_prefix_len(l);
        if (!k) return false;
        const char *d = l + k - 1;
        while (d > l && isdigit((unsigned char)d[-1])) d--;
        uint64_t num = strtoull(d, NULL, 10);
        if (prev >= 0 && (uint64_t)prev + 1 != num) return false;
        prev = (int64_t)num;
    }
    return true;
}

/* NULL when unchanged */
static char *strip_lines_joined(char *const *L, size_t n, const char *text) {
    size_t non_empty = 0, headers = 0, prefixed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!*L[i] || is_read_metadata_line(L[i])) continue;
        non_empty++;
        if (hl_header_row(L[i])) {
            headers++;
            continue;
        }
        if (hl_prefix_len(L[i])) prefixed++;
    }
    if (!non_empty) return NULL;
    size_t content = non_empty - headers;
    if (!content || prefixed != content || !numbers_count_up_by_one(L, n)) return NULL;
    sb_t b = SB_INIT;
    bool first = true, changed = false;
    for (size_t i = 0; i < n; i++) {
        if (is_read_metadata_line(L[i]) || hl_header_row(L[i])) {
            changed = true;
            continue;
        }
        const char *s = L[i];
        size_t k;
        while ((k = hl_prefix_len(s)) > 0) s += k;
        if (s != L[i]) changed = true;
        if (!first) sb_putc(&b, '\n');
        sb_append(&b, s);
        first = false;
    }
    if (!changed) {
        sb_free(&b);
        return NULL;
    }
    char *r = sb_detach(&b);
    if (text && !strcmp(r, text)) {
        free(r);
        return NULL;
    }
    return r;
}

/* LOOSE_HASHLINE_HEADER_RE: ^\s*\[[^#\r\n]+#[^ \t\r\n]*\]\s*$ */
static bool loose_header_row(const char *line) {
    const char *p = skip_ws(line);
    if (*p != '[') return false;
    p++;
    const char *start = p;
    while (*p && *p != '#' && *p != '\r' && *p != '\n') p++;
    if (p == start || *p != '#') return false;
    p++;
    const char *tag = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    /* the greedy tag run must end with `]`, followed only by whitespace */
    return p > tag && p[-1] == ']' && *skip_ws(p) == 0;
}

char *hl_strip_write_content(const char *content, bool *stripped) {
    *stripped = false;
    /* PLAIN_FIRST_ROW_RE fast path: the first non-empty row starts with a plain content character */
    const char *p = content;
    while (*p == '\n') p++;
    size_t k;
    while (*p != '\n' && (k = ws_at(p)) > 0) p += k;
    bool plain = *p && *p != '\n' && !ws_at(p) && !strchr("[>+*-.", *p) && !isdigit((unsigned char)*p) &&
                 !starts(p, "\xE2\x80\xA6");
    if (plain) {
        uint32_t cp;
        int n = utf8_decode(p, strlen(p), &cp);
        if (n > 1 && ((cp >= 0x660 && cp <= 0x669) || (cp >= 0xFF10 && cp <= 0xFF19))) plain = false;
    }
    if (plain) return xstrdup(content);
    strvec_t L = split_nl(content);
    char *r = strip_lines_joined(L.v, L.n, content);
    if (r) {
        strvec_free(&L);
        *stripped = true;
        return r;
    }
    size_t hi = 0;
    while (hi < L.n && blank(L.v[hi])) hi++;
    if (hi == L.n || !loose_header_row(L.v[hi])) {
        strvec_free(&L);
        return xstrdup(content);
    }
    free(L.v[hi]);
    memmove(L.v + hi, L.v + hi + 1, (L.n - hi - 1) * sizeof(char *));
    L.n--;
    r = strip_lines_joined(L.v, L.n, NULL);
    strvec_free(&L);
    if (!r) return xstrdup(content);
    *stripped = true;
    return r;
}
