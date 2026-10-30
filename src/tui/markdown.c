/* markdown.c - markdown renderer and syntax highlighter */
#include "tui/markdown.h"

#include <ctype.h>

#define MD_MAX_DEPTH 8
#define MD_MAX_COLS 32
#define BOX_H "\xe2\x94\x80"
#define BOX_V "\xe2\x94\x82"

typedef struct {
    const tui_theme_t *th;
    const tui_md_opts_t *o;
    char base[512]; /* SGR prefix re-applied after every reset */
} md_ctx_t;

/* dst->base = src->base + a + b (+ c); keeps src->base unchanged if it would not fit */
static void ctx_extend(md_ctx_t *dst, const md_ctx_t *src, const char *a, const char *b, const char *c) {
    *dst = *src;
    size_t n = strlen(src->base), la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (n + la + lb + lc >= sizeof dst->base) return;
    memcpy(dst->base + n, a, la);
    memcpy(dst->base + n + la, b, lb);
    memcpy(dst->base + n + la + lb, c, lc);
    dst->base[n + la + lb + lc] = 0;
}

void tui_md_opts_default(tui_md_opts_t *o) {
    memset(o, 0, sizeof *o);
    o->hyperlinks = true;
    o->code_indent = 2;
    o->text_slot = -1;
}

/* ====================================================================== */
/* syntax highlighting                                                     */
/* ====================================================================== */

typedef struct {
    const char *names; /* space separated aliases, padded with spaces */
    const char *keywords;
    const char *types;
    const char *lc1, *lc2;       /* line comment starters */
    const char *bc_open, *bc_close;
    bool backtick;               /* `...` strings (multi-line) */
    bool triple;                 /* """ / ''' strings (multi-line) */
    bool upper_types;            /* Capitalized identifiers are types */
    bool single_quote_str;       /* ' starts a string (else char/lifetime handling) */
} lang_t;

static const lang_t LANGS[] = {
    {" c h cpp c++ cc cxx hpp hh objc objective-c cuda ",
     " if else for while do switch case default break continue return goto sizeof typedef struct union enum static "
     "extern const volatile register inline restrict auto signed unsigned class namespace template typename public "
     "private protected virtual override final new delete this throw try catch using operator friend constexpr "
     "nullptr true false NULL include define ifdef ifndef endif pragma noexcept decltype static_cast "
     "reinterpret_cast const_cast dynamic_cast ",
     " int char short long float double void bool size_t ssize_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t "
     "int32_t int64_t uintptr_t intptr_t FILE std string vector map ",
     "//", NULL, "/*", "*/", false, false, false, true},
    {" js javascript jsx mjs cjs ts typescript tsx node ",
     " if else for while do switch case default break continue return function var let const class extends new "
     "delete typeof instanceof in of this super throw try catch finally async await yield import export from as "
     "default null undefined true false void static get set interface type enum implements namespace declare "
     "readonly abstract keyof satisfies ",
     " string number boolean any unknown never object symbol bigint Promise Array Map Set Record ",
     "//", NULL, "/*", "*/", true, false, true, true},
    {" python py py3 python3 ",
     " if elif else for while break continue return def class lambda try except finally raise with as import from "
     "global nonlocal pass yield assert del in is not and or None True False async await match case self ",
     " int str float bool list dict set tuple bytes object type ",
     "#", NULL, NULL, NULL, false, true, true, true},
    {" rust rs ",
     " if else for while loop break continue return fn let mut const static struct enum impl trait type where pub "
     "use mod crate self Self super match as in ref move unsafe async await dyn true false extern ",
     " i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str String Vec Option Result Box ",
     "//", NULL, "/*", "*/", false, false, true, false},
    {" go golang ",
     " if else for range break continue return func var const type struct interface map chan go defer select switch "
     "case default package import fallthrough goto nil true false iota ",
     " int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 float32 float64 string bool byte rune error any ",
     "//", NULL, "/*", "*/", true, false, false, false},
    {" java kotlin kt scala swift cs csharp c# dart ",
     " if else for while do switch case default break continue return class interface extends implements new this "
     "super throw throws try catch finally import package public private protected static final abstract "
     "synchronized void null true false var val fun let func struct override when is in as object namespace using ",
     " int long short byte float double boolean char String Integer Object List Map ",
     "//", NULL, "/*", "*/", false, true, true, true},
    {" sh bash zsh shell console shellscript fish ksh ",
     " if then else elif fi for while until do done case esac in function return break continue local export "
     "readonly declare unset echo exit source alias set shift trap eval exec cd test ",
     "", "#", NULL, NULL, NULL, false, false, true, false},
    {" json jsonc json5 ", " true false null ", "", "//", NULL, NULL, NULL, false, false, false, false},
    {" yaml yml toml ini ", " true false null yes no on off ", "", "#", NULL, NULL, NULL, false, false, true, false},
    {" sql postgres postgresql mysql sqlite ",
     " select from where insert into values update set delete create table drop alter index join left right inner "
     "outer on group by order having limit offset as and or not null is in like between distinct union all case "
     "when then else end primary key foreign references default exists SELECT FROM WHERE INSERT INTO VALUES "
     "UPDATE SET DELETE CREATE TABLE DROP ALTER INDEX JOIN LEFT RIGHT INNER OUTER ON GROUP BY ORDER HAVING LIMIT "
     "OFFSET AS AND OR NOT NULL IS IN LIKE BETWEEN DISTINCT UNION ALL CASE WHEN THEN ELSE END PRIMARY KEY ",
     " int integer text varchar char boolean float real date timestamp serial bigint INT INTEGER TEXT VARCHAR ",
     "--", NULL, "/*", "*/", false, false, true, false},
    {" ruby rb ",
     " if elsif else unless case when while until for in do end def class module return yield begin rescue ensure "
     "raise break next redo retry self super nil true false and or not then require attr_accessor lambda proc ",
     "", "#", NULL, NULL, NULL, false, false, true, true},
    {" lua ",
     " if then else elseif end for while do repeat until function local return break in and or not nil true false "
     "goto ",
     "", "--", NULL, NULL, NULL, false, false, true, false},
    {" php ",
     " if else elseif for foreach while do switch case default break continue return function class new public "
     "private protected static echo namespace use null true false array fn match ",
     "", "//", "#", "/*", "*/", false, false, true, true},
};

