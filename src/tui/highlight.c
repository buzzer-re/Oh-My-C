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
 *   - Python builtin types and exceptions are type color even when called (their call's arguments stay function
 *     color), except in a class's base list
 *   - JS/TS (probed against omp, tests/tui/golden/hlts.ansi): identifiers are variable color (no function-call
 *     color for arguments); bound names take function color for a function value; object literal keys, `extends`
 *     targets and properties stay plain (TypeScript colors properties except in an assignment target); `new Foo` is
 *     function color in TypeScript, variable in JavaScript; annotation and conditional colons are keyword color;
 *     TypeScript leaves call / control-statement parens and index brackets unscoped, JavaScript punctuates them
 * Unsupported languages render verbatim, as omp does. */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "tui/markdown.h"

#include <ctype.h>
#include <strings.h>

enum { H_NONE = -1, H_COMMENT, H_KEYWORD, H_FUNCTION, H_VARIABLE, H_STRING, H_NUMBER, H_TYPE, H_OPERATOR, H_PUNCT,
       H_INSERTED, H_DELETED };

static const tui_tc_t HSLOT[] = {TUI_TC_SYNTAX_COMMENT,  TUI_TC_SYNTAX_KEYWORD, TUI_TC_SYNTAX_FUNCTION,
                                 TUI_TC_SYNTAX_VARIABLE, TUI_TC_SYNTAX_STRING,  TUI_TC_SYNTAX_NUMBER,
                                 TUI_TC_SYNTAX_TYPE,     TUI_TC_SYNTAX_OPERATOR, TUI_TC_SYNTAX_PUNCTUATION,
                                 TUI_TC_TOOL_DIFF_ADDED, TUI_TC_TOOL_DIFF_REMOVED};

typedef enum { F_C, F_JS, F_PY, F_SH, F_JSON, F_YAML, F_TOML, F_SQL, F_DIFF, F_RUBY, F_LUA, F_HTML, F_CSS, F_REGEX } family_t;

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

/* JavaScript builtin classes (support.class): `new Error` is type color, `new Foo` function color */
static const char JS_CLASSES[] = " Array ArrayBuffer BigInt Boolean DataView Date Error EvalError Float32Array Float64Array "
                                 "Function Int8Array Int16Array Int32Array Intl JSON Map Math Number Object Promise Proxy "
                                 "RangeError ReferenceError Reflect RegExp Set String Symbol SyntaxError TypeError URIError "
                                 "Uint8Array Uint16Array Uint32Array WeakMap WeakRef WeakSet ";

static const lang_t LANGS[] = {
    /* syntect's bundled RegExp.sublime-syntax: its `re` extension and its name (grep headers pass the name) */
    {" re regular expression ", F_REGEX, "", "", "", "", "", "", "", false, false, false},
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
     "", " int str float bool list dict set tuple bytes object type complex frozenset bytearray "
     /* support.type.exception.python */
     "ArithmeticError AssertionError AttributeError BaseException BlockingIOError BrokenPipeError BufferError "
     "BytesWarning ChildProcessError ConnectionAbortedError ConnectionError ConnectionRefusedError "
     "ConnectionResetError DeprecationWarning EnvironmentError EOFError Exception FileExistsError FileNotFoundError "
     "FloatingPointError FutureWarning GeneratorExit ImportError ImportWarning IndentationError IndexError "
     "InterruptedError IOError IsADirectoryError KeyboardInterrupt KeyError LookupError MemoryError "
     "ModuleNotFoundError NameError NotADirectoryError NotImplementedError OSError OverflowError "
     "PendingDeprecationWarning PermissionError ProcessLookupError RecursionError ReferenceError ResourceWarning "
     "RuntimeError RuntimeWarning StopAsyncIteration StopIteration SyntaxError SyntaxWarning SystemError SystemExit "
     "TabError TimeoutError TypeError UnboundLocalError UnicodeDecodeError UnicodeEncodeError UnicodeError "
     "UnicodeTranslateError UnicodeWarning UserWarning ValueError Warning ZeroDivisionError ",
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
    {" zig zon ", F_C, /* Zig.sublime-syntax (omp 3411531f39) */
     " while for break return continue asm defer errdefer unreachable async await suspend nosuspend resume if else "
     "switch try catch orelse usingnamespace and or const var extern packed export pub noalias inline noinline "
     "comptime volatile align linksection threadlocal allowzero test struct enum union error fn anyframe ",
     " i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f16 f32 f64 f80 f128 bool void noreturn type anyerror "
     "anyopaque anytype comptime_int comptime_float c_int c_uint c_long c_ulong c_char ",
     "", " null undefined true false ", "", " fn ", " struct enum union ", true, false, false},
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
    bool py_class;   /* python: a `class` line */
    bool py_bases;   /* python: inside its base list */
    bool bind;       /* js: after var/let/const, the names it binds (until `=`, `:`, `;`, of/in or the line end) */
    int bind_depth;  /* js: `{` / `[` nesting of a destructuring pattern */
    bool js;         /* JS/TS grammar: no meta.function-call color for unscoped text */
    bool ts_type;    /* ts: a `type` / `interface` statement, whose braces hold type members ('T' frames) */
    bool after_dot;  /* js: the previous token was a `.` accessor */
    int ternary;     /* js: open `?` of a conditional expression */
} hs_t;

