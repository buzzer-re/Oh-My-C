/* htmlform.c - see htmlform.h. Ports omp html-form/parse.ts (FormBuilder), data.ts and paginate.ts over a small
 * from-scratch HTML tree builder that follows htmlparser2's tokenizer and implied-close rules (what linkedom's
 * parseHTML uses upstream). */
#define _GNU_SOURCE /* memmem: glibc declares it only with _GNU_SOURCE (build-tools/check_apple.sh) */
#include "text/htmlform.h"

#include "text/regex.h"
#include "text/rxbt.h"

#include <ctype.h>
#include <math.h>

/* ======================================================================== */
/* JavaScript whitespace (\s)                                               */
/* ======================================================================== */

/* bytes of the JavaScript `\s` code point at p (0: none) */
static size_t js_space(const char *p) {
    unsigned char c = (unsigned char)*p;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return 1;
    if (c < 0x80) return 0;
    uint32_t cp;
    int n = utf8_decode(p, strlen(p) < 4 ? strlen(p) : 4, &cp);
    if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
        cp == 0x205F || cp == 0x3000 || cp == 0xFEFF)
        return (size_t)n;
    return 0;
}

char *hf_one_line(const char *s) {
    sb_t b = SB_INIT;
    bool pending = false;
    for (const char *p = s ? s : ""; *p;) {
        size_t w = js_space(p);
        if (w) {
            pending = true;
            p += w;
            continue;
        }
        if (pending && b.len) sb_putc(&b, ' ');
        pending = false;
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

/* String.prototype.trim / trimStart / trimEnd */
static char *js_trim(const char *s, bool start, bool end) {
    const char *a = s ? s : "";
    if (start)
        for (size_t w; (w = js_space(a));) a += w;
    size_t n = strlen(a), keep = n;
    if (end) {
        keep = 0; /* the end of the last code point that is not whitespace */
        for (size_t k = 0; k < n;) {
            size_t w = js_space(a + k);
            if (w) {
                k += w;
                continue;
            }
            k++;
            while (k < n && ((unsigned char)a[k] & 0xC0) == 0x80) k++;
            keep = k;
        }
    }
    return xstrndup(a, keep);
}

static bool js_blank(const char *s) {
    for (const char *p = s ? s : ""; *p;) {
        size_t w = js_space(p);
        if (!w) return false;
        p += w;
    }
    return true;
}

size_t hf_utf16_len(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* ======================================================================== */
/* JavaScript Number(string)                                                */
/* ======================================================================== */

bool hf_js_number(const char *text, double *out) {
    char *t = js_trim(text, true, true);
    const char *s = t;
    bool ok = false;
    double v = 0;
    if (!*s) {
        ok = true;
        v = 0;
    } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X' || s[1] == 'o' || s[1] == 'O' || s[1] == 'b' || s[1] == 'B')) {
        int base = (s[1] == 'x' || s[1] == 'X') ? 16 : (s[1] == 'o' || s[1] == 'O') ? 8 : 2;
        const char *p = s + 2;
        if (*p) {
            ok = true;
            for (; *p; p++) {
                int d = isdigit((unsigned char)*p) ? *p - '0' : isalpha((unsigned char)*p) ? tolower((unsigned char)*p) - 'a' + 10 : 99;
                if (d >= base) {
                    ok = false;
                    break;
                }
                v = v * base + d;
            }
        }
    } else {
        const char *p = s;
        bool neg = false;
        if (*p == '+' || *p == '-') neg = *p++ == '-';
        if (!strcmp(p, "Infinity")) {
            ok = true;
            v = neg ? -INFINITY : INFINITY;
        } else {
            /* StrDecimalLiteral: digits [. digits] [e[+-]digits], at least one digit in the mantissa */
            const char *q = p;
            size_t digits = 0;
            while (isdigit((unsigned char)*q)) q++, digits++;
            if (*q == '.') {
                q++;
                while (isdigit((unsigned char)*q)) q++, digits++;
            }
            if (digits && (*q == 'e' || *q == 'E')) {
                const char *e = q + 1;
                if (*e == '+' || *e == '-') e++;
                if (isdigit((unsigned char)*e)) {
                    while (isdigit((unsigned char)*e)) e++;
                    q = e;
                } else q = NULL;
            }
            if (digits && q && !*q) {
                ok = true;
                v = strtod(s, NULL);
            }
        }
    }
    free(t);
    if (ok && out) *out = v;
    return ok;
}

static bool js_number_finite(const char *text, double *out) {
    double v;
    if (!hf_js_number(text, &v) || !isfinite(v)) return false;
    *out = v;
    return true;
}

/* String(number) */
static char *js_num_str(double d) {
    sb_t b = SB_INIT;
    json_js_num_sb(&b, d);
    return sb_detach(&b);
}

/* ======================================================================== */
/* entities                                                                 */
/* ======================================================================== */

static const struct {
    const char *name, *val;
    bool legacy; /* recognized without the trailing `;` */
} ENTITIES[] = {
    {"amp", "&", true}, {"lt", "<", true}, {"gt", ">", true}, {"quot", "\"", true}, {"apos", "'", false},
    {"nbsp", "\xc2\xa0", true}, {"copy", "\xc2\xa9", true}, {"reg", "\xc2\xae", true}, {"trade", "\xe2\x84\xa2", false},
    {"hellip", "\xe2\x80\xa6", false}, {"mdash", "\xe2\x80\x94", false}, {"ndash", "\xe2\x80\x93", false},
    {"lsquo", "\xe2\x80\x98", false}, {"rsquo", "\xe2\x80\x99", false}, {"ldquo", "\xe2\x80\x9c", false},
    {"rdquo", "\xe2\x80\x9d", false}, {"sbquo", "\xe2\x80\x9a", false}, {"bdquo", "\xe2\x80\x9e", false},
    {"laquo", "\xc2\xab", true}, {"raquo", "\xc2\xbb", true}, {"bull", "\xe2\x80\xa2", false},
    {"middot", "\xc2\xb7", true}, {"times", "\xc3\x97", true}, {"divide", "\xc3\xb7", true}, {"deg", "\xc2\xb0", true},
    {"plusmn", "\xc2\xb1", true}, {"micro", "\xc2\xb5", true}, {"para", "\xc2\xb6", true}, {"sect", "\xc2\xa7", true},
    {"cent", "\xc2\xa2", true}, {"pound", "\xc2\xa3", true}, {"yen", "\xc2\xa5", true}, {"euro", "\xe2\x82\xac", false},
    {"curren", "\xc2\xa4", true}, {"brvbar", "\xc2\xa6", true}, {"uml", "\xc2\xa8", true}, {"macr", "\xc2\xaf", true},
    {"acute", "\xc2\xb4", true}, {"cedil", "\xc2\xb8", true}, {"ordf", "\xc2\xaa", true}, {"ordm", "\xc2\xba", true},
    {"not", "\xc2\xac", true}, {"shy", "\xc2\xad", true}, {"sup1", "\xc2\xb9", true}, {"sup2", "\xc2\xb2", true},
    {"sup3", "\xc2\xb3", true}, {"frac14", "\xc2\xbc", true}, {"frac12", "\xc2\xbd", true}, {"frac34", "\xc2\xbe", true},
    {"iquest", "\xc2\xbf", true}, {"iexcl", "\xc2\xa1", true}, {"larr", "\xe2\x86\x90", false},
    {"rarr", "\xe2\x86\x92", false}, {"uarr", "\xe2\x86\x91", false}, {"darr", "\xe2\x86\x93", false},
    {"harr", "\xe2\x86\x94", false}, {"lArr", "\xe2\x87\x90", false}, {"rArr", "\xe2\x87\x92", false},
    {"hArr", "\xe2\x87\x94", false}, {"le", "\xe2\x89\xa4", false}, {"ge", "\xe2\x89\xa5", false},
    {"ne", "\xe2\x89\xa0", false}, {"asymp", "\xe2\x89\x88", false}, {"equiv", "\xe2\x89\xa1", false},
    {"infin", "\xe2\x88\x9e", false}, {"minus", "\xe2\x88\x92", false}, {"sum", "\xe2\x88\x91", false},
    {"prod", "\xe2\x88\x8f", false}, {"radic", "\xe2\x88\x9a", false}, {"part", "\xe2\x88\x82", false},
    {"nabla", "\xe2\x88\x87", false}, {"isin", "\xe2\x88\x88", false}, {"notin", "\xe2\x88\x89", false},
    {"cap", "\xe2\x88\xa9", false}, {"cup", "\xe2\x88\xaa", false}, {"and", "\xe2\x88\xa7", false},
    {"or", "\xe2\x88\xa8", false}, {"forall", "\xe2\x88\x80", false}, {"exist", "\xe2\x88\x83", false},
    {"empty", "\xe2\x88\x85", false}, {"lowast", "\xe2\x88\x97", false}, {"prime", "\xe2\x80\xb2", false},
    {"Prime", "\xe2\x80\xb3", false}, {"oline", "\xe2\x80\xbe", false}, {"frasl", "\xe2\x81\x84", false},
    {"dagger", "\xe2\x80\xa0", false}, {"Dagger", "\xe2\x80\xa1", false}, {"permil", "\xe2\x80\xb0", false},
    {"lsaquo", "\xe2\x80\xb9", false}, {"rsaquo", "\xe2\x80\xba", false}, {"spades", "\xe2\x99\xa0", false},
    {"clubs", "\xe2\x99\xa3", false}, {"hearts", "\xe2\x99\xa5", false}, {"diams", "\xe2\x99\xa6", false},
    {"loz", "\xe2\x97\x8a", false}, {"check", "\xe2\x9c\x93", false}, {"cross", "\xe2\x9c\x97", false},
    {"star", "\xe2\x98\x86", false}, {"starf", "\xe2\x98\x85", false}, {"emsp", "\xe2\x80\x83", false},
    {"ensp", "\xe2\x80\x82", false}, {"thinsp", "\xe2\x80\x89", false}, {"zwnj", "\xe2\x80\x8c", false},
    {"zwj", "\xe2\x80\x8d", false}, {"lrm", "\xe2\x80\x8e", false}, {"rlm", "\xe2\x80\x8f", false},
    {"Tab", "\t", false}, {"NewLine", "\n", false}, {"excl", "!", false}, {"num", "#", false},
    {"dollar", "$", false}, {"percnt", "%", false}, {"lpar", "(", false}, {"rpar", ")", false},
    {"ast", "*", false}, {"plus", "+", false}, {"comma", ",", false}, {"period", ".", false},
    {"sol", "/", false}, {"colon", ":", false}, {"semi", ";", false}, {"equals", "=", false},
    {"quest", "?", false}, {"commat", "@", false}, {"lsqb", "[", false}, {"bsol", "\\", false},
    {"rsqb", "]", false}, {"Hat", "^", false}, {"lowbar", "_", false}, {"grave", "`", false},
    {"lcub", "{", false}, {"verbar", "|", false}, {"vert", "|", false}, {"rcub", "}", false},
    {"Agrave", "\xc3\x80", true}, {"Aacute", "\xc3\x81", true}, {"Acirc", "\xc3\x82", true}, {"Atilde", "\xc3\x83", true},
    {"Auml", "\xc3\x84", true}, {"Aring", "\xc3\x85", true}, {"AElig", "\xc3\x86", true}, {"Ccedil", "\xc3\x87", true},
    {"Egrave", "\xc3\x88", true}, {"Eacute", "\xc3\x89", true}, {"Ecirc", "\xc3\x8a", true}, {"Euml", "\xc3\x8b", true},
    {"Igrave", "\xc3\x8c", true}, {"Iacute", "\xc3\x8d", true}, {"Icirc", "\xc3\x8e", true}, {"Iuml", "\xc3\x8f", true},
    {"ETH", "\xc3\x90", true}, {"Ntilde", "\xc3\x91", true}, {"Ograve", "\xc3\x92", true}, {"Oacute", "\xc3\x93", true},
    {"Ocirc", "\xc3\x94", true}, {"Otilde", "\xc3\x95", true}, {"Ouml", "\xc3\x96", true}, {"Oslash", "\xc3\x98", true},
    {"Ugrave", "\xc3\x99", true}, {"Uacute", "\xc3\x9a", true}, {"Ucirc", "\xc3\x9b", true}, {"Uuml", "\xc3\x9c", true},
    {"Yacute", "\xc3\x9d", true}, {"THORN", "\xc3\x9e", true}, {"szlig", "\xc3\x9f", true}, {"agrave", "\xc3\xa0", true},
    {"aacute", "\xc3\xa1", true}, {"acirc", "\xc3\xa2", true}, {"atilde", "\xc3\xa3", true}, {"auml", "\xc3\xa4", true},
    {"aring", "\xc3\xa5", true}, {"aelig", "\xc3\xa6", true}, {"ccedil", "\xc3\xa7", true}, {"egrave", "\xc3\xa8", true},
    {"eacute", "\xc3\xa9", true}, {"ecirc", "\xc3\xaa", true}, {"euml", "\xc3\xab", true}, {"igrave", "\xc3\xac", true},
    {"iacute", "\xc3\xad", true}, {"icirc", "\xc3\xae", true}, {"iuml", "\xc3\xaf", true}, {"eth", "\xc3\xb0", true},
    {"ntilde", "\xc3\xb1", true}, {"ograve", "\xc3\xb2", true}, {"oacute", "\xc3\xb3", true}, {"ocirc", "\xc3\xb4", true},
    {"otilde", "\xc3\xb5", true}, {"ouml", "\xc3\xb6", true}, {"oslash", "\xc3\xb8", true}, {"ugrave", "\xc3\xb9", true},
    {"uacute", "\xc3\xba", true}, {"ucirc", "\xc3\xbb", true}, {"uuml", "\xc3\xbc", true}, {"yacute", "\xc3\xbd", true},
    {"thorn", "\xc3\xbe", true}, {"yuml", "\xc3\xbf", true}, {"OElig", "\xc5\x92", false}, {"oelig", "\xc5\x93", false},
    {"Scaron", "\xc5\xa0", false}, {"scaron", "\xc5\xa1", false}, {"Yuml", "\xc5\xb8", false}, {"fnof", "\xc6\x92", false},
    {"circ", "\xcb\x86", false}, {"tilde", "\xcb\x9c", false}, {"Alpha", "\xce\x91", false}, {"Beta", "\xce\x92", false},
    {"Gamma", "\xce\x93", false}, {"Delta", "\xce\x94", false}, {"Theta", "\xce\x98", false}, {"Lambda", "\xce\x9b", false},
    {"Pi", "\xce\xa0", false}, {"Sigma", "\xce\xa3", false}, {"Phi", "\xce\xa6", false}, {"Psi", "\xce\xa8", false},
    {"Omega", "\xce\xa9", false}, {"alpha", "\xce\xb1", false}, {"beta", "\xce\xb2", false}, {"gamma", "\xce\xb3", false},
    {"delta", "\xce\xb4", false}, {"epsilon", "\xce\xb5", false}, {"zeta", "\xce\xb6", false}, {"eta", "\xce\xb7", false},
    {"theta", "\xce\xb8", false}, {"iota", "\xce\xb9", false}, {"kappa", "\xce\xba", false}, {"lambda", "\xce\xbb", false},
    {"mu", "\xce\xbc", false}, {"nu", "\xce\xbd", false}, {"xi", "\xce\xbe", false}, {"omicron", "\xce\xbf", false},
    {"pi", "\xcf\x80", false}, {"rho", "\xcf\x81", false}, {"sigma", "\xcf\x83", false}, {"tau", "\xcf\x84", false},
    {"upsilon", "\xcf\x85", false}, {"phi", "\xcf\x86", false}, {"chi", "\xcf\x87", false}, {"psi", "\xcf\x88", false},
    {"omega", "\xcf\x89", false},
};

/* windows-1252 remap of numeric references 0x80..0x9F */
static const uint16_t C1_REMAP[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
                                      0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

/* decode the character reference at s[0] == '&' (n bytes left); appends to out, returns bytes consumed */
static size_t decode_entity(const char *s, size_t n, bool attr, sb_t *out) {
    if (n >= 2 && s[1] == '#') {
        size_t i = 2;
        bool hex = i < n && (s[i] == 'x' || s[i] == 'X');
        if (hex) i++;
        size_t ds = i;
        uint64_t cp = 0;
        while (i < n && (hex ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i]))) {
            int d = isdigit((unsigned char)s[i]) ? s[i] - '0' : tolower((unsigned char)s[i]) - 'a' + 10;
            if (cp < 0x110000) cp = cp * (hex ? 16 : 10) + (uint64_t)d;
            i++;
        }
        if (i == ds) {
            sb_putc(out, '&');
            return 1;
        }
        if (i < n && s[i] == ';') i++;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        else if (cp >= 0x80 && cp <= 0x9F) cp = C1_REMAP[cp - 0x80];
        sb_append_utf8(out, (uint32_t)cp);
        return i;
    }
    size_t i = 1;
    while (i < n && i < 40 && isalnum((unsigned char)s[i])) i++;
    size_t len = i - 1;
    bool semi = i < n && s[i] == ';';
    if (len) {
        if (semi)
            for (size_t k = 0; k < ARRAY_LEN(ENTITIES); k++)
                if (strlen(ENTITIES[k].name) == len && !memcmp(ENTITIES[k].name, s + 1, len)) {
                    sb_append(out, ENTITIES[k].val);
                    return i + 1;
                }
        /* legacy references without `;`: the longest legacy name prefix (attributes: not before [=A-Za-z0-9]) */
        size_t best = 0, bk = 0;
        for (size_t k = 0; k < ARRAY_LEN(ENTITIES); k++) {
            size_t l = strlen(ENTITIES[k].name);
            if (!ENTITIES[k].legacy || l > len || memcmp(ENTITIES[k].name, s + 1, l) || l <= best) continue;
            best = l;
            bk = k;
        }
        if (best) {
            char next = 1 + best < n ? s[1 + best] : 0;
            if (!(attr && (isalnum((unsigned char)next) || next == '='))) {
                sb_append(out, ENTITIES[bk].val);
                return 1 + best;
            }
        }
    }
    sb_putc(out, '&');
    return 1;
}

static char *decode_text(const char *s, size_t n, bool attr) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < n;) {
        if (s[i] == '&') i += decode_entity(s + i, n - i, attr, &b);
        else sb_putc(&b, s[i++]);
    }
    return sb_detach(&b);
}

