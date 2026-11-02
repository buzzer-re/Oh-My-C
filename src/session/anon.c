/* anon.c - port of omp session/session-anonymizer.ts (SessionAnonymizer). See anon.h.
 *
 * Every field is exported by an explicit rule (anon_tables.h, generated from the upstream FIELD_RULES):
 * usage, timing, models, ids and tool names are kept; turn contents become size-annotated markers;
 * paths become mock paths; shell commands keep program names and flags; anything without a rule becomes
 * an opaque marker under a tokenized key. String lengths are JavaScript lengths (UTF-16 code units). */
#include "session/anon.h"

#include "session/anon_tables.h"

#include <ctype.h>

static json_t *json_take_str(char *s) {
    json_t *j = json_str(s);
    free(s);
    return j;
}

typedef enum {
    R_NONE = 0,
    R_NUM, R_TIME, R_ENUM, R_IDENTITY, R_AGENT, R_SPAWNS, R_ID, R_PATH, R_CMD, R_TEXT, R_LABEL, R_ERROR, R_TOOL,
    R_NAME, R_CUSTOMTYPE, R_CONTENT, R_STRUCT, R_ARGS, R_DETAILS, R_DATA, R_OPAQUE, R_TRANSFORMS, R_SESSIONPATH,
    R_RUNTIME,
} rule_t;

static const char *const RULE_NAMES[] = {"",     "num",   "time",  "enum",   "identity",   "agent",  "spawns",
                                         "id",   "path",  "cmd",   "text",   "label",      "error",  "tool",
                                         "name", "customType", "content", "struct", "args", "details", "data",
                                         "opaque", "transforms", "sessionPath", "runtime"};

#define MAX_PLACEHOLDER_LENGTH 120

struct anon {
    hmap_t tokens; /* value -> (index as pointer) */
    size_t ntokens;
    hmap_t tool_call_ids;
};

/* ---------- small helpers ---------- */

static bool in_list(const char *const *list, const char *s) {
    for (size_t i = 0; list[i]; i++)
        if (!strcmp(list[i], s)) return true;
    return false;
}

static rule_t field_rule(const char *key) {
    /* object spread: a later definition of the same key wins */
    const char *r = NULL;
    for (size_t i = 0; ANON_FIELD_RULES[i][0]; i++)
        if (!strcmp(ANON_FIELD_RULES[i][0], key)) r = ANON_FIELD_RULES[i][1];
    if (!r) return R_NONE;
    for (size_t i = 1; i < ARRAY_LEN(RULE_NAMES); i++)
        if (!strcmp(RULE_NAMES[i], r)) return (rule_t)i;
    return R_NONE;
}

/* TOOL_ENUM_VALUES[key]?.has(value) */
static bool tool_enum_has(const char *key, const char *value) {
    const char *const *p = ANON_TOOL_ENUM_VALUES;
    while (*p) {
        bool match = !strcmp(*p, key);
        p++;
        for (; *p; p++)
            if (match && !strcmp(*p, value)) return true;
        p++;
    }
    return false;
}

/* CONTENT_BLOCK_KEYS[type]: NULL-terminated key list, or NULL when the type is unknown */
static const char *const *content_block_keys(const char *type) {
    const char *const *p = ANON_CONTENT_BLOCK_KEYS;
    while (*p) {
        bool match = type && !strcmp(*p, type);
        p++;
        if (match) return p;
        while (*p) p++;
        p++;
    }
    return NULL;
}

static bool is_builtin_tool(const char *name) { return name && in_list(ANON_BUILTIN_TOOLS, name); }

/* JavaScript String.length: UTF-16 code units of a UTF-8 string */
static size_t js_length(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}

static bool is_word(int c) { return isalnum(c) || c == '_'; }
static bool all_digits(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* String(number) */
static char *js_num_string(const json_t *v) {
    sb_t b = SB_INIT;
    if (v->is_int) sb_appendf(&b, "%lld", (long long)v->u.num.i);
    else json_js_num_sb(&b, v->u.num.d);
    return sb_detach(&b);
}

static char *trim_js(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return xstrndup(s, n);
}

/* ---------- regex stand-ins ---------- */

/* ^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})$ */
static bool is_iso_timestamp(const char *s) {
    static const char shape[] = "dddd-dd-ddTdd:dd:dd";
    for (size_t i = 0; i < sizeof shape - 1; i++) {
        if (!s[i]) return false;
        if (shape[i] == 'd' ? !isdigit((unsigned char)s[i]) : s[i] != shape[i]) return false;
    }
    const char *p = s + sizeof shape - 1;
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
    }
    if (*p == 'Z') return p[1] == 0;
    if (*p == '+' || *p == '-')
        return isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && p[3] == ':' && isdigit((unsigned char)p[4]) &&
               isdigit((unsigned char)p[5]) && p[6] == 0;
    return false;
}

/* ^[\w.:/@+-]{1,128}$ */
static bool is_identifier(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || js_length(s) > 128) return false;
    for (size_t i = 0; i < n; i++)
        if (!is_word((unsigned char)s[i]) && !strchr(".:/@+-", s[i])) return false;
    return true;
}

/* (?:^id|Id|_id|Ids)$ */
static bool is_id_key(const char *k) {
    return !strcmp(k, "id") || str_ends(k, "Id") || str_ends(k, "_id") || str_ends(k, "Ids");
}

