/* sloppy.c - "sloppy" edit mode: *** Edit File / *** Find / *** Replace / *** Insert Before|After sections with
 * whitespace-insensitive anchors, … gap captures and ＋/－ marker lines.
 * Port of the taught surface of omp pi-edit modes/sloppy (parse.rs + apply.rs); see PARITY.md for the
 * recovery heuristics that are not ported. */
#include "patch.h"

#include "fuzzy.h"

#include <ctype.h>
#include <stdatomic.h>

/* shared with the conflicting-operators error so region extraction can recognize a payload the pipeline must report */
#define CONFLICTING_OPERATORS_LEAD "*** Find is followed by both"

#define GAP "…"
#define ADD_MARK "＋"
#define REM_MARK "－"
#define SEL_OPEN "⟪"
#define SEL_CLOSE "⟫"
#define IR_OPEN "«"
#define IR_PUT "»"
#define IR_BEFORE "\x01" "BEFORE "
#define IR_AFTER "\x01" "AFTER "
#define MAX_TUPLES 20000
#define ATOMICITY "No operations were applied — ops apply atomically; re-send the full corrected payload."

#define K_PARSE "parse"
#define K_MATCH "no_match"
#define K_NOCHANGE "no_change"

/* ======================================================================================
 * payload parsing
 * ====================================================================================== */

enum { H_NONE, H_EDIT, H_FIND, H_REPLACE, H_BEFORE, H_AFTER };

static bool ws_word(const char **p, const char *word) {
    size_t n = strlen(word);
    if (strncasecmp(*p, word, n) != 0) return false;
    *p += n;
    return true;
}

static bool skip_hws(const char **p) {
    const char *s = *p;
    while (**p == ' ' || **p == '\t') (*p)++;
    return *p != s;
}

/* parse a standalone header line; for H_EDIT, *path (owned, NULL = continue) and *all */
static int parse_header(const char *line, char **path, bool *all) {
    char *t = fz_trim(line);
    int kind = H_NONE;
    const char *p = t;
    if (strncmp(p, "***", 3) != 0) goto out;
    p += 3;
    if (!skip_hws(&p)) goto out;
    int k = H_NONE;
    const char *save = p;
    if (ws_word(&p, "edit") && skip_hws(&p) && ws_word(&p, "file")) k = H_EDIT;
    else if ((p = save, ws_word(&p, "find"))) k = H_FIND;
    else if ((p = save, ws_word(&p, "replace"))) k = H_REPLACE;
    else if ((p = save, ws_word(&p, "insert")) && skip_hws(&p)) {
        if (ws_word(&p, "after")) k = H_AFTER;
        else if (ws_word(&p, "before")) k = H_BEFORE;
    }
    if (k == H_NONE) goto out;
    if (*p == ':' || *p == ' ' || *p == '\t') p++;
    else if (*p) goto out;
    skip_hws(&p);
    char *rest = fz_trim(p);
    if (k != H_EDIT) {
        if (!*rest) kind = k;
        free(rest);
        goto out;
    }
    bool a = false;
    size_t rl = strlen(rest);
    if (!strcasecmp(rest, "all")) {
        rest[0] = 0;
        a = true;
    } else {
        size_t e = rl;
        while (e > 0 && !isspace((unsigned char)rest[e - 1])) e--;
        if (e > 0 && !strcasecmp(rest + e, "all")) {
            a = true;
            while (e > 0 && isspace((unsigned char)rest[e - 1])) e--;
            rest[e] = 0;
        }
    }
    char *pth = NULL;
    if (*rest == '"') {
        json_t *j = json_parse_str(rest);
        if (!json_is_str(j)) {
            json_free(j);
            free(rest);
            goto out;
        }
        if (*json_sv(j)) pth = xstrdup(json_sv(j));
        json_free(j);
    } else if (*rest) {
        pth = xstrdup(rest);
    }
    free(rest);
    if (path) *path = pth;
    else free(pth);
    if (all) *all = a;
    kind = H_EDIT;
out:
    free(t);
    return kind;
}

/* ^\*{3}\s*(Begin|End)(\s+of)?\s+(patch|edits?|file)\b | Abort | Update/Add/Delete File: */
static bool envelope_line(const char *t) {
    if (strncmp(t, "***", 3) != 0) return false;
    const char *p = t + 3;
    while (isspace((unsigned char)*p)) p++;
    if (!strncasecmp(p, "Abort", 5) || !strncasecmp(p, "Update File:", 12) || !strncasecmp(p, "Add File:", 9) ||
        !strncasecmp(p, "Delete File:", 12))
        return true;
    if (!ws_word(&p, "begin") && !ws_word(&p, "end")) return false;
    const char *q = p;
    if (isspace((unsigned char)*q)) {
        while (isspace((unsigned char)*q)) q++;
        if (ws_word(&q, "of")) p = q;
    }
    if (!isspace((unsigned char)*p)) return false;
    while (isspace((unsigned char)*p)) p++;
    if (ws_word(&p, "patch") || ws_word(&p, "edits") || ws_word(&p, "edit") || ws_word(&p, "file"))
        return !(isalnum((unsigned char)*p) || *p == '_');
    return false;
}

static bool envelope_end(const char *t) {
    if (strncmp(t, "***", 3) != 0) return false;
    const char *p = t + 3;
    while (isspace((unsigned char)*p)) p++;
    return !strncasecmp(p, "end", 3) && !(isalnum((unsigned char)p[3]) || p[3] == '_');
}

static bool fence_line(const char *l) {
    while (isspace((unsigned char)*l)) l++;
    return str_starts(l, "```") || str_starts(l, "~~~");
}

static strvec_t strip_envelope_noise(const strvec_t *in) {
    strvec_t out = {0};
    bool skipping = false, inserting = false, writing = false;
    for (size_t i = 0; i < in->n; i++) {
        const char *line = in->v[i];
        char *path = NULL;
        bool all;
        int h = parse_header(line, &path, &all);
        free(path);
        char *t = fz_trim(line);
        if (h != H_NONE) {
            inserting = !skipping && (h == H_BEFORE || h == H_AFTER);
            writing = !skipping && (h == H_REPLACE || h == H_BEFORE || h == H_AFTER);
        } else if (writing && envelope_end(t)) {
            /* a closing sentinel ending a Replace or Insert body is never content: before blank or fence lines only it
             * closes the payload, before a header or envelope row it closes that one body; a Find body keeps it, so a stray
             * one can be matched and removed */
            const char *next = NULL;
            for (size_t k = i + 1; k < in->n && !next; k++)
                if (!fz_blank(in->v[k]) && !fence_line(in->v[k])) next = in->v[k];
            if (!next) {
                free(t);
                break;
            }
            char *nt = fz_trim(next);
            bool closes = parse_header(next, NULL, NULL) != H_NONE || envelope_line(nt);
            free(nt);
            if (closes) {
                free(t);
                continue;
            }
        }
        if (inserting && h == H_NONE) {
            strvec_push(&out, line);
            free(t);
            continue;
        }
        if (envelope_line(t)) {
            skipping = envelope_end(t);
            free(t);
            continue;
        }
        free(t);
        if (skipping) {
            if (h == H_EDIT) {
                skipping = false;
                strvec_push(&out, line);
            }
            continue;
        }
        strvec_push(&out, line);
    }
    return out;
}

typedef struct {
    char *path;
    strvec_t ir;
} csec_t;
typedef VEC(csec_t) csecs_t;

static void trim_blank_lines(strvec_t *v) {
    size_t s = 0, e = v->n;
    while (s < e && fz_blank(v->v[s])) s++;
    while (e > s && fz_blank(v->v[e - 1])) e--;
    for (size_t i = 0; i < s; i++) free(v->v[i]);
    for (size_t i = e; i < v->n; i++) free(v->v[i]);
    if (e > s) memmove(v->v, v->v + s, (e - s) * sizeof(char *));
    v->n = e - s;
}

typedef struct {
    bool active;
    int insert; /* 0 = Replace, H_BEFORE, H_AFTER */
    strvec_t lines;
} putbuf_t;

