/* markdown.c - markdown renderer and syntax highlighter */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "tui/markdown.h"

#include <ctype.h>
#include <strings.h>

#include "tui/symbols.h"
#include "tui/termcaps.h"

#define MD_MAX_DEPTH 8
#define MD_MAX_COLS 32
#define BOX_H "\xe2\x94\x80"
#define BOX_V "\xe2\x94\x82"

typedef struct {
    const tui_theme_t *th;
    const tui_md_opts_t *o;
    char base[512];  /* wrapper SGR (heading, link, quote) re-applied after every reset */
    char deflt[128]; /* default prose style of the component (color, italic), applied to text runs */
    char deflt_exempt[128]; /* deflt inside a themed role under a `proseFg` default: the color is dropped */
    bool run_forced;        /* text runs have their own color (a non-proseFg default): role colors stay outside it */
    bool html_fresh;        /* renderInlineMarkdown: each inline HTML tag is normalized with a fresh list state */
    bool plain;             /* plainInlineTokens: entities and html tags stay as written, links give their label */
} md_ctx_t;

/* tui.colorfulMarkdown (setMarkdownColorful): headings, emphasis, inline code and table headers take their hues
 * from the theme's terminal.ansi palette */
static bool g_colorful = true;
void tui_md_set_colorful(bool on) { g_colorful = on; }
bool tui_md_colorful(void) { return g_colorful; }

/* theme.code */
static const char *code_seq(const md_ctx_t *c) {
    return g_colorful ? tui_ansi_fg_seq(c->th, TUI_ANSI_GREEN) : tui_fg_seq(c->th, TUI_TC_MD_CODE);
}

/* theme.heading(text, depth) (COLORFUL_HEADING_COLORS: H1 magenta, H2 yellow, deeper reuse blue) */
static const char *heading_seq(const md_ctx_t *c, int depth) {
    if (!g_colorful) return tui_fg_seq(c->th, TUI_TC_MD_HEADING);
    return tui_ansi_fg_seq(c->th, depth <= 1 ? TUI_ANSI_MAGENTA : depth == 2 ? TUI_ANSI_YELLOW : TUI_ANSI_BLUE);
}

/* #proseFgExemptContext: a semantically themed span owns its color over a `proseFg` default */
static void ctx_exempt(md_ctx_t *c) { memcpy(c->deflt, c->deflt_exempt, sizeof c->deflt); }

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


/* TERMINAL.hyperlinks: the detected OSC 8 capability under the tui.hyperlinks policy (termcaps.c) */
bool tui_md_hyperlinks_supported(void) { return tui_hyperlinks_enabled(); }

void tui_md_opts_default(tui_md_opts_t *o) {
    memset(o, 0, sizeof *o);
    o->hyperlinks = tui_md_hyperlinks_supported();
    o->code_indent = 2;
    o->text_slot = -1;
    o->bg_slot = -1;
}

/* ====================================================================== */
/* inline                                                                  */
/* ====================================================================== */

typedef struct {
    bool bold, italic, strike;
} istyle_t;

/* A text run: outer wrapper styles (heading/link/quote), then the component's default prose style, then the
 * run's own emphasis. Upstream applies the default style per text run, inside the wrappers. */
static void emit_style(sb_t *b, const md_ctx_t *c, const istyle_t *s) {
    sb_append(b, TUI_RESET);
    sb_append(b, c->base);
    /* colorfulMarkdownRoles: strong and emphasis own the span's color (a forced run color still wins) */
    bool role = g_colorful && (s->bold || s->italic);
    if (role && !c->run_forced) sb_append(b, tui_ansi_fg_seq(c->th, TUI_ANSI_MAGENTA));
    sb_append(b, role ? c->deflt_exempt : c->deflt);
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

/* ---------- emphasis closers (marked/inline-closers.ts) ---------- */

static bool em_space(char ch) { return isspace((unsigned char)ch) != 0; }
static bool em_alnum(char ch) { return isalnum((unsigned char)ch) || (unsigned char)ch >= 0x80; }
static bool em_punct(char ch) { return (unsigned char)ch < 0x80 && ispunct((unsigned char)ch); }

static bool em_can_open(const char *s, size_t i, size_t n, size_t width, char marker, char previous) {
    char before = i == 0 ? previous : s[i - 1];
    if (i == 0 && before == marker) return false;
    if (i + width >= n || em_space(s[i + width])) return false;
    return !(marker == '_' && em_alnum(before) && em_alnum(s[i + width]));
}

/* canOpenDelimiter(rest, 0, ...): matchEmphasis reads what the lexer emitted before, never a marker */
static bool em_can_open_here(const char *s, size_t i, size_t n, size_t width, char marker, char before) {
    if (before == marker) return false;
    if (i + width >= n || em_space(s[i + width])) return false;
    return !(marker == '_' && em_alnum(before) && em_alnum(s[i + width]));
}

static bool em_can_close(const char *s, size_t i, size_t n, char marker) {
    if (i == 0 || em_space(s[i - 1])) return false;
    char before = s[i - 1], after = i + 1 < n ? s[i + 1] : '\n';
    if (marker == '_' && em_alnum(before) && em_alnum(after)) return false;
    return !em_punct(before) || em_space(after) || em_punct(after);
}

typedef struct {
    size_t at;
    int depth;
} em_delim_t;

/* the delimiters a closer walk visits for one marker and width in the source: `closers` sorted by depth, then offset */
typedef struct {
    bool built;
    VEC(em_delim_t) delims, closers;
} em_index_t;

typedef struct {
    size_t at;
    int width;
    bool strong;
} em_open_t;

static int em_delim_cmp(const void *a, const void *b) {
    const em_delim_t *x = a, *y = b;
    if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static void em_index_build(em_index_t *x, const char *s, size_t n, char marker, size_t width) {
    x->built = true;
    int level = 0;
    for (size_t run = 0; run < n;) {
        if (s[run] != marker) {
            run++;
            continue;
        }
        size_t end = run + 1;
        while (end < n && s[end] == marker) end++;
        size_t bs = 0;
        while (bs < run && s[run - bs - 1] == '\\') bs++;
        for (size_t at = bs % 2 ? run + width : run; at + width <= end; at += width) {
            em_delim_t d = {at, level};
            vec_push(&x->delims, d);
            if (em_can_close(s, at, n, marker)) {
                vec_push(&x->closers, d);
                level--;
            } else if (em_can_open(s, at, n, width, marker, '\n')) {
                level++;
            }
        }
        run = end;
    }
    if (x->closers.n > 1) xqsort(x->closers.v, x->closers.n, sizeof(em_delim_t), em_delim_cmp);
}

/* EmphasisDelimiters.closeFor: the closer for the opener at `opener` in a source ending at `end`, or SIZE_MAX */
static size_t em_close_for(const em_index_t *x, const char *s, size_t n, char marker, size_t width, size_t opener, size_t end) {
    size_t walk = opener + width;
    if (width == 2 && walk + 1 < n && s[walk] == marker && s[walk + 1] == marker) return walk + width <= end ? walk : SIZE_MAX;
    size_t lo = 0, hi = x->delims.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (x->delims.v[mid].at < walk) lo = mid + 1;
        else hi = mid;
    }
    if (lo == x->delims.n) return SIZE_MAX;
    em_delim_t key = x->delims.v[lo];
    lo = 0, hi = x->closers.n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (em_delim_cmp(&x->closers.v[mid], &key) < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == x->closers.n || x->closers.v[lo].depth != key.depth) return SIZE_MAX;
    size_t close = x->closers.v[lo].at;
    return close + width <= end ? close : SIZE_MAX;
}

static const char *strstr_n(const char *h, size_t n, const char *needle) {
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (!memcmp(h + i, needle, m)) return h + i;
    return NULL;
}

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

static void put_url(sb_t *b, const char *u, size_t n) {
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)u[i] >= 0x20 && u[i] != 0x7f) sb_putc(b, u[i]);
}

/* [text](href): link-colored underlined text, then " (href)" in linkUrl color unless the text is the href
 * (or the address of a mailto: href). OSC 8 wraps both when hyperlinks are on. */
static void emit_link(const char *text, size_t tn, const char *url, size_t un, const md_ctx_t *c, sb_t *b,
                      istyle_t *st, int depth) {
    if (c->plain) {
        if (depth < 3) render_inline(text, tn, c, b, depth + 1);
        else sb_appendn(b, text, tn);
        return;
    }
    /* the enclosing emphasis stays outside the link: theme.strong(theme.link(theme.underline(text))) */
    char outer[64];
    snprintf(outer, sizeof outer, "%s%s%s%s",
             g_colorful && (st->bold || st->italic) && !c->run_forced ? tui_ansi_fg_seq(c->th, TUI_ANSI_MAGENTA) : "",
             st->bold ? "\x1b[1m" : "", st->italic ? "\x1b[3m" : "", st->strike ? "\x1b[9m" : "");
    md_ctx_t lc;
    ctx_extend(&lc, c, outer, tui_fg_seq(c->th, TUI_TC_MD_LINK), "\x1b[4m");
    ctx_exempt(&lc);
    bool hl = c->o->hyperlinks && un > 0;
    if (hl) {
        sb_append(b, "\x1b]8;;");
        put_url(b, url, un);
        sb_append(b, "\a");
    }
    istyle_t ls = {0};
    emit_style(b, &lc, &ls);
    if (depth < 3) render_inline(text, tn, &lc, b, depth + 1);
    else sb_appendn(b, text, tn);
    if (hl) sb_append(b, TUI_LINK_CLOSE);
    sb_append(b, TUI_RESET);
    sb_append(b, c->base);
    sb_append(b, outer);
    bool same = un == 0 || (tn == un && !memcmp(text, url, un));
    if (un > 7 && !strncmp(url, "mailto:", 7) && tn == un - 7 && !memcmp(text, url + 7, tn)) same = true;
    if (!same) {
        sb_putc(b, ' ');
        if (hl) {
            sb_append(b, "\x1b]8;;");
            put_url(b, url, un);
            sb_append(b, "\a");
        }
        sb_append(b, tui_fg_seq(c->th, TUI_TC_MD_LINK_URL));
        sb_putc(b, '(');
        put_url(b, url, un);
        sb_putc(b, ')');
        sb_append(b, "\x1b[39m");
        if (hl) sb_append(b, TUI_LINK_CLOSE);
    }
    emit_style(b, c, st);
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

/* one character of JavaScript's \s at s (ASCII and Unicode spaces): its byte length, else 0 */
static size_t js_space(const char *s, size_t n) {
    if (!n) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
    if (c < 0x80) return 0;
    uint32_t cp;
    int k = utf8_decode(s, n, &cp);
    if (cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f ||
        cp == 0x205f || cp == 0x3000 || cp == 0xfeff)
        return (size_t)k;
    return 0;
}

/* s.trim() === "" */
static bool js_blank(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        size_t k = js_space(s + i, n - i);
        if (!k) return false;
        i += k;
    }
    return true;
}

/* length of s.trimEnd() */
static size_t js_trim_end(const char *s, size_t n) {
    size_t end = 0;
    for (size_t i = 0; i < n;) {
        size_t k = js_space(s + i, n - i);
        if (!k) {
            uint32_t cp;
            i += (size_t)utf8_decode(s + i, n - i, &cp);
            end = i;
        } else i += k;
    }
    return end;
}

/* the value of a numeric entity (&#N; / &#xH;) at s[0], with its length in *len; -1 when there is none */
static long html_numeric_entity(const char *s, size_t n, size_t *len) {
    if (n < 4 || s[0] != '&' || s[1] != '#') return -1;
    bool hex = s[2] == 'x' || s[2] == 'X';
    size_t p = hex ? 3 : 2, k = p;
    unsigned long v = 0;
    for (; k < n && (hex ? isxdigit((unsigned char)s[k]) : isdigit((unsigned char)s[k])); k++) {
        int d = isdigit((unsigned char)s[k]) ? s[k] - '0' : tolower((unsigned char)s[k]) - 'a' + 10;
        if (v <= 0x10ffff) v = v * (hex ? 16 : 10) + (unsigned long)d;
    }
    if (k == p || k >= n || s[k] != ';' || v > 0x10ffff) return -1;
    *len = k + 1;
    return (long)v;
}

/* upstream normalizeHtmlEntitiesForTerminal, one match at s[0] == '&':
 * /&(amp|lt|gt|quot|apos|nbsp|#\d+|#x[0-9a-fA-F]+);/i -> its character; returns the bytes matched, else 0 */
