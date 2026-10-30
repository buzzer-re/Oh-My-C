/* ansi.c - ANSI-aware width, truncation, slicing and wrapping. */
#include "tui/ansi.h"

size_t tui_esc_len(const char *s, size_t n) {
    if (n < 2) return 0;
    unsigned char c = (unsigned char)s[1];
    if (c == '[') {
        size_t i = 2;
        while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)) i++;
        return i < n ? i + 1 : 0;
    }
    if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') {
        for (size_t i = 2; i < n; i++) {
            if (s[i] == '\a') return i + 1;
            if (s[i] == '\x1b') {
                if (i + 1 >= n) return 0;
                if (s[i + 1] == '\\') return i + 2;
            }
        }
        return 0;
    }
    if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%' || c == ' ') return n >= 3 ? 3 : 0;
    return 2;
}

int tui_cp_width(uint32_t cp) {
    if (cp == '\t') return TUI_TAB_WIDTH;
    return wcwidth_cp(cp);
}

int tui_visible_width_n(const char *s, size_t n) {
    int w = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {
            size_t l = tui_esc_len(s + i, n - i);
            if (!l) break;
            i += l;
            continue;
        }
        if (c < 0x80) {
            if (c >= 0x20 && c < 0x7f) w++;
            else if (c == '\t') w += TUI_TAB_WIDTH;
            i++;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        w += tui_cp_width(cp);
        i += (size_t)k;
    }
    return w;
}

int tui_visible_width(const char *s) { return s ? tui_visible_width_n(s, strlen(s)) : 0; }

/* ---------- SGR state ---------- */

void tui_sgr_reset(tui_sgr_t *st) {
    char link[sizeof st->link];
    memcpy(link, st->link, sizeof link);
    memset(st, 0, sizeof *st);
    memcpy(st->link, link, sizeof link);
}

static void set_color(char *dst, size_t cap, const char *p, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = 0;
}

static void sgr_params(tui_sgr_t *st, const char *p, size_t n) {
    if (n == 0) {
        tui_sgr_reset(st);
        return;
    }
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && p[j] != ';') j++;
        /* token p[i..j) may have colon sub-params */
        int code = 0;
        size_t k = i;
        bool has_digits = false;
        while (k < j && p[k] >= '0' && p[k] <= '9') {
            code = code * 10 + (p[k] - '0');
            if (code > 100000) code = 100000;
            k++;
            has_digits = true;
        }
        bool colon = k < j && p[k] == ':';
        if (!has_digits && j == i) code = 0;
        if (code == 38 || code == 48 || code == 58) {
            char *dst = code == 38 ? st->fg : code == 48 ? st->bg : st->ulc;
            size_t cap = sizeof st->fg;
            if (colon) {
                set_color(dst, cap, p + i, j - i);
            } else {
                /* consume following ;5;n or ;2;r;g;b */
                size_t e = j;
                int mode = -1;
                if (e < n) {
                    size_t a = e + 1, b = a;
                    while (b < n && p[b] != ';') b++;
                    mode = atoi(p + a);
                    int extra = mode == 5 ? 1 : mode == 2 ? 3 : 0;
                    e = b;
                    for (int x = 0; x < extra && e < n; x++) {
                        size_t c2 = e + 1;
                        while (c2 < n && p[c2] != ';') c2++;
                        e = c2;
                    }
                }
                set_color(dst, cap, p + i, e - i);
                j = e;
            }
        } else if (code == 0) {
            tui_sgr_reset(st);
        } else if (code == 1) st->bold = true;
        else if (code == 2) st->dim = true;
        else if (code == 3) st->italic = true;
        else if (code == 4) st->underline = !(colon && k + 1 < j && p[k + 1] == '0');
        else if (code == 5 || code == 6) st->blink = true;
        else if (code == 7) st->inverse = true;
        else if (code == 8) st->hidden = true;
        else if (code == 9) st->strike = true;
        else if (code == 21) st->underline = true;
        else if (code == 22) st->bold = st->dim = false;
        else if (code == 23) st->italic = false;
        else if (code == 24) st->underline = false;
        else if (code == 25) st->blink = false;
        else if (code == 27) st->inverse = false;
        else if (code == 28) st->hidden = false;
        else if (code == 29) st->strike = false;
        else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) set_color(st->fg, sizeof st->fg, p + i, j - i);
        else if (code == 39) st->fg[0] = 0;
        else if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) set_color(st->bg, sizeof st->bg, p + i, j - i);
        else if (code == 49) st->bg[0] = 0;
        else if (code == 59) st->ulc[0] = 0;
        i = j + 1;
    }
}