static const lang_t *find_lang(const char *lang) {
    if (!lang || !*lang) return NULL;
    char key[40];
    snprintf(key, sizeof key, " %s ", lang);
    for (char *p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (size_t i = 0; i < ARRAY_LEN(LANGS); i++)
        if (strstr(LANGS[i].names, key)) return &LANGS[i];
    return NULL;
}

bool tui_highlight_supported(const char *lang) {
    if (lang && (str_ieq(lang, "diff") || str_ieq(lang, "patch"))) return true;
    return find_lang(lang) != NULL;
}

static bool word_in(const char *list, const char *w, size_t n) {
    if (!list || n == 0 || n > 30) return false;
    char key[34];
    key[0] = ' ';
    memcpy(key + 1, w, n);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    return strstr(list, key) != NULL;
}

static void colored(sb_t *b, const tui_theme_t *th, tui_tc_t slot, const char *s, size_t n) {
    sb_append(b, tui_fg_seq(th, slot));
    sb_appendn(b, s, n);
    sb_append(b, tui_fg_seq(th, TUI_TC_MD_CODE_BLOCK));
}

typedef struct {
    int mode; /* 0 normal, 1 block comment, 2 multi-line string */
    char delim[4];
} hl_state_t;

static void highlight_line(const lang_t *L, const char *s, size_t n, hl_state_t *st, const tui_theme_t *th, sb_t *b) {
    sb_append(b, tui_fg_seq(th, TUI_TC_MD_CODE_BLOCK));
    size_t i = 0;
    while (i < n) {
        if (st->mode == 1) {
            const char *e = L->bc_close ? strstr(s + i, L->bc_close) : NULL;
            size_t end = (e && (size_t)(e - s) < n) ? (size_t)(e - s) + strlen(L->bc_close) : n;
            colored(b, th, TUI_TC_SYNTAX_COMMENT, s + i, end - i);
            if (end < n || (e && (size_t)(e - s) < n)) st->mode = 0;
            i = end;
            continue;
        }
        if (st->mode == 2) {
            size_t dl = strlen(st->delim);
            size_t j = i;
            bool closed = false;
            while (j < n) {
                if (s[j] == '\\' && j + 1 < n) {
                    j += 2;
                    continue;
                }
                if (j + dl <= n && !memcmp(s + j, st->delim, dl)) {
                    j += dl;
                    closed = true;
                    break;
                }
                j++;
            }
            colored(b, th, TUI_TC_SYNTAX_STRING, s + i, j - i);
            if (closed) st->mode = 0;
            i = j;
            continue;
        }
        char c = s[i];
        /* comments */
        const char *lcs[2] = {L->lc1, L->lc2};
        bool done = false;
        for (int k = 0; k < 2 && !done; k++) {
            if (lcs[k] && !strncmp(s + i, lcs[k], strlen(lcs[k])) &&
                !(lcs[k][0] == '#' && i > 0 && s[i - 1] == '$')) {
                colored(b, th, TUI_TC_SYNTAX_COMMENT, s + i, n - i);
                i = n;
                done = true;
            }
        }
        if (done) break;
        if (L->bc_open && !strncmp(s + i, L->bc_open, strlen(L->bc_open))) {
            st->mode = 1;
            size_t ol = strlen(L->bc_open);
            colored(b, th, TUI_TC_SYNTAX_COMMENT, s + i, ol);
            i += ol;
            continue;
        }
        /* strings */
        if (L->triple && (c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            st->mode = 2;
            st->delim[0] = st->delim[1] = st->delim[2] = c;
            st->delim[3] = 0;
            colored(b, th, TUI_TC_SYNTAX_STRING, s + i, 3);
            i += 3;
            continue;
        }
        if (c == '`' && L->backtick) {
            st->mode = 2;
            st->delim[0] = '`';
            st->delim[1] = 0;
            colored(b, th, TUI_TC_SYNTAX_STRING, s + i, 1);
            i++;
            continue;
        }
        if (c == '"' || (c == '\'' && L->single_quote_str) || (c == '\'' && !L->single_quote_str && i + 2 < n &&
                                                                (s[i + 2] == '\'' || (s[i + 1] == '\\' && i + 3 < n)))) {
            size_t j = i + 1;
            while (j < n && s[j] != c) {
                if (s[j] == '\\' && j + 1 < n) j++;
                j++;
            }
            if (j < n) j++;
            colored(b, th, TUI_TC_SYNTAX_STRING, s + i, j - i);
            i = j;
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]) &&
                                          (i == 0 || !isalnum((unsigned char)s[i - 1])))) {
            if (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_')) {
                sb_putc(b, c);
                i++;
                continue;
            }
            size_t j = i;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
            colored(b, th, TUI_TC_SYNTAX_NUMBER, s + i, j - i);
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_' || c == '$' || c == '@') {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '$' || (s[j] == '-' && L == &LANGS[6])))
                j++;
            size_t wl = j - i;
            size_t k = j;
            while (k < n && s[k] == ' ') k++;
            if (word_in(L->keywords, s + i, wl)) colored(b, th, TUI_TC_SYNTAX_KEYWORD, s + i, wl);
            else if (word_in(L->types, s + i, wl) || (L->upper_types && isupper((unsigned char)c) && wl > 1))
                colored(b, th, TUI_TC_SYNTAX_TYPE, s + i, wl);
            else if (k < n && s[k] == '(') colored(b, th, TUI_TC_SYNTAX_FUNCTION, s + i, wl);
            else if (L == &LANGS[7] && k < n && s[k] == ':') colored(b, th, TUI_TC_SYNTAX_VARIABLE, s + i, wl);
            else sb_appendn(b, s + i, wl);
            i = j;
            continue;
        }
        if (strchr("+-*/%=<>!&|^~?:", c)) {
            colored(b, th, TUI_TC_SYNTAX_OPERATOR, s + i, 1);
            i++;
            continue;
        }
        if (strchr("(){}[];,.", c)) {
            colored(b, th, TUI_TC_SYNTAX_PUNCTUATION, s + i, 1);
            i++;
            continue;
        }
        /* copy one UTF-8 char */
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        sb_appendn(b, s + i, (size_t)k);
        i += (size_t)k;
    }
    sb_append(b, "\x1b[39m");
}

