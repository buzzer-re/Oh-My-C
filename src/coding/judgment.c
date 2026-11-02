/* judgment.c - typed judgments: question validation, keyword prompt rendering and reply parsing
 * (ports of ai/src/judgment/text.ts and coding-agent/src/eval/judgment-bridge.ts). */
#include "coding/judgment.h"

#include "prompt/template.h"
#include <math.h>

#include <ctype.h>

static char *invalid(const char *fmt, const char *a, const char *b) {
    sb_t s = SB_INIT;
    sb_append(&s, "judge() received invalid arguments: ");
    sb_appendf(&s, fmt, a ? a : "", b ? b : "");
    return sb_detach(&s);
}

bool judgment_check_state(const json_t *state, char **err) {
    *err = NULL;
    if (json_is_str(state)) {
        if (json_len(state) == 0) *err = invalid("state must not be empty", NULL, NULL);
        return !*err;
    }
    if (json_is_obj(state) || json_is_arr(state)) return true;
    *err = invalid("state must be a string, a JSON object, or a JSON array", NULL, NULL);
    return false;
}

static bool check_instructions(const char *id, const json_t *q, char **err) {
    const char *s = json_get_str(q, "instructions");
    if (s && *s) return true;
    *err = invalid("question \"%s\" needs non-empty string instructions", id, NULL);
    return false;
}

json_t *judgment_parse_questions(const json_t *questions, char **err) {
    *err = NULL;
    if (!json_is_obj(questions)) {
        *err = invalid("questions must be an object keyed by question id", NULL, NULL);
        return NULL;
    }
    json_t *out = json_obj();
    const char *id;
    json_t *q;
    json_obj_foreach(questions, i, id, q) {
        if (!json_is_obj(q)) {
            *err = invalid("question \"%s\" must be an object", id, NULL);
            break;
        }
        const char *type = json_get_str(q, "type");
        if (!str_eq(type, "choice") && !str_eq(type, "bool") && !str_eq(type, "score")) {
            *err = invalid("question \"%s\" type must be \"choice\", \"bool\", or \"score\"", id, NULL);
            break;
        }
        if (!check_instructions(id, q, err)) break;
        json_t *n = json_obj();
        json_set_str(n, "type", type);
        json_set_str(n, "instructions", json_get_str(q, "instructions"));
        json_t *crit = json_get(q, "criteria");
        if (str_eq(type, "choice")) {
            if (!json_is_obj(crit)) {
                *err = invalid("choice question \"%s\" needs criteria: { label: rubric | null }", id, NULL);
            } else {
                const char *label;
                json_t *rubric;
                json_obj_foreach(crit, k, label, rubric) {
                    if (!json_is_null(rubric) && !json_is_str(rubric)) {
                        *err = invalid("choice question \"%s\" criteria \"%s\" must be a string or null", id, label);
                        break;
                    }
                }
                if (!*err && json_len(crit) < 2)
                    *err = invalid("choice question \"%s\" needs at least two options", id, NULL);
            }
        } else if (str_eq(type, "bool")) {
            if (crit && !json_is_obj(crit)) {
                *err = invalid("bool question \"%s\" criteria must be { true?: string, false?: string }", id, NULL);
            } else if (crit) {
                static const char *const sides[] = {"true", "false"};
                for (int s = 0; s < 2 && !*err; s++) {
                    json_t *d = json_get(crit, sides[s]);
                    if (d && !json_is_str(d))
                        *err = invalid("bool question \"%s\" criteria.%s must be a string", id, sides[s]);
                }
            }
        } else {
            bool ok = json_is_arr(crit);
            json_t *lv;
            json_arr_foreach(crit, k, lv) if (!json_is_str(lv)) ok = false;
            if (!ok)
                *err = invalid("score question \"%s\" needs criteria: [lowest, ..., highest] level descriptions", id, NULL);
            else if (json_len(crit) < 2)
                *err = invalid("score question \"%s\" needs at least two levels", id, NULL);
        }
        if (*err) {
            json_free(n);
            break;
        }
        if (crit) json_set(n, "criteria", json_clone(crit));
        json_set(out, id, n);
    }
    if (!*err && json_len(out) == 0) *err = invalid("questions must contain at least one question", NULL, NULL);
    if (*err) {
        json_free(out);
        return NULL;
    }
    return out;
}