/* ======================================================================== */
/* DOM                                                                      */
/* ======================================================================== */

enum { DN_ELEMENT, DN_TEXT, DN_COMMENT, DN_DOCUMENT };

typedef struct dnode {
    int type;
    char *tag;  /* element: lowercase name */
    strvec_t an, av;
    char *data; /* text */
    struct dnode *parent;
    VEC(struct dnode *) kids;
    size_t src_start, src_end; /* element source span */
} dnode_t;

typedef VEC(dnode_t *) dvec_t;

static dnode_t *dn_new(int type, dnode_t *parent) {
    dnode_t *d = xcalloc(1, sizeof *d);
    d->type = type;
    d->parent = parent;
    if (parent) vec_push(&parent->kids, d);
    return d;
}

static void dn_free(dnode_t *d) {
    if (!d) return;
    for (size_t i = 0; i < d->kids.n; i++) dn_free(d->kids.v[i]);
    free(d->kids.v);
    free(d->tag);
    free(d->data);
    strvec_free(&d->an);
    strvec_free(&d->av);
    free(d);
}

static bool is_el(const dnode_t *d, const char *tag) { return d && d->type == DN_ELEMENT && (!tag || !strcmp(d->tag, tag)); }

static const char *dn_attr(const dnode_t *d, const char *name) {
    if (!d || d->type != DN_ELEMENT) return NULL;
    for (size_t i = 0; i < d->an.n; i++)
        if (!strcmp(d->an.v[i], name)) return d->av.v[i];
    return NULL;
}

static bool dn_has(const dnode_t *d, const char *name) { return dn_attr(d, name) != NULL; }

static void dn_text_sb(const dnode_t *d, sb_t *b) {
    if (d->type == DN_TEXT) {
        sb_append(b, d->data);
        return;
    }
    if (d->type == DN_COMMENT) return;
    for (size_t i = 0; i < d->kids.n; i++) dn_text_sb(d->kids.v[i], b);
}

static char *dn_text(const dnode_t *d) {
    sb_t b = SB_INIT;
    dn_text_sb(d, &b);
    return sb_detach(&b);
}

static bool is_control(const dnode_t *d) {
    return is_el(d, NULL) && (!strcmp(d->tag, "input") || !strcmp(d->tag, "select") || !strcmp(d->tag, "textarea"));
}

typedef bool (*dn_pred)(const dnode_t *d, const void *arg);