void tui_highlight_code(const char *code, const char *lang, const tui_theme_t *th, strvec_t *out) {
    if (!th) th = tui_theme();
    char *clean = tui_replace_tabs(code ? code : "");
    strvec_t lines = str_split_lines(clean, strlen(clean));
    if (!lines.n) strvec_push(&lines, "");
    bool diff = lang && (str_ieq(lang, "diff") || str_ieq(lang, "patch"));
    const lang_t *L = diff ? NULL : find_lang(lang);
    hl_state_t st;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < lines.n; i++) {
        const char *s = lines.v[i];
        sb_t b = SB_INIT;
        /* drop control chars that could break layout */
        char *safe = NULL;
        for (const char *p = s; *p; p++)
            if ((unsigned char)*p < 0x20) {
                safe = xstrdup(s);
                for (char *q = safe; *q; q++)
                    if ((unsigned char)*q < 0x20) *q = ' ';
                s = safe;
                break;
            }
        if (diff) {
            tui_tc_t slot = TUI_TC_MD_CODE_BLOCK;
            if (s[0] == '+' && !str_starts(s, "+++")) slot = TUI_TC_TOOL_DIFF_ADDED;
            else if (s[0] == '-' && !str_starts(s, "---")) slot = TUI_TC_TOOL_DIFF_REMOVED;
            else if (str_starts(s, "@@")) slot = TUI_TC_ACCENT;
            else slot = TUI_TC_TOOL_DIFF_CONTEXT;
            tui_fg(&b, th, slot, s);
        } else if (L) {
            highlight_line(L, s, strlen(s), &st, th, &b);
        } else {
            tui_fg(&b, th, TUI_TC_MD_CODE_BLOCK, s);
        }
        free(safe);
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&lines);
    free(clean);
}

/* ====================================================================== */
/* inline                                                                  */
/* ====================================================================== */

typedef struct {
    bool bold, italic, strike;
} istyle_t;

static void emit_style(sb_t *b, const md_ctx_t *c, const istyle_t *s) {
    sb_append(b, TUI_RESET);
    sb_append(b, c->base);
    if (s->bold) sb_append(b, "\x1b[1m");
    if (s->italic) sb_append(b, "\x1b[3m");
    if (s->strike) sb_append(b, "\x1b[9m");
}

static bool is_punct(char c) { return c && ispunct((unsigned char)c); }

/* Is there a closing run of `ch` with length >= need after position i? */
static bool has_closer(const char *s, size_t i, size_t n, char ch, size_t need) {
    for (size_t j = i; j < n; j++) {
        if (s[j] == '\\') {
            j++;
            continue;
        }
        if (s[j] == '`') { /* skip code spans */
            size_t r = 0;
            while (j + r < n && s[j + r] == '`') r++;
            size_t k = j + r;
            bool found = false;
            while (k < n) {
                if (s[k] == '`') {
                    size_t r2 = 0;
                    while (k + r2 < n && s[k + r2] == '`') r2++;
                    if (r2 == r) {
                        found = true;
                        j = k + r2 - 1;
                        break;
                    }
                    k += r2;
                } else k++;
            }
            if (!found) j += r - 1;
            continue;
        }
        if (s[j] == ch) {
            size_t r = 0;
            while (j + r < n && s[j + r] == ch) r++;
            if (r >= need && j > 0 && s[j - 1] != ' ' && s[j - 1] != '\t' && s[j - 1] != '\n') {
                if (ch == '_' && j + r < n && isalnum((unsigned char)s[j + r])) {
                    j += r - 1;
                    continue;
                }
                return true;
            }
            j += r - 1;
        }
    }
    return false;
}

static void render_inline(const char *s, size_t n, const md_ctx_t *c, sb_t *b, int depth);

/* Negative-result caches keep delimiter scanning linear on adversarial input: once no
 * closer exists after position p, none exists after any later position either. */
typedef struct {
    size_t no_closer[3][2]; /* [* _ ~][need-1] -> smallest start known to have no closer */
    size_t no_backtick[9];  /* [run length] -> smallest start known to have no closing run */
} scan_cache_t;

static bool closer_cached(scan_cache_t *sc, const char *s, size_t i, size_t n, char ch, size_t need) {
    int ci = ch == '*' ? 0 : ch == '_' ? 1 : 2;
    size_t *slot = &sc->no_closer[ci][need >= 2 ? 1 : 0];
    if (i >= *slot) return false;
    bool r = has_closer(s, i, n, ch, need);
    if (!r && i < *slot) *slot = i;
    return r;
}

static void emit_link(const char *text, size_t tn, const char *url, size_t un, const md_ctx_t *c, sb_t *b,
                      istyle_t *st, int depth) {
    char *u = xstrndup(url, un);
    md_ctx_t lc;
    ctx_extend(&lc, c, tui_fg_seq(c->th, TUI_TC_MD_LINK), "\x1b[4m", "");
    bool hl = c->o->hyperlinks;
    if (hl) {
        sb_append(b, "\x1b]8;;");
        for (const char *p = u; *p; p++)
            if ((unsigned char)*p >= 0x20 && *p != 0x7f) sb_putc(b, *p);
        sb_append(b, "\a");
    }
    istyle_t ls = *st;
    emit_style(b, &lc, &ls);
    if (depth < 3) render_inline(text, tn, &lc, b, depth + 1);
    else sb_appendn(b, text, tn);
    if (hl) sb_append(b, TUI_LINK_CLOSE);
    emit_style(b, c, st);
    bool same = tn == un && !memcmp(text, url, un);
    if (str_starts(u, "mailto:") && tn == un - 7 && !memcmp(text, u + 7, tn)) same = true;
    if (!hl && !same) {
        sb_append(b, tui_fg_seq(c->th, TUI_TC_MD_LINK_URL));
        sb_append(b, " (");
        sb_append(b, u);
        sb_append(b, ")");
        emit_style(b, c, st);
    }
    free(u);
}

static size_t url_end(const char *s, size_t i, size_t n) {
    size_t j = i;
    int paren = 0;
    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '<' && s[j] != '>' && s[j] != '"' && s[j] != '`') {
        if (s[j] == '(') paren++;
        else if (s[j] == ')') {
            if (paren == 0) break;
            paren--;
        }
        j++;
    }
    while (j > i && strchr(".,;:!?'*_~", s[j - 1])) j--;
    return j;
}

