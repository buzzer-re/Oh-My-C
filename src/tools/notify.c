/* notify.c - see notify.h */
#include "notify.h"

#include "jobs.h"
#include "prompt/template.h"
#include "text/regex.h"

#include <pthread.h>

#define NOTIFY_BATCH_MS 200
#define NOTIFY_MAX_LINE_CHARS 400
#define NOTIFY_REFILL_MS 20000
#define NOTIFY_PENDING_CAP (1u << 20) /* an unterminated line longer than this is matched as it stands */

struct notifier {
    pthread_mutex_t mu;
    rx_t *rx;
    sb_t partial;      /* bytes after the last newline */
    json_t *lines;     /* matches of the open batching window */
    int64_t omitted;
    int64_t window_at; /* first match of the open window (0: none) */
    int allowance;
    int64_t refilled_at;
    bool refill, stopped;
    notify_emit_fn emit;
    void *ud;
};

/* ---------- the window ticker: one thread flushes every open window once it is 200 ms old ---------- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static VEC(notifier_t *) g_live;
static bool g_ticker;

static void flush_locked(notifier_t *n, json_t **lines, int64_t *omitted, bool *stopped) {
    int64_t now = mono_ms();
    int64_t refill = n->refill ? (now - n->refilled_at) / NOTIFY_REFILL_MS : 0;
    if (refill > 0) {
        n->allowance = (int)MIN((int64_t)NOTIFY_BURST, n->allowance + refill);
        n->refilled_at += refill * NOTIFY_REFILL_MS;
    }
    n->allowance -= 1;
    *lines = n->lines;
    *omitted = n->omitted;
    *stopped = n->allowance <= 0;
    n->lines = json_arr();
    n->omitted = 0;
    n->window_at = 0;
    n->stopped = *stopped;
}

static void *ticker_main(void *p) {
    UNUSED(p);
    pthread_mutex_lock(&g_mu);
    for (;;) {
        int64_t now = mono_ms(), next = -1;
        for (size_t i = 0; i < g_live.n; i++) {
            notifier_t *n = g_live.v[i];
            pthread_mutex_lock(&n->mu);
            bool due = !n->stopped && n->window_at && now - n->window_at >= NOTIFY_BATCH_MS;
            json_t *lines = NULL;
            int64_t omitted = 0;
            bool stopped = false;
            if (due) flush_locked(n, &lines, &omitted, &stopped);
            else if (!n->stopped && n->window_at) {
                int64_t at = n->window_at + NOTIFY_BATCH_MS;
                if (next < 0 || at < next) next = at;
            }
            pthread_mutex_unlock(&n->mu);
            if (due) n->emit(n->ud, lines, omitted, stopped); /* still registered: notifier_free waits on g_mu */
        }
        if (next < 0) pthread_cond_wait(&g_cv, &g_mu);
        else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t wait = MAX(1, next - mono_ms());
            ts.tv_sec += wait / 1000;
            ts.tv_nsec += (wait % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
    }
    return NULL;
}

/* ---------- notifier ---------- */

bool notify_pattern_ok(const char *source, char **err) {
    char *e = NULL;
    rx_t *rx = rx_compile(source ? source : "", 0, &e);
    if (rx) {
        rx_free(rx);
        return true;
    }
    *err = xasprintf("Invalid notify regex: %s", e ? e : "invalid pattern");
    free(e);
    return false;
}