static bool pred_tag(const dnode_t *d, const void *arg) { return is_el(d, arg); }
static bool pred_control(const dnode_t *d, const void *arg) {
    UNUSED(arg);
    return is_control(d);
}

/* querySelectorAll: matching descendants of `root` (not root itself) in document order */
static void dn_query_all(const dnode_t *root, dn_pred p, const void *arg, dvec_t *out) {
    for (size_t i = 0; i < root->kids.n; i++) {
        dnode_t *k = root->kids.v[i];
        if (p(k, arg)) vec_push(out, k);
        dn_query_all(k, p, arg, out);
    }
}

static dnode_t *dn_query(const dnode_t *root, dn_pred p, const void *arg) {
    for (size_t i = 0; i < root->kids.n; i++) {
        dnode_t *k = root->kids.v[i];
        if (p(k, arg)) return k;
        dnode_t *r = dn_query(k, p, arg);
        if (r) return r;
    }
    return NULL;
}

static dnode_t *dn_closest(dnode_t *d, const char *tag) {
    for (; d; d = d->parent)
        if (is_el(d, tag)) return d;
    return NULL;
}

static bool dn_contains(const dnode_t *a, const dnode_t *b) {
    for (; b; b = b->parent)
        if (a == b) return true;
    return false;
}

static dnode_t *dn_child(const dnode_t *d, const char *tag) {
    for (size_t i = 0; i < d->kids.n; i++)
        if (is_el(d->kids.v[i], tag)) return d->kids.v[i];
    return NULL;
}

/* ======================================================================== */
/* tokenizer + tree builder (htmlparser2 semantics, html mode)              */
/* ======================================================================== */

#define MAX_DEPTH 256

static bool in_list(const char *s, const char *const *list) {
    for (; *list; list++)
        if (!strcmp(s, *list)) return true;
    return false;
}

static const char *const VOID_ELEMENTS[] = {"area",  "base",   "basefont", "br",   "col",  "command", "embed",
                                            "frame", "hr",     "image",    "img",  "input", "isindex", "keygen",
                                            "link",  "meta",   "param",    "source", "track", "wbr",   NULL};
static const char *const FORM_TAGS[] = {"input", "option", "optgroup", "select", "button", "datalist", "textarea", NULL};
static const char *const P_TAG[] = {"p", NULL};

/* openImpliesClose */
static const char *const *implies_close(const char *name) {
    static const char *const TR[] = {"tr", "th", "td", NULL}, *const TH[] = {"th", NULL},
                             *const TD[] = {"thead", "th", "td", NULL}, *const BODY[] = {"head", "link", "script", NULL},
                             *const LI[] = {"li", NULL}, *const OPTION[] = {"option", NULL},
                             *const OPTGROUP[] = {"optgroup", "option", NULL}, *const DDT[] = {"dd", "dt", NULL},
                             *const RTP[] = {"rt", "rp", NULL}, *const TSECT[] = {"thead", "tbody", NULL};
    static const char *const P_OPENERS[] = {"p",    "h1",       "h2",     "h3",     "h4",     "h5",      "h6",
                                            "address", "article", "aside", "blockquote", "details", "div", "dl",
                                            "fieldset", "figcaption", "figure", "footer", "form", "header", "hr",
                                            "main", "nav", "ol", "pre", "section", "table", "ul", NULL};
    static const char *const FORM_OPENERS[] = {"select", "input", "output", "button", "datalist", "textarea", NULL};
    if (!strcmp(name, "tr")) return TR;
    if (!strcmp(name, "th")) return TH;
    if (!strcmp(name, "td")) return TD;
    if (!strcmp(name, "body")) return BODY;
    if (!strcmp(name, "li")) return LI;
    if (!strcmp(name, "option")) return OPTION;
    if (!strcmp(name, "optgroup")) return OPTGROUP;
    if (!strcmp(name, "dd") || !strcmp(name, "dt")) return DDT;
    if (!strcmp(name, "rt") || !strcmp(name, "rp")) return RTP;
    if (!strcmp(name, "tbody") || !strcmp(name, "tfoot")) return TSECT;
    if (in_list(name, P_OPENERS)) return P_TAG;
    if (in_list(name, FORM_OPENERS)) return FORM_TAGS;
    return NULL;
}

typedef struct {
    dnode_t *doc;
    dvec_t stack; /* open elements, top last */
} tb_t;

static dnode_t *tb_cur(tb_t *t) { return t->stack.n ? t->stack.v[t->stack.n - 1] : t->doc; }

static bool tb_foreign(tb_t *t) {
    static const char *const INTEGRATION[] = {"mi", "mo", "mn", "ms", "mtext", "annotation-xml", "foreignobject", "desc", "title", NULL};
    for (size_t i = t->stack.n; i-- > 0;) {
        const char *tag = t->stack.v[i]->tag;
        if (!strcmp(tag, "svg") || !strcmp(tag, "math")) return true;
        if (in_list(tag, INTEGRATION)) return false;
    }
    return false;
}

static void tb_pop(tb_t *t, size_t end) {
    dnode_t *d = t->stack.v[--t->stack.n];
    d->src_end = end;
}

static void tb_text(tb_t *t, char *text) {
    if (!*text) {
        free(text);
        return;
    }
    dnode_t *cur = tb_cur(t);
    dnode_t *last = cur->kids.n ? cur->kids.v[cur->kids.n - 1] : NULL;
    if (last && last->type == DN_TEXT) {
        char *j = xasprintf("%s%s", last->data, text);
        free(last->data);
        last->data = j;
        free(text);
        return;
    }
    dnode_t *d = dn_new(DN_TEXT, cur);
    d->data = text;
}

/* returns the opened element (pushed unless void / self-closed in foreign content) */
static dnode_t *tb_open(tb_t *t, char *name, strvec_t *an, strvec_t *av, bool self_closing, size_t start, size_t end) {
    const char *const *ic = implies_close(name);
    if (ic)
        while (t->stack.n && in_list(t->stack.v[t->stack.n - 1]->tag, ic)) tb_pop(t, start);
    bool foreign = tb_foreign(t);
    dnode_t *d = dn_new(DN_ELEMENT, tb_cur(t));
    d->tag = name;
    d->an = *an;
    d->av = *av;
    d->src_start = start;
    d->src_end = end;
    bool is_void = in_list(name, VOID_ELEMENTS);
    if (!is_void && !(self_closing && (foreign || !strcmp(name, "svg") || !strcmp(name, "math"))) && t->stack.n < MAX_DEPTH)
        vec_push(&t->stack, d);
    return d;
}

static void tb_close(tb_t *t, const char *name, size_t start, size_t end) {
    if (in_list(name, VOID_ELEMENTS)) {
        if (!strcmp(name, "br")) {
            strvec_t a = {0}, b = {0};
            tb_open(t, xstrdup("br"), &a, &b, false, start, end);
        }
        return;
    }
    for (size_t i = t->stack.n; i-- > 0;) {
        if (strcmp(t->stack.v[i]->tag, name)) continue;
        while (t->stack.n > i + 1) tb_pop(t, start);
        tb_pop(t, end);
        return;
    }
    if (!strcmp(name, "p")) {
        strvec_t a = {0}, b = {0};
        tb_open(t, xstrdup("p"), &a, &b, false, start, end);
        if (t->stack.n && tb_cur(t)->tag && !strcmp(tb_cur(t)->tag, "p")) tb_pop(t, end);
    }
}

static bool tag_char(char c) { return c && !isspace((unsigned char)c) && c != '/' && c != '>'; }

static char *lower_dup(const char *s, size_t n) {
    char *r = xstrndup(s, n);
    for (char *p = r; *p; p++) *p = (char)tolower((unsigned char)*p);
    return r;
}

/* find `</name` (case-insensitive) from i; returns its offset or n */
static size_t find_end_tag(const char *s, size_t n, size_t i, const char *name) {
    size_t l = strlen(name);
    for (; i + 2 + l <= n; i++) {
        if (s[i] != '<' || s[i + 1] != '/') continue;
        if (strncasecmp(s + i + 2, name, l)) continue;
        char c = i + 2 + l < n ? s[i + 2 + l] : 0;
        if (!c || isspace((unsigned char)c) || c == '/' || c == '>') return i;
    }
    return n;
}