static void flush_compiled(csecs_t *secs, bool all, strvec_t *find, putbuf_t *put) {
    trim_blank_lines(find);
    bool empty_put = !put->active || (!put->insert && ({
                                          bool b = true;
                                          for (size_t i = 0; i < put->lines.n && b; i++) b = fz_blank(put->lines.v[i]);
                                          b;
                                      }));
    if (!find->n && empty_put) goto clear;
    if (!secs->n) {
        csec_t s = {xstrdup(""), {0}};
        vec_push(secs, s);
    }
    strvec_t *ir = &secs->v[secs->n - 1].ir;
    strvec_push(ir, all ? IR_OPEN "*" : IR_OPEN);
    if (put->active && put->insert) {
        for (size_t i = 0; i < find->n; i++) strvec_push(ir, find->v[i]);
        char *text = strvec_join(&put->lines, "\n");
        if (put->lines.n) {
            char *t2 = xasprintf("%s\n", text);
            free(text);
            text = t2;
        }
        char *q = json_quote(text);
        strvec_push_own(ir, xasprintf("%s%s", put->insert == H_BEFORE ? IR_BEFORE : IR_AFTER, q));
        free(q);
        free(text);
        goto clear;
    }
    if (put->active) trim_blank_lines(&put->lines);
    if (!find->n) {
        if (put->active)
            for (size_t i = 0; i < put->lines.n; i++) strvec_push(ir, put->lines.v[i]);
        goto clear;
    }
    for (size_t i = 0; i < find->n; i++) strvec_push(ir, find->v[i]);
    if (put->active) {
        strvec_push(ir, IR_PUT);
        for (size_t i = 0; i < put->lines.n; i++) strvec_push(ir, put->lines.v[i]);
    }
clear:
    strvec_free(find);
    strvec_free(&put->lines);
    put->active = false;
    put->insert = 0;
}

static const char *put_operator(const putbuf_t *p) {
    return p->insert == H_BEFORE ? "*** Insert Before" : p->insert == H_AFTER ? "*** Insert After" : "*** Replace";
}

static bool compile_sections(const strvec_t *lines, csecs_t *secs, char **err) {
    enum { S_IDLE, S_FIND, S_PUT } state = S_IDLE;
    bool all = false;
    strvec_t find = {0};
    putbuf_t put = {0};
    for (size_t i = 0; i < lines->n; i++) {
        const char *line = lines->v[i];
        char *path = NULL;
        bool nall = false;
        int h = parse_header(line, &path, &nall);
        if (h == H_NONE) {
            if (state == S_PUT) strvec_push(&put.lines, line);
            else if (state == S_FIND) strvec_push(&find, line);
            else if (!fz_blank(line)) {
                state = S_FIND;
                strvec_push(&find, line);
            }
            continue;
        }
        if (h == H_EDIT) {
            flush_compiled(secs, all, &find, &put);
            state = S_IDLE;
            if (path) {
                csec_t s = {path, {0}};
                vec_push(secs, s);
                path = NULL;
            }
            all = nall;
        } else if (h == H_FIND) {
            flush_compiled(secs, all, &find, &put);
            state = S_FIND;
        } else {
            const char *incoming = h == H_REPLACE ? "*** Replace" : h == H_BEFORE ? "*** Insert Before" : "*** Insert After";
            if (put.active) {
                *err = xasprintf("*** Find is followed by both %s and %s. Use exactly one of *** Replace, *** Insert "
                                 "Before, or *** Insert After per *** Find.",
                                 put_operator(&put), incoming);
                strvec_free(&find);
                strvec_free(&put.lines);
                free(path);
                return false;
            }
            put.active = true;
            put.insert = h == H_REPLACE ? 0 : h;
            state = S_PUT;
        }
        free(path);
    }
    flush_compiled(secs, all, &find, &put);
    return true;
}

bool sloppy_split_sections(const char *input, sloppy_sections_t *out, char **err) {
    memset(out, 0, sizeof(*out));
    *err = NULL;
    /* Rust str::lines(): split on \n, strip a trailing \r, no trailing empty line */
    strvec_t raw = {0};
    const char *p = input;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t l2 = len && p[len - 1] == '\r' ? len - 1 : len;
        strvec_push_own(&raw, xstrndup(p, l2));
        p += len + (nl ? 1 : 0);
    }
    strvec_t lines = strip_envelope_noise(&raw);
    strvec_free(&raw);
    size_t s = 0;
    while (s < lines.n && fz_blank(lines.v[s])) s++;
    char *path = NULL;
    bool ok = true;
    if (s >= lines.n || parse_header(lines.v[s], &path, NULL) != H_EDIT || !path) {
        free(path);
        strvec_free(&lines);
        return true;
    }
    free(path);
    strvec_t rest = {0};
    for (size_t i = s; i < lines.n; i++) strvec_push(&rest, lines.v[i]);
    strvec_free(&lines);
    csecs_t secs = {0};
    if (!compile_sections(&rest, &secs, err)) ok = false;
    strvec_free(&rest);
    for (size_t i = 0; ok && i < secs.n; i++) {
        if (!*secs.v[i].path || !secs.v[i].ir.n) continue;
        char *body = strvec_join(&secs.v[i].ir, "\n");
        size_t k = 0;
        while (k < out->n && strcmp(out->v[k].path, secs.v[i].path) != 0) k++;
        if (k < out->n) {
            char *b2 = xasprintf("%s\n%s", out->v[k].body, body);
            free(out->v[k].body);
            out->v[k].body = b2;
            free(body);
        } else {
            sloppy_section_t sec = {xstrdup(secs.v[i].path), body};
            vec_push(out, sec);
        }
    }
    for (size_t i = 0; i < secs.n; i++) {
        free(secs.v[i].path);
        strvec_free(&secs.v[i].ir);
    }
    vec_free(&secs);
    if (!ok) sloppy_sections_free(out);
    return ok;
}

/* ---------- payloads written as text ---------- */

void sloppy_inline_regions(const char *text, sloppy_regions_t *out) {
    memset(out, 0, sizeof(*out));
    if (!strstr(text, "***")) return;
    size_t text_len = strlen(text);
    strvec_t lines = {0};
    VEC(size_t) starts = {0};
    for (const char *p = text;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        strvec_push_own(&lines, xstrndup(p, n));
        vec_push(&starts, (size_t)(p - text));
        if (!nl) break;
        p = nl + 1;
    }
    bool in_fence = false;
    size_t index = 0;
    while (index < lines.n) {
        const char *line = lines.v[index];
        if (fence_line(line)) {
            in_fence = !in_fence;
            index++;
            continue;
        }
        char *path = NULL;
        bool opener = !in_fence && parse_header(line, &path, NULL) == H_EDIT && path;
        free(path);
        if (!opener) {
            index++;
            continue;
        }
        size_t last = index, scan = index + 1;
        bool in_block = false;
        while (scan < lines.n) {
            char *t = fz_trim(lines.v[scan]);
            bool end = envelope_end(t);
            free(t);
            if (end) break;
            int header = parse_header(lines.v[scan], NULL, NULL);
            if (header == H_NONE) {
                if (!in_block) {
                    if (fz_blank(lines.v[scan])) {
                        scan++;
                        continue;
                    }
                    break;
                }
                last = scan++;
                continue;
            }
            in_block = header != H_EDIT;
            last = scan++;
        }
        sb_t payload = SB_INIT;
        for (size_t i = index; i <= last; i++) {
            if (i > index) sb_putc(&payload, '\n');
            sb_append(&payload, lines.v[i]);
        }
        char *body = sb_detach(&payload);
        /* a payload the parser rejects is prose, except conflicting operators, which the edit pipeline must report */
        sloppy_sections_t secs;
        char *err = NULL;
        bool parsed = sloppy_split_sections(body, &secs, &err);
        bool recognized = parsed ? secs.n > 0 : err && str_starts(err, CONFLICTING_OPERATORS_LEAD);
        if (parsed) sloppy_sections_free(&secs);
        free(err);
        if (recognized) {
            sloppy_region_t r = {.start = starts.v[index], .end = MIN(starts.v[last] + strlen(lines.v[last]) + 1, text_len), .payload = body};
            vec_push(out, r);
        } else {
            free(body);
        }
        index = MAX(scan, last + 1);
    }
    strvec_free(&lines);
    free(starts.v);
}

void sloppy_regions_free(sloppy_regions_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].payload);
    free(r->v);
    memset(r, 0, sizeof(*r));
}

/* mintToolCallId: ptc_<ms in base 36>_<counter in base 36> */
static void append_base36(sb_t *b, uint64_t v) {
    char tmp[16];
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
        v /= 36;
    } while (v && n < (int)sizeof tmp);
    while (n) sb_putc(b, tmp[--n]);
}

static char *mint_tool_call_id(void) {
    static _Atomic uint64_t counter;
    sb_t b = SB_INIT;
    sb_append(&b, "ptc_");
    append_base36(&b, (uint64_t)now_ms());
    sb_putc(&b, '_');
    append_base36(&b, ++counter);
    return sb_detach(&b);
}