/* ---------- state rendering ---------- */

static void xml_escape(sb_t *b, const char *s, bool attr) {
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_append(b, "&amp;"); break;
        case '<': sb_append(b, "&lt;"); break;
        case '>': sb_append(b, "&gt;"); break;
        case '"':
            if (attr) {
                sb_append(b, "&quot;");
                break;
            }
            /* fallthrough */
        default: sb_putc(b, *s);
        }
    }
}

static bool xml_tag_name(const char *k) {
    if (!(isalpha((unsigned char)*k) || *k == '_')) return false;
    for (const char *p = k + 1; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '-')) return false;
    return true;
}

/* JavaScript String(value) for scalars */
static char *scalar_text(const json_t *v) {
    if (json_is_null(v)) return xstrdup("null");
    if (json_is_str(v)) return xstrdup(json_sv(v));
    if (json_is_bool(v)) return xstrdup(v->u.b ? "true" : "false");
    return json_dump(v, 0);
}

/* ---------- Bun YAML.stringify(value, null, 2) (bun src/runtime/api/YAMLObject.rs) ---------- */

/* next code point of UTF-8 text (invalid bytes read as themselves) */
static uint32_t cp_at(const char *s, size_t n, size_t i, size_t *len) {
    unsigned char c = (unsigned char)s[i];
    size_t l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + l > n) l = 1;
    uint32_t cp = l == 1 ? c : l == 2 ? c & 0x1f : l == 3 ? c & 0x0f : c & 0x07;
    for (size_t k = 1; k < l; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
    *len = l;
    return cp;
}

typedef struct {
    uint32_t *v;
    size_t n;
} cps_t;

static cps_t to_cps(const char *s) {
    size_t n = strlen(s);
    cps_t r = {xmalloc((n + 1) * sizeof(uint32_t)), 0};
    for (size_t i = 0; i < n;) {
        size_t l;
        r.v[r.n++] = cp_at(s, n, i, &l);
        i += l;
    }
    return r;
}

static bool is_inf_suffix(const cps_t *s, size_t i) {
    if (i + 4 > s->n) return false;
    uint32_t a = s->v[i + 1], b = s->v[i + 2], c = s->v[i + 3];
    return (a == 'i' && b == 'n' && c == 'f') || (a == 'I' && b == 'n' && c == 'f') || (a == 'I' && b == 'N' && c == 'F');
}