static dnode_t *html_parse(const char *s) {
    size_t n = strlen(s);
    tb_t t = {0};
    t.doc = dn_new(DN_DOCUMENT, NULL);
    size_t i = 0, text_start = 0;
#define FLUSH_TEXT(upto)                                                                                         \
    do {                                                                                                         \
        if ((upto) > text_start) tb_text(&t, decode_text(s + text_start, (upto) - text_start, false));          \
    } while (0)
    while (i < n) {
        if (s[i] != '<') {
            i++;
            continue;
        }
        size_t lt = i;
        if (i + 4 <= n && !memcmp(s + i, "<!--", 4)) {
            FLUSH_TEXT(lt);
            const char *e = memmem(s + i + 4, n - i - 4, "-->", 3);
            i = e ? (size_t)(e - s) + 3 : n;
            text_start = i;
            continue;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            FLUSH_TEXT(lt);
            const char *e = memchr(s + i, '>', n - i);
            i = e ? (size_t)(e - s) + 1 : n;
            text_start = i;
            continue;
        }
        if (i + 1 < n && s[i + 1] == '/') {
            if (i + 2 < n && isalpha((unsigned char)s[i + 2])) {
                size_t ns = i + 2, ne = ns;
                while (ne < n && tag_char(s[ne])) ne++;
                const char *e = memchr(s + ne, '>', n - ne);
                if (!e) { /* a tag cut off by the end of the markup is dropped */
                    FLUSH_TEXT(lt);
                    text_start = i = n;
                    break;
                }
                FLUSH_TEXT(lt);
                char *name = lower_dup(s + ns, ne - ns);
                i = (size_t)(e - s) + 1;
                tb_close(&t, name, lt, i);
                free(name);
                text_start = i;
                continue;
            }
            if (i + 2 < n && s[i + 2] == '>') { /* `</>` is ignored */
                FLUSH_TEXT(lt);
                text_start = i = i + 3;
                continue;
            }
            if (i + 2 < n) { /* bogus comment */
                FLUSH_TEXT(lt);
                const char *e = memchr(s + i, '>', n - i);
                text_start = i = e ? (size_t)(e - s) + 1 : n;
                continue;
            }
            i++;
            continue;
        }
        if (i + 1 >= n || !isalpha((unsigned char)s[i + 1])) {
            i++;
            continue;
        }
        /* open tag */
        size_t ns = i + 1, ne = ns;
        while (ne < n && tag_char(s[ne])) ne++;
        size_t p = ne;
        strvec_t an = {0}, av = {0};
        bool self_closing = false, complete = false;
        while (p < n) {
            while (p < n && isspace((unsigned char)s[p])) p++;
            if (p >= n) break;
            if (s[p] == '>') {
                p++;
                complete = true;
                break;
            }
            if (s[p] == '/') {
                if (p + 1 < n && s[p + 1] == '>') {
                    self_closing = true;
                    p += 2;
                    complete = true;
                    break;
                }
                p++;
                continue;
            }
            size_t as = p;
            p++; /* an attribute name may start with `=` */
            while (p < n && !isspace((unsigned char)s[p]) && s[p] != '=' && s[p] != '>' && s[p] != '/') p++;
            char *aname = lower_dup(s + as, p - as);
            size_t q = p;
            while (q < n && isspace((unsigned char)s[q])) q++;
            char *aval = NULL;
            if (q < n && s[q] == '=') {
                q++;
                while (q < n && isspace((unsigned char)s[q])) q++;
                if (q < n && (s[q] == '"' || s[q] == '\'')) {
                    char quote = s[q];
                    const char *e = memchr(s + q + 1, quote, n - q - 1);
                    if (!e) {
                        free(aname);
                        p = n;
                        break;
                    }
                    aval = decode_text(s + q + 1, (size_t)(e - s) - q - 1, true);
                    p = (size_t)(e - s) + 1;
                } else {
                    size_t vs = q;
                    while (q < n && !isspace((unsigned char)s[q]) && s[q] != '>') q++;
                    aval = decode_text(s + vs, q - vs, true);
                    p = q;
                }
            }
            bool dup = false;
            for (size_t k = 0; k < an.n; k++) dup |= !strcmp(an.v[k], aname);
            if (dup) {
                free(aname);
                free(aval);
            } else {
                strvec_push_own(&an, aname);
                strvec_push_own(&av, aval ? aval : xstrdup(""));
            }
        }
        if (!complete) { /* cut off: dropped */
            strvec_free(&an);
            strvec_free(&av);
            FLUSH_TEXT(lt);
            text_start = i = n;
            break;
        }
        FLUSH_TEXT(lt);
        char *name = lower_dup(s + ns, ne - ns);
        dnode_t *el = tb_open(&t, name, &an, &av, self_closing, lt, p);
        i = text_start = p;
        /* raw text (script, style) and escapable raw text (textarea, title) run to their end tag */
        bool raw = !strcmp(el->tag, "script") || !strcmp(el->tag, "style");
        bool rcdata = !strcmp(el->tag, "textarea") || !strcmp(el->tag, "title");
        if ((raw || rcdata) && !self_closing && tb_cur(&t) == el) {
            size_t e = find_end_tag(s, n, i, el->tag);
            if (e > i) tb_text(&t, rcdata ? decode_text(s + i, e - i, false) : xstrndup(s + i, e - i));
            i = text_start = e;
        }
    }
    FLUSH_TEXT(i < n ? i : n);
#undef FLUSH_TEXT
    while (t.stack.n) tb_pop(&t, n);
    free(t.stack.v);
    return t.doc;
}

/* ======================================================================== */
/* spans                                                                    */
/* ======================================================================== */

static void spans_free(hf_spans_t *s) {
    for (size_t i = 0; i < s->n; i++) free(s->v[i].text);
    free(s->v);
    memset(s, 0, sizeof *s);
}

static void spans_clone(hf_spans_t *dst, const hf_spans_t *src) {
    memset(dst, 0, sizeof *dst);
    for (size_t i = 0; i < src->n; i++) {
        hf_span_t sp = {xstrdup(src->v[i].text), src->v[i].style};
        vec_push(dst, sp);
    }
}

/* appendSpan: merge into the previous span when the style matches */
static void append_span(hf_spans_t *s, const char *text, size_t len, hf_style_t style) {
    if (!len) return;
    if (s->n && s->v[s->n - 1].style == style) {
        char *j = xmalloc(strlen(s->v[s->n - 1].text) + len + 1);
        size_t l = strlen(s->v[s->n - 1].text);
        memcpy(j, s->v[s->n - 1].text, l);
        memcpy(j + l, text, len);
        j[l + len] = 0;
        free(s->v[s->n - 1].text);
        s->v[s->n - 1].text = j;
        return;
    }
    hf_span_t sp = {xstrndup(text, len), style};
    vec_push(s, sp);
}

static void append_str(hf_spans_t *s, const char *text, hf_style_t style) { append_span(s, text, strlen(text), style); }

/* text.replace(/[^\S\n]+/g, " ").replace(/ ?\n ?/g, "\n") */
static char *collapse_ws(const char *t) {
    sb_t a = SB_INIT;
    bool pending = false;
    for (const char *p = t; *p;) {
        size_t w = *p == '\n' ? 0 : js_space(p);
        if (w) {
            pending = true;
            p += w;
            continue;
        }
        if (pending) sb_putc(&a, ' ');
        pending = false;
        sb_putc(&a, *p++);
    }
    if (pending) sb_putc(&a, ' ');
    sb_t b = SB_INIT;
    const char *s = sb_cstr(&a);
    for (size_t i = 0; s[i];) {
        if (s[i] == ' ' && s[i + 1] == '\n') {
            i++;
            continue;
        }
        if (s[i] == '\n') {
            sb_putc(&b, '\n');
            i++;
            if (s[i] == ' ') i++;
            continue;
        }
        sb_putc(&b, s[i++]);
    }
    sb_free(&a);
    return sb_detach(&b);
}

/* normalizeSpans: HTML whitespace collapsing; empty spans drop */
static void normalize_spans(const hf_spans_t *in, hf_spans_t *out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < in->n; i++) {
        char *t = collapse_ws(in->v[i].text);
        append_str(out, t, in->v[i].style);
        free(t);
    }
    if (out->n) {
        char *t = js_trim(out->v[0].text, true, false);
        free(out->v[0].text);
        out->v[0].text = t;
        t = js_trim(out->v[out->n - 1].text, false, true);
        free(out->v[out->n - 1].text);
        out->v[out->n - 1].text = t;
    }
    size_t k = 0;
    for (size_t i = 0; i < out->n; i++) {
        if (!*out->v[i].text) {
            free(out->v[i].text);
            continue;
        }
        out->v[k++] = out->v[i];
    }
    out->n = k;
}

char *hf_spans_text(const hf_spans_t *s) {
    sb_t b = SB_INIT;
    for (size_t i = 0; s && i < s->n; i++) sb_append(&b, s->v[i].text);
    return sb_detach(&b);
}

/* ======================================================================== */
/* FormBuilder                                                              */
/* ======================================================================== */

static const char *const SKIPPED[] = {"button", "head", "iframe", "link", "meta", "noscript", "script", "style", "template", "title", NULL};
static const char *const BLOCK[] = {"address", "article", "aside", "blockquote", "dd", "details", "div", "dl", "dt",
                                    "figure", "figcaption", "footer", "header", "li", "main", "nav", "p", "section", NULL};
static const char *const HEADING[] = {"h1", "h2", "h3", "h4", "h5", "h6", "summary", NULL};
static const char *const IGNORED_INPUTS[] = {"button", "file", "image", "reset", "submit", NULL};

static int style_of(const char *tag, int dflt) {
    if (!strcmp(tag, "b") || !strcmp(tag, "strong")) return HF_STRONG;
    if (!strcmp(tag, "code") || !strcmp(tag, "kbd") || !strcmp(tag, "samp") || !strcmp(tag, "tt")) return HF_CODE;
    if (!strcmp(tag, "em") || !strcmp(tag, "i")) return HF_EM;
    return dflt;
}

typedef struct {
    hf_field_t *field;
    char *key; /* type\0name */
    size_t klen;
    size_t sec, block; /* the section and block of its first option */
} group_t;

typedef struct {
    const dnode_t *root;
    hf_form_t *form;
    hf_section_t *section; /* index into form->sections: sections only grow, so keep the index */
    size_t sec;
    hf_spans_t inl;
    int fieldset_depth;
    VEC(group_t) groups;
    strvec_t label_ids;     /* <label for> by control id ... */
    dvec_t label_els;       /* ... and the label */
    int depth;
    const char *src; /* the markup (svg source spans) */
} fb_t;

static hf_section_t *cur_section(fb_t *b) { return &b->form->sections.v[b->sec]; }

static void push_block(fb_t *b, hf_block_t blk) { vec_push(&cur_section(b)->blocks, blk); }

static void fb_walk(fb_t *b, const dnode_t *node, int style);

static void fb_children(fb_t *b, const dnode_t *node, int style) {
    if (b->depth > MAX_DEPTH) return;
    b->depth++;
    for (size_t i = 0; i < node->kids.n; i++) fb_walk(b, node->kids.v[i], style);
    b->depth--;
}

static void fb_flush(fb_t *b, bool heading) {
    hf_spans_t spans;
    normalize_spans(&b->inl, &spans);
    spans_free(&b->inl);
    if (!spans.n) {
        free(spans.v);
        return;
    }
    hf_block_t blk = {.kind = HF_B_TEXT, .spans = spans, .heading = heading};
    push_block(b, blk);
}

static void spans_visit(const dnode_t *node, int style, hf_spans_t *spans, int depth) {
    if (depth > MAX_DEPTH) return;
    if (node->type == DN_COMMENT) return;
    if (node->type == DN_TEXT) {
        append_str(spans, node->data, (hf_style_t)style);
        return;
    }
    if (node->type != DN_ELEMENT) return;
    const char *tag = node->tag;
    if (in_list(tag, SKIPPED) || !strcmp(tag, "input") || !strcmp(tag, "select") || !strcmp(tag, "textarea") ||
        !strcmp(tag, "svg"))
        return;
    if (!strcmp(tag, "br")) {
        append_str(spans, "\n", HF_PLAIN);
        return;
    }
    if (!strcmp(tag, "img")) {
        const char *alt = dn_attr(node, "alt");
        append_str(spans, alt ? alt : "", (hf_style_t)style);
        return;
    }
    if (in_list(tag, BLOCK) || in_list(tag, HEADING)) append_str(spans, " ", HF_PLAIN);
    for (size_t i = 0; i < node->kids.n; i++) spans_visit(node->kids.v[i], style_of(tag, style), spans, depth + 1);
}

