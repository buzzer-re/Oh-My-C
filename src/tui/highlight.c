/* highlight.c - code syntax highlighting with the color categories of omp's native highlighter
 * (crates/pi-natives/src/highlight.rs: syntect scopes -> comment, keyword, function, variable, string,
 * number, type, operator, punctuation, inserted, deleted; theme syntax* colors).
 *
 * This is not a sublime-syntax engine. Each language family is a small hand-written lexer that assigns the
 * category syntect's bundled grammar would give a token under omp's scope->color mapping, e.g.:
 *   - quote and comment delimiters are punctuation; string bodies string; escapes string; printf
 *     placeholders number; constant.language (true/null/None) number; keyword.operator keyword color
 *   - unscoped text (identifiers, whitespace) is uncolored, except inside a function call's arguments
 *     (meta.function-call), where it takes the function color
 *   - "." / "->" member accessors are operator color
 * Unsupported languages render verbatim, as omp does. */
#include "tui/markdown.h"

#include <ctype.h>
#include <strings.h>

enum { H_NONE = -1, H_COMMENT, H_KEYWORD, H_FUNCTION, H_VARIABLE, H_STRING, H_NUMBER, H_TYPE, H_OPERATOR, H_PUNCT,
       H_INSERTED, H_DELETED };

static const tui_tc_t HSLOT[] = {TUI_TC_SYNTAX_COMMENT,  TUI_TC_SYNTAX_KEYWORD, TUI_TC_SYNTAX_FUNCTION,
                                 TUI_TC_SYNTAX_VARIABLE, TUI_TC_SYNTAX_STRING,  TUI_TC_SYNTAX_NUMBER,
                                 TUI_TC_SYNTAX_TYPE,     TUI_TC_SYNTAX_OPERATOR, TUI_TC_SYNTAX_PUNCTUATION,
                                 TUI_TC_TOOL_DIFF_ADDED, TUI_TC_TOOL_DIFF_REMOVED};

typedef enum { F_C, F_JS, F_PY, F_SH, F_JSON, F_YAML, F_TOML, F_SQL, F_DIFF, F_RUBY, F_LUA, F_HTML, F_CSS } family_t;

typedef struct {
    const char *names; /* space separated aliases, padded with spaces */
    family_t fam;
    const char *keywords;
    const char *types;     /* storage.type-like builtin types (keyword color) */
    const char *support;   /* support.type / classes (type color) */
    const char *constants; /* constant.language (number color) */
    const char *builtins;  /* support.function (function color) */
    const char *defkw;     /* keywords introducing a function name */
    const char *classkw;   /* keywords introducing a type name */
    bool caps_types;       /* Capitalized identifiers are types */
    bool backtick;         /* `...` strings */
    bool hash_comment;     /* '#' line comments (php) */
} lang_t;

