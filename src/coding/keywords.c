/* keywords.c - magic keywords: prose detection (markdown-prose.ts maskNonProse + the standalone-word matcher)
 * and the hidden notices of modes/magic-keywords.ts. */
#include "coding/keywords.h"

#include "coding/app.h"
#include "config/settings.h"
#include "prompt/template.h"
#include "text/regex.h"
#include "text/rxbt.h"

#include <ctype.h>
#include <pthread.h>
#include <strings.h>

typedef struct {
    const char *id, *word, *notice; /* notice: prompts/coding-agent/prompts/system/<notice> */
    const char *requires[2];
    double hue_from, hue_to; /* the editor/bubble gradient */
    double light_from, light_to; /* lightness sweep; 0: the default lightness */
} keyword_t;

/* MAGIC_KEYWORDS. `workflowz` is detected but its notice teaches eval `workpool()`/`agent()`, which omc does not
 * implement, so (like omp skipping a notice whose required tools are inactive) it is never injected. */
static const keyword_t KEYWORDS[] = {
    {"ultrathink", "ultrathink", "ultrathink-notice.md", {NULL, NULL}, 0, 330, 0, 0},
    {"chillax", "chillax", "chillax-notice.md", {NULL, NULL}, 180, 235, 62, 45}, /* cyan darkening into deep blue */
    {"orchestrate", "orchestrate", "orchestrate-notice.md", {"task", NULL}, 150, 280, 0, 0},
    {"workflow", "workflowz", NULL, {"task", "eval"}, 30, 150, 0, 0},
    {"jevify", "jevify", "jevify-notice.md", {"eval", NULL}, 300, 420, 0, 0},
};

bool keywords_enabled(const char *id) {
    char key[64];
    snprintf(key, sizeof key, "magicKeywords.%s", id);
    return settings_bool("magicKeywords.enabled", true) && settings_bool(key, true);
}

/* ---------- maskNonProse ---------- */

static bool tag_start(char c) { return isalpha((unsigned char)c); }
static bool tag_char(char c) { return isalnum((unsigned char)c) || c == '-'; }

/* index of the `>` closing a tag whose attributes start at j (quotes honored), -1 when malformed */
static long tag_end(const char *t, size_t j, size_t n) {
    char quote = 0;
    for (size_t k = j; k < n; k++) {
        char ch = t[k];
        if (quote) {
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '>') return (long)k;
        if (ch == '<') return -1;
    }
    return -1;
}

static long matching_close(const char *t, size_t start, size_t n, const char *name, size_t nlen, const unsigned char *masked) {
    int depth = 1;
    size_t k = start;
    while (k < n) {
        if (masked[k] || t[k] != '<') {
            k++;
            continue;
        }
        size_t m = k + 1;
        bool closing = false;
        if (m < n && t[m] == '/') {
            closing = true;
            m++;
        }
        if (m >= n || !tag_start(t[m])) {
            k++;
            continue;
        }
        size_t e = m;
        while (e < n && tag_char(t[e])) e++;
        long gt = tag_end(t, e, n);
        if (gt < 0) {
            k++;
            continue;
        }
        if (e - m == nlen && !strncasecmp(t + m, name, nlen)) {
            if (closing) {
                if (--depth == 0) return gt + 1;
            } else if (t[gt - 1] != '/') depth++;
        }
        k = (size_t)gt + 1;
    }
    return -1;
}

static size_t mask_tag_at(const char *t, size_t i, size_t n, unsigned char *masked) {
    if (!strncmp(t + i, "<!--", 4)) {
        const char *end = strstr(t + i + 4, "-->");
        size_t stop = end ? (size_t)(end - t) + 3 : n;
        memset(masked + i, 1, stop - i);
        return stop;
    }
    size_t j = i + 1;
    bool closing = false;
    if (j < n && t[j] == '/') {
        closing = true;
        j++;
    }
    if (j >= n || !tag_start(t[j])) return i;
    size_t e = j;
    while (e < n && tag_char(t[e])) e++;
    long gt = tag_end(t, e, n);
    if (gt < 0) return i;
    size_t te = (size_t)gt + 1;
    bool self = t[gt - 1] == '/';
    memset(masked + i, 1, te - i);
    if (closing || self) return te;
    long close = matching_close(t, te, n, t + j, e - j, masked);
    if (close < 0) return te;
    memset(masked + te, 1, (size_t)close - te);
    return (size_t)close;
}