/* #spansOf: inline spans of an element, skipping controls */
static void spans_of(const dnode_t *el, hf_spans_t *out) {
    hf_spans_t raw = {0};
    for (size_t i = 0; i < el->kids.n; i++) spans_visit(el->kids.v[i], HF_PLAIN, &raw, 0);
    normalize_spans(&raw, out);
    spans_free(&raw);
}

static double num_attr(const dnode_t *el, const char *name, bool *has) {
    const char *raw = dn_attr(el, name);
    *has = false;
    if (!raw || js_blank(raw)) return 0;
    double v;
    if (!js_number_finite(raw, &v)) return 0;
    *has = true;
    return v;
}

/* #nameLabel: `sentry_dsn` -> `sentry dsn` */
static void name_label(const char *name, hf_spans_t *out) {
    memset(out, 0, sizeof *out);
    if (!*name) return;
    sb_t s = SB_INIT;
    for (const char *p = name; *p;) {
        if (*p == '_' || *p == '-') {
            while (*p == '_' || *p == '-') p++;
            sb_putc(&s, ' ');
        } else sb_putc(&s, *p++);
    }
    hf_span_t sp = {sb_detach(&s), HF_PLAIN};
    vec_push(out, sp);
}

static void add_field(fb_t *b, hf_field_t *f) {
    f->index = b->form->fields.n;
    vec_push(&b->form->fields, f);
    hf_block_t blk = {.kind = HF_B_FIELD, .field = f};
    push_block(b, blk);
}

static void detached_label(fb_t *b, const dnode_t *control, hf_spans_t *out) {
    memset(out, 0, sizeof *out);
    const char *id = dn_attr(control, "id");
    if (id && *id)
        for (size_t i = 0; i < b->label_ids.n; i++)
            if (!strcmp(b->label_ids.v[i], id)) {
                spans_of(b->label_els.v[i], out);
                return;
            }
    const char *text = dn_attr(control, "aria-label");
    if (!text) text = dn_attr(control, "title");
    if (text && *text) {
        hf_spans_t raw = {0};
        append_str(&raw, text, HF_PLAIN);
        normalize_spans(&raw, out);
        spans_free(&raw);
    }
}

static hf_field_t *field_new(hf_kind_t kind, const char *name, bool required) {
    hf_field_t *f = xcalloc(1, sizeof *f);
    f->kind = kind;
    f->name = xstrdup(name ? name : "");
    f->required = required;
    f->rows = 1;
    return f;
}

static void fb_control(fb_t *b, const dnode_t *control, const hf_spans_t *wrapping) {
    fb_flush(b, false);
    hf_spans_t label;
    if (wrapping && wrapping->n) spans_clone(&label, wrapping);
    else detached_label(b, control, &label);
    const char *name = dn_attr(control, "name");
    if (!name) name = "";
    bool required = dn_has(control, "required");
    const char *tag = control->tag;
    if (!strcmp(tag, "select")) {
        hf_field_t *f = field_new(HF_CHOICE, name, required);
        f->multi = dn_has(control, "multiple");
        f->select = true;
        f->label = label;
        dvec_t opts = {0};
        dn_query_all(control, pred_tag, "option", &opts);
        for (size_t i = 0; i < opts.n; i++) {
            char *raw = dn_text(opts.v[i]);
            char *text = hf_one_line(raw);
            free(raw);
            const char *v = dn_attr(opts.v[i], "value");
            hf_option_t o = {.value = xstrdup(v ? v : text), .checked = dn_has(opts.v[i], "selected"),
                             .disabled = dn_has(opts.v[i], "disabled")};
            hf_span_t sp = {text, HF_PLAIN};
            vec_push(&o.label, sp);
            vec_push(&f->opts, o);
        }
        free(opts.v);
        /* a single select always has a selection: the first enabled option unless one is marked */
        if (!f->multi) {
            bool any = false;
            for (size_t i = 0; i < f->opts.n; i++) any |= f->opts.v[i].checked;
            if (!any)
                for (size_t i = 0; i < f->opts.n; i++)
                    if (!f->opts.v[i].disabled) {
                        f->opts.v[i].checked = true;
                        break;
                    }
        }
        add_field(b, f);
        return;
    }
    if (!strcmp(tag, "textarea")) {
        hf_field_t *f = field_new(HF_TEXT, name, required);
        f->type = xstrdup("textarea");
        f->multiline = true;
        if (label.n) f->label = label;
        else {
            free(label.v);
            name_label(name, &f->label);
        }
        char *raw = dn_text(control);
        const char *v = raw;
        if (v[0] == '\r' && v[1] == '\n') v += 2;
        else if (v[0] == '\n') v++;
        f->value = xstrdup(v);
        free(raw);
        const char *ph = dn_attr(control, "placeholder");
        f->placeholder = xstrdup(ph ? ph : "");
        f->minlength = num_attr(control, "minlength", &f->has_minlength);
        f->maxlength = num_attr(control, "maxlength", &f->has_maxlength);
        bool has_rows;
        double rows = num_attr(control, "rows", &has_rows);
        f->rows = has_rows ? rows : 4;
        add_field(b, f);
        return;
    }
    const char *type_attr = dn_attr(control, "type");
    char *type = lower_dup(type_attr ? type_attr : "text", strlen(type_attr ? type_attr : "text"));
    if (in_list(type, IGNORED_INPUTS)) {
        free(type);
        spans_free(&label);
        return;
    }
    const char *value = dn_attr(control, "value");
    if (!strcmp(type, "hidden")) {
        if (*name) {
            strvec_push(&b->form->hidden_names, name);
            strvec_push(&b->form->hidden_values, value ? value : "");
        }
        free(type);
        spans_free(&label);
        return;
    }
    if (!strcmp(type, "radio") || !strcmp(type, "checkbox")) {
        hf_option_t o = {.value = xstrdup(value ? value : "on"), .checked = dn_has(control, "checked"),
                         .disabled = dn_has(control, "disabled")};
        if (label.n) o.label = label;
        else {
            free(label.v);
            hf_span_t sp = {xstrdup(value ? value : name), HF_PLAIN};
            vec_push(&o.label, sp);
        }
        size_t klen = strlen(type) + 1 + strlen(name);
        char *key = xmalloc(klen + 1);
        memcpy(key, type, strlen(type) + 1);
        memcpy(key + strlen(type) + 1, name, strlen(name) + 1);
        for (size_t i = 0; i < b->groups.n; i++)
            if (b->groups.v[i].klen == klen && !memcmp(b->groups.v[i].key, key, klen)) {
                hf_field_t *g = b->groups.v[i].field;
                vec_push(&g->opts, o);
                g->required |= required;
                /* text between the group's options: optionsEnd is the index just past the last of those blocks */
                hf_section_t *sec = cur_section(b);
                if (b->groups.v[i].sec == b->sec && sec->blocks.n && b->groups.v[i].block != sec->blocks.n - 1)
                    sec->blocks.v[b->groups.v[i].block].options_end = sec->blocks.n;
                free(key);
                free(type);
                return;
            }
        hf_field_t *f = field_new(HF_CHOICE, name, required);
        f->multi = !strcmp(type, "checkbox");
        vec_push(&f->opts, o);
        add_field(b, f);
        group_t g = {f, key, klen, b->sec, cur_section(b)->blocks.n - 1};
        vec_push(&b->groups, g);
        free(type);
        return;
    }
    hf_field_t *f = field_new(HF_TEXT, name, required);
    f->type = type;
    if (label.n) f->label = label;
    else {
        free(label.v);
        name_label(name, &f->label);
    }
    f->value = xstrdup(value ? value : "");
    const char *ph = dn_attr(control, "placeholder");
    f->placeholder = xstrdup(ph ? ph : "");
    f->min = num_attr(control, "min", &f->has_min);
    f->max = num_attr(control, "max", &f->has_max);
    const char *pat = dn_attr(control, "pattern");
    f->pattern = pat ? xstrdup(pat) : NULL;
    f->minlength = num_attr(control, "minlength", &f->has_minlength);
    f->maxlength = num_attr(control, "maxlength", &f->has_maxlength);
    f->rows = 1;
    add_field(b, f);
}

static bool registered_label(fb_t *b, const dnode_t *label) {
    for (size_t i = 0; i < b->label_els.n; i++)
        if (b->label_els.v[i] == label) return true;
    return false;
}

static void fb_label(fb_t *b, const dnode_t *label, int style) {
    const char *target = dn_attr(label, "for");
    if (target && *target && registered_label(b, label)) return;
    dvec_t controls = {0};
    dn_query_all(label, pred_control, NULL, &controls);
    if (!controls.n) {
        fb_children(b, label, style);
        return;
    }
    fb_flush(b, false);
    hf_spans_t spans;
    spans_of(label, &spans);
    for (size_t i = 0; i < controls.n; i++) fb_control(b, controls.v[i], &spans);
    spans_free(&spans);
    free(controls.v);
}

static void start_section(fb_t *b, hf_spans_t *title) {
    hf_section_t *s = cur_section(b);
    if (!s->blocks.n && !s->title.n) {
        spans_free(&s->title);
        s->title = *title;
        return;
    }
    hf_section_t ns = {.title = *title};
    vec_push(&b->form->sections, ns);
    b->sec = b->form->sections.n - 1;
}

static void fb_fieldset(fb_t *b, const dnode_t *fs) {
    fb_flush(b, false);
    const dnode_t *legend = dn_child(fs, "legend");
    if (b->fieldset_depth > 0) {
        if (legend) fb_walk(b, legend, HF_PLAIN);
    } else {
        hf_spans_t title = {0};
        if (legend) spans_of(legend, &title);
        start_section(b, &title);
    }
    b->fieldset_depth++;
    for (size_t i = 0; i < fs->kids.n; i++)
        if (fs->kids.v[i] != legend) fb_walk(b, fs->kids.v[i], HF_PLAIN);
    fb_flush(b, false);
    b->fieldset_depth--;
    if (!b->fieldset_depth) {
        hf_spans_t empty = {0};
        start_section(b, &empty);
    }
}

static void fb_list(fb_t *b, const dnode_t *list, bool ordered) {
    fb_flush(b, false);
    if (dn_query(list, pred_control, NULL)) {
        fb_children(b, list, HF_PLAIN);
        fb_flush(b, false);
        return;
    }
    hf_block_t blk = {.kind = HF_B_LIST, .ordered = ordered};
    for (size_t i = 0; i < list->kids.n; i++) {
        if (!is_el(list->kids.v[i], "li")) continue;
        hf_spans_t sp;
        spans_of(list->kids.v[i], &sp);
        if (sp.n) vec_push(&blk.items, sp);
        else free(sp.v);
    }
    if (blk.items.n) push_block(b, blk);
}