static const lang_t LANGS[] = {
    {" c h objc objective-c ", F_C,
     " if else for while do switch case default break continue return goto sizeof typedef struct union enum static "
     "extern const volatile register inline restrict auto signed unsigned _Bool _Atomic _Noreturn alignof ",
     " int char short long float double void bool ",
     " size_t ssize_t ptrdiff_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
     "FILE va_list off_t time_t pid_t ",
     " NULL true false EOF stdin stdout stderr ", "", "", " struct union enum ", false, false, false},
    {" cpp c++ cc cxx hpp hxx hh cuda ", F_C,
     " if else for while do switch case default break continue return goto sizeof typedef struct union enum static "
     "extern const volatile register inline auto signed unsigned class namespace template typename public private "
     "protected virtual override final new delete throw try catch using operator friend constexpr noexcept decltype "
     "static_cast reinterpret_cast const_cast dynamic_cast mutable explicit this co_await co_return co_yield concept "
     "requires ",
     " int char short long float double void bool wchar_t char8_t char16_t char32_t ",
     " size_t ssize_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t string vector map "
     "unordered_map set unique_ptr shared_ptr optional ",
     " NULL nullptr true false ", "", "", " class struct union enum namespace ", true, false, false},
    {" js javascript jsx mjs cjs node ", F_JS,
     " if else for while do switch case default break continue return function var let const class extends new delete "
     "typeof instanceof in of throw try catch finally async await yield import export from as static get set void "
     "with debugger super ",
     "", "", " null undefined true false NaN Infinity ", "", " function ", " class extends new ", true, true, false},
    {" ts typescript tsx mts cts ", F_JS,
     " if else for while do switch case default break continue return function var let const class extends new delete "
     "typeof instanceof in of throw try catch finally async await yield import export from as static get set void "
     "interface type enum implements namespace declare readonly abstract keyof satisfies public private protected "
     "is infer module super ",
     "", " string number boolean any unknown never object symbol bigint ", " null undefined true false NaN Infinity ",
     "", " function ", " class interface type enum extends implements new ", true, true, false},
    {" python py py3 python3 ", F_PY,
     " if elif else for while break continue return def class lambda try except finally raise with as import from "
     "global nonlocal pass yield assert del in is not and or async await match case ",
     "", " int str float bool list dict set tuple bytes object type complex frozenset bytearray ",
     " None True False NotImplemented Ellipsis __name__ __file__ ",
     " print len range open enumerate zip map filter sorted reversed min max sum abs any all isinstance issubclass "
     "getattr setattr hasattr repr input iter next super round format id hash vars dir ",
     " def ", " class ", false, false, false},
    {" rust rs ", F_C,
     " if else for while loop break continue return fn let mut const static struct enum impl trait type where pub use "
     "mod crate super match as in ref move unsafe async await dyn extern ",
     " i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str ", " String Vec Option Result Box Self ",
     " true false None Some Ok Err self ", "", " fn ", " struct enum trait impl type ", true, false, false},
    {" go golang ", F_C,
     " if else for range break continue return func var const type struct interface map chan go defer select switch "
     "case default package import fallthrough goto ",
     " int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 uintptr float32 float64 complex64 complex128 string "
     "bool byte rune error any ",
     "", " nil true false iota ",
     " append cap close copy delete len make new panic print println recover ", " func ", " type ", false, true, false},
    {" java kotlin kt kts scala swift cs csharp c# dart groovy ", F_C,
     " if else for while do switch case default break continue return class interface extends implements new this "
     "super throw throws try catch finally import package public private protected static final abstract "
     "synchronized var val fun let func struct override when is in as object namespace using guard defer where "
     "internal open data sealed companion init get set async await yield record ",
     " int long short byte float double boolean char void ",
     " String Integer Object List Map Set Boolean Double Long ", " null true false nil ", "", " fun func def ",
     " class interface struct enum object record ", true, false, false},
    {" php ", F_C,
     " if else elseif for foreach while do switch case default break continue return function class new public "
     "private protected static echo namespace use array fn match abstract final interface extends implements try "
     "catch finally throw ",
     "", "", " null true false NULL TRUE FALSE ", "", " function ", " class interface ", false, false, true},
    {" sh bash zsh shell console shellscript fish ksh ", F_SH,
     " if then else elif fi for while until do done case esac in function select time coproc ", "", "", "", "", "", "",
     false, true, false},
    {" json jsonc json5 ", F_JSON, "", "", "", " true false null ", "", "", "", false, false, false},
    {" yaml yml ", F_YAML, "", "", "", " true false null yes no on off True False Null ~ ", "", "", "", false, false, false},
    {" toml ini cfg conf ", F_TOML, "", "", "", " true false ", "", "", "", false, false, false},
    {" sql postgres postgresql mysql sqlite plsql ", F_SQL,
     " select from where insert into values update set delete create table drop alter index join left right inner outer "
     "on group by order having limit offset as and or not is in like between distinct union all case when then else end "
     "primary key foreign references default exists returning with view begin commit rollback if ",
     " int integer text varchar char boolean float real date timestamp serial bigint numeric decimal ", "",
     " null true false ", " count sum avg min max coalesce now ", "", "", false, false, false},
    {" diff patch udiff ", F_DIFF, "", "", "", "", "", "", "", false, false, false},
    {" ruby rb ", F_RUBY,
     " if elsif else unless case when while until for in do end def class module return yield begin rescue ensure "
     "raise break next redo retry super then and or not alias undef ",
     "", "", " nil true false self ", " puts print require require_relative attr_accessor attr_reader lambda proc ",
     " def ", " class module ", true, false, false},
    {" lua ", F_LUA,
     " if then else elseif end for while do repeat until function local return break in and or not goto ", "", "",
     " nil true false ", " print pairs ipairs require type tostring tonumber ", " function ", "", false, false, false},
    {" html htm xml svg vue svelte ", F_HTML, "", "", "", "", "", "", "", false, false, false},
    {" css scss less sass ", F_CSS, "", "", "", "", "", "", "", false, false, false},
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

bool tui_highlight_supported(const char *lang) { return find_lang(lang) != NULL; }

static bool word_in(const char *list, const char *w, size_t n) {
    if (!list || !*list || n == 0 || n > 30) return false;
    char key[34];
    key[0] = ' ';
    memcpy(key + 1, w, n);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    return strstr(list, key) != NULL;
}

static bool word_in_ci(const char *list, const char *w, size_t n) {
    if (!list || !*list || n == 0 || n > 30) return false;
    char key[34];
    key[0] = ' ';
    for (size_t i = 0; i < n; i++) key[i + 1] = (char)tolower((unsigned char)w[i]);
    key[n + 1] = ' ';
    key[n + 2] = 0;
    return strstr(list, key) != NULL;
}

/* ---------- emitter ---------- */

typedef struct {
    sb_t *b;
    const tui_theme_t *th;
    int cur;
} em_t;

static void em(em_t *e, int cat, const char *s, size_t n) {
    if (!n) return;
    if (cat != e->cur) {
        if (e->cur >= 0) sb_append(e->b, "\x1b[39m");
        if (cat >= 0) sb_append(e->b, tui_fg_seq(e->th, HSLOT[cat]));
        e->cur = cat;
    }
    sb_appendn(e->b, s, n);
}

static void em_end(em_t *e) {
    if (e->cur >= 0) sb_append(e->b, "\x1b[39m");
    e->cur = H_NONE;
}

/* ---------- state carried across lines ---------- */

#define MAX_FRAMES 64

typedef struct {
    int mode;        /* 0 code, 1 block comment, 2 string */
    char delim[4];   /* string delimiter */
    bool interp;     /* string allows ${..} / {..} interpolation */
    bool raw;        /* no escapes */
    char frames[MAX_FRAMES]; /* paren stack: 'c' call args, 'p' params, '(' plain, '[' '{' */
    int nf;
    int expect;      /* 1: next identifier is a function name, 2: a type name */
    int last;        /* previous significant token: 0 none/op, 1 identifier/type, 2 keyword-type */
    bool sh_cmd;     /* shell: at command position */
    bool sh_args;    /* shell: inside a command's arguments */
} hs_t;

static bool in_call(const hs_t *st) {
    for (int i = st->nf - 1; i >= 0; i--) {
        if (st->frames[i] == 'c') return true;
        if (st->frames[i] == '{' || st->frames[i] == 'p') return false;
    }
    return false;
}

static void push_frame(hs_t *st, char k) {
    if (st->nf < MAX_FRAMES) st->frames[st->nf] = k;
    st->nf++;
}

static char pop_frame(hs_t *st) {
    if (st->nf <= 0) return 0;
    st->nf--;
    return st->nf < MAX_FRAMES ? st->frames[st->nf] : '(';
}

/* the category of unscoped text at this point */
static int plain(const hs_t *st) { return in_call(st) ? H_FUNCTION : H_NONE; }

static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

static size_t number_len(const char *s, size_t n) {
    size_t j = 0;
    if (n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'b' || s[1] == 'o')) j = 2;
    while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || (s[j] == '.' && j + 1 < n && isdigit((unsigned char)s[j + 1])) ||
                     ((s[j] == '+' || s[j] == '-') && j > 0 && (s[j - 1] == 'e' || s[j - 1] == 'E') && s[0] != '0')))
        j++;
    return j ? j : 1;
}