static void render_inline(const char *s, size_t n, const md_ctx_t *c, sb_t *b, int depth) {
    istyle_t st = {0};
    scan_cache_t sc;
    memset(&sc, 0xff, sizeof sc);
    emit_style(b, c, &st);
    size_t i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n && is_punct(s[i + 1])) {
            sb_putc(b, s[i + 1]);
            i += 2;
            continue;
        }
        if (ch == '\n') {
            sb_putc(b, '\n');
            i++;
            continue;
        }
        if (ch == '`') {
            size_t r = 0;
            while (i + r < n && s[i + r] == '`') r++;
            size_t k = i + r;
            size_t close = 0;
            bool found = false;
            size_t *bt = &sc.no_backtick[MIN(r, (size_t)8)];
            if (r < 8 && i >= *bt) k = n; /* known: no closing run */
            while (k < n) {
                if (s[k] == '`') {
                    size_t r2 = 0;
                    while (k + r2 < n && s[k + r2] == '`') r2++;
                    if (r2 == r) {
                        close = k;
                        found = true;
                        break;
                    }
                    k += r2;
                } else k++;
            }
            if (!found) {
                if (r < 8 && i < *bt) *bt = i;
                sb_appendn(b, s + i, r);
                i += r;
                continue;
            }
            const char *cs = s + i + r;
            size_t cl = close - (i + r);
            if (cl >= 2 && cs[0] == ' ' && cs[cl - 1] == ' ') {
                cs++;
                cl -= 2;
            }
            sb_append(b, tui_fg_seq(c->th, TUI_TC_MD_CODE));
            for (size_t q = 0; q < cl; q++) sb_putc(b, cs[q] == '\n' ? ' ' : cs[q]);
            emit_style(b, c, &st);
            i = close + r;
            continue;
        }
        if (ch == '*' || ch == '_') {
            size_t r = 0;
            while (i + r < n && s[i + r] == ch) r++;
            char prev = i > 0 ? s[i - 1] : ' ';
            char next = i + r < n ? s[i + r] : ' ';
            bool can_open = !isspace((unsigned char)next);
            bool can_close = !isspace((unsigned char)prev);
            if (ch == '_' && isalnum((unsigned char)prev) && isalnum((unsigned char)next)) {
                sb_appendn(b, s + i, r);
                i += r;
                continue;
            }
            size_t used = 0;
            size_t rem = r;
            bool changed = false;
            if (rem >= 2) {
                if (st.bold && can_close) {
                    st.bold = false;
                    rem -= 2;
                    used += 2;
                    changed = true;
                } else if (!st.bold && can_open && closer_cached(&sc, s, i + r, n, ch, 2)) {
                    st.bold = true;
                    rem -= 2;
                    used += 2;
                    changed = true;
                }
            }
            if (rem >= 1) {
                if (st.italic && can_close) {
                    st.italic = false;
                    rem--;
                    used++;
                    changed = true;
                } else if (!st.italic && can_open && closer_cached(&sc, s, i + r, n, ch, 1)) {
                    st.italic = true;
                    rem--;
                    used++;
                    changed = true;
                }
            }
            if (changed) emit_style(b, c, &st);
            if (rem) sb_appendn(b, s + i + used, rem);
            i += r;
            continue;
        }
        if (ch == '~' && i + 1 < n && s[i + 1] == '~') {
            char next = i + 2 < n ? s[i + 2] : ' ';
            char prev = i > 0 ? s[i - 1] : ' ';
            if (st.strike && !isspace((unsigned char)prev)) {
                st.strike = false;
                emit_style(b, c, &st);
                i += 2;
                continue;
            }
            if (!st.strike && !isspace((unsigned char)next) && closer_cached(&sc, s, i + 2, n, '~', 2)) {
                st.strike = true;
                emit_style(b, c, &st);
                i += 2;
                continue;
            }
        }
        if (ch == '[' || (ch == '!' && i + 1 < n && s[i + 1] == '[')) {
            size_t ts = i + (ch == '!' ? 2 : 1);
            int dep = 0;
            size_t j = ts;
            for (; j < n && j - ts < 2000; j++) {
                if (s[j] == '\\') {
                    j++;
                    continue;
                }
                if (s[j] == '[') dep++;
                else if (s[j] == ']') {
                    if (dep == 0) break;
                    dep--;
                } else if (s[j] == '\n' && j + 1 < n && s[j + 1] == '\n') break;
            }
            if (j < n && s[j] == ']' && j + 1 < n && s[j + 1] == '(') {
                size_t us = j + 2, k = us;
                int pd = 0;
                while (k < n && k - us < 4000) {
                    if (s[k] == '(') pd++;
                    else if (s[k] == ')') {
                        if (pd == 0) break;
                        pd--;
                    } else if (s[k] == '\n') break;
                    k++;
                }
                if (k < n && s[k] == ')') {
                    size_t ue = us;
                    while (ue < k && s[ue] != ' ') ue++;
                    const char *url = s + us;
                    size_t ul = ue - us;
                    if (ul >= 2 && url[0] == '<' && url[ul - 1] == '>') {
                        url++;
                        ul -= 2;
                    }
                    emit_link(s + ts, j - ts, url, ul, c, b, &st, depth);
                    i = k + 1;
                    continue;
                }
            }
        }
        if (ch == '<') {
            size_t j = i + 1;
            while (j < n && j - i < 2000 && s[j] != '>' && s[j] != ' ' && s[j] != '\n' && s[j] != '<') j++;
            if (j < n && s[j] == '>') {
                const char *u = s + i + 1;
                size_t ul = j - i - 1;
                if ((ul > 7 && (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8))) ||
                    (ul > 7 && !strncmp(u, "mailto:", 7))) {
                    emit_link(u, ul, u, ul, c, b, &st, depth);
                    i = j + 1;
                    continue;
                }
            }
        }
        if ((ch == 'h' && (i == 0 || !isalnum((unsigned char)s[i - 1])) &&
             (!strncmp(s + i, "http://", MIN(7, n - i)) || !strncmp(s + i, "https://", MIN(8, n - i))) && n - i > 8)) {
            size_t e = url_end(s, i, n);
            if (e > i + 8) {
                emit_link(s + i, e - i, s + i, e - i, c, b, &st, depth);
                i = e;
                continue;
            }
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        if (cp < 0x20 && cp != '\t') sb_putc(b, ' ');
        else sb_appendn(b, s + i, (size_t)k);
        i += (size_t)k;
    }
    sb_append(b, TUI_RESET);
}

/* ====================================================================== */
/* blocks                                                                  */
/* ====================================================================== */

typedef enum { B_NONE, B_PARA, B_HEADING, B_CODE, B_LIST, B_QUOTE, B_TABLE, B_HR } btype_t;

typedef struct {
    const char *const *v;
    size_t n;
} lines_t;