int sloppy_recover_inline(json_t *message) {
    json_t *content = json_get_arr(message, "content");
    if (!content || !str_eq(json_get_str(message, "stopReason"), "stop")) return 0;
    json_t *block;
    json_arr_foreach(content, bi, block) {
        if (str_eq(json_get_str(block, "type"), "toolCall")) return 0;
    }
    strvec_t payloads = {0};
    json_arr_foreach(content, ti, block) {
        const char *text = json_get_str(block, "text");
        if (!str_eq(json_get_str(block, "type"), "text") || !text) continue;
        sloppy_regions_t regions;
        sloppy_inline_regions(text, &regions);
        if (!regions.n) continue;
        sb_t remaining = SB_INIT;
        size_t cursor = 0;
        for (size_t i = 0; i < regions.n; i++) {
            sb_appendn(&remaining, text + cursor, regions.v[i].start - cursor);
            cursor = regions.v[i].end;
            strvec_push(&payloads, regions.v[i].payload);
        }
        sb_append(&remaining, text + cursor);
        json_set_str(block, "text", sb_cstr(&remaining));
        sb_free(&remaining);
        sloppy_regions_free(&regions);
    }
    int recovered = (int)payloads.n;
    if (!recovered) return 0;
    for (size_t i = json_len(content); i-- > 0;) {
        json_t *b = json_at(content, i);
        const char *text = json_get_str(b, "text");
        if (str_eq(json_get_str(b, "type"), "text") && (!text || fz_blank(text))) json_free(json_arr_remove(content, i));
    }
    sb_t input = SB_INIT;
    for (size_t i = 0; i < payloads.n; i++) {
        if (i) sb_putc(&input, '\n');
        sb_append(&input, payloads.v[i]);
    }
    strvec_free(&payloads);
    json_t *call = json_obj();
    json_set_str(call, "type", "toolCall");
    char *id = mint_tool_call_id();
    json_set_str(call, "id", id);
    free(id);
    json_set_str(call, "name", "edit");
    json_t *args = json_obj();
    json_set_str(args, "input", sb_cstr(&input));
    json_set(call, "arguments", args);
    json_set_str(call, "rawBlock", sb_cstr(&input));
    sb_free(&input);
    json_push(content, call);
    return recovered;
}

void sloppy_sections_free(sloppy_sections_t *s) {
    for (size_t i = 0; i < s->n; i++) {
        free(s->v[i].path);
        free(s->v[i].body);
    }
    vec_free(s);
}

/* ======================================================================================
 * operations
 * ====================================================================================== */

enum { RW_NONE, RW_EXPLICIT, RW_BEFORE, RW_AFTER, RW_MARKERS };

typedef struct {
    int number;
    bool all;
    strvec_t find;    /* authored Find lines (IR) */
    int kind;
    strvec_t rewrite; /* RW_EXPLICIT lines */
    char *insert;     /* RW_BEFORE/RW_AFTER text */
} sop_t;
typedef VEC(sop_t) sops_t;

static void sops_free(sops_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        strvec_free(&v->v[i].find);
        strvec_free(&v->v[i].rewrite);
        free(v->v[i].insert);
    }
    vec_free(v);
}

/* full-line marker with retained indentation: returns owned stripped line or NULL */
static char *marker_content(const char *line, const char *marker) {
    size_t ind = fz_leading_ws(line);
    if (!str_starts(line + ind, marker)) return NULL;
    return xasprintf("%.*s%s", (int)ind, line, line + ind + strlen(marker));
}

static bool has_marker_lines(const strvec_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        char *a = marker_content(v->v[i], ADD_MARK), *r = marker_content(v->v[i], REM_MARK);
        bool m = a || r;
        free(a);
        free(r);
        if (m) return true;
    }
    return false;
}

static void parse_ir(const char *body, sops_t *ops) {
    strvec_t lines = fz_split(body);
    sop_t *cur = NULL;
    bool in_put = false;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (!strcmp(l, IR_OPEN) || !strcmp(l, IR_OPEN "*")) {
            sop_t o;
            memset(&o, 0, sizeof(o));
            o.number = (int)ops->n + 1;
            o.all = l[strlen(IR_OPEN)] == '*';
            vec_push(ops, o);
            cur = &ops->v[ops->n - 1];
            in_put = false;
            continue;
        }
        if (!cur) continue;
        if (!in_put && (str_starts(l, IR_BEFORE) || str_starts(l, IR_AFTER))) {
            bool before = str_starts(l, IR_BEFORE);
            json_t *j = json_parse_str(l + strlen(before ? IR_BEFORE : IR_AFTER));
            free(cur->insert);
            cur->insert = xstrdup(json_is_str(j) ? json_sv(j) : "");
            json_free(j);
            cur->kind = before ? RW_BEFORE : RW_AFTER;
            continue;
        }
        if (!in_put && !strcmp(l, IR_PUT)) {
            in_put = true;
            cur->kind = RW_EXPLICIT;
            continue;
        }
        strvec_push(in_put ? &cur->rewrite : &cur->find, l);
    }
    strvec_free(&lines);
}

/* ======================================================================================
 * normalized matching
 * ====================================================================================== */

typedef struct {
    char *text;
    size_t *starts, *ends; /* per normalized byte: raw start / end */
    size_t n;
} ntext_t;

static bool ascii_ws(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0c || c == 0x0b; }

static void ntext_build(ntext_t *t, const char *s) {
    sb_t b = SB_INIT;
    size_t len = strlen(s);
    VEC(size_t) st = {0}, en = {0};
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        size_t start = i, end = i + (size_t)k;
        i = end;
        if (ascii_ws(cp)) continue;
        if (cp < 0x80) {
            sb_putc(&b, (char)cp);
            vec_push(&st, start);
            vec_push(&en, end);
            continue;
        }
        char *one = xstrndup(s + start, (size_t)k);
        char *nm = fz_normalize_unicode(one);
        size_t nl = strlen(nm);
        sb_append(&b, nm);
        for (size_t q = 0; q < nl; q++) {
            vec_push(&st, start);
            vec_push(&en, end);
        }
        free(one);
        free(nm);
    }
    t->n = b.len;
    t->text = sb_detach(&b);
    t->starts = st.v;
    t->ends = en.v;
}

static char *normalize_str(const char *s) {
    ntext_t t;
    ntext_build(&t, s);
    free(t.starts);
    free(t.ends);
    return t.text;
}

static void ntext_free(ntext_t *t) {
    free(t->text);
    free(t->starts);
    free(t->ends);
}

/* pattern token: literal (raw + normalized) followed by an optional gap */
typedef struct {
    char *raw;
    char *norm;
    bool gap_after;
    bool line_bounded; /* the gap after this literal stays on one line */
    bool whole_line;   /* the gap after this literal is a whole-line … */
} plit_t;
typedef VEC(plit_t) plits_t;

static void plits_free(plits_t *v) {
    for (size_t i = 0; i < v->n; i++) {
        free(v->v[i].raw);
        free(v->v[i].norm);
    }
    vec_free(v);
}

typedef struct {
    size_t start, end;      /* raw span */
    size_t ls[64], le[64];  /* per literal raw extents (bounded by MAX literal count) */
    size_t nlit;
} cand_t;
typedef VEC(cand_t) cands_t;

#define MAX_LITS 64

static bool is_hws(char c) { return c == ' ' || c == '\t'; }

/* count newlines in the leading / trailing whitespace of s */
static size_t lead_newlines(const char *s) {
    size_t n = 0;
    for (; *s && isspace((unsigned char)*s); s++)
        if (*s == '\n') n++;
    return n;
}
static size_t trail_newlines(const char *s) {
    size_t n = 0, l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) {
        if (s[l - 1] == '\n') n++;
        l--;
    }
    return n;
}
static bool lead_ws(const char *s) { return *s && isspace((unsigned char)*s); }
static bool trail_ws(const char *s) {
    size_t l = strlen(s);
    return l && isspace((unsigned char)s[l - 1]);
}

typedef struct {
    const char *content;
    const ntext_t *nt;
    const plits_t *lits;
    cands_t *out;
    size_t ln[MAX_LITS], le[MAX_LITS]; /* normalized literal starts / ends */
    size_t count;
    bool overflow;
} search_t;