/* body of a string up to its closing delimiter; returns index after the closer (or n) */
static size_t string_body(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t i, size_t n, bool c_fmt) {
    size_t dl = strlen(st->delim);
    size_t start = i;
    while (i < n) {
        if (!st->raw && s[i] == '\\' && i + 1 < n) {
            em(e, H_STRING, s + start, i - start);
            em(e, H_STRING, s + i, 2); /* constant.character.escape -> string color */
            i += 2;
            start = i;
            continue;
        }
        if (c_fmt && s[i] == '%' && i + 1 < n) {
            size_t j = i + 1;
            if (s[j] == '%') {
                i += 2;
                continue;
            }
            while (j < n && strchr("-+ #0123456789.*hlLqjzt", s[j])) j++;
            if (j < n && strchr("diouxXeEfFgGaAcspn", s[j])) {
                em(e, H_STRING, s + start, i - start);
                em(e, H_NUMBER, s + i, j + 1 - i); /* constant.other.placeholder */
                i = j + 1;
                start = i;
                continue;
            }
        }
        if (st->interp && ((L->fam == F_JS || L->fam == F_SH || L->fam == F_RUBY) ? (s[i] == '$' || s[i] == '#') && i + 1 < n && s[i + 1] == '{'
                                                                                  : s[i] == '{' && !(i + 1 < n && s[i + 1] == '{'))) {
            em(e, H_STRING, s + start, i - start);
            size_t open = s[i] == '{' ? 1 : 2;
            em(e, H_PUNCT, s + i, open);
            i += open;
            int depth = 0;
            size_t j = i;
            while (j < n && !(s[j] == '}' && depth == 0)) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') depth--;
                j++;
            }
            /* interpolated code: the string scope is cleared */
            for (size_t k = i; k < j;) {
                if (ident_start(s[k])) {
                    size_t q = k;
                    while (q < j && ident_char(s[q])) q++;
                    em(e, word_in(L->constants, s + k, q - k) ? H_NUMBER : plain(st), s + k, q - k);
                    k = q;
                } else if (strchr("()[],:.", s[k])) {
                    em(e, s[k] == '.' ? H_OPERATOR : H_PUNCT, s + k, 1);
                    k++;
                } else if (strchr("+-*/%=<>!&|", s[k])) {
                    em(e, H_KEYWORD, s + k, 1);
                    k++;
                } else {
                    em(e, plain(st), s + k, 1);
                    k++;
                }
            }
            i = j;
            if (i < n) {
                em(e, H_PUNCT, s + i, 1);
                i++;
            }
            start = i;
            continue;
        }
        if (i + dl <= n && !memcmp(s + i, st->delim, dl)) {
            em(e, H_STRING, s + start, i - start);
            em(e, H_PUNCT, s + i, dl);
            st->mode = 0;
            return i + dl;
        }
        i++;
    }
    em(e, H_STRING, s + start, i - start);
    return n;
}

static void open_string(em_t *e, const lang_t *L, hs_t *st, const char *q, size_t ql, bool interp, bool raw) {
    em(e, H_PUNCT, q, ql);
    st->mode = 2;
    snprintf(st->delim, sizeof st->delim, "%.*s", (int)ql, q);
    st->interp = interp;
    st->raw = raw;
    (void)L;
}

