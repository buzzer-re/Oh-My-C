/* record.c - session recordings (see record.h). */
#define _GNU_SOURCE /* memmem, strcasestr, vasprintf: glibc before 2.38 declares them only with it */
#include "coding/record.h"

#include "config/settings.h"
#include "core/fs.h"
#include "text/regex.h"
#include "text/rxbt.h"
#include "text/yaml.h"
#include "tui/ansi.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double jnum(const json_t *j) { return json_is_num(j) ? j->u.num.d : 0; }

/* ---------- normalizeStreamRow ---------- */

static bool payload_has_file(const char *p, size_t n) { /* /(?:^|;)File=/ */
    for (size_t i = 0; i + 5 <= n; i++)
        if (!memcmp(p + i, "File=", 5) && (i == 0 || p[i - 1] == ';')) return true;
    return false;
}

/* kitty graphics payload (after "G"): a=p / a=T placement or U=1 placeholder */
static bool kitty_places(const char *p, size_t n) {
    static const char *const KEYS[] = {"a=p", "a=T", "U=1"};
    for (size_t k = 0; k < ARRAY_LEN(KEYS); k++)
        for (size_t i = 0; i + 3 <= n; i++)
            if (!memcmp(p + i, KEYS[k], 3) && (i == 0 || p[i - 1] == ',') && (i + 3 == n || p[i + 3] == ','))
                return true;
    return false;
}

static char *normalize(const char *row, size_t len, int depth);

char *record_normalize_row(const char *row) { return normalize(row, strlen(row), 0); }

static char *normalize(const char *row, size_t len, int depth) {
    if (memmem(row, len, "\xf4\x8e\xbb\xae", 4)) return xstrdup("[image]"); /* U+10EEEE placeholder */
    sb_t out = SB_INIT;
    bool image = false;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)row[i];
        if (c != 0x1b) {
            if ((c < 0x20 && c != 0x09) || c == 0x7f) {
                i++;
                continue;
            }
            if (c == 0xc2 && i + 1 < len && (unsigned char)row[i + 1] >= 0x80 && (unsigned char)row[i + 1] <= 0x9f) {
                i += 2; /* C1 control */
                continue;
            }
            sb_putc(&out, (char)c);
            i++;
            continue;
        }
        char kind = i + 1 < len ? row[i + 1] : 0;
        if (kind == '[') {
            size_t end = i + 2;
            while (end < len && ((unsigned char)row[end] < 0x40 || (unsigned char)row[end] > 0x7e)) end++;
            if (end >= len) break;
            if (row[end] == 'm') sb_appendn(&out, row + i, end + 1 - i);
            i = end + 1;
            continue;
        }
        if (kind == ']' || kind == '_' || kind == 'P') {
            size_t end = i + 2;
            bool tmux = kind == 'P' && len - end >= 5 && !memcmp(row + end, "tmux;", 5);
            while (end < len) {
                bool bell = row[end] == 0x07;
                bool escaped = tmux && end > i + 2 && row[end - 1] == 0x1b;
                bool st = row[end] == 0x1b && end + 1 < len && row[end + 1] == '\\' && !escaped;
                if (bell || st) break;
                end++;
            }
            bool by_st = end < len && row[end] == 0x1b;
            size_t seq_end = MIN(len, end + (by_st ? 2 : 1));
            const char *payload = row + i + 2;
            size_t pl = end > i + 2 ? end - (i + 2) : 0;
            if (kind == ']' && pl >= 2 && !memcmp(payload, "8;", 2)) {
                sb_appendn(&out, row + i, seq_end - i);
            } else if (kind == ']' && pl >= 3 && !memcmp(payload, "66;", 3)) {
                const char *sep = memchr(payload + 3, ';', pl - 3);
                if (sep) sb_appendn(&out, sep + 1, (size_t)(payload + pl - sep - 1));
            } else if (kind == ']' && payload_has_file(payload, pl)) {
                image = true;
            } else if (kind == '_') {
                if (pl && payload[0] == 'G' && kitty_places(payload + 1, pl - 1)) image = true;
            } else if (kind == 'P' && !tmux) {
                size_t k = 0;
                while (k < pl && (isdigit((unsigned char)payload[k]) || payload[k] == ';')) k++;
                if (k < pl && payload[k] == 'q') image = true;
            } else if (kind == 'P' && tmux && depth < 4) {
                sb_t un = SB_INIT; /* replaceAll("\x1b\x1b", "\x1b") */
                for (size_t k = 5; k < pl; k++) {
                    if (payload[k] == 0x1b && k + 1 < pl && payload[k + 1] == 0x1b) k++;
                    sb_putc(&un, payload[k]);
                }
                char *nested = normalize(un.s ? un.s : "", un.len, depth + 1);
                if (str_eq(nested, "[image]")) image = true;
                else sb_append(&out, nested);
                free(nested);
                sb_free(&un);
            }
            i = seq_end;
            continue;
        }
        /* other ESC controls: intermediates 0x20..0x2f, then the final byte */
        size_t end = i + 1;
        while (end < len && (unsigned char)row[end] >= 0x20 && (unsigned char)row[end] <= 0x2f) end++;
        i = MIN(len, end + 1);
    }
    if (image) {
        sb_free(&out);
        return xstrdup("[image]");
    }
    return sb_detach(&out);
}

