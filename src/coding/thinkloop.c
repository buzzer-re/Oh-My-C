/* thinkloop.c - stream-level loop guard (see thinkloop.h). Lengths are measured in bytes where omp counts UTF-16
 * units; ASCII reasoning (the common case) matches exactly. */
#include "coding/thinkloop.h"

#include "config/settings.h"
#include "text/regex.h"
#include "text/rxbt.h"

#include <ctype.h>
#include <pthread.h>

#define EXACT_TAIL_WINDOW 4096
#define EXACT_MAX_UNIT 1024
#define EXACT_CHECK_STRIDE 128
#define EXACT_SHORT_MAX_UNIT 60
#define EXACT_SHORT_MIN_REPEATED_CHARS 180
#define EXACT_LONG_MIN_REPEATED_CHARS 1024
#define EXACT_PUNCTUATION_MIN_REPEATED_CHARS EXACT_LONG_MIN_REPEATED_CHARS
#define SEGMENT_CHAR_CAP 700
#define SEGMENT_MIN_NORM_CHARS 60
#define SEGMENT_WINDOW 16
#define SEGMENT_SIMILARITY 0.8
#define SEGMENT_MIN_COUNT 8
#define SEGMENT_MIN_CLUSTER 4
#define LEX_NOVELTY_WINDOW 8
#define LEX_STALL_NOVELTY_FLOOR 0.2
#define LEX_STALL_MIN_RUN 8
#define GEMINI_HEADER_RUNAWAY_THRESHOLD 64

/* ---------- string sets ---------- */

typedef struct {
    hmap_t m;
} sset_t;

static void sset_init(sset_t *s) { hmap_init(&s->m); }
static void sset_add(sset_t *s, const char *k) { hmap_put(&s->m, k, (void *)1); }
static bool sset_has(const sset_t *s, const char *k) { return hmap_has(&s->m, k); }
static size_t sset_len(const sset_t *s) { return s->m.n; }
static void sset_free(sset_t *s) { hmap_free(&s->m, NULL); }

static sset_t *sset_new(void) {
    sset_t *s = xcalloc(1, sizeof *s);
    sset_init(s);
    return s;
}

static void sset_destroy(sset_t *s) {
    if (!s) return;
    sset_free(s);
    free(s);
}

/* ---------- regex helpers ---------- */

static rxbt_t *g_code_line, *g_heading, *g_bold_title;
static rx_t *g_anchor, *g_letter, *g_numsp;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void rx_init(void) {
    g_code_line = rxbt_compile("^(?:[ \\t]*```.*|(?: {2,}|\\t)(?![ \\t]*(?:[-*+]|\\d+[.)])[ \\t]).*|.*[{}\\[\\](;,>][ \\t]*)$",
                               RX_MULTILINE, NULL);
    g_heading = rxbt_compile("^[ \\t]*#{1,6}[ \\t].*$", RX_MULTILINE, NULL);
    g_bold_title = rxbt_compile("^[ \\t]*\\*{2,3}.+?\\*{2,3}[ \\t]*$", RX_MULTILINE, NULL);
    g_anchor = rx_compile("`[^`]+`|\\b\\w{2,}\\.[a-zA-Z]\\w{0,4}\\b|[\\w-]+(?:/[\\w-]+){2,}|\\b\\w+_\\w+\\b|\\b[a-z]+[A-Z]\\w*\\b|"
                          "\\b[A-Z][a-z]+[A-Z]\\w*\\b",
                          0, NULL);
    g_letter = rx_compile("\\p{L}", 0, NULL);
    g_numsp = rx_compile("^[\\p{N}\\s]*$", 0, NULL);
}

/* String.replace(re /gm, "") */
static char *bt_strip(rxbt_t *r, const char *s) {
    if (!r) return xstrdup(s);
    size_t len = strlen(s), pos = 0, last = 0;
    ptrdiff_t caps[2];
    sb_t b = SB_INIT;
    while (pos <= len && rxbt_search(r, s, len, pos, caps, 2)) {
        size_t ms = (size_t)caps[0], me = (size_t)caps[1];
        sb_appendn(&b, s + last, ms - last);
        last = me;
        pos = me > ms ? me : me + 1;
    }
    sb_appendn(&b, s + last, len - last);
    return sb_detach(&b);
}