/* ---------- C-like / JS / Python / Ruby / Lua / SQL ---------- */

static void lex_code(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    bool py = L->fam == F_PY, js = L->fam == F_JS, sql = L->fam == F_SQL;
    bool c_fmt = L->fam == F_C;
    const char *lc = py || L->fam == F_RUBY ? "#" : (sql || L->fam == F_LUA) ? "--" : "//";
    bool block = !py && L->fam != F_RUBY;
    const char *bo = L->fam == F_LUA ? "--[[" : "/*", *bc = L->fam == F_LUA ? "]]" : "*/";
    /* C preprocessor line */
    if (L->fam == F_C && st->mode == 0) {
        size_t k = 0;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == '#' && !L->hash_comment) {
            size_t j = k + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            while (j < n && isalpha((unsigned char)s[j])) j++;
            em(e, H_NONE, s, k);
            em(e, H_KEYWORD, s + k, j - k);
            bool include = (j - k >= 8 && memmem(s + k, j - k, "include", 7)) || memmem(s + k, j - k, "import", 6);
            i = j;
            if (include) {
                while (i < n && (s[i] == ' ' || s[i] == '\t')) em(e, H_NONE, s + i++, 1);
                if (i < n && (s[i] == '<' || s[i] == '"')) {
                    char close = s[i] == '<' ? '>' : '"';
                    size_t q = i + 1;
                    while (q < n && s[q] != close) q++;
                    em(e, H_PUNCT, s + i, 1);
                    em(e, H_STRING, s + i + 1, q - i - 1);
                    if (q < n) em(e, H_PUNCT, s + q, 1);
                    i = q < n ? q + 1 : n;
                }
            } else if (memmem(s + k, j - k, "define", 6)) {
                while (i < n && (s[i] == ' ' || s[i] == '\t')) em(e, H_NONE, s + i++, 1);
                size_t q = i;
                while (q < n && ident_char(s[q])) q++;
                em(e, H_VARIABLE, s + i, q - i); /* entity.name.constant.preprocessor */
                i = q;
            }
        }
    }
    while (i < n) {
        if (st->mode == 1) {
            const char *end = NULL;
            for (size_t k = i; k + strlen(bc) <= n; k++)
                if (!memcmp(s + k, bc, strlen(bc))) {
                    end = s + k;
                    break;
                }
            if (!end) {
                em(e, H_COMMENT, s + i, n - i);
                return;
            }
            em(e, H_COMMENT, s + i, (size_t)(end - s) - i);
            em(e, H_PUNCT, end, strlen(bc));
            i = (size_t)(end - s) + strlen(bc);
            st->mode = 0;
            continue;
        }
        if (st->mode == 2) {
            i = string_body(e, L, st, s, i, n, c_fmt && st->delim[0] == '"');
            continue;
        }
        char c = s[i];
        /* comments */
        if ((!strncmp(s + i, lc, strlen(lc)) && (lc[0] != '#' || !(i > 0 && s[i - 1] == '$'))) ||
            (L->hash_comment && c == '#')) {
            size_t ml = L->hash_comment && c == '#' ? 1 : strlen(lc);
            if (L->fam == F_LUA && !strncmp(s + i, "--[[", 4)) {
                em(e, H_PUNCT, s + i, 4);
                i += 4;
                st->mode = 1;
                continue;
            }
            em(e, H_PUNCT, s + i, ml);
            em(e, H_COMMENT, s + i + ml, n - i - ml);
            return;
        }
        if (block && !strncmp(s + i, bo, strlen(bo))) {
            em(e, H_PUNCT, s + i, strlen(bo));
            i += strlen(bo);
            st->mode = 1;
            continue;
        }
        /* strings */
        if (py && (c == '"' || c == '\'' || ((c == 'f' || c == 'r' || c == 'b' || c == 'u' || c == 'F' || c == 'R' || c == 'B') &&
                                             i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\'' ||
                                                           (strchr("rRbBfF", s[i + 1]) && i + 2 < n && (s[i + 2] == '"' || s[i + 2] == '\''))))) &&
            !(i > 0 && ident_char(s[i - 1]))) {
            size_t p = i;
            bool fstr = false, raw = false;
            while (p < n && s[p] != '"' && s[p] != '\'') {
                if (s[p] == 'f' || s[p] == 'F') fstr = true;
                if (s[p] == 'r' || s[p] == 'R') raw = true;
                p++;
            }
            if (p > i) em(e, H_KEYWORD, s + i, p - i); /* storage.type.string */
            size_t ql = p + 2 < n && s[p + 1] == s[p] && s[p + 2] == s[p] ? 3 : 1;
            open_string(e, L, st, s + p, ql, fstr, raw);
            i = p + ql;
            st->last = 1;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && L->backtick)) {
            if (c == '\'' && L->fam == F_C && !strstr(L->names, " go ") && i > 0 && ident_char(s[i - 1])) {
                em(e, plain(st), s + i, 1); /* rust lifetimes etc. */
                i++;
                continue;
            }
            open_string(e, L, st, s + i, 1, c == '`' && js, c == '`');
            i++;
            st->last = 1;
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            if (i > 0 && ident_char(s[i - 1])) {
                em(e, plain(st), s + i, 1);
                i++;
                continue;
            }
            size_t l = number_len(s + i, n - i);
            em(e, H_NUMBER, s + i, l);
            i += l;
            st->last = 1;
            continue;
        }
        if (ident_start(c) || (c == '$' && (js || L->fam == F_RUBY)) || (c == '@' && (py || L->fam == F_RUBY))) {
            size_t j = i + 1;
            while (j < n && (ident_char(s[j]) || (c == '$' && s[j] == '$'))) j++;
            if (L->fam == F_RUBY && j < n && (s[j] == '?' || s[j] == '!')) j++;
            size_t wl = j - i;
            size_t k = j;
            while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            bool call = k < n && s[k] == '(';
            bool macro = !call && j < n && s[j] == '!' && strstr(L->names, " rust ");
            int cat;
            if (c == '@' && py) {
                cat = H_FUNCTION; /* decorator */
            } else if (st->expect == 1) {
                cat = H_FUNCTION;
                st->expect = 0;
            } else if (st->expect == 2) {
                cat = H_TYPE;
                st->expect = 0;
            } else if (sql ? word_in_ci(L->keywords, s + i, wl) : word_in(L->keywords, s + i, wl)) {
                cat = H_KEYWORD;
                if (word_in(L->defkw, s + i, wl)) st->expect = 1;
                else if (word_in(L->classkw, s + i, wl)) st->expect = 2;
            } else if (sql ? word_in_ci(L->types, s + i, wl) : word_in(L->types, s + i, wl)) {
                cat = H_KEYWORD; /* storage.type */
            } else if (word_in(L->constants, s + i, wl) || (sql && word_in_ci(L->constants, s + i, wl))) {
                cat = H_NUMBER;
            } else if (py && (!strncmp(s + i, "self", 4) || !strncmp(s + i, "cls", 3)) && (wl == 4 || wl == 3) &&
                       (wl == 4 ? !strncmp(s + i, "self", 4) : !strncmp(s + i, "cls", 3))) {
                cat = H_VARIABLE; /* variable.language */
            } else if (js && wl == 4 && !strncmp(s + i, "this", 4)) {
                cat = H_VARIABLE;
            } else if (call) {
                cat = H_FUNCTION;
            } else if (word_in(L->support, s + i, wl) || (L->caps_types && isupper((unsigned char)c) && wl > 1 &&
                                                          !(wl > 2 && isupper((unsigned char)s[i + 1]) && isupper((unsigned char)s[j - 1])))) {
                cat = H_TYPE;
            } else if (word_in(L->builtins, s + i, wl)) {
                cat = H_FUNCTION;
            } else if (st->nf && st->frames[st->nf - 1 < MAX_FRAMES ? st->nf - 1 : MAX_FRAMES - 1] == 'p') {
                cat = H_VARIABLE; /* parameters of a definition */
            } else if (in_call(st) && k < n && s[k] == '=' && !(k + 1 < n && s[k + 1] == '=')) {
                cat = H_VARIABLE; /* keyword argument */
            } else {
                cat = plain(st);
            }
            if (macro) cat = H_NONE;
            em(e, cat, s + i, wl);
            i = j;
            if (call && cat == H_FUNCTION) {
                /* a definition's parameter list vs a call's arguments */
                bool def = cat == H_FUNCTION && (st->last == 3 || (L->fam == F_C && st->last == 1 && st->nf == 0));
                st->last = def ? 4 : 5;
            } else {
                st->last = cat == H_KEYWORD ? (word_in(L->defkw, s + i - wl, wl) ? 3 : 2) : 1;
            }
            continue;
        }
        if (c == '(') {
            push_frame(st, st->last == 4 ? 'p' : st->last == 5 ? 'c' : '(');
            em(e, H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        if (c == '[' || c == '{') {
            push_frame(st, c);
            em(e, H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            pop_frame(st);
            em(e, H_PUNCT, s + i, 1);
            i++;
            st->last = 1;
            continue;
        }
        if (c == '.' || (c == '-' && i + 1 < n && s[i + 1] == '>' && L->fam == F_C) || (c == ':' && i + 1 < n && s[i + 1] == ':')) {
            size_t l = c == '.' ? 1 : 2;
            if (c == '.' && i + 2 < n && s[i + 1] == '.' && s[i + 2] == '.') l = 3;
            em(e, l == 3 ? H_KEYWORD : H_OPERATOR, s + i, l); /* punctuation.accessor */
            i += l;
            st->last = 0;
            continue;
        }
        if (py && c == '-' && i + 1 < n && s[i + 1] == '>') {
            em(e, H_PUNCT, s + i, 2); /* punctuation.separator.annotation */
            i += 2;
            st->expect = 0;
            continue;
        }
        if (strchr("+-*/%=<>!&|^~?", c)) {
            size_t l = 1;
            while (i + l < n && l < 3 && strchr("+-*/%=<>!&|^~?", s[i + l])) l++;
            em(e, H_KEYWORD, s + i, l); /* keyword.operator */
            i += l;
            st->last = 0;
            continue;
        }
        if (strchr(",;:", c)) {
            em(e, H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        em(e, plain(st), s + i, (size_t)k);
        i += (size_t)k;
    }
}

/* ---------- shell ---------- */

static void lex_shell(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    if (st->mode != 2) {
        st->sh_cmd = true;
        st->sh_args = false;
    }
    while (i < n) {
        if (st->mode == 2) {
            i = string_body(e, L, st, s, i, n, false);
            continue;
        }
        char c = s[i];
        int base = st->sh_args ? H_FUNCTION : H_NONE;
        if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            em(e, H_PUNCT, s + i, 1);
            em(e, H_COMMENT, s + i + 1, n - i - 1);
            return;
        }
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == '-' && st->sh_args) {
                /* option: " -" punctuation, name variable.parameter */
                size_t q = j;
                while (q < n && s[q] == '-') q++;
                em(e, H_PUNCT, s + i, q - i);
                size_t r = q;
                while (r < n && (isalnum((unsigned char)s[r]) || s[r] == '-' || s[r] == '_')) r++;
                em(e, H_VARIABLE, s + q, r - q);
                i = r;
                continue;
            }
            bool op_next = j < n && strchr("|&;<>)", s[j]);
            em(e, op_next || !st->sh_args ? H_NONE : base, s + i, j - i);
            i = j;
            continue;
        }
        if (c == '"' || c == '\'') {
            open_string(e, L, st, s + i, 1, c == '"', c == '\'');
            i++;
            if (st->sh_cmd) {
                st->sh_cmd = false;
                st->sh_args = true;
            }
            continue;
        }
        if (c == '$') {
            size_t j = i + 1;
            if (j < n && s[j] == '{') {
                size_t q = j;
                while (q < n && s[q] != '}') q++;
                em(e, H_PUNCT, s + i, 2);
                em(e, H_VARIABLE, s + j + 1, q - j - 1);
                if (q < n) em(e, H_PUNCT, s + q, 1);
                i = q < n ? q + 1 : n;
            } else if (j < n && s[j] == '(') {
                em(e, H_PUNCT, s + i, 2);
                i += 2;
                st->sh_cmd = true;
                st->sh_args = false;
            } else {
                while (j < n && (ident_char(s[j]) || (j == i + 1 && strchr("?@#*$!0123456789-", s[j])))) j++;
                em(e, H_PUNCT, s + i, 1);
                em(e, H_VARIABLE, s + i + 1, j - i - 1);
                i = j;
            }
            if (st->sh_cmd) {
                st->sh_cmd = false;
                st->sh_args = true;
            }
            continue;
        }
        if (strchr("|&;<>", c)) {
            size_t l = 1;
            while (i + l < n && l < 2 && strchr("|&;<>", s[i + l])) l++;
            em(e, H_KEYWORD, s + i, l);
            i += l;
            if (c != '<' && c != '>') {
                st->sh_cmd = true;
                st->sh_args = false;
            }
            continue;
        }
        if (c == '(' || c == ')') {
            em(e, H_PUNCT, s + i, 1);
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && !strchr(" \t|&;<>\"'$()", s[j])) j++;
        if (j == i) j = i + 1;
        if (st->sh_cmd) {
            const char *eq = memchr(s + i, '=', j - i);
            if (eq && eq > s + i) {
                em(e, H_VARIABLE, s + i, (size_t)(eq - s) - i);
                em(e, H_KEYWORD, eq, 1);
                i = (size_t)(eq - s) + 1;
                continue;
            }
            if (word_in(L->keywords, s + i, j - i)) {
                em(e, H_KEYWORD, s + i, j - i);
                i = j;
                continue;
            }
            /* the command and its arguments: meta.function-call */
            size_t q = j;
            while (q < n && (s[q] == ' ' || s[q] == '\t')) q++;
            bool more = q < n && !strchr("|&;<>#-", s[q]);
            em(e, H_FUNCTION, s + i, (more ? q : j) - i);
            i = more ? q : j;
            st->sh_cmd = false;
            st->sh_args = true;
            continue;
        }
        em(e, base, s + i, j - i);
        i = j;
    }
}