/* ^(?:[0-9a-f-]{6,}|[a-z]+_(?:\d+|(?=[\w|=-]*\d)[\w|=-]{6,}))$ (case-insensitive) */
static bool is_random_id(const char *s) {
    size_t n = strlen(s), i;
    for (i = 0; i < n && (isxdigit((unsigned char)s[i]) || s[i] == '-'); i++);
    if (i == n && n >= 6) return true;
    for (i = 0; i < n && isalpha((unsigned char)s[i]); i++);
    if (i == 0 || i >= n || s[i] != '_') return false;
    const char *r = s + i + 1;
    size_t rn = strlen(r);
    if (all_digits(r, rn)) return true;
    bool digit = false;
    for (size_t k = 0; k < rn; k++) {
        if (!is_word((unsigned char)r[k]) && !strchr("|=-", r[k])) return false;
        if (isdigit((unsigned char)r[k])) digit = true;
    }
    return digit && rn >= 6;
}

/* ^\d{4}-\d{2}-\d{2}T[\d-]+Z_[0-9a-f-]+(?:\.jsonl)?$ */
static bool is_session_stem(const char *s) {
    static const char shape[] = "dddd-dd-ddT";
    for (size_t i = 0; i < sizeof shape - 1; i++) {
        if (!s[i]) return false;
        if (shape[i] == 'd' ? !isdigit((unsigned char)s[i]) : s[i] != shape[i]) return false;
    }
    const char *p = s + sizeof shape - 1;
    const char *q = p;
    while (isdigit((unsigned char)*q) || *q == '-') q++;
    if (q == p || *q != 'Z' || q[1] != '_') return false;
    p = q + 2;
    q = p;
    while (isdigit((unsigned char)*q) || (*q >= 'a' && *q <= 'f') || *q == '-') q++;
    if (q == p) return false;
    return !*q || !strcmp(q, ".jsonl");
}

/* (?:^path|Path|^paths|Paths|^cwd|Cwd|Dir|Directory|Directories|^file|File|Files)$ */
static bool is_path_key(const char *k) {
    static const char *const exact[] = {"path", "paths", "cwd", "file", NULL};
    static const char *const ends[] = {"Path", "Paths", "Cwd", "Dir", "Directory", "Directories", "File", "Files", NULL};
    if (in_list(exact, k)) return true;
    for (size_t i = 0; ends[i]; i++)
        if (str_ends(k, ends[i])) return true;
    return false;
}

/* ^[a-zA-Z][\w+.-]*:\/\/ : length of the scheme incl. "://", 0 = none */
static size_t scheme_len(const char *s) {
    if (!isalpha((unsigned char)s[0])) return 0;
    size_t i = 1;
    while (is_word((unsigned char)s[i]) || s[i] == '+' || s[i] == '.' || s[i] == '-') i++;
    return strncmp(s + i, "://", 3) ? 0 : i + 3;
}

/* ^[a-zA-Z]:(?=[\\/]|$) */
static bool has_drive(const char *s) { return isalpha((unsigned char)s[0]) && s[1] == ':' && (!s[2] || s[2] == '/' || s[2] == '\\'); }

/* one READ_SELECTOR piece: raw|img|conflicts|\d[\d,+-]*|-\d+ */
static bool selector_piece(const char *s, size_t n) {
    if ((n == 3 && !strncmp(s, "raw", 3)) || (n == 3 && !strncmp(s, "img", 3)) || (n == 9 && !strncmp(s, "conflicts", 9))) return true;
    if (n >= 1 && isdigit((unsigned char)s[0])) {
        for (size_t i = 1; i < n; i++)
            if (!isdigit((unsigned char)s[i]) && !strchr(",+-", s[i])) return false;
        return true;
    }
    return n >= 2 && s[0] == '-' && all_digits(s + 1, n - 1);
}

/* (?::(?:...))+$ : start index of the leftmost match, or -1 */
static ssize_t read_selector_at(const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        if (s[i] != ':') continue;
        bool ok = true;
        size_t p = i;
        while (p < n && ok) {
            size_t e = p + 1;
            while (e < n && s[e] != ':') e++;
            ok = selector_piece(s + p + 1, e - p - 1);
            p = e;
        }
        if (ok) return (ssize_t)i;
    }
    return -1;
}

static bool ext_chars(const char *s, size_t n, size_t lo, size_t hi) {
    if (n < lo || n > hi) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i])) return false;
    return true;
}

/* (?:\.(?:test|spec|d))?\.[A-Za-z0-9]{1,8} over the whole string */
static bool ext_suffix(const char *s) {
    if (s[0] != '.') return false;
    if (ext_chars(s + 1, strlen(s + 1), 1, 8)) return true;
    static const char *const q[] = {"test", "spec", "d"};
    for (size_t i = 0; i < 3; i++) {
        size_t l = strlen(q[i]);
        if (!strncmp(s + 1, q[i], l) && s[1 + l] == '.' && ext_chars(s + 2 + l, strlen(s + 2 + l), 1, 8)) return true;
    }
    return false;
}

/* ^(.+?)((?:\.(?:test|spec|d))?\.[A-Za-z0-9]{1,8})$ : split index (start of group 2) or 0 */
static size_t extension_split(const char *s) {
    size_t n = strlen(s);
    for (size_t i = 1; i < n; i++)
        if (ext_suffix(s + i)) return i;
    return 0;
}