/* ---------- StreamRedactor ---------- */

#define REDACTION "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
#define MIN_PLAIN 8
#define MIN_PREFIX 6

struct record_redactor {
    VEC(rxbt_t *) patterns;
    rxbt_t *assign, *secret_name, *conn;
    strvec_t plain;
};

#define B "(?<![A-Za-z0-9_-])"
#define E "(?![A-Za-z0-9_-])"
static const struct {
    const char *src;
    int flags;
} BUILTIN[] = {
    {"(?<![a-zA-Z0-9_*-])(gh[opusr]_[a-zA-Z0-9_*]{36,}|github_pat_[a-zA-Z0-9_*]{36,}|glpat-[a-zA-Z0-9_*-]{20,}|sk-proj-[a-zA-Z0-9_*-]{36,}|"
     "sk-ant-[a-zA-Z0-9_*-]{36,}|sk-[a-zA-Z0-9_*-]{48,})(?![a-zA-Z0-9_*-])",
     RX_ICASE},
    {B "(?:AKIA|ASIA)[A-Z0-9]{16}" E, 0},
    {B "AIza[A-Za-z0-9_-]{30,}" E, 0},
    {B "xox[abprs]-[A-Za-z0-9-]{10,}" E, 0},
    {B "npm_[A-Za-z0-9]{30,}" E, 0},
    {B "(?:sk|rk)_(?:live|test)_[A-Za-z0-9]{20,}" E, 0},
    {B "whsec_[A-Za-z0-9]{20,}" E, 0},
    {B "hf_[A-Za-z0-9]{30,}" E, 0},
    {B "SG\\.[A-Za-z0-9_-]{22}\\.[A-Za-z0-9_-]{43}" E, 0},
    {B "eyJ[A-Za-z0-9_-]{10,}\\.eyJ[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]{10,}" E, 0},
    {"(?<=\\bBearer )[A-Za-z0-9._~+/=-]{20,}" E, RX_ICASE},
    {"-----BEGIN [A-Z ]*PRIVATE KEY-----[\\s\\S]*?-----END [A-Z ]*PRIVATE KEY-----", 0},
    /* CREDENTIAL_PREFIX_RULES: token / line / bearer-token introducers */
    {"(?<![A-Za-z0-9_-])(?:sk-ant-|sk-proj-|sk-|gh[opusr]_|github_pat_|glpat-|xox[abprs]-|AKIA|ASIA|AIza|npm_|hf_|whsec_|rk_live_|"
     "sk_live_|sk_test_|eyJ)\\S*",
     0},
    {"(?:-----BEGIN)[^\\r\\n]*", 0},
    {"(?<=\\b(?:Bearer ))\\S+", RX_ICASE},
};
#undef B
#undef E

static void add_plain(strvec_t *v, const char *s) {
    if (s && strlen(s) >= MIN_PLAIN && !strvec_contains(v, s)) strvec_push(v, s);
}

static record_redactor_t *redactor_base(void) {
    record_redactor_t *r = xcalloc(1, sizeof *r);
    for (size_t i = 0; i < ARRAY_LEN(BUILTIN); i++) {
        char *err = NULL;
        rxbt_t *x = rxbt_compile(BUILTIN[i].src, BUILTIN[i].flags, &err);
        if (x) vec_push(&r->patterns, x);
        free(err);
    }
    r->assign = rxbt_compile("[\"']?([A-Za-z_][A-Za-z0-9_]*)[\"']?\\s*(?:=|:)\\s*(?:\"([^\"\\s]{4,})\"|'([^'\\s]{4,})'|([^\\s\"']{4,}))", 0, NULL);
    r->secret_name = rxbt_compile("(KEY|SECRET|TOKEN|PASSWORD|PASSWD|PASS|AUTH|CREDENTIAL|PRIVATE|OAUTH)(_|$)", RX_ICASE, NULL);
    r->conn = rxbt_compile("[a-z][a-z0-9+.-]*://[^/:@?#\\s]*:([^/?#@\\s]+)@", RX_ICASE, NULL);
    return r;
}

record_redactor_t *record_redactor_new(const char *const *plain, size_t n) {
    record_redactor_t *r = redactor_base();
    for (size_t i = 0; i < n; i++) add_plain(&r->plain, plain[i]);
    return r;
}

/* secrets.yml: a YAML array of {type: plain|regex, content, ...}; plain entries only */
static void secrets_file(strvec_t *out, const char *path) {
    size_t n = 0;
    char *d = fs_read(path, &n);
    if (!d) return;
    json_t *y = yaml_parse(d, n, NULL), *e;
    json_arr_foreach(y, i, e) if (str_eq(json_get_str(e, "type"), "plain")) add_plain(out, json_get_str(e, "content"));
    json_free(y);
    free(d);
}