/* ---------- data formats ---------- */

static void lex_json(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (st->mode == 2) {
            i = string_body(e, L, st, s, i, n, false);
            continue;
        }
        char c = s[i];
        if (c == '"') {
            open_string(e, L, st, s + i, 1, false, false);
            i++;
        } else if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            em(e, H_PUNCT, s + i, 2);
            em(e, H_COMMENT, s + i + 2, n - i - 2);
            return;
        } else if (isdigit((unsigned char)c) || c == '-') {
            size_t l = number_len(s + i + (c == '-'), n - i - (c == '-')) + (c == '-');
            em(e, H_NUMBER, s + i, l);
            i += l;
        } else if (isalpha((unsigned char)c)) {
            size_t j = i;
            while (j < n && isalpha((unsigned char)s[j])) j++;
            em(e, word_in(L->constants, s + i, j - i) ? H_NUMBER : H_NONE, s + i, j - i);
            i = j;
        } else if (strchr("{}[]:,", c)) {
            em(e, H_PUNCT, s + i, 1);
            i++;
        } else {
            em(e, H_NONE, s + i, 1);
            i++;
        }
    }
}

static void lex_yaml(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    em(e, H_NONE, s, i);
    if (i < n && s[i] == '#') {
        em(e, H_PUNCT, s + i, 1);
        em(e, H_COMMENT, s + i + 1, n - i - 1);
        return;
    }
    if (i + 1 < n && s[i] == '-' && s[i + 1] == ' ') {
        em(e, H_PUNCT, s + i, 1);
        em(e, H_NONE, s + i + 1, 1);
        i += 2;
    }
    /* key: */
    size_t k = i;
    while (k < n && s[k] != ':' && s[k] != '#' && s[k] != '"' && s[k] != '\'') k++;
    if (k < n && s[k] == ':' && (k + 1 == n || s[k + 1] == ' ') && k > i) {
        em(e, H_VARIABLE, s + i, k - i); /* entity.name.tag */
        em(e, H_PUNCT, s + k, 1);
        i = k + 1;
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) em(e, H_NONE, s + i++, 1);
    if (i >= n) return;
    size_t end = n;
    for (size_t q = i; q < n; q++)
        if (s[q] == '#' && q > i && s[q - 1] == ' ') {
            end = q;
            break;
        }
    size_t vend = end;
    while (vend > i && s[vend - 1] == ' ') vend--;
    if (s[i] == '"' || s[i] == '\'') {
        em(e, H_PUNCT, s + i, 1);
        size_t q = vend > i + 1 && s[vend - 1] == s[i] ? vend - 1 : vend;
        em(e, H_STRING, s + i + 1, q - i - 1);
        if (q < vend) em(e, H_PUNCT, s + q, 1);
    } else {
        bool num = true;
        for (size_t q = i; q < vend; q++)
            if (!isdigit((unsigned char)s[q]) && !strchr(".-+_eE", s[q])) num = false;
        int cat = num ? H_NUMBER : word_in(L->constants, s + i, vend - i) ? H_NUMBER : strchr("|>[{&*!", s[i]) ? H_PUNCT : H_STRING;
        em(e, cat, s + i, vend - i);
    }
    em(e, H_NONE, s + vend, end - vend);
    if (end < n) {
        em(e, H_PUNCT, s + end, 1);
        em(e, H_COMMENT, s + end + 1, n - end - 1);
    }
    (void)st;
}