notifier_t *notifier_new(const char *source, bool refill, notify_emit_fn emit, void *ud) {
    char *e = NULL;
    rx_t *rx = rx_compile(source ? source : "", 0, &e);
    free(e);
    if (!rx) return NULL;
    notifier_t *n = xcalloc(1, sizeof *n);
    pthread_mutex_init(&n->mu, NULL);
    n->rx = rx;
    n->lines = json_arr();
    n->allowance = NOTIFY_BURST;
    n->refilled_at = mono_ms();
    n->refill = refill;
    n->emit = emit;
    n->ud = ud;
    pthread_mutex_lock(&g_mu);
    vec_push(&g_live, n);
    if (!g_ticker) {
        pthread_t th;
        if (pthread_create(&th, NULL, ticker_main, NULL) == 0) {
            pthread_detach(th);
            g_ticker = true;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* truncate(line, 400): code points stand in for UTF-16 units */
static char *cut_line(const char *s, size_t len) {
    size_t cps = 0, i = 0, cut = SIZE_MAX;
    while (i < len) {
        if (cps == NOTIFY_MAX_LINE_CHARS - 1 && cut == SIZE_MAX) cut = i;
        unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        cps++;
    }
    if (cps <= NOTIFY_MAX_LINE_CHARS) return xstrndup(s, len);
    char *head = xstrndup(s, cut);
    char *r = xasprintf("%s\xe2\x80\xa6", head);
    free(head);
    return r;
}

static void collect_line(notifier_t *n, const char *s, size_t len) {
    if (len && s[len - 1] == '\r') len--;
    bool blank = true;
    for (size_t i = 0; i < len && blank; i++) blank = s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f';
    if (blank || !rx_is_match(n->rx, s, len, 0)) return;
    if (json_len(n->lines) < NOTIFY_MAX_LINES) json_push(n->lines, json_str_own(cut_line(s, len)));
    else n->omitted++;
    if (!n->window_at) n->window_at = mono_ms();
}

void notifier_feed(notifier_t *n, const char *data, size_t len) {
    if (!n || !len) return;
    pthread_mutex_lock(&n->mu);
    if (n->stopped) {
        pthread_mutex_unlock(&n->mu);
        return;
    }
    bool had_window = n->window_at != 0;
    sb_appendn(&n->partial, data, len);
    size_t start = 0;
    for (size_t i = 0; i < n->partial.len; i++)
        if (n->partial.s[i] == '\n') {
            collect_line(n, n->partial.s + start, i - start);
            start = i + 1;
        }
    if (start == 0 && n->partial.len > NOTIFY_PENDING_CAP) { /* a runaway line: match what arrived */
        collect_line(n, n->partial.s, n->partial.len);
        start = n->partial.len;
    }
    memmove(n->partial.s, n->partial.s + start, n->partial.len - start);
    sb_truncate(&n->partial, n->partial.len - start);
    bool opened = !had_window && n->window_at;
    pthread_mutex_unlock(&n->mu);
    if (opened) {
        pthread_mutex_lock(&g_mu);
        pthread_cond_signal(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
}

void notifier_finish(notifier_t *n) {
    if (!n) return;
    pthread_mutex_lock(&g_mu); /* no ticker flush races the final batch */
    pthread_mutex_lock(&n->mu);
    json_t *lines = NULL;
    int64_t omitted = 0;
    if (!n->stopped) {
        if (n->partial.len) collect_line(n, n->partial.s, n->partial.len);
        sb_truncate(&n->partial, 0);
        lines = n->lines;
        omitted = n->omitted;
        n->lines = json_arr();
        n->omitted = 0;
        n->window_at = 0;
        n->stopped = true;
    }
    pthread_mutex_unlock(&n->mu);
    pthread_mutex_unlock(&g_mu);
    if (lines && json_len(lines)) n->emit(n->ud, lines, omitted, false);
    else json_free(lines);
}

void notifier_free(notifier_t *n) {
    if (!n) return;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_live.n; i++)
        if (g_live.v[i] == n) {
            g_live.v[i] = g_live.v[--g_live.n];
            break;
        }
    pthread_mutex_unlock(&g_mu);
    rx_free(n->rx);
    sb_free(&n->partial);
    json_free(n->lines);
    pthread_mutex_destroy(&n->mu);
    free(n);
}

/* ---------- the notice ---------- */

char *notify_build_message(const json_t *entries, json_t **details) {
    *details = NULL;
    if (!json_len(entries)) return NULL;
    json_t *blocks = json_arr(), *e;
    json_arr_foreach(entries, i, e) {
        /* a job id and a service name live in separate namespaces */
        const char *id = json_get_str(e, "id"), *type = json_get_str(e, "type");
        json_t *known = NULL, *b;
        json_arr_foreach(blocks, k, b) if (str_eq(json_get_str(b, "id"), id) && str_eq(json_get_str(b, "type"), type)) known = b;
        if (!known) {
            known = json_obj();
            json_set_str(known, "id", id ? id : "");
            json_set_str(known, "type", type ? type : "bash");
            json_set_str(known, "pattern", json_get_str(e, "pattern") ? json_get_str(e, "pattern") : "");
            json_set(known, "lines", json_arr());
            json_set_int(known, "omitted", 0);
            json_set_bool(known, "running", json_get_bool(e, "running", false));
            json_set_bool(known, "stopped", false);
            if (json_get_str(e, "command")) json_set_str(known, "command", json_get_str(e, "command"));
            json_push(blocks, known);
        }
        json_t *ln;
        json_arr_foreach(json_get_arr(e, "lines"), k, ln) json_push(json_get_arr(known, "lines"), json_clone(ln));
        json_set_int(known, "omitted", json_get_int(known, "omitted", 0) + json_get_int(e, "omitted", 0));
        if (json_get_bool(e, "stopped", false)) json_set_bool(known, "stopped", true);
    }
    json_t *reports = json_arr(), *ctx_reports = json_arr(), *b;
    json_arr_foreach(blocks, i, b) {
        json_t *lines = json_get_arr(b, "lines");
        int64_t overflow = (int64_t)json_len(lines) - NOTIFY_MAX_LINES;
        if (overflow > 0) {
            while (json_len(lines) > NOTIFY_MAX_LINES) json_free(json_arr_remove(lines, json_len(lines) - 1));
            json_set_int(b, "omitted", json_get_int(b, "omitted", 0) + overflow);
        }
        /* AsyncNotifyReport (details): id, type, pattern, lines, omitted, running, stopped */
        json_t *r = json_obj();
        static const char *const keys[] = {"id", "type", "pattern", "lines", "omitted", "running", "stopped"};
        for (size_t k = 0; k < ARRAY_LEN(keys); k++) json_set(r, keys[k], json_clone(json_get(b, keys[k])));
        json_push(reports, r);
        /* the command, regex and output are text the source controls: escaped for the <system-notice> */
        json_t *c = json_clone(r);
        json_set_bool(c, "service", str_eq(json_get_str(b, "type"), "service"));
        if (json_get_str(b, "command")) {
            char *ci = job_command_identity(json_get_str(b, "command"));
            json_set(c, "command", json_str_own(job_escape_harness_tags(ci)));
            free(ci);
        }
        json_set(c, "pattern", json_str_own(job_escape_harness_tags(json_get_str(b, "pattern"))));
        json_t *el = json_arr(), *ln;
        json_arr_foreach(json_get_arr(b, "lines"), k, ln) json_push(el, json_str_own(job_escape_harness_tags(json_sv(ln) ? json_sv(ln) : "")));
        json_set(c, "lines", el);
        json_push(ctx_reports, c);
    }
    json_free(blocks);
    json_t *ctx = json_obj();
    json_set(ctx, "reports", ctx_reports);
    char *text = prompt_render("coding-agent/prompts/tools/async-notify.md", ctx);
    json_free(ctx);
    *details = json_obj();
    json_set(*details, "reports", reports);
    return text;
}
