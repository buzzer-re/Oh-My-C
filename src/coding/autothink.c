/* autothink.c - `auto` thinking: provisional level, per-turn difficulty classification, tiny-message cleanup.
 * Ports omp auto-thinking/classifier.ts, tiny/message-preproc.ts, pi-tui thinking.ts (clampAutoThinkingEffort,
 * resolveProvisionalAutoLevel) and session/model-controls.ts applyAutoThinkingLevel. */
#include "coding/autothink.h"

#include "coding/app.h"
#include "coding/evalhost.h"
#include "coding/keywords.h"
#include "config/settings.h"
#include "prompt/template.h"

#include <ctype.h>
#include <pthread.h>

/* ---------- effort ladder ---------- */

size_t autothink_supported(const ai_model_t *m, ai_effort_t out[6]) {
    size_t n = 0;
    if (!m || !m->reasoning) return 0;
    json_t *efforts = json_get_arr(m->thinking, "efforts");
    for (int e = EFFORT_MINIMAL; e <= EFFORT_MAX; e++) { /* THINKING_EFFORTS order */
        json_t *it;
        json_arr_foreach(efforts, i, it) if (str_eq(json_sv(it), effort_name((ai_effort_t)e))) {
            out[n++] = (ai_effort_t)e;
            break;
        }
    }
    return n;
}

static bool unverified(const ai_model_t *m) { return m && json_get_bool(m->thinking, "unverified", false); }

bool autothink_clamp(const ai_model_t *m, ai_effort_t effort, ai_effort_t ceiling, ai_effort_t *out) {
    ai_effort_t sup[6];
    size_t n;
    if (m) n = autothink_supported(m, sup);
    else
        for (n = 0; n < 6; n++) sup[n] = (ai_effort_t)(EFFORT_MINIMAL + n);
    if (!n) return false;
    ai_effort_t floored[6], pool[6];
    size_t nf = 0, np = 0;
    for (size_t i = 0; i < n; i++)
        if (sup[i] >= EFFORT_LOW) floored[nf++] = sup[i];
    if (!nf) {
        memcpy(floored, sup, n * sizeof *sup);
        nf = n;
    }
    for (size_t i = 0; i < nf; i++)
        if (floored[i] <= ceiling) pool[np++] = floored[i];
    if (!np) return false;
    ai_effort_t chosen = pool[0];
    for (size_t i = 0; i < np; i++) {
        if (pool[i] > effort) break;
        chosen = pool[i];
    }
    *out = chosen;
    return true;
}

bool autothink_provisional(const ai_model_t *m, ai_effort_t *out) {
    if (!m || !m->reasoning || unverified(m)) return false;
    ai_effort_t pref = EFFORT_HIGH;
    const char *dl = json_get_str(m->thinking, "defaultLevel");
    if (dl) effort_parse(dl, &pref);
    if (pref == EFFORT_OFF) pref = EFFORT_HIGH;
    return autothink_clamp(m, pref == EFFORT_MAX ? EFFORT_XHIGH : pref, EFFORT_XHIGH, out);
}

/* ---------- tiny-message preprocessing (UTF-16 lengths approximated by code points) ---------- */

#define MAX_TINY_MESSAGE_CHARS 2000
#define MIN_STRIPPED_TITLE_CHARS 12
#define RAW_SCAN_EDGE_CHARS 262144
#define CLEAN_WINDOW_EDGE_CHARS 16384

static size_t cp_count(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* byte offset of code point `idx` (len when past the end) */
static size_t cp_offset(const char *s, size_t len, size_t idx) {
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
        if (n++ == idx) return i;
    }
    return len;
}

static char *window_edges(const char *m, size_t edge) {
    size_t len = strlen(m), n = cp_count(m, len);
    if (n <= edge * 2) return xstrdup(m);
    size_t head = cp_offset(m, len, edge), tail = cp_offset(m, len, n - edge);
    return xasprintf("%.*s\n…\n%s", (int)head, m, m + tail);
}

