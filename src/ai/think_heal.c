/* think_heal.c - see think_heal.h. All delimiters are ASCII, so byte offsets match upstream's string indexes. */
#include "ai/think_heal.h"

typedef struct {
    const char *open, *close;
    bool fenced;
    bool implied_open; /* chat templates prefill this opener, so a bare close can imply it */
} tag_t;

static const tag_t TAGS[] = {
    {"<think>", "</think>", false, true},
    {"<thinking>", "</thinking>", false, false},
    {"<scratchpad>", "</scratchpad>", false, false},
    {"```thinking\n", "```", true, false},
    {"<|channel>thought\n", "<channel|>", false, false},
    {"<|start|>assistant<|channel|>analysis<|message|>", "<|end|>", false, false},
    {"<|channel|>analysis<|message|>", "<|end|>", false, false},
};
/* longest delimiter (opens + the implied "</think>" close): a held tail must be shorter */
#define MAX_DELIM 48

void think_heal_init(think_heal_t *h) { memset(h, 0, sizeof(*h)); }

void think_heal_free(think_heal_t *h) {
    sb_free(&h->buf);
    sb_free(&h->fbuf);
}

typedef struct {
    think_heal_emit_fn emit;
    void *ud;
    think_heal_t *h;
} out_t;

static int trailing_line_indent(const char *s, size_t n, int prior) {
    size_t start = 0;
    int indent = prior;
    for (size_t i = n; i > 0; i--) {
        if (s[i - 1] == '\n') {
            start = i;
            indent = 0;
            break;
        }
    }
    for (size_t i = start; i < n; i++) {
        if (indent == -1) break;
        indent = s[i] == ' ' ? indent + 1 : -1;
    }
    return indent;
}

static void emit_text(out_t *o, const char *s, size_t n) {
    if (!n) return;
    o->emit(o->ud, THINK_HEAL_TEXT, s, n);
    o->h->line_indent = trailing_line_indent(s, n, o->h->line_indent);
}

static void emit_thinking(out_t *o, const char *s, size_t n) {
    if (n) o->emit(o->ud, THINK_HEAL_THINKING, s, n);
}

static void buf_consume(sb_t *b, size_t n) {
    if (n >= b->len) {
        sb_clear(b);
        return;
    }
    memmove(b->s, b->s + n, b->len - n);
    sb_truncate(b, b->len - n);
}

static const char *find_bytes(const char *s, size_t n, const char *lit) {
    size_t l = strlen(lit);
    for (size_t i = 0; l <= n && i <= n - l; i++)
        if (s[i] == lit[0] && memcmp(s + i, lit, l) == 0) return s + i;
    return NULL;
}

static bool starts_at(const char *s, size_t n, size_t i, const char *lit) {
    size_t l = strlen(lit);
    return i + l <= n && memcmp(s + i, lit, l) == 0;
}

static size_t backtick_run(const char *s, size_t n, size_t from) {
    size_t e = from;
    while (e < n && s[e] == '`') e++;
    return e - from;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static bool blank(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!is_ws(s[i])) return false;
    return true;
}

static bool lang_token(const char *s, size_t n) {
    if (!n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' ||
              c == '#' || c == '-'))
            return false;
    }
    return true;
}

/* BACKTICK_LEAD: ≤3 lead spaces, a backtick run, the rest */
static void backtick_lead(const char *s, size_t n, size_t *ticks, size_t *rest) {
    size_t i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;
    size_t t = backtick_run(s, n, i);
    *ticks = t;
    *rest = i + t;
}

/* ---------- FencedThinkingScanner ---------- */

/* complete-line close test; returns true with *rest_off set when the line closes thinking */
static bool fence_close_line(const char *line, size_t n, size_t *rest_off) {
    size_t ticks, r;
    backtick_lead(line, n, &ticks, &r);
    if (ticks < 3) return false;
    if (r == n || blank(line + r, n - r)) {
        *rest_off = n; /* bare close */
        return true;
    }
    if (lang_token(line + r, n - r)) return false;
    *rest_off = r;
    return true;
}