static void extend_candidate(search_t *s, cand_t *c) {
    const char *content = s->content;
    size_t clen = strlen(content);
    for (size_t i = 0; i < s->lits->n; i++) {
        const plit_t *L = &s->lits->v[i];
        size_t rs, re;
        if (*L->norm) {
            rs = s->nt->starts[s->ln[i]];
            re = s->nt->ends[s->le[i] - 1];
        } else {
            rs = re = i ? c->le[i - 1] : (s->ln[i] < s->nt->n ? s->nt->starts[s->ln[i]] : clen);
        }
        /* leading whitespace of the literal: horizontal ws, then up to k newlines backwards */
        size_t floor = i ? c->le[i - 1] : 0;
        if (lead_ws(L->raw)) {
            size_t k = lead_newlines(L->raw);
            while (rs > floor && is_hws(content[rs - 1])) rs--;
            while (k > 0 && rs > floor && (content[rs - 1] == '\n' || content[rs - 1] == '\r')) {
                if (content[rs - 1] == '\n') k--;
                rs--;
                if (k > 0)
                    while (rs > floor && is_hws(content[rs - 1])) rs--;
            }
        }
        if (trail_ws(L->raw)) {
            size_t k = trail_newlines(L->raw);
            while (re < clen && (is_hws(content[re]) || content[re] == '\r')) re++;
            while (k > 0 && re < clen && content[re] == '\n') {
                re++;
                k--;
                if (k > 0)
                    while (re < clen && (is_hws(content[re]) || content[re] == '\r')) re++;
            }
        }
        if (rs < floor) rs = floor;
        if (re < rs) re = rs;
        c->ls[i] = rs;
        c->le[i] = re;
    }
    c->nlit = s->lits->n;
    c->start = c->ls[0];
    c->end = c->le[s->lits->n - 1];
}

static bool gap_ok(search_t *s, size_t i, size_t next_norm_start) {
    /* literal i-1 gap must stay on one line when line_bounded */
    const plit_t *P = &s->lits->v[i - 1];
    if (!P->line_bounded) return true;
    size_t from = *P->norm ? s->nt->ends[s->le[i - 1] - 1] : 0;
    size_t to = next_norm_start < s->nt->n ? s->nt->starts[next_norm_start] : strlen(s->content);
    for (size_t k = from; k < to; k++)
        if (s->content[k] == '\n') return false;
    return true;
}

static void dfs(search_t *s, size_t i, size_t npos) {
    if (s->overflow) return;
    if (i == s->lits->n) {
        if (s->count >= MAX_TUPLES) {
            s->overflow = true;
            return;
        }
        s->count++;
        cand_t c;
        memset(&c, 0, sizeof(c));
        extend_candidate(s, &c);
        if (s->out->n < 200) vec_push(s->out, c);
        return;
    }
    const plit_t *L = &s->lits->v[i];
    if (!*L->norm) {
        s->ln[i] = s->le[i] = npos;
        dfs(s, i + 1, npos);
        return;
    }
    size_t ll = strlen(L->norm);
    const char *p = s->nt->text + npos;
    while ((p = strstr(p, L->norm))) {
        size_t at = (size_t)(p - s->nt->text);
        if (i > 0 && !gap_ok(s, i, at)) {
            if (s->lits->v[i - 1].line_bounded) break; /* later occurrences are further away */
        } else {
            s->ln[i] = at;
            s->le[i] = at + ll;
            dfs(s, i + 1, at + ll);
            if (s->overflow) return;
        }
        p++;
        while (*p && ((unsigned char)*p & 0xc0) == 0x80) p++;
    }
}

/* ======================================================================================
 * pattern compilation
 * ====================================================================================== */

typedef struct {
    plits_t lits;
    bool lead_edge, trail_edge;
} pattern_t;

static bool line_is_gap(const char *l) {
    char *t = fz_trim(l);
    bool g = !strcmp(t, GAP);
    free(t);
    return g;
}

/* strip open edge gaps from a line list; returns flags */
static void strip_edges(strvec_t *v, bool *lead, bool *trail) {
    *lead = *trail = false;
    while (v->n && line_is_gap(v->v[0])) {
        free(v->v[0]);
        memmove(v->v, v->v + 1, (v->n - 1) * sizeof(char *));
        v->n--;
        *lead = true;
    }
    while (v->n && line_is_gap(v->v[v->n - 1])) {
        free(v->v[--v->n]);
        *trail = true;
    }
    if (v->n) {
        char *first = v->v[0];
        const char *t = fz_trim_start(first);
        if (str_starts(t, GAP)) {
            char *n = xstrdup(t + strlen(GAP));
            free(first);
            v->v[0] = n;
            *lead = true;
        }
        char *last = v->v[v->n - 1];
        char *te = fz_trim_end(last);
        if (str_ends(te, GAP)) {
            te[strlen(te) - strlen(GAP)] = 0;
            free(last);
            v->v[v->n - 1] = te;
            *trail = true;
        } else {
            free(te);
        }
    }
}

static bool compile_pattern(const strvec_t *find_in, pattern_t *pt) {
    memset(pt, 0, sizeof(*pt));
    strvec_t v = {0};
    for (size_t i = 0; i < find_in->n; i++) strvec_push(&v, find_in->v[i]);
    strip_edges(&v, &pt->lead_edge, &pt->trail_edge);
    char *text = strvec_join(&v, "\n");
    strvec_free(&v);
    /* split on … */
    const char *p = text;
    for (;;) {
        const char *g = strstr(p, GAP);
        plit_t L;
        memset(&L, 0, sizeof(L));
        L.raw = g ? xstrndup(p, (size_t)(g - p)) : xstrdup(p);
        L.norm = normalize_str(L.raw);
        L.gap_after = g != NULL;
        if (g) {
            const char *after = g + strlen(GAP);
            /* line text before the gap on its line / after it */
            const char *ls = g;
            while (ls > text && ls[-1] != '\n') ls--;
            bool before_blank = true, after_blank = true;
            for (const char *q = ls; q < g; q++)
                if (!isspace((unsigned char)*q)) before_blank = false;
            for (const char *q = after; *q && *q != '\n'; q++)
                if (!isspace((unsigned char)*q)) after_blank = false;
            L.whole_line = before_blank && after_blank;
            L.line_bounded = !before_blank && !after_blank;
            if (L.whole_line) {
                /* drop the gap line's own indentation from the literal and its newline from the next */
                size_t keep = (size_t)(ls - p);
                L.raw[keep] = 0;
            }
            p = after; /* a whole-line gap's newline stays on the next literal (line-anchored) */
        }
        vec_push(&pt->lits, L);
        if (!g || pt->lits.n >= MAX_LITS) break;
    }
    free(text);
    return true;
}

static bool visible_identifier(const char *s) {
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        int k = utf8_decode(s + i, len - i, &cp);
        if (cp >= 0x80 || isalnum((int)cp) || cp == '_' || cp == '$') {
            if (cp < 0x80 || !fz_is_js_ws(cp)) return true;
        }
        i += (size_t)k;
    }
    return false;
}

/* ======================================================================================
 * apply
 * ====================================================================================== */

typedef struct {
    size_t start, end;
    char *repl;
    int op;
} pedit_t;
typedef VEC(pedit_t) pedits_t;

static int cmp_pedit(const void *a, const void *b) {
    const pedit_t *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end < y->end ? -1 : 1;
    return x->op - y->op;
}

typedef struct {
    const char *path;
    char *err;
    const char *kind;
    size_t nops;
} actx_t;

static bool aerr(actx_t *a, const char *kind, char *msg) {
    free(a->err);
    a->err = msg;
    a->kind = kind;
    return false;
}

static size_t line_number_at(const char *content, size_t off) {
    size_t n = 1, len = strlen(content);
    for (size_t i = 0; i < off && i < len; i++)
        if (content[i] == '\n') n++;
    return n;
}

static char *numbered_preview(const char *content, size_t off) {
    strvec_t lines = fz_split(content);
    size_t anchor = line_number_at(content, off) - 1;
    size_t start = anchor >= 4 ? anchor - 4 : 0;
    if (lines.n - MIN(start, lines.n) < 10) start = lines.n > 10 ? lines.n - 10 : 0;
    sb_t b = SB_INIT;
    for (size_t i = start; i < lines.n && i < start + 10; i++) {
        if (i > start) sb_putc(&b, '\n');
        sb_appendf(&b, "%zu: %s", i + 1, lines.v[i]);
    }
    strvec_free(&lines);
    return sb_detach(&b);
}

static char *display_fragment(const char *text) {
    if (strchr(text, '\n')) {
        size_t n = 1;
        for (const char *p = text; *p; p++)
            if (*p == '\n') n++;
        if (n <= 8) return xasprintf("\n%s", text);
    }
    sb_t b = SB_INIT;
    const char *p = text;
    bool first = true;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (!first) sb_putc(&b, ' ');
        sb_appendn(&b, s, (size_t)(p - s));
        first = false;
    }
    char *compact = sb_detach(&b);
    if (fz_char_count(compact) > 80) {
        size_t k = 0, i = 0;
        while (compact[i] && k < 77) {
            i++;
            while (compact[i] && ((unsigned char)compact[i] & 0xc0) == 0x80) i++;
            k++;
        }
        char *c2 = xasprintf("%.*s…", (int)i, compact);
        free(compact);
        compact = c2;
    }
    char *q = json_quote(compact);
    free(compact);
    return q;
}

static char *edit_header(const char *path, bool all) {
    return xasprintf("*** Edit File: %s%s", path, all ? " all" : "");
}