/* ^(?:\.[A-Za-z0-9]{1,10})+$ */
static bool is_ext_only(const char *s) {
    if (!*s) return false;
    while (*s) {
        if (*s != '.') return false;
        size_t n = 0;
        while (isalnum((unsigned char)s[1 + n])) n++;
        if (n < 1 || n > 10) return false;
        s += 1 + n;
    }
    return true;
}

static bool is_known_extension(const char *ext) {
    char *low = str_lower(ext);
    strvec_t parts = str_split(low, ".");
    free(low);
    strvec_t keep = {0};
    for (size_t i = 0; i < parts.n; i++)
        if (*parts.v[i]) strvec_push(&keep, parts.v[i]);
    strvec_free(&parts);
    bool ok = keep.n > 0 && in_list(ANON_KNOWN_EXTENSIONS, keep.v[keep.n - 1]);
    for (size_t i = 0; ok && i + 1 < keep.n; i++)
        ok = !strcmp(keep.v[i], "test") || !strcmp(keep.v[i], "spec") || !strcmp(keep.v[i], "d") || in_list(ANON_KNOWN_EXTENSIONS, keep.v[i]);
    strvec_free(&keep);
    return ok;
}

static bool is_glob_char(char c) { return c && strchr("*?[]{},", c); }

/* ^messages(?:\.\w+)+$ */
static bool is_message_address(const char *s) {
    if (strncmp(s, "messages", 8)) return false;
    const char *p = s + 8;
    if (!*p) return false;
    while (*p) {
        if (*p != '.' || !is_word((unsigned char)p[1])) return false;
        p++;
        while (is_word((unsigned char)*p)) p++;
    }
    return true;
}

/* ^--?[A-Za-z][\w-]*$ */
static bool is_shell_flag(const char *s) {
    if (s[0] != '-') return false;
    const char *p = s + 1;
    if (*p == '-') p++;
    if (!isalpha((unsigned char)*p)) return false;
    for (p++; *p; p++)
        if (!is_word((unsigned char)*p) && *p != '-') return false;
    return true;
}

/* ^\$\{?\w+\}?$ */
static bool is_env_ref(const char *s) {
    if (s[0] != '$') return false;
    const char *p = s + 1;
    if (*p == '{') p++;
    if (!is_word((unsigned char)*p)) return false;
    while (is_word((unsigned char)*p)) p++;
    if (*p == '}') p++;
    return !*p;
}

/* ^([A-Za-z_]\w*)=([\s\S]*)$ : length of the name, 0 = no match */
static size_t env_assign_name(const char *s) {
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') return 0;
    size_t i = 1;
    while (is_word((unsigned char)s[i])) i++;
    return s[i] == '=' ? i : 0;
}

static bool looks_like_path(const char *v) {
    if (!*v || strpbrk(v, " \t\n\r\f\v\"") || strchr("{[(", v[0])) return false;
    return scheme_len(v) || strpbrk(v, "/\\") || v[0] == '~' || has_drive(v);
}

/* ---------- token table ---------- */

static size_t token_index(anon_t *a, const char *value) {
    void *v = hmap_get(&a->tokens, value);
    if (v) return (size_t)(uintptr_t)v;
    a->ntokens++;
    hmap_put(&a->tokens, value, (void *)(uintptr_t)a->ntokens);
    return a->ntokens;
}

anon_t *anon_new(void) {
    anon_t *a = xcalloc(1, sizeof *a);
    hmap_init(&a->tokens);
    hmap_init(&a->tool_call_ids);
    return a;
}

void anon_free(anon_t *a) {
    if (!a) return;
    hmap_free(&a->tokens, NULL);
    hmap_free(&a->tool_call_ids, NULL);
    free(a);
}

char *anon_placeholder(anon_t *a, const char *value) {
    if (!*value) return xstrdup("");
    return xasprintf("PLACEHOLDER_%zu", token_index(a, value));
}

char *anon_redact(anon_t *a, const char *value) {
    if (!*value) return xstrdup("");
    size_t lines = 1;
    for (const char *p = value; *p; p++)
        if (*p == '\n') lines++;
    return xasprintf("[redacted #%zu: %zu chars, %zu line%s]", token_index(a, value), js_length(value), lines, lines == 1 ? "" : "s");
}

static char *name_of(anon_t *a, const char *value) {
    if (value[0] == '.' && value[1]) {
        char *inner = name_of(a, value + 1);
        char *r = xasprintf(".%s", inner);
        free(inner);
        return r;
    }
    size_t sp = extension_split(value);
    if (sp) {
        char *m1 = xstrndup(value, sp);
        const char *m2 = value + sp;
        if (!all_digits(m1, strlen(m1)) && is_known_extension(m2)) {
            char *r = in_list(ANON_KEEP_SEGMENTS, m1) ? xasprintf("%s%s", m1, m2) : xasprintf("seg%zu%s", token_index(a, value), m2);
            free(m1);
            return r;
        }
        free(m1);
    }
    return xasprintf("seg%zu", token_index(a, value));
}