static char *strip_ansi(const char *m) {
    sb_t b = SB_INIT;
    for (const char *p = m; *p;) {
        if (p[0] == '\x1b' && p[1] == '[') {
            const char *q = p + 2;
            while (isdigit((unsigned char)*q) || *q == ';') q++;
            if (*q == 'm') {
                p = q + 1;
                continue;
            }
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

static char *collapse_ws(const char *m) {
    sb_t b = SB_INIT;
    for (const char *p = m; *p;) {
        if (*p == ' ' || *p == '\t') {
            while (*p == ' ' || *p == '\t') p++;
            sb_putc(&b, ' ');
        } else if (*p == '\n') {
            size_t k = 0;
            while (p[k] == '\n') k++;
            sb_appendn(&b, "\n\n", k >= 3 ? 2 : k);
            p += k;
        } else sb_putc(&b, *p++);
    }
    char *s = sb_detach(&b);
    char *t = str_trim(s);
    free(s);
    return t;
}

/* /```+[\s\S]*?(?:```+|$)/g -> " " */
static char *strip_fences(const char *m) {
    sb_t b = SB_INIT;
    for (const char *p = m; *p;) {
        if (!strncmp(p, "```", 3)) {
            const char *q = p;
            while (*q == '`') q++;
            const char *close = strstr(q, "```");
            if (close) {
                while (*close == '`') close++;
                p = close;
            } else p += strlen(p);
            sb_putc(&b, ' ');
            continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

static char *strip_code_blocks(const char *m) {
    char *r = strip_fences(m);
    char *cleaned = collapse_ws(r);
    free(r);
    if (cp_count(cleaned, strlen(cleaned)) >= MIN_STRIPPED_TITLE_CHARS) return cleaned;
    free(cleaned);
    return xstrdup(m);
}

static bool is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* /<([a-zA-Z][\w-]*)(?:\s[^>]*)?>[\s\S]*?<\/\1>/g -> " " */
static char *strip_xml_blocks(const char *m) {
    sb_t b = SB_INIT;
    for (const char *p = m; *p;) {
        if (*p == '<' && isalpha((unsigned char)p[1])) {
            const char *q = p + 1;
            while (is_word(*q) || *q == '-') q++;
            size_t nlen = (size_t)(q - p - 1);
            if (isspace((unsigned char)*q)) {
                while (*q && *q != '>') q++;
            }
            if (*q == '>') {
                char *close = xasprintf("</%.*s>", (int)nlen, p + 1);
                const char *end = strstr(q + 1, close);
                if (end) {
                    p = end + strlen(close);
                    free(close);
                    sb_putc(&b, ' ');
                    continue;
                }
                free(close);
            }
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

/* /\b[0-9a-fA-F]{12,}\b/g -> first 7 */
static char *shorten_hashes(const char *m) {
    sb_t b = SB_INIT;
    for (const char *p = m; *p;) {
        if (is_word(*p) && (p == m || !is_word(p[-1]))) {
            const char *q = p;
            bool hex = true;
            while (is_word(*q)) {
                if (!isxdigit((unsigned char)*q)) hex = false;
                q++;
            }
            if (hex && q - p >= 12) sb_appendn(&b, p, 7);
            else sb_appendn(&b, p, (size_t)(q - p));
            p = q;
            continue;
        }
        sb_putc(&b, *p++);
    }
    return sb_detach(&b);
}

static char *truncate_tiny(const char *m) {
    size_t len = strlen(m), n = cp_count(m, len);
    if (n <= MAX_TINY_MESSAGE_CHARS) return xstrdup(m);
    size_t omitted = n - MAX_TINY_MESSAGE_CHARS, head = 0, tail = 0;
    char marker[64];
    for (int pass = 0; pass < 2; pass++) {
        snprintf(marker, sizeof marker, "\n[… %zu chars omitted …]\n", omitted);
        size_t mk = cp_count(marker, strlen(marker));
        size_t kept = MAX_TINY_MESSAGE_CHARS > mk ? MAX_TINY_MESSAGE_CHARS - mk : 0;
        head = (kept * 2 + 2) / 3; /* Math.ceil(kept * 2 / 3) */
        tail = kept - head;
        omitted = n - head - tail;
    }
    snprintf(marker, sizeof marker, "\n[… %zu chars omitted …]\n", omitted);
    size_t ho = cp_offset(m, len, head), to = cp_offset(m, len, n - tail);
    return xasprintf("%.*s%s%s", (int)ho, m, marker, tail ? m + to : "");
}

char *tiny_preprocess(const char *message) {
    char *w = window_edges(message ? message : "", RAW_SCAN_EDGE_CHARS);
    char *a = strip_ansi(w);
    char *c = strip_code_blocks(a);
    char *w2 = window_edges(c, CLEAN_WINDOW_EDGE_CHARS);
    char *x = strip_xml_blocks(w2);
    char *h = shorten_hashes(x);
    char *cl = collapse_ws(h);
    char *out = truncate_tiny(cl);
    free(w);
    free(a);
    free(c);
    free(w2);
    free(x);
    free(h);
    free(cl);
    return out;
}

/* ---------- classification ---------- */

#define AUTO_THINKING_TIMEOUT_MS 4000

typedef struct {
    volatile sig_atomic_t flag;
    volatile int done;
    int64_t deadline;
} watchdog_t;

static void *watchdog(void *p) {
    watchdog_t *w = p;
    while (!w->done) {
        if (mono_ms() >= w->deadline) {
            w->flag = 1;
            break;
        }
        sleep_ms(20);
    }
    return NULL;
}

static const char *const LEVEL_NAMES[] = {"low", "medium", "high", "xhigh", "max"};

/* the `level` question (auto-thinking.json) for the target model: instructions rendered with withMax, criteria
 * narrowed to supported levels (and to xhigh unless withMax). Returns the number of levels; *q owned. */
static size_t level_question(const ai_model_t *m, bool with_max, json_t **q, ai_effort_t levels[5]) {
    *q = NULL;
    char *raw = prompt_load("coding-agent/prompts/judge/auto-thinking.json");
    json_t *file = raw ? json_parse(raw, strlen(raw), NULL) : NULL;
    free(raw);
    json_t *lv = json_get_obj(file, "level");
    if (!lv) {
        json_free(file);
        return 0;
    }
    ai_effort_t sup[6];
    size_t ns = autothink_supported(m, sup), n = 0;
    json_t *crit = json_obj();
    for (size_t i = 0; i < ARRAY_LEN(LEVEL_NAMES); i++) {
        if (!with_max && !strcmp(LEVEL_NAMES[i], "max")) continue;
        ai_effort_t e;
        effort_parse(LEVEL_NAMES[i], &e);
        bool ok = false;
        for (size_t k = 0; k < ns; k++) ok = ok || sup[k] == e;
        const char *rubric = json_get_str(json_get_obj(lv, "criteria"), LEVEL_NAMES[i]);
        if (!ok || !rubric) continue;
        json_set_str(crit, LEVEL_NAMES[i], rubric);
        levels[n++] = e;
    }
    json_t *ctx = json_obj();
    if (with_max) json_set_bool(ctx, "withMax", true);
    char *instr = tpl_render(json_get_str(lv, "instructions") ? json_get_str(lv, "instructions") : "", ctx);
    json_free(ctx);
    json_t *question = json_obj();
    json_set_str(question, "type", "choice");
    json_set_str(question, "instructions", instr);
    json_set(question, "criteria", crit);
    free(instr);
    *q = json_obj();
    json_set(*q, "level", question);
    json_free(file);
    return n;
}

/* classifyDifficulty: false on any failure (no judge, bad answer, timeout) */
static bool classify(app_t *a, const char *request, ai_effort_t *out) {
    const ai_model_t *m = a->model;
    ai_effort_t sup[6];
    size_t ns = autothink_supported(m, sup);
    bool max_ok = false;
    for (size_t k = 0; k < ns; k++) max_ok = max_ok || sup[k] == EFFORT_MAX;
    ai_effort_t ceiling = str_eq(settings_str("providers.autoThinkingMaxEffort", "xhigh"), "max") && max_ok ? EFFORT_MAX
                                                                                                          : EFFORT_XHIGH;
    json_t *qs;
    ai_effort_t levels[5];
    size_t nl = level_question(m, ceiling == EFFORT_MAX, &qs, levels);
    /* one eligible tier below max needs no judge; no online tier clamps low into the xhigh-capped pool */
    if (nl == 1 && levels[0] != EFFORT_MAX) {
        json_free(qs);
        *out = levels[0];
        return true;
    }
    if (nl == 0) {
        json_free(qs);
        return autothink_clamp(m, EFFORT_LOW, EFFORT_XHIGH, out);
    }
    ai_effort_t effort = levels[0];
    bool ok = true;
    if (nl > 1) {
        json_t *state = json_obj();
        char *pre = tiny_preprocess(request);
        json_set_str(state, "request", pre);
        free(pre);
        watchdog_t w = {.deadline = mono_ms() + AUTO_THINKING_TIMEOUT_MS};
        pthread_t th;
        bool have_th = pthread_create(&th, NULL, watchdog, &w) == 0;
        double usage[3] = {0};
        char *err = NULL;
        json_t *ans = evalhost_judge(a, state, qs, &w.flag, usage, &err);
        w.done = 1;
        if (have_th) pthread_join(th, NULL);
        a->total_cost += usage[2];
        const char *choice = json_get_str(json_get(ans, "level"), "choice");
        ok = false;
        for (size_t i = 0; choice && i < nl; i++)
            if (str_eq(choice, effort_name(levels[i]))) {
                effort = levels[i];
                ok = true;
            }
        json_free(ans);
        json_free(state);
        free(err);
    }
    json_free(qs);
    return ok && autothink_clamp(m, effort, ceiling, out);
}

bool autothink_resolve(app_t *a, const char *prompt, ai_effort_t *out) {
    const ai_model_t *m = a->model;
    ai_effort_t sup[6];
    if (!m || !m->reasoning || unverified(m) || !autothink_supported(m, sup)) return false;
    ai_effort_t e;
    bool ok;
    if (keywords_enabled("ultrathink") && keywords_contains(prompt, "ultrathink")) ok = autothink_clamp(m, EFFORT_MAX, EFFORT_MAX, &e);
    else ok = classify(a, prompt, &e);
    if (!ok) { /* keep the last classified level, else the provisional one */
        if (a->auto_resolved >= 0) {
            e = (ai_effort_t)a->auto_resolved;
            ok = true;
        } else ok = autothink_provisional(m, &e);
    }
    if (!ok) return false;
    *out = e;
    return true;
}