static void cells_of(const dnode_t *row, dvec_t *out) {
    for (size_t i = 0; i < row->kids.n; i++)
        if (is_el(row->kids.v[i], "td") || is_el(row->kids.v[i], "th")) vec_push(out, row->kids.v[i]);
}

static bool all_th(const dvec_t *cells) {
    for (size_t i = 0; i < cells->n; i++)
        if (strcmp(cells->v[i]->tag, "th")) return false;
    return true;
}

static void fb_table(fb_t *b, dnode_t *table) {
    fb_flush(b, false);
    dvec_t all = {0}, rows = {0};
    dn_query_all(table, pred_tag, "tr", &all);
    for (size_t i = 0; i < all.n; i++)
        if (dn_closest(all.v[i], "table") == table) vec_push(&rows, all.v[i]);
    free(all.v);
    if (!dn_query(table, pred_control, NULL)) {
        hf_block_t blk = {.kind = HF_B_TABLE};
        bool any = false;
        for (size_t r = 0; r < rows.n; r++) {
            dvec_t cells = {0};
            cells_of(rows.v[r], &cells);
            hf_row_t row = {0};
            for (size_t c = 0; c < cells.n; c++) {
                hf_spans_t sp;
                spans_of(cells.v[c], &sp);
                vec_push(&row, sp);
            }
            any |= row.n > 0;
            vec_push(&blk.rows, row);
            free(cells.v);
        }
        while (blk.header_rows < rows.n) {
            dnode_t *row = rows.v[blk.header_rows];
            dvec_t cells = {0};
            cells_of(row, &cells);
            bool head = dn_closest(row, "thead") != NULL || all_th(&cells);
            free(cells.v);
            if (!head) break;
            blk.header_rows++;
        }
        if (any) push_block(b, blk);
        else {
            for (size_t r = 0; r < blk.rows.n; r++) {
                for (size_t c = 0; c < blk.rows.v[r].n; c++) spans_free(&blk.rows.v[r].v[c]);
                free(blk.rows.v[r].v);
            }
            free(blk.rows.v);
        }
        free(rows.v);
        return;
    }
    /* a table of controls (a decision matrix): each row's text becomes a line and each unlabeled control borrows
     * its column header as its label */
    VEC(hf_spans_t) header = {0};
    for (size_t r = 0; r < rows.n; r++) {
        dvec_t cells = {0};
        cells_of(rows.v[r], &cells);
        if (all_th(&cells) && !dn_query(rows.v[r], pred_control, NULL)) {
            for (size_t k = 0; k < header.n; k++) spans_free(&header.v[k]);
            header.n = 0;
            for (size_t c = 0; c < cells.n; c++) {
                hf_spans_t sp;
                spans_of(cells.v[c], &sp);
                vec_push(&header, sp);
            }
            free(cells.v);
            continue;
        }
        hf_spans_t text = {0};
        for (size_t c = 0; c < cells.n; c++) {
            if (dn_query(cells.v[c], pred_control, NULL)) continue;
            hf_spans_t sp;
            spans_of(cells.v[c], &sp);
            if (sp.n) {
                if (text.n) append_str(&text, " \xc2\xb7 ", HF_PLAIN);
                for (size_t k = 0; k < sp.n; k++) append_str(&text, sp.v[k].text, sp.v[k].style);
            }
            spans_free(&sp);
        }
        if (text.n) {
            hf_block_t blk = {.kind = HF_B_TEXT, .spans = text};
            push_block(b, blk);
        } else free(text.v);
        for (size_t c = 0; c < cells.n; c++) {
            dvec_t controls = {0};
            dn_query_all(cells.v[c], pred_control, NULL, &controls);
            for (size_t k = 0; k < controls.n; k++) {
                dnode_t *own = dn_closest(controls.v[k], "label");
                hf_spans_t sp;
                spans_of(own && dn_contains(cells.v[c], own) ? own : cells.v[c], &sp);
                if (sp.n) fb_control(b, controls.v[k], &sp);
                else if (c < header.n) fb_control(b, controls.v[k], &header.v[c]);
                else fb_control(b, controls.v[k], NULL);
                spans_free(&sp);
            }
            free(controls.v);
        }
        free(cells.v);
    }
    for (size_t k = 0; k < header.n; k++) spans_free(&header.v[k]);
    free(header.v);
    free(rows.v);
}

/* the svg's accessible name: its aria-label, else its <title> */
static char *svg_title(const dnode_t *svg) {
    const char *label = dn_attr(svg, "aria-label");
    if (label && *label) return js_trim(label, true, true);
    const dnode_t *t = dn_child(svg, "title");
    char *raw = t ? dn_text(t) : xstrdup("");
    char *r = hf_one_line(raw);
    free(raw);
    return r;
}

static void fb_walk(fb_t *b, const dnode_t *node, int style) {
    if (node->type == DN_COMMENT) return;
    if (node->type == DN_TEXT) {
        append_str(&b->inl, node->data, (hf_style_t)style);
        return;
    }
    if (node->type != DN_ELEMENT) return;
    const char *tag = node->tag;
    if (in_list(tag, SKIPPED)) return;
    if (!strcmp(tag, "br")) {
        append_str(&b->inl, "\n", HF_PLAIN);
        return;
    }
    if (!strcmp(tag, "hr")) {
        fb_flush(b, false);
        return;
    }
    if (is_control(node)) {
        fb_control(b, node, NULL);
        return;
    }
    if (!strcmp(tag, "label")) {
        fb_label(b, node, style);
        return;
    }
    if (!strcmp(tag, "fieldset")) {
        fb_fieldset(b, node);
        return;
    }
    if (!strcmp(tag, "legend")) {
        fb_flush(b, false);
        fb_children(b, node, style);
        fb_flush(b, true);
        return;
    }
    if (!strcmp(tag, "ul") || !strcmp(tag, "ol")) {
        fb_list(b, node, !strcmp(tag, "ol"));
        return;
    }
    if (!strcmp(tag, "pre")) {
        fb_flush(b, false);
        char *raw = dn_text(node);
        char *t = js_trim(raw[0] == '\n' ? raw + 1 : raw, false, true);
        free(raw);
        hf_block_t blk = {.kind = HF_B_PRE, .text = t};
        push_block(b, blk);
        return;
    }
    if (!strcmp(tag, "table")) {
        fb_table(b, (dnode_t *)node);
        return;
    }
    if (!strcmp(tag, "img")) {
        const char *src = dn_attr(node, "src");
        char *s = src ? js_trim(src, true, true) : NULL;
        if (!s || !*s) {
            free(s);
            return;
        }
        fb_flush(b, false);
        const char *alt = dn_attr(node, "alt");
        hf_block_t blk = {.kind = HF_B_IMAGE, .src = s, .alt = js_trim(alt ? alt : "", true, true)};
        push_block(b, blk);
        return;
    }
    if (!strcmp(tag, "svg")) {
        fb_flush(b, false);
        size_t end = node->src_end > node->src_start ? node->src_end : node->src_start;
        hf_block_t blk = {.kind = HF_B_SVG, .markup = xstrndup(b->src + node->src_start, end - node->src_start),
                          .alt = svg_title(node)};
        push_block(b, blk);
        return;
    }
    if (in_list(tag, HEADING)) {
        fb_flush(b, false);
        fb_children(b, node, style);
        fb_flush(b, true);
        return;
    }
    if (in_list(tag, BLOCK)) {
        fb_flush(b, false);
        fb_children(b, node, style);
        fb_flush(b, false);
        return;
    }
    fb_children(b, node, style_of(tag, style));
}

/* /^\s*other\b/i */
static bool starts_with_other(const char *s) {
    while (js_space(s)) s += js_space(s);
    if (strncasecmp(s, "other", 5)) return false;
    char c = s[5];
    return !(isalnum((unsigned char)c) || c == '_');
}

static void compute_questions(hf_form_t *f) {
    for (size_t si = 0; si < f->sections.n; si++) {
        hf_section_t *sec = &f->sections.v[si];
        char *title = hf_spans_text(&sec->title);
        char *caption = xstrdup("");
        for (size_t bi = 0; bi < sec->blocks.n; bi++) {
            hf_block_t *blk = &sec->blocks.v[bi];
            if (blk->kind == HF_B_FIELD) {
                hf_field_t *fl = blk->field;
                char *own = hf_spans_text(&fl->label);
                char *lone = fl->kind == HF_CHOICE && fl->opts.n == 1 ? hf_spans_text(&fl->opts.v[0].label) : xstrdup("");
                const char *q = *own ? own : *caption ? caption : *title ? title : *lone ? lone : fl->name;
                free(fl->question);
                fl->question = hf_one_line(q);
                free(own);
                free(lone);
                free(caption);
                caption = xstrdup("");
                continue;
            }
            free(caption);
            caption = blk->kind == HF_B_TEXT ? hf_spans_text(&blk->spans) : xstrdup("");
        }
        free(caption);
        free(title);
    }
    for (size_t i = 0; i < f->fields.n; i++)
        if (!f->fields.v[i]->question) f->fields.v[i]->question = xstrdup(f->fields.v[i]->name);
}

static void collect_labels(fb_t *b, const dnode_t *root) {
    dvec_t controls = {0}, labels = {0};
    dn_query_all(root, pred_control, NULL, &controls);
    dn_query_all(root, pred_tag, "label", &labels);
    for (size_t i = 0; i < labels.n; i++) {
        const char *target = dn_attr(labels.v[i], "for");
        if (!target || !*target) continue;
        bool known = false;
        for (size_t k = 0; k < controls.n && !known; k++) {
            const char *id = dn_attr(controls.v[k], "id");
            known = id && !strcmp(id, target);
        }
        bool taken = false;
        for (size_t k = 0; k < b->label_ids.n && !taken; k++) taken = !strcmp(b->label_ids.v[k], target);
        if (!known || taken) continue;
        strvec_push(&b->label_ids, target);
        vec_push(&b->label_els, labels.v[i]);
    }
    free(controls.v);
    free(labels.v);
}

static void block_free(hf_block_t *blk) {
    spans_free(&blk->spans);
    for (size_t i = 0; i < blk->items.n; i++) spans_free(&blk->items.v[i]);
    free(blk->items.v);
    free(blk->text);
    for (size_t r = 0; r < blk->rows.n; r++) {
        for (size_t c = 0; c < blk->rows.v[r].n; c++) spans_free(&blk->rows.v[r].v[c]);
        free(blk->rows.v[r].v);
    }
    free(blk->rows.v);
    free(blk->src);
    free(blk->alt);
    free(blk->markup);
}