static size_t indent_of(const char *s) {
    size_t n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += *s == '\t' ? 4 : 1;
    return n;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool is_blank(const char *s) { return *skip_ws(s) == 0; }

static bool is_fence(const char *s, char *fch, size_t *flen, const char **info) {
    if (indent_of(s) > 3) return false;
    s = skip_ws(s);
    if (*s != '`' && *s != '~') return false;
    char c = *s;
    size_t n = 0;
    while (s[n] == c) n++;
    if (n < 3) return false;
    if (c == '`' && strchr(s + n, '`')) return false;
    if (fch) *fch = c;
    if (flen) *flen = n;
    if (info) *info = skip_ws(s + n);
    return true;
}

static int heading_level(const char *s) {
    if (indent_of(s) > 3) return 0;
    s = skip_ws(s);
    int n = 0;
    while (s[n] == '#') n++;
    if (n < 1 || n > 6) return 0;
    if (s[n] && s[n] != ' ' && s[n] != '\t') return 0;
    return n;
}

static bool is_hr(const char *s) {
    if (indent_of(s) > 3) return false;
    s = skip_ws(s);
    char c = *s;
    if (c != '-' && c != '*' && c != '_') return false;
    int cnt = 0;
    for (; *s; s++) {
        if (*s == c) cnt++;
        else if (*s != ' ' && *s != '\t') return false;
    }
    return cnt >= 3;
}

static bool is_quote(const char *s) { return indent_of(s) <= 3 && *skip_ws(s) == '>'; }

/* list item: returns marker info; content offset in bytes */
static bool list_item(const char *s, size_t *indent, bool *ordered, long *num, size_t *content_off, size_t *marker_w) {
    size_t ind = indent_of(s);
    const char *p = skip_ws(s);
    const char *m = p;
    bool ord = false;
    long nv = 0;
    if (*p == '-' || *p == '*' || *p == '+') {
        p++;
    } else if (isdigit((unsigned char)*p)) {
        int d = 0;
        while (isdigit((unsigned char)*p) && d < 9) {
            nv = nv * 10 + (*p - '0');
            p++;
            d++;
        }
        if (*p != '.' && *p != ')') return false;
        p++;
        ord = true;
    } else {
        return false;
    }
    if (*p && *p != ' ' && *p != '\t') return false;
    if (!ord && is_hr(s)) return false;
    size_t mw = (size_t)(p - m);
    const char *c = p;
    if (*c == ' ' || *c == '\t') c++;
    if (indent) *indent = ind;
    if (ordered) *ordered = ord;
    if (num) *num = nv;
    if (content_off) *content_off = (size_t)(c - s);
    if (marker_w) *marker_w = mw;
    return true;
}

static bool is_table_sep(const char *s) {
    s = skip_ws(s);
    bool dash = false, pipe = false;
    for (; *s; s++) {
        if (*s == '-') dash = true;
        else if (*s == '|') pipe = true;
        else if (*s != ':' && *s != ' ' && *s != '\t') return false;
    }
    return dash && pipe;
}

static bool is_table_start(const lines_t *L, size_t i) {
    return i + 1 < L->n && strchr(L->v[i], '|') && is_table_sep(L->v[i + 1]);
}

static bool starts_block(const lines_t *L, size_t i) {
    const char *s = L->v[i];
    return is_fence(s, NULL, NULL, NULL) || heading_level(s) || is_hr(s) || is_quote(s) ||
           list_item(s, NULL, NULL, NULL, NULL, NULL) || is_table_start(L, i);
}

static void render_blocks(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, int depth,
                          strvec_t *out);

static void push_wrapped(strvec_t *out, const char *s, int width) { tui_wrap_text_with_ansi(s, width, out); }

static void render_paragraph(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, strvec_t *out) {
    sb_t src = SB_INIT;
    for (size_t i = from; i < to; i++) {
        const char *s = skip_ws(L->v[i]);
        size_t n = strlen(s);
        bool hard = false;
        if (n >= 2 && s[n - 1] == ' ' && s[n - 2] == ' ') hard = true;
        if (n >= 1 && s[n - 1] == '\\') {
            hard = true;
            n--;
        }
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
        if (i > from) sb_putc(&src, '\n');
        sb_appendn(&src, s, n);
        (void)hard;
    }
    sb_t b = SB_INIT;
    render_inline(sb_cstr(&src), src.len, c, &b, 0);
    push_wrapped(out, sb_cstr(&b), width);
    sb_free(&b);
    sb_free(&src);
}

static void render_heading(const char *s, int level, int width, const md_ctx_t *c, strvec_t *out) {
    s = skip_ws(s) + level;
    s = skip_ws(s);
    size_t n = strlen(s);
    /* strip closing #s */
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    size_t m = n;
    while (m > 0 && s[m - 1] == '#') m--;
    if (m < n && (m == 0 || s[m - 1] == ' ')) n = m;
    while (n > 0 && s[n - 1] == ' ') n--;
    md_ctx_t hc;
    ctx_extend(&hc, c, tui_fg_seq(c->th, TUI_TC_MD_HEADING), "\x1b[1m", level == 1 ? "\x1b[4m" : "");
    sb_t b = SB_INIT;
    if (level >= 3) {
        sb_append(&b, hc.base);
        for (int i = 0; i < level; i++) sb_putc(&b, '#');
        sb_putc(&b, ' ');
    }
    render_inline(s, n, &hc, &b, 0);
    push_wrapped(out, sb_cstr(&b), width);
    sb_free(&b);
}

static void render_code(const lines_t *L, size_t from, size_t to, const char *lang, size_t strip_indent, int width,
                        const md_ctx_t *c, strvec_t *out) {
    sb_t code = SB_INIT;
    for (size_t i = from; i < to; i++) {
        const char *s = L->v[i];
        size_t k = 0;
        while (k < strip_indent && s[k] == ' ') k++;
        if (i > from) sb_putc(&code, '\n');
        sb_append(&code, s + k);
    }
    sb_t b = SB_INIT;
    sb_append(&b, "```");
    sb_append(&b, lang);
    char *border = tui_fgs(c->th, TUI_TC_MD_CODE_BLOCK_BORDER, sb_cstr(&b));
    char *tb = tui_truncate_to_width(border, width, NULL);
    strvec_push_own(out, tb);
    free(border);
    sb_free(&b);
    strvec_t hl = {0};
    if (to > from) tui_highlight_code(sb_cstr(&code), lang, c->th, &hl);
    int ind = MAX(0, MIN(c->o->code_indent, width - 1));
    int inner = MAX(1, width - ind);
    for (size_t i = 0; i < hl.n; i++) {
        strvec_t wrapped = {0};
        if (tui_visible_width(hl.v[i]) <= inner) strvec_push(&wrapped, hl.v[i]);
        else {
            /* hard-wrap code by columns (no word wrap) */
            int total = tui_visible_width(hl.v[i]);
            for (int col = 0; col < total; col += inner) strvec_push_own(&wrapped, tui_slice_cols(hl.v[i], col, inner));
        }
        for (size_t j = 0; j < wrapped.n; j++) {
            sb_t l = SB_INIT;
            tui_sb_spaces(&l, ind);
            sb_append(&l, wrapped.v[j]);
            strvec_push_own(out, sb_detach(&l));
        }
        strvec_free(&wrapped);
    }
    strvec_free(&hl);
    strvec_push_own(out, tui_fgs(c->th, TUI_TC_MD_CODE_BLOCK_BORDER, "```"));
    sb_free(&code);
}

static void render_hr(int width, const md_ctx_t *c, strvec_t *out) {
    sb_t b = SB_INIT;
    sb_append(&b, tui_fg_seq(c->th, TUI_TC_MD_HR));
    sb_repeat(&b, BOX_H, MIN(width, 80));
    sb_append(&b, "\x1b[39m");
    strvec_push_own(out, sb_detach(&b));
}

static void render_quote(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, int depth,
                         strvec_t *out) {
    strvec_t inner_src = {0};
    for (size_t i = from; i < to; i++) {
        const char *s = skip_ws(L->v[i]);
        if (*s == '>') {
            s++;
            if (*s == ' ') s++;
        }
        strvec_push(&inner_src, s);
    }
    md_ctx_t qc;
    ctx_extend(&qc, c, tui_fg_seq(c->th, TUI_TC_MD_QUOTE), "\x1b[3m", "");
    strvec_t inner = {0};
    lines_t IL = {(const char *const *)inner_src.v, inner_src.n};
    int iw = MAX(1, width - 2);
    if (depth + 1 < MD_MAX_DEPTH) render_blocks(&IL, 0, IL.n, iw, &qc, depth + 1, &inner);
    else {
        for (size_t i = 0; i < inner_src.n; i++) push_wrapped(&inner, inner_src.v[i], iw);
    }
    while (inner.n && inner.v[inner.n - 1][0] == 0) free(vec_pop(&inner));
    for (size_t i = 0; i < inner.n; i++) {
        sb_t b = SB_INIT;
        tui_fg(&b, c->th, TUI_TC_MD_QUOTE_BORDER, BOX_V " ");
        sb_append(&b, qc.base);
        sb_append(&b, inner.v[i]);
        sb_append(&b, TUI_RESET);
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&inner);
    strvec_free(&inner_src);
}

/* ---------- tables ---------- */

static void split_cells(const char *s, strvec_t *cells) {
    s = skip_ws(s);
    if (*s == '|') s++;
    sb_t cur = SB_INIT;
    bool in_code = false;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == '|') {
            sb_putc(&cur, '|');
            s++;
            continue;
        }
        if (*s == '`') in_code = !in_code;
        if (*s == '|' && !in_code) {
            char *t = str_trim(sb_cstr(&cur));
            strvec_push_own(cells, t);
            sb_clear(&cur);
            if (cells->n >= MD_MAX_COLS) break;
            continue;
        }
        sb_putc(&cur, *s);
    }
    char *t = str_trim(sb_cstr(&cur));
    if (*t || cells->n == 0) strvec_push_own(cells, t);
    else free(t);
    sb_free(&cur);
}