/* parseEnvFile values (KEY=VALUE lines, optional quotes, `export ` prefix) */
static void dotenv_values(strvec_t *out, const char *path) {
    size_t n = 0;
    char *d = fs_read(path, &n);
    if (!d) return;
    strvec_t lines = str_split_lines(d, n);
    for (size_t i = 0; i < lines.n; i++) {
        char *l = str_trim(lines.v[i]);
        const char *p = l;
        if (!strncmp(p, "export ", 7)) p += 7;
        const char *eq = strchr(p, '=');
        if (*p && *p != '#' && eq) {
            char *v = str_trim(eq + 1);
            size_t vl = strlen(v);
            if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) {
                v[vl - 1] = 0;
                memmove(v, v + 1, vl - 1);
            }
            add_plain(out, v);
            free(v);
        }
        free(l);
    }
    strvec_free(&lines);
    free(d);
}

record_redactor_t *record_redactor_load(const char *cwd, const json_t *extra) {
    record_redactor_t *r = redactor_base();
    json_t *x;
    json_arr_foreach(extra, i, x) {
        if (!json_is_str(x)) continue;
        rxbt_t *p = rxbt_compile(json_sv(x), 0, NULL);
        if (p) vec_push(&r->patterns, p);
    }
    char *proj = path_join(cwd ? cwd : ".", ".omp/secrets.yml"), *glob = omc_agent_path("secrets.yml");
    secrets_file(&r->plain, glob);
    secrets_file(&r->plain, proj);
    free(proj), free(glob);
    /* collectEnvSecrets: secret-named variables, then connection-URL passwords */
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *name = xstrndup(*e, (size_t)(eq - *e));
        if (strlen(eq + 1) >= MIN_PLAIN && r->secret_name && rxbt_search(r->secret_name, name, strlen(name), 0, NULL, 0))
            add_plain(&r->plain, eq + 1);
        free(name);
    }
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || !r->conn) continue;
        ptrdiff_t caps[4];
        const char *v = eq + 1;
        if (rxbt_search(r->conn, v, strlen(v), 0, caps, 4) && caps[0] == 0 && caps[2] >= 0) {
            char *pw = xstrndup(v + caps[2], (size_t)(caps[3] - caps[2]));
            add_plain(&r->plain, pw);
            free(pw);
        }
    }
    char *home_env = path_join(home_dir(), ".env"), *root_env = path_join(omc_config_dir(), ".env"),
         *agent_env = omc_agent_path(".env"), *proj_env = path_join(cwd ? cwd : ".", ".env");
    dotenv_values(&r->plain, home_env);
    dotenv_values(&r->plain, root_env);
    dotenv_values(&r->plain, agent_env);
    dotenv_values(&r->plain, proj_env);
    free(home_env), free(root_env), free(agent_env), free(proj_env);
    return r;
}

void record_redactor_free(record_redactor_t *r) {
    if (!r) return;
    for (size_t i = 0; i < r->patterns.n; i++) rxbt_free(r->patterns.v[i]);
    vec_free(&r->patterns);
    rxbt_free(r->assign);
    rxbt_free(r->secret_name);
    rxbt_free(r->conn);
    strvec_free(&r->plain);
    free(r);
}

typedef VEC(size_t) spans_t;

static void span_add(spans_t *s, size_t a, size_t b) {
    vec_push(s, a);
    vec_push(s, b);
}

static int cmp_span(const void *x, const void *y) {
    const size_t *a = x, *b = y;
    return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0;
}

/* the last occurrence of `needle` in [s, s+n) (lastIndexOf) */
static ptrdiff_t last_index(const char *s, size_t n, const char *needle, size_t nl) {
    if (nl > n) return -1;
    for (size_t i = n - nl + 1; i-- > 0;)
        if (!memcmp(s + i, needle, nl)) return (ptrdiff_t)i;
    return -1;
}