static void fence_update_inner(think_heal_t *h, const char *line, size_t n) {
    if (memchr(line, '\r', n)) return;
    size_t i = 0;
    while (i < n && i < 3 && line[i] == ' ') i++;
    if (i >= n || (line[i] != '`' && line[i] != '~')) return;
    char ch = line[i];
    size_t run = 0;
    while (i + run < n && line[i + run] == ch) run++;
    if (run < 3) return;
    const char *info = line + i + run;
    size_t in = n - i - run;
    if (!h->inner_ch) {
        h->inner_ch = ch;
        h->inner_len = run;
    } else if (ch == h->inner_ch && run >= h->inner_len && blank(info, in)) {
        h->inner_ch = 0;
        h->inner_len = 0;
    }
}

static void fence_reset(think_heal_t *h) {
    sb_clear(&h->fbuf);
    h->inner_ch = 0;
    h->inner_len = 0;
    h->femitted = 0;
}

/* Feed bytes; thinking deltas go out through o. Returns true once closed; *rest receives the bytes after the closer. */
static bool fence_feed(think_heal_t *h, const char *s, size_t n, bool final, out_t *o, sb_t *rest) {
    sb_appendn(&h->fbuf, s, n);
    for (;;) {
        char *b = h->fbuf.s;
        size_t len = h->fbuf.len;
        char *nl = len ? memchr(b, '\n', len) : NULL;
        if (!nl) break;
        size_t ln = (size_t)(nl - b);
        if (!h->inner_ch) {
            size_t roff;
            if (fence_close_line(b, ln, &roff)) {
                sb_appendn(rest, b + roff, len - roff); /* keep the newline with the reply */
                fence_reset(h);
                return true;
            }
        }
        emit_thinking(o, b + h->femitted, ln + 1 - h->femitted);
        fence_update_inner(h, b, ln);
        buf_consume(&h->fbuf, ln + 1);
        h->femitted = 0;
    }
    const char *tail = h->fbuf.s ? h->fbuf.s : "";
    size_t tl = h->fbuf.len;
    if (h->inner_ch) {
        emit_thinking(o, tail + h->femitted, tl - h->femitted);
        h->femitted = tl;
        return false;
    }
    size_t ticks, r;
    backtick_lead(tail, tl, &ticks, &r);
    bool rest_blank = r == tl || blank(tail + r, tl - r);
    if (final) {
        if (ticks >= 3) {
            if (!rest_blank) sb_appendn(rest, tail + r, tl - r);
            fence_reset(h);
            return true;
        }
    } else {
        if (ticks >= 3 && !rest_blank && !lang_token(tail + r, tl - r)) {
            sb_appendn(rest, tail + r, tl - r);
            fence_reset(h);
            return true;
        }
        bool hold;
        if (rest_blank) {
            bool spaces_only = tl <= 3;
            for (size_t i = 0; i < tl && spaces_only; i++)
                if (tail[i] != ' ') spaces_only = false;
            hold = ticks >= 1 || spaces_only;
        } else {
            hold = ticks >= 3 && lang_token(tail + r, tl - r);
        }
        if (hold) return false;
    }
    emit_thinking(o, tail + h->femitted, tl - h->femitted);
    if (final) fence_reset(h);
    else h->femitted = tl;
    return false;
}

/* ---------- ThinkingInbandScanner ---------- */

enum { HIT_NONE, HIT_TAG, HIT_IMPLIED_CLOSE, HIT_CODE, HIT_HOLD };