hf_form_t *hf_parse(const char *html) {
    dnode_t *doc = html_parse(html ? html : "");
    const dnode_t *form = dn_query(doc, pred_tag, "form");
    fb_t b = {0};
    b.root = form ? form : doc;
    b.form = xcalloc(1, sizeof *b.form);
    hf_section_t first = {0};
    vec_push(&b.form->sections, first);
    b.src = html ? html : "";
    collect_labels(&b, b.root);
    fb_children(&b, b.root, HF_PLAIN);
    fb_flush(&b, false);
    for (size_t i = 0; i < b.form->fields.n; i++) {
        hf_field_t *f = b.form->fields.v[i];
        if (f->kind != HF_CHOICE || f->select || f->opts.n < 2) continue;
        bool own = false;
        for (size_t k = 0; k < f->opts.n && !own; k++) {
            char *t = hf_spans_text(&f->opts.v[k].label);
            own = starts_with_other(t);
            free(t);
        }
        f->other = !own;
    }
    /* drop sections with neither blocks nor a title */
    size_t k = 0;
    for (size_t i = 0; i < b.form->sections.n; i++) {
        hf_section_t *s = &b.form->sections.v[i];
        if (s->blocks.n || s->title.n) b.form->sections.v[k++] = *s;
        else {
            spans_free(&s->title);
            free(s->blocks.v);
        }
    }
    b.form->sections.n = k;
    compute_questions(b.form);
    spans_free(&b.inl);
    for (size_t i = 0; i < b.groups.n; i++) free(b.groups.v[i].key);
    free(b.groups.v);
    strvec_free(&b.label_ids);
    free(b.label_els.v);
    dn_free(doc);
    return b.form;
}

void hf_form_free(hf_form_t *f) {
    if (!f) return;
    for (size_t i = 0; i < f->sections.n; i++) {
        hf_section_t *s = &f->sections.v[i];
        spans_free(&s->title);
        for (size_t k = 0; k < s->blocks.n; k++) block_free(&s->blocks.v[k]);
        free(s->blocks.v);
    }
    free(f->sections.v);
    for (size_t i = 0; i < f->fields.n; i++) {
        hf_field_t *fl = f->fields.v[i];
        free(fl->name);
        spans_free(&fl->label);
        free(fl->question);
        for (size_t k = 0; k < fl->opts.n; k++) {
            free(fl->opts.v[k].value);
            spans_free(&fl->opts.v[k].label);
        }
        free(fl->opts.v);
        free(fl->type);
        free(fl->value);
        free(fl->placeholder);
        free(fl->pattern);
        free(fl);
    }
    free(f->fields.v);
    strvec_free(&f->hidden_names);
    strvec_free(&f->hidden_values);
    free(f);
}

size_t hf_named_count(const hf_form_t *f) {
    size_t n = 0;
    for (size_t i = 0; f && i < f->fields.n; i++) n += *f->fields.v[i]->name != 0;
    return n;
}

/* ======================================================================== */
/* data                                                                     */
/* ======================================================================== */

bool hf_is_array_choice(const hf_field_t *f) { return f->multi && f->opts.n > 1; }

json_t *hf_choice_value(const hf_field_t *f, const bool *checked, const char *other) {
    char *typed = f->other && other ? js_trim(other, true, true) : NULL;
    if (typed && !*typed) {
        free(typed);
        typed = NULL;
    }
    if (hf_is_array_choice(f)) {
        json_t *arr = json_arr();
        for (size_t i = 0; i < f->opts.n; i++)
            if (checked && checked[i] && !f->opts.v[i].disabled) json_push_str(arr, f->opts.v[i].value);
        if (typed) json_push(arr, json_str_own(typed));
        return arr;
    }
    if (typed) return json_str_own(typed);
    for (size_t i = 0; i < f->opts.n; i++)
        if (checked && checked[i] && !f->opts.v[i].disabled) return json_str(f->opts.v[i].value);
    return NULL;
}

json_t *hf_text_value(const hf_field_t *f, const char *text) {
    if ((str_eq(f->type, "number") || str_eq(f->type, "range")) && !js_blank(text)) {
        double v;
        if (js_number_finite(text, &v)) return json_num(v);
    }
    return json_str(text ? text : "");
}