static size_t html_entity(const char *s, size_t n, sb_t *b) {
    static const struct {
        const char *name;
        const char *out;
    } named[] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}};
    if (n < 3 || s[0] != '&') return 0;
    for (size_t k = 0; k < ARRAY_LEN(named); k++) {
        size_t l = strlen(named[k].name);
        if (n > l + 1 && !strncasecmp(s + 1, named[k].name, l) && s[1 + l] == ';') {
            sb_append(b, named[k].out);
            return l + 2;
        }
    }
    if (s[1] != '#') return 0;
    bool hex = s[2] == 'x' || s[2] == 'X';
    size_t p = hex ? 3 : 2, k = p;
    unsigned long v = 0;
    for (; k < n && (hex ? isxdigit((unsigned char)s[k]) : isdigit((unsigned char)s[k])); k++) {
        int d = isdigit((unsigned char)s[k]) ? s[k] - '0' : tolower((unsigned char)s[k]) - 'a' + 10;
        if (v <= 0x10ffff) v = v * (hex ? 16 : 10) + (unsigned long)d;
    }
    if (k == p || k >= n || s[k] != ';') return 0;
    /* parseCodePoint: out of range -> "" ; a lone surrogate reaches the terminal as U+FFFD */
    if (v > 0x10ffff || v == 0) return k + 1;
    if (v < 0x20) {
        if (v == '\n') sb_putc(b, '\n'); /* other control characters are not drawn */
        return k + 1;
    }
    char u[8];
    size_t used = k + 1;
    if (v >= 0xd800 && v <= 0xdbff && used < n) {
        /* the entity leaves a lone high surrogate in omp's JS string; its terminal writer combines it with whatever
         * UTF-16 unit follows, unchecked: (hi - 0xD800) * 0x400 + (unit - 0xDC00) + 0x10000, swallowing that unit
         * (`&#xD800; ` draws U+2420 ␠, `&#xD83D;&#xDE00;` the pair's emoji). The next unit is the following
         * entity's character once decoded. */
        uint32_t unit = 0;
        size_t take = 0;
        sb_t rest = {0};
        long nv = html_numeric_entity(s + used, n - used, &take);
        if (nv >= 0xd800 && nv <= 0xdfff) unit = (uint32_t)nv;
        else {
            size_t ek = s[used] == '&' ? html_entity(s + used, n - used, &rest) : 0;
            const char *ns = ek ? sb_cstr(&rest) : s + used;
            size_t nn = ek ? rest.len : n - used;
            uint32_t c = 0;
            int cl = nn ? utf8_decode(ns, nn, &c) : 0;
            if (cl > 0 && !(c == 0xfffd && cl == 1) && c < 0x10000) {
                unit = c;
                take = ek ? ek : (size_t)cl;
                if (ek) memmove(rest.s, rest.s + cl, rest.len - (size_t)cl + 1), rest.len -= (size_t)cl;
                else rest.len = 0;
            } else take = 0;
        }
        if (take) {
            uint32_t cp = (uint32_t)(v - 0xd800) * 0x400 + unit + 0x10000 - 0xdc00;
            int ul = utf8_encode(cp >= 0xd800 && cp <= 0xdfff ? 0xfffd : cp, u);
            sb_appendn(b, u, (size_t)ul);
            if (rest.len) sb_appendn(b, rest.s, rest.len);
            sb_free(&rest);
            return used + take;
        }
        sb_free(&rest);
    }
    int ul = utf8_encode(v >= 0xd800 && v <= 0xdfff ? 0xfffd : (uint32_t)v, u);
    sb_appendn(b, u, (size_t)ul);
    return used;
}

static void html_decode_entities(const char *s, size_t n, sb_t *b) {
    for (size_t i = 0; i < n;) {
        if (s[i] == '&') {
            size_t k = html_entity(s + i, n - i, b);
            if (k) {
                i += k;
                continue;
            }
        }
        sb_putc(b, s[i++]);
    }
}

/* InlineIndexes for the inline HTML rule over one source, built on first use so a scan stays linear */
typedef struct {
    const char *s;
    size_t n;
    size_t *tag_end;      /* tagEnds: where a tag scan from each offset stops (its ">"), SIZE_MAX for none */
    size_t no_cend;       /* no "-->" starts at or after this offset */
    size_t no_code_close; /* no </code> token follows this offset */
} html_scan_t;

static void html_scan_init(html_scan_t *x, const char *s, size_t n) {
    memset(x, 0, sizeof *x);
    x->s = s;
    x->n = n;
    x->no_cend = x->no_code_close = SIZE_MAX;
}

/* tagEnds: the first ">" at or after each offset outside quotes; a quote skips past its partner and an unpaired
 * one ends the scan */
static void html_scan_tag_ends(html_scan_t *x) {
    size_t n = x->n, *e = xmalloc((n + 1) * sizeof *e), next_d = SIZE_MAX, next_s = SIZE_MAX;
    e[n] = SIZE_MAX;
    for (size_t at = n; at-- > 0;) {
        char c = x->s[at];
        if (c == '>') e[at] = at;
        else if (c == '"') {
            e[at] = next_d == SIZE_MAX ? SIZE_MAX : e[next_d + 1];
            next_d = at;
        } else if (c == '\'') {
            e[at] = next_s == SIZE_MAX ? SIZE_MAX : e[next_s + 1];
            next_s = at;
        } else e[at] = e[at + 1];
    }
    x->tag_end = e;
}

/* marked (utils/src/marked) inlineHtmlPrefix: the inline HTML at s[pos] == '<' in a source ending at `lim`: a
 * comment up to the first "-->", or /^<\/?[A-Za-z][A-Za-z0-9-]*(?:\s|\/?>)/ up to the first ">" outside quotes;
 * its length, else 0 */
static size_t inline_html_len(html_scan_t *x, size_t pos, size_t lim) {
    const char *s = x->s + pos;
    size_t n = lim - pos;
    if (n >= 4 && !strncmp(s, "<!--", 4)) {
        if (pos + 4 >= x->no_cend) return 0;
        const char *e = strstr_n(s + 4, n - 4, "-->");
        if (!e && lim == x->n) x->no_cend = pos + 4;
        return e ? (size_t)(e - s) + 3 : 0;
    }
    size_t i = 1;
    if (i < n && s[i] == '/') i++;
    if (i >= n || !isalpha((unsigned char)s[i])) return 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '-')) i++;
    if (i >= n) return 0;
    if (!js_space(s + i, n - i) && s[i] != '>' && !(s[i] == '/' && i + 1 < n && s[i + 1] == '>')) return 0;
    if (!x->tag_end) html_scan_tag_ends(x);
    size_t close = x->tag_end[pos + 1];
    return close == SIZE_MAX || close >= lim ? 0 : close - pos + 1;
}

/* htmlTagName: /^<\/?\s*([A-Za-z][A-Za-z0-9:-]*)/, lowercased ("" for a comment) */
static void html_tag_name(const char *s, size_t n, char *name, size_t cap) {
    size_t i = 1, k = 0;
    if (i < n && s[i] == '/') i++;
    while (i < n && js_space(s + i, n - i)) i += js_space(s + i, n - i);
    if (i < n && isalpha((unsigned char)s[i]))
        while (i < n && (isalnum((unsigned char)s[i]) || s[i] == ':' || s[i] == '-') && k + 1 < cap)
            name[k++] = (char)tolower((unsigned char)s[i++]);
    name[k] = 0;
}

/* HTML_TAG_REGEX /<\/?(?:br|p|ol|ul|li|span|text|code|hr|blockquote)\b(?:\s[^>]*)?\s*\/?>/i at s[0] == '<':
 * the tag's length and its lowercase name, else 0 */
static size_t html_known_tag(const char *s, size_t n, char *name, const char **no_gt) {
    static const char *const TAGS[] = {"br", "p", "ol", "ul", "li", "span", "text", "code", "hr", "blockquote"};
    size_t i = 1, k = 0;
    if (i < n && s[i] == '/') i++;
    size_t start = i;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
    if (i - start > 10 || i == start) return 0;
    for (; k < i - start; k++) name[k] = (char)tolower((unsigned char)s[start + k]);
    name[k] = 0;
    bool known = false;
    for (size_t t = 0; t < ARRAY_LEN(TAGS); t++)
        if (!strcmp(name, TAGS[t])) known = true;
    if (!known || i >= n) return 0;
    if (js_space(s + i, n - i)) {
        const char *gt = s + i >= *no_gt ? NULL : memchr(s + i, '>', n - i);
        if (!gt) *no_gt = s + i; /* no ">" after here: later tags cannot close either */
        return gt ? (size_t)(gt - s) + 1 : 0;
    }
    if (s[i] == '>') return i + 1;
    if (s[i] == '/' && i + 1 < n && s[i + 1] == '>') return i + 2;
    return 0;
}

/* HtmlNormalizationState: one entry per open <ol>/<ul> (lists, openItems and itemHasContent move together) */
typedef struct {
    bool ol;
    double next;
    bool open, content;
} html_list_t;
typedef VEC(html_list_t) html_state_t;

/* htmlOlStart: /\bstart\s*=\s*(?:"(\d+)"|'(\d+)'|(\d+))/i, else 1 */
static double html_ol_start(const char *s, size_t n) {
    for (size_t i = 0; i + 5 <= n; i++) {
        if (strncasecmp(s + i, "start", 5) || (i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '_'))) continue;
        size_t j = i + 5;
        while (j < n && js_space(s + j, n - j)) j += js_space(s + j, n - j);
        if (j >= n || s[j] != '=') continue;
        j++;
        while (j < n && js_space(s + j, n - j)) j += js_space(s + j, n - j);
        char q = j < n && (s[j] == '"' || s[j] == '\'') ? s[j] : 0;
        if (q) j++;
        size_t d = j;
        double v = 0;
        while (j < n && isdigit((unsigned char)s[j])) v = v * 10 + (s[j++] - '0');
        if (j == d || (q && (j >= n || s[j] != q))) continue;
        return v;
    }
    return 1;
}

static void html_mark_content(html_state_t *st, const char *s, size_t n) {
    if (st->n && !js_blank(s, n)) st->v[st->n - 1].content = true;
}

/* appendHtmlLineBreak */
static void html_line_break(sb_t *o, bool force) {
    size_t n = o->len;
    while (n && (o->s[n - 1] == ' ' || o->s[n - 1] == '\t')) n--;
    sb_truncate(o, n);
    if (force || !o->len || o->s[o->len - 1] != '\n') sb_putc(o, '\n');
}

/* htmlListIndent: two spaces per enclosing list (capped: a hostile nesting would grow the output quadratically) */
#define HTML_MAX_INDENT 32
static size_t html_indent(const html_state_t *st) { return 2 * (st->n > 1 ? MIN(st->n - 1, HTML_MAX_INDENT) : 0); }

/* appendHtmlListBreak: no break when the output already ends with the list's indent and a newline */
static void html_list_break(sb_t *o, const html_state_t *st) {
    size_t ind = html_indent(st);
    bool ends = o->len >= ind + 1 && o->s[o->len - 1] == '\n';
    for (size_t k = 0; ends && k < ind; k++)
        if (o->s[o->len - 2 - k] != ' ') ends = false;
    if (!ends) html_line_break(o, false);
}

/* the codeHook of #renderHtmlBlock / #renderHtmlBlockquote: theme.code, then the surrounding style again */
typedef struct {
    const md_ctx_t *c;
    const char *after;
} html_hook_t;

static void html_put(sb_t *o, const char *s, size_t n, bool code, const html_hook_t *hook) {
    if (!n) return;
    if (code && hook) {
        sb_append(o, code_seq(hook->c));
        sb_appendn(o, s, n);
        sb_append(o, TUI_RESET);
        sb_append(o, hook->after);
    } else sb_appendn(o, s, n);
}

/* normalizeHtmlForTerminal: comments dropped, entities decoded, <br>/<hr> break the line, <p>/<blockquote> separate
 * paragraphs, <ol>/<ul>/<li> become numbered or bulleted lines indented by list depth, <span>/<text> vanish and
 * <code> runs go through `hook`; any other tag stays verbatim. Appends to `o`, which must start empty. */