static char *op_payload(const sop_t *op, const char *path, bool all, const char *pattern) {
    char *h = edit_header(path, all);
    char *pat = pattern ? xstrdup(pattern) : strvec_join(&op->find, "\n");
    char *r;
    if (op->kind == RW_BEFORE || op->kind == RW_AFTER)
        r = xasprintf("%s\n*** Find\n%s\n%s\n%s", h, pat, op->kind == RW_BEFORE ? "*** Insert Before" : "*** Insert After",
                      op->insert ? op->insert : "");
    else if (op->kind == RW_EXPLICIT) {
        char *rw = strvec_join(&op->rewrite, "\n");
        r = xasprintf("%s\n*** Find\n%s\n*** Replace\n%s", h, pat, rw);
        free(rw);
    } else {
        r = xasprintf("%s\n*** Find\n%s\n", h, pat);
    }
    free(h);
    free(pat);
    return r;
}

/* closest line to a normalized fragment: (raw line, offset, score) */
static void closest_fragment(const char *content, const char *needle, char **best_text, size_t *best_off, double *best_score) {
    *best_text = xstrdup(needle);
    *best_off = 0;
    *best_score = 1.0;
    strvec_t lines = fz_split(content);
    size_t off = 0;
    size_t np;
    uint32_t *pc = fz_decode(needle, &np);
    bool have = false;
    for (size_t i = 0; i < lines.n; i++) {
        char *nl = normalize_str(lines.v[i]);
        if (*nl) {
            size_t nc;
            uint32_t *cc = fz_decode(nl, &nc);
            size_t d = fz_levenshtein(pc, np, cc, nc);
            double den = (double)MAX(MAX(strlen(needle), strlen(nl)), (size_t)1);
            double score = (double)d / den;
            if (!have || score < *best_score) {
                have = true;
                *best_score = score;
                free(*best_text);
                *best_text = fz_trim(lines.v[i]);
                *best_off = off;
            }
            free(cc);
        }
        free(nl);
        off += strlen(lines.v[i]) + 1;
    }
    free(pc);
    strvec_free(&lines);
}

static bool no_match_error(actx_t *a, const char *content, const pattern_t *pt, const sop_t *op, const strvec_t *source_find) {
    const char *literal = NULL, *needle = NULL;
    for (size_t i = 0; i < pt->lits.n; i++)
        if (*pt->lits.v[i].norm) {
            literal = pt->lits.v[i].raw;
            needle = pt->lits.v[i].norm;
            break;
        }
    char *pat_text = strvec_join(&op->find, "\n");
    if (!literal) literal = needle = pat_text;
    char *lit_trim = fz_trim(literal);
    char *nc = normalize_str(content);
    bool occurs = *needle && strstr(nc, needle);
    char *ctext;
    size_t coff;
    double cscore;
    closest_fragment(content, needle, &ctext, &coff, &cscore);
    char *frag = display_fragment(lit_trim);
    char *reason;
    size_t offset = coff;
    if (!occurs) {
        reason = xasprintf("Failed fragment: %s has 0 occurrences.", frag);
    } else {
        reason = xasprintf("Failed fragment: %s could not align.", frag);
        ntext_t nt;
        ntext_build(&nt, content);
        const char *f = strstr(nt.text, needle);
        offset = f ? nt.starts[f - nt.text] : 0;
        ntext_free(&nt);
    }
    char *first = op->all ? xasprintf("Operation %d (all) found 0 matches in %s. %s", op->number, a->path, reason)
                          : xasprintf("Operation %d did not match %s. %s", op->number, a->path, reason);
    sb_t missing = SB_INIT;
    if (has_marker_lines(source_find)) {
        strvec_t miss = {0};
        for (size_t i = 0; i < source_find->n; i++) {
            const char *l = source_find->v[i];
            char *am = marker_content(l, ADD_MARK), *rm = marker_content(l, REM_MARK);
            bool skip = am || rm || strstr(l, SEL_OPEN) || strstr(l, SEL_CLOSE);
            free(am);
            free(rm);
            if (skip) continue;
            strvec_t frs = str_split(l, GAP);
            bool any = false, absent = false;
            for (size_t k = 0; k < frs.n; k++) {
                char *nf = normalize_str(frs.v[k]);
                if (*nf) {
                    any = true;
                    if (!strstr(nc, nf)) absent = true;
                }
                free(nf);
            }
            strvec_free(&frs);
            if (any && absent) strvec_push_own(&miss, fz_trim(l));
        }
        if (miss.n) {
            sb_append(&missing, "\nUnmarked MATCH lines must already exist in the file; ");
            for (size_t i = 0; i < miss.n && i < 3; i++) {
                if (i) sb_append(&missing, ", ");
                char *d = display_fragment(miss.v[i]);
                sb_append(&missing, d);
                free(d);
            }
            if (miss.n > 3) sb_append(&missing, ", …");
            sb_appendf(&missing, " %s not. Copy real lines from the file, and mark new lines to insert with ＋.",
                       miss.n == 1 ? "does" : "do");
        }
        strvec_free(&miss);
    }
    bool standalone = a->nops == 1;
    char *correction;
    if (!occurs && cscore < 0.35 && *ctext && standalone) {
        /* replacen(literal, closest, 1) over the authored pattern */
        const char *at = strstr(pat_text, lit_trim);
        char *corrected = at ? xasprintf("%.*s%s%s", (int)(at - pat_text), pat_text, ctext, at + strlen(lit_trim))
                             : xstrdup(pat_text);
        char *pl = op_payload(op, a->path, op->all, corrected);
        correction = xasprintf("Copy-ready corrected operation:\n%s", pl);
        free(pl);
        free(corrected);
    } else if (standalone) {
        correction = xstrdup("No copy-ready correction — the closest current text is only a fuzzy match. Re-read the "
                             "region above and rebuild *** Find from the exact current text.");
    } else {
        correction = xstrdup("No copy-ready correction — retrying this operation alone would drop sibling "
                             "operations. Rebuild it inside the full payload.");
    }
    char *prev = numbered_preview(content, offset);
    char *msg = xasprintf("%s%s\nCurrent file content near the closest match (no re-read needed):\n%s\n%s", first,
                          sb_cstr(&missing), prev, correction);
    free(prev);
    free(correction);
    sb_free(&missing);
    free(first);
    free(reason);
    free(frag);
    free(ctext);
    free(nc);
    free(lit_trim);
    free(pat_text);
    return aerr(a, K_MATCH, msg);
}

static bool no_op_error(actx_t *a, int op, const char *content, size_t off, int64_t all_count) {
    char *base;
    if (op > 0) {
        if (all_count >= 0)
            base = xasprintf("Operation %d (all) matched %lld occurrences but all make no change to %s.", op,
                             (long long)all_count, a->path);
        else base = xasprintf("Operation %d makes no change to %s.", op, a->path);
    } else {
        base = xasprintf("Edits to %s made no change.", a->path);
    }
    char *msg;
    if (content) {
        char *prev = numbered_preview(content, off);
        msg = xasprintf("%s\nYour rewrite normalized to text identical to these lines. Indentation-only changes are "
                        "applied verbatim; adjust the authored *** Replace if another whitespace change was "
                        "intended.\nCurrent file content near the closest match (no re-read needed):\n%s",
                        base, prev);
        free(prev);
        free(base);
    } else {
        msg = base;
    }
    return aerr(a, K_NOCHANGE, msg);
}

/* render a Replace body: … re-emits captures in order */
static bool render_rewrite(actx_t *a, int opnum, const char *rewrite, char **caps, size_t ncaps, bool *whole_caps,
                           char **out) {
    sb_t b = SB_INIT;
    size_t used = 0;
    const char *p = rewrite;
    for (;;) {
        const char *g = strstr(p, GAP);
        if (!g) {
            sb_append(&b, p);
            break;
        }
        const char *ls = g;
        while (ls > rewrite && ls[-1] != '\n') ls--;
        const char *le = g + strlen(GAP);
        while (*le && *le != '\n') le++;
        bool whole = true;
        for (const char *q = ls; q < le; q++)
            if (q < g || q >= g + strlen(GAP)) {
                if (!isspace((unsigned char)*q)) whole = false;
            }
        if (used >= ncaps) {
            if (whole) {
                sb_free(&b);
                return aerr(a, K_MATCH,
                            xasprintf("Operation %d *** Replace has a whole-line … with no *** Find gap to re-emit. *** "
                                      "Replace is final text written verbatim: type the elided lines out, or add a "
                                      "matching … gap to *** Find. To write a literal … line, use the write tool.",
                                      opnum));
            }
            sb_appendn(&b, p, (size_t)(g - p) + strlen(GAP));
            p = g + strlen(GAP);
            continue;
        }
        const char *cap = caps[used];
        if (whole && whole_caps[used]) {
            /* replace the whole "…" line (with its own indentation) by the captured lines */
            sb_appendn(&b, p, (size_t)(ls - p));
            if (!*cap) {
                /* empty capture: drop the line and one adjoining newline */
                p = *le == '\n' ? le + 1 : le;
                if (!*le && b.len && b.s[b.len - 1] == '\n') sb_truncate(&b, b.len - 1);
            } else {
                sb_append(&b, cap);
                p = le;
            }
        } else {
            sb_appendn(&b, p, (size_t)(g - p));
            sb_append(&b, cap);
            p = g + strlen(GAP);
        }
        used++;
    }
    *out = sb_detach(&b);
    return true;
}

