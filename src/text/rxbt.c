#include "rxbt.h"

#include "regex.h"

#include <ctype.h>

#define BT_MAX_PROG 200000
#define BT_STEP_BUDGET 20000000L

enum {
    I_CHAR,     /* cp */
    I_ANY,      /* any except \n unless dotall */
    I_ANYNL,    /* any */
    I_CLASS,    /* class index, negate */
    I_SPLIT,    /* x (preferred), y */
    I_JMP,
    I_SAVE,     /* slot */
    I_ASSERT,   /* kind */
    I_LOOK,     /* sub-program [x,y), kind: 0 ahead 1 ahead-neg 2 behind 3 behind-neg */
    I_BACKREF,  /* group, icase */
    I_MATCH,
};

enum { AS_BOL, AS_EOL, AS_BOT, AS_EOT, AS_WORDB, AS_NWORDB };

typedef struct {
    uint32_t lo, hi;
} brange_t;

typedef struct {
    VEC(brange_t) r;
} bclass_t;

typedef struct {
    int op;
    uint32_t cp;
    int x, y;
    int arg;
    bool icase;
} binst_t;

struct rxbt {
    VEC(binst_t) prog;
    VEC(bclass_t) classes;
    int ngroups;
    strvec_t names; /* index = group */
};

/* ---------- parser -> program ---------- */

typedef struct {
    const char *s;
    size_t i, n;
    rxbt_t *r;
    bool icase, multi, dotall, ext, ungreedy;
    char *err;
    int depth;
} bp_t;

static int emit(bp_t *p, int op) {
    binst_t in = {.op = op};
    if (p->r->prog.n >= BT_MAX_PROG) {
        if (!p->err) p->err = xstrdup("pattern too large");
        return (int)p->r->prog.n - 1;
    }
    vec_push(&p->r->prog, in);
    return (int)p->r->prog.n - 1;
}

static binst_t *I(bp_t *p, int pc) { return &p->r->prog.v[pc]; }

static void perr(bp_t *p, const char *m) {
    if (!p->err) p->err = xasprintf("regex parse error at position %zu: %s", p->i, m);
}

static uint32_t next_cp(bp_t *p) {
    uint32_t cp;
    int k = utf8_decode(p->s + p->i, p->n - p->i, &cp);
    p->i += (size_t)(k ? k : 1);
    return cp;
}

static void class_add(bclass_t *c, uint32_t lo, uint32_t hi) {
    brange_t r = {lo, hi};
    vec_push(&c->r, r);
}

static void add_perl(bclass_t *c, char k) {
    switch (k) {
    case 'd': class_add(c, '0', '9'); break;
    case 'w':
        class_add(c, '0', '9');
        class_add(c, 'A', 'Z');
        class_add(c, 'a', 'z');
        class_add(c, '_', '_');
        class_add(c, 0x80, 0x10FFFF); /* approximate: non-ASCII letters count as word chars */
        break;
    case 's':
        class_add(c, ' ', ' ');
        class_add(c, '\t', '\r');
        break;
    }
}

static void add_negated(bclass_t *dst, const bclass_t *src) {
    /* complement of src (sorted by brute force over ranges) */
    uint32_t cur = 0;
    VEC(brange_t) sorted = {0};
    for (size_t i = 0; i < src->r.n; i++) vec_push(&sorted, src->r.v[i]);
    for (size_t i = 1; i < sorted.n; i++)
        for (size_t j = i; j > 0 && sorted.v[j].lo < sorted.v[j - 1].lo; j--) {
            brange_t t = sorted.v[j];
            sorted.v[j] = sorted.v[j - 1];
            sorted.v[j - 1] = t;
        }
    for (size_t i = 0; i < sorted.n; i++) {
        if (sorted.v[i].lo > cur) class_add(dst, cur, sorted.v[i].lo - 1);
        if (sorted.v[i].hi + 1 > cur) cur = sorted.v[i].hi + 1;
    }
    if (cur <= 0x10FFFF) class_add(dst, cur, 0x10FFFF);
    vec_free(&sorted);
}

static int new_class(bp_t *p) {
    bclass_t c = {0};
    vec_push(&p->r->classes, c);
    return (int)p->r->classes.n - 1;
}