static void html_normalize(const char *raw, size_t rn, html_state_t *st, const html_hook_t *hook, sb_t *o) {
    sb_t wc = SB_INIT;
    bool no_cend = false;
    for (size_t i = 0; i < rn;) { /* HTML_COMMENT_REGEX /<!--[\s\S]*?-->/g */
        if (!no_cend && i + 4 <= rn && !strncmp(raw + i, "<!--", 4)) {
            const char *e = strstr_n(raw + i + 4, rn - i - 4, "-->");
            if (e) {
                i = (size_t)(e - raw) + 3;
                continue;
            }
            no_cend = true; /* no later comment closes either */
        }
        sb_putc(&wc, raw[i++]);
    }
    const char *w = wc.s ? wc.s : "";
    size_t wn = wc.len, last = 0;
    bool in_code = false;
    const char *no_gt = w + wn + 1;
    sb_t text = SB_INIT;
    for (size_t p = 0; p < wn; p++) {
        char name[16];
        size_t tl = w[p] == '<' ? html_known_tag(w + p, wn - p, name, &no_gt) : 0;
        if (!tl) continue;
        sb_truncate(&text, 0);
        html_decode_entities(w + last, p - last, &text);
        bool inl = !strcmp(name, "span") || !strcmp(name, "text");
        if (inl || in_code || !js_blank(text.s, text.len)) {
            html_put(o, text.s, text.len, in_code, hook);
            html_mark_content(st, text.s, text.len);
        }
        last = p + tl;
        bool closing = w[p + 1] == '/';
        size_t q = last - 1; /* /\/\s*>$/ */
        while (q > p && js_space(w + q - 1, 1)) q--;
        bool self_closing = q > p && w[q - 1] == '/';
        if (!strcmp(name, "code")) {
            if (closing) in_code = false;
            else if (!self_closing) in_code = true;
        } else if (!strcmp(name, "br") || !strcmp(name, "hr")) {
            html_line_break(o, true);
        } else if (!strcmp(name, "p") || !strcmp(name, "blockquote")) {
            if (closing) html_line_break(o, false);
            else if (!js_blank(o->s, o->len) && o->s[o->len - 1] != '\n' &&
                     !(st->n && st->v[st->n - 1].open && !st->v[st->n - 1].content))
                html_line_break(o, false);
        } else if (!strcmp(name, "ol") || !strcmp(name, "ul")) {
            if (closing) {
                if (st->n) st->n--;
            } else if (!self_closing) {
                if (st->n && st->v[st->n - 1].open) html_list_break(o, st);
                html_list_t l = {name[0] == 'o', name[0] == 'o' ? html_ol_start(w + p, tl) : 1, false, false};
                vec_push(st, l);
            }
        } else if (!strcmp(name, "li")) {
            if (closing) {
                html_line_break(o, false);
            } else {
                if (st->n) {
                    if (st->v[st->n - 1].open) html_list_break(o, st);
                    st->v[st->n - 1].open = true;
                    st->v[st->n - 1].content = false;
                } else if (!js_blank(o->s, o->len) && o->s[o->len - 1] != '\n') {
                    html_line_break(o, false);
                }
                for (size_t k = html_indent(st); k > 0; k--) sb_putc(o, ' ');
                if (st->n && st->v[st->n - 1].ol) sb_appendf(o, "%.0f. ", st->v[st->n - 1].next++);
                else sb_append(o, "\xe2\x80\xa2 ");
            }
        } /* span, text: nothing */
        p = last - 1;
    }
    sb_truncate(&text, 0);
    html_decode_entities(w + last, wn - last, &text);
    html_mark_content(st, text.s, text.len);
    html_put(o, text.s, text.len, in_code, hook);
    sb_free(&text);
    sb_free(&wc);
}

/* a code span: wrapper styles and emphasis stay, the default prose style does not */
static void emit_code(sb_t *b, const md_ctx_t *c, const istyle_t *st, const char *text, size_t n) {
    sb_append(b, TUI_RESET);
    sb_append(b, c->base);
    if (st->bold) sb_append(b, "\x1b[1m");
    if (st->italic) sb_append(b, "\x1b[3m");
    if (st->strike) sb_append(b, "\x1b[9m");
    sb_append(b, code_seq(c));
    sb_appendn(b, text, n);
    emit_style(b, c, st);
}

/* the closing run of a backtick string of `r` at s[i] in a source ending at n, else SIZE_MAX */
static size_t code_run_close(const char *s, size_t i, size_t r, size_t n) {
    for (size_t k = i + r; k < n;) {
        if (s[k] != '`') {
            k++;
            continue;
        }
        size_t r2 = 0;
        while (k + r2 < n && s[k + r2] == '`') r2++;
        if (r2 == r) return k;
        k += r2;
    }
    return SIZE_MAX;
}

/* collapseInlineHtml: the `</code>` html token closing a <code> run, searched from `from` among the tokens of the
 * same source (escapes, code spans and other html tokens are skipped whole); its offset and length, else `n` */
static size_t html_code_close(html_scan_t *x, size_t from, size_t n, size_t *len) {
    const char *s = x->s;
    if (n == x->n && from >= x->no_code_close) return n;
    size_t no_run[8];
    for (size_t k = 0; k < ARRAY_LEN(no_run); k++) no_run[k] = SIZE_MAX;
    for (size_t j = from; j < n;) {
        if (s[j] == '\\' && j + 1 < n && is_punct(s[j + 1])) {
            j += 2;
        } else if (s[j] == '`') {
            size_t r = 0;
            while (j + r < n && s[j + r] == '`') r++;
            size_t e = r < 8 && j >= no_run[r] ? SIZE_MAX : code_run_close(s, j, r, n);
            if (e == SIZE_MAX && r < 8 && j < no_run[r]) no_run[r] = j;
            j = e == SIZE_MAX ? j + r : e + r;
        } else if (s[j] == '<') {
            size_t hl = inline_html_len(x, j, n);
            if (!hl) {
                j++;
                continue;
            }
            char name[16];
            html_tag_name(s + j, hl, name, sizeof name);
            if (!strcmp(name, "code") && s[j + 1] == '/') {
                *len = hl;
                return j;
            }
            j += hl;
        } else j++;
    }
    if (n == x->n && from < x->no_code_close) x->no_code_close = from;
    return n;
}

static void render_inline(const char *s, size_t n, const md_ctx_t *c, sb_t *b, int depth) {
    istyle_t st = {0};
    scan_cache_t sc;
    em_index_t ed[2][2] = {{{0}}};
    em_open_t es[64];
    /* each emphasis level renders its content with its own HTML list state (a nested #renderInlineTokens) */
    html_state_t hst[65] = {{0}};
    html_scan_t hx;
    html_scan_init(&hx, s, n);
    size_t en = 0, nest_start = 0;
    int nbold = st.bold ? 1 : 0, nitalic = st.italic ? 1 : 0;
    memset(&sc, 0xff, sizeof sc);
    /* no leading reset: an enclosing style (a bold table header cell) stays in effect */
    size_t start = b->len;
    sb_append(b, c->base);
    sb_append(b, c->deflt);
    bool styled = c->base[0] || c->deflt[0];
    size_t i = 0;
    bool line_start = false; /* after a hard break: drop the next line's leading whitespace */
    while (i < n) {
        char ch = s[i];
        if (en && i >= es[en - 1].at) { /* the emphasis closes here */
            em_open_t top = es[--en];
            if (top.strong) nbold--;
            else nitalic--;
            st.bold = nbold > 0;
            st.italic = nitalic > 0;
            emit_style(b, c, &st);
            if (i == top.at) i += (size_t)top.width;
            line_start = false; /* a line break inside the span trims only inside it */
            continue;
        }
        bool at_line_start = line_start;
        if (line_start) { /* the next text's /^\s+/ */
            line_start = false;
            while (i < n && js_space(s + i, n - i)) i += js_space(s + i, n - i);
            if (i >= n) break;
            ch = s[i];
        }
        if (ch == '\\' && i + 1 < n && s[i + 1] == '\n') { /* backslash hard break */
            sb_putc(b, '\n');
            i += 2;
            line_start = true;
            continue;
        }
        if (ch == '\\' && i + 1 < n && is_punct(s[i + 1])) {
            sb_putc(b, s[i + 1]);
            i += 2;
            continue;
        }
        if (ch == '\n') {
            /* two trailing spaces before the newline = hard break (renders the same) */
            sb_putc(b, '\n');
            i++;
            continue;
        }
        if (ch == '&' && !c->plain) {
            size_t k = html_entity(s + i, n - i, b);
            if (k) {
                i += k;
                continue;
            }
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
            bool all_space = true;
            for (size_t q = 0; q < cl; q++)
                if (cs[q] != ' ' && cs[q] != '\n') all_space = false;
            if (cl >= 2 && !all_space && (cs[0] == ' ' || cs[0] == '\n') && (cs[cl - 1] == ' ' || cs[cl - 1] == '\n')) {
                cs++;
                cl -= 2;
            }
            char *ct = xstrndup(cs, cl);
            for (size_t q = 0; q < cl; q++)
                if (ct[q] == '\n') ct[q] = ' ';
            emit_code(b, c, &st, ct, cl);
            free(ct);
            i = close + r;
            continue;
        }
        if (ch == '*' || ch == '_') {
            /* marked/core.ts matchEmphasis: a match opens here and closes at a known offset; no match is text, and the
             * rest of the run then follows a marker, which cannot open */
            size_t limit = en ? es[en - 1].at : n;
            char prev = i == nest_start ? '\n' : s[i - 1];
            size_t open = 0, close = 0, skip = 0;
            bool strong = false;
            if (i + 2 < n && s[i + 1] == ch && s[i + 2] == ch && em_can_open_here(s, i, limit, 3, ch, prev)) {
                char d[4] = {ch, ch, ch, 0};
                const char *e = s + i + 3;
                size_t at = SIZE_MAX;
                while ((e = strstr_n(e, (size_t)(s + limit - e), d))) {
                    size_t k = 0;
                    while ((size_t)(e - s) > k && e[-(ptrdiff_t)k - 1] == '\\') k++;
                    if (k % 2 == 0) {
                        at = (size_t)(e - s);
                        break;
                    }
                    e += 3;
                }
                if (at != SIZE_MAX && em_can_close(s, at, n, ch)) {
                    open = 1;
                    close = at + 2;
                    skip = 1;
                }
            }
            if (!open) {
                size_t w = i + 1 < n && s[i + 1] == ch ? 2 : 1;
                if (em_can_open_here(s, i, limit, w, ch, prev)) {
                    if (!ed[ch == '_'][w - 1].built) em_index_build(&ed[ch == '_'][w - 1], s, n, ch, w);
                    size_t at = em_close_for(&ed[ch == '_'][w - 1], s, n, ch, w, i, limit);
                    if (at != SIZE_MAX) {
                        open = 1;
                        close = at;
                        skip = w;
                        strong = w == 2;
                    }
                }
            }
            if (open && en < ARRAY_LEN(es)) {
                es[en++] = (em_open_t){close, strong ? 2 : 1, strong};
                hst[en].n = 0;
                line_start = false;
                if (strong) nbold++;
                else nitalic++;
                st.bold = nbold > 0;
                st.italic = nitalic > 0;
                emit_style(b, c, &st);
                i += skip;
                nest_start = i;
                continue;
            }
            sb_putc(b, ch);
            i++;
            continue;
        }
        if (ch == '~' && i + 1 < n && s[i + 1] == '~') {
            char next = i + 2 < n ? s[i + 2] : ' ';
            char prev = i > 0 ? s[i - 1] : ' ';
            if (st.strike && !isspace((unsigned char)prev) && prev != '~' && next != '~') {
                st.strike = false;
                emit_style(b, c, &st);
                i += 2;
                line_start = false;
                continue;
            }
            if (!st.strike && !isspace((unsigned char)next) && next != '~' && closer_cached(&sc, s, i + 2, n, '~', 2)) {
                st.strike = true;
                emit_style(b, c, &st);
                i += 2;
                line_start = false;
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
                    size_t u0 = us;
                    while (u0 < k && s[u0] == ' ') u0++;
                    size_t ue = u0;
                    while (ue < k && s[ue] != ' ') ue++;
                    const char *url = s + u0;
                    size_t ul = ue - u0;
                    if (ul >= 2 && url[0] == '<' && url[ul - 1] == '>') {
                        url++;
                        ul -= 2;
                    }
                    if (ch == '!') {
                        /* images render as their alt text */
                        if (depth < 3) {
                            md_ctx_t ac = *c;
                            render_inline(s + ts, j - ts, &ac, b, depth + 1);
                        } else sb_appendn(b, s + ts, j - ts);
                        emit_style(b, c, &st);
                    } else {
                        emit_link(s + ts, j - ts, url, ul, c, b, &st, depth);
                    }
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
                if (memchr(u, '@', ul) && !memchr(u, ':', ul) && ul > 2) {
                    char *mu = xasprintf("mailto:%.*s", (int)ul, u);
                    emit_link(u, ul, mu, strlen(mu), c, b, &st, depth);
                    free(mu);
                    i = j + 1;
                    continue;
                }
            }
            /* inline html (marked's html token): <code>..</code> runs collapse into a code span (collapseInlineHtml),
             * any other tag or comment goes through normalizeHtmlForTerminal */
            size_t lim = en ? es[en - 1].at : n;
            size_t hl = i < lim ? inline_html_len(&hx, i, lim) : 0;
            if (hl && c->plain) {
                sb_appendn(b, s + i, hl);
                i += hl;
                continue;
            }
            if (hl) {
                char name[16];
                html_tag_name(s + i, hl, name, sizeof name);
                if (!strcmp(name, "code")) {
                    size_t cl = 0, cj = s[i + 1] == '/' ? lim : html_code_close(&hx, i + hl, lim, &cl);
                    if (cj < lim) {
                        /* the run's tokens as plain text (plainInlineTokens), entities decoded */
                        md_ctx_t pc = *c;
                        pc.plain = true;
                        pc.base[0] = pc.deflt[0] = pc.deflt_exempt[0] = 0;
                        sb_t raw = SB_INIT, dec = SB_INIT;
                        render_inline(s + i + hl, cj - i - hl, &pc, &raw, depth + 1);
                        char *plain = tui_strip_ansi(sb_cstr(&raw));
                        html_decode_entities(plain, strlen(plain), &dec);
                        emit_code(b, c, &st, dec.s ? dec.s : "", dec.len);
                        free(plain);
                        sb_free(&raw);
                        sb_free(&dec);
                        i = cj + cl;
                        continue;
                    }
                    /* a stray </code> or an unmatched <code> is dropped */
                    i += hl;
                    line_start = at_line_start;
                    continue;
                }
                sb_t o = SB_INIT;
                html_state_t fresh = {0};
                html_normalize(s + i, hl, c->html_fresh ? &fresh : &hst[en], NULL, &o);
                free(fresh.v);
                for (size_t q = 0; q < o.len;) {
                    uint32_t cp;
                    int k = utf8_decode(o.s + q, o.len - q, &cp);
                    if (cp < 0x20 && cp != '\t' && cp != '\n') sb_putc(b, ' ');
                    else sb_appendn(b, o.s + q, (size_t)k);
                    q += (size_t)k;
                }
                if (o.len && o.s[o.len - 1] == '\n') line_start = true;
                else if (!o.len) line_start = at_line_start;
                sb_free(&o);
                i += hl;
                continue;
            }
        }
        if ((ch == 'h' || ch == 'w' || ch == 'f') && (i == 0 || strchr(" \t\n*_~(", s[i - 1]))) {
            bool www = n - i > 4 && !strncmp(s + i, "www.", 4);
            bool sch = (n - i > 8 && (!strncmp(s + i, "http://", 7) || !strncmp(s + i, "https://", 8))) ||
                       (n - i > 6 && !strncmp(s + i, "ftp://", 6));
            if (www || sch) {
                size_t e = url_end(s, i, n);
                if (e > i + (www ? 5 : 8)) {
                    if (www) {
                        char *href = xasprintf("http://%.*s", (int)(e - i), s + i);
                        emit_link(s + i, e - i, href, strlen(href), c, b, &st, depth);
                        free(href);
                    } else {
                        emit_link(s + i, e - i, s + i, e - i, c, b, &st, depth);
                    }
                    i = e;
                    continue;
                }
            }
        }
        uint32_t cp;
        int k = utf8_decode(s + i, n - i, &cp);
        if (cp < 0x20 && cp != '\t') sb_putc(b, ' ');
        else sb_appendn(b, s + i, (size_t)k);
        i += (size_t)k;
    }
    for (int x = 0; x < 2; x++)
        for (int y = 0; y < 2; y++) {
            free(ed[x][y].delims.v);
            free(ed[x][y].closers.v);
        }
    for (size_t x = 0; x < ARRAY_LEN(hst); x++) free(hst[x].v);
    free(hx.tag_end);
    if (styled || st.bold || st.italic || st.strike || memmem(b->s, b->len, "\x1b[", 2)) {
        /* the reset closes the last line: trailing line breaks (an html </li> or </p>) stay bare, as upstream strips
         * the dangling style prefix, so a wrapped cell or row does not gain a styled empty line */
        size_t nl = 0;
        while (nl < b->len - start && b->s[b->len - 1 - nl] == '\n') nl++;
        sb_truncate(b, b->len - nl);
        sb_append(b, TUI_RESET);
        for (size_t k = 0; k < nl; k++) sb_putc(b, '\n');
    }
}