/* ---------- exact suffix cycles ---------- */

static bool has_letter_or_emoji(const char *u, size_t n) {
    size_t s0, e0;
    if (g_letter && rx_find(g_letter, u, n, 0, &s0, &e0)) return true;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        int k = utf8_decode(u + i, n - i, &cp);
        if ((cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF)) return true; /* Extended_Pictographic (approx.) */
        i += (size_t)k;
    }
    return false;
}

static size_t cps(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

static char *exact_cycle(const char *t, size_t n) {
    if (n < EXACT_SHORT_MIN_REPEATED_CHARS) return NULL;
    size_t last = n - 1, max_unit = MIN((size_t)EXACT_MAX_UNIT, n / 3);
    static __thread uint16_t z[EXACT_MAX_UNIT + 1];
    size_t left = 0, right = 0;
    for (size_t i = 1; i <= max_unit; i++) {
        size_t zi = i <= right ? MIN(right - i + 1, (size_t)z[i - left]) : 0;
        while (i + zi < n && t[last - zi] == t[last - i - zi]) zi++;
        z[i] = (uint16_t)MIN(zi, (size_t)65535);
        if (i + zi - 1 > right) {
            left = i;
            right = i + zi - 1;
        }
    }
    for (size_t len = 2; len <= max_unit; len++) {
        size_t count = 1 + z[len] / len, min_count = len <= EXACT_SHORT_MAX_UNIT ? 4 : 3;
        if (count < min_count) continue;
        const char *unit = t + n - len;
        if (has_letter_or_emoji(unit, len)) {
            size_t min_chars = len <= EXACT_SHORT_MAX_UNIT && !memchr(unit, '\n', len) ? EXACT_SHORT_MIN_REPEATED_CHARS
                                                                                        : EXACT_LONG_MIN_REPEATED_CHARS;
            if (len * count >= min_chars)
                return xasprintf("repeated an exact %zu-character cycle %zu× back-to-back", cps(unit, len), count);
            continue;
        }
        if (g_numsp && rx_is_match(g_numsp, unit, len, 0)) continue;
        if (len * count >= EXACT_PUNCTUATION_MIN_REPEATED_CHARS)
            return xasprintf("repeated an exact %zu-character cycle %zu× back-to-back", cps(unit, len), count);
    }
    return NULL;
}

/* ---------- detector ---------- */

struct tl_detector {
    bool semantic;
    sb_t tail;
    size_t scanned;
    sb_t pending;
    sset_t *window[SEGMENT_WINDOW];
    size_t nwindow;
    size_t count;
    sset_t *words[LEX_NOVELTY_WINDOW];
    size_t nwords;
    int lex_run;
    sset_t *anchors[LEX_NOVELTY_WINDOW];
    size_t nanchors;
};

tl_detector_t *tl_detector_new(bool semantic) {
    pthread_once(&g_once, rx_init);
    tl_detector_t *d = xcalloc(1, sizeof *d);
    d->semantic = semantic;
    return d;
}

void tl_detector_free(tl_detector_t *d) {
    if (!d) return;
    sb_free(&d->tail);
    sb_free(&d->pending);
    for (size_t i = 0; i < d->nwindow; i++) sset_destroy(d->window[i]);
    for (size_t i = 0; i < d->nwords; i++) sset_destroy(d->words[i]);
    for (size_t i = 0; i < d->nanchors; i++) sset_destroy(d->anchors[i]);
    free(d);
}

static void push_bounded(sset_t **arr, size_t *n, size_t cap, sset_t *s) {
    if (*n == cap) {
        sset_destroy(arr[0]);
        memmove(arr, arr + 1, (cap - 1) * sizeof *arr);
        (*n)--;
    }
    arr[(*n)++] = s;
}

/* normalizeSegment: lowercase, unwrap code spans, keep [a-z0-9] runs that contain a letter */
static char *normalize_segment(const char *seg) {
    sb_t un = SB_INIT;
    for (const char *p = seg; *p;) { /* /`([^`]*)`/g -> " $1 " */
        if (*p == '`') {
            const char *q = strchr(p + 1, '`');
            if (q) {
                sb_putc(&un, ' ');
                sb_appendn(&un, p + 1, (size_t)(q - p - 1));
                sb_putc(&un, ' ');
                p = q + 1;
                continue;
            }
        }
        sb_putc(&un, *p++);
    }
    sb_t out = SB_INIT;
    const char *s = sb_cstr(&un);
    for (size_t i = 0; s[i];) {
        if (!isalnum((unsigned char)s[i]) || ((unsigned char)s[i] & 0x80)) {
            i++;
            continue;
        }
        size_t j = i;
        bool letter = false;
        while (s[j] && isalnum((unsigned char)s[j]) && !((unsigned char)s[j] & 0x80)) {
            letter = letter || isalpha((unsigned char)s[j]);
            j++;
        }
        if (letter) {
            if (out.len) sb_putc(&out, ' ');
            for (size_t k = i; k < j; k++) sb_putc(&out, (char)tolower((unsigned char)s[k]));
        }
        i = j;
    }
    sb_free(&un);
    return sb_detach(&out);
}

static sset_t *trigrams(const char *norm) {
    sset_t *s = sset_new();
    strvec_t w = str_split(norm, " ");
    strvec_t words = {0};
    for (size_t i = 0; i < w.n; i++)
        if (*w.v[i]) strvec_push(&words, w.v[i]);
    if (words.n < 3) {
        if (words.n) {
            char *j = strvec_join(&words, " ");
            sset_add(s, j);
            free(j);
        }
    } else {
        for (size_t i = 0; i + 3 <= words.n; i++) {
            char *g = xasprintf("%s %s %s", words.v[i], words.v[i + 1], words.v[i + 2]);
            sset_add(s, g);
            free(g);
        }
    }
    strvec_free(&w);
    strvec_free(&words);
    return s;
}

static double jaccard(const sset_t *a, const sset_t *b) {
    if (!sset_len(a) || !sset_len(b)) return 0;
    const sset_t *small = sset_len(a) < sset_len(b) ? a : b, *large = small == a ? b : a;
    size_t inter = 0;
    hmap_foreach(&small->m, it) if (sset_has(large, it->key)) inter++;
    size_t uni = sset_len(a) + sset_len(b) - inter;
    return uni ? (double)inter / (double)uni : 0;
}

static char *consume_segment(tl_detector_t *d, const char *raw) {
    char *a = bt_strip(g_code_line, raw);
    char *b = bt_strip(g_heading, a);
    char *segment = bt_strip(g_bold_title, b);
    free(a);
    free(b);
    char *norm = normalize_segment(segment);
    if (strlen(norm) < SEGMENT_MIN_NORM_CHARS) {
        free(norm);
        free(segment);
        return NULL;
    }
    sset_t *fp = trigrams(norm);
    size_t cluster = 1;
    for (size_t i = 0; i < d->nwindow; i++)
        if (jaccard(fp, d->window[i]) >= SEGMENT_SIMILARITY) cluster++;
    sset_t *words = sset_new();
    strvec_t ws = str_split(norm, " ");
    for (size_t i = 0; i < ws.n; i++)
        if (*ws.v[i]) sset_add(words, ws.v[i]);
    strvec_free(&ws);
    size_t prior = 0, unseen = 0;
    sset_t pv;
    sset_init(&pv);
    for (size_t i = 0; i < d->nwords; i++) hmap_foreach(&d->words[i]->m, it) sset_add(&pv, it->key);
    prior = sset_len(&pv);
    hmap_foreach(&words->m, it) if (!sset_has(&pv, it->key)) unseen++;
    sset_free(&pv);
    double novelty = prior == 0 ? 1.0 : (double)unseen / (double)sset_len(words);
    sset_t *anchors = sset_new();
    if (g_anchor) {
        size_t len = strlen(segment), pos = 0, ms, me;
        while (pos <= len && rx_find(g_anchor, segment, len, pos, &ms, &me)) {
            sb_t k = SB_INIT;
            for (size_t i = ms; i < me; i++)
                if (segment[i] != '`') sb_putc(&k, (char)tolower((unsigned char)segment[i]));
            sset_add(anchors, sb_cstr(&k));
            sb_free(&k);
            pos = me > ms ? me : me + 1;
        }
    }
    bool new_anchor = false;
    hmap_foreach(&anchors->m, it) {
        bool seen = false;
        for (size_t i = 0; i < d->nanchors && !seen; i++) seen = sset_has(d->anchors[i], it->key);
        if (!seen) {
            new_anchor = true;
            break;
        }
    }
    if (novelty <= LEX_STALL_NOVELTY_FLOOR && !new_anchor) d->lex_run++;
    else d->lex_run = 0;
    push_bounded(d->window, &d->nwindow, SEGMENT_WINDOW, fp);
    push_bounded(d->words, &d->nwords, LEX_NOVELTY_WINDOW, words);
    push_bounded(d->anchors, &d->nanchors, LEX_NOVELTY_WINDOW, anchors);
    d->count++;
    free(norm);
    free(segment);
    if (d->count >= SEGMENT_MIN_COUNT) {
        if (cluster >= SEGMENT_MIN_CLUSTER)
            return xasprintf("%zu near-identical segments within the last %d", cluster, SEGMENT_WINDOW);
        if (d->lex_run >= LEX_STALL_MIN_RUN) return xasprintf("%d low-information segments recycling recent wording", d->lex_run);
    }
    return NULL;
}

/* consume `raw` in SEGMENT_CHAR_CAP chunks */
static char *consume_chunks(tl_detector_t *d, const char *raw, size_t n) {
    for (size_t off = 0; off < n;) {
        size_t take = MIN((size_t)SEGMENT_CHAR_CAP, n - off);
        char *chunk = xstrndup(raw + off, take);
        off += take;
        char *hit = consume_segment(d, chunk);
        free(chunk);
        if (hit) return hit;
    }
    return NULL;
}

/* /\n\s*\n/ in s: start and end of the first boundary */
static bool find_boundary(const char *s, size_t n, size_t *bs, size_t *be) {
    for (size_t p = 0; p < n; p++) {
        if (s[p] != '\n') continue;
        size_t e = p + 1, lastnl = 0;
        bool found = false;
        while (e < n && isspace((unsigned char)s[e])) {
            if (s[e] == '\n') {
                lastnl = e;
                found = true;
            }
            e++;
        }
        if (found) {
            *bs = p;
            *be = lastnl + 1;
            return true;
        }
    }
    return false;
}

char *tl_detector_push(tl_detector_t *d, const char *delta, size_t len) {
    if (!delta || !len) return NULL;
    sb_appendn(&d->tail, delta, len);
    if (d->tail.len > EXACT_TAIL_WINDOW) {
        size_t cut = d->tail.len - EXACT_TAIL_WINDOW;
        while (cut < d->tail.len && ((unsigned char)d->tail.s[cut] & 0xC0) == 0x80) cut++;
        memmove(d->tail.s, d->tail.s + cut, d->tail.len - cut);
        sb_truncate(&d->tail, d->tail.len - cut);
    }
    d->scanned += len;
    if (d->scanned >= EXACT_CHECK_STRIDE || len >= EXACT_CHECK_STRIDE) {
        d->scanned = 0;
        char *hit = exact_cycle(d->tail.s, d->tail.len);
        if (hit) return hit;
    }
    if (!d->semantic) return NULL;
    sb_appendn(&d->pending, delta, len);
    for (;;) {
        size_t bs, be;
        char *raw;
        size_t rawn;
        if (find_boundary(d->pending.s, d->pending.len, &bs, &be)) {
            raw = xstrndup(d->pending.s, bs);
            rawn = bs;
            memmove(d->pending.s, d->pending.s + be, d->pending.len - be);
            sb_truncate(&d->pending, d->pending.len - be);
        } else if (d->pending.len > SEGMENT_CHAR_CAP) {
            raw = xstrndup(d->pending.s, SEGMENT_CHAR_CAP);
            rawn = SEGMENT_CHAR_CAP;
            memmove(d->pending.s, d->pending.s + SEGMENT_CHAR_CAP, d->pending.len - SEGMENT_CHAR_CAP);
            sb_truncate(&d->pending, d->pending.len - SEGMENT_CHAR_CAP);
        } else {
            return NULL;
        }
        char *hit = consume_chunks(d, raw, rawn);
        free(raw);
        if (hit) return hit;
    }
}

char *tl_detector_flush(tl_detector_t *d) {
    char *hit = exact_cycle(d->tail.s ? d->tail.s : "", d->tail.len);
    if (hit) return hit;
    if (!d->semantic || !d->pending.len) return NULL;
    char *rest = xstrndup(d->pending.s, d->pending.len);
    size_t n = d->pending.len;
    sb_truncate(&d->pending, 0);
    hit = consume_chunks(d, rest, n);
    free(rest);
    return hit;
}

/* ---------- stream guard ---------- */

struct thinkloop {
    int level; /* 0 off, 1 exact, 2 semantic */
    bool check_text, gemini_reminder;
    tl_detector_t *thinking, *text;
    bool thinking_armed, text_armed, text_started, fired;
    char *error;
    /* GeminiHeaderRunDetector */
    bool header_active;
    sb_t header_pending;
    int header_count;
    bool header_fired;
};

thinkloop_t *thinkloop_new(void) { return xcalloc(1, sizeof(thinkloop_t)); }

static void clear(thinkloop_t *t) {
    tl_detector_free(t->thinking);
    tl_detector_free(t->text);
    t->thinking = t->text = NULL;
    free(t->error);
    t->error = NULL;
    sb_free(&t->header_pending);
}

void thinkloop_free(thinkloop_t *t) {
    if (!t) return;
    clear(t);
    free(t);
}

/* loopGuardLevel: the catalog's thinking-loop-guard axis by model class (anthropic off; gemini, xai, deepseek
 * semantic; everything else exact) */
static int model_level(const ai_model_t *m) {
    const char *id = m && m->id ? m->id : "";
    const char *prov = m && m->provider ? m->provider : "";
    if (str_istr(id, "claude") || str_eq(prov, "anthropic")) return 0;
    if (str_istr(id, "gemini") || str_istr(id, "grok") || str_istr(id, "deepseek")) return 2;
    return 1;
}

static bool is_gemini(const ai_model_t *m) { return m && m->id && str_istr(m->id, "gemini"); }

void thinkloop_reset(thinkloop_t *t, const ai_model_t *m) {
    clear(t);
    const char *env = getenv("PI_NO_THINKING_LOOP_GUARD");
    bool enabled = settings_bool("model.loopGuard.enabled", true) && !(env && !strcmp(env, "1"));
    t->level = enabled ? model_level(m) : 0;
    t->check_text = settings_bool("model.loopGuard.checkAssistantContent", true);
    t->gemini_reminder = enabled && settings_bool("model.loopGuard.toolCallReminder", true) && is_gemini(m);
    t->thinking_armed = true;
    t->text_armed = t->check_text;
    t->text_started = false;
    t->fired = false;
    t->header_active = false;
    t->header_count = 0;
    t->header_fired = false;
    if (t->level) {
        t->thinking = tl_detector_new(t->level == 2);
        t->text = tl_detector_new(t->level == 2);
    }
}

/* isReasoningSummaryHeader: `## …` or a whole-line `**Title**` / `***Title***` */
static bool summary_header(const char *line) {
    if (*line == '#') {
        const char *p = line;
        int n = 0;
        while (*p == '#' && n < 7) p++, n++;
        if (n >= 1 && n <= 6 && (*p == ' ' || *p == '\t')) {
            while (*p == ' ' || *p == '\t') p++;
            if (*p && !isspace((unsigned char)*p)) return true;
        }
    }
    size_t len = strlen(line); /* /^\*{2,3}.+\*{2,3}$/ */
    if (len >= 5 && !strncmp(line, "**", 2) && !strcmp(line + len - 2, "**")) return true;
    return false;
}

static bool header_push(thinkloop_t *t, const char *delta, size_t n) {
    if (t->header_fired || !n) return false;
    sb_appendn(&t->header_pending, delta, n);
    for (;;) {
        char *nl = memchr(t->header_pending.s, '\n', t->header_pending.len);
        if (!nl) return false;
        size_t ll = (size_t)(nl - t->header_pending.s);
        char *raw = xstrndup(t->header_pending.s, ll);
        char *line = str_trim(raw);
        free(raw);
        memmove(t->header_pending.s, nl + 1, t->header_pending.len - ll - 1);
        sb_truncate(&t->header_pending, t->header_pending.len - ll - 1);
        bool hit = *line && summary_header(line) && ++t->header_count >= GEMINI_HEADER_RUNAWAY_THRESHOLD;
        free(line);
        if (hit) {
            t->header_fired = true;
            return true;
        }
    }
}

int thinkloop_on_event(thinkloop_t *t, const ai_event_t *ev) {
    if (!t || t->fired) return THINKLOOP_OK;
    /* Gemini header runaway (StreamGuards.onAssistantEvent) */
    if (t->gemini_reminder) {
        if (ev->type == AI_EV_THINKING_START) {
            t->header_active = true;
            sb_truncate(&t->header_pending, 0);
            t->header_count = 0;
            t->header_fired = false;
        } else if (t->header_active) {
            if (ev->type == AI_EV_THINKING_DELTA && header_push(t, ev->delta, ev->delta_len)) {
                t->fired = true;
                return THINKLOOP_GEMINI_HEADERS;
            }
            if (ev->type == AI_EV_TEXT_START || ev->type == AI_EV_TOOLCALL_START) {
                sb_truncate(&t->header_pending, 0);
                t->header_count = 0;
                t->header_fired = false;
            }
        }
    }
    if (!t->level) return THINKLOOP_OK;
    char *detail = NULL;
    switch (ev->type) {
    case AI_EV_THINKING_DELTA:
        if (!t->text_started) {
            t->thinking_armed = true;
            detail = tl_detector_push(t->thinking, ev->delta, ev->delta_len);
        }
        break;
    case AI_EV_THINKING_END:
        if (t->thinking_armed) detail = tl_detector_flush(t->thinking);
        break;
    case AI_EV_TEXT_DELTA:
        if (ev->delta_len) {
            t->thinking_armed = false;
            t->text_started = true;
        }
        if (t->text_armed) detail = tl_detector_push(t->text, ev->delta, ev->delta_len);
        break;
    case AI_EV_TOOLCALL_START:
    case AI_EV_TOOLCALL_DELTA: t->text_armed = false; break;
    case AI_EV_DONE:
        if (t->thinking_armed) detail = tl_detector_flush(t->thinking);
        if (!detail && t->text_armed) detail = tl_detector_flush(t->text);
        break;
    default: break;
    }
    if (!detail) return THINKLOOP_OK;
    t->fired = true;
    free(t->error);
    t->error = xasprintf("Thinking loop detected: the model repeated near-identical content (%s). Treating as a stream stall "
                         "and retrying.",
                         detail);
    free(detail);
    return THINKLOOP_LOOP;
}

const char *thinkloop_error(const thinkloop_t *t) { return t ? t->error : NULL; }
int thinkloop_header_count(const thinkloop_t *t) { return t ? t->header_count : 0; }