static bool string_is_number(const cps_t *s) {
    size_t len = s->n, i = 0;
    if (!len) return false;
    if (s->v[0] == '+' || s->v[0] == '-') {
        i = 1;
        if (i >= len) return false;
        if (s->v[i] == '.' && is_inf_suffix(s, i)) return true;
    }
    int base = 10;
    if (i + 1 < len && s->v[i] == '0') {
        if (s->v[i + 1] == 'x' || s->v[i + 1] == 'X') {
            base = 16;
            i += 2;
            if (i >= len) return false;
        } else if (s->v[i + 1] == 'o' || s->v[i + 1] == 'O') {
            base = 8;
            i += 2;
            if (i >= len) return false;
        }
    }
    bool dot = false, exp = false, minus = false;
    for (; i < len; i++) {
        uint32_t c = s->v[i];
        if (c >= '0' && c <= '9') continue;
        if ((c >= 'a' && c <= 'd') || c == 'f' || (c >= 'A' && c <= 'D') || c == 'F') {
            if (base != 16) return false;
        } else if (c == 'e' || c == 'E') {
            if (base == 10) {
                if (exp) return false;
                exp = true;
            }
        } else if (c == '.') {
            if (dot || base != 10) return false;
            dot = true;
        } else if (c == '+') {
            if (base == 16) return false;
        } else if (c == '-') {
            if (minus) return false;
            minus = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool flowish(uint32_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '[' || c == ']' || c == '{' || c == '}' || c == ','; }

static bool string_needs_quotes(const char *str) {
    cps_t s = to_cps(str);
    bool q = false;
    if (!s.n) {
        q = true;
        goto done;
    }
    uint32_t last = s.v[s.n - 1];
    if (last == ' ' || last == '\t' || last == '\n' || last == '\r' || last == ':') {
        q = true;
        goto done;
    }
    if (strchr("&*?|-<>!%@:,[]{}#'\"` \t\n\r", (int)(s.v[0] < 0x80 ? s.v[0] : 1)) && s.v[0] < 0x80 && s.v[0] != 0) {
        q = true;
        goto done;
    }
    if (s.v[0] == 0xfeff) {
        q = true;
        goto done;
    }
    static const char *const KW[] = {"true", "True", "TRUE", "false", "False", "FALSE", "yes", "Yes", "YES", "no", "No",
                                     "NO", "on", "On", "ON", "off", "Off", "OFF", "n", "N", "y", "Y", "null", "Null",
                                     "NULL", "~", ".inf", ".Inf", ".INF", ".nan", ".NaN", ".NAN", NULL};
    for (size_t k = 0; KW[k]; k++)
        if (!strcmp(str, KW[k])) {
            q = true;
            goto done;
        }
    for (size_t i = 0; i < s.n && !q; i++) {
        uint32_t c = s.v[i];
        switch (c) {
        case '{': case '}': case '[': case ']': case ',': case '#': case '`': case '\'': q = true; break;
        case ':':
            if (i + 1 < s.n && (s.v[i + 1] == ' ' || s.v[i + 1] == '\t' || s.v[i + 1] == '\n' || s.v[i + 1] == '\r')) q = true;
            break;
        case '-':
        case '.':
            if (i + 2 < s.n && s.v[i + 1] == c && s.v[i + 2] == c) {
                if (i + 3 >= s.n || flowish(s.v[i + 3])) q = true;
            }
            if (!q && i == 0 && string_is_number(&s)) q = true;
            break;
        case '+':
            if (i == 0 && string_is_number(&s)) q = true;
            break;
        default:
            if (c >= '0' && c <= '9') {
                if (i == 0 && string_is_number(&s)) q = true;
            } else if (c <= 0x1f || c == 0x22 || c == 0x7f || c == 0x85 || c == 0xa0 || c == 0x2028 || c == 0x2029) {
                q = true;
            }
        }
    }
done:
    free(s.v);
    return q;
}

static void yaml_quoted(sb_t *b, const char *s) {
    static const char *const LOW[32] = {"\\0", "\\x01", "\\x02", "\\x03", "\\x04", "\\x05", "\\x06", "\\a", "\\b", "\\t", "\\n",
                                        "\\v", "\\f", "\\r", "\\x0e", "\\x0f", "\\x10", "\\x11", "\\x12", "\\x13", "\\x14",
                                        "\\x15", "\\x16", "\\x17", "\\x18", "\\x19", "\\x1a", "\\e", "\\x1c", "\\x1d", "\\x1e",
                                        "\\x1f"};
    size_t n = strlen(s);
    sb_putc(b, '"');
    for (size_t i = 0; i < n;) {
        size_t l;
        uint32_t c = cp_at(s, n, i, &l);
        if (c < 0x20) sb_append(b, LOW[c]);
        else if (c == '"') sb_append(b, "\\\"");
        else if (c == '\\') sb_append(b, "\\\\");
        else if (c == 0x7f) sb_append(b, "\\x7f");
        else if (c == 0x85) sb_append(b, "\\N");
        else if (c == 0xa0) sb_append(b, "\\_");
        else if (c == 0x2028) sb_append(b, "\\L");
        else if (c == 0x2029) sb_append(b, "\\P");
        else sb_appendn(b, s + i, l);
        i += l;
    }
    sb_putc(b, '"');
}

static void yaml_string(sb_t *b, const char *s) {
    if (string_needs_quotes(s)) yaml_quoted(b, s);
    else sb_append(b, s);
}

typedef struct {
    sb_t *b;
    int indent;
    size_t item_offset;
} yctx_t;

static void y_newline(yctx_t *y) {
    sb_putc(y->b, '\n');
    for (int i = 0; i < y->indent * 2; i++) sb_putc(y->b, ' ');
    for (size_t i = 0; i < y->item_offset; i++) sb_putc(y->b, ' ');
}

static void y_value(yctx_t *y, const json_t *v, bool after_key, int depth) {
    bool obj = json_is_obj(v) || json_is_arr(v);
    if (after_key && !obj) sb_putc(y->b, ' ');
    if (depth > 64) { /* bounded recursion on untrusted state */
        sb_append(y->b, "null");
        return;
    }
    if (json_is_null(v)) {
        sb_append(y->b, "null");
    } else if (json_is_bool(v)) {
        sb_append(y->b, v->u.b ? "true" : "false");
    } else if (json_is_num(v)) {
        double d = v->u.num.d;
        if (isinf(d)) sb_append(y->b, d < 0 ? "-.inf" : ".inf");
        else if (isnan(d)) sb_append(y->b, ".nan");
        else if (d == 0 && signbit(d)) sb_append(y->b, "-0");
        else {
            char *t = json_dump(v, 0);
            sb_append(y->b, t);
            free(t);
        }
    } else if (json_is_str(v)) {
        yaml_string(y->b, json_sv(v));
    } else if (json_is_arr(v)) {
        if (!json_len(v)) {
            if (after_key) sb_putc(y->b, ' ');
            sb_append(y->b, "[]");
            return;
        }
        json_t *it;
        json_arr_foreach(v, i, it) {
            if (i || after_key) y_newline(y);
            sb_append(y->b, "- ");
            y->item_offset += 2;
            y_value(y, it, false, depth + 1);
            y->item_offset -= 2;
        }
    } else {
        if (!json_len(v)) {
            if (after_key) sb_putc(y->b, ' ');
            sb_append(y->b, "{}");
            return;
        }
        const char *k;
        json_t *it;
        json_obj_foreach(v, i, k, it) {
            if (i || after_key) y_newline(y);
            yaml_string(y->b, k);
            sb_putc(y->b, ':');
            y->indent++;
            y_value(y, it, true, depth + 1);
            y->indent--;
        }
    }
}

char *bun_yaml_stringify(const json_t *v) {
    sb_t b = SB_INIT;
    yctx_t y = {&b, 0, 0};
    y_value(&y, v, false, 0);
    return sb_detach(&b);
}

static void render_field(sb_t *b, const char *key, const json_t *v) {
    bool tag = xml_tag_name(key);
    if (tag) sb_appendf(b, "<%s>", key);
    else {
        sb_append(b, "<field name=\"");
        xml_escape(b, key, true);
        sb_append(b, "\">");
    }
    if (!json_is_obj(v) && !json_is_arr(v)) {
        char *t = scalar_text(v);
        xml_escape(b, t, false);
        free(t);
    } else {
        char *y = bun_yaml_stringify(v);
        sb_putc(b, '\n');
        xml_escape(b, y, false);
        sb_putc(b, '\n');
        free(y);
    }
    if (tag) sb_appendf(b, "</%s>", key);
    else sb_append(b, "</field>");
}

char *judgment_render_state(const json_t *state) {
    sb_t b = SB_INIT;
    if (!json_is_obj(state)) {
        render_field(&b, "state", state);
    } else if (json_len(state) == 0) {
        sb_append(&b, "<state>{}</state>");
    } else {
        const char *k;
        json_t *v;
        json_obj_foreach(state, i, k, v) {
            if (i) sb_putc(&b, '\n');
            render_field(&b, k, v);
        }
    }
    return sb_detach(&b);
}

/* ---------- prompt ---------- */

static json_t *rendered_questions(const json_t *questions) {
    json_t *arr = json_arr();
    const char *id;
    json_t *q;
    json_obj_foreach(questions, i, id, q) {
        json_t *r = json_obj();
        json_set_str(r, "id", id);
        json_set_str(r, "instructions", json_get_str(q, "instructions"));
        const char *type = json_get_str(q, "type");
        json_t *crit = json_get(q, "criteria");
        if (str_eq(type, "choice")) {
            json_t *opts = json_arr();
            const char *label;
            json_t *rubric;
            json_obj_foreach(crit, k, label, rubric) {
                json_t *o = json_obj();
                json_set_str(o, "label", label);
                json_set(o, "description", json_is_str(rubric) ? json_str(json_sv(rubric)) : json_null());
                json_push(opts, o);
            }
            json_set(r, "options", opts);
        } else if (str_eq(type, "score")) {
            json_t *lv = json_arr(), *d;
            json_arr_foreach(crit, k, d) {
                json_t *o = json_obj();
                json_set_int(o, "index", (int64_t)k);
                json_set_str(o, "description", json_sv(d));
                json_push(lv, o);
            }
            json_set(r, "levels", lv);
        } else {
            json_set_bool(r, "yesno", true);
            const char *y = json_get_str(crit, "true"), *n = json_get_str(crit, "false");
            if (y) json_set_str(r, "yes", y);
            if (n) json_set_str(r, "no", n);
        }
        json_push(arr, r);
    }
    return arr;
}

void judgment_render_prompt(const json_t *state, const json_t *questions, char **system, char **user) {
    json_t *qs = rendered_questions(questions);
    bool multi = json_len(qs) > 1;
    json_t *c = json_obj();
    json_set(c, "questions", json_clone(qs));
    json_set_bool(c, "multi", multi);
    json_set_bool(c, "guardState", true);
    *system = prompt_render("ai/judgment/text-judge.md", c);
    json_free(c);
    json_t *u = json_len(qs) ? json_clone(json_at(qs, 0)) : json_obj();
    json_set_bool(u, "guardState", true);
    json_set_bool(u, "multi", multi);
    json_set(u, "state", json_str_own(judgment_render_state(state)));
    *user = prompt_render("ai/judgment/text-judge-state.md", u);
    json_free(u);
    json_free(qs);
}

char *judgment_retry_system(const char *system, bool multi) {
    json_t *c = json_obj();
    json_set_str(c, "system", system);
    json_set_bool(c, "multi", multi);
    char *r = prompt_render("ai/judgment/text-judge-retry.md", c);
    json_free(c);
    return r;
}

/* ---------- reply parsing ---------- */

/* \p{L}\p{N}_ approximated: ASCII alnum/underscore and any non-ASCII byte (UTF-8 letters) */
static bool word_char(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

int judgment_index_of_word(const char *text, const char *needle) {
    size_t tl = strlen(text), nl = strlen(needle);
    if (!nl || nl > tl) return -1;
    bool first_word = word_char((unsigned char)needle[0]), last_word = word_char((unsigned char)needle[nl - 1]);
    for (size_t at = 0; at + nl <= tl; at++) {
        if (strncasecmp(text + at, needle, nl) != 0) continue;
        bool before = at == 0 || !word_char((unsigned char)text[at - 1]) || !first_word;
        bool after = at + nl == tl || !word_char((unsigned char)text[at + nl]) || !last_word;
        if (before && after) return (int)at;
    }
    return -1;
}

static int min_pos(int a, int b) { return a < 0 ? b : b < 0 ? a : (a < b ? a : b); }

int judgment_parse_yes_no(const char *text) {
    int yes = min_pos(judgment_index_of_word(text, "yes"), judgment_index_of_word(text, "true"));
    int no = min_pos(judgment_index_of_word(text, "no"), judgment_index_of_word(text, "false"));
    if (yes < 0 && no < 0) return -1;
    if (no < 0) return 1;
    if (yes < 0) return 0;
    return yes < no ? 1 : 0;
}

int judgment_parse_score(const char *text, int levels) {
    size_t n = strlen(text);
    for (size_t i = 0; i < n;) {
        if (!isdigit((unsigned char)text[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && isdigit((unsigned char)text[j])) j++;
        bool glued_before = i > 0 && word_char((unsigned char)text[i - 1]);
        bool decimal_before = i >= 2 && text[i - 1] == '.' && isdigit((unsigned char)text[i - 2]);
        bool glued_after = j < n && word_char((unsigned char)text[j]);
        bool decimal_after = j + 1 < n && text[j] == '.' && isdigit((unsigned char)text[j + 1]);
        if (!glued_before && !decimal_before && !glued_after && !decimal_after && j - i <= 9) {
            long v = strtol(text + i, NULL, 10);
            if (v < levels) return (int)v;
        }
        i = j;
    }
    return -1;
}

/* earliest label mentioned; a longer label wins a tie at the same position */
static const char *parse_choice(const char *text, const json_t *crit) {
    const char *best = NULL;
    int best_at = -1;
    const char *label;
    json_t *v;
    json_obj_foreach(crit, i, label, v) {
        int at = judgment_index_of_word(text, label);
        if (at < 0) continue;
        if (best_at < 0 || at < best_at || (at == best_at && strlen(label) > strlen(best))) {
            best = label;
            best_at = at;
        }
    }
    return best;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

/* first match of /\s*[:=]\s*|\s+-\s+/ (start index) or -1 */
static int separator_at(const char *line) {
    size_t n = strlen(line);
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j < n && is_ws(line[j])) j++;
        if (j < n && (line[j] == ':' || line[j] == '=')) return (int)i;
        if (j > i && j + 1 < n && line[j] == '-' && is_ws(line[j + 1])) return (int)i;
    }
    return -1;
}

/* `<id>: <answer>` lines (also `=` / ` - ` separators and quoted ids); first line per id wins */
static json_t *split_answer_lines(const char *text, const json_t *questions) {
    json_t *by = json_obj();
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *raw = xstrndup(p, len);
        p = nl ? nl + 1 : p + len;
        char *s = raw;
        for (;;) { /* strip leading [\s\-*•]+ */
            if (is_ws(*s) || *s == '-' || *s == '*') s++;
            else if (!strncmp(s, "\xe2\x80\xa2", 3)) s += 3;
            else break;
        }
        char *line = str_trim(s);
        free(raw);
        int sep = separator_at(line);
        if (sep > 0) {
            char *id = xstrndup(line, (size_t)sep);
            size_t il = strlen(id);
            char *idp = id;
            if (il && strchr("`\"'", id[il - 1])) id[--il] = 0;
            if (*idp && strchr("`\"'", *idp)) idp++;
            if (json_get(questions, idp) && !json_get(by, idp)) {
                const char *v = line + sep;
                while (is_ws(*v)) v++;
                if (*v == ':' || *v == '=' || *v == '-') v++;
                while (is_ws(*v)) v++;
                json_set_str(by, idp, v);
            }
            free(id);
        }
        free(line);
    }
    return by;
}

static json_t *one_hot_labels(const json_t *crit, const char *chosen) {
    json_t *p = json_obj();
    const char *label;
    json_t *v;
    json_obj_foreach(crit, i, label, v) json_set_int(p, label, str_eq(label, chosen) ? 1 : 0);
    return p;
}

static char *parse_error(const char *id, const char *output, const char *detail) {
    char *q = json_quote(output);
    char *e = xasprintf("judgment \"%s\": %s: %s", id, detail, q);
    free(q);
    return e;
}

json_t *judgment_parse_reply(const json_t *questions, const char *text, char **err) {
    *err = NULL;
    text = text ? text : "";
    bool single = json_len(questions) == 1;
    json_t *lines = single ? NULL : split_answer_lines(text, questions);
    json_t *answers = json_obj();
    const char *id;
    json_t *q;
    json_obj_foreach(questions, i, id, q) {
        const char *reply = single ? text : json_get_str(lines, id);
        if (!reply) {
            *err = parse_error(id, text, "no answer line for question");
            break;
        }
        const char *type = json_get_str(q, "type");
        json_t *crit = json_get(q, "criteria");
        json_t *a = json_obj();
        json_set_str(a, "type", type);
        if (str_eq(type, "choice")) {
            const char *c = parse_choice(reply, crit);
            if (!c) *err = parse_error(id, reply, "no option label in reply");
            else {
                json_set_str(a, "choice", c);
                json_set(a, "probabilities", one_hot_labels(crit, c));
                json_set_int(a, "confidence", 1);
            }
        } else if (str_eq(type, "bool")) {
            int v = judgment_parse_yes_no(reply);
            if (v < 0) *err = parse_error(id, reply, "no yes/no in reply");
            else json_set_int(a, "bool", v);
        } else {
            int levels = (int)json_len(crit);
            int lv = judgment_parse_score(reply, levels);
            if (lv < 0) *err = parse_error(id, reply, "no level number in reply");
            else {
                json_set_int(a, "score", lv);
                json_t *p = json_obj();
                for (int k = 0; k < levels; k++) {
                    char key[16];
                    snprintf(key, sizeof key, "%d", k);
                    json_set_int(p, key, k == lv ? 1 : 0);
                }
                json_set(a, "probabilities", p);
                json_set_int(a, "confidence", 1);
            }
        }
        if (*err) {
            json_free(a);
            break;
        }
        json_set(answers, id, a);
    }
    json_free(lines);
    if (*err) {
        json_free(answers);
        return NULL;
    }
    return answers;
}