/* ====================================================================== */
/* blocks: a line-based lexer producing marked-like block tokens, then the */
/* upstream Markdown component's per-token layout and spacing rules        */
/* ====================================================================== */

typedef enum { T_NONE, T_SPACE, T_PARA, T_HEADING, T_CODE, T_LIST, T_QUOTE, T_TABLE, T_HR, T_HTML } ttype_t;

typedef struct {
    const char *const *v;
    size_t n;
} lines_t;

typedef struct {
    ttype_t t;
    size_t from, to; /* source lines [from, to) */
    int level;       /* heading level */
    bool setext;     /* heading: underline on line to-1 */
    char hrc[8];     /* hr: source glyph */
    char lang[32];   /* code */
    size_t strip;    /* code: indent stripped from body lines */
    bool fenced;     /* code: from/to include the fences (closing fence may be missing) */
    bool closed;
} tok_t;

typedef VEC(tok_t) toks_t;

/* one rendered row; literal rows (code bodies with code_indent 0) skip margins and quote borders */
typedef struct {
    char *text;
    bool literal;
    bool nested; /* list rows already carrying their own indent */
} row_t;

typedef VEC(row_t) rows_t;

static void rows_push(rows_t *r, char *text, bool literal) {
    row_t x = {text, literal, false};
    vec_push(r, x);
}

static void rows_free(rows_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].text);
    free(r->v);
    memset(r, 0, sizeof *r);
}