char *anon_segment(anon_t *a, const char *value) {
    if (!*value || !strcmp(value, ".") || !strcmp(value, "..") || !strcmp(value, "~")) return xstrdup(value);
    if (in_list(ANON_KEEP_SEGMENTS, value)) return xstrdup(value);
    bool glob = false;
    for (const char *p = value; *p; p++)
        if (is_glob_char(*p)) glob = true;
    if (!glob) return name_of(a, value);
    /* split on glob chars, keeping them */
    sb_t b = SB_INIT;
    const char *p = value;
    while (*p) {
        if (is_glob_char(*p)) {
            sb_putc(&b, *p++);
            continue;
        }
        size_t n = 0;
        while (p[n] && !is_glob_char(p[n])) n++;
        char *piece = xstrndup(p, n);
        if (is_ext_only(piece) && is_known_extension(piece)) sb_append(&b, piece);
        else {
            char *m = name_of(a, piece);
            sb_append(&b, m);
            free(m);
        }
        free(piece);
        p += n;
    }
    return sb_detach(&b);
}

static char *single_path(anon_t *a, const char *raw) {
    char *t = trim_js(raw);
    bool blank = !*t;
    free(t);
    if (blank) return xstrdup(raw);
    char *rest = xstrdup(raw);
    char *prefix = xstrdup(""), *suffix = xstrdup("");
    size_t sl = scheme_len(rest);
    if (sl) {
        char *name = xstrndup(rest, sl - 3);
        char *low = str_lower(name);
        free(prefix);
        if (in_list(ANON_KNOWN_SCHEMES, low)) prefix = xstrndup(rest, sl);
        else {
            char *ph = anon_placeholder(a, name);
            prefix = xasprintf("%s://", ph);
            free(ph);
        }
        free(low);
        free(name);
        char *r2 = xstrdup(rest + sl);
        free(rest);
        rest = r2;
        char *q = strchr(rest, '?');
        if (q) {
            char *ph = anon_placeholder(a, q + 1);
            free(suffix);
            suffix = xasprintf("?%s", ph);
            free(ph);
            *q = 0;
        }
    } else if (has_drive(rest)) {
        free(prefix);
        prefix = xstrndup(rest, 2);
        char *r2 = xstrdup(rest + 2);
        free(rest);
        rest = r2;
    }
    ssize_t sel = read_selector_at(rest);
    if (sel > 0) {
        char *ns = xasprintf("%s%s", rest + sel, suffix);
        free(suffix);
        suffix = ns;
        rest[sel] = 0;
    }
    sb_t b = SB_INIT;
    sb_append(&b, prefix);
    for (const char *p = rest; *p;) {
        size_t n = 0;
        if (*p == '/' || *p == '\\') {
            while (p[n] == '/' || p[n] == '\\') n++;
            sb_appendn(&b, p, n);
        } else {
            while (p[n] && p[n] != '/' && p[n] != '\\') n++;
            char *piece = xstrndup(p, n);
            char *m = anon_segment(a, piece);
            sb_append(&b, m);
            free(m);
            free(piece);
        }
        p += n;
    }
    sb_append(&b, suffix);
    free(prefix);
    free(suffix);
    free(rest);
    return sb_detach(&b);
}

char *anon_path(anon_t *a, const char *value) {
    sb_t b = SB_INIT;
    const char *p = value;
    for (;;) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        char *part = xstrndup(p, n);
        char *t = trim_js(part);
        if (!*t) sb_append(&b, part);
        else {
            /* part.replace(trimmed, mapped): the trimmed text sits after the leading whitespace */
            char *at = strstr(part, t);
            char *m = single_path(a, t);
            sb_appendn(&b, part, (size_t)(at - part));
            sb_append(&b, m);
            sb_append(&b, at + strlen(t));
            free(m);
        }
        free(t);
        free(part);
        if (!semi) break;
        sb_putc(&b, ';');
        p = semi + 1;
    }
    return sb_detach(&b);
}

static char *literal(anon_t *a, const char *value, bool detect_path) {
    if (detect_path && looks_like_path(value)) return anon_path(a, value);
    if (!strchr(value, '\n') && js_length(value) <= MAX_PLACEHOLDER_LENGTH) return anon_placeholder(a, value);
    return anon_redact(a, value);
}

static json_t *opaque(anon_t *a, const json_t *value) {
    if (json_is_str(value)) return json_take_str(anon_redact(a, json_sv(value)));
    char *s = json_dump_js(value);
    json_t *r = json_take_str(anon_redact(a, s));
    free(s);
    return r;
}

/* ---------- shell commands ---------- */

typedef struct {
    bool separator;
    char *raw;
} shtok_t;
typedef VEC(shtok_t) shtoks_t;

/* tools/shell-tokenize.ts lexShellCommand: words (raw spelling) and separator runs */
static void lex_shell(const char *cmd, shtoks_t *out) {
    size_t n = strlen(cmd);
    ssize_t word = -1, sep = -1;
    bool in_single = false, in_double = false;
#define END_WORD(I_)                                                                    \
    do {                                                                                \
        if (word >= 0) {                                                                \
            shtok_t t_ = {false, xstrndup(cmd + word, (size_t)(I_) - (size_t)word)};    \
            vec_push(out, t_);                                                          \
            word = -1;                                                                  \
        }                                                                               \
    } while (0)
    for (size_t i = 0; i < n; i++) {
        char ch = cmd[i];
        if (in_single) {
            if (ch == '\'') in_single = false;
            continue;
        }
        if (in_double) {
            if (ch == '\\' && i + 1 < n && strchr("\"\\$`", cmd[i + 1])) {
                i++;
                continue;
            }
            if (ch == '"') in_double = false;
            continue;
        }
        if (strchr(" \t\n;&|()", ch)) {
            END_WORD(i);
            if (sep < 0) sep = (ssize_t)i;
            continue;
        }
        if (word < 0) {
            if (sep >= 0) {
                shtok_t t = {true, xstrndup(cmd + sep, i - (size_t)sep)};
                vec_push(out, t);
                sep = -1;
            }
            word = (ssize_t)i;
        }
        if (ch == '\'') in_single = true;
        else if (ch == '"') in_double = true;
        else if (ch == '\\' && i + 1 < n) i++;
    }
    END_WORD(n);
#undef END_WORD
    if (sep >= 0) {
        shtok_t t = {true, xstrdup(cmd + sep)};
        vec_push(out, t);
    }
}