char *record_redact_row(record_redactor_t *r, const char *row) {
    char *plain = tui_strip_ansi(row);
    size_t len = strlen(plain);
    spans_t sp = {0};
    ptrdiff_t caps[10];
    for (size_t k = 0; k < r->patterns.n; k++) {
        size_t pos = 0;
        while (pos <= len && rxbt_search(r->patterns.v[k], plain, len, pos, caps, 2)) {
            if (caps[1] > caps[0]) {
                span_add(&sp, (size_t)caps[0], (size_t)caps[1]);
                pos = (size_t)caps[1];
            } else pos = (size_t)caps[0] + 1;
        }
    }
    for (size_t pos = 0; r->assign && pos <= len && rxbt_search(r->assign, plain, len, pos, caps, 10);) {
        size_t mend = (size_t)caps[1];
        char *name = xstrndup(plain + caps[2], (size_t)(caps[3] - caps[2]));
        bool secret = r->secret_name && rxbt_search(r->secret_name, name, strlen(name), 0, NULL, 0);
        free(name);
        if (secret) {
            int g = caps[4] >= 0 ? 2 : caps[6] >= 0 ? 3 : 4;
            size_t vl = (size_t)(caps[2 * g + 1] - caps[2 * g]);
            ptrdiff_t at = last_index(plain + caps[0], (size_t)(caps[1] - caps[0]), plain + caps[2 * g], vl);
            if (at >= 0) span_add(&sp, (size_t)caps[0] + (size_t)at, (size_t)caps[0] + (size_t)at + vl);
        }
        pos = mend > (size_t)caps[0] ? mend : (size_t)caps[0] + 1;
    }
    for (size_t pos = 0; r->conn && pos <= len && rxbt_search(r->conn, plain, len, pos, caps, 4);) {
        size_t pl = (size_t)(caps[3] - caps[2]);
        ptrdiff_t at = last_index(plain + caps[0], (size_t)(caps[1] - caps[0]), plain + caps[2], pl);
        if (at >= 0) span_add(&sp, (size_t)caps[0] + (size_t)at, (size_t)caps[0] + (size_t)at + pl);
        pos = (size_t)caps[1] > (size_t)caps[0] ? (size_t)caps[1] : (size_t)caps[0] + 1;
    }
    for (size_t k = 0; k < r->plain.n; k++) {
        const char *sec = r->plain.v[k];
        size_t sl = strlen(sec);
        for (const char *p = plain; (p = memmem(p, (size_t)(plain + len - p), sec, MIN_PREFIX)); p++) {
            size_t start = (size_t)(p - plain), l = MIN_PREFIX;
            while (l < sl && start + l < len && plain[start + l] == sec[l]) l++;
            span_add(&sp, start, start + l);
        }
    }
    if (!sp.n) {
        free(plain);
        vec_free(&sp);
        return xstrdup(row);
    }
    xqsort(sp.v, sp.n / 2, 2 * sizeof(size_t), cmp_span);
    size_t m = 0;
    for (size_t i = 0; i < sp.n; i += 2) {
        if (m && sp.v[i] < sp.v[m - 1]) {
            sp.v[m - 1] = MAX(sp.v[m - 1], sp.v[i + 1]);
            continue;
        }
        sp.v[m++] = sp.v[i];
        sp.v[m++] = sp.v[i + 1];
    }
    sb_t out = SB_INIT;
    size_t cur = 0;
    for (size_t i = 0; i < m; i += 2) {
        sb_appendn(&out, plain + cur, sp.v[i] - cur);
        sb_append(&out, REDACTION);
        cur = sp.v[i + 1];
    }
    sb_append(&out, plain + cur);
    free(plain);
    vec_free(&sp);
    return sb_detach(&out);
}

/* ---------- StreamPaintEncoder ---------- */

struct record_encoder {
    record_redactor_t *redactor;
    strvec_t raw_viewport;   /* latest paint's rows, encoded at drain */
    bool have_raw;
    strvec_t history;        /* raw history rows */
    size_t history_skipped;
    bool reset, force_full, alt, last_alt_set, last_alt;
    int pcols, prows, lcols, lrows;
    strvec_t last_viewport;  /* encoded rows of the last drained viewport */
    bool have_last;
    hmap_t cache;            /* raw row -> encoded row (last drain) */
};

record_encoder_t *record_encoder_new(int columns, int rows, record_redactor_t *redactor) {
    record_encoder_t *e = xcalloc(1, sizeof *e);
    e->redactor = redactor;
    e->pcols = e->lcols = columns;
    e->prows = e->lrows = rows;
    hmap_init(&e->cache);
    return e;
}

void record_encoder_free(record_encoder_t *e) {
    if (!e) return;
    strvec_free(&e->raw_viewport);
    strvec_free(&e->history);
    strvec_free(&e->last_viewport);
    hmap_free(&e->cache, free);
    free(e);
}

void record_encoder_push(record_encoder_t *e, const record_paint_t *p) {
    if (p->reset) {
        e->reset = e->force_full = true;
        strvec_free(&e->history);
        e->history_skipped = 0;
    }
    if (p->nhistory) {
        for (size_t i = 0; i < p->nhistory; i++) strvec_push(&e->history, p->history[i]);
        size_t cap = e->history_skipped ? RECORD_HISTORY_LIMIT - 1 : RECORD_HISTORY_LIMIT;
        if (e->history.n > cap) {
            size_t excess = e->history.n - (RECORD_HISTORY_LIMIT - 1);
            e->history_skipped += excess;
            for (size_t i = 0; i < excess; i++) free(e->history.v[i]);
            memmove(e->history.v, e->history.v + excess, (e->history.n - excess) * sizeof *e->history.v);
            e->history.n -= excess;
        }
    }
    if (p->columns != e->pcols || p->rows != e->prows) e->force_full = true;
    if (e->last_alt_set && p->alt != e->last_alt) e->force_full = true;
    if (p->alt) e->force_full = true;
    e->last_alt = p->alt;
    e->last_alt_set = true;
    e->alt = p->alt;
    e->pcols = p->columns;
    e->prows = p->rows;
    strvec_free(&e->raw_viewport);
    for (size_t i = 0; i < p->nviewport; i++) strvec_push(&e->raw_viewport, p->viewport[i]);
    e->have_raw = true;
}