static size_t indent_of(const char *s) {
    size_t n = 0;
    for (; *s == ' ' || *s == '\t'; s++) n += *s == '\t' ? 4 - (n % 4) : 1;
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

/* `^ {0,3}(=+|-+)[ \t]*$`: a setext heading underline */
static bool is_setext_underline(const char *line) {
    if (indent_of(line) > 3) return false;
    const char *x = skip_ws(line);
    if (*x != '=' && *x != '-') return false;
    char u = *x;
    while (*x == u) x++;
    while (*x == ' ' || *x == '\t') x++;
    return !*x;
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

/* thematic break: 3+ of one of - * _ ─ ━ ═ = – — (spaces allowed); copies the glyph */
static bool is_hr(const char *s, char *glyph) {
    if (indent_of(s) > 3) return false;
    s = skip_ws(s);
    static const char *const G[] = {"-", "*", "_", "=", "\xe2\x94\x80", "\xe2\x94\x81", "\xe2\x95\x90",
                                    "\xe2\x80\x93", "\xe2\x80\x94"};
    for (size_t g = 0; g < ARRAY_LEN(G); g++) {
        size_t gl = strlen(G[g]);
        if (strncmp(s, G[g], gl)) continue;
        int cnt = 0;
        const char *p = s;
        while (*p) {
            if (!strncmp(p, G[g], gl)) {
                cnt++;
                p += gl;
            } else if (*p == ' ' || *p == '\t') p++;
            else return false;
        }
        if (cnt < 3) return false;
        if (glyph) snprintf(glyph, 8, "%s", G[g]);
        return true;
    }
    return false;
}

static bool is_quote(const char *s) { return indent_of(s) <= 3 && *skip_ws(s) == '>'; }

/* list item marker: bullet char ('-', '*', '+') or ordered delimiter ('.', ')') in *kind */
static bool list_item(const char *s, size_t *indent, char *kind, long *num, size_t *content_off, size_t *content_col) {
    size_t ind = indent_of(s);
    const char *p = skip_ws(s);
    char k;
    long nv = 0;
    if (*p == '-' || *p == '*' || *p == '+') {
        k = *p;
        p++;
    } else if (isdigit((unsigned char)*p)) {
        int d = 0;
        while (isdigit((unsigned char)*p) && d < 9) {
            nv = nv * 10 + (*p - '0');
            p++;
            d++;
        }
        if (*p != '.' && *p != ')') return false;
        k = *p;
        p++;
    } else {
        return false;
    }
    if (*p && *p != ' ' && *p != '\t') return false;
    if ((k == '-' || k == '*') && is_hr(s, NULL)) return false;
    size_t mcol = ind + (size_t)(p - skip_ws(s));
    const char *c = p;
    size_t sp = 0;
    while ((*c == ' ' || *c == '\t') && sp < 5) {
        c++;
        sp++;
    }
    if (!*c) sp = 1; /* empty item */
    else if (sp > 4) {
        c = p + 1;
        sp = 1;
    }
    if (indent) *indent = ind;
    if (kind) *kind = k;
    if (num) *num = nv;
    if (content_off) *content_off = (size_t)(c - s);
    if (content_col) *content_col = mcol + sp;
    return true;
}

static void split_cells(const char *s, strvec_t *cells);

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

static bool is_table_start(const lines_t *L, size_t i, size_t to) {
    if (i + 1 >= to || !strchr(L->v[i], '|') || !is_table_sep(L->v[i + 1])) return false;
    strvec_t h = {0}, d = {0};
    split_cells(L->v[i], &h);
    split_cells(L->v[i + 1], &d);
    bool ok = h.n == d.n && h.n > 0;
    strvec_free(&h);
    strvec_free(&d);
    return ok;
}

/* marked/core.ts isHtmlStart: a comment, or an opening tag of a block-level element followed by \s, ">" or "/>" */
static bool is_html_block(const char *s) {
    if (indent_of(s) > 3) return false;
    s = skip_ws(s);
    if (!strncmp(s, "<!--", 4)) return true;
    if (*s != '<') return false;
    static const char *const TAGS[] = {
        "script",   "pre",      "style",    "textarea", "address", "article",    "aside",    "base",     "basefont",
        "blockquote", "body",   "caption",  "center",   "col",     "colgroup",   "dd",       "details",  "dialog",
        "dir",      "div",      "dl",       "dt",       "fieldset", "figcaption", "figure",  "footer",   "form",
        "frame",    "frameset", "h1",       "h2",       "h3",      "h4",         "h5",       "h6",       "head",
        "header",   "hr",       "html",     "iframe",   "legend",  "li",         "link",     "main",     "menu",
        "menuitem", "nav",      "noframes", "ol",       "optgroup", "option",    "p",        "param",    "search",
        "section",  "summary",  "table",    "tbody",    "td",      "tfoot",      "th",       "thead",    "title",
        "tr",       "track",    "ul"};
    const char *p = s + 1;
    for (size_t t = 0; t < ARRAY_LEN(TAGS); t++) {
        size_t k = strlen(TAGS[t]);
        if (strncasecmp(p, TAGS[t], k)) continue;
        const char *e = p + k; /* lines carry no "\n": the end of the line is the \s */
        if (!*e || js_space(e, strlen(e)) || *e == '>' || (e[0] == '/' && e[1] == '>')) return true;
    }
    return false;
}

/* does the line hold "</tag>" (case-insensitive)? */
static bool has_close_tag(const char *s, const char *tag, size_t tl) {
    for (const char *p = strstr(s, "</"); p; p = strstr(p + 1, "</"))
        if (!strncasecmp(p + 2, tag, tl) && p[2 + tl] == '>') return true;
    return false;
}

static bool is_ref_def(const char *s) {
    if (indent_of(s) > 3) return false;
    s = skip_ws(s);
    if (*s != '[' || s[1] == ']') return false;
    const char *e = strstr(s, "]:");
    if (!e || memchr(s + 1, '[', (size_t)(e - s - 1))) return false;
    return *skip_ws(e + 2) != 0;
}

/* does line i interrupt a paragraph? */
static bool interrupts_para(const lines_t *L, size_t i, size_t to) {
    const char *s = L->v[i];
    char k;
    long num;
    size_t co;
    if (is_fence(s, NULL, NULL, NULL) || heading_level(s) || is_hr(s, NULL) || is_quote(s) || is_html_block(s))
        return true;
    if (list_item(s, NULL, &k, &num, &co, NULL) && indent_of(s) <= 3) return true; /* marked/core.ts isBlockStart: any list marker */
    return is_table_start(L, i, to);
}

static void lex_blocks(const lines_t *L, size_t from, size_t to, toks_t *out);

static size_t lex_list(const lines_t *L, size_t i, size_t to) {
    char kind;
    size_t ccol, ind0;
    list_item(L->v[i], &ind0, &kind, NULL, NULL, &ccol);
    size_t j = i + 1;
    while (j < to) {
        const char *s = L->v[j];
        if (is_blank(s)) {
            size_t k = j;
            while (k < to && is_blank(L->v[k])) k++;
            if (k >= to) break;
            const char *x = L->v[k];
            size_t xi;
            char xk;
            size_t xc;
            if (indent_of(x) >= ccol) {
                j = k;
                continue;
            }
            if (list_item(x, &xi, &xk, NULL, NULL, &xc) && xk == kind && xi < ccol) {
                ccol = xc;
                j = k + 1;
                continue;
            }
            break;
        }
        size_t xi, xc;
        char xk;
        if (indent_of(s) < ccol && list_item(s, &xi, &xk, NULL, NULL, &xc)) {
            if (xk != kind) break;
            ccol = xc;
            j++;
            continue;
        }
        if (indent_of(s) >= ccol) {
            j++;
            continue;
        }
        /* marked/core.ts parseList: no lazy continuation; an indented line that starts no block stays in the item */
        if (indent_of(s) > 0 && !(indent_of(s) <= ind0 && interrupts_para(L, j, to))) {
            j++;
            continue;
        }
        break;
    }
    return j;
}

static void lex_blocks(const lines_t *L, size_t from, size_t to, toks_t *out) {
    size_t i = from;
    bool any = false, space = false;
    while (i < to) {
        const char *s = L->v[i];
        if (is_blank(s)) {
            if (any) space = true;
            i++;
            continue;
        }
        if (space) {
            tok_t sp = {.t = T_SPACE, .from = i, .to = i};
            vec_push(out, sp);
            space = false;
        }
        tok_t t;
        memset(&t, 0, sizeof t);
        t.from = i;
        char fch;
        size_t flen;
        const char *info;
        int hl;
        if (indent_of(s) >= 4) {
            /* indented code: the paragraph loop has already taken lines attached to its text */
            t.t = T_CODE;
            t.strip = 4;
            size_t j = i, last = i;
            while (j < to && (is_blank(L->v[j]) || indent_of(L->v[j]) >= 4)) {
                if (!is_blank(L->v[j])) last = j;
                j++;
            }
            t.to = last + 1;
            i = t.to;
        } else if (is_fence(s, &fch, &flen, &info)) {
            t.t = T_CODE;
            t.fenced = true;
            t.strip = indent_of(s);
            size_t ll = 0;
            while (info[ll] && info[ll] != ' ' && info[ll] != '\t' && info[ll] != '{' && ll < sizeof t.lang - 1) {
                t.lang[ll] = info[ll];
                ll++;
            }
            t.lang[ll] = 0;
            size_t j = i + 1;
            while (j < to) {
                char c2;
                size_t l2;
                const char *inf2;
                if (is_fence(L->v[j], &c2, &l2, &inf2) && c2 == fch && l2 >= flen && !*inf2) {
                    t.closed = true;
                    break;
                }
                j++;
            }
            t.to = j < to ? j + 1 : j;
            i = t.to;
        } else if ((hl = heading_level(s))) {
            t.t = T_HEADING;
            t.level = hl;
            t.to = ++i;
        } else if (is_hr(s, t.hrc)) {
            t.t = T_HR;
            t.to = ++i;
        } else if (is_quote(s)) {
            t.t = T_QUOTE;
            size_t j = i + 1;
            while (j < to && !is_blank(L->v[j]) &&
                   (is_quote(L->v[j]) || (!interrupts_para(L, j, to) && !is_ref_def(L->v[j]))))
                j++;
            t.to = j;
            i = j;
        } else if (list_item(s, NULL, NULL, NULL, NULL, NULL)) {
            t.t = T_LIST;
            t.to = lex_list(L, i, to);
            i = t.to;
        } else if (is_html_block(s)) {
            /* a comment runs to the line holding "-->", an element to the line holding its closing tag (or the end) */
            t.t = T_HTML;
            size_t j = i + 1;
            const char *x = skip_ws(s);
            if (!strncmp(x, "<!--", 4)) {
                if (!strstr(s, "-->"))
                    while (j < to && !strstr(L->v[j++], "-->")) {}
            } else {
                size_t tl = 0;
                while (isalnum((unsigned char)x[1 + tl]) || x[1 + tl] == '_' || x[1 + tl] == '-') tl++;
                if (!has_close_tag(s, x + 1, tl))
                    while (j < to && !has_close_tag(L->v[j++], x + 1, tl)) {}
            }
            t.to = j;
            i = j;
        } else if (!any && is_ref_def(s)) {
            i++;
            continue;
        } else if (is_table_start(L, i, to)) {
            t.t = T_TABLE;
            size_t j = i + 2;
            while (j < to && !is_blank(L->v[j]) && !interrupts_para(L, j, to)) j++;
            t.to = j;
            i = j;
        } else {
            t.t = T_PARA;
            size_t j = i + 1;
            while (j < to && !is_blank(L->v[j])) {
                /* setext underline: === -> h1, --- -> h2. omp's lexer underlines one line: a paragraph line followed
                 * by an underline ends the paragraph before it and becomes the heading (isBlockStart) */
                if (is_setext_underline(L->v[j])) {
                    if (j == i + 1) {
                        t.t = T_HEADING;
                        t.level = *skip_ws(L->v[j]) == '=' ? 1 : 2;
                        t.setext = true;
                        j++;
                    }
                    break;
                }
                if (j + 1 < to && is_setext_underline(L->v[j + 1])) break;
                if (interrupts_para(L, j, to)) break;
                j++;
            }
            t.to = j;
            i = j;
        }
        vec_push(out, t);
        any = true;
    }
}

/* ---------- rendering ---------- */

static void render_tokens(const lines_t *L, const toks_t *T, int width, const md_ctx_t *c, int depth, rows_t *out);
static void render_table(const lines_t *L, size_t from, size_t to, int width, ttype_t next, const md_ctx_t *c,
                         rows_t *out);

/* paragraph source: lines joined with '\n', trailing whitespace dropped; a hard break ("  " or "\" at line
 * end) becomes "\\\n" so the inline pass drops the next line's indentation. Like marked, a soft-wrapped
 * continuation line keeps its leading whitespace (web_search's "    url" result lines). */
static char *para_source(const lines_t *L, size_t from, size_t to) {
    sb_t src = SB_INIT;
    bool prev_hard = false;
    for (size_t i = from; i < to; i++) {
        const char *s = i == from || prev_hard ? skip_ws(L->v[i]) : L->v[i];
        size_t n = strlen(s);
        bool hard = n >= 2 && s[n - 1] == ' ' && s[n - 2] == ' ';
        size_t bs = 0;
        while (bs < n && s[n - 1 - bs] == '\\') bs++;
        if (bs % 2 == 1) {
            hard = true;
            n--;
        }
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
        sb_appendn(&src, s, n);
        if (i + 1 < to) sb_append(&src, hard ? "\\\n" : "\n");
        prev_hard = hard;
    }
    return sb_detach(&src);
}

/* the source of an html token: its lines joined */
static char *html_source(const lines_t *L, size_t from, size_t to) {
    sb_t src = SB_INIT;
    for (size_t i = from; i < to; i++) {
        if (i > from) sb_putc(&src, '\n');
        sb_append(&src, L->v[i]);
    }
    return sb_detach(&src);
}

static char *inline_str(const char *s, size_t n, const md_ctx_t *c) {
    sb_t b = SB_INIT;
    render_inline(s, n, c, &b, 0);
    return sb_detach(&b);
}

static char *heading_str(const lines_t *L, const tok_t *t, const md_ctx_t *c) {
    const char *s;
    size_t n;
    char *own = NULL;
    if (t->setext) {
        own = para_source(L, t->from, t->to - 1);
        s = own;
        n = strlen(s);
    } else {
        s = skip_ws(skip_ws(L->v[t->from]) + t->level);
        n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
        size_t m = n;
        while (m > 0 && s[m - 1] == '#') m--;
        if (m < n && (m == 0 || s[m - 1] == ' ')) n = m;
        while (n > 0 && s[n - 1] == ' ') n--;
    }
    md_ctx_t hc;
    ctx_extend(&hc, c, heading_seq(c, t->level), "\x1b[1m", t->level == 1 ? "\x1b[4m" : "");
    ctx_exempt(&hc);
    sb_t b = SB_INIT;
    if (t->level >= 3) {
        sb_append(&b, TUI_RESET);
        sb_append(&b, hc.base);
        for (int i = 0; i < t->level; i++) sb_putc(&b, '#');
        sb_putc(&b, ' ');
    }
    render_inline(s, n, &hc, &b, 0);
    free(own);
    return sb_detach(&b);
}

/* fenced/indented code body as rows (highlighted when the language is supported, else verbatim) */
static void code_rows(const lines_t *L, const tok_t *t, const md_ctx_t *c, rows_t *out) {
    size_t bf = t->fenced ? t->from + 1 : t->from;
    size_t bt = t->fenced && t->closed ? t->to - 1 : t->to;
    sb_t code = SB_INIT;
    for (size_t i = bf; i < bt; i++) {
        const char *s = L->v[i];
        size_t k = 0, col = 0;
        while (col < t->strip && (s[k] == ' ' || s[k] == '\t')) {
            col += s[k] == '\t' ? 4 : 1;
            k++;
        }
        if (i > bf) sb_putc(&code, '\n');
        sb_append(&code, s + k);
    }
    strvec_t hl = {0};
    if (bt > bf || !t->closed) tui_highlight_code(sb_cstr(&code), t->lang, c->th, &hl);
    bool literal = c->o->code_indent == 0;
    for (size_t i = 0; i < hl.n; i++) {
        char *line = hl.v[i];
        hl.v[i] = NULL;
        if (!literal) {
            char *x = xasprintf("%*s%s", c->o->code_indent, "", line);
            free(line);
            line = x;
        }
        rows_push(out, line, literal);
    }
    strvec_free(&hl);
    sb_free(&code);
}

static char *code_border(const char *lang, const md_ctx_t *c) {
    char *s = xasprintf("```%s", lang ? lang : "");
    char *r = tui_fgs(c->th, TUI_TC_MD_CODE_BLOCK_BORDER, s);
    free(s);
    return r;
}

static char *hr_str(int width, const char *src, const md_ctx_t *c) {
    const char *def = tui_sym("md.hrChar");
    if (!def || !*def) def = "\xe2\x94\x80";
    bool ascii = !strcmp(def, "-");
    const char *g = def;
    if (src && *src) {
        if (!strcmp(src, "=")) g = "=";
        else if (!strcmp(src, "\xe2\x95\x90")) g = ascii ? "=" : src;
        else if (!strcmp(src, "\xe2\x94\x81") || !strcmp(src, "\xe2\x94\x80") || !strcmp(src, "\xe2\x80\x93") ||
                 !strcmp(src, "\xe2\x80\x94"))
            g = ascii ? "-" : src;
    }
    sb_t b = SB_INIT;
    sb_append(&b, tui_fg_seq(c->th, TUI_TC_MD_HR));
    sb_repeat(&b, g, MIN(width, 80));
    sb_append(&b, "\x1b[39m");
    return sb_detach(&b);
}

/* blockquote: inner tokens at width-2, trailing blank rows dropped, each styled row wrapped and prefixed with
 * the quote border; literal code rows pass through unbordered */
/* the quote style: quote color and italic in place of the default prose style */
static void quote_ctx(md_ctx_t *qc, const md_ctx_t *c) {
    ctx_extend(qc, c, tui_fg_seq(c->th, TUI_TC_MD_QUOTE), "\x1b[3m", "");
    qc->deflt[0] = 0;
    qc->deflt_exempt[0] = 0;
    qc->run_forced = false;
}

static void quote_border(const rows_t *inner, int width, const md_ctx_t *c, const md_ctx_t *qc, rows_t *out);

static void render_quote(const lines_t *L, const tok_t *t, int width, const md_ctx_t *c, int depth, rows_t *out) {
    strvec_t src = {0};
    for (size_t i = t->from; i < t->to; i++) {
        const char *s = skip_ws(L->v[i]);
        if (*s == '>') {
            s++;
            if (*s == ' ') s++;
            else if (*s == '\t') s++;
        }
        strvec_push(&src, s);
    }
    md_ctx_t qc;
    quote_ctx(&qc, c);
    int iw = MAX(1, width - 2);
    rows_t inner = {0};
    lines_t IL = {(const char *const *)src.v, src.n};
    if (depth + 1 < MD_MAX_DEPTH) {
        toks_t T = {0};
        lex_blocks(&IL, 0, IL.n, &T);
        render_tokens(&IL, &T, iw, &qc, depth + 1, &inner);
        free(T.v);
    } else {
        for (size_t i = 0; i < src.n; i++) rows_push(&inner, xstrdup(src.v[i]), false);
    }
    while (inner.n && !inner.v[inner.n - 1].text[0]) free(vec_pop(&inner).text);
    quote_border(&inner, width, c, &qc, out);
    rows_free(&inner);
    strvec_free(&src);
}

/* #applyQuoteBorder: quote-styled rows wrapped to width - 2 behind the border; literal code rows pass through */
static void quote_border(const rows_t *inner, int width, const md_ctx_t *c, const md_ctx_t *qc, rows_t *out) {
    int iw = MAX(1, width - 2);
    const char *qb = tui_sym("md.quoteBorder");
    char *border_txt = xasprintf("%s ", qb && *qb ? qb : "\xe2\x96\x8f");
    char *border = tui_fgs(c->th, TUI_TC_MD_QUOTE_BORDER, border_txt);
    free(border_txt);
    for (size_t i = 0; i < inner->n; i++) {
        strvec_t w = {0};
        if (inner->v[i].literal) {
            tui_wrap_text_with_ansi(inner->v[i].text, iw, &w);
            if (!w.n) strvec_push(&w, "");
            for (size_t k = 0; k < w.n; k++) {
                rows_push(out, w.v[k], true);
                w.v[k] = NULL;
            }
        } else {
            char *styled = xasprintf("%s%s%s", qc->base, inner->v[i].text, TUI_RESET);
            tui_wrap_text_with_ansi(styled, iw, &w);
            free(styled);
            for (size_t k = 0; k < w.n; k++) rows_push(out, xasprintf("%s%s" TUI_RESET, border, w.v[k]), false);
        }
        strvec_free(&w);
    }
    free(border);
}

static void render_list(const lines_t *L, const tok_t *t, int depth, int width, const md_ctx_t *c, rows_t *out);

/* rows of one list item, without the item's bullet/indent (render_list adds those); nested list rows come
 * back tagged `nested` and already indented */
static void render_list_item(const lines_t *IL, int depth, int width, int content_width, const md_ctx_t *c,
                             rows_t *out) {
    toks_t T = {0};
    lex_blocks(IL, 0, IL->n, &T);
    for (size_t k = 0; k < T.n; k++) {
        const tok_t *t = &T.v[k];
        switch (t->t) {
        case T_SPACE: break;
        case T_LIST: {
            rows_t nested = {0};
            if (depth + 1 < MD_MAX_DEPTH) render_list(IL, t, depth + 1, width, c, &nested);
            for (size_t r = 0; r < nested.n; r++) {
                nested.v[r].nested = true;
                vec_push(out, nested.v[r]);
            }
            free(nested.v);
            break;
        }
        case T_CODE:
            rows_push(out, code_border(t->lang, c), false);
            code_rows(IL, t, c, out);
            rows_push(out, code_border("", c), false);
            break;
        case T_TABLE: render_table(IL, t->from, t->to, content_width, k + 1 < T.n ? T.v[k + 1].t : T_NONE, c, out); break;
        case T_HEADING: rows_push(out, heading_str(IL, t, c), false); break;
        case T_HR: rows_push(out, hr_str(content_width, t->hrc, c), false); break;
        case T_QUOTE: render_quote(IL, t, content_width, c, depth + 1, out); break;
        case T_HTML: { /* an html token in a list item renders as inline html: normalized, no rules or quotes */
            char *src = html_source(IL, t->from, t->to);
            sb_t o = SB_INIT;
            html_state_t st = {0};
            html_normalize(src, strlen(src), &st, NULL, &o);
            if (t->to < IL->n && !is_blank(IL->v[t->to])) sb_putc(&o, '\n'); /* the token's raw keeps its newline */
            if (o.len) {
                sb_t r = SB_INIT;
                sb_append(&r, c->base);
                sb_append(&r, c->deflt);
                sb_appendn(&r, o.s, o.len);
                if (c->base[0] || c->deflt[0]) sb_append(&r, TUI_RESET);
                rows_push(out, sb_detach(&r), false);
            }
            free(st.v);
            sb_free(&o);
            free(src);
            break;
        }
        default: {
            char *src = para_source(IL, t->from, t->to);
            rows_push(out, inline_str(src, strlen(src), c), false);
            free(src);
        }
        }
    }
    free(T.v);
}

/* strip the task-list checkbox ("[ ] " / "[x] ") from item text; *task: 0 none, 1 open, 2 checked */
static const char *skip_task(const char *s, int *task) {
    if (s[0] == '[' && (s[1] == ' ' || s[1] == 'x' || s[1] == 'X') && s[2] == ']' && (s[3] == ' ' || s[3] == '\t')) {
        *task = s[1] == ' ' ? 1 : 2;
        return skip_ws(s + 3);
    }
    *task = 0;
    return s;
}

static void push_item_row(rows_t *out, const row_t *line, const char *first, const char *cont, int width) {
    if (line->literal) {
        strvec_t w = {0};
        tui_wrap_text_with_ansi(line->text, MAX(1, width), &w);
        if (!w.n) strvec_push(&w, "");
        for (size_t k = 0; k < w.n; k++) {
            rows_push(out, w.v[k], true);
            w.v[k] = NULL;
        }
        strvec_free(&w);
        return;
    }
    int pw = tui_visible_width(first);
    strvec_t w = {0};
    if (pw >= width) {
        rows_push(out, tui_truncate_to_width(first, width, ""), false);
        tui_wrap_text_with_ansi(line->text, MAX(1, width), &w);
        for (size_t k = 0; k < w.n; k++) {
            rows_push(out, w.v[k], false);
            w.v[k] = NULL;
        }
        strvec_free(&w);
        return;
    }
    tui_wrap_text_with_ansi(line->text, width - pw, &w);
    if (!w.n) rows_push(out, xstrdup(first), false);
    for (size_t k = 0; k < w.n; k++) rows_push(out, xasprintf("%s%s", k ? cont : first, w.v[k]), false);
    strvec_free(&w);
}

/* a list item's body: the first line after the marker, then lines up to the next sibling marker, de-indented to
 * the item's content column; returns the line after the item */
static size_t list_item_body(const lines_t *L, const tok_t *t, size_t i, char kind, size_t co, size_t cc, strvec_t *body,
                             int *task) {
    strvec_push(body, skip_task(L->v[i] + co, task));
    size_t j = i + 1;
    while (j < t->to) {
        const char *s = L->v[j];
        size_t ji, jc;
        char jk;
        if (!is_blank(s) && indent_of(s) < cc && list_item(s, &ji, &jk, NULL, NULL, &jc) && jk == kind) break;
        size_t k = 0, col = 0;
        while ((s[k] == ' ' || s[k] == '\t') && col < cc) {
            col += s[k] == '\t' ? 4 : 1;
            k++;
        }
        strvec_push(body, s + k);
        j++;
    }
    while (body->n > 1 && is_blank(body->v[body->n - 1])) free(vec_pop(body));
    return j;
}

static void render_list(const lines_t *L, const tok_t *t, int depth, int width, const md_ctx_t *c, rows_t *out) {
    char kind;
    long start;
    size_t ccol, coff, ind;
    list_item(L->v[t->from], &ind, &kind, &start, &coff, &ccol);
    bool ordered = kind == '.' || kind == ')';
    char indent[2 * MD_MAX_DEPTH + 1];
    int il = MIN(2 * depth, 2 * MD_MAX_DEPTH);
    memset(indent, ' ', (size_t)il);
    indent[il] = 0;
    long idx = 0;
    size_t i = t->from;
    while (i < t->to) {
        size_t co, cc, ii;
        char kk;
        if (!list_item(L->v[i], &ii, &kk, NULL, &co, &cc) || kk != kind) {
            i++;
            continue;
        }
        strvec_t body = {0};
        int task = 0;
        size_t j = list_item_body(L, t, i, kind, co, cc, &body, &task);
        char bullet[24];
        if (ordered) snprintf(bullet, sizeof bullet, "%ld. ", start + idx);
        else snprintf(bullet, sizeof bullet, "- ");
        idx++;
        /* a task item draws its checkbox after the bare marker, in the item's text style, so wrapped rows hang
         * under the task text */
        int bw = (int)strlen(bullet) + (task ? 4 : 0);
        char *first;
        if (task) {
            bool styled = c->base[0] || c->deflt[0];
            first = xasprintf("%s%s%s\x1b[39m%s%s[%c] %s", indent, tui_fg_seq(c->th, TUI_TC_MD_LIST_BULLET), bullet,
                              c->base, c->deflt, task == 2 ? 'x' : ' ', styled ? TUI_RESET : "");
        } else {
            first = xasprintf("%s%s%s\x1b[39m", indent, tui_fg_seq(c->th, TUI_TC_MD_LIST_BULLET), bullet);
        }
        char *cont = xasprintf("%s%*s", indent, bw, "");
        rows_t item = {0};
        lines_t BL = {(const char *const *)body.v, body.n};
        render_list_item(&BL, depth, width, MAX(1, width - tui_visible_width(cont)), c, &item);
        if (!item.n) rows_push(out, xstrdup(first), false);
        for (size_t q = 0; q < item.n; q++) {
            if (item.v[q].nested) {
                vec_push(out, item.v[q]);
                item.v[q].text = NULL;
            } else {
                push_item_row(out, &item.v[q], q == 0 ? first : cont, cont, width);
            }
        }
        rows_free(&item);
        free(first);
        free(cont);
        strvec_free(&body);
        i = j;
    }
}

/* ---------- tables (upstream #renderTable) ---------- */

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

static int longest_word(const char *s, int cap) {
    char *plain = tui_strip_ansi(s);
    int best = 0;
    for (char *p = plain; *p;) {
        while (*p && isspace((unsigned char)*p)) p++;
        char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (e > p) best = MAX(best, tui_visible_width_n(p, (size_t)(e - p)));
        p = e;
    }
    free(plain);
    return cap > 0 ? MIN(best, cap) : best;
}

static int max_line_width(const char *s) {
    int best = 0;
    const char *p = s;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        best = MAX(best, tui_visible_width_n(p, n));
        if (!nl) break;
        p = nl + 1;
    }
    return best;
}

static void wrap_cell(const char *text, int w, strvec_t *out) {
    strvec_t lines = {0};
    tui_wrap_text_with_ansi(text, MAX(1, w), &lines);
    while (lines.n > 1 && !lines.v[lines.n - 1][0]) free(vec_pop(&lines));
    for (size_t i = 0; i < lines.n; i++) {
        char *fit = tui_visible_width(lines.v[i]) > MAX(1, w) ? tui_truncate_to_width(lines.v[i], MAX(1, w), NULL)
                                                               : xstrdup(lines.v[i]);
        strvec_push_own(out, xasprintf("%s\x1b[22m\x1b[23m\x1b[39m", fit));
        free(fit);
    }
    strvec_free(&lines);
}

static char *align_cell(const char *text, int w, char align) {
    int gap = MAX(0, w - tui_visible_width(text));
    if (!gap) return xstrdup(text);
    if (align == 'r') return xasprintf("%*s%s", gap, "", text);
    if (align == 'c') return xasprintf("%*s%s%*s", gap / 2, "", text, gap - gap / 2, "");
    return xasprintf("%s%*s", text, gap, "");
}

static void table_rule(sb_t *b, const int *w, int nc, const char *l, const char *m, const char *r) {
    const char *h = tui_sym("boxSharp.horizontal");
    sb_append(b, l);
    for (int i = 0; i < nc; i++) {
        sb_repeat(b, h, w[i] + 2);
        sb_append(b, i + 1 < nc ? m : r);
    }
}

static void render_table(const lines_t *L, size_t from, size_t to, int width, ttype_t next, const md_ctx_t *c,
                         rows_t *out) {
    strvec_t header = {0}, seps = {0};
    split_cells(L->v[from], &header);
    split_cells(L->v[from + 1], &seps);
    int nc = (int)MIN(header.n, MD_MAX_COLS);
    if (nc == 0) goto done;
    int avail_cells = width - (3 * nc + 1);
    if (avail_cells < nc) {
        /* too narrow: the raw markdown, wrapped */
        for (size_t i = from; i < to; i++) {
            strvec_t w = {0};
            tui_wrap_text_with_ansi(L->v[i], MAX(1, width), &w);
            for (size_t k = 0; k < w.n; k++) {
                rows_push(out, w.v[k], false);
                w.v[k] = NULL;
            }
            strvec_free(&w);
        }
        if (next != T_NONE && next != T_SPACE) rows_push(out, xstrdup(""), false);
        goto done;
    }
    {
        char align[MD_MAX_COLS];
        for (int i = 0; i < nc; i++) {
            const char *sp = i < (int)seps.n ? seps.v[i] : "";
            size_t sl = strlen(sp);
            bool l = sl && sp[0] == ':', r = sl && sp[sl - 1] == ':';
            align[i] = l && r ? 'c' : r ? 'r' : 'l';
        }
        size_t nrows = 1 + (to - from - 2);
        char ***cells = xcalloc(nrows, sizeof(char **));
        for (size_t r = 0; r < nrows; r++) {
            strvec_t raw = {0};
            if (r == 0)
                for (size_t i = 0; i < header.n; i++) strvec_push(&raw, header.v[i]);
            else split_cells(L->v[from + 1 + r], &raw);
            cells[r] = xcalloc((size_t)nc, sizeof(char *));
            for (int i = 0; i < nc; i++) {
                const char *t = i < (int)raw.n ? raw.v[i] : "";
                md_ctx_t cc = *c;
                if (r == 0 && g_colorful) ctx_exempt(&cc); /* theme.tableHeader owns the cell's color */
                cells[r][i] = inline_str(t, strlen(t), &cc);
            }
            strvec_free(&raw);
        }
        int nat[MD_MAX_COLS], minw[MD_MAX_COLS], w[MD_MAX_COLS];
        for (int i = 0; i < nc; i++) {
            nat[i] = 0;
            minw[i] = 1;
            for (size_t r = 0; r < nrows; r++) {
                nat[i] = MAX(nat[i], max_line_width(cells[r][i]));
                minw[i] = MAX(minw[i], longest_word(cells[r][i], 30));
            }
        }
        int mincol[MD_MAX_COLS];
        int min_sum = 0;
        for (int i = 0; i < nc; i++) min_sum += (mincol[i] = minw[i]);
        if (min_sum > avail_cells) {
            int remaining = avail_cells - nc;
            for (int i = 0; i < nc; i++) mincol[i] = 1;
            if (remaining > 0) {
                long tw = 0;
                for (int i = 0; i < nc; i++) tw += MAX(0, minw[i] - 1);
                int alloc = 0;
                for (int i = 0; i < nc; i++) {
                    int g = tw > 0 ? (int)((double)MAX(0, minw[i] - 1) / (double)tw * remaining) : 0;
                    mincol[i] += g;
                    alloc += g;
                }
                int left = remaining - alloc;
                for (int i = 0; left > 0 && i < nc; i++, left--) mincol[i]++;
            }
            min_sum = 0;
            for (int i = 0; i < nc; i++) min_sum += mincol[i];
        }
        int total_nat = 3 * nc + 1;
        for (int i = 0; i < nc; i++) total_nat += nat[i];
        if (total_nat <= width) {
            for (int i = 0; i < nc; i++) w[i] = MAX(nat[i], mincol[i]);
        } else {
            long grow_pot = 0;
            for (int i = 0; i < nc; i++) grow_pot += MAX(0, nat[i] - mincol[i]);
            int extra = MAX(0, avail_cells - min_sum);
            int alloc = 0;
            for (int i = 0; i < nc; i++) {
                int g = grow_pot > 0 ? (int)((double)MAX(0, nat[i] - mincol[i]) / (double)grow_pot * extra) : 0;
                w[i] = mincol[i] + g;
                alloc += w[i];
            }
            int remaining = avail_cells - alloc;
            while (remaining > 0) {
                bool grew = false;
                for (int i = 0; i < nc && remaining > 0; i++)
                    if (w[i] < nat[i]) {
                        w[i]++;
                        remaining--;
                        grew = true;
                    }
                if (!grew) break;
            }
        }
        const char *v = tui_sym("boxSharp.vertical");
        sb_t b = SB_INIT;
        table_rule(&b, w, nc, tui_sym("boxSharp.topLeft"), tui_sym("boxSharp.teeDown"), tui_sym("boxSharp.topRight"));
        rows_push(out, sb_detach(&b), false);
        sb_t sep = SB_INIT;
        table_rule(&sep, w, nc, tui_sym("boxSharp.teeRight"), tui_sym("boxSharp.cross"), tui_sym("boxSharp.teeLeft"));
        for (size_t r = 0; r < nrows; r++) {
            strvec_t wrapped[MD_MAX_COLS];
            size_t h = 1;
            for (int i = 0; i < nc; i++) {
                memset(&wrapped[i], 0, sizeof wrapped[i]);
                wrap_cell(cells[r][i], w[i], &wrapped[i]);
                h = MAX(h, wrapped[i].n);
            }
            for (size_t li = 0; li < h; li++) {
                sb_append(&b, v);
                sb_putc(&b, ' ');
                for (int i = 0; i < nc; i++) {
                    if (i) {
                        sb_putc(&b, ' ');
                        sb_append(&b, v);
                        sb_putc(&b, ' ');
                    }
                    char *a = align_cell(li < wrapped[i].n ? wrapped[i].v[li] : "", w[i], align[i]);
                    if (r == 0) {
                        /* theme.tableHeader: ansiFg("yellow", bold(cell)) when colorful, else theme.bold(); the
                         * styles re-open after every inner reset */
                        const char *yl = g_colorful ? tui_ansi_fg_seq(c->th, TUI_ANSI_YELLOW) : "";
                        sb_append(&b, yl);
                        sb_append(&b, "\x1b[1m");
                        for (const char *q = a; *q;) {
                            if (!strncmp(q, "\x1b[22m", 5) || !strncmp(q, "\x1b[0m", 4)) {
                                size_t l = q[2] == '2' ? 5 : 4;
                                sb_appendn(&b, q, l);
                                if (l == 4) sb_append(&b, yl);
                                sb_append(&b, "\x1b[1m");
                                q += l;
                                continue;
                            }
                            if (*yl && !strncmp(q, "\x1b[39m", 5)) {
                                sb_appendn(&b, q, 5);
                                sb_append(&b, yl);
                                q += 5;
                                continue;
                            }
                            sb_putc(&b, *q++);
                        }
                        sb_append(&b, "\x1b[22m");
                        if (*yl) sb_append(&b, "\x1b[39m");
                    } else sb_append(&b, a);
                    free(a);
                }
                sb_putc(&b, ' ');
                sb_append(&b, v);
                rows_push(out, sb_detach(&b), false);
            }
            for (int i = 0; i < nc; i++) strvec_free(&wrapped[i]);
            if (r + 1 < nrows) rows_push(out, xstrdup(sb_cstr(&sep)), false);
        }
        sb_free(&sep);
        table_rule(&b, w, nc, tui_sym("boxSharp.bottomLeft"), tui_sym("boxSharp.teeUp"),
                   tui_sym("boxSharp.bottomRight"));
        rows_push(out, sb_detach(&b), false);
        if (next != T_NONE && next != T_SPACE) rows_push(out, xstrdup(""), false);
        for (size_t r = 0; r < nrows; r++) {
            for (int i = 0; i < nc; i++) free(cells[r][i]);
            free(cells[r]);
        }
        free(cells);
    }
done:
    strvec_free(&header);
    strvec_free(&seps);
}

/* #renderHtmlBlock's flushText: a normalized run of block HTML as default-styled lines */
static void html_block_text(const char *s, size_t n, html_state_t *st, const md_ctx_t *c, rows_t *out) {
    char after[sizeof c->base + sizeof c->deflt + 8];
    snprintf(after, sizeof after, "%s%s", c->base, c->deflt);
    html_hook_t hook = {c, after};
    sb_t o = SB_INIT;
    html_normalize(s, n, st, &hook, &o);
    if (!js_blank(o.s, o.len)) {
        strvec_t lines = str_split_lines(o.s, o.len); /* splitTerminalLines */
        while (lines.n > 1 && !lines.v[lines.n - 1][0]) free(vec_pop(&lines));
        for (size_t i = 0; i < lines.n; i++) {
            const char *l = lines.v[i];
            size_t ln = js_trim_end(l, strlen(l));
            if (js_blank(l, ln)) rows_push(out, xstrdup(""), false);
            else rows_push(out, xasprintf(TUI_RESET "%s%s%.*s" TUI_RESET, c->base, c->deflt, (int)ln, l), false);
        }
        strvec_free(&lines);
    }
    sb_free(&o);
}

/* #renderHtmlBlockquote: the inner HTML of a <blockquote>, normalized, behind the quote border */
static void html_blockquote(const char *s, size_t n, int width, const md_ctx_t *c, rows_t *out) {
    md_ctx_t qc;
    quote_ctx(&qc, c);
    html_hook_t hook = {&qc, qc.base};
    html_state_t st = {0};
    sb_t o = SB_INIT;
    html_normalize(s, n, &st, &hook, &o);
    free(st.v);
    strvec_t lines = str_split_lines(o.s ? o.s : "", o.len);
    while (lines.n > 1 && !lines.v[lines.n - 1][0]) free(vec_pop(&lines));
    rows_t inner = {0};
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        rows_push(&inner, xstrndup(l, js_trim_end(l, strlen(l))), false);
    }
    while (inner.n && !inner.v[inner.n - 1].text[0]) free(vec_pop(&inner).text);
    quote_border(&inner, width, c, &qc, out);
    rows_free(&inner);
    strvec_free(&lines);
    sb_free(&o);
}

