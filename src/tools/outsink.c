/* outsink.c - see outsink.h */
#include "outsink.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#define ELLIPSIS "\xE2\x80\xA6"
#define ELLIPSIS_LEN 3

/* =====================================================================================
 * text helpers
 * ===================================================================================== */

size_t out_count_nl(const char *s, size_t len) {
    size_t n = 0;
    for (const char *p = s, *e = s + len; p < e && (p = memchr(p, '\n', (size_t)(e - p))); p++) n++;
    return n;
}

char *out_format_bytes(int64_t b) {
    if (b < 1024) return xasprintf("%lldB", (long long)b);
    if (b < 1024 * 1024) return xasprintf("%.1fKB", (double)b / 1024.0);
    if (b < 1024LL * 1024 * 1024) return xasprintf("%.1fMB", (double)b / (1024.0 * 1024.0));
    return xasprintf("%.1fGB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

char *out_format_grouped(int64_t n) {
    char digits[32];
    snprintf(digits, sizeof digits, "%lld", (long long)(n < 0 ? -n : n));
    size_t dl = strlen(digits);
    sb_t b = SB_INIT;
    if (n < 0) sb_putc(&b, '-');
    for (size_t i = 0; i < dl; i++) {
        if (i && (dl - i) % 3 == 0) sb_putc(&b, ',');
        sb_putc(&b, digits[i]);
    }
    return sb_detach(&b);
}

char *out_middle_marker(int64_t elided_lines, int64_t elided_bytes) {
    if (elided_lines <= 1) return xasprintf("[" ELLIPSIS "%lldB elided" ELLIPSIS "]", (long long)elided_bytes);
    return xasprintf("[" ELLIPSIS "%lldln elided" ELLIPSIS "]", (long long)elided_lines);
}

/* ---------- UTF-8 decoding (WHATWG, streaming) ---------- */

typedef struct {
    unsigned char seq[4];
    int have, need;
    unsigned char lo, hi;
} u8dec_t;

static void u8_emit_fffd(sb_t *o) { sb_appendn(o, "\xEF\xBF\xBD", 3); }

static void u8dec_feed(u8dec_t *d, const unsigned char *s, size_t n, sb_t *o) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (d->need == 0) {
            /* fast path: run of ASCII */
            size_t j = i;
            while (j < n && s[j] < 0x80) j++;
            if (j > i) {
                sb_appendn(o, (const char *)s + i, j - i);
                i = j;
                continue;
            }
            d->lo = 0x80, d->hi = 0xBF;
            if (c >= 0xC2 && c <= 0xDF) d->need = 1;
            else if (c >= 0xE0 && c <= 0xEF) {
                if (c == 0xE0) d->lo = 0xA0;
                if (c == 0xED) d->hi = 0x9F;
                d->need = 2;
            } else if (c >= 0xF0 && c <= 0xF4) {
                if (c == 0xF0) d->lo = 0x90;
                if (c == 0xF4) d->hi = 0x8F;
                d->need = 3;
            } else {
                u8_emit_fffd(o);
                i++;
                continue;
            }
            d->seq[0] = c;
            d->have = 1;
            i++;
            continue;
        }
        if (c < d->lo || c > d->hi) {
            /* invalid continuation: replace the maximal subpart, reprocess this byte */
            d->need = d->have = 0;
            u8_emit_fffd(o);
            continue;
        }
        d->lo = 0x80, d->hi = 0xBF;
        d->seq[d->have++] = c;
        i++;
        if (d->have == d->need + 1) {
            sb_appendn(o, (const char *)d->seq, (size_t)d->have);
            d->need = d->have = 0;
        }
    }
}

static void u8dec_flush(u8dec_t *d, sb_t *o) {
    if (d->need) u8_emit_fffd(o);
    d->need = d->have = 0;
}

/* ---------- ANSI escape stripping (Bun.stripANSI) ---------- */

/* length of the escape sequence at s[0] (ESC or a C1 introducer encoded as C2 xx); 0 = incomplete at end
 * (only when `partial_ok`), else >= 1 bytes to drop */
static size_t ansi_seq_len(const unsigned char *s, size_t n, bool partial_ok, bool *incomplete) {
    *incomplete = false;
    size_t i;
    int kind; /* 1 CSI, 2 OSC, 3 string (DCS/SOS/PM/APC), 4 nF/Fp/Fe escape */
    if (s[0] == 0x1B) {
        if (n < 2) {
            if (partial_ok) *incomplete = true;
            return 1;
        }
        unsigned char c = s[1];
        if (c == '[') kind = 1;
        else if (c == ']') kind = 2;
        else if (c == 'P' || c == 'X' || c == '^' || c == '_') kind = 3;
        else if (c >= 0x20 && c <= 0x7E) kind = 4;
        else return 1; /* lone ESC */
        i = 2;
        if (kind == 4) {
            i = 1;
            while (i < n && s[i] >= 0x20 && s[i] <= 0x2F) i++;
            if (i >= n) {
                if (partial_ok) *incomplete = true;
                return i;
            }
            if (s[i] >= 0x30 && s[i] <= 0x7E) return i + 1;
            return i;
        }
    } else { /* C2 9B / 9D / 90 98 9E 9F */
        unsigned char c = s[1];
        kind = c == 0x9B ? 1 : c == 0x9D ? 2 : 3;
        i = 2;
    }
    if (kind == 1) {
        while (i < n && s[i] >= 0x20 && s[i] <= 0x3F) i++;
        if (i >= n) {
            if (partial_ok) *incomplete = true;
            return i;
        }
        if (s[i] >= 0x40 && s[i] <= 0x7E) return i + 1;
        return i;
    }
    /* OSC / string: until BEL (OSC), ESC \ or C2 9C */
    for (; i < n; i++) {
        if (s[i] == 0x07 && kind == 2) return i + 1;
        if (s[i] == 0x1B && i + 1 < n && s[i + 1] == '\\') return i + 2;
        if (s[i] == 0xC2 && i + 1 < n && s[i + 1] == 0x9C) return i + 2;
    }
    if (partial_ok) *incomplete = true;
    return n;
}

static bool is_ansi_intro(const unsigned char *s, size_t n, size_t i) {
    if (s[i] == 0x1B) return true;
    if (s[i] == 0xC2 && i + 1 < n) {
        unsigned char c = s[i + 1];
        return c == 0x9B || c == 0x9D || c == 0x90 || c == 0x98 || c == 0x9E || c == 0x9F;
    }
    return false;
}

/* drop controls (C0 except \t \n, DEL, C1) from valid UTF-8; appends to o */
static void drop_controls(const unsigned char *s, size_t n, sb_t *o) {
    size_t i = 0, run = 0;
    while (i < n) {
        unsigned char c = s[i];
        size_t k = 0;
        if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) k = 1;
        else if (c == 0xC2 && i + 1 < n && s[i + 1] >= 0x80 && s[i + 1] <= 0x9F) k = 2;
        if (!k) {
            i++;
            continue;
        }
        sb_appendn(o, (const char *)s + run, i - run);
        i += k;
        run = i;
    }
    sb_appendn(o, (const char *)s + run, n - run);
}

/* sanitizeWellFormedText on valid UTF-8 */
static void sanitize_valid(const unsigned char *s, size_t n, sb_t *o) {
    if (!memchr(s, 0x1B, n)) {
        drop_controls(s, n, o);
        return;
    }
    sb_t t = SB_INIT;
    size_t i = 0, run = 0;
    while (i < n) {
        if (!is_ansi_intro(s, n, i)) {
            i++;
            continue;
        }
        sb_appendn(&t, (const char *)s + run, i - run);
        bool inc;
        i += ansi_seq_len(s + i, n - i, false, &inc);
        run = i;
    }
    sb_appendn(&t, (const char *)s + run, n - run);
    drop_controls((const unsigned char *)t.s, t.len, o);
    sb_free(&t);
}