static char *env_name(anon_t *a, const char *name) { return in_list(ANON_ENV_NAMES, name) ? xstrdup(name) : anon_placeholder(a, name); }

/* `$NAME` / `${NAME}` with the first word run mapped */
static char *env_ref(anon_t *a, const char *word) {
    const char *p = word;
    while (*p && !is_word((unsigned char)*p)) p++;
    size_t n = 0;
    while (is_word((unsigned char)p[n])) n++;
    char *name = xstrndup(p, n);
    char *m = env_name(a, name);
    char *r = xasprintf("%.*s%s%s", (int)(p - word), word, m, p + n);
    free(m);
    free(name);
    return r;
}

static char *shell_value(anon_t *a, const char *word, bool is_path) {
    size_t n = strlen(word);
    if ((word[0] == '"' || word[0] == '\'') && n >= 2 && word[n - 1] == word[0]) {
        char *inner = xstrndup(word + 1, n - 2);
        char *m = shell_value(a, inner, is_path);
        char *r = xasprintf("%c%s%c", word[0], m, word[0]);
        free(m);
        free(inner);
        return r;
    }
    if (!n) return xstrdup("");
    if (is_env_ref(word)) return env_ref(a, word);
    if (is_path || looks_like_path(word)) return anon_path(a, word);
    return literal(a, word, true);
}

static char *shell_arg(anon_t *a, const char *word, const char *program, int arg_index) {
    if (is_env_ref(word)) return env_ref(a, word);
    if (!program) return shell_value(a, word, false);
    if (word[0] == '-' && all_digits(word + 1, strlen(word + 1)))
        return in_list(ANON_NUMERIC_FLAG_PROGRAMS, program) ? xstrdup(word) : shell_value(a, word, false);
    if (word[0] == '-' && isalpha((unsigned char)word[1])) {
        if (strlen(word) <= 2) return xstrdup(word);
        char *v = shell_value(a, word + 2, false);
        char *r = xasprintf("%.2s%s", word, v);
        free(v);
        return r;
    }
    if (is_shell_flag(word)) return in_list(ANON_LONG_FLAGS, word) ? xstrdup(word) : shell_value(a, word, false);
    /* ^(--[A-Za-z][\w-]*)=([\s\S]*)$ with a known flag */
    if (!strncmp(word, "--", 2) && isalpha((unsigned char)word[2])) {
        size_t i = 3;
        while (is_word((unsigned char)word[i]) || word[i] == '-') i++;
        if (word[i] == '=') {
            char *flag = xstrndup(word, i);
            if (in_list(ANON_LONG_FLAGS, flag)) {
                char *v = shell_value(a, word + i + 1, false);
                char *r = xasprintf("%s=%s", flag, v);
                free(v);
                free(flag);
                return r;
            }
            free(flag);
        }
    }
    if (arg_index == 0 && in_list(ANON_SUBCOMMANDS, word)) return xstrdup(word);
    return shell_value(a, word, false);
}

/* ^\d*(?:>>?|<<?-?) : length of the redirection operator, 0 = none */
static size_t redirect_len(const char *w) {
    size_t i = 0;
    while (isdigit((unsigned char)w[i])) i++;
    if (w[i] == '>') return i + (w[i + 1] == '>' ? 2 : 1);
    if (w[i] == '<') {
        size_t j = i + (w[i + 1] == '<' ? 2 : 1);
        return w[j] == '-' ? j + 1 : j;
    }
    return 0;
}

char *anon_command(anon_t *a, const char *value) {
    shtoks_t toks = {0};
    lex_shell(value, &toks);
    sb_t out = SB_INIT;
    bool command_start = true, redirect_target = false, descriptor_next = false, options_ended = false;
    const char *program = NULL;
    int arg_index = 0;
    const char *previous = "";
    for (size_t k = 0; k < toks.n; k++) {
        const char *raw = toks.v[k].raw;
        if (toks.v[k].separator) {
            sb_append(&out, raw);
            size_t pl = strlen(previous);
            descriptor_next = !strcmp(raw, "&") && pl && strchr("<>", previous[pl - 1]);
            if (descriptor_next) redirect_target = false;
            else if (strpbrk(raw, "\n;&|()")) {
                command_start = true;
                program = NULL;
                options_ended = false;
            }
            continue;
        }
        const char *word = raw;
        previous = word;
        if (descriptor_next && all_digits(word, strlen(word))) {
            descriptor_next = false;
            sb_append(&out, word);
            continue;
        }
        descriptor_next = false;
        size_t rl = redirect_len(word);
        if (rl) {
            sb_appendn(&out, word, rl);
            if (word[rl]) {
                char *v = shell_value(a, word + rl, true);
                sb_append(&out, v);
                free(v);
            }
            redirect_target = !word[rl];
            continue;
        }
        if (redirect_target) {
            redirect_target = false;
            char *v = shell_value(a, word, true);
            sb_append(&out, v);
            free(v);
            continue;
        }
        if (command_start) {
            size_t en = env_assign_name(word);
            if (en) {
                char *name = xstrndup(word, en);
                char *nm = env_name(a, name), *v = shell_value(a, word + en + 1, false);
                sb_appendf(&out, "%s=%s", nm, v);
                free(nm);
                free(v);
                free(name);
                continue;
            }
            command_start = in_list(ANON_COMMAND_PREFIXES, word);
            program = NULL;
            for (size_t i = 0; ANON_SHELL_COMMANDS[i]; i++)
                if (!strcmp(ANON_SHELL_COMMANDS[i], word)) program = ANON_SHELL_COMMANDS[i];
            if (program) sb_append(&out, program);
            else {
                char *v = shell_value(a, word, false);
                sb_append(&out, v);
                free(v);
            }
            arg_index = 0;
            options_ended = false;
            continue;
        }
        if (options_ended) {
            char *v = shell_value(a, word, false);
            sb_append(&out, v);
            free(v);
            continue;
        }
        if (!strcmp(word, "--")) {
            options_ended = true;
            sb_append(&out, word);
            continue;
        }
        char *v = shell_arg(a, word, program, arg_index);
        sb_append(&out, v);
        free(v);
        if (word[0] != '-') arg_index++;
    }
    for (size_t k = 0; k < toks.n; k++) free(toks.v[k].raw);
    vec_free(&toks);
    return sb_detach(&out);
}