static bool in_call(const hs_t *st) {
    for (int i = MIN(st->nf, MAX_FRAMES) - 1; i >= 0; i--) { /* frames past the cap are plain parens */
        if (st->frames[i] == 'c') return true;
        if (st->frames[i] == '{' || st->frames[i] == 'T' || st->frames[i] == 'p') return false;
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
static int plain(const hs_t *st) { return !st->js && in_call(st) ? H_FUNCTION : H_NONE; }

static char top_frame(const hs_t *st) { return st->nf ? st->frames[MIN(st->nf, MAX_FRAMES) - 1] : 0; }

/* ts: the property at s[j] ends a member chain that is assigned (`a.b.c = x`, not `==` / `=>`) */
static bool js_assigned_chain(const char *s, size_t j, size_t n) {
    for (;;) {
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '.' && j + 1 < n && (isalpha((unsigned char)s[j + 1]) || s[j + 1] == '_' || s[j + 1] == '$')) {
            j++;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '$')) j++;
            continue;
        }
        break;
    }
    return j < n && s[j] == '=' && !(j + 1 < n && (s[j + 1] == '=' || s[j + 1] == '>'));
}

/* js: the `(` at s[k] closes on this line into `{`, `=>` or a TypeScript `:` return type: a parameter list */
static bool js_params_at(const char *s, size_t k, size_t n) {
    int depth = 0;
    for (; k < n; k++) {
        if (s[k] == '(') depth++;
        else if (s[k] == ')' && --depth == 0) break;
    }
    if (k >= n) return false;
    k++;
    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
    return k < n && (s[k] == '{' || s[k] == ':' || (s[k] == '=' && k + 1 < n && s[k + 1] == '>'));
}

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

/* js: after a bound name at s[j], `= function`, `= async`, `= (...) =>` or `= name =>` (a function value) */
static bool js_function_value(const char *s, size_t j, size_t n) {
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j >= n || s[j] != '=' || (j + 1 < n && (s[j + 1] == '=' || s[j + 1] == '>'))) return false;
    j++;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if ((n - j >= 8 && !strncmp(s + j, "function", 8) && (j + 8 == n || !ident_char(s[j + 8]))) ||
        (n - j >= 5 && !strncmp(s + j, "async", 5) && (j + 5 == n || !ident_char(s[j + 5]))))
        return true;
    size_t k = j;
    if (k < n && s[k] == '(') {
        int depth = 0;
        for (; k < n; k++) {
            if (s[k] == '(') depth++;
            else if (s[k] == ')' && --depth == 0) break;
        }
        if (k >= n) return false;
        k++;
    } else {
        while (k < n && ident_char(s[k])) k++;
        if (k == j) return false;
    }
    while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
    return k + 1 < n && s[k] == '=' && s[k + 1] == '>';
}