static uint32_t parse_escape_cp(bp_t *p, char e) {
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'a': return 7;
    case 'e': return 27;
    case 'x': {
        uint32_t v = 0;
        if (p->i < p->n && p->s[p->i] == '{') {
            p->i++;
            while (p->i < p->n && p->s[p->i] != '}') v = v * 16 + (uint32_t)(isdigit((unsigned char)p->s[p->i]) ? p->s[p->i] - '0' : (tolower((unsigned char)p->s[p->i]) - 'a' + 10)), p->i++;
            p->i++;
        } else {
            for (int k = 0; k < 2 && p->i < p->n && isxdigit((unsigned char)p->s[p->i]); k++, p->i++)
                v = v * 16 + (uint32_t)(isdigit((unsigned char)p->s[p->i]) ? p->s[p->i] - '0' : (tolower((unsigned char)p->s[p->i]) - 'a' + 10));
        }
        return v;
    }
    default: return (uint32_t)(unsigned char)e;
    }
}

static bool parse_bracket(bp_t *p, int ci) {
    /* p->i is after '[' */
    bool neg = false;
    if (p->i < p->n && p->s[p->i] == '^') {
        neg = true;
        p->i++;
    }
    bclass_t tmp = {0};
    bool first = true;
    while (p->i < p->n && (p->s[p->i] != ']' || first)) {
        first = false;
        if (p->s[p->i] == '[' && p->i + 1 < p->n && p->s[p->i + 1] == ':') {
            const char *e = strstr(p->s + p->i, ":]");
            if (e) {
                char nm[16] = {0};
                size_t l = (size_t)(e - (p->s + p->i + 2));
                if (l < sizeof nm) memcpy(nm, p->s + p->i + 2, l);
                bool pneg = nm[0] == '^';
                bclass_t pc = {0};
                const char *n = pneg ? nm + 1 : nm;
                if (!strcmp(n, "alpha")) { class_add(&pc, 'a', 'z'); class_add(&pc, 'A', 'Z'); }
                else if (!strcmp(n, "digit")) class_add(&pc, '0', '9');
                else if (!strcmp(n, "alnum")) { class_add(&pc, 'a', 'z'); class_add(&pc, 'A', 'Z'); class_add(&pc, '0', '9'); }
                else if (!strcmp(n, "space")) { class_add(&pc, ' ', ' '); class_add(&pc, '\t', '\r'); }
                else if (!strcmp(n, "upper")) class_add(&pc, 'A', 'Z');
                else if (!strcmp(n, "lower")) class_add(&pc, 'a', 'z');
                else if (!strcmp(n, "punct")) { class_add(&pc, '!', '/'); class_add(&pc, ':', '@'); class_add(&pc, '[', '`'); class_add(&pc, '{', '~'); }
                else if (!strcmp(n, "xdigit")) { class_add(&pc, '0', '9'); class_add(&pc, 'a', 'f'); class_add(&pc, 'A', 'F'); }
                else if (!strcmp(n, "word")) add_perl(&pc, 'w');
                if (pneg) add_negated(&tmp, &pc);
                else for (size_t k = 0; k < pc.r.n; k++) vec_push(&tmp.r, pc.r.v[k]);
                vec_free(&pc.r);
                p->i = (size_t)(e - p->s) + 2;
                continue;
            }
        }
        uint32_t lo;
        if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
            p->i++;
            char e = p->s[p->i++];
            if (strchr("dwsDWS", e)) {
                bclass_t pc = {0};
                add_perl(&pc, (char)tolower((unsigned char)e));
                if (isupper((unsigned char)e)) add_negated(&tmp, &pc);
                else for (size_t k = 0; k < pc.r.n; k++) vec_push(&tmp.r, pc.r.v[k]);
                vec_free(&pc.r);
                continue;
            }
            lo = parse_escape_cp(p, e);
        } else {
            lo = next_cp(p);
        }
        uint32_t hi = lo;
        if (p->i + 1 < p->n && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {
            p->i++;
            if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
                p->i++;
                hi = parse_escape_cp(p, p->s[p->i++]);
            } else {
                hi = next_cp(p);
            }
        }
        class_add(&tmp, lo, hi);
    }
    if (p->i >= p->n) {
        vec_free(&tmp.r);
        perr(p, "unclosed character class");
        return false;
    }
    p->i++;
    bclass_t *dst = &p->r->classes.v[ci];
    if (neg) add_negated(dst, &tmp);
    else for (size_t k = 0; k < tmp.r.n; k++) vec_push(&dst->r, tmp.r.v[k]);
    vec_free(&tmp.r);
    return true;
}