/* ---------- structure walk ---------- */

static json_t *field(anon_t *a, rule_t rule, const json_t *value, const json_t *parent, const char *key, bool from_tool);
static json_t *strct(anon_t *a, const json_t *object, bool from_tool);

static void set_placeholder_key(anon_t *a, json_t *out, const char *key, json_t *v) {
    char *k = anon_placeholder(a, key);
    json_set(out, k, v);
    free(k);
}

/* ARG_SCHEMAS[tool]: declared keys, `#` marks numeric fields */
static const char *arg_schema(const char *tool) {
    for (size_t i = 0; tool && ANON_ARG_SCHEMAS[i][0]; i++)
        if (!strcmp(ANON_ARG_SCHEMAS[i][0], tool)) return ANON_ARG_SCHEMAS[i][1];
    return NULL;
}

static bool schema_has(const char *spec, const char *key, bool numeric) {
    size_t n = strlen(key);
    for (const char *p = spec; *p;) {
        while (*p == ' ') p++;
        size_t l = strcspn(p, " ");
        bool num = l && p[l - 1] == '#';
        size_t kl = num ? l - 1 : l;
        if (kl == n && !strncmp(p, key, n) && (!numeric || num)) return true;
        p += l;
    }
    return false;
}

static json_t *arg_value(anon_t *a, const json_t *value, const char *key, const char *spec) {
    if (json_is_null(value) || json_is_bool(value)) return json_clone(value);
    if (json_is_num(value)) {
        if (key && schema_has(spec, key, true)) return json_clone(value);
        char *s = js_num_string(value);
        json_t *r = json_take_str(anon_placeholder(a, s));
        free(s);
        return r;
    }
    if (json_is_arr(value)) {
        json_t *arr = json_arr(), *it;
        json_arr_foreach(value, i, it) json_push(arr, arg_value(a, it, key, spec));
        return arr;
    }
    if (json_is_obj(value)) {
        json_t *out = json_obj();
        const char *k;
        json_t *v;
        json_obj_foreach(value, i, k, v) {
            if (in_list(ANON_OPAQUE_ARG_KEYS, k)) json_set(out, k, opaque(a, v));
            else if (schema_has(spec, k, false) || !strcmp(k, "i") || !strcmp(k, "intent")) json_set(out, k, arg_value(a, v, k, spec));
            else set_placeholder_key(a, out, k, opaque(a, v));
        }
        return out;
    }
    const char *s = json_sv(value);
    if (!key) return json_take_str(literal(a, s, true));
    if (is_id_key(key)) return json_take_str(anon_segment(a, s));
    if (in_list(ANON_PATH_KEYS, key) || is_path_key(key)) return json_take_str(anon_path(a, s));
    if (!strcmp(key, "command") || !strcmp(key, "cmd")) return json_take_str(anon_command(a, s));
    if (!strcmp(key, "agent")) return field(a, R_AGENT, value, NULL, key, true);
    if (tool_enum_has(key, s)) return json_clone(value);
    if (in_list(ANON_TEXT_ARG_KEYS, key)) return json_take_str(anon_redact(a, s));
    return json_take_str(literal(a, s, !in_list(ANON_PATTERN_ARG_KEYS, key)));
}

static json_t *tool_args(anon_t *a, const json_t *value, const json_t *parent) {
    const char *tool = json_get_str(parent, "name") ? json_get_str(parent, "name") : json_get_str(parent, "toolName");
    const char *spec = is_builtin_tool(tool) ? arg_schema(tool) : NULL;
    if (!spec) return opaque(a, value);
    if (!json_is_str(value)) return arg_value(a, value, NULL, spec);
    json_t *parsed = json_parse_str(json_sv(value));
    if (!parsed) return json_take_str(anon_redact(a, json_sv(value)));
    json_t *m = arg_value(a, parsed, NULL, spec);
    json_free(parsed);
    char *s = json_dump_js(m);
    json_free(m);
    return json_take_str(s);
}