static void lex_toml(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    em(e, H_NONE, s, i);
    if (i < n && (s[i] == '#' || s[i] == ';')) {
        em(e, H_PUNCT, s + i, 1);
        em(e, H_COMMENT, s + i + 1, n - i - 1);
        return;
    }
    if (i < n && s[i] == '[') {
        size_t q = i;
        while (q < n && s[q] == '[') q++;
        size_t r = q;
        while (r < n && s[r] != ']') r++;
        em(e, H_PUNCT, s + i, q - i);
        em(e, H_TYPE, s + q, r - q); /* entity.name.section */
        em(e, H_PUNCT, s + r, n - r);
        return;
    }
    const char *eq = memchr(s + i, '=', n - i);
    if (eq) {
        size_t k = (size_t)(eq - s);
        size_t ke = k;
        while (ke > i && s[ke - 1] == ' ') ke--;
        em(e, H_VARIABLE, s + i, ke - i);
        em(e, H_NONE, s + ke, k - ke);
        em(e, H_KEYWORD, eq, 1);
        i = k + 1;
        lex_json(e, L, st, s + i, n - i);
        return;
    }
    em(e, H_NONE, s + i, n - i);
}

static void lex_diff(em_t *e, const char *s, size_t n) {
    if (!strncmp(s, "+++", 3) || !strncmp(s, "---", 3) || !strncmp(s, "@@", 2) || !strncmp(s, "diff ", 5) ||
        !strncmp(s, "index ", 6)) {
        em(e, H_KEYWORD, s, n);
    } else if (n && (s[0] == '+' || s[0] == '>')) {
        em(e, H_PUNCT, s, 1);
        em(e, H_INSERTED, s + 1, n - 1);
    } else if (n && (s[0] == '-' || s[0] == '<')) {
        em(e, H_PUNCT, s, 1);
        em(e, H_DELETED, s + 1, n - 1);
    } else {
        em(e, H_NONE, s, n);
    }
}