/* FENCE_RE /^( {0,3})([`~]{3,})/ on one line: marker length (0 = no match) and its offset */
static size_t fence_open(const char *line, size_t len, size_t *indent) {
    size_t i = 0;
    while (i < len && i < 3 && line[i] == ' ') i++;
    size_t j = i;
    while (j < len && (line[j] == '`' || line[j] == '~')) j++;
    *indent = i;
    return j - i >= 3 ? j - i : 0;
}

char *keywords_mask_non_prose(const char *text) {
    if (!strchr(text, '`') && !strchr(text, '<') && !strstr(text, "~~~")) return xstrdup(text);
    size_t n = strlen(text);
    unsigned char *masked = xcalloc(n + 1, 1);
    char fence_ch = 0;
    size_t fence_len = 0, ls = 0;
    while (ls <= n) {
        const char *nlp = memchr(text + ls, '\n', n - ls);
        size_t nl = nlp ? (size_t)(nlp - text) : n;
        const char *line = text + ls;
        size_t llen = nl - ls, ind = 0, mk = fence_open(line, llen, &ind);
        if (fence_ch) {
            memset(masked + ls, 1, llen);
            if (mk && line[ind] == fence_ch && mk >= fence_len) {
                bool blank = true;
                for (size_t k = ind + mk; k < llen; k++) blank = blank && isspace((unsigned char)line[k]);
                if (blank) fence_ch = 0, fence_len = 0;
            }
        } else if (mk) {
            char ch = line[ind];
            bool bad = ch == '`' && memchr(line + ind + mk, '`', llen - ind - mk);
            if (!bad) {
                fence_ch = ch;
                fence_len = mk;
                memset(masked + ls, 1, llen);
            }
        }
        if (nl == n) break;
        ls = nl + 1;
    }
    size_t i = 0;
    while (i < n) {
        if (masked[i]) {
            i++;
            continue;
        }
        char c = text[i];
        if (c == '`') {
            size_t re = i;
            while (re < n && text[re] == '`') re++;
            size_t run = re - i, k = re;
            long close = -1;
            while (k < n) {
                if (masked[k]) {
                    k++;
                    continue;
                }
                if (text[k] == '`') {
                    size_t e = k;
                    while (e < n && text[e] == '`') e++;
                    if (e - k == run) {
                        close = (long)e;
                        break;
                    }
                    k = e;
                    continue;
                }
                k++;
            }
            if (close >= 0) {
                memset(masked + i, 1, (size_t)close - i);
                i = (size_t)close;
            } else i = re;
            continue;
        }
        if (c == '<') {
            size_t end = mask_tag_at(text, i, n, masked);
            i = end > i ? end : i + 1;
            continue;
        }
        i++;
    }
    char *out = xstrdup(text);
    for (size_t p = 0; p < n; p++)
        if (masked[p] && out[p] != '\n') out[p] = ' ';
    free(masked);
    return out;
}

/* ---------- detection ---------- */

static rx_t *g_letnum;
static pthread_once_t g_letnum_once = PTHREAD_ONCE_INIT;
static void letnum_init(void) { g_letnum = rx_compile("^[\\p{L}\\p{N}]", 0, NULL); }

/* \p{L} / \p{N} membership of the code point at s (the linear engine's Unicode tables) */
static bool letter_or_number(const char *s, size_t n) {
    pthread_once(&g_letnum_once, letnum_init);
    return g_letnum && rx_is_match(g_letnum, s, n, 0);
}

static size_t cp_len_at(const char *t, size_t i, size_t n) {
    size_t k = i + 1;
    while (k < n && ((unsigned char)t[k] & 0xC0) == 0x80) k++;
    return k - i;
}

/* LEFT_BOUNDARY (?<![\p{L}\p{N}_./\\-])(?<!::), then the word, then RIGHT_BOUNDARY
 * (?![\p{L}\p{N}_/\\-])(?!\.[\p{L}\p{N}_-])(?!\() */