static int scan_visible(const char *b, size_t n, bool final, bool implied, size_t *idx, const tag_t **tag, size_t *ticks) {
    size_t hold_from = final ? n : (n + 1 > MAX_DELIM ? n + 1 - MAX_DELIM : 0);
    for (size_t i = 0; i < n; i++) {
        char c = b[i];
        if (c != '<' && c != '`') continue;
        for (size_t t = 0; t < ARRAY_LEN(TAGS); t++) {
            if (starts_at(b, n, i, TAGS[t].open)) {
                *idx = i;
                *tag = &TAGS[t];
                return HIT_TAG;
            }
        }
        if (implied) {
            for (size_t t = 0; t < ARRAY_LEN(TAGS); t++) {
                if (TAGS[t].implied_open && starts_at(b, n, i, TAGS[t].close)) {
                    *idx = i;
                    *tag = &TAGS[t];
                    return HIT_IMPLIED_CLOSE;
                }
            }
        }
        if (i >= hold_from) {
            size_t rl = n - i;
            for (size_t t = 0; t < ARRAY_LEN(TAGS); t++) {
                if ((strlen(TAGS[t].open) > rl && memcmp(TAGS[t].open, b + i, rl) == 0) ||
                    (implied && TAGS[t].implied_open && strlen(TAGS[t].close) > rl && memcmp(TAGS[t].close, b + i, rl) == 0)) {
                    *idx = i;
                    return HIT_HOLD;
                }
            }
        }
        if (c == '`') {
            size_t run = backtick_run(b, n, i);
            *idx = i;
            if (!final && i + run == n) return HIT_HOLD;
            *ticks = run;
            return HIT_CODE;
        }
    }
    return HIT_NONE;
}

/* first maximal backtick run of exactly `ticks` at/after `from`, or (size_t)-1 */
static size_t find_backtick_run(const char *b, size_t n, size_t from, size_t ticks) {
    for (size_t i = from; i < n;) {
        if (b[i] != '`') {
            i++;
            continue;
        }
        size_t run = backtick_run(b, n, i);
        if (run == ticks) return i;
        i += run;
    }
    return (size_t)-1;
}

static size_t find_fence_close_end(const char *b, size_t n, size_t ticks, bool final) {
    for (size_t start = 0; start <= n;) {
        const char *nl = start < n ? memchr(b + start, '\n', n - start) : NULL;
        size_t end = nl ? (size_t)(nl - b) : n;
        size_t ls = start, le = end;
        while (ls < le && is_ws(b[ls])) ls++;
        while (le > ls && is_ws(b[le - 1])) le--;
        bool all_ticks = le > ls;
        for (size_t i = ls; i < le; i++)
            if (b[i] != '`') all_ticks = false;
        if (all_ticks && le - ls >= ticks && (nl || final)) return nl ? end + 1 : n;
        if (!nl) break;
        start = end + 1;
    }
    return (size_t)-1;
}

/* inside a Markdown code region; returns true when it closed and the loop should continue */
static bool emit_code(think_heal_t *h, bool final, out_t *o) {
    const char *b = h->buf.s;
    size_t n = h->buf.len;
    if (h->code_fenced) {
        size_t end = find_fence_close_end(b, n, (size_t)h->code_ticks, final);
        if (end != (size_t)-1) {
            emit_text(o, b, end);
            buf_consume(&h->buf, end);
            h->code_ticks = 0;
            h->code_fenced = false;
            return true;
        }
        if (final) {
            emit_text(o, b, n);
            sb_clear(&h->buf);
            h->code_ticks = 0;
            h->code_fenced = false;
            return false;
        }
        for (size_t i = n; i > 0; i--) {
            if (b[i - 1] == '\n') {
                emit_text(o, b, i);
                buf_consume(&h->buf, i);
                break;
            }
        }
        return false;
    }
    size_t close = find_backtick_run(b, n, 0, (size_t)h->code_ticks);
    if (close != (size_t)-1 && (final || close + (size_t)h->code_ticks < n)) {
        size_t e = close + (size_t)h->code_ticks;
        emit_text(o, b, e);
        buf_consume(&h->buf, e);
        h->code_ticks = 0;
        return true;
    }
    size_t hold = 0;
    if (!final)
        while (hold < n && b[n - 1 - hold] == '`') hold++;
    emit_text(o, b, n - hold);
    buf_consume(&h->buf, n - hold);
    if (final) h->code_ticks = 0;
    return false;
}