static void parse_alt(bp_t *p);

/* copy instructions [a,b) to the end, relocating jumps */
static void dup_range(bp_t *p, int a, int b) {
    int off = (int)p->r->prog.n - a;
    for (int k = a; k < b; k++) {
        binst_t in = p->r->prog.v[k];
        if (in.op == I_SPLIT || in.op == I_JMP || in.op == I_LOOK) {
            in.x += off;
            if (in.op != I_JMP) in.y += off;
        }
        if (p->r->prog.n >= BT_MAX_PROG) {
            perr(p, "pattern too large");
            return;
        }
        vec_push(&p->r->prog, in);
    }
}

static void parse_atom(bp_t *p, bool *empty) {
    *empty = false;
    char c = p->s[p->i];
    if (c == '(') {
        p->i++;
        int group = -1;
        int look = -1;
        bool saved_i = p->icase, saved_m = p->multi, saved_d = p->dotall, saved_x = p->ext;
        if (p->i < p->n && p->s[p->i] == '?') {
            p->i++;
            char k = p->i < p->n ? p->s[p->i] : 0;
            if (k == ':') p->i++;
            else if (k == '=') { look = 0; p->i++; }
            else if (k == '!') { look = 1; p->i++; }
            else if (k == '<' && p->i + 1 < p->n && (p->s[p->i + 1] == '=' || p->s[p->i + 1] == '!')) {
                look = p->s[p->i + 1] == '=' ? 2 : 3;
                p->i += 2;
            } else if (k == 'P' || k == '<') {
                if (k == 'P') p->i++;
                if (p->s[p->i] != '<') { perr(p, "bad group name"); return; }
                p->i++;
                const char *e = memchr(p->s + p->i, '>', p->n - p->i);
                if (!e) { perr(p, "unclosed group name"); return; }
                group = p->r->ngroups++;
                while ((int)p->r->names.n < group) strvec_push(&p->r->names, "");
                strvec_push_own(&p->r->names, xstrndup(p->s + p->i, (size_t)(e - (p->s + p->i))));
                p->i = (size_t)(e - p->s) + 1;
            } else {
                /* flags: (?imsx-imsx) or (?flags:...) */
                bool on = true, scoped = false;
                while (p->i < p->n && p->s[p->i] != ')' && p->s[p->i] != ':') {
                    char f = p->s[p->i++];
                    if (f == '-') on = false;
                    else if (f == 'i') p->icase = on;
                    else if (f == 'm') p->multi = on;
                    else if (f == 's') p->dotall = on;
                    else if (f == 'x') p->ext = on;
                    else if (f == 'U') p->ungreedy = on;
                    else { perr(p, "unknown flag"); return; }
                }
                if (p->i < p->n && p->s[p->i] == ':') {
                    scoped = true;
                    p->i++;
                } else {
                    p->i++; /* ')' : flags apply to rest of enclosing group */
                    *empty = true;
                    return;
                }
                (void)scoped;
            }
        } else {
            group = p->r->ngroups++;
            while ((int)p->r->names.n <= group) strvec_push(&p->r->names, "");
        }
        if (look >= 0) {
            int li = emit(p, I_LOOK);
            int start = (int)p->r->prog.n;
            parse_alt(p);
            emit(p, I_MATCH);
            I(p, li)->x = start;
            I(p, li)->y = (int)p->r->prog.n;
            I(p, li)->arg = look;
        } else if (group >= 0) {
            int s0 = emit(p, I_SAVE);
            I(p, s0)->arg = 2 * group;
            parse_alt(p);
            int s1 = emit(p, I_SAVE);
            I(p, s1)->arg = 2 * group + 1;
        } else {
            parse_alt(p);
        }
        if (p->i >= p->n || p->s[p->i] != ')') {
            perr(p, "unclosed group");
            return;
        }
        p->i++;
        p->icase = saved_i;
        p->multi = saved_m;
        p->dotall = saved_d;
        p->ext = saved_x;
        return;
    }
    if (c == '[') {
        p->i++;
        int ci = new_class(p);
        if (!parse_bracket(p, ci)) return;
        int in = emit(p, I_CLASS);
        I(p, in)->arg = ci;
        I(p, in)->icase = p->icase;
        return;
    }
    if (c == '.') {
        p->i++;
        emit(p, p->dotall ? I_ANYNL : I_ANY);
        return;
    }
    if (c == '^' || c == '$') {
        p->i++;
        int in = emit(p, I_ASSERT);
        I(p, in)->arg = c == '^' ? (p->multi ? AS_BOL : AS_BOT) : (p->multi ? AS_EOL : AS_EOT);
        return;
    }
    if (c == '\\' && p->i + 1 < p->n) {
        p->i++;
        char e = p->s[p->i++];
        if (e == 'b' || e == 'B') {
            int in = emit(p, I_ASSERT);
            I(p, in)->arg = e == 'b' ? AS_WORDB : AS_NWORDB;
            return;
        }
        if (e == 'A' || e == 'z' || e == 'Z') {
            int in = emit(p, I_ASSERT);
            I(p, in)->arg = e == 'A' ? AS_BOT : AS_EOT;
            return;
        }
        if (strchr("dwsDWS", e)) {
            int ci = new_class(p);
            bclass_t pc = {0};
            add_perl(&pc, (char)tolower((unsigned char)e));
            if (isupper((unsigned char)e)) add_negated(&p->r->classes.v[ci], &pc);
            else for (size_t k = 0; k < pc.r.n; k++) vec_push(&p->r->classes.v[ci].r, pc.r.v[k]);
            vec_free(&pc.r);
            int in = emit(p, I_CLASS);
            I(p, in)->arg = ci;
            return;
        }
        if (e >= '1' && e <= '9') {
            int g = e - '0';
            while (p->i < p->n && isdigit((unsigned char)p->s[p->i]) && g * 10 + (p->s[p->i] - '0') < p->r->ngroups) g = g * 10 + (p->s[p->i++] - '0');
            int in = emit(p, I_BACKREF);
            I(p, in)->arg = g;
            I(p, in)->icase = p->icase;
            return;
        }
        if (e == 'k' && p->i < p->n && p->s[p->i] == '<') {
            const char *end = memchr(p->s + p->i, '>', p->n - p->i);
            if (!end) { perr(p, "bad \\k"); return; }
            char *nm = xstrndup(p->s + p->i + 1, (size_t)(end - p->s - p->i - 1));
            p->i = (size_t)(end - p->s) + 1;
            int g = -1;
            for (size_t k = 0; k < p->r->names.n; k++)
                if (!strcmp(p->r->names.v[k], nm)) g = (int)k;
            free(nm);
            if (g < 0) { perr(p, "unknown group name"); return; }
            int in = emit(p, I_BACKREF);
            I(p, in)->arg = g;
            I(p, in)->icase = p->icase;
            return;
        }
        int in = emit(p, I_CHAR);
        I(p, in)->cp = parse_escape_cp(p, e);
        I(p, in)->icase = p->icase;
        return;
    }
    uint32_t cp = next_cp(p);
    int in = emit(p, I_CHAR);
    I(p, in)->cp = cp;
    I(p, in)->icase = p->icase;
}