typedef struct {
    pedits_t edits;
    strvec_t notes;
    VEC(char *) deletions; /* indexed by op number - 1 (NULL = none) */
    size_t last_match;
    bool have_last;
} plan_t;

static size_t line_start_of(const char *c, size_t off) {
    while (off > 0 && c[off - 1] != '\n') off--;
    return off;
}
static size_t line_end_of(const char *c, size_t off) {
    while (c[off] && c[off] != '\n') off++;
    return off;
}

static bool only_hws(const char *c, size_t a, size_t b) {
    for (size_t i = a; i < b; i++)
        if (!is_hws(c[i]) && c[i] != '\r') return false;
    return true;
}

static size_t count_nonblank_lines(const char *s, size_t len) {
    size_t n = 0;
    const char *p = s, *e = s + len;
    while (p < e) {
        const char *nl = memchr(p, '\n', (size_t)(e - p));
        const char *le = nl ? nl : e;
        bool blank = true;
        for (const char *q = p; q < le; q++)
            if (!isspace((unsigned char)*q)) blank = false;
        if (!blank) n++;
        p = nl ? nl + 1 : e;
    }
    return n;
}

static bool blank_line_at(const char *c, size_t ls) {
    size_t le = line_end_of(c, ls);
    return only_hws(c, ls, le);
}

/* marker op rendering: keep matched file lines for context rows, drop － rows, insert ＋ rows */
static char *render_markers(const char *content, size_t rs, size_t re, const strvec_t *find) {
    /* raw lines of the region */
    strvec_t raw = {0};
    {
        char *reg = xstrndup(content + rs, re - rs);
        raw = fz_split(reg);
        free(reg);
    }
    /* pattern rows: kind 0 context, 1 removed, 2 added */
    size_t nrows = find->n;
    int *kind = xmalloc((nrows + 1) * sizeof(int));
    char **text = xmalloc((nrows + 1) * sizeof(char *));
    size_t nmapped = 0;
    for (size_t i = 0; i < nrows; i++) {
        char *am = marker_content(find->v[i], ADD_MARK), *rm = am ? NULL : marker_content(find->v[i], REM_MARK);
        kind[i] = am ? 2 : rm ? 1 : 0;
        text[i] = am ? am : rm ? rm : xstrdup(find->v[i]);
        if (kind[i] != 2 && !fz_blank(text[i])) nmapped++;
    }
    size_t raw_nonblank = 0;
    for (size_t i = 0; i < raw.n; i++)
        if (!fz_blank(raw.v[i])) raw_nonblank++;
    char *result = NULL;
    if (raw_nonblank == nmapped) {
        /* map pattern non-blank rows -> raw non-blank rows */
        int *raw_kind = xcalloc(raw.n + 1, sizeof(int));
        strvec_t *after = xcalloc(raw.n + 1, sizeof(strvec_t));
        strvec_t before = {0};
        size_t r = 0;
        int64_t last_mapped = -1;
        for (size_t i = 0; i < nrows; i++) {
            if (kind[i] == 2) {
                if (last_mapped < 0) strvec_push(&before, text[i]);
                else strvec_push(&after[last_mapped], text[i]);
                continue;
            }
            if (fz_blank(text[i])) continue;
            while (r < raw.n && fz_blank(raw.v[r])) r++;
            if (r >= raw.n) break;
            raw_kind[r] = kind[i];
            last_mapped = (int64_t)r;
            r++;
        }
        strvec_t out = {0};
        for (size_t i = 0; i < before.n; i++) strvec_push(&out, before.v[i]);
        for (size_t i = 0; i < raw.n; i++) {
            if (raw_kind[i] != 1) strvec_push(&out, raw.v[i]);
            for (size_t k = 0; k < after[i].n; k++) strvec_push(&out, after[i].v[k]);
            strvec_free(&after[i]);
        }
        result = strvec_join(&out, "\n");
        strvec_free(&out);
        strvec_free(&before);
        free(after);
        free(raw_kind);
    } else {
        strvec_t out = {0};
        for (size_t i = 0; i < nrows; i++)
            if (kind[i] != 1) strvec_push(&out, text[i]);
        result = strvec_join(&out, "\n");
        strvec_free(&out);
    }
    for (size_t i = 0; i < nrows; i++) free(text[i]);
    free(text);
    free(kind);
    strvec_free(&raw);
    return result;
}

/* marker op with whole-line … gaps: render each literal's rows over its own region, keep gap text verbatim */
static char *render_marker_groups(const char *content, size_t start, size_t end, const cand_t *c, const strvec_t *find,
                                  size_t nlits) {
    /* split authored rows at whole-line gap rows (open edge gaps dropped) */
    VEC(strvec_t) groups = {0};
    strvec_t cur = {0};
    size_t s0 = 0, e0 = find->n;
    while (s0 < e0 && line_is_gap(find->v[s0])) s0++;
    while (e0 > s0 && line_is_gap(find->v[e0 - 1])) e0--;
    for (size_t i = s0; i < e0; i++) {
        if (line_is_gap(find->v[i])) {
            vec_push(&groups, cur);
            memset(&cur, 0, sizeof(cur));
        } else {
            strvec_push(&cur, find->v[i]);
        }
    }
    vec_push(&groups, cur);
    char *r;
    if (groups.n <= 1 || groups.n != nlits) {
        r = render_markers(content, start, end, find);
    } else {
        sb_t b = SB_INIT;
        size_t pos = start;
        for (size_t i = 0; i < groups.n; i++) {
            size_t rs = i ? line_start_of(content, c->ls[i]) : start;
            if (rs < pos) rs = pos;
            size_t re = i + 1 < groups.n ? c->le[i] : end;
            if (re > rs && content[re - 1] == '\n') re--;
            sb_appendn(&b, content + pos, rs - pos);
            char *part = render_markers(content, rs, re, &groups.v[i]);
            sb_append(&b, part);
            free(part);
            pos = re;
        }
        sb_appendn(&b, content + pos, end > pos ? end - pos : 0);
        r = sb_detach(&b);
    }
    for (size_t i = 0; i < groups.n; i++) strvec_free(&groups.v[i]);
    vec_free(&groups);
    return r;
}