static void consume(think_heal_t *h, bool final, out_t *o) {
    for (int guard = 0; guard < 1000000; guard++) {
        if (h->fenced) {
            sb_t rest = SB_INIT;
            size_t plen = h->buf.len;
            char *pending = sb_detach(&h->buf);
            bool closed = fence_feed(h, pending, plen, final, o, &rest);
            free(pending);
            if (closed) sb_appendn(&h->buf, rest.s ? rest.s : "", rest.len);
            sb_free(&rest);
            if (closed || final) {
                h->close_tag = NULL;
                h->fenced = false;
                fence_reset(h);
            }
            if (h->fenced) break;
            continue;
        }
        if (h->buf.len == 0) break;
        if (h->close_tag) {
            const char *b = h->buf.s;
            const char *hit = find_bytes(b, h->buf.len, h->close_tag);
            if (!hit) {
                size_t hold = 0;
                if (!final) {
                    size_t cl = strlen(h->close_tag);
                    size_t max = MIN(h->buf.len, cl - 1);
                    for (size_t k = max; k > 0; k--) {
                        if (memcmp(b + h->buf.len - k, h->close_tag, k) == 0) {
                            hold = k;
                            break;
                        }
                    }
                }
                emit_thinking(o, b, h->buf.len - hold);
                buf_consume(&h->buf, h->buf.len - hold);
                break;
            }
            size_t at = (size_t)(hit - b);
            emit_thinking(o, b, at);
            buf_consume(&h->buf, at + strlen(h->close_tag));
            h->close_tag = NULL;
            continue;
        }
        if (h->code_ticks > 0) {
            if (emit_code(h, final, o)) continue;
            break;
        }
        size_t idx = 0, ticks = 0;
        const tag_t *tag = NULL;
        int kind = scan_visible(h->buf.s, h->buf.len, final, h->implied_open, &idx, &tag, &ticks);
        if (kind == HIT_NONE) {
            emit_text(o, h->buf.s, h->buf.len);
            sb_clear(&h->buf);
            break;
        }
        if (idx > 0) emit_text(o, h->buf.s, idx);
        if (kind == HIT_HOLD) {
            buf_consume(&h->buf, idx);
            break;
        }
        if (kind == HIT_IMPLIED_CLOSE) {
            buf_consume(&h->buf, idx + strlen(tag->close));
            o->emit(o->ud, THINK_HEAL_IMPLIED_END, NULL, 0);
            continue;
        }
        if (kind == HIT_CODE) {
            bool fenced = ticks >= 3 && h->line_indent >= 0 && h->line_indent <= 3;
            emit_text(o, h->buf.s + idx, ticks);
            buf_consume(&h->buf, idx + ticks);
            h->code_ticks = (int)ticks;
            h->code_fenced = fenced;
            continue;
        }
        buf_consume(&h->buf, idx + strlen(tag->open));
        h->close_tag = tag->close;
        if (tag->fenced) {
            h->fenced = true;
            fence_reset(h);
        }
    }
}

void think_heal_feed(think_heal_t *h, const char *s, size_t n, think_heal_emit_fn emit, void *ud) {
    if (!n) return;
    sb_appendn(&h->buf, s, n);
    out_t o = {emit, ud, h};
    consume(h, false, &o);
}

void think_heal_flush(think_heal_t *h, think_heal_emit_fn emit, void *ud) {
    out_t o = {emit, ud, h};
    consume(h, true, &o);
    if (h->buf.len == 0) return;
    if (h->close_tag) emit_thinking(&o, h->buf.s, h->buf.len);
    else emit_text(&o, h->buf.s, h->buf.len);
    sb_clear(&h->buf);
    h->close_tag = NULL;
}