static void skip_ext(bp_t *p) {
    if (!p->ext) return;
    while (p->i < p->n) {
        if (isspace((unsigned char)p->s[p->i])) p->i++;
        else if (p->s[p->i] == '#') {
            while (p->i < p->n && p->s[p->i] != '\n') p->i++;
        } else break;
    }
}

static void parse_concat(bp_t *p) {
    if (++p->depth > 200) {
        perr(p, "nesting too deep");
        return;
    }
    for (;;) {
        skip_ext(p);
        if (p->err || p->i >= p->n || p->s[p->i] == '|' || p->s[p->i] == ')') break;
        int start = (int)p->r->prog.n;
        bool empty;
        parse_atom(p, &empty);
        if (p->err || empty) continue;
        int end = (int)p->r->prog.n;
        skip_ext(p);
        if (p->i >= p->n) break;
        char q = p->s[p->i];
        int mn = -1, mx = -1;
        if (q == '*') { mn = 0; mx = -1; p->i++; }
        else if (q == '+') { mn = 1; mx = -1; p->i++; }
        else if (q == '?') { mn = 0; mx = 1; p->i++; }
        else if (q == '{') {
            size_t save = p->i;
            p->i++;
            char *e;
            long a = strtol(p->s + p->i, &e, 10);
            if (e == p->s + p->i) { p->i = save; continue; }
            p->i = (size_t)(e - p->s);
            long b = a;
            if (p->s[p->i] == ',') {
                p->i++;
                if (p->s[p->i] == '}') b = -1;
                else { b = strtol(p->s + p->i, &e, 10); p->i = (size_t)(e - p->s); }
            }
            if (p->s[p->i] != '}') { p->i = save; continue; }
            p->i++;
            if (a > 1000 || b > 1000) { perr(p, "repetition count too large"); return; }
            mn = (int)a;
            mx = (int)b;
        } else continue;
        bool lazy = false;
        if (p->i < p->n && p->s[p->i] == '?') { lazy = true; p->i++; }
        else if (p->i < p->n && p->s[p->i] == '+') p->i++; /* possessive: treated greedy */
        if (p->ungreedy) lazy = !lazy;
        /* move the atom body out, then rebuild: body x mn, then optional parts */
        int blen = end - start;
        binst_t *body = xmalloc(sizeof(binst_t) * (size_t)(blen ? blen : 1));
        memcpy(body, p->r->prog.v + start, sizeof(binst_t) * (size_t)blen);
        p->r->prog.n = (size_t)start;
        /* helper: append body relocated to current end */
        #define APPEND_BODY()                                                                     \
            do {                                                                                  \
                int off_ = (int)p->r->prog.n - start;                                            \
                for (int k_ = 0; k_ < blen; k_++) {                                               \
                    binst_t in_ = body[k_];                                                       \
                    if (in_.op == I_SPLIT || in_.op == I_JMP || in_.op == I_LOOK) {              \
                        in_.x += off_;                                                            \
                        if (in_.op != I_JMP) in_.y += off_;                                       \
                    }                                                                             \
                    if (p->r->prog.n >= BT_MAX_PROG) { perr(p, "pattern too large"); break; }    \
                    vec_push(&p->r->prog, in_);                                                   \
                }                                                                                 \
            } while (0)
        for (int k = 0; k < mn && !p->err; k++) APPEND_BODY();
        if (mx < 0) {
            /* L: split body, out; body; jmp L */
            int l = emit(p, I_SPLIT);
            APPEND_BODY();
            int j = emit(p, I_JMP);
            I(p, j)->x = l;
            int out = (int)p->r->prog.n;
            I(p, l)->x = lazy ? out : l + 1;
            I(p, l)->y = lazy ? l + 1 : out;
        } else {
            int opt = mx - mn;
            VEC(int) splits = {0};
            for (int k = 0; k < opt && !p->err; k++) {
                int sp = emit(p, I_SPLIT);
                vec_push(&splits, sp);
                APPEND_BODY();
            }
            int out = (int)p->r->prog.n;
            for (size_t k = 0; k < splits.n; k++) {
                int sp = splits.v[k];
                I(p, sp)->x = lazy ? out : sp + 1;
                I(p, sp)->y = lazy ? sp + 1 : out;
            }
            vec_free(&splits);
        }
        #undef APPEND_BODY
        free(body);
        UNUSED(dup_range);
    }
    p->depth--;
}