static void lex_code(em_t *e, const lang_t *L, hs_t *st, const char *s, size_t n) {
    size_t i = 0;
    bool py = L->fam == F_PY, js = L->fam == F_JS, sql = L->fam == F_SQL;
    bool ts = js && strstr(L->names, " ts ") != NULL; /* TypeScript's grammar differs from JavaScript's in places */
    st->bind = false; /* a declaration's names end with its line here */
    st->py_class = st->py_bases = false;
    st->js = js;
    st->ts_type = st->after_dot = false;
    st->ternary = 0;
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
        bool was_dot = st->after_dot;
        st->after_dot = false;
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
            } else if (st->expect == 3) {
                cat = H_NONE; /* js: the class `extends` names (entity.other.inherited-class) */
                st->expect = 0;
            } else if (st->expect == 4) {
                /* js: `new Name` (TypeScript names a constructor call, JavaScript leaves it a variable) */
                cat = word_in(JS_CLASSES, s + i, wl) ? H_TYPE : ts ? H_FUNCTION : H_VARIABLE;
                st->expect = 0;
            } else if (sql ? word_in_ci(L->keywords, s + i, wl) : word_in(L->keywords, s + i, wl)) {
                cat = H_KEYWORD;
                if (js && word_in(" var let const ", s + i, wl)) st->bind = true, st->bind_depth = 0;
                if (py && wl == 5 && !strncmp(s + i, "class", 5)) st->py_class = true;
                else if (js && word_in(" of in ", s + i, wl)) st->bind = false;
                if (word_in(L->defkw, s + i, wl)) st->expect = 1;
                else if (word_in(L->classkw, s + i, wl)) st->expect = 2;
                if (js && wl == 7 && !strncmp(s + i, "extends", 7)) st->expect = 3;
                else if (js && wl == 3 && !strncmp(s + i, "new", 3)) st->expect = 4;
                if (js && strstr(L->names, " ts ") && word_in(" type interface ", s + i, wl)) st->ts_type = true;
            } else if (sql ? word_in_ci(L->types, s + i, wl) : word_in(L->types, s + i, wl)) {
                cat = H_KEYWORD; /* storage.type */
            } else if (word_in(L->constants, s + i, wl) || (sql && word_in_ci(L->constants, s + i, wl))) {
                cat = H_NUMBER;
            } else if (py && (!strncmp(s + i, "self", 4) || !strncmp(s + i, "cls", 3)) && (wl == 4 || wl == 3) &&
                       (wl == 4 ? !strncmp(s + i, "self", 4) : !strncmp(s + i, "cls", 3))) {
                cat = H_VARIABLE; /* variable.language */
            } else if (js && wl == 4 && !strncmp(s + i, "this", 4)) {
                cat = H_VARIABLE;
            } else if (py && word_in(L->support, s + i, wl) && !st->py_bases) {
                /* support.type(.exception).python, called or not, beats meta.function-call; class bases stay plain */
                cat = H_TYPE;
            } else if (js && st->bind && !word_in(L->keywords, s + i, wl)) {
                /* the names a var/let/const binds (variable.other.readwrite / .constant); a function value names a
                 * function (entity.name.function) */
                cat = st->bind_depth == 0 && js_function_value(s, j, n) ? H_FUNCTION : H_VARIABLE;
            } else if (call) {
                cat = H_FUNCTION;
            } else if (js && was_dot && (!ts || js_assigned_chain(s, j, n))) {
                cat = H_NONE; /* a property: plain in JavaScript, and in an assignment target in TS */
            } else if (js && top_frame(st) == '{' && !st->ternary && k < n && s[k] == ':' && !(k + 1 < n && s[k + 1] == ':')) {
                cat = H_NONE; /* an object literal key */
            } else if (!st->py_bases && (word_in(L->support, s + i, wl) || (L->caps_types && isupper((unsigned char)c) && wl > 1 &&
                                                          !(wl > 2 && isupper((unsigned char)s[i + 1]) && isupper((unsigned char)s[j - 1]))))) {
                cat = H_TYPE;
            } else if (word_in(L->builtins, s + i, wl)) {
                cat = H_FUNCTION;
            } else if (st->nf && st->frames[st->nf - 1 < MAX_FRAMES ? st->nf - 1 : MAX_FRAMES - 1] == 'p') {
                cat = H_VARIABLE; /* parameters of a definition */
            } else if (in_call(st) && k < n && s[k] == '=' && !(k + 1 < n && s[k + 1] == '=')) {
                cat = H_VARIABLE; /* keyword argument */
            } else {
                cat = js ? H_VARIABLE : plain(st); /* JS/TS: variable.other.readwrite */
            }
            if (macro) cat = H_NONE;
            em(e, cat, s + i, wl);
            i = j;
            if (call && (cat == H_FUNCTION || (py && cat == H_TYPE && !st->py_class && word_in(L->support, s + i - wl, wl)) ||
                         (js && cat == H_TYPE))) {
                /* a definition's parameter list vs a call's arguments */
                bool def = (cat == H_FUNCTION && (st->last == 3 || (L->fam == F_C && st->last == 1 && st->nf == 0))) ||
                           (js && js_params_at(s, k, n));
                st->last = def ? 4 : 5;
            } else if (js && cat == H_KEYWORD && word_in(" if for while switch catch with ", s + i - wl, wl)) {
                st->last = 7; /* its `(` is unscoped */
            } else {
                st->last = cat == H_KEYWORD ? (word_in(L->defkw, s + i - wl, wl) ? 3 : 2) : 1;
            }
            continue;
        }
        if (c == '(') {
            if (py && st->last == 1 && st->py_class && st->nf == 0) st->py_bases = true; /* class Name( */
            st->expect = 0; /* an anonymous `function (` names nothing */
            char fk = st->last == 4 ? 'p' : st->last == 5 ? 'c' : '(';
            /* js: a call's or a control statement's parens are unscoped; `function (`, `(...) =>` are parameters */
            if (ts && st->last == 7) fk = 'k';
            else if (js && (st->last == 3 || (fk == '(' && js_params_at(s, i, n)))) fk = 'p';
            push_frame(st, fk);
            em(e, ts && (fk == 'c' || fk == 'k') ? H_NONE : H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        if (c == '[' || c == '{') {
            /* js: brackets are unscoped outside a destructuring pattern; a type statement's braces hold members */
            char fk = ts && c == '[' && !st->bind ? 'b' : js && c == '{' && st->ts_type ? 'T' : c;
            if (st->bind) st->bind_depth++;
            push_frame(st, fk);
            em(e, fk == 'b' ? H_NONE : H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (st->bind && c != ')' && st->bind_depth > 0) st->bind_depth--;
            else if (c == ')') st->bind = false;
            if (c == ')') st->py_bases = false;
            char fk = pop_frame(st);
            em(e, ts && (fk == 'c' || fk == 'k' || fk == 'b') ? H_NONE : H_PUNCT, s + i, 1);
            i++;
            st->last = 1;
            continue;
        }
        if (c == '.' || (c == '-' && i + 1 < n && s[i + 1] == '>' && L->fam == F_C) || (c == ':' && i + 1 < n && s[i + 1] == ':')) {
            size_t l = c == '.' ? 1 : 2;
            if (c == '.' && i + 2 < n && s[i + 1] == '.' && s[i + 2] == '.') l = 3;
            em(e, l == 3 ? H_KEYWORD : H_OPERATOR, s + i, l); /* punctuation.accessor */
            if (c == '.' && l == 1) st->after_dot = true;
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
            if (c == '=' && st->bind_depth == 0) st->bind = false;
            size_t l = 1;
            while (i + l < n && l < 3 && strchr("+-*/%=<>!&|^~?", s[i + l])) l++;
            if (js && c == '?' && l == 1 && !(i + 1 < n && s[i + 1] == '.')) st->ternary++;
            em(e, H_KEYWORD, s + i, l); /* keyword.operator */
            i += l;
            st->last = 0;
            continue;
        }
        if (strchr(",;:", c)) {
            /* TypeScript: a type annotation's colon is keyword.operator.type.annotation */
            bool annot = c == ':' && strstr(L->names, " ts ") &&
                         ((st->bind && st->bind_depth == 0) || top_frame(st) == 'p' || top_frame(st) == 'T');
            if (js && c == ':' && st->ternary > 0) annot = true, st->ternary--; /* the conditional's `:` operator */
            if (c == ';' || (c == ':' && st->bind_depth == 0)) st->bind = false;
            em(e, annot ? H_KEYWORD : H_PUNCT, s + i, 1);
            i++;
            st->last = 0;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        /* JavaScript: the space between `new` and its constructor is in the call's (function color) scope */
        em(e, js && !ts && st->expect == 4 ? H_FUNCTION : plain(st), s + i, (size_t)k);
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

/* ---------- Regular Expression (RegExp.sublime-syntax under omp's scope mapping) ----------
 * Observed against omp's native highlighter: anchors, class escapes, `.`, quantifiers, `|`, group parens and set
 * brackets are keyword; group-type marks (`?:` `?=` `?!` `?<=` `?<!` `?>`), set ranges and POSIX class names are
 * number; other escapes are string; name delimiters and comment groups are punctuation; group / backreference names
 * and numbers are variable; comment text is comment. */

static bool rx_class_escape(char c) { return c && strchr("bBAzZGdDwWsShHvVRXK", c) != NULL; }

static bool rx_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* `<name>` / `'name'` / `{name}`: punctuation, variable, punctuation; returns the bytes taken (0: no match) */
static size_t rx_name_ex(em_t *e, const char *s, size_t n, size_t i, bool allow_num, bool emit);
static size_t rx_name(em_t *e, const char *s, size_t n, size_t i, bool allow_num) {
    return rx_name_ex(e, s, n, i, allow_num, true);
}

static size_t rx_name_ex(em_t *e, const char *s, size_t n, size_t i, bool allow_num, bool emit) {
    if (i >= n) return 0;
    char open = s[i], close = open == '<' ? '>' : open == '{' ? '}' : open == '\'' ? '\'' : 0;
    if (!close) return 0;
    size_t j = i + 1;
    if (allow_num && j < n && (s[j] == '-' || s[j] == '+')) j++;
    size_t k = j;
    while (k < n && rx_word(s[k])) k++;
    if (k == j || k >= n || s[k] != close) return 0;
    if (!emit) return k + 1 - i;
    em(e, H_PUNCT, s + i, 1);
    em(e, H_VARIABLE, s + i + 1, k - i - 1);
    em(e, H_PUNCT, s + k, 1);
    return k + 1 - i;
}

/* a quantifier at s[i]: `*` `+` `?` or `{n}` `{n,}` `{n,m}`, then an optional lazy `?` / possessive `+` */
static size_t rx_quant(const char *s, size_t n, size_t i) {
    size_t j = i;
    if (s[i] == '*' || s[i] == '+' || s[i] == '?') j = i + 1;
    else if (s[i] == '{') {
        size_t k = i + 1, d = 0;
        while (k < n && isdigit((unsigned char)s[k])) k++, d++;
        if (!d) return 0;
        if (k < n && s[k] == ',') {
            k++;
            while (k < n && isdigit((unsigned char)s[k])) k++;
        }
        if (k >= n || s[k] != '}') return 0;
        j = k + 1;
    } else return 0;
    if (j < n && (s[j] == '?' || s[j] == '+')) j++;
    return j - i;
}

/* one escape outside or inside a set; returns its length */
static size_t rx_escape(em_t *e, const char *s, size_t n, size_t i, bool in_set) {
    if (i + 1 >= n) { /* a trailing backslash escapes nothing */
        em(e, H_NONE, s + i, 1);
        return 1;
    }
    char c = s[i + 1];
    if (!in_set && c >= '1' && c <= '9') { /* backreference */
        size_t k = i + 1;
        while (k < n && isdigit((unsigned char)s[k])) k++;
        em(e, H_KEYWORD, s + i, 1);
        em(e, H_VARIABLE, s + i + 1, k - i - 1);
        return k - i;
    }
    if (c == '0') { /* octal */
        size_t k = i + 2;
        while (k < n && k < i + 4 && s[k] >= '0' && s[k] <= '7') k++;
        em(e, H_STRING, s + i, k - i);
        return k - i;
    }
    if (!in_set && (c == 'k' || c == 'g')) {
        em(e, H_KEYWORD, s + i, 2);
        size_t t = rx_name(e, s, n, i + 2, c == 'g');
        if (!t && c == 'g') {
            size_t k = i + 2;
            if (k < n && (s[k] == '-' || s[k] == '+')) k++;
            size_t d = k;
            while (k < n && isdigit((unsigned char)s[k])) k++;
            if (k > d) {
                em(e, H_VARIABLE, s + i + 2, k - i - 2);
                t = k - i - 2;
            }
        }
        return 2 + t;
    }
    if (c == 'p' || c == 'P') {
        if (i + 2 < n && s[i + 2] == '{') {
            size_t k = i + 3;
            while (k < n && (rx_word(s[k]) || s[k] == '=' || s[k] == ' ' || s[k] == '-')) k++;
            if (k > i + 3 && k < n && s[k] == '}') {
                em(e, H_KEYWORD, s + i, k + 1 - i);
                return k + 1 - i;
            }
        } else if (i + 2 < n && isalpha((unsigned char)s[i + 2])) {
            em(e, H_KEYWORD, s + i, 3);
            return 3;
        }
        em(e, H_STRING, s + i, 2);
        return 2;
    }
    if (c == 'c' && i + 2 < n) { /* control character: no scope color */
        em(e, H_NONE, s + i, 3);
        return 3;
    }
    if (c == 'x') {
        if (i + 2 < n && s[i + 2] == '{') {
            size_t k = i + 3;
            while (k < n && isxdigit((unsigned char)s[k])) k++;
            if (k < n && s[k] == '}') {
                em(e, H_STRING, s + i, k + 1 - i);
                return k + 1 - i;
            }
        }
        if (i + 3 < n && isxdigit((unsigned char)s[i + 2]) && isxdigit((unsigned char)s[i + 3])) {
            em(e, H_STRING, s + i, 4);
            return 4;
        }
        em(e, H_NONE, s + i, 2); /* `\x` without two hex digits is no escape the grammar colors */
        return 2;
    }
    if (c == 'u') {
        size_t k = i + 2;
        while (k < n && k < i + 6 && isxdigit((unsigned char)s[k])) k++;
        if (k - i - 2 != 4) k = i + 2;
        em(e, H_STRING, s + i, k - i);
        return k - i;
    }
    if (rx_class_escape(c)) {
        em(e, H_KEYWORD, s + i, 2);
        return 2;
    }
    /* multi-byte escaped character: keep its whole UTF-8 sequence */
    size_t l = 1;
    while (i + 1 + l < n && ((unsigned char)s[i + 1 + l] & 0xC0) == 0x80) l++;
    em(e, H_STRING, s + i, 1 + l);
    return 1 + l;
}

/* an escape's length inside a set, without emitting it */
static size_t rx_escape_len(em_t *e, const char *s, size_t n, size_t i) {
    sb_t tmp = SB_INIT;
    em_t t = {&tmp, e->th, H_NONE};
    size_t l = rx_escape(&t, s, n, i, true);
    sb_free(&tmp);
    return l;
}

static const char *const RX_POSIX[] = {"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph",
                                       "lower", "print", "punct", "space", "upper", "word",  "xdigit"};

/* a set body from s[i] (just past `[` / `[^`); returns the index past its closing `]` (or n) */
static size_t rx_set(em_t *e, const char *s, size_t n, size_t i, int depth) {
    size_t start = i;
    while (i < n) {
        char c = s[i];
        if (c == ']' && i > start) {
            em(e, H_KEYWORD, s + i, 1);
            return i + 1;
        }
        if (c == '[' && i + 1 < n && s[i + 1] == ':') {
            size_t k = i + 2;
            while (k < n && isalpha((unsigned char)s[k])) k++;
            bool known = false;
            for (size_t q = 0; q < ARRAY_LEN(RX_POSIX); q++)
                if (k - i - 2 == strlen(RX_POSIX[q]) && !strncmp(s + i + 2, RX_POSIX[q], k - i - 2)) known = true;
            if (known && k + 1 < n && s[k] == ':' && s[k + 1] == ']') {
                em(e, H_KEYWORD, s + i, 1);
                em(e, H_NUMBER, s + i + 1, k + 1 - i - 1);
                em(e, H_KEYWORD, s + k + 1, 1);
                i = k + 2;
                continue;
            }
        }
        if (c == '[' && depth < 8) { /* a nested set */
            size_t k = i + 1;
            if (k < n && s[k] == '^') k++;
            em(e, H_KEYWORD, s + i, k - i);
            i = rx_set(e, s, n, k, depth + 1);
            continue;
        }
        /* a range: endpoint '-' endpoint, literal endpoints colored as the range */
        size_t a_len = c == '\\' ? rx_escape_len(e, s, n, i) : 1;
        while (c != '\\' && i + a_len < n && ((unsigned char)s[i + a_len] & 0xC0) == 0x80) a_len++;
        if (i + a_len + 1 < n && s[i + a_len] == '-' && s[i + a_len + 1] != ']') {
            size_t b = i + a_len + 1;
            if (s[b] != '[') { /* escaped endpoints (class escapes too) read as escaped characters */
                if (c == '\\') em(e, H_STRING, s + i, a_len);
                else em(e, H_NUMBER, s + i, a_len);
                em(e, H_NUMBER, s + i + a_len, 1);
                size_t bl;
                if (s[b] == '\\') {
                    bl = rx_escape_len(e, s, n, b);
                    em(e, H_STRING, s + b, bl);
                } else {
                    bl = 1;
                    while (b + bl < n && ((unsigned char)s[b + bl] & 0xC0) == 0x80) bl++;
                    em(e, H_NUMBER, s + b, bl);
                }
                i = b + bl;
                continue;
            }
        }
        if (c == '\\') {
            i += rx_escape(e, s, n, i, true);
            continue;
        }
        em(e, H_NONE, s + i, 1);
        i++;
    }
    return n;
}

static void lex_regex(em_t *e, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        size_t q;
        if (c == '\\') {
            if (i + 1 < n && s[i + 1] == 'Q') { /* \Q … \E: literal text */
                em(e, H_KEYWORD, s + i, 2);
                const char *end = n > i + 2 ? memmem(s + i + 2, n - i - 2, "\\E", 2) : NULL;
                size_t stop = end ? (size_t)(end - s) : n;
                em(e, H_NONE, s + i + 2, stop - i - 2);
                if (end) em(e, H_KEYWORD, end, 2);
                i = end ? stop + 2 : n;
                continue;
            }
            i += rx_escape(e, s, n, i, false);
        } else if (c == '^' || c == '$' || c == '.' || c == '|' || c == ')') {
            em(e, H_KEYWORD, s + i, 1);
            i++;
        } else if (c == '[') {
            size_t k = i + 1;
            if (k < n && s[k] == '^') k++;
            em(e, H_KEYWORD, s + i, k - i);
            i = rx_set(e, s, n, k, 0);
        } else if (c == '(') {
            size_t k = i + 1;
            if (k < n && s[k] == '*') { /* (*VERB) / (*VERB:arg) */
                size_t v = k + 1;
                while (v < n && (isupper((unsigned char)s[v]) || s[v] == '_')) v++;
                if (v > k + 1 && v < n && s[v] == ':') while (v < n && s[v] != ')') v++;
                if (v > k + 1 && v < n && s[v] == ')') {
                    em(e, H_KEYWORD, s + i, v + 1 - i);
                    i = v + 1;
                    continue;
                }
            }
            if (k >= n || s[k] != '?') {
                em(e, H_KEYWORD, s + i, 1);
                i++;
                continue;
            }
            const char *t = s + k + 1;
            size_t rest = n - k - 1;
            if (rest && *t == '#') { /* (?# comment ) */
                const char *close = memchr(t, ')', rest);
                em(e, H_PUNCT, s + i, 3);
                size_t stop = close ? (size_t)(close - s) : n;
                em(e, H_COMMENT, s + i + 3, stop - i - 3);
                if (close) em(e, H_PUNCT, close, 1);
                i = close ? stop + 1 : n;
            } else if (rest && (*t == ':' || *t == '=' || *t == '!' || *t == '>')) {
                em(e, H_KEYWORD, s + i, 1);
                em(e, H_NUMBER, s + k, 2);
                i = k + 2;
            } else if (rest >= 2 && *t == '<' && (t[1] == '=' || t[1] == '!')) {
                em(e, H_KEYWORD, s + i, 1);
                em(e, H_NUMBER, s + k, 3);
                i = k + 3;
            } else if (rest && (*t == '<' || *t == '\'') && rx_name_ex(e, s, n, k + 1, false, false) > 0) {
                em(e, H_KEYWORD, s + i, 2); /* (?<name> / (?'name' */
                i = k + 1 + rx_name(e, s, n, k + 1, false);
            } else if (rest && *t == '(') { /* (?(cond)…) */
                em(e, H_KEYWORD, s + i, 3);
                size_t j = k + 2, d = j;
                while (j < n && isdigit((unsigned char)s[j])) j++;
                if (j > d) em(e, H_VARIABLE, s + d, j - d);
                else j += rx_name(e, s, n, j, false);
                i = j;
            } else if (rest && (*t == '&' || *t == '+' || *t == '-' || isdigit((unsigned char)*t))) {
                size_t j = k + 1;
                if (*t == '&') {
                    j++;
                    size_t w = j;
                    while (w < n && rx_word(s[w])) w++;
                    if (w > j && w < n && s[w] == ')') {
                        em(e, H_KEYWORD, s + i, j - i);
                        em(e, H_VARIABLE, s + j, w - j);
                        i = w;
                        continue;
                    }
                } else {
                    size_t w = j + (*t == '+' || *t == '-');
                    size_t d0 = w;
                    while (w < n && isdigit((unsigned char)s[w])) w++;
                    if (w > d0 && w < n && s[w] == ')') {
                        em(e, H_KEYWORD, s + i, 2);
                        em(e, H_VARIABLE, s + j, w - j);
                        i = w;
                        continue;
                    }
                }
                goto flags;
            } else {
            flags:;
                /* (?flags) / (?flags:  — imnsxu, optionally `-` and more */
                size_t j = k + 1;
                while (j < n && strchr("imnsxu", s[j]) && s[j]) j++;
                bool ok = j > k + 1;
                if (j < n && s[j] == '-') {
                    size_t f = ++j;
                    while (j < n && strchr("imnsxu", s[j]) && s[j]) j++;
                    ok = j > f; /* `(?i-)` / `(?-)`: a dash needs flags after it */
                }
                if (ok && j < n && (s[j] == ')' || s[j] == ':')) {
                    em(e, H_KEYWORD, s + i, j + 1 - i);
                    i = j + 1;
                } else { /* an unknown group form: the paren alone, its `?` literal */
                    em(e, H_KEYWORD, s + i, 1);
                    em(e, H_NONE, s + k, 1);
                    i = k + 1;
                }
            }
        } else if ((c == '*' || c == '+' || c == '?') && (i == 0 || s[i - 1] == '(' || s[i - 1] == '|')) {
            em(e, H_NONE, s + i, 1); /* nothing before it to repeat */
            i++;
        } else if ((q = rx_quant(s, n, i)) > 0) {
            em(e, H_KEYWORD, s + i, q);
            i += q;
        } else {
            size_t l = 1;
            while (i + l < n && ((unsigned char)s[i + l] & 0xC0) == 0x80) l++;
            em(e, H_NONE, s + i, l);
            i += l;
        }
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
            case F_REGEX: lex_regex(&e, s, n); break;
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