static void table_border(sb_t *b, const int *w, int nc, const char *l, const char *m, const char *r) {
    sb_append(b, l);
    for (int i = 0; i < nc; i++) {
        sb_repeat(b, BOX_H, w[i] + 2);
        sb_append(b, i + 1 < nc ? m : r);
    }
}

static void render_table(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, strvec_t *out) {
    strvec_t header = {0}, seps = {0};
    split_cells(L->v[from], &header);
    split_cells(L->v[from + 1], &seps);
    int nc = (int)MIN(header.n, MD_MAX_COLS);
    char align[MD_MAX_COLS];
    for (int i = 0; i < nc; i++) {
        const char *sp = i < (int)seps.n ? seps.v[i] : "";
        size_t sl = strlen(sp);
        bool l = sl && sp[0] == ':', r = sl && sp[sl - 1] == ':';
        align[i] = l && r ? 'c' : r ? 'r' : 'l';
    }
    /* rendered cells: rows[0] = header */
    size_t nrows = 1 + (to - from - 2);
    char ***cells = xcalloc(nrows, sizeof(char **));
    md_ctx_t hc;
    ctx_extend(&hc, c, "\x1b[1m", "", "");
    for (size_t r = 0; r < nrows; r++) {
        strvec_t raw = {0};
        if (r == 0) {
            for (size_t i = 0; i < header.n; i++) strvec_push(&raw, header.v[i]);
        } else split_cells(L->v[from + 1 + r], &raw);
        cells[r] = xcalloc((size_t)nc, sizeof(char *));
        for (int i = 0; i < nc; i++) {
            const char *t = i < (int)raw.n ? raw.v[i] : "";
            sb_t b = SB_INIT;
            render_inline(t, strlen(t), r == 0 ? &hc : c, &b, 0);
            cells[r][i] = sb_detach(&b);
        }
        strvec_free(&raw);
    }
    int nat[MD_MAX_COLS], w[MD_MAX_COLS];
    int sum = 0;
    for (int i = 0; i < nc; i++) {
        nat[i] = 3;
        for (size_t r = 0; r < nrows; r++) nat[i] = MAX(nat[i], tui_visible_width(cells[r][i]));
        sum += nat[i];
    }
    int avail = width - (3 * nc + 1);
    if (avail < nc * 3) {
        /* too narrow for a grid: render rows as "a | b" lines */
        for (size_t r = 0; r < nrows; r++) {
            sb_t b = SB_INIT;
            for (int i = 0; i < nc; i++) {
                if (i) sb_append(&b, " | ");
                sb_append(&b, cells[r][i]);
            }
            push_wrapped(out, sb_cstr(&b), width);
            sb_free(&b);
        }
    } else {
        if (sum <= avail) {
            for (int i = 0; i < nc; i++) w[i] = nat[i];
        } else {
            int used = 0;
            for (int i = 0; i < nc; i++) {
                w[i] = MAX(3, (int)((long)avail * nat[i] / sum));
                w[i] = MIN(w[i], nat[i]);
                used += w[i];
            }
            /* hand leftover columns to the widest truncated columns */
            for (int guard = 0; used < avail && guard < 10000; guard++) {
                int best = -1;
                for (int i = 0; i < nc; i++)
                    if (w[i] < nat[i] && (best < 0 || nat[i] - w[i] > nat[best] - w[best])) best = i;
                if (best < 0) break;
                w[best]++;
                used++;
            }
            while (used > avail) {
                int best = 0;
                for (int i = 1; i < nc; i++)
                    if (w[i] > w[best]) best = i;
                if (w[best] <= 1) break;
                w[best]--;
                used--;
            }
        }
        const char *bc = tui_fg_seq(c->th, TUI_TC_BORDER_MUTED);
        sb_t b = SB_INIT;
        sb_append(&b, bc);
        table_border(&b, w, nc, "\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");
        sb_append(&b, "\x1b[39m");
        strvec_push_own(out, sb_detach(&b));
        for (size_t r = 0; r < nrows; r++) {
            strvec_t wrapped[MD_MAX_COLS];
            size_t h = 1;
            for (int i = 0; i < nc; i++) {
                memset(&wrapped[i], 0, sizeof wrapped[i]);
                tui_wrap_text_with_ansi(cells[r][i], w[i], &wrapped[i]);
                h = MAX(h, wrapped[i].n);
            }
            for (size_t li = 0; li < h; li++) {
                sb_append(&b, bc);
                sb_append(&b, BOX_V);
                sb_append(&b, "\x1b[39m");
                for (int i = 0; i < nc; i++) {
                    const char *cell = li < wrapped[i].n ? wrapped[i].v[li] : "";
                    int cw = tui_visible_width(cell);
                    int pad = MAX(0, w[i] - cw);
                    int lp = align[i] == 'r' ? pad : align[i] == 'c' ? pad / 2 : 0;
                    sb_putc(&b, ' ');
                    tui_sb_spaces(&b, lp);
                    sb_append(&b, cell);
                    sb_append(&b, TUI_RESET);
                    tui_sb_spaces(&b, pad - lp);
                    sb_putc(&b, ' ');
                    sb_append(&b, bc);
                    sb_append(&b, BOX_V);
                    sb_append(&b, "\x1b[39m");
                }
                strvec_push_own(out, sb_detach(&b));
            }
            for (int i = 0; i < nc; i++) strvec_free(&wrapped[i]);
            if (r == 0) {
                sb_append(&b, bc);
                table_border(&b, w, nc, "\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
                sb_append(&b, "\x1b[39m");
                strvec_push_own(out, sb_detach(&b));
            }
        }
        sb_append(&b, bc);
        table_border(&b, w, nc, "\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");
        sb_append(&b, "\x1b[39m");
        strvec_push_own(out, sb_detach(&b));
    }
    for (size_t r = 0; r < nrows; r++) {
        for (int i = 0; i < nc; i++) free(cells[r][i]);
        free(cells[r]);
    }
    free(cells);
    strvec_free(&header);
    strvec_free(&seps);
}

/* ---------- lists ---------- */

static void render_list(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, int depth,
                        strvec_t *out) {
    size_t base_ind;
    bool ordered;
    long num;
    size_t coff, mw;
    list_item(L->v[from], &base_ind, &ordered, &num, &coff, &mw);
    long counter = num;
    size_t i = from;
    while (i < to) {
        size_t ind, co, mww;
        bool ord;
        long nv;
        if (!list_item(L->v[i], &ind, &ord, &nv, &co, &mww)) {
            i++;
            continue;
        }
        size_t content_col = ind + mww + 1;
        /* collect item body */
        strvec_t body = {0};
        strvec_push(&body, L->v[i] + co);
        size_t j = i + 1;
        while (j < to) {
            const char *s = L->v[j];
            size_t ji, jc, jm;
            bool jo;
            long jn;
            if (!is_blank(s) && list_item(s, &ji, &jo, &jn, &jc, &jm) && ji <= base_ind + 1) break;
            /* de-indent continuation lines */
            size_t k = 0, col = 0;
            while (s[k] == ' ' && col < content_col) {
                k++;
                col++;
            }
            strvec_push(&body, s + k);
            j++;
        }
        while (body.n > 1 && is_blank(body.v[body.n - 1])) free(vec_pop(&body));
        char bullet[24];
        if (ordered) snprintf(bullet, sizeof bullet, "%ld. ", counter++);
        else snprintf(bullet, sizeof bullet, "- ");
        int bw = (int)strlen(bullet);
        int iw = MAX(1, width - bw);
        strvec_t rendered = {0};
        lines_t BL = {(const char *const *)body.v, body.n};
        if (depth + 1 < MD_MAX_DEPTH) render_blocks(&BL, 0, BL.n, iw, c, depth + 1, &rendered);
        else
            for (size_t q = 0; q < body.n; q++) push_wrapped(&rendered, body.v[q], iw);
        if (!rendered.n) strvec_push(&rendered, "");
        for (size_t q = 0; q < rendered.n; q++) {
            sb_t b = SB_INIT;
            if (q == 0) tui_fg(&b, c->th, TUI_TC_MD_LIST_BULLET, bullet);
            else tui_sb_spaces(&b, bw);
            sb_append(&b, rendered.v[q]);
            strvec_push_own(out, sb_detach(&b));
        }
        strvec_free(&rendered);
        strvec_free(&body);
        i = j;
    }
}

static void render_blocks(const lines_t *L, size_t from, size_t to, int width, const md_ctx_t *c, int depth,
                          strvec_t *out) {
    size_t i = from;
    btype_t prev = B_NONE;
    bool blank_before = false;
    while (i < to) {
        const char *s = L->v[i];
        if (is_blank(s)) {
            blank_before = true;
            i++;
            continue;
        }
        strvec_t tmp = {0};
        btype_t t;
        char fch;
        size_t flen;
        const char *info;
        int hl;
        if (is_fence(s, &fch, &flen, &info)) {
            t = B_CODE;
            size_t ind = indent_of(s);
            char lang[32];
            size_t ll = 0;
            while (info[ll] && info[ll] != ' ' && info[ll] != '{' && ll < sizeof lang - 1) {
                lang[ll] = info[ll];
                ll++;
            }
            lang[ll] = 0;
            size_t j = i + 1;
            while (j < to) {
                char c2;
                size_t l2;
                const char *inf2;
                if (is_fence(L->v[j], &c2, &l2, &inf2) && c2 == fch && l2 >= flen && !*inf2) break;
                j++;
            }
            render_code(L, i + 1, j, lang, ind, width, c, &tmp);
            i = j < to ? j + 1 : j;
        } else if ((hl = heading_level(s))) {
            t = B_HEADING;
            render_heading(s, hl, width, c, &tmp);
            i++;
        } else if (is_hr(s)) {
            t = B_HR;
            render_hr(width, c, &tmp);
            i++;
        } else if (is_quote(s)) {
            t = B_QUOTE;
            size_t j = i;
            while (j < to && is_quote(L->v[j])) j++;
            render_quote(L, i, j, width, c, depth, &tmp);
            i = j;
        } else if (is_table_start(L, i)) {
            t = B_TABLE;
            size_t j = i + 2;
            while (j < to && !is_blank(L->v[j]) && strchr(L->v[j], '|')) j++;
            render_table(L, i, j, width, c, &tmp);
            i = j;
        } else if (list_item(s, NULL, NULL, NULL, NULL, NULL)) {
            t = B_LIST;
            size_t base = indent_of(s);
            bool first_ordered = false;
            list_item(s, NULL, &first_ordered, NULL, NULL, NULL);
            size_t j = i + 1;
            while (j < to) {
                const char *x = L->v[j];
                if (is_blank(x)) {
                    /* continue only if the next non-blank line is indented or an item */
                    size_t k = j;
                    while (k < to && is_blank(L->v[k])) k++;
                    if (k >= to) break;
                    if (indent_of(L->v[k]) > base || (list_item(L->v[k], NULL, NULL, NULL, NULL, NULL) &&
                                                      indent_of(L->v[k]) <= base + 1 && indent_of(L->v[k]) >= base)) {
                        j = k;
                        continue;
                    }
                    break;
                }
                if (indent_of(x) > base) {
                    j++;
                    continue;
                }
                bool xo;
                if (list_item(x, NULL, &xo, NULL, NULL, NULL)) {
                    if (xo != first_ordered) break; /* bullet type change starts a new list */
                    j++;
                    continue;
                }
                if (starts_block(L, j)) break;
                j++; /* lazy continuation */
            }
            render_list(L, i, j, width, c, depth, &tmp);
            i = j;
        } else {
            t = B_PARA;
            size_t j = i + 1;
            while (j < to && !is_blank(L->v[j]) && !starts_block(L, j)) j++;
            render_paragraph(L, i, j, width, c, &tmp);
            i = j;
        }
        if (prev != B_NONE && !(prev == B_PARA && t == B_LIST && !blank_before)) strvec_push(out, "");
        for (size_t k = 0; k < tmp.n; k++) strvec_push_own(out, tmp.v[k]);
        free(tmp.v);
        prev = t;
        blank_before = false;
    }
}

static void init_ctx(md_ctx_t *c, const tui_md_opts_t *o) {
    memset(c, 0, sizeof *c);
    c->o = o;
    c->th = o->theme ? o->theme : tui_theme();
    if (o->text_slot >= 0) snprintf(c->base, sizeof c->base, "%s", tui_fg_seq(c->th, (tui_tc_t)o->text_slot));
}

void tui_md_render(const char *text, int width, const tui_md_opts_t *opts, strvec_t *out) {
    tui_md_opts_t def;
    if (!opts) {
        tui_md_opts_default(&def);
        opts = &def;
    }
    if (width < 1) width = 1;
    md_ctx_t c;
    init_ctx(&c, opts);
    char *clean = tui_replace_tabs(text ? text : "");
    strvec_t lines = str_split_lines(clean, strlen(clean));
    lines_t L = {(const char *const *)lines.v, lines.n};
    size_t first = out->n;
    render_blocks(&L, 0, L.n, width, &c, 0, out);
    /* clamp pathological cases (prefixes/wide chars wider than tiny widths) */
    for (size_t i = first; i < out->n; i++) {
        if (tui_visible_width(out->v[i]) <= width) continue;
        char *t = tui_truncate_to_width(out->v[i], width, "");
        free(out->v[i]);
        out->v[i] = t;
    }
    strvec_free(&lines);
    free(clean);
}

char *tui_md_inline(const char *text, const tui_md_opts_t *opts) {
    tui_md_opts_t def;
    if (!opts) {
        tui_md_opts_default(&def);
        opts = &def;
    }
    md_ctx_t c;
    init_ctx(&c, opts);
    sb_t b = SB_INIT;
    render_inline(text ? text : "", text ? strlen(text) : 0, &c, &b, 0);
    return sb_detach(&b);
}

/* ====================================================================== */
/* component                                                               */
/* ====================================================================== */

static void md_invalidate(tui_component_t *cp) {
    tui_markdown_t *m = (tui_markdown_t *)cp;
    strvec_free(&m->cache);
    free(m->cache_text);
    m->cache_text = NULL;
    m->cache_width = -1;
}

static void md_render(tui_component_t *cp, int width, strvec_t *out) {
    tui_markdown_t *m = (tui_markdown_t *)cp;
    if (!m->text || !*m->text) return;
    if (!(m->cache_text && m->cache_width == width && !strcmp(m->cache_text, m->text))) {
        md_invalidate(cp);
        strvec_t lines = {0};
        tui_md_render(m->text, MAX(1, width - 2 * m->padding_x), &m->opts, &lines);
        for (int i = 0; i < m->padding_y; i++) strvec_push(&m->cache, "");
        for (size_t i = 0; i < lines.n; i++) {
            sb_t b = SB_INIT;
            tui_sb_spaces(&b, m->padding_x);
            sb_append(&b, lines.v[i]);
            strvec_push_own(&m->cache, sb_detach(&b));
        }
        for (int i = 0; i < m->padding_y; i++) strvec_push(&m->cache, "");
        strvec_free(&lines);
        m->cache_text = xstrdup(m->text);
        m->cache_width = width;
    }
    for (size_t i = 0; i < m->cache.n; i++) strvec_push(out, m->cache.v[i]);
}

static void md_destroy(tui_component_t *cp) {
    tui_markdown_t *m = (tui_markdown_t *)cp;
    md_invalidate(cp);
    free(m->text);
    free(m);
}

static const tui_component_vt MD_VT = {"markdown", md_render, NULL, md_invalidate, md_destroy};

tui_markdown_t *tui_markdown_new(const char *text, int padding_x, int padding_y) {
    tui_markdown_t *m = xcalloc(1, sizeof *m);
    tui_component_init(&m->base, &MD_VT);
    m->text = xstrdup(text ? text : "");
    m->padding_x = MAX(0, padding_x);
    m->padding_y = MAX(0, padding_y);
    m->cache_width = -1;
    tui_md_opts_default(&m->opts);
    return m;
}

void tui_markdown_set_text(tui_markdown_t *m, const char *text) {
    free(m->text);
    m->text = xstrdup(text ? text : "");
    tui_component_request_render(&m->base);
}

void tui_markdown_append(tui_markdown_t *m, const char *delta) {
    char *t = xasprintf("%s%s", m->text, delta ? delta : "");
    free(m->text);
    m->text = t;
    tui_component_request_render(&m->base);
}