static void parse_alt(bp_t *p) {
    int start = (int)p->r->prog.n;
    parse_concat(p);
    if (p->i < p->n && p->s[p->i] == '|' && !p->err) {
        /* split L1, L2; L1: first; jmp end; L2: rest */
        p->i++;
        int blen = (int)p->r->prog.n - start;
        binst_t *first = xmalloc(sizeof(binst_t) * (size_t)(blen ? blen : 1));
        memcpy(first, p->r->prog.v + start, sizeof(binst_t) * (size_t)blen);
        p->r->prog.n = (size_t)start;
        int sp = emit(p, I_SPLIT);
        for (int k = 0; k < blen; k++) {
            binst_t in = first[k];
            if (in.op == I_SPLIT || in.op == I_JMP || in.op == I_LOOK) {
                in.x += 1;
                if (in.op != I_JMP) in.y += 1;
            }
            vec_push(&p->r->prog, in);
        }
        free(first);
        int j = emit(p, I_JMP);
        I(p, sp)->x = sp + 1;
        I(p, sp)->y = (int)p->r->prog.n;
        parse_alt(p);
        I(p, j)->x = (int)p->r->prog.n;
    }
}

rxbt_t *rxbt_compile(const char *pattern, int flags, char **err) {
    rxbt_t *r = xcalloc(1, sizeof(*r));
    r->ngroups = 1;
    strvec_push(&r->names, "");
    bp_t p = {.s = pattern, .n = strlen(pattern), .r = r, .icase = flags & RX_ICASE, .multi = flags & RX_MULTILINE,
              .dotall = flags & RX_DOTALL, .ext = flags & RX_EXTENDED, .ungreedy = flags & RX_UNGREEDY};
    int s0 = emit(&p, I_SAVE);
    I(&p, s0)->arg = 0;
    parse_alt(&p);
    if (!p.err && p.i < p.n) perr(&p, "unmatched ')'");
    int s1 = emit(&p, I_SAVE);
    I(&p, s1)->arg = 1;
    emit(&p, I_MATCH);
    if (p.err) {
        if (err) *err = p.err;
        else free(p.err);
        rxbt_free(r);
        return NULL;
    }
    return r;
}