/* normalized + redacted, reusing the last drain's results; `next` receives this call's results */
static char *encode_row(record_encoder_t *e, const char *raw, hmap_t *next) {
    const char *hit = hmap_get(next, raw);
    if (!hit) hit = hmap_get(&e->cache, raw);
    char *enc;
    if (hit) enc = xstrdup(hit);
    else {
        char *n = record_normalize_row(raw);
        enc = e->redactor ? record_redact_row(e->redactor, n) : xstrdup(n);
        free(n);
    }
    if (!hmap_has(next, raw)) hmap_put(next, raw, xstrdup(enc));
    return enc;
}

static json_t *rows_json(const strvec_t *rows) {
    json_t *a = json_arr();
    for (size_t i = 0; i < rows->n; i++) json_push_str(a, rows->v[i]);
    return a;
}

static json_t *frame(const char *t) {
    json_t *f = json_obj();
    json_set_str(f, "t", t);
    return f;
}

json_t *record_encoder_drain(record_encoder_t *e) {
    json_t *frames = json_arr();
    if (e->reset) json_push(frames, frame("reset"));
    if (e->history.n || e->history_skipped) {
        hmap_t tmp;
        hmap_init(&tmp);
        strvec_t rows = {0};
        if (e->history_skipped) strvec_push_own(&rows, xasprintf("\xe2\x80\xa6 (%zu rows skipped)", e->history_skipped));
        for (size_t i = 0; i < e->history.n; i++) strvec_push_own(&rows, encode_row(e, e->history.v[i], &tmp));
        json_t *f = frame("history");
        json_set(f, "rows", rows_json(&rows));
        json_push(frames, f);
        strvec_free(&rows);
        hmap_free(&tmp, free);
    }
    bool resized = e->pcols != e->lcols || e->prows != e->lrows;
    if (resized) {
        json_t *f = frame("resize");
        json_set_int(f, "cols", e->pcols);
        json_set_int(f, "rows", e->prows);
        json_push(frames, f);
        e->lcols = e->pcols;
        e->lrows = e->prows;
    }
    bool force = e->force_full || e->reset || resized || e->alt;
    e->reset = false;
    strvec_free(&e->history);
    e->history_skipped = 0;
    e->force_full = false;
    if (!e->have_raw) return frames;
    e->have_raw = false;
    hmap_t next;
    hmap_init(&next);
    strvec_t vp = {0};
    for (size_t i = 0; i < e->raw_viewport.n; i++) strvec_push_own(&vp, encode_row(e, e->raw_viewport.v[i], &next));
    hmap_free(&e->cache, free);
    e->cache = next;
    strvec_free(&e->raw_viewport);
    bool had_last = e->have_last;
    strvec_t last = e->last_viewport;
    e->last_viewport = vp;
    e->have_last = true;
    if (!had_last || force) {
        json_t *f = frame("viewport");
        json_set(f, "rows", rows_json(&vp));
        json_push(frames, f);
        strvec_free(&last);
        return frames;
    }
    json_t *ops = json_arr();
    size_t changed = 0;
    for (size_t i = 0; i < vp.n; i++) {
        if (i < last.n && !strcmp(vp.v[i], last.v[i])) continue;
        json_t *op = json_arr();
        json_push(op, json_int((int64_t)i));
        json_push_str(op, vp.v[i]);
        json_push(ops, op);
        changed++;
    }
    if (last.n > vp.n) changed += last.n - vp.n;
    strvec_free(&last);
    if (!changed) {
        json_free(ops);
        return frames;
    }
    if (changed > vp.n / 2.0) {
        json_free(ops);
        json_t *f = frame("viewport");
        json_set(f, "rows", rows_json(&vp));
        json_push(frames, f);
    } else {
        json_t *f = frame("patch");
        json_set(f, "ops", ops);
        json_set_int(f, "rows", (int64_t)vp.n);
        json_push(frames, f);
    }
    return frames;
}

/* ---------- files ---------- */

char *record_dir(void) {
    const char *t = getenv("TMPDIR");
    char *base = xstrdup(t && *t ? t : "/tmp");
    size_t n = strlen(base);
    if (n > 1 && base[n - 1] == '/') base[n - 1] = 0;
    char *d = path_join(base, "omp-recordings");
    free(base);
    return d;
}

char *record_new_path(const char *session_id) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H-%M-%S", &tm);
    char *dir = record_dir();
    char *name = xasprintf("%s-%.8s%s", stamp, session_id ? session_id : "", RECORD_EXTENSION);
    char *p = path_join(dir, name);
    free(dir), free(name);
    return p;
}

char *record_latest(void) {
    char *dir = record_dir();
    dirlist_t l;
    char *best = NULL;
    int64_t best_mt = 0;
    if (fs_list_dir(dir, &l) == 0) {
        for (size_t i = 0; i < l.n; i++) {
            if (!str_ends(l.v[i].name, RECORD_EXTENSION)) continue;
            char *p = path_join(dir, l.v[i].name);
            struct stat st;
            int64_t mt = stat(p, &st) == 0 ? (int64_t)st.st_mtime : 0; /* st_mtim is Linux-only */
            if (!best || mt > best_mt) {
                free(best);
                best = p;
                best_mt = mt;
            } else free(p);
        }
        dirlist_free(&l);
    }
    free(dir);
    return best;
}