static json_t *content_block(anon_t *a, const json_t *block, bool from_tool) {
    const char *type = json_get_str(block, "type");
    const char *const *allowed = content_block_keys(type);
    if (!allowed) return opaque(a, block);
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(block, i, k, v) {
        rule_t r = in_list(allowed, k) ? field_rule(k) : R_NONE;
        if (r == R_NONE) set_placeholder_key(a, out, k, opaque(a, v));
        else if (!strcmp(k, "type")) json_set(out, k, json_clone(v));
        else json_set(out, k, field(a, r, v, block, k, from_tool && !str_eq(type, "toolCall")));
    }
    if (str_eq(type, "toolCall") && json_get_str(block, "id")) hmap_put(&a->tool_call_ids, json_get_str(block, "id"), (void *)1);
    return out;
}

static json_t *execution_log(anon_t *a, const json_t *data) {
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(data, i, k, v) {
        if (!strcmp(k, "toolCallId"))
            json_set(out, k, field(a, R_ID, v, data, k, !(json_is_str(v) && hmap_has(&a->tool_call_ids, json_sv(v)))));
        else if (!strcmp(k, "toolName")) json_set(out, k, field(a, R_TOOL, v, data, k, true));
        else if (!strcmp(k, "startedAt")) json_set(out, k, field(a, R_TIME, v, data, k, true));
        else if (!strcmp(k, "args")) json_set(out, k, tool_args(a, v, data));
        else if (!strcmp(k, "intent")) json_set(out, k, field(a, R_LABEL, v, data, k, true));
        else set_placeholder_key(a, out, k, opaque(a, v));
    }
    return out;
}

static bool is_version(const char *s) {
    const char *p = s;
    for (int part = 0; part < 3; part++) {
        if (!isdigit((unsigned char)*p)) return false;
        while (isdigit((unsigned char)*p)) p++;
        if (part < 2) {
            if (*p != '.') return false;
            p++;
        }
    }
    for (; *p; p++)
        if (!is_word((unsigned char)*p) && !strchr(".+-", *p)) return false;
    return true;
}

static json_t *runtime_stamp(anon_t *a, const json_t *stamp) {
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(stamp, i, k, v) {
        const char *s = json_sv(v);
        if (!s) set_placeholder_key(a, out, k, opaque(a, v));
        else if (!strcmp(k, "version")) json_set(out, k, is_version(s) ? json_str(s) : json_take_str(anon_placeholder(a, s)));
        else if (!strcmp(k, "commit")) {
            size_t n = strlen(s);
            bool hex = n >= 7 && n <= 64;
            for (size_t j = 0; hex && j < n; j++) hex = isdigit((unsigned char)s[j]) || (s[j] >= 'a' && s[j] <= 'f');
            json_set(out, k, hex ? json_str(s) : json_take_str(anon_placeholder(a, s)));
        } else if (!strcmp(k, "branch") || !strcmp(k, "diff") || !strcmp(k, "label")) json_set(out, k, json_take_str(anon_placeholder(a, s)));
        else set_placeholder_key(a, out, k, opaque(a, v));
    }
    return out;
}

static bool is_mcp_tool_name(const char *s) { return !strncmp(s, "mcp__", 5); }

static char *string_rule(anon_t *a, rule_t rule, const char *value, const char *key, bool from_tool) {
    switch (rule) {
    case R_TIME: return is_iso_timestamp(value) ? xstrdup(value) : anon_placeholder(a, value);
    case R_ENUM:
        if (from_tool) return tool_enum_has(key, value) ? xstrdup(value) : anon_placeholder(a, value);
        return js_length(value) <= 64 && is_identifier(value) ? xstrdup(value) : anon_placeholder(a, value);
    case R_IDENTITY:
        if (from_tool) {
            /* ^[\w.-]+\/[\w.:@+-]+$ */
            const char *sl = strchr(value, '/');
            bool ok = sl && sl > value && sl[1];
            for (const char *p = value; ok && p < sl; p++) ok = is_word((unsigned char)*p) || *p == '.' || *p == '-';
            for (const char *p = sl ? sl + 1 : value; ok && *p; p++) ok = is_word((unsigned char)*p) || strchr(".:@+-", *p);
            return ok ? xstrdup(value) : anon_placeholder(a, value);
        }
        return is_identifier(value) ? xstrdup(value) : anon_placeholder(a, value);
    case R_AGENT: return in_list(ANON_BUNDLED_AGENTS, value) ? xstrdup(value) : anon_placeholder(a, value);
    case R_SPAWNS: {
        if (!*value || !strcmp(value, "*")) return xstrdup(value);
        strvec_t parts = str_split(value, ",");
        sb_t b = SB_INIT;
        for (size_t i = 0; i < parts.n; i++) {
            char *t = trim_js(parts.v[i]);
            char *m = string_rule(a, R_AGENT, t, key, from_tool);
            sb_appendf(&b, "%s%s", i ? "," : "", m);
            free(m);
            free(t);
        }
        strvec_free(&parts);
        return sb_detach(&b);
    }
    case R_ID: return !from_tool && is_random_id(value) ? xstrdup(value) : anon_segment(a, value);
    case R_PATH: return anon_path(a, value);
    case R_SESSIONPATH: {
        const char *base = value + strlen(value);
        while (base > value && base[-1] != '/' && base[-1] != '\\') base--;
        char *dir = xstrndup(value, (size_t)(base - value));
        char *dm = *dir ? anon_path(a, dir) : xstrdup("");
        char *bm = is_session_stem(base) ? xstrdup(base) : anon_segment(a, base);
        char *r = xasprintf("%s%s", dm, bm);
        free(dir);
        free(dm);
        free(bm);
        return r;
    }
    case R_CMD: return anon_command(a, value);
    case R_LABEL:
    case R_STRUCT: return anon_placeholder(a, value);
    case R_ERROR: {
        /* ^\d{3}\b */
        if (isdigit((unsigned char)value[0]) && isdigit((unsigned char)value[1]) && isdigit((unsigned char)value[2]) &&
            !is_word((unsigned char)value[3])) {
            const char *rest = value + 3;
            while (isspace((unsigned char)*rest)) rest++;
            char *red = anon_redact(a, rest);
            char *r = xasprintf("%.3s %s", value, red);
            free(red);
            return r;
        }
        return anon_redact(a, value);
    }
    case R_TOOL: {
        bool ok = is_builtin_tool(value);
        if (!ok && !is_mcp_tool_name(value) && isalpha((unsigned char)value[0]) && js_length(value) <= 64) {
            ok = true;
            for (const char *p = value + 1; ok && *p; p++) ok = is_word((unsigned char)*p) || *p == '.' || *p == '-';
        }
        return ok ? xstrdup(value) : anon_placeholder(a, value);
    }
    case R_CUSTOMTYPE: {
        bool ok = value[0] >= 'a' && value[0] <= 'z' && js_length(value) <= 64;
        for (const char *p = value + 1; ok && *p; p++) ok = islower((unsigned char)*p) || isdigit((unsigned char)*p) || strchr("_:.-", *p);
        return ok ? xstrdup(value) : anon_placeholder(a, value);
    }
    default: return anon_redact(a, value);
    }
}