/* #renderHtmlBlock: a standalone <hr> becomes a rule and a balanced <blockquote>..</blockquote> a quote
 * (BLOCK_HTML_REGEX /<hr\b[^>]*\/?>|<blockquote\b[^>]*>([\s\S]*?)<\/blockquote>/gi); the HTML between them is
 * normalized to text with one list state */
static void render_html(const lines_t *L, const tok_t *t, int width, const md_ctx_t *c, rows_t *out) {
    char *s = html_source(L, t->from, t->to);
    size_t n = strlen(s), last = 0;
    html_state_t st = {0};
    bool no_gt = false, no_close = false; /* a failed search fails from every later start too */
    for (size_t p = 0; p < n; p++) {
        if (s[p] != '<' || no_gt) continue;
        size_t end = 0, in0 = 0, in1 = 0;
        bool quote = false;
        if (!strncasecmp(s + p, "<hr", 3) && !(isalnum((unsigned char)s[p + 3]) || s[p + 3] == '_')) {
            const char *gt = strchr(s + p + 3, '>');
            if (gt) end = (size_t)(gt - s) + 1;
            else no_gt = true;
        } else if (!no_close && !strncasecmp(s + p, "<blockquote", 11) &&
                   !(isalnum((unsigned char)s[p + 11]) || s[p + 11] == '_')) {
            const char *gt = strchr(s + p + 11, '>');
            const char *cl = gt ? strcasestr(gt + 1, "</blockquote>") : NULL;
            if (!gt) no_gt = true;
            else if (!cl) no_close = true;
            if (cl) {
                quote = true;
                in0 = (size_t)(gt - s) + 1;
                in1 = (size_t)(cl - s);
                end = in1 + 13;
            }
        }
        if (!end) continue;
        html_block_text(s + last, p - last, &st, c, out);
        if (quote) {
            rows_t q = {0};
            html_blockquote(s + in0, in1 - in0, width, c, &q);
            for (size_t r = 0; r < q.n; r++) q.v[r].nested = true; /* wrapped already */
            for (size_t r = 0; r < q.n; r++) vec_push(out, q.v[r]);
            free(q.v);
        } else {
            rows_push(out, hr_str(width, NULL, c), false);
        }
        last = end;
        p = end - 1;
    }
    html_block_text(s + last, n - last, &st, c, out);
    free(st.v);
    free(s);
}