/* locate one op against content and append its planned edits */
static bool plan_op(actx_t *a, const char *content, const sop_t *op, plan_t *plan) {
    int num = op->number;
    strvec_t find = {0}, rewrite = {0};
    int kind = op->kind;
    bool markers = false;
    char *insert = op->insert ? xstrdup(op->insert) : NULL;
    for (size_t i = 0; i < op->find.n; i++) strvec_push(&find, op->find.v[i]);
    for (size_t i = 0; i < op->rewrite.n; i++) strvec_push(&rewrite, op->rewrite.v[i]);
    bool ok = false;
    pattern_t pt;
    memset(&pt, 0, sizeof(pt));
    cands_t cands = {0};
    ntext_t nt;
    memset(&nt, 0, sizeof(nt));

    if (kind == RW_NONE) {
        if (has_marker_lines(&find)) {
            markers = true;
            kind = RW_MARKERS;
        } else {
            char *src = strvec_join(&find, "\n");
            char *ns = normalize_str(src), *nc = normalize_str(content);
            const char *echo = *ns && strstr(nc, ns) ? "\nIts lines already exist in the file unchanged — if this "
                                                       "operation only restated context, delete it; anchors belong "
                                                       "inside the operation that edits them."
                                                     : "";
            char *h = edit_header(a->path, op->all);
            aerr(a, K_PARSE,
                 xasprintf("Operation %d has *** Find but no *** Replace.%s\nCopy-ready corrected payload (fill in the "
                           "new text):\n%s\n*** Find\n%s\n*** Replace\n{new text}\n",
                           num, echo, h, src));
            free(h);
            free(src);
            free(ns);
            free(nc);
            goto done;
        }
    } else if (kind == RW_EXPLICIT && has_marker_lines(&rewrite)) {
        /* ＋/－ rows inside Replace: drop － rows, strip ＋; an all-＋ body inserts after the match */
        bool all_add = true;
        strvec_t nr = {0};
        for (size_t i = 0; i < rewrite.n; i++) {
            char *am = marker_content(rewrite.v[i], ADD_MARK), *rm = am ? NULL : marker_content(rewrite.v[i], REM_MARK);
            if (!am && !fz_blank(rewrite.v[i])) all_add = false;
            if (rm) {
                all_add = false;
                free(rm);
                continue;
            }
            strvec_push_own(&nr, am ? am : xstrdup(rewrite.v[i]));
        }
        strvec_free(&rewrite);
        rewrite = nr;
        if (all_add) {
            char *t = strvec_join(&rewrite, "\n");
            free(insert);
            insert = xasprintf("%s\n", t);
            free(t);
            kind = RW_AFTER;
        }
    }

    /* the text that must be found: for marker ops, context + removed rows */
    strvec_t match_rows = {0};
    for (size_t i = 0; i < find.n; i++) {
        if (markers) {
            char *am = marker_content(find.v[i], ADD_MARK);
            if (am) {
                free(am);
                continue;
            }
            char *rm = marker_content(find.v[i], REM_MARK);
            strvec_push_own(&match_rows, rm ? rm : xstrdup(find.v[i]));
        } else {
            strvec_push(&match_rows, find.v[i]);
        }
    }
    {
        char *all_text = strvec_join(&find, "\n");
        bool blank = fz_blank(all_text);
        size_t opens = str_count(all_text, SEL_OPEN), closes = str_count(all_text, SEL_CLOSE);
        free(all_text);
        if (blank) {
            aerr(a, K_MATCH, xasprintf("Operation %d has an empty pattern.", num));
            strvec_free(&match_rows);
            goto done;
        }
        if (opens > closes || closes > opens) {
            aerr(a, K_MATCH,
                 opens > closes ? xasprintf("Operation %d has an unclosed selection marker ⟪; add closing ⟫.", num)
                                : xasprintf("Operation %d has an unmatched closing selection marker ⟫; add opening ⟪.", num));
            strvec_free(&match_rows);
            goto done;
        }
    }
    compile_pattern(&match_rows, &pt);
    strvec_free(&match_rows);
    {
        bool any = false, ident = false;
        for (size_t i = 0; i < pt.lits.n; i++)
            if (*pt.lits.v[i].norm) {
                any = true;
                if (visible_identifier(pt.lits.v[i].raw)) ident = true;
            }
        if (!any) {
            aerr(a, K_MATCH, xasprintf("Operation %d needs visible current text.", num));
            goto done;
        }
        if (!ident) {
            aerr(a, K_MATCH, xasprintf("Operation %d pattern is too generic; include a distinctive name or statement.", num));
            goto done;
        }
    }
    /* a Replace made only of … lines has nothing to re-emit */
    if (kind == RW_EXPLICIT && rewrite.n) {
        bool only_gaps = true;
        for (size_t i = 0; i < rewrite.n && only_gaps; i++) only_gaps = line_is_gap(rewrite.v[i]);
        if (only_gaps) {
            aerr(a, K_MATCH,
                 xasprintf("Operation %d *** Replace has a whole-line … with no *** Find gap to re-emit. *** Replace is "
                           "final text written verbatim: type the elided lines out, or add a matching … gap to *** "
                           "Find. To write a literal … line, use the write tool.",
                           num));
            goto done;
        }
    }
    /* Replace edge gaps re-emit as nothing */
    if (kind == RW_EXPLICIT && (pt.lead_edge || pt.trail_edge)) {
        /* only strip the edges the Find had */
        bool l2, t2;
        strvec_t copy = {0};
        for (size_t i = 0; i < rewrite.n; i++) strvec_push(&copy, rewrite.v[i]);
        strip_edges(&copy, &l2, &t2);
        if ((!l2 || pt.lead_edge) && (!t2 || pt.trail_edge)) {
            strvec_free(&rewrite);
            rewrite = copy;
        } else {
            strvec_free(&copy);
        }
    }

    ntext_build(&nt, content);
    search_t s;
    memset(&s, 0, sizeof(s));
    s.content = content;
    s.nt = &nt;
    s.lits = &pt.lits;
    s.out = &cands;
    dfs(&s, 0, 0);
    if (!cands.n) {
        no_match_error(a, content, &pt, op, &op->find);
        goto done;
    }
    if (s.overflow) {
        aerr(a, K_MATCH, xasprintf("Operation %d pattern is too broad; add another distinctive … anchor or more context.", num));
        goto done;
    }
    {
        /* raw exact match? (whitespace-lenient note for marker ops) */
        char *raw_pat = NULL;
        {
            sb_t b = SB_INIT;
            for (size_t i = 0; i < pt.lits.n; i++) sb_append(&b, pt.lits.v[i].raw);
            raw_pat = sb_detach(&b);
        }
        bool exact = pt.lits.n == 1 && strstr(content, raw_pat);
        free(raw_pat);

        /* render each candidate */
        size_t nc = cands.n;
        VEC(pedit_t) planned = {0};
        size_t noops = 0;
        for (size_t ci = 0; ci < nc; ci++) {
            cand_t *c = &cands.v[ci];
            /* all: skip candidates overlapping an earlier chosen one */
            if (op->all) {
                bool ov = false;
                for (size_t k = 0; k < planned.n; k++)
                    if (c->start < planned.v[k].end && planned.v[k].start < c->end) ov = true;
                for (size_t k = 0; k < ci && !ov; k++)
                    if (cands.v[k].start == c->start) ov = true; /* lazy: first tuple per start */
                if (ov) continue;
            }
            size_t start = c->start, end = c->end;
            char *repl = NULL;
            if (kind == RW_BEFORE || kind == RW_AFTER) {
                size_t at = kind == RW_BEFORE ? line_start_of(content, start) : line_end_of(content, end);
                if (kind == RW_AFTER) {
                    if (content[at] == '\n') {
                        at++;
                        repl = xstrdup(insert);
                    } else {
                        size_t il = strlen(insert);
                        repl = xasprintf("\n%.*s", (int)(il && insert[il - 1] == '\n' ? il - 1 : il), insert);
                    }
                } else {
                    repl = xstrdup(insert);
                }
                start = end = at;
                if (!*repl) {
                    free(repl);
                    noops++;
                    if (!plan->have_last) {
                        plan->last_match = c->start;
                        plan->have_last = true;
                    }
                    continue;
                }
            } else if (kind == RW_MARKERS) {
                start = line_start_of(content, start);
                if (!only_hws(content, start, c->start)) start = c->start;
                size_t le = line_end_of(content, end);
                if (only_hws(content, end, le)) end = le;
                repl = render_marker_groups(content, start, end, c, &op->find, pt.lits.n);
                if (!exact) {
                    char *note = xasprintf("Note: operation %d's *** Find differed from the file in whitespace only and "
                                           "was matched leniently. Inserted lines are written exactly as authored — "
                                           "verify their indentation.",
                                           num);
                    if (!strvec_contains(&plan->notes, note)) strvec_push(&plan->notes, note);
                    free(note);
                }
            } else {
                /* explicit Replace */
                char **caps = xcalloc(MAX_LITS, sizeof(char *));
                bool *whole = xcalloc(MAX_LITS, sizeof(bool));
                size_t ncaps = 0;
                for (size_t i = 0; i + 1 < pt.lits.n; i++) {
                    size_t cs = c->le[i], ce = c->ls[i + 1];
                    if (pt.lits.v[i].whole_line) {
                        /* whole lines between the anchors, without the newline before the next literal */
                        whole[ncaps] = true;
                        if (ce > cs && content[ce - 1] == '\n') ce--;
                    }
                    caps[ncaps++] = xstrndup(content + cs, ce > cs ? ce - cs : 0);
                }
                char *rw = strvec_join(&rewrite, "\n");
                if (pt.lits.v[0].raw[0] && is_hws(pt.lits.v[0].raw[0])) {
                    size_t ls = line_start_of(content, start);
                    if (only_hws(content, ls, start)) start = ls;
                }
                bool r = render_rewrite(a, num, rw, caps, ncaps, whole, &repl);
                free(rw);
                for (size_t i = 0; i < ncaps; i++) free(caps[i]);
                free(caps);
                free(whole);
                if (!r) {
                    for (size_t k = 0; k < planned.n; k++) free(planned.v[k].repl);
                    vec_free(&planned);
                    goto done;
                }
                if (!*repl) {
                    /* deletion of whole lines */
                    size_t ls = line_start_of(content, start), le = line_end_of(content, end);
                    if (only_hws(content, ls, start) && only_hws(content, end, le)) {
                        start = ls;
                        end = le;
                        if (content[end] == '\n') {
                            end++;
                            /* collapse a blank seam when both neighbours are blank */
                            if (start > 0 && content[end]) {
                                size_t prev_ls = line_start_of(content, start - 1);
                                if (blank_line_at(content, prev_ls) && blank_line_at(content, end)) {
                                    size_t nle = line_end_of(content, end);
                                    end = content[nle] == '\n' ? nle + 1 : nle;
                                }
                            }
                        } else if (start > 0) {
                            start--; /* last line without newline: take the preceding one */
                        }
                    }
                    size_t lines = count_nonblank_lines(content + start, end - start);
                    if (lines && (size_t)(num - 1) < plan->deletions.n && !plan->deletions.v[num - 1])
                        plan->deletions.v[num - 1] = xasprintf("Note: operation %d deleted %zu line(s); an empty *** "
                                                               "Replace means deletion — resend with the final text if "
                                                               "you meant to replace.",
                                                               num, lines);
                }
            }
            if (!plan->have_last) {
                plan->last_match = c->start;
                plan->have_last = true;
            }
            if (end - start == strlen(repl) && !memcmp(content + start, repl, end - start)) {
                free(repl);
                noops++;
                continue;
            }
            pedit_t e = {start, end, repl, num};
            vec_push(&planned, e);
            if (!op->all) break;
        }
        if (!op->all && cands.n > 1) {
            /* ambiguity: unless every candidate yields the same outcome */
            bool same = true;
            for (size_t ci = 1; ci < cands.n && same && planned.n; ci++)
                same = cands.v[ci].start == cands.v[0].start && cands.v[ci].end == cands.v[0].end;
            if (!same) {
                for (size_t k = 0; k < planned.n; k++) free(planned.v[k].repl);
                vec_free(&planned);
                sb_t b = SB_INIT;
                for (size_t ci = 0; ci < 2 && ci < cands.n; ci++) {
                    if (ci) sb_append(&b, "\n\n");
                    char *pl = op_payload(op, a->path, false, NULL);
                    sb_appendf(&b, "Near line %zu:\n%s", line_number_at(content, cands.v[ci].start), pl);
                    free(pl);
                }
                char *pl_all = NULL;
                if (kind == RW_EXPLICIT || kind == RW_BEFORE || kind == RW_AFTER) {
                    char *p = op_payload(op, a->path, true, NULL);
                    pl_all = xasprintf("\n\nAll candidates receive the same rewrite; retry every match:\n%s", p);
                    free(p);
                }
                aerr(a, K_MATCH,
                     xasprintf("Operation %d is ambiguous: %zu ordered tuples match.\n\nAdd context that only the intended "
                               "match has — one of these:\n\n%s%s",
                               num, s.count, sb_cstr(&b), pl_all ? pl_all : ""));
                free(pl_all);
                sb_free(&b);
                goto done;
            }
        }
        if (!planned.n) {
            vec_free(&planned);
            if (op->all) no_op_error(a, num, NULL, 0, (int64_t)noops);
            else no_op_error(a, num, content, plan->last_match, -1);
            goto done;
        }
        for (size_t k = 0; k < planned.n; k++) vec_push(&plan->edits, planned.v[k]);
        vec_free(&planned);
    }
    ok = true;
done:
    free(insert);
    strvec_free(&find);
    strvec_free(&rewrite);
    plits_free(&pt.lits);
    vec_free(&cands);
    ntext_free(&nt);
    return ok;
}