/* URL.canParse, approximated: a scheme, and for http(s)/ws(s)/ftp a non-empty host with a valid port */
static bool url_can_parse(const char *text) {
    char *t = js_trim(text, true, true); /* leading/trailing C0 and spaces are stripped by the parser */
    const char *s = t;
    bool ok = false;
    if (isalpha((unsigned char)*s)) {
        const char *p = s + 1;
        while (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.') p++;
        if (*p == ':') {
            char *scheme = lower_dup(s, (size_t)(p - s));
            static const char *const SPECIAL[] = {"http", "https", "ws", "wss", "ftp", NULL};
            if (in_list(scheme, SPECIAL)) {
                const char *a = p + 1;
                while (*a == '/' || *a == '\\') a++;
                size_t alen = strcspn(a, "/?#\\");
                char *auth = xstrndup(a, alen);
                char *host = strrchr(auth, '@') ? strrchr(auth, '@') + 1 : auth;
                char *port = NULL;
                if (*host == '[') {
                    char *rb = strchr(host, ']');
                    ok = rb != NULL;
                    if (rb && rb[1] == ':') port = rb + 2;
                    else if (rb && rb[1]) ok = false;
                } else {
                    char *colon = strchr(host, ':');
                    if (colon) {
                        *colon = 0;
                        port = colon + 1;
                    }
                    ok = *host != 0 && !host[strcspn(host, " \t\n\r#%/:<>?@[\\]^|")];
                }
                if (ok && port && *port) {
                    long v = 0;
                    for (const char *q = port; *q && ok; q++) {
                        if (!isdigit((unsigned char)*q)) ok = false;
                        else if ((v = v * 10 + (*q - '0')) > 65535) ok = false;
                    }
                }
                free(auth);
            } else ok = true;
            free(scheme);
        }
    }
    free(t);
    return ok;
}

/* /^[^\s@]+@[^\s@]+$/ */
static bool email_like(const char *s) {
    const char *at = strchr(s, '@');
    if (!at || at == s || !at[1] || strchr(at + 1, '@')) return false;
    for (const char *p = s; *p;) {
        size_t w = js_space(p);
        if (w) return false;
        p++;
    }
    return true;
}

/* `^(?:pattern)$` as HTML matches `pattern`; -1 when the pattern does not compile */
static int pattern_matches(const char *pattern, const char *text) {
    char *full = xasprintf("^(?:%s)$", pattern);
    rx_t *rx = rx_compile(full, 0, NULL);
    int r = -1;
    if (rx) {
        r = rx_is_match(rx, text, strlen(text), 0);
        rx_free(rx);
    } else {
        rxbt_t *bt = rxbt_compile(full, 0, NULL);
        if (bt) {
            r = rxbt_search(bt, text, strlen(text), 0, NULL, 0);
            rxbt_free(bt);
        }
    }
    free(full);
    return r;
}

/* the first `n` UTF-16 units of s (owned) */
static char *utf16_prefix(const char *s, size_t n) {
    size_t units = 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (p[i]) {
        size_t len = p[i] < 0x80 ? 1 : p[i] < 0xE0 ? 2 : p[i] < 0xF0 ? 3 : 4;
        size_t u = len == 4 ? 2 : 1;
        if (units + u > n) break;
        units += u;
        for (size_t k = 0; k < len && p[i]; k++) i++; /* a truncated sequence never steps over the NUL */
    }
    return xstrndup(s, i);
}

char *hf_field_error(const hf_field_t *f, const json_t *value) {
    if (f->kind == HF_CHOICE) {
        bool empty = !value || (json_is_arr(value) && !json_len(value));
        if (!f->required || !empty) return NULL;
        return xstrdup(hf_is_array_choice(f) ? "Select at least one" : "Required");
    }
    char *text;
    if (!value) text = xstrdup("");
    else if (json_is_str(value)) text = xstrdup(json_sv(value));
    else if (json_is_num(value)) text = js_num_str(value->u.num.d);
    else text = xstrdup("");
    char *err = NULL;
    if (js_blank(text)) {
        err = f->required ? xstrdup("Required") : NULL;
        free(text);
        return err;
    }
    if (str_eq(f->type, "number") || str_eq(f->type, "range")) {
        double v;
        if (!js_number_finite(text, &v)) err = xstrdup("Enter a number");
        else if (f->has_min && v < f->min) {
            char *m = js_num_str(f->min);
            err = xasprintf("Must be at least %s", m);
            free(m);
        } else if (f->has_max && v > f->max) {
            char *m = js_num_str(f->max);
            err = xasprintf("Must be at most %s", m);
            free(m);
        }
        if (err) {
            free(text);
            return err;
        }
    }
    double len = (double)hf_utf16_len(text);
    if (f->has_minlength && len < f->minlength) {
        char *m = js_num_str(f->minlength);
        err = xasprintf("Needs at least %s characters", m);
        free(m);
    } else if (f->has_maxlength && len > f->maxlength) {
        char *m = js_num_str(f->maxlength);
        err = xasprintf("At most %s characters", m);
        free(m);
    } else if (str_eq(f->type, "email") && !email_like(text)) err = xstrdup("Enter an email address");
    else if (str_eq(f->type, "url") && !url_can_parse(text)) err = xstrdup("Enter a full URL");
    else if (f->pattern && *f->pattern && pattern_matches(f->pattern, text) == 0) {
        if (hf_utf16_len(f->pattern) > 40) {
            char *p = utf16_prefix(f->pattern, 39);
            err = xasprintf("Must match %s\xe2\x80\xa6", p);
            free(p);
        } else err = xasprintf("Must match %s", f->pattern);
    }
    free(text);
    return err;
}

json_t *hf_default_data(const hf_form_t *f) {
    json_t *data = json_obj();
    for (size_t i = 0; i < f->hidden_names.n; i++) json_set_str(data, f->hidden_names.v[i], f->hidden_values.v[i]);
    for (size_t i = 0; i < f->fields.n; i++) {
        const hf_field_t *fl = f->fields.v[i];
        if (!*fl->name) continue;
        json_t *v;
        if (fl->kind == HF_TEXT) v = hf_text_value(fl, fl->value);
        else {
            bool *checked = xcalloc(fl->opts.n + 1, sizeof *checked);
            for (size_t k = 0; k < fl->opts.n; k++) checked[k] = fl->opts.v[k].checked;
            v = hf_choice_value(fl, checked, NULL);
            free(checked);
        }
        if (v) json_set(data, fl->name, v);
    }
    return data;
}

static char *answer_label(const hf_field_t *f, const char *raw) {
    for (size_t i = 0; i < f->opts.n; i++)
        if (!strcmp(f->opts.v[i].value, raw)) {
            char *t = hf_spans_text(&f->opts.v[i].label);
            char *o = hf_one_line(t);
            free(t);
            if (*o) return o;
            free(o);
            return xstrdup(raw);
        }
    return f->other ? xasprintf("%s: %s", HF_OTHER_LABEL, raw) : xstrdup(raw);
}

static char *answer_text(const hf_field_t *f, const json_t *value) {
    if (!value) return xstrdup("");
    if (f->kind == HF_TEXT) {
        if (json_is_str(value)) return xstrdup(json_sv(value));
        if (json_is_num(value)) return js_num_str(value->u.num.d);
        return xstrdup("");
    }
    if (json_is_arr(value)) {
        sb_t b = SB_INIT;
        const json_t *x;
        json_arr_foreach(value, i, x) {
            char *l = answer_label(f, json_is_str(x) ? json_sv(x) : "");
            if (i) sb_append(&b, ", ");
            sb_append(&b, l);
            free(l);
        }
        return sb_detach(&b);
    }
    if (json_is_num(value)) {
        char *s = js_num_str(value->u.num.d);
        char *r = answer_label(f, s);
        free(s);
        return r;
    }
    return answer_label(f, json_is_str(value) ? json_sv(value) : "");
}

char *hf_block_text(const hf_block_t *b) {
    sb_t l = SB_INIT;
    switch (b->kind) {
    case HF_B_TEXT: return hf_spans_text(&b->spans);
    case HF_B_PRE: return xstrdup(b->text ? b->text : "");
    case HF_B_LIST:
        for (size_t i = 0; i < b->items.n; i++) {
            char *it = hf_spans_text(&b->items.v[i]);
            if (i) sb_putc(&l, '\n');
            if (b->ordered) sb_appendf(&l, "%zu. %s", i + 1, it);
            else sb_appendf(&l, "- %s", it);
            free(it);
        }
        return sb_detach(&l);
    case HF_B_TABLE:
        for (size_t r = 0; r < b->rows.n; r++) {
            if (r) sb_putc(&l, '\n');
            for (size_t c = 0; c < b->rows.v[r].n; c++) {
                char *ct = hf_spans_text(&b->rows.v[r].v[c]);
                if (c) sb_append(&l, " | ");
                sb_append(&l, ct);
                free(ct);
            }
        }
        return sb_detach(&l);
    default: return NULL;
    }
}

static bool blank_text(const char *t) {
    for (; t && *t; t++)
        if (!isspace((unsigned char)*t)) return false;
    return true;
}

/* oneLine(text) === question */
static bool is_question(const char *text, const char *question) {
    if (!question) return false;
    char *o = hf_one_line(text);
    bool eq = !strcmp(o, question);
    free(o);
    return eq;
}

strvec_t hf_form_context(const hf_form_t *f) {
    strvec_t parts = {0};
    for (size_t si = 0; si < f->sections.n; si++) {
        const hf_section_t *sec = &f->sections.v[si];
        for (size_t bi = 0; bi <= sec->blocks.n; bi++) {
            char *text;
            if (bi == 0) {
                char *t = hf_spans_text(&sec->title);
                text = str_trim(t);
                free(t);
            } else text = hf_block_text(&sec->blocks.v[bi - 1]);
            bool keep = text && !blank_text(text);
            for (size_t k = 0; keep && k < f->fields.n; k++)
                if (*f->fields.v[k]->name && is_question(text, f->fields.v[k]->question)) keep = false;
            if (keep) strvec_push_own(&parts, text);
            else free(text);
        }
    }
    return parts;
}

static char *join_parts(const strvec_t *v) {
    sb_t b = SB_INIT;
    for (size_t i = 0; i < v->n; i++) {
        if (i) sb_append(&b, "\n\n");
        sb_append(&b, v->v[i]);
    }
    return sb_detach(&b);
}

char **hf_field_contexts(const hf_form_t *f) {
    size_t n = f->fields.n;
    strvec_t *parts = xcalloc(n + 1, sizeof *parts);
    bool *named = xcalloc(n + 1, sizeof *named);
    strvec_t pending = {0};
    strvec_t *last = NULL;
    for (size_t si = 0; si < f->sections.n; si++) {
        const hf_section_t *sec = &f->sections.v[si];
        char *t = hf_spans_text(&sec->title), *title = str_trim(t);
        free(t);
        if (*title) strvec_push(&pending, title);
        free(title);
        /* a group with text between its options keeps that text until owner_end, unless a named field sits among
         * those options: the text then stays with the field it precedes */
        strvec_t *owner = NULL;
        size_t owner_end = 0;
        for (size_t bi = 0; bi < sec->blocks.n; bi++) {
            const hf_block_t *b = &sec->blocks.v[bi];
            if (b->kind == HF_B_FIELD) {
                if (!*b->field->name) continue;
                size_t fi = b->field->index;
                strvec_t *own = &parts[fi];
                named[fi] = true;
                for (size_t k = 0; k < pending.n; k++)
                    if (!is_question(pending.v[k], b->field->question)) strvec_push(own, pending.v[k]);
                strvec_free(&pending);
                last = own;
                owner = own;
                size_t end = b->options_end;
                bool intervening = false;
                for (size_t at = bi + 1; at < end && at < sec->blocks.n; at++)
                    if (sec->blocks.v[at].kind == HF_B_FIELD && *sec->blocks.v[at].field->name) intervening = true;
                owner_end = intervening ? 0 : end;
                continue;
            }
            char *text = hf_block_text(b);
            if (!text || blank_text(text)) {
                free(text);
                continue;
            }
            if (owner && bi < owner_end) strvec_push_own(owner, text);
            else strvec_push_own(&pending, text);
        }
    }
    if (last)
        for (size_t k = 0; k < pending.n; k++) strvec_push(last, pending.v[k]);
    strvec_free(&pending);
    char **out = xcalloc(n + 1, sizeof *out);
    for (size_t i = 0; i < n; i++) {
        if (named[i]) out[i] = join_parts(&parts[i]);
        strvec_free(&parts[i]);
    }
    free(parts);
    free(named);
    return out;
}

json_t *hf_answers(const hf_form_t *f, const json_t *data) {
    json_t *out = json_arr();
    for (size_t i = 0; i < f->fields.n; i++) {
        const hf_field_t *fl = f->fields.v[i];
        if (!*fl->name) continue;
        json_t *a = json_obj();
        json_set_str(a, "name", fl->name);
        json_set_str(a, "question", fl->question ? fl->question : fl->name);
        json_set(a, "answer", json_str_own(answer_text(fl, json_get(data, fl->name))));
        json_push(out, a);
    }
    return out;
}

static void pretty(sb_t *b, const json_t *v, int depth) {
    if (json_is_obj(v)) {
        if (!json_len(v)) {
            sb_append(b, "{}");
            return;
        }
        sb_append(b, "{\n");
        for (size_t i = 0; i < v->u.obj.n; i++) {
            sb_repeat(b, "  ", depth + 1);
            json_js_quote_sb(b, v->u.obj.keys[i], strlen(v->u.obj.keys[i]));
            sb_append(b, ": ");
            pretty(b, v->u.obj.vals[i], depth + 1);
            if (i + 1 < v->u.obj.n) sb_putc(b, ',');
            sb_putc(b, '\n');
        }
        sb_repeat(b, "  ", depth);
        sb_putc(b, '}');
        return;
    }
    if (json_is_arr(v)) {
        if (!json_len(v)) {
            sb_append(b, "[]");
            return;
        }
        sb_append(b, "[\n");
        for (size_t i = 0; i < json_len(v); i++) {
            sb_repeat(b, "  ", depth + 1);
            pretty(b, json_at(v, i), depth + 1);
            if (i + 1 < json_len(v)) sb_putc(b, ',');
            sb_putc(b, '\n');
        }
        sb_repeat(b, "  ", depth);
        sb_putc(b, ']');
        return;
    }
    json_dump_js_sb(b, v);
}

char *hf_data_json(const json_t *data) {
    sb_t b = SB_INIT;
    pretty(&b, data, 0);
    return sb_detach(&b);
}

/* ======================================================================== */
/* pagination                                                               */
/* ======================================================================== */

int hf_page_height(const hf_atoms_t *a, size_t start, size_t end) {
    int used = start < a->n && a->v[start].continuation ? 1 : 0;
    for (size_t i = start; i < end && i < a->n; i++) used += (i == start ? 0 : a->v[i].gap) + (int)a->v[i].lines.n;
    return used;
}

static double break_score(int rank, int used, int budget) {
    double bonus = rank == HF_BREAK_SECTION ? 0.5 : rank == HF_BREAK_BLOCK ? 0.25 : 0;
    return bonus - (double)(budget - used) / budget;
}

static size_t best_break(const hf_atoms_t *a, size_t start, size_t end, int budget) {
    size_t best = end;
    double best_score = -INFINITY;
    for (size_t c = end; c > start; c--) {
        int rank = a->v[c].break_rank;
        if (rank == HF_BREAK_NEVER) continue;
        double score = break_score(rank, hf_page_height(a, start, c), budget);
        if (score > best_score) {
            best = c;
            best_score = score;
        }
    }
    return best;
}

void hf_paginate(const hf_atoms_t *a, int rows, hf_pages_t *out) {
    int budget = MAX(1, rows);
    out->n = 0;
    size_t start = 0;
    while (start < a->n) {
        size_t end = start + 1;
        while (end < a->n && hf_page_height(a, start, end + 1) <= budget) end++;
        if (end < a->n) end = best_break(a, start, end, budget);
        hf_page_t p = {start, end};
        vec_push(out, p);
        start = end;
    }
    if (!out->n) {
        hf_page_t p = {0, 0};
        vec_push(out, p);
    }
}

void hf_atoms_free(hf_atoms_t *a) {
    for (size_t i = 0; i < a->n; i++) {
        strvec_free(&a->v[i].lines);
        free(a->v[i].continuation);
    }
    free(a->v);
    memset(a, 0, sizeof *a);
}