/* the next standalone occurrence of `word` at or after `from`, -1 when none */
static long word_find(const char *text, const char *word, size_t from) {
    size_t n = strlen(text), wl = strlen(word);
    if (from > n) return -1;
    for (const char *p = strstr(text + from, word); p; p = strstr(p + 1, word)) {
        size_t i = (size_t)(p - text), e = i + wl;
        if (i > 0) {
            size_t ps = i - 1;
            while (ps > 0 && ((unsigned char)text[ps] & 0xC0) == 0x80) ps--;
            char c = text[ps];
            if (strchr("_./\\-", c) || letter_or_number(text + ps, i - ps)) continue;
            if (i >= 2 && text[i - 1] == ':' && text[i - 2] == ':') continue;
        }
        if (e < n) {
            char c = text[e];
            if (strchr("_/\\-(", c) || letter_or_number(text + e, cp_len_at(text, e, n))) continue;
            if (c == '.' && e + 1 < n && (strchr("_-", text[e + 1]) || letter_or_number(text + e + 1, cp_len_at(text, e + 1, n))))
                continue;
        }
        return (long)i;
    }
    return -1;
}

static bool word_match(const char *text, const char *word) { return word_find(text, word, 0) >= 0; }

bool keywords_contains(const char *text, const char *word) {
    if (!text || !word || !strstr(text, word) || !word_match(text, word)) return false;
    char *masked = keywords_mask_non_prose(text);
    bool hit = word_match(masked, word);
    free(masked);
    return hit;
}

size_t keywords_ranges(const char *text, keyword_range_t **out) {
    *out = NULL;
    bool any = false;
    for (size_t i = 0; i < ARRAY_LEN(KEYWORDS) && !any; i++) any = strstr(text, KEYWORDS[i].word) != NULL;
    if (!any) return 0;
    char *masked = keywords_mask_non_prose(text);
    VEC(keyword_range_t) v = {0};
    for (size_t k = 0; k < ARRAY_LEN(KEYWORDS); k++) {
        size_t wl = strlen(KEYWORDS[k].word), at = 0;
        long hit;
        while ((hit = word_find(masked, KEYWORDS[k].word, at)) >= 0) {
            keyword_range_t r = {(size_t)hit, (size_t)hit + wl, (int)k};
            vec_push(&v, r);
            at = (size_t)hit + wl;
        }
    }
    free(masked);
    *out = v.v;
    return v.n;
}

void keywords_glow_spec(int kw, double *hue_from, double *hue_to, double *light_from, double *light_to) {
    const keyword_t *k = &KEYWORDS[kw];
    *hue_from = k->hue_from;
    *hue_to = k->hue_to;
    *light_from = k->light_from ? k->light_from : 62;
    *light_to = k->light_to ? k->light_to : 62;
}

/* ---------- notices ---------- */

json_t *keywords_notices(app_t *a, const char *text) {
    json_t *out = json_arr();
    json_t *ctx = NULL;
    int64_t ts = now_ms();
    for (size_t i = 0; i < ARRAY_LEN(KEYWORDS); i++) {
        const keyword_t *k = &KEYWORDS[i];
        if (!keywords_enabled(k->id) || !keywords_contains(text, k->word)) continue;
        if (!k->notice) continue;
        if (!ctx) {
            ctx = json_obj();
            json_t *tools = json_arr();
            pthread_mutex_lock(&a->agent->mu);
            /* getEnabledToolNames: the active tools plus the mounted tool commands */
            for (size_t t = 0; t < a->agent->tools.n; t++) json_push(tools, json_str(a->agent->tools.v[t]->name));
            for (size_t t = 0; t < a->agent->all_tools.n; t++)
                if (a->agent->all_tools.v[t]->mounted) json_push(tools, json_str(a->agent->all_tools.v[t]->name));
            pthread_mutex_unlock(&a->agent->mu);
            json_set(ctx, "tools", tools);
        }
        bool ok = true;
        for (size_t r = 0; r < 2 && k->requires[r]; r++) {
            bool found = false;
            json_t *t;
            json_arr_foreach(json_get(ctx, "tools"), j, t) found = found || str_eq(json_sv(t), k->requires[r]);
            ok = ok && found;
        }
        if (!ok) continue;
        char path[128];
        snprintf(path, sizeof path, "coding-agent/prompts/system/%s", k->notice);
        char *body = prompt_render(path, ctx);
        char *trim = str_trim(body ? body : "");
        free(body);
        json_t *m = msg_developer_text(trim);
        free(trim);
        json_set_str(m, "attribution", "user");
        char type[64];
        snprintf(type, sizeof type, "%s-notice", k->id);
        json_set_str(m, "customType", type);
        json_set_bool(m, "display", false);
        json_set_int(m, "timestamp", ts);
        json_push(out, m);
    }
    json_free(ctx);
    return out;
}