void rxbt_free(rxbt_t *r) {
    if (!r) return;
    for (size_t i = 0; i < r->classes.n; i++) vec_free(&r->classes.v[i].r);
    vec_free(&r->classes);
    vec_free(&r->prog);
    strvec_free(&r->names);
    free(r);
}

int rxbt_ngroups(const rxbt_t *r) { return r->ngroups; }

int rxbt_group_index(const rxbt_t *r, const char *name) {
    for (size_t i = 0; i < r->names.n; i++)
        if (*r->names.v[i] && !strcmp(r->names.v[i], name)) return (int)i;
    return -1;
}

const char *rxbt_group_name(const rxbt_t *r, int i) {
    return i >= 0 && (size_t)i < r->names.n && *r->names.v[i] ? r->names.v[i] : NULL;
}

/* ---------- matcher ---------- */

typedef struct {
    const rxbt_t *r;
    const char *s;
    size_t len;
    long steps;
    ptrdiff_t *caps;
    int ncaps;
} bm_t;

static uint32_t fold(uint32_t c) {
    if (c < 128) return (uint32_t)tolower((int)c);
    return c;
}

static bool class_has(const bclass_t *c, uint32_t cp, bool icase) {
    for (size_t i = 0; i < c->r.n; i++) {
        if (cp >= c->r.v[i].lo && cp <= c->r.v[i].hi) return true;
        if (icase && cp < 128) {
            uint32_t a = (uint32_t)tolower((int)cp), b = (uint32_t)toupper((int)cp);
            if ((a >= c->r.v[i].lo && a <= c->r.v[i].hi) || (b >= c->r.v[i].lo && b <= c->r.v[i].hi)) return true;
        }
    }
    return false;
}

static bool is_word_at(const bm_t *m, size_t pos, bool before) {
    if (before) {
        if (pos == 0) return false;
        unsigned char c = (unsigned char)m->s[pos - 1];
        return c >= 0x80 || isalnum(c) || c == '_';
    }
    if (pos >= m->len) return false;
    unsigned char c = (unsigned char)m->s[pos];
    return c >= 0x80 || isalnum(c) || c == '_';
}

typedef struct {
    int pc;
    size_t pos;
    int slot;       /* >=0: restore caps[slot] = val */
    ptrdiff_t val;
} frame_t;

static bool run(bm_t *m, int pc0, size_t pos0, size_t *end_out, int depth);