static void lex_html(em_t *e, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (st->mode == 1) {
            const char *end = strstr(s + i, "-->");
            size_t stop = end && (size_t)(end - s) < n ? (size_t)(end - s) : n;
            em(e, H_COMMENT, s + i, stop - i);
            if (stop < n) {
                em(e, H_PUNCT, s + stop, 3);
                st->mode = 0;
                i = stop + 3;
            } else i = n;
            continue;
        }
        if (!strncmp(s + i, "<!--", 4)) {
            em(e, H_PUNCT, s + i, 4);
            i += 4;
            st->mode = 1;
            continue;
        }
        if (s[i] == '<') {
            size_t j = i + 1;
            if (j < n && (s[j] == '/' || s[j] == '!' || s[j] == '?')) j++;
            em(e, H_PUNCT, s + i, j - i);
            size_t q = j;
            while (q < n && (isalnum((unsigned char)s[q]) || s[q] == '-' || s[q] == ':')) q++;
            em(e, H_FUNCTION, s + j, q - j); /* entity.name.tag -> variable? syntect HTML: entity.name.tag */
            i = q;
            while (i < n && s[i] != '>') {
                if (s[i] == '"' || s[i] == '\'') {
                    char qc = s[i];
                    size_t r = i + 1;
                    while (r < n && s[r] != qc) r++;
                    em(e, H_PUNCT, s + i, 1);
                    em(e, H_STRING, s + i + 1, r - i - 1);
                    if (r < n) em(e, H_PUNCT, s + r, 1);
                    i = r < n ? r + 1 : n;
                } else if (isalpha((unsigned char)s[i])) {
                    size_t r = i;
                    while (r < n && (isalnum((unsigned char)s[r]) || s[r] == '-' || s[r] == ':')) r++;
                    em(e, H_VARIABLE, s + i, r - i); /* entity.other.attribute-name */
                    i = r;
                } else if (s[i] == '=' || s[i] == '/') {
                    em(e, H_PUNCT, s + i, 1);
                    i++;
                } else {
                    em(e, H_NONE, s + i, 1);
                    i++;
                }
            }
            if (i < n) {
                em(e, H_PUNCT, s + i, 1);
                i++;
            }
            continue;
        }
        size_t j = i;
        while (j < n && s[j] != '<') j++;
        em(e, H_NONE, s + i, j - i);
        i = j;
    }
}