char *out_sanitize(const char *s, size_t len, size_t *out_len) {
    sb_t u = SB_INIT, o = SB_INIT;
    u8dec_t d = {0};
    u8dec_feed(&d, (const unsigned char *)s, len, &u);
    u8dec_flush(&d, &u);
    /* toWellFormed only replaces lone surrogates (none in decoded UTF-8): U+FFFD from decoding stays */
    sanitize_valid((const unsigned char *)u.s, u.len, &o);
    sb_free(&u);
    if (out_len) *out_len = o.len;
    return sb_detach(&o);
}

static void normalize_cr(const char *s, size_t n, bool *pending_cr, sb_t *o) {
    size_t i = 0;
    if (*pending_cr && n) {
        *pending_cr = false;
        sb_putc(o, '\n');
        if (s[0] == '\n') i = 1;
    }
    while (i < n) {
        const char *cr = memchr(s + i, '\r', n - i);
        if (!cr) {
            sb_appendn(o, s + i, n - i);
            break;
        }
        size_t at = (size_t)(cr - s);
        sb_appendn(o, s + i, at - i);
        if (at == n - 1) {
            *pending_cr = true;
            break;
        }
        sb_putc(o, '\n');
        i = s[at + 1] == '\n' ? at + 2 : at + 1;
    }
}

char *out_sanitize_lines(const char *s, size_t len, size_t *out_len) {
    sb_t u = SB_INIT, c = SB_INIT, o = SB_INIT;
    u8dec_t d = {0};
    u8dec_feed(&d, (const unsigned char *)s, len, &u);
    u8dec_flush(&d, &u);
    bool pcr = false;
    normalize_cr(u.s ? u.s : "", u.len, &pcr, &c);
    if (pcr) sb_putc(&c, '\n');
    sanitize_valid((const unsigned char *)(c.s ? c.s : ""), c.len, &o);
    sb_free(&u);
    sb_free(&c);
    if (out_len) *out_len = o.len;
    return sb_detach(&o);
}

/* =====================================================================================
 * byte windows + truncation
 * ===================================================================================== */