static bool run(bm_t *m, int pc0, size_t pos0, size_t *end_out, int depth) {
    if (depth > 32) return false;
    VEC(frame_t) st = {0};
    frame_t f0 = {pc0, pos0, -1, 0};
    vec_push(&st, f0);
    bool matched = false;
    while (st.n && !matched) {
        frame_t f = vec_pop(&st);
        if (f.slot >= 0) {
            m->caps[f.slot] = f.val;
            continue;
        }
        int pc = f.pc;
        size_t pos = f.pos;
        for (;;) {
            if (++m->steps > BT_STEP_BUDGET) goto out;
            const binst_t *in = &m->r->prog.v[pc];
            if (in->op == I_MATCH) {
                matched = true;
                if (end_out) *end_out = pos;
                break;
            }
            if (in->op == I_CHAR) {
                if (pos >= m->len) break;
                uint32_t cp;
                int k = utf8_decode(m->s + pos, m->len - pos, &cp);
                if (!(cp == in->cp || (in->icase && fold(cp) == fold(in->cp)))) break;
                pos += (size_t)k;
                pc++;
            } else if (in->op == I_ANY || in->op == I_ANYNL) {
                if (pos >= m->len) break;
                uint32_t cp;
                int k = utf8_decode(m->s + pos, m->len - pos, &cp);
                if (in->op == I_ANY && cp == '\n') break;
                pos += (size_t)k;
                pc++;
            } else if (in->op == I_CLASS) {
                if (pos >= m->len) break;
                uint32_t cp;
                int k = utf8_decode(m->s + pos, m->len - pos, &cp);
                if (!class_has(&m->r->classes.v[in->arg], cp, in->icase)) break;
                pos += (size_t)k;
                pc++;
            } else if (in->op == I_SPLIT) {
                frame_t alt = {in->y, pos, -1, 0};
                vec_push(&st, alt);
                pc = in->x;
            } else if (in->op == I_JMP) {
                pc = in->x;
            } else if (in->op == I_SAVE) {
                if (in->arg < m->ncaps) {
                    frame_t undo = {0, 0, in->arg, m->caps[in->arg]};
                    vec_push(&st, undo);
                    m->caps[in->arg] = (ptrdiff_t)pos;
                }
                pc++;
            } else if (in->op == I_ASSERT) {
                bool ok;
                switch (in->arg) {
                case AS_BOT: ok = pos == 0; break;
                case AS_EOT: ok = pos == m->len; break;
                case AS_BOL: ok = pos == 0 || m->s[pos - 1] == '\n'; break;
                case AS_EOL: ok = pos == m->len || m->s[pos] == '\n'; break;
                case AS_WORDB: ok = is_word_at(m, pos, true) != is_word_at(m, pos, false); break;
                default: ok = is_word_at(m, pos, true) == is_word_at(m, pos, false); break;
                }
                if (!ok) break;
                pc++;
            } else if (in->op == I_BACKREF) {
                ptrdiff_t a = 2 * in->arg < m->ncaps ? m->caps[2 * in->arg] : -1;
                ptrdiff_t b = 2 * in->arg + 1 < m->ncaps ? m->caps[2 * in->arg + 1] : -1;
                if (a < 0 || b < a) {
                    pc++;
                    continue;
                }
                size_t l = (size_t)(b - a);
                if (pos + l > m->len) break;
                bool eq = in->icase ? strncasecmp(m->s + a, m->s + pos, l) == 0 : memcmp(m->s + a, m->s + pos, l) == 0;
                if (!eq) break;
                pos += l;
                pc++;
            } else if (in->op == I_LOOK) {
                bool behind = in->arg >= 2, neg = in->arg == 1 || in->arg == 3;
                bool found = false;
                ptrdiff_t *saved = xmalloc(sizeof(ptrdiff_t) * (size_t)m->ncaps);
                memcpy(saved, m->caps, sizeof(ptrdiff_t) * (size_t)m->ncaps);
                if (!behind) {
                    found = run(m, in->x, pos, NULL, depth + 1);
                } else {
                    /* try every start before pos whose sub-match ends exactly at pos */
                    size_t lo = pos > 4096 ? pos - 4096 : 0;
                    for (size_t st0 = pos + 1; st0-- > lo && !found;) {
                        size_t e;
                        bm_t sub = *m;
                        sub.len = pos;
                        if (run(&sub, in->x, st0, &e, depth + 1) && e == pos) found = true;
                        m->steps = sub.steps;
                    }
                }
                if (neg || !found) memcpy(m->caps, saved, sizeof(ptrdiff_t) * (size_t)m->ncaps);
                free(saved);
                if (found == neg) break;
                pc = in->y;
            } else {
                break;
            }
        }
    }
out:
    vec_free(&st);
    return matched;
}

bool rxbt_search(const rxbt_t *r, const char *s, size_t len, size_t start, ptrdiff_t *caps, int ncaps) {
    int n = 2 * r->ngroups;
    ptrdiff_t *c = xmalloc(sizeof(ptrdiff_t) * (size_t)n);
    bm_t m = {.r = r, .s = s, .len = len, .caps = c, .ncaps = n};
    bool ok = false;
    for (size_t p = start; p <= len; p++) {
        for (int k = 0; k < n; k++) c[k] = -1;
        size_t e;
        if (run(&m, 0, p, &e, 0)) {
            ok = true;
            break;
        }
        if (m.steps > BT_STEP_BUDGET) break;
        /* advance one code point */
        if (p < len) {
            uint32_t cp;
            int k = utf8_decode(s + p, len - p, &cp);
            p += (size_t)(k > 0 ? k : 1) - 1;
        }
    }
    if (ok && caps) {
        for (int k = 0; k < ncaps; k++) caps[k] = k < n ? c[k] : -1;
    }
    free(c);
    return ok;
}