static void plan_free(plan_t *p) {
    for (size_t i = 0; i < p->edits.n; i++) free(p->edits.v[i].repl);
    vec_free(&p->edits);
    strvec_free(&p->notes);
    for (size_t i = 0; i < p->deletions.n; i++) free(p->deletions.v[i]);
    vec_free(&p->deletions);
}

char *sloppy_apply(const char *text, const char *body, const char *path, strvec_t *notes, char **err,
                   const char **err_kind) {
    *err = NULL;
    sops_t ops = {0};
    parse_ir(body, &ops);
    actx_t a = {path, NULL, K_MATCH, ops.n};
    char *cur = xstrdup(text);
    bool *pending = xcalloc(ops.n + 1, sizeof(bool));
    for (size_t i = 0; i < ops.n; i++) pending[i] = true;
    int *overlapped = xcalloc(ops.n + 1, sizeof(int)); /* later -> earlier (1-based), 0 none */
    strvec_t all_notes = {0};
    char **deletions = xcalloc(ops.n + 1, sizeof(char *));
    bool failed = false, have_last = false;
    size_t last_match = 0;
    for (;;) {
        bool any = false;
        for (size_t i = 0; i < ops.n; i++) any |= pending[i];
        if (!any) break;
        plan_t plan;
        memset(&plan, 0, sizeof(plan));
        for (size_t i = 0; i < ops.n; i++) vec_push(&plan.deletions, NULL);
        for (size_t i = 0; i < ops.n && !failed; i++)
            if (pending[i] && !plan_op(&a, cur, &ops.v[i], &plan)) failed = true;
        if (failed) {
            sb_t pre = SB_INIT;
            for (size_t i = 0; i < ops.n; i++)
                if (overlapped[i])
                    sb_appendf(&pre, "Note: operation %zu overlaps operation %d, so it was located in the file as "
                                     "operation %d left it.\n",
                               i + 1, overlapped[i], overlapped[i]);
            if (pre.len) {
                char *m = xasprintf("%s%s", sb_cstr(&pre), a.err);
                free(a.err);
                a.err = m;
            }
            sb_free(&pre);
            plan_free(&plan);
            break;
        }
        if (!have_last && plan.have_last) {
            last_match = plan.last_match;
            have_last = true;
        }
        qsort(plan.edits.v, plan.edits.n, sizeof(pedit_t), cmp_pedit);
        /* defer later ops whose edits overlap an earlier op's */
        int *deferred = xcalloc(ops.n + 1, sizeof(int));
        for (size_t i = 0; i < plan.edits.n; i++)
            for (size_t j = i + 1; j < plan.edits.n; j++) {
                pedit_t *x = &plan.edits.v[i], *y = &plan.edits.v[j];
                if (x->op == y->op || deferred[x->op - 1] || deferred[y->op - 1]) continue;
                /* merge_edits: an insertion at a replacement's start stays beside it, two insertions at one spot do not */
                bool ov = (x->start < y->end && y->start < x->end) || (x->start == y->start && x->end == x->start && y->end == y->start);
                if (!ov) continue;
                int later = MAX(x->op, y->op), earlier = MIN(x->op, y->op);
                deferred[later - 1] = earlier;
            }
        sb_t b = SB_INIT;
        size_t pos = 0;
        for (size_t i = 0; i < plan.edits.n; i++) {
            pedit_t *e = &plan.edits.v[i];
            if (deferred[e->op - 1] || e->start < pos) continue;
            sb_appendn(&b, cur + pos, e->start - pos);
            sb_append(&b, e->repl);
            pos = e->end;
        }
        sb_append(&b, cur + pos);
        free(cur);
        cur = sb_detach(&b);
        for (size_t i = 0; i < plan.notes.n; i++) strvec_push(&all_notes, plan.notes.v[i]);
        for (size_t i = 0; i < ops.n; i++) {
            if (!deferred[i] && plan.deletions.v[i] && !deletions[i]) {
                deletions[i] = plan.deletions.v[i];
                plan.deletions.v[i] = NULL;
            }
            if (!deferred[i] && overlapped[i] && pending[i]) {
                char *n = xasprintf("Note: operation %zu overlaps operation %d, so it was located in the file as "
                                    "operation %d left it.",
                                    i + 1, overlapped[i], overlapped[i]);
                strvec_push_own(&all_notes, n);
            }
        }
        for (size_t i = 0; i < ops.n; i++) {
            pending[i] = deferred[i] != 0;
            overlapped[i] = deferred[i];
        }
        free(deferred);
        plan_free(&plan);
    }
    if (!failed && !strcmp(cur, text)) {
        no_op_error(&a, 0, text, have_last ? last_match : 0, -1);
        failed = true;
    }
    char *result = NULL;
    if (failed) {
        if (strstr(a.err, ATOMICITY)) *err = a.err;
        else {
            *err = xasprintf("%s\n%s", ATOMICITY, a.err);
            free(a.err);
        }
        *err_kind = a.kind;
        free(cur);
    } else {
        for (size_t i = 0; i < all_notes.n; i++) strvec_push(notes, all_notes.v[i]);
        for (size_t i = 0; i < ops.n; i++)
            if (deletions[i]) strvec_push(notes, deletions[i]);
        result = cur;
        free(a.err);
    }
    for (size_t i = 0; i < ops.n; i++) free(deletions[i]);
    free(deletions);
    strvec_free(&all_notes);
    free(pending);
    free(overlapped);
    sops_free(&ops);
    return result;
}