static void render_tokens(const lines_t *L, const toks_t *T, int width, const md_ctx_t *c, int depth, rows_t *out) {
    for (size_t k = 0; k < T->n; k++) {
        const tok_t *t = &T->v[k];
        ttype_t next = k + 1 < T->n ? T->v[k + 1].t : T_NONE;
        bool gap = next != T_NONE && next != T_SPACE;
        switch (t->t) {
        case T_SPACE: rows_push(out, xstrdup(""), false); break;
        case T_HEADING:
            rows_push(out, heading_str(L, t, c), false);
            if (gap) rows_push(out, xstrdup(""), false);
            break;
        case T_PARA: {
            char *src = para_source(L, t->from, t->to);
            rows_push(out, inline_str(src, strlen(src), c), false);
            free(src);
            if (gap && next != T_LIST) rows_push(out, xstrdup(""), false);
            break;
        }
        case T_CODE:
            rows_push(out, code_border(t->lang, c), false);
            code_rows(L, t, c, out);
            rows_push(out, code_border("", c), false);
            if (gap) rows_push(out, xstrdup(""), false);
            break;
        case T_LIST: {
            rows_t lr = {0};
            render_list(L, t, 0, width, c, &lr);
            for (size_t r = 0; r < lr.n; r++) lr.v[r].nested = true; /* already wrapped */
            for (size_t r = 0; r < lr.n; r++) vec_push(out, lr.v[r]);
            free(lr.v);
            break;
        }
        case T_TABLE: {
            rows_t tr = {0};
            render_table(L, t->from, t->to, width, next, c, &tr);
            for (size_t r = 0; r < tr.n; r++) tr.v[r].nested = true; /* fitted to width already */
            for (size_t r = 0; r < tr.n; r++) vec_push(out, tr.v[r]);
            free(tr.v);
            break;
        }
        case T_QUOTE: {
            rows_t q = {0};
            render_quote(L, t, width, c, depth, &q);
            for (size_t r = 0; r < q.n; r++) q.v[r].nested = true; /* wrapped already */
            for (size_t r = 0; r < q.n; r++) vec_push(out, q.v[r]);
            free(q.v);
            if (gap) rows_push(out, xstrdup(""), false);
            break;
        }
        case T_HR:
            rows_push(out, hr_str(width, t->hrc, c), false);
            if (gap) rows_push(out, xstrdup(""), false);
            break;
        case T_HTML: render_html(L, t, width, c, out); break;
        case T_NONE: break;
        }
    }
}