void tui_sgr_feed(tui_sgr_t *st, const char *seq, size_t n) {
    if (n >= 3 && seq[1] == '[' && seq[n - 1] == 'm') {
        /* skip private-marker SGR-like sequences such as CSI > 4 ; 2 m */
        if (n > 3 && (seq[2] == '<' || seq[2] == '=' || seq[2] == '>' || seq[2] == '?')) return;
        sgr_params(st, seq + 2, n - 3);
        return;
    }
    if (n >= 5 && seq[1] == ']' && seq[2] == '8' && seq[3] == ';') {
        /* OSC 8 ; params ; uri ST */
        size_t end = n;
        if (seq[n - 1] == '\a') end = n - 1;
        else if (n >= 2 && seq[n - 1] == '\\') end = n - 2;
        const char *body = seq + 4;
        size_t blen = end > 4 ? end - 4 : 0;
        const char *semi = memchr(body, ';', blen);
        if (!semi) return;
        size_t urilen = blen - (size_t)(semi + 1 - body);
        if (urilen == 0 || blen >= sizeof st->link) st->link[0] = 0;
        else {
            memcpy(st->link, body, blen);
            st->link[blen] = 0;
        }
    }
}

bool tui_sgr_active(const tui_sgr_t *st) {
    return st->bold || st->dim || st->italic || st->underline || st->blink || st->inverse || st->hidden ||
           st->strike || st->fg[0] || st->bg[0] || st->ulc[0];
}

void tui_sgr_emit(const tui_sgr_t *st, sb_t *out) {
    if (tui_sgr_active(st)) {
        sb_append(out, "\x1b[");
        bool first = true;
#define ADD(s_)                         \
    do {                                \
        if (!first) sb_putc(out, ';'); \
        sb_append(out, s_);             \
        first = false;                  \
    } while (0)
        if (st->bold) ADD("1");
        if (st->dim) ADD("2");
        if (st->italic) ADD("3");
        if (st->underline) ADD("4");
        if (st->blink) ADD("5");
        if (st->inverse) ADD("7");
        if (st->hidden) ADD("8");
        if (st->strike) ADD("9");
        if (st->fg[0]) ADD(st->fg);
        if (st->bg[0]) ADD(st->bg);
        if (st->ulc[0]) ADD(st->ulc);
#undef ADD
        sb_putc(out, 'm');
    }
    if (st->link[0]) {
        sb_append(out, "\x1b]8;");
        sb_append(out, st->link);
        sb_putc(out, '\a');
    }
}

void tui_sgr_emit_close(const tui_sgr_t *st, sb_t *out) {
    if (tui_sgr_active(st)) sb_append(out, TUI_RESET);
    if (st->link[0]) sb_append(out, TUI_LINK_CLOSE);
}

/* ---------- truncate / slice ---------- */

char *tui_truncate_to_width(const char *s, int width, const char *ellipsis) {
    if (!s) return xstrdup("");
    size_t n = strlen(s);
    if (width < 0) width = 0;
    if (tui_visible_width_n(s, n) <= width) return xstrdup(s);
    if (!ellipsis) ellipsis = "\xe2\x80\xa6";
    int ew = tui_visible_width(ellipsis);
    if (ew > width) {
        ellipsis = "";
        ew = 0;
    }
    int target = width - ew;
    sb_t b = SB_INIT;
    tui_sgr_t st;
    memset(&st, 0, sizeof st);
    int col = 0;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '\x1b') {
            size_t l = tui_esc_len(s + i, n - i);
            if (!l) break;
            tui_sgr_feed(&st, s + i, l);
            sb_appendn(&b, s + i, l);
            i += l;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        int w = tui_cp_width(cp);
        if (col + w > target) break;
        if (cp == '\t') tui_sb_spaces(&b, TUI_TAB_WIDTH);
        else sb_appendn(&b, s + i, (size_t)k);
        col += w;
        i += (size_t)k;
    }
    sb_append(&b, TUI_RESET);
    if (st.link[0]) sb_append(&b, TUI_LINK_CLOSE);
    sb_append(&b, ellipsis);
    return sb_detach(&b);
}

