/* judgment.c - typed judgments: question validation, keyword prompt rendering and reply parsing
 * (ports of ai/src/judgment/text.ts and coding-agent/src/eval/judgment-bridge.ts). */
#include "coding/judgment.h"

#include "prompt/template.h"
#include "text/yaml.h"

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
        char *y = yaml_dump(v);
        size_t n = strlen(y);
        while (n && y[n - 1] == '\n') y[--n] = 0;
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