static void init_ctx(md_ctx_t *c, const tui_md_opts_t *o) {
    memset(c, 0, sizeof *c);
    c->o = o;
    c->th = o->theme ? o->theme : tui_theme();
    sb_t d = SB_INIT;
    if (o->text_sgr) sb_append(&d, o->text_sgr);
    else if (o->text_slot >= 0 && strcmp(tui_fg_seq(c->th, (tui_tc_t)o->text_slot), "\x1b[39m") != 0)
        sb_append(&d, tui_fg_seq(c->th, (tui_tc_t)o->text_slot)); /* an empty theme color styles nothing: role colors stay */
    bool colored = d.len > 0;
    if (o->italic) sb_append(&d, "\x1b[3m");
    snprintf(c->deflt, sizeof c->deflt, "%s", sb_cstr(&d));
    sb_free(&d);
    if (o->prose_fg) snprintf(c->deflt_exempt, sizeof c->deflt_exempt, "%s", o->italic ? "\x1b[3m" : "");
    else memcpy(c->deflt_exempt, c->deflt, sizeof c->deflt);
    c->run_forced = colored && !o->prose_fg;
}

/* re-open the line background after every reset that would clear it (upstream bgFill) */
static char *apply_bg(const char *line, int width, const char *bg) {
    sb_t b = SB_INIT;
    sb_append(&b, bg);
    for (const char *p = line; *p;) {
        if (!strncmp(p, "\x1b[0m", 4) || !strncmp(p, "\x1b[49m", 5)) {
            size_t l = p[2] == '0' ? 4 : 5;
            sb_appendn(&b, p, l);
            sb_append(&b, bg);
            p += l;
            continue;
        }
        sb_putc(&b, *p++);
    }
    tui_sb_spaces(&b, width - tui_visible_width(line));
    sb_append(&b, "\x1b[49m");
    return sb_detach(&b);
}

bool tui_md_ends_in_paragraph(const char *text) {
    char *clean = tui_replace_tabs(text ? text : "");
    strvec_t lines = str_split_lines(clean, strlen(clean));
    lines_t L = {(const char *const *)lines.v, lines.n};
    toks_t T = {0};
    lex_blocks(&L, 0, L.n, &T);
    bool r = false;
    for (size_t i = T.n; i-- > 0;)
        if (T.v[i].t != T_SPACE) {
            r = T.v[i].t == T_PARA;
            break;
        }
    free(T.v);
    strvec_free(&lines);
    free(clean);
    return r;
}

void tui_md_render(const char *text, int width, const tui_md_opts_t *opts, strvec_t *out) {
    tui_md_opts_t def;
    if (!opts) {
        tui_md_opts_default(&def);
        opts = &def;
    }
    if (width < 1) width = 1;
    int pad = MAX(0, opts->padding_x);
    int cw = MAX(1, width - 2 * pad);
    md_ctx_t c;
    init_ctx(&c, opts);
    char *clean = tui_replace_tabs(text ? text : "");
    strvec_t lines = str_split_lines(clean, strlen(clean));
    lines_t L = {(const char *const *)lines.v, lines.n};
    toks_t T = {0};
    lex_blocks(&L, 0, L.n, &T);
    rows_t rows = {0};
    render_tokens(&L, &T, cw, &c, 0, &rows);
    free(T.v);
    const char *bg = opts->bg_slot >= 0 ? tui_bg_seq(c.th, (tui_tc_t)opts->bg_slot) : NULL;
    for (size_t i = 0; i < rows.n; i++) {
        strvec_t w = {0};
        if (rows.v[i].nested) strvec_push(&w, rows.v[i].text);
        else tui_wrap_text_with_ansi(rows.v[i].text, cw, &w);
        for (size_t k = 0; k < w.n; k++) {
            char *line = w.v[k];
            if (tui_visible_width(line) > (rows.v[i].literal ? width : cw)) {
                char *t = tui_truncate_to_width(line, rows.v[i].literal ? width : cw, "");
                free(line);
                line = t;
            }
            if (!rows.v[i].literal && pad) {
                char *x = xasprintf("%*s%s", pad, "", line);
                free(line);
                line = x;
            }
            if (bg) {
                char *x = apply_bg(line, width, bg);
                free(line);
                line = x;
            }
            strvec_push_own(out, line);
            w.v[k] = NULL;
        }
        strvec_free(&w);
    }
    rows_free(&rows);
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
    c.html_fresh = true;
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
    if (!m->text || !*skip_ws(m->text)) return;
    if (!(m->cache_text && m->cache_width == width && !strcmp(m->cache_text, m->text))) {
        md_invalidate(cp);
        tui_md_opts_t o = m->opts;
        o.padding_x = m->padding_x;
        const char *bg = o.bg_slot >= 0 ? tui_bg_seq(o.theme ? o.theme : tui_theme(), (tui_tc_t)o.bg_slot) : NULL;
        for (int i = 0; i < m->padding_y; i++)
            strvec_push_own(&m->cache, bg ? apply_bg("", width, bg) : xstrdup(""));
        tui_md_render(m->text, width, &o, &m->cache);
        for (int i = 0; i < m->padding_y; i++)
            strvec_push_own(&m->cache, bg ? apply_bg("", width, bg) : xstrdup(""));
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

/* ---------- copy-targets.ts extractBlocks over the renderer's own tokens ---------- */

static void extract_tokens(const lines_t *L, const toks_t *T, int depth, tui_md_block_fn fn, void *ud) {
    for (size_t ti = 0; ti < T->n; ti++) {
        const tok_t *t = &T->v[ti];
        if (t->t == T_CODE && t->fenced) {
            const char *info = "";
            is_fence(L->v[t->from], NULL, NULL, &info);
            char *lang = str_trim(info);
            sb_t b = SB_INIT;
            size_t end = t->closed ? t->to - 1 : t->to;
            for (size_t i = t->from + 1; i < end; i++) {
                const char *x = L->v[i];
                size_t k = 0; /* indentCodeCompensation: up to the fence's indent */
                while (k < t->strip && x[k] == ' ') k++;
                if (i > t->from + 1) sb_putc(&b, '\n');
                sb_append(&b, x + k);
            }
            fn(ud, true, lang, sb_detach(&b));
            free(lang);
        } else if (t->t == T_QUOTE) {
            sb_t b = SB_INIT;
            for (size_t i = t->from; i < t->to; i++) {
                const char *x = L->v[i];
                if (is_quote(x)) {
                    x = skip_ws(x) + 1;
                    if (*x == ' ' || *x == '\t') x++;
                }
                if (i > t->from) sb_putc(&b, '\n');
                sb_append(&b, x);
            }
            while (b.len && b.s[b.len - 1] == '\n') b.s[--b.len] = 0;
            fn(ud, false, NULL, sb_detach(&b));
        } else if (t->t == T_LIST && depth + 1 < MD_MAX_DEPTH) {
            char kind;
            size_t ccol, coff, ind;
            list_item(L->v[t->from], &ind, &kind, NULL, &coff, &ccol);
            size_t i = t->from;
            while (i < t->to) {
                size_t co, cc, ii;
                char kk;
                if (!list_item(L->v[i], &ii, &kk, NULL, &co, &cc) || kk != kind) {
                    i++;
                    continue;
                }
                strvec_t body = {0};
                int task;
                i = list_item_body(L, t, i, kind, co, cc, &body, &task);
                lines_t BL = {(const char *const *)body.v, body.n};
                toks_t BT = {0};
                lex_blocks(&BL, 0, BL.n, &BT);
                extract_tokens(&BL, &BT, depth + 1, fn, ud);
                vec_free(&BT);
                strvec_free(&body);
            }
        }
    }
}

void tui_md_extract_blocks(const char *text, tui_md_block_fn fn, void *ud) {
    strvec_t lines = {0};
    const char *p = text ? text : "";
    for (;;) {
        const char *nl = strchr(p, '\n');
        strvec_push_own(&lines, nl ? xstrndup(p, (size_t)(nl - p)) : xstrdup(p));
        if (!nl) break;
        p = nl + 1;
    }
    lines_t L = {(const char *const *)lines.v, lines.n};
    toks_t T = {0};
    lex_blocks(&L, 0, L.n, &T);
    extract_tokens(&L, &T, 0, fn, ud);
    vec_free(&T);
    strvec_free(&lines);
}

/* ---------- holdPendingBullet ---------- */

/* whether the last block of L (recursing into the last list item / quote child) is a setext heading underlined by
 * the source's last line */
static bool ends_in_setext(const lines_t *L, int depth) {
    if (depth >= MD_MAX_DEPTH || !L->n) return false;
    toks_t T = {0};
    lex_blocks(L, 0, L->n, &T);
    bool r = false;
    const tok_t *t = T.n ? &T.v[T.n - 1] : NULL;
    if (t && t->to == L->n) {
        if (t->t == T_HEADING) r = t->setext;
        else if (t->t == T_QUOTE) {
            strvec_t src = {0};
            for (size_t i = t->from; i < t->to; i++) {
                const char *x = L->v[i];
                if (is_quote(x)) {
                    x = skip_ws(x) + 1;
                    if (*x == ' ' || *x == '\t') x++;
                }
                strvec_push(&src, x);
            }
            lines_t QL = {(const char *const *)src.v, src.n};
            r = ends_in_setext(&QL, depth + 1);
            strvec_free(&src);
        } else if (t->t == T_LIST) {
            char kind;
            list_item(L->v[t->from], NULL, &kind, NULL, NULL, NULL);
            size_t i = t->from;
            while (i < t->to) {
                size_t co, cc, ii;
                char kk;
                if (!list_item(L->v[i], &ii, &kk, NULL, &co, &cc) || kk != kind) {
                    i++;
                    continue;
                }
                strvec_t body = {0};
                int task;
                size_t j = list_item_body(L, t, i, kind, co, cc, &body, &task);
                if (j >= t->to) { /* the last item */
                    lines_t BL = {(const char *const *)body.v, body.n};
                    r = ends_in_setext(&BL, depth + 1);
                }
                strvec_free(&body);
                i = j;
            }
        }
    }
    vec_free(&T);
    return r;
}

void tui_md_hold_pending_bullet(char *text) {
    if (!text) return;
    size_t n = strlen(text);
    char *nl = strrchr(text, '\n');
    if (!nl) return;
    /* PENDING_BULLET_LINE: \n[ \t>]*-[ \t]*$ */
    const char *p = nl + 1;
    while (*p == ' ' || *p == '\t' || *p == '>') p++;
    if (*p != '-') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return;
    strvec_t lines = {0};
    const char *q = text;
    for (;;) {
        const char *e = strchr(q, '\n');
        strvec_push_own(&lines, e ? xstrndup(q, (size_t)(e - q)) : xstrdup(q));
        if (!e) break;
        q = e + 1;
    }
    lines_t L = {(const char *const *)lines.v, lines.n};
    if (ends_in_setext(&L, 0)) *nl = 0;
    strvec_free(&lines);
    (void)n;
}