static bool is_dim(const json_t *v) { return v && json_is_num(v) && jnum(v) >= 0 && jnum(v) == (double)(int64_t)jnum(v); }

static bool str_rows(const json_t *a) {
    if (!json_is_arr(a)) return false;
    json_t *x;
    json_arr_foreach(a, i, x) if (!json_is_str(x)) return false;
    return true;
}

/* isSessionFrame minus hello / paused */
static bool screen_frame(const json_t *f) {
    const char *t = json_get_str(f, "t");
    if (!t) return false;
    if (!strcmp(t, "resize")) return is_dim(json_get(f, "cols")) && is_dim(json_get(f, "rows"));
    if (!strcmp(t, "history") || !strcmp(t, "viewport")) return str_rows(json_get(f, "rows"));
    if (!strcmp(t, "reset")) return true;
    if (!strcmp(t, "patch")) {
        if (!is_dim(json_get(f, "rows")) || !json_is_arr(json_get(f, "ops"))) return false;
        json_t *op;
        json_arr_foreach(json_get(f, "ops"), i, op) {
            if (!json_is_arr(op) || json_len(op) != 2) return false;
            const json_t *ix = json_at(op, 0);
            if (!json_is_num(ix) || jnum(ix) != (double)(int64_t)jnum(ix) || !json_is_str(json_at(op, 1))) return false;
        }
        return true;
    }
    return false;
}

json_t *record_parse(const char *text, size_t len, char **err) {
    *err = NULL;
    strvec_t lines = str_split_lines(text, len);
    bool trailing_nl = len && text[len - 1] == '\n';
    json_t *header = NULL, *events = json_arr();
    size_t lineno = 0;
    for (size_t i = 0; i < lines.n; i++) {
        lineno++;
        char *t = str_trim(lines.v[i]);
        bool blank = !*t;
        free(t);
        if (blank) continue;
        json_t *v = json_parse(lines.v[i], strlen(lines.v[i]), NULL);
        if (!v) {
            if (i == lines.n - 1 && !trailing_nl) break; /* a truncated final line (recorder killed mid-write) */
            *err = xasprintf("malformed recording: invalid JSON on line %zu", lineno);
            break;
        }
        if (!header) {
            header = v;
            continue;
        }
        if (!json_is_arr(v) || json_len(v) != 2 || !is_dim(json_at(v, 0)) || !screen_frame(json_at(v, 1))) {
            json_free(v);
            *err = xasprintf("malformed recording event on line %zu", lineno);
            break;
        }
        json_push(events, v);
    }
    strvec_free(&lines);
    if (!*err && (!header || !json_is_num(json_get(header, "ompcast")) || !is_dim(json_get(header, "cols")) ||
                  !is_dim(json_get(header, "rows")) || !json_get_str(header, "title") || !json_get_str(header, "createdAt")))
        *err = xstrdup("not an omp recording (missing ompcast header)");
    if (!*err && json_get_num(header, "ompcast", 0) != RECORD_VERSION)
        *err = xasprintf("unsupported recording version %g", json_get_num(header, "ompcast", 0));
    if (*err) {
        json_free(header);
        json_free(events);
        return NULL;
    }
    json_t *r = json_obj();
    json_set(r, "header", header);
    json_set(r, "events", events);
    return r;
}

/* ---------- SessionRecorder ---------- */

struct record_writer {
    char *path;
    FILE *f;
    record_redactor_t *redactor;
    record_encoder_t *enc;
    int64_t started, last_paint;
};

static char *iso_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t t = tv.tv_sec;
    gmtime_r(&t, &tm);
    return xasprintf("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec, (int)(tv.tv_usec / 1000));
}

record_writer_t *record_writer_open(const char *path, int columns, int rows, const char *title,
                                    record_redactor_t *redactor, char **err) {
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        fs_mkdir_p(dir, 0755);
    }
    free(dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        *err = xasprintf("%s: %s", path, strerror(errno));
        record_redactor_free(redactor);
        return NULL;
    }
    record_writer_t *w = xcalloc(1, sizeof *w);
    w->path = xstrdup(path);
    w->f = f;
    w->redactor = redactor;
    w->enc = record_encoder_new(columns, rows, redactor);
    w->started = mono_ms();
    json_t *h = json_obj();
    json_set_int(h, "ompcast", RECORD_VERSION);
    json_set_int(h, "cols", columns);
    json_set_int(h, "rows", rows);
    json_set_str(h, "title", title ? title : "");
    char *created = iso_now();
    json_set_str(h, "createdAt", created);
    free(created);
    char *line = json_dump(h, 0);
    fprintf(f, "%s\n", line);
    fflush(f);
    free(line);
    json_free(h);
    return w;
}

void record_writer_paint(record_writer_t *w, const record_paint_t *p, int64_t now) {
    record_encoder_push(w->enc, p);
    w->last_paint = now - w->started;
}

void record_writer_flush(record_writer_t *w) {
    json_t *frames = record_encoder_drain(w->enc), *f;
    json_arr_foreach(frames, i, f) {
        json_t *ev = json_arr();
        json_push(ev, json_int(w->last_paint));
        json_push(ev, json_clone(f));
        char *line = json_dump(ev, 0);
        fprintf(w->f, "%s\n", line);
        free(line);
        json_free(ev);
    }
    json_free(frames);
    fflush(w->f);
}