static void lex_css(em_t *e, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (st->mode == 1) {
            const char *end = strstr(s + i, "*/");
            size_t stop = end && (size_t)(end - s) < n ? (size_t)(end - s) : n;
            em(e, H_COMMENT, s + i, stop - i);
            if (stop < n) {
                em(e, H_PUNCT, s + stop, 2);
                st->mode = 0;
                i = stop + 2;
            } else i = n;
            continue;
        }
        char c = s[i];
        if (!strncmp(s + i, "/*", 2)) {
            em(e, H_PUNCT, s + i, 2);
            i += 2;
            st->mode = 1;
        } else if (c == '{' || c == '}') {
            if (c == '{') st->nf = 1;
            else st->nf = 0;
            em(e, H_PUNCT, s + i, 1);
            i++;
        } else if (strchr(":;,()", c)) {
            em(e, H_PUNCT, s + i, 1);
            i++;
        } else if (isdigit((unsigned char)c) || (c == '#' && st->nf)) {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '%')) j++;
            em(e, H_NUMBER, s + i, j - i);
            i = j;
        } else if (isalpha((unsigned char)c) || c == '-' || c == '.' || c == '#') {
            size_t j = i + 1;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_')) j++;
            size_t k = j;
            while (k < n && s[k] == ' ') k++;
            int cat = st->nf ? (k < n && s[k] == ':' ? H_TYPE : H_NONE) : (c == '.' || c == '#') ? H_TYPE : H_KEYWORD;
            em(e, cat, s + i, j - i);
            i = j;
        } else {
            em(e, H_NONE, s + i, 1);
            i++;
        }
    }
}

void tui_highlight_code(const char *code, const char *lang, const tui_theme_t *th, strvec_t *out) {
    if (!th) th = tui_theme();
    char *clean = tui_replace_tabs(code ? code : "");
    strvec_t lines = str_split_lines(clean, strlen(clean));
    if (!lines.n) strvec_push(&lines, "");
    const lang_t *L = find_lang(lang);
    hs_t st;
    memset(&st, 0, sizeof st);
    for (size_t i = 0; i < lines.n; i++) {
        /* control characters would break the layout */
        for (char *p = lines.v[i]; *p; p++)
            if ((unsigned char)*p < 0x20) *p = ' ';
        const char *s = lines.v[i];
        size_t n = strlen(s);
        sb_t b = SB_INIT;
        em_t e = {&b, th, H_NONE};
        if (!L) sb_append(&b, s);
        else {
            switch (L->fam) {
            case F_SH: lex_shell(&e, L, &st, s, n); break;
            case F_JSON: lex_json(&e, L, &st, s, n); break;
            case F_YAML: lex_yaml(&e, L, &st, s, n); break;
            case F_TOML: lex_toml(&e, L, &st, s, n); break;
            case F_DIFF: lex_diff(&e, s, n); break;
            case F_HTML: lex_html(&e, &st, s, n); break;
            case F_CSS: lex_css(&e, &st, s, n); break;
            default: lex_code(&e, L, &st, s, n); break;
            }
            em_end(&e);
            /* an unterminated single-line string ends at the line end (except multi-line kinds) */
            if (st.mode == 2 && strlen(st.delim) == 1 && st.delim[0] != '`' && L->fam != F_SH) st.mode = 0;
            if (L->fam == F_PY && st.mode == 0) {
                /* python statements end at the line end: an open call frame continues only inside brackets */
            }
        }
        strvec_push_own(out, sb_detach(&b));
    }
    strvec_free(&lines);
    free(clean);
}