char *tui_slice_cols(const char *s, int start, int len) {
    sb_t b = SB_INIT;
    if (!s || len <= 0) return sb_detach(&b);
    size_t n = strlen(s);
    int end = start + len;
    tui_sgr_t st;
    memset(&st, 0, sizeof st);
    int col = 0;
    bool started = false;
    size_t i = 0;
    while (i < n && col < end) {
        if (s[i] == '\x1b') {
            size_t l = tui_esc_len(s + i, n - i);
            if (!l) break;
            if (started) sb_appendn(&b, s + i, l);
            tui_sgr_feed(&st, s + i, l);
            i += l;
            continue;
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        int w = tui_cp_width(cp);
        if (col + w > start && !started) {
            started = true;
            tui_sgr_emit(&st, &b);
        }
        if (col >= start && col + w <= end) {
            if (cp == '\t') tui_sb_spaces(&b, TUI_TAB_WIDTH);
            else sb_appendn(&b, s + i, (size_t)k);
        } else if (w > 0 && col + w > start) {
            /* straddles a boundary: fill the visible part with spaces */
            int a = MAX(col, start), z = MIN(col + w, end);
            tui_sb_spaces(&b, z - a);
        }
        col += w;
        i += (size_t)k;
    }
    if (started) tui_sgr_emit_close(&st, &b);
    return sb_detach(&b);
}

/* ---------- wrap ---------- */

typedef struct {
    strvec_t *out;
    sb_t line;
    int col;
    int width;
    tui_sgr_t st;
} wrapper_t;

static void wr_break(wrapper_t *w) {
    tui_sgr_emit_close(&w->st, &w->line);
    strvec_push_own(w->out, sb_detach(&w->line));
    w->col = 0;
    tui_sgr_emit(&w->st, &w->line);
}

static bool is_ws(char c) { return c == ' ' || c == '\t'; }

static void wrap_one_line(wrapper_t *w, const char *s, size_t n) {
    sb_t pend = SB_INIT;
    int pend_w = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\x1b') {
            size_t l = tui_esc_len(s + i, n - i);
            if (!l) l = n - i;
            tui_sgr_feed(&w->st, s + i, l);
            /* keep ordering relative to pending whitespace; if the whitespace is dropped at a
             * break, the effect survives through the state re-emitted on the next line */
            sb_appendn(pend.len ? &pend : &w->line, s + i, l);
            i += l;
            continue;
        }
        if (is_ws(c)) {
            if (c == '\t') {
                sb_repeat(&pend, " ", TUI_TAB_WIDTH);
                pend_w += TUI_TAB_WIDTH;
            } else {
                sb_putc(&pend, ' ');
                pend_w++;
            }
            i++;
            continue;
        }
        /* logical word: non-ws chars plus embedded escapes */
        size_t e = i;
        int ww = 0;
        while (e < n && !is_ws(s[e])) {
            if (s[e] == '\x1b') {
                size_t l = tui_esc_len(s + e, n - e);
                if (!l) l = n - e;
                e += l;
                continue;
            }
            uint32_t cp;
            int k = utf8_decode(s + e, n - e, &cp);
            ww += tui_cp_width(cp);
            e += (size_t)k;
        }
        bool long_word = ww > w->width;
        if (w->col + pend_w + ww <= w->width) {
            sb_appendn(&w->line, pend.s ? pend.s : "", pend.len);
            w->col += pend_w;
        } else if (!long_word) {
            if (w->col > 0) wr_break(w);
        } else {
            if (w->col > 0 && w->col + pend_w + 1 <= w->width) {
                sb_appendn(&w->line, pend.s ? pend.s : "", pend.len);
                w->col += pend_w;
            } else if (w->col > 0) {
                wr_break(w);
            }
        }
        sb_clear(&pend);
        pend_w = 0;
        while (i < e) {
            if (s[i] == '\x1b') {
                size_t l = tui_esc_len(s + i, e - i);
                if (!l) l = e - i;
                tui_sgr_feed(&w->st, s + i, l);
                sb_appendn(&w->line, s + i, l);
                i += l;
                continue;
            }
            uint32_t cp;
            int k = utf8_decode(s + i, e - i, &cp);
            int cw = tui_cp_width(cp);
            if (long_word && w->col > 0 && w->col + cw > w->width) wr_break(w);
            sb_appendn(&w->line, s + i, (size_t)k);
            w->col += cw;
            i += (size_t)k;
        }
    }
    if (pend_w > 0 && w->col + pend_w <= w->width) {
        sb_appendn(&w->line, pend.s, pend.len);
        w->col += pend_w;
    }
    sb_free(&pend);
}

void tui_wrap_text_with_ansi(const char *s, int width, strvec_t *out) {
    if (width < 1) width = 1;
    wrapper_t w;
    memset(&w, 0, sizeof w);
    w.out = out;
    w.width = width;
    if (!s) s = "";
    const char *p = s;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && p[len - 1] == '\r') len--;
        sb_clear(&w.line);
        w.col = 0;
        tui_sgr_emit(&w.st, &w.line);
        wrap_one_line(&w, p, len);
        strvec_push_own(out, sb_detach(&w.line));
        if (!nl) break;
        p = nl + 1;
    }
    sb_free(&w.line);
}

/* ---------- misc ---------- */

void tui_sb_spaces(sb_t *b, int n) {
    if (n <= 0) return;
    sb_reserve(b, (size_t)n);
    memset(b->s + b->len, ' ', (size_t)n);
    b->len += (size_t)n;
    b->s[b->len] = 0;
}

char *tui_pad_to_width(const char *s, int width) {
    sb_t b = SB_INIT;
    sb_append(&b, s ? s : "");
    tui_sb_spaces(&b, width - tui_visible_width(s));
    return sb_detach(&b);
}

char *tui_replace_tabs(const char *s) {
    sb_t b = SB_INIT;
    for (; s && *s; s++) {
        if (*s == '\t') tui_sb_spaces(&b, TUI_TAB_WIDTH);
        else sb_putc(&b, *s);
    }
    return sb_detach(&b);
}

char *tui_strip_ansi(const char *s) {
    sb_t b = SB_INIT;
    size_t n = s ? strlen(s) : 0, i = 0;
    while (i < n) {
        if (s[i] == '\x1b') {
            size_t l = tui_esc_len(s + i, n - i);
            if (!l) break;
            i += l;
            continue;
        }
        sb_putc(&b, s[i++]);
    }
    return sb_detach(&b);
}