int64_t record_writer_elapsed(const record_writer_t *w, int64_t now) { return now - w->started; }
const char *record_writer_path(const record_writer_t *w) { return w->path; }

void record_writer_close(record_writer_t *w) {
    if (!w) return;
    record_writer_flush(w);
    fclose(w->f);
    record_encoder_free(w->enc);
    record_redactor_free(w->redactor);
    free(w->path);
    free(w);
}

/* ---------- playback (player.ts) ---------- */

typedef struct {
    int cols, rows;
    strvec_t history, viewport;
} screen_t;

static void fit_viewport(strvec_t *v, int rows) {
    while ((int)v->n > rows) free(v->v[--v->n]);
    while ((int)v->n < rows) strvec_push(v, "");
}

/* applyScreenFrame */
static void apply_frame(screen_t *s, const json_t *f) {
    const char *t = json_get_str(f, "t");
    if (str_eq(t, "resize")) {
        s->cols = (int)json_d2i(json_get_num(f, "cols", 0));
        s->rows = (int)json_d2i(json_get_num(f, "rows", 0));
        fit_viewport(&s->viewport, s->rows);
    } else if (str_eq(t, "history")) {
        json_t *x;
        json_arr_foreach(json_get(f, "rows"), i, x) strvec_push(&s->history, json_sv(x));
        if (s->history.n > RECORD_HISTORY_LIMIT) {
            size_t drop = s->history.n - RECORD_HISTORY_LIMIT;
            for (size_t i = 0; i < drop; i++) free(s->history.v[i]);
            memmove(s->history.v, s->history.v + drop, (s->history.n - drop) * sizeof *s->history.v);
            s->history.n -= drop;
        }
    } else if (str_eq(t, "viewport")) {
        strvec_free(&s->viewport);
        json_t *x;
        json_arr_foreach(json_get(f, "rows"), i, x) strvec_push(&s->viewport, json_sv(x));
    } else if (str_eq(t, "patch")) {
        int rows = (int)json_d2i(json_get_num(f, "rows", 0));
        s->rows = rows;
        fit_viewport(&s->viewport, rows);
        json_t *op;
        json_arr_foreach(json_get(f, "ops"), i, op) {
            int64_t ix = (int64_t)jnum(json_at(op, 0));
            if (ix >= 0 && ix < rows) {
                free(s->viewport.v[ix]);
                s->viewport.v[ix] = xstrdup(json_sv(json_at(op, 1)));
            }
        }
    } else if (str_eq(t, "reset")) {
        strvec_free(&s->history);
        strvec_free(&s->viewport);
    }
}

typedef struct {
    screen_t screen;
    VEC(char *) painted; /* NULL marks a row needing repaint */
    sb_t out;
    int width, height;
} painter_t;

static void term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else *w = 80, *h = 24;
}

static void painted_clear(painter_t *p) {
    for (size_t i = 0; i < p->painted.n; i++) free(p->painted.v[i]);
    p->painted.n = 0;
}

static void line_out(painter_t *p, int row, const char *content) {
    char *tabs = tui_replace_tabs(content), *fitted = tui_truncate_to_width(tabs, p->width, "");
    const char *close = strstr(fitted, "\x1b]8;") ? "\x1b]8;;\x1b\\" : "";
    sb_appendf(&p->out, "\x1b[%d;1H\x1b[0m\x1b[2K%s\x1b[0m%s", row, fitted, close);
    free(tabs), free(fitted);
}

static void commit_rows(painter_t *p, const json_t *rows) {
    int region = (int)p->painted.n, height = p->height;
    int capacity = region > 0 ? region : height, top = height - region + 1;
    size_t n = json_len(rows);
    for (size_t start = 0; start < n; start += (size_t)capacity) {
        size_t chunk = MIN((size_t)capacity, n - start);
        if (region > 0) {
            for (size_t k = 0; k < chunk; k++) line_out(p, top + (int)k, json_sv(json_at(rows, start + k)));
            sb_appendf(&p->out, "\x1b[%d;1H", height);
            for (size_t k = 0; k < chunk; k++) sb_putc(&p->out, '\n');
        } else {
            sb_appendf(&p->out, "\x1b[%d;1H", height);
            for (size_t k = 0; k < chunk; k++) sb_putc(&p->out, '\n');
            int first = height - (int)chunk + 1;
            for (size_t k = 0; k < chunk; k++) line_out(p, first + (int)k, json_sv(json_at(rows, start + k)));
        }
    }
    for (size_t i = 0; i < p->painted.n; i++) {
        free(p->painted.v[i]);
        p->painted.v[i] = NULL;
    }
}

static void painter_apply(painter_t *p, const json_t *f) {
    const char *t = json_get_str(f, "t");
    if (str_eq(t, "reset")) {
        sb_append(&p->out, "\x1b[0m\x1b[H\x1b[2J\x1b[3J");
        painted_clear(p);
    } else if (str_eq(t, "history")) commit_rows(p, json_get(f, "rows"));
    apply_frame(&p->screen, f);
}