static json_t *field(anon_t *a, rule_t rule, const json_t *value, const json_t *parent, const char *key, bool from_tool) {
    switch (rule) {
    case R_OPAQUE: return opaque(a, value);
    case R_ARGS:
        if (json_get_str(parent, "name") || json_get_str(parent, "toolName")) return tool_args(a, value, parent);
        break;
    case R_DETAILS:
        return str_eq(json_get_str(parent, "role"), "toolResult") && is_builtin_tool(json_get_str(parent, "toolName")) && json_is_obj(value)
                   ? strct(a, value, true)
                   : opaque(a, value);
    case R_DATA:
        return str_eq(json_get_str(parent, "type"), "custom") && str_eq(json_get_str(parent, "customType"), "tool_execution_start") &&
                       json_is_obj(value)
                   ? execution_log(a, value)
                   : opaque(a, value);
    case R_RUNTIME: return json_is_obj(value) ? runtime_stamp(a, value) : opaque(a, value);
    case R_NAME: {
        const char *t = json_get_str(parent, "type");
        return field(a, t && in_list(ANON_TOOL_CALL_TYPES, t) ? R_TOOL : R_LABEL, value, parent, key, from_tool);
    }
    default: break;
    }
    if (json_is_null(value) || json_is_num(value) || json_is_bool(value)) {
        if (rule != R_LABEL) return json_clone(value);
        char *s = json_is_num(value) ? js_num_string(value) : json_dump_js(value);
        json_t *r = json_take_str(anon_placeholder(a, s));
        free(s);
        return r;
    }
    if (json_is_arr(value)) {
        json_t *arr = json_arr(), *it;
        json_arr_foreach(value, i, it) json_push(arr, field(a, rule, it, parent, key, from_tool));
        return arr;
    }
    if (json_is_obj(value)) {
        if (rule == R_CONTENT) {
            const char *role = json_get_str(parent, "role");
            bool untrusted = from_tool || str_eq(role, "toolResult") || str_eq(role, "custom") || str_eq(role, "hookMessage") ||
                             str_eq(json_get_str(parent, "type"), "custom_message");
            return content_block(a, value, untrusted);
        }
        if (rule == R_TRANSFORMS) {
            json_t *out = strct(a, value, from_tool);
            const char *p = json_get_str(value, "path");
            if (p && is_message_address(p)) json_set_str(out, "path", p);
            return out;
        }
        bool descends = rule == R_STRUCT || rule == R_ENUM || rule == R_ID || rule == R_PATH || rule == R_TOOL;
        return descends ? strct(a, value, from_tool) : opaque(a, value);
    }
    return json_take_str(string_rule(a, rule, json_sv(value), key, from_tool));
}

static json_t *strct(anon_t *a, const json_t *object, bool from_tool) {
    const char *type = json_get_str(object, "type");
    if (!from_tool && type && in_list(ANON_TOOL_CALL_TYPES, type) && json_get_str(object, "id"))
        hmap_put(&a->tool_call_ids, json_get_str(object, "id"), (void *)1);
    json_t *out = json_obj();
    const char *k;
    json_t *v;
    json_obj_foreach(object, i, k, v) {
        rule_t r = field_rule(k);
        if (r == R_NONE) set_placeholder_key(a, out, k, opaque(a, v));
        else json_set(out, k, field(a, r, v, object, k, from_tool));
    }
    return out;
}

json_t *anon_entry(anon_t *a, const json_t *record) { return json_is_obj(record) ? strct(a, record, false) : opaque(a, record); }

char *anon_entry_line(anon_t *a, const json_t *record) {
    json_t *e = anon_entry(a, record);
    char *s = json_dump_js(e);
    json_free(e);
    return s;
}