size_t out_head_bytes(const char *s, size_t len, size_t max) {
    if (len <= max) return len;
    size_t i = max;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

size_t out_tail_start(const char *s, size_t len, size_t max) {
    if (len <= max) return 0;
    size_t i = len - max;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void trunc_init(out_trunc_t *r) {
    memset(r, 0, sizeof *r);
    r->elided_bytes = r->elided_lines = r->head_lines = r->tail_lines = -1;
}

static void no_trunc(const char *s, size_t len, int64_t total_lines, int64_t total_bytes, out_trunc_t *r) {
    trunc_init(r);
    r->content = xstrndup(s, len);
    r->content_len = len;
    r->total_lines = total_lines;
    r->total_bytes = total_bytes;
    r->output_lines = total_lines;
    r->output_bytes = total_bytes;
}

void out_trunc_free(out_trunc_t *r) {
    free(r->content);
    r->content = NULL;
}

void out_truncate_head(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, out_trunc_t *r) {
    int64_t total_bytes = (int64_t)len, total_lines = (int64_t)out_count_nl(s, len) + 1;
    if (total_lines <= max_lines && total_bytes <= max_bytes) {
        no_trunc(s, len, total_lines, total_bytes, r);
        return;
    }
    trunc_init(r);
    r->total_lines = total_lines;
    r->total_bytes = total_bytes;
    r->truncated = true;
    int64_t included = 0, used = 0;
    size_t cut = 0, cursor = 0;
    int by = OUT_BY_LINES;
    while (included < max_lines) {
        const char *nl = memchr(s + cursor, '\n', len - cursor);
        size_t line_end = nl ? (size_t)(nl - s) : len;
        int64_t sep = included > 0 ? 1 : 0;
        int64_t remaining = max_bytes - used - sep;
        if (remaining < 0) {
            by = OUT_BY_BYTES;
            break;
        }
        int64_t lb = (int64_t)(line_end - cursor);
        if (lb > remaining) {
            by = OUT_BY_BYTES;
            if (included == 0) {
                r->content = xstrdup("");
                r->by = OUT_BY_BYTES;
                r->first_line_exceeds_limit = true;
                return;
            }
            break;
        }
        used += sep + lb;
        included++;
        cut = line_end;
        if (!nl) break;
        cursor = line_end + 1;
    }
    if (included >= max_lines && used <= max_bytes) by = OUT_BY_LINES;
    r->by = by;
    r->content = xstrndup(s, cut);
    r->content_len = cut;
    r->output_lines = included;
    r->output_bytes = used;
}

void out_truncate_tail(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, out_trunc_t *r) {
    int64_t total_bytes = (int64_t)len, total_lines = (int64_t)out_count_nl(s, len) + 1;
    if (total_lines <= max_lines && total_bytes <= max_bytes) {
        no_trunc(s, len, total_lines, total_bytes, r);
        return;
    }
    trunc_init(r);
    r->total_lines = total_lines;
    r->total_bytes = total_bytes;
    r->truncated = true;
    int64_t included = 0, used = 0;
    size_t start = len, end = len;
    int by = OUT_BY_LINES;
    while (included < max_lines) {
        size_t ls = end;
        while (ls > 0 && s[ls - 1] != '\n') ls--;
        bool has_nl = ls > 0; /* a newline precedes this line */
        int64_t sep = included > 0 ? 1 : 0;
        int64_t remaining = max_bytes - used - sep;
        if (remaining < 0) {
            by = OUT_BY_BYTES;
            break;
        }
        int64_t lb = (int64_t)(end - ls);
        bool partial = false;
        size_t pstart = 0, plen = 0;
        if (lb > remaining) {
            by = OUT_BY_BYTES;
            if (lb > max_bytes) {
                partial = true;
                pstart = ls + out_tail_start(s + ls, end - ls, (size_t)remaining);
                plen = end - pstart;
            }
        } else {
            used += sep + lb;
            included++;
            start = ls;
            if (!has_nl) break;
            end = ls - 1;
            continue;
        }
        if (partial && (plen > 0 || included == 0)) {
            bool complete = included > 0;
            bool sepnl = complete && plen > 0;
            sb_t b = SB_INIT;
            sb_appendn(&b, s + pstart, plen);
            if (sepnl) sb_putc(&b, '\n');
            if (complete) sb_appendn(&b, s + start, len - start);
            r->content_len = b.len;
            r->content = sb_detach(&b);
            r->by = OUT_BY_BYTES;
            r->output_lines = included + (plen > 0 ? 1 : 0);
            r->output_bytes = used + (sepnl ? 1 : 0) + (int64_t)plen;
            r->partial_byte_windows = plen > 0;
            r->last_line_partial = plen > 0 && !complete;
            return;
        }
        break;
    }
    if (included >= max_lines && used <= max_bytes) by = OUT_BY_LINES;
    r->by = by;
    r->content = xstrndup(s + start, len - start);
    r->content_len = len - start;
    r->output_lines = included;
    r->output_bytes = used;
}

void out_truncate_middle(const char *s, size_t len, int64_t max_bytes, int64_t max_lines, int64_t max_head_bytes,
                         int64_t max_head_lines, out_trunc_t *r) {
    int64_t head_b = max_head_bytes >= 0 ? max_head_bytes : max_bytes / 2;
    int64_t tail_b = MAX(0, max_bytes - head_b);
    int64_t head_l = max_head_lines >= 0 ? max_head_lines : MAX(1, max_lines / 2);
    int64_t tail_l = MAX(0, max_lines - head_l);
    int64_t total_bytes = (int64_t)len, total_lines = (int64_t)out_count_nl(s, len) + 1;
    if (total_bytes <= max_bytes && total_lines <= max_lines) {
        no_trunc(s, len, total_lines, total_bytes, r);
        return;
    }
    if (head_b <= 0 || head_l <= 0) {
        out_truncate_tail(s, len, tail_b ? tail_b : max_bytes, tail_l ? tail_l : max_lines, r);
        return;
    }
    if (tail_b <= 0 || tail_l <= 0) {
        out_truncate_head(s, len, head_b, head_l, r);
        return;
    }
    out_trunc_t head, tail;
    out_truncate_head(s, len, head_b, head_l, &head);
    out_truncate_tail(s, len, tail_b, tail_l, &tail);
    int64_t hk = head.output_lines, tk = tail.output_lines;
    int64_t hbk = head.output_bytes, tbk = tail.output_bytes;
    bool tail_partial = tail.partial_byte_windows || tail.last_line_partial;
    if (hk == 0 || head.first_line_exceeds_limit || tk == 0) {
        bool byte_head = hk == 0 || head.first_line_exceeds_limit;
        size_t bh_len = byte_head ? out_head_bytes(s, len, (size_t)MIN(head_b, total_bytes)) : head.content_len;
        const char *bh = byte_head ? s : head.content;
        int64_t actual_head_lines = byte_head ? 1 : hk;
        int64_t remaining = MAX(0, total_bytes - (int64_t)bh_len);
        bool byte_tail = tk == 0;
        char *bt;
        size_t bt_len;
        if (byte_tail) {
            size_t st = out_tail_start(s, len, (size_t)MIN(tail_b, remaining));
            bt = xstrndup(s + st, len - st);
            bt_len = len - st;
        } else {
            out_trunc_t lim;
            out_truncate_tail(s, len, MIN(tail_b, remaining), tail_l, &lim);
            bt = lim.content;
            bt_len = (size_t)lim.output_bytes;
            if (bt_len > lim.content_len) bt_len = lim.content_len;
            lim.content = NULL;
        }
        int64_t actual_tail_lines = byte_tail ? 1 : MIN(tk, tail_l);
        int64_t elided = MAX(0, total_bytes - (int64_t)bh_len - (int64_t)bt_len);
        if (elided == 0) {
            free(bt);
            out_trunc_free(&head);
            out_trunc_free(&tail);
            no_trunc(s, len, total_lines, total_bytes, r);
            return;
        }
        char *marker = out_middle_marker(0, elided);
        sb_t b = SB_INIT;
        sb_appendn(&b, bh, bh_len);
        sb_putc(&b, '\n');
        sb_append(&b, marker);
        sb_putc(&b, '\n');
        sb_appendn(&b, bt, bt_len);
        free(marker);
        free(bt);
        trunc_init(r);
        r->truncated = true;
        r->by = OUT_BY_MIDDLE;
        r->total_lines = total_lines;
        r->total_bytes = total_bytes;
        r->output_lines = actual_head_lines + actual_tail_lines + 1;
        r->output_bytes = (int64_t)b.len;
        r->elided_lines = MAX(0, total_lines - actual_head_lines - actual_tail_lines);
        r->head_lines = actual_head_lines;
        r->tail_lines = actual_tail_lines;
        r->partial_byte_windows = byte_head || byte_tail || tail_partial;
        r->elided_bytes = elided;
        r->last_line_partial = tail.last_line_partial;
        r->content_len = b.len;
        r->content = sb_detach(&b);
        out_trunc_free(&head);
        out_trunc_free(&tail);
        return;
    }
    if (hk + tk >= total_lines && !tail_partial) {
        out_trunc_free(&head);
        out_trunc_free(&tail);
        no_trunc(s, len, total_lines, total_bytes, r);
        return;
    }
    int64_t elided_lines = total_lines - hk - tk;
    int64_t elided_bytes = MAX(0, total_bytes - hbk - tbk);
    char *marker = out_middle_marker(elided_lines, elided_bytes);
    sb_t b = SB_INIT;
    sb_appendn(&b, head.content, head.content_len);
    sb_putc(&b, '\n');
    sb_append(&b, marker);
    sb_putc(&b, '\n');
    sb_appendn(&b, tail.content, tail.content_len);
    trunc_init(r);
    r->truncated = true;
    r->by = OUT_BY_MIDDLE;
    r->total_lines = total_lines;
    r->total_bytes = total_bytes;
    r->output_lines = hk + tk + 1;
    r->output_bytes = hbk + tbk + (int64_t)strlen(marker) + 2;
    r->elided_lines = elided_lines;
    r->elided_bytes = elided_bytes;
    r->head_lines = hk;
    r->tail_lines = tk;
    r->partial_byte_windows = tail_partial;
    r->last_line_partial = tail.last_line_partial;
    r->content_len = b.len;
    r->content = sb_detach(&b);
    free(marker);
    out_trunc_free(&head);
    out_trunc_free(&tail);
}

char *out_enforce_inline_cap(const char *text, size_t len, size_t max_bytes, const char *artifact_id, bool *capped) {
    if (capped) *capped = false;
    if (max_bytes == 0 || len <= max_bytes) return xstrndup(text, len);
    if (capped) *capped = true;
    size_t hl = out_head_bytes(text, len, (size_t)((double)max_bytes * 0.6));
    /* trimHeadToLineBoundary: drop the partial last line (keep it when there is no newline at all) */
    for (size_t i = hl; i > 0; i--)
        if (text[i - 1] == '\n') {
            if (i - 1 > 0) hl = i - 1;
            break;
        }
    size_t ts = out_tail_start(text, len, (size_t)((double)max_bytes * 0.25));
    /* trimTailToLineBoundary */
    const char *nl = memchr(text + ts, '\n', len - ts);
    if (nl && (size_t)(nl - text) != len - 1) ts = (size_t)(nl - text) + 1;
    int64_t elided = MAX(0, (int64_t)len - (int64_t)hl - (int64_t)(len - ts));
    sb_t b = SB_INIT;
    sb_appendn(&b, text, hl);
    sb_appendf(&b, "\n[" ELLIPSIS "%lldB elided" ELLIPSIS "]\n", (long long)elided);
    sb_appendn(&b, text + ts, len - ts);
    if (artifact_id) {
        if (!(b.len && b.s[b.len - 1] == '\n')) sb_putc(&b, '\n');
        sb_appendf(&b, "[raw output: artifact://%s]", artifact_id);
    }
    return sb_detach(&b);
}

/* =====================================================================================
 * OutputSink
 * ===================================================================================== */

#define ARTIFACT_TAIL_MAX_SLACK_BYTES (4 * 1024 * 1024)
#define ESC_HOLD_MAX 4096

struct out_sink {
    /* input decoding */
    u8dec_t dec;
    bool pending_cr;
    sb_t esc_hold; /* incomplete escape sequence carried to the next chunk */

    sb_t buffer; /* rolling tail */
    bool tail_untrimmed;
    sb_t head;
    size_t head_lines;
    int64_t total_lines, total_bytes;
    bool saw_data, truncated;

    size_t cur_line_bytes;
    bool col_ellipsis;
    int64_t col_dropped, col_lines, col_first, col_last;
    bool col_unmapped;

    char *artifact_path, *artifact_id;
    FILE *file;
    bool file_tried, finalized;
    const char *artifact_error;

    size_t spill, head_limit, max_columns;
    int64_t art_max, art_head_budget, art_tail_budget, art_tail_slack;
    int64_t art_head_written;
    bool art_head_closed;
    sb_t art_ring;
    int64_t art_tail_incoming, art_elided;
};

out_sink_t *out_sink_new(const out_sink_opts_t *o) {
    out_sink_t *s = xcalloc(1, sizeof *s);
    out_sink_opts_t d = {0};
    if (!o) {
        d.artifact_max_bytes = -1;
        d.artifact_head_bytes = -1;
        o = &d;
    }
    s->artifact_path = o->artifact_path ? xstrdup(o->artifact_path) : NULL;
    s->artifact_id = o->artifact_id ? xstrdup(o->artifact_id) : NULL;
    s->spill = o->spill_threshold ? o->spill_threshold : OUT_DEFAULT_MAX_BYTES;
    s->head_limit = MIN(o->head_bytes, s->spill / 2);
    s->max_columns = o->max_columns;
    s->art_max = o->artifact_max_bytes < 0 ? OUT_ARTIFACT_DEFAULT_MAX_BYTES : o->artifact_max_bytes;
    int64_t hreq = o->artifact_head_bytes >= 0 ? o->artifact_head_bytes : MIN(OUT_ARTIFACT_DEFAULT_HEAD_BYTES, s->art_max / 2);
    s->art_head_budget = MAX(0, MIN(hreq, s->art_max));
    s->art_tail_budget = MAX(0, s->art_max - s->art_head_budget);
    s->art_tail_slack = MIN(s->art_tail_budget, ARTIFACT_TAIL_MAX_SLACK_BYTES);
    return s;
}

void out_sink_free(out_sink_t *s) {
    if (!s) return;
    if (s->file) fclose(s->file);
    sb_free(&s->esc_hold);
    sb_free(&s->buffer);
    sb_free(&s->head);
    sb_free(&s->art_ring);
    free(s->artifact_path);
    free(s->artifact_id);
    free(s);
}

size_t out_sink_total_bytes(const out_sink_t *s) { return (size_t)s->total_bytes; }

/* ---------- artifact file ---------- */

static void art_error(out_sink_t *s, const char *op) {
    if (!s->artifact_error) s->artifact_error = op;
    sb_free(&s->art_ring);
}

static void art_write(out_sink_t *s, const char *p, size_t n, const char *op) {
    if (!s->file || s->artifact_error || !n) return;
    if (fwrite(p, 1, n, s->file) != n) art_error(s, op);
}

static void art_trim_tail(out_sink_t *s) {
    if ((int64_t)s->art_ring.len <= s->art_tail_budget) return;
    size_t st = out_tail_start(s->art_ring.s, s->art_ring.len, (size_t)s->art_tail_budget);
    memmove(s->art_ring.s, s->art_ring.s + st, s->art_ring.len - st);
    sb_truncate(&s->art_ring, s->art_ring.len - st);
}

static void art_push_tail(out_sink_t *s, const char *p, size_t n) {
    if (s->artifact_error) return;
    s->art_tail_incoming += (int64_t)n;
    if ((int64_t)n >= s->art_tail_budget) {
        size_t st = out_tail_start(p, n, (size_t)s->art_tail_budget);
        sb_clear(&s->art_ring);
        sb_appendn(&s->art_ring, p + st, n - st);
        return;
    }
    sb_appendn(&s->art_ring, p, n);
    if ((int64_t)s->art_ring.len > s->art_tail_budget + s->art_tail_slack) art_trim_tail(s);
}

static void art_emit(out_sink_t *s, const char *p, size_t n) {
    if (!s->file || s->artifact_error || !n) return;
    if (s->art_max == 0) {
        art_write(s, p, n, "write");
        return;
    }
    int64_t room = s->art_head_closed ? 0 : s->art_head_budget - s->art_head_written;
    if (room >= (int64_t)n) {
        art_write(s, p, n, "write");
        s->art_head_written += (int64_t)n;
        return;
    }
    const char *ov = p;
    size_t ovn = n;
    if (room > 0) {
        size_t hb = out_head_bytes(p, n, (size_t)room);
        if (hb > 0) {
            art_write(s, p, hb, "write");
            s->art_head_written += (int64_t)hb;
        }
        s->art_head_closed = true;
        ov = p + hb;
        ovn = n - hb;
    }
    if (ovn == 0 || s->art_tail_budget == 0) {
        if (ovn > 0) s->art_tail_incoming += (int64_t)ovn;
        return;
    }
    art_push_tail(s, ov, ovn);
}

static void art_write_chunk(out_sink_t *s, const char *p, size_t n) {
    if (s->artifact_error) return;
    if (!s->file) {
        if (s->file_tried) return;
        s->file_tried = true;
        s->file = fopen(s->artifact_path, "w");
        if (!s->file) {
            art_error(s, "open");
            return;
        }
        if (s->head.len) art_emit(s, s->head.s, s->head.len);
        if (s->buffer.len) art_emit(s, s->buffer.s, s->buffer.len);
    }
    art_emit(s, p, n);
}

static void art_flush_tail_if_capped(out_sink_t *s) {
    if (!s->file || s->artifact_error || s->art_max == 0) return;
    art_trim_tail(s);
    int64_t tb = (int64_t)s->art_ring.len;
    int64_t dropped = MAX(0, s->art_tail_incoming - tb);
    if (tb == 0 && dropped == 0) return;
    if (dropped > 0) {
        s->art_elided = dropped;
        int64_t hw = s->art_head_written, total = hw + s->art_tail_incoming;
        char *a = out_format_bytes(hw), *b = out_format_bytes(tb), *c = out_format_bytes(total), *d = out_format_bytes(dropped);
        char *notice = xasprintf("%s[ARTIFACT TRUNCATED: kept first %s + last %s of %s; %s elided from the middle]%s",
                                 hw > 0 ? "\n" : "", a, b, c, d, tb > 0 && s->art_ring.s[0] != '\n' ? "\n" : "");
        art_write(s, notice, strlen(notice), "flush");
        free(notice);
        free(a), free(b), free(c), free(d);
    }
    if (tb > 0) art_write(s, s->art_ring.s, s->art_ring.len, "flush");
}

static void art_close(out_sink_t *s) {
    if (s->finalized) return;
    s->finalized = true;
    if (!s->file) return;
    art_flush_tail_if_capped(s);
    if (!s->artifact_error && fflush(s->file) != 0) art_error(s, "flush");
    if (fclose(s->file) != 0 && !s->artifact_error) art_error(s, "end");
    s->file = NULL;
    s->file_tried = true;
}

/* ---------- column cap ---------- */

static bool fits_column_cap(out_sink_t *s, const char *c, size_t n) {
    size_t max = s->max_columns;
    int64_t room = (int64_t)max - (int64_t)s->cur_line_bytes;
    size_t cursor = 0;
    for (;;) {
        const char *nl = memchr(c + cursor, '\n', n - cursor);
        size_t seg_end = nl ? (size_t)(nl - c) : n;
        /* omp measures UTF-16 units x3; bytes are exact here */
        if ((int64_t)(seg_end - cursor) > room) return false;
        if (!nl) break;
        cursor = seg_end + 1;
        room = (int64_t)max;
    }
    if (cursor == 0) s->cur_line_bytes += n;
    else s->cur_line_bytes = n - cursor;
    return true;
}

static void apply_column_cap(out_sink_t *s, const char *c, size_t n, int64_t first_line, sb_t *o) {
    if (!s->col_ellipsis && fits_column_cap(s, c, n)) {
        sb_appendn(o, c, n);
        return;
    }
    size_t max = s->max_columns, cursor = 0;
    int64_t line = first_line;
    while (cursor < n) {
        const char *nl = memchr(c + cursor, '\n', n - cursor);
        size_t seg_end = nl ? (size_t)(nl - c) : n;
        if (seg_end > cursor) {
            size_t seg = seg_end - cursor;
            if (s->col_ellipsis) {
                s->col_dropped += (int64_t)seg;
            } else {
                int64_t remaining = (int64_t)max - (int64_t)s->cur_line_bytes;
                if ((int64_t)seg <= remaining) {
                    sb_appendn(o, c + cursor, seg);
                    s->cur_line_bytes += seg;
                } else {
                    int64_t head_room = MAX(0, remaining - ELLIPSIS_LEN);
                    size_t kept = 0;
                    if (head_room > 0) {
                        kept = out_head_bytes(c + cursor, seg, (size_t)head_room);
                        sb_appendn(o, c + cursor, kept);
                    }
                    sb_appendn(o, ELLIPSIS, ELLIPSIS_LEN);
                    s->col_dropped += (int64_t)(seg - kept);
                    s->col_lines++;
                    if (line > 0) {
                        if (s->col_first == 0) s->col_first = line;
                        s->col_last = line;
                    } else {
                        s->col_unmapped = true;
                    }
                    s->cur_line_bytes += kept + ELLIPSIS_LEN;
                    s->col_ellipsis = true;
                }
            }
        }
        if (!nl) break;
        sb_putc(o, '\n');
        s->cur_line_bytes = 0;
        s->col_ellipsis = false;
        cursor = seg_end + 1;
        if (line > 0) line++;
    }
}

/* ---------- head / tail ---------- */

static void trim_tail(out_sink_t *s) {
    if (!s->tail_untrimmed) return;
    s->tail_untrimmed = false;
    size_t thr = s->spill > s->head.len ? s->spill - s->head.len : 0;
    if (s->buffer.len <= thr) return;
    size_t st = out_tail_start(s->buffer.s, s->buffer.len, thr);
    memmove(s->buffer.s, s->buffer.s + st, s->buffer.len - st);
    sb_truncate(&s->buffer, s->buffer.len - st);
}

static void push_tail(out_sink_t *s, const char *c, size_t n) {
    if (!n) return;
    size_t thr = s->spill > s->head.len ? s->spill - s->head.len : 0;
    if (s->buffer.len + n <= thr) {
        sb_appendn(&s->buffer, c, n);
        return;
    }
    s->truncated = true;
    if (n >= thr) {
        size_t st = out_tail_start(c, n, thr);
        sb_clear(&s->buffer);
        sb_appendn(&s->buffer, c + st, n - st);
        s->tail_untrimmed = false;
        return;
    }
    sb_appendn(&s->buffer, c, n);
    s->tail_untrimmed = true;
    if (s->buffer.len > thr * 2) trim_tail(s);
}

static void store(out_sink_t *s, const char *c, size_t n) {
    s->total_bytes += (int64_t)n;
    int64_t first_line = s->total_lines + 1;
    if (n > 0) {
        s->saw_data = true;
        s->total_lines += (int64_t)out_count_nl(c, n);
    }
    sb_t capbuf = SB_INIT;
    const char *cp = c;
    size_t cn = n;
    if (s->max_columns > 0 && n > 0) {
        apply_column_cap(s, c, n, first_line, &capbuf);
        cp = capbuf.s ? capbuf.s : "";
        cn = capbuf.len;
    }
    bool capped_this = cn < n;
    size_t thr = s->spill > s->head.len ? s->spill - s->head.len : 0;
    bool will_overflow = s->buffer.len + cn > thr;
    if (s->artifact_path && (s->file || capped_this || will_overflow)) art_write_chunk(s, c, n);
    if (cn == 0) {
        sb_free(&capbuf);
        return;
    }
    const char *tc = cp;
    size_t tn = cn;
    if (s->head_limit > 0 && s->head.len < s->head_limit) {
        size_t room = s->head_limit - s->head.len;
        if (cn <= room) {
            sb_appendn(&s->head, cp, cn);
            s->head_lines += out_count_nl(cp, cn);
            sb_free(&capbuf);
            return;
        }
        size_t hb = out_head_bytes(cp, cn, room);
        if (hb > 0) {
            sb_appendn(&s->head, cp, hb);
            s->head_lines += out_count_nl(cp, hb);
            tc = cp + hb;
            tn = cn - hb;
        }
    }
    push_tail(s, tc, tn);
    sb_free(&capbuf);
}

/* decode + CR-normalize + sanitize one raw chunk; escape sequences split across chunks are held back */
static void sink_ingest(out_sink_t *s, const char *data, size_t len, bool final) {
    if (s->finalized) return;
    sb_t u = SB_INIT;
    if (s->esc_hold.len) {
        sb_appendn(&u, s->esc_hold.s, s->esc_hold.len);
        sb_clear(&s->esc_hold);
    }
    u8dec_feed(&s->dec, (const unsigned char *)data, len, &u);
    if (final) u8dec_flush(&s->dec, &u);
    /* hold back an unterminated escape sequence at the end (bounded) */
    if (!final && u.len) {
        const unsigned char *b = (const unsigned char *)u.s;
        size_t from = u.len > ESC_HOLD_MAX ? u.len - ESC_HOLD_MAX : 0;
        for (size_t i = from; i < u.len; i++) {
            if (!is_ansi_intro(b, u.len, i)) continue;
            bool inc;
            size_t l = ansi_seq_len(b + i, u.len - i, true, &inc);
            if (inc) {
                sb_appendn(&s->esc_hold, u.s + i, u.len - i);
                sb_truncate(&u, i);
                break;
            }
            i += l - 1;
        }
    }
    sb_t c = SB_INIT, o = SB_INIT;
    normalize_cr(u.s ? u.s : "", u.len, &s->pending_cr, &c);
    sanitize_valid((const unsigned char *)(c.s ? c.s : ""), c.len, &o);
    store(s, o.s ? o.s : "", o.len);
    sb_free(&u);
    sb_free(&c);
    sb_free(&o);
}

void out_sink_push(out_sink_t *s, const char *data, size_t len) {
    if (!len) return;
    sink_ingest(s, data, len, false);
}

void out_sink_push_str(out_sink_t *s, const char *text) { out_sink_push(s, text, strlen(text)); }

typedef struct {
    char *body;
    size_t body_len;
    int64_t output_bytes, output_lines, elided_bytes, elided_lines;
} composed_t;

static void compose_body(out_sink_t *s, composed_t *r) {
    trim_tail(s);
    int64_t total_lines = s->saw_data ? s->total_lines + 1 : 0;
    size_t hb = s->head.len, tb = s->buffer.len;
    const char *head = s->head.s ? s->head.s : "", *tail = s->buffer.s ? s->buffer.s : "";
    bool head_nl = hb && head[hb - 1] == '\n';
    int64_t head_lines = (int64_t)s->head_lines + (hb > 0 && !head_nl ? 1 : 0);
    int64_t tail_lines = tb > 0 ? (int64_t)out_count_nl(tail, tb) + 1 : 0;
    int64_t eff_total = MAX(0, s->total_bytes - s->col_dropped);
    r->elided_bytes = r->elided_lines = -1;
    sb_t b = SB_INIT;
    if (hb > 0 && eff_total > (int64_t)(hb + tb)) {
        int64_t eb = MAX(0, eff_total - (int64_t)hb - (int64_t)tb);
        int64_t el = MAX(0, total_lines - head_lines - tail_lines);
        char *marker = out_middle_marker(el, eb);
        bool hsep = !head_nl, tsep = !(tb && tail[0] == '\n');
        sb_appendn(&b, head, hb);
        if (hsep) sb_putc(&b, '\n');
        sb_append(&b, marker);
        if (tsep) sb_putc(&b, '\n');
        sb_appendn(&b, tail, tb);
        r->output_bytes = (int64_t)(hb + strlen(marker) + tb) + hsep + tsep;
        r->output_lines = head_lines + 1 + tail_lines;
        r->elided_bytes = eb;
        r->elided_lines = el;
        free(marker);
    } else if (hb > 0) {
        sb_appendn(&b, head, hb);
        sb_appendn(&b, tail, tb);
        r->output_bytes = (int64_t)(hb + tb);
        r->output_lines = b.len ? (int64_t)out_count_nl(b.s, b.len) + 1 : 0;
    } else {
        sb_appendn(&b, tail, tb);
        r->output_bytes = (int64_t)tb;
        r->output_lines = tail_lines;
    }
    r->body_len = b.len;
    r->body = sb_detach(&b);
}

char *out_sink_preview(out_sink_t *s) {
    composed_t c;
    compose_body(s, &c);
    return c.body;
}

void out_sink_dump(out_sink_t *s, const char *notice, out_summary_t *sum) {
    memset(sum, 0, sizeof *sum);
    if (!s->finalized) {
        sink_ingest(s, "", 0, true); /* flush the decoder and any held escape */
        if (s->pending_cr) {
            s->pending_cr = false;
            store(s, "\n", 1);
        }
    }
    int64_t total_lines = s->saw_data ? s->total_lines + 1 : 0;
    art_close(s);
    composed_t c;
    compose_body(s, &c);
    if (c.elided_bytes >= 0) s->truncated = true;
    sb_t o = SB_INIT;
    if (notice && *notice) sb_appendf(&o, "[%s]\n", notice);
    sb_appendn(&o, c.body, c.body_len);
    free(c.body);
    sum->output_len = o.len;
    sum->output = sb_detach(&o);
    sum->truncated = s->truncated;
    sum->total_lines = total_lines;
    sum->total_bytes = s->total_bytes;
    sum->output_lines = c.output_lines;
    sum->output_bytes = c.output_bytes;
    sum->elided_bytes = c.elided_bytes;
    sum->elided_lines = c.elided_lines;
    if (s->col_lines > 0) {
        sum->column_dropped_bytes = s->col_dropped;
        sum->column_truncated_lines = s->col_lines;
        sum->column_max = (int64_t)s->max_columns;
    } else if (s->col_dropped > 0) {
        sum->column_dropped_bytes = s->col_dropped;
    }
    if (s->col_first > 0 && !s->col_unmapped) {
        sum->column_first = s->col_first;
        sum->column_last = s->col_last;
    }
    bool have_file = s->file_tried && !s->artifact_error && s->artifact_path;
    sum->artifact_error = s->artifact_error;
    if (have_file && s->artifact_id) {
        sum->artifact_id = xstrdup(s->artifact_id);
        sum->artifact_elided_bytes = s->art_elided;
    }
}

void out_summary_free(out_summary_t *sum) {
    free(sum->output);
    free(sum->artifact_id);
    sum->output = sum->artifact_id = NULL;
}

/* =====================================================================================
 * output meta
 * ===================================================================================== */

static json_t *meta_obj(json_t **meta) {
    if (!*meta) *meta = json_obj();
    return *meta;
}

static json_t *range_obj(int64_t a, int64_t b) {
    json_t *r = json_obj();
    json_set_int(r, "start", a);
    json_set_int(r, "end", b);
    return r;
}

void out_meta_column_truncated(json_t **meta, int64_t max_column, const char *unit, const char *artifact_id,
                               int64_t artifact_elided_bytes, int64_t first, int64_t last, const char *selector_base) {
    if (max_column <= 0) return;
    json_t *m = meta_obj(meta);
    json_t *limits = json_get_obj(m, "limits");
    if (!limits) {
        limits = json_obj();
        json_set(m, "limits", limits);
    }
    json_t *c = json_obj();
    json_set_int(c, "maxColumn", max_column);
    json_set_str(c, "unit", unit ? unit : "chars");
    if (artifact_id) json_set_str(c, "artifactId", artifact_id);
    if (artifact_id && artifact_elided_bytes > 0) json_set_int(c, "artifactElidedBytes", artifact_elided_bytes);
    bool lines = (artifact_id || selector_base) && first > 0;
    if (lines) {
        json_t *l = json_obj();
        json_set_int(l, "first", first);
        json_set_int(l, "last", last);
        json_set(c, "lines", l);
        if (selector_base) json_set_str(c, "selectorBase", selector_base);
    }
    json_set(limits, "columnTruncated", c);
}

static void set_limit(json_t **meta, const char *key, int64_t reached, int64_t suggestion) {
    if (reached <= 0) return;
    json_t *m = meta_obj(meta);
    json_t *limits = json_get_obj(m, "limits");
    if (!limits) {
        limits = json_obj();
        json_set(m, "limits", limits);
    }
    json_t *l = json_obj();
    json_set_int(l, "reached", reached);
    if (suggestion >= 0) json_set_int(l, "suggestion", suggestion);
    json_set(limits, key, l);
}

void out_meta_match_limit(json_t **meta, int64_t reached, int64_t suggestion) {
    set_limit(meta, "matchLimit", reached, suggestion < 0 ? reached * 2 : suggestion);
}
void out_meta_result_limit(json_t **meta, int64_t reached, int64_t suggestion) { set_limit(meta, "resultLimit", reached, suggestion); }
void out_meta_head_limit(json_t **meta, int64_t reached, int64_t suggestion) {
    set_limit(meta, "headLimit", reached, suggestion < 0 ? reached * 2 : suggestion);
}

void out_meta_from_summary(json_t **meta, const out_summary_t *sum, const char *direction, int64_t start_line,
                           int64_t total_file_lines) {
    if (sum->artifact_error) json_set_str(meta_obj(meta), "artifactError", sum->artifact_error);
    if (sum->column_max > 0 && sum->column_truncated_lines > 0)
        out_meta_column_truncated(meta, sum->column_max, "bytes", sum->artifact_id, sum->artifact_elided_bytes,
                                  sum->column_first, sum->column_last, NULL);
    if (!sum->truncated) return;
    if (start_line <= 0) start_line = 1;
    int64_t total_lines = total_file_lines > 0 ? total_file_lines : sum->total_lines;
    const char *aid = sum->artifact_error ? NULL : sum->artifact_id;
    int64_t aeb = aid ? sum->artifact_elided_bytes : 0;
    json_t *t = json_obj();
    if (sum->elided_bytes > 0) {
        int64_t el = sum->elided_lines >= 0 ? sum->elided_lines : MAX(0, total_lines - sum->output_lines);
        int64_t kept = MAX(0, sum->output_lines - 1);
        int64_t hl = (kept + 1) / 2, tl = kept - hl;
        json_set_str(t, "direction", "middle");
        json_set_str(t, "truncatedBy", "middle");
        json_set_int(t, "totalLines", total_lines);
        json_set_int(t, "totalBytes", sum->total_bytes);
        json_set_int(t, "outputLines", sum->output_lines);
        json_set_int(t, "outputBytes", sum->output_bytes);
        if (hl > 0) json_set(t, "headRange", range_obj(1, hl));
        if (tl > 0) json_set(t, "tailRange", range_obj(total_lines - tl + 1, total_lines));
        json_set_int(t, "elidedBytes", sum->elided_bytes);
        json_set_int(t, "elidedLines", el);
        if (aid) json_set_str(t, "artifactId", aid);
        if (aeb > 0) json_set_int(t, "artifactElidedBytes", aeb);
        json_set(meta_obj(meta), "truncation", t);
        return;
    }
    const char *by = sum->output_bytes < sum->total_bytes ? "bytes" : sum->output_lines < sum->total_lines ? "lines" : "bytes";
    int64_t a, b;
    bool tail = str_eq(direction, "tail");
    if (tail) {
        a = total_lines - sum->output_lines + 1;
        b = total_lines;
    } else {
        a = start_line;
        b = start_line + sum->output_lines - 1;
    }
    json_set_str(t, "direction", direction);
    json_set_str(t, "truncatedBy", by);
    json_set_int(t, "totalLines", total_lines);
    json_set_int(t, "totalBytes", sum->total_bytes);
    json_set_int(t, "outputLines", sum->output_lines);
    json_set_int(t, "outputBytes", sum->output_bytes);
    json_set(t, "shownRange", range_obj(a, b));
    if (aid) json_set_str(t, "artifactId", aid);
    if (aeb > 0) json_set_int(t, "artifactElidedBytes", aeb);
    if (str_eq(direction, "head")) json_set_int(t, "nextOffset", b + 1);
    json_set(meta_obj(meta), "truncation", t);
}

void out_meta_from_trunc(json_t **meta, const out_trunc_t *r, const char *direction, int64_t start_line,
                         int64_t total_file_lines, const char *artifact_id, int64_t max_bytes, int64_t next_offset) {
    if (!r->truncated) return;
    if (start_line <= 0) start_line = 1;
    int64_t ol = r->output_lines, ob = r->output_bytes;
    bool middle = str_eq(direction, "middle") || r->by == OUT_BY_MIDDLE;
    const char *by = middle ? "middle" : r->by == OUT_BY_LINES ? "lines" : "bytes";
    int64_t etl = total_file_lines > 0 ? total_file_lines : r->total_lines;
    json_t *t = json_obj();
    json_set_str(t, "direction", direction);
    if (r->first_line_exceeds_limit) {
        json_set_str(t, "truncatedBy", "bytes");
        json_set_int(t, "totalLines", etl);
        json_set_int(t, "totalBytes", r->total_bytes);
        json_set_int(t, "outputLines", ol);
        json_set_int(t, "outputBytes", ob);
        json_set(t, "shownRange", range_obj(start_line, start_line));
        json_set_bool(t, "partialLine", true);
        if (artifact_id) json_set_str(t, "artifactId", artifact_id);
        json_set(meta_obj(meta), "truncation", t);
        return;
    }
    if (middle) {
        int64_t el = r->elided_lines >= 0 ? r->elided_lines : MAX(0, etl - ol);
        int64_t eb = r->elided_bytes >= 0 ? r->elided_bytes : MAX(0, r->total_bytes - ob);
        int64_t kept = MAX(0, ol - 1);
        int64_t hl = r->head_lines >= 0 ? r->head_lines : (kept + 1) / 2;
        int64_t tl = r->tail_lines >= 0 ? r->tail_lines : kept - hl;
        json_set_str(t, "direction", "middle");
        json_set_str(t, "truncatedBy", "middle");
        json_set_int(t, "totalLines", etl);
        json_set_int(t, "totalBytes", r->total_bytes);
        json_set_int(t, "outputLines", ol);
        json_set_int(t, "outputBytes", ob);
        if (etl > 1 && !r->partial_byte_windows) {
            if (hl > 0) json_set(t, "headRange", range_obj(1, hl));
            if (tl > 0) json_set(t, "tailRange", range_obj(etl - tl + 1, etl));
        }
        json_set_int(t, "elidedLines", el);
        json_set_int(t, "elidedBytes", eb);
        if (artifact_id) json_set_str(t, "artifactId", artifact_id);
        json_set(meta_obj(meta), "truncation", t);
        return;
    }
    int64_t a, b;
    if (str_eq(direction, "tail")) {
        a = r->total_lines - ol + 1;
        b = r->total_lines;
    } else {
        a = start_line;
        b = start_line + ol - 1;
    }
    json_set_str(t, "truncatedBy", by);
    json_set_int(t, "totalLines", etl);
    json_set_int(t, "totalBytes", r->total_bytes);
    json_set_int(t, "outputLines", ol);
    json_set_int(t, "outputBytes", ob);
    if (max_bytes > 0) json_set_int(t, "maxBytes", max_bytes);
    json_set(t, "shownRange", range_obj(a, b));
    if (artifact_id) json_set_str(t, "artifactId", artifact_id);
    if (str_eq(direction, "head") && next_offset >= 0) json_set_int(t, "nextOffset", next_offset > 0 ? next_offset : b + 1);
    json_set(meta_obj(meta), "truncation", t);
}

/* formatFullOutputReference */
static void full_output_ref(sb_t *b, const char *id, int64_t aeb) {
    if (aeb > 0) {
        char *f = out_format_bytes(aeb);
        sb_appendf(b, "Read artifact://%s for a head/tail sample of the output; %s from its middle was not saved", id, f);
        free(f);
    } else {
        sb_appendf(b, "Read artifact://%s for full output", id);
    }
}

static bool has_key(const json_t *o, const char *k) { return json_get(o, k) && !json_is_null(json_get(o, k)); }

/* formatTruncationMetaNotice */
static void truncation_notice(sb_t *b, const json_t *t, const json_t *source) {
    const char *aid = json_get_str(t, "artifactId");
    sb_t ref = SB_INIT;
    if (aid) {
        if (str_eq(json_get_str(source, "type"), "report"))
            sb_appendf(&ref, "Read artifact://%s for full report (%s)", aid, json_get_str(source, "value"));
        else full_output_ref(&ref, aid, json_get_int(t, "artifactElidedBytes", 0));
    }
    int64_t total_lines = json_get_int(t, "totalLines", 0), total_bytes = json_get_int(t, "totalBytes", 0);
    int64_t ol = json_get_int(t, "outputLines", 0), ob = json_get_int(t, "outputBytes", 0);
    if (str_eq(json_get_str(t, "direction"), "middle")) {
        const json_t *h = json_get_obj(t, "headRange"), *tr = json_get_obj(t, "tailRange");
        int64_t eb = has_key(t, "elidedBytes") ? json_get_int(t, "elidedBytes", 0) : MAX(0, total_bytes - ob);
        int64_t el = has_key(t, "elidedLines") ? json_get_int(t, "elidedLines", 0) : MAX(0, total_lines - ol);
        char *fb = out_format_bytes(eb);
        if (h && tr) {
            char *g = out_format_grouped(el);
            sb_appendf(b, "Showing lines %lld-%lld and %lld-%lld of %lld; %s middle line%s (%s) elided",
                       (long long)json_get_int(h, "start", 0), (long long)json_get_int(h, "end", 0),
                       (long long)json_get_int(tr, "start", 0), (long long)json_get_int(tr, "end", 0), (long long)total_lines, g,
                       el == 1 ? "" : "s", fb);
            free(g);
        } else if (eb > 0) {
            char *g = out_format_grouped(total_lines);
            sb_appendf(b, "Showing head and tail bytes of %s line%s; %s elided", g, total_lines == 1 ? "" : "s", fb);
            free(g);
        } else {
            sb_appendf(b, "Showing %lld of %lld lines; middle elided", (long long)MIN(ol, total_lines), (long long)total_lines);
        }
        free(fb);
        if (has_key(t, "nextOffset")) sb_appendf(b, ". Use :%lld to continue", (long long)json_get_int(t, "nextOffset", 0));
        if (ref.len) sb_appendf(b, ". %s", ref.s);
        sb_free(&ref);
        return;
    }
    if (json_get_bool(t, "partialLine", false)) {
        const json_t *r = json_get_obj(t, "shownRange");
        char *a = out_format_bytes(ob), *c = out_format_bytes(total_bytes);
        sb_appendf(b, "Showing line %lld (partial, %s of %s) of %lld", (long long)(r ? json_get_int(r, "start", 1) : 1), a, c,
                   (long long)total_lines);
        free(a);
        free(c);
        if (ref.len) sb_appendf(b, ". %s", ref.s);
        sb_free(&ref);
        return;
    }
    const json_t *r = json_get_obj(t, "shownRange");
    if (r && json_get_int(r, "end", 0) >= json_get_int(r, "start", 0))
        sb_appendf(b, "Showing lines %lld-%lld of %lld", (long long)json_get_int(r, "start", 0), (long long)json_get_int(r, "end", 0),
                   (long long)total_lines);
    else sb_appendf(b, "Showing %lld of %lld lines", (long long)ol, (long long)total_lines);
    if (str_eq(json_get_str(t, "truncatedBy"), "bytes")) {
        char *f = out_format_bytes(has_key(t, "maxBytes") ? json_get_int(t, "maxBytes", 0) : ob);
        sb_appendf(b, " (%s limit)", f);
        free(f);
    }
    if (has_key(t, "nextOffset")) sb_appendf(b, ". Use :%lld to continue", (long long)json_get_int(t, "nextOffset", 0));
    if (ref.len) sb_appendf(b, ". %s", ref.s);
    sb_free(&ref);
}

static void artifact_error_notice(sb_t *b, const char *err) {
    sb_appendf(b, "Full output was not saved completely (artifact %s failed)", err);
}

char *out_format_notice(const json_t *meta) {
    if (!json_is_obj(meta)) return xstrdup("");
    strvec_t parts = {0};
    const json_t *t = json_get_obj(meta, "truncation");
    if (t) {
        sb_t b = SB_INIT;
        truncation_notice(&b, t, json_get_obj(meta, "source"));
        strvec_push_own(&parts, sb_detach(&b));
    }
    if (json_get_str(meta, "artifactError")) {
        sb_t b = SB_INIT;
        artifact_error_notice(&b, json_get_str(meta, "artifactError"));
        strvec_push_own(&parts, sb_detach(&b));
    }
    const json_t *lim = json_get_obj(meta, "limits"), *l;
    if ((l = json_get_obj(lim, "matchLimit")))
        strvec_push_own(&parts, xasprintf("%lld matches limit reached. Use limit=%lld for more", (long long)json_get_int(l, "reached", 0),
                                          (long long)json_get_int(l, "suggestion", 0)));
    if ((l = json_get_obj(lim, "resultLimit"))) {
        if (has_key(l, "suggestion"))
            strvec_push_own(&parts, xasprintf("%lld results limit reached. Use limit=%lld for more", (long long)json_get_int(l, "reached", 0),
                                              (long long)json_get_int(l, "suggestion", 0)));
        else strvec_push_own(&parts, xasprintf("%lld results limit reached", (long long)json_get_int(l, "reached", 0)));
    }
    if ((l = json_get_obj(lim, "headLimit")))
        strvec_push_own(&parts, xasprintf("%lld results limit reached. Use limit=%lld for more", (long long)json_get_int(l, "reached", 0),
                                          (long long)json_get_int(l, "suggestion", 0)));
    if ((l = json_get_obj(lim, "columnTruncated"))) {
        sb_t b = SB_INIT;
        const char *unit = json_get_str(l, "unit");
        sb_appendf(&b, "Some lines truncated to %lld %s", (long long)json_get_int(l, "maxColumn", 0), unit ? unit : "chars");
        const char *aid = json_get_str(l, "artifactId");
        int64_t aeb = json_get_int(l, "artifactElidedBytes", 0);
        char *base = json_get_str(l, "selectorBase") ? xstrdup(json_get_str(l, "selectorBase"))
                     : (aid && aeb <= 0)              ? xasprintf("artifact://%s", aid)
                                                      : NULL;
        const json_t *ln = json_get_obj(l, "lines");
        if (ln && base) {
            int64_t first = json_get_int(ln, "first", 0), last = json_get_int(ln, "last", 0);
            sb_appendf(&b, ". Use %s:raw:%lld-%lld to read line %lld whole", base, (long long)first, (long long)first, (long long)first);
            if (last > first) sb_appendf(&b, "; cut lines run to line %lld, each read the same way", (long long)last);
        } else if (aid) {
            sb_append(&b, ". ");
            full_output_ref(&b, aid, aeb);
        }
        free(base);
        strvec_push_own(&parts, sb_detach(&b));
    }
    sb_t out = SB_INIT;
    if (parts.n) {
        char *j = strvec_join(&parts, ". ");
        sb_appendf(&out, "\n\n[%s]", j);
        free(j);
    }
    strvec_free(&parts);
    return sb_detach(&out);
}

char *out_format_warning(const json_t *meta) {
    if (!json_is_obj(meta)) return NULL;
    const json_t *t = json_get_obj(meta, "truncation");
    const char *err = json_get_str(meta, "artifactError");
    if (!t && !err) return NULL;
    sb_t b = SB_INIT;
    if (t) truncation_notice(&b, t, json_get_obj(meta, "source"));
    if (err) {
        if (t) sb_append(&b, ". ");
        artifact_error_notice(&b, err);
    }
    return sb_detach(&b);
}

static size_t trim_end_len(const char *s, size_t n) {
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return n;
}

char *out_strip_notice(const char *text, const json_t *meta) {
    char *notice = out_format_notice(meta);
    if (!*notice) {
        free(notice);
        return xstrdup(text);
    }
    size_t tl = trim_end_len(text, strlen(text)), nl = trim_end_len(notice, strlen(notice));
    char *r;
    if (tl >= nl && memcmp(text + tl - nl, notice, nl) == 0) {
        /* stripTrailingNotice(trimmedText, trimmedNotice, false): last occurrence == the suffix */
        r = xstrndup(text, tl - nl);
    } else {
        r = xstrdup(text);
    }
    free(notice);
    return r;
}

void out_append_notice(json_t *content, const json_t *meta) {
    char *notice = out_format_notice(meta);
    if (!*notice) {
        free(notice);
        return;
    }
    for (size_t i = json_len(content); i-- > 0;) {
        json_t *blk = json_at(content, i);
        if (str_eq(json_get_str(blk, "type"), "text")) {
            const char *t = json_get_str(blk, "text");
            char *n = xasprintf("%s%s", t ? t : "", notice);
            json_set(blk, "text", json_str_own(n));
            free(notice);
            return;
        }
    }
    char *tr = str_trim(notice);
    json_t *blk = json_obj();
    json_set_str(blk, "type", "text");
    json_set_str(blk, "text", tr);
    json_push(content, blk);
    free(tr);
    free(notice);
}

char *out_bash_execution_text(const json_t *msg) {
    sb_t b = SB_INIT;
    const char *cmd = json_get_str(msg, "command");
    sb_appendf(&b, "Ran `%s`\n", cmd ? cmd : "");
    const char *out = json_get_str(msg, "output");
    if (out && *out) sb_appendf(&b, "```\n%s\n```", out);
    else sb_append(&b, "(no output)");
    const json_t *ec = json_get(msg, "exitCode");
    if (json_get_bool(msg, "cancelled", false)) sb_append(&b, "\n\n(command cancelled)");
    else if (json_is_num(ec) && json_get_int(msg, "exitCode", 0) != 0)
        sb_appendf(&b, "\n\nCommand exited with code %lld", (long long)json_get_int(msg, "exitCode", 0));
    char *n = out_format_notice(json_get_obj(msg, "meta"));
    sb_append(&b, n);
    free(n);
    return sb_detach(&b);
}

char *out_python_execution_text(const json_t *msg) {
    sb_t b = SB_INIT;
    const char *code = json_get_str(msg, "code");
    sb_appendf(&b, "Ran Python:\n```python\n%s\n```\n", code ? code : "");
    const char *out = json_get_str(msg, "output");
    if (out && *out) sb_appendf(&b, "Output:\n```\n%s\n```", out);
    else sb_append(&b, "(no output)");
    const json_t *ec = json_get(msg, "exitCode");
    if (json_get_bool(msg, "cancelled", false)) sb_append(&b, "\n\n(execution cancelled)");
    else if (json_is_num(ec) && json_get_int(msg, "exitCode", 0) != 0)
        sb_appendf(&b, "\n\nExecution failed with code %lld", (long long)json_get_int(msg, "exitCode", 0));
    char *n = out_format_notice(json_get_obj(msg, "meta"));
    sb_append(&b, n);
    free(n);
    return sb_detach(&b);
}