static void painter_flush(painter_t *p, int fd) {
    const strvec_t *rows = &p->screen.viewport;
    int count = MIN(p->height, (int)rows->n);
    int top = p->height - count + 1;
    for (int row = p->height - (int)p->painted.n + 1; row < top; row++) sb_appendf(&p->out, "\x1b[%d;1H\x1b[0m\x1b[2K", row);
    bool full = (int)p->painted.n != count;
    for (int i = 0; i < count; i++) {
        const char *v = rows->v[rows->n - (size_t)count + (size_t)i];
        if (!full && p->painted.v[i] && !strcmp(p->painted.v[i], v)) continue;
        line_out(p, top + i, v);
    }
    painted_clear(p);
    for (int i = 0; i < count; i++) vec_push(&p->painted, xstrdup(rows->v[rows->n - (size_t)count + (size_t)i]));
    if (p->out.len) {
        sb_t w = SB_INIT;
        sb_append(&w, "\x1b[?2026h");
        sb_appendn(&w, sb_cstr(&p->out), p->out.len);
        sb_append(&w, "\x1b[?2026l");
        ssize_t off = 0;
        while ((size_t)off < w.len) {
            ssize_t k = write(fd, w.s + off, w.len - (size_t)off);
            if (k <= 0) break;
            off += k;
        }
        sb_free(&w);
        sb_clear(&p->out);
    }
}

static void write_all(int fd, const char *s) {
    size_t n = strlen(s), off = 0;
    while (off < n) {
        ssize_t k = write(fd, s + off, n - off);
        if (k <= 0) break;
        off += (size_t)k;
    }
}

static volatile sig_atomic_t g_winch;
static void on_winch(int sig) { g_winch = 1; }

/* PlaybackControl.wait: let `ms` of unpaused time pass; false once the viewer quits */
static bool control_wait(double ms, bool *paused, painter_t *p) {
    double remaining = ms;
    for (;;) {
        if (g_winch) { /* onResize: invalidate and repaint */
            g_winch = 0;
            term_size(&p->width, &p->height);
            sb_append(&p->out, "\x1b[0m\x1b[2J");
            painted_clear(p);
            painter_flush(p, STDOUT_FILENO);
        }
        if (!*paused && remaining <= 0) return true;
        struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
        int64_t t0 = mono_ms();
        int timeout = *paused ? 200 : (int)MIN(remaining, 200.0);
        int rc = poll(&pfd, 1, timeout < 0 ? 0 : timeout);
        if (!*paused) remaining -= (double)(mono_ms() - t0);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            char buf[64];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) continue;
            /* a lone ESC quits; other ESC-prefixed input is ignored */
            if (buf[0] == 0x1b && n > 1) continue;
            if (buf[0] == 0x1b || memchr(buf, 'q', (size_t)n) || memchr(buf, 0x03, (size_t)n)) return false;
            if (memchr(buf, ' ', (size_t)n)) *paused = !*paused;
        }
    }
}

int record_play(const json_t *recording, double speed, int64_t idle_limit_ms) {
    painter_t p = {0};
    const json_t *h = json_get(recording, "header");
    p.screen.cols = (int)json_d2i(json_get_num(h, "cols", 80));
    p.screen.rows = (int)json_d2i(json_get_num(h, "rows", 24));
    term_size(&p.width, &p.height);
    struct termios saved;
    bool raw = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    if (raw) {
        struct termios t = saved;
        t.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    struct sigaction sa = {0}, old;
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, &old);
    sb_t begin = SB_INIT;
    sb_append(&begin, "\x1b[?25l\x1b[?7l");
    for (int i = 0; i < p.height - 1; i++) sb_putc(&begin, '\n');
    write_all(STDOUT_FILENO, sb_cstr(&begin));
    sb_free(&begin);
    const json_t *events = json_get(recording, "events");
    size_t n = json_len(events);
    double previous = 0;
    bool paused = false;
    for (size_t i = 0; i < n;) {
        double at = jnum(json_at(json_at(events, i), 0));
        double gap = at - previous;
        if (gap < 0) gap = 0;
        if (idle_limit_ms >= 0 && gap > (double)idle_limit_ms) gap = (double)idle_limit_ms;
        previous = at;
        if (!control_wait(gap / speed, &paused, &p)) break;
        while (i < n && jnum(json_at(json_at(events, i), 0)) == at) painter_apply(&p, json_at(json_at(events, i++), 1));
        painter_flush(&p, STDOUT_FILENO);
    }
    painter_flush(&p, STDOUT_FILENO);
    char *end = xasprintf("\x1b[0m\x1b[%d;1H\n\x1b[?7h\x1b[?25h", p.height);
    write_all(STDOUT_FILENO, end);
    free(end);
    sigaction(SIGWINCH, &old, NULL);
    if (raw) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    painted_clear(&p);
    vec_free(&p.painted);
    strvec_free(&p.screen.history);
    strvec_free(&p.screen.viewport);
    sb_free(&p.out);
    return 0;
}
